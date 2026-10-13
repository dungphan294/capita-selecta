/*
 * Pico 2 W — BLE central / GATT client
 *
 * The question this answers: why does reading a BLE sensor seem to require a
 * phone application? It does not. A phone is convenient because it already
 * contains three things the microcontroller was missing, and all three can be
 * put in firmware:
 *
 *   1. the central role      — scan, connect, act as GATT client
 *   2. the assigned numbers  — what UUID 0x2A6E means, and how to read it
 *   3. the security manager  — pair and bond, so encrypted reads succeed
 *
 * This program supplies all three. It scans actively, decodes every
 * advertising payload, connects to a chosen peripheral, walks its entire GATT
 * database, reads every readable characteristic, pairs on demand when the
 * peripheral insists on encryption, and subscribes to the first notifying
 * characteristic it finds. The console output is what a scanner application
 * shows on its screen; no phone is involved at any point.
 *
 * Choose a target at build time:
 *   cmake -DTARGET_NAME="SensorTag" ..
 *   cmake -DTARGET_ADDR="A4:C1:38:11:22:33" ..
 * With neither, it scans, reports, and connects to the strongest connectable
 * device that advertises a name.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"

#include "ble_names.h"

/* -------------------------------------------------------------- */
/* Tunables                                                        */
/* -------------------------------------------------------------- */

#define SCAN_INTERVAL     0x0060   /* 60 ms, in 0.625 ms units */
#define SCAN_WINDOW       0x0030   /* listen for half of it    */
#define SCAN_SECONDS      8        /* how long to survey before choosing */

#define MAX_DEVICES       24
#define MAX_SERVICES      12
#define MAX_CHARS         40
#define MAX_VALUE_PRINT   96

#ifndef TARGET_NAME
#define TARGET_NAME ""             /* substring match on the advertised name */
#endif
#ifndef TARGET_ADDR
#define TARGET_ADDR ""             /* exact match, "AA:BB:CC:DD:EE:FF"       */
#endif

/* -------------------------------------------------------------- */
/* Scan results                                                    */
/* -------------------------------------------------------------- */

typedef struct {
    bd_addr_t      address;
    bd_addr_type_t address_type;
    int8_t         rssi;
    char           name[31];
    uint8_t        connectable;   /* from the advertising event type */
    uint16_t       seen;          /* reports received, a crude proximity signal */
} device_t;

static device_t devices[MAX_DEVICES];
static int      device_count = 0;

/* Advertising event types that mean "you may connect to me". */
static int event_type_is_connectable(uint8_t event_type)
{
    return event_type == 0 /* ADV_IND */ || event_type == 1 /* ADV_DIRECT_IND */;
}

static device_t *find_device(bd_addr_t addr)
{
    for (int i = 0; i < device_count; i++)
        if (memcmp(devices[i].address, addr, sizeof(bd_addr_t)) == 0)
            return &devices[i];
    return NULL;
}

/* Copy a name out of an advertising payload, if it carries one. */
static int extract_name(const uint8_t *data, uint8_t data_len,
                        char *out, size_t out_size)
{
    ad_context_t ctx;
    for (ad_iterator_init(&ctx, data_len, data);
         ad_iterator_has_more(&ctx);
         ad_iterator_next(&ctx))
    {
        uint8_t type = ad_iterator_get_data_type(&ctx);
        if (type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME &&
            type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) continue;

        uint8_t        len  = ad_iterator_get_data_len(&ctx);
        const uint8_t *name = ad_iterator_get_data(&ctx);
        size_t         n    = (len < out_size - 1) ? len : out_size - 1;
        memcpy(out, name, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------- */
/* GATT database, as discovered                                    */
/* -------------------------------------------------------------- */

typedef struct {
    gatt_client_service_t svc;
    int                   first_char;   /* index into chars[] */
    int                   char_count;
} service_entry_t;

static service_entry_t services[MAX_SERVICES];
static int             service_count = 0;

static gatt_client_characteristic_t chars[MAX_CHARS];
static int                          char_count = 0;

/* -------------------------------------------------------------- */
/* State machine                                                   */
/* -------------------------------------------------------------- */

typedef enum {
    ST_IDLE,
    ST_SCANNING,
    ST_CONNECTING,
    ST_DISCOVER_SERVICES,
    ST_DISCOVER_CHARS,
    ST_READ_VALUES,
    ST_SUBSCRIBE,
    ST_LISTENING,
} state_t;

static state_t  state = ST_IDLE;
static uint16_t connection_handle = HCI_CON_HANDLE_INVALID;

static int service_cursor = 0;   /* which service we are discovering into */
static int read_cursor    = 0;   /* which characteristic we are reading   */
static int notify_index   = -1;  /* first NOTIFY/INDICATE characteristic   */
static int pairing_tried  = 0;   /* one pairing attempt, not a loop        */

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;
static btstack_timer_source_t                 scan_timer;
static gatt_client_notification_t             notification_listener;

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size);
static void read_next_value(void);

/* -------------------------------------------------------------- */
/* Scanning                                                        */
/* -------------------------------------------------------------- */

static void start_scan(void);
static void scan_finished(btstack_timer_source_t *ts);

static void start_scan(void)
{
    device_count  = 0;
    service_count = 0;
    char_count    = 0;
    notify_index  = -1;
    pairing_tried = 0;
    state         = ST_SCANNING;

    printf("\n=== Scanning (active, %d s) ===\n\n", SCAN_SECONDS);

    /* Active scanning sends SCAN_REQ and collects SCAN_RSP, which is where
     * most peripherals put their full name. A passive scan sees neither. */
    gap_set_scan_parameters(1 /* active */, SCAN_INTERVAL, SCAN_WINDOW);
    gap_start_scan();

    btstack_run_loop_set_timer_handler(&scan_timer, &scan_finished);
    btstack_run_loop_set_timer(&scan_timer, SCAN_SECONDS * 1000);
    btstack_run_loop_add_timer(&scan_timer);
}

/* Which of the devices found should we open up? */
static device_t *choose_target(void)
{
    bd_addr_t wanted;
    int have_wanted = strlen(TARGET_ADDR) > 0 &&
                      sscanf_bd_addr(TARGET_ADDR, wanted);

    device_t *best = NULL;

    for (int i = 0; i < device_count; i++) {
        device_t *d = &devices[i];
        if (!d->connectable) continue;

        if (have_wanted) {
            if (memcmp(d->address, wanted, sizeof(bd_addr_t)) == 0) return d;
            continue;
        }

        if (strlen(TARGET_NAME) > 0) {
            if (strstr(d->name, TARGET_NAME) != NULL) return d;
            continue;
        }

        /* No target configured: strongest named device wins. */
        if (d->name[0] == '\0') continue;
        if (best == NULL || d->rssi > best->rssi) best = d;
    }

    return best;
}

static void scan_finished(btstack_timer_source_t *ts)
{
    UNUSED(ts);
    gap_stop_scan();

    printf("\n=== %d device%s found ===\n\n", device_count,
           device_count == 1 ? "" : "s");
    printf("  %-18s %-6s %-5s %-5s %s\n",
           "ADDRESS", "RSSI", "SEEN", "CONN", "NAME");

    for (int i = 0; i < device_count; i++) {
        device_t *d = &devices[i];
        printf("  %-18s %4d   %4u  %-5s %s\n",
               bd_addr_to_str(d->address), d->rssi, d->seen,
               d->connectable ? "yes" : "no",
               d->name[0] ? d->name : "(none)");
    }

    device_t *target = choose_target();
    if (target == NULL) {
        printf("\nNo connectable target matched. Rescanning.\n");
        start_scan();
        return;
    }

    printf("\n=== Connecting to %s  %s ===\n\n",
           bd_addr_to_str(target->address),
           target->name[0] ? target->name : "(no name)");

    state = ST_CONNECTING;
    gap_connect(target->address, target->address_type);
}

/* -------------------------------------------------------------- */
/* Discovery, reading, subscribing                                 */
/* -------------------------------------------------------------- */

/* Discover characteristics for services[service_cursor], or move on. */
static void discover_next_service_chars(void)
{
    if (service_cursor >= service_count) {
        /* Every service walked. Print the map, then read values. */
        printf("\n=== GATT database: %d services, %d characteristics ===\n",
               service_count, char_count);

        for (int s = 0; s < service_count; s++) {
            uint16_t u = services[s].svc.uuid16;
            printf("\n  Service 0x%04X  %s  (handles 0x%04X-0x%04X)\n",
                   u, ble_uuid16_name(u),
                   services[s].svc.start_group_handle,
                   services[s].svc.end_group_handle);

            for (int c = services[s].first_char;
                 c < services[s].first_char + services[s].char_count; c++)
            {
                char props[72];
                ble_properties_str(chars[c].properties, props, sizeof(props));
                printf("    char 0x%04X  %-24s [%s]\n",
                       chars[c].uuid16, ble_uuid16_name(chars[c].uuid16), props);
            }
        }

        printf("\n=== Reading every readable characteristic ===\n\n");
        state       = ST_READ_VALUES;
        read_cursor = 0;
        read_next_value();
        return;
    }

    state = ST_DISCOVER_CHARS;
    services[service_cursor].first_char = char_count;
    services[service_cursor].char_count = 0;

    gatt_client_discover_characteristics_for_service(
        &packet_handler, connection_handle, &services[service_cursor].svc);
}

/* Read the next READ-able characteristic, or move to subscribing. */
static void read_next_value(void)
{
    while (read_cursor < char_count &&
           (chars[read_cursor].properties & ATT_PROPERTY_READ) == 0)
        read_cursor++;

    if (read_cursor < char_count) {
        gatt_client_read_value_of_characteristic(
            &packet_handler, connection_handle, &chars[read_cursor]);
        return;
    }

    /* Nothing left to read. Subscribe to the first notifying characteristic. */
    for (int i = 0; i < char_count; i++) {
        if (chars[i].properties &
            (ATT_PROPERTY_NOTIFY | ATT_PROPERTY_INDICATE)) {
            notify_index = i;
            break;
        }
    }

    if (notify_index < 0) {
        printf("\n=== Done. No notifying characteristic; disconnecting. ===\n");
        gap_disconnect(connection_handle);
        return;
    }

    printf("\n=== Subscribing to 0x%04X %s ===\n\n",
           chars[notify_index].uuid16,
           ble_uuid16_name(chars[notify_index].uuid16));

    state = ST_SUBSCRIBE;

    /* Two steps, both required: register a local listener for incoming
     * values, and write the Client Characteristic Configuration descriptor
     * so the peripheral actually starts sending them. */
    gatt_client_listen_for_characteristic_value_updates(
        &notification_listener, &packet_handler,
        connection_handle, &chars[notify_index]);

    uint16_t cfg = (chars[notify_index].properties & ATT_PROPERTY_NOTIFY)
                 ? GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION
                 : GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION;

    gatt_client_write_client_characteristic_configuration(
        &packet_handler, connection_handle, &chars[notify_index], cfg);
}

static void print_value(uint16_t uuid16, const uint8_t *value, uint16_t len,
                        const char *prefix)
{
    char decoded[MAX_VALUE_PRINT];
    ble_decode_value(uuid16, value, len, decoded, sizeof(decoded));
    printf("  %s0x%04X  %-28s %s\n", prefix, uuid16,
           ble_uuid16_name(uuid16), decoded);
}

/* -------------------------------------------------------------- */
/* Event handling                                                  */
/* -------------------------------------------------------------- */

static void handle_advertising_report(uint8_t *packet)
{
    bd_addr_t address;
    gap_event_advertising_report_get_address(packet, address);

    uint8_t        event_type = gap_event_advertising_report_get_advertising_event_type(packet);
    int8_t         rssi       = gap_event_advertising_report_get_rssi(packet);
    uint8_t        data_len   = gap_event_advertising_report_get_data_length(packet);
    const uint8_t *data       = gap_event_advertising_report_get_data(packet);

    device_t *d = find_device(address);

    if (d == NULL) {
        if (device_count >= MAX_DEVICES) return;

        d = &devices[device_count++];
        memcpy(d->address, address, sizeof(bd_addr_t));
        d->address_type =
            (bd_addr_type_t) gap_event_advertising_report_get_address_type(packet);
        d->name[0]      = '\0';
        d->seen         = 0;
        d->connectable  = 0;

        /* First sighting: dump the whole payload, decoded. This is the view a
         * scanner application gives, and the reason a plain report count is
         * not enough to work with. */
        printf("%s  RSSI %4d dBm  (event type %u)\n",
               bd_addr_to_str(address), rssi, event_type);
        ble_print_ad(data, data_len, 4);
        printf("\n");
    }

    d->rssi = rssi;
    d->seen++;
    if (event_type_is_connectable(event_type)) d->connectable = 1;

    /* A scan response usually carries the name the first packet lacked. */
    if (d->name[0] == '\0') extract_name(data, data_len, d->name, sizeof(d->name));
}

static void handle_gatt_event(uint8_t *packet)
{
    switch (hci_event_packet_get_type(packet)) {

    case GATT_EVENT_SERVICE_QUERY_RESULT:
        if (service_count < MAX_SERVICES) {
            gatt_event_service_query_result_get_service(
                packet, &services[service_count].svc);
            service_count++;
        }
        break;

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
        if (char_count < MAX_CHARS) {
            gatt_event_characteristic_query_result_get_characteristic(
                packet, &chars[char_count]);
            char_count++;
            services[service_cursor].char_count++;
        }
        break;

    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
        print_value(chars[read_cursor].uuid16,
                    gatt_event_characteristic_value_query_result_get_value(packet),
                    gatt_event_characteristic_value_query_result_get_value_length(packet),
                    "");
        break;

    case GATT_EVENT_NOTIFICATION:
        print_value(chars[notify_index].uuid16,
                    gatt_event_notification_get_value(packet),
                    gatt_event_notification_get_value_length(packet),
                    "notify ");
        break;

    case GATT_EVENT_INDICATION:
        print_value(chars[notify_index].uuid16,
                    gatt_event_indication_get_value(packet),
                    gatt_event_indication_get_value_length(packet),
                    "indicate ");
        break;

    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t status = gatt_event_query_complete_get_att_status(packet);

        switch (state) {
        case ST_DISCOVER_SERVICES:
            if (status != ATT_ERROR_SUCCESS) {
                printf("Service discovery failed, ATT error 0x%02X\n", status);
                gap_disconnect(connection_handle);
                break;
            }
            service_cursor = 0;
            discover_next_service_chars();
            break;

        case ST_DISCOVER_CHARS:
            service_cursor++;
            discover_next_service_chars();
            break;

        case ST_READ_VALUES:
            /*
             * An encrypted characteristic answers with an ATT error until the
             * link is encrypted. This is the point at which a phone silently
             * pairs on your behalf — here it is done explicitly, once.
             */
            if ((status == ATT_ERROR_INSUFFICIENT_ENCRYPTION ||
                 status == ATT_ERROR_INSUFFICIENT_AUTHENTICATION) &&
                !pairing_tried)
            {
                printf("  0x%04X  %-28s needs encryption; pairing...\n",
                       chars[read_cursor].uuid16,
                       ble_uuid16_name(chars[read_cursor].uuid16));
                pairing_tried = 1;
                sm_request_pairing(connection_handle);
                break;   /* resume from SM_EVENT_PAIRING_COMPLETE */
            }

            if (status != ATT_ERROR_SUCCESS)
                printf("  0x%04X  %-28s (ATT error 0x%02X)\n",
                       chars[read_cursor].uuid16,
                       ble_uuid16_name(chars[read_cursor].uuid16), status);

            read_cursor++;
            read_next_value();
            break;

        case ST_SUBSCRIBE:
            if (status != ATT_ERROR_SUCCESS) {
                printf("Subscribe failed, ATT error 0x%02X. Disconnecting.\n", status);
                gap_disconnect(connection_handle);
                break;
            }
            printf("Subscribed. Streaming values until disconnect.\n\n");
            state = ST_LISTENING;
            break;

        default:
            break;
        }
        break;
    }

    default:
        break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {

    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            bd_addr_t local;
            gap_local_bd_addr(local);
            printf("Bluetooth ready. Local address %s\n", bd_addr_to_str(local));
            start_scan();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
        if (state == ST_SCANNING) handle_advertising_report(packet);
        break;

    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) !=
            GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;

        if (gap_subevent_le_connection_complete_get_status(packet) !=
            ERROR_CODE_SUCCESS) {
            printf("Connection failed (status 0x%02X). Rescanning.\n",
                   gap_subevent_le_connection_complete_get_status(packet));
            start_scan();
            break;
        }

        connection_handle =
            gap_subevent_le_connection_complete_get_connection_handle(packet);
        printf("Connected, handle 0x%04X. Discovering services...\n",
               connection_handle);

        state         = ST_DISCOVER_SERVICES;
        service_count = 0;
        char_count    = 0;
        gatt_client_discover_primary_services(&packet_handler, connection_handle);
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("\nDisconnected (reason 0x%02X).\n",
               hci_event_disconnection_complete_get_reason(packet));
        connection_handle = HCI_CON_HANDLE_INVALID;
        start_scan();
        break;

    /* --- Security manager: what makes encrypted characteristics readable --- */

    case SM_EVENT_JUST_WORKS_REQUEST:
        /* No display and no keyboard, so Just Works is the only method
         * available. Accepting it is the firmware equivalent of the phone's
         * "Pair?" dialog. */
        printf("  Pairing request (Just Works) — accepting.\n");
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;

    case SM_EVENT_PAIRING_COMPLETE:
        if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
            printf("  Paired and bonded. Retrying the read.\n");
        } else {
            printf("  Pairing failed (status 0x%02X, reason 0x%02X). Skipping.\n",
                   sm_event_pairing_complete_get_status(packet),
                   sm_event_pairing_complete_get_reason(packet));
            read_cursor++;   /* give up on this one characteristic */
        }
        if (state == ST_READ_VALUES) read_next_value();
        break;

    default:
        /* GATT client events arrive as HCI event packets too. */
        if (event >= GATT_EVENT_QUERY_COMPLETE &&
            event <= GATT_EVENT_MTU) {
            handle_gatt_event(packet);
        }
        break;
    }
}

/* -------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);   /* let the UART settle so the first lines are not lost */

    printf("\nPico 2 W — BLE central / GATT client\n");

    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    l2cap_init();
    sm_init();
    gatt_client_init();      /* the half a scanner-only build leaves out */

    /* No screen and no keypad, so pairing can only ever be Just Works.
     * Bonding is requested so keys are kept and a resolvable private address
     * can be recognised again on the next connection. */
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    hci_power_control(HCI_POWER_ON);

    btstack_run_loop_execute();   /* never returns */
    return 0;
}
