/*
 * GGUF file parser — extract model metadata
 *
 * Parses GGUF header + metadata key-value pairs to extract:
 *   - Architecture, quantization type, layer count
 *   - Hidden size, vocab size, attention heads, context length
 *   - Tensor offsets for layer index generation
 *
 * Reference: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
 */

#ifndef OSITOFS_GGUF_H
#define OSITOFS_GGUF_H

#include <stdint.h>

#include "../../include/common/ositofs2_format.h"

/* GGUF magic and version */
#define GGUF_MAGIC    0x46475547  /* "GGUF" */
#define GGUF_VERSION  3

/* GGUF value types */
enum gguf_type {
    GGUF_TYPE_UINT8   = 0,
    GGUF_TYPE_INT8    = 1,
    GGUF_TYPE_UINT16  = 2,
    GGUF_TYPE_INT16   = 3,
    GGUF_TYPE_UINT32  = 4,
    GGUF_TYPE_INT32   = 5,
    GGUF_TYPE_FLOAT32 = 6,
    GGUF_TYPE_BOOL    = 7,
    GGUF_TYPE_STRING  = 8,
    GGUF_TYPE_ARRAY   = 9,
    GGUF_TYPE_UINT64  = 10,
    GGUF_TYPE_INT64   = 11,
    GGUF_TYPE_FLOAT64 = 12,
};

/* GGUF tensor types (quantization) */
enum ggml_type {
    GGML_TYPE_F32     = 0,
    GGML_TYPE_F16     = 1,
    GGML_TYPE_Q4_0    = 2,
    GGML_TYPE_Q4_1    = 3,
    GGML_TYPE_Q5_0    = 6,
    GGML_TYPE_Q5_1    = 7,
    GGML_TYPE_Q8_0    = 8,
    GGML_TYPE_Q8_1    = 9,
    GGML_TYPE_Q2_K    = 10,
    GGML_TYPE_Q3_K    = 11,
    GGML_TYPE_Q4_K    = 12,
    GGML_TYPE_Q5_K    = 13,
    GGML_TYPE_Q6_K    = 14,
    GGML_TYPE_IQ2_XXS = 16,
    GGML_TYPE_IQ3_XXS = 18,
};

/* Parsed model info */
typedef struct {
    char     model_name[OSFS2_MODEL_NAME_LEN];
    uint32_t quant_type;       /* OSFS2_QUANT_* */
    uint32_t num_layers;
    uint32_t hidden_size;
    uint32_t vocab_size;
    uint32_t head_count;
    uint32_t kv_head_count;
    uint32_t context_length;

    /* Tensor data offset (from file start) and total tensor size */
    uint64_t tensor_data_offset;
    uint64_t tensor_data_size;

    /* Layer offsets (byte offset from tensor_data_offset) */
    uint32_t layer_count;
    uint64_t layer_offsets[OSFS2_MAX_LAYERS];
} gguf_model_info_t;

/*
 * Parse GGUF file and extract model metadata.
 * Returns 0 on success, -1 on error.
 * fd must be a regular file (not O_DIRECT).
 */
int gguf_parse(const char *path, gguf_model_info_t *info);

#endif /* OSITOFS_GGUF_H */
