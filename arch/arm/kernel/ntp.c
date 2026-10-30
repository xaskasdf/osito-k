/*
 * ntp.c -- the time of day, by SNTP (x86's ntp.c, ported)
 *
 * The phone has no clock the apps processor can read as UTC, so the time
 * is 0 ("unknown") until ntp_sync gets it over the network:
 * time.google.com, else pool.ntp.org, port 123, from a random source port
 * each try. The time then advances from the boot CPU's 100 Hz tick.
 * Certificate checks need it (x509.c); the net task syncs after DHCP.
 */

#include "../include/hal.h"
#include "net.h"

typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;    /* LI(2) | VN(3) | Mode(3) */
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec, ref_ts_frac;
    uint32_t orig_ts_sec, orig_ts_frac;
    uint32_t recv_ts_sec, recv_ts_frac;
    uint32_t tx_ts_sec, tx_ts_frac;     /* the server's transmit time */
} ntp_packet_t;

/* 1900 to 1970: 70 years and 17 leap days */
#define NTP_UNIX_OFFSET 2208988800U

static volatile int got_reply;
static uint32_t reply_utc;

static uint64_t sync_tick;              /* timer_get_tick_count() then */
static uint32_t sync_utc;
static volatile int synced;

static uint32_t be32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static void ntp_handler(const uint8_t *src_ip, uint16_t src_port,
                        const void *data, uint32_t len)
{
    (void)src_ip;
    (void)src_port;
    if (len < sizeof(ntp_packet_t))
        return;
    const ntp_packet_t *pkt = data;
    uint32_t sec = be32(pkt->tx_ts_sec);
    uint8_t mode = pkt->li_vn_mode & 7;
    if (mode != 4 || pkt->stratum == 0 || sec < NTP_UNIX_OFFSET)
        return;                         /* not a server's answer, or a kiss-o'-death */
    reply_utc = sec - NTP_UNIX_OFFSET;
    got_reply = 1;
}

/* The caller holds the network (net_hold) */
int ntp_sync(void)
{
    uint8_t ip[4];
    if (net_dns_resolve("time.google.com", ip) < 0 &&
        net_dns_resolve("pool.ntp.org", ip) < 0) {
        serial_puts("[NTP ] no server: DNS failed\n");
        return -1;
    }
    ntp_packet_t req;
    memset(&req, 0, sizeof(req));
    req.li_vn_mode = 0x23;              /* version 4, client */
    for (int attempt = 0; attempt < 3; attempt++) {
        uint16_t r;
        random_bytes(&r, sizeof(r));
        uint16_t port = (uint16_t)(49152 + r % 16384);  /* the ephemeral range */
        got_reply = 0;
        net_udp_listen(port, ntp_handler);
        uint64_t t0 = timer_ms();
        int sent = 0;
        while (!got_reply && timer_ms() - t0 < 2000) {
            if (!sent)
                sent = net_udp_send(ip, 123, port, &req, sizeof(req)) == 0;
            net_poll();
            task_yield();
        }
        net_udp_unlisten(port);
        if (got_reply) {
            sync_tick = timer_get_tick_count();
            sync_utc = reply_utc;
            synced = 1;
            serial_puts("[NTP ] synced: ");
            ntp_print(reply_utc);
            serial_puts(" UTC\n");
            return 0;
        }
    }
    serial_puts("[NTP ] no answer\n");
    return -1;
}

uint32_t ntp_get_utc(void)
{
    if (!synced)
        return 0;
    return sync_utc + (uint32_t)((timer_get_tick_count() - sync_tick) / 100);
}

int ntp_is_synced(void)
{
    return synced;
}

/* Unix seconds as "YYYY-MM-DD hh:mm:ss" (Howard Hinnant's civil_from_days) */
void ntp_print(uint32_t t)
{
    int64_t z = t / 86400 + 719468;
    int64_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    uint32_t y = (uint32_t)(yoe + era * 400) + (m <= 2);
    uint32_t s = t % 86400;
    uint32_t parts[6] = { y, m, d, s / 3600, s / 60 % 60, s % 60 };
    static const char sep[6] = { 0, '-', '-', ' ', ':', ':' };
    for (int i = 0; i < 6; i++) {
        if (i)
            serial_putc(sep[i]);
        if (parts[i] < 10)
            serial_putc('0');
        serial_putdec(parts[i]);
    }
}
