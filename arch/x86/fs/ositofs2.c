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
#ifndef __EMSCRIPTEN__
#include "ositofs3.h"
#endif

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
extern uint32_t disk_lba_size(void);
extern uint64_t disk_lba_count(void);
#ifndef __EMSCRIPTEN__
extern void sched_yield(void);
#endif
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── Driver state ────────────────────────────────────────────── */

static uint64_t     partition_offset;  /* Byte offset of OsitoFS partition on NVMe */
static uint64_t     partition_size;
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
static uint32_t file_capacity;
static uint32_t file_table_size;
static uint32_t crc_table_offset;
static uint32_t layer_index_offset;

/* Boot-time epoch from CMOS RTC (set once on mount) */
static uint64_t boot_epoch_sec;

/* ── Block usage bitmap (in-memory, rebuilt on mount) ────────── */
/* Tracks which blocks are in use. Enables block reclamation       */
/* on delete — freed blocks can be reused by subsequent creates.  */

#define BLK_BITMAP_BYTES  (OSFS2_MAX_BLOCKS / 8)  /* 32KB for 262144 blocks */
static uint8_t blk_bitmap[BLK_BITMAP_BYTES];      /* 1 = used, 0 = free */
static uint32_t file_open_refs[OSFS2_MAX_FILE_SLOTS];
static uint64_t file_revisions[OSFS2_MAX_FILE_SLOTS];
static osfs2_journal_record_t journal_record;
static osfs2_journal_commit_t journal_commit;
static volatile uint32_t journal_lock;
static volatile uint32_t mutation_lock;
static uint64_t journal_transaction_id;

extern bool syscall_file_is_mapped(void *file) __attribute__((weak));
static void osfs2_spin_lock(volatile uint32_t *lock);
static void osfs2_spin_unlock(volatile uint32_t *lock);

static const char *osfs2_entry_name(const osfs2_file_t *file)
{
    if (file && (file->flags & OSFS2_FLAG_LONG_NAME) && file->model_name[0])
        return file->model_name;
    return file ? file->name : NULL;
}

static int osfs2_set_entry_name(osfs2_file_t *file, const char *name)
{
    size_t len = strlen(name);
    bool was_long = (file->flags & OSFS2_FLAG_LONG_NAME) != 0;
    if (len >= OSFS2_MODEL_NAME_LEN) return -1;

    memset(file->name, 0, sizeof(file->name));
    file->flags &= ~OSFS2_FLAG_LONG_NAME;
    if (len < OSFS2_NAME_LEN) {
        strcpy(file->name, name);
        if (was_long) memset(file->model_name, 0, sizeof(file->model_name));
        return 0;
    }
    if (file->flags & (OSFS2_FLAG_INLINE | OSFS2_FLAG_GGUF)) return -1;

    memcpy(file->model_name, name, len + 1);
    file->flags |= OSFS2_FLAG_LONG_NAME;

    const char hex[] = "0123456789abcdef";
    uint32_t crc = osfs2_crc32(name, len);
    size_t prefix = OSFS2_NAME_LEN - 10;
    memcpy(file->name, name, prefix);
    file->name[prefix] = '~';
    for (int i = 0; i < 8; i++)
        file->name[prefix + 1 + i] = hex[(crc >> (28 - i * 4)) & 0xF];
    return 0;
}

static int osfs2_file_slot(const osfs2_file_t *file)
{
    if (!file_table || !file) return -1;
    uintptr_t base = (uintptr_t)file_table;
    uintptr_t address = (uintptr_t)file;
    uintptr_t bytes = sizeof(*file_table) * file_capacity;
    if (address < base || address >= base + bytes ||
        (address - base) % sizeof(*file_table))
        return -1;
    return (int)((address - base) / sizeof(*file_table));
}

int osfs2_file_retain(void *opaque)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_retain(opaque);
#endif
    osfs2_spin_lock(&mutation_lock);
    int slot = osfs2_file_slot((osfs2_file_t *)opaque);
    if (slot < 0 || !(file_table[slot].flags & OSFS2_FLAG_VALID) ||
        file_open_refs[slot] == UINT32_MAX) {
        osfs2_spin_unlock(&mutation_lock);
        return -1;
    }
    file_open_refs[slot]++;
    osfs2_spin_unlock(&mutation_lock);
    return 0;
}

void osfs2_file_release(void *opaque)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) {
        osfs3_file_release(opaque);
        return;
    }
#endif
    osfs2_spin_lock(&mutation_lock);
    int slot = osfs2_file_slot((osfs2_file_t *)opaque);
    if (slot >= 0 && file_open_refs[slot]) file_open_refs[slot]--;
    osfs2_spin_unlock(&mutation_lock);
}

static bool osfs2_file_busy(osfs2_file_t *file)
{
    int slot = osfs2_file_slot(file);
    if (slot < 0) return true;
    if (file_open_refs[slot]) return true;
    return syscall_file_is_mapped && syscall_file_is_mapped(file);
}

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

static int blk_range_free(uint32_t start, uint32_t count);  /* fwd decl (fsck) */

/* Rebuild bitmap from file table (called on mount) */
static void blk_bitmap_rebuild(void)
{
    memset(blk_bitmap, 0, sizeof(blk_bitmap));

    /* Metadata blocks always used */
    for (uint32_t i = 0; i < data_start; i++)
        blk_bitmap_set(i);

    /* Sanitize malformed entries before any name, extent, or size is trusted.
     * Legacy 64-byte names are truncated in memory rather than discarded. */
    for (uint32_t i = 0; i < file_capacity; i++) {
        osfs2_file_t *f = &file_table[i];
        if (!(f->flags & OSFS2_FLAG_VALID)) continue;
        bool terminated = false;
        for (uint32_t n = 0; n < OSFS2_NAME_LEN; n++)
            if (!f->name[n]) terminated = true;
        if (!terminated) f->name[OSFS2_NAME_LEN - 1] = '\0';

        bool valid = true;
        if (f->flags & OSFS2_FLAG_LONG_NAME) {
            bool long_terminated = false;
            for (uint32_t n = 0; n < OSFS2_MODEL_NAME_LEN; n++)
                if (!f->model_name[n]) long_terminated = true;
            if (!long_terminated)
                f->model_name[OSFS2_MODEL_NAME_LEN - 1] = '\0';
            if (!f->model_name[0] ||
                (f->flags & (OSFS2_FLAG_INLINE | OSFS2_FLAG_GGUF)))
                valid = false;
        }
        if (f->layer_index_slot != 0xFFFF &&
            f->layer_index_slot >= OSFS2_MAX_MODELS)
            valid = false;
        if (f->flags & OSFS2_FLAG_INLINE) {
            if (f->size > OSFS2_INLINE_MAX || f->start_block || f->block_count)
                valid = false;
        } else if (f->block_count) {
            uint64_t end = (uint64_t)f->start_block + f->block_count;
            if (f->start_block < data_start || end > superblock.total_blocks ||
                f->size > (uint64_t)f->block_count * blk_size)
                valid = false;
        } else if (f->size) {
            valid = false;
        }
        if (!valid) {
            serial_puts("[OsitoFS] fsck: dropping malformed file-table entry\n");
            memset(f, 0, sizeof(*f));
        }
    }

    /* Mark each valid file's blocks — in TWO passes so stale/duplicate extents
     * can't keep a live file's data blocks. This is a mount-time fsck (run once,
     * mount-only). Motivating bug: an old allocator handed UnrealTournament.log
     * (size 0) the SAME block as Entry.unr (11617 B); the engine reopened that
     * stale log entry and its write clobbered Entry.unr's package data, so a
     * later read of Entry.unr returned "Log:" text → "ReadFile beyond EOF" →
     * an unrecoverable C++ throw cascade that reset the session at the menu. */

    /* Pass 1: real files (size>0) are authoritative. On overlap (corruption),
     * drop the LATER claimant so it can't keep a live file's blocks. */
    for (uint32_t i = 0; i < file_capacity; i++) {
        osfs2_file_t *f = &file_table[i];
        if (!(f->flags & OSFS2_FLAG_VALID)) continue;
        if (f->size == 0 || f->block_count == 0) continue;
        if (!blk_range_free(f->start_block, f->block_count)) {
            serial_puts("[OsitoFS] fsck: '");
            serial_puts(f->name);
            serial_puts("' extent overlaps a live file — dropping (corrupt)\n");
            f->flags &= ~OSFS2_FLAG_VALID;
            continue;
        }
        for (uint32_t b = 0; b < f->block_count; b++)
            blk_bitmap_set(f->start_block + b);
    }

    /* Pass 2: empty files (size==0) that still CLAIM data blocks. A size-0 file
     * whose block overlaps a real file is stale corruption (the log-on-Entry.unr
     * case): writing it would clobber the real file. Drop it (no data to lose) +
     * release its block; the owner re-creates it cleanly via the fixed allocator. */
    for (uint32_t i = 0; i < file_capacity; i++) {
        osfs2_file_t *f = &file_table[i];
        if (!(f->flags & OSFS2_FLAG_VALID)) continue;
        if (f->size != 0 || f->block_count == 0) continue;
        if (!blk_range_free(f->start_block, f->block_count)) {
            serial_puts("[OsitoFS] fsck: empty '");
            serial_puts(f->name);
            serial_puts("' claims a live block — dropping (stale)\n");
            f->flags &= ~OSFS2_FLAG_VALID;
            f->start_block = 0;
            f->block_count = 0;
            continue;
        }
        for (uint32_t b = 0; b < f->block_count; b++)
            blk_bitmap_set(f->start_block + b);
    }

    /* Recompute counters from the file table and bitmap. Mount-time overlap
     * repair can invalidate entries, so the persisted count may be stale. */
    uint32_t used = 0;
    uint32_t valid = 0;
    for (uint32_t i = 0; i < file_capacity; i++)
        if (file_table[i].flags & OSFS2_FLAG_VALID) valid++;
    for (uint32_t b = data_start; b < superblock.total_blocks; b++) {
        if (blk_bitmap_test(b)) used++;
    }
    superblock.used_blocks = used + data_start;
    superblock.file_count = valid;
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

#define OSFS2_HASH_SLOTS  32768
#define OSFS2_HASH_MASK   (OSFS2_HASH_SLOTS - 1)
#define OSFS2_HASH_EMPTY  0xFFFF

static uint16_t name_hash[OSFS2_HASH_SLOTS];
static volatile uint32_t name_hash_generation;

typedef struct {
    uint16_t file_index;
    uint16_t prefix_len;
} osfs2_dir_hash_entry_t;

static osfs2_dir_hash_entry_t dir_hash[OSFS2_HASH_SLOTS];

static char osfs2_path_fold(char c)
{
    if (c == '/') return '\\';
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static uint32_t osfs2_path_hash_fn(const char *path, uint32_t len)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= (uint8_t)osfs2_path_fold(path[i]);
        hash *= 16777619u;
    }
    return hash & OSFS2_HASH_MASK;
}

static bool osfs2_path_equal_n(const char *a, const char *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (!a[i] || !b[i] ||
            osfs2_path_fold(a[i]) != osfs2_path_fold(b[i])) return false;
    return true;
}

static void osfs2_dir_hash_insert(uint16_t file_index, uint16_t prefix_len)
{
    const char *path = osfs2_entry_name(&file_table[file_index]);
    uint32_t slot = osfs2_path_hash_fn(path, prefix_len);
    for (uint32_t probe = 0; probe < OSFS2_HASH_SLOTS; probe++) {
        osfs2_dir_hash_entry_t *entry = &dir_hash[slot];
        if (entry->file_index == OSFS2_HASH_EMPTY) {
            entry->file_index = file_index;
            entry->prefix_len = prefix_len;
            return;
        }
        const char *existing =
            osfs2_entry_name(&file_table[entry->file_index]);
        if (entry->prefix_len == prefix_len &&
            osfs2_path_equal_n(existing, path, prefix_len)) return;
        slot = (slot + 1) & OSFS2_HASH_MASK;
    }
}

static uint32_t osfs2_name_hash_fn(const char *name)
{
    char lower[OSFS2_MODEL_NAME_LEN];
    int len = 0;
    while (name[len] && len < OSFS2_MODEL_NAME_LEN - 1) {
        lower[len] = osfs2_path_fold(name[len]);
        len++;
    }
    lower[len] = '\0';
    return osfs2_crc32(lower, (size_t)len) & OSFS2_HASH_MASK;
}

static bool osfs2_name_equal_ci(const char *a, const char *b)
{
    while (*a && *b && osfs2_path_fold(*a) == osfs2_path_fold(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static void osfs2_hash_insert(uint16_t idx)
{
    uint32_t slot = osfs2_name_hash_fn(osfs2_entry_name(&file_table[idx]));
    while (name_hash[slot] != OSFS2_HASH_EMPTY)
        slot = (slot + 1) & OSFS2_HASH_MASK;
    name_hash[slot] = idx;
}

static void osfs2_hash_build(void)
{
    __atomic_add_fetch(&name_hash_generation, 1, __ATOMIC_ACQ_REL);
    for (uint32_t i = 0; i < OSFS2_HASH_SLOTS; i++) {
        name_hash[i] = OSFS2_HASH_EMPTY;
        dir_hash[i].file_index = OSFS2_HASH_EMPTY;
        dir_hash[i].prefix_len = 0;
    }
    for (uint32_t i = 0; i < file_capacity; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;
        osfs2_hash_insert((uint16_t)i);
        const char *path = osfs2_entry_name(&file_table[i]);
        for (uint32_t j = 1; path[j]; j++) {
            if ((path[j] == '\\' || path[j] == '/') &&
                path[j - 1] != '\\' && path[j - 1] != '/')
                osfs2_dir_hash_insert((uint16_t)i, (uint16_t)j);
        }
    }
    __atomic_add_fetch(&name_hash_generation, 1, __ATOMIC_RELEASE);
}

/* ── Read from partition ─────────────────────────────────────── */

static int osfs2_part_read(uint64_t offset, void *buf, uint64_t len)
{
    if (offset > partition_size || len > partition_size - offset ||
        partition_offset > UINT64_MAX - offset)
        return -1;
    return disk_read_bytes(partition_offset + offset, buf, len);
}

static int osfs2_part_write(uint64_t offset, const void *buf, uint64_t len)
{
    if (offset > partition_size || len > partition_size - offset ||
        partition_offset > UINT64_MAX - offset)
        return -1;
    return disk_write_bytes(partition_offset + offset, buf, len);
}

static bool osfs2_super_valid(const osfs2_super_t *value)
{
    if (!value || value->magic != OSFS2_MAGIC ||
        !osfs2_supported_version(value->version) ||
        !osfs2_valid_block_size(value->block_size) ||
        !osfs2_valid_layout(value))
        return false;
    osfs2_super_t copy = *value;
    uint32_t expected = copy.crc32;
    copy.crc32 = 0;
    return expected == osfs2_crc32(&copy, sizeof(copy));
}

static bool osfs2_journal_record_valid(osfs2_journal_record_t *record)
{
    if (!record || record->magic != OSFS2_JOURNAL_MAGIC ||
        record->version != OSFS2_JOURNAL_VERSION ||
        record->entry_count == 0 ||
        record->entry_count > OSFS2_JOURNAL_MAX_ENTRIES ||
        record->page_count == 0 ||
        record->page_count > OSFS2_JOURNAL_MAX_PAGES ||
        record->operation < OSFS2_JOURNAL_OP_RENAME ||
        record->operation > OSFS2_JOURNAL_OP_REPLACE ||
        !osfs2_super_valid(&record->before_super) ||
        !osfs2_super_valid(&record->after_super))
        return false;
    if (record->before_super.version != record->after_super.version ||
        record->before_super.block_size != record->after_super.block_size ||
        memcmp(record->before_super.uuid, record->after_super.uuid,
               sizeof(record->before_super.uuid)) != 0 ||
        record->after_super.total_blocks <=
            osfs2_layout_data_start_blk(&record->after_super) ||
        record->after_super.total_blocks > OSFS2_MAX_BLOCKS ||
        (uint64_t)record->after_super.total_blocks *
            record->after_super.block_size > partition_size)
        return false;
    for (uint32_t i = 0; i < record->page_count; i++) {
        if (record->pages[i] >= osfs2_layout_filetab_size(
                &record->after_super) / OSFS2_METADATA_PAGE_SIZE)
            return false;
        if (i && record->pages[i] == record->pages[0]) return false;
    }
    for (uint32_t i = 0; i < record->entry_count; i++) {
        if (record->slots[i] >= osfs2_layout_max_files(
                &record->after_super)) return false;
        uint32_t page = (record->slots[i] * sizeof(osfs2_file_t)) /
                        OSFS2_METADATA_PAGE_SIZE;
        bool found = false;
        for (uint32_t j = 0; j < record->page_count; j++)
            if (record->pages[j] == page) found = true;
        if (!found || (i && record->slots[i] == record->slots[0])) return false;

        const osfs2_file_t *after = NULL;
        for (uint32_t j = 0; j < record->page_count; j++) {
            if (record->pages[j] != page) continue;
            uint32_t in_page = (record->slots[i] * sizeof(osfs2_file_t)) %
                               OSFS2_METADATA_PAGE_SIZE;
            after = (const osfs2_file_t *)(record->after_pages[j] + in_page);
        }
        if (!after) return false;
        if (after->flags & OSFS2_FLAG_VALID) {
            bool terminated = false;
            for (uint32_t n = 0; n < OSFS2_NAME_LEN; n++)
                if (!after->name[n]) terminated = true;
            if (!terminated) return false;
            if (after->flags & OSFS2_FLAG_INLINE) {
                if (after->size > OSFS2_INLINE_MAX || after->start_block ||
                    after->block_count) return false;
            } else if (after->block_count) {
                uint64_t end = (uint64_t)after->start_block +
                               after->block_count;
                if (after->start_block <
                        osfs2_layout_data_start_blk(
                            &record->after_super) ||
                    end > record->after_super.total_blocks ||
                    after->size > (uint64_t)after->block_count *
                                      record->after_super.block_size)
                    return false;
            } else if (after->size) {
                return false;
            }
        }
    }
    uint32_t expected = record->record_crc32;
    record->record_crc32 = 0;
    uint32_t actual = osfs2_crc32(record, sizeof(*record));
    record->record_crc32 = expected;
    return expected == actual;
}

static bool osfs2_journal_commit_valid(osfs2_journal_commit_t *commit)
{
    if (!commit || commit->magic != OSFS2_JOURNAL_COMMIT_MAGIC ||
        commit->version != OSFS2_JOURNAL_VERSION)
        return false;
    uint32_t expected = commit->commit_crc32;
    commit->commit_crc32 = 0;
    uint32_t actual = osfs2_crc32(commit, sizeof(*commit));
    commit->commit_crc32 = expected;
    return expected == actual;
}

static int osfs2_journal_apply(const osfs2_journal_record_t *record)
{
    for (uint32_t i = 0; i < record->page_count; i++) {
        uint64_t offset = OSFS2_FILETAB_OFF +
            (uint64_t)record->pages[i] * OSFS2_METADATA_PAGE_SIZE;
        if (osfs2_part_write(offset, record->after_pages[i],
                             OSFS2_METADATA_PAGE_SIZE) < 0)
            return -1;
    }
    if (osfs2_part_write(0, &record->after_super,
                         sizeof(record->after_super)) < 0 ||
        osfs2_part_write(OSFS2_SUPER_BACKUP_OFF, &record->after_super,
                         sizeof(record->after_super)) < 0)
        return -1;
    return disk_flush();
}

static int osfs2_journal_clear_commit(void)
{
    memset(&journal_commit, 0, sizeof(journal_commit));
    if (osfs2_part_write(OSFS2_JOURNAL_COMMIT_OFF, &journal_commit,
                         sizeof(journal_commit)) < 0)
        return -1;
    return disk_flush();
}

/* Replay committed metadata before trusting either superblock or the file
 * table. A torn commit marker is uncommitted by definition and is discarded. */
static int osfs2_journal_recover(void)
{
    if (osfs2_part_read(OSFS2_JOURNAL_COMMIT_OFF, &journal_commit,
                        sizeof(journal_commit)) < 0)
        return -1;
    if (journal_commit.magic != OSFS2_JOURNAL_COMMIT_MAGIC)
        return 0;
    if (!osfs2_journal_commit_valid(&journal_commit)) {
        serial_puts("[OsitoFS] Discarding torn journal commit\n");
        return osfs2_journal_clear_commit();
    }
    if (osfs2_part_read(OSFS2_JOURNAL_RECORD_OFF, &journal_record,
                        sizeof(journal_record)) < 0 ||
        !osfs2_journal_record_valid(&journal_record) ||
        journal_record.transaction_id != journal_commit.transaction_id ||
        journal_record.record_crc32 != journal_commit.record_crc32) {
        serial_puts("[OsitoFS] Committed journal record is invalid\n");
        return -1;
    }
    osfs2_super_t current_super;
    bool have_current = osfs2_part_read(0, &current_super,
                                        sizeof(current_super)) == 0 &&
                        osfs2_super_valid(&current_super);
    if (!have_current)
        have_current = osfs2_part_read(OSFS2_SUPER_BACKUP_OFF, &current_super,
                                       sizeof(current_super)) == 0 &&
                       osfs2_super_valid(&current_super);
    if (have_current &&
        (current_super.version != journal_record.after_super.version ||
         current_super.block_size != journal_record.after_super.block_size ||
         memcmp(current_super.uuid, journal_record.after_super.uuid,
                sizeof(current_super.uuid)) != 0)) {
        serial_puts("[OsitoFS] Journal belongs to another filesystem\n");
        return -1;
    }
    serial_puts("[OsitoFS] Replaying committed metadata transaction\n");
    if (osfs2_journal_apply(&journal_record) < 0)
        return -1;
    return osfs2_journal_clear_commit();
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

int osfs2_mount(uint64_t part_offset, uint64_t part_bytes)
{
    partition_offset = part_offset;
    uint64_t device_bytes = disk_lba_count() * (uint64_t)disk_lba_size();
    if (!part_bytes) {
        if (part_offset >= device_bytes) return -1;
        part_bytes = device_bytes - part_offset;
    }
    if (part_offset > device_bytes || part_bytes > device_bytes - part_offset ||
        part_bytes < OSFS2_DATA_OFF + OSFS2_MIN_BLOCK_SIZE)
        return -1;
    partition_size = part_bytes;
    mounted = false;

    serial_puts("[OsitoFS] Mounting at partition offset ");
    serial_puthex(part_offset, 16);
    serial_puts("\n");

    if (osfs2_journal_recover() < 0) {
        serial_puts("[OsitoFS] Journal recovery failed\n");
        return -1;
    }

    /* Read superblock (first 512 bytes of block 0) */
    if (osfs2_part_read(0, &superblock, sizeof(superblock)) < 0) {
        serial_puts("[OsitoFS] Failed to read superblock\n");
        return -1;
    }

    if (!osfs2_super_valid(&superblock)) {
        serial_puts("[OsitoFS] Primary superblock invalid, trying backup...\n");
        if (osfs2_part_read(OSFS2_SUPER_BACKUP_OFF, &superblock,
                            sizeof(superblock)) < 0 ||
            !osfs2_super_valid(&superblock)) {
            serial_puts("[OsitoFS] Backup superblock also invalid\n");
            return -1;
        }
        serial_puts("[OsitoFS] Using backup superblock\n");
    }
    boot_epoch_sec = rtc_to_epoch();

    /* Set runtime block size from superblock */
    if (!osfs2_valid_block_size(superblock.block_size)) {
        serial_puts("[OsitoFS] Invalid block size: ");
        serial_putdec(superblock.block_size);
        serial_puts("\n");
        return -1;
    }
    if (!osfs2_valid_layout(&superblock)) {
        serial_puts("[OsitoFS] Invalid metadata layout\n");
        return -1;
    }
    blk_size = superblock.block_size;
    blk_shift = osfs2_block_shift(blk_size);
    data_start = osfs2_layout_data_start_blk(&superblock);
    file_capacity = osfs2_layout_max_files(&superblock);
    file_table_size = osfs2_layout_filetab_size(&superblock);
    crc_table_offset = osfs2_layout_crctab_off(&superblock);
    layer_index_offset = osfs2_layout_layeridx_off(&superblock);
    if (superblock.total_blocks <= data_start ||
        superblock.total_blocks > OSFS2_MAX_BLOCKS ||
        (uint64_t)superblock.total_blocks * blk_size > partition_size) {
        serial_puts("[OsitoFS] Invalid filesystem geometry\n");
        return -1;
    }

    serial_puts("[OsitoFS] Superblock OK: label=\"");
    serial_puts(superblock.label);
    serial_puts("\", files=");
    serial_putdec(superblock.file_count);
    serial_puts(", blocks=");
    serial_putdec(superblock.total_blocks);
    serial_puts(", blk_size=");
    serial_putdec(blk_size);
    serial_puts(", slots=");
    serial_putdec(file_capacity);
    serial_puts("\n");

    /* Read the versioned file table. Stored as upper-half virt
     * so it stays reachable after the lower-half identity map is gone
     * from user PML4s. */
    void *ft_phys = mem_alloc_aligned(file_table_size, 4096);
    if (!ft_phys) {
        serial_puts("[OsitoFS] Failed to allocate file table\n");
        return -1;
    }
    file_table = (osfs2_file_t *)PHYS_TO_VIRT(ft_phys);
    memset(file_open_refs, 0, sizeof(file_open_refs));

    if (osfs2_part_read(OSFS2_FILETAB_OFF, file_table, file_table_size) < 0) {
        serial_puts("[OsitoFS] Failed to read file table\n");
        return -1;
    }

    /* The many-file layout spends the full metadata region on file entries. */
    crc_table = NULL;
    if (osfs2_layout_has_crc_table(&superblock)) {
        void *crc_phys = mem_alloc_aligned(OSFS2_CRCTAB_SIZE, 4096);
        if (crc_phys) {
            crc_table = (uint32_t *)PHYS_TO_VIRT(crc_phys);
            if (osfs2_part_read(crc_table_offset, crc_table,
                                OSFS2_CRCTAB_SIZE) < 0) {
                serial_puts("[OsitoFS] CRC table read failed (verification disabled)\n");
                mem_free_pages(crc_phys, OSFS2_CRCTAB_SIZE / 4096);
                crc_table = NULL;
            }
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
        superblock.crc32 = 0;
        superblock.crc32 = osfs2_crc32(&superblock, sizeof(superblock));
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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) {
        osfs3_list_dir(osfs3_resolve_path(""));
        return;
    }
#endif
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
    for (uint32_t i = 0; i < file_capacity; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;

        osfs2_file_t *f = &file_table[i];
        file_count++;

        /* Serial output only (per-file listing slows framebuffer on real HW) */
        serial_puts("  ");
        serial_puts(osfs2_entry_name(f));
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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_find_first(pattern, start_idx);
#endif
    if (!mounted) return -1;
    for (int i = start_idx; i < (int)file_capacity; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) continue;
        const char *name = osfs2_entry_name(&file_table[i]);
        for (const char *p = name; *p; p++)
            if (*p == '\\' || *p == '/') name = p + 1;
        if (osfs2_wildcard_match(pattern, name)) return i;
    }
    return -1;
}

/* Get file entry by index */
osfs2_file_t *osfs2_get_file(int index)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return (osfs2_file_t *)osfs3_get_file(index);
#endif
    if (index < 0 || index >= (int)file_capacity) return NULL;
    if (!(file_table[index].flags & OSFS2_FLAG_VALID)) return NULL;
    return &file_table[index];
}

/* ── Find file by name ───────────────────────────────────────── */

osfs2_file_t *osfs2_find(const char *name)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return (osfs2_file_t *)osfs3_find(name);
#endif
    if (!mounted) return NULL;
    uint32_t slot = osfs2_name_hash_fn(name);
    while (name_hash[slot] != OSFS2_HASH_EMPTY) {
        uint16_t idx = name_hash[slot];
        if ((file_table[idx].flags & OSFS2_FLAG_VALID) &&
            strcmp(osfs2_entry_name(&file_table[idx]), name) == 0)
            return &file_table[idx];
        slot = (slot + 1) & OSFS2_HASH_MASK;
    }
    return NULL;
}

/* Case-insensitive find — for Win32 compat (Windows filenames are CI) */
osfs2_file_t *osfs2_find_exact_ci(const char *name)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return (osfs2_file_t *)osfs3_find_ci(name);
#endif
    if (!mounted || !name) return NULL;

    for (;;) {
        uint32_t generation = __atomic_load_n(&name_hash_generation,
                                               __ATOMIC_ACQUIRE);
        if (generation & 1U) continue;

        osfs2_file_t *result = NULL;
        uint32_t slot = osfs2_name_hash_fn(name);
        while (name_hash[slot] != OSFS2_HASH_EMPTY) {
            uint16_t idx = name_hash[slot];
            if ((file_table[idx].flags & OSFS2_FLAG_VALID) &&
                osfs2_name_equal_ci(osfs2_entry_name(&file_table[idx]), name)) {
                result = &file_table[idx];
                break;
            }
            slot = (slot + 1) & OSFS2_HASH_MASK;
        }

        /* A writer marks the generation odd while rebuilding. Retry against
         * the new table instead of falling back to an O(n) inode scan. */
        uint32_t current = __atomic_load_n(&name_hash_generation,
                                            __ATOMIC_ACQUIRE);
        if (generation == current) return result;
    }
}

osfs2_file_t *osfs2_find_ci(const char *name)
{
    osfs2_file_t *file = osfs2_find_exact_ci(name);
    if (file || !name) return file;
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return NULL;
#endif

    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base != name ? osfs2_find_exact_ci(base) : NULL;
}

bool osfs2_directory_exists_ci(const char *directory)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_directory_exists_ci(directory);
#endif
    if (!mounted || !directory) return false;
    while (*directory == '\\' || *directory == '/') directory++;
    uint32_t len = 0;
    while (directory[len]) len++;
    while (len && (directory[len - 1] == '\\' || directory[len - 1] == '/'))
        len--;
    if (!len || len > 0xFFFFU) return false;

    uint32_t slot = osfs2_path_hash_fn(directory, len);
    for (uint32_t probe = 0; probe < OSFS2_HASH_SLOTS; probe++) {
        osfs2_dir_hash_entry_t *entry = &dir_hash[slot];
        if (entry->file_index == OSFS2_HASH_EMPTY) return false;
        const char *stored = osfs2_entry_name(&file_table[entry->file_index]);
        if (entry->prefix_len == len &&
            osfs2_path_equal_n(stored, directory, len)) return true;
        slot = (slot + 1) & OSFS2_HASH_MASK;
    }
    return false;
}

/* ── Read file data ──────────────────────────────────────────── */

int osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return osfs3_read_file(file, offset, buf, len);
#endif
    if (!mounted || !file || (!buf && len) || len > 0x7FFFFFFFULL) return -1;
    if (offset > file->size || len > file->size - offset) return -1;

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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return osfs3_read_file_block(file, block_index, buf);
#endif
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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return osfs3_read_file(file, offset, buf, len);
#endif
    if (!mounted || !file || !crc_table) {
        /* No CRC table — fall back to unverified read */
        return osfs2_read(file, offset, buf, len);
    }
    if ((file->flags & OSFS2_FLAG_INLINE) || len > 0x7FFFFFFFULL)
        return osfs2_read(file, offset, buf, len);
    if (offset > file->size || len > file->size - offset) return -1;

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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return NULL;
#endif
    if (!mounted) return NULL;

    for (uint32_t i = 0; i < file_capacity; i++) {
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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return -1;
#endif
    if (!mounted || !osfs2_layout_has_layer_index(&superblock) ||
        slot >= OSFS2_MAX_MODELS) return -1;

    uint64_t offset = layer_index_offset
                    + (uint64_t)slot * sizeof(osfs2_layer_idx_t);
    return osfs2_part_read(offset, li, sizeof(*li));
}

static void osfs2_spin_lock(volatile uint32_t *lock)
{
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
#ifndef __EMSCRIPTEN__
        __asm__ volatile ("pause");
        sched_yield();
#else
        __asm__ volatile ("" ::: "memory");
#endif
    }
}

static void osfs2_spin_unlock(volatile uint32_t *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static const osfs2_file_t *osfs2_transaction_entry(
    uint32_t slot, const uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES], uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (slots[i] == slot) return &after[i];
    return &file_table[slot];
}

static bool osfs2_entry_has_extent(const osfs2_file_t *entry)
{
    return (entry->flags & OSFS2_FLAG_VALID) &&
           !(entry->flags & OSFS2_FLAG_INLINE) && entry->block_count;
}

static bool osfs2_same_extent(const osfs2_file_t *a,
                              const osfs2_file_t *b)
{
    return osfs2_entry_has_extent(a) && osfs2_entry_has_extent(b) &&
           a->start_block == b->start_block &&
           a->block_count == b->block_count;
}

static bool osfs2_hashed_name_changed(const osfs2_file_t *before,
                                      const osfs2_file_t *after)
{
    bool before_valid = (before->flags & OSFS2_FLAG_VALID) != 0;
    bool after_valid = (after->flags & OSFS2_FLAG_VALID) != 0;
    if (before_valid != after_valid) return true;
    if (!before_valid) return false;
    return strcmp(osfs2_entry_name(before), osfs2_entry_name(after)) != 0;
}

static int osfs2_transaction_super(
    osfs2_super_t *result,
    const uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES], uint32_t count)
{
    *result = superblock;
    uint64_t files = result->file_count;
    uint64_t used = result->used_blocks;
    uint32_t old_hwm = result->next_data_block;
    uint32_t max_after_end = data_start;
    bool may_lower_hwm = false;

    for (uint32_t i = 0; i < count; i++) {
        const osfs2_file_t *before = &file_table[slots[i]];
        const osfs2_file_t *next = &after[i];
        bool before_valid = (before->flags & OSFS2_FLAG_VALID) != 0;
        bool after_valid = (next->flags & OSFS2_FLAG_VALID) != 0;
        uint32_t before_blocks = osfs2_entry_has_extent(before)
                               ? before->block_count : 0;
        uint32_t after_blocks = osfs2_entry_has_extent(next)
                              ? next->block_count : 0;

        if (after_blocks) {
            uint64_t end = (uint64_t)next->start_block + after_blocks;
            if (next->start_block < data_start ||
                end > result->total_blocks) return -1;
            if (end > max_after_end) max_after_end = (uint32_t)end;
        }

        if (before_valid) {
            if (!files) return -1;
            files--;
        }
        if (after_valid) files++;
        if (used < before_blocks) return -1;
        used -= before_blocks;
        used += after_blocks;
        if (files > file_capacity || used > result->total_blocks) return -1;

        if (osfs2_entry_has_extent(before) &&
            !osfs2_same_extent(before, next)) {
            uint64_t before_end = (uint64_t)before->start_block +
                                  before->block_count;
            if (before_end >= old_hwm) may_lower_hwm = true;
        }
    }

    uint32_t hwm = old_hwm < data_start ? data_start : old_hwm;
    if (max_after_end > hwm) hwm = max_after_end;
    if (may_lower_hwm && max_after_end < old_hwm) {
        hwm = data_start;
        for (uint32_t slot = 0; slot < file_capacity; slot++) {
            const osfs2_file_t *entry = osfs2_transaction_entry(
                slot, slots, after, count);
            if (!osfs2_entry_has_extent(entry)) continue;
            uint64_t end = (uint64_t)entry->start_block + entry->block_count;
            if (entry->start_block < data_start ||
                end > result->total_blocks) return -1;
            if (end > hwm) hwm = (uint32_t)end;
        }
    }

    result->file_count = (uint32_t)files;
    result->used_blocks = (uint32_t)used;
    result->next_data_block = hwm;
    result->crc32 = 0;
    result->crc32 = osfs2_crc32(result, sizeof(*result));
    return 0;
}

static int osfs2_commit_metadata(
    uint32_t operation,
    const uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES], uint32_t count)
{
    if (!mounted || !count || count > OSFS2_JOURNAL_MAX_ENTRIES)
        return -1;
    for (uint32_t i = 0; i < count; i++)
        if (slots[i] >= file_capacity ||
            (i && slots[i] == slots[0])) return -1;

    osfs2_spin_lock(&journal_lock);
    memset(&journal_record, 0, sizeof(journal_record));
    journal_record.magic = OSFS2_JOURNAL_MAGIC;
    journal_record.version = OSFS2_JOURNAL_VERSION;
    journal_record.operation = operation;
    journal_record.entry_count = count;
    journal_record.transaction_id = ++journal_transaction_id;
    journal_record.before_super = superblock;
    for (uint32_t i = 0; i < count; i++) {
        journal_record.slots[i] = slots[i];
        journal_record.before_entries[i] = file_table[slots[i]];
        uint32_t page = (slots[i] * sizeof(osfs2_file_t)) /
                        OSFS2_METADATA_PAGE_SIZE;
        uint32_t page_slot = journal_record.page_count;
        for (uint32_t j = 0; j < journal_record.page_count; j++)
            if (journal_record.pages[j] == page) page_slot = j;
        if (page_slot == journal_record.page_count) {
            journal_record.pages[page_slot] = page;
            memcpy(journal_record.after_pages[page_slot],
                   (uint8_t *)file_table +
                       (uint64_t)page * OSFS2_METADATA_PAGE_SIZE,
                   OSFS2_METADATA_PAGE_SIZE);
            journal_record.page_count++;
        }
        uint32_t in_page = (slots[i] * sizeof(osfs2_file_t)) %
                           OSFS2_METADATA_PAGE_SIZE;
        memcpy(journal_record.after_pages[page_slot] + in_page, &after[i],
               sizeof(osfs2_file_t));
    }
    if (osfs2_transaction_super(&journal_record.after_super, slots, after,
                                count) < 0) {
        osfs2_spin_unlock(&journal_lock);
        return -1;
    }
    journal_record.record_crc32 = 0;
    journal_record.record_crc32 = osfs2_crc32(&journal_record,
                                              sizeof(journal_record));

    int result = osfs2_journal_clear_commit();
    if (result == 0)
        result = osfs2_part_write(OSFS2_JOURNAL_RECORD_OFF, &journal_record,
                                  sizeof(journal_record));
    if (result == 0)
        result = disk_flush();

    if (result == 0) {
        memset(&journal_commit, 0, sizeof(journal_commit));
        journal_commit.magic = OSFS2_JOURNAL_COMMIT_MAGIC;
        journal_commit.version = OSFS2_JOURNAL_VERSION;
        journal_commit.transaction_id = journal_record.transaction_id;
        journal_commit.record_crc32 = journal_record.record_crc32;
        journal_commit.commit_crc32 = 0;
        journal_commit.commit_crc32 = osfs2_crc32(&journal_commit,
                                                  sizeof(journal_commit));
        result = osfs2_part_write(OSFS2_JOURNAL_COMMIT_OFF, &journal_commit,
                                  sizeof(journal_commit));
    }
    if (result == 0)
        result = disk_flush();
    if (result == 0)
        result = osfs2_journal_apply(&journal_record);

    if (result == 0) {
        bool rebuild_hash = false;
        for (uint32_t i = 0; i < count; i++) {
            osfs2_file_t before = file_table[slots[i]];
            if (osfs2_hashed_name_changed(&before, &after[i]))
                rebuild_hash = true;
            if (osfs2_entry_has_extent(&before) &&
                !osfs2_same_extent(&before, &after[i])) {
                for (uint32_t block = 0; block < before.block_count; block++) {
                    blk_bitmap_clear(before.start_block + block);
                    if (crc_table &&
                        before.start_block + block < OSFS2_MAX_BLOCKS)
                        crc_table[before.start_block + block] = 0;
                }
            }
            file_table[slots[i]] = after[i];
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!osfs2_entry_has_extent(&after[i]) ||
                osfs2_same_extent(&journal_record.before_entries[i],
                                  &after[i]))
                continue;
            for (uint32_t block = 0; block < after[i].block_count; block++)
                blk_bitmap_set(after[i].start_block + block);
        }
        superblock = journal_record.after_super;
        if (rebuild_hash) osfs2_hash_build();
        if (osfs2_journal_clear_commit() < 0) {
            serial_puts("[OsitoFS] Journal clear failed; disabling mount\n");
            mounted = false;
            result = -1;
        }
    } else if (journal_commit.magic == OSFS2_JOURNAL_COMMIT_MAGIC) {
        /* A durable commit must be replayed before any further mutation. */
        mounted = false;
    }
    osfs2_spin_unlock(&journal_lock);
    return result;
}

static uint32_t osfs2_alloc_blocks(uint32_t blocks);

static int osfs2_zero_extent(uint32_t start, uint64_t length)
{
    static const uint8_t zeros[4096];
    uint64_t offset = (uint64_t)start << blk_shift;
    while (length) {
        uint64_t chunk = length > sizeof(zeros) ? sizeof(zeros) : length;
        if (osfs2_part_write(offset, zeros, chunk) < 0) return -1;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}

static int osfs2_zero_and_flush_crc(uint32_t start, uint32_t blocks)
{
    if (!crc_table || !blocks) return disk_flush();
    if ((uint64_t)start + blocks > OSFS2_MAX_BLOCKS) return -1;
    for (uint32_t block = start; block < start + blocks; block++)
        crc_table[block] = 0;
    uint32_t first_page = start / (OSFS2_METADATA_PAGE_SIZE / sizeof(uint32_t));
    uint32_t last_page = (start + blocks - 1) /
                         (OSFS2_METADATA_PAGE_SIZE / sizeof(uint32_t));
    for (uint32_t page = first_page; page <= last_page; page++) {
        if (osfs2_part_write(crc_table_offset +
                (uint64_t)page * OSFS2_METADATA_PAGE_SIZE,
                (uint8_t *)crc_table +
                    (uint64_t)page * OSFS2_METADATA_PAGE_SIZE,
                OSFS2_METADATA_PAGE_SIZE) < 0)
            return -1;
    }
    return disk_flush();
}

static int osfs2_commit_one(uint32_t operation, uint32_t slot,
                            const osfs2_file_t *after)
{
    uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES] = { slot, 0 };
    osfs2_file_t entries[OSFS2_JOURNAL_MAX_ENTRIES];
    memset(entries, 0, sizeof(entries));
    entries[0] = *after;
    return osfs2_commit_metadata(operation, slots, entries, 1);
}

/* ── Create a new file ──────────────────────────────────────── */

static osfs2_file_t *osfs2_create_impl(const char *name, uint64_t size)
{
    if (!mounted || !name) return NULL;
    if (strlen(name) >= OSFS2_MODEL_NAME_LEN) return NULL;

    /* Check name doesn't already exist */
    if (osfs2_find(name)) {
        serial_puts("[OsitoFS] File already exists: ");
        serial_puts(name);
        serial_puts("\n");
        return NULL;
    }

    /* Find free slot */
    int slot = -1;
    for (uint32_t i = 0; i < file_capacity; i++) {
        if (!(file_table[i].flags & OSFS2_FLAG_VALID)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        serial_puts("[OsitoFS] File table full\n");
        return NULL;
    }

    osfs2_file_t after;
    memset(&after, 0, sizeof(after));
    if (osfs2_set_entry_name(&after, name) < 0) return NULL;
    after.size = size;
    after.flags |= OSFS2_FLAG_VALID;
    after.layer_index_slot = 0xFFFF;
    after.create_time = osfs2_get_time();
    after.modify_time = after.create_time;

    uint32_t blocks = 0;
    if (size <= OSFS2_INLINE_MAX && !(after.flags & OSFS2_FLAG_LONG_NAME)) {
        after.flags |= OSFS2_FLAG_INLINE;
    } else if (size) {
        uint64_t blocks64 = size / blk_size + (size % blk_size != 0);
        if (!blocks64 || blocks64 > UINT32_MAX ||
            blocks64 > superblock.total_blocks - data_start)
            return NULL;
        blocks = (uint32_t)blocks64;
        uint32_t start = osfs2_alloc_blocks(blocks);
        if (!start) return NULL;
        after.start_block = start;
        after.block_count = blocks;
        if (osfs2_zero_extent(start, (uint64_t)blocks << blk_shift) < 0 ||
            osfs2_zero_and_flush_crc(start, blocks) < 0) {
            for (uint32_t b = 0; b < blocks; b++)
                blk_bitmap_clear(start + b);
            return NULL;
        }
    }

    if (osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE, (uint32_t)slot,
                         &after) < 0) {
        for (uint32_t b = 0; b < blocks; b++)
            blk_bitmap_clear(after.start_block + b);
        return NULL;
    }
    osfs2_file_t *f = &file_table[slot];

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

osfs2_file_t *osfs2_create(const char *name, uint64_t size)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return (osfs2_file_t *)osfs3_create(name, size);
#endif
    osfs2_spin_lock(&mutation_lock);
    osfs2_file_t *result = osfs2_create_impl(name, size);
    int slot = osfs2_file_slot(result);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 2, __ATOMIC_RELEASE);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

/* ── Write data to an existing file ─────────────────────────── */

/* Allocate a contiguous run of `blocks` data blocks via the authoritative
 * live-extent bitmap (same policy as osfs2_create). Returns the start block,
 * or 0 if no non-overlapping run is available. Marks the run used + advances
 * the next_data_block hint. */
static uint32_t osfs2_alloc_blocks(uint32_t blocks)
{
    if (blocks == 0) blocks = 1;
    uint32_t start = blk_bitmap_find_free(blocks);
    if (start && !blk_range_free(start, blocks)) start = 0;
    if (!start) {
        uint32_t hint = superblock.next_data_block;
        if (hint < data_start) hint = data_start;
        if (blk_range_free(hint, blocks)) {
            start = hint;
        } else {
            uint32_t hwm = data_start;
            for (uint32_t b = data_start; b < superblock.total_blocks; b++)
                if (blk_bitmap_test(b)) hwm = b + 1;
            if (blk_range_free(hwm, blocks)) start = hwm;
            else return 0;
        }
    }
    if (!blk_range_free(start, blocks)) return 0;
    for (uint32_t b = 0; b < blocks; b++) blk_bitmap_set(start + b);
    return start;
}

static void osfs2_release_block_range(uint32_t start, uint32_t blocks)
{
    for (uint32_t b = 0; b < blocks; b++) {
        blk_bitmap_clear(start + b);
        if (crc_table && start + b < OSFS2_MAX_BLOCKS)
            crc_table[start + b] = 0;
    }
    while (superblock.next_data_block > data_start &&
           !blk_bitmap_test(superblock.next_data_block - 1))
        superblock.next_data_block--;
}

static void osfs2_invalidate_crc_range(osfs2_file_t *file, uint64_t offset,
                                       uint64_t len)
{
    if (!crc_table || !len) return;
    uint32_t first = file->start_block + (uint32_t)(offset >> blk_shift);
    uint32_t last = file->start_block +
                    (uint32_t)((offset + len - 1) >> blk_shift);
    for (uint32_t b = first; b <= last && b < OSFS2_MAX_BLOCKS; b++)
        crc_table[b] = 0;
}

static int osfs2_zero_file_range(osfs2_file_t *file, uint64_t offset,
                                 uint64_t len)
{
    static const uint8_t zeros[4096];
    while (len) {
        uint64_t chunk = len > sizeof(zeros) ? sizeof(zeros) : len;
        uint64_t absolute = ((uint64_t)file->start_block << blk_shift) + offset;
        if (osfs2_part_write(absolute, zeros, chunk) < 0) return -1;
        offset += chunk;
        len -= chunk;
    }
    return 0;
}

static int osfs2_write_impl(osfs2_file_t *file, uint64_t offset,
                            const void *buf, uint64_t len)
{
    if (!mounted || !file || (!buf && len)) {
        serial_puts("[OsitoFS] write: invalid arguments mounted=");
        serial_putdec(mounted);
        serial_puts(" file=0x");
        serial_puthex((uint64_t)file, 16);
        serial_puts(" buf=0x");
        serial_puthex((uint64_t)buf, 16);
        serial_puts("\n");
        return -1;
    }
    int slot = osfs2_file_slot(file);
    if (slot < 0 || !(file->flags & OSFS2_FLAG_VALID)) {
        serial_puts("[OsitoFS] write: invalid file ptr=0x");
        serial_puthex((uint64_t)file, 16);
        serial_puts(" slot=");
        serial_putdec((uint64_t)(int64_t)slot);
        serial_puts(" flags=0x");
        serial_puthex(file->flags, 8);
        serial_puts("\n");
        return -1;
    }
    if (len == 0) return 0;
    if (offset > UINT64_MAX - len) return -1;
    uint64_t end = offset + len;

    /* Inline files: write to model_name field, persist via file table */
    if (file->flags & OSFS2_FLAG_INLINE) {
        if (end <= OSFS2_INLINE_MAX) {
            osfs2_file_t after = *file;
            memcpy(after.model_name + offset, buf, len);
            if (end > after.size) after.size = end;
            after.crc32 = 0;
            after.modify_time = osfs2_get_time();
            return osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                                    (uint32_t)slot, &after);
        }
        /* The file grew past the inline limit → CONVERT it to a block-backed
         * file: allocate an extent, migrate the existing inline bytes into
         * it, clear the INLINE flag, then fall through to the normal block
         * write below for the new data. (cc1 hits this writing assembly
         * larger than OSFS2_INLINE_MAX to a freshly O_CREAT'd .s file.) */
        uint64_t newsize = end;
        uint64_t blocks64 = newsize / blk_size + (newsize % blk_size != 0);
        if (!blocks64 || blocks64 > UINT32_MAX ||
            blocks64 > superblock.total_blocks - data_start) return -1;
        uint32_t blocks = (uint32_t)blocks64;
        uint32_t start   = osfs2_alloc_blocks(blocks);
        if (!start) {
            serial_puts("[OsitoFS] inline→block: no space for grow\n");
            return -1;
        }
        uint8_t saved[OSFS2_INLINE_MAX];
        memcpy(saved, file->model_name, sizeof(saved));
        uint64_t oldsize = file->size > OSFS2_INLINE_MAX ? OSFS2_INLINE_MAX
                                                          : file->size;
        osfs2_file_t staged = *file;
        staged.flags &= ~OSFS2_FLAG_INLINE;
        staged.start_block = start;
        staged.block_count = blocks;

        if (oldsize && osfs2_part_write((uint64_t)start << blk_shift,
                                        saved, oldsize) < 0) {
            serial_puts("[OsitoFS] inline->block: old data write failed\n");
            osfs2_release_block_range(start, blocks);
            return -1;
        }
        if (offset > oldsize &&
            osfs2_zero_file_range(&staged, oldsize, offset - oldsize) < 0) {
            serial_puts("[OsitoFS] inline->block: gap zero failed\n");
            osfs2_release_block_range(start, blocks);
            return -1;
        }
        if (osfs2_part_write(((uint64_t)start << blk_shift) + offset,
                             buf, len) < 0) {
            serial_puts("[OsitoFS] inline->block: payload write failed\n");
            osfs2_release_block_range(start, blocks);
            return -1;
        }

        memset(staged.model_name, 0, OSFS2_INLINE_MAX);
        staged.size = newsize;
        staged.crc32 = 0;
        staged.modify_time = osfs2_get_time();
        osfs2_invalidate_crc_range(&staged, 0, newsize);
        if (osfs2_zero_and_flush_crc(start, blocks) < 0) {
            serial_puts("[OsitoFS] inline->block: CRC flush failed\n");
            for (uint32_t b = 0; b < blocks; b++) blk_bitmap_clear(start + b);
            return -1;
        }
        if (osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                             (uint32_t)slot, &staged) < 0) {
            serial_puts("[OsitoFS] inline->block: metadata commit failed\n");
            for (uint32_t b = 0; b < blocks; b++) blk_bitmap_clear(start + b);
            return -1;
        }
        return 0;
    }

    uint64_t capacity = (uint64_t)file->block_count << blk_shift;
    if (end > capacity) {
        uint64_t blocks64 = end / blk_size + (end % blk_size != 0);
        uint64_t doubled = (uint64_t)file->block_count * 2;
        if (blocks64 < doubled) blocks64 = doubled;
        if (!blocks64) blocks64 = 1;
        if (blocks64 > UINT32_MAX ||
            blocks64 > superblock.total_blocks - data_start) return -1;

        uint32_t blocks = (uint32_t)blocks64;
        uint32_t start = osfs2_alloc_blocks(blocks);
        if (!start) return -1;
        osfs2_file_t staged = *file;
        staged.start_block = start;
        staged.block_count = blocks;

        static uint8_t copy_buf[4096];
        uint64_t copied = 0;
        while (copied < file->size) {
            uint64_t chunk = file->size - copied;
            if (chunk > sizeof(copy_buf)) chunk = sizeof(copy_buf);
            if (osfs2_part_read(((uint64_t)file->start_block << blk_shift) + copied,
                                copy_buf, chunk) < 0 ||
                osfs2_part_write(((uint64_t)start << blk_shift) + copied,
                                 copy_buf, chunk) < 0) {
                osfs2_release_block_range(start, blocks);
                return -1;
            }
            copied += chunk;
        }
        if (offset > file->size &&
            osfs2_zero_file_range(&staged, file->size, offset - file->size) < 0) {
            osfs2_release_block_range(start, blocks);
            return -1;
        }
        if (osfs2_part_write(((uint64_t)start << blk_shift) + offset,
                             buf, len) < 0) {
            osfs2_release_block_range(start, blocks);
            return -1;
        }

        staged.size = end > file->size ? end : file->size;
        staged.crc32 = 0;
        staged.modify_time = osfs2_get_time();
        if (osfs2_zero_and_flush_crc(start, blocks) < 0) {
            osfs2_release_block_range(start, blocks);
            return -1;
        }
        if (osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                             (uint32_t)slot, &staged) < 0) {
            osfs2_release_block_range(start, blocks);
            return -1;
        }
        return 0;
    }

    if (offset > file->size) {
        if (osfs2_zero_file_range(file, file->size, offset - file->size) < 0)
            return -1;
        osfs2_invalidate_crc_range(file, file->size, offset - file->size);
    }

    uint64_t abs_offset = ((uint64_t)file->start_block << blk_shift) + offset;
    int ret = osfs2_part_write(abs_offset, buf, len);
    if (ret < 0) return -1;

    osfs2_invalidate_crc_range(file, offset, len);

    uint64_t changed_start = offset > file->size ? file->size : offset;
    uint32_t first_block = file->start_block +
                           (uint32_t)(changed_start >> blk_shift);
    uint32_t last_block = file->start_block +
                          (uint32_t)((end - 1) >> blk_shift);
    if (osfs2_zero_and_flush_crc(first_block,
                                 last_block - first_block + 1) < 0)
        return -1;

    osfs2_file_t after = *file;
    if (end > after.size) after.size = end;
    after.crc32 = 0;
    after.modify_time = osfs2_get_time();
    return osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                            (uint32_t)slot, &after);
}

int osfs2_write(osfs2_file_t *file, uint64_t offset, const void *buf,
                uint64_t len)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_write(file, offset, buf, len);
#endif
    osfs2_spin_lock(&mutation_lock);
    int slot = osfs2_file_slot(file);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_ACQ_REL);
    int result = osfs2_write_impl(file, offset, buf, len);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_RELEASE);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

/* Write within an existing allocation without publishing a new file size.
 * Boot diagnostics use this with osfs2_set_size_reserved() so their fixed
 * extent survives each append while normal truncate still reclaims space. */
static int osfs2_write_data_impl(osfs2_file_t *file, uint64_t offset,
                                 const void *buf, uint64_t len)
{
    if (!mounted || !file || (!buf && len)) return -1;
    int slot = osfs2_file_slot(file);
    if (slot < 0 || !(file->flags & OSFS2_FLAG_VALID)) return -1;
    if (!len) return 0;
    if (offset > UINT64_MAX - len) return -1;

    uint64_t capacity = (file->flags & OSFS2_FLAG_INLINE)
                      ? OSFS2_INLINE_MAX
                      : (uint64_t)file->block_count << blk_shift;
    if (offset + len > capacity) return -1;

    if (file->flags & OSFS2_FLAG_INLINE) {
        memcpy(file->model_name + offset, buf, len);
        return 0;
    }

    uint64_t absolute = ((uint64_t)file->start_block << blk_shift) + offset;
    if (osfs2_part_write(absolute, buf, len) < 0) return -1;

    uint32_t first = file->start_block + (uint32_t)(offset >> blk_shift);
    uint32_t last = file->start_block +
                    (uint32_t)((offset + len - 1) >> blk_shift);
    return osfs2_zero_and_flush_crc(first, last - first + 1);
}

int osfs2_write_data(osfs2_file_t *file, uint64_t offset, const void *buf,
                     uint64_t len)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_write_data(file, offset, buf, len);
#endif
    osfs2_spin_lock(&mutation_lock);
    int slot = osfs2_file_slot(file);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_ACQ_REL);
    int result = osfs2_write_data_impl(file, offset, buf, len);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_RELEASE);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

static int osfs2_set_size_reserved_impl(osfs2_file_t *file, uint64_t size)
{
    if (!mounted || !file || !(file->flags & OSFS2_FLAG_VALID)) return -1;
    int slot = osfs2_file_slot(file);
    if (slot < 0) return -1;

    uint64_t capacity = (file->flags & OSFS2_FLAG_INLINE)
                      ? OSFS2_INLINE_MAX
                      : (uint64_t)file->block_count << blk_shift;
    if (size > capacity) return -1;

    osfs2_file_t after = *file;
    after.size = size;
    after.crc32 = 0;
    after.modify_time = osfs2_get_time();
    return osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                            (uint32_t)slot, &after);
}

int osfs2_set_size_reserved(osfs2_file_t *file, uint64_t size)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_set_size_reserved(file, size);
#endif
    osfs2_spin_lock(&mutation_lock);
    int slot = osfs2_file_slot(file);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_ACQ_REL);
    int result = osfs2_set_size_reserved_impl(file, size);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_RELEASE);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

/* Resize a file without changing its identity. Growth reuses the transactional
 * write path, which relocates extents and zero-fills sparse gaps as needed. */
static int osfs2_truncate_impl(osfs2_file_t *file, uint64_t size)
{
    if (!mounted || !file || !(file->flags & OSFS2_FLAG_VALID)) return -1;
    if (size > file->size) {
        static const uint8_t zero;
        return osfs2_write_impl(file, size - 1, &zero, 1);
    }
    if (size == file->size &&
        (size != 0 || (file->flags & OSFS2_FLAG_INLINE))) return 0;

    int slot = osfs2_file_slot(file);
    if (slot < 0) return -1;

    if (file->flags & OSFS2_FLAG_INLINE) {
        osfs2_file_t after = *file;
        memset(after.model_name + size, 0, OSFS2_INLINE_MAX - size);
        after.size = size;
        after.crc32 = 0;
        after.modify_time = osfs2_get_time();
        return osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                                (uint32_t)slot, &after);
    } else if (size <= OSFS2_INLINE_MAX &&
               !(file->flags & OSFS2_FLAG_LONG_NAME)) {
        uint8_t saved[OSFS2_INLINE_MAX];
        memset(saved, 0, sizeof(saved));
        if (size && osfs2_part_read((uint64_t)file->start_block << blk_shift,
                                   saved, size) < 0)
            return -1;

        uint32_t old_start = file->start_block;
        uint32_t old_blocks = file->block_count;
        osfs2_file_t after = *file;
        after.start_block = 0;
        after.block_count = 0;
        after.flags &= ~(OSFS2_FLAG_GGUF | OSFS2_FLAG_RAW);
        after.flags |= OSFS2_FLAG_INLINE;
        memset(after.model_name, 0, OSFS2_INLINE_MAX);
        if (size) memcpy(after.model_name, saved, size);
        after.size = size;
        after.crc32 = 0;
        after.modify_time = osfs2_get_time();
        if (osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                             (uint32_t)slot, &after) < 0)
            return -1;
        for (uint32_t b = 0; b < old_blocks; b++)
            if (crc_table && old_start + b < OSFS2_MAX_BLOCKS)
                crc_table[old_start + b] = 0;
        return 0;
    }

    osfs2_file_t after = *file;
    after.size = size;
    after.crc32 = 0;
    after.modify_time = osfs2_get_time();
    return osfs2_commit_one(OSFS2_JOURNAL_OP_REPLACE,
                            (uint32_t)slot, &after);
}

int osfs2_truncate(osfs2_file_t *file, uint64_t size)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_truncate(file, size);
#endif
    osfs2_spin_lock(&mutation_lock);
    int slot = osfs2_file_slot(file);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_ACQ_REL);
    int result = osfs2_truncate_impl(file, size);
    if (slot >= 0)
        __atomic_add_fetch(&file_revisions[slot], 1, __ATOMIC_RELEASE);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

/* Rename a regular file without moving its data extent. With replacement, the
 * old destination must not be open or mapped because v2 file descriptors point
 * directly at file-table slots. */
static int osfs2_rename_impl(const char *from, const char *to, bool replace)
{
    if (!mounted || !from || !to) return -1;
    if (strlen(from) >= OSFS2_MODEL_NAME_LEN ||
        strlen(to) >= OSFS2_MODEL_NAME_LEN)
        return -1;

    osfs2_file_t *file = osfs2_find(from);
    if (!file) return -1;
    if (strcmp(from, to) == 0) return 0;
    osfs2_file_t *target = osfs2_find(to);
    if (target && !replace) return -2;
    if (target && osfs2_file_busy(target)) return -3;

    uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES] = {
        (uint32_t)osfs2_file_slot(file), 0
    };
    osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES];
    after[0] = *file;
    if (osfs2_set_entry_name(&after[0], to) < 0) return -1;
    uint32_t count = 1;
    if (target) {
        slots[1] = (uint32_t)osfs2_file_slot(target);
        memset(&after[1], 0, sizeof(after[1]));
        count = 2;
    }
    return osfs2_commit_metadata(OSFS2_JOURNAL_OP_RENAME, slots, after,
                                 count);
}

int osfs2_rename(const char *from, const char *to, bool replace)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_rename(from, to, replace);
#endif
    osfs2_spin_lock(&mutation_lock);
    int result = osfs2_rename_impl(from, to, replace);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

/* ── Delete a file ──────────────────────────────────────────── */

static int osfs2_delete_impl(const char *name)
{
    if (!mounted || !name) return -1;

    osfs2_file_t *f = osfs2_find(name);
    if (!f) return -1;
    if (osfs2_file_busy(f)) return -2;

    uint32_t freed_blocks = f->block_count;
    uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES] = {
        (uint32_t)osfs2_file_slot(f), 0
    };
    osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES];
    memset(&after, 0, sizeof(after));
    if (osfs2_commit_metadata(OSFS2_JOURNAL_OP_DELETE, slots, after, 1) < 0)
        return -1;

    serial_puts("[OsitoFS] Deleted '");
    serial_puts(name);
    serial_puts("' (freed ");
    serial_putdec(freed_blocks);
    serial_puts(" blocks)\n");
    return 0;
}

int osfs2_delete(const char *name)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_delete(name);
#endif
    osfs2_spin_lock(&mutation_lock);
    int result = osfs2_delete_impl(name);
    osfs2_spin_unlock(&mutation_lock);
    return result;
}

/* ── Accessors ───────────────────────────────────────────────── */

bool osfs2_is_mounted(void)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return true;
#endif
    return mounted;
}

uint32_t osfs2_file_count(void)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_count();
#endif
    return mounted ? superblock.file_count : 0;
}

const char *osfs2_label(void)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_label();
#endif
    return mounted ? superblock.label : "";
}

uint64_t osfs2_file_size(osfs2_file_t *file)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_size(file);
#endif
    return file ? file->size : 0;
}

const char *osfs2_file_name(osfs2_file_t *file)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_name(file);
#endif
    return osfs2_entry_name(file);
}

osfs2_file_t *osfs2_file_by_index(uint32_t idx)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return (osfs2_file_t *)osfs3_file_at(idx);
#endif
    if (!mounted) return NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < file_capacity; i++) {
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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_free_blocks();
#endif
    if (!mounted) return 0;
    uint32_t total_data = superblock.total_blocks - data_start;
    uint32_t used_data = superblock.used_blocks - data_start;
    /* Clamp at zero — used_data > total_data shows up after osfs2_create
     * over-allocates a tight FS. The display gets clean output and the
     * underflow doesn't propagate to MB-conversion math. */
    return used_data >= total_data ? 0 : total_data - used_data;
}
uint32_t osfs2_get_block_size(void) {
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_block_size();
#endif
    return mounted ? blk_size : 0;
}
uint32_t osfs2_total_blocks(void) {
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_total_blocks();
#endif
    return mounted && superblock.total_blocks > data_start
        ? superblock.total_blocks - data_start : 0;
}
uint32_t osfs2_max_files(void) {
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_max_files();
#endif
    return mounted ? file_capacity : 0;
}

uint32_t osfs2_file_ctime(osfs2_file_t *f) {
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_ctime(f);
#endif
    return f ? f->create_time : 0;
}
uint32_t osfs2_file_mtime(osfs2_file_t *f) {
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_mtime(f);
#endif
    return f ? f->modify_time : 0;
}
uint64_t osfs2_file_revision(osfs2_file_t *f) {
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_revision(f);
#endif
    int slot = osfs2_file_slot(f);
    return slot >= 0
        ? __atomic_load_n(&file_revisions[slot], __ATOMIC_ACQUIRE) : 0;
}

/* Get nth valid file (0-indexed). Returns NULL if out of range. */
osfs2_file_t *osfs2_file_at(uint32_t index)
{
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted())
        return (osfs2_file_t *)osfs3_file_at(index);
#endif
    if (!mounted || !file_table) return NULL;
    uint32_t count = 0;
    for (uint32_t i = 0; i < file_capacity; i++) {
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
#ifndef __EMSCRIPTEN__
    if (osfs3_is_mounted()) return osfs3_file_byte_offset(file);
#endif
    if (!mounted || !file) return 0;
    return partition_offset + ((uint64_t)file->start_block << blk_shift);
}
