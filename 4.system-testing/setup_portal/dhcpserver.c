#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include "dhcpserver.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* Option codes we read or write. */
#define OPT_PAD          0
#define OPT_SUBNET_MASK  1
#define OPT_ROUTER       3
#define OPT_DNS          6
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_END          255

#define LEASE_SECONDS 86400

/* The fixed BOOTP header, as it appears on the wire. */
typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64], file[128];
    uint32_t cookie;
    uint8_t  options[312];
} dhcp_msg_t;

static uint8_t *find_option(dhcp_msg_t *msg, size_t opt_len, uint8_t code)
{
    uint8_t *p   = msg->options;
    uint8_t *end = msg->options + opt_len;

    while (p < end && *p != OPT_END) {
        if (*p == OPT_PAD) { p++; continue; }
        if (p + 1 >= end) break;
        if (*p == code) return p;
        p += 2 + p[1];             /* code, length, value */
    }
    return NULL;
}

static uint8_t *write_option(uint8_t *p, uint8_t code, uint8_t len, const void *val)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static uint8_t *write_option_u32(uint8_t *p, uint8_t code, uint32_t val)
{
    uint32_t be = lwip_htonl(val);
    return write_option(p, code, 4, &be);
}

/* Give this MAC the slot it already has, or the first free one. */
static int lease_for(dhcp_server_t *d, const uint8_t *mac)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < DHCPS_MAX_LEASES; i++)
        if (d->leases[i].in_use && memcmp(d->leases[i].mac, mac, 6) == 0)
            return i;

    for (int i = 0; i < DHCPS_MAX_LEASES; i++)
        if (!d->leases[i].in_use || (int32_t)(now - d->leases[i].expiry_ms) > 0)
            return i;

    return -1;
}

static void dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *src_addr, u16_t src_port)
{
    (void)src_addr;
    (void)src_port;

    dhcp_server_t *d = (dhcp_server_t *)arg;
    dhcp_msg_t     msg;

    if (p->tot_len < offsetof(dhcp_msg_t, options) + 4) { pbuf_free(p); return; }

    size_t copied = pbuf_copy_partial(p, &msg, sizeof(msg), 0);
    size_t opt_len = copied > offsetof(dhcp_msg_t, options)
                   ? copied - offsetof(dhcp_msg_t, options) : 0;
    pbuf_free(p);

    uint8_t *type_opt = find_option(&msg, opt_len, OPT_MSG_TYPE);
    if (type_opt == NULL) return;

    int slot = lease_for(d, msg.chaddr);
    if (slot < 0) return;                       /* pool exhausted */

    uint32_t server = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&d->server_ip)));
    uint32_t offer  = (server & 0xFFFFFF00u) | (uint32_t)(2 + slot);

    uint8_t reply_type;
    switch (type_opt[2]) {
    case DHCP_DISCOVER:
        reply_type = DHCP_OFFER;
        break;

    case DHCP_REQUEST: {
        /* Honour the address the client asked for only if it is the one we
         * would have offered; otherwise stay silent and let it retry. */
        uint8_t *req = find_option(&msg, opt_len, OPT_REQUESTED_IP);
        if (req && req[1] == 4) {
            uint32_t wanted = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
                              ((uint32_t)req[4] << 8)  |  (uint32_t)req[5];
            if (wanted != offer) return;
        }
        memcpy(d->leases[slot].mac, msg.chaddr, 6);
        d->leases[slot].in_use    = 1;
        d->leases[slot].expiry_ms = to_ms_since_boot(get_absolute_time())
                                  + LEASE_SECONDS * 1000u;
        reply_type = DHCP_ACK;
        break;
    }

    default:
        return;                                  /* RELEASE, INFORM, ... */
    }

    /* Build the reply in place: the client's own header with our answers. */
    msg.op = 2;                                  /* BOOTREPLY */
    memset(msg.yiaddr, 0, 4);
    msg.yiaddr[0] = (uint8_t)(offer >> 24);
    msg.yiaddr[1] = (uint8_t)(offer >> 16);
    msg.yiaddr[2] = (uint8_t)(offer >> 8);
    msg.yiaddr[3] = (uint8_t)offer;

    uint8_t *o = msg.options;
    memset(o, 0, sizeof(msg.options));

    o = write_option(o, OPT_MSG_TYPE, 1, &reply_type);
    o = write_option_u32(o, OPT_SERVER_ID,
                         lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&d->server_ip))));
    o = write_option_u32(o, OPT_SUBNET_MASK,
                         lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&d->netmask))));
    /* Router and DNS both point at the board: there is nowhere else to go. */
    o = write_option_u32(o, OPT_ROUTER,
                         lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&d->server_ip))));
    o = write_option_u32(o, OPT_DNS,
                         lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&d->server_ip))));
    o = write_option_u32(o, OPT_LEASE_TIME, LEASE_SECONDS);
    *o++ = OPT_END;

    size_t reply_len = (size_t)(o - (uint8_t *)&msg);

    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)reply_len, PBUF_RAM);
    if (out == NULL) return;
    memcpy(out->payload, &msg, reply_len);

    /* The client has no address yet, so the reply goes to the broadcast
     * address — it is matched by the transaction id, not by destination. */
    udp_sendto(pcb, out, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT);
    pbuf_free(out);
}

void dhcp_server_init(dhcp_server_t *d, ip_addr_t *server_ip, ip_addr_t *netmask)
{
    memset(d, 0, sizeof(*d));
    ip_addr_copy(d->server_ip, *server_ip);
    ip_addr_copy(d->netmask, *netmask);

    d->pcb = udp_new();
    if (d->pcb == NULL) { printf("dhcps: udp_new failed\n"); return; }

    if (udp_bind(d->pcb, IP_ANY_TYPE, DHCP_SERVER_PORT) != ERR_OK) {
        printf("dhcps: bind to port %d failed\n", DHCP_SERVER_PORT);
        udp_remove(d->pcb);
        d->pcb = NULL;
        return;
    }

    udp_recv(d->pcb, dhcp_recv, d);
    printf("dhcps: serving %s/24, pool .2-.%d\n",
           ipaddr_ntoa(&d->server_ip), 1 + DHCPS_MAX_LEASES);
}

void dhcp_server_deinit(dhcp_server_t *d)
{
    if (d->pcb) { udp_remove(d->pcb); d->pcb = NULL; }
}
