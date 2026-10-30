/*
 * tensor_sdot.c -- quantized matvecs with SDOT (the ARMv8.2 dot product)
 *
 * tensor_neon.c's kernels widen every weight to a float and multiply it by
 * a float input. Here the input is quantized once per matvec as llama.cpp
 * does (its Q8_0): per 32 values a scale and 32 int8 (q8_quantize). A
 * block's dot product is then two SDOTs, each 16 int8 x int8 products
 * summed into four int32, times the two scales. Rounding the input costs
 * a fraction of a percent of relative error (`llm selftest` measures it
 * against the float kernels).
 *
 * Every SM8350 core has the instructions (A55, A78, X1); QEMU's Cortex-A72
 * doesn't, so tensor_sdot_detect reads ID_AA64ISAR0_EL1 and without them
 * the float kernels stay in use. Only this file is built with +dotprod
 * (the Makefile), and with FP registers like tensor_neon.c: the same
 * pinning applies.
 */

#include <arm_neon.h>
#include "../include/types.h"
#include "tensor_sdot.h"

int tensor_sdot;
int tensor_sdot_wanted = 1;

int tensor_sdot_detect(void)
{
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    return ((isar0 >> 44) & 0xF) >= 1;     /* DP, bits 47:44 */
}

static inline float half(const uint8_t *p)
{
    __fp16 h;
    memcpy(&h, p, 2);
    return (float)h;
}

void q8_quantize(q8_input_t *x, const float *in, uint32_t cols)
{
    for (uint32_t b = 0; b < cols / 32; b++) {
        const float *p = in + (uint64_t)b * 32;
        float32x4_t v[8], amax = vdupq_n_f32(0.0f), sum = vdupq_n_f32(0.0f);
        for (int i = 0; i < 8; i++) {
            v[i] = vld1q_f32(p + 4 * i);
            amax = vmaxq_f32(amax, vabsq_f32(v[i]));
            sum = vaddq_f32(sum, v[i]);
        }
        float m = vmaxvq_f32(amax), id = m > 0.0f ? 127.0f / m : 0.0f;
        x->d[b] = m / 127.0f;
        x->s[b] = vaddvq_f32(sum);
        int8_t *q = x->q + (uint64_t)b * 32;
        for (int i = 0; i < 8; i += 2) {
            int32x4_t a = vcvtnq_s32_f32(vmulq_n_f32(v[i], id));
            int32x4_t c = vcvtnq_s32_f32(vmulq_n_f32(v[i + 1], id));
            vst1_s8(q + 4 * i, vmovn_s16(vcombine_s16(vmovn_s32(a), vmovn_s32(c))));
        }
    }
}

/* Q4_0's 16 bytes against 32 input values: byte j holds value j in its low
 * nibble and value j + 16 in its high one, both offset by 8 */
static inline int32x4_t dot_q4_0(const uint8_t *qs, const int8_t *xq)
{
    uint8x16_t q = vld1q_u8(qs);
    int8x16_t lo = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(q, vdupq_n_u8(0x0F))), vdupq_n_s8(8));
    int8x16_t hi = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(q, 4)), vdupq_n_s8(8));
    return vdotq_s32(vdotq_s32(vdupq_n_s32(0), lo, vld1q_s8(xq)), hi, vld1q_s8(xq + 16));
}

/* Q4_1: the same nibbles, unsigned (0..15) */
static inline int32x4_t dot_q4_1(const uint8_t *qs, const int8_t *xq)
{
    uint8x16_t q = vld1q_u8(qs);
    int8x16_t lo = vreinterpretq_s8_u8(vandq_u8(q, vdupq_n_u8(0x0F)));
    int8x16_t hi = vreinterpretq_s8_u8(vshrq_n_u8(q, 4));
    return vdotq_s32(vdotq_s32(vdupq_n_s32(0), lo, vld1q_s8(xq)), hi, vld1q_s8(xq + 16));
}

static inline int32x4_t dot_q8_0(const uint8_t *qs, const int8_t *xq)
{
    const int8_t *w = (const int8_t *)qs;
    return vdotq_s32(vdotq_s32(vdupq_n_s32(0), vld1q_s8(w), vld1q_s8(xq)),
                     vld1q_s8(w + 16), vld1q_s8(xq + 16));
}

/* Two blocks a turn, in two accumulators, for the latency of the chain */
void matvec_q4_0_sdot(float *out, const void *weight, const q8_input_t *x,
                      uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks = cols / 32;
    for (uint32_t r = 0; r < rows; r++) {
        float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
        uint32_t b = 0;
        for (; b + 1 < blocks; b += 2, w += 36) {
            int32x4_t p0 = dot_q4_0(w + 2, x->q + (uint64_t)b * 32);
            int32x4_t p1 = dot_q4_0(w + 20, x->q + (uint64_t)b * 32 + 32);
            a0 = vfmaq_n_f32(a0, vcvtq_f32_s32(p0), half(w) * x->d[b]);
            a1 = vfmaq_n_f32(a1, vcvtq_f32_s32(p1), half(w + 18) * x->d[b + 1]);
        }
        if (b < blocks) {
            int32x4_t p0 = dot_q4_0(w + 2, x->q + (uint64_t)b * 32);
            a0 = vfmaq_n_f32(a0, vcvtq_f32_s32(p0), half(w) * x->d[b]);
            w += 18;
        }
        out[r] = vaddvq_f32(vaddq_f32(a0, a1));
    }
}

/* Q4_1: a block's dot product is d * sum(q x) + m * sum(x) */
void matvec_q4_1_sdot(float *out, const void *weight, const q8_input_t *x,
                      uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks = cols / 32;
    for (uint32_t r = 0; r < rows; r++) {
        float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
        float mins = 0.0f;
        uint32_t b = 0;
        for (; b + 1 < blocks; b += 2, w += 40) {
            int32x4_t p0 = dot_q4_1(w + 4, x->q + (uint64_t)b * 32);
            int32x4_t p1 = dot_q4_1(w + 24, x->q + (uint64_t)b * 32 + 32);
            a0 = vfmaq_n_f32(a0, vcvtq_f32_s32(p0), half(w) * x->d[b]);
            a1 = vfmaq_n_f32(a1, vcvtq_f32_s32(p1), half(w + 20) * x->d[b + 1]);
            mins += half(w + 2) * x->s[b] + half(w + 22) * x->s[b + 1];
        }
        if (b < blocks) {
            int32x4_t p0 = dot_q4_1(w + 4, x->q + (uint64_t)b * 32);
            a0 = vfmaq_n_f32(a0, vcvtq_f32_s32(p0), half(w) * x->d[b]);
            mins += half(w + 2) * x->s[b];
            w += 20;
        }
        out[r] = vaddvq_f32(vaddq_f32(a0, a1)) + mins;
    }
}

/* Q8_0: per 32 values an f16 scale and 32 int8 */
void matvec_q8_0_sdot(float *out, const void *weight, const q8_input_t *x,
                      uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks = cols / 32;
    for (uint32_t r = 0; r < rows; r++) {
        float32x4_t a0 = vdupq_n_f32(0.0f), a1 = vdupq_n_f32(0.0f);
        uint32_t b = 0;
        for (; b + 1 < blocks; b += 2, w += 68) {
            int32x4_t p0 = dot_q8_0(w + 2, x->q + (uint64_t)b * 32);
            int32x4_t p1 = dot_q8_0(w + 36, x->q + (uint64_t)b * 32 + 32);
            a0 = vfmaq_n_f32(a0, vcvtq_f32_s32(p0), half(w) * x->d[b]);
            a1 = vfmaq_n_f32(a1, vcvtq_f32_s32(p1), half(w + 34) * x->d[b + 1]);
        }
        if (b < blocks) {
            int32x4_t p0 = dot_q8_0(w + 2, x->q + (uint64_t)b * 32);
            a0 = vfmaq_n_f32(a0, vcvtq_f32_s32(p0), half(w) * x->d[b]);
            w += 34;
        }
        out[r] = vaddvq_f32(vaddq_f32(a0, a1));
    }
}
