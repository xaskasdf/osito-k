/*
 * OsitoK x86-64 — NTP Client (RFC 5905 simplified)
 *
 * One-shot time synchronization via UDP port 123.
 * Queries pool.ntp.org, extracts transmit timestamp,
 * computes UTC offset for kernel time.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern int      net_dns_resolve(const char *hostname, uint8_t ip_out[4]);
extern int      net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                             uint16_t src_port, const void *data, uint32_t len);
extern void     net_udp_listen(uint16_t port, void *handler);
extern void     net_poll(void);
extern uint64_t idt_get_ticks(void);

/* ── NTP packet (48 bytes) ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;   /* LI(2) | VN(3) | Mode(3) */
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t recv_ts_sec;
    uint32_t recv_ts_frac;
    uint32_t tx_ts_sec;    /* Server transmit timestamp (NTP epoch) */
    uint32_t tx_ts_frac;
} ntp_packet_t;

/* NTP epoch: January 1, 1900. Unix epoch: January 1, 1970.
 * Difference: 70 years + 17 leap days = 2208988800 seconds. */
#define NTP_UNIX_OFFSET 2208988800ULL

/* ── NTP state ───────────────────────────────────────────────── */

static volatile bool ntp_got_reply;
static uint32_t      ntp_unix_time;  /* UTC seconds since 1970 */

/* Kernel time offset: added to APIC ticks to get wall-clock time.
 * Set once by NTP, read by anyone needing real time. */
static uint64_t ntp_boot_tick;       /* idt_get_ticks() at sync moment */
static uint32_t ntp_boot_utc;        /* UTC seconds at sync moment */
static bool     ntp_synced;

/* ── Byte swap ───────────────────────────────────────────────── */

static inline uint32_t ntohl(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

/* ── UDP handler (receives NTP response) ─────────────────────── */

static void ntp_handler(const uint8_t *src_ip, uint16_t src_port,
                        const uint8_t *data, uint32_t len)
{
    (void)src_ip; (void)src_port;
    if (len < sizeof(ntp_packet_t)) return;

    const ntp_packet_t *pkt = (const ntp_packet_t *)data;

    /* Extract server transmit timestamp (big-endian NTP seconds) */
    uint32_t ntp_sec = ntohl(pkt->tx_ts_sec);
    if (ntp_sec < NTP_UNIX_OFFSET) return;  /* sanity: before 1970? */

    ntp_unix_time = ntp_sec - (uint32_t)NTP_UNIX_OFFSET;
    ntp_got_reply = true;
}

/* ── Public API ──────────────────────────────────────────────── */

int ntp_sync(void)
{
    /* Try time.google.com first (anycast, very reliable), fall back
     * to pool.ntp.org if the lookup fails.  pool.ntp.org rotates
     * through many servers; some are slow or unresponsive, and the
     * 3-attempt retry loop below isn't enough to ride that out. */
    serial_puts("[NTP] Resolving time.google.com...\n");
    uint8_t ntp_ip[4];
    int dns_ok = (net_dns_resolve("time.google.com", ntp_ip) == 0);
    if (!dns_ok) {
        serial_puts("[NTP] time.google.com DNS failed, trying pool.ntp.org\n");
        if (net_dns_resolve("pool.ntp.org", ntp_ip) < 0) {
            serial_puts("[NTP] DNS resolution failed\n");
            return -1;
        }
    }

    serial_puts("[NTP] Server: ");
    serial_putdec(ntp_ip[0]); serial_puts(".");
    serial_putdec(ntp_ip[1]); serial_puts(".");
    serial_putdec(ntp_ip[2]); serial_puts(".");
    serial_putdec(ntp_ip[3]); serial_puts("\n");

    /* Register UDP listener */
    ntp_got_reply = false;
    net_udp_listen(12321, (void *)ntp_handler);

    /* Build NTP request: Version 4, Mode 3 (client) */
    ntp_packet_t req;
    memset(&req, 0, sizeof(req));
    req.li_vn_mode = 0x23;  /* LI=0, VN=4, Mode=3 */

    /* Send query — retry up to 3 times */
    for (int attempt = 0; attempt < 3; attempt++) {
        ntp_got_reply = false;
        if (net_udp_send(ntp_ip, 123, 12321, &req, sizeof(req)) < 0) {
            net_poll();
            continue;
        }

        /* Wait for response (2s timeout = 200 ticks) */
        uint64_t start = idt_get_ticks();
        while (!ntp_got_reply && (idt_get_ticks() - start) < 200) {
            extern void net_poll_wait(void);
            net_poll_wait();
        }

        if (ntp_got_reply) break;
        serial_puts("[NTP] Retry...\n");
    }

    if (!ntp_got_reply) {
        serial_puts("[NTP] No response\n");
        return -1;
    }

    /* Store sync point */
    ntp_boot_tick = idt_get_ticks();
    ntp_boot_utc = ntp_unix_time;
    ntp_synced = true;

    /* Decode time for display */
    uint32_t t = ntp_unix_time;
    uint32_t secs = t % 60; t /= 60;
    uint32_t mins = t % 60; t /= 60;
    uint32_t hours = t % 24;

    serial_puts("[NTP] UTC time: ");
    serial_putdec(hours); serial_puts(":");
    if (mins < 10) serial_puts("0");
    serial_putdec(mins); serial_puts(":");
    if (secs < 10) serial_puts("0");
    serial_putdec(secs); serial_puts(" (unix=");
    serial_putdec(ntp_unix_time); serial_puts(")\n");

    fb_puts(" NTP: synced\n");
    return 0;
}

/* Get current UTC seconds (NTP-corrected if synced, else 0) */
uint32_t ntp_get_utc(void)
{
    if (!ntp_synced) return 0;
    uint64_t elapsed_ticks = idt_get_ticks() - ntp_boot_tick;
    return ntp_boot_utc + (uint32_t)(elapsed_ticks / 100);  /* 100Hz ticks */
}

/* Is NTP synced? */
bool ntp_is_synced(void)
{
    return ntp_synced;
}
