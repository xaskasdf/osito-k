/*
 * OsitoK x86-64 — mDNS/DNS-SD Responder
 *
 * Responds to multicast DNS queries for "osito-k.local".
 * Allows other devices on the LAN to find OsitoK by hostname.
 * Listens on UDP port 5353 (224.0.0.251).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

extern void     net_udp_listen(uint16_t port, void *handler);
extern int      net_udp_send(const uint8_t dst_ip[4], uint16_t dst_port,
                             uint16_t src_port, const void *data, uint32_t len);
extern void     net_get_mac(uint8_t mac_out[6]);

/* ── mDNS Constants ──────────────────────────────────────────── */

#define MDNS_PORT    5353
static const uint8_t mdns_mcast_ip[4] = {224, 0, 0, 251};

/* Our hostname: "osito-k" → "osito-k.local" */
static const char *mdns_hostname = "osito-k";

/* ── DNS name encoding ───────────────────────────────────────── */

/* Encode "osito-k.local" as DNS label sequence */
static int mdns_encode_name(uint8_t *buf)
{
    int p = 0;
    /* "osito-k" label */
    const char *h = mdns_hostname;
    int hlen = 0;
    while (h[hlen]) hlen++;
    buf[p++] = (uint8_t)hlen;
    for (int i = 0; i < hlen; i++) buf[p++] = (uint8_t)h[i];
    /* "local" label */
    buf[p++] = 5;
    buf[p++] = 'l'; buf[p++] = 'o'; buf[p++] = 'c';
    buf[p++] = 'a'; buf[p++] = 'l';
    /* Terminator */
    buf[p++] = 0;
    return p;
}

/* Case-insensitive DNS label comparison */
static bool mdns_name_match(const uint8_t *query, int qlen,
                            const uint8_t *expected, int elen)
{
    if (qlen != elen) return false;
    for (int i = 0; i < qlen; i++) {
        uint8_t a = query[i], b = expected[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

/* ── mDNS State ──────────────────────────────────────────────── */

static bool mdns_active;
static uint8_t our_ip_cache[4];

/* ── UDP handler ─────────────────────────────────────────────── */

static void mdns_handler(const uint8_t *src_ip, uint16_t src_port,
                         const uint8_t *data, uint32_t len)
{
    (void)src_ip; (void)src_port;
    if (len < 12) return;  /* DNS header minimum */

    /* Parse DNS header */
    uint16_t flags = ((uint16_t)data[2] << 8) | data[3];
    if (flags & 0x8000) return;  /* Response, not query — ignore */

    uint16_t qdcount = ((uint16_t)data[4] << 8) | data[5];
    if (qdcount == 0) return;

    /* Parse first question */
    const uint8_t *qp = data + 12;
    const uint8_t *end = data + len;

    /* Build expected name for comparison */
    uint8_t expected[32];
    int elen = mdns_encode_name(expected);

    /* Read question name labels */
    uint8_t qname[64];
    int qnlen = 0;
    while (qp < end && *qp != 0 && qnlen < 60) {
        uint8_t llen = *qp++;
        if (qp + llen > end) return;
        qname[qnlen++] = llen;
        for (int i = 0; i < llen; i++)
            qname[qnlen++] = *qp++;
    }
    if (qp >= end) return;
    qname[qnlen++] = 0;  /* terminator */
    qp++;  /* skip zero terminator */
    if (qp + 4 > end) return;

    uint16_t qtype  = ((uint16_t)qp[0] << 8) | qp[1];
    /* uint16_t qclass = ((uint16_t)qp[2] << 8) | qp[3]; */

    /* Check if query matches our name (A record, type 1) */
    if (qtype != 1) return;
    if (!mdns_name_match(qname, qnlen, expected, elen)) return;

    serial_puts("[mDNS] Query for osito-k.local → responding\n");

    /* Build mDNS response */
    uint8_t resp[128];
    int rp = 0;

    /* DNS header: ID=0 (mDNS), QR=1 (response), AA=1 */
    resp[rp++] = 0; resp[rp++] = 0;     /* ID */
    resp[rp++] = 0x84; resp[rp++] = 0;  /* Flags: QR=1, AA=1 */
    resp[rp++] = 0; resp[rp++] = 0;     /* QDCOUNT = 0 */
    resp[rp++] = 0; resp[rp++] = 1;     /* ANCOUNT = 1 */
    resp[rp++] = 0; resp[rp++] = 0;     /* NSCOUNT */
    resp[rp++] = 0; resp[rp++] = 0;     /* ARCOUNT */

    /* Answer: name + type A + class IN + TTL + RDATA (4 bytes IP) */
    int nlen = mdns_encode_name(resp + rp);
    rp += nlen;

    /* Type A (1) */
    resp[rp++] = 0; resp[rp++] = 1;
    /* Class IN (1) with cache-flush bit */
    resp[rp++] = 0x80; resp[rp++] = 1;
    /* TTL: 120 seconds */
    resp[rp++] = 0; resp[rp++] = 0; resp[rp++] = 0; resp[rp++] = 120;
    /* RDLENGTH: 4 */
    resp[rp++] = 0; resp[rp++] = 4;
    /* RDATA: our IPv4 address */
    memcpy(resp + rp, our_ip_cache, 4);
    rp += 4;

    /* Send response to mDNS multicast */
    net_udp_send(mdns_mcast_ip, MDNS_PORT, MDNS_PORT, resp, (uint32_t)rp);
}

/* ── Public API ──────────────────────────────────────────────── */

void mdns_init(const uint8_t our_ip[4])
{
    memcpy(our_ip_cache, our_ip, 4);
    net_udp_listen(MDNS_PORT, (void *)mdns_handler);
    mdns_active = true;

    serial_puts("[mDNS] Responder active: osito-k.local → ");
    serial_putdec(our_ip[0]); serial_puts(".");
    serial_putdec(our_ip[1]); serial_puts(".");
    serial_putdec(our_ip[2]); serial_puts(".");
    serial_putdec(our_ip[3]); serial_puts("\n");
}

bool mdns_is_active(void) { return mdns_active; }

/* Update IP (call after DHCP renewal) */
void mdns_update_ip(const uint8_t ip[4])
{
    memcpy(our_ip_cache, ip, 4);
}
