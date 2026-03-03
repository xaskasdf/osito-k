/*
 * OsitoK x86-64 — GGUF Model Loader
 *
 * Loads GGUF files from OsitoFS into RAM and builds a tensor table
 * with direct pointers into the loaded data.
 *
 * Reference: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
 */

#ifndef OSITOK_GGUF_H
#define OSITOK_GGUF_H

#include "../include/types.h"

/* GGUF magic */
#define GGUF_MAGIC    0x46475547  /* "GGUF" */

/* GGUF value types (for skipping metadata) */
#define GGUF_TYPE_UINT8    0
#define GGUF_TYPE_INT8     1
#define GGUF_TYPE_UINT16   2
#define GGUF_TYPE_INT16    3
#define GGUF_TYPE_UINT32   4
#define GGUF_TYPE_INT32    5
#define GGUF_TYPE_FLOAT32  6
#define GGUF_TYPE_BOOL     7
#define GGUF_TYPE_STRING   8
#define GGUF_TYPE_ARRAY    9
#define GGUF_TYPE_UINT64   10
#define GGUF_TYPE_INT64    11
#define GGUF_TYPE_FLOAT64  12

/* GGML tensor types (quantization) */
#define GGML_TYPE_F32      0
#define GGML_TYPE_F16      1
#define GGML_TYPE_Q4_0     2
#define GGML_TYPE_Q4_1     3
#define GGML_TYPE_Q5_0     6
#define GGML_TYPE_Q5_1     7
#define GGML_TYPE_Q8_0     8
#define GGML_TYPE_Q8_1     9
#define GGML_TYPE_Q2_K     10
#define GGML_TYPE_Q3_K     11
#define GGML_TYPE_Q4_K     12
#define GGML_TYPE_Q5_K     13
#define GGML_TYPE_Q6_K     14

/* ── Tensor descriptor ──────────────────────────────────────── */

typedef struct {
    char     name[64];      /* e.g. "blk.0.attn_q.weight" */
    uint32_t type;          /* GGML_TYPE_* */
    uint32_t n_dims;
    uint64_t ne[4];         /* shape (elements per dimension) */
    uint64_t offset;        /* byte offset from tensor_data start */
    void    *data;          /* direct pointer into loaded buffer */
    uint64_t size;          /* total bytes for this tensor */
} gguf_tensor_t;

/* ── Loaded model ───────────────────────────────────────────── */

#define GGUF_MODEL_NAME_LEN  128

typedef struct gguf_model {
    /* Metadata (from OsitoFS file entry) */
    char     model_name[GGUF_MODEL_NAME_LEN];
    uint32_t quant_type;
    uint32_t num_layers;
    uint32_t hidden_size;
    uint32_t vocab_size;
    uint32_t head_count;
    uint32_t kv_head_count;
    uint32_t context_length;

    /* Raw file data in RAM */
    void    *file_data;         /* complete file buffer */
    uint64_t file_size;

    /* Tensor data region */
    void    *tensor_data;       /* = file_data + tensor_data_offset */
    uint64_t tensor_data_offset;
    uint64_t tensor_data_size;

    /* Tensor table */
    uint32_t      num_tensors;
    gguf_tensor_t *tensors;     /* allocated array */

    /* Layer offsets (from OsitoFS layer index) */
    uint32_t layer_count;
    uint64_t layer_offsets[255];
} gguf_model_t;

/*
 * gguf_load — Load first GGUF file from OsitoFS into RAM
 *
 * Allocates memory for file data + tensor table.
 * Returns 0 on success, -1 on error.
 */
int gguf_load(gguf_model_t *model);

/*
 * gguf_find_tensor — Find tensor by name
 *
 * Returns pointer to tensor descriptor, or NULL if not found.
 */
gguf_tensor_t *gguf_find_tensor(gguf_model_t *model, const char *name);

#endif /* OSITOK_GGUF_H */
