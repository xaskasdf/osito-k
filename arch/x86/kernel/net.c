/*
 * OsitoK x86-64 — Minimal Network Stack
 *
 * ARP responder + IPv4 + UDP. Polling mode, static IP.
 * No fragmentation, no ICMP (yet), no TCP.
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

#define ARP_TABLE_SIZE 16

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

/* ── Network State ───────────────────────────────────────────── */

static uint8_t our_ip[4];
static uint8_t our_mac[ETH_ALEN];
static uint16_t ip_id_counter;

/* Packet buffer for receive */
static uint8_t rx_pkt[2048];
/* Packet buffer for transmit (building frames) */
static uint8_t tx_pkt[2048];

/* Broadcast MAC */
static const uint8_t bcast_mac[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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
    /* Resolve destination MAC */
    arp_entry_t *entry = arp_lookup(dst_ip);
    if (!entry) {
        arp_send_request(dst_ip);
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
        }
    }
}

/* ── Send UDP Datagram ───────────────────────────────────────── */

int net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                 uint16_t src_port, const void *data, uint32_t len)
{
    /* Resolve destination MAC via ARP */
    arp_entry_t *entry = arp_lookup(dst_ip);
    if (!entry) {
        /* Send ARP request and bail — caller should retry */
        arp_send_request(dst_ip);
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
