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

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putc(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Tokenizer decode (tokenizer.c) — returns NULL if tokenizer not initialized */
extern const char *tok_global_decode(uint32_t id);

/* ── Utility helpers ─────────────────────────────────────────── */

#define PAGE_SZ 4096

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
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
    default:
        memset(dst, 0, dim * sizeof(float));
        break;
    }
}

/* ── Matvec dispatch by tensor type ──────────────────────────── */

static void matvec(float *out, gguf_tensor_t *tensor,
                   const float *input, uint32_t rows, uint32_t cols)
{
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

    /* Copy architecture from model metadata */
    state->dim        = model->hidden_size;
    state->n_layers   = model->num_layers;
    state->n_heads    = model->head_count;
    state->n_kv_heads = model->kv_head_count;
    state->vocab_size = model->vocab_size;
    state->max_seq    = max_seq;

    /* Derived dimensions */
    state->head_dim   = state->dim / state->n_heads;
    state->kv_dim     = state->n_kv_heads * state->head_dim;
    state->gqa_ratio  = state->n_heads / state->n_kv_heads;

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
    state->weights.layers = (llama_layer_t *)mem_alloc_pages(layer_table_pages);
    if (!state->weights.layers) {
        serial_puts("[LLAMA] ERROR: failed to alloc layer table\n");
        return -1;
    }
    memset(state->weights.layers, 0, (size_t)layer_table_size);

    /* ── Resolve per-layer tensors ── */
    uint32_t resolved = 0;
    for (uint32_t l = 0; l < state->n_layers; l++) {
        llama_layer_t *ly = &state->weights.layers[l];

        ly->attn_norm    = gguf_find_tensor(model, build_layer_name(nbuf, l, "attn_norm.weight"));
        ly->attn_q       = gguf_find_tensor(model, build_layer_name(nbuf, l, "attn_q.weight"));
        ly->attn_k       = gguf_find_tensor(model, build_layer_name(nbuf, l, "attn_k.weight"));
        ly->attn_v       = gguf_find_tensor(model, build_layer_name(nbuf, l, "attn_v.weight"));
        ly->attn_output  = gguf_find_tensor(model, build_layer_name(nbuf, l, "attn_output.weight"));
        ly->ffn_norm     = gguf_find_tensor(model, build_layer_name(nbuf, l, "ffn_norm.weight"));
        ly->ffn_gate     = gguf_find_tensor(model, build_layer_name(nbuf, l, "ffn_gate.weight"));
        ly->ffn_up       = gguf_find_tensor(model, build_layer_name(nbuf, l, "ffn_up.weight"));
        ly->ffn_down     = gguf_find_tensor(model, build_layer_name(nbuf, l, "ffn_down.weight"));

        if (ly->attn_norm && ly->attn_q && ly->attn_k && ly->attn_v &&
            ly->attn_output && ly->ffn_norm && ly->ffn_gate &&
            ly->ffn_up && ly->ffn_down) {
            resolved++;
        } else {
            serial_puts("[LLAMA] WARNING: layer ");
            serial_putdec(l);
            serial_puts(" incomplete (");
            if (!ly->attn_norm)   serial_puts("attn_norm ");
            if (!ly->attn_q)      serial_puts("attn_q ");
            if (!ly->attn_k)      serial_puts("attn_k ");
            if (!ly->attn_v)      serial_puts("attn_v ");
            if (!ly->attn_output) serial_puts("attn_out ");
            if (!ly->ffn_norm)    serial_puts("ffn_norm ");
            if (!ly->ffn_gate)    serial_puts("ffn_gate ");
            if (!ly->ffn_up)      serial_puts("ffn_up ");
            if (!ly->ffn_down)    serial_puts("ffn_down ");
            serial_puts(")\n");
        }
    }

    if (resolved < state->n_layers) {
        serial_puts("[LLAMA] ERROR: only ");
        serial_putdec(resolved);
        serial_puts("/");
        serial_putdec(state->n_layers);
        serial_puts(" layers resolved\n");
        mem_free_pages(state->weights.layers, layer_table_pages);
        state->weights.layers = NULL;
        return -1;
    }

    serial_puts("[LLAMA] All ");
    serial_putdec(state->n_layers);
    serial_puts(" layers resolved\n");

    /* ── Allocate KV cache ── */
    uint64_t kv_table_size = state->n_layers * sizeof(llama_kv_layer_t);
    uint64_t kv_table_pages = pages_for(kv_table_size);
    state->kv_cache = (llama_kv_layer_t *)mem_alloc_pages(kv_table_pages);
    if (!state->kv_cache) {
        serial_puts("[LLAMA] ERROR: failed to alloc KV table\n");
        return -1;
    }
    memset(state->kv_cache, 0, (size_t)kv_table_size);

    uint64_t kv_layer_bytes = (uint64_t)max_seq * state->kv_dim * sizeof(float);
    uint64_t kv_layer_pages = pages_for(kv_layer_bytes);
    uint64_t kv_total_pages = 0;

    for (uint32_t l = 0; l < state->n_layers; l++) {
        state->kv_cache[l].k = (float *)mem_alloc_pages(kv_layer_pages);
        state->kv_cache[l].v = (float *)mem_alloc_pages(kv_layer_pages);
        if (!state->kv_cache[l].k || !state->kv_cache[l].v) {
            serial_puts("[LLAMA] ERROR: failed to alloc KV cache layer ");
            serial_putdec(l);
            serial_puts("\n");
            return -1;
        }
        memset(state->kv_cache[l].k, 0, (size_t)kv_layer_bytes);
        memset(state->kv_cache[l].v, 0, (size_t)kv_layer_bytes);
        kv_total_pages += kv_layer_pages * 2;
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

    /* ── Allocate scratch buffers (single allocation) ── */
    uint64_t scratch_size =
        (uint64_t)state->dim * sizeof(float) +          /* x */
        (uint64_t)state->dim * sizeof(float) +          /* xb */
        (uint64_t)state->dim * sizeof(float) +          /* xb2 */
        (uint64_t)state->dim * sizeof(float) +          /* q */
        (uint64_t)state->kv_dim * sizeof(float) +       /* k */
        (uint64_t)state->kv_dim * sizeof(float) +       /* v */
        (uint64_t)max_seq * sizeof(float) +              /* att */
        (uint64_t)state->ffn_dim * sizeof(float) +      /* hb */
        (uint64_t)state->ffn_dim * sizeof(float) +      /* hb2 */
        (uint64_t)state->vocab_size * sizeof(float);     /* logits */

    uint64_t scratch_pages = pages_for(scratch_size);
    state->scratch = (float *)mem_alloc_pages(scratch_pages);
    if (!state->scratch) {
        serial_puts("[LLAMA] ERROR: failed to alloc scratch (");
        serial_putdec(scratch_size / 1024);
        serial_puts(" KB)\n");
        return -1;
    }
    memset(state->scratch, 0, (size_t)(scratch_pages * PAGE_SZ));

    /* Partition scratch */
    float *ptr = state->scratch;
    state->x      = ptr; ptr += state->dim;
    state->xb     = ptr; ptr += state->dim;
    state->xb2    = ptr; ptr += state->dim;
    state->q      = ptr; ptr += state->dim;
    state->k      = ptr; ptr += state->kv_dim;
    state->v      = ptr; ptr += state->kv_dim;
    state->att    = ptr; ptr += max_seq;
    state->hb     = ptr; ptr += state->ffn_dim;
    state->hb2    = ptr; ptr += state->ffn_dim;
    state->logits = ptr;

    serial_puts("[LLAMA] Scratch: ");
    serial_putdec(scratch_size / 1024);
    serial_puts(" KB\n");
    serial_puts("[LLAMA] Init complete. Ready for inference.\n");

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  llama_forward — Single token forward pass
 * ══════════════════════════════════════════════════════════════ */

int llama_forward(llama_state_t *s, uint32_t token)
{
    uint32_t dim     = s->dim;
    uint32_t kv_dim  = s->kv_dim;
    uint32_t hd      = s->head_dim;
    uint32_t pos     = s->pos;

    /* ── Embed token ── */
    embed_token(s->x, s->weights.token_embd, token, dim);

    /* ── Transformer layers ── */
    for (uint32_t l = 0; l < s->n_layers; l++) {
        llama_layer_t *ly = &s->weights.layers[l];

        /* Attention norm */
        rmsnorm(s->xb, s->x, norm_data(ly->attn_norm), dim);

        /* Q, K, V projections */
        matvec(s->q, ly->attn_q, s->xb, dim, dim);
        matvec(s->k, ly->attn_k, s->xb, kv_dim, dim);
        matvec(s->v, ly->attn_v, s->xb, kv_dim, dim);

        /* RoPE */
        rope(s->q, s->n_heads,    hd, pos, 500000.0f);
        rope(s->k, s->n_kv_heads, hd, pos, 500000.0f);

        /* Store K, V in cache */
        float *kc = s->kv_cache[l].k + (uint64_t)pos * kv_dim;
        float *vc = s->kv_cache[l].v + (uint64_t)pos * kv_dim;
        memcpy(kc, s->k, kv_dim * sizeof(float));
        memcpy(vc, s->v, kv_dim * sizeof(float));

        /* ── Grouped Query Attention ── */
        float scale = 1.0f / sqrtf_bare((float)hd);

        for (uint32_t h = 0; h < s->n_heads; h++) {
            uint32_t kv_h = h / s->gqa_ratio;

            float *q_head = s->q + h * hd;

            /* Compute attention scores for all positions */
            for (uint32_t p = 0; p <= pos; p++) {
                float *k_pos = s->kv_cache[l].k + (uint64_t)p * kv_dim + kv_h * hd;
                float dot = 0.0f;
                for (uint32_t i = 0; i < hd; i++)
                    dot += q_head[i] * k_pos[i];
                s->att[p] = dot * scale;
            }

            /* Softmax over scores */
            softmax(s->att, pos + 1);

            /* Weighted sum of values */
            float *out_head = s->xb2 + h * hd;
            memset(out_head, 0, hd * sizeof(float));
            for (uint32_t p = 0; p <= pos; p++) {
                float *v_pos = s->kv_cache[l].v + (uint64_t)p * kv_dim + kv_h * hd;
                float a = s->att[p];
                for (uint32_t i = 0; i < hd; i++)
                    out_head[i] += a * v_pos[i];
            }
        }

        /* Output projection */
        matvec(s->xb, ly->attn_output, s->xb2, dim, dim);

        /* Residual connection */
        vec_add(s->x, s->x, s->xb, dim);

        /* ── FFN ── */
        rmsnorm(s->xb, s->x, norm_data(ly->ffn_norm), dim);

        /* Gate + Up projections */
        matvec(s->hb,  ly->ffn_gate, s->xb, s->ffn_dim, dim);
        matvec(s->hb2, ly->ffn_up,   s->xb, s->ffn_dim, dim);

        /* SwiGLU: SiLU(gate) * up */
        silu_inplace(s->hb, s->ffn_dim);
        vec_mul(s->hb, s->hb, s->hb2, s->ffn_dim);

        /* Down projection */
        matvec(s->xb, ly->ffn_down, s->hb, dim, s->ffn_dim);

        /* Residual connection */
        vec_add(s->x, s->x, s->xb, dim);
    }

    /* ── Final norm + logits ── */
    rmsnorm(s->x, s->x, norm_data(s->weights.output_norm), dim);
    matvec(s->logits, s->weights.output, s->x, s->vocab_size, dim);

    s->pos++;
    return 0;
}

/* ── Argmax over logits ──────────────────────────────────────── */

static uint32_t argmax(const float *v, uint32_t n)
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
    uint64_t total_t0 = rdtsc();
    uint32_t total_tokens = 0;

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

    uint32_t next = argmax(state->logits, state->vocab_size);

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
        llama_forward(state, next);
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
        serial_puts(" ms)\n");
        total_tokens++;

        /* Print decoded text to framebuffer */
        if (text) fb_puts(text);

        next = argmax(state->logits, state->vocab_size);
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
    serial_puts(" ms/tok)\n");

    fb_puts(" Done: ");
    fb_putdec(total_tokens);
    fb_puts(" tok, ");
    fb_putdec(ms_per_tok);
    fb_puts(" ms/tok\n");
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
