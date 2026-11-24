/*
 * tensor_neon.c -- the inference hot paths in NEON
 *
 * The inference stack (tensor.c, inference.c) comes from arch/x86, which
 * calls its SIMD kernels by their AVX2 names whenever tensor_has_avx2()
 * says so. On AArch64 these are those names: NEON kernels with the same
 * signatures, giving the scalar code's results up to rounding (`llm
 * selftest` compares the two). NEON is part of ARMv8, so they're always
 * usable; tensor_simd = 0 (`llm simd off`) goes back to the scalar code.
 *
 * Built with FP registers (FP_SRCS in the Makefile). The scheduler saves
 * no FP/SIMD state, so callers keep this on one CPU (llm.c pins its task).
 */

#include <arm_neon.h>
#include "../include/types.h"

/* An f16 in memory, by the FP unit's own conversion */
static inline float half(const uint8_t *p)
{
    __fp16 h;
    memcpy(&h, p, 2);
    return (float)h;
}

/* Q4_0: per 32 values an f16 scale and 16 bytes; byte j holds value j in
 * its low nibble and value j + 16 in its high one, both offset by 8
 * (dequant_q4_0 in tensor.c) */
void matvec_q4_0_avx2(float *out, const void *weight, const float *input,
                      uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks = cols / 32;
    const int8x16_t eight = vdupq_n_s8(8);
    const uint8x16_t low = vdupq_n_u8(0x0F);

    for (uint32_t r = 0; r < rows; r++) {
        float32x4_t acc = vdupq_n_f32(0.0f);
        for (uint32_t b = 0; b < blocks; b++, w += 18) {
            uint8x16_t q = vld1q_u8(w + 2);
            int8x16_t lo = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(q, low)), eight);
            int8x16_t hi = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(q, 4)), eight);
            int16x8_t l0 = vmovl_s8(vget_low_s8(lo)), l1 = vmovl_s8(vget_high_s8(lo));
            int16x8_t h0 = vmovl_s8(vget_low_s8(hi)), h1 = vmovl_s8(vget_high_s8(hi));
            const float *x = input + (uint64_t)b * 32;

            /* Two chains for the FMA latency */
            float32x4_t d0 = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(l0))), vld1q_f32(x));
            float32x4_t d1 = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(l0))), vld1q_f32(x + 4));
            d0 = vfmaq_f32(d0, vcvtq_f32_s32(vmovl_s16(vget_low_s16(l1))), vld1q_f32(x + 8));
            d1 = vfmaq_f32(d1, vcvtq_f32_s32(vmovl_s16(vget_high_s16(l1))), vld1q_f32(x + 12));
            d0 = vfmaq_f32(d0, vcvtq_f32_s32(vmovl_s16(vget_low_s16(h0))), vld1q_f32(x + 16));
            d1 = vfmaq_f32(d1, vcvtq_f32_s32(vmovl_s16(vget_high_s16(h0))), vld1q_f32(x + 20));
            d0 = vfmaq_f32(d0, vcvtq_f32_s32(vmovl_s16(vget_low_s16(h1))), vld1q_f32(x + 24));
            d1 = vfmaq_f32(d1, vcvtq_f32_s32(vmovl_s16(vget_high_s16(h1))), vld1q_f32(x + 28));
            acc = vfmaq_n_f32(acc, vaddq_f32(d0, d1), half(w));
        }
        out[r] = vaddvq_f32(acc);
    }
}

/* Q4_1: f16 scale d and minimum m, then nibbles as in Q4_0 but unsigned:
 * a block's dot product is d * sum(q x) + m * sum(x) */
void matvec_q4_1_neon(float *out, const void *weight, const float *input,
                      uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks = cols / 32;
    const uint8x16_t low = vdupq_n_u8(0x0F);

    for (uint32_t r = 0; r < rows; r++) {
        float32x4_t acc = vdupq_n_f32(0.0f);
        for (uint32_t b = 0; b < blocks; b++, w += 20) {
            uint8x16_t q = vld1q_u8(w + 4);
            uint8x16_t lo = vandq_u8(q, low), hi = vshrq_n_u8(q, 4);
            uint16x8_t l0 = vmovl_u8(vget_low_u8(lo)), l1 = vmovl_u8(vget_high_u8(lo));
            uint16x8_t h0 = vmovl_u8(vget_low_u8(hi)), h1 = vmovl_u8(vget_high_u8(hi));
            const float *x = input + (uint64_t)b * 32;
            float32x4_t x0 = vld1q_f32(x), x1 = vld1q_f32(x + 4), x2 = vld1q_f32(x + 8),
                        x3 = vld1q_f32(x + 12), x4 = vld1q_f32(x + 16), x5 = vld1q_f32(x + 20),
                        x6 = vld1q_f32(x + 24), x7 = vld1q_f32(x + 28);
            float32x4_t d0 = vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(l0))), x0);
            float32x4_t d1 = vmulq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(l0))), x1);
            d0 = vfmaq_f32(d0, vcvtq_f32_u32(vmovl_u16(vget_low_u16(l1))), x2);
            d1 = vfmaq_f32(d1, vcvtq_f32_u32(vmovl_u16(vget_high_u16(l1))), x3);
            d0 = vfmaq_f32(d0, vcvtq_f32_u32(vmovl_u16(vget_low_u16(h0))), x4);
            d1 = vfmaq_f32(d1, vcvtq_f32_u32(vmovl_u16(vget_high_u16(h0))), x5);
            d0 = vfmaq_f32(d0, vcvtq_f32_u32(vmovl_u16(vget_low_u16(h1))), x6);
            d1 = vfmaq_f32(d1, vcvtq_f32_u32(vmovl_u16(vget_high_u16(h1))), x7);
            float32x4_t sx = vaddq_f32(vaddq_f32(vaddq_f32(x0, x1), vaddq_f32(x2, x3)),
                                       vaddq_f32(vaddq_f32(x4, x5), vaddq_f32(x6, x7)));
            acc = vfmaq_n_f32(acc, vaddq_f32(d0, d1), half(w));
            acc = vfmaq_n_f32(acc, sx, half(w + 2));
        }
        out[r] = vaddvq_f32(acc);
    }
}

/* Q8_0: per 32 values an f16 scale and 32 int8 (SmolLM2's token embedding,
 * which is also its output layer) */
void matvec_q8_0_neon(float *out, const void *weight, const float *input,
                      uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks = cols / 32;

    for (uint32_t r = 0; r < rows; r++) {
        float32x4_t acc = vdupq_n_f32(0.0f);
        for (uint32_t b = 0; b < blocks; b++, w += 34) {
            int8x16_t q0 = vld1q_s8((const int8_t *)w + 2), q1 = vld1q_s8((const int8_t *)w + 18);
            int16x8_t l0 = vmovl_s8(vget_low_s8(q0)), l1 = vmovl_s8(vget_high_s8(q0));
            int16x8_t h0 = vmovl_s8(vget_low_s8(q1)), h1 = vmovl_s8(vget_high_s8(q1));
            const float *x = input + (uint64_t)b * 32;

            float32x4_t d0 = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(l0))), vld1q_f32(x));
            float32x4_t d1 = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(l0))), vld1q_f32(x + 4));
            d0 = vfmaq_f32(d0, vcvtq_f32_s32(vmovl_s16(vget_low_s16(l1))), vld1q_f32(x + 8));
            d1 = vfmaq_f32(d1, vcvtq_f32_s32(vmovl_s16(vget_high_s16(l1))), vld1q_f32(x + 12));
            d0 = vfmaq_f32(d0, vcvtq_f32_s32(vmovl_s16(vget_low_s16(h0))), vld1q_f32(x + 16));
            d1 = vfmaq_f32(d1, vcvtq_f32_s32(vmovl_s16(vget_high_s16(h0))), vld1q_f32(x + 20));
            d0 = vfmaq_f32(d0, vcvtq_f32_s32(vmovl_s16(vget_low_s16(h1))), vld1q_f32(x + 24));
            d1 = vfmaq_f32(d1, vcvtq_f32_s32(vmovl_s16(vget_high_s16(h1))), vld1q_f32(x + 28));
            acc = vfmaq_n_f32(acc, vaddq_f32(d0, d1), half(w));
        }
        out[r] = vaddvq_f32(acc);
    }
}

/* F16 weights, row-major (brandon-tiny ships every layer as f16) */
void matvec_f16_avx2(float *out, const void *weight, const float *input,
                     uint32_t rows, uint32_t cols)
{
    const __fp16 *w = (const __fp16 *)weight;
    uint32_t c8 = cols & ~7u;

    for (uint32_t r = 0; r < rows; r++) {
        const __fp16 *row = w + (uint64_t)r * cols;
        float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
        for (uint32_t c = 0; c < c8; c += 8) {
            float16x8_t h = vld1q_f16(row + c);
            a0 = vfmaq_f32(a0, vcvt_f32_f16(vget_low_f16(h)), vld1q_f32(input + c));
            a1 = vfmaq_f32(a1, vcvt_high_f32_f16(h), vld1q_f32(input + c + 4));
        }
        float sum = vaddvq_f32(vaddq_f32(a0, a1));
        for (uint32_t c = c8; c < cols; c++)
            sum += (float)row[c] * input[c];
        out[r] = sum;
    }
}

extern float sqrtf_bare(float x);

/* out = x / rms(x) * weight, eps 1e-5 (rmsnorm_scalar) */
void rmsnorm_avx2(float *out, const float *x, const float *weight, uint32_t n)
{
    float32x4_t ss = vdupq_n_f32(0.0f);
    uint32_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        ss = vfmaq_f32(ss, v, v);
    }
    float sum = vaddvq_f32(ss);
    for (; i < n; i++)
        sum += x[i] * x[i];
    float scale = 1.0f / sqrtf_bare(sum / (float)n + 1e-5f);
    for (i = 0; i + 4 <= n; i += 4)
        vst1q_f32(out + i, vmulq_f32(vmulq_n_f32(vld1q_f32(x + i), scale), vld1q_f32(weight + i)));
    for (; i < n; i++)
        out[i] = x[i] * scale * weight[i];
}

void vec_add_avx2(float *out, const float *a, const float *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 4 <= n; i += 4)
        vst1q_f32(out + i, vaddq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    for (; i < n; i++)
        out[i] = a[i] + b[i];
}

void vec_mul_avx2(float *out, const float *a, const float *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 4 <= n; i += 4)
        vst1q_f32(out + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    for (; i < n; i++)
        out[i] = a[i] * b[i];
}
