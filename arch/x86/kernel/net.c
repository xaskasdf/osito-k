/*
 * OsitoK x86-64 — Minimal Network Stack
 *
 * ARP responder + IPv4 + UDP + ICMP + TCP. Polling mode, static IP.
 * TCP is client-only (connect out, no listen/accept).
 */

#include "../include/types.h"
#include "net.h"
#include "../drivers/i211.h"

/* ── External Functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

/* ── Byte Order ──────────────────────────────────────────────── */

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t htonl(uint32_t h) {
    return ((h >> 24) & 0x000000FF) |
           ((h >> 8)  & 0x0000FF00) |
           ((h << 8)  & 0x00FF0000) |
           ((h << 24) & 0xFF000000);
}

static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

/* ── ARP Table ───────────────────────────────────────────────── */

#define ARP_TABLE_SIZE 64

typedef struct {
    uint8_t  ip[4];
    uint8_t  mac[ETH_ALEN];
    bool     valid;
} arp_entry_t;

static arp_entry_t arp_table[ARP_TABLE_SIZE];

/* ── UDP Listeners ───────────────────────────────────────────── */

#define MAX_UDP_LISTENERS 4

typedef struct {
    uint16_t      port;
    udp_handler_t handler;
} udp_listener_t;

static udp_listener_t udp_listeners[MAX_UDP_LISTENERS];
static int udp_listener_count;

/* ── TCP Connections ─────────────────────────────────────────── */

static tcp_conn_t tcp_conns[TCP_MAX_CONNS];

/* ── TCP Listeners (passive open) ────────────────────────────── */

#define TCP_MAX_LISTENERS  4

typedef struct {
    uint16_t  port;
    bool      active;
    int       pending_conn;   /* conn index of accepted SYN_RCVD, or -1 */
} tcp_listener_t;

static tcp_listener_t tcp_listeners[TCP_MAX_LISTENERS];

/* ── Network State ───────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

static uint8_t our_ip[4];
static uint8_t our_mac[ETH_ALEN];
static uint8_t gateway_ip[4] = {10, 0, 2, 2};    /* Default: QEMU SLIRP */
static uint8_t netmask[4]    = {255, 255, 255, 0};
static uint16_t ip_id_counter;

/* Packet buffer for receive */
static uint8_t rx_pkt[2048];
/* Packet buffer for transmit (building frames) */
static uint8_t tx_pkt[2048];

/* Broadcast MAC */
static const uint8_t bcast_mac[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Forward declarations */
static void handle_tcp(const uint8_t *src_ip, const uint8_t *pkt, uint32_t len);
static uint32_t tcp_isn_counter = 0x12345678;

/* ── IP Checksum (RFC 1071) ──────────────────────────────────── */

static uint16_t ip_checksum(const void *data, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(const uint8_t *)p;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    return (uint16_t)~sum;
}

/* ── Helper: compare IP addresses ────────────────────────────── */

static bool ip_eq(const uint8_t a[4], const uint8_t b[4])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ── ARP Table Lookup / Update ───────────────────────────────── */

/* Return the IP to ARP for: direct if on-subnet, gateway otherwise */
static const uint8_t *arp_nexthop(const uint8_t dst_ip[4])
{
    for (int i = 0; i < 4; i++) {
        if ((dst_ip[i] & netmask[i]) != (our_ip[i] & netmask[i]))
            return gateway_ip;
    }
    return dst_ip;
}

static arp_entry_t *arp_lookup(const uint8_t ip[4])
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && ip_eq(arp_table[i].ip, ip))
            return &arp_table[i];
    }
    return NULL;
}

static void arp_update(const uint8_t ip[4], const uint8_t mac[ETH_ALEN])
{
    /* Update existing entry */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && ip_eq(arp_table[i].ip, ip)) {
            memcpy(arp_table[i].mac, mac, ETH_ALEN);
            return;
        }
    }
    /* Add new entry in first free slot */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            memcpy(arp_table[i].ip, ip, 4);
            memcpy(arp_table[i].mac, mac, ETH_ALEN);
            arp_table[i].valid = true;
            return;
        }
    }
    /* Table full — overwrite slot 0 */
    memcpy(arp_table[0].ip, ip, 4);
    memcpy(arp_table[0].mac, mac, ETH_ALEN);
    arp_table[0].valid = true;
}

/* ── Send Raw Ethernet Frame ─────────────────────────────────── */

static int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
                    const void *payload, uint32_t payload_len)
{
    if (ETH_HDR_LEN + payload_len > sizeof(tx_pkt))
        return -1;

    eth_hdr_t *eth = (eth_hdr_t *)tx_pkt;
    memcpy(eth->dst, dst, ETH_ALEN);
    memcpy(eth->src, our_mac, ETH_ALEN);
    eth->ethertype = htons(ethertype);

    memcpy(tx_pkt + ETH_HDR_LEN, payload, payload_len);

    /* Pad to minimum Ethernet frame size (60 bytes without CRC) */
    uint32_t frame_len = ETH_HDR_LEN + payload_len;
    if (frame_len < 60) {
        memset(tx_pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    return i211_send(tx_pkt, frame_len);
}

/* ── ARP: Send Reply ─────────────────────────────────────────── */

static void arp_send_reply(const uint8_t *dst_mac, const uint8_t *dst_ip)
{
    arp_pkt_t arp;
    arp.htype = htons(ARP_HW_ETHER);
    arp.ptype = htons(ETH_TYPE_IP4);
    arp.hlen  = ETH_ALEN;
    arp.plen  = 4;
    arp.oper  = htons(ARP_OP_REPLY);
    memcpy(arp.sha, our_mac, ETH_ALEN);
    memcpy(arp.spa, our_ip, 4);
    memcpy(arp.tha, dst_mac, ETH_ALEN);
    memcpy(arp.tpa, dst_ip, 4);

    eth_send(dst_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* ── ARP: Send Request ───────────────────────────────────────── */

static void arp_send_request(const uint8_t *target_ip)
{
    arp_pkt_t arp;
    arp.htype = htons(ARP_HW_ETHER);
    arp.ptype = htons(ETH_TYPE_IP4);
    arp.hlen  = ETH_ALEN;
    arp.plen  = 4;
    arp.oper  = htons(ARP_OP_REQUEST);
    memcpy(arp.sha, our_mac, ETH_ALEN);
    memcpy(arp.spa, our_ip, 4);
    memset(arp.tha, 0, ETH_ALEN);
    memcpy(arp.tpa, target_ip, 4);

    eth_send(bcast_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* ── Handle ARP Packet ───────────────────────────────────────── */

static void handle_arp(const uint8_t *pkt, uint32_t len)
{
    if (len < sizeof(arp_pkt_t))
        return;

    const arp_pkt_t *arp = (const arp_pkt_t *)pkt;

    /* Only handle Ethernet + IPv4 ARP */
    if (ntohs(arp->htype) != ARP_HW_ETHER || ntohs(arp->ptype) != ETH_TYPE_IP4)
        return;

    /* Always learn sender */
    arp_update(arp->spa, arp->sha);

    if (ntohs(arp->oper) == ARP_OP_REQUEST && ip_eq(arp->tpa, our_ip)) {
        /* ARP request for our IP — send reply */
        serial_puts("[NET] ARP request from ");
        serial_putdec(arp->spa[0]); serial_puts(".");
        serial_putdec(arp->spa[1]); serial_puts(".");
        serial_putdec(arp->spa[2]); serial_puts(".");
        serial_putdec(arp->spa[3]); serial_puts("\n");

        arp_send_reply(arp->sha, arp->spa);
    }

}

/* ── ICMP ────────────────────────────────────────────────────── */

#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO_REQUEST 8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

static uint32_t icmp_rx_count;

static void icmp_send(const uint8_t dst_ip[4], uint8_t type, uint8_t code,
                       uint16_t id, uint16_t seq,
                       const void *data, uint32_t data_len)
{
    /* Resolve next-hop MAC (gateway for off-subnet) */
    const uint8_t *nexthop = arp_nexthop(dst_ip);
    arp_entry_t *entry = arp_lookup(nexthop);
    if (!entry) {
        arp_send_request(nexthop);
        return;
    }

    uint32_t icmp_len = sizeof(icmp_hdr_t) + data_len;
    uint32_t ip_total = sizeof(ipv4_hdr_t) + icmp_len;

    if (ETH_HDR_LEN + ip_total > sizeof(tx_pkt))
        return;

    /* Ethernet */
    eth_hdr_t *eth = (eth_hdr_t *)tx_pkt;
    memcpy(eth->dst, entry->mac, ETH_ALEN);
    memcpy(eth->src, our_mac, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_IP4);

    /* IPv4 */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(tx_pkt + ETH_HDR_LEN);
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((uint16_t)ip_total);
    ip->id        = htons(ip_id_counter++);
    ip->frag      = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_ICMP;
    ip->checksum  = 0;
    memcpy(ip->src, our_ip, 4);
    memcpy(ip->dst, dst_ip, 4);
    ip->checksum  = ip_checksum(ip, sizeof(ipv4_hdr_t));

    /* ICMP */
    icmp_hdr_t *icmp = (icmp_hdr_t *)(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t));
    icmp->type     = type;
    icmp->code     = code;
    icmp->checksum = 0;
    icmp->id       = id;
    icmp->seq      = seq;

    if (data && data_len > 0)
        memcpy((uint8_t *)icmp + sizeof(icmp_hdr_t), data, data_len);

    /* ICMP checksum covers entire ICMP message */
    icmp->checksum = ip_checksum(icmp, icmp_len);

    uint32_t frame_len = ETH_HDR_LEN + ip_total;
    if (frame_len < 60) {
        memset(tx_pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    i211_send(tx_pkt, frame_len);
}

static void handle_icmp(const uint8_t *src_ip, const uint8_t *pkt, uint32_t len)
{
    if (len < sizeof(icmp_hdr_t))
        return;

    const icmp_hdr_t *icmp = (const icmp_hdr_t *)pkt;

    /* Verify ICMP checksum */
    if (ip_checksum(pkt, len) != 0)
        return;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        /* Echo reply: same id/seq/data, type=0 */
        uint32_t data_len = len - sizeof(icmp_hdr_t);
        const uint8_t *data = pkt + sizeof(icmp_hdr_t);

        icmp_send(src_ip, ICMP_ECHO_REPLY, 0,
                  icmp->id, icmp->seq, data, data_len);
    }

    if (icmp->type == ICMP_ECHO_REPLY && icmp->code == 0) {
        /* Got a ping reply — increment counter for shell ping cmd */
        icmp_rx_count++;
    }
}

/* ── ICMP: Send Echo Request (ping) ─────────────────────────── */

static uint16_t ping_id = 0x4F53;  /* "OS" */

void net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq)
{
    /* 32 bytes of timestamp-like payload */
    uint8_t data[32];
    memset(data, 'O', 32);

    icmp_send(dst_ip, ICMP_ECHO_REQUEST, 0,
              htons(ping_id), htons(seq), data, 32);
}

uint32_t net_icmp_get_rx_count(void) { return icmp_rx_count; }

/* ── Handle IPv4 Packet ──────────────────────────────────────── */

static void handle_ipv4(const uint8_t *pkt, uint32_t len)
{
    if (len < sizeof(ipv4_hdr_t))
        return;

    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)pkt;

    /* Only IPv4, no options (IHL=5) */
    if ((ip->ver_ihl & 0xF0) != 0x40)
        return;

    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    uint32_t total = ntohs(ip->total_len);

    if (total > len || ihl > total)
        return;

    /* Check destination is us */
    if (!ip_eq(ip->dst, our_ip))
        return;

    /* Verify header checksum */
    if (ip_checksum(ip, ihl) != 0)
        return;

    /* Learn sender's MAC from Ethernet frame (already in ARP table via ARP,
     * but for cases where we get IP without prior ARP) */

    const uint8_t *payload = pkt + ihl;
    uint32_t payload_len = total - ihl;

    if (ip->proto == IP_PROTO_ICMP) {
        handle_icmp(ip->src, payload, payload_len);
        return;
    }

    if (ip->proto == IP_PROTO_TCP) {
        handle_tcp(ip->src, payload, payload_len);
        return;
    }

    if (ip->proto == IP_PROTO_UDP) {
        /* Handle UDP */
        if (payload_len < sizeof(udp_hdr_t))
            return;

        const udp_hdr_t *udp = (const udp_hdr_t *)payload;
        uint16_t dst_port = ntohs(udp->dst_port);
        uint16_t src_port = ntohs(udp->src_port);
        uint32_t udp_data_len = ntohs(udp->length);

        if (udp_data_len < sizeof(udp_hdr_t) || udp_data_len > payload_len)
            return;

        const uint8_t *udp_data = payload + sizeof(udp_hdr_t);
        uint32_t data_len = udp_data_len - sizeof(udp_hdr_t);

        /* Find listener for this port */
        for (int i = 0; i < udp_listener_count; i++) {
            if (udp_listeners[i].port == dst_port && udp_listeners[i].handler) {
                udp_listeners[i].handler(ip->src, src_port, udp_data, data_len);
                return;
            }
        }
    }
}

/* ── Initialize Network Stack ────────────────────────────────── */

void net_init(const uint8_t ip[4])
{
    memcpy(our_ip, ip, 4);
    i211_get_mac(our_mac);

    memset(arp_table, 0, sizeof(arp_table));
    memset(udp_listeners, 0, sizeof(udp_listeners));
    memset(tcp_conns, 0, sizeof(tcp_conns));
    udp_listener_count = 0;
    ip_id_counter = 1;

    serial_puts("[NET] IP: ");
    serial_putdec(our_ip[0]); serial_puts(".");
    serial_putdec(our_ip[1]); serial_puts(".");
    serial_putdec(our_ip[2]); serial_puts(".");
    serial_putdec(our_ip[3]); serial_puts("\n");

    fb_puts(" IP: ");
    fb_putdec(our_ip[0]); fb_puts(".");
    fb_putdec(our_ip[1]); fb_puts(".");
    fb_putdec(our_ip[2]); fb_puts(".");
    fb_putdec(our_ip[3]); fb_puts("\n");
}

void net_set_gateway(const uint8_t gw[4])
{
    memcpy(gateway_ip, gw, 4);
    serial_puts("[NET] Gateway: ");
    serial_putdec(gw[0]); serial_puts(".");
    serial_putdec(gw[1]); serial_puts(".");
    serial_putdec(gw[2]); serial_puts(".");
    serial_putdec(gw[3]); serial_puts("\n");
}

void net_set_ip(const uint8_t ip[4])
{
    memcpy(our_ip, ip, 4);
    serial_puts("[NET] IP updated: ");
    serial_putdec(ip[0]); serial_puts(".");
    serial_putdec(ip[1]); serial_puts(".");
    serial_putdec(ip[2]); serial_puts(".");
    serial_putdec(ip[3]); serial_puts("\n");
}

void net_set_netmask(const uint8_t mask[4])
{
    memcpy(netmask, mask, 4);
}

void net_get_mac(uint8_t mac_out[6])
{
    memcpy(mac_out, our_mac, 6);
}

uint8_t *net_get_ip_ptr(void) { return our_ip; }

/* Send raw UDP broadcast (src IP = 0.0.0.0, dst IP = 255.255.255.255).
 * Used by DHCP before we have an IP address. */
int net_udp_send_broadcast(uint16_t dst_port, uint16_t src_port,
                           const void *data, uint32_t len)
{
    uint32_t udp_len = sizeof(udp_hdr_t) + len;
    uint32_t ip_total = sizeof(ipv4_hdr_t) + udp_len;
    if (ETH_HDR_LEN + ip_total > sizeof(tx_pkt)) return -1;

    static const uint8_t bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_hdr_t *eth = (eth_hdr_t *)tx_pkt;
    memcpy(eth->dst, bcast_mac, 6);
    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons(ETH_TYPE_IP4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(tx_pkt + ETH_HDR_LEN);
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((uint16_t)ip_total);
    ip->id        = htons(ip_id_counter++);
    ip->frag      = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_UDP;
    ip->checksum  = 0;
    memset(ip->src, 0, 4);                              /* 0.0.0.0 */
    memset(ip->dst, 0xFF, 4);                           /* 255.255.255.255 */
    ip->checksum  = ip_checksum(ip, sizeof(ipv4_hdr_t));

    udp_hdr_t *udp = (udp_hdr_t *)(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)udp_len);
    udp->checksum = 0;

    memcpy(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t),
           data, len);

    uint32_t frame_len = ETH_HDR_LEN + ip_total;
    if (frame_len < 60) { memset(tx_pkt + frame_len, 0, 60 - frame_len); frame_len = 60; }
    return i211_send(tx_pkt, frame_len);
}

/* Forward declaration for retransmit in net_poll */
static int tcp_send_segment(tcp_conn_t *conn, uint8_t flags,
                            const void *data, uint32_t len);

/* ── Poll for Incoming Packets ───────────────────────────────── */

void net_poll(void)
{
    uint32_t len = 0;

    while (i211_recv(rx_pkt, &len) == 0) {
        if (len < ETH_HDR_LEN)
            continue;

        const eth_hdr_t *eth = (const eth_hdr_t *)rx_pkt;
        uint16_t ethertype = ntohs(eth->ethertype);
        const uint8_t *payload = rx_pkt + ETH_HDR_LEN;
        uint32_t payload_len = len - ETH_HDR_LEN;

        switch (ethertype) {
        case ETH_TYPE_ARP:
            handle_arp(payload, payload_len);
            break;
        case ETH_TYPE_IP4:
            handle_ipv4(payload, payload_len);
            break;
        case 0x86DD: { /* ETH_TYPE_IP6 */
            extern void ipv6_handle_packet(const uint8_t *data, uint32_t len,
                                           const uint8_t *src_mac);
            ipv6_handle_packet(payload, payload_len, eth->src);
            break;
        }
        }
    }

    /* TCP retransmit check — process all connections with unACKed data */
    uint64_t now = idt_get_ticks();
    for (int ci = 0; ci < TCP_MAX_CONNS; ci++) {
        tcp_conn_t *tc = &tcp_conns[ci];
        if (tc->state != TCP_ESTABLISHED || tc->tx_len == 0) continue;
        if (now < tc->rto_tick) continue;

        /* Timeout — retransmit oldest unACKed segment */
        uint32_t chunk = tc->tx_len;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        tcp_send_segment(tc, TCP_ACK | TCP_PSH, tc->tx_buf, chunk);
        tc->rto_count++;

        /* Exponential backoff: 3s, 6s, 12s, 24s, max 60s */
        uint32_t backoff = 300;
        for (uint32_t b = 0; b < tc->rto_count && backoff < 6000; b++)
            backoff *= 2;
        tc->rto_tick = now + backoff;

        /* Give up after 8 retransmits (~4 minutes) */
        if (tc->rto_count >= 8) {
            serial_puts("[TCP] Retransmit limit, closing conn ");
            serial_putdec((uint64_t)ci);
            serial_puts("\n");
            tc->state = TCP_CLOSED;
            tc->tx_len = 0;
        }
    }
}

/* ── Send UDP Datagram ───────────────────────────────────────── */

int net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                 uint16_t src_port, const void *data, uint32_t len)
{
    /* Resolve next-hop MAC (gateway for off-subnet destinations) */
    const uint8_t *nexthop = arp_nexthop(dst_ip);
    arp_entry_t *entry = arp_lookup(nexthop);
    if (!entry) {
        /* Send ARP request and bail — caller should retry */
        arp_send_request(nexthop);
        return -1;
    }

    /* Build UDP header */
    uint32_t udp_len = sizeof(udp_hdr_t) + len;
    uint32_t ip_total = sizeof(ipv4_hdr_t) + udp_len;

    if (ETH_HDR_LEN + ip_total > sizeof(tx_pkt))
        return -1;

    /* Build frame in tx_pkt */
    eth_hdr_t *eth = (eth_hdr_t *)tx_pkt;
    memcpy(eth->dst, entry->mac, ETH_ALEN);
    memcpy(eth->src, our_mac, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_IP4);

    /* IPv4 header */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(tx_pkt + ETH_HDR_LEN);
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((uint16_t)ip_total);
    ip->id        = htons(ip_id_counter++);
    ip->frag      = 0;
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_UDP;
    ip->checksum  = 0;
    memcpy(ip->src, our_ip, 4);
    memcpy(ip->dst, dst_ip, 4);
    ip->checksum  = ip_checksum(ip, sizeof(ipv4_hdr_t));

    /* UDP header */
    udp_hdr_t *udp = (udp_hdr_t *)(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((uint16_t)udp_len);
    udp->checksum = 0;  /* UDP checksum optional for IPv4 */

    /* Copy payload */
    memcpy(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t),
           data, len);

    /* Pad and send */
    uint32_t frame_len = ETH_HDR_LEN + ip_total;
    if (frame_len < 60) {
        memset(tx_pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    return i211_send(tx_pkt, frame_len);
}

/* ── TCP Checksum (pseudo-header) ─────────────────────────────── */

static uint16_t tcp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                              const void *tcp_seg, uint32_t tcp_len)
{
    /* Pseudo-header + TCP segment checksum.
     * All 16-bit words must be read consistently (native endianness). */
    uint32_t sum = 0;

    /* IP addresses — read as native 16-bit words (same as TCP data) */
    const uint16_t *sip = (const uint16_t *)src_ip;
    const uint16_t *dip = (const uint16_t *)dst_ip;
    sum += sip[0];
    sum += sip[1];
    sum += dip[0];
    sum += dip[1];

    /* Protocol + TCP length — in network byte order for consistency */
    sum += htons((uint16_t)IP_PROTO_TCP);
    sum += htons((uint16_t)tcp_len);

    /* TCP segment */
    const uint16_t *p = (const uint16_t *)tcp_seg;
    uint32_t remaining = tcp_len;
    while (remaining > 1) {
        sum += *p++;
        remaining -= 2;
    }
    if (remaining == 1)
        sum += *(const uint8_t *)p;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

/* ── TCP: Send Segment ───────────────────────────────────────── */

#define TCP_MSS 1460  /* Ethernet MTU 1500 - 20 IP - 20 TCP */

static int tcp_send_segment(tcp_conn_t *conn, uint8_t flags,
                             const void *data, uint32_t data_len)
{
    /* Resolve next-hop MAC (gateway for off-subnet) */
    const uint8_t *nexthop = arp_nexthop(conn->remote_ip);
    arp_entry_t *entry = arp_lookup(nexthop);
    if (!entry) {
        arp_send_request(nexthop);
        return -1;
    }

    uint32_t tcp_hdr_len = 20;  /* No options */
    uint32_t tcp_total = tcp_hdr_len + data_len;
    uint32_t ip_total  = sizeof(ipv4_hdr_t) + tcp_total;

    if (ETH_HDR_LEN + ip_total > sizeof(tx_pkt))
        return -1;

    /* Ethernet */
    eth_hdr_t *eth = (eth_hdr_t *)tx_pkt;
    memcpy(eth->dst, entry->mac, ETH_ALEN);
    memcpy(eth->src, our_mac, ETH_ALEN);
    eth->ethertype = htons(ETH_TYPE_IP4);

    /* IPv4 */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(tx_pkt + ETH_HDR_LEN);
    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((uint16_t)ip_total);
    ip->id        = htons(ip_id_counter++);
    ip->frag      = htons(0x4000);  /* Don't Fragment */
    ip->ttl       = 64;
    ip->proto     = IP_PROTO_TCP;
    ip->checksum  = 0;
    memcpy(ip->src, our_ip, 4);
    memcpy(ip->dst, conn->remote_ip, 4);
    ip->checksum  = ip_checksum(ip, sizeof(ipv4_hdr_t));

    /* TCP */
    tcp_hdr_t *tcp = (tcp_hdr_t *)(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t));
    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq      = htonl(conn->snd_nxt);
    tcp->ack      = htonl(conn->rcv_nxt);
    tcp->data_off = (uint8_t)((tcp_hdr_len / 4) << 4);
    tcp->flags    = flags;
    tcp->window   = htons(TCP_RX_BUF_SIZE);
    tcp->checksum = 0;
    tcp->urgent   = 0;

    /* Copy payload */
    if (data && data_len > 0)
        memcpy(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t) + tcp_hdr_len,
               data, data_len);

    /* TCP checksum */
    tcp->checksum = tcp_checksum(our_ip, conn->remote_ip, tcp, tcp_total);

    /* Advance send sequence for data + SYN/FIN (they consume seq space) */
    conn->snd_nxt += data_len;
    if (flags & TCP_SYN) conn->snd_nxt++;
    if (flags & TCP_FIN) conn->snd_nxt++;

    /* Send */
    uint32_t frame_len = ETH_HDR_LEN + ip_total;
    if (frame_len < 60) {
        memset(tx_pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    return i211_send(tx_pkt, frame_len);
}

/* ── TCP: Handle Incoming Segment ─────────────────────────────── */

static void handle_tcp(const uint8_t *src_ip, const uint8_t *pkt, uint32_t len)
{
    if (len < 20)
        return;

    const tcp_hdr_t *tcp = (const tcp_hdr_t *)pkt;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq  = ntohl(tcp->seq);
    uint32_t ack  = ntohl(tcp->ack);
    uint8_t  flags = tcp->flags;
    uint32_t hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;

    if (hdr_len < 20 || hdr_len > len)
        return;

    const uint8_t *data = pkt + hdr_len;
    uint32_t data_len = len - hdr_len;

    /* Find matching connection */
    tcp_conn_t *conn = NULL;
    int conn_idx = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &tcp_conns[i];
        if (c->state != TCP_CLOSED &&
            c->local_port == dst_port &&
            c->remote_port == src_port &&
            ip_eq(c->remote_ip, src_ip)) {
            conn = c;
            conn_idx = i;
            break;
        }
    }

    if (!conn) {
        /* No matching connection — check if a listener exists for this port */
        if (!(flags & TCP_SYN) || (flags & TCP_ACK))
            return;  /* Only accept bare SYN */

        /* Find listener for this port */
        tcp_listener_t *listener = NULL;
        for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
            if (tcp_listeners[i].active && tcp_listeners[i].port == dst_port) {
                listener = &tcp_listeners[i];
                break;
            }
        }
        if (!listener)
            return;  /* No listener — silently drop */

        /* Allocate connection slot for the incoming connection */
        int new_idx = -1;
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            if (tcp_conns[i].state == TCP_CLOSED) {
                new_idx = i;
                break;
            }
        }
        if (new_idx < 0) {
            /* No free slots — send RST */
            return;
        }

        /* Initialize server-side connection */
        conn = &tcp_conns[new_idx];
        conn_idx = new_idx;
        memset(conn, 0, sizeof(tcp_conn_t));
        memcpy(conn->remote_ip, src_ip, 4);
        conn->local_port  = dst_port;
        conn->remote_port = src_port;
        conn->rcv_nxt     = seq + 1;       /* SYN consumes 1 seq byte */
        conn->snd_nxt     = tcp_isn_counter;
        tcp_isn_counter  += 64000;
        conn->snd_una     = conn->snd_nxt;
        conn->state       = TCP_SYN_RCVD;
        conn->last_activity = idt_get_ticks();

        /* Send SYN+ACK */
        tcp_send_segment(conn, TCP_SYN | TCP_ACK, NULL, 0);

        /* Notify listener */
        listener->pending_conn = new_idx;

        serial_puts("[TCP] SYN received, sent SYN+ACK (conn ");
        serial_putdec(new_idx);
        serial_puts(" port ");
        serial_putdec(dst_port);
        serial_puts(")\n");
        return;
    }

    conn->last_activity = idt_get_ticks();

    /* RST handling — reset connection */
    if (flags & TCP_RST) {
        serial_puts("[TCP] RST received on conn ");
        serial_putdec(conn_idx);
        serial_puts("\n");
        conn->state = TCP_CLOSED;
        return;
    }

    switch (conn->state) {
    case TCP_SYN_RCVD:
        /* Expecting ACK of our SYN+ACK → transition to ESTABLISHED */
        if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
            conn->snd_una = ack;
            conn->state = TCP_ESTABLISHED;
            serial_puts("[TCP] Accepted (conn ");
            serial_putdec(conn_idx);
            serial_puts(")\n");
        }
        break;

    case TCP_SYN_SENT:
        /* Expecting SYN+ACK */
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == conn->snd_nxt) {
                conn->rcv_nxt = seq + 1;
                conn->snd_una = ack;
                conn->state = TCP_ESTABLISHED;
                /* Send ACK */
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
                serial_puts("[TCP] Connected (conn ");
                serial_putdec(conn_idx);
                serial_puts(")\n");
            }
        }
        break;

    case TCP_ESTABLISHED:
        /* ACK processing with retransmit tracking */
        if (flags & TCP_ACK) {
            if (ack > conn->snd_una) {
                /* New data ACKed — advance tx_buf, reset dup count */
                uint32_t acked = ack - conn->snd_una;
                conn->snd_una = ack;
                conn->dup_ack_count = 0;
                /* Shift retransmit buffer */
                if (conn->tx_len > 0) {
                    if (acked >= conn->tx_len) {
                        conn->tx_len = 0;  /* all ACKed */
                    } else {
                        uint32_t remain = conn->tx_len - acked;
                        for (uint32_t bi = 0; bi < remain; bi++)
                            conn->tx_buf[bi] = conn->tx_buf[bi + acked];
                        conn->tx_len = remain;
                        conn->tx_seq += acked;
                    }
                    conn->rto_tick = idt_get_ticks() + 300;
                    conn->rto_count = 0;
                }
            } else if (ack == conn->snd_una && conn->tx_len > 0) {
                /* Duplicate ACK — fast retransmit at 3 dups (RFC 5681) */
                conn->dup_ack_count++;
                if (conn->dup_ack_count >= 3) {
                    uint32_t chunk = conn->tx_len;
                    if (chunk > TCP_MSS) chunk = TCP_MSS;
                    tcp_send_segment(conn, TCP_ACK | TCP_PSH,
                                     conn->tx_buf, chunk);
                    conn->dup_ack_count = 0;
                    conn->rto_tick = idt_get_ticks() + 300;
                }
            }
        }

        /* Receive data */
        if (data_len > 0 && seq == conn->rcv_nxt) {
            uint32_t space = TCP_RX_BUF_SIZE - conn->rx_len;
            uint32_t copy = data_len < space ? data_len : space;
            if (copy > 0) {
                memcpy(conn->rx_buf + conn->rx_len, data, copy);
                conn->rx_len += copy;
            }
            conn->rcv_nxt += data_len;
            /* ACK the data */
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        }

        /* FIN from remote */
        if (flags & TCP_FIN) {
            conn->rcv_nxt = seq + data_len + 1;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
            serial_puts("[TCP] Remote FIN (conn ");
            serial_putdec(conn_idx);
            serial_puts(")\n");
        }
        break;

    case TCP_FIN_WAIT_1:
        if (flags & TCP_ACK)
            conn->snd_una = ack;

        /* Receive remaining data */
        if (data_len > 0 && seq == conn->rcv_nxt) {
            uint32_t space = TCP_RX_BUF_SIZE - conn->rx_len;
            uint32_t copy = data_len < space ? data_len : space;
            if (copy > 0) {
                memcpy(conn->rx_buf + conn->rx_len, data, copy);
                conn->rx_len += copy;
            }
            conn->rcv_nxt += data_len;
        }

        if ((flags & TCP_FIN) && (flags & TCP_ACK)) {
            /* Simultaneous FIN+ACK — go to TIME_WAIT */
            conn->rcv_nxt = seq + data_len + 1;
            conn->state = TCP_TIME_WAIT;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        } else if (flags & TCP_ACK) {
            conn->state = TCP_FIN_WAIT_2;
        } else if (flags & TCP_FIN) {
            conn->rcv_nxt = seq + data_len + 1;
            conn->state = TCP_TIME_WAIT;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_FIN_WAIT_2:
        /* Receive remaining data */
        if (data_len > 0 && seq == conn->rcv_nxt) {
            uint32_t space = TCP_RX_BUF_SIZE - conn->rx_len;
            uint32_t copy = data_len < space ? data_len : space;
            if (copy > 0) {
                memcpy(conn->rx_buf + conn->rx_len, data, copy);
                conn->rx_len += copy;
            }
            conn->rcv_nxt += data_len;
        }

        if (flags & TCP_FIN) {
            conn->rcv_nxt = seq + data_len + 1;
            conn->state = TCP_TIME_WAIT;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
            conn->state = TCP_CLOSED;
            serial_puts("[TCP] Closed (conn ");
            serial_putdec(conn_idx);
            serial_puts(")\n");
        }
        break;

    case TCP_CLOSE_WAIT:
        /* Waiting for our close() call — just ACK data */
        if (flags & TCP_ACK)
            conn->snd_una = ack;
        break;

    case TCP_TIME_WAIT:
        /* In TIME_WAIT: re-ACK any FIN retransmits, then close */
        if (flags & TCP_FIN)
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        /* We'll close after timeout in net_poll or net_tcp_close */
        break;
    }
}

/* ── TCP: Public API ──────────────────────────────────────────── */

int net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                    uint16_t src_port)
{
    /* Find free connection slot */
    int idx = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (tcp_conns[i].state == TCP_CLOSED) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        serial_puts("[TCP] No free connection slots\n");
        return -1;
    }

    /* Ensure we have ARP for next-hop (gateway for off-subnet) */
    const uint8_t *nexthop = arp_nexthop(dst_ip);
    if (!arp_lookup(nexthop)) {
        arp_send_request(nexthop);
        /* Poll for ARP reply (2s timeout, 200 ticks) */
        uint64_t arp_start = idt_get_ticks();
        while (!arp_lookup(nexthop) && (idt_get_ticks() - arp_start) < 200) {
            net_poll();
            __asm__ volatile ("hlt");
        }
        if (!arp_lookup(nexthop)) {
            serial_puts("[TCP] ARP timeout for ");
            serial_putdec(nexthop[0]); serial_puts(".");
            serial_putdec(nexthop[1]); serial_puts(".");
            serial_putdec(nexthop[2]); serial_puts(".");
            serial_putdec(nexthop[3]); serial_puts("\n");
            return -1;
        }
    }

    /* Initialize connection */
    tcp_conn_t *conn = &tcp_conns[idx];
    memset(conn, 0, sizeof(tcp_conn_t));
    memcpy(conn->remote_ip, dst_ip, 4);
    conn->local_port  = src_port;
    conn->remote_port = dst_port;
    conn->snd_nxt = tcp_isn_counter;
    tcp_isn_counter += 64000;  /* Simple ISN increment */
    conn->snd_una = conn->snd_nxt;
    conn->state = TCP_SYN_SENT;
    conn->last_activity = idt_get_ticks();

    serial_puts("[TCP] Connecting to ");
    serial_putdec(dst_ip[0]); serial_puts(".");
    serial_putdec(dst_ip[1]); serial_puts(".");
    serial_putdec(dst_ip[2]); serial_puts(".");
    serial_putdec(dst_ip[3]); serial_puts(":");
    serial_putdec(dst_port);
    serial_puts("\n");

    /* Send SYN */
    tcp_send_segment(conn, TCP_SYN, NULL, 0);

    /* Wait for SYN-ACK (5s timeout = 500 ticks at 100Hz) */
    uint64_t start = idt_get_ticks();
    while (conn->state == TCP_SYN_SENT && (idt_get_ticks() - start) < 500) {
        net_poll();
        __asm__ volatile ("hlt");
    }

    if (conn->state != TCP_ESTABLISHED) {
        serial_puts("[TCP] Connect timeout\n");
        conn->state = TCP_CLOSED;
        return -1;
    }

    return idx;
}

int net_tcp_send(int conn_idx, const void *data, uint32_t len)
{
    if (conn_idx < 0 || conn_idx >= TCP_MAX_CONNS)
        return -1;

    tcp_conn_t *conn = &tcp_conns[conn_idx];
    if (conn->state != TCP_ESTABLISHED && conn->state != TCP_CLOSE_WAIT)
        return -1;

    const uint8_t *ptr = (const uint8_t *)data;
    uint32_t sent = 0;

    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS)
            chunk = TCP_MSS;

        uint8_t flags = TCP_ACK | TCP_PSH;
        if (tcp_send_segment(conn, flags, ptr + sent, chunk) < 0) {
            net_poll();
            if (tcp_send_segment(conn, flags, ptr + sent, chunk) < 0)
                return sent > 0 ? (int)sent : -1;
        }

        /* Save in retransmit buffer for potential retransmission */
        if (conn->tx_len + chunk <= TCP_TX_BUF_SIZE) {
            memcpy(conn->tx_buf + conn->tx_len, ptr + sent, chunk);
            if (conn->tx_len == 0)
                conn->tx_seq = conn->snd_nxt - chunk;
            conn->tx_len += chunk;
            conn->rto_tick = idt_get_ticks() + 300;  /* 3s initial RTO */
            conn->rto_count = 0;
        }

        sent += chunk;
        net_poll();
    }

    return (int)sent;
}

int net_tcp_recv(int conn_idx, void *buf, uint32_t buf_size)
{
    if (conn_idx < 0 || conn_idx >= TCP_MAX_CONNS)
        return -1;

    tcp_conn_t *conn = &tcp_conns[conn_idx];

    /* If data available, return it */
    if (conn->rx_len > 0) {
        uint32_t copy = conn->rx_len < buf_size ? conn->rx_len : buf_size;
        memcpy(buf, conn->rx_buf, copy);
        /* Shift remaining data down (forward copy, dst < src, so safe) */
        if (copy < conn->rx_len) {
            uint32_t remain = conn->rx_len - copy;
            for (uint32_t i = 0; i < remain; i++)
                conn->rx_buf[i] = conn->rx_buf[copy + i];
        }
        conn->rx_len -= copy;
        return (int)copy;
    }

    /* No data — check if connection is gone */
    if (conn->state == TCP_CLOSE_WAIT || conn->state == TCP_TIME_WAIT ||
        conn->state == TCP_CLOSED || conn->state == TCP_LAST_ACK)
        return -1;

    return 0;  /* No data yet */
}

int net_tcp_recv_timeout(int conn_idx, void *buf, uint32_t buf_size,
                         uint32_t timeout_ticks)
{
    if (conn_idx < 0 || conn_idx >= TCP_MAX_CONNS)
        return -1;

    uint64_t start = idt_get_ticks();

    while ((idt_get_ticks() - start) < timeout_ticks) {
        net_poll();

        int r = net_tcp_recv(conn_idx, buf, buf_size);
        if (r != 0)
            return r;  /* Data or closed */

        __asm__ volatile ("hlt");
    }

    return 0;  /* Timeout */
}

void net_tcp_close(int conn_idx)
{
    if (conn_idx < 0 || conn_idx >= TCP_MAX_CONNS)
        return;

    tcp_conn_t *conn = &tcp_conns[conn_idx];

    if (conn->state == TCP_ESTABLISHED) {
        conn->state = TCP_FIN_WAIT_1;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, NULL, 0);
    } else if (conn->state == TCP_CLOSE_WAIT) {
        conn->state = TCP_LAST_ACK;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, NULL, 0);
    } else if (conn->state == TCP_SYN_SENT) {
        conn->state = TCP_CLOSED;
        return;
    } else {
        conn->state = TCP_CLOSED;
        return;
    }

    /* Wait for close to complete (3s = 300 ticks) */
    uint64_t start = idt_get_ticks();
    while (conn->state != TCP_CLOSED && conn->state != TCP_TIME_WAIT &&
           (idt_get_ticks() - start) < 300) {
        net_poll();
        __asm__ volatile ("hlt");
    }

    /* TIME_WAIT → CLOSED immediately (we don't need 2MSL in bare-metal) */
    conn->state = TCP_CLOSED;
}

int net_tcp_state(int conn_idx)
{
    if (conn_idx < 0 || conn_idx >= TCP_MAX_CONNS)
        return TCP_CLOSED;
    return tcp_conns[conn_idx].state;
}

/* ── TCP Server: Listen / Accept ─────────────────────────────── */

int net_tcp_listen(uint16_t port)
{
    /* Check for duplicate listener */
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (tcp_listeners[i].active && tcp_listeners[i].port == port)
            return i;  /* Already listening */
    }

    /* Find free listener slot */
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!tcp_listeners[i].active) {
            tcp_listeners[i].port = port;
            tcp_listeners[i].active = true;
            tcp_listeners[i].pending_conn = -1;
            serial_puts("[TCP] Listening on port ");
            serial_putdec(port);
            serial_puts("\n");
            return i;
        }
    }

    serial_puts("[TCP] No free listener slots\n");
    return -1;
}

int net_tcp_accept(int listener_idx, uint32_t timeout_ticks)
{
    if (listener_idx < 0 || listener_idx >= TCP_MAX_LISTENERS)
        return -1;

    tcp_listener_t *listener = &tcp_listeners[listener_idx];
    if (!listener->active)
        return -1;

    /* Clear any stale pending_conn */
    if (listener->pending_conn >= 0) {
        int pc = listener->pending_conn;
        if (tcp_conns[pc].state == TCP_CLOSED)
            listener->pending_conn = -1;
    }

    uint64_t start = idt_get_ticks();

    while ((idt_get_ticks() - start) < timeout_ticks) {
        net_poll();

        /* Check if a SYN was received and handshake is completing */
        int pc = listener->pending_conn;
        if (pc >= 0 && tcp_conns[pc].state == TCP_ESTABLISHED) {
            listener->pending_conn = -1;
            return pc;
        }

        __asm__ volatile ("hlt");
    }

    return -1;  /* Timeout */
}

void net_tcp_stop_listen(int listener_idx)
{
    if (listener_idx < 0 || listener_idx >= TCP_MAX_LISTENERS)
        return;
    tcp_listeners[listener_idx].active = false;
    tcp_listeners[listener_idx].pending_conn = -1;
    serial_puts("[TCP] Stopped listening on port ");
    serial_putdec(tcp_listeners[listener_idx].port);
    serial_puts("\n");
}

/* ── DNS Resolver ────────────────────────────────────────────── */

static uint8_t dns_server[4] = {10, 0, 2, 3};  /* QEMU SLIRP default */
static uint8_t dns_result_ip[4];
static volatile int dns_got_reply = 0;
static uint16_t dns_query_id = 0x1234;

void net_dns_set_server(const uint8_t ip[4])
{
    memcpy(dns_server, ip, 4);
}

/* DNS response handler (registered as UDP listener on port 53) */
static void dns_handler(const uint8_t *src_ip, uint16_t src_port,
                         const void *data, uint32_t len)
{
    (void)src_ip;
    (void)src_port;

    if (len < 12)
        return;

    const uint8_t *pkt = (const uint8_t *)data;

    /* Check transaction ID matches */
    uint16_t id = ((uint16_t)pkt[0] << 8) | pkt[1];
    if (id != dns_query_id)
        return;

    /* Check QR=1 (response), RCODE=0 (no error) */
    uint8_t flags1 = pkt[2];
    uint8_t flags2 = pkt[3];
    if (!(flags1 & 0x80))      /* Not a response */
        return;
    if ((flags2 & 0x0F) != 0)  /* Error code */
        return;

    uint16_t ancount = ((uint16_t)pkt[6] << 8) | pkt[7];
    if (ancount == 0)
        return;

    /* Skip header (12 bytes) + question section */
    uint32_t off = 12;

    /* Skip QNAME (labels terminated by 0) */
    while (off < len && pkt[off] != 0) {
        if ((pkt[off] & 0xC0) == 0xC0) {
            off += 2;  /* Pointer — 2 bytes */
            goto past_qname;
        }
        off += 1 + pkt[off];  /* Label length + label */
    }
    off++;  /* Skip terminal 0 */
past_qname:
    off += 4;  /* Skip QTYPE (2) + QCLASS (2) */

    /* Parse answers — find first A record (type 1) */
    for (uint16_t i = 0; i < ancount && off + 10 < len; i++) {
        /* Skip NAME (may be pointer or labels) */
        if ((pkt[off] & 0xC0) == 0xC0) {
            off += 2;
        } else {
            while (off < len && pkt[off] != 0)
                off += 1 + pkt[off];
            off++;
        }

        if (off + 10 > len) break;

        uint16_t rtype  = ((uint16_t)pkt[off] << 8) | pkt[off + 1];
        /* uint16_t rclass = ((uint16_t)pkt[off+2] << 8) | pkt[off+3]; */
        /* uint32_t ttl    = ...; */
        uint16_t rdlen  = ((uint16_t)pkt[off + 8] << 8) | pkt[off + 9];
        off += 10;

        if (rtype == 1 && rdlen == 4 && off + 4 <= len) {
            /* A record — IPv4 address */
            memcpy(dns_result_ip, pkt + off, 4);
            dns_got_reply = 1;
            return;
        }

        off += rdlen;
    }
}

int net_dns_resolve(const char *hostname, uint8_t ip_out[4])
{
    /* Build DNS query packet */
    uint8_t query[256];
    uint32_t qlen = 0;

    /* Header: ID, flags, qdcount=1 */
    dns_query_id++;
    query[0] = (uint8_t)(dns_query_id >> 8);
    query[1] = (uint8_t)(dns_query_id & 0xFF);
    query[2] = 0x01;  /* RD=1 (recursion desired) */
    query[3] = 0x00;
    query[4] = 0x00; query[5] = 0x01;  /* QDCOUNT = 1 */
    query[6] = 0x00; query[7] = 0x00;  /* ANCOUNT = 0 */
    query[8] = 0x00; query[9] = 0x00;  /* NSCOUNT = 0 */
    query[10] = 0x00; query[11] = 0x00; /* ARCOUNT = 0 */
    qlen = 12;

    /* Encode hostname as DNS labels: "api.anthropic.com" → 3api9anthropic3com0 */
    const char *p = hostname;
    while (*p) {
        /* Find end of label (next dot or end) */
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        uint32_t label_len = (uint32_t)(dot - p);

        if (label_len == 0 || label_len > 63 || qlen + 1 + label_len > 250)
            return -1;

        query[qlen++] = (uint8_t)label_len;
        for (uint32_t i = 0; i < label_len; i++)
            query[qlen++] = (uint8_t)p[i];

        p = dot;
        if (*p == '.') p++;
    }
    query[qlen++] = 0;  /* Terminal zero */

    /* QTYPE = A (1), QCLASS = IN (1) */
    query[qlen++] = 0x00; query[qlen++] = 0x01;  /* TYPE A */
    query[qlen++] = 0x00; query[qlen++] = 0x01;  /* CLASS IN */

    /* Register DNS response handler (use port 10053 as source) */
    dns_got_reply = 0;
    net_udp_listen(10053, dns_handler);

    /* Send query — retry if ARP not yet resolved */
    serial_puts("[DNS] Resolving ");
    serial_puts(hostname);
    serial_puts("...\n");

    for (int attempt = 0; attempt < 5; attempt++) {
        if (net_udp_send(dns_server, 53, 10053, query, qlen) == 0)
            break;
        /* ARP not resolved yet — poll and retry */
        net_poll();
        __asm__ volatile ("hlt");
    }

    /* Poll for response (3s timeout = 300 ticks) */
    uint64_t start = idt_get_ticks();
    while (!dns_got_reply && (idt_get_ticks() - start) < 300) {
        net_poll();
        __asm__ volatile ("hlt");
    }

    if (dns_got_reply) {
        memcpy(ip_out, dns_result_ip, 4);
        serial_puts("[DNS] Resolved: ");
        serial_putdec(ip_out[0]); serial_puts(".");
        serial_putdec(ip_out[1]); serial_puts(".");
        serial_putdec(ip_out[2]); serial_puts(".");
        serial_putdec(ip_out[3]); serial_puts("\n");
        return 0;
    }

    serial_puts("[DNS] Timeout\n");
    return -1;
}

/* ── Register UDP Listener ───────────────────────────────────── */

void net_udp_listen(uint16_t port, udp_handler_t handler)
{
    if (udp_listener_count >= MAX_UDP_LISTENERS) {
        serial_puts("[NET] Too many UDP listeners\n");
        return;
    }

    udp_listeners[udp_listener_count].port = port;
    udp_listeners[udp_listener_count].handler = handler;
    udp_listener_count++;

    serial_puts("[NET] Listening UDP :");
    serial_putdec(port);
    serial_puts("\n");

    fb_puts(" Listening UDP :");
    fb_putdec(port);
    fb_puts("\n");
}
