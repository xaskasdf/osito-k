/*
 * OsitoFS v3 on-disk format.
 *
 * v3 uses hierarchical directories and extent-based file allocation.  The
 * 64 KiB block size is intentional: directory record lengths are uint16_t,
 * and this geometry also keeps small-file overhead practical for Win32 apps.
 */

#ifndef OSITOFS3_FORMAT_H
#define OSITOFS3_FORMAT_H

#ifdef __KERNEL_X86__
#include "types.h"
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define OSFS3_MAGIC              0x4F534633U /* "OSF3" */
#define OSFS3_VERSION            3U
#define OSFS3_BLOCK_SIZE         (64U * 1024U)
#define OSFS3_BLOCK_SHIFT        16U

#define OSFS3_SUPERBLOCK_BLK     0U
#define OSFS3_INODE_BITMAP_BLK   1U
#define OSFS3_BLOCK_BITMAP_BLK   2U
#define OSFS3_INODE_TABLE_BLK    3U

#define OSFS3_DEFAULT_INODES     16384U
#define OSFS3_MAX_INODES         131072U
#define OSFS3_MAX_EXTENTS        12U
#define OSFS3_NAME_MAX           255U
#define OSFS3_PATH_MAX           260U
#define OSFS3_BITMAP_BITS        (OSFS3_BLOCK_SIZE * 8U)

/* File types, matching the POSIX S_IFMT values. */
#define OSFS3_S_IFMT   0170000
#define OSFS3_S_IFSOCK 0140000
#define OSFS3_S_IFLNK  0120000
#define OSFS3_S_IFREG  0100000
#define OSFS3_S_IFBLK  0060000
#define OSFS3_S_IFDIR  0040000
#define OSFS3_S_IFCHR  0020000
#define OSFS3_S_IFIFO  0010000

/* Directory entry types, matching POSIX DT_* values. */
#define OSFS3_DT_UNKNOWN  0U
#define OSFS3_DT_FIFO     1U
#define OSFS3_DT_CHR      2U
#define OSFS3_DT_DIR      4U
#define OSFS3_DT_BLK      6U
#define OSFS3_DT_REG      8U
#define OSFS3_DT_LNK      10U
#define OSFS3_DT_SOCK     12U

/* Optional inode fields stored in space that was reserved in v3 images. */
#define OSFS3_INODE_FLAG_BTIME_VALID (1U << 0)
#define OSFS3_INODE_FLAG_DOS_HIDDEN  (1U << 1)
#define OSFS3_INODE_FLAG_DOS_SYSTEM  (1U << 2)
#define OSFS3_INODE_FLAG_DOS_NOARCH  (1U << 3)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t root_inode;
    uint8_t  uuid[16];
    char     label[32];
    uint64_t create_time;
    uint32_t crc32;
    uint8_t  reserved[512 - 96];
} osfs3_super_t;

_Static_assert(sizeof(osfs3_super_t) == 512,
               "OsitoFS v3 superblock must be 512 bytes");

typedef struct __attribute__((packed)) {
    uint32_t start_block;
    uint32_t block_count;
} osfs3_extent_t;

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint16_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t flags;
    uint32_t extent_count;
    osfs3_extent_t extents[OSFS3_MAX_EXTENTS];
    union {
        char symlink_target[64];
        uint8_t reserved_ext[64];
    };
    uint32_t crc32;
    uint64_t birth_time;             /* Unix epoch seconds (0 = ctime) */
    uint8_t  reserved[256 - 224];
} osfs3_inode_t;

_Static_assert(sizeof(osfs3_inode_t) == 256,
               "OsitoFS v3 inode must be 256 bytes");

#define OSFS3_INODES_PER_BLOCK \
    (OSFS3_BLOCK_SIZE / (uint32_t)sizeof(osfs3_inode_t))
#define OSFS3_INODE_TABLE_BLOCKS(inodes) \
    (((inodes) + OSFS3_INODES_PER_BLOCK - 1U) / OSFS3_INODES_PER_BLOCK)
#define OSFS3_FIRST_DATA_BLOCK(inodes) \
    (OSFS3_INODE_TABLE_BLK + OSFS3_INODE_TABLE_BLOCKS(inodes))

static inline uint32_t osfs3_inode_table_blocks(uint32_t total_inodes)
{
    return OSFS3_INODE_TABLE_BLOCKS(total_inodes);
}

static inline uint32_t osfs3_first_data_block_for_inodes(
    uint32_t total_inodes)
{
    return OSFS3_FIRST_DATA_BLOCK(total_inodes);
}

static inline uint32_t osfs3_max_inodes(void)
{
    return OSFS3_BITMAP_BITS;
}

static inline uint32_t osfs3_max_blocks(void)
{
    return OSFS3_BITMAP_BITS;
}

static inline int osfs3_valid_inode_count(uint32_t total_inodes)
{
    return total_inodes >= OSFS3_INODES_PER_BLOCK &&
           total_inodes <= osfs3_max_inodes() &&
           (total_inodes % OSFS3_INODES_PER_BLOCK) == 0;
}

/* Directories are files containing aligned, variable-length records. */
typedef struct __attribute__((packed)) {
    uint32_t inode;       /* 0 means a free/deleted record. */
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  type;
    char     name[];
} osfs3_dentry_t;

#define OSFS3_DIR_REC_LEN(name_len) \
    (((uint32_t)sizeof(osfs3_dentry_t) + (uint32_t)(name_len) + 3U) & ~3U)
#define OSFS3_DIR_BLOCK_BYTES (OSFS3_BLOCK_SIZE - 4U)

#define OSFS3_CRC32_POLY 0xEDB88320U

static inline uint32_t osfs3_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (OSFS3_CRC32_POLY & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

#endif /* OSITOFS3_FORMAT_H */
