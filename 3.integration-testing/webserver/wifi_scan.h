/*
 * Cached Wi-Fi scan.
 *
 * cyw43_wifi_scan() is asynchronous: results arrive on a callback over a
 * couple of seconds. An HTTP handler cannot wait for that, so scans are
 * started from the main loop and the finished list is cached here for the
 * next page load.
 */

#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "wifi_store.h"   /* WIFI_SSID_MAX */

#define WIFI_SCAN_MAX 24

typedef struct {
    char     ssid[WIFI_SSID_MAX + 1];
    int16_t  rssi;        /* dBm */
    uint16_t channel;
    bool     secured;     /* false for an open network */
} wifi_ap_t;

/* Main context only — talks to the driver. */
void wifi_scan_start(void);

/* Call from the main loop. Latches completion and publishes the results. */
void wifi_scan_poll(void);

bool wifi_scan_busy(void);

/* Results from the last completed scan, strongest signal first. */
int wifi_scan_results(const wifi_ap_t **out);

/* Milliseconds since the last scan completed, or UINT32_MAX if never. */
uint32_t wifi_scan_age_ms(void);

#endif /* WIFI_SCAN_H */
