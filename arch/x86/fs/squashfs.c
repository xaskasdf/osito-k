/*
 * OsitoK x86-64 — SquashFS Read-Only Driver
 *
 * Reads files from SquashFS images (Linux live CDs, snap packages, embedded).
 * Supports zlib-compressed blocks, inode/directory tables.
 * Minimal: no xattr, no fragments table, no UID/GID table.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* zlib decompression (from kernel/zlib.c) */
extern int zlib_inflate(const uint8_t *src, uint32_t src_len,
                        uint8_t *dst, uint32_t *out_len);

/* Block device read */
static int (*sqfs_read)(uint64_t offset, void *buf, uint64_t len);

/* ── SquashFS On-Disk Structures ─────────────────────────────── */

#define SQFS_MAGIC       0x73717368  /* "hsqs" LE */
#define SQFS_COMP_ZLIB   1
#define SQFS_COMP_LZ4    5
#define SQFS_COMP_ZSTD   6

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t modification_time;
    uint32_t block_size;        /* Uncompressed block size (usually 128KB) */
    uint32_t fragment_count;
    uint16_t compression;       /* 1=zlib, 5=lz4, 6=zstd */
    uint16_t block_log;         /* log2(block_size) */
    uint16_t flags;
    uint16_t id_count;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t root_inode;        /* Reference to root directory inode */
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t xattr_table_start;
    uint64_t inode_table_start;
    uint64_t dir_table_start;
    uint64_t fragment_table_start;
    uint64_t export_table_start;
} sqfs_super_t;

/* Inode types */
#define SQFS_DIR_TYPE    1
#define SQFS_REG_TYPE    2
#define SQFS_LDIR_TYPE   8   /* Extended directory */
#define SQFS_LREG_TYPE   9   /* Extended regular file */

/* Common inode header */
typedef struct __attribute__((packed)) {
    uint16_t inode_type;
    uint16_t permissions;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
} sqfs_inode_hdr_t;

/* Regular file inode (type 2) */
typedef struct __attribute__((packed)) {
    sqfs_inode_hdr_t hdr;
    uint32_t start_block;        /* Block offset of first data block */
    uint32_t fragment;
    uint32_t offset;             /* Fragment offset */
    uint32_t file_size;
    /* Followed by block_size_list[] */
} sqfs_reg_inode_t;

/* Directory inode (type 1) */
typedef struct __attribute__((packed)) {
    sqfs_inode_hdr_t hdr;
    uint32_t start_block;        /* Block offset in directory table */
    uint32_t hard_link_count;
    uint16_t file_size;          /* Total uncompressed dir entry size + 3 */
    uint16_t offset;             /* Offset within the uncompressed block */
    uint32_t parent_inode;
} sqfs_dir_inode_t;

/* Directory header (in directory table) */
typedef struct __attribute__((packed)) {
    uint32_t count;              /* entries - 1 in this header */
    uint32_t start;              /* inode table block offset */
    uint32_t inode_number;       /* Base inode number */
} sqfs_dir_header_t;

/* Directory entry (follows header) */
typedef struct __attribute__((packed)) {
    uint16_t offset;             /* Offset within inode table block */
    int16_t  inode_offset;       /* Delta from base inode number */
    uint16_t type;               /* SQFS_DIR_TYPE, SQFS_REG_TYPE, etc. */
    uint16_t name_size;          /* name length - 1 */
    char     name[];
} sqfs_dir_entry_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t image_offset;       /* Byte offset of image on device */
    sqfs_super_t sb;
} sqfs;

/* ── Metadata Block Reading ──────────────────────────────────── */

/* SquashFS metadata blocks: 2-byte header + compressed/uncompressed data.
 * Header bit 15 = uncompressed flag, bits 0-14 = data length. */
static int sqfs_read_metadata(uint64_t offset, uint8_t *out, uint32_t *out_len,
                              uint64_t *next_offset)
{
    uint8_t hdr[2];
    if (sqfs_read(sqfs.image_offset + offset, hdr, 2) < 0) return -1;

    uint16_t header = hdr[0] | ((uint16_t)hdr[1] << 8);
    bool uncompressed = (header & 0x8000) != 0;
    uint16_t data_len = header & 0x7FFF;

    if (data_len == 0 || data_len > 8192) return -1;

    uint8_t *compressed = (uint8_t *)kmalloc(data_len);
    if (!compressed) return -1;

    if (sqfs_read(sqfs.image_offset + offset + 2, compressed, data_len) < 0) {
        kfree(compressed);
        return -1;
    }

    if (uncompressed) {
        memcpy(out, compressed, data_len);
        *out_len = data_len;
    } else {
        /* Decompress with zlib */
        *out_len = 8192;
        if (zlib_inflate(compressed, data_len, out, out_len) < 0) {
            kfree(compressed);
            return -1;
        }
    }

    kfree(compressed);
    if (next_offset) *next_offset = offset + 2 + data_len;
    return 0;
}

/* ── Directory Reading ───────────────────────────────────────── */

typedef void (*sqfs_dir_cb)(const char *name, uint16_t type,
                            uint32_t inode_num, void *ctx);

static void sqfs_read_dir(uint32_t dir_block, uint16_t dir_offset,
                          uint16_t dir_size, sqfs_dir_cb cb, void *ctx)
{
    /* Read the metadata block containing directory entries */
    uint64_t meta_offset = sqfs.sb.dir_table_start + dir_block;
    uint8_t meta_buf[8192];
    uint32_t meta_len = 0;

    if (sqfs_read_metadata(meta_offset, meta_buf, &meta_len, NULL) < 0)
        return;

    if (dir_offset >= meta_len) return;

    uint8_t *p = meta_buf + dir_offset;
    uint8_t *end = meta_buf + meta_len;
    uint32_t bytes_read = 0;

    while (p + 12 <= end && bytes_read < dir_size - 3) {
        sqfs_dir_header_t *dhdr = (sqfs_dir_header_t *)p;
        p += sizeof(sqfs_dir_header_t);
        bytes_read += sizeof(sqfs_dir_header_t);

        uint32_t entry_count = dhdr->count + 1;
        for (uint32_t i = 0; i < entry_count && p + 8 <= end; i++) {
            sqfs_dir_entry_t *de = (sqfs_dir_entry_t *)p;
            uint16_t name_len = de->name_size + 1;

            if (p + 8 + name_len > end) break;

            char name[256];
            int nl = name_len < 255 ? name_len : 255;
            memcpy(name, de->name, nl);
            name[nl] = '\0';

            uint32_t ino = dhdr->inode_number + de->inode_offset;
            cb(name, de->type, ino, ctx);

            p += 8 + name_len;
            bytes_read += 8 + name_len;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────── */

int squashfs_mount(int (*read_fn)(uint64_t offset, void *buf, uint64_t len),
                   uint64_t image_offset)
{
    sqfs_read = read_fn;
    sqfs.image_offset = image_offset;

    /* Read superblock */
    if (sqfs_read(image_offset, &sqfs.sb, sizeof(sqfs_super_t)) < 0)
        return -1;

    if (sqfs.sb.magic != SQFS_MAGIC) {
        serial_puts("[SQFS] Invalid magic\n");
        return -1;
    }

    if (sqfs.sb.compression != SQFS_COMP_ZLIB) {
        serial_puts("[SQFS] Unsupported compression (need zlib, got ");
        serial_putdec(sqfs.sb.compression);
        serial_puts(")\n");
        return -1;
    }

    sqfs.mounted = true;

    serial_puts("[SQFS] Mounted: ");
    serial_putdec(sqfs.sb.inode_count);
    serial_puts(" inodes, block_size=");
    serial_putdec(sqfs.sb.block_size);
    serial_puts(", ");
    serial_putdec(sqfs.sb.bytes_used / 1024);
    serial_puts(" KB\n");
    return 0;
}

bool squashfs_is_mounted(void) { return sqfs.mounted; }

static void sqfs_ls_cb(const char *name, uint16_t type,
                       uint32_t inode_num, void *ctx)
{
    (void)inode_num; (void)ctx;
    serial_puts(type == SQFS_DIR_TYPE ? "  [DIR] " : "  ");
    serial_puts(name);
    serial_puts("\n");
}

int squashfs_ls(void)
{
    if (!sqfs.mounted) return -1;

    /* Decode root inode reference: upper 16 bits = metadata block offset,
     * lower 16 bits = offset within uncompressed block */
    uint32_t root_block = (uint32_t)(sqfs.sb.root_inode >> 16);
    uint16_t root_offset = (uint16_t)(sqfs.sb.root_inode & 0xFFFF);

    /* Read root inode metadata block */
    uint64_t meta_offset = sqfs.sb.inode_table_start + root_block;
    uint8_t meta_buf[8192];
    uint32_t meta_len = 0;
    if (sqfs_read_metadata(meta_offset, meta_buf, &meta_len, NULL) < 0)
        return -1;

    if (root_offset + sizeof(sqfs_dir_inode_t) > meta_len)
        return -1;

    sqfs_dir_inode_t *root = (sqfs_dir_inode_t *)(meta_buf + root_offset);
    if (root->hdr.inode_type != SQFS_DIR_TYPE &&
        root->hdr.inode_type != SQFS_LDIR_TYPE)
        return -1;

    serial_puts("[SQFS] Root directory:\n");
    sqfs_read_dir(root->start_block, root->offset, root->file_size,
                  sqfs_ls_cb, NULL);
    return 0;
}
