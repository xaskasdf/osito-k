/*
 * OsitoK x86-64 — Netfilter (Packet Filtering)
 *
 * Basic packet filtering: accept/drop by IP, port, protocol.
 * Minimal iptables-compatible rule table.
 * Called from net_poll() before packet dispatch.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── Filter Rule ─────────────────────────────────────────────── */

#define NF_MAX_RULES  32
#define NF_ACCEPT     0
#define NF_DROP       1

/* Match criteria (0 = wildcard / match all) */
typedef struct {
    bool     active;
    uint8_t  action;         /* NF_ACCEPT or NF_DROP */
    uint8_t  protocol;       /* 6=TCP, 17=UDP, 0=any */
    uint8_t  direction;      /* 0=input, 1=output, 2=both */
    uint32_t src_ip;         /* 0 = any source */
    uint32_t src_mask;       /* Netmask for source */
    uint32_t dst_ip;         /* 0 = any destination */
    uint32_t dst_mask;
    uint16_t src_port;       /* 0 = any port */
    uint16_t dst_port;       /* 0 = any port */
    uint64_t match_count;    /* Statistics */
} nf_rule_t;

static nf_rule_t rules[NF_MAX_RULES];
static int rule_count;
static bool nf_enabled;

/* Default policy: ACCEPT (no rules = allow everything) */
static uint8_t default_policy = NF_ACCEPT;

/* ── Public API ──────────────────────────────────────────────── */

void nf_init(void)
{
    memset(rules, 0, sizeof(rules));
    rule_count = 0;
    nf_enabled = true;
    serial_puts("[NF] Netfilter initialized (default: ACCEPT)\n");
}

/* Add a filter rule. Returns rule index or -1. */
int nf_add_rule(uint8_t action, uint8_t protocol, uint8_t direction,
                uint32_t src_ip, uint32_t src_mask,
                uint32_t dst_ip, uint32_t dst_mask,
                uint16_t src_port, uint16_t dst_port)
{
    if (rule_count >= NF_MAX_RULES) return -1;

    nf_rule_t *r = &rules[rule_count];
    r->active = true;
    r->action = action;
    r->protocol = protocol;
    r->direction = direction;
    r->src_ip = src_ip;
    r->src_mask = src_mask;
    r->dst_ip = dst_ip;
    r->dst_mask = dst_mask;
    r->src_port = src_port;
    r->dst_port = dst_port;
    r->match_count = 0;

    serial_puts("[NF] Rule ");
    serial_putdec((uint64_t)rule_count);
    serial_puts(": ");
    serial_puts(action == NF_DROP ? "DROP" : "ACCEPT");
    serial_puts(" proto=");
    serial_putdec(protocol);
    serial_puts("\n");

    return rule_count++;
}

/* Delete a rule by index */
int nf_delete_rule(int idx)
{
    if (idx < 0 || idx >= rule_count) return -1;
    rules[idx].active = false;
    return 0;
}

/* Set default policy */
void nf_set_policy(uint8_t policy)
{
    default_policy = policy;
}

/* ── Packet Check (called from network stack) ────────────────── */

/* Check a packet against the rule table.
 * Returns NF_ACCEPT or NF_DROP.
 *
 * @direction: 0=incoming, 1=outgoing
 * @proto: IP protocol (6=TCP, 17=UDP, 1=ICMP)
 * @src_ip, dst_ip: network byte order
 * @src_port, dst_port: host byte order */
uint8_t nf_check_packet(uint8_t direction, uint8_t proto,
                        uint32_t src_ip, uint32_t dst_ip,
                        uint16_t src_port, uint16_t dst_port)
{
    if (!nf_enabled) return NF_ACCEPT;

    for (int i = 0; i < rule_count; i++) {
        nf_rule_t *r = &rules[i];
        if (!r->active) continue;

        /* Direction check */
        if (r->direction != 2 && r->direction != direction) continue;

        /* Protocol check */
        if (r->protocol != 0 && r->protocol != proto) continue;

        /* Source IP check (with mask) */
        if (r->src_ip != 0 && (src_ip & r->src_mask) != (r->src_ip & r->src_mask))
            continue;

        /* Destination IP check */
        if (r->dst_ip != 0 && (dst_ip & r->dst_mask) != (r->dst_ip & r->dst_mask))
            continue;

        /* Port checks */
        if (r->src_port != 0 && r->src_port != src_port) continue;
        if (r->dst_port != 0 && r->dst_port != dst_port) continue;

        /* Match! */
        r->match_count++;
        return r->action;
    }

    return default_policy;
}

/* ── Statistics ──────────────────────────────────────────────── */

void nf_list_rules(void)
{
    serial_puts("[NF] Rules (");
    serial_putdec((uint64_t)rule_count);
    serial_puts(" active, default=");
    serial_puts(default_policy == NF_DROP ? "DROP" : "ACCEPT");
    serial_puts("):\n");

    for (int i = 0; i < rule_count; i++) {
        if (!rules[i].active) continue;
        serial_puts("  ");
        serial_putdec((uint64_t)i);
        serial_puts(": ");
        serial_puts(rules[i].action == NF_DROP ? "DROP " : "ACCEPT ");
        serial_puts("proto=");
        serial_putdec(rules[i].protocol);
        if (rules[i].dst_port) {
            serial_puts(" dport=");
            serial_putdec(rules[i].dst_port);
        }
        serial_puts(" matches=");
        serial_putdec(rules[i].match_count);
        serial_puts("\n");
    }
}

void nf_enable(void) { nf_enabled = true; }
void nf_disable(void) { nf_enabled = false; }
