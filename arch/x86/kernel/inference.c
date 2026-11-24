/*
 * OsitoK x86-64 — Llama Inference Runtime
 *
 * Complete transformer forward pass for Llama 3.2 1B (Q4_0).
 * Uses tensor.h primitives: matvec_q4_0/q8_0, rmsnorm, softmax,
 * silu_inplace, rope, vec_add, vec_mul.
 *
 * ~5-10s/token on scalar CPU @ 3GHz. Demo functional.
 */

#include "inference.h"
#include "tensor.h"
#include "../include/paging.h"
#include "../include/tensor_arena.h"
#include "../include/dispatch.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putc(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_pages(uint64_t count);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern float expf_bare(float x);
extern int   nvme_read(uint64_t lba, uint32_t count, void *buf);
extern int   nvme_read_async(uint64_t lba, uint32_t count, uint64_t phys_addr);
extern int   nvme_wait_cq(uint16_t cid);
extern uint32_t nvme_get_lba_size(void);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Tokenizer decode (tokenizer.c) — returns NULL if tokenizer not initialized */
extern const char *tok_global_decode(uint32_t id);

/* Forward declarations for sampling tuners (defined later in this file). */
void llama_set_sampling(float temperature, float top_p);
void llama_set_penalty(float rep, float presence, float frequency);
void llama_set_ngram_size(uint32_t n);

/* ── Utility helpers ─────────────────────────────────────────── */

#define PAGE_SZ 4096

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

static uint64_t pages_for(uint64_t bytes)
{
    return (bytes + PAGE_SZ - 1) / PAGE_SZ;
}

/* ── Name builder (no snprintf) ──────────────────────────────── */

static char *build_layer_name(char *buf, uint32_t layer, const char *suffix)
{
    buf[0] = 'b'; buf[1] = 'l'; buf[2] = 'k'; buf[3] = '.';
    int pos = 4;
    if (layer >= 100) buf[pos++] = '0' + (layer / 100);
    if (layer >= 10)  buf[pos++] = '0' + ((layer / 10) % 10);
    buf[pos++] = '0' + (layer % 10);
    buf[pos++] = '.';
    while (*suffix) buf[pos++] = *suffix++;
    buf[pos] = '\0';
    return buf;
}

/* ── Token embedding ─────────────────────────────────────────── */

static void embed_token(float *dst, gguf_tensor_t *embd, uint32_t token, uint32_t dim)
{
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
    case GGML_TYPE_Q4_K:
    case GGML_TYPE_Q6_K: {
        /* For Llama 3 — token_embd often Q4_K or Q6_K. Reuse the matvec
         * dequant by setting input = one-hot(token), but cheaper:
         * dequant one row directly via a unit-input matvec of size 1.
         * Use the row-extract approach: do a "matvec" with rows=1 where
         * we point at the right row offset. */
        size_t row_bytes = (embd->type == GGML_TYPE_Q4_K)
            ? ((size_t)dim / 256) * 144
            : ((size_t)dim / 256) * 210;
        const void *row_ptr = (const uint8_t *)embd->data + (uint64_t)token * row_bytes;
        /* Build a unit vector tmp[dim] = column-identity, then matvec with
         * weight = row gives back the dequantized row. Simpler: dequant
         * directly by calling matvec with one-hot input of size dim.
         * Actually easiest is to dot against unit-vector inputs[i]=1 to
         * extract sum of dequant values; that gives the SUM not the row.
         *
         * Cleanest: build a tiny scratch and run matvec_qX_k_scalar with
         * rows=dim, treating the row as a 1×dim "matrix" multiplied by a
         * one-hot of size dim. Cheaper: write a direct dequant.
         *
         * Direct dequant: walk the block layout and write to dst[]. */
        if (embd->type == GGML_TYPE_Q4_K) {
            const uint8_t *block_base = (const uint8_t *)row_ptr;
            uint32_t nb = dim / 256;
            for (uint32_t b = 0; b < nb; b++) {
                const uint8_t *block = block_base + b * 144;
                float d    = f16_to_f32(*(const uint16_t *)(block + 0));
                float dmin = f16_to_f32(*(const uint16_t *)(block + 2));
                const uint8_t *sc = block + 4;
                const uint8_t *qs = block + 16;
                float *yp = dst + b * 256;
                int is = 0;
                for (int chunk = 0; chunk < 256; chunk += 64) {
                    uint8_t s1, m1b, s2, m2b;
                    if (is + 0 < 4) { s1 = sc[is+0]&63; m1b = sc[is+0+4]&63; }
                    else { int sj=is+0; s1=(sc[sj+4]&0xF)|((sc[sj-4]>>6)<<4);
                           m1b=(sc[sj+4]>>4)|((sc[sj]>>6)<<4); }
                    float d1 = d * (float)s1, mm1 = dmin * (float)m1b;
                    if (is + 1 < 4) { s2 = sc[is+1]&63; m2b = sc[is+1+4]&63; }
                    else { int sj=is+1; s2=(sc[sj+4]&0xF)|((sc[sj-4]>>6)<<4);
                           m2b=(sc[sj+4]>>4)|((sc[sj]>>6)<<4); }
                    float d2 = d * (float)s2, mm2 = dmin * (float)m2b;
                    for (int l = 0; l < 32; l++)
                        yp[chunk + l]      = d1 * (float)(qs[l] & 0xF) - mm1;
                    for (int l = 0; l < 32; l++)
                        yp[chunk + 32 + l] = d2 * (float)(qs[l] >> 4) - mm2;
                    qs += 32; is += 2;
                }
            }
        } else {  /* Q6_K */
            const uint8_t *block_base = (const uint8_t *)row_ptr;
            uint32_t nb = dim / 256;
            for (uint32_t b = 0; b < nb; b++) {
                const uint8_t *block = block_base + b * 210;
                const uint8_t *ql = block + 0;
                const uint8_t *qh = block + 128;
                const int8_t  *sc = (const int8_t *)(block + 192);
                float d = f16_to_f32(*(const uint16_t *)(block + 208));
                float *yp = dst + b * 256;
                for (int n = 0; n < 256; n += 128) {
                    for (int l = 0; l < 32; l++) {
                        int is = l / 16;
                        int8_t q1 = (int8_t)((ql[l +  0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                        int8_t q2 = (int8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                        int8_t q3 = (int8_t)((ql[l +  0]  >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                        int8_t q4 = (int8_t)((ql[l + 32]  >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                        yp[n + l +  0] = d * (float)sc[is + 0] * (float)q1;
                        yp[n + l + 32] = d * (float)sc[is + 2] * (float)q2;
                        yp[n + l + 64] = d * (float)sc[is + 4] * (float)q3;
                        yp[n + l + 96] = d * (float)sc[is + 6] * (float)q4;
                    }
                    ql += 64; qh += 32; sc += 8;
                }
            }
        }
        break;
    }
    default: {
        memset(dst, 0, dim * sizeof(float));
        static int embed_warned = 0;
        if (!embed_warned) {
            embed_warned = 1;
            serial_puts("[embed WARN] unsupported tensor type=");
            serial_putdec((uint64_t)embd->type);
            serial_puts(" -- embed zeroed!\n");
        }
        break;
    }
    }
}

/* ── Matvec dispatch by tensor type ──────────────────────────── */

/* GPU matvec routing: kept as a non-default toggle for benchmarking
 * only. Per-call wasm_wgpu_matvec costs ~1 ms round-trip on mapAsync,
 * so for brandon-tiny's small shapes (~16K elems / matvec) every
 * dispatch is a net loss. The handle-based path (wasm_wgpu_matvec_h)
 * is the holistic answer — see brandon_forward_one_gpu (todo). */
int g_brandon_use_gpu_matvec = 0;

#ifdef __EMSCRIPTEN__
extern int wasm_wgpu_init(void);
extern int wasm_wgpu_matvec(const float *w, const float *vin, float *out,
                             int rows, int cols);
#endif

static void matvec(float *out, gguf_tensor_t *tensor,
                   const float *input, uint32_t rows, uint32_t cols)
{
#ifdef __EMSCRIPTEN__
    /* Only fires under bdebug gpu 1 + a giant shape. The break-even
     * threshold is so high (~100k elements) that brandon-tiny only
     * triggers it on the LM head, and even there round-trip dominates.
     * Useful as a knob to time the real GPU compute against CPU. */
    if (g_brandon_use_gpu_matvec && tensor->type == GGML_TYPE_F32 &&
        (uint64_t)rows * cols >= 100000 && wasm_wgpu_init()) {
        if (wasm_wgpu_matvec((const float *)tensor->data, input, out,
                              (int)rows, (int)cols) == 0) {
            return;
        }
    }
#endif
    switch (tensor->type) {
    case GGML_TYPE_Q4_0:
        /* Go through the boot-selected dispatch table so any future
         * SIMD variant (AVX-512) gets picked up without re-linking. */
        disp.matvec_q4_0(out, tensor->data, input, rows, cols);
        break;
    case GGML_TYPE_Q8_0:
        disp.matvec_q8_0(out, tensor->data, input, rows, cols);
        break;
    case GGML_TYPE_Q4_K: {
        extern void matvec_q4_k_scalar(float *, const void *, const float *,
                                        uint32_t, uint32_t);
        matvec_q4_k_scalar(out, tensor->data, input, rows, cols);
        break;
    }
    case GGML_TYPE_Q6_K: {
        extern void matvec_q6_k_scalar(float *, const void *, const float *,
                                        uint32_t, uint32_t);
        matvec_q6_k_scalar(out, tensor->data, input, rows, cols);
        break;
    }
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
    case GGML_TYPE_F16: {
        /* F16 weights (brandon-tiny ships every layer as f16). Native
         * x86 dispatches to matvec_f16_avx2 (vcvtph2ps hardware
         * conversion 8 lanes at a time). WASM falls through to scalar
         * dequant — but the WASM build pre-dequants F16 → F32 at load
         * time (gguf_dequant_f16_to_f32 in wasm_init.c) so this path
         * never fires there. */
#ifndef __EMSCRIPTEN__
        extern void matvec_f16_avx2(float *out, const void *weight,
                                     const float *input,
                                     uint32_t rows, uint32_t cols);
        matvec_f16_avx2(out, tensor->data, input, rows, cols);
#else
        const uint16_t *w = (const uint16_t *)tensor->data;
        for (uint32_t r = 0; r < rows; r++) {
            float sum = 0.0f;
            const uint16_t *row = w + (uint64_t)r * cols;
            for (uint32_t c = 0; c < cols; c++)
                sum += f16_to_f32(row[c]) * input[c];
            out[r] = sum;
        }
#endif
        break;
    }
    default: {
        memset(out, 0, rows * sizeof(float));
        /* Loud one-shot warning so we know if any unsupported quant
         * type silently zeros a matvec. */
        static uint32_t warned_types = 0;
        if (!(warned_types & (1u << (tensor->type & 31)))) {
            warned_types |= (1u << (tensor->type & 31));
            serial_puts("[matvec WARN] unsupported tensor type=");
            serial_putdec((uint64_t)tensor->type);
            serial_puts(" -- output zeroed!\n");
        }
        break;
    }
    }
}

/* ── Get float data from norm tensor ─────────────────────────── */

static const float *norm_data(gguf_tensor_t *tensor)
{
    return (const float *)tensor->data;
}

/* ══════════════════════════════════════════════════════════════
 *  llama_init — Resolve weights, allocate scratch + KV cache
 * ══════════════════════════════════════════════════════════════ */

int llama_init(llama_state_t *state, gguf_model_t *model, uint32_t max_seq)
{
    memset(state, 0, sizeof(*state));
    state->model = model;

    /* Architecture dispatch: "brandon" uses block-shared layer_map. */
    {
        const char *a = model->architecture[0] ? model->architecture : "llama";
        for (uint32_t i = 0; i < GGUF_ARCH_LEN - 1 && a[i]; i++)
            state->arch[i] = a[i];
    }
    bool is_brandon = (state->arch[0] == 'b');

    /* Copy architecture from model metadata */
    state->dim        = model->hidden_size;
    state->n_heads    = model->head_count;
    state->n_kv_heads = model->kv_head_count;
    state->vocab_size = model->vocab_size;
    state->max_seq    = max_seq;

    /* For brandon, num_layers in the GGUF is the *unique block count*.
     * The forward pass walks compute_layer_count logical layers, each
     * indexed via brandon_layer_map[] into one of the unique blocks. */
    if (is_brandon && model->brandon_compute_layer_count) {
        state->n_layers        = model->brandon_compute_layer_count;
        state->n_unique_blocks = model->num_layers;
    } else {
        state->n_layers        = model->num_layers;
        state->n_unique_blocks = model->num_layers;
    }
    state->layer_map           = is_brandon ? model->brandon_layer_map : NULL;
    state->use_dwa             = is_brandon && model->brandon_use_dwa;
    state->use_value_residual  = is_brandon && model->brandon_use_value_residual;
    state->n_registers         = is_brandon ? model->brandon_n_registers : 0;

    /* Derived dimensions */
    state->head_dim   = state->dim / state->n_heads;
    state->kv_dim     = state->n_kv_heads * state->head_dim;
    state->gqa_ratio     = state->n_heads / state->n_kv_heads;
    state->rope_freq_base = model->rope_freq_base > 0.0f ? model->rope_freq_base : 10000.0f;
    state->rms_eps        = model->attn_layer_norm_rms_eps > 0.0f ?
                              model->attn_layer_norm_rms_eps : 1e-5f;

    /* Discover ffn_dim from first layer gate tensor */
    char nbuf[64];
    build_layer_name(nbuf, 0, "ffn_gate.weight");
    gguf_tensor_t *gate0 = gguf_find_tensor(model, nbuf);
    if (!gate0) {
        serial_puts("[LLAMA] ERROR: cannot find blk.0.ffn_gate.weight\n");
        return -1;
    }
    state->ffn_dim = (uint32_t)gate0->ne[1];

    /* Validate */
    if (state->dim == 0 || state->n_layers == 0 || state->n_heads == 0 ||
        state->n_kv_heads == 0 || state->vocab_size == 0) {
        serial_puts("[LLAMA] ERROR: invalid model dimensions\n");
        return -1;
    }

    serial_puts("\n[LLAMA] Architecture: dim=");
    serial_putdec(state->dim);
    serial_puts(" layers=");
    serial_putdec(state->n_layers);
    serial_puts(" heads=");
    serial_putdec(state->n_heads);
    serial_puts(" kv_heads=");
    serial_putdec(state->n_kv_heads);
    serial_puts(" head_dim=");
    serial_putdec(state->head_dim);
    serial_puts(" kv_dim=");
    serial_putdec(state->kv_dim);
    serial_puts("\n[LLAMA]   ffn_dim=");
    serial_putdec(state->ffn_dim);
    serial_puts(" vocab=");
    serial_putdec(state->vocab_size);
    serial_puts(" max_seq=");
    serial_putdec(state->max_seq);
    serial_puts(" rope_theta=");
    serial_putdec((uint64_t)state->rope_freq_base);
    serial_puts("\n");

    /* ── Resolve global tensors ── */
    state->weights.token_embd = gguf_find_tensor(model, "token_embd.weight");
    state->weights.output_norm = gguf_find_tensor(model, "output_norm.weight");
    state->weights.output = gguf_find_tensor(model, "output.weight");

    if (!state->weights.token_embd) {
        serial_puts("[LLAMA] ERROR: missing token_embd.weight\n");
        return -1;
    }
    if (!state->weights.output_norm) {
        serial_puts("[LLAMA] ERROR: missing output_norm.weight\n");
        return -1;
    }
    /* output.weight may be tied to token_embd */
    if (!state->weights.output) {
        serial_puts("[LLAMA] output.weight not found, using token_embd (tied)\n");
        state->weights.output = state->weights.token_embd;
    }

    /* ── Allocate layer table ── */
    uint64_t layer_table_size = state->n_layers * sizeof(llama_layer_t);
    uint64_t layer_table_pages = pages_for(layer_table_size);
    void *layer_phys = mem_alloc_pages(layer_table_pages);
    if (!layer_phys) {
        serial_puts("[LLAMA] ERROR: failed to alloc layer table\n");
        return -1;
    }
    state->weights.layers = (llama_layer_t *)PHYS_TO_VIRT(layer_phys);
    memset(state->weights.layers, 0, (size_t)layer_table_size);

    /* ── Resolve per-block tensors, fan out via layer_map for brandon ──
     *
     * For arch=brandon, weights.layers has n_layers (=24) entries but
     * the GGUF only stores n_unique_blocks (=12) sets of weights named
     * blk.{0..11}.*. We resolve once per unique block, then alias each
     * logical layer's pointers via layer_map[L].
     *
     * For arch=llama, n_unique_blocks == n_layers and the loop is a
     * straight 1:1 mapping. */
    {
        uint64_t blocks_size = (uint64_t)state->n_unique_blocks * sizeof(llama_layer_t);
        llama_layer_t *blocks = (llama_layer_t *)mem_alloc_aligned(blocks_size, 64);
        if (!blocks) {
            serial_puts("[LLAMA] ERROR: failed to alloc unique-block table\n");
            return -1;
        }
        memset(blocks, 0, (size_t)blocks_size);

        uint32_t resolved = 0;
        for (uint32_t b = 0; b < state->n_unique_blocks; b++) {
            llama_layer_t *ly = &blocks[b];

            ly->attn_norm    = gguf_find_tensor(model, build_layer_name(nbuf, b, "attn_norm.weight"));
            ly->attn_q       = gguf_find_tensor(model, build_layer_name(nbuf, b, "attn_q.weight"));
            ly->attn_k       = gguf_find_tensor(model, build_layer_name(nbuf, b, "attn_k.weight"));
            ly->attn_v       = gguf_find_tensor(model, build_layer_name(nbuf, b, "attn_v.weight"));
            ly->attn_output  = gguf_find_tensor(model, build_layer_name(nbuf, b, "attn_output.weight"));
            ly->ffn_norm     = gguf_find_tensor(model, build_layer_name(nbuf, b, "ffn_norm.weight"));
            ly->ffn_gate     = gguf_find_tensor(model, build_layer_name(nbuf, b, "ffn_gate.weight"));
            ly->ffn_up       = gguf_find_tensor(model, build_layer_name(nbuf, b, "ffn_up.weight"));
            ly->ffn_down     = gguf_find_tensor(model, build_layer_name(nbuf, b, "ffn_down.weight"));

            if (ly->attn_norm && ly->attn_q && ly->attn_k && ly->attn_v &&
                ly->attn_output && ly->ffn_norm && ly->ffn_gate &&
                ly->ffn_up && ly->ffn_down) {
                resolved++;
            } else {
                serial_puts("[LLAMA] WARNING: block ");
                serial_putdec(b);
                serial_puts(" incomplete\n");
            }
        }

        if (resolved < state->n_unique_blocks) {
            serial_puts("[LLAMA] ERROR: only ");
            serial_putdec(resolved);
            serial_puts("/");
            serial_putdec(state->n_unique_blocks);
            serial_puts(" unique blocks resolved\n");
            return -1;
        }

        for (uint32_t l = 0; l < state->n_layers; l++) {
            uint32_t b = state->layer_map ? state->layer_map[l] : l;
            if (b >= state->n_unique_blocks) {
                serial_puts("[LLAMA] ERROR: layer_map[");
                serial_putdec(l); serial_puts("]=");
                serial_putdec(b); serial_puts(" out of range\n");
                return -1;
            }
            state->weights.layers[l] = blocks[b];
        }
    }

    /* Resolve brandon-specific tensors */
    if (is_brandon) {
        state->ffn_dim = model->feed_forward_length ? model->feed_forward_length : state->ffn_dim;

        if (state->use_dwa) {
            state->dwa_weights = gguf_find_tensor(model, "dwa.weight");
            if (!state->dwa_weights) {
                serial_puts("[LLAMA] ERROR: brandon use_dwa but dwa.weight missing\n");
                return -1;
            }
        }
        if (state->n_registers > 0) {
            state->register_weights = gguf_find_tensor(model, "register.weight");
            if (!state->register_weights) {
                serial_puts("[LLAMA] ERROR: brandon n_registers>0 but register.weight missing\n");
                return -1;
            }
        }
    }

    serial_puts("[LLAMA] All ");
    serial_putdec(state->n_layers);
    serial_puts(" layers resolved\n");

    /* ── Allocate KV cache ── */
    uint64_t kv_table_size = state->n_layers * sizeof(llama_kv_layer_t);
    uint64_t kv_table_pages = pages_for(kv_table_size);
    void *kv_table_phys = mem_alloc_pages(kv_table_pages);
    if (!kv_table_phys) {
        serial_puts("[LLAMA] ERROR: failed to alloc KV table\n");
        return -1;
    }
    state->kv_cache = (llama_kv_layer_t *)PHYS_TO_VIRT(kv_table_phys);
    memset(state->kv_cache, 0, (size_t)kv_table_size);

    uint64_t kv_layer_bytes = (uint64_t)max_seq * state->kv_dim * sizeof(float);
    uint64_t kv_layer_pages = pages_for(kv_layer_bytes);
    uint64_t kv_total_pages = 0;

    /* Try superpage-backed arena first (2MB pages → ~0 TLB misses during
     * attention). Fall back to regular allocator if arena unavailable. */
    {
        for (uint32_t l = 0; l < state->n_layers; l++) {
            void *k = 0, *v = 0;
            if (g_tensor_arena.virt_base) {
                k = tensor_arena_alloc(&g_tensor_arena, kv_layer_bytes, 64);
                v = tensor_arena_alloc(&g_tensor_arena, kv_layer_bytes, 64);
            }
            if (!k || !v) {
                void *k_phys = mem_alloc_pages(kv_layer_pages);
                void *v_phys = mem_alloc_pages(kv_layer_pages);
                if (!k_phys || !v_phys) {
                    serial_puts("[LLAMA] ERROR: failed to alloc KV cache layer ");
                    serial_putdec(l);
                    serial_puts("\n");
                    return -1;
                }
                k = PHYS_TO_VIRT(k_phys);
                v = PHYS_TO_VIRT(v_phys);
            }
            state->kv_cache[l].k = (float *)k;
            state->kv_cache[l].v = (float *)v;
            memset(state->kv_cache[l].k, 0, (size_t)kv_layer_bytes);
            memset(state->kv_cache[l].v, 0, (size_t)kv_layer_bytes);
            kv_total_pages += kv_layer_pages * 2;
        }
    }

    serial_puts("[LLAMA] KV cache: ");
    serial_putdec((kv_total_pages * PAGE_SZ) / (1024 * 1024));
    serial_puts(" MB (");
    serial_putdec(state->n_layers);
    serial_puts(" layers x ");
    serial_putdec(max_seq);
    serial_puts(" seq x ");
    serial_putdec(state->kv_dim);
    serial_puts(" kv_dim)\n");

    /* ── Allocate scratch buffers (single allocation, cache-colored) ──
     *
     * Buffers used concurrently on different cores (Q on BSP, K on AP0,
     * V on AP1; gate on BSP, up on AP0) are separated by L2_COLOR_PAD
     * bytes to avoid L2 cache set aliasing. On a typical x86-64 L2
     * (256KB, 8-way, 64B lines, 512 sets), bits [6:14] of the address
     * determine the set. Two buffers that differ by <32KB in address
     * alias to the same sets and evict each other. The padding ensures
     * concurrent buffers map to different set regions. */
#define L2_COLOR_PAD  (32 * 1024)  /* 32KB — one full L2 set stride */

    uint64_t scratch_size =
        (uint64_t)state->dim * sizeof(float) +          /* x */
        (uint64_t)state->dim * sizeof(float) +          /* xb */
        (uint64_t)state->dim * sizeof(float) +          /* xb2 */
        (uint64_t)state->dim * sizeof(float) +          /* q */
        L2_COLOR_PAD +                                   /* ── color boundary ── */
        (uint64_t)state->kv_dim * sizeof(float) +       /* k (AP0) */
        L2_COLOR_PAD +                                   /* ── color boundary ── */
        (uint64_t)state->kv_dim * sizeof(float) +       /* v (AP1) */
        (uint64_t)max_seq * sizeof(float) +              /* att */
        (uint64_t)state->ffn_dim * sizeof(float) +      /* hb (BSP) */
        L2_COLOR_PAD +                                   /* ── color boundary ── */
        (uint64_t)state->ffn_dim * sizeof(float) +      /* hb2 (AP0) */
        (uint64_t)state->vocab_size * sizeof(float);     /* logits */

    uint64_t scratch_pages = pages_for(scratch_size);

    /* Prefer superpage arena for scratch — this is the hottest memory in
     * the forward pass, so TLB coverage matters most here. */
    {
        state->scratch = 0;
        if (g_tensor_arena.virt_base) {
            state->scratch = (float *)tensor_arena_alloc(&g_tensor_arena,
                                                          scratch_pages * PAGE_SZ,
                                                          2 * 1024 * 1024);
        }
        if (!state->scratch) {
            void *scratch_phys = mem_alloc_pages(scratch_pages);
            if (!scratch_phys) {
                serial_puts("[LLAMA] ERROR: failed to alloc scratch (");
                serial_putdec(scratch_size / 1024);
                serial_puts(" KB)\n");
                return -1;
            }
            state->scratch = (float *)PHYS_TO_VIRT(scratch_phys);
        }
    }
    memset(state->scratch, 0, (size_t)(scratch_pages * PAGE_SZ));

    /* Partition scratch (cache-colored: pad between concurrent buffers) */
    float *ptr = state->scratch;
    state->x      = ptr; ptr += state->dim;
    state->xb     = ptr; ptr += state->dim;
    state->xb2    = ptr; ptr += state->dim;
    state->q      = ptr; ptr += state->dim;
    ptr = (float *)((uint64_t)ptr + L2_COLOR_PAD);  /* Q↔K color boundary */
    state->k      = ptr; ptr += state->kv_dim;
    ptr = (float *)((uint64_t)ptr + L2_COLOR_PAD);  /* K↔V color boundary */
    state->v      = ptr; ptr += state->kv_dim;
    state->att    = ptr; ptr += max_seq;
    state->hb     = ptr; ptr += state->ffn_dim;
    ptr = (float *)((uint64_t)ptr + L2_COLOR_PAD);  /* gate↔up color boundary */
    state->hb2    = ptr; ptr += state->ffn_dim;
    state->logits = ptr;

    serial_puts("[LLAMA] Scratch: ");
    serial_putdec(scratch_size / 1024);
    serial_puts(" KB\n");

    /* ── Allocate per-AP attention scratch buffers ── */
    {
        extern int ap_worker_count;
        int n_ap = ap_worker_count;
        if (n_ap > LLAMA_MAX_AP_SCRATCH) n_ap = LLAMA_MAX_AP_SCRATCH;
        for (int i = 0; i < n_ap; i++) {
            uint64_t att_bytes = (uint64_t)max_seq * sizeof(float);
            uint64_t att_pages = pages_for(att_bytes);
            void *att_phys = mem_alloc_pages(att_pages);
            if (att_phys) {
                state->att_scratch[i] = (float *)PHYS_TO_VIRT(att_phys);
                memset(state->att_scratch[i], 0, (size_t)att_bytes);
            }
        }
        if (n_ap > 0) {
            serial_puts("[LLAMA] Attention scratch: ");
            serial_putdec((uint64_t)n_ap);
            serial_puts(" AP buffers\n");
        }
    }

    /* ── Brandon-arch extras (v_first + DWA scratch) ── */
    if (is_brandon) {
        /* Pre-load brandon-tiny's tested sampling recipe so the chat
         * works out of the box (matches test_generation.py). User can
         * still override via 'temp', 'penalty', 'ngram' at the shell. */
        llama_set_sampling(0.7f, 0.9f);
        llama_set_penalty(1.2f, 0.0f, 0.0f);
        llama_set_ngram_size(3);

        if (state->use_value_residual) {
            state->v_first = (float *)mem_alloc_aligned(
                (uint64_t)state->kv_dim * sizeof(float), 64);
            if (!state->v_first) {
                serial_puts("[LLAMA] ERROR: failed to alloc v_first\n");
                return -1;
            }
            memset(state->v_first, 0, (size_t)state->kv_dim * sizeof(float));
        }
        if (state->use_dwa) {
            uint64_t bytes = (uint64_t)(state->n_layers + 1) * state->dim * sizeof(float);
            state->dwa_buf = (float *)mem_alloc_aligned(bytes, 64);
            if (!state->dwa_buf) {
                serial_puts("[LLAMA] ERROR: failed to alloc dwa_buf\n");
                return -1;
            }
            memset(state->dwa_buf, 0, (size_t)bytes);
        }
        serial_puts("[BRANDON] arch=brandon n_unique=");
        serial_putdec(state->n_unique_blocks);
        serial_puts(" n_layers=");
        serial_putdec(state->n_layers);
        serial_puts(" registers=");
        serial_putdec(state->n_registers);
        if (state->use_dwa)            serial_puts(" dwa");
        if (state->use_value_residual) serial_puts(" v_residual");
        serial_puts("\n");
    }

    serial_puts("[LLAMA] Init complete. Ready for inference.\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  SMP helpers for inference-layer parallelism
 * ══════════════════════════════════════════════════════════════ */

extern int smp_submit_any(void (*)(void*, void*), void*, void*);
extern void smp_wait(int);
extern int ap_worker_count;

typedef struct {
    float         *out;
    gguf_tensor_t *tensor;
    const float   *input;
    uint32_t       rows, cols;
} matvec_arg_t;

static void matvec_worker(void *arg, void *result)
{
    (void)result;
    matvec_arg_t *a = (matvec_arg_t *)arg;
    /* Prefetch input vector into this AP's L2 — it's hot in BSP's cache
     * from rmsnorm but cold here. 16 floats = 64 bytes = 1 cache line. */
    for (uint32_t i = 0; i < a->cols; i += 16)
        __builtin_prefetch(a->input + i, 0, 1);
    matvec(a->out, a->tensor, a->input, a->rows, a->cols);
}

/* Attention head chunk — processes a range of heads on an AP */
typedef struct {
    const float    *q;          /* [dim] */
    float          *xb2;        /* [dim] output concat */
    const float    *kv_k;       /* kv_cache[l].k */
    const float    *kv_v;       /* kv_cache[l].v */
    float          *att_buf;    /* per-AP attention scratch [max_seq] */
    uint32_t        h_start, h_end;
    uint32_t        pos, hd, kv_dim, gqa_ratio;
    float           scale;
} attn_chunk_t;

static void attention_heads_worker(void *arg, void *result)
{
    (void)result;
    attn_chunk_t *c = (attn_chunk_t *)arg;

    /* Prefetch Q vector for our head range into this AP's cache */
    for (uint32_t i = c->h_start * c->hd; i < c->h_end * c->hd; i += 16)
        __builtin_prefetch(c->q + i, 0, 1);

    for (uint32_t h = c->h_start; h < c->h_end; h++) {
        uint32_t kv_h = h / c->gqa_ratio;
        const float *q_head = c->q + h * c->hd;

        /* Attention scores */
        for (uint32_t p = 0; p <= c->pos; p++) {
            const float *k_pos = c->kv_k + (uint64_t)p * c->kv_dim + kv_h * c->hd;
            float dot = 0.0f;
            for (uint32_t i = 0; i < c->hd; i++)
                dot += q_head[i] * k_pos[i];
            c->att_buf[p] = dot * c->scale;
        }

        softmax(c->att_buf, c->pos + 1);

        /* Weighted sum of values */
        float *out_head = c->xb2 + h * c->hd;
        memset(out_head, 0, c->hd * sizeof(float));
        for (uint32_t p = 0; p <= c->pos; p++) {
            const float *v_pos = c->kv_v + (uint64_t)p * c->kv_dim + kv_h * c->hd;
            float a = c->att_buf[p];
            for (uint32_t i = 0; i < c->hd; i++)
                out_head[i] += a * v_pos[i];
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  llama_forward — Single token forward pass
 * ══════════════════════════════════════════════════════════════ */

int llama_forward(llama_state_t *s, uint32_t token)
{
    /* Brandon-arch (block-shared TinyLlama) takes a separate path. */
    if (s->arch[0] == 'b')
        return brandon_forward(s, token);

    uint32_t dim     = s->dim;
    uint32_t kv_dim  = s->kv_dim;
    uint32_t hd      = s->head_dim;
    uint32_t pos     = s->pos;

    /* Tag whole forward pass for PMU profiling (shell cmd: `perf`). */
    extern void perf_phase_enter(int slot, const char *name);
    perf_phase_enter(0 /* PERF_PHASE_FORWARD */, "llama_forward");

    /* ── Embed token ── */
    embed_token(s->x, s->weights.token_embd, token, dim);

#ifdef __EMSCRIPTEN__
    /* Lazy GPU KV-cache + attn pipeline init for llama-arch models.
     * Mirrors the brandon init pattern. Gated on: F32 weights for
     * layer 0, rope_freq_base < 100000 (no Llama 3 NTK scaling — the
     * shader's pow(rope_base, -e) doesn't match scaled freqs), and
     * the user toggle (off by default; flip with `bdebug llama_attn`).
     * Shader now implements Llama 3 NTK rope_scaling (factor=32,
     * lo=1, hi=4, orig_ctx=8192) auto-gated on rope_base>=100000, so
     * both Llama 2 (theta=10000) and Llama 3.x families are
     * supported here. */
    extern bool g_llama_use_gpu_attn;
    extern bool g_llama_attn_init;
    if (g_llama_use_gpu_attn && !g_llama_attn_init &&
        s->n_layers > 0 &&
        (s->weights.layers[0].attn_q->type == GGML_TYPE_F32 ||
         s->weights.layers[0].attn_q->type == GGML_TYPE_Q4_K)) {
        extern int wasm_wgpu_kvcache_alloc(int layer, int max_seq, int kv_dim);
        bool all_ok = true;
        for (uint32_t l = 0; l < s->n_layers; l++) {
            if (wasm_wgpu_kvcache_alloc((int)l,
                                         (int)s->max_seq,
                                         (int)kv_dim) != 0) {
                all_ok = false; break;
            }
        }
        g_llama_attn_init = all_ok;
        if (all_ok) serial_puts("[llama] GPU attn KV-cache pool ready\n");
    }
#endif

    /* ── Transformer layers ── */
    for (uint32_t l = 0; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        /* Attention norm */
        rmsnorm(s->xb, s->x, norm_data(ly->attn_norm), dim);

#ifdef __EMSCRIPTEN__
        /* GPU fused-attn fast path. Replaces QKV+RoPE+KV-cache+
         * attention+output-projection with a single GPU dispatch.
         * Requires all four attn weights to be F32 (the shader has no
         * Q4_K dequant inline yet) and a non-scaled RoPE base. KV
         * cache stays GPU-side across the whole generation. */
        bool gpu_attn_ok = false;
        bool all_f32 = ly->attn_q->type      == GGML_TYPE_F32 &&
                       ly->attn_k->type      == GGML_TYPE_F32 &&
                       ly->attn_v->type      == GGML_TYPE_F32 &&
                       ly->attn_output->type == GGML_TYPE_F32;
        bool all_q4k = ly->attn_q->type      == GGML_TYPE_Q4_K &&
                       ly->attn_k->type      == GGML_TYPE_Q4_K &&
                       ly->attn_v->type      == GGML_TYPE_Q4_K &&
                       ly->attn_output->type == GGML_TYPE_Q4_K;
        extern bool g_llama_use_gpu_predequant;
        int weight_dtype = all_f32 ? 0
                         : (all_q4k ? (g_llama_use_gpu_predequant ? 3 : 2)
                                    : -1);
        if (g_llama_use_gpu_attn && g_llama_attn_init && weight_dtype >= 0) {
            extern int wasm_wgpu_fused_attn(int layer,
                const float *x, const void *wq, const void *wk,
                const void *wv, const void *wo, float *out,
                int dim, int kv_dim, int head_dim, int n_heads,
                int n_kv_heads, int gqa_ratio, int pos, int max_seq,
                float scale, float rope_base, int weight_dtype);
            float scale = 1.0f / sqrtf_bare((float)hd);
            int rc = wasm_wgpu_fused_attn((int)l,
                s->xb,
                ly->attn_q->data,
                ly->attn_k->data,
                ly->attn_v->data,
                ly->attn_output->data,
                s->xb,
                (int)dim, (int)kv_dim, (int)hd, (int)s->n_heads,
                (int)s->n_kv_heads, (int)s->gqa_ratio,
                (int)pos, (int)s->max_seq, scale, s->rope_freq_base,
                weight_dtype);
            if (rc == 0) {
                vec_add(s->x, s->x, s->xb, dim);
                gpu_attn_ok = true;
            }
        }
        if (gpu_attn_ok) goto llama_ffn_block;
#endif

        /* Q, K, V projections — K and V on APs, Q on BSP */
        if (ap_worker_count > 0) {
            matvec_arg_t k_arg = {s->k, ly->attn_k, s->xb, kv_dim, dim};
            matvec_arg_t v_arg = {s->v, ly->attn_v, s->xb, kv_dim, dim};
            int k_ap = smp_submit_any(matvec_worker, &k_arg, NULL);
            int v_ap = (k_ap >= 0) ? smp_submit_any(matvec_worker, &v_arg, NULL) : -1;

            matvec(s->q, ly->attn_q, s->xb, dim, dim);  /* BSP does Q (largest) */

            if (k_ap >= 0) smp_wait(k_ap);
            if (v_ap >= 0) smp_wait(v_ap);
            else matvec(s->v, ly->attn_v, s->xb, kv_dim, dim);
            if (k_ap < 0) matvec(s->k, ly->attn_k, s->xb, kv_dim, dim);

        } else {
            matvec(s->q, ly->attn_q, s->xb, dim, dim);
            matvec(s->k, ly->attn_k, s->xb, kv_dim, dim);
            matvec(s->v, ly->attn_v, s->xb, kv_dim, dim);
        }

        /* RoPE */
        rope(s->q, s->n_heads,    hd, pos, s->rope_freq_base);
        rope(s->k, s->n_kv_heads, hd, pos, s->rope_freq_base);

        /* Store K, V in cache */
        float *kc = s->kv_cache[l].k + (uint64_t)pos * kv_dim;
        float *vc = s->kv_cache[l].v + (uint64_t)pos * kv_dim;
        memcpy(kc, s->k, kv_dim * sizeof(float));
        memcpy(vc, s->v, kv_dim * sizeof(float));

        /* ── Grouped Query Attention ── */
        float scale = 1.0f / sqrtf_bare((float)hd);

        /* Partition heads across APs + BSP */
        {
            int n_workers = ap_worker_count;
            if (n_workers > LLAMA_MAX_AP_SCRATCH)
                n_workers = LLAMA_MAX_AP_SCRATCH;
            /* Only use APs that have scratch buffers allocated */
            while (n_workers > 0 && !s->att_scratch[n_workers - 1])
                n_workers--;

            if (n_workers > 0 && s->n_heads >= 4) {
                int total_parts = n_workers + 1;
                uint32_t chunk = s->n_heads / total_parts;
                if (chunk == 0) { chunk = 1; n_workers = s->n_heads - 1; }

                attn_chunk_t chunks[LLAMA_MAX_AP_SCRATCH];
                int ap_ids[LLAMA_MAX_AP_SCRATCH];

                for (int w = 0; w < n_workers; w++) {
                    chunks[w].q         = s->q;
                    chunks[w].xb2       = s->xb2;
                    chunks[w].kv_k      = s->kv_cache[l].k;
                    chunks[w].kv_v      = s->kv_cache[l].v;
                    chunks[w].att_buf   = s->att_scratch[w];
                    chunks[w].h_start   = w * chunk;
                    chunks[w].h_end     = (w + 1) * chunk;
                    chunks[w].pos       = pos;
                    chunks[w].hd        = hd;
                    chunks[w].kv_dim    = kv_dim;
                    chunks[w].gqa_ratio = s->gqa_ratio;
                    chunks[w].scale     = scale;
                    ap_ids[w] = smp_submit_any(attention_heads_worker, &chunks[w], NULL);
                }

                /* BSP handles the remaining heads */
                uint32_t bsp_start = n_workers * chunk;
                for (uint32_t h = bsp_start; h < s->n_heads; h++) {
                    uint32_t kv_h = h / s->gqa_ratio;
                    const float *q_head = s->q + h * hd;

                    for (uint32_t p = 0; p <= pos; p++) {
                        const float *k_pos = s->kv_cache[l].k + (uint64_t)p * kv_dim + kv_h * hd;
                        float dot = 0.0f;
                        for (uint32_t i = 0; i < hd; i++)
                            dot += q_head[i] * k_pos[i];
                        s->att[p] = dot * scale;
                    }
                    softmax(s->att, pos + 1);

                    float *out_head = s->xb2 + h * hd;
                    memset(out_head, 0, hd * sizeof(float));
                    for (uint32_t p = 0; p <= pos; p++) {
                        const float *v_pos = s->kv_cache[l].v + (uint64_t)p * kv_dim + kv_h * hd;
                        float a = s->att[p];
                        for (uint32_t i = 0; i < hd; i++)
                            out_head[i] += a * v_pos[i];
                    }
                }

                /* Wait for AP workers */
                for (int w = 0; w < n_workers; w++)
                    if (ap_ids[w] >= 0) smp_wait(ap_ids[w]);
            } else {
                /* Serial fallback */
                for (uint32_t h = 0; h < s->n_heads; h++) {
                    uint32_t kv_h = h / s->gqa_ratio;
                    const float *q_head = s->q + h * hd;

                    for (uint32_t p = 0; p <= pos; p++) {
                        const float *k_pos = s->kv_cache[l].k + (uint64_t)p * kv_dim + kv_h * hd;
                        float dot = 0.0f;
                        for (uint32_t i = 0; i < hd; i++)
                            dot += q_head[i] * k_pos[i];
                        s->att[p] = dot * scale;
                    }
                    softmax(s->att, pos + 1);

                    float *out_head = s->xb2 + h * hd;
                    memset(out_head, 0, hd * sizeof(float));
                    for (uint32_t p = 0; p <= pos; p++) {
                        const float *v_pos = s->kv_cache[l].v + (uint64_t)p * kv_dim + kv_h * hd;
                        float a = s->att[p];
                        for (uint32_t i = 0; i < hd; i++)
                            out_head[i] += a * v_pos[i];
                    }
                }
            }
        }

        /* Output projection */
        matvec(s->xb, ly->attn_output, s->xb2, dim, dim);

        /* Residual connection */
        vec_add(s->x, s->x, s->xb, dim);

#ifdef __EMSCRIPTEN__
    llama_ffn_block:
        /* GPU FFN fast path: single compute dispatch fuses rmsnorm +
         * gate/up Q4_K matvecs + SwiGLU + down Q4_K matvec + residual.
         * Updates s->x in place. Conditions: all three FFN weights are
         * Q4_K and the toggle is on (`bdebug llama_ffn 1`). */
        {
            extern bool g_llama_use_gpu_ffn;
            bool ffn_all_q4k = ly->ffn_gate->type == GGML_TYPE_Q4_K &&
                               ly->ffn_up->type   == GGML_TYPE_Q4_K &&
                               ly->ffn_down->type == GGML_TYPE_Q4_K;
            if (g_llama_use_gpu_ffn && ffn_all_q4k) {
                extern int wasm_wgpu_ffn_q4k(int layer, float *x,
                    const void *w_gate, const void *w_up, const void *w_down,
                    const float *ffn_norm, int dim, int ffn_dim, float eps);
                int rc = wasm_wgpu_ffn_q4k((int)l, s->x,
                    ly->ffn_gate->data, ly->ffn_up->data, ly->ffn_down->data,
                    norm_data(ly->ffn_norm),
                    (int)dim, (int)s->ffn_dim, 1e-5f);
                if (rc == 0) goto llama_ffn_done;
            }
        }
#endif
        /* ── FFN (CPU) ── */
        rmsnorm(s->xb, s->x, norm_data(ly->ffn_norm), dim);

        /* Gate + Up projections — Up on AP, Gate on BSP */
        if (ap_worker_count > 0) {
            matvec_arg_t up_arg = {s->hb2, ly->ffn_up, s->xb, s->ffn_dim, dim};
            int up_ap = smp_submit_any(matvec_worker, &up_arg, NULL);

            matvec(s->hb, ly->ffn_gate, s->xb, s->ffn_dim, dim);  /* BSP does gate */

            if (up_ap >= 0) smp_wait(up_ap);
            else matvec(s->hb2, ly->ffn_up, s->xb, s->ffn_dim, dim);

        } else {
            matvec(s->hb,  ly->ffn_gate, s->xb, s->ffn_dim, dim);
            matvec(s->hb2, ly->ffn_up,   s->xb, s->ffn_dim, dim);
        }

        /* SwiGLU: SiLU(gate) * up */
        silu_inplace(s->hb, s->ffn_dim);
        vec_mul(s->hb, s->hb, s->hb2, s->ffn_dim);

        /* Down projection */
        matvec(s->xb, ly->ffn_down, s->hb, dim, s->ffn_dim);

        /* Residual connection */
        vec_add(s->x, s->x, s->xb, dim);

#ifdef __EMSCRIPTEN__
    llama_ffn_done:
#endif
        /* Prefetch next layer's weights into cache while we loop back.
         * The attn_q weight matrix is the first thing accessed in the
         * next iteration — prefetch its start and the norm weights. */
        if (l + 1 < s->n_layers) {
            llama_layer_t *next = &s->weights.layers[l + 1];
            __builtin_prefetch(norm_data(next->attn_norm), 0, 1);
            __builtin_prefetch(next->attn_q->data, 0, 0);
            __builtin_prefetch(next->attn_k->data, 0, 0);
        }
    }

    /* ── Final norm + logits ── */
    rmsnorm(s->x, s->x, norm_data(s->weights.output_norm), dim);
#ifdef __EMSCRIPTEN__
    /* GPU LM head fast path: Q6_K matvec over the full vocab. The
     * rmsnorm above runs on CPU (cheap, ~2K elements). The matvec
     * is the dominant CPU cost — 128K × 2048 with Q6_K dequant inline
     * → ~150 ms/tok. GPU multi-WG handles it in <30 ms.
     * Gated on the toggle + Q6_K output weight. */
    extern bool g_llama_use_gpu_lm_head;
    bool lm_gpu_ok = false;
    if (g_llama_use_gpu_lm_head &&
        s->weights.output->type == GGML_TYPE_Q6_K) {
        extern int wasm_wgpu_lm_head_q6k(const float *x_norm, const void *w,
                                          float *logits, int dim, int vocab);
        int rc = wasm_wgpu_lm_head_q6k(s->x, s->weights.output->data,
                                        s->logits, (int)dim, (int)s->vocab_size);
        if (rc == 0) lm_gpu_ok = true;
    }
    if (!lm_gpu_ok)
        matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);
#else
    matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);
#endif

    s->pos++;

    extern void perf_phase_exit(int slot);
    perf_phase_exit(0 /* PERF_PHASE_FORWARD */);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  brandon-arch forward — block_sharing + DenseFormer DWA +
 *  Value Residual Learning + register tokens.
 *  Spec: ~/osito-a-models/docs/brandon-arch-spec.md
 * ══════════════════════════════════════════════════════════════ */

/* Runtime debug toggles to bisect brandon's forward pass. Default ON
 * matches the spec; setting any to 0 lets us check whether the
 * gibberish output traces to that one feature. */
static bool g_brandon_use_dwa            = true;
static bool g_brandon_use_value_residual = true;

/* Llama-arch GPU attention toggle (off by default; the F32-only gate
 * means Llama 1B Q4_K_M skips this path automatically — useful today
 * for F32 TinyLlama variants and any future F32 dequant cache). */
bool g_llama_use_gpu_attn       = false;
bool g_llama_use_gpu_predequant = false;  /* dtype-3: Q4_K → on-GPU F32 mirror */
bool g_llama_use_gpu_ffn        = false;  /* fused Q4_K FFN compute */
bool g_llama_use_gpu_lm_head    = false;  /* Q6_K LM head matvec */
bool g_llama_attn_init          = false;

static bool g_brandon_use_registers      = true;
static bool g_brandon_debug_logits       = false;

/* Off by default. When ON and the model uses neither value-residual
 * nor DWA (the fused_attn WGSL shader has no slot for either), the
 * attention block (rmsnorm-output through output projection) is
 * dispatched to WebGPU as a single fused kernel instead of running
 * the CPU loop. Toggle via 'bdebug attn 1'. */
int  g_brandon_use_gpu_attn       = 0;
static int  g_brandon_attn_init   = 0;

/* One pass through the n_layers logical stack. produce_logits=false
 * is used during register prefill (the output isn't consumed). */
static int brandon_forward_one(llama_state_t *s, uint32_t pos, bool produce_logits)
{
    uint32_t dim    = s->dim;
    uint32_t kv_dim = s->kv_dim;
    uint32_t hd     = s->head_dim;

    bool eff_use_dwa            = s->use_dwa            && g_brandon_use_dwa;
    bool eff_use_value_residual = s->use_value_residual && g_brandon_use_value_residual;

    /* dwa_buf[0] := embedding (input to layer 0). */
    if (eff_use_dwa)
        memcpy(s->dwa_buf, s->x, dim * sizeof(float));

    /* Value Residual Learning is per-forward (per-token), not per-slot.
     * Reset the capture flag so layer 0 of THIS forward refreshes v_first. */
    s->v_first_captured = false;

#ifdef __EMSCRIPTEN__
    /* One-shot: allocate per-layer GPU KV cache the first time we
     * enter forward with the GPU attn flag on. */
    if (g_brandon_use_gpu_attn && !g_brandon_attn_init &&
        !eff_use_dwa && !eff_use_value_residual) {
        extern int wasm_wgpu_init(void);
        extern int wasm_wgpu_kvcache_alloc(int layer, int max_seq, int kv_dim);
        if (wasm_wgpu_init()) {
            for (uint32_t l = 0; l < s->n_layers; l++)
                wasm_wgpu_kvcache_alloc((int)l, (int)s->max_seq, (int)kv_dim);
            g_brandon_attn_init = 1;
            serial_puts("[brandon] GPU attn: KV cache pool allocated\n");
        }
    }
#endif

    for (uint32_t l = 0; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        /* Attention norm */
        rmsnorm(s->xb, s->x, norm_data(ly->attn_norm), dim);

#ifdef __EMSCRIPTEN__
        /* GPU fused-attn fast path. Replaces QKV+VR+RoPE+KV-cache+
         * attention+O-projection with a single GPU dispatch. Only
         * fires when the model is non-DWA non-VR F32. KV cache stays
         * GPU-side for the whole generation. */
        bool gpu_attn_ok = false;
        if (g_brandon_use_gpu_attn && g_brandon_attn_init &&
            !eff_use_dwa && !eff_use_value_residual &&
            ly->attn_q->type == GGML_TYPE_F32 &&
            ly->attn_k->type == GGML_TYPE_F32 &&
            ly->attn_v->type == GGML_TYPE_F32 &&
            ly->attn_output->type == GGML_TYPE_F32) {
            extern int wasm_wgpu_fused_attn(int layer,
                const float *x, const void *wq, const void *wk,
                const void *wv, const void *wo, float *out,
                int dim, int kv_dim, int head_dim, int n_heads,
                int n_kv_heads, int gqa_ratio, int pos, int max_seq,
                float scale, float rope_base, int weight_dtype);
            float scale = 1.0f / sqrtf_bare((float)hd);
            int rc = wasm_wgpu_fused_attn((int)l,
                s->xb,
                ly->attn_q->data,
                ly->attn_k->data,
                ly->attn_v->data,
                ly->attn_output->data,
                s->xb,                 /* output: post-O projection */
                (int)dim, (int)kv_dim, (int)hd, (int)s->n_heads,
                (int)s->n_kv_heads, (int)s->gqa_ratio,
                (int)pos, (int)s->max_seq, scale, s->rope_freq_base, 0);
            if (rc == 0) {
                vec_add(s->x, s->x, s->xb, dim);
                gpu_attn_ok = true;
            }
        }
        if (gpu_attn_ok) goto ffn_block;
#endif

        /* Q, K, V projections */
        matvec(s->q, ly->attn_q, s->xb, dim, dim);
        matvec(s->k, ly->attn_k, s->xb, kv_dim, dim);
        matvec(s->v, ly->attn_v, s->xb, kv_dim, dim);

        /* Value Residual Learning (model.py:243-244):
         *   - Layer 0: capture raw V (pre-RoPE, pre-residual). Layer 0
         *     itself uses raw V; do NOT add v_first to it.
         *   - Layers 1..n: add v_first element-wise to V before cache + attn.
         * V never gets RoPE. */
        if (eff_use_value_residual) {
            if (l == 0) {
                if (!s->v_first_captured) {
                    memcpy(s->v_first, s->v, kv_dim * sizeof(float));
                    s->v_first_captured = true;
                }
            } else {
                for (uint32_t i = 0; i < kv_dim; i++)
                    s->v[i] += s->v_first[i];
            }
        }

        /* RoPE — Q and K only; V stays unrotated */
        rope(s->q, s->n_heads,    hd, pos, s->rope_freq_base);
        rope(s->k, s->n_kv_heads, hd, pos, s->rope_freq_base);

        /* Store K, V in cache at this position */
        float *kc = s->kv_cache[l].k + (uint64_t)pos * kv_dim;
        float *vc = s->kv_cache[l].v + (uint64_t)pos * kv_dim;
        memcpy(kc, s->k, kv_dim * sizeof(float));
        memcpy(vc, s->v, kv_dim * sizeof(float));

        /* GQA serial path — small model, AP orchestration not worth it */
        float scale = 1.0f / sqrtf_bare((float)hd);
        for (uint32_t h = 0; h < s->n_heads; h++) {
            uint32_t kv_h = h / s->gqa_ratio;
            const float *q_head = s->q + h * hd;

            for (uint32_t p = 0; p <= pos; p++) {
                const float *k_pos = s->kv_cache[l].k + (uint64_t)p * kv_dim + kv_h * hd;
                float dot = 0.0f;
                for (uint32_t i = 0; i < hd; i++)
                    dot += q_head[i] * k_pos[i];
                s->att[p] = dot * scale;
            }
            softmax(s->att, pos + 1);

            float *out_head = s->xb2 + h * hd;
            memset(out_head, 0, hd * sizeof(float));
            for (uint32_t p = 0; p <= pos; p++) {
                const float *v_pos = s->kv_cache[l].v + (uint64_t)p * kv_dim + kv_h * hd;
                float a = s->att[p];
                for (uint32_t i = 0; i < hd; i++)
                    out_head[i] += a * v_pos[i];
            }
        }

        /* Output projection + residual */
        matvec(s->xb, ly->attn_output, s->xb2, dim, dim);
        vec_add(s->x, s->x, s->xb, dim);

ffn_block: ;
        /* FFN: rmsnorm → SwiGLU → residual */
        rmsnorm(s->xb, s->x, norm_data(ly->ffn_norm), dim);
        matvec(s->hb,  ly->ffn_gate, s->xb, s->ffn_dim, dim);
        matvec(s->hb2, ly->ffn_up,   s->xb, s->ffn_dim, dim);
        silu_inplace(s->hb, s->ffn_dim);
        vec_mul(s->hb, s->hb, s->hb2, s->ffn_dim);
        matvec(s->xb, ly->ffn_down, s->hb, dim, s->ffn_dim);
        vec_add(s->x, s->x, s->xb, dim);

        /* DenseFormer DWA: append layer-L output, replace x with weighted sum */
        if (eff_use_dwa) {
            float *h_slot = s->dwa_buf + (uint64_t)(l + 1) * dim;
            memcpy(h_slot, s->x, dim * sizeof(float));

            const float *dwa = (const float *)s->dwa_weights->data;
            uint32_t row_stride = s->n_layers + 1;
            const float *w_row = dwa + (uint64_t)l * row_stride;

            for (uint32_t i = 0; i < dim; i++) s->x[i] = 0.0f;
            for (uint32_t j = 0; j <= l + 1; j++) {
                float w = w_row[j];
                if (w == 0.0f) continue;
                const float *src = s->dwa_buf + (uint64_t)j * dim;
                for (uint32_t i = 0; i < dim; i++)
                    s->x[i] += w * src[i];
            }
        }
    }

    /* Final norm + LM head (skipped during register prefill) */
    if (produce_logits) {
#ifdef __EMSCRIPTEN__
        /* Fused GPU path: rmsnorm + matvec dispatched as one command
         * buffer with a single mapAsync. Skips the CPU pass entirely
         * for this op; falls back if WebGPU init failed. */
        extern int wasm_wgpu_brandon_lm_head(const float *x,
            const float *w_norm, const float *w_lm, float *logits,
            int dim, int vocab, float eps);
        bool gpu_ok = false;
        if (g_brandon_use_gpu_matvec &&
            s->weights.output->type == GGML_TYPE_F32 &&
            s->weights.output_norm->type == GGML_TYPE_F32) {
            if (wasm_wgpu_brandon_lm_head(s->x,
                    norm_data(s->weights.output_norm),
                    (const float *)s->weights.output->data,
                    s->logits, (int)dim, (int)s->vocab_size, 1e-5f) == 0)
                gpu_ok = true;
        }
        if (!gpu_ok) {
            rmsnorm(s->x, s->x, norm_data(s->weights.output_norm), dim);
            matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);
        }
#else
        rmsnorm(s->x, s->x, norm_data(s->weights.output_norm), dim);
        matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);
#endif

        /* NaN/inf scrub */
        for (uint32_t i = 0; i < s->vocab_size; i++) {
            float v = s->logits[i];
            if (v != v || v > 1e30f || v < -1e30f)
                s->logits[i] = -1e30f;
        }
    }
    return 0;
}

int brandon_forward(llama_state_t *s, uint32_t token)
{
    /* Lazy register prefill on the first call. The n_registers learnable
     * embeddings occupy positions 0..n_registers-1 of the KV cache; user
     * tokens then start at position n_registers. */
    if (g_brandon_use_registers && s->n_registers > 0 && !s->registers_prefilled) {
        const uint8_t *reg_data = (const uint8_t *)s->register_weights->data;
        uint32_t reg_dtype = s->register_weights->type;
        uint64_t reg_row_bytes = (uint64_t)s->dim *
                                 (reg_dtype == GGML_TYPE_F32 ? 4 : 2);

        for (uint32_t r = 0; r < s->n_registers; r++) {
            const uint8_t *src = reg_data + r * reg_row_bytes;
            if (reg_dtype == GGML_TYPE_F32) {
                memcpy(s->x, src, s->dim * sizeof(float));
            } else {
                /* F16 → F32 (matches embed_token's f16 branch) */
                const uint16_t *h = (const uint16_t *)src;
                for (uint32_t i = 0; i < s->dim; i++)
                    s->x[i] = f16_to_f32(h[i]);
            }
            if (brandon_forward_one(s, s->pos, /*produce_logits=*/false) != 0)
                return -1;
            s->pos++;
        }
        s->registers_prefilled = true;
    }

    /* User token forward */
    embed_token(s->x, s->weights.token_embd, token, s->dim);
    if (brandon_forward_one(s, s->pos, /*produce_logits=*/true) != 0)
        return -1;
    s->pos++;

    /* Debug: dump top-3 candidate tokens + their logit values to serial. */
    if (g_brandon_debug_logits) {
        float *L = s->logits;
        uint32_t n = s->vocab_size;
        uint32_t a0 = 0, a1 = 0, a2 = 0;
        float v0 = -1e30f, v1 = -1e30f, v2 = -1e30f;
        for (uint32_t i = 0; i < n; i++) {
            float v = L[i];
            if (v > v0) { v2 = v1; a2 = a1; v1 = v0; a1 = a0; v0 = v; a0 = i; }
            else if (v > v1) { v2 = v1; a2 = a1; v1 = v; a1 = i; }
            else if (v > v2) { v2 = v; a2 = i; }
        }
        /* Mean + std of logits (rough sanity). */
        float sum = 0.0f, sumsq = 0.0f;
        for (uint32_t i = 0; i < n; i++) { sum += L[i]; sumsq += L[i]*L[i]; }
        float mean = sum / (float)n;
        float var = sumsq / (float)n - mean * mean;
        serial_puts("[DBG] top3 tok="); serial_putdec(a0);
        serial_puts(","); serial_putdec(a1);
        serial_puts(","); serial_putdec(a2);
        serial_puts(" vals*1k="); serial_putdec((uint64_t)(uint32_t)(int32_t)(v0*1000.0f));
        serial_puts(","); serial_putdec((uint64_t)(uint32_t)(int32_t)(v1*1000.0f));
        serial_puts(","); serial_putdec((uint64_t)(uint32_t)(int32_t)(v2*1000.0f));
        serial_puts(" mean*1k="); serial_putdec((uint64_t)(uint32_t)(int32_t)(mean*1000.0f));
        serial_puts(" var*1k="); serial_putdec((uint64_t)(uint32_t)(int32_t)(var*1000.0f));
        serial_puts("\n");
    }
    return 0;
}

/* ── Forward from a specific layer (for speculation adoption) ─ */

int llama_forward_from_layer(llama_state_t *s, uint32_t token,
                             uint32_t start_layer)
{
    uint32_t dim    = s->dim;
    uint32_t kv_dim = s->kv_dim;
    uint32_t hd     = s->head_dim;
    uint32_t pos    = s->pos;

    if (start_layer == 0)
        embed_token(s->x, s->weights.token_embd, token, dim);

    for (uint32_t l = start_layer; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        rmsnorm(s->xb, s->x, norm_data(ly->attn_norm), dim);
        matvec(s->q, ly->attn_q, s->xb, dim, dim);
        matvec(s->k, ly->attn_k, s->xb, kv_dim, dim);
        matvec(s->v, ly->attn_v, s->xb, kv_dim, dim);

        rope(s->q, s->n_heads,    hd, pos, s->rope_freq_base);
        rope(s->k, s->n_kv_heads, hd, pos, s->rope_freq_base);

        float *kc = s->kv_cache[l].k + (uint64_t)pos * kv_dim;
        float *vc = s->kv_cache[l].v + (uint64_t)pos * kv_dim;
        memcpy(kc, s->k, kv_dim * sizeof(float));
        memcpy(vc, s->v, kv_dim * sizeof(float));

        /* Simplified GQA (no SMP in continuation path) */
        for (uint32_t h = 0; h < s->n_heads; h++) {
            uint32_t kv_h = h / s->gqa_ratio;
            float *qh = s->q + h * hd;
            float *xb2h = s->xb2 + h * hd;
            float max_s = -1e30f;
            for (uint32_t t = 0; t <= pos; t++) {
                float *kt = s->kv_cache[l].k + t * kv_dim + kv_h * hd;
                float sc = 0;
                for (uint32_t d = 0; d < hd; d++) sc += qh[d] * kt[d];
                sc /= sqrtf_bare((float)hd);
                s->att[t] = sc;
                if (sc > max_s) max_s = sc;
            }
            float sum = 0;
            for (uint32_t t = 0; t <= pos; t++) {
                s->att[t] = expf_bare(s->att[t] - max_s);
                sum += s->att[t];
            }
            float inv = 1.0f / sum;
            for (uint32_t t = 0; t <= pos; t++) s->att[t] *= inv;
            for (uint32_t d = 0; d < hd; d++) xb2h[d] = 0;
            for (uint32_t t = 0; t <= pos; t++) {
                float *vt = s->kv_cache[l].v + t * kv_dim + kv_h * hd;
                float w = s->att[t];
                for (uint32_t d = 0; d < hd; d++) xb2h[d] += w * vt[d];
            }
        }
        matvec(s->xb, ly->attn_output, s->xb2, dim, dim);
        for (uint32_t i = 0; i < dim; i++) s->x[i] += s->xb[i];

        /* FFN */
        rmsnorm(s->xb, s->x, norm_data(ly->ffn_norm), dim);
        matvec(s->hb, ly->ffn_gate, s->xb, s->ffn_dim, dim);
        matvec(s->hb2, ly->ffn_up, s->xb, s->ffn_dim, dim);
        for (uint32_t i = 0; i < s->ffn_dim; i++)
            s->hb[i] = (s->hb[i] / (1.0f + expf_bare(-s->hb[i]))) * s->hb2[i];
        matvec(s->xb, ly->ffn_down, s->hb, dim, s->ffn_dim);
        for (uint32_t i = 0; i < dim; i++) s->x[i] += s->xb[i];
    }

    rmsnorm(s->x, s->x, norm_data(s->weights.output_norm), dim);
    matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);
    s->pos++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  NVMe-direct layer streaming
 *
 *  For models larger than RAM: only 2 layer buffers in memory.
 *  DMA-prefetch next layer while computing current layer.
 *  At NVMe 3.5 GB/s and ~4MB/layer, DMA is ~1ms vs ~62ms compute.
 * ══════════════════════════════════════════════════════════════ */

int llama_init_streaming(llama_state_t *s)
{
    if (!s || !s->model) return -1;

    /* Build DMA map from GGUF tensor offsets → NVMe LBAs */
    tensor_dma_map_t *dmap = (tensor_dma_map_t *)PHYS_TO_VIRT(
        mem_alloc_aligned(sizeof(tensor_dma_map_t), 8));
    if (!dmap) return -1;

    if (tensor_dma_build_map(s->model, dmap) < 0) return -1;
    s->dma_map = dmap;

    /* Calculate per-layer weight size.  Each layer has 9 tensors:
     * attn_norm, attn_q, attn_k, attn_v, attn_output,
     * ffn_norm, ffn_gate, ffn_up, ffn_down. */
    uint64_t layer_size = 0;
    llama_layer_t *ly = &s->weights.layers[0];
    gguf_tensor_t *tensors[] = {
        ly->attn_norm, ly->attn_q, ly->attn_k, ly->attn_v, ly->attn_output,
        ly->ffn_norm, ly->ffn_gate, ly->ffn_up, ly->ffn_down
    };
    for (int i = 0; i < 9; i++)
        layer_size += tensors[i]->size;

    /* Round up to page boundary */
    layer_size = (layer_size + 4095) & ~4095ULL;
    s->layer_buf_size = layer_size;

    /* Allocate two layer buffers (ping-pong) */
    s->layer_buf[0] = PHYS_TO_VIRT(mem_alloc_aligned(layer_size, 4096));
    s->layer_buf[1] = PHYS_TO_VIRT(mem_alloc_aligned(layer_size, 4096));
    if (!s->layer_buf[0] || !s->layer_buf[1]) return -1;

    /* Record the first tensor index for each layer in the DMA map */
    for (uint32_t l = 0; l < s->n_layers && l < 256; l++) {
        /* Find the layer's attn_norm tensor index in the global tensor list */
        gguf_tensor_t *norm = s->weights.layers[l].attn_norm;
        for (uint32_t t = 0; t < dmap->num_entries; t++) {
            if (&s->model->tensors[t] == norm) {
                s->layer_tensor_start[l] = t;
                break;
            }
        }
    }

    serial_puts("[INF] Streaming init: ");
    serial_putdec(layer_size / 1024);
    serial_puts(" KB/layer, 2 ping-pong buffers\n");
    return 0;
}

/* Rebase a layer's tensor data pointers into a layer buffer.
 * After NVMe DMA loads the layer's weights into buf, the tensor
 * descriptors need to point into buf instead of the original mmap. */
static void layer_rebase_tensors(llama_layer_t *ly, void *buf,
                                 uint64_t layer_base_offset,
                                 uint64_t tensor_data_offset)
{
    gguf_tensor_t *tensors[] = {
        ly->attn_norm, ly->attn_q, ly->attn_k, ly->attn_v, ly->attn_output,
        ly->ffn_norm, ly->ffn_gate, ly->ffn_up, ly->ffn_down
    };
    uint8_t *base = (uint8_t *)buf;
    for (int i = 0; i < 9; i++) {
        uint64_t tensor_file_offset = tensor_data_offset + tensors[i]->offset;
        tensors[i]->data = base + (tensor_file_offset - layer_base_offset);
    }
}

int llama_forward_streaming(llama_state_t *s, uint32_t token)
{
    if (!s->dma_map || !s->layer_buf[0]) return llama_forward(s, token);

    uint32_t dim     = s->dim;
    uint32_t kv_dim  = s->kv_dim;
    uint32_t pos     = s->pos;

    /* Embed token (uses global token_embd — always in RAM) */
    embed_token(s->x, s->weights.token_embd, token, dim);

    /* Pre-load layer 0 synchronously into buf[0] */
    tensor_dma_entry_t *e0 = &s->dma_map->entries[s->layer_tensor_start[0]];
    uint64_t layer0_lba = e0->lba;
    uint32_t lbas = (uint32_t)(s->layer_buf_size / nvme_get_lba_size());
    nvme_read(layer0_lba, lbas, (void *)VIRT_TO_PHYS(s->layer_buf[0]));
    layer_rebase_tensors(&s->weights.layers[0], s->layer_buf[0],
                         e0->lba * nvme_get_lba_size(),
                         s->dma_map->tensor_data_offset);

    for (uint32_t l = 0; l < s->n_layers; l++) {
        int cur = l & 1;
        int nxt = 1 - cur;

        /* Start async DMA for layer l+1 into the other buffer */
        int dma_cid = -1;
        if (l + 1 < s->n_layers) {
            tensor_dma_entry_t *en = &s->dma_map->entries[s->layer_tensor_start[l + 1]];
            dma_cid = nvme_read_async(en->lba, lbas,
                                      (uint64_t)VIRT_TO_PHYS(s->layer_buf[nxt]));
        }

        /* Compute layer l — identical to llama_forward's inner loop
         * but tensors point into layer_buf[cur] via rebase */
        llama_layer_t *ly = &s->weights.layers[l];

        /* ... the full layer computation goes here ...
         * (attn norm, Q/K/V, RoPE, KV cache, attention, FFN)
         * This is the same code as llama_forward lines 500-667. */
        rmsnorm(s->xb, s->x, norm_data(ly->attn_norm), dim);
        matvec(s->q, ly->attn_q, s->xb, dim, dim);
        matvec(s->k, ly->attn_k, s->xb, kv_dim, dim);
        matvec(s->v, ly->attn_v, s->xb, kv_dim, dim);

        /* RoPE */
        rope(s->q, s->n_heads, s->head_dim, pos, s->rope_freq_base);
        rope(s->k, s->n_kv_heads, s->head_dim, pos, s->rope_freq_base);

        /* KV cache store */
        float *kc = s->kv_cache[l].k + pos * kv_dim;
        float *vc = s->kv_cache[l].v + pos * kv_dim;
        for (uint32_t i = 0; i < kv_dim; i++) { kc[i] = s->k[i]; vc[i] = s->v[i]; }

        /* Grouped query attention (simplified single-thread path) */
        for (uint32_t h = 0; h < s->n_heads; h++) {
            uint32_t kv_h = h / s->gqa_ratio;
            float *qh = s->q + h * s->head_dim;
            float *xb2h = s->xb2 + h * s->head_dim;
            float max_score = -1e30f;

            for (uint32_t t = 0; t <= pos; t++) {
                float *kt = s->kv_cache[l].k + t * kv_dim + kv_h * s->head_dim;
                float score = 0;
                for (uint32_t d = 0; d < s->head_dim; d++) score += qh[d] * kt[d];
                score /= sqrtf_bare((float)s->head_dim);
                s->att[t] = score;
                if (score > max_score) max_score = score;
            }
            /* Softmax */
            float sum = 0;
            for (uint32_t t = 0; t <= pos; t++) {
                s->att[t] = expf_bare(s->att[t] - max_score);
                sum += s->att[t];
            }
            float inv = 1.0f / sum;
            for (uint32_t t = 0; t <= pos; t++) s->att[t] *= inv;
            /* Weighted V sum */
            for (uint32_t d = 0; d < s->head_dim; d++) xb2h[d] = 0;
            for (uint32_t t = 0; t <= pos; t++) {
                float *vt = s->kv_cache[l].v + t * kv_dim + kv_h * s->head_dim;
                float w = s->att[t];
                for (uint32_t d = 0; d < s->head_dim; d++) xb2h[d] += w * vt[d];
            }
        }
        matvec(s->xb, ly->attn_output, s->xb2, dim, dim);
        for (uint32_t i = 0; i < dim; i++) s->x[i] += s->xb[i];

        /* FFN */
        rmsnorm(s->xb, s->x, norm_data(ly->ffn_norm), dim);
        matvec(s->hb, ly->ffn_gate, s->xb, s->ffn_dim, dim);
        matvec(s->hb2, ly->ffn_up, s->xb, s->ffn_dim, dim);
        for (uint32_t i = 0; i < s->ffn_dim; i++)
            s->hb[i] = (s->hb[i] / (1.0f + expf_bare(-s->hb[i]))) * s->hb2[i];
        matvec(s->xb, ly->ffn_down, s->hb, dim, s->ffn_dim);
        for (uint32_t i = 0; i < dim; i++) s->x[i] += s->xb[i];

        /* Wait for next layer's DMA to complete */
        if (dma_cid >= 0) {
            nvme_wait_cq((uint16_t)dma_cid);
            /* Rebase next layer's tensor pointers into the new buffer */
            tensor_dma_entry_t *en = &s->dma_map->entries[s->layer_tensor_start[l + 1]];
            layer_rebase_tensors(&s->weights.layers[l + 1], s->layer_buf[nxt],
                                 en->lba * nvme_get_lba_size(),
                                 s->dma_map->tensor_data_offset);
        }
    }

    /* Final: output norm + logits (uses global weights — always in RAM) */
    rmsnorm(s->x, s->x, norm_data(s->weights.output_norm), dim);
    matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);

    s->pos++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Speculative token execution
 *
 *  While BSP does softmax + top-p sampling for token T, an AP
 *  starts the forward pass for the PREDICTED token T+1 (argmax).
 *  On hit (~60-80%): skip 4 layers of compute.  On miss: discard.
 *  Shadow KV buffers prevent main cache corruption.
 * ══════════════════════════════════════════════════════════════ */

static spec_state_t *g_spec;  /* global for decode loop access */

int llama_spec_init(llama_state_t *s)
{
    uint32_t dim = s->dim, kv_dim = s->kv_dim, ffn_dim = s->ffn_dim;
    uint64_t total = (2 * dim + dim + kv_dim + kv_dim + s->max_seq +
                      2 * ffn_dim + dim) * sizeof(float);
    total += SPEC_MAX_LAYERS * 2 * kv_dim * sizeof(float);

    void *phys = mem_alloc_aligned(total, 64);
    if (!phys) return -1;
    float *p = (float *)PHYS_TO_VIRT(phys);

    spec_state_t *sp = (spec_state_t *)PHYS_TO_VIRT(
        mem_alloc_aligned(sizeof(spec_state_t), 8));
    if (!sp) return -1;

    sp->spec_x   = p; p += dim;
    sp->spec_xb  = p; p += dim;
    sp->spec_xb2 = p; p += dim;
    sp->spec_q   = p; p += dim;
    sp->spec_k   = p; p += kv_dim;
    sp->spec_v   = p; p += kv_dim;
    sp->spec_att = p; p += s->max_seq;
    sp->spec_hb  = p; p += ffn_dim;
    sp->spec_hb2 = p; p += ffn_dim;
    for (int i = 0; i < SPEC_MAX_LAYERS; i++) {
        sp->shadow_k[i] = p; p += kv_dim;
        sp->shadow_v[i] = p; p += kv_dim;
    }
    sp->hits = sp->misses = 0;
    sp->spec_complete = 0;

    g_spec = sp;
    serial_puts("[INF] Speculative execution initialized\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  KV cache checkpoint/restore — instant prompt resume
 *
 *  Saves KV cache entries 0..pos-1 to NVMe via OsitoFS.
 *  Restore loads them back and sets pos, skipping recompute.
 * ══════════════════════════════════════════════════════════════ */

extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);

int llama_checkpoint_kv(llama_state_t *s, const char *filename)
{
    if (!s || s->pos == 0) return -1;

    /* Calculate total size: header + K/V data for all layers */
    uint64_t kv_bytes_per_layer = (uint64_t)s->pos * s->kv_dim * sizeof(float);
    uint64_t total = sizeof(llama_ckpt_header_t) +
                     s->n_layers * 2 * kv_bytes_per_layer;

    void *phys = mem_alloc_aligned(total, 4096);
    if (!phys) {
        serial_puts("[CKPT] Alloc failed\n");
        return -1;
    }
    uint8_t *buf = (uint8_t *)PHYS_TO_VIRT(phys);

    /* Write header */
    llama_ckpt_header_t *hdr = (llama_ckpt_header_t *)buf;
    hdr->magic    = LLAMA_CKPT_MAGIC;
    hdr->version  = 1;
    hdr->pos      = s->pos;
    hdr->n_layers = s->n_layers;
    hdr->kv_dim   = s->kv_dim;
    hdr->max_seq  = s->max_seq;

    /* Write K and V for each layer (only positions 0..pos-1) */
    uint8_t *p = buf + sizeof(llama_ckpt_header_t);
    for (uint32_t l = 0; l < s->n_layers; l++) {
        memcpy(p, s->kv_cache[l].k, kv_bytes_per_layer);
        p += kv_bytes_per_layer;
        memcpy(p, s->kv_cache[l].v, kv_bytes_per_layer);
        p += kv_bytes_per_layer;
    }

    /* Write to OsitoFS: create file then write data */
    void *file = osfs2_create(filename, total);
    int rc = file ? osfs2_write(file, 0, buf, total) : -1;
    extern void mem_free_pages(void *addr, uint64_t count);
    mem_free_pages(phys, (total + 4095) / 4096);

    if (rc < 0) {
        serial_puts("[CKPT] Write failed\n");
        return -1;
    }

    serial_puts("[CKPT] Saved KV cache: pos=");
    serial_putdec(s->pos);
    serial_puts(", ");
    serial_putdec(total / 1024);
    serial_puts(" KB\n");
    return 0;
}

int llama_restore_kv(llama_state_t *s, const char *filename)
{
    if (!s) return -1;

    /* Find and read the checkpoint file */
    extern void *osfs2_find(const char *name);
    extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);

    void *file = osfs2_find(filename);
    if (!file) {
        serial_puts("[CKPT] File not found: ");
        serial_puts(filename);
        serial_puts("\n");
        return -1;
    }

    /* Read header */
    llama_ckpt_header_t hdr;
    if (osfs2_read(file, 0, &hdr, sizeof(hdr)) < 0) return -1;

    if (hdr.magic != LLAMA_CKPT_MAGIC || hdr.version != 1) {
        serial_puts("[CKPT] Invalid checkpoint\n");
        return -1;
    }
    if (hdr.n_layers != s->n_layers || hdr.kv_dim != s->kv_dim) {
        serial_puts("[CKPT] Model mismatch\n");
        return -1;
    }
    if (hdr.pos > s->max_seq) {
        serial_puts("[CKPT] pos exceeds max_seq\n");
        return -1;
    }

    /* Read K and V for each layer */
    uint64_t kv_bytes_per_layer = (uint64_t)hdr.pos * hdr.kv_dim * sizeof(float);
    uint64_t offset = sizeof(llama_ckpt_header_t);

    for (uint32_t l = 0; l < hdr.n_layers; l++) {
        if (osfs2_read(file, offset, s->kv_cache[l].k, kv_bytes_per_layer) < 0)
            return -1;
        offset += kv_bytes_per_layer;
        if (osfs2_read(file, offset, s->kv_cache[l].v, kv_bytes_per_layer) < 0)
            return -1;
        offset += kv_bytes_per_layer;
    }

    s->pos = hdr.pos;

    serial_puts("[CKPT] Restored KV cache: pos=");
    serial_putdec(s->pos);
    serial_puts("\n");
    return 0;
}

/* ── Sampling ────────────────────────────────────────────────── */

uint32_t argmax(const float *v, uint32_t n)
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

/* PRNG: xorshift64 seeded from RDTSC */
static uint64_t rng_state;

static void rng_seed(void)
{
    rng_state = rdtsc();
    if (rng_state == 0) rng_state = 0xDEADBEEFCAFE;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* Random float in [0, 1) */
static float rng_float(void)
{
    return (float)(rng_next() >> 11) / (float)(1ULL << 53);
}

/* Index pair for sorting logits */
typedef struct { float val; uint32_t idx; } logit_idx_t;

/* Sample from logits with temperature + top-p (nucleus sampling).
 *
 * temperature: scales logits before softmax (0 = greedy, 1 = normal)
 * top_p: cumulative probability cutoff (0.9 = typical)
 *
 * Uses a partial sort: find top-k candidates above threshold,
 * then sample from their softmax distribution.
 */
uint32_t sample_topp(float *logits, uint32_t n,
                     float temperature, float top_p)
{
    /* Temperature 0 = greedy */
    if (temperature <= 0.0f)
        return argmax(logits, n);

    /* Apply temperature */
    float inv_temp = 1.0f / temperature;
    for (uint32_t i = 0; i < n; i++)
        logits[i] *= inv_temp;

    /* Find max for numerical stability */
    float max_val = logits[0];
    for (uint32_t i = 1; i < n; i++)
        if (logits[i] > max_val) max_val = logits[i];

    /* Softmax + collect candidates above a threshold.
     * Pre-filter: skip logits more than 20 below max (exp(-20) ≈ 2e-9) */
    extern float expf_bare(float x);
    float cutoff = max_val - 20.0f;
    float sum = 0.0f;

    /* Use scratch buffer from state — logits array has vocab_size entries,
     * we need at most vocab_size candidates. Use stack for small buffer,
     * otherwise allocate. For 128K vocab, each entry is 8 bytes = 1MB.
     * Use a fixed max to avoid stack overflow. */
    #define SAMPLE_MAX_CANDIDATES 4096
    static logit_idx_t candidates[SAMPLE_MAX_CANDIDATES];
    uint32_t n_cand = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (logits[i] < cutoff) continue;
        float p = expf_bare(logits[i] - max_val);
        sum += p;
        if (n_cand < SAMPLE_MAX_CANDIDATES) {
            candidates[n_cand].val = p;
            candidates[n_cand].idx = i;
            n_cand++;
        }
    }

    if (n_cand == 0)
        return argmax(logits, n);

    /* Normalize */
    for (uint32_t i = 0; i < n_cand; i++)
        candidates[i].val /= sum;

    /* Sort by probability (descending) — insertion sort, small n_cand */
    for (uint32_t i = 1; i < n_cand; i++) {
        logit_idx_t key = candidates[i];
        int j = (int)i - 1;
        while (j >= 0 && candidates[j].val < key.val) {
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = key;
    }

    /* Top-p truncation */
    float cum = 0.0f;
    uint32_t last = n_cand;
    for (uint32_t i = 0; i < n_cand; i++) {
        cum += candidates[i].val;
        if (cum >= top_p) {
            last = i + 1;
            break;
        }
    }

    /* Re-normalize truncated distribution */
    float trunc_sum = 0.0f;
    for (uint32_t i = 0; i < last; i++)
        trunc_sum += candidates[i].val;

    /* Sample */
    float r = rng_float() * trunc_sum;
    float acc = 0.0f;
    for (uint32_t i = 0; i < last; i++) {
        acc += candidates[i].val;
        if (acc >= r)
            return candidates[i].idx;
    }

    return candidates[last - 1].idx;
}

/* Default sampling parameters */
static float g_temperature = 0.6f;
static float g_top_p = 0.9f;

/* ── Repetition-penalty parameters (Goldilocks defaults: off) ──
 *
 * Brandon-tiny-10m and other small models collapse into degenerate
 * attractors (the "United States" loop documented in osito-a/agent.c:112)
 * when sampled greedily. The classic mitigation is a sliding-window
 * penalty over the last N generated tokens.
 *
 *   - rep_penalty       (multiplicative, llama.cpp classic):
 *       logits[t] /= rep_penalty   if logits[t] > 0
 *       logits[t] *= rep_penalty   if logits[t] < 0
 *   - presence_penalty  (additive once per unique recent token, OpenAI)
 *   - frequency_penalty (additive scaled by occurrence count, OpenAI)
 *
 * Standard "balanced de-repetition" recipe: rep=1.10 + freq=0.05.
 * Values default to identity so existing chat behavior is unchanged
 * until `penalty` is run from the shell.                             */
static float    g_rep_penalty       = 1.0f;   /* >1 = penalize */
static float    g_presence_penalty  = 0.0f;
static float    g_frequency_penalty = 0.0f;

/* no_repeat_ngram_size: HuggingFace-style structural blocker. When >=2,
 * any token T that would produce an N-gram (last_N-1 + T) already seen
 * in the window has its logit forced to -inf so it can never be picked.
 * 0 = disabled, 3 = balanced (matches brandon-tiny test_generation.py). */
static uint32_t g_no_repeat_ngram = 0;

#define LLAMA_RECENT_TOKENS 256       /* window for both penalty + ngram */
static uint32_t g_recent_buf[LLAMA_RECENT_TOKENS];
static uint32_t g_recent_count = 0;
static uint32_t g_recent_head  = 0;
/* Linear-order copy of the generation history for ngram lookup.
 * recent_buf is a ring; ngram_buf is rebuilt linearly. */
static uint32_t g_ngram_buf[LLAMA_RECENT_TOKENS];
static uint32_t g_ngram_count = 0;

void llama_set_sampling(float temperature, float top_p)
{
    g_temperature = temperature;
    g_top_p = top_p;
}

void brandon_set_features(int dwa, int v_residual, int registers)
{
    g_brandon_use_dwa            = dwa != 0;
    g_brandon_use_value_residual = v_residual != 0;
    g_brandon_use_registers      = registers != 0;
}
void brandon_set_debug_logits(int on) { g_brandon_debug_logits = on != 0; }

/* Read-only state accessors for shell `info` command. */
int llama_state_dim(void *s)
{ return s ? (int)((llama_state_t *)s)->dim : 0; }
int llama_state_layers(void *s)
{ return s ? (int)((llama_state_t *)s)->n_layers : 0; }
/* Debug accessor: dump the first 8 floats of embed_token(token) result. */
/* Dump first 4 tensors' ne[] dims and type to verify GGUF layout. */
/* Dump the dequantized first row of attn_q[0]. Uses embed_token's
 * dequant path because that's verified consistent with matvec.
 *
 * These four llama_debug_* helpers rely on libc malloc/free/sqrt, which
 * only exist in the WASM (emscripten) build.  Gating with __EMSCRIPTEN__
 * so the x86 kernel still links. */
#ifdef __EMSCRIPTEN__
void llama_debug_dump_row(void *s_in)
{
    llama_state_t *s = (llama_state_t *)s_in;
    extern void serial_puts(const char *);
    extern void serial_putdec(uint64_t);
    gguf_tensor_t *t = s->weights.layers[0].attn_q;
    uint32_t dim = s->dim;
    extern void *malloc(unsigned long); extern void free(void *);
    /* Dump raw bytes of first block (d, dmin, 12 scales). */
    const uint8_t *b = (const uint8_t *)t->data;
    serial_puts("[row0 raw] d=");
    serial_putdec((uint64_t)b[0]); serial_puts(",");
    serial_putdec((uint64_t)b[1]); serial_puts(" dmin=");
    serial_putdec((uint64_t)b[2]); serial_puts(",");
    serial_putdec((uint64_t)b[3]); serial_puts(" scales=");
    for (int i = 0; i < 12; i++) {
        serial_putdec((uint64_t)b[4 + i]);
        serial_puts(",");
    }
    serial_puts(" qs[0..7]=");
    for (int i = 0; i < 8; i++) {
        serial_putdec((uint64_t)b[16 + i]);
        serial_puts(",");
    }
    serial_puts("\n");
    float *tmp = (float *)malloc((size_t)dim * sizeof(float));
    /* Treat row 0 of attn_q like an embed: dequant the first row's bytes. */
    /* attn_q is [out_dim=dim, in_dim=dim]. Row 0 = first output's weights. */
    embed_token(tmp, t, 0, dim);
    /* Print first 12 values *1k, and compute sum-of-squares for magnitude. */
    serial_puts("[row0 attn_q] first12*1k=");
    float ssq = 0.0f;
    float maxabs = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        ssq += tmp[i] * tmp[i];
        float a = tmp[i] < 0 ? -tmp[i] : tmp[i];
        if (a > maxabs) maxabs = a;
    }
    for (int i = 0; i < 12; i++) {
        serial_putdec((uint64_t)(uint32_t)(int32_t)(tmp[i] * 1000.0f));
        serial_puts(",");
    }
    serial_puts(" rms*1k=");
    extern double sqrt(double);
    float rms = (float)sqrt(ssq / dim);
    serial_putdec((uint64_t)(uint32_t)(int32_t)(rms * 1000.0f));
    serial_puts(" maxabs*1k=");
    serial_putdec((uint64_t)(uint32_t)(int32_t)(maxabs * 1000.0f));
    serial_puts("\n");
    free(tmp);
}

void llama_debug_dump_shapes(void *s_in)
{
    llama_state_t *s = (llama_state_t *)s_in;
    extern void serial_puts(const char *);
    extern void serial_putdec(uint64_t);
    struct { const char *name; gguf_tensor_t *t; } picks[6] = {
        {"token_embd", s->weights.token_embd},
        {"attn_q[0]",  s->weights.layers[0].attn_q},
        {"attn_k[0]",  s->weights.layers[0].attn_k},
        {"attn_v[0]",  s->weights.layers[0].attn_v},
        {"attn_o[0]",  s->weights.layers[0].attn_output},
        {"ffn_gate[0]", s->weights.layers[0].ffn_gate},
    };
    for (int i = 0; i < 6; i++) {
        if (!picks[i].t) continue;
        gguf_tensor_t *t = picks[i].t;
        serial_puts("[shape] "); serial_puts(picks[i].name);
        serial_puts(" type="); serial_putdec((uint64_t)t->type);
        serial_puts(" ne=");
        for (uint32_t d = 0; d < t->n_dims; d++) {
            serial_putdec(t->ne[d]);
            if (d + 1 < t->n_dims) serial_puts("x");
        }
        serial_puts("\n");
    }
}

void llama_debug_embed(void *s_in, uint32_t token, float *out8)
{
    llama_state_t *s = (llama_state_t *)s_in;
    uint32_t dim = s->dim;
    extern void *malloc(unsigned long); extern void free(void *);
    float *tmp = (float *)malloc((size_t)dim * sizeof(float));
    if (!tmp) return;
    embed_token(tmp, s->weights.token_embd, token, dim);
    for (int i = 0; i < 8; i++) out8[i] = tmp[i];
    free(tmp);
}

/* Debug accessor: do matvec(rows=1) on token_embd row[token] with one-hot
 * input — extracts the SAME row via the dispatcher, bypassing embed_token.
 * If embed_token's inline dequant has a bug but matvec is correct, the
 * two paths will disagree. */
void llama_debug_matvec_row(void *s_in, uint32_t token, float *out8)
{
    llama_state_t *s = (llama_state_t *)s_in;
    uint32_t dim = s->dim;
    extern void *malloc(unsigned long); extern void free(void *);
    /* Build one-hot input of size dim (one 1.0, rest zero — extracts col c
     * via dot product). But matvec computes out[r] = sum_c w[r][c]*in[c],
     * not extract a row. To extract a row, we need to pretend the embedding
     * row IS a single-row weight matrix and use one-hot input.
     *
     * embed_token reads row[token] which is dim consecutive elements.
     * matvec_q4_k_scalar with rows=1, cols=dim, weight=&token_embd_data[token*row_bytes]
     * + one-hot input at position k produces: out[0] = dequantized w[0][k]
     * which is the k-th element of the embedding row. So we'd need dim
     * matvec calls to recover the full row. Instead, just do dim calls and
     * grab the first 8. */
    extern void matvec_q4_k_scalar(float *, const void *, const float *,
                                    uint32_t, uint32_t);
    extern void matvec_q6_k_scalar(float *, const void *, const float *,
                                    uint32_t, uint32_t);
    gguf_tensor_t *t = s->weights.token_embd;
    size_t row_bytes;
    if (t->type == GGML_TYPE_Q4_K) row_bytes = (size_t)(dim/256) * 144;
    else if (t->type == GGML_TYPE_Q6_K) row_bytes = (size_t)(dim/256) * 210;
    else { for (int i = 0; i < 8; i++) out8[i] = 0; return; }
    const void *row = (const uint8_t *)t->data + (uint64_t)token * row_bytes;
    float *inp = (float *)malloc((size_t)dim * sizeof(float));
    for (int k = 0; k < 8; k++) {
        for (uint32_t i = 0; i < dim; i++) inp[i] = 0.0f;
        inp[k] = 1.0f;
        float v;
        if (t->type == GGML_TYPE_Q4_K) matvec_q4_k_scalar(&v, row, inp, 1, dim);
        else matvec_q6_k_scalar(&v, row, inp, 1, dim);
        out8[k] = v;
    }
    free(inp);
}
#endif /* __EMSCRIPTEN__ — close llama_debug_* block */

int llama_state_vocab(void *s)
{ return s ? (int)((llama_state_t *)s)->vocab_size : 0; }
const char *llama_state_arch(void *s)
{ return s ? ((llama_state_t *)s)->arch : ""; }

void llama_set_penalty(float rep, float presence, float frequency)
{
    g_rep_penalty       = rep > 0.0f ? rep : 1.0f;
    g_presence_penalty  = presence > 0.0f ? presence : 0.0f;
    g_frequency_penalty = frequency > 0.0f ? frequency : 0.0f;
}

void llama_get_penalty(float *rep, float *presence, float *frequency)
{
    if (rep)       *rep       = g_rep_penalty;
    if (presence)  *presence  = g_presence_penalty;
    if (frequency) *frequency = g_frequency_penalty;
}

void     llama_set_ngram_size(uint32_t n) { g_no_repeat_ngram = n; }
uint32_t llama_get_ngram_size(void)       { return g_no_repeat_ngram; }

static void recent_reset(void)
{
    g_recent_count = 0;
    g_recent_head  = 0;
    g_ngram_count  = 0;
}

static void recent_push(uint32_t tok)
{
    g_recent_buf[g_recent_head] = tok;
    g_recent_head = (g_recent_head + 1) % LLAMA_RECENT_TOKENS;
    if (g_recent_count < LLAMA_RECENT_TOKENS) g_recent_count++;

    /* Linear ngram buffer — drop the oldest token if full so the
     * window slides over the most recent N tokens. */
    if (g_ngram_count == LLAMA_RECENT_TOKENS) {
        for (uint32_t i = 0; i < LLAMA_RECENT_TOKENS - 1; i++)
            g_ngram_buf[i] = g_ngram_buf[i + 1];
        g_ngram_buf[LLAMA_RECENT_TOKENS - 1] = tok;
    } else {
        g_ngram_buf[g_ngram_count++] = tok;
    }
}

/* no_repeat_ngram: scan the linear history for any (n-1)-gram that
 * matches the trailing (n-1) tokens of g_ngram_buf. For each match,
 * the token at position [match + n-1] is forbidden — set its logit
 * to -INF so sampling never chooses it. */
static void apply_no_repeat_ngram(float *logits, uint32_t vocab)
{
    uint32_t n = g_no_repeat_ngram;
    if (n < 2 || g_ngram_count < n) return;

    uint32_t prefix = n - 1;
    const uint32_t *tail = &g_ngram_buf[g_ngram_count - prefix];
    /* Slide a window of length n over the history; when the first
     * (n-1) tokens of the window match the tail, the n-th token is
     * banned. */
    for (uint32_t i = 0; i + n <= g_ngram_count; i++) {
        bool match = true;
        for (uint32_t j = 0; j < prefix; j++) {
            if (g_ngram_buf[i + j] != tail[j]) { match = false; break; }
        }
        if (match) {
            uint32_t banned = g_ngram_buf[i + prefix];
            if (banned < vocab) logits[banned] = -1e30f;
        }
    }
}

/* Apply rep + presence + frequency penalties to logits in place over
 * the recent-token window. Skips entirely when no penalty is active. */
static void apply_penalties(float *logits, uint32_t n)
{
    if (g_recent_count == 0) return;
    if (g_rep_penalty <= 1.0f &&
        g_presence_penalty <= 0.0f &&
        g_frequency_penalty <= 0.0f) return;

    /* rep + presence: once per unique token in the window. */
    if (g_rep_penalty > 1.0f || g_presence_penalty > 0.0f) {
        for (uint32_t i = 0; i < g_recent_count; i++) {
            uint32_t t = g_recent_buf[i];
            if (t >= n) continue;
            bool seen = false;
            for (uint32_t j = 0; j < i; j++) {
                if (g_recent_buf[j] == t) { seen = true; break; }
            }
            if (seen) continue;
            float v = logits[t];
            if (g_rep_penalty > 1.0f) {
                if (v > 0.0f) v /= g_rep_penalty;
                else if (v < 0.0f) v *= g_rep_penalty;
            }
            if (g_presence_penalty > 0.0f) v -= g_presence_penalty;
            logits[t] = v;
        }
    }

    /* frequency: each occurrence subtracts. */
    if (g_frequency_penalty > 0.0f) {
        for (uint32_t i = 0; i < g_recent_count; i++) {
            uint32_t t = g_recent_buf[i];
            if (t < n) logits[t] -= g_frequency_penalty;
        }
    }
}

static uint32_t sample_next(float *logits, uint32_t vocab_size)
{
    apply_penalties(logits, vocab_size);
    apply_no_repeat_ngram(logits, vocab_size);
    return sample_topp(logits, vocab_size, g_temperature, g_top_p);
}

/* ══════════════════════════════════════════════════════════════
 *  llama_generate — Prefill + decode loop
 * ══════════════════════════════════════════════════════════════ */

#define LLAMA_EOS_1  128001  /* <|end_of_text|> */
#define LLAMA_EOS_2  128009  /* <|eot_id|> */

void llama_generate(llama_state_t *state, const uint32_t *prompt,
                    uint32_t prompt_len, uint32_t max_tokens)
{
    serial_puts("\n[LLAMA] === Inference ===\n");
    serial_puts("[LLAMA] Prompt: ");
    serial_putdec(prompt_len);
    serial_puts(" tokens, max generate: ");
    serial_putdec(max_tokens);
    serial_puts("\n");

    fb_puts("\n Inference: ");
    fb_putdec(prompt_len);
    fb_puts(" prompt, ");
    fb_putdec(max_tokens);
    fb_puts(" max gen\n");

    state->pos = 0;
    rng_seed();
    uint64_t total_t0 = rdtsc();
    uint32_t total_tokens = 0;

    serial_puts("[LLAMA] Sampling: temp=");
    /* Print temperature as fixed point (avoid printf %f) */
    uint32_t temp_int = (uint32_t)(g_temperature * 10.0f);
    serial_putdec(temp_int / 10); serial_puts(".");
    serial_putdec(temp_int % 10);
    serial_puts(", top_p=");
    uint32_t topp_int = (uint32_t)(g_top_p * 10.0f);
    serial_putdec(topp_int / 10); serial_puts(".");
    serial_putdec(topp_int % 10);
    serial_puts("\n");

    /* ── Prefill: process prompt tokens ── */
    serial_puts("[LLAMA] Prefill:\n");
    for (uint32_t i = 0; i < prompt_len; i++) {
        uint64_t t0 = rdtsc();
        llama_forward(state, prompt[i]);
        uint64_t t1 = rdtsc();
        uint64_t ms = (t1 - t0) / 3000000;

        serial_puts("  tok ");
        serial_putdec(prompt[i]);
        serial_puts(" (");
        serial_putdec(ms);
        serial_puts(" ms)\n");
        total_tokens++;
    }

    /* ── Decode: generate new tokens ── */
    serial_puts("[LLAMA] Generating:\n");
    fb_puts(" Generating...\n");

    uint32_t next = sample_next(state->logits, state->vocab_size);

    for (uint32_t step = 0; step < max_tokens; step++) {
        /* Check EOS */
        if (next == LLAMA_EOS_1 || next == LLAMA_EOS_2) {
            serial_puts("  [");
            serial_putdec(step + 1);
            serial_puts("] token ");
            serial_putdec(next);
            serial_puts(" (EOS)\n");
            total_tokens++;
            break;
        }

        /* Check max sequence length */
        if (state->pos >= state->max_seq) {
            serial_puts("  [max_seq reached]\n");
            break;
        }

        uint64_t t0 = rdtsc();

        /* Speculative execution: if spec predicted correctly from the
         * previous iteration, adopt the speculated layers and skip them.
         * Otherwise, run the full forward pass. */
        if (g_spec && g_spec->spec_complete == 1 &&
            next == g_spec->predicted_token) {
            /* HIT: commit shadow KV to main cache */
            uint32_t kv_dim = state->kv_dim;
            uint32_t spos = g_spec->spec_pos;
            for (uint32_t sl = 0; sl < g_spec->spec_layers_done; sl++) {
                memcpy(state->kv_cache[sl].k + (uint64_t)spos * kv_dim,
                       g_spec->shadow_k[sl], kv_dim * sizeof(float));
                memcpy(state->kv_cache[sl].v + (uint64_t)spos * kv_dim,
                       g_spec->shadow_v[sl], kv_dim * sizeof(float));
            }
            /* Continue from where speculation left off */
            memcpy(state->x, g_spec->spec_x, state->dim * sizeof(float));
            llama_forward_from_layer(state, next, g_spec->spec_layers_done);
            g_spec->hits++;
        } else {
            if (g_spec && g_spec->spec_complete == 1)
                g_spec->misses++;
            llama_forward(state, next);
        }

        uint64_t t1 = rdtsc();
        uint64_t ms = (t1 - t0) / 3000000;

        serial_puts("  [");
        serial_putdec(step + 1);
        serial_puts("] ");

        /* Decode token to text if tokenizer available */
        const char *text = tok_global_decode(next);
        if (text) {
            serial_puts("\"");
            serial_puts(text);
            serial_puts("\"");
        } else {
            serial_puts("token ");
            serial_putdec(next);
        }
        serial_puts(" (");
        serial_putdec(ms);
        serial_puts(" ms");
        if (g_spec && g_spec->spec_complete == 1 && next == g_spec->predicted_token)
            serial_puts(" SPEC-HIT");
        serial_puts(")\n");
        total_tokens++;

        /* Print decoded text to framebuffer */
        if (text) fb_puts(text);

        /* Start speculation for NEXT token: predict via argmax,
         * then compute first SPEC_MAX_LAYERS on shadow buffers.
         * This overlaps with the sampling below on a real SMP system. */
        if (g_spec && ap_worker_count >= 1 &&
            next != LLAMA_EOS_1 && next != LLAMA_EOS_2) {
            uint32_t predicted = argmax(state->logits, state->vocab_size);
            g_spec->predicted_token = predicted;
            g_spec->spec_pos = state->pos;  /* pos was already incremented */
            g_spec->spec_complete = 0;

            /* Run speculative layers 0..SPEC_MAX_LAYERS-1 with shadow KV.
             * In a full implementation this would be submitted to an AP
             * via smp_submit_any. For now, run inline (still benefits
             * from the forward_from_layer skip on next iteration). */
            embed_token(g_spec->spec_x, state->weights.token_embd,
                        predicted, state->dim);
            uint32_t kv_dim = state->kv_dim;
            uint32_t hd = state->head_dim;
            uint32_t spos = g_spec->spec_pos;
            uint32_t layers_done = 0;

            for (uint32_t l = 0; l < SPEC_MAX_LAYERS && l < state->n_layers; l++) {
                llama_layer_t *ly = &state->weights.layers[l];
                float *sx = g_spec->spec_x;
                float *sxb = g_spec->spec_xb;

                rmsnorm(sxb, sx, norm_data(ly->attn_norm), state->dim);
                matvec(g_spec->spec_q, ly->attn_q, sxb, state->dim, state->dim);
                matvec(g_spec->spec_k, ly->attn_k, sxb, kv_dim, state->dim);
                matvec(g_spec->spec_v, ly->attn_v, sxb, kv_dim, state->dim);

                rope(g_spec->spec_q, state->n_heads, hd, spos, state->rope_freq_base);
                rope(g_spec->spec_k, state->n_kv_heads, hd, spos, state->rope_freq_base);

                /* Write to SHADOW KV, not main cache */
                memcpy(g_spec->shadow_k[l], g_spec->spec_k, kv_dim * sizeof(float));
                memcpy(g_spec->shadow_v[l], g_spec->spec_v, kv_dim * sizeof(float));

                /* Attention: use main KV cache for positions 0..spos-1,
                 * shadow for position spos */
                for (uint32_t h = 0; h < state->n_heads; h++) {
                    uint32_t kv_h = h / state->gqa_ratio;
                    float *qh = g_spec->spec_q + h * hd;
                    float *xb2h = g_spec->spec_xb2 + h * hd;
                    float max_s = -1e30f;
                    /* Score against main KV (0..spos-1) */
                    for (uint32_t t = 0; t < spos; t++) {
                        float *kt = state->kv_cache[l].k + t * kv_dim + kv_h * hd;
                        float sc = 0;
                        for (uint32_t d = 0; d < hd; d++) sc += qh[d] * kt[d];
                        sc /= sqrtf_bare((float)hd);
                        g_spec->spec_att[t] = sc;
                        if (sc > max_s) max_s = sc;
                    }
                    /* Score against shadow KV (position spos) */
                    {
                        float *kt = g_spec->shadow_k[l] + kv_h * hd;
                        float sc = 0;
                        for (uint32_t d = 0; d < hd; d++) sc += qh[d] * kt[d];
                        sc /= sqrtf_bare((float)hd);
                        g_spec->spec_att[spos] = sc;
                        if (sc > max_s) max_s = sc;
                    }
                    float sum = 0;
                    for (uint32_t t = 0; t <= spos; t++) {
                        g_spec->spec_att[t] = expf_bare(g_spec->spec_att[t] - max_s);
                        sum += g_spec->spec_att[t];
                    }
                    float inv = 1.0f / sum;
                    for (uint32_t t = 0; t <= spos; t++) g_spec->spec_att[t] *= inv;
                    for (uint32_t d = 0; d < hd; d++) xb2h[d] = 0;
                    for (uint32_t t = 0; t < spos; t++) {
                        float *vt = state->kv_cache[l].v + t * kv_dim + kv_h * hd;
                        float w = g_spec->spec_att[t];
                        for (uint32_t d = 0; d < hd; d++) xb2h[d] += w * vt[d];
                    }
                    /* Shadow V for position spos */
                    {
                        float *vt = g_spec->shadow_v[l] + kv_h * hd;
                        float w = g_spec->spec_att[spos];
                        for (uint32_t d = 0; d < hd; d++) xb2h[d] += w * vt[d];
                    }
                }
                matvec(sxb, ly->attn_output, g_spec->spec_xb2, state->dim, state->dim);
                for (uint32_t i = 0; i < state->dim; i++) sx[i] += sxb[i];

                /* FFN */
                rmsnorm(sxb, sx, norm_data(ly->ffn_norm), state->dim);
                matvec(g_spec->spec_hb, ly->ffn_gate, sxb, state->ffn_dim, state->dim);
                matvec(g_spec->spec_hb2, ly->ffn_up, sxb, state->ffn_dim, state->dim);
                for (uint32_t i = 0; i < state->ffn_dim; i++)
                    g_spec->spec_hb[i] = (g_spec->spec_hb[i] /
                        (1.0f + expf_bare(-g_spec->spec_hb[i]))) * g_spec->spec_hb2[i];
                matvec(sxb, ly->ffn_down, g_spec->spec_hb, state->dim, state->ffn_dim);
                for (uint32_t i = 0; i < state->dim; i++) sx[i] += sxb[i];

                layers_done++;
            }
            g_spec->spec_layers_done = layers_done;
            g_spec->spec_complete = 1;
        }

        next = sample_next(state->logits, state->vocab_size);
    }

    /* ── Summary ── */
    uint64_t total_t1 = rdtsc();
    uint64_t total_ms = (total_t1 - total_t0) / 3000000;
    uint64_t ms_per_tok = total_tokens > 0 ? total_ms / total_tokens : 0;

    serial_puts("\n[LLAMA] Done: ");
    serial_putdec(total_tokens);
    serial_puts(" tokens in ");
    serial_putdec(total_ms);
    serial_puts(" ms (");
    serial_putdec(ms_per_tok);
    serial_puts(" ms/tok)");
    if (g_spec && (g_spec->hits + g_spec->misses) > 0) {
        serial_puts(" spec=");
        serial_putdec(g_spec->hits);
        serial_puts("/");
        serial_putdec(g_spec->hits + g_spec->misses);
    }
    serial_puts("\n");

    fb_puts(" Done: ");
    fb_putdec(total_tokens);
    fb_puts(" tok, ");
    fb_putdec(ms_per_tok);
    fb_puts(" ms/tok\n");
}

/* ══════════════════════════════════════════════════════════════
 *  llama_chat — Text-in, text-out inference via tokenizer
 *
 *  Tokenizes input text, prepends BOS + Llama 3 chat template,
 *  runs prefill + generation, streams decoded output via callback.
 * ══════════════════════════════════════════════════════════════ */

/* Tokenizer functions (tokenizer.c) */
extern int      tok_encode(const void *tok, const char *text, uint32_t text_len,
                            uint32_t *out, uint32_t max_out);
extern bool     tok_is_ready(const void *tok);
extern uint32_t tok_get_bos_id(const void *tok);
extern uint32_t tok_get_eos_id(const void *tok);
extern uint32_t tok_find_special(const void *tok, const char *s);
extern char     g_tokenizer[];

int llama_chat(llama_state_t *state, const char *text,
               uint32_t max_tokens,
               void (*on_token)(const char *text, void *ctx), void *ctx)
{
    if (!state || !text) return -1;

    uint32_t text_len = 0;
    const char *p = text;
    while (*p++) text_len++;

    uint32_t bos_id = tok_get_bos_id(g_tokenizer);
    uint32_t eos_id = tok_get_eos_id(g_tokenizer);

    if (!tok_is_ready(g_tokenizer)) {
        serial_puts("[CHAT] Tokenizer not ready, BOS-only fallback\n");
        state->pos = 0;
        uint32_t bos[] = { bos_id };
        llama_generate(state, bos, 1, max_tokens);
        return 0;
    }

    /*
     * Auto-detect chat template by searching for special tokens in vocab.
     *
     * ChatML (SmolLM2, Qwen, Mistral-v3 ...):
     *   <|im_start|>user\n{text}<|im_end|>\n<|im_start|>assistant\n
     *
     * Llama 3 (token IDs are always < vocab_size for real Llama 3 models):
     *   <|begin_of_text|>...<|start_header_id|>user<|end_header_id|>...
     *
     * Fallback (base models / unknown): BOS + raw text
     */
    uint32_t im_start = tok_find_special(g_tokenizer, "<|im_start|>");
    uint32_t im_end   = tok_find_special(g_tokenizer, "<|im_end|>");

    uint32_t tokens[1024];
    uint32_t n = 0;
    int r;

    if (im_start != UINT32_MAX && im_end != UINT32_MAX) {
        /* ── ChatML template ── */
        tokens[n++] = im_start;
        r = tok_encode(g_tokenizer, "user\n", 5, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        r = tok_encode(g_tokenizer, text, text_len, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        tokens[n++] = im_end;
        r = tok_encode(g_tokenizer, "\n", 1, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        tokens[n++] = im_start;
        r = tok_encode(g_tokenizer, "assistant\n", 10, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        /* generation stops when model produces im_end */
        eos_id = im_end;

    } else if (bos_id >= 128000) {
        /* ── Llama 3 template (BOS is a special high-ID token) ── */
        uint32_t hdr_open  = bos_id + 6;   /* <|start_header_id|> */
        uint32_t hdr_close = bos_id + 7;   /* <|end_header_id|> */
        uint32_t eot       = bos_id + 9;   /* <|eot_id|> */
        tokens[n++] = bos_id;
        tokens[n++] = hdr_open;
        r = tok_encode(g_tokenizer, "user", 4, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        tokens[n++] = hdr_close;
        r = tok_encode(g_tokenizer, "\n\n", 2, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        r = tok_encode(g_tokenizer, text, text_len, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        tokens[n++] = eot;
        tokens[n++] = hdr_open;
        r = tok_encode(g_tokenizer, "assistant", 9, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
        tokens[n++] = hdr_close;
        r = tok_encode(g_tokenizer, "\n\n", 2, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;

    } else {
        /* ── Raw: BOS + text (base models) ── */
        tokens[n++] = bos_id;
        r = tok_encode(g_tokenizer, text, text_len, tokens + n, 1024 - n);
        if (r > 0) n += (uint32_t)r;
    }

    serial_puts("[CHAT] Prompt: ");
    serial_putdec(n);
    serial_puts(" tokens\n");

    /* Reset state */
    state->pos = 0;
    /* Brandon: re-run register prefill at the start of each chat call
     * so positions 0..n_registers-1 are freshly written before the user
     * tokens. Without this, cache slots 0..3 hold whatever was there
     * from the previous chat. */
    state->registers_prefilled = false;
    state->v_first_captured    = false;
    /* Empty the repetition-penalty window so we don't carry over the
     * previous chat's tokens into this one. */
    recent_reset();

    /* Prefill */
    uint64_t t0 = rdtsc();
    for (uint32_t i = 0; i < n; i++)
        llama_forward(state, tokens[i]);
    uint64_t t1 = rdtsc();

    serial_puts("[CHAT] Prefill: ");
#ifdef __EMSCRIPTEN__
    serial_putdec(t1 - t0);  /* emscripten_get_now() is already ms */
#else
    serial_putdec((t1 - t0) / 3000000);
#endif
    serial_puts(" ms\n");

    /* Generate */
    rng_seed();
    uint32_t next = sample_next(state->logits, state->vocab_size);
    uint32_t gen = 0;

    for (uint32_t step = 0; step < max_tokens; step++) {
        if (next == eos_id || next == LLAMA_EOS_1 || next == LLAMA_EOS_2) break;
        if (state->pos >= state->max_seq) break;

        recent_push(next);

        /* Decode and deliver */
        const char *tok_text = tok_global_decode(next);
        if (tok_text && on_token) {
            on_token(tok_text, ctx);
        }

#ifdef __EMSCRIPTEN__
        /* Yield to the JS event loop after each token so the browser
         * UI updates and pumps postMessages (oi_chat bridge etc.).
         * Costs ~1ms; negligible vs the seconds spent in matmul. */
        extern void emscripten_sleep(unsigned int ms);
        emscripten_sleep(0);
#endif

        llama_forward(state, next);
        gen++;
        next = sample_next(state->logits, state->vocab_size);
    }

    uint64_t t2 = rdtsc();
#ifdef __EMSCRIPTEN__
    uint64_t gen_ms = gen > 0 ? (t2 - t1) : 0;
#else
    uint64_t gen_ms = gen > 0 ? (t2 - t1) / 3000000 : 0;
#endif
    uint64_t ms_per_tok = gen > 0 ? gen_ms / gen : 0;

    serial_puts("[CHAT] Generated: ");
    serial_putdec(gen);
    serial_puts(" tokens (");
    serial_putdec(ms_per_tok);
    serial_puts(" ms/tok)\n");

    return (int)gen;
}

/* ══════════════════════════════════════════════════════════════
 *  llama_embed_text — last-token hidden-state embedder
 *
 *  Tokenizes `text` (no chat template — raw + BOS), runs forward
 *  over each token, then copies the post-final-norm hidden state
 *  at the last position to `out`. Output is L2-normalized and
 *  truncated to min(model_dim, max_dim). Returns the effective
 *  embedding dimension written, or -1 on failure.
 *
 *  Use case: query similarity for RAG cache, semantic dedup of
 *  shell history, ad-hoc cosine-sim debugging. Note the resulting
 *  vector lives in the loaded model's hidden space (e.g. llama
 *  1B = 2048d), which is NOT the same space as the bge-large
 *  corpus (1024d) — use this for self-similarity, not for
 *  cross-system retrieval against externally-embedded data.
 * ══════════════════════════════════════════════════════════════ */
int llama_embed_text(llama_state_t *state, const char *text,
                     float *out, int max_dim)
{
    if (!state || !text || !out || max_dim < 1) return -1;
    if (!tok_is_ready(g_tokenizer)) return -1;

    uint32_t text_len = 0;
    for (const char *p = text; *p; p++) text_len++;
    if (text_len == 0) return -1;

    uint32_t bos_id = tok_get_bos_id(g_tokenizer);
    uint32_t tokens[1024];
    uint32_t n = 0;
    tokens[n++] = bos_id;
    int r = tok_encode(g_tokenizer, text, text_len,
                       tokens + n, 1024 - n);
    if (r > 0) n += (uint32_t)r;
    if (n < 2) return -1;

    state->pos = 0;
    state->registers_prefilled = false;
    state->v_first_captured    = false;
    recent_reset();

    for (uint32_t i = 0; i < n; i++)
        llama_forward(state, tokens[i]);

    /* After the last forward, s->x holds the post-final-norm
     * hidden state for the last token (final rmsnorm is applied
     * in-place before the LM head). Snapshot + L2-normalize. */
    uint32_t dim = state->dim;
    int eff = (int)dim < max_dim ? (int)dim : max_dim;
    double sumsq = 0.0;
    for (int i = 0; i < eff; i++) {
        float v = state->x[i];
        if (v != v) v = 0.0f;
        out[i] = v;
        sumsq += (double)v * (double)v;
    }
    if (sumsq > 1e-12) {
        float inv = 1.0f / sqrtf_bare((float)sumsq);
        for (int i = 0; i < eff; i++) out[i] *= inv;
    }
    return eff;
}

/* ══════════════════════════════════════════════════════════════
 *  llama_chat_with_system — RAG-style ChatML chat
 *
 *  Prepends a system turn before the user turn:
 *    <|im_start|>system\n{system}<|im_end|>\n
 *    <|im_start|>user\n{user}<|im_end|>\n
 *    <|im_start|>assistant\n
 *
 *  Falls back to llama_chat (no system turn) when system_text is empty
 *  or when the tokenizer doesn't expose ChatML special tokens.
 * ══════════════════════════════════════════════════════════════ */

int llama_chat_with_system(llama_state_t *state,
                            const char *system_text,
                            const char *user_text,
                            uint32_t max_tokens,
                            void (*on_token)(const char *text, void *ctx),
                            void *ctx)
{
    if (!state || !user_text) return -1;
    if (!system_text || !*system_text)
        return llama_chat(state, user_text, max_tokens, on_token, ctx);

    if (!tok_is_ready(g_tokenizer))
        return llama_chat(state, user_text, max_tokens, on_token, ctx);

    uint32_t im_start = tok_find_special(g_tokenizer, "<|im_start|>");
    uint32_t im_end   = tok_find_special(g_tokenizer, "<|im_end|>");
    if (im_start == UINT32_MAX || im_end == UINT32_MAX)
        return llama_chat(state, user_text, max_tokens, on_token, ctx);

    uint32_t sys_len = 0;  while (system_text[sys_len]) sys_len++;
    uint32_t usr_len = 0;  while (user_text  [usr_len]) usr_len++;

    /* Larger token buffer for RAG: 4096 tokens fits brandon's 256 max_seq
     * after register prefill (registers + prompt < max_seq is checked at
     * forward time; oversize prompts get truncated by the cache cap). */
    static uint32_t tokens[4096];
    uint32_t n = 0;
    int r;

    /* system turn */
    tokens[n++] = im_start;
    r = tok_encode(g_tokenizer, "system\n", 7, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;
    r = tok_encode(g_tokenizer, system_text, sys_len, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;
    tokens[n++] = im_end;
    r = tok_encode(g_tokenizer, "\n", 1, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;

    /* user turn */
    tokens[n++] = im_start;
    r = tok_encode(g_tokenizer, "user\n", 5, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;
    r = tok_encode(g_tokenizer, user_text, usr_len, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;
    tokens[n++] = im_end;
    r = tok_encode(g_tokenizer, "\n", 1, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;

    /* assistant turn opener */
    tokens[n++] = im_start;
    r = tok_encode(g_tokenizer, "assistant\n", 10, tokens + n, 4096 - n);
    if (r > 0) n += (uint32_t)r;

    serial_puts("[RAG] Prompt: ");
    serial_putdec(n);
    serial_puts(" tokens\n");

    /* Reset state (brandon also re-prefills registers) */
    state->pos = 0;
    state->registers_prefilled = false;
    state->v_first_captured    = false;
    recent_reset();

    /* Cap prefill at max_seq - max_tokens so generation has headroom */
    uint32_t avail = state->max_seq > max_tokens ? state->max_seq - max_tokens : 1;
    if (n > avail) {
        serial_puts("[RAG] truncating ");
        serial_putdec(n - avail);
        serial_puts(" tail tokens\n");
        n = avail;
    }

    /* Prefill */
    uint64_t t0 = rdtsc();
    for (uint32_t i = 0; i < n; i++)
        llama_forward(state, tokens[i]);
    uint64_t t1 = rdtsc();
    serial_puts("[RAG] Prefill: ");
#ifdef __EMSCRIPTEN__
    serial_putdec(t1 - t0);
#else
    serial_putdec((t1 - t0) / 3000000);
#endif
    serial_puts(" ms\n");

    /* Generate (stops on im_end) */
    rng_seed();
    uint32_t next = sample_next(state->logits, state->vocab_size);
    uint32_t gen = 0;
    for (uint32_t step = 0; step < max_tokens; step++) {
        if (next == im_end) break;
        if (state->pos >= state->max_seq) break;

        recent_push(next);

        const char *tok_text = tok_global_decode(next);
        if (tok_text && on_token) on_token(tok_text, ctx);

#ifdef __EMSCRIPTEN__
        extern void emscripten_sleep(unsigned int ms);
        emscripten_sleep(0);
#endif
        llama_forward(state, next);
        gen++;
        next = sample_next(state->logits, state->vocab_size);
    }

    uint64_t t2 = rdtsc();
#ifdef __EMSCRIPTEN__
    uint64_t gen_ms = gen > 0 ? (t2 - t1) : 0;
#else
    uint64_t gen_ms = gen > 0 ? (t2 - t1) / 3000000 : 0;
#endif
    serial_puts("[RAG] Generated: ");
    serial_putdec(gen);
    serial_puts(" tokens (");
    serial_putdec(gen > 0 ? gen_ms / gen : 0);
    serial_puts(" ms/tok)\n");
    return (int)gen;
}

/* ══════════════════════════════════════════════════════════════
 *  llama_free — Release all allocations
 * ══════════════════════════════════════════════════════════════ */

void llama_free(llama_state_t *state)
{
    if (!state) return;

    /* Free KV cache */
    if (state->kv_cache) {
        uint64_t kv_layer_bytes = (uint64_t)state->max_seq * state->kv_dim * sizeof(float);
        uint64_t kv_layer_pages = pages_for(kv_layer_bytes);
        for (uint32_t l = 0; l < state->n_layers; l++) {
            if (state->kv_cache[l].k)
                mem_free_pages(state->kv_cache[l].k, kv_layer_pages);
            if (state->kv_cache[l].v)
                mem_free_pages(state->kv_cache[l].v, kv_layer_pages);
        }
        uint64_t kv_table_pages = pages_for(state->n_layers * sizeof(llama_kv_layer_t));
        mem_free_pages(state->kv_cache, kv_table_pages);
    }

    /* Free scratch */
    if (state->scratch) {
        uint64_t scratch_size =
            (uint64_t)state->dim * 4 * sizeof(float) +
            (uint64_t)state->kv_dim * 2 * sizeof(float) +
            (uint64_t)state->max_seq * sizeof(float) +
            (uint64_t)state->ffn_dim * 2 * sizeof(float) +
            (uint64_t)state->vocab_size * sizeof(float);
        mem_free_pages(state->scratch, pages_for(scratch_size));
    }

    /* Free layer table */
    if (state->weights.layers) {
        uint64_t layer_table_pages = pages_for(state->n_layers * sizeof(llama_layer_t));
        mem_free_pages(state->weights.layers, layer_table_pages);
    }

    serial_puts("[LLAMA] Freed inference state\n");
}
