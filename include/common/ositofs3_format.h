/*
 * OsitoFS v3 — On-disk format definitions
 *
 * Designed for bare-metal x86-64 and POSIX compatibility.
 * Extent-based allocation, hierarchical directories, and symlinks.
 *
 * Block size: 1MB (optimal for NVMe DMA)
 */

#ifndef OSITOFS3_FORMAT_H
#define OSITOFS3_FORMAT_H

#ifdef __KERNEL_X86__
#include "types.h"
#else
#include <stdint.h>
#include <stddef.h>
#endif

/* ── Constants ───────────────────────────────────────────────── */

#define OSFS3_MAGIC           0x4F534633   /* "OSF3" */
#define OSFS3_VERSION         3
#define OSFS3_BLOCK_SIZE      (1024 * 1024) /* 1MB */
#define OSFS3_BLOCK_SHIFT     20

#define OSFS3_SUPERBLOCK_BLK  0
/* Block 1: Inode Bitmap */
/* Block 2: Block Bitmap */
/* Block 3..K: Inode Table */
/* Block K+1..N: Data Blocks */

#define OSFS3_INODES_PER_BLOCK (OSFS3_BLOCK_SIZE / sizeof(osfs3_inode_t))
#define OSFS3_MAX_EXTENTS      12  /* Direct extents per inode before indirection (future) */
#define OSFS3_NAME_MAX         255 /* Maximum length of a filename */

/* File types (matching POSIX S_IFMT) */
#define OSFS3_S_IFMT   0170000
#define OSFS3_S_IFSOCK 0140000
#define OSFS3_S_IFLNK  0120000
#define OSFS3_S_IFREG  0100000
#define OSFS3_S_IFBLK  0060000
#define OSFS3_S_IFDIR  0040000
#define OSFS3_S_IFCHR  0020000
#define OSFS3_S_IFIFO  0010000

/* Dentry types (matching POSIX DT_*) */
#define OSFS3_DT_UNKNOWN  0
#define OSFS3_DT_FIFO     1
#define OSFS3_DT_CHR      2
#define OSFS3_DT_DIR      4
#define OSFS3_DT_BLK      6
#define OSFS3_DT_REG      8
#define OSFS3_DT_LNK      10
#define OSFS3_DT_SOCK     12

/* ── Superblock (512 bytes, padded) ──────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;              /* OSFS3_MAGIC */
    uint32_t version;            /* OSFS3_VERSION */
    uint32_t block_size;         /* 1MB */
    uint32_t total_blocks;       /* Total blocks on device */
    uint32_t free_blocks;        /* Number of free data blocks */
    uint32_t total_inodes;       /* Total inodes available */
    uint32_t free_inodes;        /* Number of free inodes */
    uint32_t first_data_block;   /* Block number where data blocks start */
    uint32_t root_inode;         /* Inode number of the root directory (usually 1) */
    uint8_t  uuid[16];           /* Filesystem UUID */
    char     label[32];          /* Human-readable label */
    uint64_t create_time;        /* Unix timestamp */
    uint32_t crc32;              /* CRC32 of superblock (excluding this field) */
    uint8_t  reserved[512 - 96]; /* Pad to 512 bytes */
} osfs3_super_t;

_Static_assert(sizeof(osfs3_super_t) == 512, "superblock must be 512 bytes");

/* ── Extent ──────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t start_block;        /* Physical block number */
    uint32_t block_count;        /* Number of contiguous blocks */
} osfs3_extent_t;

/* ── Inode (256 bytes) ───────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t mode;               /* File type and permissions (S_IFMT) */
    uint16_t nlink;              /* Number of hard links */
    uint32_t uid;                /* User ID */
    uint32_t gid;                /* Group ID */
    uint64_t size;               /* Size in bytes */
    uint64_t atime;              /* Access time */
    uint64_t mtime;              /* Modification time */
    uint64_t ctime;              /* Creation/Status change time */
    uint32_t flags;              /* File flags (e.g., GGUF, RAW) */
    uint32_t extent_count;       /* Number of active extents */
    
    /* Direct extents. */
    osfs3_extent_t extents[OSFS3_MAX_EXTENTS]; 
    
    /* Symlink optimization: store short symlink targets directly in the inode */
    union {
        char symlink_target[64];
        uint8_t reserved_ext[64];
    };
    
    uint32_t crc32;              /* CRC32 of file data (optional/lazy) */
    uint8_t  reserved[256 - 216]; /* Pad to 256 bytes */
} osfs3_inode_t;

_Static_assert(sizeof(osfs3_inode_t) == 256, "inode must be 256 bytes");

/* ── Directory Entry (Variable length) ───────────────────────── */
/* Directories are just files (S_IFDIR) containing a sequence of these entries. */

typedef struct __attribute__((packed)) {
    uint32_t inode;              /* Inode number (0 = free/deleted entry) */
    uint16_t rec_len;            /* Record length (to find the next entry) */
    uint8_t  name_len;           /* Length of the filename */
    uint8_t  type;               /* File type (OSFS3_DT_*) */
    char     name[];             /* Filename (not null-terminated on disk to save space, but padded) */
} osfs3_dentry_t;

/* Helper macros */
#define OSFS3_DIR_REC_LEN(name_len) (((sizeof(osfs3_dentry_t) + (name_len) + 3) & ~3))

/* ── CRC32 polynomial ────────────────────────────────────────── */

#define OSFS3_CRC32_POLY 0xEDB88320

static inline uint32_t osfs3_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (OSFS3_CRC32_POLY & (-(crc & 1)));
    }
    return ~crc;
}

#endif /* OSITOFS3_FORMAT_H */
