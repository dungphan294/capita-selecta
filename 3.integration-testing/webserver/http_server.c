/*
 * HTTP server on lwIP's raw TCP API.
 *
 *   GET  /              -> /index.html from flash
 *   GET  /app.css, ...  -> any file embedded from web/ by tools/embed_assets.py
 *   GET  /api/status    -> JSON: mode, network, ip, temperature, uptime, ...
 *   GET  /api/networks  -> JSON: the last completed scan
 *   POST /api/scan      -> 204, queues a scan
 *   POST /connect       -> ssid=...&other=...&pass=... , queues a join
 *   POST /forget        -> queues erasing the stored network
 *
 * In access-point mode any other GET also returns the page, which is what
 * makes a phone's captive-portal sheet appear.
 *
 * Two things govern the shape of this file:
 *
 *  - Core 0's stack is 2 KB and these callbacks run in lwIP's background
 *    interrupt on it. Nothing large may be a local; every buffer lives in the
 *    heap-allocated http_conn_t.
 *
 *  - Static assets are const arrays in .rodata, and flash is memory-mapped, so
 *    they go to tcp_write() without TCP_WRITE_FLAG_COPY. Nothing is copied
 *    into RAM on the way out, whatever the file's size.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/tcp.h"

#include "app.h"
#include "assets.h"
#include "http_server.h"
#include "wifi_scan.h"
#include "wifi_store.h"

#define HTTP_PORT    80
#define REQUEST_MAX  1024
#define HEADER_MAX    224
#define DYN_MAX      4096    /* largest dynamic body: /api/networks, 24 entries */

typedef struct {
    char   req[REQUEST_MAX];
    size_t req_len;
    bool   handled;

    char   hdr[HEADER_MAX];
    size_t hdr_len;

    char   dyn[DYN_MAX];     /* generated bodies are built in here */

    const unsigned char *body;          /* -> dyn, or -> flash */
    size_t               body_len;
    bool                 body_in_flash; /* if so, send it without copying */

    size_t written;          /* handed to lwIP, across hdr then body */
    size_t sent;             /* acknowledged by the client */
} http_conn_t;

/* -- building strings ------------------------------------------------------ */

/* snprintf that accumulates and clamps instead of overrunning. */
static size_t appendf(char *buf, size_t cap, size_t len, const char *fmt, ...)
{
    if (cap == 0 || len >= cap - 1)
        return len;

    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);

    if (w < 0)
        return len;

    size_t next = len + (size_t)w;
    return next > cap - 1 ? cap - 1 : next;
}

/* SSIDs come off the air, so they are never pasted into JSON raw. */
static size_t append_json_string(char *buf, size_t cap, size_t len, const char *s)
{
    len = appendf(buf, cap, len, "\"");
    for (; *s && len + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  len = appendf(buf, cap, len, "\\\""); break;
        case '\\': len = appendf(buf, cap, len, "\\\\"); break;
        case '\n': len = appendf(buf, cap, len, "\\n");  break;
        case '\r': len = appendf(buf, cap, len, "\\r");  break;
        case '\t': len = appendf(buf, cap, len, "\\t");  break;
        default:
            if (c < 0x20)
                len = appendf(buf, cap, len, "\\u%04x", c);
            else
                len = appendf(buf, cap, len, "%c", c);
            break;
        }
    }
    return appendf(buf, cap, len, "\"");
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Pulls one field out of an application/x-www-form-urlencoded body. */
static bool form_field(const char *body, size_t body_len, const char *key,
                       char *out, size_t cap)
{
    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_len;

    out[0] = '\0';

    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        const char *pair_end = amp ? amp : end;

        if ((size_t)(pair_end - p) > key_len &&
            strncmp(p, key, key_len) == 0 && p[key_len] == '=') {

            const char *v = p + key_len + 1;
            size_t n = 0;
            while (v < pair_end && n + 1 < cap) {
                if (*v == '+') {
                    out[n++] = ' ';
                    v++;
                } else if (*v == '%' && v + 3 <= pair_end &&
                           hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[n++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else {
                    out[n++] = *v++;
                }
            }
            out[n] = '\0';
            return true;
        }

        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

/* -- assembling responses -------------------------------------------------- */

/* Finishes a response whose body is already in c->dyn. */
static void respond_dyn(http_conn_t *c, const char *status, const char *ctype,
                        size_t body_len)
{
    c->body          = (const unsigned char *)c->dyn;
    c->body_len      = body_len;
    c->body_in_flash = false;

    c->hdr_len = appendf(c->hdr, HEADER_MAX, 0,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, (unsigned)body_len);
}

/* A response with no body at all. */
static void respond_empty(http_conn_t *c, const char *status)
{
    c->body = NULL;
    c->body_len = 0;
    c->body_in_flash = false;

    c->hdr_len = appendf(c->hdr, HEADER_MAX, 0,
        "HTTP/1.1 %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n", status);
}

/* Zero-copy out of flash. Honours If-None-Match with a 304. */
static void respond_asset(http_conn_t *c, const asset_t *a, const char *inm)
{
    if (inm && strstr(inm, a->etag)) {
        c->body = NULL;
        c->body_len = 0;
        c->body_in_flash = false;
        c->hdr_len = appendf(c->hdr, HEADER_MAX, 0,
            "HTTP/1.1 304 Not Modified\r\n"
            "ETag: \"%s\"\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n", a->etag);
        return;
    }

    c->body          = a->data;
    c->body_len      = a->len;
    c->body_in_flash = true;

    /* no-cache means "revalidate before use", not "do not store" — the
     * browser comes back with If-None-Match and usually gets a 304. */
    c->hdr_len = appendf(c->hdr, HEADER_MAX, 0,
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
}

static void respond_text(http_conn_t *c, const char *status, const char *text)
{
    size_t n = appendf(c->dyn, DYN_MAX, 0, "%s", text);
    respond_dyn(c, status, "text/plain; charset=utf-8", n);
}

/* Small self-contained page, so the plain forms still work without app.js. */
static void respond_notice(http_conn_t *c, const char *title, const char *detail)
{
    size_t n = appendf(c->dyn, DYN_MAX, 0,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Pico 2 W</title>"
        "<body style=\"font:16px/1.5 system-ui,sans-serif;max-width:32rem;"
        "margin:3rem auto;padding:0 1rem;background:#0e0f11;color:#e8eaed\">"
        "<h1 style=\"font-size:1.25rem\">%s</h1><p style=\"color:#8b919b\">%s</p>"
        "<p><a href=\"/\" style=\"color:#4ade80\">Back</a></p>",
        title, detail);
    respond_dyn(c, "200 OK", "text/html; charset=utf-8", n);
}

/* -- JSON endpoints -------------------------------------------------------- */

static void api_status(http_conn_t *c)
{
    size_t n = 0;

    n = appendf(c->dyn, DYN_MAX, n, "{\"mode\":\"%s\",\"network\":",
                app_mode() == APP_MODE_AP ? "ap" : "sta");
    n = append_json_string(c->dyn, DYN_MAX, n, app_network());

    n = appendf(c->dyn, DYN_MAX, n, ",\"ip\":");
    n = append_json_string(c->dyn, DYN_MAX, n, app_ip());

    n = appendf(c->dyn, DYN_MAX, n,
        ",\"temp_c\":%.2f,\"uptime_s\":%lu,\"requests\":%lu,"
        "\"stored\":%s,\"scanning\":%s,\"message\":",
        app_temperature_c(),
        (unsigned long)app_uptime_s(),
        app_requests(),
        app_has_stored_creds() ? "true" : "false",
        wifi_scan_busy() ? "true" : "false");
    n = append_json_string(c->dyn, DYN_MAX, n, app_last_message());

    n = appendf(c->dyn, DYN_MAX, n, "}");

    respond_dyn(c, "200 OK", "application/json; charset=utf-8", n);
}

static void api_networks(http_conn_t *c)
{
    const wifi_ap_t *aps;
    int count = wifi_scan_results(&aps);
    size_t n = appendf(c->dyn, DYN_MAX, 0, "[");

    for (int i = 0; i < count; i++) {
        /* Stop cleanly rather than emitting half an object. */
        if (n + 160 >= DYN_MAX)
            break;
        if (i)
            n = appendf(c->dyn, DYN_MAX, n, ",");
        n = appendf(c->dyn, DYN_MAX, n, "{\"ssid\":");
        n = append_json_string(c->dyn, DYN_MAX, n, aps[i].ssid);
        n = appendf(c->dyn, DYN_MAX, n,
            ",\"rssi\":%d,\"channel\":%u,\"secured\":%s}",
            aps[i].rssi, (unsigned)aps[i].channel,
            aps[i].secured ? "true" : "false");
    }

    n = appendf(c->dyn, DYN_MAX, n, "]");
    respond_dyn(c, "200 OK", "application/json; charset=utf-8", n);
}

/* -- routing --------------------------------------------------------------- */

static void route(http_conn_t *c, const char *method, const char *path,
                  const char *body, size_t body_len, const char *inm)
{
    bool is_get  = strcmp(method, "GET")  == 0;
    bool is_head = strcmp(method, "HEAD") == 0;
    bool is_post = strcmp(method, "POST") == 0;

    if (is_get || is_head) {
        const asset_t *a = asset_find(strcmp(path, "/") == 0 ? "/index.html" : path);
        if (a) {
            respond_asset(c, a, inm);
            if (is_head) { c->body = NULL; c->body_len = 0; }
            return;
        }

        if (strcmp(path, "/api/status") == 0)   { api_status(c);   return; }
        if (strcmp(path, "/api/networks") == 0) { api_networks(c); return; }

        /* Captive portal: in setup mode anything else is the page too, so
         * whatever the phone probes on joining opens the portal sheet. */
        if (app_mode() == APP_MODE_AP) {
            const asset_t *index = asset_find("/index.html");
            if (index) { respond_asset(c, index, NULL); return; }
        }

        respond_text(c, "404 Not Found", "not found\n");
        return;
    }

    if (is_post && strcmp(path, "/api/scan") == 0) {
        app_request_scan();
        respond_empty(c, "204 No Content");
        return;
    }

    if (is_post && strcmp(path, "/connect") == 0) {
        char ssid[WIFI_SSID_MAX + 1], other[WIFI_SSID_MAX + 1];
        char pass[WIFI_PASS_MAX + 1];

        form_field(body, body_len, "ssid",  ssid,  sizeof ssid);
        form_field(body, body_len, "other", other, sizeof other);
        form_field(body, body_len, "pass",  pass,  sizeof pass);

        const char *chosen = other[0] != '\0' ? other : ssid;

        if (chosen[0] == '\0') {
            respond_notice(c, "No network chosen",
                "Pick one from the list or type a name, then submit again.");
            return;
        }

        app_request_connect(chosen, pass);
        respond_notice(c, "Joining network",
            "This connection will drop and the board's address will change. "
            "Watch the serial console for the new one. If the join fails, the "
            "setup access point comes back on its own.");
        return;
    }

    if (is_post && strcmp(path, "/forget") == 0) {
        app_request_forget();
        respond_notice(c, "Forgetting network",
            "The stored credentials are being erased and the board will "
            "restart into setup mode.");
        return;
    }

    respond_text(c, "404 Not Found", "not found\n");
}

/* -- sending --------------------------------------------------------------- */

static void send_more(struct tcp_pcb *pcb, http_conn_t *c)
{
    size_t total = c->hdr_len + c->body_len;

    while (c->written < total) {
        u16_t space = tcp_sndbuf(pcb);
        if (space == 0)
            break;

        const void *src;
        size_t      remaining;
        u8_t        flags;

        if (c->written < c->hdr_len) {
            src       = c->hdr + c->written;
            remaining = c->hdr_len - c->written;
            flags     = TCP_WRITE_FLAG_COPY;
        } else {
            size_t off = c->written - c->hdr_len;
            src       = c->body + off;
            remaining = c->body_len - off;
            /* Flash never moves, so lwIP may reference it directly. A RAM
             * body lives in c->dyn, which outlives the send, but copying it
             * keeps the pbuf chain independent of the connection struct. */
            flags     = c->body_in_flash ? 0 : TCP_WRITE_FLAG_COPY;
        }

        u16_t chunk = remaining < space ? (u16_t)remaining : space;
        if (c->written + chunk < total)
            flags |= TCP_WRITE_FLAG_MORE;

        if (tcp_write(pcb, src, chunk, flags) != ERR_OK)
            break;
        c->written += chunk;
    }

    tcp_output(pcb);
}

static void conn_close(struct tcp_pcb *pcb, http_conn_t *c)
{
    tcp_arg(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    free(c);
    tcp_close(pcb);
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    http_conn_t *c = (http_conn_t *)arg;
    if (!c)
        return ERR_OK;

    c->sent += len;

    if (c->written < c->hdr_len + c->body_len) {
        send_more(pcb, c);
        return ERR_OK;
    }

    if (c->sent >= c->hdr_len + c->body_len)   /* everything acknowledged */
        conn_close(pcb, c);

    return ERR_OK;
}

/* -- receiving ------------------------------------------------------------- */

/* Copies a header's value out of the request, NUL-terminated. */
static bool header_value(const char *req, const char *name, char *out, size_t cap)
{
    const char *p = req;
    size_t name_len = strlen(name);

    out[0] = '\0';

    /* Header names are case-insensitive; walk line by line. */
    while (p && *p) {
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t n = 0;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n + 1 < cap) {
                out[n] = v[n];
                n++;
            }
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

static void try_handle(struct tcp_pcb *pcb, http_conn_t *c)
{
    c->req[c->req_len] = '\0';

    char *headers_end = strstr(c->req, "\r\n\r\n");
    if (!headers_end)
        return;                          /* wait for the rest */

    const char *body      = headers_end + 4;
    size_t      body_have = c->req_len - (size_t)(body - c->req);

    char cl[16];
    size_t body_need = 0;
    if (header_value(c->req, "Content-Length", cl, sizeof cl))
        body_need = (size_t)strtoul(cl, NULL, 10);

    if (body_have < body_need && c->req_len < REQUEST_MAX - 1)
        return;                          /* wait for the body */

    char method[8] = {0}, path[160] = {0};
    if (sscanf(c->req, "%7s %159s", method, path) != 2) {
        respond_text(c, "400 Bad Request", "bad request\n");
    } else {
        char *q = strchr(path, '?');     /* no query parameters are used */
        if (q) *q = '\0';

        char inm[96];
        bool have_inm = header_value(c->req, "If-None-Match", inm, sizeof inm);

        app_count_request();
        route(c, method, path, body, body_have, have_inm ? inm : NULL);

        printf("%s %s -> %u + %u bytes%s\n", method, path,
               (unsigned)c->hdr_len, (unsigned)c->body_len,
               c->body_in_flash ? " (flash)" : "");
    }

    c->handled = true;
    c->written = 0;
    c->sent    = 0;
    send_more(pcb, c);
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_conn_t *c = (http_conn_t *)arg;
    (void)err;

    if (!p) {                            /* client closed */
        conn_close(pcb, c);
        return ERR_OK;
    }

    if (c && !c->handled) {
        size_t room = REQUEST_MAX - 1 - c->req_len;
        size_t take = p->tot_len < room ? p->tot_len : room;
        if (take)
            c->req_len += pbuf_copy_partial(p, c->req + c->req_len, (u16_t)take, 0);
    }

    tcp_recved(pcb, p->tot_len);         /* reopen the receive window */
    pbuf_free(p);

    if (c && !c->handled)
        try_handle(pcb, c);

    return ERR_OK;
}

static void http_err(void *arg, err_t err)
{
    (void)err;
    free(arg);                           /* lwIP has already freed the pcb */
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || pcb == NULL)
        return ERR_VAL;

    http_conn_t *c = calloc(1, sizeof(http_conn_t));
    if (!c)
        return ERR_MEM;                  /* lwIP drops the connection */

    tcp_arg(pcb, c);
    tcp_recv(pcb, http_recv);
    tcp_sent(pcb, http_sent);
    tcp_err(pcb, http_err);
    return ERR_OK;
}

bool http_server_start(void)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        printf("tcp_new failed\n");
        return false;
    }

    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        printf("bind to port %d failed\n", HTTP_PORT);
        return false;
    }

    struct tcp_pcb *listener = tcp_listen_with_backlog(pcb, 4);
    if (!listener) {
        printf("listen failed\n");
        return false;
    }

    tcp_accept(listener, http_accept);

    printf("Serving %d embedded asset%s:\n", ASSET_COUNT,
           ASSET_COUNT == 1 ? "" : "s");
    for (size_t i = 0; i < ASSET_COUNT; i++)
        printf("  %-14s %6u bytes %s\n", assets[i].path,
               (unsigned)assets[i].len, assets[i].gzip ? "gzip" : "stored");

    return true;
}
