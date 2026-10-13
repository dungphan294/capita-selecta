#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "btstack.h"
#include "ble_names.h"

/* ------------------------------------------------------------------ */
/* Assigned numbers                                                    */
/* ------------------------------------------------------------------ */

/*
 * A deliberately partial table. The full Bluetooth SIG assigned-numbers list
 * runs to thousands of entries and would dominate the binary; these are the
 * ones that actually turn up on a desk. Everything else prints as its hex
 * UUID, which is still enough to look up by hand.
 */
struct uuid_name { uint16_t uuid; const char *name; };

static const struct uuid_name uuid16_table[] = {
    /* GATT services */
    { 0x1800, "Generic Access" },
    { 0x1801, "Generic Attribute" },
    { 0x1802, "Immediate Alert" },
    { 0x1803, "Link Loss" },
    { 0x1804, "Tx Power" },
    { 0x180A, "Device Information" },
    { 0x180D, "Heart Rate" },
    { 0x180F, "Battery Service" },
    { 0x1810, "Blood Pressure" },
    { 0x1812, "Human Interface Device" },
    { 0x1816, "Cycling Speed and Cadence" },
    { 0x1818, "Cycling Power" },
    { 0x181A, "Environmental Sensing" },
    { 0x181B, "Body Composition" },
    { 0x181C, "User Data" },
    { 0x181D, "Weight Scale" },
    { 0x1826, "Fitness Machine" },
    { 0x183B, "Binary Sensor" },
    { 0xFCD2, "Allterco / BTHome" },
    { 0xFE95, "Xiaomi Inc." },
    { 0xFE9F, "Google" },
    { 0xFEAA, "Google Eddystone" },

    /* GATT characteristics */
    { 0x2A00, "Device Name" },
    { 0x2A01, "Appearance" },
    { 0x2A04, "Peripheral Preferred Connection Parameters" },
    { 0x2A05, "Service Changed" },
    { 0x2A07, "Tx Power Level" },
    { 0x2A19, "Battery Level" },
    { 0x2A23, "System ID" },
    { 0x2A24, "Model Number String" },
    { 0x2A25, "Serial Number String" },
    { 0x2A26, "Firmware Revision String" },
    { 0x2A27, "Hardware Revision String" },
    { 0x2A28, "Software Revision String" },
    { 0x2A29, "Manufacturer Name String" },
    { 0x2A2B, "Current Time" },
    { 0x2A37, "Heart Rate Measurement" },
    { 0x2A38, "Body Sensor Location" },
    { 0x2A50, "PnP ID" },
    { 0x2A5B, "CSC Measurement" },
    { 0x2A63, "Cycling Power Measurement" },
    { 0x2A6D, "Pressure" },
    { 0x2A6E, "Temperature" },
    { 0x2A6F, "Humidity" },
    { 0x2A76, "UV Index" },
    { 0x2A77, "Irradiance" },
    { 0x2A7E, "Aerobic Heart Rate Lower Limit" },
    { 0x2A98, "Weight" },
    { 0x2A9D, "Weight Measurement" },
    { 0x2AF9, "General Device Info" },

    /* Descriptors, which show up in a full dump */
    { 0x2901, "Characteristic User Description" },
    { 0x2902, "Client Characteristic Configuration" },
    { 0x2904, "Characteristic Presentation Format" },
};

const char *ble_uuid16_name(uint16_t uuid16)
{
    for (size_t i = 0; i < sizeof(uuid16_table) / sizeof(uuid16_table[0]); i++)
        if (uuid16_table[i].uuid == uuid16) return uuid16_table[i].name;
    return "unknown";
}

static const struct uuid_name company_table[] = {
    { 0x0006, "Microsoft" },
    { 0x004C, "Apple, Inc." },
    { 0x0075, "Samsung Electronics" },
    { 0x00E0, "Google" },
    { 0x0157, "Anhui Huami (Amazfit)" },
    { 0x0171, "Amazon" },
    { 0x0499, "Ruuvi Innovations" },
    { 0x05A7, "Sonos" },
    { 0x0A09, "Allterco Robotics (Shelly)" },
    { 0x038F, "Xiaomi" },
    { 0x0059, "Nordic Semiconductor" },
    { 0x02E5, "Espressif" },
    { 0x0D00, "Raspberry Pi Ltd" },
};

const char *ble_company_name(uint16_t company_id)
{
    for (size_t i = 0; i < sizeof(company_table) / sizeof(company_table[0]); i++)
        if (company_table[i].uuid == company_id) return company_table[i].name;
    return "unknown";
}

const char *ble_appearance_name(uint16_t appearance)
{
    /* Appearance is a 10-bit category plus a 6-bit subcategory. The category
     * alone is the useful part. */
    switch (appearance >> 6) {
    case 0x000: return "Unknown";
    case 0x001: return "Phone";
    case 0x002: return "Computer";
    case 0x003: return "Watch";
    case 0x005: return "Display";
    case 0x00A: return "Thermometer";
    case 0x00B: return "Heart Rate Sensor";
    case 0x00C: return "Blood Pressure";
    case 0x00D: return "Human Interface Device";
    case 0x00E: return "Glucose Meter";
    case 0x010: return "Cycling";
    case 0x015: return "Weight Scale";
    case 0x021: return "Sensor";
    case 0x040: return "Earbud";
    default:    return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* ATT properties                                                      */
/* ------------------------------------------------------------------ */

void ble_properties_str(uint8_t properties, char *out, size_t out_size)
{
    static const char *names[8] = {
        "BROADCAST", "READ", "WRITE_NR", "WRITE",
        "NOTIFY", "INDICATE", "SIGNED_WRITE", "EXT_PROP"
    };

    out[0] = '\0';
    size_t used = 0;

    for (int bit = 0; bit < 8; bit++) {
        if ((properties & (1u << bit)) == 0) continue;

        size_t need = strlen(names[bit]) + (used ? 1 : 0);
        if (used + need + 1 >= out_size) break;

        if (used) out[used++] = ' ';
        strcpy(out + used, names[bit]);
        used += strlen(names[bit]);
    }

    if (used == 0 && out_size > 1) strcpy(out, "-");
}

/* ------------------------------------------------------------------ */
/* Value decoding                                                      */
/* ------------------------------------------------------------------ */

static void hex_dump(const uint8_t *v, uint16_t len, char *out, size_t out_size)
{
    size_t used = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (used + 4 >= out_size) { /* elide the tail rather than truncate mid-byte */
            if (used + 4 < out_size + 4 && used >= 4) strcpy(out + used - 4, " ...");
            break;
        }
        used += (size_t) snprintf(out + used, out_size - used, "%s%02X",
                                  i ? " " : "", v[i]);
    }
    out[used < out_size ? used : out_size - 1] = '\0';
}

/* True when every byte is printable ASCII — the test for a GATT string. */
static int is_text(const uint8_t *v, uint16_t len)
{
    if (len == 0) return 0;
    for (uint16_t i = 0; i < len; i++)
        if (v[i] < 0x20 || v[i] > 0x7E) return 0;
    return 1;
}

static void copy_text(const uint8_t *v, uint16_t len, char *out, size_t out_size)
{
    size_t n = (len < out_size - 1) ? len : out_size - 1;
    memcpy(out, v, n);
    out[n] = '\0';
}

void ble_decode_value(uint16_t uuid16, const uint8_t *v, uint16_t len,
                      char *out, size_t out_size)
{
    if (out_size == 0) return;
    if (len == 0) { strcpy(out, "(empty)"); return; }

    switch (uuid16) {

    /* Strings, per the Device Information Service. */
    case 0x2A00: case 0x2A24: case 0x2A25: case 0x2A26:
    case 0x2A27: case 0x2A28: case 0x2A29: case 0x2901:
        copy_text(v, len, out, out_size);
        return;

    /* uint8 percentage. */
    case 0x2A19:
        if (len >= 1) { snprintf(out, out_size, "%u %%", v[0]); return; }
        break;

    /* sint8, decibel-milliwatts. */
    case 0x2A07:
        if (len >= 1) { snprintf(out, out_size, "%d dBm", (int8_t) v[0]); return; }
        break;

    /* sint16, 0.01 degC resolution. */
    case 0x2A6E:
        if (len >= 2) {
            int16_t raw = (int16_t) little_endian_read_16(v, 0);
            snprintf(out, out_size, "%d.%02u degC", raw / 100,
                     (unsigned) (raw < 0 ? -raw : raw) % 100);
            return;
        }
        break;

    /* uint16, 0.01 % resolution. */
    case 0x2A6F:
        if (len >= 2) {
            uint16_t raw = little_endian_read_16(v, 0);
            snprintf(out, out_size, "%u.%02u %% RH", raw / 100, raw % 100);
            return;
        }
        break;

    /* uint32, 0.1 Pa resolution. */
    case 0x2A6D:
        if (len >= 4) {
            uint32_t raw = little_endian_read_32(v, 0);
            snprintf(out, out_size, "%lu.%lu Pa",
                     (unsigned long) raw / 10, (unsigned long) raw % 10);
            return;
        }
        break;

    case 0x2A01:
        if (len >= 2) {
            uint16_t a = little_endian_read_16(v, 0);
            snprintf(out, out_size, "0x%04X (%s)", a, ble_appearance_name(a));
            return;
        }
        break;

    /* Heart Rate Measurement: a flags byte, then 8- or 16-bit BPM. */
    case 0x2A37:
        if (len >= 2) {
            unsigned bpm = (v[0] & 0x01) && len >= 3
                         ? little_endian_read_16(v, 1) : v[1];
            snprintf(out, out_size, "%u bpm", bpm);
            return;
        }
        break;

    /* PnP ID: vendor source, vendor, product, version. */
    case 0x2A50:
        if (len >= 7) {
            snprintf(out, out_size,
                     "vendor 0x%04X (%s), product 0x%04X, version %u.%u.%u",
                     little_endian_read_16(v, 1),
                     v[0] == 1 ? "Bluetooth SIG" : "USB-IF",
                     little_endian_read_16(v, 3),
                     v[6], v[5] >> 4, v[5] & 0x0F);
            return;
        }
        break;

    /* Peripheral Preferred Connection Parameters. */
    case 0x2A04:
        if (len >= 8) {
            snprintf(out, out_size,
                     "interval %u-%u (x1.25 ms), latency %u, timeout %u (x10 ms)",
                     little_endian_read_16(v, 0), little_endian_read_16(v, 2),
                     little_endian_read_16(v, 4), little_endian_read_16(v, 6));
            return;
        }
        break;

    default:
        break;
    }

    /* No specification to apply: show it as text if it reads as text, hex if not. */
    if (is_text(v, len)) copy_text(v, len, out, out_size);
    else                 hex_dump(v, len, out, out_size);
}

/* ------------------------------------------------------------------ */
/* Advertising payload                                                 */
/* ------------------------------------------------------------------ */

static void print_flags(uint8_t f, int indent)
{
    printf("%*sFlags                 0x%02X%s%s%s%s\n", indent, "", f,
           (f & 0x01) ? " LE-limited-discoverable" : "",
           (f & 0x02) ? " LE-general-discoverable" : "",
           (f & 0x04) ? " BR/EDR-not-supported"    : "",
           (f & 0x18) ? " simultaneous-LE/BR-EDR"  : "");
}

static void print_uuid16_list(const uint8_t *d, uint8_t len, int indent,
                              const char *label)
{
    for (uint8_t i = 0; i + 1 < len; i += 2) {
        uint16_t u = little_endian_read_16(d, i);
        printf("%*s%-22s0x%04X  %s\n", indent, "", i ? "" : label,
               u, ble_uuid16_name(u));
    }
}

void ble_print_ad(const uint8_t *data, uint8_t len, int indent)
{
    ad_context_t ctx;

    for (ad_iterator_init(&ctx, len, data);
         ad_iterator_has_more(&ctx);
         ad_iterator_next(&ctx))
    {
        uint8_t        type = ad_iterator_get_data_type(&ctx);
        uint8_t        dlen = ad_iterator_get_data_len(&ctx);
        const uint8_t *d    = ad_iterator_get_data(&ctx);

        char buf[96];

        switch (type) {
        case BLUETOOTH_DATA_TYPE_FLAGS:
            if (dlen >= 1) print_flags(d[0], indent);
            break;

        case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
        case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME:
            copy_text(d, dlen, buf, sizeof(buf));
            printf("%*s%-22s%s\n", indent, "",
                   type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME
                       ? "Local name" : "Local name (short)", buf);
            break;

        case BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
            print_uuid16_list(d, dlen, indent, "Services (partial)");
            break;
        case BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
            print_uuid16_list(d, dlen, indent, "Services");
            break;

        case BLUETOOTH_DATA_TYPE_TX_POWER_LEVEL:
            if (dlen >= 1)
                printf("%*s%-22s%d dBm\n", indent, "", "Tx power", (int8_t) d[0]);
            break;

        case BLUETOOTH_DATA_TYPE_APPEARANCE:
            if (dlen >= 2) {
                uint16_t a = little_endian_read_16(d, 0);
                printf("%*s%-22s0x%04X  %s\n", indent, "", "Appearance",
                       a, ble_appearance_name(a));
            }
            break;

        /*
         * Service data is where connection-free sensors put their readings —
         * BTHome, Xiaomi, Eddystone. The UUID says whose format it is.
         */
        case BLUETOOTH_DATA_TYPE_SERVICE_DATA:
            if (dlen >= 2) {
                uint16_t u = little_endian_read_16(d, 0);
                hex_dump(d + 2, (uint16_t) (dlen - 2), buf, sizeof(buf));
                printf("%*s%-22s0x%04X (%s)  %s\n", indent, "", "Service data",
                       u, ble_uuid16_name(u), buf);
            }
            break;

        /*
         * Manufacturer-specific data: the first two bytes are an assigned
         * company identifier, the rest is undocumented by design. This is
         * precisely the payload a vendor's own application knows how to read
         * and nobody else does.
         */
        case BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA:
            if (dlen >= 2) {
                uint16_t c = little_endian_read_16(d, 0);
                hex_dump(d + 2, (uint16_t) (dlen - 2), buf, sizeof(buf));
                printf("%*s%-22s0x%04X (%s)  %s\n", indent, "", "Manufacturer",
                       c, ble_company_name(c), buf);
            }
            break;

        default:
            hex_dump(d, dlen, buf, sizeof(buf));
            printf("%*sAD type 0x%02X          %s\n", indent, "", type, buf);
            break;
        }
    }
}
