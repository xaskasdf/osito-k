/*
 * OsitoK x86-64 — DHCP Client (RFC 2131)
 *
 * Automatic IP configuration via DISCOVER → OFFER → REQUEST → ACK.
 * Uses UDP ports 67 (server) / 68 (client).
 */

#include "../include/types.h"
#include "net.h"

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
extern int      net_udp_send_broadcast(uint16_t dst_port, uint16_t src_port,
                                       const void *data, uint32_t len);
extern void     net_poll(void);
extern uint64_t idt_get_ticks(void);
extern void     boot_diag_mark(const char *reason);
extern void     boot_diag_heartbeat(const char *reason);

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
static uint64_t dhcp_epoch, dhcp_lease_epoch;
static uint8_t dhcp_mac[6], dhcp_phase, dhcp_state_lock, dhcp_operation_lock;
static bool dhcp_renewing;
static unsigned dhcp_rx_logs;
static uint64_t dhcp_retry_seed;
static struct {
    uint8_t ip[4], server[4], mask[4], gateway[4], dns[4];
    uint32_t seconds;
} dhcp_bound;

static void dhcp_save_bound(void)
{
    memcpy(dhcp_bound.ip, offered_ip, 4);
    memcpy(dhcp_bound.server, server_ip, 4);
    memcpy(dhcp_bound.mask, offered_mask, 4);
    memcpy(dhcp_bound.gateway, offered_gw, 4);
    memcpy(dhcp_bound.dns, offered_dns, 4);
    dhcp_bound.seconds = offered_lease;
}

static void dhcp_restore_bound(void)
{
    memcpy(offered_ip, dhcp_bound.ip, 4);
    memcpy(server_ip, dhcp_bound.server, 4);
    memcpy(offered_mask, dhcp_bound.mask, 4);
    memcpy(offered_gw, dhcp_bound.gateway, 4);
    memcpy(offered_dns, dhcp_bound.dns, 4);
    offered_lease = dhcp_bound.seconds;
}

static void dhcp_lock(void)
{
    while (__atomic_test_and_set(&dhcp_state_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void dhcp_unlock(void) { __atomic_clear(&dhcp_state_lock, __ATOMIC_RELEASE); }

static void dhcp_wait_phase(uint8_t phase)
{
    dhcp_lock();
    dhcp_phase = phase;
    if (phase) __atomic_store_n(&dhcp_got_reply, false, __ATOMIC_RELEASE);
    dhcp_unlock();
}

static bool dhcp_link_valid(void)
{
    return dhcp_epoch == net_link_epoch() && net_link_ready();
}

typedef struct {
    uint8_t type, overload, mask[4], gateway[4], dns[4], server[4];
    uint32_t lease, seen;
} dhcp_options_t;

/* ── Helpers ─────────────────────────────────────────────────── */

static inline uint32_t be32(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

static uint64_t dhcp_retry_ticks(unsigned attempt)
{
    /* RFC 2131 4.1: 4s, 8s, ... with +/-1s jitter, bounded at 64s.
     * This MAC/tick-seeded PRNG is only for scheduling, not cryptography. */
    dhcp_retry_seed = dhcp_retry_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    uint64_t ticks = (400ULL << (attempt < 4 ? attempt : 4)) - 100 +
                     ((dhcp_retry_seed >> 32) % 201);
    return ticks < 6400 ? ticks : 6400;
}

static void print_ip(const uint8_t ip[4])
{
    serial_putdec(ip[0]); serial_puts(".");
    serial_putdec(ip[1]); serial_puts(".");
    serial_putdec(ip[2]); serial_puts(".");
    serial_putdec(ip[3]);
}

/* Parse DHCP options from received packet */
static bool parse_options(dhcp_options_t *out, const uint8_t *opts, uint32_t len, bool primary)
{
    uint32_t i = 0;
    while (i < len) {
        uint8_t opt = opts[i++];
        if (opt == 0) continue;       /* Pad */
        if (opt == 255) return true;
        if (i >= len) return false;
        uint8_t olen = opts[i++];
        if (olen > len - i) return false;
        unsigned bit = opt == 1 ? 1 : opt == 51 ? 2 : opt == 53 ? 4 : opt == 54 ? 8 : opt == 52 ? 16 : 0;
        if (out->seen & bit) return false;
        out->seen |= bit;

        switch (opt) {
        case 1:  /* Subnet Mask */
            if (olen != 4) return false;
            memcpy(out->mask, &opts[i], 4);
            break;
        case 3:  /* Router */
            if (!olen || (olen & 3)) return false;
            if (!(out->seen & 32)) memcpy(out->gateway, &opts[i], 4);
            out->seen |= 32;
            break;
        case 6:  /* DNS Server */
            if (!olen || (olen & 3)) return false;
            if (!(out->seen & 64)) memcpy(out->dns, &opts[i], 4);
            out->seen |= 64;
            break;
        case 51: /* Lease Time */
            if (olen != 4) return false;
            {
                out->lease = ((uint32_t)opts[i] << 24) |
                                ((uint32_t)opts[i+1] << 16) |
                                ((uint32_t)opts[i+2] << 8) |
                                opts[i+3];
            }
            break;
        case 53: /* DHCP Message Type */
            if (olen != 1) return false;
            out->type = opts[i];
            break;
        case 54: /* Server Identifier */
            if (olen != 4) return false;
            memcpy(out->server, &opts[i], 4);
            break;
        case 52:
            if (!primary || olen != 1 || !opts[i] || opts[i] > 3) return false;
            out->overload = opts[i];
            break;
        }
        i += olen;
    }
    return false;
}

static bool dhcp_unicast(const uint8_t ip[4])
{
    return ip[0] && ip[0] != 127 && ip[0] < 224;
}

static void dhcp_rx_log(const uint8_t *src, uint32_t len, const char *result)
{
    unsigned logged = __atomic_load_n(&dhcp_rx_logs, __ATOMIC_RELAXED);
    do {
        if (logged >= 16) return;
    } while (!__atomic_compare_exchange_n(&dhcp_rx_logs, &logged, logged + 1,
                                          false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    serial_puts("[DHCP] RX src="); print_ip(src);
    serial_puts(" bytes="); serial_putdec(len);
    serial_puts(" result="); serial_puts(result); serial_puts("\n");
}

/* ── UDP handler (port 68) ───────────────────────────────────── */

static void dhcp_handler(const uint8_t *src_ip, uint16_t src_port,
                         const void *data, uint32_t len)
{
    /* A relay's source need not equal option 54. */
    if (src_port != 67) return;
    if (!data || len < 240 || len > 1472) { dhcp_rx_log(src_ip, len, "length"); return; }
    if (__atomic_test_and_set(&dhcp_state_lock, __ATOMIC_ACQUIRE)) {
        dhcp_rx_log(src_ip, len, "busy"); return;
    }

    const dhcp_packet_t *pkt = (const dhcp_packet_t *)data;
    const char *result = "inactive";
    if (!dhcp_phase || dhcp_got_reply || !dhcp_link_valid()) goto out;
    result = "identity/xid/cookie";
    if (pkt->op != DHCP_OP_REPLY || pkt->htype != 1 || pkt->hlen != 6 ||
        memcmp(pkt->chaddr, dhcp_mac, 6) || pkt->xid != dhcp_xid || pkt->cookie != be32(DHCP_MAGIC)) goto out;
    dhcp_options_t options = {0};
    result = "options";
    if (!parse_options(&options, (const uint8_t *)data + 240, len - 240, true)) goto out;
    if ((options.overload & 1) && !parse_options(&options, pkt->file, sizeof(pkt->file), false)) goto out;
    if ((options.overload & 2) && !parse_options(&options, pkt->sname, sizeof(pkt->sname), false)) goto out;
    result = "server/message-type";
    if ((options.seen & 12) != 12 || !dhcp_unicast(options.server)) goto out;
    if (dhcp_phase == DHCP_OFFER) {
        if (options.type != DHCP_OFFER) goto out;
    } else {
        if ((options.type != DHCP_ACK && options.type != DHCP_NAK) ||
            memcmp(options.server, server_ip, 4)) goto out;
        if (options.type == DHCP_NAK) {
            dhcp_msg_type = DHCP_NAK;
            result = "NAK";
            __atomic_store_n(&dhcp_got_reply, true, __ATOMIC_RELEASE);
            goto out;
        }
        result = "address";
        if (memcmp(pkt->yiaddr, offered_ip, 4)) goto out;
    }
    result = "lease/address";
    if (!(options.seen & 2) || !options.lease || !dhcp_unicast(pkt->yiaddr)) goto out;
    if (!(options.seen & 1) && dhcp_phase != DHCP_OFFER) memcpy(options.mask, offered_mask, 4);
    uint32_t mask = ((uint32_t)options.mask[0] << 24) | ((uint32_t)options.mask[1] << 16) |
                    ((uint32_t)options.mask[2] << 8) | options.mask[3];
    uint32_t inverse = ~mask;
    result = "netmask";
    if (!mask || (inverse & (inverse + 1))) goto out;
    memcpy(offered_ip, pkt->yiaddr, 4);
    memcpy(server_ip, options.server, 4);
    memcpy(offered_mask, options.mask, 4);
    memcpy(offered_gw, options.gateway, 4);
    memcpy(offered_dns, options.dns, 4);
    offered_lease = options.lease;
    dhcp_msg_type = options.type;
    __atomic_store_n(&dhcp_got_reply, true, __ATOMIC_RELEASE);
    result = options.type == DHCP_OFFER ? "OFFER accepted" : "ACK accepted";
out:
    dhcp_unlock();
    dhcp_rx_log(src_ip, len, result);
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
    if (dhcp_renewing) {
        memcpy(pkt.ciaddr, offered_ip, 4);
        pkt.flags = 0;
    }

    /* Build options */
    int p = 0;
    /* Option 53: DHCP Message Type */
    pkt.options[p++] = 53; pkt.options[p++] = 1;
    pkt.options[p++] = msg_type;

    if (msg_type == DHCP_REQUEST && req_ip && !dhcp_renewing) {
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

    if (!dhcp_link_valid()) return -1;
    int rc = dhcp_renewing ? net_udp_send(server_ip, 67, 68, &pkt, sizeof(pkt)) :
                            net_udp_send_broadcast(67, 68, &pkt, sizeof(pkt));
    serial_puts("[DHCP] TX "); serial_puts(msg_type == DHCP_DISCOVER ? "DISCOVER" : "REQUEST");
    serial_puts(" xid="); serial_puthex(dhcp_xid, 8);
    serial_puts(rc < 0 ? " send-failed\n" : " queued\n");
    return rc;
}

/* ── Public API ──────────────────────────────────────────────── */

int dhcp_discover(void)
{
    if (__atomic_test_and_set(&dhcp_operation_lock, __ATOMIC_ACQUIRE)) return -1;
    __atomic_store_n(&dhcp_rx_logs, 0, __ATOMIC_RELAXED);
    net_poll();
    dhcp_epoch = net_link_epoch();
    dhcp_renewing = false;
    net_get_mac(dhcp_mac);
    if (!net_link_ready() || (dhcp_mac[0] & 1) ||
        !(dhcp_mac[0] | dhcp_mac[1] | dhcp_mac[2] | dhcp_mac[3] | dhcp_mac[4] | dhcp_mac[5])) goto failed;
    serial_puts("[DHCP] Starting discovery...\n");
    fb_puts(" DHCP: discovering...\n");
    boot_diag_mark("dhcp-start");

    /* Generate transaction ID from tick counter */
    dhcp_xid = (uint32_t)(idt_get_ticks() * 0x5DEECE66DULL + 0xB);
    dhcp_retry_seed = ((uint64_t)dhcp_mac[0] << 40) | ((uint64_t)dhcp_mac[1] << 32) |
                      ((uint64_t)dhcp_mac[2] << 24) | ((uint64_t)dhcp_mac[3] << 16) |
                      ((uint64_t)dhcp_mac[4] << 8) | dhcp_mac[5];
    dhcp_retry_seed ^= (uint64_t)dhcp_xid * 0x9E3779B97F4A7C15ULL;

    /* Register UDP listener on port 68 */
    if (net_udp_listen(68, dhcp_handler) != 0) goto failed;

    /* Send DISCOVER — retry up to 4 times */
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t timeout = dhcp_retry_ticks((unsigned)attempt);
        dhcp_wait_phase(DHCP_OFFER);
        dhcp_send(DHCP_DISCOVER, NULL);

        /* Keep polling while waiting; a link change cancels the transaction. */
        uint64_t start = idt_get_ticks();
        while (!__atomic_load_n(&dhcp_got_reply, __ATOMIC_ACQUIRE) &&
               dhcp_link_valid() && (idt_get_ticks() - start) < timeout) {
            extern void net_poll_wait(void);
            boot_diag_heartbeat("dhcp-wait-offer");
            net_poll_wait();
        }
        dhcp_wait_phase(0);
        if (!dhcp_link_valid()) goto failed;

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
        boot_diag_mark("dhcp-retry");
    }

    if (!dhcp_got_reply || dhcp_msg_type != DHCP_OFFER) {
        serial_puts("[DHCP] No OFFER received, giving up\n");
        fb_puts(" DHCP: failed\n");
        boot_diag_mark("dhcp-failed");
        goto failed;
    }

    /* Send REQUEST for the offered IP */
    uint64_t start;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        uint64_t timeout = dhcp_retry_ticks(attempt);
        dhcp_wait_phase(DHCP_ACK);
        dhcp_send(DHCP_REQUEST, offered_ip);
        start = idt_get_ticks();
        while (!__atomic_load_n(&dhcp_got_reply, __ATOMIC_ACQUIRE) &&
               dhcp_link_valid() && (idt_get_ticks() - start) < timeout) {
            extern void net_poll_wait(void);
            boot_diag_heartbeat("dhcp-wait-ack");
            net_poll_wait();
        }
        dhcp_wait_phase(0);
        if (dhcp_got_reply || !dhcp_link_valid()) break;
    }

    if (!dhcp_got_reply || dhcp_msg_type != DHCP_ACK) {
        serial_puts("[DHCP] No ACK received\n");
        fb_puts(" DHCP: no ACK\n");
        boot_diag_mark("dhcp-failed");
        goto failed;
    }

    /* Apply configuration */
    start = idt_get_ticks();
    while (net_apply_ipv4(dhcp_epoch, offered_ip, offered_mask, offered_gw, offered_dns)) {
        if (!dhcp_link_valid() || idt_get_ticks() - start >= 100) goto failed;
        extern void net_poll_wait(void);
        net_poll_wait();
    }

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
    dhcp_lease_epoch = dhcp_epoch;
    dhcp_lease_ticks = (uint64_t)offered_lease * 100;  /* convert to 100Hz ticks */
    dhcp_save_bound();

    boot_diag_mark("dhcp-done");
    __atomic_clear(&dhcp_operation_lock, __ATOMIC_RELEASE);
    return 0;
failed:
    dhcp_wait_phase(0);
    if (dhcp_lease_epoch != net_link_epoch() || !net_link_ready()) dhcp_lease_ticks = 0;
    dhcp_restore_bound();
    __atomic_clear(&dhcp_operation_lock, __ATOMIC_RELEASE);
    return -1;
}

/* Check if lease needs renewal. Call periodically from main loop.
 * Renews at T1 = 50% of lease (RFC 2131 recommendation). */
void dhcp_check_renewal(void)
{
    if (__atomic_test_and_set(&dhcp_operation_lock, __ATOMIC_ACQUIRE)) return;
    net_poll();
    if (dhcp_lease_epoch != net_link_epoch() || !net_link_ready()) dhcp_lease_ticks = 0;
    if (!dhcp_lease_ticks) goto out;
    uint64_t elapsed = idt_get_ticks() - dhcp_lease_start;
    if (elapsed >= dhcp_lease_ticks) {
        const uint8_t zero[4] = {0};
        if (!net_apply_ipv4(dhcp_lease_epoch, zero, zero, zero, zero)) dhcp_lease_ticks = 0;
        goto out;
    }
    uint64_t t1 = dhcp_lease_ticks / 2;  /* Renew at 50% of lease */

    if (elapsed < t1) goto out;
    dhcp_epoch = dhcp_lease_epoch;
    dhcp_renewing = true;
    __atomic_store_n(&dhcp_rx_logs, 0, __ATOMIC_RELAXED);
    dhcp_xid = (uint32_t)(idt_get_ticks() * 0x5DEECE66DULL + 0xB);

    serial_puts("[DHCP] Lease renewal...\n");
    dhcp_wait_phase(DHCP_ACK);
    dhcp_send(DHCP_REQUEST, offered_ip);

    /* Brief wait for ACK (1s) */
    uint64_t start = idt_get_ticks();
    while (!__atomic_load_n(&dhcp_got_reply, __ATOMIC_ACQUIRE) &&
           dhcp_link_valid() && (idt_get_ticks() - start) < 100) {
        extern void net_poll_wait(void);
        net_poll_wait();
    }
    dhcp_wait_phase(0);

    if (dhcp_link_valid() && dhcp_got_reply && dhcp_msg_type == DHCP_ACK &&
        !net_apply_ipv4(dhcp_epoch, offered_ip, offered_mask, offered_gw, offered_dns)) {
        dhcp_lease_start = idt_get_ticks();
        dhcp_lease_ticks = (uint64_t)offered_lease * 100;
        dhcp_save_bound();
        serial_puts("[DHCP] Lease renewed (");
        serial_putdec(offered_lease);
        serial_puts("s)\n");
    } else {
        if (dhcp_link_valid() && dhcp_got_reply && dhcp_msg_type == DHCP_NAK) {
            const uint8_t zero[4] = {0};
            if (!net_apply_ipv4(dhcp_epoch, zero, zero, zero, zero)) dhcp_lease_ticks = 0;
        }
        dhcp_restore_bound();
        serial_puts("[DHCP] Renewal failed, will retry\n");
    }
out:
    dhcp_renewing = false;
    __atomic_clear(&dhcp_operation_lock, __ATOMIC_RELEASE);
}
