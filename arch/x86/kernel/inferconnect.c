/*
 * arch/x86/kernel/inferconnect.c — InferConnect worker discovery
 *
 * Compatibility-mode broadcaster for the InferConnect protocol
 * (~/inferconnect/src/network/discovery.cpp). Announces this kernel
 * as a worker node every BROADCAST_INTERVAL_TICKS ticks via a UDP
 * multicast packet to 239.255.255.250:19999.
 *
 * Wire format (ASCII, no NUL):
 *     INFERCONNECT:<rpc_port>:<vram_bytes>:<backend_name>
 *
 * Notes:
 *   - We are not yet RPC-capable (no ggml-rpc server), so rpc_port
 *     advertises 0 until phase 2 lands. A coordinator that sees this
 *     entry will skip layer assignment and treat us as discovery-only,
 *     which matches what we can deliver today.
 *   - "vram" is reused for "system memory available for inference"
 *     since we don't have a GPU exposed yet on x86; future work moves
 *     this to GPU memory once `kernel/drivers/gpu*.c` is wired up.
 *   - On QEMU SLIRP the multicast packet stays inside the VM (SLIRP
 *     doesn't bridge multicast onto the host LAN), but on real i211
 *     hardware the broadcast reaches every node on L2.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern int  net_udp_send_multicast(const uint8_t group_ip[4],
                                    uint16_t dst_port, uint16_t src_port,
                                    const void *data, uint32_t len);
extern uint64_t mem_get_free(void);
extern uint64_t idt_get_ticks(void);
extern int  kthread_create(const char *name, void (*fn)(void *), void *data);
extern void sched_yield(void);

#define IC_GROUP_IP   { 239, 255, 255, 250 }
#define IC_PORT       19999
#define IC_INTERVAL_TICKS  200   /* 200 * 10ms ≈ 2s, matches reference */
#define IC_BACKEND    "osito-a-x86-avx2"

static int      ic_kthread_idx = -1;
static volatile int ic_running = 0;    /* set by start, cleared by stop */
static uint16_t ic_rpc_port = 0;       /* 0 until ggml-rpc lands */
static uint64_t ic_packets_sent = 0;
static uint64_t ic_last_send_tick = 0;
static bool     ic_first_send_reported = false;

/* Append decimal to dst at *pos, capped at cap. Returns chars written. */
static int ic_append_dec(char *dst, int *pos, int cap, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int written = 0;
    while (n > 0 && *pos < cap) { dst[(*pos)++] = tmp[--n]; written++; }
    return written;
}

static int ic_build_msg(char *buf, int cap)
{
    static const char prefix[] = "INFERCONNECT:";
    int p = 0;
    for (uint32_t i = 0; i < sizeof(prefix) - 1 && p < cap; i++)
        buf[p++] = prefix[i];
    ic_append_dec(buf, &p, cap, ic_rpc_port);
    if (p < cap) buf[p++] = ':';
    ic_append_dec(buf, &p, cap, mem_get_free());
    if (p < cap) buf[p++] = ':';
    static const char backend[] = IC_BACKEND;
    for (uint32_t i = 0; i < sizeof(backend) - 1 && p < cap; i++)
        buf[p++] = backend[i];
    return p;
}

static void ic_thread(void *data)
{
    (void)data;
    static const uint8_t grp[4] = IC_GROUP_IP;
    char msg[96];

    serial_puts("[IC] worker broadcast start: 239.255.255.250:19999\n");

    extern void inferconnect_probe_all(void);
    bool auto_probe_done = false;
    while (ic_running) {
        uint64_t now = idt_get_ticks();
        if (now - ic_last_send_tick >= IC_INTERVAL_TICKS) {
            int len = ic_build_msg(msg, sizeof(msg));
            int rc = net_udp_send_multicast(grp, IC_PORT, IC_PORT,
                                             msg, (uint32_t)len);
            /* Discovery fallback: also limited-broadcast the heartbeat.
             * macOS vmnet-shared bridges broadcast between guests but
             * drops the 239.255.255.250 multicast group, so the multicast
             * send alone never reaches peers there. The broadcast carries
             * our real source IP (ic_peer_handler keys peers off it).
             * Harmless on real LANs (peers dedupe by IP). */
            extern int net_udp_send_broadcast_self(uint16_t, uint16_t,
                                                    const void *, uint32_t);
            if (rc == 0) {
                (void)net_udp_send_broadcast_self(IC_PORT, IC_PORT,
                                                  msg, (uint32_t)len);
            }
            if (rc == 0) ic_packets_sent++;
            /* After a few heartbeats, give peers time to land in our
             * table and then probe each one over TCP/RPC. One-shot:
             * proves bidirectional kernel-to-kernel discovery works.
             * Runs once per boot. */
            if (!auto_probe_done && ic_packets_sent >= 4) {
                auto_probe_done = true;
                /* Only the kernel with the LOWER IP runs the smoke
                 * (probe + remote task). The peer with higher IP just
                 * stays as RPC server and answers. Avoids
                 * bidirectional connect contention on a minimal TCP
                 * stack without proper TIME_WAIT handling. */
                extern void virtio_net_get_mac(uint8_t mac[6]);
                extern void i211_get_mac(uint8_t mac[6]);
                extern bool virtio_net_is_ready(void);
                uint8_t mac[6];
                if (virtio_net_is_ready()) virtio_net_get_mac(mac);
                else                        i211_get_mac(mac);
                uint8_t our_last = (mac[5] & 0xEF) | 0x10;
                if (our_last == 0 || our_last == 0xFF) our_last = 0x10;
                extern bool inferconnect_should_initiate(uint8_t our_last);
                if (inferconnect_should_initiate(our_last)) {
                    /* Single TCP connection: bundle the discovery
                     * handshake AND the agent task in one session
                     * so we never tear down + reconnect (the kernel
                     * TCP doesn't yet handle rapid re-connects to
                     * the same peer cleanly). probe_all is omitted
                     * here — its discovery probe is now folded into
                     * remote_task_smoke's single connection. */
                    extern void inferconnect_remote_task_smoke(void);
                    inferconnect_remote_task_smoke();
                }
            }
            /* Log only the first send result (success or fail) so we
             * can tell from the boot trace whether the NIC accepted
             * the multicast frame; subsequent sends would flood the
             * log. After that, `agent inferconnect status` reports
             * the running counter. */
            if (!ic_first_send_reported) {
                serial_puts(rc == 0 ? "[IC] first heartbeat sent OK\n"
                                    : "[IC] first heartbeat send failed\n");
                ic_first_send_reported = true;
            }
            ic_last_send_tick = now;
        }
        sched_yield();
    }

    serial_puts("[IC] worker broadcast stopped\n");
}

int  inferconnect_running(void)        { return ic_kthread_idx >= 0; }
uint64_t inferconnect_packets(void)    { return ic_packets_sent; }
void inferconnect_set_rpc_port(uint16_t p) { ic_rpc_port = p; }

/* ── Peer listener ─────────────────────────────────────────────── */
/* Ingests heartbeats from other InferConnect workers on the LAN so
 * the kernel can act as a coordinator too — track who else is
 * announcing themselves and (eventually) federate inference with
 * them. For now we just maintain a small table and log first sights. */

#define IC_MAX_PEERS  16

typedef struct {
    bool     valid;
    uint8_t  ip[4];
    uint16_t rpc_port;
    uint64_t free_mem;
    uint64_t last_seen_tick;
    char     backend[32];
} ic_peer_t;

static ic_peer_t ic_peers[IC_MAX_PEERS];
static uint32_t  ic_peers_seen_total;

extern void net_udp_listen(uint16_t port,
    void (*handler)(const uint8_t *src_ip, uint16_t src_port,
                    const void *data, uint32_t len));

static int ic_str_eq(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

static int ic_parse_dec(const char **p, uint64_t *out)
{
    uint64_t v = 0; int any = 0;
    while (**p >= '0' && **p <= '9') { v = v*10 + (uint64_t)(**p - '0'); (*p)++; any = 1; }
    *out = v;
    return any;
}

static void ic_peer_handler(const uint8_t *src_ip, uint16_t src_port,
                             const void *data, uint32_t len)
{
    (void)src_port;
    if (len < 14 || len > 200) return;
    const char *s = (const char *)data;
    if (!ic_str_eq(s, "INFERCONNECT:", 13)) return;

    /* Skip our own multicast (loopback): if src_ip matches our IP, drop. */
    extern uint8_t our_ip_storage[4] __attribute__((weak));   /* not exported; use a different cue */
    /* The kernel doesn't expose our_ip publicly here, so fall back to a
     * "is this a peer we don't already have under same IP+port" check
     * — duplicate-suppression below makes that idempotent. */

    const char *p = s + 13;
    uint64_t rpc_port = 0, free_mem = 0;
    if (!ic_parse_dec(&p, &rpc_port)) return;
    if (*p != ':') return; p++;
    if (!ic_parse_dec(&p, &free_mem)) return;
    if (*p != ':') return; p++;
    /* p .. end = backend name */
    char backend[32]; uint32_t bn = 0;
    while (*p && bn < sizeof(backend) - 1 && (uintptr_t)(p - s) < len)
        backend[bn++] = *p++;
    backend[bn] = '\0';

    /* Find existing peer slot or grab a free one. Match by IP. */
    int slot = -1;
    for (int i = 0; i < IC_MAX_PEERS; i++) {
        if (ic_peers[i].valid &&
            ic_peers[i].ip[0] == src_ip[0] && ic_peers[i].ip[1] == src_ip[1] &&
            ic_peers[i].ip[2] == src_ip[2] && ic_peers[i].ip[3] == src_ip[3]) {
            slot = i; break;
        }
    }
    bool is_new = false;
    if (slot < 0) {
        for (int i = 0; i < IC_MAX_PEERS; i++) {
            if (!ic_peers[i].valid) { slot = i; is_new = true; break; }
        }
    }
    if (slot < 0) return;   /* table full */

    ic_peer_t *pe = &ic_peers[slot];
    pe->valid = true;
    for (int i = 0; i < 4; i++) pe->ip[i] = src_ip[i];
    pe->rpc_port = (uint16_t)rpc_port;
    pe->free_mem = free_mem;
    pe->last_seen_tick = idt_get_ticks();
    for (uint32_t i = 0; i < sizeof(pe->backend) && i < bn + 1; i++)
        pe->backend[i] = backend[i];

    if (is_new) {
        ic_peers_seen_total++;
        serial_puts("[IC-PEER] discovered ");
        serial_putdec(src_ip[0]); serial_puts(".");
        serial_putdec(src_ip[1]); serial_puts(".");
        serial_putdec(src_ip[2]); serial_puts(".");
        serial_putdec(src_ip[3]); serial_puts(":");
        serial_putdec(rpc_port);
        serial_puts(" (");
        serial_puts(backend);
        serial_puts(", ");
        serial_putdec(free_mem >> 20);
        serial_puts(" MiB)\n");
    }
}

/* ── Accessors for cluster.c (parallel meta table indexed identically) ── */

uint32_t inferconnect_max_peers(void) { return IC_MAX_PEERS; }

int inferconnect_peer_get(int idx,
                          uint8_t ip[4], uint16_t *rpc_port,
                          uint64_t *free_mem, char backend[32],
                          uint64_t *last_seen_tick)
{
    if (idx < 0 || idx >= IC_MAX_PEERS) return -1;
    if (!ic_peers[idx].valid) return -1;
    ic_peer_t *p = &ic_peers[idx];
    if (ip)         { for (int i = 0; i < 4; i++) ip[i] = p->ip[i]; }
    if (rpc_port)   *rpc_port = p->rpc_port;
    if (free_mem)   *free_mem = p->free_mem;
    if (backend)    { for (int i = 0; i < 32; i++) backend[i] = p->backend[i]; }
    if (last_seen_tick) *last_seen_tick = p->last_seen_tick;
    return 0;
}

void inferconnect_peer_invalidate(int idx)
{
    if (idx < 0 || idx >= IC_MAX_PEERS) return;
    ic_peers[idx].valid = false;
}

/* Public API for shell + status reporting. */
uint32_t inferconnect_peers_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < IC_MAX_PEERS; i++) if (ic_peers[i].valid) n++;
    return n;
}

uint32_t inferconnect_peers_total_seen(void) { return ic_peers_seen_total; }

/* Render the peer table into a caller buffer, one line per valid
 * peer:  <ip>:<port> backend=<name> mem=<MiB>MiB last=<ticks>\n
 * Used by the agent's peer_list tool so the model sees who's
 * reachable. Returns bytes written (NUL-terminates when cap > 0). */
int inferconnect_peers_render(char *buf, uint32_t cap)
{
    if (!buf || cap == 0) return -1;
    uint32_t off = 0;
    uint32_t n_valid = 0;
    for (int i = 0; i < IC_MAX_PEERS; i++) {
        if (!ic_peers[i].valid) continue;
        if (off + 64 >= cap) break;     /* leave room for next line */
        ic_peer_t *p = &ic_peers[i];
        char tmp[24]; int t;
        /* ip a.b.c.d */
        for (int o = 0; o < 4; o++) {
            t = 0; uint32_t v = p->ip[o];
            if (v == 0) tmp[t++] = '0';
            else { while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; } }
            while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
            if (o < 3 && off + 1 < cap) buf[off++] = '.';
        }
        if (off + 1 < cap) buf[off++] = ':';
        t = 0; uint32_t pv = p->rpc_port;
        if (pv == 0) tmp[t++] = '0';
        else { while (pv) { tmp[t++] = (char)('0' + pv % 10); pv /= 10; } }
        while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        /* backend */
        const char *kw = " backend=";
        for (int k = 0; kw[k] && off + 1 < cap; k++) buf[off++] = kw[k];
        for (int k = 0; p->backend[k] && k < 32 && off + 1 < cap; k++)
            buf[off++] = p->backend[k];
        /* mem */
        kw = " mem=";
        for (int k = 0; kw[k] && off + 1 < cap; k++) buf[off++] = kw[k];
        t = 0; uint64_t mv = p->free_mem >> 20;
        if (mv == 0) tmp[t++] = '0';
        else { while (mv) { tmp[t++] = (char)('0' + mv % 10); mv /= 10; } }
        while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        if (off + 4 < cap) { buf[off++] = 'M'; buf[off++] = 'i'; buf[off++] = 'B'; }
        /* last */
        kw = " last=";
        for (int k = 0; kw[k] && off + 1 < cap; k++) buf[off++] = kw[k];
        t = 0; uint64_t lv = p->last_seen_tick;
        if (lv == 0) tmp[t++] = '0';
        else { while (lv) { tmp[t++] = (char)('0' + lv % 10); lv /= 10; } }
        while (t > 0 && off + 1 < cap) buf[off++] = tmp[--t];
        if (off + 1 < cap) buf[off++] = '\n';
        n_valid++;
    }
    if (n_valid == 0) {
        const char *m = "(no peers)";
        for (uint32_t i = 0; m[i] && off + 1 < cap; i++) buf[off++] = m[i];
    }
    if (off < cap) buf[off] = 0;
    return (int)off;
}

void inferconnect_peers_dump(void)
{
    for (int i = 0; i < IC_MAX_PEERS; i++) {
        if (!ic_peers[i].valid) continue;
        ic_peer_t *p = &ic_peers[i];
        serial_puts("  ");
        serial_putdec(p->ip[0]); serial_puts(".");
        serial_putdec(p->ip[1]); serial_puts(".");
        serial_putdec(p->ip[2]); serial_puts(".");
        serial_putdec(p->ip[3]); serial_puts(":");
        serial_putdec((uint64_t)p->rpc_port);
        serial_puts(" ");
        serial_puts(p->backend);
        serial_puts(" (");
        serial_putdec(p->free_mem >> 20);
        serial_puts(" MiB, last=");
        serial_putdec(p->last_seen_tick);
        serial_puts(")\n");
    }
}

/* True if our IP's host byte is strictly less than at least one
 * known peer's — i.e. we should be the active "client" in the
 * bidirectional pair, the peer is just a server. */
bool inferconnect_should_initiate(uint8_t our_last)
{
    for (int i = 0; i < IC_MAX_PEERS; i++) {
        if (!ic_peers[i].valid) continue;
        if (our_last < ic_peers[i].ip[3]) return true;
    }
    return false;
}

/* Probe every peer in the table via TCP RPC handshake. Drives the
 * client side of the kernel-to-kernel discovery loop — we already
 * track peers from heartbeats; this proves the RPC channel back works
 * too. Used by `agent inferconnect probe_all` and the auto-probe
 * kthread. */
void inferconnect_probe_all(void)
{
    extern int inferconnect_probe_peer(const uint8_t ip[4], uint16_t port);
    uint32_t probed = 0, ok = 0;
    for (int i = 0; i < IC_MAX_PEERS; i++) {
        if (!ic_peers[i].valid) continue;
        if (ic_peers[i].rpc_port == 0) continue;
        probed++;
        if (inferconnect_probe_peer(ic_peers[i].ip,
                                     ic_peers[i].rpc_port) == 0) ok++;
    }
    serial_puts("[IC-CLIENT] probed ");
    serial_putdec((uint64_t)probed);
    serial_puts(" peer(s), ");
    serial_putdec((uint64_t)ok);
    serial_puts(" ok\n");
}

/* Send a federated agent task to the first known peer. One-shot
 * smoke test: proves the OsitoA-native RPC opcode round-trips end
 * to end across two kernels. Used by the auto-probe in the
 * broadcaster after discovery has had time to stabilise.
 *
 * To avoid bidirectional connect races (both kernels firing remote
 * tasks at each other simultaneously, exhausting TCP conn slots),
 * only the kernel with the LOWER IP initiates. The peer with the
 * higher IP just stays as RPC server and answers. */
void inferconnect_remote_task_smoke(void)
{
    extern int inferconnect_remote_agent_task(const uint8_t ip[4],
                                                uint16_t port,
                                                const char *prompt,
                                                char *out, uint32_t cap);
    extern void net_get_our_ip(uint8_t out[4]) __attribute__((weak));
    /* No public our_ip getter; fall back to picking lowest peer IP
     * vs ours by sneaking the local IP from the first stamped
     * heartbeat (we know it's 10.0.2.X). Simpler: rank peers in the
     * table — only initiate if our IP < the peer's IP. */
    for (int i = 0; i < IC_MAX_PEERS; i++) {
        if (!ic_peers[i].valid || ic_peers[i].rpc_port == 0) continue;
        /* Compare last octet of MAC since our IP is derived from it
         * (mac[5] | 0x10). Pick the right NIC driver — virtio_net or
         * i211 — based on which one nic_ops is bound to. The vtable
         * abstraction is in kernel/nic.h but as a static-inline; use
         * the underlying driver's get_mac directly. */
        extern void virtio_net_get_mac(uint8_t mac[6]);
        extern void i211_get_mac(uint8_t mac[6]);
        extern bool virtio_net_is_ready(void);
        uint8_t mac[6];
        if (virtio_net_is_ready()) virtio_net_get_mac(mac);
        else                        i211_get_mac(mac);
        uint8_t our_last = (mac[5] & 0xEF) | 0x10;
        if (our_last == 0 || our_last == 0xFF) our_last = 0x10;
        if (ic_peers[i].ip[3] <= our_last) continue;  /* not the initiator */

        char buf[512];
        int n = inferconnect_remote_agent_task(
            ic_peers[i].ip, ic_peers[i].rpc_port,
            "list your free memory in MiB", buf, sizeof(buf));
        if (n > 0) {
            serial_puts("[IC-CLIENT] remote task reply (");
            serial_putdec((uint64_t)n);
            serial_puts(" B): ");
            serial_puts(buf);
            serial_puts("\n");
        } else {
            serial_puts("[IC-CLIENT] remote task failed\n");
        }
        return;   /* one peer is enough for the smoke */
    }
}

int inferconnect_peer_listener_start(void)
{
    /* Idempotent. net_udp_listen registers our callback on port 19999;
     * incoming multicast (or unicast) UDP to that port triggers
     * ic_peer_handler. Combined with handle_ipv4's multicast acceptance
     * patch in net.c, this means we ingest peer heartbeats without any
     * IGMP machinery (the LAN/QEMU virtual L2 forwards them). */
    net_udp_listen(19999, ic_peer_handler);
    return 0;
}

int inferconnect_start(void)
{
    if (ic_running) return 0;
    ic_running = 1;
    ic_kthread_idx = kthread_create("inferconnect-bcast", ic_thread, NULL);
    if (ic_kthread_idx < 0) { ic_running = 0; return -1; }
    return 0;
}

void inferconnect_stop(void)
{
    if (!ic_running) return;
    ic_running = 0;            /* kthread loop notices and exits */
    ic_kthread_idx = -1;
}
