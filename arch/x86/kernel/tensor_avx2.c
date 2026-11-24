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

/* Process one Q4_0 block (32 values): F16C scale + nibble unpack + 4×FMA.
 * Shared between the unrolled and remainder loops. */
static inline void __attribute__((always_inline))
process_q4_block(const uint8_t **wp, const float **inp,
                 __m256 *acc, const __m256i bias, const __m128i nibmask)
{
    /* Scale: F16C hardware conversion (2 instructions vs ~20 software) */
    __m128i sh = _mm_cvtsi32_si128(*(const uint16_t *)*wp);
    __m256 scale_v = _mm256_broadcastss_ps(_mm_cvtph_ps(sh));
    *wp += 2;

    /* Load 16 nibble bytes, extract lo/hi, interleave */
    __m128i raw = _mm_loadu_si128((const __m128i *)*wp);
    __m128i lo_nib = _mm_and_si128(raw, nibmask);
    __m128i hi_nib = _mm_and_si128(_mm_srli_epi16(raw, 4), nibmask);
    __m128i ilo = _mm_unpacklo_epi8(lo_nib, hi_nib);
    __m128i ihi = _mm_unpackhi_epi8(lo_nib, hi_nib);

    /* 4 groups of 8 values: dequant + FMA */
    __m256 f0 = _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_sub_epi32(_mm256_cvtepu8_epi32(ilo), bias)), scale_v);
    *acc = _mm256_fmadd_ps(f0, _mm256_loadu_ps(*inp), *acc);

    __m256 f1 = _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_bsrli_si128(ilo, 8)), bias)), scale_v);
    *acc = _mm256_fmadd_ps(f1, _mm256_loadu_ps(*inp + 8), *acc);

    __m256 f2 = _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_sub_epi32(_mm256_cvtepu8_epi32(ihi), bias)), scale_v);
    *acc = _mm256_fmadd_ps(f2, _mm256_loadu_ps(*inp + 16), *acc);

    __m256 f3 = _mm256_mul_ps(_mm256_cvtepi32_ps(
        _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_bsrli_si128(ihi, 8)), bias)), scale_v);
    *acc = _mm256_fmadd_ps(f3, _mm256_loadu_ps(*inp + 24), *acc);

    *wp += 16;
    *inp += 32;
}

void __hot matvec_q4_0_avx2(float *out, const void *weight,
                       const float *input, uint32_t rows, uint32_t cols)
{
    const uint8_t *w = (const uint8_t *)weight;
    uint32_t blocks_per_row = cols / 32;
    uint32_t unrolled = blocks_per_row / 4;
    uint32_t remainder = blocks_per_row & 3;
    const __m256i bias = _mm256_set1_epi32(8);
    const __m128i nibmask = _mm_set1_epi8(0x0F);

    for (uint32_t r = 0; r < rows; r++) {
        __m256 acc0 = _mm256_setzero_ps();
        __m256 acc1 = _mm256_setzero_ps();
        const float *inp = input;

        /* 4-block unrolled loop: 128 values per iteration, 2 accumulators
         * to hide FMA latency (5 cycles on Haswell, 4 on Skylake+).
         * For dim=2048: 64 blocks / 4 = 16 iterations. */
        for (uint32_t u = 0; u < unrolled; u++) {
            process_q4_block(&w, &inp, &acc0, bias, nibmask);
            process_q4_block(&w, &inp, &acc1, bias, nibmask);
            process_q4_block(&w, &inp, &acc0, bias, nibmask);
            process_q4_block(&w, &inp, &acc1, bias, nibmask);
        }

        /* Remainder: 0-3 blocks */
        for (uint32_t b = 0; b < remainder; b++) {
            process_q4_block(&w, &inp, &acc0, bias, nibmask);
        }

        out[r] = hsum256(_mm256_add_ps(acc0, acc1));
    }
}

/* ══════════════════════════════════════════════════════════
 *  AVX2 + F16C matvec_f16 — for brandon-tiny and any model with
 *  raw fp16 weight tensors. _mm256_cvtph_ps converts 8 fp16 → 8
 *  fp32 in a single hardware instruction (vcvtph2ps), avoiding
 *  the ~5-op scalar dequant in inner loops.
 * ══════════════════════════════════════════════════════════ */

void __hot matvec_f16_avx2(float *out, const void *weight,
                           const float *input, uint32_t rows, uint32_t cols)
{
    const uint16_t *w = (const uint16_t *)weight;
    uint32_t c8 = cols & ~7u;        /* multiple of 8 */

    for (uint32_t r = 0; r < rows; r++) {
        const uint16_t *row = w + (uint64_t)r * cols;
        __m256 acc = _mm256_setzero_ps();

        for (uint32_t c = 0; c < c8; c += 8) {
            __m128i hv = _mm_loadu_si128((const __m128i *)(row + c));
            __m256 wv = _mm256_cvtph_ps(hv);
            __m256 iv = _mm256_loadu_ps(input + c);
            acc = _mm256_fmadd_ps(wv, iv, acc);
        }

        float sum = hsum256(acc);
        /* Tail: scalar dequant for any leftover < 8 */
        for (uint32_t c = c8; c < cols; c++) {
            __m128i sh = _mm_cvtsi32_si128(row[c]);
            float wf = _mm_cvtss_f32(_mm_cvtph_ps(sh));
            sum += wf * input[c];
        }
        out[r] = sum;
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
