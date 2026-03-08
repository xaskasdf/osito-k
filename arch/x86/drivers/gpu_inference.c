/*
 * OsitoK x86-64 — GPU-Accelerated Llama Inference (X40 + X-INF1/2/3)
 *
 * Hybrid GPU/CPU forward pass orchestration.
 *
 * Three tiers (auto-selected based on kernel + VRAM availability):
 *   X-INF3: VRAM-resident weights + activations. All model weights
 *           uploaded to VRAM once at boot (~18s). Zero PCIe transfers
 *           for matvec/rmsnorm — only attention KV via PCIe.
 *   X-INF2: VRAM-resident activations, weights per-dispatch.
 *   X-INF1: Per-op upload/dispatch/download, CPU fallback.
 *
 * ── Data flow ─────────────────────────────────────────────
 *
 *   X-INF3 (VRAM weights):
 *     Weights:      VRAM (uploaded once at boot, ~18s via PRAMIN)
 *     Activations:  VRAM (persistent, X-INF2)
 *     KV cache:     System RAM (per-layer, per-position)
 *     GPU transfer: Weights 0, activations ~0, only KV+attention via PCIe
 *
 *   X-INF1 fallback:
 *     Weights:      System RAM → VRAM per dispatch
 *     Activations:  System RAM (scratch buffers)
 *     GPU transfer: PRAMIN upload → GPU compute → PRAMIN download
 *
 * ── Dispatch table (Llama 3.2 1B, per token) ──────────────
 *
 *   Operation           Calls/token   Compute     Dispatch
 *   ──────────────────  ───────────   ──────────  ──────────
 *   matvec_q4_0         7×16 = 112    ~90% cost   GPU (streaming)
 *   rmsnorm             3×16+1 = 49   ~2%         GPU
 *   softmax             32×16 = 512   ~3%         CPU (per-head)
 *   rope                1×16 = 16     ~1%         GPU (q+k combined)
 *   silu_mul             1×16 = 16    ~1%         GPU (fused)
 *   vec_add             2×16 = 32     ~1%         GPU (chunked)
 *   attention (QKV)     32×16 = 512   ~1%         CPU-only
 */

#include "gpu_inference.h"
#include "../kernel/tensor.h"
#include "gpu.h"
#include "gpu_tensor.h"
#include "sass.h"

/* ── External Functions ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

/* ── Utility ─────────────────────────────────────────────── */

static inline uint64_t rdtsc_gpu(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Token embedding (CPU-only, table lookup) ────────────── */

static void gpu_embed_token(float *dst, gguf_tensor_t *embd,
                            uint32_t token, uint32_t dim)
{
    /* Embedding is a table lookup — not parallelizable on GPU.
     * Stays on CPU regardless of dispatch table. */
    switch (embd->type) {
    case GGML_TYPE_Q4_0: {
        uint32_t blocks = dim / 32;
        uint64_t row_bytes = (uint64_t)blocks * 18;
        const void *row = (const uint8_t *)embd->data + token * row_bytes;
        dequant_q4_0(row, dst, dim);
        break;
    }
    case GGML_TYPE_Q8_0: {
        uint32_t blocks = dim / 32;
        uint64_t row_bytes = (uint64_t)blocks * 34;
        const void *row = (const uint8_t *)embd->data + token * row_bytes;
        dequant_q8_0(row, dst, dim);
        break;
    }
    case GGML_TYPE_F16: {
        const uint16_t *src = (const uint16_t *)embd->data + (uint64_t)token * dim;
        for (uint32_t i = 0; i < dim; i++)
            dst[i] = f16_to_f32(src[i]);
        break;
    }
    case GGML_TYPE_F32: {
        const float *src = (const float *)embd->data + (uint64_t)token * dim;
        memcpy(dst, src, dim * sizeof(float));
        break;
    }
    default:
        memset(dst, 0, dim * sizeof(float));
        break;
    }
}

/* ── VRAM bump allocator save/restore ────────────────────── */

/* Save/restore pattern prevents VRAM leaks from per-dispatch temp
 * allocations (data buffers + CB0 inside gpu_dispatch_kernel).
 * The permanent scratch buffers (vram_a/b/out) stay at the start
 * of the region and are never reclaimed. */

static inline uint64_t vram_save(void)
{
    gpu_tensor_state_t *ts = gpu_tensor_get_state();
    return ts ? ts->vram_next : 0;
}

static inline void vram_restore(uint64_t saved)
{
    gpu_tensor_state_t *ts = gpu_tensor_get_state();
    if (ts) ts->vram_next = saved;
}

/* ── VRAM weight cache (X-INF3) ──────────────────────────── */

/* Look up a weight in the VRAM cache by its system RAM pointer.
 * Returns VRAM address or 0 if not cached. */
static uint64_t wcache_lookup(gpu_llama_state_t *gs, const void *host_ptr)
{
    if (!gs->weights_resident) return 0;
    for (uint32_t i = 0; i < gs->wcache.count; i++) {
        if (gs->wcache.entries[i].host_ptr == host_ptr)
            return gs->wcache.entries[i].vram_addr;
    }
    return 0;
}

/* Add a weight to the VRAM cache. Allocates from bump allocator and uploads. */
static uint64_t wcache_add(gpu_llama_state_t *gs, const void *host_ptr,
                            uint32_t size)
{
    if (gs->wcache.count >= MAX_WEIGHT_CACHE) return 0;

    uint64_t vram = gpu_tensor_alloc(size);
    if (!vram) return 0;

    if (gpu_tensor_upload(vram, host_ptr, size) < 0) return 0;

    weight_entry_t *e = &gs->wcache.entries[gs->wcache.count++];
    e->host_ptr  = host_ptr;
    e->vram_addr = vram;
    e->size      = size;
    gs->wcache.total_bytes += size;

    return vram;
}

/* Upload all model weights to VRAM at boot time.
 * Layer weights (~523MB for Llama 3.2 1B) + output (~141MB). */
static int gpu_upload_all_weights(gpu_llama_state_t *gs)
{
    llama_state_t *s = gs->cpu;
    uint32_t dim = s->dim;
    uint32_t kv_dim = s->kv_dim;
    uint32_t ffn_dim = s->ffn_dim;

    serial_puts("[GPU_INF] X-INF3: Uploading model weights to VRAM...\n");
    uint64_t t0 = rdtsc_gpu();

    gs->wcache.count = 0;
    gs->wcache.total_bytes = 0;

    /* Helper: compute Q4_0 tensor byte size */
    #define Q4_SIZE(rows, cols)  ((uint32_t)((cols) / 32) * 18 * (rows))
    #define F32_SIZE(n)          ((uint32_t)((n) * sizeof(float)))

    /* Upload per-layer weights */
    for (uint32_t l = 0; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        /* Attention weights (Q4_0) */
        if (ly->attn_q && ly->attn_q->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->attn_q->data, Q4_SIZE(dim, dim)))
                goto fail;
        }
        if (ly->attn_k && ly->attn_k->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->attn_k->data, Q4_SIZE(kv_dim, dim)))
                goto fail;
        }
        if (ly->attn_v && ly->attn_v->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->attn_v->data, Q4_SIZE(kv_dim, dim)))
                goto fail;
        }
        if (ly->attn_output && ly->attn_output->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->attn_output->data, Q4_SIZE(dim, dim)))
                goto fail;
        }

        /* FFN weights (Q4_0) */
        if (ly->ffn_gate && ly->ffn_gate->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->ffn_gate->data, Q4_SIZE(ffn_dim, dim)))
                goto fail;
        }
        if (ly->ffn_up && ly->ffn_up->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->ffn_up->data, Q4_SIZE(ffn_dim, dim)))
                goto fail;
        }
        if (ly->ffn_down && ly->ffn_down->type == GGML_TYPE_Q4_0) {
            if (!wcache_add(gs, ly->ffn_down->data, Q4_SIZE(dim, ffn_dim)))
                goto fail;
        }

        /* Norm weights (F32) */
        if (ly->attn_norm) {
            if (!wcache_add(gs, ly->attn_norm->data, F32_SIZE(dim)))
                goto fail;
        }
        if (ly->ffn_norm) {
            if (!wcache_add(gs, ly->ffn_norm->data, F32_SIZE(dim)))
                goto fail;
        }

        /* Progress every 4 layers */
        if ((l & 3) == 3 || l == s->n_layers - 1) {
            serial_puts("[GPU_INF]   Layer ");
            serial_putdec(l + 1);
            serial_puts("/");
            serial_putdec(s->n_layers);
            serial_puts(": ");
            serial_putdec(gs->wcache.total_bytes / (1024 * 1024));
            serial_puts("MB uploaded\n");
        }
    }

    /* Output norm (F32) */
    if (s->weights.output_norm) {
        if (!wcache_add(gs, s->weights.output_norm->data, F32_SIZE(dim)))
            goto fail;
    }

    /* Output projection (Q4_0) — enables GPU logits matvec */
    if (s->weights.output && s->weights.output->type == GGML_TYPE_Q4_0) {
        uint32_t out_size = Q4_SIZE(s->vocab_size, dim);
        if (!wcache_add(gs, s->weights.output->data, out_size))
            serial_puts("[GPU_INF]   Output matrix too large for VRAM, CPU fallback\n");
        /* Non-fatal: logits can still use CPU */
    }

    #undef Q4_SIZE
    #undef F32_SIZE

    uint64_t elapsed_ms = (rdtsc_gpu() - t0) / 3000000;

    serial_puts("[GPU_INF] X-INF3: ");
    serial_putdec(gs->wcache.count);
    serial_puts(" tensors, ");
    serial_putdec(gs->wcache.total_bytes / (1024 * 1024));
    serial_puts("MB in VRAM (");
    serial_putdec(elapsed_ms / 1000);
    serial_puts(".");
    serial_putdec((elapsed_ms % 1000) / 100);
    serial_puts("s)\n");

    gs->wcache.ready = true;
    gs->weights_resident = true;
    return 0;

fail:
    serial_puts("[GPU_INF] X-INF3: VRAM full at ");
    serial_putdec(gs->wcache.total_bytes / (1024 * 1024));
    serial_puts("MB (");
    serial_putdec(gs->wcache.count);
    serial_puts(" tensors). Weights NOT resident.\n");
    /* Weights already uploaded are still usable for partial acceleration,
     * but we won't set weights_resident = true. Fall back to per-dispatch. */
    gs->wcache.ready = true;  /* partial cache still works */
    return -1;
}

/* ── Matvec dispatch (CPU, type-aware) ───────────────────── */

/* Attempt GPU matvec_q4_0 dispatch (host-dispatch mode).
 * X-INF3: uses weight cache if available (skip weight upload).
 * Otherwise uploads weights + input to VRAM, dispatches, downloads output.
 * Returns 0 on success, -1 on failure (caller should fallback to CPU). */
static int gpu_matvec_q4_0_dispatch(float *out, const void *weights,
                                     const float *input,
                                     uint32_t rows, uint32_t cols,
                                     gpu_llama_state_t *gs)
{
    uint32_t blocks_per_row = cols / 32;
    uint32_t row_bytes = blocks_per_row * 18;
    uint32_t weight_bytes = rows * row_bytes;
    uint32_t input_bytes = cols * sizeof(float);
    uint32_t output_bytes = rows * sizeof(float);

    uint64_t saved = vram_save();

    /* X-INF3: check weight cache — skip weight upload */
    uint64_t w_vram = wcache_lookup(gs, weights);

    if (!w_vram) {
        /* No cache: allocate + upload weights */
        uint32_t total_needed = weight_bytes + input_bytes + output_bytes + 768;
        if (total_needed > 16 * 1024 * 1024) { vram_restore(saved); return -1; }

        w_vram = gpu_tensor_alloc(weight_bytes);
        if (!w_vram) { vram_restore(saved); return -1; }
        if (gpu_tensor_upload(w_vram, weights, weight_bytes) < 0) { vram_restore(saved); return -1; }
    }

    uint64_t x_vram = gpu_tensor_alloc(input_bytes);
    uint64_t y_vram = gpu_tensor_alloc(output_bytes);
    if (!x_vram || !y_vram) { vram_restore(saved); return -1; }

    if (gpu_tensor_upload(x_vram, input, input_bytes) < 0) { vram_restore(saved); return -1; }

    int ret = gpu_gemv_q4_0_ptx(y_vram, w_vram, x_vram, rows, cols);
    if (ret < 0) { vram_restore(saved); return -1; }

    if (gpu_tensor_download(y_vram, out, output_bytes) < 0) { vram_restore(saved); return -1; }

    vram_restore(saved);
    return 0;
}

static void gpu_matvec(float *out, gguf_tensor_t *tensor,
                       const float *input, uint32_t rows, uint32_t cols,
                       gpu_llama_state_t *gs)
{
    uint64_t t0 = rdtsc_gpu();

    /* Try GPU dispatch for Q4_0 when kernel is available */
    if (tensor->type == GGML_TYPE_Q4_0 && gs->kernels.matvec_q4_0) {
        if (gpu_matvec_q4_0_dispatch(out, tensor->data, input,
                                      rows, cols, gs) == 0) {
            gs->gpu_cycles += rdtsc_gpu() - t0;
            gs->gpu_ops++;
            return;
        }
        /* GPU dispatch failed — fall through to CPU */
    }

    /* CPU fallback */
    switch (tensor->type) {
    case GGML_TYPE_Q4_0:
        matvec_q4_0(out, tensor->data, input, rows, cols);
        break;
    case GGML_TYPE_Q8_0:
        matvec_q8_0(out, tensor->data, input, rows, cols);
        break;
    case GGML_TYPE_F32: {
        const float *w = (const float *)tensor->data;
        for (uint32_t r = 0; r < rows; r++) {
            float sum = 0.0f;
            for (uint32_t c = 0; c < cols; c++)
                sum += w[r * cols + c] * input[c];
            out[r] = sum;
        }
        break;
    }
    default:
        memset(out, 0, rows * sizeof(float));
        break;
    }

    gs->cpu_cycles += rdtsc_gpu() - t0;
    gs->cpu_ops++;
}

/* ── GPU vec_add (chunked, 256 elements per dispatch) ────── */

#define GPU_VEC_ADD_CHUNK  256

static void gpu_vec_add_dispatch(float *out, const float *a, const float *b,
                                 uint32_t n, gpu_llama_state_t *gs)
{
    if (!gs->kernels.vec_add || !gs->vram_a) {
        /* CPU fallback */
        uint64_t t0 = rdtsc_gpu();
        vec_add(out, a, b, n);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* GPU dispatch: chunk into 256-element blocks */
    uint64_t t0 = rdtsc_gpu();
    uint64_t saved = vram_save();
    uint32_t offset = 0;

    while (offset < n) {
        uint32_t chunk = n - offset;
        if (chunk > GPU_VEC_ADD_CHUNK)
            chunk = GPU_VEC_ADD_CHUNK;

        uint32_t bytes = chunk * sizeof(float);

        /* Upload a[offset..] and b[offset..] to VRAM */
        gpu_tensor_upload(gs->vram_a, a + offset, bytes);
        gpu_tensor_upload(gs->vram_b, b + offset, bytes);

        /* Dispatch GPU vec_add (CB0 allocated inside, reclaimed by restore) */
        int ret = gpu_vec_add(gs->vram_out, gs->vram_a, gs->vram_b, chunk);
        if (ret < 0) {
            vram_restore(saved);
            vec_add(out + offset, a + offset, b + offset, n - offset);
            gs->cpu_ops++;
            gs->gpu_cycles += rdtsc_gpu() - t0;
            return;
        }

        /* Download result */
        gpu_tensor_download(gs->vram_out, out + offset, bytes);
        offset += chunk;

        /* Reclaim CB0 allocations between chunks */
        vram_restore(saved);
    }

    gs->gpu_cycles += rdtsc_gpu() - t0;
    gs->gpu_ops++;
}

/* ── GPU rmsnorm dispatch ────────────────────────────────── */

static void gpu_rmsnorm_dispatch(float *out, const float *x,
                                  const float *weight, uint32_t dim,
                                  gpu_llama_state_t *gs)
{
    if (!gs->kernels.rmsnorm || !gs->vram_a) {
        uint64_t t0 = rdtsc_gpu();
        rmsnorm(out, x, weight, dim);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    uint64_t t0 = rdtsc_gpu();
    uint32_t bytes = dim * sizeof(float);
    uint64_t saved = vram_save();

    /* Upload x and weight to VRAM scratch */
    if (gpu_tensor_upload(gs->vram_a, x, bytes) < 0 ||
        gpu_tensor_upload(gs->vram_b, weight, bytes) < 0) {
        vram_restore(saved);
        rmsnorm(out, x, weight, dim);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* Dispatch rmsnorm kernel (eps=1e-5) */
    int ret = gpu_rmsnorm_ptx(gs->vram_out, gs->vram_a, gs->vram_b, dim, 1e-5f);
    if (ret < 0) {
        vram_restore(saved);
        rmsnorm(out, x, weight, dim);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* Download result */
    if (gpu_tensor_download(gs->vram_out, out, bytes) < 0) {
        vram_restore(saved);
        rmsnorm(out, x, weight, dim);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    vram_restore(saved);
    gs->gpu_cycles += rdtsc_gpu() - t0;
    gs->gpu_ops++;
}

/* ── GPU silu_mul dispatch (fused SiLU(gate) * up) ──────── */

static void gpu_silu_mul_dispatch(float *out, float *gate, const float *up,
                                   uint32_t n, gpu_llama_state_t *gs)
{
    if (!gs->kernels.silu || !gs->vram_a) {
        uint64_t t0 = rdtsc_gpu();
        silu_inplace(gate, n);
        vec_mul(out, gate, up, n);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    uint64_t t0 = rdtsc_gpu();
    uint32_t bytes = n * sizeof(float);
    uint64_t saved = vram_save();

    /* Upload gate and up to VRAM */
    if (gpu_tensor_upload(gs->vram_a, gate, bytes) < 0 ||
        gpu_tensor_upload(gs->vram_b, up, bytes) < 0) {
        vram_restore(saved);
        silu_inplace(gate, n);
        vec_mul(out, gate, up, n);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* Dispatch fused silu_mul kernel: out[i] = SiLU(gate[i]) * up[i] */
    int ret = gpu_silu_mul_ptx(gs->vram_out, gs->vram_a, gs->vram_b, n);
    if (ret < 0) {
        vram_restore(saved);
        silu_inplace(gate, n);
        vec_mul(out, gate, up, n);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* Download result */
    if (gpu_tensor_download(gs->vram_out, out, bytes) < 0) {
        vram_restore(saved);
        silu_inplace(gate, n);
        vec_mul(out, gate, up, n);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    vram_restore(saved);
    gs->gpu_cycles += rdtsc_gpu() - t0;
    gs->gpu_ops++;
}

/* ── GPU RoPE dispatch (in-place on q and k) ────────────── */

static void gpu_rope_dispatch(float *q, float *k,
                               uint32_t n_heads, uint32_t n_kv_heads,
                               uint32_t head_dim, uint32_t pos,
                               float theta, gpu_llama_state_t *gs)
{
    if (!gs->kernels.rope || !gs->vram_a) {
        uint64_t t0 = rdtsc_gpu();
        rope(q, n_heads,    head_dim, pos, theta);
        rope(k, n_kv_heads, head_dim, pos, theta);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    uint64_t t0 = rdtsc_gpu();
    uint32_t q_bytes = n_heads * head_dim * sizeof(float);
    uint32_t k_bytes = n_kv_heads * head_dim * sizeof(float);
    uint64_t saved = vram_save();

    /* Upload q and k to VRAM */
    if (gpu_tensor_upload(gs->vram_a, q, q_bytes) < 0 ||
        gpu_tensor_upload(gs->vram_b, k, k_bytes) < 0) {
        vram_restore(saved);
        rope(q, n_heads,    head_dim, pos, theta);
        rope(k, n_kv_heads, head_dim, pos, theta);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* Dispatch rope kernel (processes both q and k) */
    int ret = gpu_rope_ptx(gs->vram_a, gs->vram_b, pos,
                           n_heads, n_kv_heads, head_dim, theta);
    if (ret < 0) {
        vram_restore(saved);
        rope(q, n_heads,    head_dim, pos, theta);
        rope(k, n_kv_heads, head_dim, pos, theta);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    /* Download modified q and k back */
    if (gpu_tensor_download(gs->vram_a, q, q_bytes) < 0 ||
        gpu_tensor_download(gs->vram_b, k, k_bytes) < 0) {
        vram_restore(saved);
        rope(q, n_heads,    head_dim, pos, theta);
        rope(k, n_kv_heads, head_dim, pos, theta);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
        return;
    }

    vram_restore(saved);
    gs->gpu_cycles += rdtsc_gpu() - t0;
    gs->gpu_ops++;
}

/* ── Norm data helper ────────────────────────────────────── */

static inline const float *gpu_norm_data(gguf_tensor_t *tensor)
{
    return (const float *)tensor->data;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_init
 * ══════════════════════════════════════════════════════════ */

int gpu_llama_init(gpu_llama_state_t *gs, llama_state_t *cpu_state)
{
    serial_puts("\n[GPU_INF] == X40: GPU-Accelerated Inference ==\n");

    memset(gs, 0, sizeof(*gs));
    gs->cpu = cpu_state;

    /* Detect available GPU kernels */
    sass_kernel_t *k_vec_add = sass_get_kernel("vec_add_f32");
    if (k_vec_add && k_vec_add->uploaded) {
        gs->kernels.vec_add = true;
        serial_puts("[GPU_INF] Kernel: vec_add_f32 (GPU)\n");
    }

    /* Compiled PTX kernel detection */
    sass_kernel_t *k_matvec = sass_get_kernel("gemv_q4_0");
    if (k_matvec && k_matvec->uploaded)
        gs->kernels.matvec_q4_0 = true;

    sass_kernel_t *k_rmsnorm = sass_get_kernel("rmsnorm");
    if (k_rmsnorm && k_rmsnorm->uploaded)
        gs->kernels.rmsnorm = true;

    sass_kernel_t *k_softmax = sass_get_kernel("softmax");
    if (k_softmax && k_softmax->uploaded)
        gs->kernels.softmax = true;

    sass_kernel_t *k_silu = sass_get_kernel("silu_mul");
    if (k_silu && k_silu->uploaded)
        gs->kernels.silu = true;

    sass_kernel_t *k_rope = sass_get_kernel("rope");
    if (k_rope && k_rope->uploaded)
        gs->kernels.rope = true;

    sass_kernel_t *k_addinp = sass_get_kernel("add_inplace");
    if (k_addinp && k_addinp->uploaded)
        gs->kernels.add_inplace = true;

    /* Count available GPU kernels */
    uint32_t gpu_count = 0;
    if (gs->kernels.vec_add)      gpu_count++;
    if (gs->kernels.matvec_q4_0)  gpu_count++;
    if (gs->kernels.rmsnorm)      gpu_count++;
    if (gs->kernels.softmax)      gpu_count++;
    if (gs->kernels.silu)         gpu_count++;
    if (gs->kernels.rope)         gpu_count++;
    if (gs->kernels.add_inplace)  gpu_count++;

    serial_puts("[GPU_INF] GPU kernels: ");
    serial_putdec(gpu_count);
    serial_puts("/8, CPU fallback: ");
    serial_putdec(8 - gpu_count);
    serial_puts("/8\n");

    /* Allocate VRAM scratch buffers for host-dispatch mode (X-INF1).
     * Each buffer holds max(dim, ffn_dim) floats.
     * dim=2048 → 8KB, ffn_dim=8192 → 32KB. Use ffn_dim as worst case. */
    uint32_t dim = cpu_state->dim;
    uint32_t kv_dim = cpu_state->kv_dim;
    uint32_t ffn_dim = cpu_state->ffn_dim;
    uint32_t max_dim = ffn_dim;
    if (dim > max_dim) max_dim = dim;
    gs->vram_buf_size = max_dim * sizeof(float);

    gpu_tensor_state_t *ts = gpu_tensor_get_state();
    if (ts && ts->initialized) {
        gs->vram_a   = gpu_tensor_alloc(gs->vram_buf_size);
        gs->vram_b   = gpu_tensor_alloc(gs->vram_buf_size);
        gs->vram_out = gpu_tensor_alloc(gs->vram_buf_size);

        if (gs->vram_a && gs->vram_b && gs->vram_out) {
            serial_puts("[GPU_INF] VRAM scratch: 3 x ");
            serial_putdec(gs->vram_buf_size / 1024);
            serial_puts("KB at 0x");
            serial_puthex(gs->vram_a, 8);
            serial_puts("\n");
        } else {
            serial_puts("[GPU_INF] VRAM scratch alloc failed, GPU ops disabled\n");
            memset(&gs->kernels, 0, sizeof(gs->kernels));
        }

        /* Allocate VRAM-resident activation buffers (X-INF2).
         * Llama 3.2 1B: dim=2048, kv_dim=512, ffn_dim=8192.
         * Total: ~116KB. Persists across layers — no per-op transfers. */
        gs->acts.x   = gpu_tensor_alloc(dim * sizeof(float));
        gs->acts.xb  = gpu_tensor_alloc(dim * sizeof(float));
        gs->acts.xb2 = gpu_tensor_alloc(dim * sizeof(float));
        gs->acts.q   = gpu_tensor_alloc(dim * sizeof(float));
        gs->acts.k   = gpu_tensor_alloc(kv_dim * sizeof(float));
        gs->acts.v   = gpu_tensor_alloc(kv_dim * sizeof(float));
        gs->acts.hb  = gpu_tensor_alloc(ffn_dim * sizeof(float));
        gs->acts.hb2 = gpu_tensor_alloc(ffn_dim * sizeof(float));

        bool acts_ok = gs->acts.x && gs->acts.xb && gs->acts.xb2 &&
                       gs->acts.q && gs->acts.k && gs->acts.v &&
                       gs->acts.hb && gs->acts.hb2;

        /* VRAM-resident mode requires all core kernels + activation buffers */
        gs->vram_resident = acts_ok &&
                            gs->kernels.matvec_q4_0 &&
                            gs->kernels.rmsnorm &&
                            gs->kernels.silu &&
                            gs->kernels.rope &&
                            gs->kernels.add_inplace;

        if (acts_ok) {
            uint32_t acts_kb = (dim * 4 + dim * 4 + dim * 4 + dim * 4 +
                                kv_dim * 4 + kv_dim * 4 +
                                ffn_dim * 4 + ffn_dim * 4) / 1024;
            serial_puts("[GPU_INF] VRAM activations: ");
            serial_putdec(acts_kb);
            serial_puts("KB (x,xb,xb2,q,k,v,hb,hb2)\n");
        }

        /* X-INF3: Upload all model weights to VRAM (one-time boot cost) */
        if (gs->vram_resident && gs->kernels.matvec_q4_0) {
            gpu_upload_all_weights(gs);
        }
    } else {
        serial_puts("[GPU_INF] GPU tensor subsystem not ready, CPU-only mode\n");
    }

    /* Dispatch summary */
    const char *mode_str = "host-dispatch (X-INF1)";
    if (gs->vram_resident && gs->weights_resident)
        mode_str = "VRAM-resident weights+acts (X-INF3)";
    else if (gs->vram_resident)
        mode_str = "VRAM-resident acts (X-INF2)";
    serial_puts("[GPU_INF] Mode: ");
    serial_puts(mode_str);
    serial_puts("\n");

    serial_puts("[GPU_INF] Dispatch plan:\n");
    serial_puts("[GPU_INF]   matvec_q4_0: ");
    if (gs->weights_resident && gs->kernels.matvec_q4_0)
        serial_puts("GPU (VRAM weights)");
    else if (gs->kernels.matvec_q4_0)
        serial_puts("GPU (streaming)");
    else
        serial_puts("CPU (AVX2)");
    serial_puts(" [~90%]\n");
    serial_puts("[GPU_INF]   rmsnorm:     ");
    serial_puts(gs->kernels.rmsnorm ? (gs->weights_resident ? "GPU (VRAM)" : "GPU") : "CPU");
    serial_puts(" [49/tok]\n");
    serial_puts("[GPU_INF]   silu_mul:    ");
    serial_puts(gs->kernels.silu ? "GPU (fused)" : "CPU");
    serial_puts(" [16/tok]\n");
    serial_puts("[GPU_INF]   rope:        ");
    serial_puts(gs->kernels.rope ? "GPU (q+k)" : "CPU");
    serial_puts(" [16/tok]\n");
    serial_puts("[GPU_INF]   add_inplace: ");
    serial_puts(gs->kernels.add_inplace ? "GPU (residual)" : "CPU");
    serial_puts(" [32/tok]\n");
    serial_puts("[GPU_INF]   softmax:     CPU (per-head) [512/tok]\n");
    serial_puts("[GPU_INF]   attention:   CPU (GQA loop)\n");
    if (gs->weights_resident) {
        serial_puts("[GPU_INF]   logits:      ");
        serial_puts(wcache_lookup(gs, gs->cpu->weights.output->data) ?
                     "GPU (VRAM)" : "CPU (AVX2)");
        serial_puts("\n");
    }

    gs->initialized = true;

    fb_puts(" GPU Inf: ");
    if (gs->weights_resident)
        fb_puts("VRAM-weights ");
    else if (gs->vram_resident)
        fb_puts("VRAM-acts ");
    fb_putdec(gpu_count);
    fb_puts("/8 kernels\n");

    serial_puts("[GPU_INF] == init complete ==\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  VRAM-native dispatch helpers (X-INF2/INF3)
 *
 *  These take VRAM addresses for activations.
 *  X-INF3: weights looked up from weight cache (zero transfer).
 *  X-INF2 fallback: weights uploaded per dispatch.
 *  CB0 allocations reclaimed via save/restore.
 * ══════════════════════════════════════════════════════════ */

/* Matvec with VRAM-resident input/output.
 * X-INF3: tries weight cache first (zero transfer).
 * Fallback: uploads weights per dispatch (X-INF2 behavior). */
static int gpu_matvec_vram(uint64_t out_vram, gguf_tensor_t *tensor,
                            uint64_t input_vram, uint32_t rows, uint32_t cols,
                            gpu_llama_state_t *gs)
{
    if (tensor->type != GGML_TYPE_Q4_0) return -1;

    uint32_t blocks_per_row = cols / 32;
    uint32_t row_bytes = blocks_per_row * 18;
    uint32_t weight_bytes = rows * row_bytes;

    uint64_t t0 = rdtsc_gpu();

    /* X-INF3: check weight cache — zero transfer path */
    uint64_t w_vram = wcache_lookup(gs, tensor->data);
    if (w_vram) {
        uint64_t saved = vram_save();
        int ret = gpu_gemv_q4_0_ptx(out_vram, w_vram, input_vram, rows, cols);
        vram_restore(saved);
        if (ret == 0) { gs->gpu_cycles += rdtsc_gpu() - t0; gs->gpu_ops++; }
        return ret;
    }

    /* Fallback: upload weights per dispatch */
    if (weight_bytes > 16 * 1024 * 1024) return -1;

    uint64_t saved = vram_save();
    w_vram = gpu_tensor_alloc(weight_bytes);
    if (!w_vram) { vram_restore(saved); return -1; }

    if (gpu_tensor_upload(w_vram, tensor->data, weight_bytes) < 0) {
        vram_restore(saved); return -1;
    }

    int ret = gpu_gemv_q4_0_ptx(out_vram, w_vram, input_vram, rows, cols);
    vram_restore(saved);

    if (ret == 0) { gs->gpu_cycles += rdtsc_gpu() - t0; gs->gpu_ops++; }
    return ret;
}

/* Rmsnorm with VRAM-resident x.
 * X-INF3: tries weight cache first (zero transfer). */
static int gpu_rmsnorm_vram(uint64_t out_vram, uint64_t x_vram,
                             const float *weight_ram, uint32_t dim,
                             gpu_llama_state_t *gs)
{
    uint64_t t0 = rdtsc_gpu();
    uint32_t bytes = dim * sizeof(float);

    /* X-INF3: check weight cache */
    uint64_t w_vram = wcache_lookup(gs, weight_ram);
    if (w_vram) {
        uint64_t saved = vram_save();
        int ret = gpu_rmsnorm_ptx(out_vram, x_vram, w_vram, dim, 1e-5f);
        vram_restore(saved);
        if (ret == 0) { gs->gpu_cycles += rdtsc_gpu() - t0; gs->gpu_ops++; }
        return ret;
    }

    /* Fallback: upload weight per dispatch */
    uint64_t saved = vram_save();
    w_vram = gpu_tensor_alloc(bytes);
    if (!w_vram) { vram_restore(saved); return -1; }

    if (gpu_tensor_upload(w_vram, weight_ram, bytes) < 0) {
        vram_restore(saved); return -1;
    }

    int ret = gpu_rmsnorm_ptx(out_vram, x_vram, w_vram, dim, 1e-5f);
    vram_restore(saved);

    if (ret == 0) {
        gs->gpu_cycles += rdtsc_gpu() - t0;
        gs->gpu_ops++;
    }
    return ret;
}

/* SiLU*mul — all operands in VRAM, zero upload */
static int gpu_silu_mul_vram(uint64_t out_vram, uint64_t gate_vram,
                              uint64_t up_vram, uint32_t n,
                              gpu_llama_state_t *gs)
{
    uint64_t t0 = rdtsc_gpu();
    uint64_t saved = vram_save();
    int ret = gpu_silu_mul_ptx(out_vram, gate_vram, up_vram, n);
    vram_restore(saved);
    if (ret == 0) { gs->gpu_cycles += rdtsc_gpu() - t0; gs->gpu_ops++; }
    return ret;
}

/* RoPE — all in VRAM, in-place modification */
static int gpu_rope_vram(uint64_t q_vram, uint64_t k_vram,
                          uint32_t n_heads, uint32_t n_kv_heads,
                          uint32_t head_dim, uint32_t pos, float theta,
                          gpu_llama_state_t *gs)
{
    uint64_t t0 = rdtsc_gpu();
    uint64_t saved = vram_save();
    int ret = gpu_rope_ptx(q_vram, k_vram, pos, n_heads, n_kv_heads,
                           head_dim, theta);
    vram_restore(saved);
    if (ret == 0) { gs->gpu_cycles += rdtsc_gpu() - t0; gs->gpu_ops++; }
    return ret;
}

/* Add-inplace — all in VRAM: a[i] += b[i] */
static int gpu_add_inplace_vram(uint64_t a_vram, uint64_t b_vram,
                                 uint32_t n, gpu_llama_state_t *gs)
{
    uint64_t t0 = rdtsc_gpu();
    uint64_t saved = vram_save();
    int ret = gpu_add_inplace_ptx(a_vram, b_vram, n);
    vram_restore(saved);
    if (ret == 0) { gs->gpu_cycles += rdtsc_gpu() - t0; gs->gpu_ops++; }
    return ret;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_forward_vram — VRAM-resident forward pass (X-INF2)
 *
 *  Activations stay in VRAM between ops. Only weights are
 *  uploaded per dispatch. CPU attention downloads q/k/v,
 *  uploads xb2 back.
 * ══════════════════════════════════════════════════════════ */

static int gpu_llama_forward_vram(gpu_llama_state_t *gs, uint32_t token)
{
    llama_state_t *s = gs->cpu;
    gpu_vram_acts_t *a = &gs->acts;
    uint32_t dim     = s->dim;
    uint32_t kv_dim  = s->kv_dim;
    uint32_t hd      = s->head_dim;
    uint32_t pos     = s->pos;

    /* ── Embed token (CPU → VRAM) ── */
    gpu_embed_token(s->x, s->weights.token_embd, token, dim);
    gpu_tensor_upload(a->x, s->x, dim * sizeof(float));

    /* ── Transformer layers ── */
    for (uint32_t l = 0; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        /* Attention norm: x_vram → xb_vram (upload weight only) */
        gpu_rmsnorm_vram(a->xb, a->x, gpu_norm_data(ly->attn_norm), dim, gs);

        /* Q, K, V projections: xb_vram → q/k/v_vram (upload weights) */
        gpu_matvec_vram(a->q, ly->attn_q, a->xb, dim, dim, gs);
        gpu_matvec_vram(a->k, ly->attn_k, a->xb, kv_dim, dim, gs);
        gpu_matvec_vram(a->v, ly->attn_v, a->xb, kv_dim, dim, gs);

        /* RoPE: q/k_vram in-place (zero transfer) */
        gpu_rope_vram(a->q, a->k, s->n_heads, s->n_kv_heads,
                      hd, pos, 500000.0f, gs);

        /* ── Download q, k, v for CPU attention ── */
        gpu_tensor_download(a->q, s->q, dim * sizeof(float));
        gpu_tensor_download(a->k, s->k, kv_dim * sizeof(float));
        gpu_tensor_download(a->v, s->v, kv_dim * sizeof(float));

        /* Store K, V in cache (CPU) */
        float *kc = s->kv_cache[l].k + (uint64_t)pos * kv_dim;
        float *vc = s->kv_cache[l].v + (uint64_t)pos * kv_dim;
        memcpy(kc, s->k, kv_dim * sizeof(float));
        memcpy(vc, s->v, kv_dim * sizeof(float));

        /* ── GQA Attention (CPU — irregular access pattern) ── */
        float scale = 1.0f / sqrtf_bare((float)hd);
        uint64_t att_t0 = rdtsc_gpu();

        for (uint32_t h = 0; h < s->n_heads; h++) {
            uint32_t kv_h = h / s->gqa_ratio;
            float *q_head = s->q + h * hd;

            for (uint32_t p = 0; p <= pos; p++) {
                float *k_pos = s->kv_cache[l].k + (uint64_t)p * kv_dim + kv_h * hd;
                float dot = 0.0f;
                for (uint32_t i = 0; i < hd; i++)
                    dot += q_head[i] * k_pos[i];
                s->att[p] = dot * scale;
            }

            softmax(s->att, pos + 1);

            float *out_head = s->xb2 + h * hd;
            memset(out_head, 0, hd * sizeof(float));
            for (uint32_t p = 0; p <= pos; p++) {
                float *v_pos = s->kv_cache[l].v + (uint64_t)p * kv_dim + kv_h * hd;
                float aa = s->att[p];
                for (uint32_t i = 0; i < hd; i++)
                    out_head[i] += aa * v_pos[i];
            }
        }
        gs->cpu_cycles += rdtsc_gpu() - att_t0;
        gs->cpu_ops++;

        /* ── Upload attention output → VRAM ── */
        gpu_tensor_upload(a->xb2, s->xb2, dim * sizeof(float));

        /* Output projection: xb2_vram → xb_vram (upload weights) */
        gpu_matvec_vram(a->xb, ly->attn_output, a->xb2, dim, dim, gs);

        /* Residual: x_vram += xb_vram (zero transfer) */
        gpu_add_inplace_vram(a->x, a->xb, dim, gs);

        /* ── FFN (entire chain in VRAM) ── */
        gpu_rmsnorm_vram(a->xb, a->x, gpu_norm_data(ly->ffn_norm), dim, gs);

        /* Gate + Up: xb_vram → hb/hb2_vram (upload weights) */
        gpu_matvec_vram(a->hb,  ly->ffn_gate, a->xb, s->ffn_dim, dim, gs);
        gpu_matvec_vram(a->hb2, ly->ffn_up,   a->xb, s->ffn_dim, dim, gs);

        /* SwiGLU: SiLU(hb) * hb2 → hb (zero transfer) */
        gpu_silu_mul_vram(a->hb, a->hb, a->hb2, s->ffn_dim, gs);

        /* Down projection: hb_vram → xb_vram (upload weights) */
        gpu_matvec_vram(a->xb, ly->ffn_down, a->hb, dim, s->ffn_dim, gs);

        /* Residual: x_vram += xb_vram (zero transfer) */
        gpu_add_inplace_vram(a->x, a->xb, dim, gs);
    }

    /* ── Final norm (in VRAM) ── */
    gpu_rmsnorm_vram(a->x, a->x, gpu_norm_data(s->weights.output_norm), dim, gs);

    /* ── Logits matvec: vocab (128K×2048) ── */
    if (gs->weights_resident && s->weights.output &&
        wcache_lookup(gs, s->weights.output->data)) {
        /* X-INF3: output weights in VRAM — GPU logits, download result */
        gpu_matvec_vram(a->xb, s->weights.output, a->x,
                        s->vocab_size, dim, gs);
        gpu_tensor_download(a->xb, s->logits,
                            s->vocab_size * sizeof(float));
    } else {
        /* CPU fallback: download x, matvec on CPU */
        gpu_tensor_download(a->x, s->x, dim * sizeof(float));
        uint64_t t0 = rdtsc_gpu();
        matvec_q4_0(s->logits, s->weights.output->data, s->x,
                    s->vocab_size, dim);
        gs->cpu_cycles += rdtsc_gpu() - t0;
        gs->cpu_ops++;
    }

    s->pos++;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_forward_host — Per-op upload/download (X-INF1 fallback)
 * ══════════════════════════════════════════════════════════ */

static int gpu_llama_forward_host(gpu_llama_state_t *gs, uint32_t token)
{
    llama_state_t *s = gs->cpu;
    uint32_t dim     = s->dim;
    uint32_t kv_dim  = s->kv_dim;
    uint32_t hd      = s->head_dim;
    uint32_t pos     = s->pos;

    /* ── Embed token (always CPU) ── */
    gpu_embed_token(s->x, s->weights.token_embd, token, dim);

    /* ── Transformer layers ── */
    for (uint32_t l = 0; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        /* Attention norm — GPU when available */
        gpu_rmsnorm_dispatch(s->xb, s->x, gpu_norm_data(ly->attn_norm), dim, gs);

        /* Q, K, V projections — GPU matvec_q4_0 when available */
        gpu_matvec(s->q, ly->attn_q, s->xb, dim, dim, gs);
        gpu_matvec(s->k, ly->attn_k, s->xb, kv_dim, dim, gs);
        gpu_matvec(s->v, ly->attn_v, s->xb, kv_dim, dim, gs);

        /* RoPE — GPU when available (combined q+k dispatch) */
        gpu_rope_dispatch(s->q, s->k, s->n_heads, s->n_kv_heads,
                          hd, pos, 500000.0f, gs);

        /* Store K, V in cache */
        float *kc = s->kv_cache[l].k + (uint64_t)pos * kv_dim;
        float *vc = s->kv_cache[l].v + (uint64_t)pos * kv_dim;
        memcpy(kc, s->k, kv_dim * sizeof(float));
        memcpy(vc, s->v, kv_dim * sizeof(float));

        /* ── Grouped Query Attention (CPU — irregular access pattern) ── */
        float scale = 1.0f / sqrtf_bare((float)hd);

        for (uint32_t h = 0; h < s->n_heads; h++) {
            uint32_t kv_h = h / s->gqa_ratio;
            float *q_head = s->q + h * hd;

            for (uint32_t p = 0; p <= pos; p++) {
                float *k_pos = s->kv_cache[l].k + (uint64_t)p * kv_dim + kv_h * hd;
                float dot = 0.0f;
                for (uint32_t i = 0; i < hd; i++)
                    dot += q_head[i] * k_pos[i];
                s->att[p] = dot * scale;
            }

            softmax(s->att, pos + 1);

            float *out_head = s->xb2 + h * hd;
            memset(out_head, 0, hd * sizeof(float));
            for (uint32_t p = 0; p <= pos; p++) {
                float *v_pos = s->kv_cache[l].v + (uint64_t)p * kv_dim + kv_h * hd;
                float aa = s->att[p];
                for (uint32_t i = 0; i < hd; i++)
                    out_head[i] += aa * v_pos[i];
            }
        }

        /* Output projection */
        gpu_matvec(s->xb, ly->attn_output, s->xb2, dim, dim, gs);

        /* Residual connection */
        gpu_vec_add_dispatch(s->x, s->x, s->xb, dim, gs);

        /* ── FFN ── */
        gpu_rmsnorm_dispatch(s->xb, s->x, gpu_norm_data(ly->ffn_norm), dim, gs);

        gpu_matvec(s->hb,  ly->ffn_gate, s->xb, s->ffn_dim, dim, gs);
        gpu_matvec(s->hb2, ly->ffn_up,   s->xb, s->ffn_dim, dim, gs);

        gpu_silu_mul_dispatch(s->hb, s->hb, s->hb2, s->ffn_dim, gs);

        gpu_matvec(s->xb, ly->ffn_down, s->hb, dim, s->ffn_dim, gs);

        gpu_vec_add_dispatch(s->x, s->x, s->xb, dim, gs);
    }

    /* ── Final norm + logits ── */
    gpu_rmsnorm_dispatch(s->x, s->x, gpu_norm_data(s->weights.output_norm), dim, gs);
    gpu_matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim, gs);

    s->pos++;
    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_forward — dispatch to VRAM or host path
 * ══════════════════════════════════════════════════════════ */

int gpu_llama_forward(gpu_llama_state_t *gs, uint32_t token)
{
    if (gs->vram_resident)
        return gpu_llama_forward_vram(gs, token);
    else
        return gpu_llama_forward_host(gs, token);
}

/* ── Argmax ──────────────────────────────────────────────── */

static uint32_t gpu_argmax(const float *v, uint32_t n)
{
    uint32_t best = 0;
    float best_val = v[0];
    for (uint32_t i = 1; i < n; i++) {
        if (v[i] > best_val) {
            best_val = v[i];
            best = i;
        }
    }
    return best;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_generate — Prefill + decode with GPU dispatch
 * ══════════════════════════════════════════════════════════ */

#define LLAMA_EOS_1  128001
#define LLAMA_EOS_2  128009

void gpu_llama_generate(gpu_llama_state_t *gs, const uint32_t *prompt,
                        uint32_t prompt_len, uint32_t max_tokens)
{
    llama_state_t *s = gs->cpu;

    serial_puts("\n[GPU_INF] === GPU-Accelerated Inference ===\n");
    serial_puts("[GPU_INF] Prompt: ");
    serial_putdec(prompt_len);
    serial_puts(" tokens, max generate: ");
    serial_putdec(max_tokens);
    serial_puts("\n");

    fb_puts("\n GPU Inf: ");
    fb_putdec(prompt_len);
    fb_puts(" prompt, ");
    fb_putdec(max_tokens);
    fb_puts(" max gen\n");

    /* Reset stats */
    gs->gpu_ops = 0;
    gs->cpu_ops = 0;
    gs->gpu_cycles = 0;
    gs->cpu_cycles = 0;
    s->pos = 0;

    uint64_t total_t0 = rdtsc_gpu();

    /* ── Prefill ── */
    serial_puts("[GPU_INF] Prefill:\n");
    for (uint32_t i = 0; i < prompt_len; i++) {
        uint64_t t0 = rdtsc_gpu();
        gpu_llama_forward(gs, prompt[i]);
        uint64_t t1 = rdtsc_gpu();
        uint64_t ms = (t1 - t0) / 3000000;

        serial_puts("  tok ");
        serial_putdec(prompt[i]);
        serial_puts(" (");
        serial_putdec(ms);
        serial_puts(" ms)\n");
    }

    /* ── Decode ── */
    serial_puts("[GPU_INF] Decode:\n");
    fb_puts(" > ");

    uint32_t generated = 0;
    uint32_t next_token = gpu_argmax(s->logits, s->vocab_size);

    for (uint32_t t = 0; t < max_tokens; t++) {
        if (next_token == LLAMA_EOS_1 || next_token == LLAMA_EOS_2) {
            serial_puts("\n[GPU_INF] EOS token ");
            serial_putdec(next_token);
            serial_puts("\n");
            break;
        }

        uint64_t t0 = rdtsc_gpu();
        gpu_llama_forward(gs, next_token);
        uint64_t t1 = rdtsc_gpu();
        uint64_t ms = (t1 - t0) / 3000000;

        serial_puts("  [");
        serial_putdec(next_token);
        serial_puts("] (");
        serial_putdec(ms);
        serial_puts(" ms)\n");

        generated++;
        next_token = gpu_argmax(s->logits, s->vocab_size);
    }

    uint64_t total_t1 = rdtsc_gpu();
    uint64_t total_ms = (total_t1 - total_t0) / 3000000;

    serial_puts("[GPU_INF] Generated ");
    serial_putdec(generated);
    serial_puts(" tokens in ");
    serial_putdec(total_ms);
    serial_puts(" ms\n");

    if (generated > 0) {
        serial_puts("[GPU_INF] Avg: ");
        serial_putdec(total_ms / generated);
        serial_puts(" ms/token\n");
    }

    fb_puts("\n ");
    fb_putdec(generated);
    fb_puts(" tokens, ");
    fb_putdec(total_ms);
    fb_puts("ms\n");

    gpu_llama_stats(gs);
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_stats — Print dispatch statistics
 * ══════════════════════════════════════════════════════════ */

void gpu_llama_stats(gpu_llama_state_t *gs)
{
    serial_puts("[GPU_INF] Dispatch stats:\n");
    serial_puts("[GPU_INF]   GPU ops: ");
    serial_putdec(gs->gpu_ops);
    serial_puts(" (");
    serial_putdec(gs->gpu_cycles / 3000000);
    serial_puts(" ms)\n");
    serial_puts("[GPU_INF]   CPU ops: ");
    serial_putdec(gs->cpu_ops);
    serial_puts(" (");
    serial_putdec(gs->cpu_cycles / 3000000);
    serial_puts(" ms)\n");

    uint32_t total = gs->gpu_ops + gs->cpu_ops;
    if (total > 0) {
        uint32_t gpu_pct = (gs->gpu_ops * 100) / total;
        serial_puts("[GPU_INF]   GPU dispatch: ");
        serial_putdec(gpu_pct);
        serial_puts("% of ops\n");
    }
    if (gs->weights_resident) {
        serial_puts("[GPU_INF]   Weights: VRAM-resident (");
        serial_putdec(gs->wcache.total_bytes / (1024 * 1024));
        serial_puts("MB, ");
        serial_putdec(gs->wcache.count);
        serial_puts(" tensors)\n");
    }
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_benchmark — GPU vs CPU timing comparison
 * ══════════════════════════════════════════════════════════ */

int gpu_llama_benchmark(gpu_llama_state_t *gs)
{
    serial_puts("\n[GPU_INF] == Benchmark: GPU vs CPU ==\n");

    if (!gs || !gs->cpu) {
        serial_puts("[GPU_INF] Not initialized\n");
        return -1;
    }

    uint32_t dim = gs->cpu->dim;

    /* Prepare test vectors in scratch space */
    float *a = gs->cpu->xb;    /* Borrow scratch buffers */
    float *b = gs->cpu->xb2;
    float *c_cpu = gs->cpu->q;
    float *c_gpu = gs->cpu->k;

    for (uint32_t i = 0; i < dim; i++) {
        a[i] = 1.0f + (float)(i % 100) * 0.01f;
        b[i] = 2.0f + (float)(i % 50) * 0.02f;
    }

    /* ── CPU vec_add benchmark ── */
    uint32_t iters = 100;
    uint64_t t0 = rdtsc_gpu();
    for (uint32_t r = 0; r < iters; r++)
        vec_add(c_cpu, a, b, dim);
    uint64_t cpu_us = (rdtsc_gpu() - t0) / 3000;

    serial_puts("[GPU_INF] CPU vec_add (");
    serial_putdec(dim);
    serial_puts(" x ");
    serial_putdec(iters);
    serial_puts("): ");
    serial_putdec(cpu_us);
    serial_puts(" us total, ");
    serial_putdec(cpu_us / iters);
    serial_puts(" us/iter\n");

    /* ── GPU vec_add benchmark (if available) ── */
    if (gs->kernels.vec_add && gs->vram_a) {
        /* Warm up */
        gpu_vec_add_dispatch(c_gpu, a, b, dim, gs);

        uint32_t gpu_iters = 10;  /* Fewer — GPU dispatch is slow via PRAMIN */
        t0 = rdtsc_gpu();
        for (uint32_t r = 0; r < gpu_iters; r++)
            gpu_vec_add_dispatch(c_gpu, a, b, dim, gs);
        uint64_t gpu_us = (rdtsc_gpu() - t0) / 3000;

        serial_puts("[GPU_INF] GPU vec_add (");
        serial_putdec(dim);
        serial_puts(" x ");
        serial_putdec(gpu_iters);
        serial_puts("): ");
        serial_putdec(gpu_us);
        serial_puts(" us total, ");
        serial_putdec(gpu_us / gpu_iters);
        serial_puts(" us/iter\n");

        /* Verify correctness: compare first 4 elements */
        bool match = true;
        uint32_t check[4], ref[4];
        memcpy(check, c_gpu, 16);
        memcpy(ref, c_cpu, 16);
        for (int i = 0; i < 4; i++) {
            if (check[i] != ref[i]) match = false;
        }
        serial_puts("[GPU_INF] GPU/CPU match: ");
        serial_puts(match ? "OK" : "MISMATCH");
        serial_puts("\n");

        /* Speedup (negative = GPU slower, expected with PRAMIN) */
        uint64_t cpu_per = cpu_us / iters;
        uint64_t gpu_per = gpu_us / gpu_iters;
        if (gpu_per > 0) {
            if (cpu_per > gpu_per) {
                serial_puts("[GPU_INF] GPU speedup: ");
                serial_putdec(cpu_per / gpu_per);
                serial_puts("x\n");
            } else {
                serial_puts("[GPU_INF] GPU overhead: ");
                serial_putdec(gpu_per / (cpu_per > 0 ? cpu_per : 1));
                serial_puts("x slower (PRAMIN transfer bottleneck)\n");
                serial_puts("[GPU_INF] Note: CE DMA would reduce transfer overhead ~100x\n");
            }
        }
    } else {
        serial_puts("[GPU_INF] GPU vec_add not available, skipping GPU benchmark\n");
    }

    /* ── matvec_q4_0 benchmark (CPU only, for reference) ── */
    if (gs->cpu->weights.layers && gs->cpu->weights.layers[0].attn_q) {
        gguf_tensor_t *wq = gs->cpu->weights.layers[0].attn_q;
        uint32_t rows = dim, cols = dim;
        float *input = a;
        float *output = c_cpu;

        uint32_t mv_iters = 5;
        t0 = rdtsc_gpu();
        for (uint32_t r = 0; r < mv_iters; r++)
            matvec_q4_0(output, wq->data, input, rows, cols);
        uint64_t mv_us = (rdtsc_gpu() - t0) / 3000;

        serial_puts("[GPU_INF] CPU matvec_q4_0 (");
        serial_putdec(rows);
        serial_puts("x");
        serial_putdec(cols);
        serial_puts(" x ");
        serial_putdec(mv_iters);
        serial_puts("): ");
        serial_putdec(mv_us);
        serial_puts(" us total, ");
        serial_putdec(mv_us / mv_iters);
        serial_puts(" us/iter\n");
        serial_puts("[GPU_INF] (GPU matvec_q4_0 kernel: TODO — would be ~10-50x faster)\n");
    }

    serial_puts("[GPU_INF] == Benchmark complete ==\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_benchmark_standalone — No model needed
 * ══════════════════════════════════════════════════════════ */

int gpu_llama_benchmark_standalone(void)
{
    serial_puts("\n[GPU_INF] == X40: Standalone GPU Benchmark ==\n");

    gpu_tensor_state_t *ts = gpu_tensor_get_state();
    if (!ts || !ts->initialized) {
        serial_puts("[GPU_INF] GPU tensor subsystem not ready\n");
        return -1;
    }

    /* Detect available kernels */
    sass_kernel_t *k_vec = sass_get_kernel("vec_add_f32");
    bool has_gpu_vec = (k_vec && k_vec->uploaded);

    serial_puts("[GPU_INF] vec_add_f32: ");
    serial_puts(has_gpu_vec ? "GPU available\n" : "GPU not available\n");

    /* Allocate test buffers (256 floats = 1KB) */
    uint32_t n = 256;
    uint32_t buf_bytes = n * sizeof(float);
    float a[256], b[256], c_cpu[256], c_gpu[256];

    for (uint32_t i = 0; i < n; i++) {
        a[i] = 1.0f + (float)i * 0.1f;
        b[i] = 100.0f + (float)i * 0.05f;
    }

    /* CPU benchmark */
    uint32_t iters = 1000;
    uint64_t t0 = rdtsc_gpu();
    for (uint32_t r = 0; r < iters; r++)
        vec_add(c_cpu, a, b, n);
    uint64_t cpu_us = (rdtsc_gpu() - t0) / 3000;

    serial_puts("[GPU_INF] CPU vec_add (256 x 1000): ");
    serial_putdec(cpu_us);
    serial_puts(" us\n");

    /* GPU benchmark */
    if (has_gpu_vec) {
        uint64_t va = gpu_tensor_alloc(buf_bytes);
        uint64_t vb = gpu_tensor_alloc(buf_bytes);
        uint64_t vc = gpu_tensor_alloc(buf_bytes);

        if (va && vb && vc) {
            gpu_tensor_upload(va, a, buf_bytes);
            gpu_tensor_upload(vb, b, buf_bytes);

            /* Warm up */
            gpu_vec_add(vc, va, vb, n);

            uint32_t gpu_iters = 10;
            t0 = rdtsc_gpu();
            for (uint32_t r = 0; r < gpu_iters; r++)
                gpu_vec_add(vc, va, vb, n);
            uint64_t gpu_us = (rdtsc_gpu() - t0) / 3000;

            serial_puts("[GPU_INF] GPU vec_add (256 x 10): ");
            serial_putdec(gpu_us);
            serial_puts(" us\n");

            /* Verify */
            gpu_tensor_download(vc, c_gpu, buf_bytes);
            uint32_t raw_cpu[4], raw_gpu[4];
            memcpy(raw_cpu, c_cpu, 16);
            memcpy(raw_gpu, c_gpu, 16);
            bool match = true;
            for (int i = 0; i < 4; i++)
                if (raw_cpu[i] != raw_gpu[i]) match = false;
            serial_puts("[GPU_INF] Verify: ");
            serial_puts(match ? "OK\n" : "MISMATCH\n");
        }

        gpu_tensor_reset();
    }

    serial_puts("[GPU_INF] == Standalone benchmark complete ==\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  gpu_llama_free
 * ══════════════════════════════════════════════════════════ */

void gpu_llama_free(gpu_llama_state_t *gs)
{
    if (!gs) return;
    /* VRAM buffers freed by gpu_tensor_reset() — no individual free */
    gs->initialized = false;
    gs->cpu = NULL;
}
