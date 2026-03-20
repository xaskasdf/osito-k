/*
 * tensor_neon.c -- ARM64 NEON SIMD tensor operations
 *
 * NEON equivalents of AVX2 hot paths for AI inference.
 * 128-bit SIMD (4 floats) vs AVX2's 256-bit (8 floats).
 * Expected ~3-4x speedup over scalar C on Cortex-A72/A78.
 *
 * Requires: -march=armv8.2-a (NEON always available on ARMv8)
 *           Must NOT use -mgeneral-regs-only
 */

#include <arm_neon.h>
#include "../include/types.h"

/* ── f16→f32 conversion (matches tensor.c) ───────────────── */

static inline float f16_to_f32_neon(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) { union { uint32_t i; float f; } u = { .i = sign << 31 }; return u.f; }
        while (!(mant & 0x400)) { mant <<= 1; exp--; }
        exp++; mant &= 0x3FF;
    } else if (exp == 0x1F) {
        union { uint32_t i; float f; } u = { .i = (sign << 31) | 0x7F800000 | (mant << 13) };
        return u.f;
    }
    union { uint32_t i; float f; } u = { .i = (sign << 31) | ((exp + 112) << 23) | (mant << 13) };
    return u.f;
}

/* ── Q4_0 block size ─────────────────────────────────────── */

#define Q4_0_VALUES  32
#define Q4_0_BYTES   18   /* 2 (scale f16) + 16 (nibbles) */

/* ── Quantized matrix-vector multiply (NEON, ~90% of inference) ── */

void matvec_q4_0_neon(float *out, const void *weight,
                      const float *input, int rows, int cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    int blocks_per_row = cols / Q4_0_VALUES;
    const int32x4_t bias = vdupq_n_s32(8);

    for (int r = 0; r < rows; r++) {
        float32x4_t acc0 = vdupq_n_f32(0.0f);
        float32x4_t acc1 = vdupq_n_f32(0.0f);
        float32x4_t acc2 = vdupq_n_f32(0.0f);
        float32x4_t acc3 = vdupq_n_f32(0.0f);

        for (int b = 0; b < blocks_per_row; b++) {
            /* Read scale (f16) */
            float scale = f16_to_f32_neon(*(const uint16_t *)w);
            w += 2;
            float32x4_t scale_v = vdupq_n_f32(scale);
            const float *inp = input + b * Q4_0_VALUES;

            /* Load 16 nibble bytes → 32 values */
            uint8x16_t raw = vld1q_u8(w);
            uint8x16_t lo_nib = vandq_u8(raw, vdupq_n_u8(0x0F));
            uint8x16_t hi_nib = vshrq_n_u8(raw, 4);

            /* Process lo nibbles (values 0,2,4,...,30) and hi nibbles (1,3,5,...,31)
             * Interleave: pair[i] = {lo[i], hi[i]} → values[2i], values[2i+1] */

            /* Group 0: values 0-3 (lo[0],hi[0],lo[1],hi[1]) */
            uint8x8_t lo_low8 = vget_low_u8(lo_nib);   /* lo[0..7] */
            uint8x8_t hi_low8 = vget_low_u8(hi_nib);   /* hi[0..7] */

            /* Widen lo[0..3] to s32 and subtract bias */
            uint16x8_t lo16 = vmovl_u8(lo_low8);        /* lo[0..7] as u16 */
            int32x4_t v0 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(lo16))), bias);
            float32x4_t f0 = vmulq_f32(vcvtq_f32_s32(v0), scale_v);
            acc0 = vfmaq_f32(acc0, f0, vld1q_f32(inp));        /* values 0,2,4,6 × input[0,2,4,6] */

            /* Group 1: hi[0..3] → values 1,3,5,7 */
            uint16x8_t hi16 = vmovl_u8(hi_low8);
            int32x4_t v1 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(hi16))), bias);
            float32x4_t f1 = vmulq_f32(vcvtq_f32_s32(v1), scale_v);
            acc1 = vfmaq_f32(acc1, f1, vld1q_f32(inp + 1));    /* values 1,3,5,7 × input[1,3,5,7] */

            /* Group 2: lo[4..7] → values 8,10,12,14 */
            int32x4_t v2 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(lo16))), bias);
            float32x4_t f2 = vmulq_f32(vcvtq_f32_s32(v2), scale_v);
            acc2 = vfmaq_f32(acc2, f2, vld1q_f32(inp + 8));

            /* Group 3: hi[4..7] → values 9,11,13,15 */
            int32x4_t v3 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(hi16))), bias);
            float32x4_t f3 = vmulq_f32(vcvtq_f32_s32(v3), scale_v);
            acc3 = vfmaq_f32(acc3, f3, vld1q_f32(inp + 9));

            /* Groups 4-7: hi nibble bytes 8-15 (values 16-31) */
            uint8x8_t lo_high8 = vget_high_u8(lo_nib);
            uint8x8_t hi_high8 = vget_high_u8(hi_nib);
            uint16x8_t lo16h = vmovl_u8(lo_high8);
            uint16x8_t hi16h = vmovl_u8(hi_high8);

            int32x4_t v4 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(lo16h))), bias);
            acc0 = vfmaq_f32(acc0, vmulq_f32(vcvtq_f32_s32(v4), scale_v), vld1q_f32(inp + 16));

            int32x4_t v5 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(hi16h))), bias);
            acc1 = vfmaq_f32(acc1, vmulq_f32(vcvtq_f32_s32(v5), scale_v), vld1q_f32(inp + 17));

            int32x4_t v6 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(lo16h))), bias);
            acc2 = vfmaq_f32(acc2, vmulq_f32(vcvtq_f32_s32(v6), scale_v), vld1q_f32(inp + 24));

            int32x4_t v7 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(hi16h))), bias);
            acc3 = vfmaq_f32(acc3, vmulq_f32(vcvtq_f32_s32(v7), scale_v), vld1q_f32(inp + 25));

            w += 16;
        }

        /* Horizontal sum: 4 accumulators → 1 scalar */
        float32x4_t sum01 = vaddq_f32(acc0, acc1);
        float32x4_t sum23 = vaddq_f32(acc2, acc3);
        float32x4_t sum = vaddq_f32(sum01, sum23);
        out[r] = vaddvq_f32(sum);
    }
}

/* ── RMS normalization (NEON) ────────────────────────────── */

extern float sqrtf_bare(float x);

void rmsnorm_neon(float *out, const float *x, const float *weight, int n)
{
    /* Pass 1: sum of squares */
    float32x4_t ss_acc = vdupq_n_f32(0.0f);
    int i;
    for (i = 0; i + 4 <= n; i += 4) {
        float32x4_t xv = vld1q_f32(x + i);
        ss_acc = vfmaq_f32(ss_acc, xv, xv);
    }
    float ss = vaddvq_f32(ss_acc);
    for (; i < n; i++) ss += x[i] * x[i];

    float rms = 1.0f / sqrtf_bare(ss / (float)n + 1e-5f);
    float32x4_t rms_v = vdupq_n_f32(rms);

    /* Pass 2: out = x * rms * weight */
    for (i = 0; i + 4 <= n; i += 4) {
        float32x4_t xv = vld1q_f32(x + i);
        float32x4_t wv = vld1q_f32(weight + i);
        vst1q_f32(out + i, vmulq_f32(vmulq_f32(xv, rms_v), wv));
    }
    for (; i < n; i++) out[i] = x[i] * rms * weight[i];
}

/* ── Vector add (NEON) ───────────────────────────────────── */

void vec_add_neon(float *a, const float *b, int n)
{
    int i;
    for (i = 0; i + 4 <= n; i += 4) {
        float32x4_t av = vld1q_f32(a + i);
        float32x4_t bv = vld1q_f32(b + i);
        vst1q_f32(a + i, vaddq_f32(av, bv));
    }
    for (; i < n; i++) a[i] += b[i];
}

/* ── Vector multiply (NEON) ──────────────────────────────── */

void vec_mul_neon(float *a, const float *b, int n)
{
    int i;
    for (i = 0; i + 4 <= n; i += 4) {
        float32x4_t av = vld1q_f32(a + i);
        float32x4_t bv = vld1q_f32(b + i);
        vst1q_f32(a + i, vmulq_f32(av, bv));
    }
    for (; i < n; i++) a[i] *= b[i];
}
