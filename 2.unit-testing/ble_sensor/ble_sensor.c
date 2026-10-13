/*
 * Pico 2 W — reference BLE sensor
 *
 * The known-good target for the other two projects. It reads the RP2350's
 * on-die temperature and publishes it twice, by the two different mechanisms
 * a BLE device has:
 *
 *   broadcast   Environmental Sensing service data in every advertisement.
 *               Readable by any scanner, with no connection, no GATT client
 *               and no pairing. This is what setup_portal picks up.
 *
 *   connection  a GATT Temperature characteristic (0x2A6E), plus Device
 *               Information and Battery services. Requires a central with a
 *               GATT client. This is what ble_central reads.
 *
 * Both carry the same number in the same encoding — sint16, centi-degrees
 * Celsius — so disagreement between them is a fault in the reader, not in
 * the source. That is what makes this useful for verification rather than
 * just for demonstration.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "btstack.h"

#include "sensor_gatt.h"

#define H_TEMP     ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_TEMPERATURE_01_VALUE_HANDLE
#define H_TEMP_CCC ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_TEMPERATURE_01_CLIENT_CONFIGURATION_HANDLE
#define H_BATTERY  ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_BATTERY_LEVEL_01_VALUE_HANDLE

#define DEVICE_NAME     "Pico2W-Sensor"
#define UPDATE_MS       1000
#define ADC_VREF        3.3f
#define ADC_RESOLUTION  (1 << 12)

static btstack_packet_callback_registration_t hci_event_registration;
static btstack_timer_source_t                 update_timer;

static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static bool             subscribed = false;

static int16_t  temperature_centi = 0;   /* sint16, 0.01 degC — the GATT encoding */
static uint8_t  battery_level     = 100;
static uint32_t update_count      = 0;

/* -------------------------------------------------------------- */
/* Sensor                                                          */
/* -------------------------------------------------------------- */

/* Identical to the internal_temperature project: 16 samples averaged, then
 * the datasheet conversion at the sensor's own calibration point. */
static float read_celsius(void)
{
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    float voltage = (sum / 16.0f) * ADC_VREF / ADC_RESOLUTION;
    return 27.0f - (voltage - 0.706f) / 0.001721f;
}

/* -------------------------------------------------------------- */
/* Advertising                                                     */
/* -------------------------------------------------------------- */

/*
 * The payload, laid out by hand so the service-data bytes can be patched in
 * place on every update rather than rebuilt:
 *
 *   02 01 06                       flags
 *   05 16 1A18 <lo> <hi>           service data, UUID 0x181A, sint16 centi-degC
 *   0E 09 "Pico2W-Sensor"          complete local name
 *
 * 3 + 7 + 15 = 25 bytes, inside the 31-byte limit.
 */
static uint8_t adv_data[] = {
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,

    0x05, BLUETOOTH_DATA_TYPE_SERVICE_DATA,
    0x1A, 0x18,              /* 0x181A Environmental Sensing, little-endian */
    0x00, 0x00,              /* temperature, patched by update() */

    0x0E, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P','i','c','o','2','W','-','S','e','n','s','o','r',
};

#define ADV_TEMP_OFFSET 7    /* index of the low byte above */

static void start_advertising(void)
{
    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));

    /* 0x00A0 units of 0.625 ms = 100 ms between advertisements. */
    gap_advertisements_set_params(0x00A0, 0x00A0, 0 /* ADV_IND */, 0,
                                  null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), adv_data);
    gap_advertisements_enable(1);

    printf("Advertising as '%s'\n", DEVICE_NAME);
}

/* -------------------------------------------------------------- */
/* GATT server                                                     */
/* -------------------------------------------------------------- */

static uint16_t att_read_callback(hci_con_handle_t handle, uint16_t att_handle,
                                  uint16_t offset, uint8_t *buffer,
                                  uint16_t buffer_size)
{
    UNUSED(handle);

    if (att_handle == H_TEMP) {
        uint8_t v[2];
        little_endian_store_16(v, 0, (uint16_t)temperature_centi);
        return att_read_callback_handle_blob(v, sizeof(v), offset,
                                             buffer, buffer_size);
    }

    if (att_handle == H_BATTERY)
        return att_read_callback_handle_blob(&battery_level, 1, offset,
                                             buffer, buffer_size);

    return 0;
}

static int att_write_callback(hci_con_handle_t handle, uint16_t att_handle,
                              uint16_t transaction_mode, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size)
{
    UNUSED(transaction_mode);
    UNUSED(offset);
    UNUSED(buffer_size);

    if (att_handle == H_TEMP_CCC) {
        subscribed = little_endian_read_16(buffer, 0) ==
                     GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
        con_handle = handle;
        printf("Temperature notifications %s\n", subscribed ? "on" : "off");
    }
    return 0;
}

/* -------------------------------------------------------------- */

static void update(btstack_timer_source_t *ts)
{
    float c = read_celsius();
    temperature_centi = (int16_t)(c * 100.0f);
    update_count++;

    /* Patch the advertisement in place and hand the controller the new
     * payload. Every scanner in range now sees the current reading without
     * connecting to anything. */
    little_endian_store_16(adv_data, ADV_TEMP_OFFSET, (uint16_t)temperature_centi);
    gap_advertisements_set_data(sizeof(adv_data), adv_data);

    if (con_handle != HCI_CON_HANDLE_INVALID && subscribed)
        att_server_request_can_send_now_event(con_handle);

    if (update_count % 10 == 1)
        printf("[%lu] %.2f degC  (advertised as 0x%04X)\n",
               (unsigned long)update_count, (double)c,
               (unsigned)(uint16_t)temperature_centi);

    btstack_run_loop_set_timer(ts, UPDATE_MS);
    btstack_run_loop_add_timer(ts);
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
        {
            bd_addr_t local;
            gap_local_bd_addr(local);
            printf("Bluetooth ready. Local address %s\n", bd_addr_to_str(local));
        }
        start_advertising();

        btstack_run_loop_set_timer_handler(&update_timer, &update);
        btstack_run_loop_set_timer(&update_timer, UPDATE_MS);
        btstack_run_loop_add_timer(&update_timer);
        break;

    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) ==
            GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            printf("Central connected\n");
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("Central disconnected\n");
        con_handle = HCI_CON_HANDLE_INVALID;
        subscribed = false;
        gap_advertisements_enable(1);   /* advertising stops on connect */
        break;

    case ATT_EVENT_CAN_SEND_NOW:
        if (con_handle != HCI_CON_HANDLE_INVALID && subscribed) {
            uint8_t v[2];
            little_endian_store_16(v, 0, (uint16_t)temperature_centi);
            att_server_notify(con_handle, H_TEMP, v, sizeof(v));
        }
        break;

    default:
        break;
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);

    printf("\nPico 2 W — reference BLE sensor\n");

    adc_init();
    adc_set_temp_sensor_enabled(true);

    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    l2cap_init();
    sm_init();
    att_server_init(profile_data, att_read_callback, att_write_callback);

    hci_event_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_registration);
    att_server_register_packet_handler(&packet_handler);

    hci_power_control(HCI_POWER_ON);

    btstack_run_loop_execute();
    return 0;
}
