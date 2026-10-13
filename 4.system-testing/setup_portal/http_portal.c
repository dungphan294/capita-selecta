/*
 * http_portal — the settings page.
 *
 * Served identically in access-point mode and in station mode, so the page
 * you configure the board with before it has a network is the same page you
 * manage it with afterwards.
 *
 * lwIP's raw TCP API, as in the webserver project, with two differences that
 * the extra content forces: the response is sent incrementally rather than in
 * one tcp_write, and POST bodies are accumulated across packets.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#include "http_portal.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "ble_portal.h"
#include "portal_actions.h"
#include "assets.h"

#define HTTP_PORT        80
#define REQUEST_MAX      1024
#define PAGE_MAX         12288
#define MAX_WIFI_ROWS    12
#define MAX_BLE_ROWS     12

static unsigned long request_count = 0;
static char          page_scratch[PAGE_MAX];
static char          notice[160] = "";

typedef struct {
    char   req[REQUEST_MAX];
    size_t req_len;

    char  *resp;          /* headers; also the body for generated responses */
    size_t resp_len;
    size_t resp_sent;

    /*
     * A static asset's body is not copied anywhere. It is a const array in
     * .rodata and flash is memory-mapped, so lwIP is handed the pointer and
     * reads it in place. NULL for generated responses.
     */
    const unsigned char *body;
    size_t body_len;
    size_t body_sent;
} conn_t;

/* ---------------------------------------------------------------- */
/* Small string helpers                                              */
/* ---------------------------------------------------------------- */

/* Append with truncation, tracking the write position. */
static void app(char *buf, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void app(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap - 1) return;

    va_list ap_;
    va_start(ap_, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap_);
    va_end(ap_);

    if (n > 0) *len += ((size_t)n < cap - *len) ? (size_t)n : (cap - *len - 1);
}

/* HTML-escape into caller storage: SSIDs and BLE names are attacker-chosen
 * text being pasted into a page. */
static void esc(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < out_size; p++) {
        switch (*p) {
        case '<': memcpy(out + o, "&lt;",   4); o += 4; break;
        case '>': memcpy(out + o, "&gt;",   4); o += 4; break;
        case '&': memcpy(out + o, "&amp;",  5); o += 5; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        case '\'':memcpy(out + o, "&#39;",  5); o += 5; break;
        default:
            if ((unsigned char)*p >= 0x20) out[o++] = *p;
            break;
        }
    }
    out[o] = '\0';
}

/*
 * JSON-escape into caller storage. The HTML page has esc() for the same
 * reason; this is its JSON counterpart, and it is not optional: a BLE device
 * name is chosen by whoever owns the device, and one double quote in it would
 * otherwise produce a response no parser accepts.
 */
static void jesc(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < out_size; p++) {
        unsigned char ch = (unsigned char)*p;
        switch (ch) {
        case '"':  memcpy(out + o, "\\\"", 2); o += 2; break;
        case '\\': memcpy(out + o, "\\\\", 2); o += 2; break;
        case '\n': memcpy(out + o, "\\n",  2); o += 2; break;
        case '\r': memcpy(out + o, "\\r",  2); o += 2; break;
        case '\t': memcpy(out + o, "\\t",  2); o += 2; break;
        default:
            if (ch < 0x20) { o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", ch); }
            else           { out[o++] = (char)ch; }
            break;
        }
    }
    out[o] = '\0';
}

/*
 * Copy the request target out of "GET /path?query HTTP/1.1", stopping at the
 * query string. Hand-rolled rather than sscanf("%*s %159s"): newlib's scanf
 * pulls in __ssvfscanf_r, _strtod_l and its float tables, about 23 KB of
 * flash, to do what this does in four lines.
 */
static bool request_path(const char *req, char *out, size_t cap)
{
    const char *sp = strchr(req, ' ');
    if (sp == NULL) return false;
    sp++;

    size_t n = 0;
    while (sp[n] != '\0' && sp[n] != ' ' && sp[n] != '\r' &&
           sp[n] != '?'  && n + 1 < cap) {
        out[n] = sp[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Pull one field out of an application/x-www-form-urlencoded body. */
static bool form_field(const char *body, const char *key,
                       char *out, size_t out_size)
{
    size_t keylen = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            const char *v = p + keylen + 1;
            size_t o = 0;

            while (*v && *v != '&' && o + 1 < out_size) {
                if (*v == '+') { out[o++] = ' '; v++; }
                else if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[o++] = (char)((hexval(v[1]) << 4) | hexval(v[2]));
                    v += 3;
                } else out[o++] = *v++;
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }

    if (out_size) out[0] = '\0';
    return false;
}

/* ---------------------------------------------------------------- */
/* Page                                                              */
/* ---------------------------------------------------------------- */

static const char *state_colour(void)
{
    switch (wifi_state()) {
    case WIFI_JOINED:  return "#4ade80";
    case WIFI_JOINING: return "#fbbf24";
    case WIFI_FAILED:  return "#f87171";
    case WIFI_AP_MODE: return "#60a5fa";
    default:           return "#888";
    }
}

static size_t build_page(char *out, size_t cap)
{
    size_t n = 0;
    char   e1[128], e2[128];

    uint32_t up = to_ms_since_boot(get_absolute_time()) / 1000;

    app(out, cap, &n,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Pico 2 W setup</title><style>"
        "body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:1.5rem;"
        "background:#111;color:#eee;line-height:1.5}"
        "main{max-width:44rem;margin:0 auto}"
        "h1{font-size:1.4rem;margin:0 0 .25rem}"
        "h2{font-size:1rem;margin:2rem 0 .5rem;color:#9ca3af;"
        "text-transform:uppercase;letter-spacing:.06em}"
        ".sub{color:#888;margin-bottom:1.5rem;font-size:.9rem}"
        ".badge{display:inline-block;padding:.15rem .6rem;border-radius:999px;"
        "font-size:.8rem;font-weight:600;color:#111;background:%s}"
        "table{border-collapse:collapse;width:100%%;font-size:.9rem}"
        "td,th{padding:.45rem .4rem;border-bottom:1px solid #2a2a2a;text-align:left}"
        "th{color:#888;font-weight:500;font-size:.8rem}"
        "td.n{text-align:right;font-variant-numeric:tabular-nums}"
        "input,select{background:#1c1c1c;border:1px solid #333;color:#eee;"
        "padding:.5rem;border-radius:6px;font-size:1rem;width:100%%;"
        "box-sizing:border-box;margin:.25rem 0 .75rem}"
        "button{background:#2563eb;color:#fff;border:0;padding:.55rem 1.1rem;"
        "border-radius:6px;font-size:.95rem;cursor:pointer}"
        "button.sec{background:#374151}button.danger{background:#991b1b}"
        ".card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;"
        "padding:1rem;margin:.5rem 0}"
        ".big{font-size:2rem;font-weight:600;color:#4ade80}"
        ".warn{background:#422006;border:1px solid #92400e;color:#fcd34d;"
        "padding:.7rem .9rem;border-radius:8px;margin:.5rem 0;font-size:.9rem}"
        ".err{background:#450a0a;border:1px solid #991b1b;color:#fca5a5;"
        "padding:.7rem .9rem;border-radius:8px;margin:.5rem 0;font-size:.9rem}"
        ".ok{background:#052e16;border:1px solid #166534;color:#86efac;"
        "padding:.7rem .9rem;border-radius:8px;margin:.5rem 0;font-size:.9rem}"
        ".ent{color:#fbbf24}"
        "</style></head><body><main>",
        state_colour());

    app(out, cap, &n,
        "<h1>Raspberry Pi Pico 2 W</h1>"
        "<div class=\"sub\">Setup portal &middot; "
        "<span class=\"badge\">%s</span> &middot; %s &middot; "
        "up %lu h %lu m %lu s &middot; %lu requests</div>",
        wifi_state_str(), wifi_ip(),
        (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
        (unsigned long)(up % 60), request_count);

    if (notice[0]) {
        esc(notice, e1, sizeof(e1));
        app(out, cap, &n, "<div class=\"ok\">%s</div>", e1);
        notice[0] = '\0';
    }

    if (wifi_state() == WIFI_FAILED && wifi_fail_reason()[0]) {
        esc(wifi_fail_reason(), e1, sizeof(e1));
        app(out, cap, &n, "<div class=\"err\"><b>WiFi failed.</b> %s</div>", e1);
    }

    /* ---- WiFi ---- */

    app(out, cap, &n, "<h2>WiFi</h2>");

    if (wifi_scan_busy()) {
        app(out, cap, &n, "<div class=\"card\">Scanning&hellip; "
                          "<a href=\"/\" style=\"color:#60a5fa\">refresh</a></div>");
    } else if (wifi_scan_count() > 0) {
        app(out, cap, &n,
            "<table><tr><th>Network</th><th>Security</th>"
            "<th class=\"n\">Signal</th><th class=\"n\">Ch</th></tr>");

        int rows = wifi_scan_count() < MAX_WIFI_ROWS ? wifi_scan_count() : MAX_WIFI_ROWS;
        for (int i = 0; i < rows; i++) {
            const wifi_ap_t *ap = wifi_scan_result(i);
            esc(ap->ssid, e1, sizeof(e1));
            app(out, cap, &n,
                "<tr><td>%s%s</td><td>%s%s</td>"
                "<td class=\"n\">%d dBm</td><td class=\"n\">%u</td></tr>",
                e1, ap->enterprise ? " <span class=\"ent\">&#9888;</span>" : "",
                wifi_auth_name(ap->auth_mode),
                ap->enterprise ? " <span class=\"ent\">(802.1X)</span>" : "",
                ap->rssi, ap->channel);
        }
        app(out, cap, &n, "</table>");
    }

    app(out, cap, &n,
        "<form method=\"post\" action=\"/scan\" style=\"margin:.5rem 0 1rem\">"
        "<button class=\"sec\" type=\"submit\">Scan for networks</button></form>");

    esc(config()->wifi_ssid, e1, sizeof(e1));
    app(out, cap, &n,
        "<form method=\"post\" action=\"/wifi\">"
        "<label>SSID</label>"
        "<input name=\"ssid\" value=\"%s\" maxlength=\"32\" "
        "autocapitalize=\"off\" autocorrect=\"off\">"
        "<label>Password</label>"
        "<input name=\"pass\" type=\"password\" maxlength=\"63\" "
        "placeholder=\"leave blank for an open network\">"
        "<button type=\"submit\">Save and connect</button>"
        "</form>", e1);

    app(out, cap, &n,
        "<div class=\"warn\"><b>WPA2-Enterprise (eduroam) is not supported.</b> "
        "The CYW43439 driver in the Pico SDK offers four authorisation modes, "
        "all pre-shared key: open, WPA-TKIP, WPA2-AES and WPA2-mixed. There is "
        "no EAP supplicant, so 802.1X networks cannot be joined at any "
        "configuration. Use a phone hotspot, a WPA2-PSK IoT network, or a "
        "travel router that does 802.1X upstream.</div>");

    /* ---- BLE ---- */

    app(out, cap, &n, "<h2>Bluetooth</h2>");

    const ble_device_t *pin = ble_pinned_device();
    if (pin) {
        esc(pin->name, e1, sizeof(e1));
        app(out, cap, &n, "<div class=\"card\"><b>%s</b> &middot; %s", e1, pin->addr);
        if (pin->has_temperature)
            app(out, cap, &n, "<div class=\"big\">%.2f &deg;C</div>",
                (double)pin->temperature_c);
        else
            app(out, cap, &n, "<div class=\"sub\" style=\"margin:.5rem 0 0\">"
                              "no environmental service data in its "
                              "advertisements</div>");
        app(out, cap, &n, "<div class=\"sub\" style=\"margin:0\">"
                          "%d dBm &middot; %u advertisements seen</div></div>",
            pin->rssi, pin->seen);
    } else if (config()->ble_target_name[0]) {
        esc(config()->ble_target_name, e1, sizeof(e1));
        app(out, cap, &n,
            "<div class=\"card\">Target <b>%s</b> has not been seen yet.</div>", e1);
    }

    app(out, cap, &n,
        "<table><tr><th>Address</th><th>Name</th>"
        "<th class=\"n\">RSSI</th><th class=\"n\">Seen</th>"
        "<th class=\"n\">Reading</th></tr>");

    int bcount = ble_device_count();
    int brows  = bcount < MAX_BLE_ROWS ? bcount : MAX_BLE_ROWS;
    for (int i = 0; i < brows; i++) {
        const ble_device_t *d = ble_device(i);
        esc(d->name[0] ? d->name : "-", e1, sizeof(e1));

        if (d->has_temperature)
            snprintf(e2, sizeof(e2), "%.2f &deg;C", (double)d->temperature_c);
        else
            snprintf(e2, sizeof(e2), "-");

        app(out, cap, &n,
            "<tr><td>%s</td><td>%s</td><td class=\"n\">%d</td>"
            "<td class=\"n\">%u</td><td class=\"n\">%s</td></tr>",
            d->addr, e1, d->rssi, d->seen, e2);
    }
    app(out, cap, &n, "</table>");

    if (bcount > brows)
        app(out, cap, &n, "<div class=\"sub\">%d more not shown.</div>",
            bcount - brows);

    esc(config()->ble_target_name, e1, sizeof(e1));
    app(out, cap, &n,
        "<form method=\"post\" action=\"/ble\">"
        "<label>Pin a device by advertised name</label>"
        "<input name=\"target\" value=\"%s\" maxlength=\"31\" "
        "placeholder=\"Pico2W-Sensor\">"
        "<button type=\"submit\">Save target</button></form>", e1);

    /* ---- Footer ---- */

    app(out, cap, &n,
        "<h2>Maintenance</h2>"
        "<form method=\"post\" action=\"/reset\">"
        "<button class=\"danger\" type=\"submit\">Erase saved settings</button>"
        "</form>"
        "<div class=\"sub\" style=\"margin-top:2rem\">"
        "Settings are stored in the last flash sector and survive a power "
        "cycle. BLE setup service: 4A7F0000-1C2B-4F7A-9D3E-0A1B2C3D4E5F</div>"
        "</main></body></html>");

    return n;
}

/* ---------------------------------------------------------------- */
/* Request handling                                                  */
/* ---------------------------------------------------------------- */

/* Small helper: allocate and store just a header, with no body. */
static bool resp_header_only(conn_t *c, const char *text, size_t len)
{
    c->resp = malloc(len);
    if (c->resp == NULL) return false;
    memcpy(c->resp, text, len);
    c->resp_len = len;
    c->body     = NULL;
    c->body_len = 0;
    return true;
}

/* Allocate a header and point the body at flash — nothing is copied. */
static bool resp_asset(conn_t *c, const asset_t *a, bool head_only)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "%s"
        "ETag: \"%s\"\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        a->content_type, (unsigned)a->len,
        a->gzip ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" : "",
        a->etag);

    c->resp = malloc((size_t)n);
    if (c->resp == NULL) return false;
    memcpy(c->resp, hdr, (size_t)n);
    c->resp_len = (size_t)n;

    c->body     = head_only ? NULL : a->data;
    c->body_len = head_only ? 0    : a->len;
    return true;
}

/* Wrap a freshly built body in page_scratch with headers. */
static bool resp_scratch(conn_t *c, const char *ctype, size_t body_len)
{
    c->resp = malloc(body_len + 192);
    if (c->resp == NULL) return false;

    int hdr = snprintf(c->resp, 192,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
        "Content-Length: %u\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", ctype, (unsigned)body_len);

    memcpy(c->resp + hdr, page_scratch, body_len);
    c->resp_len = (size_t)hdr + body_len;
    c->body     = NULL;
    c->body_len = 0;
    return true;
}

/* Build the response for c->req into c. False only on allocation failure. */
static bool build_response(conn_t *c)
{
    const char *req = c->req;
    const char *body_start = strstr(req, "\r\n\r\n");
    const char *body = body_start ? body_start + 4 : "";

    char field[128];

    if (strncmp(req, "POST /wifi", 10) == 0) {
        if (form_field(body, "ssid", field, sizeof(field))) {
            strncpy(config()->wifi_ssid, field, CFG_SSID_MAX - 1);
            config()->wifi_ssid[CFG_SSID_MAX - 1] = '\0';
        }
        if (form_field(body, "pass", field, sizeof(field))) {
            strncpy(config()->wifi_pass, field, CFG_PASS_MAX - 1);
            config()->wifi_pass[CFG_PASS_MAX - 1] = '\0';
        }
        config()->provisioned = 1;
        portal_queue_action(PORTAL_JOIN);
        snprintf(notice, sizeof(notice),
                 "Saved. Connecting to '%s' — reload in a few seconds.",
                 config()->wifi_ssid);

    } else if (strncmp(req, "POST /ble", 9) == 0) {
        if (form_field(body, "target", field, sizeof(field))) {
            strncpy(config()->ble_target_name, field, CFG_NAME_MAX - 1);
            config()->ble_target_name[CFG_NAME_MAX - 1] = '\0';
            ble_set_target(config()->ble_target_name);
        }
        portal_queue_action(PORTAL_SAVE);
        snprintf(notice, sizeof(notice), "BLE target saved.");

    } else if (strncmp(req, "POST /scan", 10) == 0) {
        portal_queue_action(PORTAL_SCAN);
        snprintf(notice, sizeof(notice), "Scanning — reload in a few seconds.");

    } else if (strncmp(req, "POST /reset", 11) == 0) {
        portal_queue_action(PORTAL_FACTORY_RESET);
        snprintf(notice, sizeof(notice), "Settings erased.");
    }

    /* Every POST answers with a redirect, so a reload does not resubmit. */
    if (strncmp(req, "POST ", 5) == 0) {
        static const char redirect[] =
            "HTTP/1.1 303 See Other\r\nLocation: /\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        return resp_header_only(c, redirect, sizeof(redirect) - 1);
    }

    /* Static assets, straight out of flash. "/" means index.html. */
    bool is_get  = strncmp(req, "GET ", 4) == 0;
    bool is_head = strncmp(req, "HEAD ", 5) == 0;

    if (is_get || is_head) {
        char path[160];
        if (request_path(req, path, sizeof path)) {
            const asset_t *a =
                asset_find(strcmp(path, "/") == 0 ? "/index.html" : path);
            if (a) return resp_asset(c, a, is_head);
        }
    }

    /*
     * JSON — now the page's only data source, so it carries everything the
     * frontend renders. Every string that originates off the air goes through
     * jesc() on the way out.
     */
    if (strncmp(req, "GET /api/json", 13) == 0) {
        char esc_buf[160];
        size_t n = 0;

        /* -- wifi -- */
        app(page_scratch, PAGE_MAX, &n, "{\"wifi\":{\"state\":\"%s\"",
            wifi_state_str());

        jesc(config()->wifi_ssid, esc_buf, sizeof esc_buf);
        app(page_scratch, PAGE_MAX, &n, ",\"ssid\":\"%s\"", esc_buf);

        app(page_scratch, PAGE_MAX, &n, ",\"ip\":\"%s\"", wifi_ip());

        jesc(wifi_fail_reason(), esc_buf, sizeof esc_buf);
        app(page_scratch, PAGE_MAX, &n, ",\"fail_reason\":\"%s\"", esc_buf);

        app(page_scratch, PAGE_MAX, &n, ",\"scanning\":%s,\"scan\":[",
            wifi_scan_busy() ? "true" : "false");

        for (int i = 0; i < wifi_scan_count(); i++) {
            const wifi_ap_t *ap = wifi_scan_result(i);
            if (ap == NULL) break;
            if (n + 200 >= PAGE_MAX) break;     /* stop whole, never half */

            jesc(ap->ssid, esc_buf, sizeof esc_buf);
            app(page_scratch, PAGE_MAX, &n,
                "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,"
                "\"auth\":\"%s\",\"enterprise\":%s}",
                i ? "," : "", esc_buf, ap->rssi, (unsigned)ap->channel,
                wifi_auth_name(ap->auth_mode),
                ap->enterprise ? "true" : "false");
        }
        app(page_scratch, PAGE_MAX, &n, "]}");

        /* -- ble -- */
        app(page_scratch, PAGE_MAX, &n, ",\"ble\":{\"connected\":%s",
            ble_is_connected() ? "true" : "false");

        const ble_device_t *pin = ble_pinned_device();
        if (pin) {
            jesc(pin->name, esc_buf, sizeof esc_buf);
            app(page_scratch, PAGE_MAX, &n,
                ",\"pinned\":{\"addr\":\"%s\",\"name\":\"%s\","
                "\"rssi\":%d,\"seen\":%u",
                pin->addr, esc_buf, pin->rssi, pin->seen);
            if (pin->has_temperature)
                app(page_scratch, PAGE_MAX, &n, ",\"temperature_c\":%.2f",
                    (double)pin->temperature_c);
            app(page_scratch, PAGE_MAX, &n, "}");
        } else {
            app(page_scratch, PAGE_MAX, &n, ",\"pinned\":null");
        }

        app(page_scratch, PAGE_MAX, &n, ",\"devices\":[");
        for (int i = 0; i < ble_device_count(); i++) {
            const ble_device_t *d = ble_device(i);
            if (d == NULL) break;
            if (n + 220 >= PAGE_MAX) break;

            jesc(d->name, esc_buf, sizeof esc_buf);
            app(page_scratch, PAGE_MAX, &n,
                "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"seen\":%u",
                i ? "," : "", d->addr, esc_buf, d->rssi, d->seen);
            if (d->has_temperature)
                app(page_scratch, PAGE_MAX, &n, ",\"temperature_c\":%.2f",
                    (double)d->temperature_c);
            app(page_scratch, PAGE_MAX, &n, "}");
        }
        app(page_scratch, PAGE_MAX, &n, "]}");

        /* -- shared -- */
        jesc(notice, esc_buf, sizeof esc_buf);
        app(page_scratch, PAGE_MAX, &n,
            ",\"notice\":\"%s\",\"requests\":%lu}", esc_buf, request_count);

        return resp_scratch(c, "application/json; charset=utf-8", n);
    }

    /*
     * Anything else — /classic, and every captive-portal probe a phone makes
     * on joining — gets the server-rendered page. It is kept deliberately:
     * it needs no JavaScript and renders the scan lists itself, so it is the
     * honest fallback when app.js is blocked, and it is the better thing to
     * hand a captive-portal sheet for the same reason.
     */
    size_t body_len = build_page(page_scratch, PAGE_MAX);
    return resp_scratch(c, "text/html; charset=utf-8", body_len);
}

static void conn_free(conn_t *c)
{
    if (c == NULL) return;
    if (c->resp) free(c->resp);
    free(c);
}

/* Write as much as the send buffer currently allows; the rest follows from
 * the sent callback as the peer acknowledges. */
static void pump(struct tcp_pcb *pcb, conn_t *c)
{
    while (c->resp_sent < c->resp_len) {
        u16_t space = tcp_sndbuf(pcb);
        if (space == 0) { tcp_output(pcb); return; }

        size_t left  = c->resp_len - c->resp_sent;
        u16_t  chunk = (u16_t)(left < space ? left : space);

        if (tcp_write(pcb, c->resp + c->resp_sent, chunk, TCP_WRITE_FLAG_COPY)
            != ERR_OK) break;

        c->resp_sent += chunk;
    }

    /*
     * A flash body goes out without TCP_WRITE_FLAG_COPY. lwIP keeps the
     * pointer until the bytes are acknowledged, which is safe only because
     * flash never moves — and it means a large asset costs no more RAM than
     * a small one. Writing flash while one of these is in flight would read
     * garbage, which is why settings are only ever written from main().
     */
    while (c->body != NULL && c->body_sent < c->body_len) {
        u16_t space = tcp_sndbuf(pcb);
        if (space == 0) break;

        size_t left  = c->body_len - c->body_sent;
        u16_t  chunk = (u16_t)(left < space ? left : space);

        if (tcp_write(pcb, c->body + c->body_sent, chunk, 0) != ERR_OK) break;

        c->body_sent += chunk;
    }
    tcp_output(pcb);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = (conn_t *)arg;
    if (c == NULL) return ERR_OK;

    /*
     * Closing only once everything has been acknowledged. Closing earlier
     * discards unacknowledged bytes — the page still renders, so the fault is
     * invisible to the eye and only a Content-Length check catches it.
     */
    if (c->resp_sent >= c->resp_len && c->body_sent >= c->body_len &&
        len > 0 && tcp_sndbuf(pcb) >= TCP_SND_BUF) {
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        conn_free(c);
        tcp_close(pcb);
        return ERR_OK;
    }

    pump(pcb, c);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = (conn_t *)arg;
    (void)err;

    if (p == NULL) {                       /* peer closed */
        tcp_arg(pcb, NULL);
        conn_free(c);
        tcp_close(pcb);
        return ERR_OK;
    }

    if (c == NULL) { pbuf_free(p); tcp_close(pcb); return ERR_OK; }

    size_t space = REQUEST_MAX - 1 - c->req_len;
    size_t take  = p->tot_len < space ? p->tot_len : space;
    pbuf_copy_partial(p, c->req + c->req_len, (u16_t)take, 0);
    c->req_len += take;
    c->req[c->req_len] = '\0';

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (c->resp != NULL) return ERR_OK;    /* already answering */

    const char *headers_end = strstr(c->req, "\r\n\r\n");
    if (headers_end == NULL) return ERR_OK;   /* headers still arriving */

    /* A POST body may lag the headers by a packet. Wait for all of it. */
    const char *cl = strstr(c->req, "Content-Length:");
    if (cl) {
        long want = strtol(cl + 15, NULL, 10);
        long have = (long)(c->req_len - (size_t)(headers_end + 4 - c->req));
        if (have < want && c->req_len < REQUEST_MAX - 1) return ERR_OK;
    }

    request_count++;
    if (!build_response(c)) {
        tcp_arg(pcb, NULL);
        conn_free(c);
        tcp_close(pcb);
        return ERR_OK;
    }

    c->resp_sent = 0;
    c->body_sent = 0;
    pump(pcb, c);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    conn_free((conn_t *)arg);   /* the pcb is already gone */
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;

    conn_t *c = calloc(1, sizeof(conn_t));
    if (c == NULL) { tcp_abort(pcb); return ERR_ABRT; }

    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    return ERR_OK;
}

bool http_portal_init(void)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL) { printf("http: tcp_new failed\n"); return false; }

    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        printf("http: bind to port %d failed\n", HTTP_PORT);
        tcp_close(pcb);
        return false;
    }

    struct tcp_pcb *listener = tcp_listen_with_backlog(pcb, 2);
    if (listener == NULL) { printf("http: listen failed\n"); tcp_close(pcb); return false; }

    tcp_accept(listener, on_accept);
    printf("http: listening on port %d\n", HTTP_PORT);
    for (size_t i = 0; i < ASSET_COUNT; i++)
        printf("http:   %-14s %6u bytes %s\n", assets[i].path,
               (unsigned)assets[i].len, assets[i].gzip ? "gzip" : "stored");
    return true;
}

unsigned long http_request_count(void) { return request_count; }
