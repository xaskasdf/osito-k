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
            float scale = f16_to_f32_neon(*(const uint16_t *)w);
            w += 2;
            float32x4_t scale_v = vdupq_n_f32(scale);
            const float *inp = input + b * Q4_0_VALUES;

            /* Load 16 nibble bytes → extract lo/hi nibbles */
            uint8x16_t raw = vld1q_u8(w);
            uint8x16_t lo_nib = vandq_u8(raw, vdupq_n_u8(0x0F));
            uint8x16_t hi_nib = vshrq_n_u8(raw, 4);

            /*
             * Q4_0 layout: byte j → val[2j] = lo nibble, val[2j+1] = hi nibble
             * Scalar: sum += scale * (lo[j] * inp[2j] + hi[j] * inp[2j+1])
             *
             * We interleave: zip(lo[0..3], hi[0..3]) → {lo0,hi0,lo1,hi1,lo2,hi2,lo3,hi3}
             * This matches val[0..7] and can be multiplied by inp[0..7] directly.
             */

            /* Bytes 0-3 → values 0-7 */
            uint8x8_t lo_low8 = vget_low_u8(lo_nib);
            uint8x8_t hi_low8 = vget_low_u8(hi_nib);

            /* Interleave: {lo[0],hi[0],lo[1],hi[1],...,lo[3],hi[3]} = values 0-7 */
            uint8x8x2_t zip03 = vzip_u8(
                vreinterpret_u8_u32(vdup_n_u32(0)),  /* placeholder */
                vreinterpret_u8_u32(vdup_n_u32(0))
            );
            /* Manual interleave for first 4 bytes → 8 values */
            {
                uint8_t tmp[8];
                uint8_t lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, lo_low8);
                vst1_u8(hi_arr, hi_low8);
                for (int j = 0; j < 4; j++) {
                    tmp[j * 2]     = lo_arr[j];
                    tmp[j * 2 + 1] = hi_arr[j];
                }
                uint8x8_t interleaved_0 = vld1_u8(tmp);
                uint16x8_t wide_0 = vmovl_u8(interleaved_0);
                /* values 0-3 */
                int32x4_t v0 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide_0))), bias);
                float32x4_t f0 = vmulq_f32(vcvtq_f32_s32(v0), scale_v);
                acc0 = vfmaq_f32(acc0, f0, vld1q_f32(inp + 0));
                /* values 4-7 */
                int32x4_t v1 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide_0))), bias);
                float32x4_t f1 = vmulq_f32(vcvtq_f32_s32(v1), scale_v);
                acc1 = vfmaq_f32(acc1, f1, vld1q_f32(inp + 4));
            }

            /* Bytes 4-7 → values 8-15 */
            {
                uint8_t tmp[8];
                uint8_t lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, lo_low8);
                vst1_u8(hi_arr, hi_low8);
                for (int j = 0; j < 4; j++) {
                    tmp[j * 2]     = lo_arr[4 + j];
                    tmp[j * 2 + 1] = hi_arr[4 + j];
                }
                uint8x8_t interleaved_1 = vld1_u8(tmp);
                uint16x8_t wide_1 = vmovl_u8(interleaved_1);
                int32x4_t v2 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide_1))), bias);
                acc2 = vfmaq_f32(acc2, vmulq_f32(vcvtq_f32_s32(v2), scale_v), vld1q_f32(inp + 8));
                int32x4_t v3 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide_1))), bias);
                acc3 = vfmaq_f32(acc3, vmulq_f32(vcvtq_f32_s32(v3), scale_v), vld1q_f32(inp + 12));
            }

            /* Bytes 8-11 → values 16-23 */
            {
                uint8x8_t lo_high8 = vget_high_u8(lo_nib);
                uint8x8_t hi_high8 = vget_high_u8(hi_nib);
                uint8_t tmp[8];
                uint8_t lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, lo_high8);
                vst1_u8(hi_arr, hi_high8);
                for (int j = 0; j < 4; j++) {
                    tmp[j * 2]     = lo_arr[j];
                    tmp[j * 2 + 1] = hi_arr[j];
                }
                uint8x8_t interleaved_2 = vld1_u8(tmp);
                uint16x8_t wide_2 = vmovl_u8(interleaved_2);
                int32x4_t v4 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide_2))), bias);
                acc0 = vfmaq_f32(acc0, vmulq_f32(vcvtq_f32_s32(v4), scale_v), vld1q_f32(inp + 16));
                int32x4_t v5 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide_2))), bias);
                acc1 = vfmaq_f32(acc1, vmulq_f32(vcvtq_f32_s32(v5), scale_v), vld1q_f32(inp + 20));
            }

            /* Bytes 12-15 → values 24-31 */
            {
                uint8x8_t lo_high8 = vget_high_u8(lo_nib);
                uint8x8_t hi_high8 = vget_high_u8(hi_nib);
                uint8_t tmp[8];
                uint8_t lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, lo_high8);
                vst1_u8(hi_arr, hi_high8);
                for (int j = 0; j < 4; j++) {
                    tmp[j * 2]     = lo_arr[4 + j];
                    tmp[j * 2 + 1] = hi_arr[4 + j];
                }
                uint8x8_t interleaved_3 = vld1_u8(tmp);
                uint16x8_t wide_3 = vmovl_u8(interleaved_3);
                int32x4_t v6 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide_3))), bias);
                acc2 = vfmaq_f32(acc2, vmulq_f32(vcvtq_f32_s32(v6), scale_v), vld1q_f32(inp + 24));
                int32x4_t v7 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide_3))), bias);
                acc3 = vfmaq_f32(acc3, vmulq_f32(vcvtq_f32_s32(v7), scale_v), vld1q_f32(inp + 28));
            }

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
