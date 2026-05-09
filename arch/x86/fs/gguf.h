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
#define GGUF_MAGIC    0x46554747  /* "GGUF" — bytes 0x47,0x47,0x55,0x46 LE */

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
#define GGUF_ARCH_LEN        32

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
    float    rope_freq_base;     /* RoPE theta (default 10000) */
    uint32_t feed_forward_length; /* FFN hidden size (brandon.feed_forward_length) */

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

    /* ── Architecture tag + brandon-arch metadata ────────────────
     * `architecture` is the canonical tag from `general.architecture`
     * ("llama" or "brandon"). When "brandon", the brandon_* fields
     * are populated from `brandon.*` GGUF keys; the layer_map is
     * allocated by gguf_load_from_mem and indexes into 0..num_layers-1.
     * See ~/osito-a-models/docs/brandon-arch-spec.md.                */
    char     architecture[GGUF_ARCH_LEN];
    uint32_t  brandon_compute_layer_count;   /* logical layers, ≥ num_layers */
    uint32_t *brandon_layer_map;             /* [compute_layer_count] → block id */
    bool      brandon_use_dwa;
    bool      brandon_use_value_residual;
    uint32_t  brandon_n_registers;
    uint32_t  brandon_n_loops;
    bool      brandon_weight_tying;
    uint32_t  rope_dim_count;                /* head_dim by default */
    float     attn_layer_norm_rms_eps;       /* RMSNorm epsilon */
} gguf_model_t;

/* ── Tensor DMA map for NVMe-direct streaming ──────────────── */

typedef struct {
    uint64_t lba;           /* Starting LBA on NVMe */
    uint32_t lba_count;     /* Number of LBAs to read */
    uint64_t size;          /* Exact byte size */
} tensor_dma_entry_t;

typedef struct {
    tensor_dma_entry_t *entries;  /* [num_tensors] */
    uint32_t num_entries;
    uint64_t file_lba_base;       /* LBA of file start on NVMe */
    uint64_t tensor_data_offset;  /* Byte offset of tensor data in file */
} tensor_dma_map_t;

int tensor_dma_build_map(gguf_model_t *model, tensor_dma_map_t *map);

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

/* ── Tokenizer data extracted from GGUF metadata ──────────── */

#define GGUF_TOK_MODEL_LEN  16
#define GGUF_TOK_MODEL_GPT2  "gpt2"
#define GGUF_TOK_MODEL_LLAMA "llama"  /* SPM (SentencePiece) */

typedef struct {
    /* Tokenizer model identifier (tokenizer.ggml.model). "llama" = SPM,
     * "gpt2" = byte-level BPE. Empty if unset. */
    char tok_model[GGUF_TOK_MODEL_LEN];

    /* Array of token strings (pointers into token_data buffer) */
    const char **tokens;
    uint32_t    *token_lens;
    uint32_t     n_tokens;

    /* Array of merge rule strings (pointers into merge_data buffer) */
    const char **merges;
    uint32_t    *merge_lens;
    uint32_t     n_merges;

    /* SPM aux: per-token scores + type codes. NULL for BPE models.
     * Both arrays are length n_tokens when present. Zero-copy pointers
     * into the GGUF file buffer. */
    float    *scores;
    uint32_t *token_types;

    /* Special token IDs (UINT32_MAX = unset) */
    uint32_t bos_id;
    uint32_t eos_id;
    uint32_t unk_id;
    uint32_t pad_id;

    /* Backing buffers (caller must free) */
    void *token_data;       /* raw strings for tokens */
    void *merge_data;       /* raw strings for merges */

    bool valid;
} gguf_tokenizer_t;

/*
 * gguf_load_tokenizer — Extract tokenizer from GGUF metadata.
 *
 * Must be called after gguf_load (model->file_data must be valid).
 * Allocates memory for token/merge arrays.
 * Returns 0 on success, -1 if no tokenizer found.
 */
int gguf_load_tokenizer(gguf_model_t *model, gguf_tokenizer_t *tok);

/*
 * gguf_dequant_f16_to_f32 — Convert all F16 tensors to F32 in place.
 *
 * For each tensor with type==GGML_TYPE_F16 in model->tensors[]:
 *   - Allocate a new f32 buffer of 2x the size
 *   - Decode every element via the kernel's f16_to_f32 helper
 *   - Repoint tensor->data, update type=F32 and size accordingly
 *
 * Memory cost: 2x the F16 tensor footprint. CPU cost: one-time at
 * model load, scalar; for brandon-tiny-10m (~20MB f16) this is
 * ~50-100 ms. After this runs, matvec hits the (auto-vectorizable)
 * F32 path on every layer instead of the scalar f16-dequant-on-the-
 * fly slow path.
 *
 * Returns 0 on success, -1 if any allocation fails.
 */
int gguf_dequant_f16_to_f32(gguf_model_t *model);

/*
 * gguf_free_tokenizer — Free tokenizer data.
 */
void gguf_free_tokenizer(gguf_tokenizer_t *tok);

#endif /* OSITOK_GGUF_H */
