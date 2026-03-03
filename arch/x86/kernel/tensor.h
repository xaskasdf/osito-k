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

float f16_to_f32(uint16_t h);       /* IEEE 754 half -> float */
float sqrtf_bare(float x);          /* SSE sqrtss */
float expf_bare(float x);           /* x87: fldl2e + f2xm1 + fscale */
float sinf_bare(float x);           /* x87 fsin */
float cosf_bare(float x);           /* x87 fcos */
float powf_bare(float base, float exponent); /* x87: fyl2x + 2^x */

/* ── Dequantization ──────────────────────────────────── */

void dequant_q4_0(const void *src, float *dst, uint64_t n);
void dequant_q8_0(const void *src, float *dst, uint64_t n);

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

/* ── Self-test + benchmark ───────────────────────────── */

void tensor_benchmark(void);

#endif /* OSITOK_TENSOR_H */
