/*
 * Pico 2 W — HTTP server
 *
 * Joins a WiFi network, then serves a small status page on port 80 using
 * lwIP's raw TCP API. The page reports uptime, the RP2350's on-die
 * temperature, and how many requests have been served, and refreshes itself
 * every 5 seconds.
 *
 * Credentials come from wifi_secrets.cmake via CMake defines.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

#define HTTP_PORT       80
#define RESPONSE_MAX    2048
#define ADC_VREF        3.3f
#define ADC_RESOLUTION  (1 << 12)

static unsigned long request_count = 0;

/* temperature (see the internal_temperature project) */

static float read_celsius(void)
{
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    float voltage = (sum / 16.0f) * ADC_VREF / ADC_RESOLUTION;
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

/* HTTP plumbing */

typedef struct {
    char   buf[RESPONSE_MAX];
    size_t len;
    size_t sent;
} http_conn_t;

static size_t build_page(char *out, size_t out_size)
{
    uint32_t up_s  = to_ms_since_boot(get_absolute_time()) / 1000;
    float    tempc = read_celsius();
    const char *ip = ipaddr_ntoa(netif_ip4_addr(netif_default));

    char body[1400];
    int body_len = snprintf(body, sizeof(body),
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"5\">"
        "<title>Pico 2 W</title><style>"
        "body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:2rem;"
        "background:#111;color:#eee}"
        "h1{margin:0 0 .25rem;font-size:1.5rem}"
        ".sub{color:#888;margin-bottom:2rem}"
        "table{border-collapse:collapse;width:100%%;max-width:28rem}"
        "td{padding:.6rem .4rem;border-bottom:1px solid #333}"
        "td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
        ".t{font-size:2.5rem;font-weight:600;color:#4ade80}"
        "</style></head><body>"
        "<h1>Raspberry Pi Pico 2 W</h1>"
        "<div class=\"sub\">RP2350 &middot; served from the board itself</div>"
        "<div class=\"t\">%.2f &deg;C</div>"
        "<table>"
        "<tr><td>IP address</td><td>%s</td></tr>"
        "<tr><td>Uptime</td><td>%lu h %lu m %lu s</td></tr>"
        "<tr><td>Requests served</td><td>%lu</td></tr>"
        "<tr><td>Chip</td><td>RP2350 (Cortex-M33)</td></tr>"
        "<tr><td>WiFi</td><td>CYW43439</td></tr>"
        "</table></body></html>",
        tempc, ip,
        (unsigned long)(up_s / 3600),
        (unsigned long)((up_s % 3600) / 60),
        (unsigned long)(up_s % 60),
        request_count);

    return snprintf(out, out_size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n%s", body_len, body);
}

static err_t http_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    http_conn_t *c = (http_conn_t *)arg;
    c->sent += len;
    if (c->sent >= c->len) {          // everything acknowledged
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        free(c);
        tcp_close(pcb);
    }
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    http_conn_t *c = (http_conn_t *)arg;

    if (!p) {                          // client closed the connection
        tcp_close(pcb);
        free(c);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);       // tell lwIP we consumed it
    pbuf_free(p);

    if (c->len == 0) {                 // respond once per connection
        request_count++;
        c->len  = build_page(c->buf, sizeof(c->buf));
        c->sent = 0;
        printf("Request %lu -> %u bytes\n", request_count, (unsigned)c->len);
        tcp_write(pcb, c->buf, c->len, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    }
    return ERR_OK;
}

static void http_err(void *arg, err_t err)
{
    (void)err;
    free(arg);                         // lwIP has already freed the pcb
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;

    http_conn_t *c = calloc(1, sizeof(http_conn_t));
    if (!c) return ERR_MEM;

    tcp_arg(pcb, c);
    tcp_recv(pcb, http_recv);
    tcp_sent(pcb, http_sent);
    tcp_err(pcb,  http_err);
    return ERR_OK;
}

static bool start_server(void)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) { printf("tcp_new failed\n"); return false; }

    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        printf("bind to port %d failed\n", HTTP_PORT);
        return false;
    }

    struct tcp_pcb *listener = tcp_listen_with_backlog(pcb, 4);
    if (!listener) { printf("listen failed\n"); return false; }

    tcp_accept(listener, http_accept);
    return true;
}

/* main */

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    if (WIFI_SSID[0] == '\0') {
        printf("ERROR: WIFI_SSID is empty. Set it in wifi_secrets.cmake.\n");
        while (true) sleep_ms(1000);
    }

    adc_init();
    adc_set_temp_sensor_enabled(true);

    printf("Starting Wi-Fi...\n");
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS)) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to '%s'...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_MIXED_PSK, 30000)) {
        printf("Failed to connect\n");
        return 1;
    }

    const char *ip = ipaddr_ntoa(netif_ip4_addr(netif_default));
    printf("Connected. IP address: %s\n", ip);

    if (!start_server()) return 1;
    printf("HTTP server listening on http://%s/\n", ip);
    printf("Open that address in a browser on the same network.\n\n");

    while (true) {
        // lwIP runs on a background interrupt, so main has nothing to do.
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(100);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(1900);
    }
}
