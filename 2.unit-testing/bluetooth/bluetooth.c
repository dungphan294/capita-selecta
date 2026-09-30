/*
 * Pico 2 W — BLE scanner
 *
 * Brings up the CYW43439's Bluetooth radio via BTstack, scans for nearby
 * Bluetooth Low Energy advertisements, and prints each one to the console:
 * address, signal strength, and the device name when it advertises one.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"

#define SCAN_INTERVAL 0x0030   // 30 ms, in 0.625 ms units
#define SCAN_WINDOW   0x0030   // listen for the whole interval

#define DEVICE_NAME   "Pico2W"

/*
 * Advertising payload. BLE advertising data is a packed sequence of
 * length-type-value records; the length byte counts the type byte plus
 * the value, but not itself.
 */
static uint8_t adv_data[] = {
    // Flags: LE General Discoverable, BR/EDR not supported
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    // Complete local name
    sizeof(DEVICE_NAME), BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P','i','c','o','2','W',
};
static const uint8_t adv_data_len = sizeof(adv_data);

static void start_advertising(void)
{
    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));

    // 0x0030 units of 0.625 ms = 30 ms between advertisements
    gap_advertisements_set_params(0x0030, 0x0030,
                                  0 /* ADV_IND: connectable, undirected */,
                                  0, null_addr, 0x07 /* all 3 channels */, 0x00);
    gap_advertisements_set_data(adv_data_len, adv_data);
    gap_advertisements_enable(1);

    printf("Advertising as '%s' — look for it in a BLE scanner app.\n", DEVICE_NAME);
}

static btstack_packet_callback_registration_t hci_event_callback_registration;
static unsigned int report_count = 0;

// Pull a device name out of the advertising payload, if it carries one.
static void extract_name(const uint8_t *data, uint8_t data_len,
                         char *out, size_t out_size)
{
    out[0] = '\0';
    ad_context_t context;
    for (ad_iterator_init(&context, data_len, data);
         ad_iterator_has_more(&context);
         ad_iterator_next(&context))
    {
        uint8_t type = ad_iterator_get_data_type(&context);
        if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
            type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME)
        {
            uint8_t len = ad_iterator_get_data_len(&context);
            const uint8_t *name = ad_iterator_get_data(&context);
            size_t n = (len < out_size - 1) ? len : out_size - 1;
            memcpy(out, name, n);
            out[n] = '\0';
            return;
        }
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet))
    {
    case BTSTACK_EVENT_STATE:
        // The stack reports HCI_STATE_WORKING once the controller is ready.
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
        {
            bd_addr_t local_addr;
            gap_local_bd_addr(local_addr);
            printf("Bluetooth ready. Local address: %s\n", bd_addr_to_str(local_addr));
            start_advertising();

            printf("Scanning for BLE devices...\n\n");
            gap_set_scan_parameters(0 /* passive */, SCAN_INTERVAL, SCAN_WINDOW);
            gap_start_scan();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
    {
        bd_addr_t address;
        gap_event_advertising_report_get_address(packet, address);
        int8_t  rssi     = gap_event_advertising_report_get_rssi(packet);
        uint8_t data_len = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *data = gap_event_advertising_report_get_data(packet);

        char name[32];
        extract_name(data, data_len, name, sizeof(name));

        printf("[%3u] %s  RSSI %4d dBm  %s\n",
               ++report_count,
               bd_addr_to_str(address),
               rssi,
               name[0] ? name : "(no name)");
        break;
    }

    default:
        break;
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);   // give the UART time to settle so the first lines are seen

    printf("Starting Bluetooth...\n");

    if (cyw43_arch_init())
    {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    l2cap_init();   // BTstack's L2CAP layer
    sm_init();      // security manager — required even for passive scanning

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    hci_power_control(HCI_POWER_ON);

    // Hands control to BTstack; never returns.
    btstack_run_loop_execute();
    return 0;
}
