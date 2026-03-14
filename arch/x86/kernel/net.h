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

/* ── TCP ─────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;          /* upper 4 bits = header len / 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* TCP connection states */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT_1  3
#define TCP_FIN_WAIT_2  4
#define TCP_CLOSE_WAIT  5
#define TCP_LAST_ACK    6
#define TCP_TIME_WAIT   7
#define TCP_LISTEN      8
#define TCP_SYN_RCVD    9

#define TCP_RX_BUF_SIZE 8192
#define TCP_MAX_CONNS   32

typedef struct {
    int       state;
    uint8_t   remote_ip[4];
    uint16_t  local_port;
    uint16_t  remote_port;

    uint32_t  snd_nxt;      /* next seq to send */
    uint32_t  snd_una;      /* oldest unacked seq */
    uint32_t  rcv_nxt;      /* next expected seq from remote */

    uint8_t   rx_buf[TCP_RX_BUF_SIZE];
    uint32_t  rx_len;       /* bytes available in rx_buf */

    uint64_t  last_activity;  /* tick of last packet */
} tcp_conn_t;

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

/* ICMP */
void     net_icmp_send_echo(const uint8_t dst_ip[4], uint16_t seq);
uint32_t net_icmp_get_rx_count(void);

/* ── TCP API ─────────────────────────────────────────────────── */

/* Connect to remote host. Blocks until handshake completes or timeout.
 * Returns connection index (0..TCP_MAX_CONNS-1) or -1 on failure. */
int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                     uint16_t src_port);

/* Listen for incoming connections on a port.
 * Returns listener index or -1 on failure. */
int  net_tcp_listen(uint16_t port);

/* Accept incoming connection on a listener. Blocks until SYN arrives
 * or timeout. Returns connection index or -1 on timeout. */
int  net_tcp_accept(int listener, uint32_t timeout_ticks);

/* Send data on established connection. Blocks until sent or timeout.
 * Returns bytes sent, or -1 on error. */
int  net_tcp_send(int conn, const void *data, uint32_t len);

/* Receive data from connection. Non-blocking — returns bytes copied
 * to buf, or 0 if nothing available, or -1 if connection closed. */
int  net_tcp_recv(int conn, void *buf, uint32_t buf_size);

/* Receive data with timeout (in ticks). Blocks until data arrives
 * or timeout. Returns bytes read, 0 on timeout, -1 on closed. */
int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                          uint32_t timeout_ticks);

/* Stop listening on a port */
void net_tcp_stop_listen(int listener);

/* Close TCP connection gracefully. Blocks for FIN handshake. */
void net_tcp_close(int conn);

/* Get TCP connection state */
int  net_tcp_state(int conn);

/* ── DNS API ─────────────────────────────────────────────────── */

/* Resolve hostname to IPv4 address. Blocks until response or timeout.
 * Returns 0 on success (ip_out filled), -1 on failure. */
int  net_dns_resolve(const char *hostname, uint8_t ip_out[4]);

/* Set DNS server IP (default: 10.0.2.3 for QEMU SLIRP) */
void net_dns_set_server(const uint8_t ip[4]);

#endif /* OSITOK_NET_H */
