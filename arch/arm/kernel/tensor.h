/*
 * OsitoK x86-64 — Tensor Compute Engine
 *
 * Freestanding math + quantized tensor operations for LLM inference.
 * No libc dependency. Uses x87 FPU and SSE instructions.
 *
 * Supported formats: Q4_0, Q8_0 (GGML compatible)
 * Target model: Llama 3.2 1B (Q4_0, 16 layers, hidden=2048)
 */

#ifndef OSITOK_TENSOR_H
#define OSITOK_TENSOR_H

#include "../include/types.h"

/* ── Freestanding math (x87/SSE) ─────────────────────── */

float    f16_to_f32(uint16_t h);    /* IEEE 754 half -> float */
uint16_t f32_to_f16(float f);       /* IEEE 754 float -> half */
float sqrtf_bare(float x);          /* fsqrt */
float expf_bare(float x);           /* the rest in double precision */
float sinf_bare(float x);
float cosf_bare(float x);
float powf_bare(float base, float exponent);

/* ── Dequantization ──────────────────────────────────── */

void dequant_q4_0(const void *src, float *dst, uint64_t n);
void dequant_q8_0(const void *src, float *dst, uint64_t n);
void dequant_q4_1(const void *src, float *dst, uint64_t n);
void matvec_q4_1(float *out, const void *weight,
                 const float *input, uint32_t rows, uint32_t cols);
void quantize_q8_0(const float *src, void *dst, uint64_t n);

/* ── Quantized matrix-vector multiply ────────────────── */
/* out[rows] = weight[rows x cols, quantized] x input[cols] */

void matvec_q4_0(float *out, const void *weight,
                 const float *input, uint32_t rows, uint32_t cols);
void matvec_q8_0(float *out, const void *weight,
                 const float *input, uint32_t rows, uint32_t cols);

/* ── Vector operations ───────────────────────────────── */

void rmsnorm(float *out, const float *x,
             const float *weight, uint32_t n);
void softmax(float *x, uint32_t n);        /* in-place */
void silu_inplace(float *x, uint32_t n);   /* x / (1 + exp(-x)) */
void vec_add(float *out, const float *a,
             const float *b, uint32_t n);
void vec_mul(float *out, const float *a,
             const float *b, uint32_t n);

/* ── RoPE (Rotary Position Embedding) ────────────────── */

void rope(float *vec, uint32_t n_heads,
          uint32_t head_dim, uint32_t pos, float theta);

/* ── SIMD paths: x86's AVX2 names, NEON here (tensor_neon.c) ── */

void matvec_q4_0_avx2(float *out, const void *weight,
                       const float *input, uint32_t rows, uint32_t cols);
void rmsnorm_avx2(float *out, const float *x,
                  const float *weight, uint32_t n);
void vec_add_avx2(float *out, const float *a,
                  const float *b, uint32_t n);
void vec_mul_avx2(float *out, const float *a,
                  const float *b, uint32_t n);

/* ── AVX2 detection + enable ────────────────────────── */

/* Detect AVX2/FMA via CPUID, enable AVX state (CR4 + XCR0).
 * Returns 1 if AVX2+FMA available, 0 otherwise. */
int  tensor_avx2_detect(void);

/* Returns cached result after first detect call */
int  tensor_has_avx2(void);

/* ── Self-test + benchmark ───────────────────────────── */

void tensor_benchmark(void);

#endif /* OSITOK_TENSOR_H */
