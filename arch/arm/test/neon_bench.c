/*
 * neon_bench.c — NEON vs scalar Q4_0 matvec benchmark
 *
 * Measures performance on realistic matrix sizes (2048x2048).
 * Build: aarch64-linux-gnu-gcc -O2 -static -march=armv8.2-a -o neon_bench neon_bench.c -lm
 * Run:   qemu-aarch64 ./neon_bench
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define Q4_0_VALUES  32
#define Q4_0_BYTES   18

static float f16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    if (exp == 0) { if (!mant) { union { uint32_t i; float f; } u = { .i = sign<<31 }; return u.f; }
        while (!(mant & 0x400)) { mant <<= 1; exp--; } exp++; mant &= 0x3FF; }
    else if (exp == 0x1F) { union { uint32_t i; float f; } u = { .i = (sign<<31)|0x7F800000|(mant<<13) }; return u.f; }
    union { uint32_t i; float f; } u = { .i = (sign<<31)|((exp+112)<<23)|(mant<<13) }; return u.f;
}
static uint16_t f32_to_f16(float f) {
    union { float f; uint32_t i; } u = { .f = f };
    uint32_t sign = (u.i >> 16) & 0x8000;
    int exp = ((u.i >> 23) & 0xFF) - 112;
    if (exp <= 0) return sign; if (exp >= 31) return sign | 0x7C00;
    return sign | (exp << 10) | ((u.i >> 13) & 0x3FF);
}

/* Scalar */
static void matvec_scalar(float *out, const uint8_t *w, const float *inp, int rows, int cols) {
    int bpr = cols / Q4_0_VALUES;
    for (int r = 0; r < rows; r++) {
        float sum = 0;
        for (int b = 0; b < bpr; b++) {
            float s = f16_to_f32(*(const uint16_t *)w); w += 2;
            const float *x = inp + b * Q4_0_VALUES;
            for (int j = 0; j < 16; j++) {
                sum += s * (((w[j]&0xF)-8) * x[j*2] + ((w[j]>>4)-8) * x[j*2+1]);
            }
            w += 16;
        }
        out[r] = sum;
    }
}

/* NEON — include same implementation */
#include <arm_neon.h>
#define f16_to_f32_neon f16_to_f32

/* Simplified: just extern from the kernel version would be better,
   but for standalone test we inline it */
static void matvec_neon(float *out, const uint8_t *w, const float *inp, int rows, int cols) {
    int bpr = cols / Q4_0_VALUES;
    const int32x4_t bias = vdupq_n_s32(8);
    for (int r = 0; r < rows; r++) {
        float32x4_t a0=vdupq_n_f32(0), a1=vdupq_n_f32(0), a2=vdupq_n_f32(0), a3=vdupq_n_f32(0);
        for (int b = 0; b < bpr; b++) {
            float sc = f16_to_f32(*(const uint16_t *)w); w += 2;
            float32x4_t sv = vdupq_n_f32(sc);
            const float *x = inp + b * 32;
            uint8x16_t raw = vld1q_u8(w);
            uint8x16_t lo = vandq_u8(raw, vdupq_n_u8(0x0F));
            uint8x16_t hi = vshrq_n_u8(raw, 4);
            /* 4 groups of 8 values each */
            for (int g = 0; g < 4; g++) {
                uint8_t tmp[8], la[8], ha[8];
                vst1_u8(la, g < 2 ? vget_low_u8(lo) : vget_high_u8(lo));
                vst1_u8(ha, g < 2 ? vget_low_u8(hi) : vget_high_u8(hi));
                int off = (g & 1) * 4;
                for (int j = 0; j < 4; j++) { tmp[j*2] = la[off+j]; tmp[j*2+1] = ha[off+j]; }
                uint16x8_t wd = vmovl_u8(vld1_u8(tmp));
                int32x4_t vlo = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(wd))), bias);
                int32x4_t vhi = vsubq_s32(vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(wd))), bias);
                int base = g * 8;
                if (g & 1) {
                    a2 = vfmaq_f32(a2, vmulq_f32(vcvtq_f32_s32(vlo), sv), vld1q_f32(x+base));
                    a3 = vfmaq_f32(a3, vmulq_f32(vcvtq_f32_s32(vhi), sv), vld1q_f32(x+base+4));
                } else {
                    a0 = vfmaq_f32(a0, vmulq_f32(vcvtq_f32_s32(vlo), sv), vld1q_f32(x+base));
                    a1 = vfmaq_f32(a1, vmulq_f32(vcvtq_f32_s32(vhi), sv), vld1q_f32(x+base+4));
                }
            }
            w += 16;
        }
        out[r] = vaddvq_f32(vaddq_f32(vaddq_f32(a0,a1), vaddq_f32(a2,a3)));
    }
}

#define ROWS 256
#define COLS 512
#define ITERS 10

int main(void) {
    int bpr = COLS / Q4_0_VALUES;
    int wsize = ROWS * bpr * Q4_0_BYTES;

    uint8_t *weight = malloc(wsize);
    float *input = malloc(COLS * sizeof(float));
    float *out_s = malloc(ROWS * sizeof(float));
    float *out_n = malloc(ROWS * sizeof(float));

    /* Fill data */
    srand(42);
    uint8_t *wp = weight;
    for (int r = 0; r < ROWS; r++) {
        for (int b = 0; b < bpr; b++) {
            *(uint16_t *)wp = f32_to_f16(0.01f * (rand() % 100)); wp += 2;
            for (int j = 0; j < 16; j++) wp[j] = rand() & 0xFF;
            wp += 16;
        }
    }
    for (int i = 0; i < COLS; i++) input[i] = 0.01f * (rand() % 200 - 100);

    /* Warmup */
    matvec_scalar(out_s, weight, input, ROWS, COLS);
    matvec_neon(out_n, weight, input, ROWS, COLS);

    /* Benchmark scalar */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) matvec_scalar(out_s, weight, input, ROWS, COLS);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double scalar_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* Benchmark NEON */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) matvec_neon(out_n, weight, input, ROWS, COLS);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double neon_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* Verify */
    float max_err = 0;
    for (int r = 0; r < ROWS; r++) {
        float d = fabsf(out_s[r] - out_n[r]);
        if (d > max_err) max_err = d;
    }

    printf("=== Q4_0 matvec benchmark (%dx%d, %d iters) ===\n", ROWS, COLS, ITERS);
    printf("Scalar: %.1f ms  (%.1f ms/iter)\n", scalar_ms, scalar_ms/ITERS);
    printf("NEON:   %.1f ms  (%.1f ms/iter)\n", neon_ms, neon_ms/ITERS);
    printf("Speedup: %.2fx\n", scalar_ms / neon_ms);
    printf("Max error: %.8f\n", max_err);
    printf("Correctness: %s\n", max_err < 0.01f ? "PASS" : "FAIL");

    free(weight); free(input); free(out_s); free(out_n);
    return 0;
}
