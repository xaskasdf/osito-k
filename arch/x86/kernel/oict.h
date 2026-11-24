/*
 * arch/x86/kernel/oict.h — OsitoA Inference Channel Transport
 *
 * Layered on top of the existing inferconnect RPC framing
 * (`[u8 cmd | u64 size | bytes]`). Three new opcodes in the
 * OsitoA-native >=200 range so we never collide with ggml-rpc:
 *
 *   201  RPC_CMD_OSITOA_TOKENS  (A → B, prompt tokens)
 *   202  RPC_CMD_OSITOA_LOGITS  (B → A, sampled top-k logits)
 *   203  RPC_CMD_OSITOA_HIDDEN  (A → B, layer-boundary handoff)
 *
 * Each opcode's payload starts with the 12-byte oict_msg header.
 * Compression strategy lives in the `encoding` field — see the
 * OICT_ENC_* enums below. v1 is single-frame (no chunking); the
 * `flags` byte reserves bit 0 for a future chunked extension.
 */

#ifndef OSITOA_OICT_H
#define OSITOA_OICT_H

#include "../include/types.h"

/* Opcodes routed by inferconnect_rpc.c::ic_handle_connection. */
#define RPC_CMD_OSITOA_TOKENS           201
#define RPC_CMD_OSITOA_LOGITS           202
#define RPC_CMD_OSITOA_HIDDEN           203
#define RPC_CMD_OSITOA_BRANDON_BUNDLE   204   /* brandon DWA + v_first cut */

/* OICT v1 wire constants. */
#define OICT_VERSION_V1             1
#define OICT_LOGITS_K               64    /* default top-k for logits */

/* flavor field — what semantic payload follows the header. */
enum {
    OICT_FLAVOR_TOKENS         = 0,
    OICT_FLAVOR_LOGITS         = 1,
    OICT_FLAVOR_HIDDEN         = 2,
    OICT_FLAVOR_KV_DELTA       = 3,    /* reserved, phase 4+ */
    OICT_FLAVOR_BRANDON_BUNDLE = 4,    /* DWA history + v_first */
    OICT_FLAVOR_ERROR          = 0xFF, /* server → client, recoverable fault */
};

/* encoding field — how the payload bytes are laid out. */
enum {
    OICT_ENC_RAW_F32    = 0,    /* dim × float32, big and lossless */
    OICT_ENC_F16        = 1,    /* dim × float16, 2× over F32 */
    OICT_ENC_Q8_0       = 2,    /* ceil(dim/32) blocks of 34 B (F16 scale + 32 i8) */
    OICT_ENC_TOPK_Q8_0  = 3,    /* k × (u32 idx + i8 q + F16 block scale) */
    OICT_ENC_U32_TOKENS = 4,    /* dim × uint32 token IDs */
};

/* flags bits — reserved for v2. v1 must send flags=0 *unless* the
 * payload is followed by an HMAC-SHA-256 tag (32 bytes), in which
 * case OICT_FLAG_HMAC must be set. The tag is computed over the
 * concatenation of [oict_msg_t (12 B) || payload]; the receiver
 * recomputes with its shared key and rejects on mismatch. The 32-
 * byte tag is NOT part of the payload size carried in the
 * surrounding RPC envelope's u64 — wait, scratch that. The tag IS
 * part of the body bytes the broker forwards, so the receiver
 * deducts 32 bytes off the body length before treating the rest
 * as the OICT payload. See oict_ws_client.c + inferconnect_oict.c. */
#define OICT_FLAG_CHUNKED   0x01
#define OICT_FLAG_HMAC      0x02

/* 12-byte packed header. All fields little-endian (host order on x86). */
typedef struct __attribute__((packed)) {
    uint8_t  flavor;
    uint8_t  encoding;
    uint8_t  flags;
    uint8_t  version;
    uint32_t task_id;
    uint32_t dim;       /* element count, OR k for top-k logits */
} oict_msg_t;

_Static_assert(sizeof(oict_msg_t) == 12, "oict_msg_t must be 12 bytes");

/* Worst-case payloads — bounds checked by inferconnect_rpc.c against
 * IC_RPC_FRAME_MAX so we fail loud at boot if the model outgrows the
 * frame budget. Numbers come from the plan's payload table. */
#define OICT_MAX_TOKEN_BATCH    256                          /* tokens / message */
#define OICT_MAX_HIDDEN_DIM     2048                         /* Llama 3.2-1B hidden */
#define OICT_MAX_HIDDEN_BYTES   (((OICT_MAX_HIDDEN_DIM + 31) / 32) * 34)
#define OICT_MAX_TOPK           128                          /* upper bound on k */
#define OICT_MAX_TOPK_BYTES     (OICT_MAX_TOPK * (4 + 1) + 2 /* F16 scale */)

/* Brandon-tiny bundle bounds. Wire payload at the cut:
 *
 *   header  (12 B)
 *   uint32  cut_start                     ← number of layer-vectors that follow
 *   uint32  kv_dim                        ← length of the v_first slice
 *   (cut_start+1) × dim   floats Q8_0     ← slot->dwa_buf[0..cut_start]
 *   kv_dim                floats Q8_0     ← slot->v_first
 *
 * Worst case sized so the framed payload + RPC envelope (9 B) fits
 * inside IC_RPC_FRAME_MAX = 16384.
 *
 *   (32+1) × 384 × 34/32 = 13464   bundle bytes for cut=32, dim=384
 *   (24+1) × 512 × 34/32 = 13600   bundle bytes for cut=24, dim=512
 *
 * Pick (CUT=24, DIM=512) so brandon-tiny variants up to ~30M params
 * fit. The static_assert in inferconnect_oict.c enforces the budget. */
#define OICT_MAX_BRANDON_DIM    512
#define OICT_MAX_BRANDON_KV_DIM OICT_MAX_BRANDON_DIM
#define OICT_MAX_BRANDON_CUT    24
#define OICT_BRANDON_PRELUDE    (4 + 4)    /* cut_start + kv_dim u32s */
#define OICT_MAX_BRANDON_BYTES  ( OICT_BRANDON_PRELUDE                                   \
                                + ((OICT_MAX_BRANDON_CUT + 1)                            \
                                   * ((OICT_MAX_BRANDON_DIM + 31) / 32) * 34)            \
                                + ((OICT_MAX_BRANDON_KV_DIM + 31) / 32) * 34 )

/* Server-side handlers (inferconnect_oict.c). Called from the
 * dispatch switch in inferconnect_rpc.c::ic_handle_connection.
 * Each receives the body length already validated by the outer
 * frame-max check; reads the payload via ic_recv_exact, processes,
 * sends a framed response back on the same conn.
 *
 * Returns 0 to keep the connection open, -1 to close. */
int oict_handle_tokens         (int conn, uint64_t in_size);
int oict_handle_logits         (int conn, uint64_t in_size);
int oict_handle_hidden         (int conn, uint64_t in_size);
int oict_handle_brandon_bundle (int conn, uint64_t in_size);

/* Pure-buffer dispatcher for opcode 204. Takes the inbound bytes
 * (oict_msg_t header + brandon prelude + Q8_0 payload) and produces
 * a response (oict_msg_t header + top-k F16 logits) in `out_buf`.
 *
 * Used by both the TCP RPC path (oict_handle_brandon_bundle wraps
 * this) and the WebSocket path (kthread reads frame, calls this,
 * writes reply via ws_oict_send_direct).
 *
 * Returns the response byte count on success, -1 on any error. */
int oict_dispatch_brandon_bundle(const uint8_t *in_buf, uint32_t in_len,
                                  uint8_t *out_buf, uint32_t out_cap);

/* Read the shared 32-byte OICT auth key into `out`. The kernel
 * looks for `oict-key.txt` in OsitoFS (32 raw bytes; >32 truncates,
 * <32 zero-pads). Returns the same 32-byte zero key on every kernel
 * if no sentinel is present — works for self-test but provides no
 * authentication. Set a real shared key via:
 *     head -c 32 /dev/urandom > /tmp/oict-key.txt
 *     ositofs-write build/nvme.img /tmp/oict-key.txt
 * on each kernel that should accept the others' bundles. */
void oict_get_shared_key(uint8_t out_key[32]);

/* Client-side helpers (inferconnect_client.c). Each opens a fresh
 * outbound connection (or reuses an existing one — TBD), sends one
 * OICT-framed message, awaits a single response, returns the
 * received payload size on success or -1 on error. */
int oict_remote_send_tokens   (const uint8_t ip[4], uint16_t port,
                                const uint32_t *toks, uint32_t n_tok,
                                uint8_t *out_buf, uint32_t out_cap);
int oict_remote_send_hidden   (const uint8_t ip[4], uint16_t port,
                                const float *hidden, uint32_t dim,
                                uint8_t *out_buf, uint32_t out_cap);
int oict_remote_request_logits(const uint8_t ip[4], uint16_t port,
                                const uint32_t *prompt_toks, uint32_t n_prompt,
                                uint32_t *out_indices, float *out_values,
                                uint32_t k);

/* Brandon-tiny pipeline-parallel cut. Sender ships its slot's
 * DWA history (cut_start+1 layer vectors of length dim) plus the
 * value-residual seed (kv_dim) Q8_0-quantized. Receiver dequantizes,
 * resumes layers [cut_start, n_layers) via brandon_forward_range_async,
 * returns top-k F16 logits as opcode 202. Returns the response payload
 * size on success or -1 on error. */
int oict_remote_send_brandon_bundle(const uint8_t ip[4], uint16_t port,
                                     uint32_t cut_start, uint32_t dim,
                                     uint32_t kv_dim,
                                     const float *dwa_history,
                                     const float *v_first,
                                     uint32_t *out_indices,
                                     float    *out_values,
                                     uint32_t k);

/* Bench primitive: opens one TCP conn, sends N×hidden Q8_0 messages,
 * polls echo responses, returns elapsed ticks (caller computes
 * throughput). Used by `agent inferconnect bench`. */
int oict_bench_hidden(const uint8_t ip[4], uint16_t port,
                       uint32_t hidden_dim, uint32_t iterations,
                       uint64_t *out_ticks, uint64_t *out_bytes);

#endif /* OSITOA_OICT_H */
