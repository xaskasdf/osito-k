/*
 * OsitoK x86-64 — OsitoFS v2 Bare-Metal Driver
 *
 * Reads OsitoFS v2 from NVMe via the NVMe read driver.
 * Supports: mount (validate superblock), list files, read blocks.
 *
 * The partition byte offset must be provided at mount time
 * (from GPT table parsing or hardcoded).
 */

#include "../include/types.h"
#include "../../../include/common/ositofs2_format.h"

/* ── Declarations ────────────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);
extern void fb_putc(char c, uint32_t color);

extern int nvme_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── Driver state ────────────────────────────────────────────── */

static uint64_t     partition_offset;  /* Byte offset of OsitoFS partition on NVMe */
static osfs2_super_t superblock;
static bool          mounted;

/* Cached file table (first 4096 * 256 = 1MB — read on mount) */
static osfs2_file_t *file_table;

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
    file_table = (osfs2_file_t *)mem_alloc_aligned(OSFS2_BLOCK_SIZE, 4096);
    if (!file_table) {
        serial_puts("[OsitoFS] Failed to allocate file table\n");
        return -1;
    }

    if (osfs2_read_block_data(OSFS2_FILETAB_BLK, file_table) < 0) {
        serial_puts("[OsitoFS] Failed to read file table\n");
        return -1;
    }

    mounted = true;

    fb_puts("\n OsitoFS v2 [");
    fb_puts(superblock.label);
    fb_puts("] — ");
    fb_putdec(superblock.file_count);
    fb_puts(" file(s)\n");

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

    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;

        osfs2_file_t *f = &file_table[i];

        /* Serial output */
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

        /* Framebuffer output */
        fb_puts("  ");
        fb_puts_color(f->name, 0x0000FF00); /* Green */

        /* Size */
        fb_puts("  ");
        if (f->size >= (uint64_t)1024 * 1024 * 1024) {
            fb_putdec(f->size / (1024 * 1024 * 1024));
            fb_puts(".");
            fb_putdec((f->size % (1024 * 1024 * 1024)) / (1024 * 1024 * 100));
            fb_puts(" GB");
        } else if (f->size >= 1024 * 1024) {
            fb_putdec(f->size / (1024 * 1024));
            fb_puts(" MB");
        } else {
            fb_putdec(f->size);
            fb_puts(" B");
        }

        if (f->flags & OSFS2_FLAG_GGUF) {
            fb_puts("  [");
            fb_puts(f->model_name);
            fb_puts(" ");
            fb_putdec(f->num_layers);
            fb_puts("L]");
        }
        fb_puts("\n");
    }

    /* Summary */
    uint32_t data_blocks = superblock.total_blocks - OSFS2_DATA_START_BLK;
    uint32_t used = superblock.next_data_block - OSFS2_DATA_START_BLK;
    fb_puts("  ");
    fb_putdec(used);
    fb_puts("/");
    fb_putdec(data_blocks);
    fb_puts(" MB used\n");
}

/* ── Find file by name ───────────────────────────────────────── */

osfs2_file_t *osfs2_find(const char *name)
{
    if (!mounted) return NULL;

    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if ((file_table[i].flags & OSFS2_FLAG_VALID) &&
            strcmp(file_table[i].name, name) == 0) {
            return &file_table[i];
        }
    }
    return NULL;
}

/* ── Read file data ──────────────────────────────────────────── */

int osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len)
{
    if (!mounted || !file) return -1;
    if (offset + len > file->size) return -1;

    uint64_t abs_offset = ((uint64_t)file->start_block << OSFS2_BLOCK_SHIFT) + offset;
    return osfs2_part_read(abs_offset, buf, len);
}

/* ── Read a full block from a file ───────────────────────────── */

int osfs2_read_file_block(osfs2_file_t *file, uint32_t block_index, void *buf)
{
    if (!mounted || !file) return -1;
    if (block_index >= file->block_count) return -1;

    return osfs2_read_block_data(file->start_block + block_index, buf);
}

/* ── Accessors ───────────────────────────────────────────────── */

bool osfs2_is_mounted(void)  { return mounted; }
uint32_t osfs2_file_count(void) { return mounted ? superblock.file_count : 0; }
const char *osfs2_label(void) { return mounted ? superblock.label : ""; }
