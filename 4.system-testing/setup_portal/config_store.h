/*
 * config_store — settings that survive a power cycle.
 *
 * The whole point of a provisioning portal is that you configure the board
 * once and it remembers. Credentials therefore cannot live in CMake defines
 * (which is what caused defect #4 in the WiFi project: an undefined CMake
 * variable expands to an empty string, silently). They live in the last
 * sector of flash, behind a magic number and a CRC, so a blank or corrupted
 * sector is detected rather than interpreted as valid empty credentials.
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define CFG_SSID_MAX    33   /* 32 octets + NUL */
#define CFG_PASS_MAX    64
#define CFG_NAME_MAX    32
#define CFG_ADDR_MAX    18   /* "AA:BB:CC:DD:EE:FF" + NUL */

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* WiFi station */
    char     wifi_ssid[CFG_SSID_MAX];
    char     wifi_pass[CFG_PASS_MAX];
    uint32_t wifi_auth;          /* a CYW43_AUTH_* value */

    /* BLE central */
    char     ble_target_name[CFG_NAME_MAX];
    char     ble_target_addr[CFG_ADDR_MAX];
    uint8_t  ble_scan_enabled;
    uint8_t  ble_connect_enabled;

    uint8_t  provisioned;        /* set once WiFi credentials have been saved */
    uint8_t  _pad;

    uint32_t crc;                /* over every byte above */
} portal_config_t;

/* Load from flash into the single in-RAM copy. Returns false and installs
 * defaults when the sector is blank or fails its CRC. */
bool config_load(void);

/* The live copy. Edit it, then call config_save(). */
portal_config_t *config(void);

/* Erase and rewrite the sector. Returns false if the write could not be made
 * safely or the read-back does not match. */
bool config_save(void);

/* Back to defaults in RAM and in flash. */
bool config_factory_reset(void);

#endif /* CONFIG_STORE_H */
