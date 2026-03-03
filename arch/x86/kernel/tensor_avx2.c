/*
 * OsitoK x86-64 — AVX2/FMA Tensor Ops (X-CPU1)
 *
 * Vectorized implementations of hot-path tensor operations.
 * This file MUST be compiled with -mavx2 -mfma.
 * Runtime dispatch in tensor.c selects AVX2 vs scalar via CPUID.
 *
 * Speedup estimate: ~4-8x on matvec_q4_0 (90% of inference compute).
 */

#include "tensor.h"
#include <immintrin.h>

/* ── Horizontal sum of 8 floats in __m256 ─────────────────── */

static inline float hsum256(__m256 v)
{
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 lo = _mm256_castps256_ps128(v);
    lo = _mm_add_ps(lo, hi);              /* 4 floats */
    __m128 shuf = _mm_movehdup_ps(lo);    /* {1,1,3,3} */
    lo = _mm_add_ps(lo, shuf);            /* {0+1, ?, 2+3, ?} */
    shuf = _mm_movehl_ps(shuf, lo);       /* {2+3, ?, ?, ?} */
    lo = _mm_add_ss(lo, shuf);            /* {sum, ?, ?, ?} */
    return _mm_cvtss_f32(lo);
}

/* ══════════════════════════════════════════════════════════
 *  AVX2 matvec_q4_0 — main hot path
 *
 *  Per Q4_0 block (32 values = 16 bytes of nibbles + 2B scale):
 *    1. Load 16 nibble bytes → extract lo/hi → interleave
 *    2. Extend 8 uint8 → 8 int32 → subtract bias 8 → float
 *    3. Scale × FMA with input vector
 *    4. Accumulate across 4 groups of 8
 * ══════════════════════════════════════════════════════════ */

void matvec_q4_0_avx2(float *out, const void *weight,
                       const float *input, uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks_per_row = cols / 32;
    const __m256i bias = _mm256_set1_epi32(8);

    for (uint32_t r = 0; r < rows; r++) {
        __m256 acc = _mm256_setzero_ps();

        for (uint32_t b = 0; b < blocks_per_row; b++) {
            /* Scale: f16 → f32, broadcast */
            float scale = f16_to_f32(*(const uint16_t *)w);
            w += 2;
            __m256 scale_v = _mm256_set1_ps(scale);
            const float *inp = input + b * 32;

            /*
             * Load all 16 nibble bytes for this block.
             * Layout: byte[j] has lo nibble (bits 0-3) and hi nibble (bits 4-7).
             * Output ordering: dst[j*2] = lo, dst[j*2+1] = hi.
             */
            __m128i raw = _mm_loadu_si128((const __m128i *)w);

            /* Extract lo nibbles (bits 0-3) and hi nibbles (bits 4-7) */
            __m128i lo_nib = _mm_and_si128(raw, _mm_set1_epi8(0x0F));
            __m128i hi_nib = _mm_and_si128(_mm_srli_epi16(raw, 4),
                                           _mm_set1_epi8(0x0F));

            /* Interleave: lo0,hi0,lo1,hi1,... (matches scalar output order) */
            __m128i interleaved_lo = _mm_unpacklo_epi8(lo_nib, hi_nib); /* bytes 0-7 → 16 values */
            __m128i interleaved_hi = _mm_unpackhi_epi8(lo_nib, hi_nib); /* bytes 8-15 → 16 values */

            /* Group 0: values 0-7 (from interleaved_lo low 8 bytes) */
            __m256i v0 = _mm256_sub_epi32(
                _mm256_cvtepu8_epi32(interleaved_lo), bias);
            __m256 f0 = _mm256_mul_ps(_mm256_cvtepi32_ps(v0), scale_v);
            acc = _mm256_fmadd_ps(f0, _mm256_loadu_ps(inp), acc);

            /* Group 1: values 8-15 (from interleaved_lo high 8 bytes) */
            __m256i v1 = _mm256_sub_epi32(
                _mm256_cvtepu8_epi32(_mm_bsrli_si128(interleaved_lo, 8)), bias);
            __m256 f1 = _mm256_mul_ps(_mm256_cvtepi32_ps(v1), scale_v);
            acc = _mm256_fmadd_ps(f1, _mm256_loadu_ps(inp + 8), acc);

            /* Group 2: values 16-23 (from interleaved_hi low 8 bytes) */
            __m256i v2 = _mm256_sub_epi32(
                _mm256_cvtepu8_epi32(interleaved_hi), bias);
            __m256 f2 = _mm256_mul_ps(_mm256_cvtepi32_ps(v2), scale_v);
            acc = _mm256_fmadd_ps(f2, _mm256_loadu_ps(inp + 16), acc);

            /* Group 3: values 24-31 (from interleaved_hi high 8 bytes) */
            __m256i v3 = _mm256_sub_epi32(
                _mm256_cvtepu8_epi32(_mm_bsrli_si128(interleaved_hi, 8)), bias);
            __m256 f3 = _mm256_mul_ps(_mm256_cvtepi32_ps(v3), scale_v);
            acc = _mm256_fmadd_ps(f3, _mm256_loadu_ps(inp + 24), acc);

            w += 16;
        }

        out[r] = hsum256(acc);
    }
}

/* ══════════════════════════════════════════════════════════
 *  AVX2 rmsnorm — sum-of-squares + element-wise multiply
 * ══════════════════════════════════════════════════════════ */

void rmsnorm_avx2(float *out, const float *x,
                  const float *weight, uint32_t n)
{
    /* Pass 1: sum of squares */
    __m256 ss_acc = _mm256_setzero_ps();
    uint32_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        __m256 xv = _mm256_loadu_ps(x + i);
        ss_acc = _mm256_fmadd_ps(xv, xv, ss_acc);
    }

    float ss = hsum256(ss_acc);
    for (; i < n; i++)
        ss += x[i] * x[i];

    float rms = 1.0f / sqrtf_bare(ss / (float)n + 1e-5f);
    __m256 rms_v = _mm256_set1_ps(rms);

    /* Pass 2: out = x * rms * weight */
    for (i = 0; i + 8 <= n; i += 8) {
        __m256 xv = _mm256_loadu_ps(x + i);
        __m256 wv = _mm256_loadu_ps(weight + i);
        __m256 result = _mm256_mul_ps(_mm256_mul_ps(xv, rms_v), wv);
        _mm256_storeu_ps(out + i, result);
    }
    for (; i < n; i++)
        out[i] = x[i] * rms * weight[i];
}

/* ══════════════════════════════════════════════════════════
 *  AVX2 vec_add / vec_mul
 * ══════════════════════════════════════════════════════════ */

void vec_add_avx2(float *out, const float *a,
                  const float *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        __m256 av = _mm256_loadu_ps(a + i);
        __m256 bv = _mm256_loadu_ps(b + i);
        _mm256_storeu_ps(out + i, _mm256_add_ps(av, bv));
    }
    for (; i < n; i++)
        out[i] = a[i] + b[i];
}

void vec_mul_avx2(float *out, const float *a,
                  const float *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        __m256 av = _mm256_loadu_ps(a + i);
        __m256 bv = _mm256_loadu_ps(b + i);
        _mm256_storeu_ps(out + i, _mm256_mul_ps(av, bv));
    }
    for (; i < n; i++)
        out[i] = a[i] * b[i];
}
