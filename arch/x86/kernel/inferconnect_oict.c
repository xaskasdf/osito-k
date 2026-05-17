/*
 * arch/x86/kernel/inferconnect_oict.c — OICT server handlers
 *
 * Phase 1: echo handlers that round-trip the payload bytes-for-bytes
 * back to the client. Validates the wire format, frame-size bump, and
 * Q8_0 quantize fidelity end-to-end without touching the inference
 * engine. Phase 2/3 swap these handlers for real inference paths
 * (forward-pass driver for tokens/logits, layer-range driver for
 * hidden-state handoff).
 *
 * Wire framing (all little-endian, host order on x86):
 *   [u8 cmd | u64 size | bytes]                         ← outer RPC envelope
 *      bytes = [oict_msg_t (12 B) | payload]
 *
 * Response framing mirrors request:
 *   [u64 size | bytes]                                  ← outer RPC envelope
 *      bytes = [oict_msg_t (12 B) | payload]
 *
 * On unknown encoding / version the handler drains the body to keep
 * framing aligned, sends an OICT_FLAVOR_ERROR reply with empty
 * payload, and keeps the connection open (recoverable application
 * fault, not a framing fault).
 */

#include "../include/types.h"
#include "oict.h"
#include "../fs/vfs.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

/* From inferconnect_rpc.c — same socket helpers used by the existing
 * handlers. Declared static there; we re-declare matching wrappers
 * locally so we can call them. */
extern int  ic_recv_exact(int conn, void *buf, uint32_t n);
extern int  net_tcp_send(int conn, const void *data, uint32_t len);

/* Compile-time sanity: every planned single-frame payload must fit. */
_Static_assert(sizeof(oict_msg_t) + OICT_MAX_HIDDEN_BYTES + 9 <= 16384,
               "Llama-1B hidden Q8_0 must fit in IC_RPC_FRAME_MAX");
_Static_assert(sizeof(oict_msg_t) + OICT_MAX_TOPK_BYTES   + 9 <= 16384,
               "Top-k logits must fit in IC_RPC_FRAME_MAX");
_Static_assert(sizeof(oict_msg_t) + OICT_MAX_TOKEN_BATCH * 4 + 9 <= 16384,
               "Token batch must fit in IC_RPC_FRAME_MAX");
_Static_assert(sizeof(oict_msg_t) + OICT_MAX_BRANDON_BYTES + 9 <= 16384,
               "Brandon bundle must fit in IC_RPC_FRAME_MAX");

/* Drain N bytes from `conn` into a scratch buffer (recv-side trash).
 * Used when validation rejects a payload but we want to keep the
 * stream framing aligned so subsequent OICT messages still parse. */
static int oict_drain(int conn, uint64_t n)
{
    static uint8_t scratch[1024];
    while (n > 0) {
        uint32_t want = n > sizeof(scratch) ? (uint32_t)sizeof(scratch) : (uint32_t)n;
        if (ic_recv_exact(conn, scratch, want) < 0) return -1;
        n -= (uint64_t)want;
    }
    return 0;
}

/* Send a framed OICT response: [u64 total_size][oict_msg][payload]. */
static int oict_send_response(int conn, const oict_msg_t *hdr,
                               const void *payload, uint32_t payload_len)
{
    uint64_t total = sizeof(*hdr) + payload_len;
    if (net_tcp_send(conn, &total, 8) < 0) return -1;
    if (net_tcp_send(conn, hdr, sizeof(*hdr)) < 0) return -1;
    if (payload_len && net_tcp_send(conn, payload, payload_len) < 0)
        return -1;
    return 0;
}

/* Send an OICT_FLAVOR_ERROR sentinel with zero payload. Caller has
 * already drained the malformed body. Connection stays open. */
static int oict_send_error(int conn, uint32_t task_id)
{
    oict_msg_t err = {
        .flavor = OICT_FLAVOR_ERROR,
        .encoding = 0,
        .flags = 0,
        .version = OICT_VERSION_V1,
        .task_id = task_id,
        .dim = 0,
    };
    return oict_send_response(conn, &err, NULL, 0);
}

/* ── Common header validation ─────────────────────────────────── */

static int oict_recv_and_validate(int conn, uint64_t in_size,
                                   oict_msg_t *out_hdr,
                                   uint8_t *out_buf, uint32_t out_cap)
{
    if (in_size < sizeof(oict_msg_t)) return -1;

    if (ic_recv_exact(conn, out_hdr, sizeof(*out_hdr)) < 0) return -1;
    if (out_hdr->version != OICT_VERSION_V1) {
        serial_puts("[OICT] bad version\n");
        oict_drain(conn, in_size - sizeof(*out_hdr));
        oict_send_error(conn, out_hdr->task_id);
        return -1;
    }
    if (out_hdr->flags & ~OICT_FLAG_CHUNKED) {
        serial_puts("[OICT] reserved flag bits set\n");
        oict_drain(conn, in_size - sizeof(*out_hdr));
        oict_send_error(conn, out_hdr->task_id);
        return -1;
    }
    if (out_hdr->flags & OICT_FLAG_CHUNKED) {
        /* v1 doesn't implement chunked yet. Reject explicitly. */
        serial_puts("[OICT] chunked not supported in v1\n");
        oict_drain(conn, in_size - sizeof(*out_hdr));
        oict_send_error(conn, out_hdr->task_id);
        return -1;
    }
    uint64_t payload = in_size - sizeof(*out_hdr);
    if (payload > out_cap) {
        serial_puts("[OICT] payload exceeds buffer\n");
        oict_drain(conn, payload);
        oict_send_error(conn, out_hdr->task_id);
        return -1;
    }
    if (payload && ic_recv_exact(conn, out_buf, (uint32_t)payload) < 0)
        return -1;
    return (int)payload;
}

/* ── Phase 1 echo handlers ────────────────────────────────────── */

/* External hooks into the inference engine for Phase 2 forward + sample. */
extern void *prompt_llama;
extern int   llama_forward_slot(void *s, uint32_t slot_id, uint32_t token);
extern uint64_t llama_forward_async(void *s, uint32_t slot_id, uint32_t token);
extern void     llama_request_wait(void *s, uint64_t seq);
extern int   llama_forward_slot_range(void *s, uint32_t slot_id, uint32_t tok,
                                       uint32_t layer_start, uint32_t layer_end,
                                       const float *hidden_in, float *hidden_out);
extern uint64_t llama_forward_range_async(void *s, uint32_t slot_id,
                                          uint32_t token,
                                          uint32_t layer_start, uint32_t layer_end,
                                          const float *hidden_in,
                                          float *hidden_out);
extern uint32_t llama_state_n_layers(void *s);
extern uint32_t llama_state_dim(void *s);
extern uint32_t llama_state_kv_dim(void *s);
extern char    *llama_state_arch(void *s);
extern bool     llama_state_use_dwa(void *s);
extern bool     llama_state_use_value_residual(void *s);
extern float *llama_state_logits(void *s);
extern uint32_t llama_state_vocab_size(void *s);
#define llama_state_vocab(s) llama_state_vocab_size(s)
extern uint32_t llama_slot_pos(void *s, uint32_t slot_id);
extern void     llama_slot_set_pos(void *s, uint32_t slot_id, uint32_t pos);
extern uint64_t brandon_forward_range_async(void *s, uint32_t slot_id,
                                             uint32_t token,
                                             uint32_t cut_start, uint32_t cut_end,
                                             const float *dwa_history,
                                             const float *v_first);

/* From tensor.h (we already declared quantize_q8_0). */
extern void quantize_q8_0(const float *src, void *dst, uint64_t n);

/* Pack top-k entries of `logits` (length vocab) into a wire-format
 * blob using F16 encoding for the values. Layout:
 *   [k × u32 index][k × u16 F16 value]
 *
 * F16 (instead of Q8_0) keeps this self-contained — no per-block
 * scale, no MXCSR-sensitive scaling math, just bit manipulation per
 * value. Q8_0 stays available for the hidden-state path where the
 * shell-context kthread has already exercised it cleanly (Phase 1
 * bench). For the top-k logits returned by the RPC kthread, F16 is
 * easier to reason about and still gets us 2× over F32. */
static uint32_t pack_topk_f16(const float *logits, uint32_t vocab,
                                uint32_t k,
                                uint8_t *out, uint32_t out_cap)
{
    if (k == 0 || k > OICT_MAX_TOPK) k = OICT_LOGITS_K;
    if (vocab == 0) return 0;

    static uint32_t tk_idx[OICT_MAX_TOPK];
    static float    tk_val[OICT_MAX_TOPK];
    for (uint32_t i = 0; i < k; i++) { tk_idx[i] = 0; tk_val[i] = -1e30f; }

    uint32_t min_pos = 0;
    for (uint32_t v = 0; v < vocab; v++) {
        float x = logits[v];
        if (x <= tk_val[min_pos]) continue;
        tk_idx[min_pos] = v;
        tk_val[min_pos] = x;
        float m = tk_val[0]; uint32_t mp = 0;
        for (uint32_t j = 1; j < k; j++)
            if (tk_val[j] < m) { m = tk_val[j]; mp = j; }
        min_pos = mp;
    }

    uint32_t need = k * 4 + k * 2;
    if (out_cap < need) return 0;
    uint8_t *p = out;
    for (uint32_t i = 0; i < k; i++) {
        *(uint32_t *)p = tk_idx[i];
        p += 4;
    }
    extern uint16_t f32_to_f16(float f);
    for (uint32_t i = 0; i < k; i++) {
        *(uint16_t *)p = f32_to_f16(tk_val[i]);
        p += 2;
    }
    return need;
}

int oict_handle_tokens(int conn, uint64_t in_size)
{
    static uint8_t  buf[OICT_MAX_TOKEN_BATCH * 4];
    static uint8_t  rsp_buf[OICT_MAX_TOPK_BYTES + OICT_MAX_TOPK * 4 + 64];
    oict_msg_t hdr;
    int n = oict_recv_and_validate(conn, in_size, &hdr, buf, sizeof(buf));
    if (n < 0) return 0;

    if (hdr.encoding != OICT_ENC_U32_TOKENS || hdr.flavor != OICT_FLAVOR_TOKENS) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }
    if (hdr.dim == 0 || hdr.dim * 4 != (uint32_t)n) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }

    serial_puts("[OICT] forward request: ");
    serial_putdec((uint64_t)hdr.dim);
    serial_puts(" tokens task=");
    serial_putdec((uint64_t)hdr.task_id);
    serial_puts("\n");

    if (!prompt_llama) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }

    /* Real forward path via the AP worker's MPSC ring (same code
     * path the agent kthreads use for inference). Borrows slot 0
     * with snapshot/restore around the call so concurrent agent
     * users aren't disturbed. */
    const uint32_t slot_id   = 0;
    uint32_t       saved_pos = llama_slot_pos(prompt_llama, slot_id);
    const uint32_t *toks = (const uint32_t *)buf;

    serial_puts("[OICT]   forward "); serial_putdec((uint64_t)hdr.dim);
    serial_puts(" tokens via AP worker\n");

    uint64_t last_seq = 0;
    for (uint32_t i = 0; i < hdr.dim; i++) {
        uint64_t seq = llama_forward_async(prompt_llama, slot_id, toks[i]);
        if (seq) last_seq = seq;
        else {
            llama_slot_set_pos(prompt_llama, slot_id, saved_pos);
            oict_send_error(conn, hdr.task_id);
            return 0;
        }
    }
    if (last_seq) llama_request_wait(prompt_llama, last_seq);

    float   *logits = llama_state_logits(prompt_llama);
    uint32_t vocab  = llama_state_vocab(prompt_llama);
    llama_slot_set_pos(prompt_llama, slot_id, saved_pos);

    if (!logits || vocab == 0) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }
    serial_puts("[OICT]   forward done, packing top-k vocab=");
    serial_putdec((uint64_t)vocab); serial_puts("\n");
    uint32_t k    = OICT_LOGITS_K;
    uint32_t plen = pack_topk_f16(logits, vocab, k,
                                    rsp_buf, sizeof(rsp_buf));

    if (plen == 0) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }

    serial_puts("[OICT] forward done, top-");
    serial_putdec((uint64_t)k);
    serial_puts(" packed in ");
    serial_putdec((uint64_t)plen);
    serial_puts(" B\n");

    oict_msg_t rsp = {
        .flavor = OICT_FLAVOR_LOGITS,
        .encoding = OICT_ENC_F16,    /* k×u32 idx + k×u16 F16 value */
        .flags = 0, .version = OICT_VERSION_V1,
        .task_id = hdr.task_id, .dim = k,
    };
    return oict_send_response(conn, &rsp, rsp_buf, plen);
}

int oict_handle_logits(int conn, uint64_t in_size)
{
    static uint8_t buf[OICT_MAX_TOPK_BYTES];
    oict_msg_t hdr;
    int n = oict_recv_and_validate(conn, in_size, &hdr, buf, sizeof(buf));
    if (n < 0) return 0;

    serial_puts("[OICT] logits echo: k=");
    serial_putdec((uint64_t)hdr.dim);
    serial_puts("\n");

    return oict_send_response(conn, &hdr, buf, (uint32_t)n);
}

int oict_handle_hidden(int conn, uint64_t in_size)
{
    static uint8_t buf[OICT_MAX_HIDDEN_BYTES];
    static uint8_t rsp_buf[OICT_MAX_TOPK * 4 + OICT_MAX_TOPK * 2];
    oict_msg_t hdr;
    int n = oict_recv_and_validate(conn, in_size, &hdr, buf, sizeof(buf));
    if (n < 0) return 0;

    /* Phase 3 wire format demo: peer A computes layers [0..mid),
     * Q8_0-quantizes the hidden state, sends here. We dequantize
     * (proves the encoding survives transit), summarize it into
     * a top-k logits response so the coordinator gets a final
     * sampling distribution.
     *
     * The full pipeline-parallel "run layers [mid..N) on dequantized
     * hidden" requires `llama_forward_slot_range` — a refactor of
     * llama_forward_slot to skip embed + skip the [0..mid) loop and
     * start from a provided x[]. That refactor hits the same FPU /
     * slot-state race we saw in Phase 2 (RPC kthread vs agent slot
     * 0). It's a follow-up: the wire format is proven here. */

    if (hdr.encoding != OICT_ENC_Q8_0 || hdr.flavor != OICT_FLAVOR_HIDDEN) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }
    uint32_t dim     = hdr.dim;
    uint32_t qbytes  = (dim / 32) * 34;
    if (dim == 0 || dim % 32 != 0 || qbytes != (uint32_t)n) {
        oict_send_error(conn, hdr.task_id);
        return 0;
    }

    serial_puts("[OICT] hidden recv: dim=");
    serial_putdec((uint64_t)dim);
    serial_puts(" qbytes=");
    serial_putdec((uint64_t)qbytes);
    serial_puts("\n");

    /* Dequantize into a scratch buffer. */
    extern void dequant_q8_0(const void *src, float *dst, uint64_t n);
    static float dequant_scratch[OICT_MAX_HIDDEN_DIM];
    dequant_q8_0(buf, dequant_scratch, dim);

    /* Try the real pipeline-parallel cut: feed the dequantized
     * activation into llama_forward_slot_range starting from the
     * second half of the model, do the LM head, return real top-k
     * logits. Returns -1 on brandon (block_sharing arch — refactor
     * pending), in which case we fall back to deterministic
     * synthesis so the wire format demo still round-trips. */
    uint32_t k    = OICT_LOGITS_K;
    uint32_t plen = 0;

    uint32_t n_layers = llama_state_n_layers(prompt_llama);
    uint32_t mdim     = llama_state_dim(prompt_llama);
    if (n_layers > 0 && mdim == dim) {
        const uint32_t mid     = n_layers / 2;
        const uint32_t slot_id = 0;
        uint32_t saved_pos = llama_slot_pos(prompt_llama, slot_id);
        /* Dispatch through the AP worker (same path as Phase 2's full
         * forward), so the layer-range runs on the dedicated AP CPU
         * with its initialized FPU/MXCSR state — calling synchronously
         * from the BSP RPC kthread hangs on rmsnorm under HVF. */
        uint64_t seq = llama_forward_range_async(prompt_llama, slot_id,
                                                  /*tok unused*/0,
                                                  mid, n_layers,
                                                  dequant_scratch, NULL);
        int rc = -1;
        if (seq) {
            llama_request_wait(prompt_llama, seq);
            rc = 0;
        }
        llama_slot_set_pos(prompt_llama, slot_id, saved_pos);
        if (rc == 0) {
            float   *logits = llama_state_logits(prompt_llama);
            uint32_t vocab  = llama_state_vocab(prompt_llama);
            plen = pack_topk_f16(logits, vocab, k, rsp_buf, sizeof(rsp_buf));
            serial_puts("[OICT] hidden→logits via layer-range, "
                        "vocab="); serial_putdec((uint64_t)vocab);
            serial_puts("\n");
        } else {
            serial_puts("[OICT] layer-range refused (arch), falling back to synthetic\n");
        }
    }

    /* Fallback: deterministic synthesis so brandon-loaded peers can
     * still demonstrate the wire format. Same prompt → same logits. */
    if (plen == 0) {
        static float synthetic_logits[4096];
        uint32_t vocab = 4096;
        for (uint32_t i = 0; i < vocab; i++) {
            uint32_t h = 2166136261u;
            for (uint32_t j = 0; j < dim; j++) {
                extern uint16_t f32_to_f16(float f);
                uint32_t bits = (uint32_t)f32_to_f16(dequant_scratch[j]);
                h ^= bits + i;
                h *= 16777619u;
            }
            int32_t signed_h = (int32_t)h;
            synthetic_logits[i] = (float)(signed_h / 100000);
        }
        plen = pack_topk_f16(synthetic_logits, vocab, k,
                              rsp_buf, sizeof(rsp_buf));
    }
    if (plen == 0) { oict_send_error(conn, hdr.task_id); return 0; }

    serial_puts("[OICT] hidden→logits done, ");
    serial_putdec((uint64_t)plen);
    serial_puts(" B\n");

    oict_msg_t rsp = {
        .flavor = OICT_FLAVOR_LOGITS,
        .encoding = OICT_ENC_F16,
        .flags = 0, .version = OICT_VERSION_V1,
        .task_id = hdr.task_id, .dim = k,
    };
    int rc = oict_send_response(conn, &rsp, rsp_buf, plen);
    if (rc < 0) serial_puts("[OICT] send_response failed\n");
    return rc;
}

/* ── Phase B — brandon-tiny bundle handler (opcode 204) ────────── */

/* Read the shared OICT auth key from OsitoFS sentinel `oict-key.txt`.
 * Zero-fills `out` first so a missing or short file leaves the
 * remainder zero — both peers using zero-key produces a valid
 * (but unauthenticated) HMAC. Real deployments write 32 random
 * bytes via `ositofs-write`. */
void oict_get_shared_key(uint8_t out_key[32])
{
    for (int i = 0; i < 32; i++) out_key[i] = 0;
    vfs_node_t n;
    /* mode=0 = read, matches main.c's osfs_find wrapper. */
    if (!vfs_find("oict-key.txt", 0, &n)) return;
    uint64_t want = n.size < 32 ? n.size : 32;
    if (want) (void)vfs_read(&n, 0, out_key, want);
}

extern void hmac_sha256(const void *key, uint32_t key_len,
                          const void *data, uint32_t data_len,
                          uint8_t mac[32]);

/* Constant-time 32-byte memcmp. */
static int oict_ct_memeq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0 ? 1 : 0;
}

/* Pure-buffer dispatcher: parse a brandon bundle (oict_msg_t header
 * + brandon prelude + Q8_0 payload [+ optional 32-byte HMAC tag])
 * from `in_buf`, run the suffix forward locally, write a top-k F16
 * LOGITS response (with matching HMAC tag if the request had one)
 * to `out_buf`. Used by both the TCP RPC arm and the WS arm. */
int oict_dispatch_brandon_bundle(const uint8_t *in_buf, uint32_t in_len,
                                  uint8_t *out_buf, uint32_t out_cap)
{
    static float dwa_scratch[(OICT_MAX_BRANDON_CUT + 1) * OICT_MAX_BRANDON_DIM];
    static float v_scratch[OICT_MAX_BRANDON_KV_DIM];

    if (!in_buf || !out_buf) return -1;
    if (in_len < sizeof(oict_msg_t) + OICT_BRANDON_PRELUDE) return -1;
    if (out_cap < sizeof(oict_msg_t)) return -1;

    oict_msg_t hdr;
    for (uint32_t i = 0; i < sizeof(hdr); i++) ((uint8_t *)&hdr)[i] = in_buf[i];
    if (hdr.version != OICT_VERSION_V1 ||
        hdr.encoding != OICT_ENC_Q8_0 ||
        hdr.flavor   != OICT_FLAVOR_BRANDON_BUNDLE)
        return -1;

    /* If HMAC flag is set, verify the trailing 32-byte tag over
     * [hdr || body-without-tag] before doing any inference work. */
    bool hmac_in = (hdr.flags & OICT_FLAG_HMAC) != 0;
    if (hmac_in) {
        if (in_len < sizeof(hdr) + OICT_BRANDON_PRELUDE + 32) return -1;
        uint8_t key[32]; oict_get_shared_key(key);
        uint8_t want[32];
        hmac_sha256(key, 32, in_buf, in_len - 32, want);
        if (!oict_ct_memeq(want, in_buf + in_len - 32, 32)) {
            serial_puts("[OICT] brandon bundle HMAC mismatch — refusing\n");
            return -1;
        }
        serial_puts("[OICT] brandon bundle HMAC ok\n");
    }

    const uint8_t *body = in_buf + sizeof(hdr);
    uint32_t       body_len = (in_len - sizeof(hdr)) - (hmac_in ? 32 : 0);

    uint32_t cut_start = ((const uint32_t *)body)[0];
    uint32_t kv_dim    = ((const uint32_t *)body)[1];
    uint32_t dim       = hdr.dim;

    if (dim == 0 || dim % 32 != 0 || dim > OICT_MAX_BRANDON_DIM ||
        kv_dim == 0 || kv_dim % 32 != 0 || kv_dim > OICT_MAX_BRANDON_KV_DIM ||
        cut_start == 0 || cut_start > OICT_MAX_BRANDON_CUT)
        return -1;

    uint32_t dwa_qbytes = (cut_start + 1) * (dim / 32) * 34;
    uint32_t v_qbytes   = (kv_dim / 32) * 34;
    if (body_len != OICT_BRANDON_PRELUDE + dwa_qbytes + v_qbytes) return -1;

    serial_puts("[OICT] brandon bundle dispatch: cut=");
    serial_putdec((uint64_t)cut_start);
    serial_puts(" dim="); serial_putdec((uint64_t)dim);
    serial_puts(" kv_dim="); serial_putdec((uint64_t)kv_dim);
    serial_puts(" body="); serial_putdec((uint64_t)body_len);
    serial_puts("\n");

    if (!prompt_llama) return -1;
    char *arch = llama_state_arch(prompt_llama);
    if (!arch || arch[0] != 'b') return -1;
    uint32_t mdim    = llama_state_dim(prompt_llama);
    uint32_t mkv     = llama_state_kv_dim(prompt_llama);
    uint32_t mlayers = llama_state_n_layers(prompt_llama);
    if (mdim != dim || mkv != kv_dim || cut_start >= mlayers) return -1;

    extern void dequant_q8_0(const void *src, float *dst, uint64_t n);
    const uint8_t *p = body + OICT_BRANDON_PRELUDE;
    dequant_q8_0(p, dwa_scratch, (uint64_t)(cut_start + 1) * dim);
    p += dwa_qbytes;
    dequant_q8_0(p, v_scratch, kv_dim);

    const uint32_t slot_id = 0;
    uint32_t saved_pos = llama_slot_pos(prompt_llama, slot_id);
    /* Try the async AP arm first — keeps the WS handler responsive so
     * drain-polls and TLS retransmits keep flowing while the receiver-
     * side forward runs on AP1. Falls back to sync on the calling
     * thread if the worker isn't alive (engine not pinned) or the
     * dispatch returns the sync-fallback sentinel. The sync fallback
     * preserves the previous behavior; only the async path is new. */
    extern uint64_t brandon_forward_range_async(void *s, uint32_t slot,
                                                 uint32_t token,
                                                 uint32_t cut_s, uint32_t cut_e,
                                                 const float *dwa, const float *vf);
    extern int      brandon_forward_slot_range(void *s, uint32_t slot,
                                                 uint32_t token,
                                                 uint32_t cut_s, uint32_t cut_e,
                                                 const float *dwa, const float *vf);

    int rc = -1;
    uint64_t seq = brandon_forward_range_async(prompt_llama, slot_id, 0,
                                                 cut_start, mlayers,
                                                 dwa_scratch, v_scratch);
    if (seq == 0) {
        /* Ring full — fall back to sync on the calling thread. */
        rc = brandon_forward_slot_range(prompt_llama, slot_id, 0,
                                          cut_start, mlayers,
                                          dwa_scratch, v_scratch);
    } else if (seq == (uint64_t)-1) {
        /* Sync-fallback sentinel from inside async (worker not alive). */
        rc = 0;
    } else {
        llama_request_wait(prompt_llama, seq);
        rc = 0;
    }
    llama_slot_set_pos(prompt_llama, slot_id, saved_pos);
    if (rc != 0) return -1;

    float   *logits = llama_state_logits(prompt_llama);
    uint32_t vocab  = llama_state_vocab(prompt_llama);
    if (!logits || vocab == 0) return -1;

    uint32_t k = OICT_LOGITS_K;
    if (out_cap < sizeof(oict_msg_t) + k * 4 + k * 2) return -1;

    /* Pack response: oict_msg_t header followed by top-k F16
     * (and a 32-byte HMAC tag iff the request was authenticated). */
    oict_msg_t rsp = {
        .flavor = OICT_FLAVOR_LOGITS,
        .encoding = OICT_ENC_F16,
        .flags = (uint8_t)(hmac_in ? OICT_FLAG_HMAC : 0),
        .version = OICT_VERSION_V1,
        .task_id = hdr.task_id, .dim = k,
    };
    if (out_cap < sizeof(rsp) + (hmac_in ? 32u : 0u)) return -1;
    for (uint32_t i = 0; i < sizeof(rsp); i++) out_buf[i] = ((uint8_t *)&rsp)[i];
    uint32_t plen = pack_topk_f16(logits, vocab, k,
                                    out_buf + sizeof(rsp),
                                    out_cap - sizeof(rsp) - (hmac_in ? 32u : 0u));
    if (plen == 0) return -1;
    uint32_t total = (uint32_t)sizeof(rsp) + plen;
    if (hmac_in) {
        uint8_t key[32]; oict_get_shared_key(key);
        hmac_sha256(key, 32, out_buf, total, out_buf + total);
        total += 32;
    }
    return (int)total;
}

int oict_handle_brandon_bundle(int conn, uint64_t in_size)
{
    static uint8_t in_buf[OICT_MAX_BRANDON_BYTES + sizeof(oict_msg_t)];
    static uint8_t out_buf[sizeof(oict_msg_t) + OICT_MAX_TOPK * (4 + 2) + 64];

    if (in_size > sizeof(in_buf)) {
        oict_drain(conn, in_size);
        oict_send_error(conn, 0);
        return 0;
    }
    if (ic_recv_exact(conn, in_buf, (uint32_t)in_size) < 0) return -1;

    int rsp_len = oict_dispatch_brandon_bundle(in_buf, (uint32_t)in_size,
                                                 out_buf, sizeof(out_buf));
    if (rsp_len < 0) {
        /* Best-effort: extract task_id for the error reply. */
        oict_msg_t in_hdr = { 0 };
        if (in_size >= sizeof(in_hdr))
            for (uint32_t i = 0; i < sizeof(in_hdr); i++)
                ((uint8_t *)&in_hdr)[i] = in_buf[i];
        oict_send_error(conn, in_hdr.task_id);
        return 0;
    }

    /* The dispatcher wrote oict_msg_t at out_buf[0..12) and the
     * payload at [12..rsp_len). Wrap with the RPC envelope (u64
     * size) and ship. */
    uint64_t total = (uint64_t)rsp_len;
    if (net_tcp_send(conn, &total, 8) < 0) return -1;
    if (net_tcp_send(conn, out_buf, (uint32_t)rsp_len) < 0) return -1;
    return 0;
}
