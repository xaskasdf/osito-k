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

/* 128 KiB — with RFC 7323 window scaling negotiated on SYN
 * (kind=3, shift=3) we can advertise up to (TCP_RX_BUF_SIZE >> 3)
 * in the 16-bit window field, which the peer multiplies back up to
 * the full buffer size.  Without scaling the cap was 64 KiB, which
 * throttled CF responses past ~15 KiB once the application drain
 * rate lagged the inbound burst (the bge-large /embed body is
 * the canonical reproducer at 19 KiB).  Memory cost:
 * TCP_MAX_CONNS (32) × 128 KiB ≈ 4 MiB BSS — acceptable on 512 MiB. */
#define TCP_RX_BUF_SIZE 131072
#define TCP_RX_WSCALE   3   /* log2 of the granularity we advertise */
#define TCP_TX_BUF_SIZE 4096
#define TCP_MAX_CONNS   32
#define TCP_MSS         1460

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

    /* Retransmit state (RFC 5681) */
    uint8_t   tx_buf[TCP_TX_BUF_SIZE]; /* unACKed sent data */
    uint32_t  tx_len;           /* bytes in tx_buf */
    uint32_t  tx_seq;           /* sequence number of tx_buf[0] */
    uint64_t  rto_tick;         /* next retransmit time (ticks) */
    uint32_t  rto_count;        /* retransmit attempts */
    uint32_t  dup_ack_count;    /* consecutive duplicate ACKs */

    /* Window scaling (RFC 7323) */
    uint8_t   snd_wscale;      /* remote window scale factor */
    uint8_t   rcv_wscale;      /* our window scale (log2 of RX buf) */
    uint32_t  snd_wnd;         /* remote advertised window (scaled) */

    /* Selective ACK (RFC 2018). Receiver-side state: when an out-of-order
     * segment arrives we record its [start, end) sequence range so the
     * next outgoing ACK can advertise it via the SACK option (kind=5).
     * Up to 4 blocks per RFC; the most-recent block is shipped first
     * (matches Linux + the RFC 2883 D-SACK recommendation). Blocks are
     * coalesced when adjacent and discarded as rcv_nxt catches up. */
    uint8_t   sack_ok;            /* peer sent SACK_PERMITTED in SYN */
    uint8_t   n_sack_blocks;      /* 0..4 valid entries in sack_blocks */
    uint32_t  sack_blocks[4][2];  /* [start, end) seq pairs */

    /* TCP timestamps + PAWS (RFC 7323 §3-5). Both sides exchange a
     * 32-bit timestamp on every segment post-handshake. The receiver
     * echoes the most-recent in-order TS Value, which lets the sender
     * measure RTT precisely and lets the receiver discard segments
     * that arrive with a TS older than ts_recent (Protection Against
     * Wrapped Sequences, useful on >32-bit/s links). */
    uint8_t   tsopt_ok;       /* peer sent TS option in SYN */
    uint32_t  ts_recent;      /* most-recent in-order peer TS Value */

    /* RTT smoothing (RFC 6298 Jacobson). srtt=0 means no measurement
     * yet — first sample seeds srtt and rttvar directly. rto is the
     * current dynamic retransmit timeout in ticks; clamped to
     * [100, 60000]. */
    uint32_t  srtt;
    uint32_t  rttvar;
    uint32_t  rto;

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

/* ── Async Network API ───────────────────────────────────────── */

/* Callback: result is operation-specific (conn_idx, 0=success, -1=error) */
typedef void (*net_async_cb_t)(int result, void *ctx);

/* Async DNS resolve — returns immediately, calls cb when done.
 * ip_out must remain valid until callback fires.
 * Returns 0 if queued, -1 if no slots available. */
int net_dns_resolve_async(const char *hostname, uint8_t ip_out[4],
                          net_async_cb_t cb, void *ctx);

/* Async TCP connect — returns immediately, calls cb(conn_idx, ctx).
 * Returns 0 if queued, -1 if no slots available. */
int net_tcp_connect_async(const uint8_t dst_ip[4], uint16_t dst_port,
                          uint16_t src_port,
                          net_async_cb_t cb, void *ctx);

/* Check if any async operations are pending */
int net_async_pending(void);

#endif /* OSITOK_NET_H */
