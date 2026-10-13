/*
 * Pico 2 W — HTTP server with Wi-Fi provisioning
 *
 * Serves a status page on port 80 that also scans for nearby networks and
 * lets you join one from the browser. The choice is written to a reserved
 * flash sector, so it survives a power cycle. If there is nothing stored and
 * the compiled-in network cannot be joined, the board hosts its own access
 * point and serves the same page there as a captive portal.
 *
 * Boot order:
 *   stored credentials -> compiled-in WIFI_SSID -> setup access point
 *
 * The HTTP callbacks run in lwIP's background interrupt and must not touch
 * the radio, so they queue work through the app_request_* functions in app.h
 * and the main loop below carries it out.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include "lwip/netif.h"

#include "app.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "http_server.h"
#include "wifi_scan.h"
#include "wifi_store.h"

#define AP_SSID         "pico-setup"
#define AP_PASS         "picosetup"      /* WPA2 needs at least 8 characters */
#define JOIN_TIMEOUT_MS 20000

/* Let a queued response drain before the radio is disturbed. */
#define ACTION_DELAY_MS 1200

#define ADC_VREF        3.3f
#define ADC_RESOLUTION  (1 << 12)

static app_mode_t    mode = APP_MODE_AP;
static char          joined_ssid[WIFI_SSID_MAX + 1];
static bool          have_stored;
static unsigned long request_count;
static char          message[192];

static volatile bool want_scan;
static volatile bool want_connect;
static volatile bool want_forget;
static wifi_creds_t  pending;
static absolute_time_t action_at;

/* -- app.h: status --------------------------------------------------------- */

app_mode_t app_mode(void) { return mode; }

const char *app_ip(void)
{
    struct netif *nif = (mode == APP_MODE_AP)
                      ? &cyw43_state.netif[CYW43_ITF_AP]
                      : &cyw43_state.netif[CYW43_ITF_STA];
    return ipaddr_ntoa(netif_ip4_addr(nif));
}

const char *app_network(void)
{
    return mode == APP_MODE_AP ? AP_SSID : joined_ssid;
}

bool app_has_stored_creds(void) { return have_stored; }

float app_temperature_c(void)
{
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    float voltage = (sum / 16.0f) * ADC_VREF / ADC_RESOLUTION;
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

uint32_t app_uptime_s(void)
{
    return to_ms_since_boot(get_absolute_time()) / 1000;
}

unsigned long app_requests(void)       { return request_count; }
void          app_count_request(void)  { request_count++; }
const char   *app_last_message(void)   { return message; }

/* -- app.h: queued actions ------------------------------------------------- */

void app_request_scan(void)
{
    want_scan = true;
}

void app_request_connect(const char *ssid, const char *pass)
{
    snprintf(pending.ssid, sizeof pending.ssid, "%s", ssid);
    snprintf(pending.pass, sizeof pending.pass, "%s", pass);
    action_at    = make_timeout_time_ms(ACTION_DELAY_MS);
    want_connect = true;
}

void app_request_forget(void)
{
    action_at   = make_timeout_time_ms(ACTION_DELAY_MS);
    want_forget = true;
}

/* -- access point ---------------------------------------------------------- */

static void start_ap(void)
{
    printf("Starting setup access point '%s' (password '%s')\n", AP_SSID, AP_PASS);

    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASS, CYW43_AUTH_WPA2_AES_PSK);

    struct netif *nif = &cyw43_state.netif[CYW43_ITF_AP];
    ip4_addr_t ip, mask;
    IP4_ADDR(&ip,   192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    cyw43_arch_lwip_begin();
    netif_set_addr(nif, &ip, &mask, &ip);
    cyw43_arch_lwip_end();

    dhcp_server_start(nif, &ip);
    dns_server_start(&ip);

    mode = APP_MODE_AP;
    joined_ssid[0] = '\0';
    printf("Join '%s' and open http://192.168.4.1/\n", AP_SSID);
}

static void stop_ap(void)
{
    dns_server_stop();
    dhcp_server_stop();
    cyw43_arch_disable_ap_mode();
}

/* -- joining --------------------------------------------------------------- */

/* Returns true once the board is on the named network. */
static bool join(const wifi_creds_t *creds)
{
    bool open = creds->pass[0] == '\0';

    printf("Connecting to '%s'...\n", creds->ssid);
    int err = cyw43_arch_wifi_connect_timeout_ms(
        creds->ssid,
        open ? NULL : creds->pass,
        open ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK,
        JOIN_TIMEOUT_MS);

    if (err != 0) {
        printf("Failed to connect (%d)\n", err);
        return false;
    }

    mode = APP_MODE_STA;
    snprintf(joined_ssid, sizeof joined_ssid, "%s", creds->ssid);
    printf("Connected. IP address: %s\n", app_ip());
    return true;
}

static void do_connect(void)
{
    if (mode == APP_MODE_AP)
        stop_ap();

    cyw43_arch_enable_sta_mode();

    if (join(&pending)) {
        if (wifi_store_save(&pending)) {
            have_stored = true;
            snprintf(message, sizeof message,
                     "Joined and saved \"%s\".", pending.ssid);
            printf("Credentials saved to flash.\n");
        } else {
            snprintf(message, sizeof message,
                     "Joined \"%s\", but saving to flash failed — this will not "
                     "survive a restart.", pending.ssid);
            printf("WARNING: flash write failed\n");
        }
        printf("HTTP server listening on http://%s/\n", app_ip());
        return;
    }

    snprintf(message, sizeof message,
             "Could not join \"%s\". Check the password, and that it is a 2.4 GHz "
             "WPA2 network.", pending.ssid);

    cyw43_arch_disable_sta_mode();
    start_ap();
}

static void do_forget(void)
{
    if (wifi_store_erase())
        printf("Stored credentials erased. Restarting.\n");
    else
        printf("WARNING: erase failed. Restarting anyway.\n");

    sleep_ms(200);              /* let the serial output flush */
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

/* -- main ------------------------------------------------------------------ */

static bool load_boot_creds(wifi_creds_t *creds)
{
    if (wifi_store_load(creds)) {
        have_stored = true;
        printf("Using stored credentials for '%s'\n", creds->ssid);
        return true;
    }

    if (WIFI_SSID[0] != '\0') {
        snprintf(creds->ssid, sizeof creds->ssid, "%s", WIFI_SSID);
        snprintf(creds->pass, sizeof creds->pass, "%s", WIFI_PASSWORD);
        printf("No stored network; using the compiled-in '%s'\n", creds->ssid);
        return true;
    }

    printf("No stored network and no compiled-in default.\n");
    return false;
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    adc_init();
    adc_set_temp_sensor_enabled(true);

    printf("Starting Wi-Fi...\n");
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_NETHERLANDS)) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    if (!http_server_start())
        return 1;

    wifi_creds_t creds;
    bool joined = false;

    if (load_boot_creds(&creds)) {
        cyw43_arch_enable_sta_mode();
        joined = join(&creds);
        if (!joined)
            cyw43_arch_disable_sta_mode();
    }

    if (joined) {
        printf("HTTP server listening on http://%s/\n", app_ip());
        printf("Open that address in a browser on the same network.\n\n");
    } else {
        snprintf(message, sizeof message,
                 "Not joined to any network yet — pick one below.");
        start_ap();
        printf("\n");
    }

    while (true) {
        wifi_scan_poll();

        if (want_scan && !wifi_scan_busy()) {
            want_scan = false;
            wifi_scan_start();
        }

        if (want_connect && time_reached(action_at)) {
            want_connect = false;
            do_connect();
        }

        if (want_forget && time_reached(action_at)) {
            want_forget = false;
            do_forget();
        }

        /* One blink when joined, two while hosting the setup access point. */
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(80);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        if (mode == APP_MODE_AP) {
            sleep_ms(120);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            sleep_ms(80);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(620);
        } else {
            sleep_ms(820);
        }
    }
}
