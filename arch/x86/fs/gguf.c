/*
 * OsitoK x86-64 — GGUF Model Loader
 *
 * Loads GGUF files from OsitoFS v2 into RAM and builds an in-memory
 * tensor table with direct data pointers.
 *
 * Flow:
 *   1. Find first GGUF file in OsitoFS file table (FLAG_GGUF)
 *   2. Read layer index from block 3 (if available)
 *   3. Allocate contiguous RAM for entire file
 *   4. Read file data from NVMe in 1MB chunks
 *   5. Parse GGUF header in-memory -> tensor table
 *   6. Set data pointers into loaded buffer
 */

#include "../include/types.h"
#include "gguf.h"
#include "../../../include/common/ositofs2_format.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern osfs2_file_t *osfs2_find_gguf(void);

/* Tracked for kexec state preservation */
static void    *g_gguf_mmap_base;
static uint64_t g_gguf_mmap_size;
void    *gguf_get_mmap_base(void) { return g_gguf_mmap_base; }
uint64_t gguf_get_mmap_size(void) { return g_gguf_mmap_size; }
extern int osfs2_read_layer_index(uint16_t slot, osfs2_layer_idx_t *li);
extern int osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len);

/* ── In-memory cursor ────────────────────────────────────────── */

typedef struct {
    const uint8_t *buf;
    uint64_t       size;
    uint64_t       pos;
} gguf_cursor_t;

static int cur_read(gguf_cursor_t *c, void *out, uint64_t n)
{
    if (c->pos + n > c->size) return -1;
    memcpy(out, c->buf + c->pos, (size_t)n);
    c->pos += n;
    return 0;
}

static int cur_u32(gguf_cursor_t *c, uint32_t *val)
{
    return cur_read(c, val, 4);
}

static int cur_u64(gguf_cursor_t *c, uint64_t *val)
{
    return cur_read(c, val, 8);
}

static int cur_f32(gguf_cursor_t *c, float *val)
{
    return cur_read(c, val, 4);
}

static int cur_skip(gguf_cursor_t *c, uint64_t n)
{
    if (c->pos + n > c->size) return -1;
    c->pos += n;
    return 0;
}

/*
 * Read GGUF string: uint64_t len + data (NOT null-terminated in file).
 * If out is NULL, the string is skipped.
 */
static int cur_string(gguf_cursor_t *c, char *out, uint64_t max_len)
{
    uint64_t len;
    if (cur_u64(c, &len) < 0) return -1;

    if (!out || max_len == 0) {
        return cur_skip(c, len);
    }

    if (len < max_len) {
        if (cur_read(c, out, len) < 0) return -1;
        out[len] = '\0';
    } else {
        /* Truncate: read what fits, skip the rest */
        if (cur_read(c, out, max_len - 1) < 0) return -1;
        out[max_len - 1] = '\0';
        if (cur_skip(c, len - (max_len - 1)) < 0) return -1;
    }
    return 0;
}

/* Skip a GGUF value of given type */
static int cur_skip_value(gguf_cursor_t *c, uint32_t type)
{
    switch (type) {
    case GGUF_TYPE_UINT8:
    case GGUF_TYPE_INT8:
    case GGUF_TYPE_BOOL:    return cur_skip(c, 1);
    case GGUF_TYPE_UINT16:
    case GGUF_TYPE_INT16:   return cur_skip(c, 2);
    case GGUF_TYPE_UINT32:
    case GGUF_TYPE_INT32:
    case GGUF_TYPE_FLOAT32: return cur_skip(c, 4);
    case GGUF_TYPE_UINT64:
    case GGUF_TYPE_INT64:
    case GGUF_TYPE_FLOAT64: return cur_skip(c, 8);
    case GGUF_TYPE_STRING:
        return cur_string(c, NULL, 0);
    case GGUF_TYPE_ARRAY: {
        uint32_t arr_type;
        uint64_t arr_len;
        if (cur_u32(c, &arr_type) < 0) return -1;
        if (cur_u64(c, &arr_len) < 0) return -1;
        for (uint64_t i = 0; i < arr_len; i++)
            if (cur_skip_value(c, arr_type) < 0) return -1;
        return 0;
    }
    default:
        serial_puts("[GGUF] Unknown value type ");
        serial_putdec(type);
        serial_puts("\n");
        return -1;
    }
}

/* ── Tensor byte size calculation ────────────────────────────── */

static uint64_t ggml_block_size(uint32_t type)
{
    switch (type) {
    case GGML_TYPE_F32:  return 4;
    case GGML_TYPE_F16:  return 2;
    case GGML_TYPE_Q4_0: return 18;   /* 32 vals: 2 + 16 */
    case GGML_TYPE_Q4_1: return 20;   /* 32 vals: 4 + 16 */
    case GGML_TYPE_Q5_0: return 22;   /* 32 vals: 2 + 4 + 16 */
    case GGML_TYPE_Q5_1: return 24;   /* 32 vals: 4 + 4 + 16 */
    case GGML_TYPE_Q8_0: return 34;   /* 32 vals: 2 + 32 */
    case GGML_TYPE_Q8_1: return 36;   /* 32 vals: 4 + 32 */
    case GGML_TYPE_Q2_K: return 84;   /* 256 vals */
    case GGML_TYPE_Q3_K: return 110;  /* 256 vals */
    case GGML_TYPE_Q4_K: return 144;  /* 256 vals */
    case GGML_TYPE_Q5_K: return 176;  /* 256 vals */
    case GGML_TYPE_Q6_K: return 210;  /* 256 vals */
    default:             return 0;
    }
}

static uint64_t ggml_group_size(uint32_t type)
{
    switch (type) {
    case GGML_TYPE_F32:
    case GGML_TYPE_F16:  return 1;
    case GGML_TYPE_Q4_0:
    case GGML_TYPE_Q4_1:
    case GGML_TYPE_Q5_0:
    case GGML_TYPE_Q5_1:
    case GGML_TYPE_Q8_0:
    case GGML_TYPE_Q8_1: return 32;
    case GGML_TYPE_Q2_K:
    case GGML_TYPE_Q3_K:
    case GGML_TYPE_Q4_K:
    case GGML_TYPE_Q5_K:
    case GGML_TYPE_Q6_K: return 256;
    default:             return 0;
    }
}

static uint64_t gguf_tensor_byte_size(uint32_t type, uint64_t elements)
{
    uint64_t bs = ggml_block_size(type);
    uint64_t gs = ggml_group_size(type);
    if (bs == 0 || gs == 0) return elements * 4; /* fallback: f32 */
    return (elements / gs) * bs;
}

/* ── Quant type name ─────────────────────────────────────────── */

static const char *gguf_quant_name(uint32_t osfs2_quant)
{
    switch (osfs2_quant) {
    case OSFS2_QUANT_F32:     return "F32";
    case OSFS2_QUANT_F16:     return "F16";
    case OSFS2_QUANT_Q4_0:    return "Q4_0";
    case OSFS2_QUANT_Q4_1:    return "Q4_1";
    case OSFS2_QUANT_Q5_0:    return "Q5_0";
    case OSFS2_QUANT_Q5_1:    return "Q5_1";
    case OSFS2_QUANT_Q8_0:    return "Q8_0";
    case OSFS2_QUANT_Q2_K:    return "Q2_K";
    case OSFS2_QUANT_Q3_K:    return "Q3_K";
    case OSFS2_QUANT_Q4_K:    return "Q4_K";
    case OSFS2_QUANT_Q5_K:    return "Q5_K";
    case OSFS2_QUANT_Q6_K:    return "Q6_K";
    case OSFS2_QUANT_IQ2_XXS: return "IQ2_XXS";
    case OSFS2_QUANT_IQ3_XXS: return "IQ3_XXS";
    default:                  return "unknown";
    }
}

/* ── In-memory GGUF parser ───────────────────────────────────── */

static bool key_eq(const char *key, const char *target);
static int  gguf_extract_hyperparams(gguf_model_t *model);

static int gguf_parse_buffer(gguf_model_t *model)
{
    gguf_cursor_t c = {
        .buf  = (const uint8_t *)model->file_data,
        .size = model->file_size,
        .pos  = 0
    };

    /* Header: magic + version + tensor_count + kv_count */
    uint32_t magic, version;
    uint64_t tensor_count, kv_count;

    if (cur_u32(&c, &magic) < 0 || magic != GGUF_MAGIC) {
        serial_puts("[GGUF] Bad magic\n");
        return -1;
    }
    if (cur_u32(&c, &version) < 0 || version < 2 || version > 3) {
        serial_puts("[GGUF] Unsupported version\n");
        return -1;
    }
    if (cur_u64(&c, &tensor_count) < 0) return -1;
    if (cur_u64(&c, &kv_count) < 0) return -1;

    serial_puts("[GGUF] GGUF v");
    serial_putdec(version);
    serial_puts(": ");
    serial_putdec(tensor_count);
    serial_puts(" tensors, ");
    serial_putdec(kv_count);
    serial_puts(" metadata keys\n");

    /* Walk metadata KV pairs. Most keys go to OsitoFS-cached file
     * entry fields (num_layers, hidden_size, etc.) when the file was
     * written, so we skip them here. The exception is
     * `general.architecture`: it's not stored in osfs2_file_t, but
     * llama_init() reads model->architecture to dispatch between the
     * standard Llama forward and the brandon-arch path (block-shared
     * layers, register prefill, DWA, value residual). Without setting
     * it here, every brandon-tiny model falls through to the Llama
     * path with mismatched layer plumbing → forward stalls and never
     * produces a token. */
    for (uint64_t i = 0; i < kv_count; i++) {
        char key[64];
        if (cur_string(&c, key, sizeof key) < 0) {
            serial_puts("[GGUF] Failed at metadata key ");
            serial_putdec(i);
            serial_puts("\n");
            return -1;
        }
        uint32_t val_type;
        if (cur_u32(&c, &val_type) < 0) return -1;
        if (key_eq(key, "general.architecture") &&
            val_type == GGUF_TYPE_STRING) {
            if (cur_string(&c, model->architecture,
                            sizeof model->architecture) < 0)
                return -1;
            serial_puts("[GGUF] architecture=");
            serial_puts(model->architecture);
            serial_puts("\n");
            continue;
        }
        if (cur_skip_value(&c, val_type) < 0) {
            serial_puts("[GGUF] Failed at metadata value ");
            serial_putdec(i);
            serial_puts("\n");
            return -1;
        }
    }

    /* Allocate tensor table */
    if (tensor_count == 0) {
        model->num_tensors = 0;
        model->tensors = NULL;
        return 0;
    }

    model->num_tensors = (uint32_t)tensor_count;
    uint64_t table_size = tensor_count * sizeof(gguf_tensor_t);
    model->tensors = (gguf_tensor_t *)mem_alloc_aligned(table_size, 4096);
    if (!model->tensors) {
        serial_puts("[GGUF] Failed to alloc tensor table\n");
        return -1;
    }
    memset(model->tensors, 0, (size_t)table_size);

    /* Parse tensor info */
    for (uint64_t i = 0; i < tensor_count; i++) {
        gguf_tensor_t *t = &model->tensors[i];

        /* Name */
        if (cur_string(&c, t->name, sizeof(t->name)) < 0) return -1;

        /* n_dimensions — read raw, cap stored value to 4 */
        uint32_t n_dims_raw;
        if (cur_u32(&c, &n_dims_raw) < 0) return -1;
        t->n_dims = n_dims_raw > 4 ? 4 : n_dims_raw;

        /* Dimensions — read all, store up to 4 */
        uint64_t total_elements = 1;
        for (uint32_t d = 0; d < n_dims_raw; d++) {
            uint64_t dim;
            if (cur_u64(&c, &dim) < 0) return -1;
            if (d < 4) t->ne[d] = dim;
            total_elements *= dim;
        }

        /* Type */
        if (cur_u32(&c, &t->type) < 0) return -1;

        /* Offset (relative to tensor data start) */
        if (cur_u64(&c, &t->offset) < 0) return -1;

        /* Compute byte size */
        t->size = gguf_tensor_byte_size(t->type, total_elements);
    }

    /* Tensor data starts after header+metadata+tensor_info, aligned to 32 */
    uint64_t alignment = 32;
    model->tensor_data_offset = (c.pos + alignment - 1) & ~(alignment - 1);
    model->tensor_data = (uint8_t *)model->file_data + model->tensor_data_offset;

    /* Compute total tensor data size */
    uint64_t max_end = 0;
    for (uint32_t i = 0; i < model->num_tensors; i++) {
        uint64_t end = model->tensors[i].offset + model->tensors[i].size;
        if (end > max_end) max_end = end;
    }
    model->tensor_data_size = max_end;

    /* Set direct data pointers */
    for (uint32_t i = 0; i < model->num_tensors; i++) {
        model->tensors[i].data = (uint8_t *)model->tensor_data + model->tensors[i].offset;
    }

    return 0;
}

/* ── Main loader ─────────────────────────────────────────────── */

int gguf_load(gguf_model_t *model)
{
    memset(model, 0, sizeof(*model));

    /* Find first GGUF file in OsitoFS */
    osfs2_file_t *file = osfs2_find_gguf();
    if (!file) {
        serial_puts("[GGUF] No GGUF file found in OsitoFS\n");
        return -1;
    }

    /* Copy metadata from OsitoFS file entry */
    memcpy(model->model_name, file->model_name, GGUF_MODEL_NAME_LEN);
    model->quant_type     = file->quant_type;
    model->num_layers     = file->num_layers;
    model->hidden_size    = file->hidden_size;
    model->vocab_size     = file->vocab_size;
    model->head_count     = file->head_count;
    model->kv_head_count  = file->kv_head_count;
    model->context_length = file->context_length;
    model->file_size      = file->size;

    serial_puts("[GGUF] Loading \"");
    serial_puts(file->name);
    serial_puts("\" (");
    serial_putdec(file->size / (1024 * 1024));
    serial_puts(" MB)...\n");

    fb_puts("\n Loading ");
    fb_puts_color(file->name, 0x0000FF00);
    fb_puts(" (");
    fb_putdec(file->size / (1024 * 1024));
    fb_puts(" MB)...\n");

    /* Read layer index from block 3 (if available) */
    if (file->layer_index_slot != 0xFFFF) {
        osfs2_layer_idx_t li;
        if (osfs2_read_layer_index(file->layer_index_slot, &li) == 0) {
            model->layer_count = li.num_layers;
            if (model->layer_count > 255) model->layer_count = 255;
            for (uint32_t i = 0; i < model->layer_count; i++)
                model->layer_offsets[i] = li.layer_offset[i];
        }
    }

    /* Allocate contiguous RAM for entire file */
    model->file_data = mem_alloc_aligned(file->size, 4096);
    g_gguf_mmap_base = model->file_data;
    g_gguf_mmap_size = file->size;
    if (!model->file_data) {
        serial_puts("[GGUF] Failed to allocate ");
        serial_putdec(file->size / (1024 * 1024));
        serial_puts(" MB\n");
        return -1;
    }

    /* Read file data in 1MB chunks */
    uint64_t offset = 0;
    uint64_t remaining = file->size;

    while (remaining > 0) {
        uint64_t chunk = remaining < OSFS2_IO_CHUNK ? remaining : OSFS2_IO_CHUNK;
        if (osfs2_read(file, offset, (uint8_t *)model->file_data + offset, chunk) < 0) {
            serial_puts("[GGUF] Read failed at offset ");
            serial_puthex(offset, 16);
            serial_puts("\n");
            return -1;
        }
        offset += chunk;
        remaining -= chunk;

        /* Progress every 64MB */
        if ((offset % (64 * 1024 * 1024)) == 0 || remaining == 0) {
            serial_puts("[GGUF] Reading... ");
            serial_putdec(offset / (1024 * 1024));
            serial_puts("/");
            serial_putdec(file->size / (1024 * 1024));
            serial_puts(" MB\n");
        }
    }

    /* Extract metadata first: fills model->architecture +
     * brandon.{use_dwa,use_value_residual,n_registers,compute_layer_count,
     * layer_map,...} plus the rest of the brandon.* / llama.* hyperparams
     * that osfs2_file_t doesn't cache. Without this the runtime sees
     * arch="" → falls through to the plain Llama forward → mismatched
     * layer plumbing → wedge or token soup. */
    if (gguf_extract_hyperparams(model) < 0) {
        serial_puts("[GGUF] Hyperparam extract failed\n");
        return -1;
    }

    /* Parse GGUF header in-memory (tensor table). */
    if (gguf_parse_buffer(model) < 0) {
        serial_puts("[GGUF] Parse failed\n");
        return -1;
    }

    /* Report results */
    serial_puts("[GGUF] Model: ");
    serial_puts(model->model_name);
    serial_puts("\n[GGUF] Quant: ");
    serial_puts(gguf_quant_name(model->quant_type));
    serial_puts(", Layers: ");
    serial_putdec(model->num_layers);
    serial_puts(", Hidden: ");
    serial_putdec(model->hidden_size);
    serial_puts(", Vocab: ");
    serial_putdec(model->vocab_size);
    serial_puts("\n[GGUF] Heads: ");
    serial_putdec(model->head_count);
    serial_puts(" (KV: ");
    serial_putdec(model->kv_head_count);
    serial_puts("), Context: ");
    serial_putdec(model->context_length);
    serial_puts("\n[GGUF] Tensor data: ");
    serial_putdec(model->tensor_data_size / (1024 * 1024));
    serial_puts(" MB at ");
    serial_puthex((uint64_t)model->tensor_data, 16);
    serial_puts("\n[GGUF] Model ready for inference\n");

    fb_puts(" Model: ");
    fb_puts_color(model->model_name, 0x0000FF00);
    fb_puts("\n ");
    fb_puts(gguf_quant_name(model->quant_type));
    fb_puts("  L:");
    fb_putdec(model->num_layers);
    fb_puts("  H:");
    fb_putdec(model->hidden_size);
    fb_puts("  V:");
    fb_putdec(model->vocab_size);
    fb_puts("\n Tensors: ");
    fb_putdec(model->num_tensors);
    fb_puts(" (");
    fb_putdec(model->tensor_data_size / (1024 * 1024));
    fb_puts(" MB)\n");

    return 0;
}

/* ── Find tensor by name ─────────────────────────────────────── */

gguf_tensor_t *gguf_find_tensor(gguf_model_t *model, const char *name)
{
    for (uint32_t i = 0; i < model->num_tensors; i++) {
        if (strcmp(model->tensors[i].name, name) == 0)
            return &model->tensors[i];
    }
    return NULL;
}

/* ── Tokenizer extraction from GGUF metadata ─────────────────── */

/* Read GGUF string into key_buf, returning length. -1 on error. */
static int cur_string_len(gguf_cursor_t *c, char *out, uint64_t max_len)
{
    uint64_t len;
    if (cur_u64(c, &len) < 0) return -1;

    if (out && max_len > 0) {
        uint64_t copy = len < (max_len - 1) ? len : (max_len - 1);
        if (cur_read(c, out, copy) < 0) return -1;
        out[copy] = '\0';
        if (len > copy && cur_skip(c, len - copy) < 0) return -1;
    } else {
        if (cur_skip(c, len) < 0) return -1;
    }
    return (int)len;
}

/* Compare GGUF key name */
static bool key_eq(const char *key, const char *target)
{
    while (*key && *target) {
        if (*key != *target) return false;
        key++;
        target++;
    }
    return *key == *target;
}

int gguf_load_tokenizer(gguf_model_t *model, gguf_tokenizer_t *tok)
{
    memset(tok, 0, sizeof(*tok));

    if (!model->file_data || model->file_size < 32) return -1;

    /* Re-parse the GGUF header to find tokenizer metadata */
    gguf_cursor_t c;
    c.buf = (const uint8_t *)model->file_data;
    c.size = model->file_size;
    c.pos = 0;

    uint32_t magic, version;
    uint64_t tensor_count, kv_count;
    if (cur_u32(&c, &magic) < 0) return -1;
    if (magic != GGUF_MAGIC) return -1;
    if (cur_u32(&c, &version) < 0) return -1;
    if (cur_u64(&c, &tensor_count) < 0) return -1;
    if (cur_u64(&c, &kv_count) < 0) return -1;

    tok->bos_id = 1;   /* Safe defaults (overwritten from GGUF metadata) */
    tok->eos_id = 2;
    tok->unk_id = (uint32_t)-1;
    tok->pad_id = (uint32_t)-1;

    /* First pass: scan for tokenizer keys to get sizes / offsets. */
    uint64_t tokens_offset = 0, merges_offset = 0;
    uint64_t scores_offset = 0, types_offset  = 0;
    uint32_t n_tokens = 0, n_merges = 0;
    uint32_t n_scores = 0, n_types = 0;

    for (uint64_t i = 0; i < kv_count; i++) {
        char key[128];
        if (cur_string_len(&c, key, sizeof(key)) < 0) return -1;

        uint32_t val_type;
        if (cur_u32(&c, &val_type) < 0) return -1;

        if (key_eq(key, "tokenizer.ggml.model") && val_type == GGUF_TYPE_STRING) {
            cur_string(&c, tok->tok_model, sizeof(tok->tok_model));
        } else if (key_eq(key, "tokenizer.ggml.tokens") && val_type == GGUF_TYPE_ARRAY) {
            uint32_t arr_type;
            uint64_t arr_len;
            if (cur_u32(&c, &arr_type) < 0) return -1;
            if (cur_u64(&c, &arr_len) < 0) return -1;
            if (arr_type != GGUF_TYPE_STRING) {
                for (uint64_t j = 0; j < arr_len; j++)
                    if (cur_skip_value(&c, arr_type) < 0) return -1;
                continue;
            }
            tokens_offset = c.pos;
            n_tokens = (uint32_t)arr_len;
            for (uint64_t j = 0; j < arr_len; j++)
                if (cur_string(&c, NULL, 0) < 0) return -1;
        } else if (key_eq(key, "tokenizer.ggml.merges") && val_type == GGUF_TYPE_ARRAY) {
            uint32_t arr_type;
            uint64_t arr_len;
            if (cur_u32(&c, &arr_type) < 0) return -1;
            if (cur_u64(&c, &arr_len) < 0) return -1;
            if (arr_type != GGUF_TYPE_STRING) {
                for (uint64_t j = 0; j < arr_len; j++)
                    if (cur_skip_value(&c, arr_type) < 0) return -1;
                continue;
            }
            merges_offset = c.pos;
            n_merges = (uint32_t)arr_len;
            for (uint64_t j = 0; j < arr_len; j++)
                if (cur_string(&c, NULL, 0) < 0) return -1;
        } else if (key_eq(key, "tokenizer.ggml.scores") && val_type == GGUF_TYPE_ARRAY) {
            uint32_t arr_type;
            uint64_t arr_len;
            if (cur_u32(&c, &arr_type) < 0) return -1;
            if (cur_u64(&c, &arr_len) < 0) return -1;
            if (arr_type != GGUF_TYPE_FLOAT32) {
                for (uint64_t j = 0; j < arr_len; j++)
                    if (cur_skip_value(&c, arr_type) < 0) return -1;
                continue;
            }
            scores_offset = c.pos;
            n_scores = (uint32_t)arr_len;
            if (cur_skip(&c, arr_len * 4) < 0) return -1;
        } else if (key_eq(key, "tokenizer.ggml.token_type") && val_type == GGUF_TYPE_ARRAY) {
            uint32_t arr_type;
            uint64_t arr_len;
            if (cur_u32(&c, &arr_type) < 0) return -1;
            if (cur_u64(&c, &arr_len) < 0) return -1;
            if (arr_type != GGUF_TYPE_INT32 && arr_type != GGUF_TYPE_UINT32) {
                for (uint64_t j = 0; j < arr_len; j++)
                    if (cur_skip_value(&c, arr_type) < 0) return -1;
                continue;
            }
            types_offset = c.pos;
            n_types = (uint32_t)arr_len;
            if (cur_skip(&c, arr_len * 4) < 0) return -1;
        } else if (key_eq(key, "tokenizer.ggml.bos_token_id")) {
            if (val_type == GGUF_TYPE_UINT32 || val_type == GGUF_TYPE_INT32)
                cur_u32(&c, &tok->bos_id);
            else cur_skip_value(&c, val_type);
        } else if (key_eq(key, "tokenizer.ggml.eos_token_id")) {
            if (val_type == GGUF_TYPE_UINT32 || val_type == GGUF_TYPE_INT32)
                cur_u32(&c, &tok->eos_id);
            else cur_skip_value(&c, val_type);
        } else if (key_eq(key, "tokenizer.ggml.unknown_token_id")) {
            if (val_type == GGUF_TYPE_UINT32 || val_type == GGUF_TYPE_INT32)
                cur_u32(&c, &tok->unk_id);
            else cur_skip_value(&c, val_type);
        } else if (key_eq(key, "tokenizer.ggml.padding_token_id")) {
            if (val_type == GGUF_TYPE_UINT32 || val_type == GGUF_TYPE_INT32)
                cur_u32(&c, &tok->pad_id);
            else cur_skip_value(&c, val_type);
        } else {
            if (cur_skip_value(&c, val_type) < 0) return -1;
        }
    }

    if (n_tokens == 0) {
        serial_puts("[GGUF] No tokenizer found in metadata\n");
        return -1;
    }

    serial_puts("[GGUF] Tokenizer model=\"");
    serial_puts(tok->tok_model[0] ? tok->tok_model : "(unset)");
    serial_puts("\" tokens=");
    serial_putdec(n_tokens);
    serial_puts(" merges=");
    serial_putdec(n_merges);
    serial_puts(" scores=");
    serial_putdec(n_scores);
    serial_puts("\n");

    /* Second pass: extract token strings (zero-copy pointers into file). */
    tok->tokens = (const char **)mem_alloc_aligned(
        (uint64_t)n_tokens * sizeof(char *), 8);
    tok->token_lens = (uint32_t *)mem_alloc_aligned(
        (uint64_t)n_tokens * sizeof(uint32_t), 4);
    if (!tok->tokens || !tok->token_lens) {
        serial_puts("[GGUF] Failed to alloc token arrays\n");
        return -1;
    }
    tok->n_tokens = n_tokens;

    c.pos = tokens_offset;
    for (uint32_t i = 0; i < n_tokens; i++) {
        uint64_t slen;
        if (cur_u64(&c, &slen) < 0) return -1;
        tok->tokens[i] = (const char *)(c.buf + c.pos);
        tok->token_lens[i] = (uint32_t)slen;
        if (cur_skip(&c, slen) < 0) return -1;
    }
    tok->token_data = NULL;

    /* Optional: BPE merges */
    if (n_merges > 0 && merges_offset > 0) {
        tok->merges = (const char **)mem_alloc_aligned(
            (uint64_t)n_merges * sizeof(char *), 8);
        tok->merge_lens = (uint32_t *)mem_alloc_aligned(
            (uint64_t)n_merges * sizeof(uint32_t), 4);
        if (tok->merges && tok->merge_lens) {
            tok->n_merges = n_merges;
            c.pos = merges_offset;
            for (uint32_t i = 0; i < n_merges; i++) {
                uint64_t slen;
                if (cur_u64(&c, &slen) < 0) break;
                tok->merges[i] = (const char *)(c.buf + c.pos);
                tok->merge_lens[i] = (uint32_t)slen;
                if (cur_skip(&c, slen) < 0) break;
            }
            tok->merge_data = NULL;
        }
    }

    /* Optional: SPM scores + token types (zero-copy into file buffer). */
    if (n_scores == n_tokens && scores_offset > 0)
        tok->scores = (float *)((const uint8_t *)c.buf + scores_offset);
    if (n_types == n_tokens && types_offset > 0)
        tok->token_types = (uint32_t *)((const uint8_t *)c.buf + types_offset);

    /* Brandon doesn't ship a vocab_size key; canonical source is
     * len(tokenizer.ggml.tokens). Fill it for llama_init validation. */
    if (model->vocab_size == 0)
        model->vocab_size = n_tokens;

    tok->valid = true;
    return 0;
}

void gguf_free_tokenizer(gguf_tokenizer_t *tok)
{
    /* Zero-copy: tokens/merges point into file buffer, don't free.
     * Only free the pointer arrays. */
    (void)tok;
    /* Arrays allocated via mem_alloc_aligned — not individually freeable
     * with current allocator. Leak is acceptable (boot-time allocation). */
}

extern float f16_to_f32(uint16_t h);

int gguf_dequant_f16_to_f32(gguf_model_t *model)
{
    if (!model || !model->tensors) return -1;

    uint64_t n_converted = 0;
    uint64_t bytes_added = 0;

    for (uint32_t i = 0; i < model->num_tensors; i++) {
        gguf_tensor_t *t = &model->tensors[i];
        if (t->type != GGML_TYPE_F16) continue;

        /* Element count from shape; ne[k]==0 means dimension unused */
        uint64_t n_elems = 1;
        for (uint32_t d = 0; d < t->n_dims && d < 4; d++)
            if (t->ne[d] > 0) n_elems *= t->ne[d];

        if (n_elems == 0) continue;

        uint64_t new_bytes = n_elems * sizeof(float);
        float *out = (float *)mem_alloc_aligned(new_bytes, 64);
        if (!out) {
            serial_puts("[GGUF] dequant alloc failed at tensor ");
            serial_putdec(i); serial_puts("\n");
            return -1;
        }

        const uint16_t *src = (const uint16_t *)t->data;
        for (uint64_t k = 0; k < n_elems; k++)
            out[k] = f16_to_f32(src[k]);

        t->data = out;
        t->type = GGML_TYPE_F32;
        t->size = new_bytes;
        n_converted++;
        bytes_added += new_bytes;
    }

    serial_puts("[GGUF] Pre-dequant F16->F32: ");
    serial_putdec(n_converted);
    serial_puts(" tensors, +");
    serial_putdec(bytes_added / (1024 * 1024));
    serial_puts(" MB\n");
    return 0;
}

/* Selective Q4_K → F32 dequant. Walks every tensor whose name contains
 * `name_filter` (e.g. ".attn_" matches attn_q/k/v/output across all
 * layers) and converts the data in-place. Used to satisfy the GPU
 * fused-attn F32-only gate for Q4_K_M models. FFN tensors stay
 * Q4_K — they'd dwarf the memory budget.
 *
 * Memory cost reference (Llama 1B Q4_K_M, dim=2048, 16 layers):
 *   attn_q + attn_o = 16 MB each per layer
 *   attn_k + attn_v =  4 MB each per layer
 *   Total ≈ 640 MB extra
 * Caller decides if this fits. The function logs the totals and
 * returns the number of tensors converted (or -1 on error). */
/* Inline Q4_K block dequant — scalar fallback. Equivalent to the
 * one in tensor.c but kept local to avoid exposing the static. */
static void gguf_q4k_scale_min(int j, const uint8_t *q,
                                uint8_t *d, uint8_t *m)
{
    if (j < 4) { *d = q[j] & 63; *m = q[j+4] & 63; }
    else {
        *d = (q[j+4] & 0x0F) | ((q[j-4] >> 6) << 4);
        *m = (q[j+4] >> 4)   | ((q[j]   >> 6) << 4);
    }
}
static void gguf_dequant_q4k_block(const uint8_t *block, float *y)
{
    float d    = f16_to_f32(*(const uint16_t *)(block + 0));
    float dmin = f16_to_f32(*(const uint16_t *)(block + 2));
    const uint8_t *scales = block + 4;
    const uint8_t *qs     = block + 16;
    int is = 0;
    for (int chunk = 0; chunk < 256; chunk += 64) {
        uint8_t sc, m;
        gguf_q4k_scale_min(is + 0, scales, &sc, &m);
        float d1 = d * (float)sc, m1 = dmin * (float)m;
        gguf_q4k_scale_min(is + 1, scales, &sc, &m);
        float d2 = d * (float)sc, m2 = dmin * (float)m;
        for (int l = 0; l < 32; l++)
            y[chunk +     l] = d1 * (float)(qs[l] & 0xF) - m1;
        for (int l = 0; l < 32; l++)
            y[chunk + 32 + l] = d2 * (float)(qs[l] >> 4) - m2;
        qs += 32; is += 2;
    }
}

int gguf_dequant_q4k_to_f32(gguf_model_t *model, const char *name_filter)
{
    if (!model || !model->tensors) return -1;
    uint64_t n_converted = 0, bytes_added = 0;
    for (uint32_t i = 0; i < model->num_tensors; i++) {
        gguf_tensor_t *t = &model->tensors[i];
        if (t->type != GGML_TYPE_Q4_K) continue;
        if (name_filter && *name_filter) {
            const char *hay = t->name, *needle = name_filter;
            bool match = false;
            for (; *hay && !match; hay++) {
                const char *h = hay, *n = needle;
                while (*h && *n && *h == *n) { h++; n++; }
                if (!*n) match = true;
            }
            if (!match) continue;
        }
        uint64_t n_elems = 1;
        for (uint32_t d = 0; d < t->n_dims && d < 4; d++)
            if (t->ne[d] > 0) n_elems *= t->ne[d];
        if (n_elems == 0 || (n_elems % 256) != 0) continue;
        uint64_t n_blocks = n_elems / 256;
        uint64_t new_bytes = n_elems * sizeof(float);
        float *out = (float *)mem_alloc_aligned(new_bytes, 64);
        if (!out) {
            serial_puts("[GGUF] Q4_K dequant alloc failed at ");
            serial_puts(t->name); serial_puts("\n");
            return -1;
        }
        const uint8_t *src = (const uint8_t *)t->data;
        for (uint64_t b = 0; b < n_blocks; b++)
            gguf_dequant_q4k_block(src + b * 144, out + b * 256);
        t->data = out;
        t->type = GGML_TYPE_F32;
        t->size = new_bytes;
        n_converted++;
        bytes_added += new_bytes;
    }
    serial_puts("[GGUF] Pre-dequant Q4_K->F32 (filter='");
    serial_puts(name_filter ? name_filter : ""); serial_puts("'): ");
    serial_putdec(n_converted); serial_puts(" tensors, +");
    serial_putdec(bytes_added / (1024 * 1024)); serial_puts(" MB\n");
    return (int)n_converted;
}

/* ── WASM: load from in-memory buffer ───────────────────────────
 *
 * Replaces gguf_load for the WASM build where OsitoFS is unavailable.
 * The caller provides the raw GGUF file bytes already in RAM.
 * Does two metadata passes:
 *   1. gguf_extract_hyperparams — fills model->num_layers, hidden_size, etc.
 *   2. gguf_parse_buffer        — fills model->tensors + data pointers
 */

/* Extract architecture hyperparameters from GGUF KV metadata. */
static int gguf_extract_hyperparams(gguf_model_t *model)
{
    gguf_cursor_t c = {
        .buf  = (const uint8_t *)model->file_data,
        .size = model->file_size,
        .pos  = 0
    };

    uint32_t magic, version;
    uint64_t tensor_count, kv_count;
    if (cur_u32(&c, &magic) < 0 || magic != GGUF_MAGIC) return -1;
    if (cur_u32(&c, &version) < 0) return -1;
    if (cur_u64(&c, &tensor_count) < 0) return -1;
    if (cur_u64(&c, &kv_count) < 0) return -1;

    /* Track where brandon.layer_map lives so we can copy it after sizing */
    uint64_t layer_map_offset = 0;
    uint32_t layer_map_count  = 0;
    bool     have_compute_lc  = false;

    for (uint64_t i = 0; i < kv_count; i++) {
        char key[128];
        if (cur_string(&c, key, sizeof(key)) < 0) return -1;

        uint32_t val_type;
        if (cur_u32(&c, &val_type) < 0) return -1;

        if (key_eq(key, "general.name") && val_type == GGUF_TYPE_STRING) {
            cur_string(&c, model->model_name, GGUF_MODEL_NAME_LEN);
        } else if (key_eq(key, "general.architecture") && val_type == GGUF_TYPE_STRING) {
            cur_string(&c, model->architecture, sizeof(model->architecture));
        } else if ((key_eq(key, "llama.embedding_length") ||
                    key_eq(key, "brandon.embedding_length")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->hidden_size);
        } else if ((key_eq(key, "llama.block_count") ||
                    key_eq(key, "brandon.block_count")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->num_layers);
        } else if ((key_eq(key, "llama.attention.head_count") ||
                    key_eq(key, "brandon.attention.head_count")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->head_count);
        } else if ((key_eq(key, "llama.attention.head_count_kv") ||
                    key_eq(key, "brandon.attention.head_count_kv")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->kv_head_count);
        } else if ((key_eq(key, "llama.context_length") ||
                    key_eq(key, "brandon.context_length")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->context_length);
        } else if ((key_eq(key, "llama.feed_forward_length") ||
                    key_eq(key, "brandon.feed_forward_length")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->feed_forward_length);
        } else if (key_eq(key, "general.quantization_version") && val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->quant_type);
        } else if ((key_eq(key, "llama.rope.freq_base") ||
                    key_eq(key, "brandon.rope.freq_base")) &&
                   val_type == GGUF_TYPE_FLOAT32) {
            cur_f32(&c, &model->rope_freq_base);
        } else if ((key_eq(key, "llama.rope.dimension_count") ||
                    key_eq(key, "brandon.rope.dimension_count")) &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->rope_dim_count);
        } else if ((key_eq(key, "llama.attention.layer_norm_rms_epsilon") ||
                    key_eq(key, "brandon.attention.layer_norm_rms_epsilon")) &&
                   val_type == GGUF_TYPE_FLOAT32) {
            cur_f32(&c, &model->attn_layer_norm_rms_eps);
        } else if (key_eq(key, "brandon.compute_layer_count") &&
                   val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->brandon_compute_layer_count);
            have_compute_lc = true;
        } else if (key_eq(key, "brandon.use_dwa") && val_type == GGUF_TYPE_BOOL) {
            uint8_t b = 0; cur_read(&c, &b, 1);
            model->brandon_use_dwa = b != 0;
        } else if (key_eq(key, "brandon.use_value_residual") && val_type == GGUF_TYPE_BOOL) {
            uint8_t b = 0; cur_read(&c, &b, 1);
            model->brandon_use_value_residual = b != 0;
        } else if (key_eq(key, "brandon.weight_tying") && val_type == GGUF_TYPE_BOOL) {
            uint8_t b = 0; cur_read(&c, &b, 1);
            model->brandon_weight_tying = b != 0;
        } else if (key_eq(key, "brandon.n_registers") && val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->brandon_n_registers);
        } else if (key_eq(key, "brandon.n_loops") && val_type == GGUF_TYPE_UINT32) {
            cur_u32(&c, &model->brandon_n_loops);
        } else if (key_eq(key, "brandon.layer_map") && val_type == GGUF_TYPE_ARRAY) {
            uint32_t arr_type; uint64_t arr_len;
            if (cur_u32(&c, &arr_type) < 0) return -1;
            if (cur_u64(&c, &arr_len) < 0) return -1;
            if (arr_type != GGUF_TYPE_INT32 && arr_type != GGUF_TYPE_UINT32) {
                for (uint64_t j = 0; j < arr_len; j++)
                    if (cur_skip_value(&c, arr_type) < 0) return -1;
                continue;
            }
            layer_map_offset = c.pos;
            layer_map_count  = (uint32_t)arr_len;
            if (cur_skip(&c, arr_len * 4) < 0) return -1;
        } else if (key_eq(key, "tokenizer.ggml.tokens") && val_type == GGUF_TYPE_ARRAY) {
            uint32_t arr_type; uint64_t arr_len;
            if (cur_u32(&c, &arr_type) < 0) return -1;
            if (cur_u64(&c, &arr_len) < 0) return -1;
            model->vocab_size = (uint32_t)arr_len;
            for (uint64_t j = 0; j < arr_len; j++)
                if (cur_skip_value(&c, arr_type) < 0) return -1;
        } else {
            if (cur_skip_value(&c, val_type) < 0) return -1;
        }
    }

    /* Resolve layer_map: copy out of file buffer into an aligned uint32_t
     * array so the consumer can index without worrying about endianness or
     * unaligned access. Allocated once per model load (boot-time leak ok). */
    if (layer_map_count > 0 && layer_map_offset > 0) {
        if (!have_compute_lc)
            model->brandon_compute_layer_count = layer_map_count;
        model->brandon_layer_map = (uint32_t *)mem_alloc_aligned(
            (uint64_t)layer_map_count * sizeof(uint32_t), 4);
        if (!model->brandon_layer_map) return -1;
        memcpy(model->brandon_layer_map,
               (const uint8_t *)model->file_data + layer_map_offset,
               (size_t)layer_map_count * sizeof(uint32_t));
    }

    if (model->kv_head_count == 0) model->kv_head_count = model->head_count;
    if (model->rope_freq_base == 0.0f) model->rope_freq_base = 10000.0f;
    return 0;
}

int gguf_load_from_mem(gguf_model_t *model, void *data, uint64_t size)
{
    memset(model, 0, sizeof(*model));
    model->file_data = data;
    model->file_size = size;

    serial_puts("[GGUF] Parsing in-memory buffer (");
    serial_putdec(size / (1024 * 1024));
    serial_puts(" MB)...\n");

    if (gguf_extract_hyperparams(model) < 0) {
        serial_puts("[GGUF] Metadata extraction failed\n");
        return -1;
    }

    serial_puts("[GGUF] \"");
    serial_puts(model->model_name[0] ? model->model_name : "unnamed");
    serial_puts("\" layers=");
    serial_putdec(model->num_layers);
    serial_puts(" dim=");
    serial_putdec(model->hidden_size);
    serial_puts(" vocab=");
    serial_putdec(model->vocab_size);
    serial_puts("\n");

    if (gguf_parse_buffer(model) < 0) {
        serial_puts("[GGUF] Tensor parse failed\n");
        return -1;
    }

    serial_puts("[GGUF] ");
    serial_putdec(model->num_tensors);
    serial_puts(" tensors, ");
    serial_putdec(model->tensor_data_size / (1024 * 1024));
    serial_puts(" MB tensor data\n");
    return 0;
}

/* ── Tensor DMA map — build LBA-to-tensor mapping for NVMe-direct ── */

int tensor_dma_build_map(gguf_model_t *model, tensor_dma_map_t *map)
{
    if (!model || !map || model->num_tensors == 0) return -1;

    extern uint64_t osfs2_file_byte_offset(osfs2_file_t *file);
    extern uint32_t nvme_get_lba_size(void);

    /* Find the GGUF file entry to get its byte offset on NVMe */
    osfs2_file_t *gguf_file = osfs2_find_gguf();
    if (!gguf_file) return -1;

    uint64_t file_abs_offset = osfs2_file_byte_offset(gguf_file);
    uint32_t lba_size = nvme_get_lba_size();
    if (lba_size == 0) return -1;

    map->file_lba_base = file_abs_offset / lba_size;
    map->tensor_data_offset = model->tensor_data_offset;
    map->num_entries = model->num_tensors;

    /* Allocate entry table */
    void *phys = mem_alloc_aligned(model->num_tensors * sizeof(tensor_dma_entry_t), 8);
    map->entries = (tensor_dma_entry_t *)((uintptr_t)phys + 0xFFFF800000000000ULL);
    if (!map->entries) return -1;

    for (uint32_t i = 0; i < model->num_tensors; i++) {
        gguf_tensor_t *t = &model->tensors[i];
        uint64_t byte_off = model->tensor_data_offset + t->offset;
        map->entries[i].lba = map->file_lba_base + byte_off / lba_size;
        map->entries[i].lba_count = (t->size + lba_size - 1) / lba_size;
        map->entries[i].size = t->size;
    }

    serial_puts("[GGUF] DMA map built: ");
    serial_putdec(map->num_entries);
    serial_puts(" tensors, file LBA base ");
    serial_putdec(map->file_lba_base);
    serial_puts("\n");
    return 0;
}
