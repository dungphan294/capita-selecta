#include <stdio.h>
#include <string.h>

#include "lwip/udp.h"

#include "dnsserver.h"

#define DNS_PORT      53
#define DNS_MAX_LEN  512
#define DNS_HDR_LEN   12

#define FLAG_RESPONSE 0x8000
#define FLAG_AUTH     0x0400

#define TYPE_A         1
#define CLASS_IN       1
#define ANSWER_TTL    60

static struct udp_pcb *pcb;
static ip4_addr_t      answer;

/* Walks a QNAME, returning the offset just past it, or 0 if malformed. */
static size_t skip_qname(const uint8_t *buf, size_t len, size_t at)
{
    while (at < len) {
        uint8_t label = buf[at];
        if (label == 0)
            return at + 1;
        if ((label & 0xC0) != 0)     /* compression pointer: not expected here */
            return 0;
        at += 1 + label;
    }
    return 0;
}

static void dns_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                     const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)upcb;

    uint8_t buf[DNS_MAX_LEN];

    if (p->tot_len < DNS_HDR_LEN || p->tot_len > sizeof buf) { pbuf_free(p); return; }
    size_t len = pbuf_copy_partial(p, buf, sizeof buf, 0);
    pbuf_free(p);

    uint16_t flags    = (uint16_t)(buf[2] << 8 | buf[3]);
    uint16_t qdcount  = (uint16_t)(buf[4] << 8 | buf[5]);

    if (flags & FLAG_RESPONSE)        /* already a response */
        return;
    if (qdcount != 1)                 /* keep it to the common case */
        return;

    size_t qname_end = skip_qname(buf, len, DNS_HDR_LEN);
    if (qname_end == 0 || qname_end + 4 > len)
        return;

    uint16_t qtype = (uint16_t)(buf[qname_end] << 8 | buf[qname_end + 1]);
    size_t   qlen  = qname_end + 4 - DNS_HDR_LEN;   /* question section length */

    /* Header: response, authoritative, same RD bit back. */
    buf[2] = (uint8_t)((FLAG_RESPONSE | FLAG_AUTH | (flags & 0x0100)) >> 8);
    buf[3] = (uint8_t)(flags & 0x0080);             /* preserve RA, clear rcode */
    buf[6] = 0; buf[7] = 0;                         /* ANCOUNT */
    buf[8] = 0; buf[9] = 0;                         /* NSCOUNT */
    buf[10] = 0; buf[11] = 0;                       /* ARCOUNT */

    size_t out = DNS_HDR_LEN + qlen;

    /* The answer we are about to append is 16 bytes; a query that nearly
     * fills the buffer must be answered without one. */
    if (qtype == TYPE_A && out + 16 <= sizeof buf) {
        uint8_t *a = buf + out;
        *a++ = 0xC0; *a++ = 0x0C;                   /* name: pointer to the question */
        *a++ = 0; *a++ = TYPE_A;
        *a++ = 0; *a++ = CLASS_IN;
        *a++ = 0; *a++ = 0;
        *a++ = (uint8_t)(ANSWER_TTL >> 8); *a++ = (uint8_t)ANSWER_TTL;
        *a++ = 0; *a++ = 4;                         /* RDLENGTH */
        memcpy(a, &answer.addr, 4);
        a += 4;

        out = (size_t)(a - buf);
        buf[6] = 0; buf[7] = 1;                     /* ANCOUNT = 1 */
    }

    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)out, PBUF_RAM);
    if (!q)
        return;
    memcpy(q->payload, buf, out);
    udp_sendto(pcb, q, addr, port);
    pbuf_free(q);
}

bool dns_server_start(const ip4_addr_t *answer_ip)
{
    if (pcb)
        return true;

    answer = *answer_ip;

    pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        printf("dns: udp_new failed\n");
        return false;
    }

    if (udp_bind(pcb, IP_ANY_TYPE, DNS_PORT) != ERR_OK) {
        printf("dns: bind to port %d failed\n", DNS_PORT);
        udp_remove(pcb);
        pcb = NULL;
        return false;
    }

    udp_recv(pcb, dns_recv, NULL);
    return true;
}

void dns_server_stop(void)
{
    if (!pcb)
        return;
    udp_remove(pcb);
    pcb = NULL;
}
