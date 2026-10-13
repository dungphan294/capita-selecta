/*
 * ble_portal — both LE roles at once.
 *
 *   peripheral : a provisioning GATT server, reachable with no network at all.
 *                This is the "connect over BLE first" half.
 *   central    : a passive scanner that decodes advertisements, so the portal
 *                page can show a pinned known sensor and a list of everything
 *                else in range.
 *
 * The two roles coexist because they use the radio at different moments:
 * advertising transmits in its own slots, scanning listens between them.
 */

#ifndef BLE_PORTAL_H
#define BLE_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#define BLE_MAX_SEEN 16

typedef struct {
    char     addr[18];
    char     name[24];
    int8_t   rssi;
    uint16_t seen;
    bool     has_temperature;    /* decoded from 0x181A service data */
    float    temperature_c;
    uint32_t last_ms;
} ble_device_t;

void  ble_portal_init(void);

/* Scanner view, for the portal page. */
int                 ble_device_count(void);
const ble_device_t *ble_device(int index);
const ble_device_t *ble_pinned_device(void);   /* NULL when not seen yet */
void                ble_set_target(const char *name);

/* Peripheral side. */
bool  ble_is_connected(void);
void  ble_set_status(const char *text);        /* notified to a subscriber */

#endif /* BLE_PORTAL_H */
