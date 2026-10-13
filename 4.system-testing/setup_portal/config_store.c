#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "config_store.h"

#define CONFIG_MAGIC    0x50434647u   /* "PCFG" */
#define CONFIG_VERSION  1u

/*
 * Where in flash this lives, counting sectors back from the end:
 *
 *   -1  reserved — the RP2350 errata E10 workaround wants the last sector
 *       free, and the SDK's own BTstack flash bank deliberately skips it
 *   -2  \ BTstack's link-key store (PICO_FLASH_BANK_TOTAL_SIZE, 2 sectors),
 *   -3  / claimed whenever pico_btstack_flash_bank is linked
 *   -4  <- this configuration
 *
 * Sector -4 is therefore the first one that is safe both today and if
 * bonding keys are persisted later. FLASH_SECTOR_SIZE is 4096 and is the
 * smallest erasable unit, so one sector is the minimum cost of storing
 * anything at all.
 */
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (4u * FLASH_SECTOR_SIZE))
#define CONFIG_FLASH_ADDR   ((const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET))

static portal_config_t live;

/* Plain bitwise CRC-32: a table would cost more flash than the time it saves
 * on a 128-byte structure written once per configuration change. */
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint32_t config_crc(const portal_config_t *c)
{
    return crc32(c, offsetof(portal_config_t, crc));
}

static void install_defaults(void)
{
    memset(&live, 0, sizeof(live));
    live.magic              = CONFIG_MAGIC;
    live.version            = CONFIG_VERSION;
    live.wifi_auth          = 4;   /* CYW43_AUTH_WPA2_AES_PSK, see wifi_manager.h */
    live.ble_scan_enabled   = 1;
    live.ble_connect_enabled = 1;
    live.provisioned        = 0;
}

bool config_load(void)
{
    portal_config_t stored;
    memcpy(&stored, CONFIG_FLASH_ADDR, sizeof(stored));

    if (stored.magic != CONFIG_MAGIC) {
        printf("config: no saved configuration (magic 0x%08lX), using defaults\n",
               (unsigned long)stored.magic);
        install_defaults();
        return false;
    }

    if (stored.version != CONFIG_VERSION) {
        printf("config: version %lu on flash, this build expects %u — defaults\n",
               (unsigned long)stored.version, CONFIG_VERSION);
        install_defaults();
        return false;
    }

    uint32_t want = config_crc(&stored);
    if (stored.crc != want) {
        printf("config: CRC mismatch (0x%08lX on flash, 0x%08lX computed) — defaults\n",
               (unsigned long)stored.crc, (unsigned long)want);
        install_defaults();
        return false;
    }

    /* Defensive: a corrupt-but-correctly-CRC'd record must still not be able
     * to hand unterminated strings to the rest of the program. */
    live = stored;
    live.wifi_ssid[CFG_SSID_MAX - 1]       = '\0';
    live.wifi_pass[CFG_PASS_MAX - 1]       = '\0';
    live.ble_target_name[CFG_NAME_MAX - 1] = '\0';
    live.ble_target_addr[CFG_ADDR_MAX - 1] = '\0';

    printf("config: loaded. SSID '%s', provisioned %s\n",
           live.wifi_ssid, live.provisioned ? "yes" : "no");
    return true;
}

portal_config_t *config(void) { return &live; }

/*
 * Runs with interrupts disabled and the other core parked. Nothing in here
 * may touch flash for any other reason — no printf to a flash-resident
 * string, no function call that might not be in RAM. Keep it to the two
 * flash primitives, which are themselves copied to RAM by the SDK.
 */
static void flash_write_op(void *param)
{
    const uint8_t *page = (const uint8_t *)param;
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
}

bool config_save(void)
{
    /* flash_range_program writes whole 256-byte pages, so the record is
     * copied into a page-sized, correctly-aligned buffer first. */
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

    _Static_assert(sizeof(portal_config_t) <= FLASH_PAGE_SIZE,
                   "configuration no longer fits in one flash page");

    live.magic   = CONFIG_MAGIC;
    live.version = CONFIG_VERSION;
    live.crc     = config_crc(&live);

    memset(page, 0xFF, sizeof(page));
    memcpy(page, &live, sizeof(live));

    int rc = flash_safe_execute(flash_write_op, page, 1000);
    if (rc != PICO_OK) {
        printf("config: flash_safe_execute failed (%d)\n", rc);
        return false;
    }

    /* Read back rather than trust the write. */
    if (memcmp(CONFIG_FLASH_ADDR, &live, sizeof(live)) != 0) {
        printf("config: read-back mismatch after save\n");
        return false;
    }

    printf("config: saved (%u bytes, CRC 0x%08lX)\n",
           (unsigned)sizeof(live), (unsigned long)live.crc);
    return true;
}

bool config_factory_reset(void)
{
    install_defaults();
    return config_save();
}
