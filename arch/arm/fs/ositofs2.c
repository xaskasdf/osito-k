/*
 * OsitoFS v2 driver for AArch64 (port from x86-64)
 *
 * Full R/W driver with block reclamation.
 * Uses VirtIO-blk backend instead of NVMe.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "../../../include/common/ositofs2_format.h"
#include "../drivers/virtio_blk.h"

/* ── Declarations ────────────────────────────────────────────── */

/* Block I/O: redirect to VirtIO-blk */
static inline int nvme_read_bytes(uint64_t off, void *buf, uint64_t len) {
    return virtio_blk_read_bytes(off, buf, len);
}
static inline int nvme_write_bytes(uint64_t off, const void *buf, uint64_t len) {
    return virtio_blk_write_bytes(off, buf, len);
}
static inline int nvme_flush(void) {
    return virtio_blk_flush();
}

/* strcpy for freestanding */
static inline char *osfs2_strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

/* ── Driver state ────────────────────────────────────────────── */

static uint64_t     partition_offset;  /* Byte offset of OsitoFS partition on NVMe */
static osfs2_super_t superblock;
static bool          mounted;

/* Cached file table (first 4096 * 256 = 1MB — read on mount) */
static osfs2_file_t *file_table;

/* ── Block usage bitmap (in-memory, rebuilt on mount) ────────── */
/* Tracks which 1MB blocks are in use. Enables block reclamation  */
/* on delete — freed blocks can be reused by subsequent creates.  */

#define BLK_BITMAP_BYTES  (OSFS2_MAX_BLOCKS / 8)  /* 32KB for 262144 blocks */
static uint8_t blk_bitmap[BLK_BITMAP_BYTES];      /* 1 = used, 0 = free */

static inline void blk_bitmap_set(uint32_t blk)
{
    if (blk < OSFS2_MAX_BLOCKS)
        blk_bitmap[blk / 8] |= (1 << (blk % 8));
}

static inline void blk_bitmap_clear(uint32_t blk)
{
    if (blk < OSFS2_MAX_BLOCKS)
        blk_bitmap[blk / 8] &= ~(1 << (blk % 8));
}

static inline int blk_bitmap_test(uint32_t blk)
{
    if (blk >= OSFS2_MAX_BLOCKS) return 1;
    return (blk_bitmap[blk / 8] >> (blk % 8)) & 1;
}

/* Rebuild bitmap from file table (called on mount) */
static void blk_bitmap_rebuild(void)
{
    memset(blk_bitmap, 0, sizeof(blk_bitmap));

    /* Metadata blocks 0-3 always used */
    for (uint32_t i = 0; i < OSFS2_DATA_START_BLK; i++)
        blk_bitmap_set(i);

    /* Mark each valid file's blocks (limit to file_count for partial reads) */
    uint32_t scan_limit = superblock.file_count + 16;
    if (scan_limit > OSFS2_MAX_FILES) scan_limit = OSFS2_MAX_FILES;
    for (uint32_t i = 0; i < scan_limit; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;
        osfs2_file_t *f = &file_table[i];
        for (uint32_t b = 0; b < f->block_count; b++)
            blk_bitmap_set(f->start_block + b);
    }

    /* Recompute used_blocks from bitmap (fixes stale superblock values) */
    uint32_t used = 0;
    for (uint32_t b = OSFS2_DATA_START_BLK; b < superblock.total_blocks; b++) {
        if (blk_bitmap_test(b)) used++;
    }
    superblock.used_blocks = used + OSFS2_DATA_START_BLK;
}

/* Find contiguous free region (first-fit). Returns start block, or 0 if none. */
static uint32_t blk_bitmap_find_free(uint32_t count)
{
    uint32_t run_start = 0;
    uint32_t run_len = 0;

    for (uint32_t b = OSFS2_DATA_START_BLK; b < superblock.total_blocks; b++) {
        if (!blk_bitmap_test(b)) {
            if (run_len == 0) run_start = b;
            run_len++;
            if (run_len == count) return run_start;
        } else {
            run_len = 0;
        }
    }
    return 0;
}

/* ── Read from partition ─────────────────────────────────────── */

static int osfs2_part_read(uint64_t offset, void *buf, uint64_t len)
{
    return nvme_read_bytes(partition_offset + offset, buf, len);
}

static int osfs2_read_block_data(uint32_t block, void *buf)
{
    uint64_t offset = (uint64_t)block << OSFS2_BLOCK_SHIFT;
    return osfs2_part_read(offset, buf, OSFS2_BLOCK_SIZE);
}

/* ── Mount ───────────────────────────────────────────────────── */

int osfs2_mount(uint64_t part_offset)
{
    partition_offset = part_offset;
    mounted = false;

    serial_puts("[OsitoFS] Mounting at partition offset ");
    serial_puthex(part_offset, 16);
    serial_puts("\n");

    /* Read superblock (first 512 bytes of block 0) */
    if (osfs2_part_read(0, &superblock, sizeof(superblock)) < 0) {
        serial_puts("[OsitoFS] Failed to read superblock\n");
        return -1;
    }

    /* Validate */
    if (superblock.magic != OSFS2_MAGIC) {
        serial_puts("[OsitoFS] Bad magic: ");
        serial_puthex(superblock.magic, 8);
        serial_puts("\n");
        return -1;
    }

    if (superblock.version != OSFS2_VERSION) {
        serial_puts("[OsitoFS] Unsupported version: ");
        serial_putdec(superblock.version);
        serial_puts("\n");
        return -1;
    }

    /* Verify CRC */
    uint32_t saved_crc = superblock.crc32;
    superblock.crc32 = 0;
    uint32_t calc_crc = osfs2_crc32(&superblock, sizeof(superblock));
    superblock.crc32 = saved_crc;

    if (calc_crc != saved_crc) {
        serial_puts("[OsitoFS] Superblock CRC mismatch\n");
        return -1;
    }

    serial_puts("[OsitoFS] Superblock OK: label=\"");
    serial_puts(superblock.label);
    serial_puts("\", files=");
    serial_putdec(superblock.file_count);
    serial_puts(", blocks=");
    serial_putdec(superblock.total_blocks);
    serial_puts("\n");

    /* Read file table */
    /* Read file table — only enough for actual files (QEMU TCG is slow) */
    {
        uint32_t ft_entries = superblock.file_count + 16;  /* read a few extra */
        if (ft_entries > OSFS2_MAX_FILES) ft_entries = OSFS2_MAX_FILES;
        uint64_t ft_bytes = (uint64_t)ft_entries * sizeof(osfs2_file_t);
        ft_bytes = (ft_bytes + 4095) & ~4095ULL;  /* round up to 4KB */
        if (ft_bytes < 4096) ft_bytes = 4096;

        file_table = (osfs2_file_t *)mem_alloc_pages((ft_bytes + 4095) / 4096);
        if (!file_table) {
            serial_puts("[OsitoFS] Failed to allocate file table\n");
            return -1;
        }

        uint64_t ft_offset = (uint64_t)OSFS2_FILETAB_BLK << OSFS2_BLOCK_SHIFT;
        if (osfs2_part_read(ft_offset, file_table, ft_bytes) < 0) {
            serial_puts("[OsitoFS] Failed to read file table\n");
            return -1;
        }
    }
    serial_puts("[OsitoFS] File table loaded\n");

    mounted = true;

    /* Build block usage bitmap from file table */
    blk_bitmap_rebuild();

    /* Shrink next_data_block if trailing blocks are free (recover from old append-only) */
    while (superblock.next_data_block > OSFS2_DATA_START_BLK &&
           !blk_bitmap_test(superblock.next_data_block - 1))
        superblock.next_data_block--;

    uint32_t data_blks = superblock.total_blocks - OSFS2_DATA_START_BLK;
    uint32_t used_data = superblock.used_blocks - OSFS2_DATA_START_BLK;

    serial_puts("[OsitoFS] Block bitmap built: ");
    serial_putdec(used_data);
    serial_puts("/");
    serial_putdec(data_blks);
    serial_puts(" data blocks used, ");
    serial_putdec(data_blks - used_data);
    serial_puts(" free\n");

    fb_puts("\n OsitoFS v2 [");
    fb_puts(superblock.label);
    fb_puts("] — ");
    fb_putdec(superblock.file_count);
    fb_puts(" file(s), ");
    fb_putdec(data_blks - used_data);
    fb_puts(" MB free\n");

    return 0;
}

/* ── List files ──────────────────────────────────────────────── */

void osfs2_list(void)
{
    if (!mounted) {
        fb_puts(" OsitoFS: not mounted\n");
        return;
    }

    if (superblock.file_count == 0) {
        fb_puts(" (empty filesystem)\n");
        serial_puts("[OsitoFS] Empty filesystem\n");
        return;
    }

    serial_puts("[OsitoFS] File listing:\n");

    uint32_t file_count = 0;
    uint32_t list_limit = superblock.file_count + 16;
    if (list_limit > OSFS2_MAX_FILES) list_limit = OSFS2_MAX_FILES;
    for (uint32_t i = 0; i < list_limit; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;

        osfs2_file_t *f = &file_table[i];
        file_count++;

        /* Serial output only (per-file listing slows framebuffer on real HW) */
        serial_puts("  ");
        serial_puts(f->name);
        serial_puts("  size=");
        serial_putdec(f->size);
        if (f->flags & OSFS2_FLAG_GGUF) {
            serial_puts("  model=");
            serial_puts(f->model_name);
            serial_puts("  layers=");
            serial_putdec(f->num_layers);
        }
        serial_puts("\n");
    }

    /* Framebuffer: summary only */
    uint32_t data_blocks = superblock.total_blocks - OSFS2_DATA_START_BLK;
    uint32_t used_data = superblock.used_blocks - OSFS2_DATA_START_BLK;
    fb_puts("  ");
    fb_putdec(file_count);
    fb_puts(" files, ");
    fb_putdec(used_data);
    fb_puts("/");
    fb_putdec(data_blocks);
    fb_puts(" MB used\n");
}

/* ── Simple wildcard match (*.ext style) ─────────────────────── */

static int osfs2_wildcard_match(const char *pattern, const char *name)
{
    /* Only support "*.ext" and "*" patterns */
    if (pattern[0] == '*' && pattern[1] == '.') {
        /* Match by extension */
        const char *ext = pattern + 1;  /* ".ext" */
        int elen = 0;
        while (ext[elen]) elen++;
        int nlen = 0;
        while (name[nlen]) nlen++;
        if (nlen < elen) return 0;
        for (int i = 0; i < elen; i++) {
            char a = name[nlen - elen + i];
            char b = ext[i];
            /* case-insensitive */
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
        }
        return 1;
    }
    if (pattern[0] == '*' && pattern[1] == '\0')
        return 1;  /* match everything */
    /* Exact match fallback */
    return strcmp(pattern, name) == 0;
}

/* Find first file matching a pattern. Returns file index or -1. */
int osfs2_find_first(const char *pattern, int start_idx)
{
    if (!mounted) return -1;
    for (int i = start_idx; i < OSFS2_MAX_FILES; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;
        if (osfs2_wildcard_match(pattern, file_table[i].name))
            return i;
    }
    return -1;
}

/* Get file entry by index */
osfs2_file_t *osfs2_get_file(int index)
{
    if (index < 0 || index >= OSFS2_MAX_FILES) return NULL;
    if (!(file_table[index].flags & OSFS2_FLAG_VALID)) return NULL;
    return &file_table[index];
}

/* ── Find file by name ───────────────────────────────────────── */

osfs2_file_t *osfs2_find(const char *name)
{
    if (!mounted) return NULL;

    uint32_t find_limit = superblock.file_count + 16;
    if (find_limit > OSFS2_MAX_FILES) find_limit = OSFS2_MAX_FILES;
    for (uint32_t i = 0; i < find_limit; i++) {
        if ((file_table[i].flags & OSFS2_FLAG_VALID) &&
            strcmp(file_table[i].name, name) == 0) {
            return &file_table[i];
        }
    }
    return NULL;
}

/* Case-insensitive find — for Win32 compat (Windows filenames are CI) */
osfs2_file_t *osfs2_find_ci(const char *name)
{
    if (!mounted || !name) return NULL;

    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;
        const char *a = file_table[i].name;
        const char *b = name;
        int match = 1;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) { match = 0; break; }
            a++; b++;
        }
        if (match && !*a && !*b) return &file_table[i];
    }
    return NULL;
}

/* ── Read file data ──────────────────────────────────────────── */

int osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len)
{
    if (!mounted || !file) return -1;
    if (offset + len > file->size) return -1;

    uint64_t abs_offset = ((uint64_t)file->start_block << OSFS2_BLOCK_SHIFT) + offset;
    int rc = osfs2_part_read(abs_offset, buf, len);
    if (rc < 0) return -1;
    return (int)len;  /* nvme_read_bytes returns 0 on success, not byte count */
}

/* ── Read a full block from a file ───────────────────────────── */

int osfs2_read_file_block(osfs2_file_t *file, uint32_t block_index, void *buf)
{
    if (!mounted || !file) return -1;
    if (block_index >= file->block_count) return -1;

    return osfs2_read_block_data(file->start_block + block_index, buf);
}

/* ── Find first GGUF file ────────────────────────────────────── */

osfs2_file_t *osfs2_find_gguf(void)
{
    if (!mounted) return NULL;

    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if ((file_table[i].flags & OSFS2_FLAG_VALID) &&
            (file_table[i].flags & OSFS2_FLAG_GGUF)) {
            return &file_table[i];
        }
    }
    return NULL;
}

/* ── Read layer index from block 3 ──────────────────────────── */

int osfs2_read_layer_index(uint16_t slot, osfs2_layer_idx_t *li)
{
    if (!mounted || slot >= OSFS2_MAX_MODELS) return -1;

    uint64_t offset = ((uint64_t)OSFS2_LAYERIDX_BLK << OSFS2_BLOCK_SHIFT)
                    + (uint64_t)slot * sizeof(osfs2_layer_idx_t);
    return osfs2_part_read(offset, li, sizeof(*li));
}

/* ── Write to partition ──────────────────────────────────────── */

static int osfs2_part_write(uint64_t offset, const void *buf, uint64_t len)
{
    return nvme_write_bytes(partition_offset + offset, buf, len);
}

/* ── Persist superblock to disk ─────────────────────────────── */

static int osfs2_write_superblock(void)
{
    superblock.crc32 = 0;
    superblock.crc32 = osfs2_crc32(&superblock, sizeof(superblock));
    return osfs2_part_write(0, &superblock, sizeof(superblock));
}

/* ── Persist file table to disk ─────────────────────────────── */

static int osfs2_write_file_table(void)
{
    return osfs2_part_write((uint64_t)OSFS2_FILETAB_BLK << OSFS2_BLOCK_SHIFT,
                            file_table, OSFS2_BLOCK_SIZE);
}

/* ── Create a new file ──────────────────────────────────────── */

osfs2_file_t *osfs2_create(const char *name, uint64_t size)
{
    if (!mounted || !name) return NULL;

    /* Check name doesn't already exist */
    if (osfs2_find(name)) {
        serial_puts("[OsitoFS] File already exists: ");
        serial_puts(name);
        serial_puts("\n");
        return NULL;
    }

    /* Find free slot */
    int slot = -1;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        serial_puts("[OsitoFS] File table full\n");
        return NULL;
    }

    /* Calculate blocks needed */
    uint32_t blocks = (uint32_t)((size + OSFS2_BLOCK_SIZE - 1) >> OSFS2_BLOCK_SHIFT);
    if (blocks == 0) blocks = 1;

    /* Try to reuse freed blocks first (first-fit in bitmap) */
    uint32_t start = blk_bitmap_find_free(blocks);
    if (!start) {
        /* No reusable gap — append at high-water mark */
        if (superblock.next_data_block + blocks > superblock.total_blocks) {
            serial_puts("[OsitoFS] Not enough space: need ");
            serial_putdec(blocks);
            serial_puts(" blocks\n");
            return NULL;
        }
        start = superblock.next_data_block;
        superblock.next_data_block += blocks;
    }

    /* Mark blocks as used in bitmap */
    for (uint32_t b = 0; b < blocks; b++)
        blk_bitmap_set(start + b);

    /* Fill file entry */
    osfs2_file_t *f = &file_table[slot];
    memset(f, 0, sizeof(*f));
    osfs2_strcpy(f->name, name);
    f->size = size;
    f->start_block = start;
    f->block_count = blocks;
    f->flags = OSFS2_FLAG_VALID;
    f->layer_index_slot = 0xFFFF;

    /* Update superblock */
    superblock.used_blocks += blocks;
    superblock.file_count++;

    /* Persist */
    if (osfs2_write_file_table() < 0 || osfs2_write_superblock() < 0) {
        serial_puts("[OsitoFS] Failed to persist metadata\n");
        return NULL;
    }
    nvme_flush();

    serial_puts("[OsitoFS] Created '");
    serial_puts(name);
    serial_puts("' size=");
    serial_putdec(size);
    serial_puts(" blocks=");
    serial_putdec(blocks);
    serial_puts(" @ block ");
    serial_putdec(f->start_block);
    serial_puts("\n");

    return f;
}

/* ── Write data to an existing file ─────────────────────────── */

int osfs2_write(osfs2_file_t *file, uint64_t offset, const void *buf, uint64_t len)
{
    if (!mounted || !file || !buf) return -1;
    if (offset + len > (uint64_t)file->block_count << OSFS2_BLOCK_SHIFT) return -1;

    uint64_t abs_offset = ((uint64_t)file->start_block << OSFS2_BLOCK_SHIFT) + offset;
    int ret = osfs2_part_write(abs_offset, buf, len);
    if (ret < 0) return -1;

    /* Update file size if we wrote past current end */
    if (offset + len > file->size) {
        file->size = offset + len;
        osfs2_write_file_table();
        osfs2_write_superblock();
    }

    return 0;
}

/* ── Delete a file ──────────────────────────────────────────── */

int osfs2_delete(const char *name)
{
    if (!mounted || !name) return -1;

    osfs2_file_t *f = osfs2_find(name);
    if (!f) return -1;

    uint32_t freed_blocks = f->block_count;

    /* Free blocks in bitmap */
    for (uint32_t b = 0; b < f->block_count; b++)
        blk_bitmap_clear(f->start_block + b);

    /* Mark file entry as invalid */
    f->flags = 0;
    superblock.file_count--;
    superblock.used_blocks -= freed_blocks;

    /* Shrink high-water mark if we freed trailing blocks */
    while (superblock.next_data_block > OSFS2_DATA_START_BLK &&
           !blk_bitmap_test(superblock.next_data_block - 1))
        superblock.next_data_block--;

    if (osfs2_write_file_table() < 0 || osfs2_write_superblock() < 0)
        return -1;

    nvme_flush();

    serial_puts("[OsitoFS] Deleted '");
    serial_puts(name);
    serial_puts("' (freed ");
    serial_putdec(freed_blocks);
    serial_puts(" blocks)\n");
    return 0;
}

/* ── Accessors ───────────────────────────────────────────────── */

bool osfs2_is_mounted(void)  { return mounted; }
uint32_t osfs2_file_count(void) { return mounted ? superblock.file_count : 0; }
const char *osfs2_label(void) { return mounted ? superblock.label : ""; }
uint64_t osfs2_file_size(osfs2_file_t *file) { return file ? file->size : 0; }
const char *osfs2_file_name(osfs2_file_t *file) { return file ? file->name : NULL; }
osfs2_file_t *osfs2_file_by_index(uint32_t idx)
{
    if (!mounted) return NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (file_table[i].flags & OSFS2_FLAG_VALID) {
            if (n == idx) return &file_table[i];
            n++;
        }
    }
    return NULL;
}
uint32_t osfs2_free_blocks(void) {
    if (!mounted) return 0;
    uint32_t total_data = superblock.total_blocks - OSFS2_DATA_START_BLK;
    uint32_t used_data = superblock.used_blocks - OSFS2_DATA_START_BLK;
    return total_data - used_data;
}

/* Get nth valid file (0-indexed). Returns NULL if out of range. */
osfs2_file_t *osfs2_file_at(uint32_t index)
{
    if (!mounted || !file_table) return NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (file_table[i].flags & OSFS2_FLAG_VALID) {
            if (count == index) return &file_table[i];
            count++;
        }
    }
    return NULL;
}
