#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "wifi_scan.h"

/* Filled by the driver callback as results stream in. */
static wifi_ap_t staging[WIFI_SCAN_MAX];
static int       staging_count;

/* Published to readers once a scan completes. */
static wifi_ap_t published[WIFI_SCAN_MAX];
static int       published_count;

static bool     scanning;
static uint32_t completed_at_ms;
static bool     ever_completed;

static int scan_result(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;

    if (r->ssid_len == 0)                      /* hidden network */
        return 0;

    char ssid[WIFI_SSID_MAX + 1];
    size_t n = r->ssid_len > WIFI_SSID_MAX ? WIFI_SSID_MAX : r->ssid_len;
    memcpy(ssid, r->ssid, n);
    ssid[n] = '\0';

    /* The same AP is reported once per channel it beacons on; keep the
     * strongest sighting of each name. */
    for (int i = 0; i < staging_count; i++) {
        if (strcmp(staging[i].ssid, ssid) == 0) {
            if (r->rssi > staging[i].rssi) {
                staging[i].rssi    = r->rssi;
                staging[i].channel = r->channel;
            }
            return 0;
        }
    }

    if (staging_count == WIFI_SCAN_MAX)
        return 0;

    wifi_ap_t *ap = &staging[staging_count++];
    snprintf(ap->ssid, sizeof ap->ssid, "%s", ssid);
    ap->rssi    = r->rssi;
    ap->channel = r->channel;
    ap->secured = r->auth_mode != 0;
    return 0;
}

void wifi_scan_start(void)
{
    if (scanning)
        return;

    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof opts);

    staging_count = 0;

    cyw43_arch_lwip_begin();
    int err = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result);
    cyw43_arch_lwip_end();

    if (err != 0) {
        printf("scan failed to start (%d)\n", err);
        return;
    }
    scanning = true;
    printf("Scanning...\n");
}

void wifi_scan_poll(void)
{
    if (!scanning)
        return;

    cyw43_arch_lwip_begin();
    bool active = cyw43_wifi_scan_active(&cyw43_state);
    cyw43_arch_lwip_end();

    if (active)
        return;

    /* Strongest first — a simple insertion sort over at most 24 entries. */
    for (int i = 1; i < staging_count; i++) {
        wifi_ap_t key = staging[i];
        int j = i - 1;
        while (j >= 0 && staging[j].rssi < key.rssi) {
            staging[j + 1] = staging[j];
            j--;
        }
        staging[j + 1] = key;
    }

    cyw43_arch_lwip_begin();
    memcpy(published, staging, sizeof published);
    published_count = staging_count;
    cyw43_arch_lwip_end();

    scanning        = false;
    ever_completed  = true;
    completed_at_ms = to_ms_since_boot(get_absolute_time());
    printf("Scan complete: %d network%s\n",
           published_count, published_count == 1 ? "" : "s");
}

bool wifi_scan_busy(void)
{
    return scanning;
}

int wifi_scan_results(const wifi_ap_t **out)
{
    *out = published;
    return published_count;
}

uint32_t wifi_scan_age_ms(void)
{
    if (!ever_completed)
        return UINT32_MAX;
    return to_ms_since_boot(get_absolute_time()) - completed_at_ms;
}
