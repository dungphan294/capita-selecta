/*
 * ble_names — the knowledge a phone application has and a bare scanner does not.
 *
 * A BLE device transmits numbers: 16-bit UUIDs, company identifiers, and
 * characteristic values in whatever encoding the Bluetooth SIG specified for
 * that UUID. Nothing in the protocol says what any of it means. The lookup
 * tables and decoders here are that missing half — once the firmware carries
 * them, the console output is as readable as a scanner app's screen.
 */

#ifndef BLE_NAMES_H
#define BLE_NAMES_H

#include <stddef.h>
#include <stdint.h>

/* Assigned-number lookups. Return a static string, never NULL;
 * unknown numbers come back as "unknown". */
const char *ble_uuid16_name(uint16_t uuid16);
const char *ble_company_name(uint16_t company_id);
const char *ble_appearance_name(uint16_t appearance);

/* "READ WRITE NOTIFY" from an ATT property bitmap, into caller storage. */
void ble_properties_str(uint8_t properties, char *out, size_t out_size);

/*
 * Decode a characteristic value according to the GATT specification for its
 * UUID — battery level as a percentage, temperature as degrees Celsius, and
 * so on. Falls back to printable text, then to hex. Always NUL-terminates.
 */
void ble_decode_value(uint16_t uuid16, const uint8_t *value, uint16_t len,
                      char *out, size_t out_size);

/*
 * Print a full advertising or scan-response payload: every AD structure,
 * named and decoded, one per line, each prefixed by `indent` spaces.
 */
void ble_print_ad(const uint8_t *data, uint8_t len, int indent);

#endif /* BLE_NAMES_H */
