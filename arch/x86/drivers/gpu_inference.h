/*
 * OsitoK x86-64 — GPU-Accelerated Llama Inference (X40 + X-INF1/INF2)
 *
 * Orchestration layer for hybrid GPU/CPU forward pass.
 *
 * Two modes:
 *   VRAM-resident (X-INF2): Activation vectors stay in VRAM between ops.
 *     Only weights are uploaded per-dispatch, activations persist.
 *     Requires all core kernels available (matvec, rmsnorm, silu_mul, rope).
 *     CPU attention loop downloads q/k/v, uploads xb2 output.
 *
 *   Host-dispatch (X-INF1): Per-op upload/dispatch/download with CPU fallback.
 *     Works with any subset of GPU kernels. Slower due to PRAMIN round trips.
 *
 * Weights stay in system RAM (Q4_0, too large for VRAM).
 * KV cache stays in system RAM (grows with sequence length).
 */

#ifndef OSITOK_GPU_INFERENCE_H
#define OSITOK_GPU_INFERENCE_H

#include "../include/types.h"
#include "../kernel/inference.h"

/* ── GPU Kernel Availability ──────────────────────────── */

typedef struct {
    bool vec_add;           /* vec_add_f32 (small vectors) */
    bool vec_mul;           /* vec_mul_f32 */
    bool matvec_q4_0;      /* Q4_0 dequant + dot product */
    bool rmsnorm;           /* parallel reduction + normalize */
    bool softmax;           /* max + exp + sum + divide */
    bool silu;              /* SiLU activation */
    bool rope;              /* Rotary position embedding */
    bool add_inplace;       /* a[i] += b[i] */
} gpu_kernel_table_t;

/* ── VRAM-Resident Activation Buffers (X-INF2) ──────── */

typedef struct {
    uint64_t x;      /* [dim] main hidden state — persists across layers */
    uint64_t xb;     /* [dim] post-norm scratch */
    uint64_t xb2;    /* [dim] attention output */
    uint64_t q;      /* [dim] query projection */
    uint64_t k;      /* [kv_dim] key projection */
    uint64_t v;      /* [kv_dim] value projection */
    uint64_t hb;     /* [ffn_dim] FFN gate output */
    uint64_t hb2;    /* [ffn_dim] FFN up output */
} gpu_vram_acts_t;

/* ── GPU Inference State ──────────────────────────────── */

typedef struct {
    llama_state_t      *cpu;        /* CPU inference state (weights, KV, scratch) */

    /* VRAM scratch for host-dispatch mode (X-INF1) */
    uint64_t            vram_a;     /* Input buffer A */
    uint64_t            vram_b;     /* Input buffer B */
    uint64_t            vram_out;   /* Output buffer */
    uint32_t            vram_buf_size; /* Size of each buffer in bytes */

    /* VRAM-resident activations (X-INF2) */
    gpu_vram_acts_t     acts;       /* Persistent activation buffers */
    bool                vram_resident; /* true = VRAM path, false = host path */

    /* Dispatch table */
    gpu_kernel_table_t  kernels;

    /* Stats */
    uint32_t            gpu_ops;    /* Operations dispatched to GPU */
    uint32_t            cpu_ops;    /* Operations that used CPU fallback */
    uint64_t            gpu_cycles; /* Total GPU dispatch cycles (rdtsc) */
    uint64_t            cpu_cycles; /* Total CPU compute cycles */

    bool                initialized;
} gpu_llama_state_t;

/* ── Public API ───────────────────────────────────────── */

/* Initialize GPU inference: detect kernels, allocate VRAM scratch */
int  gpu_llama_init(gpu_llama_state_t *gs, llama_state_t *cpu_state);

/* GPU-accelerated forward pass (one token) */
int  gpu_llama_forward(gpu_llama_state_t *gs, uint32_t token);

/* Generate tokens with GPU acceleration */
void gpu_llama_generate(gpu_llama_state_t *gs, const uint32_t *prompt,
                        uint32_t prompt_len, uint32_t max_tokens);

/* Print dispatch stats */
void gpu_llama_stats(gpu_llama_state_t *gs);

/* Benchmark: compare GPU vs CPU for available operations */
int  gpu_llama_benchmark(gpu_llama_state_t *gs);

/* Free GPU inference state (VRAM scratch) */
void gpu_llama_free(gpu_llama_state_t *gs);

#endif /* OSITOK_GPU_INFERENCE_H */
