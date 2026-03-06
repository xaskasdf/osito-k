/*
 * OsitoK x86-64 — GPU Tensor Operations (X39)
 *
 * Dispatch layer for GPU-accelerated tensor operations.
 * Manages VRAM buffers, data upload/download, and kernel dispatch.
 *
 * Current kernels (hand-encoded SM75 SASS):
 *   - store_pattern: STG validation (proves GMMU+compute pipeline)
 *   - vec_add_f32:   Vector addition dst[i] = a[i] + b[i]
 *
 * Future kernels (compile PTX with ptxas, embed SASS bytes):
 *   - matvec_q4_0:   Q4_0 dequant + dot product
 *   - rmsnorm:       Parallel reduction + normalize
 *   - softmax:       Max + exp + sum + divide
 *   - rope:          Rotary position embedding
 *   - silu:          SiLU activation (x * sigmoid(x))
 */

#ifndef OSITOK_GPU_TENSOR_H
#define OSITOK_GPU_TENSOR_H

#include "../include/types.h"

/* VRAM buffer region: starts after SASS kernels at 260MB */
#define GPU_TENSOR_VRAM_OFFSET_MB  260
#define GPU_TENSOR_VRAM_SIZE_MB      4   /* 4MB for tensor buffers */

/* GPU tensor buffer state */
typedef struct {
    uint64_t vram_base;       /* Start of tensor buffer region */
    uint64_t vram_next;       /* Next free address (bump allocator) */
    uint64_t vram_end;        /* End of buffer region */
    bool     initialized;
} gpu_tensor_state_t;

/* ── Initialization ─────────────────────────────────────── */

/* Initialize GPU tensor subsystem (VRAM allocator + run self-tests) */
int  gpu_tensor_init(void);

/* Get tensor state */
gpu_tensor_state_t *gpu_tensor_get_state(void);

/* ── VRAM Buffer Management ─────────────────────────────── */

/* Allocate VRAM buffer (256-byte aligned). Returns GPU VA, 0 on failure. */
uint64_t gpu_tensor_alloc(uint32_t size);

/* Reset VRAM allocator (free all buffers) */
void gpu_tensor_reset(void);

/* ── Data Transfer (via PRAMIN window) ──────────────────── */

/* Upload data from host RAM to VRAM buffer */
int gpu_tensor_upload(uint64_t vram_addr, const void *data, uint32_t size);

/* Download data from VRAM buffer to host RAM */
int gpu_tensor_download(uint64_t vram_addr, void *data, uint32_t size);

/* ── Tensor Operations (dispatch SASS kernels) ──────────── */

/* Vector addition: dst[i] = a[i] + b[i] (max 256 elements, 1-block) */
int gpu_vec_add(uint64_t dst_vram, uint64_t a_vram, uint64_t b_vram, uint32_t n);

/* ── Self-Test ──────────────────────────────────────────── */

/* Run GPU tensor self-test (store_pattern + vec_add if available) */
int gpu_tensor_test(void);

#endif /* OSITOK_GPU_TENSOR_H */
