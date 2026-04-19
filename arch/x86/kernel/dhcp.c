/*
 * OsitoK x86-64 — DHCP Client (RFC 2131)
 *
 * Automatic IP configuration via DISCOVER → OFFER → REQUEST → ACK.
 * Uses UDP ports 67 (server) / 68 (client).
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern void     net_get_mac(uint8_t mac_out[6]);
extern void     net_set_ip(const uint8_t ip[4]);
extern void     net_set_gateway(const uint8_t gw[4]);
extern void     net_set_netmask(const uint8_t mask[4]);
extern void     net_dns_set_server(const uint8_t ip[4]);
extern void     net_udp_listen(uint16_t port, void *handler);
extern int      net_udp_send_broadcast(uint16_t dst_port, uint16_t src_port,
                                       const void *data, uint32_t len);
extern void     net_poll(void);
extern uint64_t idt_get_ticks(void);

/* ── DHCP packet format ──────────────────────────────────────── */

#define DHCP_OP_REQUEST   1
#define DHCP_OP_REPLY     2
#define DHCP_HTYPE_ETH    1
#define DHCP_MAGIC        0x63825363  /* big-endian cookie */

/* DHCP message types (option 53) */
#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_ACK          5
#define DHCP_NAK          6

typedef struct __attribute__((packed)) {
    uint8_t  op;           /* 1 = request, 2 = reply */
    uint8_t  htype;        /* 1 = Ethernet */
    uint8_t  hlen;         /* 6 for Ethernet MAC */
    uint8_t  hops;
    uint32_t xid;          /* Transaction ID */
    uint16_t secs;
    uint16_t flags;        /* 0x8000 = broadcast */
    uint8_t  ciaddr[4];    /* Client IP (0 if requesting) */
    uint8_t  yiaddr[4];    /* Your (offered) IP */
    uint8_t  siaddr[4];    /* Server IP */
    uint8_t  giaddr[4];    /* Gateway IP */
    uint8_t  chaddr[16];   /* Client MAC (6 bytes + 10 pad) */
    uint8_t  sname[64];    /* Server name (unused) */
    uint8_t  file[128];    /* Boot filename (unused) */
    uint32_t cookie;       /* Magic cookie: 0x63825363 */
    uint8_t  options[312]; /* Variable-length DHCP options */
} dhcp_packet_t;

/* ── DHCP state ──────────────────────────────────────────────── */

static uint32_t dhcp_xid;
static volatile uint8_t dhcp_msg_type;   /* Last received message type */
static uint8_t  offered_ip[4];
static uint8_t  server_ip[4];
static uint8_t  offered_mask[4];
static uint8_t  offered_gw[4];
static uint8_t  offered_dns[4];
static uint32_t offered_lease;
static volatile bool dhcp_got_reply;
static uint64_t dhcp_lease_start;   /* tick when lease was acquired */
static uint64_t dhcp_lease_ticks;   /* lease duration in 100Hz ticks */

/* ── Helpers ─────────────────────────────────────────────────── */

static inline uint32_t be32(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

static void print_ip(const uint8_t ip[4])
{
    serial_putdec(ip[0]); serial_puts(".");
    serial_putdec(ip[1]); serial_puts(".");
    serial_putdec(ip[2]); serial_puts(".");
    serial_putdec(ip[3]);
}

/* Parse DHCP options from received packet */
static void parse_options(const uint8_t *opts, uint32_t len)
{
    dhcp_msg_type = 0;
    offered_lease = 0;
    memset(offered_mask, 0, 4);
    memset(offered_gw, 0, 4);
    memset(offered_dns, 0, 4);

    uint32_t i = 0;
    while (i < len) {
        uint8_t opt = opts[i++];
        if (opt == 0) continue;       /* Pad */
        if (opt == 255) break;         /* End */
        if (i >= len) break;
        uint8_t olen = opts[i++];
        if (i + olen > len) break;

        switch (opt) {
        case 1:  /* Subnet Mask */
            if (olen >= 4) memcpy(offered_mask, &opts[i], 4);
            break;
        case 3:  /* Router */
            if (olen >= 4) memcpy(offered_gw, &opts[i], 4);
            break;
        case 6:  /* DNS Server */
            if (olen >= 4) memcpy(offered_dns, &opts[i], 4);
            break;
        case 51: /* Lease Time */
            if (olen >= 4) {
                offered_lease = ((uint32_t)opts[i] << 24) |
                                ((uint32_t)opts[i+1] << 16) |
                                ((uint32_t)opts[i+2] << 8) |
                                opts[i+3];
            }
            break;
        case 53: /* DHCP Message Type */
            if (olen >= 1) dhcp_msg_type = opts[i];
            break;
        case 54: /* Server Identifier */
            if (olen >= 4) memcpy(server_ip, &opts[i], 4);
            break;
        }
        i += olen;
    }
}

/* ── UDP handler (port 68) ───────────────────────────────────── */

static void dhcp_handler(const uint8_t *src_ip, uint16_t src_port,
                         const uint8_t *data, uint32_t len)
{
    (void)src_ip; (void)src_port;
    if (len < sizeof(dhcp_packet_t) - 312) return;

    const dhcp_packet_t *pkt = (const dhcp_packet_t *)data;
    if (pkt->op != DHCP_OP_REPLY) return;
    if (pkt->xid != dhcp_xid) return;
    if (pkt->cookie != be32(DHCP_MAGIC)) return;

    /* Extract offered IP */
    memcpy(offered_ip, pkt->yiaddr, 4);
    memcpy(server_ip, pkt->siaddr, 4);

    /* Parse options */
    uint32_t opts_len = len - (uint32_t)((const uint8_t *)pkt->options - data);
    parse_options(pkt->options, opts_len);

    dhcp_got_reply = true;
}

/* ── Build and send DHCP packet ──────────────────────────────── */

static int dhcp_send(uint8_t msg_type, const uint8_t *req_ip)
{
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op    = DHCP_OP_REQUEST;
    pkt.htype = DHCP_HTYPE_ETH;
    pkt.hlen  = 6;
    pkt.xid   = dhcp_xid;
    pkt.flags = 0x0080;  /* broadcast flag (big-endian 0x8000) */
    pkt.cookie = be32(DHCP_MAGIC);

    net_get_mac(pkt.chaddr);

    /* Build options */
    int p = 0;
    /* Option 53: DHCP Message Type */
    pkt.options[p++] = 53; pkt.options[p++] = 1;
    pkt.options[p++] = msg_type;

    if (msg_type == DHCP_REQUEST && req_ip) {
        /* Option 50: Requested IP Address */
        pkt.options[p++] = 50; pkt.options[p++] = 4;
        memcpy(&pkt.options[p], req_ip, 4); p += 4;
        /* Option 54: Server Identifier */
        pkt.options[p++] = 54; pkt.options[p++] = 4;
        memcpy(&pkt.options[p], server_ip, 4); p += 4;
    }

    /* Option 55: Parameter Request List */
    pkt.options[p++] = 55; pkt.options[p++] = 4;
    pkt.options[p++] = 1;   /* Subnet Mask */
    pkt.options[p++] = 3;   /* Router */
    pkt.options[p++] = 6;   /* DNS Server */
    pkt.options[p++] = 51;  /* Lease Time */

    /* End */
    pkt.options[p++] = 255;

    return net_udp_send_broadcast(67, 68, &pkt, sizeof(pkt));
}

/* ── Public API ──────────────────────────────────────────────── */

int dhcp_discover(void)
{
    serial_puts("[DHCP] Starting discovery...\n");
    fb_puts(" DHCP: discovering...\n");

    /* Generate transaction ID from tick counter */
    dhcp_xid = (uint32_t)(idt_get_ticks() * 0x5DEECE66DULL + 0xB);

    /* Register UDP listener on port 68 */
    net_udp_listen(68, (void *)dhcp_handler);

    /* Send DISCOVER — retry up to 4 times */
    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_got_reply = false;
        dhcp_send(DHCP_DISCOVER, NULL);

        /* Wait for OFFER (3s timeout = 300 ticks at 100Hz) */
        uint64_t start = idt_get_ticks();
        while (!dhcp_got_reply && (idt_get_ticks() - start) < 300) {
            extern void net_poll_wait(void);
            net_poll_wait();
        }

        if (dhcp_got_reply && dhcp_msg_type == DHCP_OFFER) {
            serial_puts("[DHCP] OFFER received: ");
            print_ip(offered_ip);
            serial_puts(" from server ");
            print_ip(server_ip);
            serial_puts("\n");
            break;
        }
        serial_puts("[DHCP] Retry ");
        serial_putdec((uint64_t)(attempt + 1));
        serial_puts("...\n");
    }

    if (!dhcp_got_reply || dhcp_msg_type != DHCP_OFFER) {
        serial_puts("[DHCP] No OFFER received, giving up\n");
        fb_puts(" DHCP: failed\n");
        return -1;
    }

    /* Send REQUEST for the offered IP */
    dhcp_got_reply = false;
    dhcp_send(DHCP_REQUEST, offered_ip);

    /* Wait for ACK (3s timeout) */
    uint64_t start = idt_get_ticks();
    while (!dhcp_got_reply && (idt_get_ticks() - start) < 300) {
        extern void net_poll_wait(void);
        net_poll_wait();
    }

    if (!dhcp_got_reply || dhcp_msg_type != DHCP_ACK) {
        serial_puts("[DHCP] No ACK received\n");
        fb_puts(" DHCP: no ACK\n");
        return -1;
    }

    /* Apply configuration */
    net_set_ip(offered_ip);
    if (offered_mask[0] || offered_mask[1] || offered_mask[2] || offered_mask[3])
        net_set_netmask(offered_mask);
    if (offered_gw[0] || offered_gw[1] || offered_gw[2] || offered_gw[3])
        net_set_gateway(offered_gw);
    if (offered_dns[0] || offered_dns[1] || offered_dns[2] || offered_dns[3])
        net_dns_set_server(offered_dns);

    serial_puts("[DHCP] Configured: IP=");
    print_ip(offered_ip);
    serial_puts(" GW=");
    print_ip(offered_gw);
    serial_puts(" DNS=");
    print_ip(offered_dns);
    serial_puts(" lease=");
    serial_putdec(offered_lease);
    serial_puts("s\n");

    fb_puts(" DHCP: ");
    /* Simple IP display on framebuffer */
    char ipstr[20];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) ipstr[n++] = '.';
        uint8_t v = offered_ip[i];
        if (v >= 100) ipstr[n++] = '0' + v / 100;
        if (v >= 10) ipstr[n++] = '0' + (v / 10) % 10;
        ipstr[n++] = '0' + v % 10;
    }
    ipstr[n] = '\0';
    fb_puts(ipstr);
    fb_puts("\n");

    /* Store lease timing for renewal */
    dhcp_lease_start = idt_get_ticks();
    dhcp_lease_ticks = (uint64_t)offered_lease * 100;  /* convert to 100Hz ticks */

    return 0;
}

/* Check if lease needs renewal. Call periodically from main loop.
 * Renews at T1 = 50% of lease (RFC 2131 recommendation). */
void dhcp_check_renewal(void)
{
    if (!dhcp_lease_ticks) return;
    uint64_t elapsed = idt_get_ticks() - dhcp_lease_start;
    uint64_t t1 = dhcp_lease_ticks / 2;  /* Renew at 50% of lease */

    if (elapsed < t1) return;

    serial_puts("[DHCP] Lease renewal...\n");
    dhcp_got_reply = false;
    dhcp_send(DHCP_REQUEST, offered_ip);

    /* Brief wait for ACK (1s) */
    uint64_t start = idt_get_ticks();
    while (!dhcp_got_reply && (idt_get_ticks() - start) < 100) {
        extern void net_poll_wait(void);
        net_poll_wait();
    }

    if (dhcp_got_reply && dhcp_msg_type == DHCP_ACK) {
        dhcp_lease_start = idt_get_ticks();
        dhcp_lease_ticks = (uint64_t)offered_lease * 100;
        serial_puts("[DHCP] Lease renewed (");
        serial_putdec(offered_lease);
        serial_puts("s)\n");
    } else {
        serial_puts("[DHCP] Renewal failed, will retry\n");
    }
}
