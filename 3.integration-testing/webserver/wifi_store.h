/*
 * Flash-backed Wi-Fi credential store.
 *
 * One reserved sector at the very top of flash holds the last network the
 * board was told to join. The program image starts at the bottom of flash and
 * never grows into it.
 */

#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <stdbool.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
} wifi_creds_t;

/* True if a valid record was found; *out is untouched otherwise. */
bool wifi_store_load(wifi_creds_t *out);

/* Erases the sector and writes the record. Verifies the readback.
 * Disables interrupts for the duration, so never call this with an HTTP
 * response still in flight. */
bool wifi_store_save(const wifi_creds_t *creds);

/* Erases the sector, so the next boot falls back to the compiled-in network. */
bool wifi_store_erase(void);

#endif /* WIFI_STORE_H */
