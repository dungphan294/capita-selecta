/*
 * wifi_manager — scanning, joining, and access-point mode.
 *
 * Joining is asynchronous on purpose. The portal has to keep serving pages
 * while an attempt is in progress, and the user has to be able to see *why*
 * an attempt failed. A blocking connect with a single error code is how the
 * WiFi project ended up unable to distinguish "wrong password" from
 * "5 GHz network" from "SSID not present" — all three returned -2.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_MAX_SCAN_RESULTS 24

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    int16_t rssi;
    uint16_t channel;
    uint8_t auth_mode;
    bool    enterprise;   /* best-effort: see wifi_ssid_is_enterprise() */
} wifi_ap_t;

typedef enum {
    WIFI_IDLE,          /* nothing attempted yet */
    WIFI_AP_MODE,       /* we are the access point */
    WIFI_JOINING,
    WIFI_JOINED,
    WIFI_FAILED,
} wifi_state_t;

void         wifi_manager_init(void);

/* Scanning. Non-blocking; poll wifi_scan_busy(). */
bool         wifi_scan_start(void);
bool         wifi_scan_busy(void);
int          wifi_scan_count(void);
const wifi_ap_t *wifi_scan_result(int index);

/* Station mode. Non-blocking; call wifi_poll() regularly. */
bool         wifi_join(const char *ssid, const char *pass, uint32_t auth);
void         wifi_poll(void);
wifi_state_t wifi_state(void);
const char  *wifi_state_str(void);
const char  *wifi_fail_reason(void);   /* "" unless state is WIFI_FAILED */
const char  *wifi_ip(void);            /* "0.0.0.0" when not joined */

/* Access-point mode, for configuring the board before it has any network. */
bool         wifi_start_ap(const char *ssid, const char *pass);
void         wifi_stop_ap(void);

/* Helpers for the portal page. */
const char  *wifi_auth_name(uint8_t auth_mode);

/*
 * True for SSIDs known to be WPA2-Enterprise (802.1X).
 *
 * This is a name-based list, not a measurement. The CYW43 driver reports an
 * auth_mode taken from the beacon's capability bits without parsing the RSN
 * information element, so an enterprise network is indistinguishable from
 * WPA2-PSK in the scan result. Since the radio cannot join either kind of
 * enterprise network, naming the common ones is more useful than a
 * measurement that cannot be made.
 */
bool         wifi_ssid_is_enterprise(const char *ssid);

#endif /* WIFI_MANAGER_H */
