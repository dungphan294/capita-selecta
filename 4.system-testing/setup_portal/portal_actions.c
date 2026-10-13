#include <stdio.h>
#include <string.h>

#include "portal_actions.h"
#include "wifi_manager.h"

static volatile portal_action_t pending = PORTAL_NONE;

void portal_queue_action(portal_action_t action)
{
    if (action <= PORTAL_FACTORY_RESET) pending = action;
}

portal_action_t portal_take_action(void)
{
    portal_action_t a = pending;
    pending = PORTAL_NONE;
    return a;
}

int portal_url(char *out, size_t out_size)
{
    return snprintf(out, out_size, "http://%s/", wifi_ip());
}

int portal_scan_line(int index, char *out, size_t out_size)
{
    const wifi_ap_t *ap = wifi_scan_result(index);
    if (ap == NULL) { if (out_size) out[0] = '\0'; return 0; }

    return snprintf(out, out_size, "%s %d %s%s",
                    ap->ssid, ap->rssi, wifi_auth_name(ap->auth_mode),
                    ap->enterprise ? " ENTERPRISE" : "");
}
