/*
 * Pico 2 W — BLE + WiFi setup portal
 *
 * The board is configurable before it is on any network, and manageable once
 * it is. Three access paths, in the order you need them:
 *
 *   1. BLE          a provisioning GATT service, always advertising. Write an
 *                   SSID and a password, send the join command, watch the
 *                   status characteristic. Needs no network of any kind.
 *   2. Access point until credentials are saved, the board raises its own
 *                   WPA2 network, "Pico2W-Setup", runs a DHCP server, and
 *                   serves the settings page at 192.168.4.1.
 *   3. Station      once joined, the same settings page on the real network.
 *
 * The BLE radio is doing two jobs at once throughout: advertising the setup
 * service as a peripheral, and passively scanning as a central so the page
 * can show nearby sensors.
 *
 * On WPA2-Enterprise: see wifi_manager.c. The radio cannot do it, so the
 * portal says so instead of timing out.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "lwip/ip_addr.h"
#include "dhcpserver.h"

#include "config_store.h"
#include "wifi_manager.h"
#include "ble_portal.h"
#include "http_portal.h"
#include "portal_actions.h"

#define AP_SSID      "Pico2W-Setup"
#define AP_PASSWORD  "picosetup"      /* WPA2 needs 8 characters minimum */

static dhcp_server_t dhcp_server;
static bool          ap_running = false;

/* -------------------------------------------------------------- */

static void start_access_point(void)
{
    if (ap_running) return;

    wifi_start_ap(AP_SSID, AP_PASSWORD);

    /* In AP mode nothing else hands out addresses, so a client would sit at
     * a self-assigned 169.254.x.x and never reach the portal. */
    ip_addr_t gw, mask;
    IP4_ADDR(ip_2_ip4(&gw),   192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);
    dhcp_server_init(&dhcp_server, &gw, &mask);

    ap_running = true;

    printf("\n  Join WiFi '%s' (password '%s'), then open http://192.168.4.1/\n\n",
           AP_SSID, AP_PASSWORD);
    ble_set_status("Setup AP up: " AP_SSID);
}

static void stop_access_point(void)
{
    if (!ap_running) return;
    dhcp_server_deinit(&dhcp_server);
    wifi_stop_ap();
    ap_running = false;
}

static void try_join_saved(void)
{
    stop_access_point();

    char msg[96];
    snprintf(msg, sizeof(msg), "Joining %s", config()->wifi_ssid);
    ble_set_status(msg);

    if (!wifi_join(config()->wifi_ssid, config()->wifi_pass, config()->wifi_auth)) {
        /* Refused outright — enterprise, or no SSID. Fall back to the AP so
         * the portal stays reachable. */
        ble_set_status(wifi_fail_reason());
        start_access_point();
    }
}

static void run_action(portal_action_t action)
{
    switch (action) {
    case PORTAL_JOIN:
        config_save();
        try_join_saved();
        break;

    case PORTAL_SAVE:
        config_save();
        ble_set_status("Settings saved");
        break;

    case PORTAL_SCAN:
        ble_set_status("Scanning WiFi");
        wifi_scan_start();
        break;

    case PORTAL_START_AP:
        start_access_point();
        break;

    case PORTAL_FACTORY_RESET:
        config_factory_reset();
        ble_set_target("");
        ble_set_status("Factory reset");
        start_access_point();
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);     /* let the UART settle so the first lines are not lost */

    printf("\n==========================================\n");
    printf("  Pico 2 W — BLE + WiFi setup portal\n");
    printf("==========================================\n\n");

    config_load();

    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    /*
     * One async context drives everything: the CYW43 driver, lwIP, and
     * BTstack's run loop. Both radios are the same chip on the same SPI bus,
     * so they have to share a scheduler.
     *
     * No btstack_run_loop_init() call here on purpose. cyw43_arch_init() has
     * already done it: cyw43_arch_threadsafe_background.c calls
     * btstack_cyw43_init(), which does btstack_memory_init() and
     * btstack_run_loop_init() against this same context. Calling it again
     * would reinitialise the run loop's timer and data-source lists.
     */

    ble_portal_init();          /* advertise the setup service, start scanning */
    wifi_manager_init();
    http_portal_init();

    if (config()->provisioned && config()->wifi_ssid[0]) {
        printf("Saved credentials found for '%s'.\n", config()->wifi_ssid);
        try_join_saved();
    } else {
        printf("No saved credentials.\n");
        start_access_point();
    }

    printf("Configure over BLE ('%s') or over WiFi.\n\n", "Pico2W-Setup");

    absolute_time_t next_tick = make_timeout_time_ms(500);
    wifi_state_t    last_state = (wifi_state_t)-1;

    while (true) {
        /* lwIP and BTstack run in the background async context; this loop
         * only has to do the work that must not happen inside a callback. */
        run_action(portal_take_action());
        wifi_poll();

        if (wifi_state() != last_state) {
            last_state = wifi_state();

            char msg[128];
            if (wifi_state() == WIFI_JOINED) {
                snprintf(msg, sizeof(msg), "Connected. http://%s/", wifi_ip());
                printf("\n  Portal now at http://%s/\n\n", wifi_ip());
            } else if (wifi_state() == WIFI_FAILED) {
                snprintf(msg, sizeof(msg), "Failed: %s", wifi_fail_reason());
                /* Keep the board reachable so the mistake can be corrected. */
                start_access_point();
            } else {
                snprintf(msg, sizeof(msg), "WiFi %s", wifi_state_str());
            }
            ble_set_status(msg);
        }

        if (absolute_time_diff_us(get_absolute_time(), next_tick) < 0) {
            next_tick = make_timeout_time_ms(500);
            wifi_scan_busy();     /* latches scan completion */
        }

        sleep_ms(20);
    }
}
