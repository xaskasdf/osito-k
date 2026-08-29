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
extern int      net_udp_listen(uint16_t port, void *handler);
extern int      net_udp_unlisten(uint16_t port, void *handler);
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
static volatile bool ntp_synced;

/* Before NTP succeeds, calibrate the RTC once and advance it from the
 * monotonic APIC clock. Re-reading CMOS for every Win32 timestamp is both
 * expensive and only second-granular. */
static volatile uint32_t rtc_clock_state; /* 0=empty, 1=initializing, 2=ready */
static uint64_t rtc_boot_tick;
static uint32_t rtc_boot_utc;

/* ── Byte swap ───────────────────────────────────────────────── */

static inline uint32_t ntohl(uint32_t v)
{
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

typedef struct {
    uint8_t sec, min, hour, day, month, year, century, status_b;
} rtc_time_t;

static inline uint8_t rtc_cmos_read(uint8_t reg)
{
    uint8_t value;
    __asm__ volatile ("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"((uint16_t)0x71));
    return value;
}

static int rtc_read_raw(rtc_time_t *t)
{
    int spin = 0;
    while ((rtc_cmos_read(0x0A) & 0x80) && spin++ < 100000)
        __asm__ volatile ("pause");
    if (rtc_cmos_read(0x0A) & 0x80) return -1;

    t->sec      = rtc_cmos_read(0x00);
    t->min      = rtc_cmos_read(0x02);
    t->hour     = rtc_cmos_read(0x04);
    t->day      = rtc_cmos_read(0x07);
    t->month    = rtc_cmos_read(0x08);
    t->year     = rtc_cmos_read(0x09);
    t->century  = rtc_cmos_read(0x32);
    t->status_b = rtc_cmos_read(0x0B);
    return (rtc_cmos_read(0x0A) & 0x80) ? -1 : 0;
}

static int rtc_same(const rtc_time_t *a, const rtc_time_t *b)
{
    return a->sec == b->sec && a->min == b->min &&
           a->hour == b->hour && a->day == b->day &&
           a->month == b->month && a->year == b->year &&
           a->century == b->century && a->status_b == b->status_b;
}

static uint8_t rtc_bcd(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static uint32_t rtc_civil_to_unix(int year, int month, int day,
                                  int hour, int minute, int second)
{
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
                   + (unsigned)day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t seconds = (int64_t)(era * 146097 + (int)doe - 719468) * 86400
                    + hour * 3600 + minute * 60 + second;
    return (seconds > 0 && seconds <= 0xFFFFFFFFLL) ? (uint32_t)seconds : 0;
}

static uint32_t rtc_get_utc(void)
{
    rtc_time_t a, b;
    int stable = 0;
    for (int tries = 0; tries < 4; tries++) {
        if (rtc_read_raw(&a) == 0 && rtc_read_raw(&b) == 0 && rtc_same(&a, &b)) {
            stable = 1;
            break;
        }
    }
    if (!stable) return 0;

    int pm = (a.hour & 0x80) != 0;
    a.hour &= 0x7F;
    if (!(a.status_b & 0x04)) {
        a.sec = rtc_bcd(a.sec); a.min = rtc_bcd(a.min);
        a.hour = rtc_bcd(a.hour); a.day = rtc_bcd(a.day);
        a.month = rtc_bcd(a.month); a.year = rtc_bcd(a.year);
        a.century = rtc_bcd(a.century);
    }
    if (!(a.status_b & 0x02))
        a.hour = (uint8_t)((a.hour % 12) + (pm ? 12 : 0));

    int year = (a.century >= 19 && a.century <= 99)
             ? a.century * 100 + a.year : 2000 + a.year;
    static const uint8_t mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (a.month < 1 || a.month > 12 || a.day < 1 ||
        a.hour > 23 || a.min > 59 || a.sec > 59) return 0;
    int max_day = mdays[a.month - 1];
    if (a.month == 2 && (year % 4 == 0) &&
        (year % 100 != 0 || year % 400 == 0)) max_day++;
    if (a.day > max_day) return 0;
    return rtc_civil_to_unix(year, a.month, a.day, a.hour, a.min, a.sec);
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

    /* Build NTP request: Version 4, Mode 3 (client) */
    ntp_packet_t req;
    memset(&req, 0, sizeof(req));
    req.li_vn_mode = 0x23;  /* LI=0, VN=4, Mode=3 */

    /* Send query — retry up to 3 times.  Use a fresh ephemeral source
     * port per attempt: macOS pf (vmnet-shared) only installs reverse
     * NAT mappings for the BSD ephemeral range [49152..65535] by default
     * (and some configs gate on [32768..60999]).  Earlier code pinned
     * src_port=12321 and the reply silently disappeared in pf.        */
    extern void random_get_bytes(void *buf, uint32_t len);
    ntp_got_reply = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        ntp_got_reply = false;
        uint16_t r;
        random_get_bytes(&r, sizeof(r));
        /* Map into [49152, 65535] — IANA-registered ephemeral range,
         * tightest overlap of Linux/Darwin/BSD defaults.              */
        uint16_t src_port = 49152 + (uint16_t)(r % (65535 - 49152 + 1));
        if (net_udp_listen(src_port, (void *)ntp_handler) < 0) {
            serial_puts("[NTP] source port registration failed\n");
            continue;
        }
        serial_puts("[NTP] src_port=");
        serial_putdec(src_port);
        serial_puts("\n");
        if (net_udp_send(ntp_ip, 123, src_port, &req, sizeof(req)) < 0) {
            net_udp_unlisten(src_port, (void *)ntp_handler);
            net_poll();
            continue;
        }

        /* Wait for response (2s timeout = 200 ticks) */
        uint64_t start = idt_get_ticks();
        while (!ntp_got_reply && (idt_get_ticks() - start) < 200) {
            extern void net_poll_wait(void);
            net_poll_wait();
        }

        net_udp_unlisten(src_port, (void *)ntp_handler);

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
    __atomic_store_n(&ntp_synced, true, __ATOMIC_RELEASE);

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

/* UTC in Windows-compatible 100 ns units since the Unix epoch. NTP wins
 * after synchronization; otherwise CMOS seeds a monotonic wall clock. */
uint64_t ntp_get_utc_100ns(void)
{
    uint64_t now_tick = idt_get_ticks();
    if (__atomic_load_n(&ntp_synced, __ATOMIC_ACQUIRE)) {
        uint64_t elapsed_ticks = now_tick - ntp_boot_tick;
        return (uint64_t)ntp_boot_utc * 10000000ULL +
               elapsed_ticks * 100000ULL;
    }

    uint32_t state = __atomic_load_n(&rtc_clock_state, __ATOMIC_ACQUIRE);
    if (state != 2) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&rtc_clock_state, &expected, 1, false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            uint32_t utc = rtc_get_utc();
            if (!utc) {
                __atomic_store_n(&rtc_clock_state, 0, __ATOMIC_RELEASE);
                return 0;
            }
            rtc_boot_utc = utc;
            rtc_boot_tick = idt_get_ticks();
            __atomic_store_n(&rtc_clock_state, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&rtc_clock_state, __ATOMIC_ACQUIRE) == 1)
                __asm__ volatile ("pause");
            if (__atomic_load_n(&rtc_clock_state, __ATOMIC_ACQUIRE) != 2)
                return 0;
        }
    }

    now_tick = idt_get_ticks();
    return (uint64_t)rtc_boot_utc * 10000000ULL +
           (now_tick - rtc_boot_tick) * 100000ULL;
}

uint32_t ntp_get_utc(void)
{
    return (uint32_t)(ntp_get_utc_100ns() / 10000000ULL);
}

/* Is NTP synced? */
bool ntp_is_synced(void)
{
    return __atomic_load_n(&ntp_synced, __ATOMIC_ACQUIRE);
}
