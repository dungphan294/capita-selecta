#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "wifi_store.h"

#define STORE_MAGIC   0x50574946u   /* 'PWIF' — a record we wrote */
#define STORE_VERSION 1u

/* Last sector of flash. The linker places the image from the bottom up. */
#define STORE_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    char     ssid[WIFI_SSID_MAX + 1];
    char     pass[WIFI_PASS_MAX + 1];
    uint32_t crc;               /* over every byte before this field */
} record_t;

_Static_assert(sizeof(record_t) <= FLASH_PAGE_SIZE,
               "record must fit in a single flash page");

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static const record_t *stored(void)
{
    return (const record_t *)(XIP_BASE + STORE_OFFSET);
}

bool wifi_store_load(wifi_creds_t *out)
{
    const record_t *r = stored();

    if (r->magic != STORE_MAGIC || r->version != STORE_VERSION)
        return false;

    const size_t covered = offsetof(record_t, crc);
    if (crc32(r, covered) != r->crc) {
        printf("wifi_store: checksum mismatch, ignoring stored credentials\n");
        return false;
    }

    /* A truncated write could leave an unterminated string. */
    if (r->ssid[WIFI_SSID_MAX] != '\0' || r->pass[WIFI_PASS_MAX] != '\0')
        return false;
    if (r->ssid[0] == '\0')
        return false;

    memcpy(out->ssid, r->ssid, sizeof out->ssid);
    memcpy(out->pass, r->pass, sizeof out->pass);
    return true;
}

static bool write_page(const void *src, size_t len)
{
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    if (src) memcpy(page, src, len);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(STORE_OFFSET, FLASH_SECTOR_SIZE);
    if (src) flash_range_program(STORE_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    if (!src)
        return stored()->magic != STORE_MAGIC;

    return memcmp((const void *)(XIP_BASE + STORE_OFFSET), src, len) == 0;
}

bool wifi_store_save(const wifi_creds_t *creds)
{
    record_t rec;
    memset(&rec, 0, sizeof rec);
    rec.magic   = STORE_MAGIC;
    rec.version = STORE_VERSION;

    /* creds fields are already bounded and NUL-terminated by the caller. */
    snprintf(rec.ssid, sizeof rec.ssid, "%s", creds->ssid);
    snprintf(rec.pass, sizeof rec.pass, "%s", creds->pass);
    rec.crc = crc32(&rec, offsetof(record_t, crc));

    return write_page(&rec, sizeof rec);
}

bool wifi_store_erase(void)
{
    return write_page(NULL, 0);
}
