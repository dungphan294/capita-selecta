/*
 * portal_actions — the one place BLE and HTTP meet.
 *
 * Both front ends can ask for the same five things. Neither performs them:
 * a BLE write runs inside the Bluetooth stack's packet handling and an HTTP
 * request runs inside an lwIP callback, and joining a network or erasing a
 * flash sector from either context is how you discover what a watchdog reset
 * looks like. They queue; main() executes.
 */

#ifndef PORTAL_ACTIONS_H
#define PORTAL_ACTIONS_H

#include <stddef.h>

typedef enum {
    PORTAL_NONE          = 0,
    PORTAL_JOIN          = 1,
    PORTAL_SAVE          = 2,
    PORTAL_SCAN          = 3,
    PORTAL_START_AP      = 4,
    PORTAL_FACTORY_RESET = 5,
} portal_action_t;

void            portal_queue_action(portal_action_t action);
portal_action_t portal_take_action(void);

/* Shared with both front ends. */
int  portal_url(char *out, size_t out_size);              /* "http://1.2.3.4/" */
int  portal_scan_line(int index, char *out, size_t out_size);  /* 0 when past the end */

#endif /* PORTAL_ACTIONS_H */
