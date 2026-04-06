/*
 * OsitoK x86-64 — ext2 Read-Only Driver
 *
 * Reads files from ext2/ext3 partitions (Linux disks).
 * Supports inode lookup, directory traversal, block groups,
 * indirect blocks (single/double). No writing, no ext4 extents.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Block device read — set by ext2_mount */
static int (*ext2_read_sectors)(uint64_t lba, uint32_t count, void *buf);
static uint64_t ext2_part_lba;

/* ── ext2 On-Disk Structures ────────────────────────────────── */

#define EXT2_SUPER_OFFSET  1024  /* Superblock at byte offset 1024 */
#define EXT2_MAGIC         0xEF53
#define EXT2_ROOT_INO      2     /* Root directory inode */

/* Inode file types (from i_mode) */
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

/* Directory entry file types (from d_file_type) */
#define EXT2_FT_REG   1
#define EXT2_FT_DIR   2

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;     /* block_size = 1024 << this */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* Rev 1+ fields */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
} ext2_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_group_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;           /* File size (lower 32 bits) */
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;         /* 512-byte blocks allocated */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];      /* Block pointers: 0-11 direct, 12 indirect,
                                  13 double-indirect, 14 triple-indirect */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;        /* aka i_size_high for regular files */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];           /* Variable length, NOT null-terminated */
} ext2_dir_entry_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint16_t inode_size;
    uint32_t group_count;
    uint32_t first_data_block;
    ext2_group_desc_t *groups;  /* Block group descriptor table */
} e2;

/* ── Block I/O ───────────────────────────────────────────────── */

static int ext2_read_block(uint32_t block, void *buf)
{
    uint64_t byte_off = (uint64_t)block * e2.block_size;
    uint32_t sectors = e2.block_size / 512;
    uint64_t lba = ext2_part_lba + (byte_off / 512);
    return ext2_read_sectors(lba, sectors, buf);
}

/* ── Inode lookup ────────────────────────────────────────────── */

static int ext2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    if (ino < 1) return -1;
    uint32_t group = (ino - 1) / e2.inodes_per_group;
    uint32_t index = (ino - 1) % e2.inodes_per_group;

    if (group >= e2.group_count) return -1;

    /* Inode table block for this group */
    uint32_t inode_table_block = e2.groups[group].bg_inode_table;
    uint64_t byte_off = (uint64_t)inode_table_block * e2.block_size +
                        (uint64_t)index * e2.inode_size;

    /* Read the sector containing this inode */
    uint8_t sector[512];
    uint64_t sector_lba = ext2_part_lba + byte_off / 512;
    if (ext2_read_sectors(sector_lba, 1, sector) < 0) return -1;

    uint32_t off_in_sector = (uint32_t)(byte_off % 512);
    memcpy(out, sector + off_in_sector,
           e2.inode_size < sizeof(ext2_inode_t) ? e2.inode_size : sizeof(ext2_inode_t));
    return 0;
}

/* ── Data block lookup (with indirect support) ───────────────── */

static uint32_t ext2_get_block(const ext2_inode_t *inode, uint32_t idx)
{
    uint32_t ptrs_per_block = e2.block_size / 4;

    /* Direct blocks (0-11) */
    if (idx < 12) return inode->i_block[idx];

    /* Single indirect (12) */
    idx -= 12;
    if (idx < ptrs_per_block) {
        uint32_t *indirect = (uint32_t *)kmalloc(e2.block_size);
        if (!indirect) return 0;
        ext2_read_block(inode->i_block[12], indirect);
        uint32_t blk = indirect[idx];
        kfree(indirect);
        return blk;
    }

    /* Double indirect (13) */
    idx -= ptrs_per_block;
    if (idx < ptrs_per_block * ptrs_per_block) {
        uint32_t *dind = (uint32_t *)kmalloc(e2.block_size);
        if (!dind) return 0;
        ext2_read_block(inode->i_block[13], dind);
        uint32_t ind_block = dind[idx / ptrs_per_block];
        kfree(dind);

        uint32_t *ind = (uint32_t *)kmalloc(e2.block_size);
        if (!ind) return 0;
        ext2_read_block(ind_block, ind);
        uint32_t blk = ind[idx % ptrs_per_block];
        kfree(ind);
        return blk;
    }

    return 0;  /* Triple indirect not supported */
}

/* ── Directory traversal ─────────────────────────────────────── */

typedef void (*ext2_dir_cb)(const char *name, uint32_t inode,
                            uint8_t file_type, void *ctx);

static void ext2_read_dir(uint32_t dir_ino, ext2_dir_cb cb, void *ctx)
{
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) < 0) return;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return;

    uint32_t dir_size = inode.i_size;
    uint8_t *buf = (uint8_t *)kmalloc(e2.block_size);
    if (!buf) return;

    uint32_t blocks = (dir_size + e2.block_size - 1) / e2.block_size;
    uint32_t pos = 0;

    for (uint32_t bi = 0; bi < blocks && pos < dir_size; bi++) {
        uint32_t blk = ext2_get_block(&inode, bi);
        if (blk == 0) break;
        ext2_read_block(blk, buf);

        uint32_t off = 0;
        while (off < e2.block_size && pos + off < dir_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len > 0) {
                char name[256];
                int nl = de->name_len < 255 ? de->name_len : 255;
                memcpy(name, de->name, nl);
                name[nl] = '\0';

                /* Skip . and .. */
                if (!(nl == 1 && name[0] == '.') &&
                    !(nl == 2 && name[0] == '.' && name[1] == '.'))
                    cb(name, de->inode, de->file_type, ctx);
            }
            off += de->rec_len;
        }
        pos += e2.block_size;
    }
    kfree(buf);
}

/* ── Public API: Mount ───────────────────────────────────────── */

int ext2_mount(uint64_t part_lba,
               int (*read_fn)(uint64_t lba, uint32_t count, void *buf))
{
    ext2_read_sectors = read_fn;
    ext2_part_lba = part_lba;

    /* Read superblock (at byte offset 1024 from partition start) */
    uint8_t sb_buf[1024];
    if (ext2_read_sectors(part_lba + 2, 2, sb_buf) < 0) {
        serial_puts("[EXT2] Failed to read superblock\n");
        return -1;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) {
        serial_puts("[EXT2] Invalid magic (expected 0xEF53, got 0x");
        serial_puthex(sb->s_magic, 4);
        serial_puts(")\n");
        return -1;
    }

    e2.block_size = 1024U << sb->s_log_block_size;
    e2.inodes_per_group = sb->s_inodes_per_group;
    e2.blocks_per_group = sb->s_blocks_per_group;
    e2.inode_size = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    e2.first_data_block = sb->s_first_data_block;
    e2.group_count = (sb->s_blocks_count + sb->s_blocks_per_group - 1) /
                     sb->s_blocks_per_group;

    /* Read block group descriptor table (block after superblock) */
    uint32_t gdt_block = e2.first_data_block + 1;
    uint32_t gdt_size = e2.group_count * sizeof(ext2_group_desc_t);
    uint32_t gdt_blocks = (gdt_size + e2.block_size - 1) / e2.block_size;

    e2.groups = (ext2_group_desc_t *)kmalloc((uint64_t)gdt_blocks * e2.block_size);
    if (!e2.groups) return -1;

    for (uint32_t i = 0; i < gdt_blocks; i++)
        ext2_read_block(gdt_block + i,
                        (uint8_t *)e2.groups + (uint64_t)i * e2.block_size);

    e2.mounted = true;

    serial_puts("[EXT2] Mounted: block_size=");
    serial_putdec(e2.block_size);
    serial_puts(" groups=");
    serial_putdec(e2.group_count);
    serial_puts(" inodes/group=");
    serial_putdec(e2.inodes_per_group);
    serial_puts("\n");

    return 0;
}

bool ext2_is_mounted(void) { return e2.mounted; }

/* ── Public API: List root directory ─────────────────────────── */

static void ext2_ls_cb(const char *name, uint32_t inode,
                       uint8_t file_type, void *ctx)
{
    (void)inode; (void)ctx;
    if (file_type == EXT2_FT_DIR)
        serial_puts("  [DIR] ");
    else
        serial_puts("  ");
    serial_puts(name);
    serial_puts("\n");
}

int ext2_ls(void)
{
    if (!e2.mounted) return -1;
    serial_puts("[EXT2] Root directory:\n");
    ext2_read_dir(EXT2_ROOT_INO, ext2_ls_cb, NULL);
    return 0;
}

/* ── Public API: Find file in root ───────────────────────────── */

typedef struct {
    const char *target;
    uint32_t    ino;
    bool        found;
} ext2_find_ctx_t;

static void ext2_find_cb(const char *name, uint32_t inode,
                         uint8_t file_type, void *ctx)
{
    ext2_find_ctx_t *fc = (ext2_find_ctx_t *)ctx;
    (void)file_type;
    if (!fc->found) {
        const char *a = name, *b = fc->target;
        bool match = true;
        while (*a && *b) { if (*a++ != *b++) { match = false; break; } }
        if (match && *a == *b) { fc->ino = inode; fc->found = true; }
    }
}

int ext2_find(const char *name, uint32_t *ino_out)
{
    if (!e2.mounted) return -1;
    ext2_find_ctx_t ctx = { .target = name, .found = false };
    ext2_read_dir(EXT2_ROOT_INO, ext2_find_cb, &ctx);
    if (!ctx.found) return -1;
    if (ino_out) *ino_out = ctx.ino;
    return 0;
}

/* ── Public API: Read file ───────────────────────────────────── */

int ext2_read_file(const char *name, uint64_t offset, void *buf, uint64_t len)
{
    uint32_t ino;
    if (ext2_find(name, &ino) < 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;

    uint32_t size = inode.i_size;
    if (offset >= size) return 0;
    if (offset + len > size) len = size - offset;

    uint8_t *blk_buf = (uint8_t *)kmalloc(e2.block_size);
    if (!blk_buf) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;
    uint32_t block_idx = (uint32_t)(offset / e2.block_size);
    uint32_t block_off = (uint32_t)(offset % e2.block_size);

    while (remaining > 0) {
        uint32_t blk = ext2_get_block(&inode, block_idx);
        if (blk == 0) break;

        ext2_read_block(blk, blk_buf);

        uint32_t avail = e2.block_size - block_off;
        uint32_t copy = (remaining < avail) ? (uint32_t)remaining : avail;
        memcpy(dst, blk_buf + block_off, copy);

        dst += copy;
        remaining -= copy;
        block_off = 0;
        block_idx++;
    }

    kfree(blk_buf);
    return (int)(len - remaining);
}
