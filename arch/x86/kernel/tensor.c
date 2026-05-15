/*
 * OsitoK x86-64 — Tensor Compute Engine
 *
 * Scalar C implementation of quantized tensor operations for LLM inference.
 * Bare-metal freestanding — uses x87 FPU and SSE, no libc.
 *
 * Supported quantization formats: Q4_0, Q8_0 (GGML compatible)
 * Target model: Llama 3.2 1B (Q4_0, 16 layers, hidden=2048)
 */

#include "tensor.h"
#include "../include/paging.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <math.h>   /* expf, sinf, cosf, sqrtf, powf from hosted libc */
#endif

/* ── External functions ──────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── AVX2 Detection + Enable ────────────────────────── */
/* Thin wrappers over cpu_features. cpu_features_detect() runs early
 * in main.c:kernel_entry() and handles CR4.OSXSAVE + XCR0 setup.
 * These stay for backward compatibility; prefer reading cpu_features
 * directly in new code. */

#ifndef __EMSCRIPTEN__
#include "../include/cpu_features.h"

int tensor_avx2_detect(void)
{
    /* cpu_features_detect() already ran at boot; just read the cached result */
    return cpu_features.avx2 && cpu_features.fma;
}
#else
int tensor_avx2_detect(void) { return 0; }
#endif

int tensor_has_avx2(void)
{
#ifdef __EMSCRIPTEN__
    return 0;
#else
    return cpu_features.avx2 && cpu_features.fma;
#endif
}

/* ── Utility helpers ─────────────────────────────────── */

static inline uint64_t rdtsc(void)
{
#ifndef __EMSCRIPTEN__
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return (uint64_t)emscripten_get_now();
#endif
}

static inline float fabsf_bare(float x)
{
    uint32_t bits;
    memcpy(&bits, &x, 4);
    bits &= 0x7FFFFFFF;
    float r;
    memcpy(&r, &bits, 4);
    return r;
}

static void serial_putfloat(float f, int decimals)
{
    if (f < 0.0f) {
        serial_puts("-");
        f = -f;
    }
    uint32_t integer = (uint32_t)f;
    serial_putdec(integer);
    serial_puts(".");
    f -= (float)integer;
    for (int d = 0; d < decimals; d++) {
        f *= 10.0f;
        uint32_t digit = (uint32_t)f;
        if (digit > 9) digit = 9;
        char c[2] = { (char)('0' + digit), '\0' };
        serial_puts(c);
        f -= (float)digit;
    }
}

/* ══════════════════════════════════════════════════════════
 *  Freestanding math (x87 FPU / SSE)
 * ══════════════════════════════════════════════════════════ */

/* IEEE 754 half-float (float16) -> float32, pure bit manipulation */
float f16_to_f32(uint16_t h)
{
    uint32_t sign = ((uint32_t)(h & 0x8000)) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t man  = h & 0x3FF;
    uint32_t bits;

    if (exp == 0) {
        if (man == 0) {
            bits = sign; /* +/- zero */
        } else {
            /* Denormalized: shift mantissa until hidden bit appears.
             * After N shifts (N = final-exp - 1 in this loop), the value
             * encodes as fp32 with biased_exp = 113 - N = 114 - final_exp.
             * The previous formula (113 - exp) was off by one and made
             * subnormal fp16 reads return exactly HALF the correct value,
             * which corrupted Q4_K_M / Q6_K dequant for Llama 3.2-1B where
             * super-block scales are subnormal (~2e-5). */
            exp = 1;
            while (!(man & 0x400)) {
                man <<= 1;
                exp++;
            }
            man &= 0x3FF;
            bits = sign | (((uint32_t)(127 - 15 + 2 - exp)) << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | (man << 13); /* inf/nan */
    } else {
        bits = sign | (((uint32_t)(exp + 112)) << 23) | (man << 13); /* normal */
    }

    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* sqrt — SSE sqrtss on x86, libc sqrtf on WASM */
float sqrtf_bare(float x)
{
#ifndef __EMSCRIPTEN__
    float r;
    __asm__ ("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    return sqrtf(x);
#endif
}

/* Math functions — x87 FPU on x86, libc on WASM */
#ifndef __EMSCRIPTEN__

float expf_bare(float x)
{
    float result;
    __asm__ volatile (
        "flds   %[x]\n\t"
        "fldl2e\n\t"
        "fmulp\n\t"
        "fld    %%st(0)\n\t"
        "frndint\n\t"
        "fsub   %%st, %%st(1)\n\t"
        "fxch   %%st(1)\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp   %%st(1)\n\t"
        "fstps  %[out]\n\t"
        : [out] "=m"(result)
        : [x] "m"(x)
    );
    return result;
}

float sinf_bare(float x)
{
    float result;
    __asm__ volatile (
        "flds  %[x]\n\t"
        "fsin\n\t"
        "fstps %[out]\n\t"
        : [out] "=m"(result)
        : [x] "m"(x)
    );
    return result;
}

float cosf_bare(float x)
{
    float result;
    __asm__ volatile (
        "flds  %[x]\n\t"
        "fcos\n\t"
        "fstps %[out]\n\t"
        : [out] "=m"(result)
        : [x] "m"(x)
    );
    return result;
}

float powf_bare(float base, float exponent)
{
    float result;
    __asm__ volatile (
        "flds   %[exp]\n\t"
        "flds   %[base]\n\t"
        "fyl2x\n\t"
        "fld    %%st(0)\n\t"
        "frndint\n\t"
        "fsub   %%st, %%st(1)\n\t"
        "fxch   %%st(1)\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp   %%st(1)\n\t"
        "fstps  %[out]\n\t"
        : [out] "=m"(result)
        : [base] "m"(base), [exp] "m"(exponent)
    );
    return result;
}

#else /* __EMSCRIPTEN__ — delegate to libc math */

float expf_bare(float x)  { return expf(x); }
float sinf_bare(float x)  { return sinf(x); }
float cosf_bare(float x)  { return cosf(x); }
float powf_bare(float base, float exponent) { return powf(base, exponent); }

#endif /* __EMSCRIPTEN__ */

/* ══════════════════════════════════════════════════════════
 *  Dequantization
 * ══════════════════════════════════════════════════════════ */

/* Q4_0 block: 2B scale (f16) + 16B nibbles (32 values) = 18 bytes */
#define Q4_0_BLOCK_SIZE 18
#define Q4_0_VALUES     32

void dequant_q4_0(const void *src, float *dst, uint64_t n)
{
    const uint8_t *p = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i += Q4_0_VALUES) {
        float scale = f16_to_f32(*(const uint16_t *)p);
        p += 2;
        for (int j = 0; j < 16; j++) {
            dst[i + j]      = ((int)(p[j] & 0xF) - 8) * scale;  /* low nibbles: [0..15]  */
            dst[i + j + 16] = ((int)(p[j] >> 4)  - 8) * scale;  /* high nibbles: [16..31] */
        }
        p += 16;
    }
}

/* Q8_0 block: 2B scale (f16) + 32B int8 values = 34 bytes */
#define Q8_0_BLOCK_SIZE 34
#define Q8_0_VALUES     32

void dequant_q8_0(const void *src, float *dst, uint64_t n)
{
    const uint8_t *p = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i += Q8_0_VALUES) {
        float scale = f16_to_f32(*(const uint16_t *)p);
        p += 2;
        const int8_t *vals = (const int8_t *)p;
        for (int j = 0; j < 32; j++)
            dst[i + j] = vals[j] * scale;
        p += 32;
    }
}

/* ══════════════════════════════════════════════════════════
 *  Quantized matrix-vector multiply
 * ══════════════════════════════════════════════════════════ */

/*
 * matvec_q4_0 — hot path (~90% of inference compute)
 *
 * Inline dequant + dot product per row. Does NOT materialize
 * the full dequantized row in float, saving memory bandwidth.
 */
static void matvec_q4_0_scalar(float *out, const void *weight,
                               const float *input, uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks_per_row = cols / Q4_0_VALUES;

    for (uint32_t r = 0; r < rows; r++) {
        float sum = 0.0f;
        for (uint32_t b = 0; b < blocks_per_row; b++) {
            float scale = f16_to_f32(*(const uint16_t *)w);
            w += 2;
            const float *inp = input + b * Q4_0_VALUES;
            for (int j = 0; j < 16; j++) {
                int lo = (w[j] & 0xF) - 8;
                int hi = (w[j] >> 4)  - 8;
                sum += scale * (lo * inp[j] + hi * inp[j + 16]);
            }
            w += 16;
        }
        out[r] = sum;
    }
}

/* matvec_q4_k_scalar — Llama Q4_K_M (quantization with super-blocks).
 * 256 elems per block, 144 bytes:
 *   2  bytes : f16 d        (super-scale for amounts)
 *   2  bytes : f16 dmin     (super-scale for mins)
 *   12 bytes : packed 6-bit scales+mins for 8 sub-blocks
 *   128 bytes: 256 4-bit quants (low/high nibble interleaved per 64) */
static void q4k_get_scale_min(int j, const uint8_t *q, uint8_t *d, uint8_t *m)
{
    if (j < 4) {
        *d = q[j]     & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0x0F) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >>   4) | ((q[j - 0] >> 6) << 4);
    }
}

#if defined(__EMSCRIPTEN__) && defined(__wasm_simd128__)
#include <wasm_simd128.h>

/* SIMD msimd128 dequant: process 16 quants per iteration. Each
 * iteration loads 16 packed bytes (32 nibbles = 16 low + 16 high)
 * and converts them to 32 f32s via u8→u16→u32→f32 widening. Saves
 * ~3x vs the scalar loop on Llama 1B because Q4_K dequant dominates
 * the per-token compute budget at WASM scalar speeds. */
static void dequant_q4_k_block(const uint8_t *block, float *y)
{
    float d    = f16_to_f32(*(const uint16_t *)(block + 0));
    float dmin = f16_to_f32(*(const uint16_t *)(block + 2));
    const uint8_t *scales = block + 4;
    const uint8_t *qs     = block + 16;
    const v128_t mask_lo  = wasm_u8x16_splat(0x0F);

    int is = 0;
    for (int chunk = 0; chunk < 256; chunk += 64) {
        uint8_t sc, m;
        q4k_get_scale_min(is + 0, scales, &sc, &m);
        float d1 = d * (float)sc, m1 = dmin * (float)m;
        q4k_get_scale_min(is + 1, scales, &sc, &m);
        float d2 = d * (float)sc, m2 = dmin * (float)m;
        const v128_t d1_v = wasm_f32x4_splat(d1);
        const v128_t m1_v = wasm_f32x4_splat(m1);
        const v128_t d2_v = wasm_f32x4_splat(d2);
        const v128_t m2_v = wasm_f32x4_splat(m2);

        /* qs holds 32 bytes for this chunk:
         *   low nibbles → 32 elements (sub-block is)
         *   high nibbles → 32 elements (sub-block is+1)
         * Process in two 16-byte groups via SIMD. */
        for (int half = 0; half < 32; half += 16) {
            v128_t b  = wasm_v128_load(qs + half);
            v128_t lo = wasm_v128_and(b, mask_lo);
            v128_t hi = wasm_u8x16_shr(b, 4);

            /* u8 → u16 (low/high halves) → u32 (low/high halves)
             * → i32 → f32. Six widenings per 16-byte input. */
            v128_t lo_u16_a = wasm_u16x8_extend_low_u8x16(lo);
            v128_t lo_u16_b = wasm_u16x8_extend_high_u8x16(lo);
            v128_t hi_u16_a = wasm_u16x8_extend_low_u8x16(hi);
            v128_t hi_u16_b = wasm_u16x8_extend_high_u8x16(hi);

            v128_t lo_0 = wasm_u32x4_extend_low_u16x8(lo_u16_a);
            v128_t lo_1 = wasm_u32x4_extend_high_u16x8(lo_u16_a);
            v128_t lo_2 = wasm_u32x4_extend_low_u16x8(lo_u16_b);
            v128_t lo_3 = wasm_u32x4_extend_high_u16x8(lo_u16_b);
            v128_t hi_0 = wasm_u32x4_extend_low_u16x8(hi_u16_a);
            v128_t hi_1 = wasm_u32x4_extend_high_u16x8(hi_u16_a);
            v128_t hi_2 = wasm_u32x4_extend_low_u16x8(hi_u16_b);
            v128_t hi_3 = wasm_u32x4_extend_high_u16x8(hi_u16_b);

            v128_t f0 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(lo_0), d1_v), m1_v);
            v128_t f1 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(lo_1), d1_v), m1_v);
            v128_t f2 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(lo_2), d1_v), m1_v);
            v128_t f3 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(lo_3), d1_v), m1_v);
            v128_t g0 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(hi_0), d2_v), m2_v);
            v128_t g1 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(hi_1), d2_v), m2_v);
            v128_t g2 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(hi_2), d2_v), m2_v);
            v128_t g3 = wasm_f32x4_sub(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(hi_3), d2_v), m2_v);

            wasm_v128_store(y + chunk + half +  0, f0);
            wasm_v128_store(y + chunk + half +  4, f1);
            wasm_v128_store(y + chunk + half +  8, f2);
            wasm_v128_store(y + chunk + half + 12, f3);
            wasm_v128_store(y + chunk + 32 + half +  0, g0);
            wasm_v128_store(y + chunk + 32 + half +  4, g1);
            wasm_v128_store(y + chunk + 32 + half +  8, g2);
            wasm_v128_store(y + chunk + 32 + half + 12, g3);
        }
        qs += 32; is += 2;
    }
}
#else
/* Scalar fallback for non-WASM-SIMD targets. */
static void dequant_q4_k_block(const uint8_t *block, float *y)
{
    float d    = f16_to_f32(*(const uint16_t *)(block + 0));
    float dmin = f16_to_f32(*(const uint16_t *)(block + 2));
    const uint8_t *scales = block + 4;
    const uint8_t *qs     = block + 16;
    int is = 0;
    for (int chunk = 0; chunk < 256; chunk += 64) {
        uint8_t sc, m;
        q4k_get_scale_min(is + 0, scales, &sc, &m);
        float d1 = d * (float)sc, m1 = dmin * (float)m;
        q4k_get_scale_min(is + 1, scales, &sc, &m);
        float d2 = d * (float)sc, m2 = dmin * (float)m;
        for (int l = 0; l < 32; l++)
            y[chunk +     l] = d1 * (float)(qs[l] & 0xF) - m1;
        for (int l = 0; l < 32; l++)
            y[chunk + 32 + l] = d2 * (float)(qs[l] >> 4) - m2;
        qs += 32; is += 2;
    }
}
#endif /* __EMSCRIPTEN__ && __wasm_simd128__ — close #if at line 326 */

void matvec_q4_k_scalar(float *out, const void *weight,
                         const float *input, uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks_per_row = cols / 256;
    size_t bytes_per_row = (size_t)blocks_per_row * 144;
    /* Use a per-row dequant scratch on the stack (fixed 256 floats = 1KB
     * per block — small). For each block, dequant to a tiny array then
     * do a clean F32 dot product. Slightly slower than the fused path
     * but eliminates any accumulator-order ambiguity. */
    float scratch[256];

    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *row = w + r * bytes_per_row;
        float sum = 0.0f;
        for (uint32_t b = 0; b < blocks_per_row; b++) {
            dequant_q4_k_block(row + b * 144, scratch);
            const float *inp = input + b * 256;
            for (int i = 0; i < 256; i++)
                sum += scratch[i] * inp[i];
        }
        out[r] = sum;
    }
}

/* matvec_q6_k_scalar — Llama Q6_K (used for output projection / token_embd
 * in some quants). 256 elems per block, 210 bytes:
 *   128 bytes : ql        (low 4 bits of each 6-bit quant)
 *   64  bytes : qh        (high 2 bits packed 4-per-byte)
 *   16  bytes : scales    (int8_t per 16-element sub-block)
 *   2   bytes : f16 d     (super-scale) */
static void dequant_q6_k_block(const uint8_t *block, float *y)
{
    const uint8_t *ql = block + 0;
    const uint8_t *qh = block + 128;
    const int8_t  *sc = (const int8_t *)(block + 192);
    float d = f16_to_f32(*(const uint16_t *)(block + 208));
    for (int n = 0; n < 256; n += 128) {
        for (int l = 0; l < 32; l++) {
            int is = l / 16;
            int8_t q1 = (int8_t)((ql[l +  0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
            int8_t q2 = (int8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
            int8_t q3 = (int8_t)((ql[l +  0]  >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
            int8_t q4 = (int8_t)((ql[l + 32]  >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
            y[n + l +  0] = d * (float)sc[is + 0] * (float)q1;
            y[n + l + 32] = d * (float)sc[is + 2] * (float)q2;
            y[n + l + 64] = d * (float)sc[is + 4] * (float)q3;
            y[n + l + 96] = d * (float)sc[is + 6] * (float)q4;
        }
        ql += 64; qh += 32; sc += 8;
    }
}

void matvec_q6_k_scalar(float *out, const void *weight,
                         const float *input, uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks_per_row = cols / 256;
    size_t bytes_per_row = (size_t)blocks_per_row * 210;
    /* dequant-then-dot — same pattern as Q4_K. The dequant is scalar
     * but the dot product loop auto-vectorizes cleanly with -O3 +
     * msimd128, gaining most of the SIMD benefit without the much
     * more involved Q6_K dequant-side intrinsics (Q6_K extracts 4
     * sub-quadrants per element across two byte arrays). */
    float scratch[256];
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *row = w + r * bytes_per_row;
        float sum = 0.0f;
        for (uint32_t b = 0; b < blocks_per_row; b++) {
            dequant_q6_k_block(row + b * 210, scratch);
            const float *inp = input + b * 256;
            for (int i = 0; i < 256; i++)
                sum += scratch[i] * inp[i];
        }
        out[r] = sum;
    }
}

/* SMP parallel matvec — split rows across AP workers */
typedef struct {
    float       *out;
    const void  *weight;
    const float *input;
    uint32_t     row_start, row_end, cols;
    int          use_avx2;
} matvec_chunk_t;

static void matvec_q4_0_chunk(void *arg, void *result)
{
    (void)result;
    matvec_chunk_t *c = (matvec_chunk_t *)arg;
    /* Advance weight pointer to row_start */
    uint32_t bytes_per_row = (c->cols / 32) * 18; /* Q4_0: 18 bytes per 32 values */
    const void *w = (const uint8_t *)c->weight + (uint64_t)c->row_start * bytes_per_row;
    uint32_t chunk_rows = c->row_end - c->row_start;
    if (c->use_avx2) {
        extern void matvec_q4_0_avx2(float*, const void*, const float*, uint32_t, uint32_t);
        matvec_q4_0_avx2(c->out + c->row_start, w, c->input, chunk_rows, c->cols);
    } else {
        matvec_q4_0_scalar(c->out + c->row_start, w, c->input, chunk_rows, c->cols);
    }
}

void matvec_q4_0(float *out, const void *weight,
                 const float *input, uint32_t rows, uint32_t cols)
{
    extern int ap_worker_count;
    extern int smp_submit(int, void (*)(void*, void*), void*, void*);
    extern void smp_wait(int);

    int avx2 = tensor_has_avx2();
    int workers = ap_worker_count;

    /* Only parallelize large matvecs (IPI overhead not worth it for small) */
    if (workers <= 0 || rows < 128) {
        if (avx2)
            matvec_q4_0_avx2(out, weight, input, rows, cols);
        else
            matvec_q4_0_scalar(out, weight, input, rows, cols);
        return;
    }

    /* Cap at 3 workers (diminishing returns) */
    if (workers > 3) workers = 3;
    int total_parts = workers + 1;  /* workers + BSP */
    uint32_t chunk = rows / total_parts;

    /* Submit chunks to APs — work-stealing handles nesting:
     * if called from an AP, sub-tasks go to our deque and
     * idle cores (or BSP in smp_wait) steal them. */
    matvec_chunk_t chunks[3];
    int tids[3];
    for (int i = 0; i < workers; i++) {
        chunks[i].out       = out;
        chunks[i].weight    = weight;
        chunks[i].input     = input;
        chunks[i].row_start = i * chunk;
        chunks[i].row_end   = (i + 1) * chunk;
        chunks[i].cols      = cols;
        chunks[i].use_avx2  = avx2;
        tids[i] = smp_submit(i, matvec_q4_0_chunk, &chunks[i], 0);
    }

    /* BSP does the last chunk */
    uint32_t bsp_start = workers * chunk;
    uint32_t bytes_per_row = (cols / 32) * 18;
    const void *bsp_w = (const uint8_t *)weight + (uint64_t)bsp_start * bytes_per_row;
    if (avx2)
        matvec_q4_0_avx2(out + bsp_start, bsp_w, input, rows - bsp_start, cols);
    else
        matvec_q4_0_scalar(out + bsp_start, bsp_w, input, rows - bsp_start, cols);

    /* Wait — smp_wait does useful work (steals from others) while waiting */
    for (int i = 0; i < workers; i++)
        if (tids[i] >= 0) smp_wait(tids[i]);
}

void matvec_q8_0(float *out, const void *weight,
                 const float *input, uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks_per_row = cols / Q8_0_VALUES;

    for (uint32_t r = 0; r < rows; r++) {
        float sum = 0.0f;
        for (uint32_t b = 0; b < blocks_per_row; b++) {
            float scale = f16_to_f32(*(const uint16_t *)w);
            w += 2;
            const int8_t *vals = (const int8_t *)w;
            const float *inp = input + b * Q8_0_VALUES;
            for (int j = 0; j < 32; j++)
                sum += scale * vals[j] * inp[j];
            w += 32;
        }
        out[r] = sum;
    }
}

/* ══════════════════════════════════════════════════════════
 *  Vector operations
 * ══════════════════════════════════════════════════════════ */

static void rmsnorm_scalar(float *out, const float *x, const float *weight, uint32_t n)
{
    float ss = 0.0f;
    for (uint32_t i = 0; i < n; i++)
        ss += x[i] * x[i];
    float rms = 1.0f / sqrtf_bare(ss / n + 1e-5f);
    for (uint32_t i = 0; i < n; i++)
        out[i] = x[i] * rms * weight[i];
}

void rmsnorm(float *out, const float *x, const float *weight, uint32_t n)
{
    if (tensor_has_avx2())
        rmsnorm_avx2(out, x, weight, n);
    else
        rmsnorm_scalar(out, x, weight, n);
}

/* Numerically stable softmax (in-place) */
void softmax(float *x, uint32_t n)
{
    float max = x[0];
    for (uint32_t i = 1; i < n; i++)
        if (x[i] > max) max = x[i];

    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        x[i] = expf_bare(x[i] - max);
        sum += x[i];
    }

    float inv = 1.0f / sum;
    for (uint32_t i = 0; i < n; i++)
        x[i] *= inv;
}

/* SiLU activation: x * sigmoid(x) = x / (1 + exp(-x)) */
void silu_inplace(float *x, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        x[i] = x[i] / (1.0f + expf_bare(-x[i]));
}

void vec_add(float *out, const float *a, const float *b, uint32_t n)
{
    if (tensor_has_avx2()) {
        vec_add_avx2(out, a, b, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        out[i] = a[i] + b[i];
}

void vec_mul(float *out, const float *a, const float *b, uint32_t n)
{
    if (tensor_has_avx2()) {
        vec_mul_avx2(out, a, b, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        out[i] = a[i] * b[i];
}

/* ══════════════════════════════════════════════════════════
 *  RoPE (Rotary Position Embedding)
 * ══════════════════════════════════════════════════════════ */

/*
 * For each head, rotate pairs (2i, 2i+1) by angle = pos / theta^(2i/dim)
 * theta = 500000.0 for Llama 3.2
 */
void rope(float *vec, uint32_t n_heads, uint32_t head_dim,
          uint32_t pos, float theta)
{
    /* Llama 3.x rope_scaling: factor=32, low_freq_factor=1,
     * high_freq_factor=4, original_max_position_embeddings=8192.
     * Auto-detect by theta>=500000 (Llama 3 hallmark vs Llama 2's
     * theta=10000). Without this scaling the high-i (low-freq) dims
     * get rotated by ~32x the correct angle, corrupting attention. */
    int llama3 = (theta >= 100000.0f);
    const float FACTOR = 32.0f;
    const float LO_FF  = 1.0f;
    const float HI_FF  = 4.0f;
    const float ORIG_CTX = 8192.0f;
    const float LO_WAVE  = ORIG_CTX / LO_FF;     /* 8192 */
    const float HI_WAVE  = ORIG_CTX / HI_FF;     /* 2048 */
    const float TWO_PI   = 6.28318530717958647692f;
    for (uint32_t h = 0; h < n_heads; h++) {
        float *head = vec + h * head_dim;
        for (uint32_t i = 0; i < head_dim; i += 2) {
            float inv_freq = 1.0f / powf_bare(theta, (float)i / (float)head_dim);
            if (llama3) {
                float wavelen = TWO_PI / inv_freq;
                if (wavelen > LO_WAVE) {
                    inv_freq /= FACTOR;
                } else if (wavelen >= HI_WAVE) {
                    /* Smooth interpolation in the medium-freq band. */
                    float smooth = (ORIG_CTX / wavelen - LO_FF) / (HI_FF - LO_FF);
                    inv_freq = (1.0f - smooth) * (inv_freq / FACTOR) + smooth * inv_freq;
                }
                /* wavelen < HI_WAVE → unchanged (high freq) */
            }
            float angle = (float)pos * inv_freq;
            float cos_a = cosf_bare(angle);
            float sin_a = sinf_bare(angle);
            float x0 = head[i], x1 = head[i + 1];
            head[i]     = x0 * cos_a - x1 * sin_a;
            head[i + 1] = x0 * sin_a + x1 * cos_a;
        }
    }
}

/* ══════════════════════════════════════════════════════════
 *  Self-test + Benchmark
 * ══════════════════════════════════════════════════════════ */

/* Fill Q4_0 weight matrix with deterministic pattern */
static void fill_q4_0(uint8_t *w, uint32_t rows, uint32_t cols)
{
    uint32_t blocks_per_row = cols / 32;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t b = 0; b < blocks_per_row; b++) {
            /* Cycle through scales: 0.5, 1.0, 2.0 */
            uint16_t scales[] = { 0x3800, 0x3C00, 0x4000 };
            *(uint16_t *)w = scales[(r + b) % 3];
            w += 2;
            for (int j = 0; j < 16; j++) {
                uint8_t lo = (uint8_t)((r + b + j) % 16);
                uint8_t hi = (uint8_t)((r + b + j + 1) % 16);
                w[j] = lo | (uint8_t)(hi << 4);
            }
            w += 16;
        }
    }
}

/* Fill Q8_0 weight matrix with deterministic pattern */
static void fill_q8_0(uint8_t *w, uint32_t rows, uint32_t cols)
{
    uint32_t blocks_per_row = cols / 32;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t b = 0; b < blocks_per_row; b++) {
            uint16_t scales[] = { 0x3800, 0x3C00, 0x4000 };
            *(uint16_t *)w = scales[(r + b) % 3];
            w += 2;
            int8_t *vals = (int8_t *)w;
            for (int j = 0; j < 32; j++)
                vals[j] = (int8_t)(((r + b + j) % 17) - 8);
            w += 32;
        }
    }
}

#define PAGE_SZ 4096

static uint32_t pages_for(uint32_t bytes)
{
    return (bytes + PAGE_SZ - 1) / PAGE_SZ;
}

/* SMP worker wrapper for boot parallelism (called from main.c) */
void tensor_benchmark_worker(void *arg, void *result)
{
    (void)arg; (void)result;
    tensor_benchmark();
}

void tensor_benchmark(void)
{
    serial_puts("\n[TENSOR] === Tensor Compute Engine ===\n");

    /* Detect and enable AVX2 */
    int avx2 = tensor_avx2_detect();
    serial_puts("[TENSOR] AVX2+FMA: ");
    serial_puts(avx2 ? "ENABLED\n" : "not available (scalar fallback)\n");

    fb_puts("\n Tensor engine self-test...\n");

    int pass = 1;

    /* ── Test 1: Float sanity ────────────────────────── */
    {
        volatile float a = 1.0f, b = 1.0f;
        float sum = a + b;
        float prod = 3.0f * 4.0f;
        if (sum == 2.0f && prod == 12.0f)
            serial_puts("[TENSOR] Float test: OK\n");
        else {
            serial_puts("[TENSOR] Float test: FAIL\n");
            pass = 0;
        }
    }

    /* ── Test 2: f16 -> f32 ──────────────────────────── */
    {
        float v1 = f16_to_f32(0x3C00);  /* 1.0 */
        float v2 = f16_to_f32(0x4000);  /* 2.0 */
        float v3 = f16_to_f32(0xBC00);  /* -1.0 */
        float v4 = f16_to_f32(0x0000);  /* 0.0 */
        if (v1 == 1.0f && v2 == 2.0f && v3 == -1.0f && v4 == 0.0f)
            serial_puts("[TENSOR] f16->f32 test: OK\n");
        else {
            serial_puts("[TENSOR] f16->f32 test: FAIL (");
            serial_putfloat(v1, 3); serial_puts(", ");
            serial_putfloat(v2, 3); serial_puts(", ");
            serial_putfloat(v3, 3); serial_puts(", ");
            serial_putfloat(v4, 3); serial_puts(")\n");
            pass = 0;
        }
    }

    /* ── Test 3: Q4_0 dequant ────────────────────────── */
    {
        /* Block: scale=1.0, all nibbles 0x53 (lo=3,hi=5)
         * Values: (3-8)*1.0 = -5.0, (5-8)*1.0 = -3.0 repeating */
        uint8_t block[18];
        *(uint16_t *)block = 0x3C00;
        for (int j = 0; j < 16; j++)
            block[2 + j] = 0x53;

        float out[32];
        dequant_q4_0(block, out, 32);

        int ok = 1;
        for (int j = 0; j < 32; j += 2) {
            if (out[j] != -5.0f || out[j + 1] != -3.0f) ok = 0;
        }
        if (ok)
            serial_puts("[TENSOR] Q4_0 dequant test: OK\n");
        else {
            serial_puts("[TENSOR] Q4_0 dequant test: FAIL\n");
            pass = 0;
        }
    }

    /* ── Test 4: Q4_0 matvec 256x256 ─────────────────── */
    {
        uint32_t rows = 256, cols = 256;
        uint32_t w_bytes = rows * (cols / 32) * Q4_0_BLOCK_SIZE;
        uint32_t w_pg   = pages_for(w_bytes);
        uint32_t v_pg   = pages_for(cols * sizeof(float));

        void *weights_phys = mem_alloc_pages(w_pg);
        void *input_phys   = mem_alloc_pages(v_pg);
        void *output_phys  = mem_alloc_pages(v_pg);
        void *ref_phys     = mem_alloc_pages(v_pg);
        void *tmp_phys     = mem_alloc_pages(v_pg);

        if (weights_phys && input_phys && output_phys && ref_phys && tmp_phys) {
            uint8_t *weights = (uint8_t *)PHYS_TO_VIRT(weights_phys);
            float   *input   = (float *)PHYS_TO_VIRT(input_phys);
            float   *output  = (float *)PHYS_TO_VIRT(output_phys);
            float   *ref     = (float *)PHYS_TO_VIRT(ref_phys);
            float   *tmp     = (float *)PHYS_TO_VIRT(tmp_phys);
            fill_q4_0(weights, rows, cols);
            for (uint32_t i = 0; i < cols; i++)
                input[i] = (float)((i % 5) + 1) * 0.1f;

            /* Optimized path */
            matvec_q4_0(output, weights, input, rows, cols);

            /* Reference: dequant + naive dot */
            float max_err = 0.0f;
            const uint8_t *wp = weights;
            for (uint32_t r = 0; r < rows; r++) {
                dequant_q4_0(wp, tmp, cols);
                wp += (cols / 32) * Q4_0_BLOCK_SIZE;
                float dot = 0.0f;
                for (uint32_t c = 0; c < cols; c++)
                    dot += tmp[c] * input[c];
                ref[r] = dot;
                float err = fabsf_bare(output[r] - ref[r]);
                if (err > max_err) max_err = err;
            }

            serial_puts("[TENSOR] Q4_0 matvec 256x256: ");
            if (max_err < 0.01f) {
                serial_puts("OK (max err: ");
                serial_putfloat(max_err, 3);
                serial_puts(")\n");
            } else {
                serial_puts("FAIL (max err: ");
                serial_putfloat(max_err, 3);
                serial_puts(")\n");
                pass = 0;
            }
        } else {
            serial_puts("[TENSOR] Q4_0 matvec: SKIP (alloc failed)\n");
        }

        if (weights_phys) mem_free_pages(weights_phys, w_pg);
        if (input_phys)   mem_free_pages(input_phys,   v_pg);
        if (output_phys)  mem_free_pages(output_phys,  v_pg);
        if (ref_phys)     mem_free_pages(ref_phys,     v_pg);
        if (tmp_phys)     mem_free_pages(tmp_phys,     v_pg);
    }

    /* ── Test 5: Q8_0 matvec 256x256 ─────────────────── */
    {
        uint32_t rows = 256, cols = 256;
        uint32_t w_bytes = rows * (cols / 32) * Q8_0_BLOCK_SIZE;
        uint32_t w_pg   = pages_for(w_bytes);
        uint32_t v_pg   = pages_for(cols * sizeof(float));

        void *weights_phys = mem_alloc_pages(w_pg);
        void *input_phys   = mem_alloc_pages(v_pg);
        void *output_phys  = mem_alloc_pages(v_pg);
        void *ref_phys     = mem_alloc_pages(v_pg);
        void *tmp_phys     = mem_alloc_pages(v_pg);

        if (weights_phys && input_phys && output_phys && ref_phys && tmp_phys) {
            uint8_t *weights = (uint8_t *)PHYS_TO_VIRT(weights_phys);
            float   *input   = (float *)PHYS_TO_VIRT(input_phys);
            float   *output  = (float *)PHYS_TO_VIRT(output_phys);
            float   *ref     = (float *)PHYS_TO_VIRT(ref_phys);
            float   *tmp     = (float *)PHYS_TO_VIRT(tmp_phys);
            fill_q8_0(weights, rows, cols);
            for (uint32_t i = 0; i < cols; i++)
                input[i] = (float)((i % 5) + 1) * 0.1f;

            matvec_q8_0(output, weights, input, rows, cols);

            float max_err = 0.0f;
            const uint8_t *wp = weights;
            for (uint32_t r = 0; r < rows; r++) {
                dequant_q8_0(wp, tmp, cols);
                wp += (cols / 32) * Q8_0_BLOCK_SIZE;
                float dot = 0.0f;
                for (uint32_t c = 0; c < cols; c++)
                    dot += tmp[c] * input[c];
                ref[r] = dot;
                float err = fabsf_bare(output[r] - ref[r]);
                if (err > max_err) max_err = err;
            }

            if (max_err < 0.01f)
                serial_puts("[TENSOR] Q8_0 matvec 256x256: OK\n");
            else {
                serial_puts("[TENSOR] Q8_0 matvec 256x256: FAIL (max err: ");
                serial_putfloat(max_err, 3);
                serial_puts(")\n");
                pass = 0;
            }
        } else {
            serial_puts("[TENSOR] Q8_0 matvec: SKIP (alloc failed)\n");
        }

        if (weights_phys) mem_free_pages(weights_phys, w_pg);
        if (input_phys)   mem_free_pages(input_phys,   v_pg);
        if (output_phys)  mem_free_pages(output_phys,  v_pg);
        if (ref_phys)     mem_free_pages(ref_phys,     v_pg);
        if (tmp_phys)     mem_free_pages(tmp_phys,     v_pg);
    }

    /* ── Test 6: RMSNorm ─────────────────────────────── */
    {
        float x[] = { 1.0f, 2.0f, 3.0f, 4.0f };
        float w[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float out[4];
        rmsnorm(out, x, w, 4);
        /* With w=1, rmsnorm guarantees mean(out^2) = 1.0 */
        float ss = 0.0f;
        for (int i = 0; i < 4; i++) ss += out[i] * out[i];
        float mean_sq = ss / 4.0f;
        if (fabsf_bare(mean_sq - 1.0f) < 0.01f)
            serial_puts("[TENSOR] RMSNorm test: OK\n");
        else {
            serial_puts("[TENSOR] RMSNorm test: FAIL (mean_sq=");
            serial_putfloat(mean_sq, 3);
            serial_puts(")\n");
            pass = 0;
        }
    }

    /* ── Test 7: Softmax ─────────────────────────────── */
    {
        float x[] = { 1.0f, 2.0f, 3.0f };
        softmax(x, 3);
        float sum = x[0] + x[1] + x[2];
        int order_ok = (x[0] < x[1]) && (x[1] < x[2]);
        if (fabsf_bare(sum - 1.0f) < 0.001f && order_ok)
            serial_puts("[TENSOR] Softmax test: OK\n");
        else {
            serial_puts("[TENSOR] Softmax test: FAIL (sum=");
            serial_putfloat(sum, 3);
            serial_puts(")\n");
            pass = 0;
        }
    }

    /* ── Test 8: SiLU ────────────────────────────────── */
    {
        float x[] = { 0.0f, 1.0f, -1.0f };
        silu_inplace(x, 3);
        /* silu(0) = 0, silu(1) ~ 0.731, silu(-1) ~ -0.269 */
        int ok = fabsf_bare(x[0]) < 0.001f &&
                 fabsf_bare(x[1] - 0.731f) < 0.01f &&
                 fabsf_bare(x[2] + 0.269f) < 0.01f;
        if (ok)
            serial_puts("[TENSOR] SiLU test: OK\n");
        else {
            serial_puts("[TENSOR] SiLU test: FAIL (");
            serial_putfloat(x[0], 3); serial_puts(", ");
            serial_putfloat(x[1], 3); serial_puts(", ");
            serial_putfloat(x[2], 3); serial_puts(")\n");
            pass = 0;
        }
    }

    /* ── Perf: matvec_q4_0 2048x2048 ─────────────────── */
    {
        uint32_t rows = 2048, cols = 2048;
        uint32_t w_bytes = rows * (cols / 32) * Q4_0_BLOCK_SIZE;
        uint32_t w_pg   = pages_for(w_bytes);
        uint32_t v_pg   = pages_for(cols * sizeof(float));

        void *weights_phys = mem_alloc_pages(w_pg);
        void *input_phys   = mem_alloc_pages(v_pg);
        void *output_phys  = mem_alloc_pages(v_pg);

        if (weights_phys && input_phys && output_phys) {
            uint8_t *weights = (uint8_t *)PHYS_TO_VIRT(weights_phys);
            float   *input   = (float *)PHYS_TO_VIRT(input_phys);
            float   *output  = (float *)PHYS_TO_VIRT(output_phys);
            fill_q4_0(weights, rows, cols);
            for (uint32_t i = 0; i < cols; i++)
                input[i] = (float)((i % 7) + 1) * 0.1f;

            uint64_t t0 = rdtsc();
            matvec_q4_0(output, weights, input, rows, cols);
            uint64_t t1 = rdtsc();
            uint64_t cycles = t1 - t0;
            uint64_t ms_est = cycles / 3000000; /* ~3GHz estimate */

            serial_puts("[TENSOR] Perf: matvec_q4_0 2048x2048 = ");
            serial_putdec(cycles);
            serial_puts(" cycles (~");
            serial_putdec(ms_est);
            serial_puts(" ms @ 3GHz)\n");
        } else {
            serial_puts("[TENSOR] Perf: SKIP (alloc failed)\n");
        }

        if (weights_phys) mem_free_pages(weights_phys, w_pg);
        if (input_phys)   mem_free_pages(input_phys,   v_pg);
        if (output_phys)  mem_free_pages(output_phys,  v_pg);
    }

    /* ── Perf: AVX2 vs scalar comparison (if AVX2 available) ── */
    if (avx2) {
        uint32_t rows = 2048, cols = 2048;
        uint32_t w_bytes = rows * (cols / 32) * Q4_0_BLOCK_SIZE;
        uint32_t w_pg   = pages_for(w_bytes);
        uint32_t v_pg   = pages_for(cols * sizeof(float));

        void *weights_phys = mem_alloc_pages(w_pg);
        void *input2_phys  = mem_alloc_pages(v_pg);
        void *out_s_phys   = mem_alloc_pages(v_pg);
        void *out_a_phys   = mem_alloc_pages(v_pg);

        if (weights_phys && input2_phys && out_s_phys && out_a_phys) {
            uint8_t *weights = (uint8_t *)PHYS_TO_VIRT(weights_phys);
            float   *input2  = (float *)PHYS_TO_VIRT(input2_phys);
            float   *out_s   = (float *)PHYS_TO_VIRT(out_s_phys);
            float   *out_a   = (float *)PHYS_TO_VIRT(out_a_phys);
            fill_q4_0(weights, rows, cols);
            for (uint32_t i = 0; i < cols; i++)
                input2[i] = (float)((i % 7) + 1) * 0.1f;

            /* Scalar path */
            uint64_t ts0 = rdtsc();
            matvec_q4_0_scalar(out_s, weights, input2, rows, cols);
            uint64_t ts1 = rdtsc();

            /* AVX2 path */
            uint64_t ta0 = rdtsc();
            matvec_q4_0_avx2(out_a, weights, input2, rows, cols);
            uint64_t ta1 = rdtsc();

            uint64_t scalar_ms = (ts1 - ts0) / 3000000;
            uint64_t avx2_ms   = (ta1 - ta0) / 3000000;

            serial_puts("[TENSOR] Perf: scalar=");
            serial_putdec(scalar_ms);
            serial_puts("ms  AVX2=");
            serial_putdec(avx2_ms);
            serial_puts("ms");
            if (avx2_ms > 0) {
                serial_puts("  speedup=");
                serial_putdec(scalar_ms / avx2_ms);
                serial_puts("x");
            }
            serial_puts("\n");

            /* Verify AVX2 matches scalar */
            float max_diff = 0.0f;
            for (uint32_t i = 0; i < rows; i++) {
                float d = out_s[i] - out_a[i];
                if (d < 0) d = -d;
                if (d > max_diff) max_diff = d;
            }
            serial_puts("[TENSOR] AVX2 vs scalar max diff: ");
            serial_putfloat(max_diff, 6);
            serial_puts(max_diff < 0.01f ? " OK\n" : " MISMATCH\n");
            if (max_diff >= 0.01f) pass = 0;
        }

        if (weights_phys) mem_free_pages(weights_phys, w_pg);
        if (input2_phys)  mem_free_pages(input2_phys,  v_pg);
        if (out_s_phys)   mem_free_pages(out_s_phys,   v_pg);
        if (out_a_phys)   mem_free_pages(out_a_phys,   v_pg);
    }

    /* ── Summary ─────────────────────────────────────── */
    if (pass) {
        serial_puts("[TENSOR] Engine ready");
        if (avx2) serial_puts(" (AVX2)");
        serial_puts("\n\n");
        fb_puts(" Tensor engine: OK");
        if (avx2) fb_puts(" [AVX2]");
        fb_puts("\n");
    } else {
        serial_puts("[TENSOR] Engine: SOME TESTS FAILED\n\n");
        fb_puts(" Tensor engine: ERRORS\n");
    }
}
