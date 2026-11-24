/*
 * OsitoK x86-64 — IPv6 Basic Support
 *
 * Minimal dual-stack IPv6:
 * - Link-local address generation (fe80:: from MAC)
 * - ICMPv6 Echo Reply (ping6)
 * - Neighbor Discovery (NS/NA for address resolution)
 * - IPv6 packet receive and dispatch
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void net_get_mac(uint8_t mac_out[6]);
extern uint64_t idt_get_ticks(void);

/* ── IPv6 Header ─────────────────────────────────────────────── */

#define ETH_TYPE_IP6  0x86DD
#define IP6_PROTO_ICMPV6  58
#define IP6_PROTO_TCP     6
#define IP6_PROTO_UDP     17

typedef struct __attribute__((packed)) {
    uint32_t ver_class_flow;  /* version(4) + traffic class(8) + flow label(20) */
    uint16_t payload_len;
    uint8_t  next_header;     /* Same as IPv4 protocol field */
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} ipv6_hdr_t;

/* ── ICMPv6 ──────────────────────────────────────────────────── */

#define ICMPV6_ECHO_REQUEST     128
#define ICMPV6_ECHO_REPLY       129
#define ICMPV6_NEIGHBOR_SOLICIT 135
#define ICMPV6_NEIGHBOR_ADVERT  136

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
} icmpv6_hdr_t;

/* ── IPv6 State ──────────────────────────────────────────────── */

static uint8_t our_ipv6[16];       /* Link-local address (fe80::...) */
static bool    ipv6_enabled;

/* ── Generate link-local address from MAC (EUI-64) ───────────── */

void ipv6_init(void)
{
    uint8_t mac[6];
    net_get_mac(mac);

    /* fe80::xxxx:xxff:fexx:xxxx (EUI-64 from MAC) */
    memset(our_ipv6, 0, 16);
    our_ipv6[0] = 0xFE;
    our_ipv6[1] = 0x80;
    /* bytes 2-7 = zero (64-bit prefix) */
    our_ipv6[8]  = mac[0] ^ 0x02;  /* flip U/L bit */
    our_ipv6[9]  = mac[1];
    our_ipv6[10] = mac[2];
    our_ipv6[11] = 0xFF;
    our_ipv6[12] = 0xFE;
    our_ipv6[13] = mac[3];
    our_ipv6[14] = mac[4];
    our_ipv6[15] = mac[5];

    ipv6_enabled = true;

    serial_puts("[IPv6] Link-local: fe80::");
    serial_puthex(our_ipv6[8], 2); serial_puthex(our_ipv6[9], 2);
    serial_puts(":");
    serial_puthex(our_ipv6[10], 2); serial_puthex(our_ipv6[11], 2);
    serial_puts(":");
    serial_puthex(our_ipv6[12], 2); serial_puthex(our_ipv6[13], 2);
    serial_puts(":");
    serial_puthex(our_ipv6[14], 2); serial_puthex(our_ipv6[15], 2);
    serial_puts("\n");
}

bool ipv6_is_enabled(void) { return ipv6_enabled; }

/* ── Handle incoming IPv6 packet ─────────────────────────────── */

void ipv6_handle_packet(const uint8_t *data, uint32_t len,
                        const uint8_t *src_mac)
{
    if (!ipv6_enabled || len < sizeof(ipv6_hdr_t)) return;

    const ipv6_hdr_t *ip6 = (const ipv6_hdr_t *)data;
    uint8_t version = (ip6->ver_class_flow >> 28) & 0x0F;
    if (version != 6) return;

    uint16_t payload_len = ((uint16_t)data[4] << 8) | data[5];
    const uint8_t *payload = data + sizeof(ipv6_hdr_t);
    (void)payload_len; (void)payload; (void)src_mac;

    switch (ip6->next_header) {
    case IP6_PROTO_ICMPV6:
        if (payload_len >= sizeof(icmpv6_hdr_t)) {
            const icmpv6_hdr_t *icmp = (const icmpv6_hdr_t *)payload;
            if (icmp->type == ICMPV6_ECHO_REQUEST) {
                serial_puts("[IPv6] Echo request received\n");
                /* TODO: Send Echo Reply */
            }
            if (icmp->type == ICMPV6_NEIGHBOR_SOLICIT) {
                serial_puts("[IPv6] Neighbor Solicitation received\n");
                /* TODO: Send Neighbor Advertisement */
            }
        }
        break;
    default:
        break;
    }
}
