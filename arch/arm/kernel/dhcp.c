/*
 * dhcp.c -- DHCP client (RFC 2131): DISCOVER, OFFER, REQUEST, ACK
 *
 * Asks with the broadcast flag set, so the server's replies come as
 * broadcasts, which net.c takes while the address is still 0.0.0.0. An ACK
 * sets the address, mask, gateway and DNS server. The lease isn't renewed;
 * Windows' Internet Connection Sharing, the usual server here, gives a
 * week. The caller holds the stack (net_hold).
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "net.h"

#define SERVER_PORT     67
#define CLIENT_PORT     68
#define WAIT_MS         2000
#define TRIES           3

#define DISCOVER        1
#define OFFER           2
#define REQUEST         3
#define ACK             5
#define NAK             6

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint8_t  xid[4];
    uint8_t  secs[2], flags[2];
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64], file[128];
    uint8_t  cookie[4];
    uint8_t  options[60];                   /* 300 bytes in all, BOOTP's minimum */
} dhcp_msg_t;

static const uint8_t cookie[4] = { 99, 130, 83, 99 };
static const uint8_t bcast[4] = { 255, 255, 255, 255 };
static uint8_t xid[4];
static dhcp_msg_t reply;
static uint32_t reply_len;
static volatile int reply_type;
static int listening;

/* An option's value from a message: its length, or -1 */
static int option(const dhcp_msg_t *m, uint32_t len, uint8_t code, uint8_t *out, int max)
{
    const uint8_t *p = m->options, *end = (const uint8_t *)m + len;
    while (p < end && *p != 255) {
        if (*p == 0) {
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end)
            break;
        if (*p == code) {
            int n = p[1] < max ? p[1] : max;
            memcpy(out, p + 2, (uint64_t)n);
            return p[1];
        }
        p += 2 + p[1];
    }
    return -1;
}

static void on_reply(const uint8_t *src_ip, uint16_t src_port, const void *data, uint32_t len)
{
    (void)src_ip;
    (void)src_port;
    const dhcp_msg_t *m = (const dhcp_msg_t *)data;
    if (len < 240 || m->op != 2 || memcmp(m->xid, xid, 4) || memcmp(m->cookie, cookie, 4))
        return;
    uint8_t type = 0;
    if (option(m, len, 53, &type, 1) != 1)
        return;
    reply_len = len < sizeof(reply) ? len : sizeof(reply);
    memcpy(&reply, m, reply_len);
    reply_type = type;
}

static void send_msg(uint8_t type, const uint8_t want[4], const uint8_t server[4])
{
    static dhcp_msg_t m;
    memset(&m, 0, sizeof(m));
    m.op = 1;
    m.htype = 1;
    m.hlen = 6;
    memcpy(m.xid, xid, 4);
    m.flags[0] = 0x80;                      /* broadcast replies */
    net_get_mac(m.chaddr);
    memcpy(m.cookie, cookie, 4);
    uint8_t *o = m.options;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (want) {
        *o++ = 50; *o++ = 4; memcpy(o, want, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, server, 4); o += 4;
    }
    *o++ = 12; *o++ = 7; memcpy(o, "osito-k", 7); o += 7;      /* host name */
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;         /* mask, router, DNS */
    *o = 255;
    net_udp_send(bcast, SERVER_PORT, CLIENT_PORT, &m, sizeof(m));
}

/* Wait for a reply of one of two types */
static int wait_for(int a, int b)
{
    for (uint32_t t = 0; t < WAIT_MS; t += 10) {
        net_poll();
        if (reply_type == a || reply_type == b)
            return reply_type;
        task_delay_ms(10);
    }
    return 0;
}

static void put_ip(const uint8_t ip[4])
{
    for (int i = 0; i < 4; i++) {
        serial_putdec(ip[i]);
        if (i < 3)
            serial_putc('.');
    }
}

/* Configure the stack from a DHCP server: 0 bound, -1 no server answered */
int dhcp_run(void)
{
    static const uint8_t none[4] = { 0, 0, 0, 0 };
    if (!listening) {
        net_udp_listen(CLIENT_PORT, on_reply);
        listening = 1;
    }
    net_set_ip(none, none);
    for (int try = 0; try < TRIES; try++) {
        uint32_t r = (uint32_t)timer_ms() * 2654435761u ^ (uint32_t)try;
        memcpy(xid, &r, 4);
        reply_type = 0;
        send_msg(DISCOVER, (const uint8_t *)0, (const uint8_t *)0);
        if (wait_for(OFFER, OFFER) != OFFER)
            continue;
        uint8_t offered[4], server[4];
        memcpy(offered, reply.yiaddr, 4);
        if (option(&reply, reply_len, 54, server, 4) != 4)
            memcpy(server, reply.siaddr, 4);
        reply_type = 0;
        send_msg(REQUEST, offered, server);
        if (wait_for(ACK, NAK) != ACK)
            continue;

        uint8_t mask[4] = { 255, 255, 255, 0 }, router[4], dns[4];
        option(&reply, reply_len, 1, mask, 4);
        net_set_ip(reply.yiaddr, mask);
        if (option(&reply, reply_len, 3, router, 4) >= 4)
            net_set_gateway(router);
        if (option(&reply, reply_len, 6, dns, 4) >= 4)
            net_dns_set_server(dns);
        serial_puts("[DHCP] ");
        put_ip(reply.yiaddr);
        serial_puts(" from ");
        put_ip(server);
        serial_puts("\n");
        return 0;
    }
    return -1;
}
