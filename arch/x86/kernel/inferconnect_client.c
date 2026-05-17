/*
 * arch/x86/kernel/inferconnect_client.c — kernel-to-kernel ggml-rpc client
 *
 * Closes the loop on InferConnect peer discovery: once another kernel
 * announces itself via UDP heartbeat (handled in inferconnect.c's peer
 * listener), this module connects to that peer's RPC server over TCP
 * and runs the v4.0.0 discovery handshake (HELLO + DEVICE_COUNT +
 * GET_DEVICE_MEMORY). The result is logged to serial and stashed in
 * the peer table so `agent inferconnect peers` can show the verified
 * device count + memory of each remote node.
 *
 * Wire format (mirrors inferconnect_rpc.c, the server side):
 *   Request : | u8 cmd | u64 input_size | input_size bytes |
 *   Response: | u64 output_size | output_size bytes |
 *
 * Used by:
 *   - `agent inferconnect probe <ip> <port>` shell command (one-shot)
 *   - Future: an auto-probe kthread that pings each new peer.
 */

#include "../include/types.h"
#include "oict.h"
#include "tensor.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int digits);
extern int  net_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                             uint16_t src_port);

/* Allocate a fresh ephemeral source port each call so back-to-back
 * connects don't reuse the same 4-tuple before the prior connection
 * has fully drained its TIME_WAIT-equivalent state on the peer side. */
static uint16_t ic_next_ephemeral(void)
{
    static uint16_t ctr = 49152;
    uint16_t p = ctr++;
    if (ctr > 65500) ctr = 49152;
    return p;
}
extern int  net_tcp_send(int conn, const void *data, uint32_t len);
extern int  net_tcp_recv_timeout(int conn, void *buf, uint32_t buf_size,
                                  uint32_t timeout_ticks);
extern void net_tcp_close(int conn);

/* Mirror the protocol constants from inferconnect_rpc.c */
#define IC_RPC_PROTO_MAJOR    4
#define IC_RPC_CONN_CAPS_SIZE 24
enum {
    RPC_CMD_GET_DEVICE_MEMORY  = 11,
    RPC_CMD_HELLO              = 14,
    RPC_CMD_DEVICE_COUNT       = 15,
    RPC_CMD_OSITOA_AGENT_TASK  = 200,
};

#pragma pack(push, 1)
struct hello_req { uint8_t conn_caps[IC_RPC_CONN_CAPS_SIZE]; };
struct hello_rsp {
    uint8_t major, minor, patch, padding;
    uint8_t conn_caps[IC_RPC_CONN_CAPS_SIZE];
};
struct dev_count_rsp { uint32_t device_count; };
struct dev_mem_req   { uint32_t device; };
struct dev_mem_rsp   { uint64_t free_mem; uint64_t total_mem; };
#pragma pack(pop)

/* Recv exactly N bytes (or fail). 5s tick timeout (~500 ticks @ 100Hz). */
static int recv_exact(int conn, void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < n) {
        int r = net_tcp_recv_timeout(conn, p + got, n - got, 500);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

/* Send the request framing and read back the response framing. On
 * success, fills `out` with up to `out_cap` bytes from the response
 * payload. Returns the response size or -1 on error. */
static int rpc_call(int conn, uint8_t cmd,
                    const void *in, uint32_t in_size,
                    void *out, uint32_t out_cap)
{
    if (net_tcp_send(conn, &cmd, 1) < 0) return -1;
    uint64_t in64 = in_size;
    if (net_tcp_send(conn, &in64, 8) < 0) return -1;
    if (in_size && net_tcp_send(conn, in, in_size) < 0) return -1;

    uint64_t out_size;
    if (recv_exact(conn, &out_size, 8) < 0) return -1;
    if (out_size > out_cap) return -1;
    if (out_size && recv_exact(conn, out, (uint32_t)out_size) < 0) return -1;
    return (int)out_size;
}

/* Probe a single peer. Logs result to serial and (optionally) updates
 * the peer table via the callbacks below. Returns 0 on success. */
int inferconnect_probe_peer(const uint8_t ip[4], uint16_t port)
{
    serial_puts("[IC-CLIENT] probing ");
    serial_putdec(ip[0]); serial_puts(".");
    serial_putdec(ip[1]); serial_puts(".");
    serial_putdec(ip[2]); serial_puts(".");
    serial_putdec(ip[3]); serial_puts(":");
    serial_putdec((uint64_t)port);
    serial_puts("\n");

    int conn = net_tcp_connect(ip, port, ic_next_ephemeral());
    if (conn < 0) {
        serial_puts("[IC-CLIENT] connect failed\n");
        return -1;
    }

    int rc = -1;
    uint8_t out_buf[64];

    /* HELLO */
    struct hello_req hreq = {0};
    int n = rpc_call(conn, RPC_CMD_HELLO, &hreq, sizeof(hreq),
                     out_buf, sizeof(out_buf));
    if (n != (int)sizeof(struct hello_rsp)) {
        serial_puts("[IC-CLIENT] HELLO failed\n");
        goto done;
    }
    struct hello_rsp *hrsp = (struct hello_rsp *)out_buf;
    if (hrsp->major != IC_RPC_PROTO_MAJOR) {
        serial_puts("[IC-CLIENT] proto mismatch\n");
        goto done;
    }
    serial_puts("[IC-CLIENT]   HELLO ok v");
    serial_putdec((uint64_t)hrsp->major); serial_puts(".");
    serial_putdec((uint64_t)hrsp->minor); serial_puts(".");
    serial_putdec((uint64_t)hrsp->patch); serial_puts("\n");

    /* DEVICE_COUNT */
    n = rpc_call(conn, RPC_CMD_DEVICE_COUNT, NULL, 0, out_buf, sizeof(out_buf));
    if (n != (int)sizeof(struct dev_count_rsp)) {
        serial_puts("[IC-CLIENT] DEVICE_COUNT failed\n");
        goto done;
    }
    uint32_t ndev = ((struct dev_count_rsp *)out_buf)->device_count;
    serial_puts("[IC-CLIENT]   devices=");
    serial_putdec((uint64_t)ndev); serial_puts("\n");

    /* GET_DEVICE_MEMORY for device 0 */
    struct dev_mem_req mreq = {0};
    n = rpc_call(conn, RPC_CMD_GET_DEVICE_MEMORY, &mreq, sizeof(mreq),
                 out_buf, sizeof(out_buf));
    if (n != (int)sizeof(struct dev_mem_rsp)) {
        serial_puts("[IC-CLIENT] GET_DEVICE_MEMORY failed\n");
        goto done;
    }
    struct dev_mem_rsp *m = (struct dev_mem_rsp *)out_buf;
    serial_puts("[IC-CLIENT]   free=");
    serial_putdec(m->free_mem >> 20); serial_puts(" MiB total=");
    serial_putdec(m->total_mem >> 20); serial_puts(" MiB\n");

    rc = 0;
done:
    net_tcp_close(conn);
    return rc;
}

/* Send an OsitoA agent task to a peer kernel. The peer runs the
 * prompt on its local agent (via agent_run_subtask), and returns
 * the result text. Synchronous — blocks until response or timeout.
 *
 * Returns: number of bytes written to `out` on success (always
 * NUL-terminates if cap > 0), -1 on failure. */
int inferconnect_remote_agent_task(const uint8_t ip[4], uint16_t port,
                                    const char *prompt,
                                    char *out, uint32_t cap)
{
    if (!prompt || !out || cap == 0) return -1;
    uint32_t plen = 0;
    while (prompt[plen] && plen < 1024) plen++;

    serial_puts("[IC-CLIENT] remote agent task -> ");
    serial_putdec(ip[0]); serial_puts(".");
    serial_putdec(ip[1]); serial_puts(".");
    serial_putdec(ip[2]); serial_puts(".");
    serial_putdec(ip[3]); serial_puts(":");
    serial_putdec((uint64_t)port); serial_puts(" (\"");
    serial_puts(prompt); serial_puts("\")\n");

    int conn = net_tcp_connect(ip, port, ic_next_ephemeral());
    if (conn < 0) {
        serial_puts("[IC-CLIENT] connect failed\n");
        return -1;
    }

    /* Send: u8 cmd | u64 in_size | u32 prompt_len | prompt_bytes */
    uint8_t  cmd = RPC_CMD_OSITOA_AGENT_TASK;
    uint64_t in_size = 4 + plen;
    if (net_tcp_send(conn, &cmd, 1) < 0) goto fail;
    if (net_tcp_send(conn, &in_size, 8) < 0) goto fail;
    if (net_tcp_send(conn, &plen, 4) < 0) goto fail;
    if (plen && net_tcp_send(conn, prompt, plen) < 0) goto fail;

    /* Recv: u64 out_size | u32 result_len | result_bytes. The remote
     * subtask can take a few seconds — give it a generous timeout. */
    uint64_t out_size;
    if (recv_exact(conn, &out_size, 8) < 0) goto fail;
    if (out_size < 4 || out_size > 4096) goto fail;
    uint32_t result_len;
    if (recv_exact(conn, &result_len, 4) < 0) goto fail;
    if (result_len > out_size - 4) goto fail;

    uint32_t copy = result_len < cap - 1 ? result_len : cap - 1;
    if (copy && recv_exact(conn, out, copy) < 0) goto fail;
    /* Drain any extra bytes we couldn't fit so the connection stays
     * sane if the caller's buffer was too small. */
    if (result_len > copy) {
        char scratch[64];
        uint32_t left = result_len - copy;
        while (left > 0) {
            uint32_t want = left < sizeof(scratch) ? left : (uint32_t)sizeof(scratch);
            if (recv_exact(conn, scratch, want) < 0) break;
            left -= want;
        }
    }
    out[copy] = 0;
    net_tcp_close(conn);
    return (int)copy;
fail:
    net_tcp_close(conn);
    return -1;
}

/* ── OICT client ───────────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

/* Send a fully-framed OICT request and read back a single response.
 * Reused by all three flavor wrappers. Returns response payload size
 * on success or -1 on error. The caller must size out_buf to fit
 * `OICT_MAX_*_BYTES` for the flavor it uses. */
static int oict_one_shot(const uint8_t ip[4], uint16_t port,
                          uint8_t cmd, const oict_msg_t *hdr,
                          const void *payload, uint32_t payload_len,
                          oict_msg_t *out_hdr,
                          uint8_t *out_buf, uint32_t out_cap)
{
    int conn = net_tcp_connect(ip, port, ic_next_ephemeral());
    if (conn < 0) return -1;

    uint64_t in_size = sizeof(*hdr) + payload_len;
    if (net_tcp_send(conn, &cmd, 1) < 0)             goto fail;
    if (net_tcp_send(conn, &in_size, 8) < 0)         goto fail;
    if (net_tcp_send(conn, hdr, sizeof(*hdr)) < 0)   goto fail;
    if (payload_len && net_tcp_send(conn, payload, payload_len) < 0) goto fail;

    uint64_t out_size;
    if (recv_exact(conn, &out_size, 8) < 0)          goto fail;
    if (out_size < sizeof(*out_hdr) || out_size > 16384) goto fail;
    if (recv_exact(conn, out_hdr, sizeof(*out_hdr)) < 0) goto fail;
    uint64_t pl = out_size - sizeof(*out_hdr);
    if (pl > out_cap) goto fail;
    if (pl && recv_exact(conn, out_buf, (uint32_t)pl) < 0) goto fail;

    net_tcp_close(conn);
    return (int)pl;
fail:
    net_tcp_close(conn);
    return -1;
}

int oict_remote_send_tokens(const uint8_t ip[4], uint16_t port,
                             const uint32_t *toks, uint32_t n_tok,
                             uint8_t *out_buf, uint32_t out_cap)
{
    if (n_tok > OICT_MAX_TOKEN_BATCH) return -1;
    oict_msg_t hdr = {
        .flavor = OICT_FLAVOR_TOKENS,
        .encoding = OICT_ENC_U32_TOKENS,
        .flags = 0, .version = OICT_VERSION_V1,
        .task_id = 0, .dim = n_tok,
    };
    oict_msg_t rsp;
    return oict_one_shot(ip, port, RPC_CMD_OSITOA_TOKENS,
                          &hdr, toks, n_tok * 4,
                          &rsp, out_buf, out_cap);
}

int oict_remote_send_hidden(const uint8_t ip[4], uint16_t port,
                             const float *hidden, uint32_t dim,
                             uint8_t *out_buf, uint32_t out_cap)
{
    if (dim == 0 || dim % 32 != 0 || dim > OICT_MAX_HIDDEN_DIM) return -1;
    /* Q8_0-quantize on the way out. Block layout matches dequant_q8_0. */
    static uint8_t qbuf[OICT_MAX_HIDDEN_BYTES];
    uint32_t qbytes = (dim / 32) * 34;
    quantize_q8_0(hidden, qbuf, dim);

    oict_msg_t hdr = {
        .flavor = OICT_FLAVOR_HIDDEN,
        .encoding = OICT_ENC_Q8_0,
        .flags = 0, .version = OICT_VERSION_V1,
        .task_id = 0, .dim = dim,
    };
    oict_msg_t rsp;
    return oict_one_shot(ip, port, RPC_CMD_OSITOA_HIDDEN,
                          &hdr, qbuf, qbytes,
                          &rsp, out_buf, out_cap);
}

/* Brandon bundle: cut_start in [1, OICT_MAX_BRANDON_CUT], dim and
 * kv_dim each multiple-of-32 and ≤ their MAX. Wire layout matches
 * the server-side parser:
 *
 *   [u32 cut_start][u32 kv_dim][(cut_start+1)·dim Q8_0][kv_dim Q8_0]
 *
 * Returns the number of (idx,value) pairs decoded into out_indices/
 * out_values (≤ k), or -1 on error. */
int oict_remote_send_brandon_bundle(const uint8_t ip[4], uint16_t port,
                                     uint32_t cut_start, uint32_t dim,
                                     uint32_t kv_dim,
                                     const float *dwa_history,
                                     const float *v_first,
                                     uint32_t *out_indices,
                                     float    *out_values,
                                     uint32_t k)
{
    if (cut_start == 0 || cut_start > OICT_MAX_BRANDON_CUT)        return -1;
    if (dim == 0 || dim % 32 != 0 || dim > OICT_MAX_BRANDON_DIM)   return -1;
    if (kv_dim == 0 || kv_dim % 32 != 0 ||
        kv_dim > OICT_MAX_BRANDON_KV_DIM)                          return -1;
    if (!dwa_history || !v_first)                                  return -1;
    if (k == 0 || k > OICT_MAX_TOPK) k = OICT_LOGITS_K;

    static uint8_t txbuf[OICT_MAX_BRANDON_BYTES];
    uint32_t dwa_qbytes = (cut_start + 1) * (dim / 32) * 34;
    uint32_t v_qbytes   = (kv_dim / 32) * 34;
    uint32_t total      = OICT_BRANDON_PRELUDE + dwa_qbytes + v_qbytes;

    /* Pack prelude. */
    ((uint32_t *)txbuf)[0] = cut_start;
    ((uint32_t *)txbuf)[1] = kv_dim;
    /* Quantize DWA history then v_first inline. quantize_q8_0 takes
     * a float count and writes (count/32)*34 bytes. */
    quantize_q8_0(dwa_history, txbuf + OICT_BRANDON_PRELUDE,
                  (uint64_t)(cut_start + 1) * dim);
    quantize_q8_0(v_first, txbuf + OICT_BRANDON_PRELUDE + dwa_qbytes,
                  kv_dim);

    oict_msg_t hdr = {
        .flavor = OICT_FLAVOR_BRANDON_BUNDLE,
        .encoding = OICT_ENC_Q8_0,
        .flags = 0, .version = OICT_VERSION_V1,
        .task_id = 0, .dim = dim,
    };
    static uint8_t rxbuf[OICT_MAX_TOPK * 4 + OICT_MAX_TOPK * 2 + 64];
    oict_msg_t rsp;
    int n = oict_one_shot(ip, port, RPC_CMD_OSITOA_BRANDON_BUNDLE,
                          &hdr, txbuf, total,
                          &rsp, rxbuf, sizeof(rxbuf));
    if (n < 0) return -1;
    if (rsp.flavor != OICT_FLAVOR_LOGITS) return -1;
    uint32_t got_k = rsp.dim;
    if (got_k == 0 || got_k > OICT_MAX_TOPK) return -1;

    if (rsp.encoding == OICT_ENC_F16) {
        if ((uint32_t)n < got_k * 4 + got_k * 2) return -1;
        for (uint32_t i = 0; i < got_k; i++)
            out_indices[i] = ((uint32_t *)rxbuf)[i];
        const uint16_t *vals = (const uint16_t *)(rxbuf + got_k * 4);
        for (uint32_t i = 0; i < got_k; i++)
            out_values[i] = f16_to_f32(vals[i]);
        return (int)got_k;
    }
    return -1;
}

/* Send tokens to a peer and receive top-k logits. The peer runs a
 * full forward pass over the supplied tokens via its local
 * llama_state and returns (idx, value) pairs for the top-k tokens
 * (default k=64). Receiver dequantizes the Q8_0-packed values.
 *
 * out_indices  → must hold at least k entries
 * out_values   → must hold at least k entries
 * Returns the number of entries received (= k on success), -1 on
 * error. */
int oict_remote_request_logits(const uint8_t ip[4], uint16_t port,
                                const uint32_t *prompt_toks, uint32_t n_prompt,
                                uint32_t *out_indices, float *out_values,
                                uint32_t k)
{
    if (n_prompt == 0 || n_prompt > OICT_MAX_TOKEN_BATCH) return -1;
    if (k == 0 || k > OICT_MAX_TOPK) k = OICT_LOGITS_K;

    oict_msg_t hdr = {
        .flavor = OICT_FLAVOR_TOKENS,
        .encoding = OICT_ENC_U32_TOKENS,
        .flags = 0, .version = OICT_VERSION_V1,
        .task_id = 0, .dim = n_prompt,
    };
    static uint8_t  rxbuf[OICT_MAX_TOPK_BYTES + OICT_MAX_TOPK * 4 + 64];
    oict_msg_t rsp;
    int n = oict_one_shot(ip, port, RPC_CMD_OSITOA_TOKENS,
                           &hdr, prompt_toks, n_prompt * 4,
                           &rsp, rxbuf, sizeof(rxbuf));
    if (n < 0) return -1;
    if (rsp.flavor != OICT_FLAVOR_LOGITS) return -1;
    uint32_t got_k = rsp.dim;
    if (got_k == 0 || got_k > OICT_MAX_TOPK) return -1;

    /* F16 encoding: [got_k × u32 idx][got_k × u16 F16 value] */
    if (rsp.encoding == OICT_ENC_F16) {
        if ((uint32_t)n < got_k * 4 + got_k * 2) return -1;
        for (uint32_t i = 0; i < got_k; i++)
            out_indices[i] = ((uint32_t *)rxbuf)[i];
        const uint16_t *vals = (const uint16_t *)(rxbuf + got_k * 4);
        for (uint32_t i = 0; i < got_k; i++)
            out_values[i] = f16_to_f32(vals[i]);
        return (int)got_k;
    }
    return -1;
}

/* Bench primitive: opens ONE TCP connection, reuses it for `iters`
 * back-to-back hidden-state messages. Phase 1 echo handler returns
 * the same bytes; we measure end-to-end ticks + total bytes. Caller
 * (shell) computes MB/s. */
int oict_bench_hidden(const uint8_t ip[4], uint16_t port,
                       uint32_t hidden_dim, uint32_t iterations,
                       uint64_t *out_ticks, uint64_t *out_bytes)
{
    if (hidden_dim == 0 || hidden_dim % 32 != 0 ||
        hidden_dim > OICT_MAX_HIDDEN_DIM) return -1;

    /* Synthetic source vector — small absmax so Q8_0 stays in range. */
    static float src[OICT_MAX_HIDDEN_DIM];
    for (uint32_t i = 0; i < hidden_dim; i++)
        src[i] = ((float)(i % 257) - 128.0f) * (1.0f / 128.0f);

    static uint8_t qbuf[OICT_MAX_HIDDEN_BYTES];
    static uint8_t rxbuf[OICT_MAX_HIDDEN_BYTES];
    uint32_t qbytes = (hidden_dim / 32) * 34;
    quantize_q8_0(src, qbuf, hidden_dim);

    int conn = net_tcp_connect(ip, port, ic_next_ephemeral());
    if (conn < 0) return -1;

    uint64_t t0 = idt_get_ticks();
    uint64_t total_bytes = 0;

    for (uint32_t i = 0; i < iterations; i++) {
        oict_msg_t hdr = {
            .flavor = OICT_FLAVOR_HIDDEN,
            .encoding = OICT_ENC_Q8_0,
            .flags = 0, .version = OICT_VERSION_V1,
            .task_id = i, .dim = hidden_dim,
        };
        uint8_t  cmd = RPC_CMD_OSITOA_HIDDEN;
        uint64_t in_size = sizeof(hdr) + qbytes;
        if (net_tcp_send(conn, &cmd, 1) < 0)            goto fail;
        if (net_tcp_send(conn, &in_size, 8) < 0)        goto fail;
        if (net_tcp_send(conn, &hdr, sizeof(hdr)) < 0)  goto fail;
        if (net_tcp_send(conn, qbuf, qbytes) < 0)       goto fail;

        uint64_t out_size;
        oict_msg_t rsp;
        if (recv_exact(conn, &out_size, 8) < 0)         goto fail;
        if (recv_exact(conn, &rsp, sizeof(rsp)) < 0)    goto fail;
        uint64_t pl = out_size - sizeof(rsp);
        if (pl != qbytes) goto fail;
        if (recv_exact(conn, rxbuf, (uint32_t)pl) < 0)  goto fail;

        total_bytes += in_size + out_size + 9;  /* + outer envelope */
    }

    uint64_t t1 = idt_get_ticks();
    net_tcp_close(conn);
    if (out_ticks) *out_ticks = t1 - t0;
    if (out_bytes) *out_bytes = total_bytes;
    return 0;
fail:
    net_tcp_close(conn);
    return -1;
}
