/*
 * Minimal DHCP server, enough to hand an address to a phone or laptop that
 * joins the board's setup access point. Offers a small pool on the server's
 * own /24 and points DNS back at itself.
 */

#ifndef DHCPSERVER_H
#define DHCPSERVER_H

#include <stdbool.h>

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

bool dhcp_server_start(struct netif *nif, const ip4_addr_t *server_ip);
void dhcp_server_stop(void);

#endif /* DHCPSERVER_H */
