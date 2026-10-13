/*
 * Captive-portal DNS. Answers every A query with the board's own address so
 * that whatever the phone tries to load lands on the setup page, and the OS
 * shows its "sign in to network" sheet.
 */

#ifndef DNSSERVER_H
#define DNSSERVER_H

#include <stdbool.h>

#include "lwip/ip4_addr.h"

bool dns_server_start(const ip4_addr_t *answer_ip);
void dns_server_stop(void);

#endif /* DNSSERVER_H */
