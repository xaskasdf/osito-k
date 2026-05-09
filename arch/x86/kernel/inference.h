/*
 * OsitoK x86-64 — Llama Inference Runtime
 *
 * Transformer forward pass for Llama 3.2 1B.
 * Orchestrates tensor.h primitives (matvec, rmsnorm, softmax, SiLU, RoPE)
 * over GGUF model data loaded by gguf.h.
 *
 * Architecture: dim=2048, 16 layers, 32 heads, 8 KV heads (GQA 4:1),
 * head_dim=64, ffn_dim=8192, vocab=128256, rope_theta=500000.0
 */

#ifndef OSITOK_INFERENCE_H
#define OSITOK_INFERENCE_H

#include "../include/types.h"
#include "../fs/gguf.h"

/* ── Per-layer weight pointers (resolved from GGUF) ────────── */

typedef struct {
    gguf_tensor_t *attn_norm;       /* [dim], F32 */
    gguf_tensor_t *attn_q;          /* [dim, dim], Q4_0 */
    gguf_tensor_t *attn_k;          /* [kv_dim, dim], Q4_0 */
    gguf_tensor_t *attn_v;          /* [kv_dim, dim], Q4_0 */
    gguf_tensor_t *attn_output;     /* [dim, dim], Q4_0 */
    gguf_tensor_t *ffn_norm;        /* [dim], F32 */
    gguf_tensor_t *ffn_gate;        /* [ffn_dim, dim], Q4_0 */
    gguf_tensor_t *ffn_up;          /* [ffn_dim, dim], Q4_0 */
    gguf_tensor_t *ffn_down;        /* [dim, ffn_dim], Q4_0 */
} llama_layer_t;

typedef struct {
    gguf_tensor_t *token_embd;      /* [vocab, dim] */
    gguf_tensor_t *output_norm;     /* [dim], F32 */
    gguf_tensor_t *output;          /* [vocab, dim] */
    llama_layer_t *layers;          /* [n_layers] */
} llama_weights_t;

/* ── KV cache per layer ────────────────────────────────────── */

typedef struct {
    float *k;       /* [max_seq, kv_dim] */
    float *v;       /* [max_seq, kv_dim] */
} llama_kv_layer_t;

/* ── Full inference state ──────────────────────────────────── */

typedef struct {
    gguf_model_t *model;

    /* Architecture */
    uint32_t dim, n_layers, n_heads, n_kv_heads;
    uint32_t head_dim, kv_dim, ffn_dim, vocab_size, max_seq;
    uint32_t gqa_ratio;     /* n_heads / n_kv_heads */
    float    rope_freq_base; /* RoPE theta (10000 = default, 500000 = Llama 3) */
    float    rms_eps;        /* RMSNorm epsilon (1e-5 default, 1e-6 Llama 3) */

    /* Architecture dispatch — "llama" (default) or "brandon" (block-shared
     * TinyLlama variant w/ DenseFormer DWA + Value Residual + register
     * tokens — see ~/osito-a-models/docs/brandon-arch-spec.md). For
     * "brandon", n_layers holds the *logical* layer count (compute_layer_count)
     * and weights.layers[L] is aliased via layer_map into one of the
     * n_unique_blocks unique stored blocks. */
    char     arch[GGUF_ARCH_LEN];
    uint32_t  n_unique_blocks;       /* brandon: distinct stored blocks */
    uint32_t *layer_map;             /* [n_layers] → block index; NULL for llama */
    bool      use_dwa;
    bool      use_value_residual;
    uint32_t  n_registers;
    bool      registers_prefilled;
    bool      v_first_captured;
    float    *v_first;               /* [kv_dim] when use_value_residual */
    float    *dwa_buf;               /* [(n_layers+1) * dim] when use_dwa */
    gguf_tensor_t *dwa_weights;      /* [n_layers, n_layers+1] F32 */
    gguf_tensor_t *register_weights; /* [n_registers, dim] */

    /* Resolved weights */
    llama_weights_t weights;

    /* KV cache */
    llama_kv_layer_t *kv_cache;     /* [n_layers] */

    /* Scratch buffers (single allocation, partitioned) */
    float *scratch;     /* base pointer for free */
    float *x;           /* [dim] current activation */
    float *xb;          /* [dim] after rmsnorm / attention output */
    float *xb2;         /* [dim] multi-head concat output */
    float *q;           /* [dim] query */
    float *k;           /* [kv_dim] key (current step) */
    float *v;           /* [kv_dim] value (current step) */
    float *att;         /* [max_seq] attention scores (BSP) */
    float *hb;          /* [ffn_dim] gate */
    float *hb2;         /* [ffn_dim] up */
    float *logits;      /* [vocab_size] output */

    /* Per-AP scratch for parallel attention heads */
    #define LLAMA_MAX_AP_SCRATCH 4
    float *att_scratch[LLAMA_MAX_AP_SCRATCH]; /* [max_seq] each, NULL if unused */

    uint32_t pos;       /* Current sequence position */

    /* NVMe-direct layer streaming (optional — NULL if all weights in RAM) */
    tensor_dma_map_t *dma_map;
    void *layer_buf[2];            /* Ping-pong layer weight buffers */
    uint64_t layer_buf_size;       /* Bytes per layer buffer */
    uint32_t layer_tensor_start[256]; /* First tensor index per layer */
} llama_state_t;

/* ── Public API ────────────────────────────────────────────── */

int  llama_init(llama_state_t *state, gguf_model_t *model, uint32_t max_seq);
int  llama_forward(llama_state_t *state, uint32_t token);
int  brandon_forward(llama_state_t *state, uint32_t token);
void llama_generate(llama_state_t *state, const uint32_t *prompt,
                    uint32_t prompt_len, uint32_t max_tokens);
void llama_free(llama_state_t *state);

/* RAG-style chat: wraps system_text + user_text in a ChatML prompt
 * (system + user + assistant turns) and runs prefill + generation.
 * Falls back to llama_chat behavior when system_text is NULL/empty. */
int  llama_chat_with_system(llama_state_t *state,
                             const char *system_text,
                             const char *user_text,
                             uint32_t max_tokens,
                             void (*on_token)(const char *text, void *ctx),
                             void *ctx);

/* NVMe-direct layer streaming */
int  llama_init_streaming(llama_state_t *state);
int  llama_forward_streaming(llama_state_t *state, uint32_t token);

/* ── Speculative token execution ──────────────────────────── */

#define SPEC_MAX_LAYERS  4

typedef struct {
    float *shadow_k[SPEC_MAX_LAYERS];  /* [kv_dim] per speculated layer */
    float *shadow_v[SPEC_MAX_LAYERS];
    float *spec_x;                     /* [dim] speculative activation */
    float *spec_xb, *spec_xb2;        /* [dim] scratch */
    float *spec_q, *spec_k, *spec_v;  /* [dim], [kv_dim], [kv_dim] */
    float *spec_att;                   /* [max_seq] */
    float *spec_hb, *spec_hb2;        /* [ffn_dim] */
    uint32_t predicted_token;
    uint32_t spec_layers_done;
    uint32_t spec_pos;
    volatile int spec_complete;        /* 0=running, 1=done, -1=cancelled */
    uint64_t hits, misses;
} spec_state_t;

int  llama_spec_init(llama_state_t *state);

/* ── KV Cache checkpoint/restore ──────────────────────────── */

#define LLAMA_CKPT_MAGIC  0x4F534B4C  /* "OSKL" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t pos;
    uint32_t n_layers;
    uint32_t kv_dim;
    uint32_t max_seq;
} llama_ckpt_header_t;

int  llama_checkpoint_kv(llama_state_t *state, const char *filename);
int  llama_restore_kv(llama_state_t *state, const char *filename);

#endif /* OSITOK_INFERENCE_H */
