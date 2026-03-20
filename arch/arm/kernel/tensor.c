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

/* ── External functions ──────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── AVX2 Detection + Enable ────────────────────────── */

static int avx2_detected = -1;  /* -1 = not checked yet */

int tensor_avx2_detect(void)
{
    /* No AVX2 on AArch64 — use scalar C fallback */
    avx2_detected = 0;
    return 0;
}

int tensor_has_avx2(void)
{
    if (avx2_detected < 0)
        tensor_avx2_detect();
    return avx2_detected;
}

/* ── Utility helpers ─────────────────────────────────── */

static inline uint64_t rdtsc(void)
{
    uint64_t cnt;
    __asm__ volatile ("mrs %0, CNTPCT_EL0" : "=r"(cnt));
    return cnt;
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
            /* Denormalized: shift mantissa until hidden bit appears */
            exp = 1;
            while (!(man & 0x400)) {
                man <<= 1;
                exp++;
            }
            man &= 0x3FF;
            bits = sign | (((uint32_t)(127 - 15 + 1 - exp)) << 23) | (man << 13);
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

/* Software sqrtf — Newton-Raphson (no x87/SSE on ARM64) */
float sqrtf_bare(float x)
{
    if (x <= 0.0f) return 0.0f;
    /* Fast inverse sqrt (Quake trick) + refinement */
    union { float f; uint32_t i; } u = { .f = x };
    u.i = 0x5f3759df - (u.i >> 1);
    float y = u.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;  /* sqrt(x) = x * rsqrt(x) */
}

/* Software expf — Pade approximation */
float expf_bare(float x)
{
    if (x > 88.0f) return 3.4028235e+38f;
    if (x < -88.0f) return 0.0f;
    /* Range reduction: exp(x) = 2^n * exp(f) where f in [-0.5, 0.5] */
    float t = x * 1.4426950408889634f;  /* x * log2(e) */
    int n = (int)(t + (t >= 0 ? 0.5f : -0.5f));
    float f = x - (float)n * 0.6931471805599453f;  /* f = x - n*ln(2) */
    /* Taylor: exp(f) ≈ 1 + f + f²/2 + f³/6 + f⁴/24 + f⁵/120 */
    float ef = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.04166667f + f * 0.00833333f))));
    /* Scale by 2^n via IEEE754 exponent manipulation */
    union { float f; uint32_t i; } scale = { .i = (uint32_t)(127 + n) << 23 };
    return ef * scale.f;
}

/* Software sinf — Taylor series (for RoPE, not hot path) */
float sinf_bare(float x)
{
    /* Reduce to [-pi, pi] */
    while (x >  3.14159265f) x -= 6.28318530f;
    while (x < -3.14159265f) x += 6.28318530f;
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.00019841f)));
}

/* Software cosf — Taylor series */
float cosf_bare(float x)
{
    while (x >  3.14159265f) x -= 6.28318530f;
    while (x < -3.14159265f) x += 6.28318530f;
    float x2 = x * x;
    return 1.0f - x2 * (0.5f - x2 * (0.04166667f - x2 * (0.00138889f - x2 * 0.0000248f)));
}

/* Software powf: base^exponent = exp(exponent * ln(base)) */
float powf_bare(float base, float exponent)
{
    if (base <= 0.0f) return 0.0f;
    /* ln(base) via log2(base) * ln(2) */
    /* Approximate log2 using IEEE754 exponent */
    union { float f; uint32_t i; } u = { .f = base };
    float log2_base = (float)((int)(u.i >> 23) - 127) + (float)(u.i & 0x7FFFFF) / 8388608.0f;
    float ln_base = log2_base * 0.6931471805599453f;
    return expf_bare(exponent * ln_base);
}

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
            dst[i + j * 2]     = ((int)(p[j] & 0xF) - 8) * scale;
            dst[i + j * 2 + 1] = ((int)(p[j] >> 4)  - 8) * scale;
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
                sum += scale * (lo * inp[j * 2] + hi * inp[j * 2 + 1]);
            }
            w += 16;
        }
        out[r] = sum;
    }
}

void matvec_q4_0(float *out, const void *weight,
                 const float *input, uint32_t rows, uint32_t cols)
{
    if (tensor_has_avx2())
        matvec_q4_0_avx2(out, weight, input, rows, cols);
    else
        matvec_q4_0_scalar(out, weight, input, rows, cols);
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
    for (uint32_t h = 0; h < n_heads; h++) {
        float *head = vec + h * head_dim;
        for (uint32_t i = 0; i < head_dim; i += 2) {
            float freq = 1.0f / powf_bare(theta, (float)i / (float)head_dim);
            float angle = (float)pos * freq;
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

        uint8_t *weights = (uint8_t *)mem_alloc_pages(w_pg);
        float   *input   = (float *)mem_alloc_pages(v_pg);
        float   *output  = (float *)mem_alloc_pages(v_pg);
        float   *ref     = (float *)mem_alloc_pages(v_pg);
        float   *tmp     = (float *)mem_alloc_pages(v_pg);

        if (weights && input && output && ref && tmp) {
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

        if (weights) mem_free_pages(weights, w_pg);
        if (input)   mem_free_pages(input, v_pg);
        if (output)  mem_free_pages(output, v_pg);
        if (ref)     mem_free_pages(ref, v_pg);
        if (tmp)     mem_free_pages(tmp, v_pg);
    }

    /* ── Test 5: Q8_0 matvec 256x256 ─────────────────── */
    {
        uint32_t rows = 256, cols = 256;
        uint32_t w_bytes = rows * (cols / 32) * Q8_0_BLOCK_SIZE;
        uint32_t w_pg   = pages_for(w_bytes);
        uint32_t v_pg   = pages_for(cols * sizeof(float));

        uint8_t *weights = (uint8_t *)mem_alloc_pages(w_pg);
        float   *input   = (float *)mem_alloc_pages(v_pg);
        float   *output  = (float *)mem_alloc_pages(v_pg);
        float   *ref     = (float *)mem_alloc_pages(v_pg);
        float   *tmp     = (float *)mem_alloc_pages(v_pg);

        if (weights && input && output && ref && tmp) {
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

        if (weights) mem_free_pages(weights, w_pg);
        if (input)   mem_free_pages(input, v_pg);
        if (output)  mem_free_pages(output, v_pg);
        if (ref)     mem_free_pages(ref, v_pg);
        if (tmp)     mem_free_pages(tmp, v_pg);
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

        uint8_t *weights = (uint8_t *)mem_alloc_pages(w_pg);
        float   *input   = (float *)mem_alloc_pages(v_pg);
        float   *output  = (float *)mem_alloc_pages(v_pg);

        if (weights && input && output) {
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

        if (weights) mem_free_pages(weights, w_pg);
        if (input)   mem_free_pages(input, v_pg);
        if (output)  mem_free_pages(output, v_pg);
    }

    /* ── Perf: AVX2 vs scalar comparison (if AVX2 available) ── */
    if (avx2) {
        uint32_t rows = 2048, cols = 2048;
        uint32_t w_bytes = rows * (cols / 32) * Q4_0_BLOCK_SIZE;
        uint32_t w_pg   = pages_for(w_bytes);
        uint32_t v_pg   = pages_for(cols * sizeof(float));

        uint8_t *weights = (uint8_t *)mem_alloc_pages(w_pg);
        float   *input2  = (float *)mem_alloc_pages(v_pg);
        float   *out_s   = (float *)mem_alloc_pages(v_pg);
        float   *out_a   = (float *)mem_alloc_pages(v_pg);

        if (weights && input2 && out_s && out_a) {
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

        if (weights) mem_free_pages(weights, w_pg);
        if (input2)  mem_free_pages(input2, v_pg);
        if (out_s)   mem_free_pages(out_s, v_pg);
        if (out_a)   mem_free_pages(out_a, v_pg);
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
