/*
 * OsitoFS v2 host tools — common utilities
 *
 * Block-aligned I/O using pread/pwrite with O_DIRECT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#include <time.h>

#include "common.h"

uint32_t osfs2_block_sz = OSFS2_DEFAULT_BLOCK_SIZE;
uint32_t osfs2_file_capacity = OSFS2_MAX_FILES_STANDARD;
uint32_t osfs2_filetab_size = OSFS2_FILETAB_SIZE_STANDARD;
uint32_t osfs2_crctab_offset = OSFS2_CRCTAB_OFF_STANDARD;
uint32_t osfs2_layeridx_offset = OSFS2_LAYERIDX_OFF_STANDARD;
int osfs2_crc_table_enabled = 1;
int osfs2_layer_index_enabled = 1;

void osfs2_set_layout(uint32_t version)
{
    osfs2_file_capacity = osfs2_format_max_files(version);
    osfs2_filetab_size = osfs2_format_filetab_size(version);
    osfs2_crctab_offset = osfs2_format_crctab_off(version);
    osfs2_layeridx_offset = OSFS2_LAYERIDX_OFF_STANDARD;
    osfs2_crc_table_enabled = osfs2_format_has_crc_table(version);
    osfs2_layer_index_enabled = osfs2_format_has_layer_index(version);
}

/* ── Device I/O ──────────────────────────────────────────────── */

const char *osfs2_entry_name(const osfs2_file_t *file)
{
    if (file && (file->flags & OSFS2_FLAG_LONG_NAME) && file->model_name[0])
        return file->model_name;
    return file ? file->name : NULL;
}

int osfs2_set_entry_name(osfs2_file_t *file, const char *name)
{
    if (!file || !name) return -1;

    size_t len = strlen(name);
    int was_long = (file->flags & OSFS2_FLAG_LONG_NAME) != 0;
    if (len >= OSFS2_MODEL_NAME_LEN) return -1;

    memset(file->name, 0, sizeof(file->name));
    file->flags &= ~OSFS2_FLAG_LONG_NAME;
    if (len < OSFS2_NAME_LEN) {
        memcpy(file->name, name, len + 1);
        if (was_long) memset(file->model_name, 0, sizeof(file->model_name));
        return 0;
    }
    if (file->flags & (OSFS2_FLAG_INLINE | OSFS2_FLAG_GGUF)) return -1;

    memcpy(file->model_name, name, len + 1);
    file->flags |= OSFS2_FLAG_LONG_NAME;

    static const char hex[] = "0123456789abcdef";
    uint32_t crc = osfs2_crc32(name, len);
    size_t prefix = OSFS2_NAME_LEN - 10;
    memcpy(file->name, name, prefix);
    file->name[prefix] = '~';
    for (int i = 0; i < 8; i++)
        file->name[prefix + 1 + i] = hex[(crc >> (28 - i * 4)) & 0xF];
    return 0;
}

int osfs2_open_device(const char *path, int readonly)
{
    int flags = (readonly ? O_RDONLY : O_RDWR);
#ifdef O_DIRECT
    flags |= O_DIRECT;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "osfs2: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (flock(fd, readonly ? LOCK_SH : LOCK_EX) < 0) {
        fprintf(stderr, "osfs2: cannot lock %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void osfs2_close_device(int fd)
{
    if (fd >= 0) close(fd);
}

int osfs2_read_block(int fd, uint32_t block, void *buf)
{
    uint32_t shift = osfs2_block_shift(osfs2_block_sz);
    uint64_t offset = (uint64_t)block << shift;
    ssize_t n = pread(fd, buf, osfs2_block_sz, offset);
    if (n != (ssize_t)osfs2_block_sz) {
        fprintf(stderr, "osfs2: read block %u failed: %s\n",
                block, n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

int osfs2_write_block(int fd, uint32_t block, const void *buf)
{
    uint32_t shift = osfs2_block_shift(osfs2_block_sz);
    uint64_t offset = (uint64_t)block << shift;
    ssize_t n = pwrite(fd, buf, osfs2_block_sz, offset);
    if (n != (ssize_t)osfs2_block_sz) {
        fprintf(stderr, "osfs2: write block %u failed: %s\n",
                block, n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

int osfs2_read_bytes(int fd, uint64_t offset, void *buf, size_t len)
{
    ssize_t n = pread(fd, buf, len, offset);
    if (n != (ssize_t)len) {
        fprintf(stderr, "osfs2: read %zu bytes at 0x%llx failed: %s\n",
                len, (unsigned long long)offset,
                n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

int osfs2_write_bytes(int fd, uint64_t offset, const void *buf, size_t len)
{
    ssize_t n = pwrite(fd, buf, len, offset);
    if (n != (ssize_t)len) {
        fprintf(stderr, "osfs2: write %zu bytes at 0x%llx failed: %s\n",
                len, (unsigned long long)offset,
                n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

int osfs2_sync(int fd)
{
    if (fsync(fd) < 0) {
        fprintf(stderr, "osfs2: fsync failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Metadata journal ────────────────────────────────────────── */

static int journal_super_valid(const osfs2_super_t *sb)
{
    if (!sb || sb->magic != OSFS2_MAGIC ||
        !osfs2_supported_version(sb->version) ||
        !osfs2_valid_block_size(sb->block_size)) return 0;
    osfs2_super_t copy = *sb;
    uint32_t expected = copy.crc32;
    copy.crc32 = 0;
    return expected == osfs2_crc32(&copy, sizeof(copy));
}

static int journal_record_valid(osfs2_journal_record_t *record)
{
    if (!record || record->magic != OSFS2_JOURNAL_MAGIC ||
        record->version != OSFS2_JOURNAL_VERSION || !record->entry_count ||
        record->entry_count > OSFS2_JOURNAL_MAX_ENTRIES ||
        !record->page_count ||
        record->page_count > OSFS2_JOURNAL_MAX_PAGES ||
        record->operation < OSFS2_JOURNAL_OP_RENAME ||
        record->operation > OSFS2_JOURNAL_OP_REPLACE ||
        !journal_super_valid(&record->before_super) ||
        !journal_super_valid(&record->after_super)) return 0;
    if (record->before_super.version != record->after_super.version ||
        record->before_super.block_size != record->after_super.block_size ||
        memcmp(record->before_super.uuid, record->after_super.uuid,
               sizeof(record->before_super.uuid)) != 0 ||
        record->after_super.total_blocks <=
            osfs2_format_data_start_blk(record->after_super.version,
                                        record->after_super.block_size) ||
        record->after_super.total_blocks > OSFS2_MAX_BLOCKS) return 0;
    for (uint32_t i = 0; i < record->page_count; i++) {
        if (record->pages[i] >= osfs2_format_filetab_size(
                record->after_super.version) / OSFS2_METADATA_PAGE_SIZE)
            return 0;
        if (i && record->pages[i] == record->pages[0]) return 0;
    }
    for (uint32_t i = 0; i < record->entry_count; i++) {
        if (record->slots[i] >= osfs2_format_max_files(
                record->after_super.version)) return 0;
        uint32_t page = record->slots[i] / 16;
        int found = 0;
        for (uint32_t j = 0; j < record->page_count; j++)
            if (record->pages[j] == page) found = 1;
        if (!found || (i && record->slots[i] == record->slots[0])) return 0;

        const osfs2_file_t *after = NULL;
        for (uint32_t j = 0; j < record->page_count; j++)
            if (record->pages[j] == page)
                after = (const osfs2_file_t *)(record->after_pages[j] +
                        (record->slots[i] % 16) * sizeof(osfs2_file_t));
        if (!after) return 0;
        if (after->flags & OSFS2_FLAG_VALID) {
            if (strnlen(after->name, OSFS2_NAME_LEN) == OSFS2_NAME_LEN)
                return 0;
            if (after->flags & OSFS2_FLAG_INLINE) {
                if (after->size > OSFS2_INLINE_MAX || after->start_block ||
                    after->block_count) return 0;
            } else if (after->block_count) {
                uint64_t end = (uint64_t)after->start_block +
                               after->block_count;
                if (after->start_block < osfs2_format_data_start_blk(
                        record->after_super.version,
                        record->after_super.block_size) ||
                    end > record->after_super.total_blocks ||
                    after->size > (uint64_t)after->block_count *
                                      record->after_super.block_size)
                    return 0;
            } else if (after->size) return 0;
        }
    }
    uint32_t expected = record->record_crc32;
    record->record_crc32 = 0;
    uint32_t actual = osfs2_crc32(record, sizeof(*record));
    record->record_crc32 = expected;
    return expected == actual;
}

static int journal_commit_valid(osfs2_journal_commit_t *commit)
{
    if (!commit || commit->magic != OSFS2_JOURNAL_COMMIT_MAGIC ||
        commit->version != OSFS2_JOURNAL_VERSION) return 0;
    uint32_t expected = commit->commit_crc32;
    commit->commit_crc32 = 0;
    uint32_t actual = osfs2_crc32(commit, sizeof(*commit));
    commit->commit_crc32 = expected;
    return expected == actual;
}

static int journal_clear_commit(int fd, void *commit_buf)
{
    memset(commit_buf, 0, OSFS2_METADATA_PAGE_SIZE);
    return osfs2_write_bytes(fd, OSFS2_JOURNAL_COMMIT_OFF, commit_buf,
                             OSFS2_METADATA_PAGE_SIZE) == 0
        ? osfs2_sync(fd) : -1;
}

static void journal_failpoint(int fd, const char *name)
{
    const char *requested = getenv("OSFS2_JOURNAL_FAILPOINT");
    if (!requested || strcmp(requested, name) != 0) return;
    (void)osfs2_sync(fd);
    _exit(86);
}

static int journal_apply(int fd, const osfs2_journal_record_t *record)
{
    void *page = osfs2_alloc_aligned(4096);
    void *super_buf = osfs2_alloc_aligned(4096);
    if (!page || !super_buf) {
        free(page); free(super_buf);
        return -1;
    }

    for (uint32_t i = 0; i < record->page_count; i++) {
        uint64_t page_offset = OSFS2_FILETAB_OFF +
            (uint64_t)record->pages[i] * OSFS2_METADATA_PAGE_SIZE;
        memcpy(page, record->after_pages[i], OSFS2_METADATA_PAGE_SIZE);
        if (osfs2_write_bytes(fd, page_offset, page,
                              OSFS2_METADATA_PAGE_SIZE) < 0) goto fail;
    }
    journal_failpoint(fd, "entries");

    memset(super_buf, 0, 4096);
    memcpy(super_buf, &record->after_super, sizeof(record->after_super));
    if (osfs2_write_bytes(fd, 0, super_buf, 4096) < 0) goto fail;
    journal_failpoint(fd, "primary-super");
    if (osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, super_buf, 4096) < 0)
        goto fail;
    journal_failpoint(fd, "backup-super");
    if (osfs2_sync(fd) < 0) goto fail;
    free(page); free(super_buf);
    return 0;

fail:
    free(page); free(super_buf);
    return -1;
}

int osfs2_journal_recover(int fd, int repair)
{
    void *record_buf = osfs2_alloc_aligned(OSFS2_JOURNAL_RECORD_SIZE);
    void *commit_buf = osfs2_alloc_aligned(4096);
    if (!record_buf || !commit_buf) {
        free(record_buf); free(commit_buf);
        return -1;
    }
    osfs2_journal_commit_t *commit = commit_buf;
    int result = osfs2_read_bytes(fd, OSFS2_JOURNAL_COMMIT_OFF, commit_buf,
                                  OSFS2_METADATA_PAGE_SIZE);
    if (result < 0) goto out;
    if (commit->magic != OSFS2_JOURNAL_COMMIT_MAGIC) {
        result = 0;
        goto out;
    }
    if (!journal_commit_valid(commit)) {
        if (!repair) {
            result = 0;
            goto out;
        }
        result = journal_clear_commit(fd, commit_buf);
        goto out;
    }
    if (osfs2_read_bytes(fd, OSFS2_JOURNAL_RECORD_OFF, record_buf,
                         OSFS2_JOURNAL_RECORD_SIZE) < 0) {
        result = -1;
        goto out;
    }
    osfs2_journal_record_t *record = record_buf;
    if (!journal_record_valid(record) ||
        record->transaction_id != commit->transaction_id ||
        record->record_crc32 != commit->record_crc32) {
        fprintf(stderr, "osfs2: committed journal record is corrupt\n");
        result = -1;
        goto out;
    }
    void *super_buf = osfs2_alloc_aligned(4096);
    if (!super_buf) {
        result = -1;
        goto out;
    }
    osfs2_super_t current;
    int have_current = 0;
    if (osfs2_read_bytes(fd, 0, super_buf, 4096) == 0) {
        memcpy(&current, super_buf, sizeof(current));
        have_current = journal_super_valid(&current);
    }
    if (!have_current &&
        osfs2_read_bytes(fd, OSFS2_SUPER_BACKUP_OFF, super_buf, 4096) == 0) {
        memcpy(&current, super_buf, sizeof(current));
        have_current = journal_super_valid(&current);
    }
    free(super_buf);
    if (have_current &&
        (current.version != record->after_super.version ||
         current.block_size != record->after_super.block_size ||
         memcmp(current.uuid, record->after_super.uuid,
                sizeof(current.uuid)) != 0)) {
        fprintf(stderr, "osfs2: journal belongs to another filesystem\n");
        result = -1;
        goto out;
    }
    if (!repair) {
        result = 1;
        goto out;
    }
    if (journal_apply(fd, record) < 0) {
        result = -1;
        goto out;
    }
    result = journal_clear_commit(fd, commit_buf);

out:
    free(record_buf); free(commit_buf);
    return result;
}

int osfs2_journal_commit_entries(
    int fd, uint32_t operation,
    const osfs2_super_t *before_super,
    const osfs2_super_t *after_super,
    const uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t before[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES], uint32_t count)
{
    if (!before_super || !after_super || !slots || !before || !after ||
        !count || count > OSFS2_JOURNAL_MAX_ENTRIES) return -1;
    void *record_buf = osfs2_alloc_aligned(OSFS2_JOURNAL_RECORD_SIZE);
    void *commit_buf = osfs2_alloc_aligned(4096);
    if (!record_buf || !commit_buf) {
        free(record_buf); free(commit_buf);
        return -1;
    }
    osfs2_journal_record_t *record = record_buf;
    osfs2_journal_commit_t *commit = commit_buf;
    record->magic = OSFS2_JOURNAL_MAGIC;
    record->version = OSFS2_JOURNAL_VERSION;
    record->operation = operation;
    record->entry_count = count;
    record->transaction_id = ((uint64_t)time(NULL) << 32) ^
                             (uint64_t)getpid() ^ (uintptr_t)record;
    record->before_super = *before_super;
    record->after_super = *after_super;
    for (uint32_t i = 0; i < count; i++) {
        if (slots[i] >= OSFS2_MAX_FILES) goto fail;
        record->slots[i] = slots[i];
        record->before_entries[i] = before[i];
        uint32_t page_index = slots[i] / 16;
        uint32_t page_slot = record->page_count;
        for (uint32_t j = 0; j < record->page_count; j++)
            if (record->pages[j] == page_index) page_slot = j;
        if (page_slot == record->page_count) {
            uint64_t page_offset = OSFS2_FILETAB_OFF +
                (uint64_t)page_index * OSFS2_METADATA_PAGE_SIZE;
            if (osfs2_read_bytes(fd, page_offset, commit_buf,
                                 OSFS2_METADATA_PAGE_SIZE) < 0) goto fail;
            memcpy(record->after_pages[page_slot], commit_buf,
                   OSFS2_METADATA_PAGE_SIZE);
            record->pages[page_slot] = page_index;
            record->page_count++;
        }
        memcpy(record->after_pages[page_slot] +
                   (slots[i] % 16) * sizeof(osfs2_file_t),
               &after[i], sizeof(osfs2_file_t));
    }
    record->record_crc32 = osfs2_crc32(record, sizeof(*record));

    if (journal_clear_commit(fd, commit_buf) < 0 ||
        osfs2_write_bytes(fd, OSFS2_JOURNAL_RECORD_OFF, record_buf,
                          OSFS2_JOURNAL_RECORD_SIZE) < 0 ||
        osfs2_sync(fd) < 0) goto fail;
    journal_failpoint(fd, "prepared");

    memset(commit_buf, 0, 4096);
    commit->magic = OSFS2_JOURNAL_COMMIT_MAGIC;
    commit->version = OSFS2_JOURNAL_VERSION;
    commit->transaction_id = record->transaction_id;
    commit->record_crc32 = record->record_crc32;
    commit->commit_crc32 = osfs2_crc32(commit, sizeof(*commit));
    if (osfs2_write_bytes(fd, OSFS2_JOURNAL_COMMIT_OFF, commit_buf,
                          OSFS2_METADATA_PAGE_SIZE) < 0 ||
        osfs2_sync(fd) < 0) goto fail;
    journal_failpoint(fd, "committed");
    if (journal_apply(fd, record) < 0) goto fail;
    if (journal_clear_commit(fd, commit_buf) < 0) goto fail;
    free(record_buf); free(commit_buf);
    return 0;

fail:
    free(record_buf); free(commit_buf);
    return -1;
}

/* ── Superblock ──────────────────────────────────────────────── */

static int osfs2_validate_super(const osfs2_super_t *sb)
{
    if (sb->magic != OSFS2_MAGIC) return -1;
    if (!osfs2_supported_version(sb->version)) return -1;
    uint32_t saved_crc = sb->crc32;
    osfs2_super_t tmp;
    memcpy(&tmp, sb, sizeof(tmp));
    tmp.crc32 = 0;
    uint32_t calc_crc = osfs2_crc32(&tmp, sizeof(tmp));
    if (calc_crc != saved_crc) return -1;
    if (!osfs2_valid_block_size(sb->block_size)) return -1;
    return 0;
}

int osfs2_read_super(int fd, osfs2_super_t *sb)
{
    int journal_status = osfs2_journal_recover(fd, 0);
    if (journal_status != 0) {
        fprintf(stderr, journal_status > 0
            ? "osfs2: committed journal pending; recover it before reading\n"
            : "osfs2: journal validation failed\n");
        return -1;
    }
    void *buf = osfs2_alloc_aligned(4096);
    if (!buf) return -1;

    if (osfs2_read_bytes(fd, 0, buf, 4096) < 0) {
        free(buf);
        return -1;
    }
    memcpy(sb, buf, sizeof(*sb));

    if (osfs2_validate_super(sb) < 0) {
        fprintf(stderr, "osfs2: primary superblock invalid, trying backup\n");
        if (osfs2_read_bytes(fd, OSFS2_SUPER_BACKUP_OFF, buf, 4096) < 0) {
            free(buf);
            return -1;
        }
        memcpy(sb, buf, sizeof(*sb));
        if (osfs2_validate_super(sb) < 0) {
            fprintf(stderr, "osfs2: backup superblock also invalid\n");
            free(buf);
            return -1;
        }
        fprintf(stderr, "osfs2: WARNING — using backup superblock\n");
    }

    free(buf);
    osfs2_block_sz = sb->block_size;
    osfs2_set_layout(sb->version);
    return 0;
}

/* ── Device size ─────────────────────────────────────────────── */

uint64_t osfs2_device_size(int fd)
{
    uint64_t size = 0;
#ifdef BLKGETSIZE64
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
#else
    {
#endif
        /* Might be a regular file */
        off_t pos = lseek(fd, 0, SEEK_END);
        if (pos < 0) return 0;
        size = (uint64_t)pos;
        lseek(fd, 0, SEEK_SET);
    }
    return size;
}

/* ── Aligned allocation ──────────────────────────────────────── */

void *osfs2_alloc_aligned(uint32_t size)
{
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, size) != 0) {
        fprintf(stderr, "osfs2: out of memory (alloc %u)\n", size);
        return NULL;
    }
    memset(buf, 0, size);
    return buf;
}

void *osfs2_alloc_block(void)
{
    return osfs2_alloc_aligned(osfs2_block_sz);
}

void osfs2_free_block(void *buf)
{
    free(buf);
}

/* ── UUID ────────────────────────────────────────────────────── */

void osfs2_gen_uuid(uint8_t uuid[16])
{
    /* Simple random UUID (version 4) */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 16; i++)
        uuid[i] = (uint8_t)(rand() & 0xFF);
    uuid[6] = (uuid[6] & 0x0F) | 0x40;  /* version 4 */
    uuid[8] = (uuid[8] & 0x3F) | 0x80;  /* variant 1 */
}

/* ── Display helpers ─────────────────────────────────────────── */

void osfs2_print_size(uint64_t bytes)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024)
        printf("%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024)
        printf("%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        printf("%.1f KB", (double)bytes / 1024.0);
    else
        printf("%llu B", (unsigned long long)bytes);
}

static const char *quant_names[] = {
    [OSFS2_QUANT_NONE]    = "none",
    [OSFS2_QUANT_F32]     = "F32",
    [OSFS2_QUANT_F16]     = "F16",
    [OSFS2_QUANT_Q8_0]    = "Q8_0",
    [OSFS2_QUANT_Q4_0]    = "Q4_0",
    [OSFS2_QUANT_Q4_1]    = "Q4_1",
    [OSFS2_QUANT_Q5_0]    = "Q5_0",
    [OSFS2_QUANT_Q5_1]    = "Q5_1",
    [OSFS2_QUANT_Q2_K]    = "Q2_K",
    [OSFS2_QUANT_Q3_K]    = "Q3_K",
    [OSFS2_QUANT_Q4_K]    = "Q4_K",
    [OSFS2_QUANT_Q5_K]    = "Q5_K",
    [OSFS2_QUANT_Q6_K]    = "Q6_K",
    [OSFS2_QUANT_IQ2_XXS] = "IQ2_XXS",
    [OSFS2_QUANT_IQ3_XXS] = "IQ3_XXS",
};

const char *osfs2_quant_name(uint32_t quant_type)
{
    if (quant_type < sizeof(quant_names) / sizeof(quant_names[0]) && quant_names[quant_type])
        return quant_names[quant_type];
    return "unknown";
}
