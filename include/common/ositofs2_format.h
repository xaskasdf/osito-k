/*
 * OsitoFS v2 — On-disk format definitions
 *
 * Shared between host tools (Linux) and bare-metal kernel (x86-64).
 * Freestanding-compatible: no libc dependencies.
 *
 * Block size: 1MB (optimal for NVMe DMA)
 * Layout:
 *   Block 0: Superblock
 *   Block 1: File Table (4096 entries × 256 bytes)
 *   Block 2: Block CRC Table (262144 × uint32)
 *   Block 3: Layer Index Table (512 slots × 2048 bytes)
 *   Block 4..N: Data blocks
 */

#ifndef OSITOFS2_FORMAT_H
#define OSITOFS2_FORMAT_H

#ifdef __KERNEL_X86__
#include "types.h"
#else
#include <stdint.h>
#include <stddef.h>
#endif

/* ── Constants ───────────────────────────────────────────────── */

#define OSFS2_MAGIC           0x4F534632   /* "OSF2" */
#define OSFS2_VERSION         2
#define OSFS2_BLOCK_SIZE      (1024 * 1024) /* 1MB */
#define OSFS2_BLOCK_SHIFT     20

#define OSFS2_SUPERBLOCK_BLK  0
#define OSFS2_FILETAB_BLK     1
#define OSFS2_CRCTAB_BLK      2
#define OSFS2_LAYERIDX_BLK    3
#define OSFS2_DATA_START_BLK  4

#define OSFS2_MAX_FILES       4096
#define OSFS2_MAX_BLOCKS      262144   /* CRC slots = 1MB / 4 */
#define OSFS2_MAX_MODELS      512
#define OSFS2_MAX_LAYERS      255

#define OSFS2_NAME_LEN        64
#define OSFS2_MODEL_NAME_LEN  128
#define OSFS2_LABEL_LEN       32

/* File flags */
#define OSFS2_FLAG_VALID      (1 << 0)
#define OSFS2_FLAG_GGUF       (1 << 1)
#define OSFS2_FLAG_RAW        (1 << 2)

/* GGUF quantization types (subset) */
#define OSFS2_QUANT_NONE      0
#define OSFS2_QUANT_F32       1
#define OSFS2_QUANT_F16       2
#define OSFS2_QUANT_Q8_0      3
#define OSFS2_QUANT_Q4_0      4
#define OSFS2_QUANT_Q4_1      5
#define OSFS2_QUANT_Q5_0      6
#define OSFS2_QUANT_Q5_1      7
#define OSFS2_QUANT_Q2_K      8
#define OSFS2_QUANT_Q3_K      9
#define OSFS2_QUANT_Q4_K      10
#define OSFS2_QUANT_Q5_K      11
#define OSFS2_QUANT_Q6_K      12
#define OSFS2_QUANT_IQ2_XXS   13
#define OSFS2_QUANT_IQ3_XXS   14

/* ── Superblock (512 bytes, padded to fill block 0) ──────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* OSFS2_MAGIC */
    uint32_t version;            /* OSFS2_VERSION */
    uint32_t block_size;         /* 1MB */
    uint32_t total_blocks;       /* Total blocks on device */
    uint32_t used_blocks;        /* Blocks in use (metadata + data) */
    uint32_t file_count;         /* Number of valid files */
    uint32_t next_data_block;    /* Next free data block (first-fit) */
    uint8_t  uuid[16];          /* Filesystem UUID */
    char     label[OSFS2_LABEL_LEN]; /* Human-readable label */
    uint64_t create_time;        /* Unix timestamp */
    uint32_t crc32;              /* CRC32 of superblock (excluding this field) */
    uint8_t  reserved[512 - 88]; /* Pad to 512 bytes */
} osfs2_super_t;

_Static_assert(sizeof(osfs2_super_t) == 512, "superblock must be 512 bytes");

/* ── File Table Entry (256 bytes each, 4096 entries = 1MB) ───── */

typedef struct __attribute__((packed)) {
    char     name[OSFS2_NAME_LEN];   /* Filename (null-terminated) */
    uint64_t size;                    /* File size in bytes */
    uint32_t start_block;            /* First data block */
    uint32_t block_count;            /* Number of contiguous blocks */
    uint32_t crc32;                  /* CRC32 of entire file data */
    uint32_t flags;                  /* OSFS2_FLAG_* */

    /* GGUF model metadata (populated if FLAG_GGUF set) */
    uint32_t quant_type;             /* OSFS2_QUANT_* */
    uint32_t num_layers;             /* Number of transformer layers */
    uint32_t hidden_size;            /* Hidden dimension */
    uint32_t vocab_size;             /* Vocabulary size */
    uint32_t head_count;             /* Attention heads */
    uint32_t kv_head_count;          /* KV heads (GQA) */
    uint32_t context_length;         /* Max context length */
    char     model_name[OSFS2_MODEL_NAME_LEN]; /* e.g. "llama-7b-q4_0" */

    uint16_t layer_index_slot;       /* Slot in Layer Index Table (0xFFFF = none) */
    uint8_t  reserved[256 - 246];    /* Pad to 256 bytes */
} osfs2_file_t;

_Static_assert(sizeof(osfs2_file_t) == 256, "file entry must be 256 bytes");

/* ── Block CRC Table (1MB = 262144 × uint32) ────────────────── */
/* One CRC32 per data block. Stored as flat array of uint32_t.   */

/* ── Layer Index Entry (2048 bytes, 512 slots = 1MB) ─────────── */

typedef struct __attribute__((packed)) {
    uint32_t num_layers;             /* Actual number of layers */
    uint32_t reserved;
    uint64_t layer_offset[OSFS2_MAX_LAYERS]; /* Byte offset from file start to each layer */
    uint8_t  pad[2048 - 8 - (OSFS2_MAX_LAYERS * 8)];
} osfs2_layer_idx_t;

_Static_assert(sizeof(osfs2_layer_idx_t) == 2048, "layer index must be 2048 bytes");

/* ── CRC32 polynomial ────────────────────────────────────────── */

#define OSFS2_CRC32_POLY 0xEDB88320

static inline uint32_t osfs2_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (OSFS2_CRC32_POLY & (-(crc & 1)));
    }
    return ~crc;
}

#endif /* OSITOFS2_FORMAT_H */
