/*
 * What the HTTP layer is allowed to know about the rest of the program.
 *
 * The HTTP callbacks run in the lwIP background interrupt, so they must never
 * block or talk to the Wi-Fi driver. Anything that touches the radio is queued
 * here with an app_request_* call and performed later by the main loop.
 */

#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_MODE_STA,     /* joined someone else's network */
    APP_MODE_AP       /* hosting the setup access point */
} app_mode_t;

app_mode_t    app_mode(void);
const char   *app_ip(void);
const char   *app_network(void);
bool          app_has_stored_creds(void);
float         app_temperature_c(void);
uint32_t      app_uptime_s(void);
unsigned long app_requests(void);
void          app_count_request(void);

/* "" when there is nothing to report. */
const char   *app_last_message(void);

/* Queued from the HTTP callbacks, performed by the main loop. */
void app_request_scan(void);
void app_request_connect(const char *ssid, const char *pass);
void app_request_forget(void);

#endif /* APP_H */
