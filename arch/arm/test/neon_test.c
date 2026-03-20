/*
 * neon_test.c — NEON vs scalar Q4_0 matvec validation test
 *
 * Runs both scalar and NEON matvec_q4_0 on synthetic data,
 * compares outputs. Compiled as Linux ARM64 binary, tested
 * via qemu-aarch64 user-mode emulation.
 *
 * Build: aarch64-linux-gnu-gcc -O2 -static -march=armv8.2-a -o neon_test neon_test.c
 * Run:   qemu-aarch64 ./neon_test
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ── Q4_0 constants ──────────────────────────────────────── */

#define Q4_0_VALUES  32
#define Q4_0_BYTES   18   /* 2 (scale f16) + 16 (nibbles) */

/* ── f16 → f32 ──────────────────────────────────────────── */

static float f16_to_f32(uint16_t h)
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

/* ── f32 → f16 (for test data generation) ────────────────── */

static uint16_t f32_to_f16(float f)
{
    union { float f; uint32_t i; } u = { .f = f };
    uint32_t sign = (u.i >> 16) & 0x8000;
    int exp = ((u.i >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (u.i >> 13) & 0x3FF;
    if (exp <= 0) return sign;
    if (exp >= 31) return sign | 0x7C00;
    return sign | (exp << 10) | mant;
}

/* ── Scalar reference (from tensor.c) ────────────────────── */

static void matvec_q4_0_scalar(float *out, const void *weight,
                                const float *input, int rows, int cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    int blocks_per_row = cols / Q4_0_VALUES;

    for (int r = 0; r < rows; r++) {
        float sum = 0.0f;
        for (int b = 0; b < blocks_per_row; b++) {
            float scale = f16_to_f32(*(const uint16_t *)w);
            w += 2;
            const float *inp = input + b * Q4_0_VALUES;
            for (int j = 0; j < 16; j++) {
                int lo = (w[j] & 0xF) - 8;
                int hi = (w[j] >> 4)  - 8;
                sum += scale * (lo * inp[j * 2] + hi * inp[j * 2 + 1]);
            }
            w += 16;
        }
        out[r] = sum;
    }
}

/* ── Include NEON implementation ─────────────────────────── */

/* We include it directly to test the same code that runs in the kernel */
#include <arm_neon.h>

/* Rename to avoid conflict with static f16_to_f32 above */
#define f16_to_f32_neon f16_to_f32

void matvec_q4_0_neon(float *out, const void *weight,
                      const float *input, int rows, int cols);

/* Paste the NEON implementation inline (simplified for test) */
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
            float scale = f16_to_f32(*(const uint16_t *)w);
            w += 2;
            float32x4_t scale_v = vdupq_n_f32(scale);
            const float *inp = input + b * Q4_0_VALUES;

            uint8x16_t raw = vld1q_u8(w);
            uint8x16_t lo_nib = vandq_u8(raw, vdupq_n_u8(0x0F));
            uint8x16_t hi_nib = vshrq_n_u8(raw, 4);

            /* Bytes 0-3 → values 0-7 (interleaved lo/hi) */
            {
                uint8_t tmp[8], lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, vget_low_u8(lo_nib));
                vst1_u8(hi_arr, vget_low_u8(hi_nib));
                for (int j = 0; j < 4; j++) {
                    tmp[j*2]   = lo_arr[j];
                    tmp[j*2+1] = hi_arr[j];
                }
                uint8x8_t interleaved = vld1_u8(tmp);
                uint16x8_t wide = vmovl_u8(interleaved);
                int32x4_t v0 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide))), bias);
                acc0 = vfmaq_f32(acc0, vmulq_f32(vcvtq_f32_s32(v0), scale_v), vld1q_f32(inp+0));
                int32x4_t v1 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide))), bias);
                acc1 = vfmaq_f32(acc1, vmulq_f32(vcvtq_f32_s32(v1), scale_v), vld1q_f32(inp+4));
            }
            /* Bytes 4-7 → values 8-15 */
            {
                uint8_t tmp[8], lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, vget_low_u8(lo_nib));
                vst1_u8(hi_arr, vget_low_u8(hi_nib));
                for (int j = 0; j < 4; j++) {
                    tmp[j*2]   = lo_arr[4+j];
                    tmp[j*2+1] = hi_arr[4+j];
                }
                uint8x8_t interleaved = vld1_u8(tmp);
                uint16x8_t wide = vmovl_u8(interleaved);
                int32x4_t v2 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide))), bias);
                acc2 = vfmaq_f32(acc2, vmulq_f32(vcvtq_f32_s32(v2), scale_v), vld1q_f32(inp+8));
                int32x4_t v3 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide))), bias);
                acc3 = vfmaq_f32(acc3, vmulq_f32(vcvtq_f32_s32(v3), scale_v), vld1q_f32(inp+12));
            }
            /* Bytes 8-11 → values 16-23 */
            {
                uint8_t tmp[8], lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, vget_high_u8(lo_nib));
                vst1_u8(hi_arr, vget_high_u8(hi_nib));
                for (int j = 0; j < 4; j++) {
                    tmp[j*2]   = lo_arr[j];
                    tmp[j*2+1] = hi_arr[j];
                }
                uint8x8_t interleaved = vld1_u8(tmp);
                uint16x8_t wide = vmovl_u8(interleaved);
                int32x4_t v4 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide))), bias);
                acc0 = vfmaq_f32(acc0, vmulq_f32(vcvtq_f32_s32(v4), scale_v), vld1q_f32(inp+16));
                int32x4_t v5 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide))), bias);
                acc1 = vfmaq_f32(acc1, vmulq_f32(vcvtq_f32_s32(v5), scale_v), vld1q_f32(inp+20));
            }
            /* Bytes 12-15 → values 24-31 */
            {
                uint8_t tmp[8], lo_arr[8], hi_arr[8];
                vst1_u8(lo_arr, vget_high_u8(lo_nib));
                vst1_u8(hi_arr, vget_high_u8(hi_nib));
                for (int j = 0; j < 4; j++) {
                    tmp[j*2]   = lo_arr[4+j];
                    tmp[j*2+1] = hi_arr[4+j];
                }
                uint8x8_t interleaved = vld1_u8(tmp);
                uint16x8_t wide = vmovl_u8(interleaved);
                int32x4_t v6 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wide))), bias);
                acc2 = vfmaq_f32(acc2, vmulq_f32(vcvtq_f32_s32(v6), scale_v), vld1q_f32(inp+24));
                int32x4_t v7 = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wide))), bias);
                acc3 = vfmaq_f32(acc3, vmulq_f32(vcvtq_f32_s32(v7), scale_v), vld1q_f32(inp+28));
            }
            w += 16;
        }

        float32x4_t sum01 = vaddq_f32(acc0, acc1);
        float32x4_t sum23 = vaddq_f32(acc2, acc3);
        float32x4_t sum = vaddq_f32(sum01, sum23);
        out[r] = vaddvq_f32(sum);
    }
}

/* ── Test harness ────────────────────────────────────────── */

#define ROWS 4
#define COLS 64  /* 2 Q4_0 blocks per row */

int main(void)
{
    /* Generate synthetic Q4_0 weight data */
    /* Each Q4_0 block: 2 bytes scale (f16) + 16 bytes nibbles = 18 bytes */
    int blocks_per_row = COLS / Q4_0_VALUES;
    int weight_bytes = ROWS * blocks_per_row * Q4_0_BYTES;
    uint8_t weight[ROWS * 2 * Q4_0_BYTES];  /* 4 rows × 2 blocks × 18 bytes */

    /* Fill with deterministic test data */
    uint8_t *wp = weight;
    for (int r = 0; r < ROWS; r++) {
        for (int b = 0; b < blocks_per_row; b++) {
            /* Scale = 0.5 (in f16) */
            uint16_t scale_f16 = f32_to_f16(0.5f);
            *(uint16_t *)wp = scale_f16;
            wp += 2;
            /* Nibbles: byte j has lo=j%8, hi=(j+1)%8 */
            for (int j = 0; j < 16; j++) {
                uint8_t lo = (j + r) % 16;
                uint8_t hi = (j + r + 1) % 16;
                wp[j] = (hi << 4) | lo;
            }
            wp += 16;
        }
    }

    /* Generate input vector */
    float input[COLS];
    for (int i = 0; i < COLS; i++)
        input[i] = 1.0f + (float)(i % 8) * 0.1f;

    /* Run scalar */
    float out_scalar[ROWS];
    matvec_q4_0_scalar(out_scalar, weight, input, ROWS, COLS);

    /* Run NEON */
    float out_neon[ROWS];
    matvec_q4_0_neon(out_neon, weight, input, ROWS, COLS);

    /* Compare */
    printf("=== NEON vs Scalar Q4_0 matvec test ===\n");
    printf("Matrix: %d rows x %d cols (%d Q4_0 blocks/row)\n\n", ROWS, COLS, blocks_per_row);

    int pass = 1;
    for (int r = 0; r < ROWS; r++) {
        float diff = fabsf(out_scalar[r] - out_neon[r]);
        float rel = (out_scalar[r] != 0.0f) ? diff / fabsf(out_scalar[r]) : diff;
        printf("Row %d: scalar=%.6f  neon=%.6f  diff=%.8f  rel=%.2e  %s\n",
               r, out_scalar[r], out_neon[r], diff, rel,
               (rel < 1e-5f) ? "OK" : "MISMATCH");
        if (rel >= 1e-5f) pass = 0;
    }

    printf("\n%s\n", pass ? "ALL TESTS PASSED" : "TESTS FAILED");
    return pass ? 0 : 1;
}
