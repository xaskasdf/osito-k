/*
 * arch/x86/kernel/cluster.c — V1 cluster membership for osito-a LAN.
 *
 * Discovery still happens via inferconnect.c's UDP multicast (port
 * 19999).  This module layers liveness + epoch + HMAC-PSK auth on top
 * of the existing peer table, runs a 1 Hz tick kthread that drives
 * state transitions and per-peer RPC heartbeats, and answers the
 * three new RPC opcodes (HEARTBEAT / CLUSTER_INFO / CLUSTER_EPOCH).
 */
#include "../include/types.h"
#include "cluster.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern uint64_t idt_get_ticks(void);
extern int  kthread_create(const char *name, void (*fn)(void *), void *data);
extern void sched_yield(void);

extern int  net_tcp_connect(const uint8_t ip[4], uint16_t port, uint16_t src_port);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                                  uint32_t timeout_ticks);
extern void net_tcp_close(int conn);
extern int  http_plain_post(const char *url, const char *content_type,
                            const uint8_t *body, uint32_t body_len,
                            uint8_t *out, uint32_t out_cap);

extern void hmac_sha256(const void *key, uint32_t key_len,
                         const void *data, uint32_t data_len, uint8_t mac[32]);
extern void oict_get_shared_key(uint8_t out_key[32]);
extern void random_get_bytes(void *buf, uint32_t len);
extern bool vfs_find(const char *path, int mode, void *out_node);
extern int  vfs_read(void *node, uint64_t offset, void *buf, uint64_t len);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);

extern uint32_t inferconnect_max_peers(void);
extern int  inferconnect_peer_get(int idx, uint8_t ip[4], uint16_t *rpc_port,
                                  uint64_t *free_mem, char backend[32],
                                  uint64_t *last_seen_tick);
extern void inferconnect_peer_invalidate(int idx);

extern int  cluster_rendezvous_start(uint16_t port);

#define CLUSTER_MAX_PEERS  16
#define CLUSTER_TICK_HZ    100ULL
#define CLUSTER_STALE_TICK (15ULL * CLUSTER_TICK_HZ)
#define CLUSTER_DEAD_TICK  (30ULL * CLUSTER_TICK_HZ)
#define CLUSTER_RPC_PING_INTERVAL (10ULL * CLUSTER_TICK_HZ)
#define CLUSTER_RDV_POST_INTERVAL ( 5ULL * CLUSTER_TICK_HZ)
#define CLUSTER_HMAC_LOG_INTERVAL (60ULL * CLUSTER_TICK_HZ)
#define CLUSTER_RPC_PORT_DEFAULT  50052
#define CLUSTER_RDV_PORT_DEFAULT  19998
#define CLUSTER_RPC_RECV_TIMEOUT  200    /* 2s @ 100Hz */

static cluster_state_t      g_cluster;
static cluster_peer_meta_t  g_meta[CLUSTER_MAX_PEERS];
static uint8_t              g_psk[32];
static volatile int         g_running = 0;
static int                  g_tick_kth = -1;

/* ── small str helpers (kernel has no libc) ────────────────── */

static uint32_t kstrlen(const char *s)
{ uint32_t n = 0; while (s && s[n]) n++; return n; }

static int kstrncmp(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

static void kmemcpy(void *d, const void *s, uint32_t n)
{ uint8_t *D=d; const uint8_t *S=s; for (uint32_t i=0;i<n;i++) D[i]=S[i]; }

static void kmemset(void *d, int v, uint32_t n)
{ uint8_t *D=d; for (uint32_t i=0;i<n;i++) D[i]=(uint8_t)v; }

static int kmemeq_ct(const uint8_t *a, const uint8_t *b, uint32_t n)
{ uint8_t d = 0; for (uint32_t i=0;i<n;i++) d |= a[i]^b[i]; return d == 0; }

static int kis_zero(const uint8_t *p, uint32_t n)
{ for (uint32_t i=0;i<n;i++) if (p[i]) return 0; return 1; }

static void put_ip(const uint8_t ip[4])
{
    for (int i = 0; i < 4; i++) {
        serial_putdec((uint64_t)ip[i]);
        if (i < 3) serial_puts(".");
    }
}

static const char *state_name(peer_state_t s)
{
    switch (s) {
    case PEER_ALIVE:   return "ALIVE";
    case PEER_STALE:   return "STALE";
    case PEER_DEAD:    return "DEAD";
    default:           return "UNKNOWN";
    }
}

/* ── JSON helpers (only what /cluster.json + rendezvous need) ─ */

/* Find `"key": "..."` and copy the inner string into out.  Returns
 * bytes copied, or 0 if not found.  No escape handling — payloads
 * are kernel-owned and don't include quotes. */
static uint32_t json_extract_str(const char *buf, uint32_t len,
                                  const char *key, char *out, uint32_t cap)
{
    uint32_t klen = kstrlen(key);
    for (uint32_t i = 0; i + klen + 4 < len; i++) {
        if (buf[i] != '"') continue;
        if (kstrncmp(buf + i + 1, key, klen) != 0) continue;
        if (buf[i + 1 + klen] != '"') continue;
        uint32_t j = i + 1 + klen + 1;
        while (j < len && (buf[j] == ' ' || buf[j] == ':' || buf[j] == '\t')) j++;
        if (j >= len || buf[j] != '"') return 0;
        j++;
        uint32_t copied = 0;
        while (j < len && buf[j] != '"' && copied + 1 < cap) {
            out[copied++] = buf[j++];
        }
        out[copied] = 0;
        return copied;
    }
    return 0;
}

/* ── Config + key bootstrap ─────────────────────────────────── */

static void load_psk(void)
{
    kmemset(g_psk, 0, 32);
    oict_get_shared_key(g_psk);
    g_cluster.psk_loaded = !kis_zero(g_psk, 32);
    serial_puts(g_cluster.psk_loaded
        ? "[CLUSTER] PSK loaded (32 B)\n"
        : "[CLUSTER] no PSK — HEARTBEAT verify will fail closed\n");
}

static void load_config(void)
{
    /* vfs_find takes (path, mode, void*) — our header types it as
     * void to keep vfs.h out of this TU. */
    static char node_buf[64];   /* vfs_node_t is ≤ 32B; pad generously */
    static char body[1024];
    if (!vfs_find("cluster.json", 0, node_buf)) return;
    /* vfs_node_t.size lives at offset 16 (fs_version u32 + ino u32 +
     * data ptr u64 + size u64).  Cast carefully. */
    typedef struct { int v; uint32_t ino; void *data; uint64_t size; } node_t;
    node_t *n = (node_t *)node_buf;
    uint64_t want = n->size < sizeof body ? n->size : sizeof body;
    if (vfs_read(node_buf, 0, body, want) <= 0) return;
    char tmp[160];
    if (json_extract_str(body, (uint32_t)want, "rendezvous_url",
                         tmp, sizeof tmp) > 0) {
        kmemcpy(g_cluster.rendezvous_url, tmp,
                kstrlen(tmp) < sizeof g_cluster.rendezvous_url - 1
                ? kstrlen(tmp) + 1 : sizeof g_cluster.rendezvous_url - 1);
        serial_puts("[CLUSTER] rendezvous_url = ");
        serial_puts(g_cluster.rendezvous_url);
        serial_puts("\n");
    }
    if (json_extract_str(body, (uint32_t)want, "rendezvous_role",
                         tmp, sizeof tmp) > 0) {
        if (kstrncmp(tmp, "server", 6) == 0) {
            g_cluster.rendezvous_is_server = true;
            serial_puts("[CLUSTER] rendezvous_role = server\n");
        }
    }
}

/* ── Public ops ─────────────────────────────────────────────── */

void cluster_init(void)
{
    kmemset(&g_cluster, 0, sizeof g_cluster);
    kmemset(g_meta, 0, sizeof g_meta);
    g_cluster.last_transition_tick = idt_get_ticks();
    load_psk();
    load_config();
    serial_puts("[CLUSTER] init: max_peers=");
    serial_putdec((uint64_t)CLUSTER_MAX_PEERS);
    serial_puts(" stale=15s dead=30s\n");
}

const cluster_state_t *cluster_state(void) { return &g_cluster; }

uint32_t cluster_count_state(peer_state_t st)
{
    uint32_t n = 0;
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++)
        if (g_meta[i].in_use && g_meta[i].state == st) n++;
    return n;
}

void cluster_set_rendezvous(const char *url)
{
    if (!url) { g_cluster.rendezvous_url[0] = 0; return; }
    uint32_t n = kstrlen(url);
    if (n >= sizeof g_cluster.rendezvous_url) n = sizeof g_cluster.rendezvous_url - 1;
    kmemcpy(g_cluster.rendezvous_url, url, n);
    g_cluster.rendezvous_url[n] = 0;
    serial_puts("[CLUSTER] rendezvous_url set\n");
}

void cluster_set_debug(bool on) { g_cluster.debug_verbose = on; }

/* ── Eviction tick ──────────────────────────────────────────── */

/* Recompute rope arcs over the ALIVE set, sorted by IP for
 * determinism.  Each peer gets an equal arc in [0, FULL).
 * Called on every epoch transition. */
static void recompute_rope_arcs(void)
{
    /* Build a sorted index list of ALIVE peers (small N, insertion). */
    int order[CLUSTER_MAX_PEERS];
    int n = 0;
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use || g_meta[i].state != PEER_ALIVE) {
            g_meta[i].rope_arc_lo_q16 = 0;
            g_meta[i].rope_arc_hi_q16 = 0;
            continue;
        }
        int j = n;
        while (j > 0) {
            /* Compare IP bytes lexicographically. */
            uint8_t *a = g_meta[order[j - 1]].ip;
            uint8_t *b = g_meta[i].ip;
            int cmp = 0;
            for (int k = 0; k < 4; k++) {
                if (a[k] != b[k]) { cmp = (int)a[k] - (int)b[k]; break; }
            }
            if (cmp <= 0) break;
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
        n++;
    }
    if (n == 0) return;
    uint32_t slice = CLUSTER_ROPE_FULL_Q16 / (uint32_t)n;
    for (int k = 0; k < n; k++) {
        cluster_peer_meta_t *m = &g_meta[order[k]];
        m->rope_arc_lo_q16 = slice * (uint32_t)k;
        m->rope_arc_hi_q16 = (k + 1 == n) ? CLUSTER_ROPE_FULL_Q16
                                          : slice * (uint32_t)(k + 1);
    }
}

static void bump_epoch(int slot, peer_state_t from, peer_state_t to)
{
    g_cluster.epoch++;
    g_cluster.last_transition_tick = idt_get_ticks();
    serial_puts("[CLUSTER] peer ");
    put_ip(g_meta[slot].ip);
    serial_puts(" ");
    serial_puts(state_name(from));
    serial_puts(" → ");
    serial_puts(state_name(to));
    serial_puts(" (epoch=");
    serial_putdec(g_cluster.epoch);
    serial_puts(")\n");
    /* Any ALIVE-set change → re-slice the rope.  Cheap: O(N²) on a
     * 16-element max array. */
    recompute_rope_arcs();
}

int cluster_nearest_alive_peer(uint32_t angle_q16)
{
    if (angle_q16 >= CLUSTER_ROPE_FULL_Q16)
        angle_q16 %= CLUSTER_ROPE_FULL_Q16;
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use || g_meta[i].state != PEER_ALIVE) continue;
        if (g_meta[i].rope_arc_lo_q16 == g_meta[i].rope_arc_hi_q16) continue;
        if (angle_q16 >= g_meta[i].rope_arc_lo_q16 &&
            angle_q16 <  g_meta[i].rope_arc_hi_q16) return i;
    }
    return -1;
}

int cluster_render_ring(char *buf, uint32_t cap)
{
    if (!buf || cap < 32) return -1;
    uint32_t off = 0;
    int rendered = 0;
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use || g_meta[i].state != PEER_ALIVE) continue;
        if (off + 64 >= cap) break;
        cluster_peer_meta_t *m = &g_meta[i];
        char tmp[16]; int t;
        for (int o = 0; o < 4; o++) {
            t = 0; uint32_t v = m->ip[o];
            if (v == 0) tmp[t++] = '0';
            else { while (v) { tmp[t++] = '0' + (v % 10); v /= 10; } }
            while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
            if (o < 3 && off + 1 < cap) buf[off++] = '.';
        }
        const char *kw = "  arc=[";
        for (int k = 0; kw[k] && off + 1 < cap; k++) buf[off++] = kw[k];
        t = 0; uint32_t lv = m->rope_arc_lo_q16;
        if (lv == 0) tmp[t++] = '0';
        else { while (lv) { tmp[t++] = '0' + (lv % 10); lv /= 10; } }
        while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        if (off + 2 < cap) { buf[off++] = ','; buf[off++] = ' '; }
        t = 0; uint32_t hv = m->rope_arc_hi_q16;
        if (hv == 0) tmp[t++] = '0';
        else { while (hv) { tmp[t++] = '0' + (hv % 10); hv /= 10; } }
        while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        if (off + 1 < cap) buf[off++] = ')';
        if (off + 1 < cap) buf[off++] = '\n';
        rendered++;
    }
    if (rendered == 0) {
        const char *m = "(no ALIVE peers)\n";
        for (uint32_t i = 0; m[i] && off + 1 < cap; i++) buf[off++] = m[i];
    }
    if (off < cap) buf[off] = 0;
    return (int)off;
}

static void cluster_scan_once(uint64_t now)
{
    uint32_t max = inferconnect_max_peers();
    if (max > CLUSTER_MAX_PEERS) max = CLUSTER_MAX_PEERS;
    for (uint32_t i = 0; i < max; i++) {
        uint8_t  ip[4] = {0};
        uint16_t port = 0;
        uint64_t free_mem = 0, last_seen = 0;
        char     backend[32] = {0};
        int rc = inferconnect_peer_get((int)i, ip, &port,
                                       &free_mem, backend, &last_seen);
        cluster_peer_meta_t *m = &g_meta[i];
        if (rc < 0) {
            /* Slot invalid in ic_peers — clear our meta too. */
            if (m->in_use) {
                peer_state_t was = m->state;
                m->in_use = false;
                m->state  = PEER_UNKNOWN;
                if (was == PEER_ALIVE || was == PEER_STALE)
                    bump_epoch((int)i, was, PEER_UNKNOWN);
            }
            continue;
        }
        /* New slot — initialise. */
        if (!m->in_use) {
            m->in_use = true;
            kmemcpy(m->ip, ip, 4);
            m->rpc_port = port;
            m->state = PEER_ALIVE;
            m->last_seen_tick = last_seen;
            bump_epoch((int)i, PEER_UNKNOWN, PEER_ALIVE);
            continue;
        }
        /* Refresh from authoritative ic_peers data. */
        kmemcpy(m->ip, ip, 4);
        m->rpc_port = port;
        uint64_t newest = last_seen;
        if (m->last_rpc_heartbeat_tick > newest)
            newest = m->last_rpc_heartbeat_tick;
        m->last_seen_tick = newest;

        uint64_t delta = now - newest;
        peer_state_t was = m->state;
        peer_state_t to  = was;
        if      (delta >= CLUSTER_DEAD_TICK)  to = PEER_DEAD;
        else if (delta >= CLUSTER_STALE_TICK) to = PEER_STALE;
        else                                  to = PEER_ALIVE;
        if (to != was) {
            m->state = to;
            bump_epoch((int)i, was, to);
            if (to == PEER_DEAD) {
                /* Reclaim the underlying slot too. */
                inferconnect_peer_invalidate((int)i);
                m->in_use = false;
                m->state  = PEER_UNKNOWN;
            }
        }
    }
}

/* ── RPC: outbound HEARTBEAT ───────────────────────────────── */

static int send_authed_op(const uint8_t ip[4], uint16_t port,
                           uint8_t cmd, uint8_t *resp, uint32_t resp_cap)
{
    int conn = net_tcp_connect(ip, port, 49152 + ((uint16_t)idt_get_ticks() & 0x3FFF));
    if (conn < 0) return -1;
    uint64_t sender_epoch = g_cluster.epoch;
    /* Build body = [u8 reserved | u64 sender_epoch | u32 mac_len=32 | mac] */
    uint8_t body[1 + 8 + 4 + 32];
    body[0] = 0;
    kmemcpy(body + 1, &sender_epoch, 8);
    uint32_t mlen = 32;
    kmemcpy(body + 9, &mlen, 4);
    /* MAC covers cmd || sender_epoch */
    uint8_t mac_in[1 + 8];
    mac_in[0] = cmd;
    kmemcpy(mac_in + 1, &sender_epoch, 8);
    hmac_sha256(g_psk, 32, mac_in, sizeof mac_in, body + 13);

    uint64_t in_size = sizeof body;
    if (net_tcp_send(conn, &cmd, 1) < 0)        goto fail;
    if (net_tcp_send(conn, &in_size, 8) < 0)    goto fail;
    if (net_tcp_send(conn, body, sizeof body) < 0) goto fail;

    uint64_t out_size = 0;
    if (net_tcp_recv_timeout(conn, &out_size, 8, CLUSTER_RPC_RECV_TIMEOUT) != 8) goto fail;
    if (out_size > resp_cap) { net_tcp_close(conn); return -1; }
    if (out_size && net_tcp_recv_timeout(conn, resp, (uint32_t)out_size,
                                          CLUSTER_RPC_RECV_TIMEOUT) != (int)out_size)
        goto fail;
    net_tcp_close(conn);
    return (int)out_size;
fail:
    net_tcp_close(conn);
    return -1;
}

int cluster_send_heartbeat(int peer_idx)
{
    if (peer_idx < 0 || peer_idx >= CLUSTER_MAX_PEERS) return -1;
    cluster_peer_meta_t *m = &g_meta[peer_idx];
    if (!m->in_use || m->rpc_port == 0) return -1;
    if (!g_cluster.psk_loaded) return -1;
    uint8_t resp[64];
    m->rpc_heartbeat_sent++;
    int n = send_authed_op(m->ip, m->rpc_port,
                            RPC_CMD_OSITOA_CLUSTER_HEARTBEAT,
                            resp, sizeof resp);
    if (n < 1) return -1;
    /* Response = [u8 status]: 0 = ok, 1 = auth fail */
    if (resp[0] != 0) { m->rpc_heartbeat_authfail++; return -1; }
    m->rpc_heartbeat_ok++;
    m->last_rpc_heartbeat_tick = idt_get_ticks();
    if (g_cluster.debug_verbose) {
        serial_puts("[CLUSTER] heartbeat → ");
        put_ip(m->ip);
        serial_puts(" ok\n");
    }
    return 0;
}

int cluster_query_epoch(int peer_idx, uint64_t *epoch_out)
{
    if (peer_idx < 0 || peer_idx >= CLUSTER_MAX_PEERS) return -1;
    cluster_peer_meta_t *m = &g_meta[peer_idx];
    if (!m->in_use || m->rpc_port == 0) return -1;
    /* CLUSTER_EPOCH is unauthenticated — cheap polling. */
    int conn = net_tcp_connect(m->ip, m->rpc_port,
                                49152 + ((uint16_t)idt_get_ticks() & 0x3FFF));
    if (conn < 0) return -1;
    uint8_t  cmd = RPC_CMD_OSITOA_CLUSTER_EPOCH;
    uint64_t in_size = 0;
    if (net_tcp_send(conn, &cmd, 1) < 0)     goto fail;
    if (net_tcp_send(conn, &in_size, 8) < 0) goto fail;
    uint64_t out_size = 0;
    if (net_tcp_recv_timeout(conn, &out_size, 8, CLUSTER_RPC_RECV_TIMEOUT) != 8) goto fail;
    if (out_size != 8) goto fail;
    if (net_tcp_recv_timeout(conn, epoch_out, 8, CLUSTER_RPC_RECV_TIMEOUT) != 8) goto fail;
    net_tcp_close(conn);
    return 0;
fail:
    net_tcp_close(conn);
    return -1;
}

/* ── RPC: inbound opcode handlers (called by inferconnect_rpc.c) ─ */

int cluster_handle_heartbeat(int conn, uint64_t in_size,
                              const uint8_t sender_ip[4])
{
    extern int  ic_recv_exact(int conn, void *buf, uint32_t n);
    if (in_size != 1 + 8 + 4 + 32) return -1;
    uint8_t  body[1 + 8 + 4 + 32];
    if (ic_recv_exact(conn, body, (uint32_t)in_size) < 0) return -1;
    uint64_t sender_epoch;
    uint32_t mac_len;
    kmemcpy(&sender_epoch, body + 1, 8);
    kmemcpy(&mac_len, body + 9, 4);
    if (mac_len != 32) return -1;

    if (!g_cluster.psk_loaded) {
        /* Reject without PSK — fail closed. */
        g_cluster.hmac_rejects_total++;
        g_cluster.hmac_rejects_since_log++;
        uint64_t out_size = 1; uint8_t status = 1;
        (void)net_tcp_send(conn, &out_size, 8);
        (void)net_tcp_send(conn, &status, 1);
        return -1;
    }

    /* Recompute MAC over (cmd || sender_epoch) and CT-compare. */
    uint8_t mac_in[1 + 8];
    mac_in[0] = RPC_CMD_OSITOA_CLUSTER_HEARTBEAT;
    kmemcpy(mac_in + 1, &sender_epoch, 8);
    uint8_t want[32];
    hmac_sha256(g_psk, 32, mac_in, sizeof mac_in, want);
    if (!kmemeq_ct(want, body + 13, 32)) {
        g_cluster.hmac_rejects_total++;
        g_cluster.hmac_rejects_since_log++;
        uint64_t out_size = 1; uint8_t status = 1;
        (void)net_tcp_send(conn, &out_size, 8);
        (void)net_tcp_send(conn, &status, 1);
        return -1;
    }

    /* Mark the sender's slot as freshly-heartbeated.  Find it by IP. */
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use) continue;
        if (g_meta[i].ip[0] == sender_ip[0] &&
            g_meta[i].ip[1] == sender_ip[1] &&
            g_meta[i].ip[2] == sender_ip[2] &&
            g_meta[i].ip[3] == sender_ip[3]) {
            g_meta[i].last_rpc_heartbeat_tick = idt_get_ticks();
            break;
        }
    }

    uint64_t out_size = 1; uint8_t status = 0;
    if (net_tcp_send(conn, &out_size, 8) < 0) return -1;
    if (net_tcp_send(conn, &status, 1)   < 0) return -1;
    return 0;
}

int cluster_handle_info_req(int conn, uint64_t in_size)
{
    if (in_size != 0) return -1;
    /* Body = [u64 epoch | u8 n_alive | repeated (u8 ip[4] | u16 port)] */
    uint8_t out[8 + 1 + CLUSTER_MAX_PEERS * 6];
    uint64_t epoch = g_cluster.epoch;
    kmemcpy(out, &epoch, 8);
    uint8_t n = 0;
    uint32_t off = 9;
    for (int i = 0; i < CLUSTER_MAX_PEERS && n < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use || g_meta[i].state != PEER_ALIVE) continue;
        kmemcpy(out + off, g_meta[i].ip, 4);  off += 4;
        kmemcpy(out + off, &g_meta[i].rpc_port, 2); off += 2;
        n++;
    }
    out[8] = n;
    uint64_t total = off;
    if (net_tcp_send(conn, &total, 8) < 0) return -1;
    if (net_tcp_send(conn, out, (uint32_t)total) < 0) return -1;
    return 0;
}

int cluster_handle_epoch_req(int conn, uint64_t in_size)
{
    if (in_size != 0) return -1;
    uint64_t out_size = 8;
    uint64_t epoch = g_cluster.epoch;
    if (net_tcp_send(conn, &out_size, 8) < 0) return -1;
    if (net_tcp_send(conn, &epoch, 8) < 0)    return -1;
    return 0;
}

/* ── Rendezvous client (best-effort POST /heartbeat) ───────── */

static void rdv_post_heartbeat(void)
{
    if (g_cluster.rendezvous_url[0] == 0) return;
    static const char ct[] = "application/json";
    /* Body = {"node_id":"osito-a-XXXX"}  — sender ip is enough id for now */
    uint8_t body[64];
    uint32_t off = 0;
    static const char prefix[] = "{\"node_id\":\"osito-a-";
    for (uint32_t i = 0; i < sizeof prefix - 1; i++) body[off++] = prefix[i];
    uint64_t t = idt_get_ticks();
    for (int shift = 12; shift >= 0; shift -= 4)
        body[off++] = "0123456789abcdef"[(t >> shift) & 0xF];
    body[off++] = '"'; body[off++] = '}';
    uint8_t resp[256];
    uint32_t url[1]; (void)url;  /* http_plain_post uses the literal URL */
    int rdv_url_len = (int)kstrlen(g_cluster.rendezvous_url);
    /* http_plain_post handles the connect+send+read internally. */
    (void)http_plain_post(g_cluster.rendezvous_url, ct,
                          body, off, resp, sizeof resp);
    (void)rdv_url_len;
}

/* ── Tick kthread ───────────────────────────────────────────── */

static void cluster_tick_thread(void *unused)
{
    (void)unused;
    uint64_t last_scan = 0;
    uint64_t last_rpc_ping = 0;
    uint64_t last_rdv_post = 0;
    uint64_t last_hmac_log = 0;
    serial_puts("[CLUSTER] tick kthread up\n");
    while (g_running) {
        uint64_t now = idt_get_ticks();

        if (now - last_scan >= CLUSTER_TICK_HZ) {
            cluster_scan_once(now);
            last_scan = now;
        }
        if (now - last_rpc_ping >= CLUSTER_RPC_PING_INTERVAL) {
            for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
                if (g_meta[i].in_use && g_meta[i].state == PEER_ALIVE)
                    (void)cluster_send_heartbeat(i);
            }
            last_rpc_ping = now;
        }
        if (g_cluster.rendezvous_url[0] &&
            now - last_rdv_post >= CLUSTER_RDV_POST_INTERVAL) {
            rdv_post_heartbeat();
            last_rdv_post = now;
        }
        if (now - last_hmac_log >= CLUSTER_HMAC_LOG_INTERVAL) {
            if (g_cluster.hmac_rejects_since_log > 0) {
                serial_puts("[CLUSTER] hmac rejects last minute: ");
                serial_putdec((uint64_t)g_cluster.hmac_rejects_since_log);
                serial_puts(" (total ");
                serial_putdec((uint64_t)g_cluster.hmac_rejects_total);
                serial_puts(")\n");
                g_cluster.hmac_rejects_since_log = 0;
            }
            last_hmac_log = now;
        }
        sched_yield();
    }
    serial_puts("[CLUSTER] tick kthread stopped\n");
}

int cluster_start(void)
{
    if (g_running) return 0;
    g_running = 1;
    g_tick_kth = kthread_create("cluster-tick", cluster_tick_thread, NULL);
    if (g_tick_kth < 0) { g_running = 0; return -1; }
    if (g_cluster.rendezvous_is_server)
        (void)cluster_rendezvous_start(CLUSTER_RDV_PORT_DEFAULT);
    return 0;
}

/* ── Operator surface ────────────────────────────────────────── */

void cluster_dump_status(void)
{
    serial_puts("cluster:\n");
    serial_puts("  epoch                = ");
    serial_putdec(g_cluster.epoch); serial_puts("\n");
    serial_puts("  alive / stale / dead = ");
    serial_putdec((uint64_t)cluster_count_state(PEER_ALIVE)); serial_puts(" / ");
    serial_putdec((uint64_t)cluster_count_state(PEER_STALE)); serial_puts(" / ");
    serial_putdec((uint64_t)cluster_count_state(PEER_DEAD));  serial_puts("\n");
    serial_puts("  psk_loaded           = ");
    serial_puts(g_cluster.psk_loaded ? "yes\n" : "no\n");
    serial_puts("  rendezvous           = ");
    serial_puts(g_cluster.rendezvous_url[0] ? g_cluster.rendezvous_url : "(none)");
    serial_puts(g_cluster.rendezvous_is_server ? " [server]\n" : "\n");
    serial_puts("  hmac_rejects_total   = ");
    serial_putdec((uint64_t)g_cluster.hmac_rejects_total); serial_puts("\n");
}

int cluster_render_peers(char *buf, uint32_t cap)
{
    if (!buf || cap < 4) return -1;
    uint32_t off = 0;
    uint32_t alive = 0;
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use) continue;
        if (off + 64 >= cap) break;
        cluster_peer_meta_t *m = &g_meta[i];
        char tmp[16];
        int t;
        for (int o = 0; o < 4; o++) {
            t = 0; uint32_t v = m->ip[o];
            if (v == 0) tmp[t++] = '0';
            else { while (v) { tmp[t++] = '0' + (v % 10); v /= 10; } }
            while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
            if (o < 3 && off + 1 < cap) buf[off++] = '.';
        }
        if (off + 1 < cap) buf[off++] = ':';
        t = 0; uint32_t pv = m->rpc_port;
        if (pv == 0) tmp[t++] = '0';
        else { while (pv) { tmp[t++] = '0' + (pv % 10); pv /= 10; } }
        while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        if (off + 1 < cap) buf[off++] = ' ';
        const char *sn = state_name(m->state);
        for (uint32_t k = 0; sn[k] && off + 1 < cap; k++) buf[off++] = sn[k];
        if (off + 6 < cap) {
            const char *ext = " hb=";
            for (uint32_t k = 0; ext[k] && off + 1 < cap; k++) buf[off++] = ext[k];
            t = 0; uint32_t hv = m->rpc_heartbeat_ok;
            if (hv == 0) tmp[t++] = '0';
            else { while (hv) { tmp[t++] = '0' + (hv % 10); hv /= 10; } }
            while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        }
        if (off + 1 < cap) buf[off++] = '\n';
        alive++;
    }
    if (alive == 0) {
        const char *m = "(no peers)\n";
        for (uint32_t i = 0; m[i] && off + 1 < cap; i++) buf[off++] = m[i];
    }
    if (off < cap) buf[off] = 0;
    return (int)off;
}

void cluster_dump_stats(void)
{
    uint32_t hb_sent = 0, hb_ok = 0, hb_auth = 0;
    for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
        if (!g_meta[i].in_use) continue;
        hb_sent += g_meta[i].rpc_heartbeat_sent;
        hb_ok   += g_meta[i].rpc_heartbeat_ok;
        hb_auth += g_meta[i].rpc_heartbeat_authfail;
    }
    serial_puts("cluster stats:\n");
    serial_puts("  rpc heartbeats sent  = ");
    serial_putdec((uint64_t)hb_sent); serial_puts("\n");
    serial_puts("  rpc heartbeats ok    = ");
    serial_putdec((uint64_t)hb_ok); serial_puts("\n");
    serial_puts("  rpc heartbeats auth  = ");
    serial_putdec((uint64_t)hb_auth); serial_puts(" (fails on our send)\n");
    serial_puts("  inbound hmac rejects = ");
    serial_putdec((uint64_t)g_cluster.hmac_rejects_total); serial_puts("\n");
}

void cluster_force_reconfigure(void)
{
    cluster_scan_once(idt_get_ticks());
    serial_puts("[CLUSTER] reconfigure forced\n");
}

/* ── Cross-node delegation probe (test harness hook) ─────────── */

static void cluster_delegate_probe_thread(void *unused)
{
    (void)unused;
    extern int inferconnect_remote_agent_task(const uint8_t ip[4],
                                                uint16_t port,
                                                const char *prompt,
                                                char *out, uint32_t cap);

    /* Wait for the LOCAL agent to finish initializing before we look
     * for peers. ALIVE peer transition already implies the peer's
     * IC-RPC server is listening, but its agent may still be coming
     * up — we add a settle window below. */
    extern bool agent_is_initialized(void);
    serial_puts("[CLUSTER-PROBE] waiting for local agent_init + ALIVE peer (≤300s)...\n");
    /* 300s: agent_init takes ~100s on a fresh boot (model + tokenizer
     * load + osfs2 RL-record reload), so a 120s window raced the probe
     * past readiness. Ported from osito-a@6040e00. */
    uint64_t deadline = idt_get_ticks() + 30000;
    int peer = -1;
    while (idt_get_ticks() < deadline) {
        if (!agent_is_initialized()) { sched_yield(); continue; }
        for (int i = 0; i < CLUSTER_MAX_PEERS; i++) {
            if (g_meta[i].in_use && g_meta[i].state == PEER_ALIVE
                && g_meta[i].rpc_port != 0) {
                peer = i; break;
            }
        }
        if (peer >= 0) break;
        sched_yield();
    }
    if (peer < 0) {
        serial_puts("[CLUSTER-PROBE] no ALIVE peer found — aborting\n");
        return;
    }
    /* Brief settle: give the peer's agent_init time too if they
     * started slightly later. 10 s is generous for paired boot. */
    uint64_t settle_until = idt_get_ticks() + 1000;
    while (idt_get_ticks() < settle_until) sched_yield();
    cluster_peer_meta_t *m = &g_meta[peer];
    serial_puts("[CLUSTER-PROBE] delegating to ");
    put_ip(m->ip); serial_puts(":");
    serial_putdec((uint64_t)m->rpc_port); serial_puts("\n");

    char resp[1024];
    /* Factual question triggers the brandon-mode RAG short-circuit on
     * the remote, which completes in ~5s — well within the RPC recv
     * timeout. A non-factual prompt would force full inference (30+ s)
     * and hit the (now 90s) recv timeout in inferconnect_client.c. */
    int n = inferconnect_remote_agent_task(m->ip, m->rpc_port,
                                            "Who wrote Hamlet?",
                                            resp, sizeof resp);
    if (n > 0) {
        resp[n < (int)sizeof resp ? n : (int)sizeof resp - 1] = 0;
        serial_puts("[CLUSTER-PROBE] reply (");
        serial_putdec((uint64_t)n); serial_puts(" B): ");
        serial_puts(resp);
        serial_puts("\n[CLUSTER-PROBE] delegation OK\n");
    } else {
        serial_puts("[CLUSTER-PROBE] delegation FAILED\n");
    }
}

void cluster_delegate_probe_start(void)
{
    (void)kthread_create("cluster-probe", cluster_delegate_probe_thread, NULL);
}

int cluster_keygen(void)
{
    uint8_t key[32];
    random_get_bytes(key, 32);
    void *f = osfs2_create("oict-key.txt", 32);
    if (!f) { serial_puts("[CLUSTER] keygen: osfs2_create failed\n"); return -1; }
    if (osfs2_write(f, 0, key, 32) < 0) {
        serial_puts("[CLUSTER] keygen: osfs2_write failed\n");
        return -1;
    }
    kmemcpy(g_psk, key, 32);
    g_cluster.psk_loaded = true;
    serial_puts("[CLUSTER] keygen: 32 random bytes → oict-key.txt\n");
    return 0;
}
