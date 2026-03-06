/*
 * OsitoK x86-64 — GPU-Accelerated Llama Inference (X40)
 *
 * Orchestration layer for hybrid GPU/CPU forward pass.
 * Dispatches tensor operations to GPU when SASS kernels are available,
 * falls back to CPU tensor.h primitives otherwise.
 *
 * Current GPU kernels: vec_add_f32 (256 elements max per dispatch)
 * Future kernels:      matvec_q4_0, rmsnorm, softmax, silu, rope, vec_mul
 *
 * Architecture:
 *   - Weights stay in system RAM (too large for VRAM buffer region)
 *   - Activation vectors uploaded to VRAM for GPU ops, downloaded after
 *   - PRAMIN transfer overhead dominates for small vectors — GPU dispatch
 *     becomes beneficial only with DMA (CE) transfers or large kernels
 *   - When matvec_q4_0 SASS kernel is compiled, the compute-bound
 *     bottleneck (~90% of forward pass) moves to GPU
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
} gpu_kernel_table_t;

/* ── GPU Inference State ──────────────────────────────── */

typedef struct {
    llama_state_t      *cpu;        /* CPU inference state (weights, KV, scratch) */

    /* VRAM scratch for GPU-dispatched vector ops */
    uint64_t            vram_a;     /* Input buffer A */
    uint64_t            vram_b;     /* Input buffer B */
    uint64_t            vram_out;   /* Output buffer */
    uint32_t            vram_buf_size; /* Size of each buffer in bytes */

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
