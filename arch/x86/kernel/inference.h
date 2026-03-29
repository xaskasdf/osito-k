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
    float *att;         /* [max_seq] attention scores */
    float *hb;          /* [ffn_dim] gate */
    float *hb2;         /* [ffn_dim] up */
    float *logits;      /* [vocab_size] output */

    uint32_t pos;       /* Current sequence position */
} llama_state_t;

/* ── Public API ────────────────────────────────────────────── */

int  llama_init(llama_state_t *state, gguf_model_t *model, uint32_t max_seq);
int  llama_forward(llama_state_t *state, uint32_t token);
void llama_generate(llama_state_t *state, const uint32_t *prompt,
                    uint32_t prompt_len, uint32_t max_tokens);
void llama_free(llama_state_t *state);

#endif /* OSITOK_INFERENCE_H */
