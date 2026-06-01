/*
 * OsitoK x86-64 — Minimal Network Stack
 *
 * ARP responder + IPv4 + UDP + ICMP + TCP. Polling mode, static IP.
 * TCP is client-only (connect out, no listen/accept).
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "net.h"
#include "../drivers/i211.h"
#include "nic.h"

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

/* ── Scheduler integration (for async net) ──────────────────── */

extern bool sched_is_enabled(void);
extern void sched_yield(void);
extern int  sched_block_current(void);  /* returns proc_idx, sets PROC_BLOCKED */
extern void sched_unblock(int proc_idx);

/* ── Net waiter table (PROC_BLOCKED wakeup from packet handlers) ── */

#define NET_MAX_WAITERS 16

typedef enum {
    NETWAIT_NONE = 0,
    NETWAIT_ARP,
    NETWAIT_TCP_ESTABLISHED,
    NETWAIT_TCP_RX,
    NETWAIT_TCP_CLOSED,
    NETWAIT_TCP_ACCEPT,
} netwait_type_t;

typedef struct {
    netwait_type_t type;
    int            target;      /* conn_idx, listener_idx, or -1 for ARP */
    int            proc_idx;    /* index in proctab[] to wake */
    uint64_t       deadline;    /* absolute tick deadline */
} net_waiter_t;

static net_waiter_t net_waiters[NET_MAX_WAITERS];
static volatile int net_waiter_count;

static int net_waiter_register(netwait_type_t type, int target,
                               uint64_t deadline)
{
    int pidx = sched_block_current();
    if (pidx < 0) return -1;
    for (int i = 0; i < NET_MAX_WAITERS; i++) {
        if (net_waiters[i].type == NETWAIT_NONE) {
            net_waiters[i].type = type;
            net_waiters[i].target = target;
            net_waiters[i].proc_idx = pidx;
            net_waiters[i].deadline = deadline;
            __sync_fetch_and_add(&net_waiter_count, 1);
            return i;
        }
    }
    /* No free slot — unblock ourselves */
    sched_unblock(pidx);
    return -1;
}

static void net_waiter_clear(int slot)
{
    if (slot >= 0 && slot < NET_MAX_WAITERS &&
        net_waiters[slot].type != NETWAIT_NONE) {
        net_waiters[slot].type = NETWAIT_NONE;
        __sync_fetch_and_sub(&net_waiter_count, 1);
    }
}

/* Wake all waiters matching type+target */
static void net_waiter_wake(netwait_type_t type, int target)
{
    for (int i = 0; i < NET_MAX_WAITERS; i++) {
        if (net_waiters[i].type == type &&
            (target < 0 || net_waiters[i].target == target)) {
            sched_unblock(net_waiters[i].proc_idx);
            net_waiters[i].type = NETWAIT_NONE;
            __sync_fetch_and_sub(&net_waiter_count, 1);
        }
    }
}

/* Called by sched_tick to check if net_poll should be invoked */
bool net_has_active_waiters(void) { return net_waiter_count > 0; }

/* ── Forward declarations ────────────────────────────────────── */

static void net_async_check(void);

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
/* 64-byte alignment: hypothesis for the all-NUL SG bug is that I210/I211
 * silently zero-DMAs when desc->addr is not cache-line aligned, even
 * though the datasheet doesn't document an alignment requirement.  The
 * single-desc path's buffer comes from mem_alloc_aligned(., 4096) so it
 * never hits this.  Force tx_pkt to a clean 64-byte boundary and re-test
 * SG before adding further chip-side context-descriptor experiments. */
static uint8_t tx_pkt[2048] __attribute__((aligned(64)));

/* ── Reply-path TX timing experiment (docs/x86-network-stack.md §9) ──
 *
 * Hypothesis-A: the USB-Ethernet bridge on the test Mac drops frames
 * whose TX completion comes within N µs of an RX completion in the
 * same direction.  Frames sent from shell-thread context (ping)
 * succeed; frames sent from sched_tick → net_poll → handle_arp/icmp →
 * eth_send → i211_send do not — even though the I211 reports DD=1 and
 * GPTC++.  Both paths share the exact same i211_send code.
 *
 * Experiment: stamp the TSC at end of the net_poll RX loop, and have
 * eth_send busy-wait until at least g_tx_post_rx_delay_us microseconds
 * have elapsed since that stamp.  A shell builtin (`txdelay <us>`)
 * tweaks the threshold live, so we can sweep [0, 5000] and see if any
 * delay value flips the reply-path from broken to working.
 *
 * If a threshold exists → hypothesis-A confirmed, follow-up: build a
 * proper deferred-TX queue that runs from the next sched_tick.  If
 * even 5 ms doesn't help → hypothesis-A refuted; reach for an external
 * sniffer (hypothesis-B/C).                                          */
volatile uint32_t g_tx_post_rx_delay_us;
static volatile uint64_t last_rx_complete_tsc;
/* TSC ticks per µs — calibrated once at boot, defaults to 3500 for a
 * ~3.5 GHz CPU when calibration hasn't run yet.  Better-than-nothing
 * for the experiment; if Phase-2 finds a threshold we'll tighten the
 * conversion using `cpu_features_tsc_freq()`.                         */
static uint64_t tsc_per_us = 3500;
void net_pre_tx_wait(void)
{
    if (g_tx_post_rx_delay_us == 0) return;
    uint64_t deadline = last_rx_complete_tsc +
                        (uint64_t)g_tx_post_rx_delay_us * tsc_per_us;
    uint64_t now;
    do {
        __asm__ volatile ("pause" ::: "memory");
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        now = ((uint64_t)hi << 32) | lo;
    } while (now < deadline);
}
static inline uint64_t net_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

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

    /* Reply-path TX timing experiment — pace TX after recent RX. */
    net_pre_tx_wait();
    return nic_send(tx_pkt, frame_len);
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

/* sched_tick (process.c) llama esto en cada IRQ del APIC timer para
 * decidir si drenar el RX ring.  Sin esto, frames entrantes (ICMP,
 * ARP del peer) se acumulan en el ring hasta que el HW empieza a
 * dropear — desde fuera el OsitoK aparece "muerto en red" aunque el
 * kernel está vivo.                                                    */
bool net_nic_irq_pending(void)
{
    return nic_ops.irq_pending && *nic_ops.irq_pending;
}

/* APIPA: lookup non-blocking — 0 si entry válida, -1 si no.              */
int net_arp_lookup_nowait(const uint8_t ip[4], uint8_t mac_out[6])
{
    arp_entry_t *e = arp_lookup(ip);
    if (!e) return -1;
    memcpy(mac_out, e->mac, ETH_ALEN);
    return 0;
}

/* APIPA: ARP request público (probe / announce).  Forward-declared
 * porque el static arp_send_request está más abajo.                      */
static void arp_send_request(const uint8_t *target_ip);
void net_arp_probe(const uint8_t target_ip[4])
{
    arp_send_request(target_ip);
}

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

    /* Wake any process blocked on ARP resolution */
    net_waiter_wake(NETWAIT_ARP, -1);  /* -1 = wake all ARP waiters */

    if (ntohs(arp->oper) == ARP_OP_REQUEST && ip_eq(arp->tpa, our_ip)) {
        /* ARP request for our IP — send reply */
        serial_puts("[NET] ARP request from ");
        serial_putdec(arp->spa[0]); serial_puts(".");
        serial_putdec(arp->spa[1]); serial_puts(".");
        serial_putdec(arp->spa[2]); serial_puts(".");
        serial_putdec(arp->spa[3]); serial_puts(" -> sending reply\n");

        arp_send_reply(arp->sha, arp->spa);

        serial_puts("[NET] ARP reply sent (eth_send returned)\n");
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

    nic_send(tx_pkt, frame_len);
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
    if (len < sizeof(ipv4_hdr_t)) return;

    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)pkt;

    /* Only log packets that are ICMP, or are addressed specifically to
     * us — skip the mDNS/multicast flood that exhausts the limit before
     * the shell is even up. */
    static int ipv4_dbg = 0;
    bool is_icmp     = (ip->proto == 1);
    bool is_for_us   = ip_eq(ip->dst, our_ip);
    bool log_this    = (is_icmp || is_for_us) && (ipv4_dbg++ < 64);

    /* Only IPv4, no options (IHL=5) */
    if ((ip->ver_ihl & 0xF0) != 0x40) {
        if (log_this) { serial_puts("[IPv4] drop ver_ihl="); serial_puthex(ip->ver_ihl, 2); serial_puts("\n"); }
        return;
    }

    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    uint32_t total = ntohs(ip->total_len);

    if (total > len || ihl > total) {
        if (log_this) { serial_puts("[IPv4] drop bad-len total="); serial_putdec(total); serial_puts(" ihl="); serial_putdec(ihl); serial_puts(" len="); serial_putdec(len); serial_puts("\n"); }
        return;
    }

    if (log_this) {
        serial_puts("[IPv4] rx src=");
        serial_putdec(ip->src[0]); serial_puts("."); serial_putdec(ip->src[1]); serial_puts(".");
        serial_putdec(ip->src[2]); serial_puts("."); serial_putdec(ip->src[3]);
        serial_puts(" dst=");
        serial_putdec(ip->dst[0]); serial_puts("."); serial_putdec(ip->dst[1]); serial_puts(".");
        serial_putdec(ip->dst[2]); serial_puts("."); serial_putdec(ip->dst[3]);
        serial_puts(" our=");
        serial_putdec(our_ip[0]); serial_puts("."); serial_putdec(our_ip[1]); serial_puts(".");
        serial_putdec(our_ip[2]); serial_puts("."); serial_putdec(our_ip[3]);
        serial_puts(" proto="); serial_putdec(ip->proto);
        serial_puts(" total="); serial_putdec(total); serial_puts("\n");
    }

    /* Check destination is us */
    if (!ip_eq(ip->dst, our_ip)) {
        if (log_this) serial_puts("[IPv4] drop: dst != our_ip\n");
        return;
    }

    /* Verify header checksum */
    if (ip_checksum(ip, ihl) != 0) {
        if (log_this) serial_puts("[IPv4] drop: bad header checksum\n");
        return;
    }

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
    nic_get_mac(our_mac);

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
    return nic_send(tx_pkt, frame_len);
}

/* Forward declaration for retransmit in net_poll */
static int tcp_send_segment(tcp_conn_t *conn, uint8_t flags,
                            const void *data, uint32_t len);

/* ── Poll for Incoming Packets ───────────────────────────────── */

static volatile bool in_net_poll;

void __hot net_poll(void)
{
    /* Reentrancy guard: sched_tick may call net_poll() while a process
     * is already inside it.  Skip if we're already polling. */
    if (__sync_lock_test_and_set(&in_net_poll, 1)) return;

    uint32_t len = 0;

    /* NAPI: check if interrupt flagged pending packets */
    bool was_irq = nic_ops.irq_pending && *nic_ops.irq_pending;

    while (nic_recv(rx_pkt, &len) == 0) {
        if (len < ETH_HDR_LEN)
            continue;

        const eth_hdr_t *eth = (const eth_hdr_t *)rx_pkt;
        uint16_t ethertype = ntohs(eth->ethertype);
        const uint8_t *payload = rx_pkt + ETH_HDR_LEN;
        uint32_t payload_len = len - ETH_HDR_LEN;

        /* Log every received frame's headline (rate-limited).  Confirms
         * net_poll is being driven and packets reach the dispatch.       */
        static int rx_dbg_n = 0;
        if (rx_dbg_n++ < 64) {
            serial_puts("[NET] rx eth dst=");
            for (int i=0;i<6;i++){serial_puthex(eth->dst[i],2); if(i<5)serial_puts(":");}
            serial_puts(" src=");
            for (int i=0;i<6;i++){serial_puthex(eth->src[i],2); if(i<5)serial_puts(":");}
            serial_puts(" type=0x"); serial_puthex(ethertype, 4);
            serial_puts(" len="); serial_putdec(len);
            serial_puts("\n");
        }

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

    /* Reply-path TX timing experiment — stamp the moment the RX loop
     * finished so eth_send (called from handle_arp/icmp) can pace its
     * subsequent TX relative to this point.                            */
    last_rx_complete_tsc = net_rdtsc();

    /* NAPI: ring drained — re-enable RX interrupt if it was the trigger.
     * Sólo el path de I211 expone i211_rx_irq_reenable; en RTL8111 el
     * IMR queda armado y no necesita re-arm explícito tras el ack del
     * ISR (escritura write-1-to-clear en el handler ya re-activa).         */
    if (was_irq) {
        if (nic_ops.irq_pending) *nic_ops.irq_pending = false;
        extern void i211_rx_irq_reenable(void) __attribute__((weak));
        if (i211_rx_irq_reenable) i211_rx_irq_reenable();
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

        /* Exponential backoff (RFC 6298 §5.5). Start from the
         * Jacobson-smoothed RTO if we have one, fall back to 300
         * ticks when no RTT sample has landed yet. Cap at 60 s. */
        uint32_t backoff = tc->rto ? tc->rto : 300;
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

    /* Advance async operations (DNS wait, ARP wait, SYN wait) */
    net_async_check();

    __sync_lock_release(&in_net_poll);
}

/* ── Scheduler-aware poll+yield ─────────────────────────────── */

void net_poll_wait(void)
{
    net_poll();
    if (sched_is_enabled())
        sched_yield();
    else
        __asm__ volatile ("hlt");
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

    /* Pad small frames (Ethernet requires ≥60 bytes on the wire). */
    uint32_t hdr_total = ETH_HDR_LEN + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t);
    uint32_t frame_len = ETH_HDR_LEN + ip_total;

    /* SG path DISABLED — kupload --dmesg via SG sends pure NUL even with
     * tx_pkt 64B-aligned (6bdcbc7) + IFCS-on-last + PAYLEN-on-last
     * (99cda9b) + kvirt_to_phys translation (9f7255f) + heap-alloc pkt
     * (2398adf).  Root cause still unclear — suspect chip-side state
     * we're not configuring, or a descriptor field combo i210/i211
     * rejects in MSI mode.  Fall back to memcpy path; it's correct &
     * adequate for kernel traffic.  Follow-up: capture TX on external
     * sniffer to confirm whether frames leave the wire at all when SG
     * is enabled.  See docs/x86-network-stack.md §SG-pending.        */
    (void)hdr_total;

    /* Copy payload into tx_pkt */
    memcpy(tx_pkt + hdr_total, data, len);

    /* Pad and send */
    if (frame_len < 60) {
        memset(tx_pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }

    return nic_send(tx_pkt, frame_len);
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

/* Sequence-number comparators (32-bit modular arithmetic, RFC 1323). */
static inline int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) >  0; }
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) <  0; }

/* Record an out-of-order [start, end) range so the next outgoing ACK
 * advertises it via SACK (RFC 2018). Coalesces with adjacent blocks
 * and moves the touched block to the front (most-recent first, per
 * RFC 2018 §4 ordering). Up to 4 blocks; older entries fall off. */
static void tcp_sack_add_block(tcp_conn_t *conn, uint32_t start, uint32_t end)
{
    if (!conn->sack_ok || seq_le(end, start)) return;
    for (uint8_t i = 0; i < conn->n_sack_blocks; i++) {
        uint32_t *b = conn->sack_blocks[i];
        if (seq_le(start, b[1]) && seq_ge(end, b[0])) {
            uint32_t s = seq_le(start, b[0]) ? start : b[0];
            uint32_t e = seq_ge(end,   b[1]) ? end   : b[1];
            for (uint8_t j = i; j > 0; j--) {
                conn->sack_blocks[j][0] = conn->sack_blocks[j - 1][0];
                conn->sack_blocks[j][1] = conn->sack_blocks[j - 1][1];
            }
            conn->sack_blocks[0][0] = s;
            conn->sack_blocks[0][1] = e;
            return;
        }
    }
    uint8_t n = conn->n_sack_blocks < 4 ? conn->n_sack_blocks : 3;
    for (uint8_t i = n; i > 0; i--) {
        conn->sack_blocks[i][0] = conn->sack_blocks[i - 1][0];
        conn->sack_blocks[i][1] = conn->sack_blocks[i - 1][1];
    }
    conn->sack_blocks[0][0] = start;
    conn->sack_blocks[0][1] = end;
    if (conn->n_sack_blocks < 4) conn->n_sack_blocks++;
}

/* Discard SACK blocks fully covered by the advanced rcv_nxt. */
static void tcp_sack_drain(tcp_conn_t *conn)
{
    uint8_t w = 0;
    for (uint8_t r = 0; r < conn->n_sack_blocks; r++) {
        if (seq_ge(conn->rcv_nxt, conn->sack_blocks[r][1])) continue;
        if (w != r) {
            conn->sack_blocks[w][0] = conn->sack_blocks[r][0];
            conn->sack_blocks[w][1] = conn->sack_blocks[r][1];
        }
        w++;
    }
    conn->n_sack_blocks = w;
}

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

    /* SYN options layout (20 bytes, header = 40 bytes total — the RFC
     * 1122 ceiling common stacks accept):
     *   MSS (4):       kind=2, len=4, mss=1460
     *   NOP (1):       kind=1                  — align WS to 4
     *   WS  (3):       kind=3, len=3, shift=TCP_RX_WSCALE
     *   SACK_OK (2):   kind=4, len=2           — RFC 2018
     *   TS (10):       kind=8, len=10, val, echo  — RFC 7323
     *
     * Non-SYN options layout (variable, all 4-byte aligned). SACK block
     * count N is in [1..4] (RFC 2018 §3 caps at 4). The SACK option
     * itself contributes 2 NOPs + 2-byte header + 8*N = 4 + 8*N bytes.
     *   TS only (10) + 2 NOPs              — emit_ts only        → 12B
     *   SACK only (N blocks)               — emit_sack only      → 4 + 8*N B
     *   TS + SACK (N blocks)               — both present        → 12 + 4 + 8*N B
     *
     * The TCP data-offset field is 4 bits → max header 60 bytes. With
     * TS we can fit at most 3 SACK blocks (20+12+4+24=60); without TS
     * all 4 blocks fit (20+4+32=56). Cap n_emit_sack accordingly so the
     * header never overflows 60 bytes.
     *
     * Plain ACKs without TS or SACK stay at the bare 20-byte header. */
    bool emit_sack = (!(flags & TCP_SYN)) && conn->sack_ok &&
                     conn->n_sack_blocks > 0;
    bool emit_ts   = (!(flags & TCP_SYN)) && conn->tsopt_ok;
    /* Number of SACK blocks we'll actually serialize (0 if !emit_sack).
     * Cap at 4 (RFC 2018), and at 3 when TS coexists (data-offset 60B
     * ceiling). conn->n_sack_blocks is already 0..4. */
    uint8_t n_emit_sack = 0;
    if (emit_sack) {
        n_emit_sack = conn->n_sack_blocks;
        if (n_emit_sack > 4) n_emit_sack = 4;
        if (emit_ts && n_emit_sack > 3) n_emit_sack = 3;
    }
    uint32_t tcp_hdr_len;
    if (flags & TCP_SYN) {
        tcp_hdr_len = 40;
    } else {
        tcp_hdr_len = 20;
        if (emit_ts)     tcp_hdr_len += 12;             /* 2 NOPs + TS(10) */
        if (n_emit_sack) tcp_hdr_len += 4 + 8 * n_emit_sack; /* 2 NOPs + kind/len + 8*N */
    }
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
    /* Advertise the ACTUAL available window so the peer stops sending
     * when our rx_buf fills.  RFC 7323 §2.2: the SYN itself MUST carry
     * the *unscaled* window value; only post-handshake segments scale.
     * For non-SYN segments we right-shift by rcv_wscale so the 16-bit
     * field can represent up to (65535 << rcv_wscale) bytes of credit. */
    uint32_t free_window = (TCP_RX_BUF_SIZE > conn->rx_len)
                         ? (TCP_RX_BUF_SIZE - conn->rx_len) : 0;
    uint16_t wnd_field;
    if (flags & TCP_SYN) {
        /* Unscaled — peer doesn't yet know our wscale, must take the
         * literal value.  Clamp to 16-bit. */
        wnd_field = (free_window > 65535) ? 65535 : (uint16_t)free_window;
    } else {
        uint32_t scaled = free_window >> conn->rcv_wscale;
        wnd_field = (scaled > 65535) ? 65535 : (uint16_t)scaled;
    }
    tcp->window   = htons(wnd_field);
    tcp->checksum = 0;
    tcp->urgent   = 0;

    uint8_t *opt = tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t) + 20;
    uint32_t ts_now = (uint32_t)idt_get_ticks();

    if (flags & TCP_SYN) {
        /* MSS (kind=2, len=4, value=1460) */
        opt[0] = 0x02; opt[1] = 0x04;
        opt[2] = (uint8_t)(TCP_MSS >> 8); opt[3] = (uint8_t)(TCP_MSS & 0xFF);
        /* NOP (kind=1) for 4-byte alignment of the next option */
        opt[4] = 0x01;
        /* Window scale (kind=3, len=3, shift=TCP_RX_WSCALE) */
        opt[5] = 0x03; opt[6] = 0x03; opt[7] = (uint8_t)TCP_RX_WSCALE;
        /* SACK_PERMITTED (kind=4, len=2) — RFC 2018 */
        opt[8] = 0x04; opt[9] = 0x02;
        /* Timestamps (kind=8, len=10, TSval, TSecr) — RFC 7323. On the
         * initial SYN TS Echo Reply is 0; in SYN-ACK it echoes the SYN's
         * TS Value. Non-SYN segments below use the same code path. */
        opt[10] = 0x08; opt[11] = 0x0A;
        uint32_t tsval = htonl(ts_now);
        uint32_t tsecr = htonl((flags & TCP_ACK) ? conn->ts_recent : 0);
        memcpy(opt + 12, &tsval, 4);
        memcpy(opt + 16, &tsecr, 4);
    } else if (emit_ts || emit_sack) {
        uint32_t off = 0;
        if (emit_ts) {
            /* Two leading NOPs to 4-byte-align the TS option */
            opt[off++] = 0x01; opt[off++] = 0x01;
            opt[off++] = 0x08; opt[off++] = 0x0A;
            uint32_t tsval = htonl(ts_now);
            uint32_t tsecr = htonl(conn->ts_recent);
            memcpy(opt + off, &tsval, 4); off += 4;
            memcpy(opt + off, &tsecr, 4); off += 4;
        }
        if (n_emit_sack) {
            /* Two NOPs + multi-block SACK (RFC 2018). Each block is 8
             * bytes (left edge + right edge, network order). Length
             * field = 2 + 8 * N. tcp_sack_add_block keeps most-recent
             * blocks at index 0; we iterate in that order. */
            opt[off++] = 0x01; opt[off++] = 0x01;
            opt[off++] = 0x05;                       /* kind = SACK */
            opt[off++] = (uint8_t)(2 + 8 * n_emit_sack); /* len */
            for (uint8_t i = 0; i < n_emit_sack; i++) {
                uint32_t blk_start = htonl(conn->sack_blocks[i][0]);
                uint32_t blk_end   = htonl(conn->sack_blocks[i][1]);
                memcpy(opt + off, &blk_start, 4); off += 4;
                memcpy(opt + off, &blk_end,   4); off += 4;
            }
        }
    }

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

    return nic_send(tx_pkt, frame_len);
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

    /* Walk TCP options. SYN-only options (window scale RFC 7323,
     * SACK_PERMITTED RFC 2018) are captured for handshake state.
     * Timestamps (kind=8) appear on SYN AND on every post-handshake
     * segment — we parse them on every packet so ts_recent stays
     * current for PAWS + RTT echo. SACK blocks on non-SYN ACKs
     * (kind=5) are ignored on the sender side for now. */
    uint8_t peer_wscale = 0;
    bool    peer_has_ws = false;
    bool    peer_sack_ok = false;
    bool    peer_has_ts = false;
    uint32_t peer_tsval = 0;
    uint32_t peer_tsecr = 0;
    bool    peer_has_sack_blk = false;
    uint32_t peer_sack_ranges[4][2];  /* up to 4 SACK blocks (RFC 2018) */
    int      peer_sack_n = 0;
    if (hdr_len > 20) {
        const uint8_t *opt = pkt + 20;
        uint32_t opt_len = hdr_len - 20;
        uint32_t i = 0;
        while (i < opt_len) {
            uint8_t kind = opt[i];
            if (kind == 0) break;            /* EOL */
            if (kind == 1) { i++; continue; } /* NOP */
            if (i + 1 >= opt_len) break;
            uint8_t l = opt[i + 1];
            if (l < 2 || i + l > opt_len) break;
            if (kind == 3 && l == 3 && (flags & TCP_SYN)) {
                peer_wscale = opt[i + 2];
                if (peer_wscale > 14) peer_wscale = 14; /* RFC 7323 cap */
                peer_has_ws = true;
            } else if (kind == 4 && l == 2 && (flags & TCP_SYN)) {
                peer_sack_ok = true;
            } else if (kind == 8 && l == 10) {
                memcpy(&peer_tsval, opt + i + 2, 4);
                memcpy(&peer_tsecr, opt + i + 6, 4);
                peer_tsval = ntohl(peer_tsval);
                peer_tsecr = ntohl(peer_tsecr);
                peer_has_ts = true;
            } else if (kind == 5 && l >= 10 && ((l - 2) % 8 == 0) &&
                       !(flags & TCP_SYN)) {
                /* SACK block list (RFC 2018).  We parse every block
                 * (up to 4 — the option fits at most 4 in 40 bytes of
                 * TCP option space) so the sender can implement
                 * RFC 6675 §4 multi-gap retransmit decisions and
                 * partial-coverage advance of the unACKed window. */
                int nblk = (int)((l - 2) / 8);
                if (nblk > 4) nblk = 4;
                for (int b = 0; b < nblk; b++) {
                    uint32_t s, e;
                    memcpy(&s, opt + i + 2 + b * 8,     4);
                    memcpy(&e, opt + i + 2 + b * 8 + 4, 4);
                    peer_sack_ranges[b][0] = ntohl(s);
                    peer_sack_ranges[b][1] = ntohl(e);
                }
                peer_sack_n = nblk;
                peer_has_sack_blk = (nblk > 0);
            }
            i += l;
        }
    }

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
        conn->sack_ok     = peer_sack_ok ? 1 : 0;
        conn->tsopt_ok    = peer_has_ts ? 1 : 0;
        if (peer_has_ts) conn->ts_recent = peer_tsval;
        conn->last_activity = idt_get_ticks();

        /* Send SYN+ACK. If ARP isn't resolved this returns -1 and
         * tcp_send_segment fires an ARP request as a side effect.
         * The connection still sits in SYN_RCVD; the host's RTO
         * SYN retransmit will hit the SYN_RCVD case and retry the
         * SYN+ACK with ARP now warm. */
        int snd_rc = tcp_send_segment(conn, TCP_SYN | TCP_ACK, NULL, 0);

        /* Notify listener */
        listener->pending_conn = new_idx;

        serial_puts(snd_rc == 0
                    ? "[TCP] SYN received, sent SYN+ACK (conn "
                    : "[TCP] SYN received, SYN+ACK deferred (ARP) (conn ");
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
        net_waiter_wake(NETWAIT_TCP_CLOSED, conn_idx);
        net_waiter_wake(NETWAIT_TCP_ESTABLISHED, conn_idx);
        net_waiter_wake(NETWAIT_TCP_RX, conn_idx);
        return;
    }

    switch (conn->state) {
    case TCP_SYN_RCVD:
        /* Expecting ACK of our SYN+ACK → transition to ESTABLISHED */
        if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
            conn->snd_una = ack;
            conn->state = TCP_ESTABLISHED;
            net_waiter_wake(NETWAIT_TCP_ESTABLISHED, conn_idx);
            net_waiter_wake(NETWAIT_TCP_ACCEPT, -1);  /* wake all accept waiters */
            serial_puts("[TCP] Accepted (conn ");
            serial_putdec(conn_idx);
            serial_puts(")\n");
        } else if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            /* Duplicate SYN — host's RTO retransmit. Our original
             * SYN+ACK was likely dropped (e.g. ARP miss on first
             * try). Reset snd_nxt back to the ISN (snd_una still
             * holds it) and re-send. tcp_send_segment will bump
             * snd_nxt by 1 again for the SYN flag.
             *
             * Without this, the listener stays stuck in SYN_RCVD
             * forever — silent SYN+ACK drops on the cold path
             * (ARP cache miss, transient TX failure) become
             * permanent connection failures. */
            conn->snd_nxt = conn->snd_una;
            int rc = tcp_send_segment(conn, TCP_SYN | TCP_ACK, NULL, 0);
            if (rc == 0) {
                serial_puts("[TCP] SYN+ACK re-sent (conn ");
                serial_putdec(conn_idx);
                serial_puts(")\n");
            }
        }
        break;

    case TCP_SYN_SENT:
        /* Expecting SYN+ACK */
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == conn->snd_nxt) {
                conn->rcv_nxt = seq + 1;
                conn->snd_una = ack;
                /* RFC 7323 §2.2: both sides must advertise WS on SYN
                 * for scaling to apply.  If the peer omitted it (some
                 * legacy boxes), neither side scales — leave
                 * snd_wscale + rcv_wscale at 0, falling back to a
                 * straight 16-bit window. */
                if (peer_has_ws) {
                    conn->snd_wscale = peer_wscale;
                    /* keep our advertised rcv_wscale from connect-time
                     * setup (TCP_RX_WSCALE) — we asked for it in our
                     * SYN, peer ack'd by sending its own WS option. */
                } else {
                    conn->snd_wscale = 0;
                    conn->rcv_wscale = 0;  /* downgrade — peer can't scale */
                }
                /* RFC 2018 §2.2: SACK only activates when *both* sides
                 * advertise SACK_PERMITTED in their SYN. We always send
                 * it; trust the peer's bit. */
                conn->sack_ok = peer_sack_ok ? 1 : 0;
                /* RFC 7323 §1.3: same rule for timestamps. ts_recent is
                 * primed with the SYN-ACK's TS Value so the first ACK
                 * we send can echo it. */
                conn->tsopt_ok = peer_has_ts ? 1 : 0;
                if (peer_has_ts) conn->ts_recent = peer_tsval;
                conn->state = TCP_ESTABLISHED;
                net_waiter_wake(NETWAIT_TCP_ESTABLISHED, conn_idx);
                /* Send ACK */
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
                serial_puts("[TCP] Connected (conn ");
                serial_putdec(conn_idx);
                if (peer_has_ws) {
                    serial_puts(", wscale snd=");
                    serial_putdec((uint64_t)conn->snd_wscale);
                    serial_puts(" rcv=");
                    serial_putdec((uint64_t)conn->rcv_wscale);
                }
                serial_puts(")\n");
            }
        }
        break;

    case TCP_ESTABLISHED:
        /* ACK processing with retransmit tracking */
        if (flags & TCP_ACK) {
            /* RTT measurement (RFC 6298 Jacobson). The peer's TSecr
             * field echoes the TSval we stamped on an earlier outbound
             * segment — `idt_get_ticks() - TSecr` is a precise sample,
             * free of the Karn ambiguity. We update on every ACK that
             * advances snd_una OR that carries a non-zero TSecr. */
            if (conn->tsopt_ok && peer_has_ts && peer_tsecr != 0) {
                uint64_t now = idt_get_ticks();
                int64_t  sample = (int64_t)((uint32_t)now - peer_tsecr);
                if (sample > 0 && sample < 60000) {
                    if (conn->srtt == 0) {
                        conn->srtt   = (uint32_t)sample;
                        conn->rttvar = (uint32_t)(sample / 2);
                    } else {
                        int32_t diff = (int32_t)sample - (int32_t)conn->srtt;
                        int32_t abs_diff = diff < 0 ? -diff : diff;
                        /* RTTVAR = 3/4 * RTTVAR + 1/4 * |diff| */
                        conn->rttvar = (uint32_t)(
                            ((int64_t)conn->rttvar * 3 + abs_diff) / 4);
                        /* SRTT = 7/8 * SRTT + 1/8 * sample
                         * ≡ SRTT + diff/8 (signed) */
                        conn->srtt = (uint32_t)(
                            (int64_t)conn->srtt + diff / 8);
                    }
                    uint32_t rto = conn->srtt + (conn->rttvar << 2);
                    if (rto < 100)   rto = 100;
                    if (rto > 60000) rto = 60000;
                    conn->rto = rto;
                }
            }

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
                    conn->rto_tick = idt_get_ticks() +
                                     (conn->rto ? conn->rto : 300);
                    conn->rto_count = 0;
                }
            } else if (ack == conn->snd_una && conn->tx_len > 0) {
                /* Duplicate ACK — fast retransmit at 3 dups (RFC 5681).
                 * RFC 6675 SACK-aware refinement: if the peer's SACK
                 * block covers our entire unACKed tx_buf, the segment
                 * arrived (just out-of-order). The missing piece is
                 * BEFORE tx_seq, not at tx_buf — so retransmitting
                 * tx_buf would burn bandwidth without making progress.
                 * Postpone rto and wait for the cumulative ACK. */
                bool tx_sacked = false;
                if (peer_has_sack_blk && conn->sack_ok) {
                    uint32_t tx_end = conn->tx_seq + conn->tx_len;
                    /* RFC 6675 §4 (a): walk every SACK block.  If ANY
                     * one fully covers [tx_seq, tx_end), the entire
                     * unACKed window is already at the receiver and
                     * we must not retransmit.  Otherwise, if a block
                     * covers a prefix [tx_seq, X) with X < tx_end,
                     * slide the unACKed window forward by (X - tx_seq)
                     * so the next retransmit (or RTO) only re-sends
                     * the still-missing tail. */
                    for (int b = 0; b < peer_sack_n; b++) {
                        uint32_t ss = peer_sack_ranges[b][0];
                        uint32_t se = peer_sack_ranges[b][1];
                        if (seq_le(ss, conn->tx_seq) &&
                            seq_ge(se, tx_end)) {
                            tx_sacked = true;
                            break;
                        }
                    }
                    if (!tx_sacked) {
                        /* Partial-prefix advance: find the largest X
                         * such that some block covers [tx_seq, X).
                         * Multiple blocks could each cover a prefix —
                         * pick the longest one. */
                        uint32_t best_x = conn->tx_seq;
                        for (int b = 0; b < peer_sack_n; b++) {
                            uint32_t ss = peer_sack_ranges[b][0];
                            uint32_t se = peer_sack_ranges[b][1];
                            if (seq_le(ss, conn->tx_seq) &&
                                seq_gt(se, best_x) &&
                                seq_lt(se, tx_end)) {
                                best_x = se;
                            }
                        }
                        if (best_x != conn->tx_seq) {
                            uint32_t adv = best_x - conn->tx_seq;
                            if (adv < conn->tx_len) {
                                uint32_t remain = conn->tx_len - adv;
                                for (uint32_t bi = 0; bi < remain; bi++)
                                    conn->tx_buf[bi] = conn->tx_buf[bi + adv];
                                conn->tx_len  = remain;
                                conn->tx_seq  = best_x;
                            }
                        }
                    }
                }
                conn->dup_ack_count++;
                if (conn->dup_ack_count >= 3 && !tx_sacked) {
                    uint32_t chunk = conn->tx_len;
                    if (chunk > TCP_MSS) chunk = TCP_MSS;
                    tcp_send_segment(conn, TCP_ACK | TCP_PSH,
                                     conn->tx_buf, chunk);
                    conn->dup_ack_count = 0;
                    conn->rto_tick = idt_get_ticks() +
                                     (conn->rto ? conn->rto : 300);
                } else if (tx_sacked) {
                    /* SACK proves the receiver has our data. Slide the
                     * timer forward so we don't fire a spurious RTO
                     * while waiting for the cumulative ACK that's
                     * blocked behind whatever the receiver is missing. */
                    conn->rto_tick = idt_get_ticks() +
                                     (conn->rto ? conn->rto : 300);
                }
            }
        }

        /* PAWS (RFC 7323 §5.3): drop segments that arrived with a TS
         * Value older than ts_recent. On a 1 Gb/s link the 32-bit seq
         * space wraps in ~34 s — without PAWS a delayed packet from a
         * previous wrap could be wrongly accepted as new in-window data.
         * We don't enforce the 24-day idle reset (§5.5) — connections
         * that idle that long fall out of our retransmit budget anyway. */
        if (conn->tsopt_ok && peer_has_ts &&
            (int32_t)(peer_tsval - conn->ts_recent) < 0) {
            /* Stale segment — send a current ACK to refresh the peer
             * (per §5.3 "an old duplicate" handling) and drop the data. */
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        } else if (data_len > 0 && seq == conn->rcv_nxt) {
            uint32_t space = TCP_RX_BUF_SIZE - conn->rx_len;
            uint32_t copy = data_len < space ? data_len : space;
            if (copy > 0) {
                memcpy(conn->rx_buf + conn->rx_len, data, copy);
                conn->rx_len += copy;
            }
            conn->rcv_nxt += data_len;
            tcp_sack_drain(conn);
            /* RFC 7323 §3.4: only update ts_recent on in-order data
             * (the segment's TS Value is the freshest the peer has
             * sent so far). */
            if (conn->tsopt_ok && peer_has_ts)
                conn->ts_recent = peer_tsval;
            /* ACK the data */
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
            /* Wake any process blocked on recv */
            net_waiter_wake(NETWAIT_TCP_RX, conn_idx);
        } else if (data_len > 0 && (int32_t)(seq - conn->rcv_nxt) > 0) {
            /* Out-of-order — record [seq, seq+data_len) as a SACK block
             * and send a duplicate ACK (rcv_nxt unchanged). The peer's
             * fast-retransmit logic picks up the missing range. */
            tcp_sack_add_block(conn, seq, seq + data_len);
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        }

        /* FIN from remote */
        if (flags & TCP_FIN) {
            conn->rcv_nxt = seq + data_len + 1;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
            net_waiter_wake(NETWAIT_TCP_RX, conn_idx);
            net_waiter_wake(NETWAIT_TCP_CLOSED, conn_idx);
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
            net_waiter_wake(NETWAIT_TCP_CLOSED, conn_idx);
        } else if (flags & TCP_ACK) {
            conn->state = TCP_FIN_WAIT_2;
        } else if (flags & TCP_FIN) {
            conn->rcv_nxt = seq + data_len + 1;
            conn->state = TCP_TIME_WAIT;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
            net_waiter_wake(NETWAIT_TCP_CLOSED, conn_idx);
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
            net_waiter_wake(NETWAIT_TCP_CLOSED, conn_idx);
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_ACK) && ack == conn->snd_nxt) {
            conn->state = TCP_CLOSED;
            net_waiter_wake(NETWAIT_TCP_CLOSED, conn_idx);
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
        uint64_t arp_deadline = arp_start + 200;
        if (sched_is_enabled()) {
            int slot = net_waiter_register(NETWAIT_ARP, -1, arp_deadline);
            if (slot >= 0) {
                while (!arp_lookup(nexthop) && idt_get_ticks() < arp_deadline) {
                    __asm__ volatile ("sti; hlt; cli" ::: "memory");
                    /* virtio-net is polled (irq_pending=NULL) — drain
                     * the RX queue here so the ARP reply doesn't sit
                     * in virtqueue indefinitely. APIC tick wakes us
                     * from hlt; net_poll() actually reads the packet. */
                    net_poll();
                }
                net_waiter_clear(slot);
            }
        } else {
            while (!arp_lookup(nexthop) && idt_get_ticks() < arp_deadline)
                net_poll_wait();
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
    conn->rcv_wscale = 3;  /* We advertise window scale 3 (8KB << 3 = 64KB) */
    conn->snd_wscale = 0;  /* Updated when we receive SYN+ACK with WS option */
    conn->snd_wnd = TCP_RX_BUF_SIZE;
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
    uint64_t syn_deadline = start + 500;
    if (sched_is_enabled()) {
        int slot = net_waiter_register(NETWAIT_TCP_ESTABLISHED, idx,
                                       syn_deadline);
        if (slot >= 0) {
            while (conn->state == TCP_SYN_SENT && idt_get_ticks() < syn_deadline) {
                __asm__ volatile ("sti; hlt; cli" ::: "memory");
                /* Polled-NIC drain — see ARP wait comment above. */
                net_poll();
            }
            net_waiter_clear(slot);
        }
    } else {
        while (conn->state == TCP_SYN_SENT && idt_get_ticks() < syn_deadline)
            net_poll_wait();
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
        uint32_t pre_len = conn->rx_len;
        uint32_t copy = conn->rx_len < buf_size ? conn->rx_len : buf_size;
        memcpy(buf, conn->rx_buf, copy);
        /* Shift remaining data down (forward copy, dst < src, so safe) */
        if (copy < conn->rx_len) {
            uint32_t remain = conn->rx_len - copy;
            for (uint32_t i = 0; i < remain; i++)
                conn->rx_buf[i] = conn->rx_buf[copy + i];
        }
        conn->rx_len -= copy;

        /* Window-update ACK: if the buffer was near-full before the
         * drain and is now substantially free, send a pure-ACK so the
         * peer notices our window reopened.  The advertised window in
         * the ACK comes from tcp_send_segment, which now reads
         * (TCP_RX_BUF_SIZE - rx_len) dynamically — so this ACK
         * effectively carries the new credit.  Without this update the
         * peer keeps treating our window as the value from the last
         * ACK we sent (often near zero), and never resumes sending
         * even though we just freed thousands of bytes. */
        if (conn->state == TCP_ESTABLISHED &&
            pre_len > (TCP_RX_BUF_SIZE / 2) &&
            conn->rx_len <= (TCP_RX_BUF_SIZE / 2)) {
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        }
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

    uint64_t deadline = idt_get_ticks() + timeout_ticks;

    while (idt_get_ticks() < deadline) {
        net_poll();

        int r = net_tcp_recv(conn_idx, buf, buf_size);
        if (r != 0)
            return r;  /* Data or closed */

        if (sched_is_enabled()) {
            int slot = net_waiter_register(NETWAIT_TCP_RX, conn_idx, deadline);
            if (slot >= 0) {
                /* PROC_BLOCKED — scheduler runs other procs; woken by
                 * handle_tcp when data arrives or by timeout in
                 * net_async_check */
                while (idt_get_ticks() < deadline) {
                    __asm__ volatile ("sti; hlt; cli" ::: "memory");
                    net_poll();
                    r = net_tcp_recv(conn_idx, buf, buf_size);
                    if (r != 0) {
                        net_waiter_clear(slot);
                        __asm__ volatile ("sti" ::: "memory"); /* leave IF=1 */
                        return r;
                    }
                    /* If we were woken but no data yet, re-register */
                    if (net_waiters[slot].type == NETWAIT_NONE) {
                        /* Waiter was cleared (wakeup or timeout) */
                        break;
                    }
                }
                net_waiter_clear(slot);
                /* Re-enable IF before any later code runs — the inner
                 * loop's trailing `cli` would otherwise leak out and any
                 * subsequent bare `hlt` (here or in a caller, e.g. a
                 * kthread returning into sched_thread_exit) could halt the
                 * BSP forever (no IRQ left to wake it). Ported from
                 * osito-a@7a72b06. */
                __asm__ volatile ("sti" ::: "memory");
                /* Re-check after wakeup */
                r = net_tcp_recv(conn_idx, buf, buf_size);
                if (r != 0) return r;
            } else {
                sched_yield();
            }
        } else {
            __asm__ volatile ("hlt");
        }
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
    uint64_t close_deadline = idt_get_ticks() + 300;
    if (sched_is_enabled()) {
        int slot = net_waiter_register(NETWAIT_TCP_CLOSED, conn_idx,
                                       close_deadline);
        if (slot >= 0) {
            while (conn->state != TCP_CLOSED && conn->state != TCP_TIME_WAIT &&
                   idt_get_ticks() < close_deadline) {
                __asm__ volatile ("sti; hlt; cli" ::: "memory");
                /* Polled-NIC drain — see ARP wait comment above. */
                net_poll();
            }
            net_waiter_clear(slot);
        }
    } else {
        while (conn->state != TCP_CLOSED && conn->state != TCP_TIME_WAIT &&
               idt_get_ticks() < close_deadline)
            net_poll_wait();
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

/* ── Cluster helpers ─────────────────────────────────────────── */

/* Send to a class-D group. Skips ARP — multicast MAC is derived
 * from the low 23 bits of the IPv4 group (RFC 1112 §6.4):
 *   01:00:5E:[bit23..0 of group_ip]
 * Send-only (no IGMP membership). Used by inferconnect peer
 * announcements + cluster_rendezvous LAN discovery. */
int net_udp_send_multicast(const uint8_t group_ip[4], uint16_t dst_port,
                            uint16_t src_port, const void *data, uint32_t len)
{
    if ((group_ip[0] & 0xF0) != 0xE0) return -1;

    uint32_t udp_len = sizeof(udp_hdr_t) + len;
    uint32_t ip_total = sizeof(ipv4_hdr_t) + udp_len;
    if (ETH_HDR_LEN + ip_total > sizeof(tx_pkt)) return -1;

    uint8_t mc_mac[6] = {
        0x01, 0x00, 0x5E,
        (uint8_t)(group_ip[1] & 0x7F), group_ip[2], group_ip[3],
    };

    eth_hdr_t *eth = (eth_hdr_t *)tx_pkt;
    memcpy(eth->dst, mc_mac, 6);
    memcpy(eth->src, our_mac, 6);
    eth->ethertype = htons(ETH_TYPE_IP4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(tx_pkt + ETH_HDR_LEN);
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->total_len = htons((uint16_t)ip_total);
    ip->id = htons(ip_id_counter++); ip->frag = 0;
    ip->ttl = 1; ip->proto = IP_PROTO_UDP; ip->checksum = 0;
    memcpy(ip->src, our_ip, 4); memcpy(ip->dst, group_ip, 4);
    ip->checksum = ip_checksum(ip, sizeof(ipv4_hdr_t));

    udp_hdr_t *udp = (udp_hdr_t *)(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons((uint16_t)udp_len);
    udp->checksum = 0;

    memcpy(tx_pkt + ETH_HDR_LEN + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t),
           data, len);

    uint32_t frame_len = ETH_HDR_LEN + ip_total;
    if (frame_len < 60) { memset(tx_pkt + frame_len, 0, 60 - frame_len); frame_len = 60; }
    return nic_send(tx_pkt, frame_len);
}

int net_tcp_get_peer_ip(int conn_idx, uint8_t ip_out[4])
{
    if (conn_idx < 0 || conn_idx >= TCP_MAX_CONNS) return -1;
    if (tcp_conns[conn_idx].state == TCP_CLOSED)   return -1;
    for (int i = 0; i < 4; i++) ip_out[i] = tcp_conns[conn_idx].remote_ip[i];
    return 0;
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

    uint64_t accept_deadline = idt_get_ticks() + timeout_ticks;

    while (idt_get_ticks() < accept_deadline) {
        net_poll();

        /* Check if a SYN was received and handshake is completing */
        int pc = listener->pending_conn;
        if (pc >= 0 && tcp_conns[pc].state == TCP_ESTABLISHED) {
            listener->pending_conn = -1;
            return pc;
        }

        if (sched_is_enabled()) {
            int slot = net_waiter_register(NETWAIT_TCP_ACCEPT, listener_idx,
                                           accept_deadline);
            if (slot >= 0) {
                while (idt_get_ticks() < accept_deadline) {
                    __asm__ volatile ("sti; hlt; cli" ::: "memory");
                    net_poll();
                    pc = listener->pending_conn;
                    if (pc >= 0 && tcp_conns[pc].state == TCP_ESTABLISHED) {
                        net_waiter_clear(slot);
                        listener->pending_conn = -1;
                        return pc;
                    }
                    if (net_waiters[slot].type == NETWAIT_NONE) break;
                }
                net_waiter_clear(slot);
            } else {
                sched_yield();
            }
        } else {
            __asm__ volatile ("hlt");
        }
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

    /* Send query — retry if ARP not yet resolved.  We use `sti; hlt`
     * (NOT bare `hlt`) so the wait works regardless of what state the
     * caller left interrupts in — `net_tcp_close` is one example of
     * a path that ends with cli (via its `sti; hlt; cli` busy-wait)
     * and would otherwise deadlock the next DNS resolve. */
    serial_puts("[DNS] Resolving ");
    serial_puts(hostname);
    serial_puts("...\n");

    for (int attempt = 0; attempt < 5; attempt++) {
        if (net_udp_send(dns_server, 53, 10053, query, qlen) == 0)
            break;
        net_poll();
        __asm__ volatile ("sti; hlt" ::: "memory");
    }

    /* Poll for response (3s timeout = 300 ticks) */
    uint64_t start = idt_get_ticks();
    while (!dns_got_reply && (idt_get_ticks() - start) < 300) {
        net_poll();
        __asm__ volatile ("sti; hlt" ::: "memory");
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

/* ══════════════════════════════════════════════════════════════
 *  Async Network Operations
 *
 *  Non-blocking variants of DNS, TCP connect, etc.
 *  net_poll() drives progress by checking pending async ops
 *  after processing incoming packets.
 * ══════════════════════════════════════════════════════════════ */

typedef enum {
    NETOP_IDLE = 0,
    NETOP_DNS_WAIT,         /* waiting for dns_got_reply */
    NETOP_ARP_WAIT,         /* waiting for ARP resolution */
    NETOP_TCP_SYN_WAIT,     /* waiting for SYN-ACK (TCP_ESTABLISHED) */
} netop_type_t;

typedef struct {
    netop_type_t    type;
    uint64_t        deadline;       /* absolute tick timeout */
    net_async_cb_t  callback;
    void           *ctx;

    union {
        struct {                    /* DNS */
            uint8_t *ip_out;       /* caller's buffer */
        } dns;
        struct {                    /* TCP connect */
            uint8_t  dst_ip[4];
            uint16_t dst_port;
            uint16_t src_port;
            int      conn_idx;     /* allocated slot, -1 until ARP done */
            uint8_t  phase;        /* 0=ARP, 1=SYN */
        } tcp;
    };
} net_async_op_t;

#define NET_MAX_ASYNC_OPS 8
static net_async_op_t async_ops[NET_MAX_ASYNC_OPS];

static int async_alloc(void)
{
    for (int i = 0; i < NET_MAX_ASYNC_OPS; i++)
        if (async_ops[i].type == NETOP_IDLE) return i;
    return -1;
}

/* Called at the end of net_poll() to advance async operations */
static void net_async_check(void)
{
    uint64_t now = idt_get_ticks();

    for (int i = 0; i < NET_MAX_ASYNC_OPS; i++) {
        net_async_op_t *op = &async_ops[i];
        if (op->type == NETOP_IDLE) continue;

        /* Timeout check */
        if (now >= op->deadline) {
            net_async_cb_t cb = op->callback;
            void *ctx = op->ctx;
            op->type = NETOP_IDLE;
            if (cb) cb(-1, ctx);  /* timeout = error */
            continue;
        }

        switch (op->type) {
        case NETOP_DNS_WAIT:
            if (dns_got_reply) {
                memcpy(op->dns.ip_out, dns_result_ip, 4);
                net_async_cb_t cb = op->callback;
                void *ctx = op->ctx;
                op->type = NETOP_IDLE;
                if (cb) cb(0, ctx);
            }
            break;

        case NETOP_ARP_WAIT: {
            const uint8_t *nexthop = arp_nexthop(op->tcp.dst_ip);
            if (arp_lookup(nexthop)) {
                /* ARP resolved — now send SYN */
                int idx = -1;
                for (int c = 0; c < TCP_MAX_CONNS; c++)
                    if (tcp_conns[c].state == TCP_CLOSED) { idx = c; break; }
                if (idx < 0) {
                    net_async_cb_t cb = op->callback;
                    void *ctx = op->ctx;
                    op->type = NETOP_IDLE;
                    if (cb) cb(-1, ctx);
                    break;
                }

                tcp_conn_t *conn = &tcp_conns[idx];
                memset(conn, 0, sizeof(tcp_conn_t));
                memcpy(conn->remote_ip, op->tcp.dst_ip, 4);
                conn->local_port  = op->tcp.src_port;
                conn->remote_port = op->tcp.dst_port;

                conn->snd_nxt = tcp_isn_counter;
                tcp_isn_counter += 64000;
                conn->snd_una = conn->snd_nxt;
                conn->rcv_wscale = 3;
                conn->snd_wnd = TCP_RX_BUF_SIZE;
                conn->state = TCP_SYN_SENT;
                conn->last_activity = now;

                tcp_send_segment(conn, TCP_SYN, NULL, 0);

                op->tcp.conn_idx = idx;
                op->tcp.phase = 1;
                op->type = NETOP_TCP_SYN_WAIT;
                op->deadline = now + 500;  /* 5s for SYN-ACK */
            }
            break;
        }

        case NETOP_TCP_SYN_WAIT: {
            int ci = op->tcp.conn_idx;
            if (ci >= 0 && tcp_conns[ci].state == TCP_ESTABLISHED) {
                net_async_cb_t cb = op->callback;
                void *ctx = op->ctx;
                int conn_idx = ci;
                op->type = NETOP_IDLE;
                if (cb) cb(conn_idx, ctx);
            } else if (ci >= 0 && tcp_conns[ci].state == TCP_CLOSED) {
                /* RST received */
                net_async_cb_t cb = op->callback;
                void *ctx = op->ctx;
                op->type = NETOP_IDLE;
                if (cb) cb(-1, ctx);
            }
            break;
        }

        default:
            break;
        }
    }

    /* ── Check net waiter deadlines ─────────────────────────────── */
    if (net_waiter_count > 0) {
        for (int i = 0; i < NET_MAX_WAITERS; i++) {
            if (net_waiters[i].type == NETWAIT_NONE) continue;
            if (now >= net_waiters[i].deadline) {
                sched_unblock(net_waiters[i].proc_idx);
                net_waiters[i].type = NETWAIT_NONE;
                __sync_fetch_and_sub(&net_waiter_count, 1);
            }
        }
    }
}

int net_dns_resolve_async(const char *hostname, uint8_t ip_out[4],
                          net_async_cb_t cb, void *ctx)
{
    int slot = async_alloc();
    if (slot < 0) return -1;

    /* Build and send DNS query (reuse logic from sync version) */
    uint8_t query[256];
    uint32_t qlen = 0;

    dns_query_id++;
    query[0] = (uint8_t)(dns_query_id >> 8);
    query[1] = (uint8_t)(dns_query_id & 0xFF);
    query[2] = 0x01; query[3] = 0x00;
    query[4] = 0x00; query[5] = 0x01;
    query[6] = 0x00; query[7] = 0x00;
    query[8] = 0x00; query[9] = 0x00;
    query[10] = 0x00; query[11] = 0x00;
    qlen = 12;

    const char *p = hostname;
    while (*p) {
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
    query[qlen++] = 0;
    query[qlen++] = 0x00; query[qlen++] = 0x01;  /* TYPE A */
    query[qlen++] = 0x00; query[qlen++] = 0x01;  /* CLASS IN */

    dns_got_reply = 0;
    net_udp_listen(10053, dns_handler);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (net_udp_send(dns_server, 53, 10053, query, qlen) == 0)
            break;
        net_poll();
    }

    async_ops[slot].type = NETOP_DNS_WAIT;
    async_ops[slot].deadline = idt_get_ticks() + 300;  /* 3s */
    async_ops[slot].callback = cb;
    async_ops[slot].ctx = ctx;
    async_ops[slot].dns.ip_out = ip_out;
    return 0;
}

int net_tcp_connect_async(const uint8_t dst_ip[4], uint16_t dst_port,
                          uint16_t src_port,
                          net_async_cb_t cb, void *ctx)
{
    int slot = async_alloc();
    if (slot < 0) return -1;

    const uint8_t *nexthop = arp_nexthop(dst_ip);

    if (arp_lookup(nexthop)) {
        /* ARP already resolved — go straight to SYN */
        int idx = -1;
        for (int c = 0; c < TCP_MAX_CONNS; c++)
            if (tcp_conns[c].state == TCP_CLOSED) { idx = c; break; }
        if (idx < 0) return -1;

        tcp_conn_t *conn = &tcp_conns[idx];
        memset(conn, 0, sizeof(tcp_conn_t));
        memcpy(conn->remote_ip, dst_ip, 4);
        conn->local_port  = src_port;
        conn->remote_port = dst_port;
        extern uint32_t tcp_isn_counter;
        conn->snd_nxt = tcp_isn_counter;
        tcp_isn_counter += 64000;
        conn->snd_una = conn->snd_nxt;
        conn->rcv_wscale = 3;
        conn->snd_wnd = TCP_RX_BUF_SIZE;
        conn->state = TCP_SYN_SENT;
        conn->last_activity = idt_get_ticks();

        tcp_send_segment(conn, TCP_SYN, NULL, 0);

        async_ops[slot].type = NETOP_TCP_SYN_WAIT;
        async_ops[slot].deadline = idt_get_ticks() + 500;
        async_ops[slot].callback = cb;
        async_ops[slot].ctx = ctx;
        async_ops[slot].tcp.conn_idx = idx;
        async_ops[slot].tcp.phase = 1;
    } else {
        /* Need ARP first */
        arp_send_request(nexthop);

        async_ops[slot].type = NETOP_ARP_WAIT;
        async_ops[slot].deadline = idt_get_ticks() + 200;  /* 2s ARP */
        async_ops[slot].callback = cb;
        async_ops[slot].ctx = ctx;
        memcpy(async_ops[slot].tcp.dst_ip, dst_ip, 4);
        async_ops[slot].tcp.dst_port = dst_port;
        async_ops[slot].tcp.src_port = src_port;
        async_ops[slot].tcp.conn_idx = -1;
        async_ops[slot].tcp.phase = 0;
    }

    return 0;
}

int net_async_pending(void)
{
    for (int i = 0; i < NET_MAX_ASYNC_OPS; i++)
        if (async_ops[i].type != NETOP_IDLE) return 1;
    return 0;
}

/* ── Register UDP Listener ───────────────────────────────────── */

void net_udp_listen(uint16_t port, udp_handler_t handler)
{
    /* Idempotent: re-registering the same (port, handler) — common
     * for net_dns_resolve which is called once per HTTP session — is
     * a no-op. Without this, MAX_UDP_LISTENERS fills after a handful
     * of resolves and subsequent calls silently drop without
     * registering. */
    for (int i = 0; i < udp_listener_count; i++) {
        if (udp_listeners[i].port == port &&
            udp_listeners[i].handler == handler) {
            return;
        }
    }
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
