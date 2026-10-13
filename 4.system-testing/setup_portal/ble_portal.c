#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"

#include "ble_portal.h"
#include "config_store.h"
#include "portal_actions.h"
#include "portal_gatt.h"

/* The generated handle names are unreadable at the point of use. */
#define H_SSID    ATT_CHARACTERISTIC_4A7F0001_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_VALUE_HANDLE
#define H_PASS    ATT_CHARACTERISTIC_4A7F0002_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_VALUE_HANDLE
#define H_CMD     ATT_CHARACTERISTIC_4A7F0003_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_VALUE_HANDLE
#define H_STATUS  ATT_CHARACTERISTIC_4A7F0004_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_VALUE_HANDLE
#define H_STATUS_CCC \
    ATT_CHARACTERISTIC_4A7F0004_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_CLIENT_CONFIGURATION_HANDLE
#define H_URL     ATT_CHARACTERISTIC_4A7F0005_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_VALUE_HANDLE
#define H_SCAN    ATT_CHARACTERISTIC_4A7F0006_1C2B_4F7A_9D3E_0A1B2C3D4E5F_01_VALUE_HANDLE

#define SCAN_INTERVAL 0x00A0   /* 100 ms, in 0.625 ms units */
#define SCAN_WINDOW   0x0030   /* 30 ms listening, leaving room to advertise */
#define DEVICE_NAME   "Pico2W-Setup"

/* ---------------------------------------------------------------- */
/* State                                                             */
/* ---------------------------------------------------------------- */

static btstack_packet_callback_registration_t hci_event_registration;
static btstack_packet_callback_registration_t att_event_registration;

static hci_con_handle_t con_handle   = HCI_CON_HANDLE_INVALID;
static bool             status_subscribed = false;
static char             status_text[96] = "starting";

static ble_device_t devices[BLE_MAX_SEEN];
static int          device_count = 0;
static char         target_name[CFG_NAME_MAX] = "";

/* ---------------------------------------------------------------- */
/* Advertising                                                       */
/* ---------------------------------------------------------------- */

static uint8_t adv_data[] = {
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    sizeof(DEVICE_NAME), BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P','i','c','o','2','W','-','S','e','t','u','p',
};

/*
 * The 128-bit service UUID will not fit alongside the name in one 31-byte
 * advertisement, so it goes in the scan response. An active scanner asks for
 * it; a passive one still sees the name.
 */
static const uint8_t scan_resp_data[] = {
    0x11, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    /* 4A7F0000-1C2B-4F7A-9D3E-0A1B2C3D4E5F, little-endian on the wire */
    0x5F,0x4E,0x3D,0x2C,0x1B,0x0A, 0x3E,0x9D, 0x7A,0x4F, 0x2B,0x1C,
    0x00,0x00,0x7F,0x4A,
};

static void start_advertising(void)
{
    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));

    gap_advertisements_set_params(0x0080, 0x0080, 0 /* ADV_IND */, 0,
                                  null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), adv_data);
    gap_scan_response_set_data(sizeof(scan_resp_data), (uint8_t *)scan_resp_data);
    gap_advertisements_enable(1);

    printf("ble: advertising as '%s'\n", DEVICE_NAME);
}

/* ---------------------------------------------------------------- */
/* Scanner                                                           */
/* ---------------------------------------------------------------- */

static ble_device_t *find_or_add(const char *addr)
{
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].addr, addr) == 0) return &devices[i];

    if (device_count >= BLE_MAX_SEEN) {
        /* Replace the stalest entry so a busy room cannot freeze the list. */
        int oldest = 0;
        for (int i = 1; i < device_count; i++)
            if (devices[i].last_ms < devices[oldest].last_ms) oldest = i;
        memset(&devices[oldest], 0, sizeof(devices[oldest]));
        strncpy(devices[oldest].addr, addr, sizeof(devices[oldest].addr) - 1);
        return &devices[oldest];
    }

    ble_device_t *d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->addr, addr, sizeof(d->addr) - 1);
    return d;
}

static void parse_advertisement(ble_device_t *d, const uint8_t *data, uint8_t len)
{
    ad_context_t ctx;

    for (ad_iterator_init(&ctx, len, data);
         ad_iterator_has_more(&ctx);
         ad_iterator_next(&ctx))
    {
        uint8_t        type  = ad_iterator_get_data_type(&ctx);
        uint8_t        dlen  = ad_iterator_get_data_len(&ctx);
        const uint8_t *value = ad_iterator_get_data(&ctx);

        if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
            type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME)
        {
            size_t n = dlen < sizeof(d->name) - 1 ? dlen : sizeof(d->name) - 1;
            memcpy(d->name, value, n);
            d->name[n] = '\0';
            continue;
        }

        /*
         * Environmental Sensing service data. This is a reading obtained
         * without connecting, without pairing and without a GATT client —
         * the cheapest way for a sensor to publish, and the companion
         * ble_sensor project transmits exactly this layout:
         *   [0..1] UUID 0x181A   [2..3] sint16 centi-degrees C
         */
        if (type == BLUETOOTH_DATA_TYPE_SERVICE_DATA && dlen >= 4) {
            if (little_endian_read_16(value, 0) == ORG_BLUETOOTH_SERVICE_ENVIRONMENTAL_SENSING) {
                int16_t centi = (int16_t)little_endian_read_16(value, 2);
                d->has_temperature = true;
                d->temperature_c   = centi / 100.0f;
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* GATT server callbacks                                             */
/* ---------------------------------------------------------------- */

static int scan_line_cursor = 0;

static uint16_t att_read_callback(hci_con_handle_t handle, uint16_t att_handle,
                                  uint16_t offset, uint8_t *buffer,
                                  uint16_t buffer_size)
{
    UNUSED(handle);

    switch (att_handle) {
    case H_SSID:
        return att_read_callback_handle_blob(
            (const uint8_t *)config()->wifi_ssid,
            (uint16_t)strlen(config()->wifi_ssid), offset, buffer, buffer_size);

    case H_STATUS:
        return att_read_callback_handle_blob(
            (const uint8_t *)status_text, (uint16_t)strlen(status_text),
            offset, buffer, buffer_size);

    case H_URL: {
        char url[48];
        int n = portal_url(url, sizeof(url));
        return att_read_callback_handle_blob((const uint8_t *)url, (uint16_t)n,
                                             offset, buffer, buffer_size);
    }

    case H_SCAN: {
        /* One network per read. A client reads until it gets an empty
         * answer, which keeps every response inside a default 23-byte MTU
         * without needing a long-read. */
        char line[64];
        int  n = portal_scan_line(scan_line_cursor, line, sizeof(line));
        if (offset == 0 && n > 0) scan_line_cursor++;
        if (n <= 0) { scan_line_cursor = 0; n = 0; }
        return att_read_callback_handle_blob((const uint8_t *)line, (uint16_t)n,
                                             offset, buffer, buffer_size);
    }

    default:
        return 0;
    }
}

static int att_write_callback(hci_con_handle_t handle, uint16_t att_handle,
                              uint16_t transaction_mode, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size)
{
    UNUSED(transaction_mode);
    UNUSED(offset);

    switch (att_handle) {
    case H_STATUS_CCC:
        status_subscribed = little_endian_read_16(buffer, 0) ==
                            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
        con_handle = handle;
        printf("ble: status notifications %s\n",
               status_subscribed ? "on" : "off");
        return 0;

    case H_SSID: {
        size_t n = buffer_size < CFG_SSID_MAX - 1 ? buffer_size : CFG_SSID_MAX - 1;
        memcpy(config()->wifi_ssid, buffer, n);
        config()->wifi_ssid[n] = '\0';
        printf("ble: SSID set to '%s'\n", config()->wifi_ssid);
        ble_set_status("SSID received");
        return 0;
    }

    case H_PASS: {
        size_t n = buffer_size < CFG_PASS_MAX - 1 ? buffer_size : CFG_PASS_MAX - 1;
        memcpy(config()->wifi_pass, buffer, n);
        config()->wifi_pass[n] = '\0';
        printf("ble: password set (%u characters)\n", (unsigned)n);
        ble_set_status("Password received");
        return 0;
    }

    case H_CMD:
        if (buffer_size < 1) return 0;
        /*
         * Commands are queued, not executed here. An ATT write callback runs
         * inside the Bluetooth stack's own packet handling; joining a network
         * or erasing a flash sector from that context is a good way to find
         * out what a watchdog reset looks like.
         */
        portal_queue_action((portal_action_t)buffer[0]);
        return 0;

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- */
/* Events                                                            */
/* ---------------------------------------------------------------- */

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
        printf("ble: controller ready\n");
        start_advertising();
        gap_set_scan_parameters(0 /* passive */, SCAN_INTERVAL, SCAN_WINDOW);
        gap_start_scan();
        break;

    case GAP_EVENT_ADVERTISING_REPORT: {
        bd_addr_t address;
        gap_event_advertising_report_get_address(packet, address);

        ble_device_t *d = find_or_add(bd_addr_to_str(address));
        d->rssi    = gap_event_advertising_report_get_rssi(packet);
        d->last_ms = to_ms_since_boot(get_absolute_time());
        d->seen++;

        parse_advertisement(d,
            gap_event_advertising_report_get_data(packet),
            gap_event_advertising_report_get_data_length(packet));
        break;
    }

    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) ==
            GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            printf("ble: setup client connected\n");
            ble_set_status("Connected over BLE");
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("ble: setup client disconnected\n");
        con_handle        = HCI_CON_HANDLE_INVALID;
        status_subscribed = false;
        /* Advertising stops on connect and must be restarted by hand. */
        gap_advertisements_enable(1);
        break;

    case ATT_EVENT_CAN_SEND_NOW:
        if (con_handle != HCI_CON_HANDLE_INVALID && status_subscribed)
            att_server_notify(con_handle, H_STATUS,
                              (const uint8_t *)status_text,
                              (uint16_t)strlen(status_text));
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* Public interface                                                  */
/* ---------------------------------------------------------------- */

void ble_portal_init(void)
{
    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    att_server_init(profile_data, att_read_callback, att_write_callback);

    hci_event_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_registration);

    att_event_registration.callback = &packet_handler;
    att_server_register_packet_handler(&packet_handler);

    strncpy(target_name, config()->ble_target_name, sizeof(target_name) - 1);

    hci_power_control(HCI_POWER_ON);
}

int ble_device_count(void) { return device_count; }

const ble_device_t *ble_device(int index)
{
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

const ble_device_t *ble_pinned_device(void)
{
    if (target_name[0] == '\0') return NULL;
    for (int i = 0; i < device_count; i++)
        if (strcmp(devices[i].name, target_name) == 0) return &devices[i];
    return NULL;
}

void ble_set_target(const char *name)
{
    strncpy(target_name, name ? name : "", sizeof(target_name) - 1);
    target_name[sizeof(target_name) - 1] = '\0';
}

bool ble_is_connected(void) { return con_handle != HCI_CON_HANDLE_INVALID; }

void ble_set_status(const char *text)
{
    strncpy(status_text, text, sizeof(status_text) - 1);
    status_text[sizeof(status_text) - 1] = '\0';

    if (con_handle != HCI_CON_HANDLE_INVALID && status_subscribed)
        att_server_request_can_send_now_event(con_handle);
}
