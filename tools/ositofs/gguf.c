/*
 * GGUF file parser — extract model metadata for OsitoFS v2
 *
 * Reads GGUF header, metadata KV pairs, and tensor info to populate
 * gguf_model_info_t with architecture details and layer byte offsets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "common.h"
#include "gguf.h"

/* ── Read helpers ────────────────────────────────────────────── */

typedef struct {
    FILE *fp;
    uint64_t pos;
} gguf_reader_t;

static int rd_bytes(gguf_reader_t *r, void *buf, size_t n)
{
    if (fread(buf, 1, n, r->fp) != n) return -1;
    r->pos += n;
    return 0;
}

static int rd_u32(gguf_reader_t *r, uint32_t *val)
{
    return rd_bytes(r, val, 4);
}

static int rd_u64(gguf_reader_t *r, uint64_t *val)
{
    return rd_bytes(r, val, 8);
}

static int rd_string(gguf_reader_t *r, char *buf, size_t max_len, uint64_t *str_len)
{
    uint64_t len;
    if (rd_u64(r, &len) < 0) return -1;
    if (str_len) *str_len = len;

    if (len >= max_len) {
        /* Read and discard long string, keep truncated version */
        if (fread(buf, 1, max_len - 1, r->fp) != max_len - 1) return -1;
        buf[max_len - 1] = '\0';
        /* Skip remainder */
        if (fseek(r->fp, (long)(len - (max_len - 1)), SEEK_CUR) < 0) return -1;
        r->pos += len;
        return 0;
    }

    if (fread(buf, 1, (size_t)len, r->fp) != (size_t)len) return -1;
    buf[len] = '\0';
    r->pos += len;
    return 0;
}

/* Skip a GGUF value of given type */
static int skip_value(gguf_reader_t *r, uint32_t type)
{
    uint64_t skip = 0;

    switch (type) {
    case GGUF_TYPE_UINT8:
    case GGUF_TYPE_INT8:
    case GGUF_TYPE_BOOL:    skip = 1; break;
    case GGUF_TYPE_UINT16:
    case GGUF_TYPE_INT16:   skip = 2; break;
    case GGUF_TYPE_UINT32:
    case GGUF_TYPE_INT32:
    case GGUF_TYPE_FLOAT32: skip = 4; break;
    case GGUF_TYPE_UINT64:
    case GGUF_TYPE_INT64:
    case GGUF_TYPE_FLOAT64: skip = 8; break;
    case GGUF_TYPE_STRING: {
        uint64_t len;
        if (rd_u64(r, &len) < 0) return -1;
        skip = len;
        break;
    }
    case GGUF_TYPE_ARRAY: {
        uint32_t arr_type;
        uint64_t arr_len;
        if (rd_u32(r, &arr_type) < 0) return -1;
        if (rd_u64(r, &arr_len) < 0) return -1;
        for (uint64_t i = 0; i < arr_len; i++)
            if (skip_value(r, arr_type) < 0) return -1;
        return 0;
    }
    default:
        fprintf(stderr, "gguf: unknown value type %u\n", type);
        return -1;
    }

    if (fseek(r->fp, (long)skip, SEEK_CUR) < 0) return -1;
    r->pos += skip;
    return 0;
}

/* Read a uint32 value */
static int read_u32_value(gguf_reader_t *r, uint32_t type, uint32_t *val)
{
    if (type == GGUF_TYPE_UINT32 || type == GGUF_TYPE_INT32) {
        return rd_u32(r, val);
    } else if (type == GGUF_TYPE_UINT64 || type == GGUF_TYPE_INT64) {
        uint64_t v;
        if (rd_u64(r, &v) < 0) return -1;
        *val = (uint32_t)v;
        return 0;
    }
    return -1;
}

/* Map GGML type to OsitoFS quant type */
static uint32_t ggml_to_osfs2_quant(uint32_t ggml_type)
{
    switch (ggml_type) {
    case GGML_TYPE_F32:     return OSFS2_QUANT_F32;
    case GGML_TYPE_F16:     return OSFS2_QUANT_F16;
    case GGML_TYPE_Q4_0:    return OSFS2_QUANT_Q4_0;
    case GGML_TYPE_Q4_1:    return OSFS2_QUANT_Q4_1;
    case GGML_TYPE_Q5_0:    return OSFS2_QUANT_Q5_0;
    case GGML_TYPE_Q5_1:    return OSFS2_QUANT_Q5_1;
    case GGML_TYPE_Q8_0:    return OSFS2_QUANT_Q8_0;
    case GGML_TYPE_Q2_K:    return OSFS2_QUANT_Q2_K;
    case GGML_TYPE_Q3_K:    return OSFS2_QUANT_Q3_K;
    case GGML_TYPE_Q4_K:    return OSFS2_QUANT_Q4_K;
    case GGML_TYPE_Q5_K:    return OSFS2_QUANT_Q5_K;
    case GGML_TYPE_Q6_K:    return OSFS2_QUANT_Q6_K;
    case GGML_TYPE_IQ2_XXS: return OSFS2_QUANT_IQ2_XXS;
    case GGML_TYPE_IQ3_XXS: return OSFS2_QUANT_IQ3_XXS;
    default:                return OSFS2_QUANT_NONE;
    }
}

/* ── GGUF block size per type (bytes per group) ──────────────── */

static uint64_t ggml_type_block_size(uint32_t type)
{
    switch (type) {
    case GGML_TYPE_F32:     return 4;
    case GGML_TYPE_F16:     return 2;
    case GGML_TYPE_Q4_0:    return 18;   /* 32 vals: 2 + 16 */
    case GGML_TYPE_Q4_1:    return 20;   /* 32 vals: 4 + 16 */
    case GGML_TYPE_Q5_0:    return 22;   /* 32 vals: 2 + 4 + 16 */
    case GGML_TYPE_Q5_1:    return 24;   /* 32 vals: 4 + 4 + 16 */
    case GGML_TYPE_Q8_0:    return 34;   /* 32 vals: 2 + 32 */
    case GGML_TYPE_Q2_K:    return 84;   /* 256 vals */
    case GGML_TYPE_Q3_K:    return 110;  /* 256 vals */
    case GGML_TYPE_Q4_K:    return 144;  /* 256 vals */
    case GGML_TYPE_Q5_K:    return 176;  /* 256 vals */
    case GGML_TYPE_Q6_K:    return 210;  /* 256 vals */
    default:                return 0;
    }
}

static uint64_t ggml_type_group_size(uint32_t type)
{
    switch (type) {
    case GGML_TYPE_F32:
    case GGML_TYPE_F16:     return 1;
    case GGML_TYPE_Q4_0:
    case GGML_TYPE_Q4_1:
    case GGML_TYPE_Q5_0:
    case GGML_TYPE_Q5_1:
    case GGML_TYPE_Q8_0:    return 32;
    case GGML_TYPE_Q2_K:
    case GGML_TYPE_Q3_K:
    case GGML_TYPE_Q4_K:
    case GGML_TYPE_Q5_K:
    case GGML_TYPE_Q6_K:    return 256;
    default:                return 0;
    }
}

/* ── Main parser ─────────────────────────────────────────────── */

int gguf_parse(const char *path, gguf_model_info_t *info)
{
    memset(info, 0, sizeof(*info));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "gguf: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    gguf_reader_t r = { .fp = fp, .pos = 0 };

    /* Header: magic + version + tensor_count + metadata_kv_count */
    uint32_t magic, version;
    uint64_t tensor_count, kv_count;

    if (rd_u32(&r, &magic) < 0 || magic != GGUF_MAGIC) {
        fprintf(stderr, "gguf: bad magic in %s\n", path);
        goto fail;
    }
    if (rd_u32(&r, &version) < 0) goto fail;
    if (version < 2 || version > 3) {
        fprintf(stderr, "gguf: unsupported version %u\n", version);
        goto fail;
    }
    if (rd_u64(&r, &tensor_count) < 0) goto fail;
    if (rd_u64(&r, &kv_count) < 0) goto fail;

    printf("  GGUF v%u: %llu tensors, %llu metadata keys\n",
           version, (unsigned long long)tensor_count, (unsigned long long)kv_count);

    /* Parse metadata KV pairs */
    char key[256];
    char str_val[256];

    for (uint64_t i = 0; i < kv_count; i++) {
        uint64_t key_len;
        if (rd_string(&r, key, sizeof(key), &key_len) < 0) goto fail;

        uint32_t val_type;
        if (rd_u32(&r, &val_type) < 0) goto fail;

        /* Match keys we care about */
        if (strcmp(key, "general.name") == 0 && val_type == GGUF_TYPE_STRING) {
            rd_string(&r, info->model_name, OSFS2_MODEL_NAME_LEN, NULL);
        } else if (strcmp(key, "general.architecture") == 0 && val_type == GGUF_TYPE_STRING) {
            rd_string(&r, str_val, sizeof(str_val), NULL);
            /* Append to model_name if empty */
            if (info->model_name[0] == '\0')
                strncpy(info->model_name, str_val, OSFS2_MODEL_NAME_LEN - 1);
        } else if (strstr(key, ".block_count") && val_type >= GGUF_TYPE_UINT32) {
            read_u32_value(&r, val_type, &info->num_layers);
        } else if (strstr(key, ".embedding_length") && val_type >= GGUF_TYPE_UINT32) {
            read_u32_value(&r, val_type, &info->hidden_size);
        } else if (strstr(key, ".attention.head_count") && !strstr(key, "head_count_kv")) {
            read_u32_value(&r, val_type, &info->head_count);
        } else if (strstr(key, ".attention.head_count_kv")) {
            read_u32_value(&r, val_type, &info->kv_head_count);
        } else if (strstr(key, ".context_length")) {
            read_u32_value(&r, val_type, &info->context_length);
        } else if (strstr(key, ".vocab_size")) {
            read_u32_value(&r, val_type, &info->vocab_size);
        } else {
            /* Skip unrecognized value */
            if (skip_value(&r, val_type) < 0) goto fail;
        }
    }

    /* Parse tensor info to get offsets and determine quant type */
    uint32_t dominant_quant = 0;
    uint64_t dominant_quant_size = 0;

    /* Track per-layer byte sizes for building layer index */
    typedef struct { char name[128]; uint32_t type; uint64_t size; uint64_t offset; } tinfo_t;
    tinfo_t *tensors = calloc((size_t)tensor_count, sizeof(tinfo_t));
    if (!tensors) goto fail;

    for (uint64_t i = 0; i < tensor_count; i++) {
        /* Tensor name */
        if (rd_string(&r, tensors[i].name, sizeof(tensors[i].name), NULL) < 0) {
            free(tensors);
            goto fail;
        }

        /* n_dimensions */
        uint32_t n_dims;
        if (rd_u32(&r, &n_dims) < 0) { free(tensors); goto fail; }

        /* dimensions */
        uint64_t total_elements = 1;
        for (uint32_t d = 0; d < n_dims; d++) {
            uint64_t dim;
            if (rd_u64(&r, &dim) < 0) { free(tensors); goto fail; }
            total_elements *= dim;
        }

        /* type */
        uint32_t ttype;
        if (rd_u32(&r, &ttype) < 0) { free(tensors); goto fail; }
        tensors[i].type = ttype;

        /* offset */
        uint64_t toffset;
        if (rd_u64(&r, &toffset) < 0) { free(tensors); goto fail; }
        tensors[i].offset = toffset;

        /* Calculate byte size */
        uint64_t bs = ggml_type_block_size(ttype);
        uint64_t gs = ggml_type_group_size(ttype);
        if (bs > 0 && gs > 0) {
            tensors[i].size = (total_elements / gs) * bs;
        } else {
            tensors[i].size = total_elements * 4; /* fallback: assume f32 */
        }

        /* Track dominant quant by total bytes */
        uint32_t q = ggml_to_osfs2_quant(ttype);
        if (q != OSFS2_QUANT_NONE && tensors[i].size > dominant_quant_size) {
            dominant_quant = q;
            dominant_quant_size = tensors[i].size;
        }
    }

    info->quant_type = dominant_quant;

    /* Tensor data starts after header+metadata+tensor_info, aligned to block_size */
    /* The alignment is determined by the GGUF spec — align to 32 bytes */
    uint64_t header_end = r.pos;
    uint64_t alignment = 32;
    info->tensor_data_offset = (header_end + alignment - 1) & ~(alignment - 1);

    /* Calculate total tensor data size */
    uint64_t max_end = 0;
    for (uint64_t i = 0; i < tensor_count; i++) {
        uint64_t end = tensors[i].offset + tensors[i].size;
        if (end > max_end) max_end = end;
    }
    info->tensor_data_size = max_end;

    /* Build layer index: find tensors matching "blk.N." pattern */
    if (info->num_layers > 0 && info->num_layers <= OSFS2_MAX_LAYERS) {
        info->layer_count = info->num_layers;

        /* For each layer, find the minimum tensor offset */
        for (uint32_t l = 0; l < info->num_layers; l++)
            info->layer_offsets[l] = UINT64_MAX;

        char prefix[32];
        for (uint64_t i = 0; i < tensor_count; i++) {
            /* Try "blk.N." pattern */
            for (uint32_t l = 0; l < info->num_layers; l++) {
                snprintf(prefix, sizeof(prefix), "blk.%u.", l);
                if (strncmp(tensors[i].name, prefix, strlen(prefix)) == 0) {
                    if (tensors[i].offset < info->layer_offsets[l])
                        info->layer_offsets[l] = tensors[i].offset;
                    break;
                }
            }
        }

        /* Replace UINT64_MAX with 0 for layers with no matching tensors */
        for (uint32_t l = 0; l < info->num_layers; l++) {
            if (info->layer_offsets[l] == UINT64_MAX)
                info->layer_offsets[l] = 0;
        }
    }

    free(tensors);
    fclose(fp);

    printf("  Model: %s\n", info->model_name);
    printf("  Quant: %s, Layers: %u, Hidden: %u, Vocab: %u\n",
           osfs2_quant_name(info->quant_type), info->num_layers,
           info->hidden_size, info->vocab_size);
    printf("  Heads: %u (KV: %u), Context: %u\n",
           info->head_count, info->kv_head_count, info->context_length);
    printf("  Tensor data: offset 0x%llx, size ",
           (unsigned long long)info->tensor_data_offset);
    osfs2_print_size(info->tensor_data_size);
    printf("\n");

    return 0;

fail:
    fclose(fp);
    return -1;
}
