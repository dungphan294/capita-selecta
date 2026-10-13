/*
 * dhcpserver — just enough DHCP to hand addresses to setup clients.
 *
 * lwIP ships a DHCP *client* only. In access-point mode the board is the
 * network, so without a server a laptop joining "Pico2W-Setup" sits at a
 * self-assigned 169.254 address and never reaches the portal. This is the
 * smallest server that makes that work: a fixed pool, no persistence, no
 * relaying, no options beyond the four a client needs to configure itself.
 */

#ifndef DHCPSERVER_H
#define DHCPSERVER_H

#include "lwip/ip_addr.h"

#define DHCPS_MAX_LEASES 8

typedef struct {
    uint8_t  mac[6];
    uint8_t  in_use;
    uint32_t expiry_ms;
} dhcps_lease_t;

typedef struct {
    struct udp_pcb *pcb;
    ip_addr_t       server_ip;
    ip_addr_t       netmask;
    dhcps_lease_t   leases[DHCPS_MAX_LEASES];
} dhcp_server_t;

void dhcp_server_init(dhcp_server_t *d, ip_addr_t *server_ip, ip_addr_t *netmask);
void dhcp_server_deinit(dhcp_server_t *d);

#endif /* DHCPSERVER_H */
