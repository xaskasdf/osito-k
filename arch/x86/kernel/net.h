/*
 * OsitoK x86-64 — Minimal Network Stack
 *
 * Ethernet / ARP / IPv4 / UDP. Polling, static IP, no fragmentation.
 */

#ifndef OSITOK_NET_H
#define OSITOK_NET_H

#include "../include/types.h"

/* ── Ethernet ────────────────────────────────────────────────── */

#define ETH_ALEN        6
#define ETH_HDR_LEN     14
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IP4    0x0800

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;         /* Big-endian */
} eth_hdr_t;

/* ── ARP ─────────────────────────────────────────────────────── */

#define ARP_HW_ETHER    0x0001
#define ARP_OP_REQUEST  0x0001
#define ARP_OP_REPLY    0x0002

typedef struct __attribute__((packed)) {
    uint16_t htype;             /* Hardware type (Ethernet = 1) */
    uint16_t ptype;             /* Protocol type (IPv4 = 0x0800) */
    uint8_t  hlen;              /* Hardware addr len (6) */
    uint8_t  plen;              /* Protocol addr len (4) */
    uint16_t oper;              /* Operation: 1=request, 2=reply */
    uint8_t  sha[ETH_ALEN];    /* Sender hardware address */
    uint8_t  spa[4];            /* Sender protocol address */
    uint8_t  tha[ETH_ALEN];    /* Target hardware address */
    uint8_t  tpa[4];            /* Target protocol address */
} arp_pkt_t;

/* ── IPv4 ────────────────────────────────────────────────────── */

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;           /* Version (4) + IHL (5) = 0x45 */
    uint8_t  tos;
    uint16_t total_len;         /* Big-endian */
    uint16_t id;
    uint16_t frag;              /* Flags + Fragment offset */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;          /* Big-endian */
    uint8_t  src[4];
    uint8_t  dst[4];
} ipv4_hdr_t;

/* ── UDP ─────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t src_port;          /* Big-endian */
    uint16_t dst_port;
    uint16_t length;            /* Header + data */
    uint16_t checksum;
} udp_hdr_t;

/* ── Network API ─────────────────────────────────────────────── */

/* UDP receive callback */
typedef void (*udp_handler_t)(const uint8_t *src_ip, uint16_t src_port,
                              const void *data, uint32_t len);

/* Initialize network stack with static IP */
void net_init(const uint8_t ip[4]);

/* Poll for incoming packets (call in main loop) */
void net_poll(void);

/* Send UDP datagram. Returns 0 on success, -1 on failure. */
int  net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                  uint16_t src_port, const void *data, uint32_t len);

/* Register UDP listener on a port */
void net_udp_listen(uint16_t port, udp_handler_t handler);

#endif /* OSITOK_NET_H */
