/*
 * OsitoK x86-64 — OsitoFS v2 Bare-Metal Driver
 *
 * Full R/W driver with block reclamation.
 * In-memory block bitmap rebuilt on mount — freed blocks are reused
 * by subsequent creates (first-fit), eliminating the old append-only
 * space leak. No on-disk format changes required.
 *
 * The partition byte offset must be provided at mount time
 * (from GPT table parsing or hardcoded).
 */

#include "../include/types.h"
#include "../include/paging.h"
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

extern int disk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
extern int disk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len);
extern int disk_flush(void);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── Driver state ────────────────────────────────────────────── */

static uint64_t     partition_offset;  /* Byte offset of OsitoFS partition on NVMe */
static osfs2_super_t superblock;
static bool          mounted;

/* Cached file table (4096 * 256 = 1MB — read on mount) */
static osfs2_file_t *file_table;

/* Cached block CRC table (262144 * 4 = 1MB — read on mount) */
static uint32_t *crc_table;

/* Runtime block size from superblock */
static uint32_t blk_size;
static uint32_t blk_shift;
static uint32_t data_start;

/* Boot-time epoch from CMOS RTC (set once on mount) */
static uint64_t boot_epoch_sec;

/* ── Block usage bitmap (in-memory, rebuilt on mount) ────────── */
/* Tracks which blocks are in use. Enables block reclamation       */
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

    /* Metadata blocks always used */
    for (uint32_t i = 0; i < data_start; i++)
        blk_bitmap_set(i);

    /* Mark each valid file's blocks */
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;
        osfs2_file_t *f = &file_table[i];
        for (uint32_t b = 0; b < f->block_count; b++)
            blk_bitmap_set(f->start_block + b);
    }

    /* Recompute used_blocks from bitmap (fixes stale superblock values) */
    uint32_t used = 0;
    for (uint32_t b = data_start; b < superblock.total_blocks; b++) {
        if (blk_bitmap_test(b)) used++;
    }
    superblock.used_blocks = used + data_start;
}

/* Find contiguous free region (first-fit). Returns start block, or 0 if none. */
static uint32_t blk_bitmap_find_free(uint32_t count)
{
    uint32_t run_start = 0;
    uint32_t run_len = 0;

    for (uint32_t b = data_start; b < superblock.total_blocks; b++) {
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

/* Returns 1 iff the whole range [start, start+count) is in-bounds AND every
 * block in it is currently FREE in the bitmap. This is the authoritative
 * overlap guard: a range that fails this check must NEVER be handed out,
 * because the bitmap is rebuilt from all live file extents on mount.
 *
 * Why this exists: osfs2_create used to fall back to appending at
 * superblock.next_data_block whenever blk_bitmap_find_free() returned 0.
 * But next_data_block can be STALE/LOW relative to the actual high-water
 * mark (observed on nvme_gcc.img: next_data_block=4 while 2034 data blocks
 * were live). Appending blindly at a stale next_data_block handed out an
 * already-occupied block (4), letting pkg/catalog.json's extent clobber
 * hello.c's live extent. Now the bitmap is the single source of truth. */
static int blk_range_free(uint32_t start, uint32_t count)
{
    if (count == 0) return 0;
    if (start < data_start) return 0;
    /* Guard against overflow and out-of-range (total_blocks bounds the FS). */
    if ((uint64_t)start + count > (uint64_t)superblock.total_blocks) return 0;
    for (uint32_t b = start; b < start + count; b++)
        if (blk_bitmap_test(b)) return 0;   /* any used block => not free */
    return 1;
}

/* ── File name hash table (in-memory, O(1) lookup) ──────────── */

#define OSFS2_HASH_SLOTS  8192
#define OSFS2_HASH_MASK   (OSFS2_HASH_SLOTS - 1)
#define OSFS2_HASH_EMPTY  0xFFFF

static uint16_t name_hash[OSFS2_HASH_SLOTS];

static uint32_t osfs2_name_hash_fn(const char *name)
{
    char lower[OSFS2_NAME_LEN];
    int len = 0;
    while (name[len] && len < OSFS2_NAME_LEN - 1) {
        char c = name[len];
        if (c >= 'A' && c <= 'Z') c += 32;
        lower[len] = c;
        len++;
    }
    lower[len] = '\0';
    return osfs2_crc32(lower, (size_t)len) & OSFS2_HASH_MASK;
}

static void osfs2_hash_insert(uint16_t idx)
{
    uint32_t slot = osfs2_name_hash_fn(file_table[idx].name);
    while (name_hash[slot] != OSFS2_HASH_EMPTY)
        slot = (slot + 1) & OSFS2_HASH_MASK;
    name_hash[slot] = idx;
}

static void osfs2_hash_build(void)
{
    for (uint32_t i = 0; i < OSFS2_HASH_SLOTS; i++)
        name_hash[i] = OSFS2_HASH_EMPTY;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (file_table[i].flags & OSFS2_FLAG_VALID)
            osfs2_hash_insert((uint16_t)i);
    }
}

/* ── Read from partition ─────────────────────────────────────── */

static int osfs2_part_read(uint64_t offset, void *buf, uint64_t len)
{
    return disk_read_bytes(partition_offset + offset, buf, len);
}

static int osfs2_read_block_data(uint32_t block, void *buf)
{
    uint64_t offset = (uint64_t)block << blk_shift;
    return osfs2_part_read(offset, buf, blk_size);
}

/* ── RTC helpers (read boot epoch once on mount) ────────────── */

static inline uint8_t cmos_rd(uint8_t reg)
{
#ifdef __EMSCRIPTEN__
    (void)reg;
    return 0;
#else
    __asm__ volatile ("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)0x71));
    return val;
#endif
}

static inline uint8_t bcd2b(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static uint64_t rtc_to_epoch(void)
{
    uint8_t sec  = bcd2b(cmos_rd(0x00));
    uint8_t min  = bcd2b(cmos_rd(0x02));
    uint8_t hour = bcd2b(cmos_rd(0x04));
    uint8_t day  = bcd2b(cmos_rd(0x07));
    uint8_t mon  = bcd2b(cmos_rd(0x08));
    uint8_t year = bcd2b(cmos_rd(0x09));
    uint32_t y = 2000 + year;
    static const uint16_t mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t days = 0;
    for (uint32_t i = 1970; i < y; i++)
        days += (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0)) ? 366 : 365;
    for (uint8_t i = 1; i < mon; i++) {
        days += mdays[i];
        if (i == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days++;
    }
    days += day - 1;
    return (uint64_t)days * 86400 + hour * 3600 + min * 60 + sec;
}

extern uint64_t idt_get_ticks(void);

static uint32_t osfs2_get_time(void)
{
    return (uint32_t)(boot_epoch_sec + idt_get_ticks() / 100);
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
        serial_puts("[OsitoFS] Primary superblock CRC mismatch, trying backup...\n");
        if (osfs2_part_read(OSFS2_SUPER_BACKUP_OFF, &superblock, sizeof(superblock)) == 0) {
            saved_crc = superblock.crc32;
            superblock.crc32 = 0;
            calc_crc = osfs2_crc32(&superblock, sizeof(superblock));
            superblock.crc32 = saved_crc;
            if (calc_crc == saved_crc && superblock.magic == OSFS2_MAGIC) {
                serial_puts("[OsitoFS] Using backup superblock\n");
                goto superblock_ok;
            }
        }
        serial_puts("[OsitoFS] Superblock CRC mismatch\n");
        return -1;
    }
superblock_ok:
    boot_epoch_sec = rtc_to_epoch();

    /* Set runtime block size from superblock */
    if (!osfs2_valid_block_size(superblock.block_size)) {
        serial_puts("[OsitoFS] Invalid block size: ");
        serial_putdec(superblock.block_size);
        serial_puts("\n");
        return -1;
    }
    blk_size = superblock.block_size;
    blk_shift = osfs2_block_shift(blk_size);
    data_start = osfs2_data_start_blk(blk_size);

    serial_puts("[OsitoFS] Superblock OK: label=\"");
    serial_puts(superblock.label);
    serial_puts("\", files=");
    serial_putdec(superblock.file_count);
    serial_puts(", blocks=");
    serial_putdec(superblock.total_blocks);
    serial_puts(", blk_size=");
    serial_putdec(blk_size);
    serial_puts("\n");

    /* Read file table (1MB at fixed offset). Stored as upper-half virt
     * so it stays reachable after the lower-half identity map is gone
     * from user PML4s. */
    void *ft_phys = mem_alloc_aligned(OSFS2_FILETAB_SIZE, 4096);
    if (!ft_phys) {
        serial_puts("[OsitoFS] Failed to allocate file table\n");
        return -1;
    }
    file_table = (osfs2_file_t *)PHYS_TO_VIRT(ft_phys);

    if (osfs2_part_read(OSFS2_FILETAB_OFF, file_table, OSFS2_FILETAB_SIZE) < 0) {
        serial_puts("[OsitoFS] Failed to read file table\n");
        return -1;
    }

    /* Read block CRC table (1MB at fixed offset) */
    void *crc_phys = mem_alloc_aligned(OSFS2_CRCTAB_SIZE, 4096);
    if (crc_phys) {
        crc_table = (uint32_t *)PHYS_TO_VIRT(crc_phys);
        if (osfs2_part_read(OSFS2_CRCTAB_OFF, crc_table, OSFS2_CRCTAB_SIZE) < 0) {
            serial_puts("[OsitoFS] CRC table read failed (verification disabled)\n");
            mem_free_pages(crc_phys, OSFS2_CRCTAB_SIZE / 4096);
            crc_table = NULL;
        }
    }

    mounted = true;

    /* Build block usage bitmap from file table */
    blk_bitmap_rebuild();

    /* Re-anchor next_data_block to the TRUE high-water mark derived from the
     * bitmap (one past the highest live block). The old code only ever
     * *shrank* this value, which could leave it inconsistent with the live
     * extents — e.g. nvme_gcc.img persisted next_data_block=4 while 2034 data
     * blocks were live. A stale-low next_data_block let osfs2_create's append
     * fallback hand out an already-occupied block, corrupting a live file.
     * Recomputing from the bitmap makes the hint correct in both directions. */
    {
        uint32_t hwm = data_start;
        for (uint32_t b = data_start; b < superblock.total_blocks; b++)
            if (blk_bitmap_test(b)) hwm = b + 1;
        superblock.next_data_block = hwm;
    }

    uint32_t data_blks = superblock.total_blocks - data_start;
    uint32_t used_data = superblock.used_blocks - data_start;

    serial_puts("[OsitoFS] Block bitmap built: ");
    serial_putdec(used_data);
    serial_puts("/");
    serial_putdec(data_blks);
    serial_puts(" data blocks used, ");
    serial_putdec(data_blks - used_data);
    serial_puts(" free\n");

    osfs2_hash_build();

    fb_puts("\n OsitoFS v2 [");
    fb_puts(superblock.label);
    fb_puts("] — ");
    fb_putdec(superblock.file_count);
    fb_puts(" file(s), ");
    fb_putdec((uint64_t)(data_blks - used_data) * blk_size / (1024 * 1024));
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
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;

        osfs2_file_t *f = &file_table[i];
        file_count++;

        /* Serial output only (per-file listing slows framebuffer on real HW) */
        serial_puts("  ");
        serial_puts(f->name);
        serial_puts("  size=");
        serial_putdec(f->size);
        if (f->modify_time > 0) {
            serial_puts("  mtime=");
            serial_putdec(f->modify_time);
        }
        if (f->flags & OSFS2_FLAG_GGUF) {
            serial_puts("  model=");
            serial_puts(f->model_name);
            serial_puts("  layers=");
            serial_putdec(f->num_layers);
        }
        serial_puts("\n");
    }

    /* Framebuffer: summary only */
    uint32_t data_blocks_l = superblock.total_blocks - data_start;
    uint32_t used_data_l = superblock.used_blocks - data_start;
    fb_puts("  ");
    fb_putdec(file_count);
    fb_puts(" files, ");
    fb_putdec(used_data_l);
    fb_puts("/");
    fb_putdec(data_blocks_l);
    fb_puts(" blocks used\n");
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
    uint32_t slot = osfs2_name_hash_fn(name);
    while (name_hash[slot] != OSFS2_HASH_EMPTY) {
        uint16_t idx = name_hash[slot];
        if ((file_table[idx].flags & OSFS2_FLAG_VALID) &&
            strcmp(file_table[idx].name, name) == 0)
            return &file_table[idx];
        slot = (slot + 1) & OSFS2_HASH_MASK;
    }
    return NULL;
}

/* Case-insensitive find — for Win32 compat (Windows filenames are CI) */
osfs2_file_t *osfs2_find_ci(const char *name)
{
    if (!mounted || !name) return NULL;
    uint32_t slot = osfs2_name_hash_fn(name);
    while (name_hash[slot] != OSFS2_HASH_EMPTY) {
        uint16_t idx = name_hash[slot];
        if (file_table[idx].flags & OSFS2_FLAG_VALID) {
            const char *a = file_table[idx].name;
            const char *b = name;
            int match = 1;
            while (*a && *b) {
                char ca = *a, cb = *b;
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) { match = 0; break; }
                a++; b++;
            }
            if (match && !*a && !*b) return &file_table[idx];
        }
        slot = (slot + 1) & OSFS2_HASH_MASK;
    }
    return NULL;
}

/* ── Read file data ──────────────────────────────────────────── */

int osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len)
{
    if (!mounted || !file) return -1;
    if (offset + len > file->size) return -1;

    /* Inline files: data stored in model_name[128] field */
    if (file->flags & OSFS2_FLAG_INLINE) {
        memcpy(buf, file->model_name + offset, len);
        return (int)len;
    }

    uint64_t abs_offset = ((uint64_t)file->start_block << blk_shift) + offset;
    int rc = osfs2_part_read(abs_offset, buf, len);
    if (rc < 0) return -1;
    return (int)len;  /* disk_read_bytes returns 0 on success, not byte count */
}

/* ── Read a full block from a file ───────────────────────────── */

int osfs2_read_file_block(osfs2_file_t *file, uint32_t block_index, void *buf)
{
    if (!mounted || !file) return -1;
    if (block_index >= file->block_count) return -1;

    uint32_t abs_block = file->start_block + block_index;
    if (osfs2_read_block_data(abs_block, buf) < 0) return -1;

    /* Verify block CRC if table is loaded */
    if (crc_table && abs_block < OSFS2_MAX_BLOCKS && crc_table[abs_block] != 0) {
        uint32_t calc = osfs2_crc32(buf, blk_size);
        if (calc != crc_table[abs_block]) {
            serial_puts("[OsitoFS] CRC MISMATCH block ");
            serial_putdec(abs_block);
            serial_puts(": stored=");
            serial_puthex(crc_table[abs_block], 8);
            serial_puts(" calc=");
            serial_puthex(calc, 8);
            serial_puts("\n");
            return -2;  /* distinct from -1 (I/O error) */
        }
    }
    return 0;
}

/* ── Verified read: reads file data with per-block CRC check ─── */

int osfs2_read_verified(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len)
{
    if (!mounted || !file || !crc_table) {
        /* No CRC table — fall back to unverified read */
        return osfs2_read(file, offset, buf, len);
    }
    if (offset + len > file->size) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t pos = offset;
    uint64_t remaining = len;

    /* Temporary block buffer for CRC verification */
    void *blk_phys = mem_alloc_aligned(blk_size, 4096);
    if (!blk_phys) return osfs2_read(file, offset, buf, len);
    void *blk_buf = PHYS_TO_VIRT(blk_phys);

    while (remaining > 0) {
        uint32_t blk_idx = (uint32_t)(pos >> blk_shift);
        uint32_t blk_off = (uint32_t)(pos & (blk_size - 1));
        uint32_t abs_block = file->start_block + blk_idx;

        if (osfs2_read_block_data(abs_block, blk_buf) < 0) {
            mem_free_pages(blk_phys, blk_size / 4096);
            return -1;
        }

        /* Verify CRC */
        if (abs_block < OSFS2_MAX_BLOCKS && crc_table[abs_block] != 0) {
            uint32_t calc = osfs2_crc32(blk_buf, blk_size);
            if (calc != crc_table[abs_block]) {
                serial_puts("[OsitoFS] CRC MISMATCH block ");
                serial_putdec(abs_block);
                serial_puts("\n");
                mem_free_pages(blk_phys, blk_size / 4096);
                return -2;
            }
        }

        uint32_t chunk = blk_size - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy(dst, (uint8_t *)blk_buf + blk_off, chunk);

        dst += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    mem_free_pages(blk_phys, blk_size / 4096);
    return (int)len;
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

    uint64_t offset = OSFS2_LAYERIDX_OFF
                    + (uint64_t)slot * sizeof(osfs2_layer_idx_t);
    return osfs2_part_read(offset, li, sizeof(*li));
}

/* ── Write to partition ──────────────────────────────────────── */

static int osfs2_part_write(uint64_t offset, const void *buf, uint64_t len)
{
    return disk_write_bytes(partition_offset + offset, buf, len);
}

/* ── Persist superblock to disk ─────────────────────────────── */

static int osfs2_write_superblock(void)
{
    superblock.crc32 = 0;
    superblock.crc32 = osfs2_crc32(&superblock, sizeof(superblock));
    int rc = osfs2_part_write(0, &superblock, sizeof(superblock));
    if (rc == 0)
        osfs2_part_write(OSFS2_SUPER_BACKUP_OFF, &superblock, sizeof(superblock));
    return rc;
}

/* ── Persist file table to disk ─────────────────────────────── */

static int osfs2_write_file_table(void)
{
    return osfs2_part_write(OSFS2_FILETAB_OFF, file_table, OSFS2_FILETAB_SIZE);
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

    /* Inline small files: store data directly in the file table entry
     * (reusing model_name[128] field, which is unused for non-GGUF files).
     * No block allocation needed — read/write go to the inode itself. */
    if (size <= OSFS2_INLINE_MAX) {
        osfs2_file_t *f = &file_table[slot];
        memset(f, 0, sizeof(*f));
        strcpy(f->name, name);
        f->size = size;
        f->start_block = 0;
        f->block_count = 0;
        f->flags = OSFS2_FLAG_VALID | OSFS2_FLAG_INLINE;
        f->layer_index_slot = 0xFFFF;
        f->create_time = osfs2_get_time();
        f->modify_time = f->create_time;
        osfs2_hash_insert((uint16_t)slot);
        superblock.file_count++;
        osfs2_write_file_table();
        osfs2_write_superblock();
        return f;
    }

    /* Calculate blocks needed */
    uint32_t blocks = (uint32_t)((size + blk_size - 1) >> blk_shift);
    if (blocks == 0) blocks = 1;

    /* Allocate a contiguous run. The in-memory bitmap (rebuilt from every
     * live file's extent on mount) is the AUTHORITATIVE allocator: first-fit
     * over the bitmap is the only path that is guaranteed not to overlap a
     * live file.
     *
     * superblock.next_data_block is treated as a HINT only. It can be stale
     * (observed: next_data_block=4 while 2034 data blocks were live), and
     * blindly appending there used to clobber a live extent. We therefore
     * only fall back to the next_data_block append path when that exact range
     * is verified FREE in the bitmap; otherwise we hard-fail rather than
     * hand out an overlapping range. */
    uint32_t start = blk_bitmap_find_free(blocks);
    if (start && !blk_range_free(start, blocks)) {
        /* Defensive: find_free should only return free runs. If not, treat
         * as "no run found" so we don't corrupt data. */
        serial_puts("[OsitoFS] BUG: find_free returned occupied run, rejecting\n");
        start = 0;
    }
    if (!start) {
        /* No reusable gap from first-fit. Try the next_data_block hint, but
         * ONLY if it does not overlap any live extent. */
        uint32_t hint = superblock.next_data_block;
        if (hint < data_start) hint = data_start;
        if (blk_range_free(hint, blocks)) {
            start = hint;
        } else {
            /* Hint is stale/occupied. Re-derive a true high-water mark from
             * the bitmap (skip every used block) and try to append above it.
             * This recovers cleanly from a corrupted/low next_data_block. */
            uint32_t hwm = data_start;
            for (uint32_t b = data_start; b < superblock.total_blocks; b++)
                if (blk_bitmap_test(b)) hwm = b + 1;
            if (blk_range_free(hwm, blocks)) {
                start = hwm;
            } else {
                serial_puts("[OsitoFS] Not enough contiguous space: need ");
                serial_putdec(blocks);
                serial_puts(" blocks\n");
                return NULL;
            }
        }
    }

    /* Final paranoia check: never hand out a range that intersects a live
     * extent. If this ever trips, fail the create rather than corrupt data. */
    if (!blk_range_free(start, blocks)) {
        serial_puts("[OsitoFS] Refusing to allocate overlapping extent @ block ");
        serial_putdec(start);
        serial_puts("\n");
        return NULL;
    }

    /* Mark blocks as used in bitmap */
    for (uint32_t b = 0; b < blocks; b++)
        blk_bitmap_set(start + b);

    /* Keep next_data_block as a forward hint: advance it past this run if we
     * just extended the high-water mark. (Gaps reused via first-fit do not
     * move it.) */
    if (start + blocks > superblock.next_data_block)
        superblock.next_data_block = start + blocks;

    /* Fill file entry */
    osfs2_file_t *f = &file_table[slot];
    memset(f, 0, sizeof(*f));
    strcpy(f->name, name);
    f->size = size;
    f->start_block = start;
    f->block_count = blocks;
    f->flags = OSFS2_FLAG_VALID;
    f->layer_index_slot = 0xFFFF;
    f->create_time = osfs2_get_time();
    f->modify_time = f->create_time;

    osfs2_hash_insert((uint16_t)slot);

    /* Update superblock */
    superblock.used_blocks += blocks;
    superblock.file_count++;

    /* Persist. If either write fails, we MUST roll back the in-memory
     * state — otherwise `ls` shows a phantom file that doesn't exist on
     * disk and never will (next mount reads the unwritten file table). */
    if (osfs2_write_file_table() < 0 || osfs2_write_superblock() < 0) {
        serial_puts("[OsitoFS] Failed to persist metadata — rolling back\n");
        /* Free the slot. */
        memset(f, 0, sizeof(*f));
        /* Free the bitmap blocks we marked. */
        for (uint32_t b = 0; b < blocks; b++)
            blk_bitmap_clear(start + b);
        superblock.used_blocks -= blocks;
        superblock.file_count--;
        /* Note: not rolling back next_data_block — leaves a small gap,
         * but blk_bitmap_find_free will reuse it on the next allocation. */
        return NULL;
    }
    disk_flush();

    extern int osfs2_verbose;
    if (osfs2_verbose) {
        serial_puts("[OsitoFS] Created '");
        serial_puts(name);
        serial_puts("' size=");
        serial_putdec(size);
        serial_puts(" blocks=");
        serial_putdec(blocks);
        serial_puts(" @ block ");
        serial_putdec(f->start_block);
        serial_puts("\n");
    }

    return f;
}

/* ── Write data to an existing file ─────────────────────────── */

int osfs2_write(osfs2_file_t *file, uint64_t offset, const void *buf, uint64_t len)
{
    if (!mounted || !file || !buf) return -1;

    /* Inline files: write to model_name field, persist via file table */
    if (file->flags & OSFS2_FLAG_INLINE) {
        if (offset + len > OSFS2_INLINE_MAX) return -1;
        memcpy(file->model_name + offset, buf, len);
        if (offset + len > file->size) {
            file->size = offset + len;
            file->modify_time = osfs2_get_time();
        }
        osfs2_write_file_table();
        return 0;
    }

    if (offset + len > (uint64_t)file->block_count << blk_shift) return -1;

    uint64_t abs_offset = ((uint64_t)file->start_block << blk_shift) + offset;
    int ret = osfs2_part_write(abs_offset, buf, len);
    if (ret < 0) return -1;

    /* Invalidate cached CRCs for written blocks (CRC=0 → skip verify on read) */
    if (crc_table) {
        uint32_t first = file->start_block + (uint32_t)(offset >> blk_shift);
        uint32_t last  = file->start_block + (uint32_t)((offset + len - 1) >> blk_shift);
        for (uint32_t b = first; b <= last && b < OSFS2_MAX_BLOCKS; b++)
            crc_table[b] = 0;
    }

    /* Update file size if we wrote past current end */
    if (offset + len > file->size) {
        file->size = offset + len;
        file->modify_time = osfs2_get_time();
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

    osfs2_hash_build();  /* Rebuild after delete (simple and correct) */

    /* Shrink high-water mark if we freed trailing blocks */
    while (superblock.next_data_block > data_start &&
           !blk_bitmap_test(superblock.next_data_block - 1))
        superblock.next_data_block--;

    if (osfs2_write_file_table() < 0 || osfs2_write_superblock() < 0)
        return -1;

    disk_flush();

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
/* Verbose flag — defaults to 0 (quiet). Native builds set it to 1
 * via shell `mount -v` style; WASM keeps it 0 to avoid noisy boot. */
int osfs2_verbose =
#ifdef __EMSCRIPTEN__
    0
#else
    1
#endif
;

uint32_t osfs2_free_blocks(void) {
    if (!mounted) return 0;
    uint32_t total_data = superblock.total_blocks - data_start;
    uint32_t used_data = superblock.used_blocks - data_start;
    /* Clamp at zero — used_data > total_data shows up after osfs2_create
     * over-allocates a tight FS. The display gets clean output and the
     * underflow doesn't propagate to MB-conversion math. */
    return used_data >= total_data ? 0 : total_data - used_data;
}
uint32_t osfs2_get_block_size(void) { return mounted ? blk_size : 0; }

uint32_t osfs2_file_ctime(osfs2_file_t *f) { return f ? f->create_time : 0; }
uint32_t osfs2_file_mtime(osfs2_file_t *f) { return f ? f->modify_time : 0; }

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

/* Return the absolute byte offset of a file's data on the NVMe device.
 * Used by tensor DMA to compute LBA addresses for direct NVMe reads. */
uint64_t osfs2_file_byte_offset(osfs2_file_t *file)
{
    if (!mounted || !file) return 0;
    return partition_offset + ((uint64_t)file->start_block << blk_shift);
}
