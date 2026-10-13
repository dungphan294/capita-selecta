#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "wifi_manager.h"

static wifi_ap_t    scan_results[WIFI_MAX_SCAN_RESULTS];
static int          scan_count = 0;
static bool         scan_running = false;

static wifi_state_t state = WIFI_IDLE;
static char         fail_reason[96] = "";
static char         ip_text[20] = "0.0.0.0";
static absolute_time_t join_deadline;
static char         joining_ssid[33] = "";

/* ---------------------------------------------------------------- */
/* Enterprise networks                                               */
/* ---------------------------------------------------------------- */

/*
 * Networks this radio provably cannot join. The CYW43439, as driven by the
 * Pico SDK's cyw43-driver, offers exactly four authorisation modes — open,
 * WPA-TKIP-PSK, WPA2-AES-PSK and WPA2-mixed-PSK. All four are pre-shared
 * key. There is no EAP implementation, no TLS supplicant and no certificate
 * store anywhere in the driver, so 802.1X is not a matter of configuration.
 */
static const char *enterprise_ssids[] = {
    "eduroam",
    "saxion",
    "saxion-secure",
    "govroom",
    "govroam",
};

bool wifi_ssid_is_enterprise(const char *ssid)
{
    if (ssid == NULL) return false;
    for (size_t i = 0; i < sizeof(enterprise_ssids) / sizeof(enterprise_ssids[0]); i++)
        if (strcasecmp(ssid, enterprise_ssids[i]) == 0) return true;
    return false;
}

const char *wifi_auth_name(uint8_t auth_mode)
{
    switch (auth_mode) {
    case 0:  return "open";
    case 1:  return "WEP";
    case 2:  return "WPA-TKIP-PSK";
    case 4:  return "WPA2-AES-PSK";
    case 5:  return "WPA2-TKIP-PSK";
    case 6:  return "WPA2-mixed-PSK";
    case 7:  return "WPA3-SAE";
    default: return "secured";
    }
}

/* ---------------------------------------------------------------- */
/* Scanning                                                          */
/* ---------------------------------------------------------------- */

static int scan_result_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    if (r == NULL || r->ssid_len == 0) return 0;    /* skip hidden SSIDs */

    char ssid[33];
    size_t n = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(ssid, r->ssid, n);
    ssid[n] = '\0';

    /* Keep the strongest sighting of each SSID rather than one row per BSSID;
     * a campus network has dozens of access points and the list is for a
     * human to read. */
    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_results[i].ssid, ssid) != 0) continue;
        if (r->rssi > scan_results[i].rssi) {
            scan_results[i].rssi    = r->rssi;
            scan_results[i].channel = r->channel;
            memcpy(scan_results[i].bssid, r->bssid, 6);
        }
        return 0;
    }

    if (scan_count >= WIFI_MAX_SCAN_RESULTS) return 0;

    wifi_ap_t *ap = &scan_results[scan_count++];
    strcpy(ap->ssid, ssid);
    memcpy(ap->bssid, r->bssid, 6);
    ap->rssi       = r->rssi;
    ap->channel    = r->channel;
    ap->auth_mode  = r->auth_mode;
    ap->enterprise = wifi_ssid_is_enterprise(ssid);
    return 0;
}

bool wifi_scan_start(void)
{
    if (scan_running) return false;

    scan_count = 0;
    cyw43_wifi_scan_options_t opts = { 0 };

    int rc = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb);
    if (rc != 0) {
        printf("wifi: scan failed to start (%d)\n", rc);
        return false;
    }

    scan_running = true;
    return true;
}

bool wifi_scan_busy(void)
{
    if (scan_running && !cyw43_wifi_scan_active(&cyw43_state)) {
        scan_running = false;
        printf("wifi: scan complete, %d network%s\n",
               scan_count, scan_count == 1 ? "" : "s");
    }
    return scan_running;
}

int wifi_scan_count(void) { return scan_count; }

const wifi_ap_t *wifi_scan_result(int index)
{
    if (index < 0 || index >= scan_count) return NULL;
    return &scan_results[index];
}

/* ---------------------------------------------------------------- */
/* Station mode                                                      */
/* ---------------------------------------------------------------- */

void wifi_manager_init(void)
{
    cyw43_arch_enable_sta_mode();
    state = WIFI_IDLE;
}

bool wifi_join(const char *ssid, const char *pass, uint32_t auth)
{
    if (ssid == NULL || ssid[0] == '\0') {
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason), "No SSID given.");
        return false;
    }

    /*
     * Refuse up front rather than let it time out. An 802.1X network and a
     * wrong password both end in the same -2, and that ambiguity is exactly
     * what makes this failure expensive to diagnose.
     */
    if (wifi_ssid_is_enterprise(ssid)) {
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason),
                 "'%s' is WPA2-Enterprise (802.1X). The CYW43439 driver "
                 "supports pre-shared keys only — it has no EAP supplicant.",
                 ssid);
        printf("wifi: %s\n", fail_reason);
        return false;
    }

    strncpy(joining_ssid, ssid, sizeof(joining_ssid) - 1);
    joining_ssid[sizeof(joining_ssid) - 1] = '\0';

    cyw43_arch_enable_sta_mode();

    const char *pw = (pass && pass[0]) ? pass : NULL;
    if (pw == NULL) auth = CYW43_AUTH_OPEN;

    printf("wifi: joining '%s' (%s)\n", ssid, wifi_auth_name((uint8_t)auth));

    int rc = cyw43_arch_wifi_connect_async(ssid, pw, auth);
    if (rc != 0) {
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason),
                 "connect_async refused the request (%d).", rc);
        return false;
    }

    state         = WIFI_JOINING;
    fail_reason[0] = '\0';
    join_deadline = make_timeout_time_ms(30000);
    return true;
}

void wifi_poll(void)
{
    if (state != WIFI_JOINING) return;

    int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    switch (link) {
    case CYW43_LINK_UP: {
        const ip4_addr_t *addr = netif_ip4_addr(netif_default);
        /* The link is up before DHCP has finished; wait for a real address. */
        if (addr == NULL || ip4_addr_get_u32(addr) == 0) break;

        strncpy(ip_text, ip4addr_ntoa(addr), sizeof(ip_text) - 1);
        ip_text[sizeof(ip_text) - 1] = '\0';
        state = WIFI_JOINED;
        printf("wifi: joined '%s', address %s\n", joining_ssid, ip_text);
        return;
    }

    case CYW43_LINK_BADAUTH:
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason),
                 "Authentication rejected — wrong password for '%s'.", joining_ssid);
        printf("wifi: %s\n", fail_reason);
        return;

    case CYW43_LINK_NONET:
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason),
                 "'%s' was not found. It may be hidden, out of range, or "
                 "5 GHz only — the CYW43439 is 2.4 GHz only.", joining_ssid);
        printf("wifi: %s\n", fail_reason);
        return;

    case CYW43_LINK_FAIL:
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason),
                 "Association with '%s' failed.", joining_ssid);
        return;

    default:
        break;   /* CYW43_LINK_DOWN / JOIN / NOIP — still in progress */
    }

    if (absolute_time_diff_us(get_absolute_time(), join_deadline) < 0) {
        state = WIFI_FAILED;
        snprintf(fail_reason, sizeof(fail_reason),
                 "Timed out after 30 s with link status %d.", link);
        printf("wifi: %s\n", fail_reason);
    }
}

wifi_state_t wifi_state(void) { return state; }

const char *wifi_state_str(void)
{
    switch (state) {
    case WIFI_IDLE:    return "idle";
    case WIFI_AP_MODE: return "access point";
    case WIFI_JOINING: return "joining";
    case WIFI_JOINED:  return "connected";
    case WIFI_FAILED:  return "failed";
    default:           return "?";
    }
}

const char *wifi_fail_reason(void) { return fail_reason; }
const char *wifi_ip(void)          { return ip_text; }

/* ---------------------------------------------------------------- */
/* Access-point mode                                                 */
/* ---------------------------------------------------------------- */

bool wifi_start_ap(const char *ssid, const char *pass)
{
    /* An open AP would expose the configuration form to anyone in range,
     * so the setup network is WPA2 even though it is temporary. */
    cyw43_arch_enable_ap_mode(ssid, pass, CYW43_AUTH_WPA2_AES_PSK);

    state = WIFI_AP_MODE;
    strncpy(ip_text, "192.168.4.1", sizeof(ip_text) - 1);
    printf("wifi: access point '%s' up on %s\n", ssid, ip_text);
    return true;
}

void wifi_stop_ap(void)
{
    cyw43_arch_disable_ap_mode();
    state = WIFI_IDLE;
}
