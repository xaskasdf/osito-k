/*
 * arch/x86/kernel/cluster_rendezvous.c — minimal HTTP rendezvous
 * server for nodes that can't share a multicast segment.  Three
 * routes: POST /register, POST /heartbeat, GET /peers.  Plaintext
 * HTTP/1.1, no TLS — peer authenticity is enforced by the HMAC-PSK
 * layer on the actual cluster RPC channel, not by the rendezvous.
 */
#include "../include/types.h"
#include "cluster.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern uint64_t idt_get_ticks(void);
extern int  kthread_create(const char *name, void (*fn)(void *), void *data);
extern void sched_yield(void);
extern int  net_tcp_listen(uint16_t port);
extern int  net_tcp_accept(int listener, uint32_t timeout_ticks);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t size, uint32_t to);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern void net_tcp_close(int conn);
extern int  net_tcp_get_peer_ip(int conn, uint8_t ip_out[4]);

#define RDV_MAX_PEERS    16
#define RDV_STALE_TICK   1500UL    /* 15s @ 100Hz */
#define RDV_REMOVE_TICK  3000UL    /* 30s @ 100Hz */
#define RDV_REQ_BUF      1024
#define RDV_RESP_BUF     2048
#define RDV_RECV_TIMEOUT 100       /* 1s */

typedef struct {
    bool     in_use;
    char     node_id[64];
    uint8_t  ip[4];
    uint16_t rpc_port;
    uint64_t last_seen_tick;
} rdv_peer_t;

static rdv_peer_t      g_rdv[RDV_MAX_PEERS];
static volatile int    g_running;
static int             g_kth = -1;
static uint16_t        g_port;
static uint32_t        g_register_count, g_heartbeat_count, g_peers_query_count;

/* ── string + JSON helpers (tiny, payload-shape-specific) ───── */

static uint32_t kstrlen(const char *s)
{ uint32_t n = 0; while (s && s[n]) n++; return n; }

static int kstrncmp(const char *a, const char *b, uint32_t n)
{ for (uint32_t i = 0; i < n; i++) { if (a[i] != b[i]) return a[i] - b[i]; if (a[i] == 0) return 0; } return 0; }

static uint32_t kappend(char *dst, uint32_t off, uint32_t cap,
                         const char *s, uint32_t n)
{
    if (off >= cap) return off;
    uint32_t room = cap - off - 1;
    uint32_t take = n < room ? n : room;
    for (uint32_t i = 0; i < take; i++) dst[off + i] = s[i];
    dst[off + take] = 0;
    return off + take;
}
static uint32_t appstr(char *d, uint32_t o, uint32_t c, const char *s)
{ return kappend(d, o, c, s, kstrlen(s)); }
static uint32_t appu32(char *d, uint32_t o, uint32_t c, uint32_t v)
{
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else { while (v) { tmp[n++] = '0' + (v % 10); v /= 10; } }
    char rev[12];
    for (int i = 0; i < n; i++) rev[i] = tmp[n - 1 - i];
    return kappend(d, o, c, rev, (uint32_t)n);
}
static uint32_t appip(char *d, uint32_t o, uint32_t c, const uint8_t ip[4])
{
    for (int i = 0; i < 4; i++) {
        o = appu32(d, o, c, ip[i]);
        if (i < 3) o = appstr(d, o, c, ".");
    }
    return o;
}

/* Parse `"key":"value"` from a JSON-ish body; copy value to out. */
static uint32_t json_str(const char *b, uint32_t bl, const char *key,
                          char *out, uint32_t cap)
{
    uint32_t klen = kstrlen(key);
    if (bl < klen + 5) return 0;
    for (uint32_t i = 0; i + klen + 4 <= bl; i++) {
        if (b[i] != '"') continue;
        if (kstrncmp(b + i + 1, key, klen) != 0) continue;
        if (b[i + 1 + klen] != '"') continue;
        uint32_t j = i + klen + 2;
        while (j < bl && (b[j] == ' ' || b[j] == ':' || b[j] == '\t')) j++;
        if (j >= bl || b[j] != '"') return 0;
        j++;
        uint32_t copied = 0;
        while (j < bl && b[j] != '"' && copied + 1 < cap) out[copied++] = b[j++];
        out[copied] = 0;
        return copied;
    }
    return 0;
}

/* ── Peer table ops ─────────────────────────────────────────── */

static int rdv_find_or_alloc(const char *node_id, const uint8_t ip[4])
{
    int free_slot = -1;
    for (int i = 0; i < RDV_MAX_PEERS; i++) {
        if (g_rdv[i].in_use && kstrncmp(g_rdv[i].node_id, node_id, 64) == 0)
            return i;
        if (!g_rdv[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -1;
    rdv_peer_t *p = &g_rdv[free_slot];
    p->in_use = true;
    uint32_t nl = kstrlen(node_id);
    if (nl >= 64) nl = 63;
    for (uint32_t i = 0; i < nl; i++) p->node_id[i] = node_id[i];
    p->node_id[nl] = 0;
    for (int i = 0; i < 4; i++) p->ip[i] = ip[i];
    return free_slot;
}

static void rdv_evict_stale(uint64_t now)
{
    for (int i = 0; i < RDV_MAX_PEERS; i++) {
        if (!g_rdv[i].in_use) continue;
        if (now - g_rdv[i].last_seen_tick >= RDV_REMOVE_TICK)
            g_rdv[i].in_use = false;
    }
}

/* ── HTTP request handling ──────────────────────────────────── */

static int send_response(int conn, int status,
                          const char *body, uint32_t body_len)
{
    char hdr[256];
    uint32_t off = 0;
    off = appstr(hdr, off, sizeof hdr, "HTTP/1.1 ");
    off = appu32(hdr, off, sizeof hdr, (uint32_t)status);
    off = appstr(hdr, off, sizeof hdr, status == 200 ? " OK\r\n" : " ERROR\r\n");
    off = appstr(hdr, off, sizeof hdr,
                 "Content-Type: application/json\r\nContent-Length: ");
    off = appu32(hdr, off, sizeof hdr, body_len);
    off = appstr(hdr, off, sizeof hdr, "\r\nConnection: close\r\n\r\n");
    if (net_tcp_send(conn, hdr, off) < 0) return -1;
    if (body_len && net_tcp_send(conn, body, body_len) < 0) return -1;
    return 0;
}

static void render_peers_json(char *out, uint32_t cap, uint32_t *out_len)
{
    uint32_t off = 0;
    off = appstr(out, off, cap, "{\"peers\":[");
    bool first = true;
    for (int i = 0; i < RDV_MAX_PEERS; i++) {
        if (!g_rdv[i].in_use) continue;
        if (!first) off = appstr(out, off, cap, ",");
        first = false;
        off = appstr(out, off, cap, "{\"node_id\":\"");
        off = appstr(out, off, cap, g_rdv[i].node_id);
        off = appstr(out, off, cap, "\",\"addr\":\"");
        off = appip(out, off, cap, g_rdv[i].ip);
        off = appstr(out, off, cap, ":");
        off = appu32(out, off, cap, g_rdv[i].rpc_port);
        off = appstr(out, off, cap, "\"}");
    }
    off = appstr(out, off, cap, "]}");
    *out_len = off;
}

static void handle_request(int conn, const uint8_t client_ip[4])
{
    static char req[RDV_REQ_BUF];
    static char body_buf[256];
    static char resp_body[RDV_RESP_BUF];

    int n = net_tcp_recv_timeout(conn, req, sizeof req - 1, RDV_RECV_TIMEOUT);
    if (n <= 0) return;
    req[n] = 0;

    /* Split request-line + headers from body at \r\n\r\n. */
    const char *hdr_end = 0;
    for (int i = 0; i + 3 < n; i++) {
        if (req[i] == '\r' && req[i+1] == '\n' &&
            req[i+2] == '\r' && req[i+3] == '\n') {
            hdr_end = req + i + 4;
            break;
        }
    }
    uint32_t hl = (uint32_t)(hdr_end ? hdr_end - req : (uint32_t)n);
    uint32_t bl = hdr_end ? (uint32_t)(n - hl) : 0;

    /* Route. */
    if (kstrncmp(req, "POST /register ", 15) == 0) {
        g_register_count++;
        char node_id[64] = "";
        char addr[64] = "";
        if (hdr_end) {
            (void)json_str(hdr_end, bl, "node_id", node_id, sizeof node_id);
            (void)json_str(hdr_end, bl, "addr",    addr,    sizeof addr);
        }
        if (!node_id[0]) {
            send_response(conn, 400, "{\"error\":\"missing node_id\"}", 27);
            return;
        }
        /* Parse port out of "ip:port" addr if present, default 50052. */
        uint16_t port = 50052;
        for (uint32_t i = 0; addr[i]; i++) {
            if (addr[i] == ':') {
                uint32_t p = 0;
                for (uint32_t j = i + 1; addr[j] >= '0' && addr[j] <= '9'; j++)
                    p = p * 10 + (uint32_t)(addr[j] - '0');
                if (p > 0 && p < 65536) port = (uint16_t)p;
                break;
            }
        }
        int slot = rdv_find_or_alloc(node_id, client_ip);
        if (slot < 0) {
            send_response(conn, 503, "{\"error\":\"table full\"}", 22);
            return;
        }
        g_rdv[slot].rpc_port = port;
        g_rdv[slot].last_seen_tick = idt_get_ticks();
        uint32_t rl;
        render_peers_json(resp_body, sizeof resp_body, &rl);
        send_response(conn, 200, resp_body, rl);
        return;
    }

    if (kstrncmp(req, "POST /heartbeat ", 16) == 0) {
        g_heartbeat_count++;
        char node_id[64] = "";
        if (hdr_end) (void)json_str(hdr_end, bl, "node_id", node_id, sizeof node_id);
        for (int i = 0; i < RDV_MAX_PEERS; i++) {
            if (g_rdv[i].in_use && kstrncmp(g_rdv[i].node_id, node_id, 64) == 0) {
                g_rdv[i].last_seen_tick = idt_get_ticks();
                send_response(conn, 200, "{\"ok\":true}", 11);
                return;
            }
        }
        send_response(conn, 404, "{\"error\":\"unknown node_id\"}", 27);
        return;
    }

    if (kstrncmp(req, "GET /peers", 10) == 0) {
        g_peers_query_count++;
        uint32_t rl;
        render_peers_json(resp_body, sizeof resp_body, &rl);
        send_response(conn, 200, resp_body, rl);
        return;
    }

    /* Use the body_buf slot just to silence unused-var; small fixed body. */
    (void)body_buf;
    send_response(conn, 404, "{\"error\":\"not found\"}", 21);
}

static void rdv_thread(void *unused)
{
    (void)unused;
    int listener = net_tcp_listen(g_port);
    if (listener < 0) {
        serial_puts("[CLUSTER-RDV] listen failed\n");
        g_running = 0;
        return;
    }
    serial_puts("[CLUSTER-RDV] HTTP rendezvous up on port ");
    serial_putdec((uint64_t)g_port); serial_puts("\n");

    uint64_t last_evict = 0;
    while (g_running) {
        int conn = net_tcp_accept(listener, 10);    /* 100ms poll */
        if (conn >= 0) {
            uint8_t ip[4] = {0};
            (void)net_tcp_get_peer_ip(conn, ip);
            handle_request(conn, ip);
            net_tcp_close(conn);
        }
        uint64_t now = idt_get_ticks();
        if (now - last_evict >= 200) {
            rdv_evict_stale(now);
            last_evict = now;
        }
        if (conn < 0) sched_yield();
    }
    serial_puts("[CLUSTER-RDV] server stopped\n");
}

int cluster_rendezvous_start(uint16_t port)
{
    if (g_running) return 0;
    g_port = port;
    g_running = 1;
    g_kth = kthread_create("cluster-rdv", rdv_thread, NULL);
    if (g_kth < 0) { g_running = 0; return -1; }
    return 0;
}
