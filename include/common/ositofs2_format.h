/*
 * OsitoFS v2 — On-disk format definitions
 *
 * Shared between host tools (Linux) and bare-metal kernel (x86-64).
 * Freestanding-compatible: no libc dependencies.
 *
 * Block size: configurable (default 1MB, min 64KB, stored in superblock)
 * Standard metadata layout (version 2, fixed at 4MB):
 *   Offset 0:    Primary superblock (512 bytes)
 *   Offset 4KB:  Backup superblock (512 bytes)
 *   Offset 8KB:  Metadata redo record (12KB)
 *   Offset 20KB: Journal commit marker (512 bytes)
 *   Offset 1MB:  File Table (4096 entries × 256 bytes = 1MB)
 *   Offset 2MB:  Block CRC Table (262144 × uint32 = 1MB)
 *   Offset 3MB:  Layer Index Table (512 slots × 2048 bytes = 1MB)
 *   Offset 4MB+: Data blocks (block_size from superblock)
 * Many-file layout (version 3): file table at 1-5MB, data at 5MB+;
 * the block CRC and layer index tables are omitted.
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

#define OSFS2_MAGIC              0x4F534632      /* "OSF2" */
#define OSFS2_VERSION            2
#define OSFS2_VERSION_LARGE_FILES 3

/* Data block size — configurable per-filesystem, stored in superblock */
#define OSFS2_DEFAULT_BLOCK_SIZE (1024 * 1024)    /* 1MB */
#define OSFS2_MIN_BLOCK_SIZE     (64 * 1024)      /* 64KB */
#define OSFS2_MAX_BLOCK_SIZE     (1024 * 1024)    /* 1MB */

/* Superblock backup (4K-aligned, within first 1MB region) */
#define OSFS2_SUPER_BACKUP_OFF   4096

/* Power-fail recovery journal. Older v2 readers ignore this previously unused
 * region, so clean existing images remain format-compatible. */
#define OSFS2_JOURNAL_RECORD_OFF 8192
#define OSFS2_JOURNAL_RECORD_SIZE 12288
#define OSFS2_JOURNAL_COMMIT_OFF (OSFS2_JOURNAL_RECORD_OFF + OSFS2_JOURNAL_RECORD_SIZE)
#define OSFS2_JOURNAL_COMMIT_SIZE 512
#define OSFS2_JOURNAL_MAGIC      0x4A324653  /* "SF2J" */
#define OSFS2_JOURNAL_VERSION    2
#define OSFS2_JOURNAL_COMMIT_MAGIC 0x43324A53 /* "SJ2C" */
#define OSFS2_JOURNAL_MAX_ENTRIES 2
#define OSFS2_JOURNAL_MAX_PAGES   2
#define OSFS2_METADATA_PAGE_SIZE  4096

#define OSFS2_JOURNAL_OP_RENAME  1
#define OSFS2_JOURNAL_OP_DELETE  2
#define OSFS2_JOURNAL_OP_REPLACE 3

/* Fixed metadata byte offsets (4MB total, independent of data block size) */
#define OSFS2_FILETAB_OFF        (1 * 1024 * 1024)
#define OSFS2_CRCTAB_OFF_STANDARD (2 * 1024 * 1024)
#define OSFS2_LAYERIDX_OFF_STANDARD (3 * 1024 * 1024)
#define OSFS2_CRCTAB_OFF         OSFS2_CRCTAB_OFF_STANDARD
#define OSFS2_LAYERIDX_OFF       OSFS2_LAYERIDX_OFF_STANDARD
#define OSFS2_DATA_OFF_STANDARD  (4 * 1024 * 1024)
#define OSFS2_DATA_OFF_LARGE     (5 * 1024 * 1024)
#define OSFS2_DATA_OFF           OSFS2_DATA_OFF_STANDARD

#define OSFS2_MAX_FILES_STANDARD 4096
#define OSFS2_MAX_FILES       OSFS2_MAX_FILES_STANDARD
#define OSFS2_MAX_FILES_LARGE 16384
#define OSFS2_MAX_BLOCKS      262144   /* CRC slots (CRCTAB_SIZE / 4) */
#define OSFS2_MAX_MODELS      512
#define OSFS2_MAX_LAYERS      255

/* Metadata region sizes (derived from MAX_* above) */
#define OSFS2_FILETAB_SIZE_STANDARD (OSFS2_MAX_FILES_STANDARD * 256) /* 1MB */
#define OSFS2_FILETAB_SIZE       OSFS2_FILETAB_SIZE_STANDARD
#define OSFS2_FILETAB_SIZE_LARGE (OSFS2_MAX_FILES_LARGE * 256) /* 4MB */
#define OSFS2_CRCTAB_SIZE        (OSFS2_MAX_BLOCKS * 4)      /* 1MB */
#define OSFS2_LAYERIDX_SIZE      (OSFS2_MAX_MODELS * 2048)   /* 1MB */

/* Version 3 dedicates offsets 1-5MB to file entries. Per-block CRCs and the
 * model layer index are omitted; file-level CRCs remain in each entry. */

static inline int osfs2_supported_version(uint32_t version) {
    return version == OSFS2_VERSION ||
           version == OSFS2_VERSION_LARGE_FILES;
}

static inline uint32_t osfs2_format_max_files(uint32_t version) {
    return version == OSFS2_VERSION_LARGE_FILES
        ? OSFS2_MAX_FILES_LARGE : OSFS2_MAX_FILES;
}

static inline uint32_t osfs2_format_filetab_size(uint32_t version) {
    return version == OSFS2_VERSION_LARGE_FILES
        ? OSFS2_FILETAB_SIZE_LARGE : OSFS2_FILETAB_SIZE;
}

static inline uint32_t osfs2_format_crctab_off(uint32_t version) {
    (void)version;
    return OSFS2_CRCTAB_OFF;
}

static inline int osfs2_format_has_crc_table(uint32_t version) {
    return version == OSFS2_VERSION;
}

static inline int osfs2_format_has_layer_index(uint32_t version) {
    return version == OSFS2_VERSION;
}

static inline uint32_t osfs2_format_data_off(uint32_t version) {
    return version == OSFS2_VERSION_LARGE_FILES
        ? OSFS2_DATA_OFF_LARGE : OSFS2_DATA_OFF_STANDARD;
}

/* Streaming I/O chunk (GGUF/GSP readers — not tied to FS block size) */
#define OSFS2_IO_CHUNK           (1024 * 1024)    /* 1MB */

#define OSFS2_NAME_LEN        64
#define OSFS2_MODEL_NAME_LEN  128
#define OSFS2_LABEL_LEN       32

/* File flags */
#define OSFS2_FLAG_VALID      (1 << 0)
#define OSFS2_FLAG_GGUF       (1 << 1)
#define OSFS2_FLAG_RAW        (1 << 2)
#define OSFS2_FLAG_INLINE     (1 << 3)  /* data stored inline in model_name[128] */
#define OSFS2_FLAG_LONG_NAME  (1 << 4)  /* full name stored in model_name[128] */
#define OSFS2_INLINE_MAX      128       /* max inline bytes */

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
    uint32_t block_size;         /* Data block size (from mkfs) */
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
    uint32_t create_time;            /* Unix epoch seconds (0 = unknown) */
    uint32_t modify_time;            /* Unix epoch seconds (0 = unknown) */
    uint8_t  reserved[256 - 254];    /* Pad to 256 bytes */
} osfs2_file_t;

_Static_assert(sizeof(osfs2_file_t) == 256, "file entry must be 256 bytes");

/* ── Metadata redo journal ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t operation;
    uint32_t entry_count;
    uint64_t transaction_id;
    uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES];
    uint32_t page_count;
    uint32_t pages[OSFS2_JOURNAL_MAX_PAGES];
    uint32_t reserved_header;
    osfs2_super_t before_super;
    osfs2_super_t after_super;
    osfs2_file_t before_entries[OSFS2_JOURNAL_MAX_ENTRIES];
    uint8_t after_pages[OSFS2_JOURNAL_MAX_PAGES][OSFS2_METADATA_PAGE_SIZE];
    uint32_t record_crc32;
    uint8_t reserved[OSFS2_JOURNAL_RECORD_SIZE - 9780];
} osfs2_journal_record_t;

_Static_assert(sizeof(osfs2_journal_record_t) == OSFS2_JOURNAL_RECORD_SIZE,
               "journal record must be 4096 bytes");

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t transaction_id;
    uint32_t record_crc32;
    uint32_t commit_crc32;
    uint8_t reserved[OSFS2_JOURNAL_COMMIT_SIZE - 24];
} osfs2_journal_commit_t;

_Static_assert(sizeof(osfs2_journal_commit_t) == OSFS2_JOURNAL_COMMIT_SIZE,
               "journal commit must be 512 bytes");

_Static_assert(OSFS2_JOURNAL_COMMIT_OFF + OSFS2_JOURNAL_COMMIT_SIZE <=
               OSFS2_FILETAB_OFF, "journal overlaps file table");

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

/* ── Block size helpers ──────────────────────────────────────── */

/* Compute log2(block_size) — block_size must be a power of 2 */
static inline uint32_t osfs2_block_shift(uint32_t block_size) {
    uint32_t shift = 0;
    while ((1u << shift) < block_size) shift++;
    return shift;
}

/* First data block number for a given block_size */
static inline uint32_t osfs2_data_start_blk(uint32_t block_size) {
    return OSFS2_DATA_OFF / block_size;
}

static inline uint32_t osfs2_format_data_start_blk(uint32_t version,
                                                    uint32_t block_size) {
    return osfs2_format_data_off(version) / block_size;
}

/* Validate block_size: power of 2 in [MIN, MAX] */
static inline int osfs2_valid_block_size(uint32_t bs) {
    return bs >= OSFS2_MIN_BLOCK_SIZE &&
           bs <= OSFS2_MAX_BLOCK_SIZE &&
           (bs & (bs - 1)) == 0;
}

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
