#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "lwip/udp.h"

#include "dhcpserver.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7

#define OPT_PAD              0
#define OPT_SUBNET_MASK      1
#define OPT_ROUTER           3
#define OPT_DNS              6
#define OPT_REQUESTED_IP    50
#define OPT_LEASE_TIME      51
#define OPT_MSG_TYPE        53
#define OPT_SERVER_ID       54
#define OPT_END            255

/* Pool: server_ip's /24, hosts .16 through .23 */
#define LEASE_FIRST   16
#define LEASE_COUNT    8
#define LEASE_TIME_S  (24 * 60 * 60)

typedef struct {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} dhcp_msg_t;

_Static_assert(offsetof(dhcp_msg_t, chaddr)  == 28, "unexpected BOOTP layout");
_Static_assert(offsetof(dhcp_msg_t, options) == 236, "unexpected BOOTP layout");

typedef struct {
    uint8_t  mac[6];
    uint32_t expires_ms;
    bool     taken;
} lease_t;

static struct udp_pcb *pcb;
static struct netif   *iface;
static ip4_addr_t      server;
static lease_t         leases[LEASE_COUNT];

/* -- option helpers -------------------------------------------------------- */

static const uint8_t *find_option(const dhcp_msg_t *msg, size_t opt_len, uint8_t want,
                                  uint8_t *len_out)
{
    const uint8_t *p   = msg->options + 4;         /* skip the magic cookie */
    const uint8_t *end = msg->options + opt_len;

    while (p < end && *p != OPT_END) {
        if (*p == OPT_PAD) { p++; continue; }
        if (p + 2 > end) break;
        uint8_t code = p[0], len = p[1];
        if (p + 2 + len > end) break;
        if (code == want) {
            if (len_out) *len_out = len;
            return p + 2;
        }
        p += 2 + len;
    }
    return NULL;
}

static uint8_t *put_opt(uint8_t *p, uint8_t code, uint8_t len, const void *val)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static uint8_t *put_opt_u8(uint8_t *p, uint8_t code, uint8_t val)
{
    return put_opt(p, code, 1, &val);
}

static uint8_t *put_opt_be32(uint8_t *p, uint8_t code, uint32_t val)
{
    uint8_t be[4] = { (uint8_t)(val >> 24), (uint8_t)(val >> 16),
                      (uint8_t)(val >> 8),  (uint8_t)val };
    return put_opt(p, code, 4, be);
}

/* -- leases ---------------------------------------------------------------- */

static int lease_for(const uint8_t *mac)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < LEASE_COUNT; i++)
        if (leases[i].taken && memcmp(leases[i].mac, mac, 6) == 0)
            return i;

    for (int i = 0; i < LEASE_COUNT; i++)
        if (!leases[i].taken)
            return i;

    /* Pool full: reuse whatever expired longest ago. */
    for (int i = 0; i < LEASE_COUNT; i++)
        if ((int32_t)(now - leases[i].expires_ms) > 0)
            return i;

    return -1;
}

static void lease_claim(int i, const uint8_t *mac)
{
    memcpy(leases[i].mac, mac, 6);
    leases[i].taken      = true;
    leases[i].expires_ms = to_ms_since_boot(get_absolute_time()) + LEASE_TIME_S * 1000u;
}

static void lease_ip(int i, uint8_t out[4])
{
    const uint8_t *s = (const uint8_t *)&server.addr;   /* network order */
    out[0] = s[0]; out[1] = s[1]; out[2] = s[2];
    out[3] = (uint8_t)(LEASE_FIRST + i);
}

/* -- the server ------------------------------------------------------------ */

static void send_reply(dhcp_msg_t *msg, size_t opt_len)
{
    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT,
                                (u16_t)(offsetof(dhcp_msg_t, options) + opt_len),
                                PBUF_RAM);
    if (!q) return;

    memcpy(q->payload, msg, q->len);
    /* Always broadcast: the client has no address configured yet. */
    udp_sendto_if(pcb, q, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT, iface);
    pbuf_free(q);
}

static void dhcp_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                      const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)upcb; (void)addr; (void)port;

    dhcp_msg_t msg;
    memset(&msg, 0, sizeof msg);
    if (p->tot_len < offsetof(dhcp_msg_t, options) + 4) { pbuf_free(p); return; }

    size_t copied = pbuf_copy_partial(p, &msg, sizeof msg, 0);
    size_t opt_len = copied - offsetof(dhcp_msg_t, options);
    pbuf_free(p);

    if (msg.op != 1)                                  /* not a request */
        return;

    static const uint8_t cookie[4] = { 99, 130, 83, 99 };
    if (memcmp(msg.options, cookie, 4) != 0)
        return;

    uint8_t  tlen = 0;
    const uint8_t *type = find_option(&msg, opt_len, OPT_MSG_TYPE, &tlen);
    if (!type || tlen != 1)
        return;

    int slot = lease_for(msg.chaddr);
    if (slot < 0)
        return;

    uint8_t reply_type;
    switch (*type) {
    case DHCPDISCOVER:
        reply_type = DHCPOFFER;
        break;

    case DHCPREQUEST: {
        uint8_t rlen = 0;
        const uint8_t *req = find_option(&msg, opt_len, OPT_REQUESTED_IP, &rlen);
        uint8_t want[4];
        lease_ip(slot, want);
        if (req && rlen == 4 && memcmp(req, want, 4) != 0) {
            reply_type = DHCPNAK;      /* asking for an address we did not offer */
            break;
        }
        lease_claim(slot, msg.chaddr);
        reply_type = DHCPACK;
        break;
    }

    case DHCPRELEASE:
    case DHCPDECLINE:
        leases[slot].taken = false;
        return;

    default:
        return;
    }

    /* Reuse the request as the reply skeleton: xid, flags and chaddr carry over. */
    msg.op    = 2;                          /* BOOTREPLY */
    msg.secs  = 0;
    memset(msg.ciaddr, 0, 4);
    memset(msg.siaddr, 0, 4);
    memcpy(msg.siaddr, &server.addr, 4);
    memset(msg.sname, 0, sizeof msg.sname);
    memset(msg.file,  0, sizeof msg.file);

    if (reply_type == DHCPNAK)
        memset(msg.yiaddr, 0, 4);
    else
        lease_ip(slot, msg.yiaddr);

    uint8_t *o = msg.options;
    memcpy(o, cookie, 4);
    o += 4;

    o = put_opt_u8(o, OPT_MSG_TYPE, reply_type);
    o = put_opt(o, OPT_SERVER_ID, 4, &server.addr);

    if (reply_type != DHCPNAK) {
        ip4_addr_t mask;
        IP4_ADDR(&mask, 255, 255, 255, 0);
        o = put_opt(o, OPT_SUBNET_MASK, 4, &mask.addr);
        o = put_opt(o, OPT_ROUTER,      4, &server.addr);
        o = put_opt(o, OPT_DNS,         4, &server.addr);   /* us, for the portal */
        o = put_opt_be32(o, OPT_LEASE_TIME, LEASE_TIME_S);
    }

    *o++ = OPT_END;

    send_reply(&msg, (size_t)(o - msg.options));
}

bool dhcp_server_start(struct netif *nif, const ip4_addr_t *server_ip)
{
    if (pcb)
        return true;

    iface  = nif;
    server = *server_ip;
    memset(leases, 0, sizeof leases);

    pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        printf("dhcp: udp_new failed\n");
        return false;
    }

    if (udp_bind(pcb, IP_ANY_TYPE, DHCP_SERVER_PORT) != ERR_OK) {
        printf("dhcp: bind to port %d failed\n", DHCP_SERVER_PORT);
        udp_remove(pcb);
        pcb = NULL;
        return false;
    }

    udp_recv(pcb, dhcp_recv, NULL);
    return true;
}

void dhcp_server_stop(void)
{
    if (!pcb)
        return;
    udp_remove(pcb);
    pcb = NULL;
}
