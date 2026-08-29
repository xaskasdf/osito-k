/*
 * arch/x86/kernel/inferconnect_rpc.c — minimal ggml-rpc server
 *
 * Implements the discovery-only subset of the ggml-rpc wire protocol
 * (llama.cpp v4.0.0) so external InferConnect coordinators can:
 *
 *   1. negotiate_hello() — version handshake + transport caps
 *   2. RPC_CMD_DEVICE_COUNT  → 1
 *   3. RPC_CMD_GET_DEVICE_MEMORY → (free_mem, total_mem) for device 0
 *
 * That is sufficient for the coordinator to register us as a worker
 * and probe our memory budget. Anything past GRAPH_COMPUTE is rejected
 * (connection closed) — the coordinator will skip layer assignment
 * and either fall back to local compute or pick another worker. This
 * is the "discovery-but-not-compute" stance the broadcaster already
 * advertises (see kernel/inferconnect.c).
 *
 * Wire format (from ggml/src/ggml-rpc/ggml-rpc.cpp):
 *
 *   Request : | u8 cmd | u64 input_size | input_size bytes |
 *   Response: | u64 output_size | output_size bytes |
 *
 * All multi-byte ints little-endian (host order on x86_64).
 */

#include "../include/types.h"
#include "oict.h"
#include "cluster.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int digits);

extern int  net_tcp_listen(uint16_t port);
extern int  net_tcp_accept(int listener_idx, uint32_t timeout_ticks);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                                  uint32_t timeout_ticks);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern void net_tcp_close(int conn);
extern int  net_tcp_get_peer_ip(int conn, uint8_t ip_out[4]);

extern uint64_t mem_get_free(void);
extern int  kthread_create(const char *name, void (*fn)(void *), void *data);
extern void sched_yield(void);

/* ── Protocol constants (ggml-rpc v4.0.0) ──────────────────────── */
#define IC_RPC_PROTO_MAJOR    4
#define IC_RPC_PROTO_MINOR    0
#define IC_RPC_PROTO_PATCH    0
#define IC_RPC_CONN_CAPS_SIZE 24

enum {
    RPC_CMD_ALLOC_BUFFER       = 0,
    RPC_CMD_GET_ALIGNMENT      = 1,
    RPC_CMD_GET_MAX_SIZE       = 2,
    RPC_CMD_BUFFER_GET_BASE    = 3,
    RPC_CMD_FREE_BUFFER        = 4,
    RPC_CMD_BUFFER_CLEAR       = 5,
    RPC_CMD_SET_TENSOR         = 6,
    RPC_CMD_SET_TENSOR_HASH    = 7,
    RPC_CMD_GET_TENSOR         = 8,
    RPC_CMD_COPY_TENSOR        = 9,
    RPC_CMD_GRAPH_COMPUTE      = 10,
    RPC_CMD_GET_DEVICE_MEMORY  = 11,
    RPC_CMD_INIT_TENSOR        = 12,
    RPC_CMD_GET_ALLOC_SIZE     = 13,
    RPC_CMD_HELLO              = 14,
    RPC_CMD_DEVICE_COUNT       = 15,
    RPC_CMD_GRAPH_RECOMPUTE    = 16,

    /* OsitoA-native opcodes — outside the ggml-rpc range so they
     * can't collide with the upstream protocol. xinocod and other
     * ggml-rpc clients ignore these; only osito-a kernels use them
     * to federate agent tasks across the mesh. */
    RPC_CMD_OSITOA_AGENT_TASK  = 200,
};

#pragma pack(push, 1)
struct rpc_msg_hello_req {
    uint8_t conn_caps[IC_RPC_CONN_CAPS_SIZE];
};
struct rpc_msg_hello_rsp {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t padding;
    uint8_t conn_caps[IC_RPC_CONN_CAPS_SIZE];
};
struct rpc_msg_device_count_rsp {
    uint32_t device_count;
};
struct rpc_msg_get_device_memory_req {
    uint32_t device;
};
struct rpc_msg_get_device_memory_rsp {
    uint64_t free_mem;
    uint64_t total_mem;
};
#pragma pack(pop)

/* ── State ─────────────────────────────────────────────────────── */
#define IC_RPC_PORT_DEFAULT  50052
#define IC_RPC_RECV_TIMEOUT  500   /* ticks (5s @ 100Hz) per chunk */
#define IC_RPC_FRAME_MAX     16384 /* sized for OICT v1 worst case:
                                     * Llama 3.2-1B hidden Q8_0 (2176 B)
                                     * + 12 B oict header + 9 B opcode
                                     * envelope. Bumped from 4096 by
                                     * the OICT plan. Anything bigger
                                     * (chunked transfers in v2) flips
                                     * OICT_FLAG_CHUNKED in the header
                                     * and uses multi-frame sequencing. */

static volatile int ic_rpc_running = 0;
static int          ic_rpc_kth_idx = -1;
static uint16_t     ic_rpc_port = IC_RPC_PORT_DEFAULT;
static uint64_t     ic_rpc_connections_handled = 0;
static uint64_t     ic_rpc_hellos_seen = 0;

/* Forward decl — set by inferconnect.c so the broadcaster can include
 * the actual rpc_port in heartbeats. Resolved at link time. */
extern void inferconnect_set_rpc_port(uint16_t p);

/* ── recv exactly N bytes from conn (or fail) ──────────────────── */
int ic_recv_exact(int conn, void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < n) {
        int r = net_tcp_recv_timeout(conn, p + got, n - got, IC_RPC_RECV_TIMEOUT);
        if (r <= 0) return -1;   /* timeout or closed */
        got += (uint32_t)r;
    }
    return 0;
}

/* ── send size + payload framing ───────────────────────────────── */
static int ic_send_response(int conn, const void *data, uint64_t size)
{
    if (net_tcp_send(conn, &size, 8) < 0) return -1;
    if (size > 0 && net_tcp_send(conn, data, (uint32_t)size) < 0) return -1;
    return 0;
}

/* Per-cmd handlers. Return 0 if the connection should remain open,
 * -1 if we should close (unsupported cmd or malformed input). */

static int ic_handle_hello(int conn, uint64_t in_size)
{
    struct rpc_msg_hello_req req;
    if (in_size != sizeof(req)) return -1;
    if (ic_recv_exact(conn, &req, sizeof(req)) < 0) return -1;

    struct rpc_msg_hello_rsp rsp;
    rsp.major   = IC_RPC_PROTO_MAJOR;
    rsp.minor   = IC_RPC_PROTO_MINOR;
    rsp.patch   = IC_RPC_PROTO_PATCH;
    rsp.padding = 0;
    /* Zero conn_caps = no transport upgrades. The client's request
     * caps go ignored; ggml-rpc will fall back to plain TCP framing. */
    for (int i = 0; i < IC_RPC_CONN_CAPS_SIZE; i++) rsp.conn_caps[i] = 0;

    ic_rpc_hellos_seen++;
    return ic_send_response(conn, &rsp, sizeof(rsp));
}

static int ic_handle_device_count(int conn, uint64_t in_size)
{
    if (in_size != 0) return -1;
    struct rpc_msg_device_count_rsp rsp;
    rsp.device_count = 1;
    return ic_send_response(conn, &rsp, sizeof(rsp));
}

static int ic_handle_get_device_memory(int conn, uint64_t in_size)
{
    struct rpc_msg_get_device_memory_req req;
    if (in_size != sizeof(req)) return -1;
    if (ic_recv_exact(conn, &req, sizeof(req)) < 0) return -1;
    if (req.device != 0) return -1;

    /* Treat free RAM as our "device memory" — we are CPU-only on the
     * x86 path until GPU drivers wire in. Total = free here, since
     * we don't track preallocated kernel reservations separately. */
    struct rpc_msg_get_device_memory_rsp rsp;
    rsp.free_mem  = mem_get_free();
    rsp.total_mem = rsp.free_mem;
    return ic_send_response(conn, &rsp, sizeof(rsp));
}

/* OsitoA-native: federated agent task. Body is `u32 prompt_len |
 * bytes`. The server runs the prompt on a local agent slot via
 * `agent_run_subtask` (synchronous, bounded budget) and returns
 * `u32 result_len | bytes`. If no model is loaded, returns a stub
 * so the protocol round-trip stays verifiable even without inference. */
static int ic_handle_agent_task(int conn, uint64_t in_size)
{
    if (in_size < 4 || in_size > 4096) return -1;

    uint32_t prompt_len;
    if (ic_recv_exact(conn, &prompt_len, 4) < 0) return -1;
    if (prompt_len + 4 != in_size) return -1;

    static char  prompt_buf[2048];
    static char  result_buf[2048];
    if (prompt_len >= sizeof(prompt_buf)) return -1;
    if (ic_recv_exact(conn, prompt_buf, prompt_len) < 0) return -1;
    prompt_buf[prompt_len] = 0;

    serial_puts("[IC-RPC] agent task ("); serial_putdec((uint64_t)prompt_len);
    serial_puts(" B): "); serial_puts(prompt_buf); serial_puts("\n");

    /* Real federated dispatch: submit the prompt to a local agent
     * slot, then block on the new wait-queue primitive until the
     * slot kthread publishes the response.  The 60 s timeout
     * bounds remote-end latency so the TCP conn doesn't hang
     * indefinitely on a busy peer; the requester sees an empty
     * result and can retry.
     *
     * On submit failure (queue full, no model) fall back to the
     * legacy echo stub so the protocol round-trip still verifies. */
    extern int64_t agent_submit_slot(uint32_t slot, const char *prompt,
                                      uint32_t max_tokens, float temp);
    extern int     agent_slot_wait(uint32_t slot, uint64_t task_id,
                                    uint32_t timeout_ticks);
    extern uint32_t agent_read_response_slot(uint32_t slot, char *out,
                                              uint32_t out_cap,
                                              uint32_t since_offset);

    uint32_t rlen = 0;
    /* Use slot 0 — single-flight per peer.  AGENT_TEMP_BANDIT (-1.0f)
     * defers temperature to the in-slot RL bandit. */
    int64_t tid = agent_submit_slot(0, prompt_buf, 256, -1.0f);
    if (tid > 0 && agent_slot_wait(0, (uint64_t)tid, 6000) == 0) {
        rlen = agent_read_response_slot(0, result_buf,
                                         (uint32_t)sizeof(result_buf), 0);
    } else {
        const char *prefix = "[osito-a peer ack] echo: ";
        for (uint32_t i = 0; prefix[i] && rlen < sizeof(result_buf); i++)
            result_buf[rlen++] = prefix[i];
        for (uint32_t i = 0; i < prompt_len && rlen < sizeof(result_buf); i++)
            result_buf[rlen++] = prompt_buf[i];
    }

    /* Response framing: u64 size | u32 result_len | bytes */
    uint64_t out_size = 4 + rlen;
    if (net_tcp_send(conn, &out_size, 8) < 0) return -1;
    if (net_tcp_send(conn, &rlen, 4) < 0) return -1;
    if (rlen && net_tcp_send(conn, result_buf, rlen) < 0) return -1;
    return 0;
}

/* Best-effort drain of an unsupported request body so we don't leave
 * partial frames in the socket before closing. */
static void ic_drain(int conn, uint64_t n)
{
    uint8_t scratch[256];
    while (n > 0) {
        uint32_t want = n > sizeof(scratch) ? (uint32_t)sizeof(scratch) : (uint32_t)n;
        int r = net_tcp_recv_timeout(conn, scratch, want, IC_RPC_RECV_TIMEOUT);
        if (r <= 0) return;
        n -= (uint64_t)r;
    }
}

static void ic_handle_connection(int conn)
{
    ic_rpc_connections_handled++;
    serial_puts("[IC-RPC] conn "); serial_putdec((uint64_t)conn);
    serial_puts(" accepted\n");
    while (1) {
        uint8_t cmd;
        uint64_t in_size;
        if (ic_recv_exact(conn, &cmd, 1) < 0) break;
        if (ic_recv_exact(conn, &in_size, 8) < 0) break;
        serial_puts("[IC-RPC]   cmd=");
        serial_putdec((uint64_t)cmd);
        serial_puts(" size=");
        serial_putdec(in_size);
        serial_puts("\n");

        /* Reject obviously oversized frames — anything > 4 KB is a
         * GRAPH/TENSOR payload we can't service yet. */
        if (in_size > IC_RPC_FRAME_MAX) {
            serial_puts("[IC-RPC] oversized frame (cmd=");
            serial_putdec((uint64_t)cmd);
            serial_puts(", size=");
            serial_putdec(in_size);
            serial_puts("), closing\n");
            break;
        }

        int rc;
        switch (cmd) {
            case RPC_CMD_HELLO:
                rc = ic_handle_hello(conn, in_size);
                break;
            case RPC_CMD_DEVICE_COUNT:
                rc = ic_handle_device_count(conn, in_size);
                break;
            case RPC_CMD_GET_DEVICE_MEMORY:
                rc = ic_handle_get_device_memory(conn, in_size);
                break;
            case RPC_CMD_OSITOA_AGENT_TASK:
                rc = ic_handle_agent_task(conn, in_size);
                break;
            case RPC_CMD_OSITOA_TOKENS:
                rc = oict_handle_tokens(conn, in_size);
                break;
            case RPC_CMD_OSITOA_LOGITS:
                rc = oict_handle_logits(conn, in_size);
                break;
            case RPC_CMD_OSITOA_HIDDEN:
                rc = oict_handle_hidden(conn, in_size);
                break;
            case RPC_CMD_OSITOA_BRANDON_BUNDLE:
                rc = oict_handle_brandon_bundle(conn, in_size);
                break;
            case RPC_CMD_OSITOA_CLUSTER_HEARTBEAT: {
                uint8_t sender_ip[4] = {0};
                (void)net_tcp_get_peer_ip(conn, sender_ip);
                rc = cluster_handle_heartbeat(conn, in_size, sender_ip);
                break;
            }
            case RPC_CMD_OSITOA_CLUSTER_INFO:
                rc = cluster_handle_info_req(conn, in_size);
                break;
            case RPC_CMD_OSITOA_CLUSTER_EPOCH:
                rc = cluster_handle_epoch_req(conn, in_size);
                break;
            default:
                /* Drain the body and close — coordinator skips us. */
                ic_drain(conn, in_size);
                serial_puts("[IC-RPC] unsupported cmd=");
                serial_putdec((uint64_t)cmd);
                serial_puts(" — closing\n");
                rc = -1;
                break;
        }
        if (rc < 0) break;
    }
    net_tcp_close(conn);
}

static void ic_rpc_thread(void *data)
{
    (void)data;
    /* Mask all SIMD floating-point exceptions in this kthread's MXCSR.
     * Default mask (0x1F80) suppresses PE/UE/OE/ZE/DE/IE. Without this,
     * a downstream call that runs FP code on denormal inputs (e.g.
     * agent_submit_slot → llama_forward) raises #XM and the kthread
     * is killed. Same fix the agent slot kthread + OpenAI server use. */
    {
        uint32_t mxcsr = 0x1F80;
        __asm__ __volatile__("ldmxcsr %0" :: "m"(mxcsr));
    }
    int listener = net_tcp_listen(ic_rpc_port);
    if (listener < 0) {
        serial_puts("[IC-RPC] failed to listen — abort\n");
        ic_rpc_running = 0;
        return;
    }
    inferconnect_set_rpc_port(ic_rpc_port);
    serial_puts("[IC-RPC] server up on port ");
    serial_putdec((uint64_t)ic_rpc_port);
    serial_puts("\n");

    while (ic_rpc_running) {
        /* Bound each accept attempt so the worker revisits its stop flag;
         * the network waiter yields cooperatively until data or timeout. */
        int conn = net_tcp_accept(listener, 10);   /* 100ms poll */
        if (conn < 0) { sched_yield(); continue; }
        ic_handle_connection(conn);
    }
    serial_puts("[IC-RPC] server stopped\n");
}

/* ── Public API ────────────────────────────────────────────────── */

int inferconnect_rpc_start(uint16_t port)
{
    if (ic_rpc_running) return 0;
    if (port != 0) ic_rpc_port = port;
    /* Publish rpc_port to the broadcaster SYNCHRONOUSLY before the
     * kthread launches, so the very first multicast heartbeat already
     * advertises the correct port. Otherwise the broadcaster's first
     * send carries port=0 (its default); peers cache port=0 for our IP
     * and cluster_send_heartbeat/delegation reject us until a later
     * heartbeat refreshes the slot. Ported from osito-a@6040e00. */
    inferconnect_set_rpc_port(ic_rpc_port);
    ic_rpc_running = 1;
    ic_rpc_kth_idx = kthread_create("inferconnect-rpc", ic_rpc_thread, NULL);
    if (ic_rpc_kth_idx < 0) { ic_rpc_running = 0; return -1; }
    return 0;
}

void inferconnect_rpc_stop(void)
{
    ic_rpc_running = 0;
    ic_rpc_kth_idx = -1;
    inferconnect_set_rpc_port(0);  /* heartbeats stop advertising port */
}

int      inferconnect_rpc_running(void)            { return ic_rpc_running; }
uint16_t inferconnect_rpc_port(void)               { return ic_rpc_port; }
uint64_t inferconnect_rpc_connections(void)        { return ic_rpc_connections_handled; }
uint64_t inferconnect_rpc_hellos(void)             { return ic_rpc_hellos_seen; }
