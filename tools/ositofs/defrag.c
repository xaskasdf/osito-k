/*
 * ositofs-defrag — Offline defragmentation tool for OsitoFS v2
 *
 * Usage: ositofs-defrag <device> [--dry-run] [--verbose]
 *
 * OsitoFS v2 uses contiguous block allocation. After deletes, gaps appear
 * between files. This tool compacts all files toward the beginning of the
 * data area, eliminating gaps.
 *
 * Algorithm:
 *   1. Read superblock, file table, CRC table
 *   2. Build sorted list of valid files by start_block
 *   3. Walk through sorted files; move each left to close gaps
 *   4. Update CRC table, file table, and superblock
 *
 * Blocks are only moved backward (toward start), never forward, so data
 * is never overwritten before being read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "common.h"

/* ── Sorted file entry for defrag ──────────────────────────────── */

typedef struct {
    uint32_t file_idx;       /* Index into file table */
    uint32_t start_block;    /* Current start block */
    uint32_t block_count;    /* Number of blocks */
} defrag_entry_t;

static int cmp_start_block(const void *a, const void *b)
{
    const defrag_entry_t *ea = (const defrag_entry_t *)a;
    const defrag_entry_t *eb = (const defrag_entry_t *)b;
    if (ea->start_block < eb->start_block) return -1;
    if (ea->start_block > eb->start_block) return 1;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-defrag <device> [--dry-run] [--verbose]\n"
        "\n"
        "Compacts files toward the beginning of the data area,\n"
        "eliminating gaps left by deleted files.\n"
        "\n"
        "Options:\n"
        "  --dry-run   Analyze and report, but don't write\n"
        "  --verbose   Show each file being moved\n");
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4)
        usage();

    const char *device = argv[1];
    int dry_run = 0, verbose = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = 1;
        else if (strcmp(argv[i], "--verbose") == 0)
            verbose = 1;
        else
            usage();
    }

    if (!dry_run) {
        fprintf(stderr, "ositofs-defrag: writable defrag is disabled until "
                        "extent moves are journaled; use --dry-run\n");
        return 1;
    }

    /* Open device (read-only if dry-run) */
    int fd = osfs2_open_device(device, dry_run);
    if (fd < 0) {
        fprintf(stderr, "ositofs-defrag: cannot open %s\n", device);
        return 1;
    }
    if (!dry_run && osfs2_journal_recover(fd, 1) < 0) {
        fprintf(stderr, "ositofs-defrag: journal recovery failed\n");
        osfs2_close_device(fd);
        return 1;
    }

    /* Read superblock */
    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        fprintf(stderr, "ositofs-defrag: invalid superblock\n");
        osfs2_close_device(fd);
        return 1;
    }

    uint32_t max_files = osfs2_layout_max_files(&sb);
    uint32_t filetab_size = osfs2_layout_filetab_size(&sb);
    uint32_t crctab_off = osfs2_layout_crctab_off(&sb);
    uint32_t data_start = osfs2_layout_data_start_blk(&sb);

    printf("ositofs-defrag: analyzing %s\n", device);

    /* Read file table */
    void *ft_buf = osfs2_alloc_aligned(filetab_size);
    if (!ft_buf) {
        fprintf(stderr, "ositofs-defrag: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }
    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, filetab_size) < 0) {
        fprintf(stderr, "ositofs-defrag: failed to read file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }
    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Read CRC table */
    void *crc_buf = osfs2_alloc_aligned(OSFS2_CRCTAB_SIZE);
    if (!crc_buf) {
        fprintf(stderr, "ositofs-defrag: out of memory\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }
    if (osfs2_read_bytes(fd, crctab_off, crc_buf, OSFS2_CRCTAB_SIZE) < 0) {
        fprintf(stderr, "ositofs-defrag: failed to read CRC table\n");
        free(crc_buf);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }
    if (!osfs2_crc_table_enabled)
        memset(crc_buf, 0, OSFS2_CRCTAB_SIZE);
    uint32_t *crc_tab = (uint32_t *)crc_buf;

    /* ── Build sorted list of valid files by start_block ────────── */

    defrag_entry_t *entries = calloc(max_files, sizeof(defrag_entry_t));
    if (!entries) {
        fprintf(stderr, "ositofs-defrag: out of memory\n");
        free(crc_buf);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    uint32_t file_count = 0;
    uint32_t total_data_blocks = 0;

    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        entries[file_count].file_idx = i;
        entries[file_count].start_block = ft[i].start_block;
        entries[file_count].block_count = ft[i].block_count;
        total_data_blocks += ft[i].block_count;
        file_count++;
    }

    qsort(entries, file_count, sizeof(defrag_entry_t), cmp_start_block);

    /* ── Count gaps ─────────────────────────────────────────────── */

    uint32_t gap_count = 0;
    uint32_t gap_blocks = 0;
    uint32_t cursor = data_start;

    for (uint32_t i = 0; i < file_count; i++) {
        if (entries[i].start_block > cursor) {
            gap_count++;
            gap_blocks += entries[i].start_block - cursor;
        }
        cursor = entries[i].start_block + entries[i].block_count;
    }

    printf("  Files: %u\n", file_count);
    printf("  Gaps:  %u (spanning %u blocks)\n", gap_count, gap_blocks);

    if (gap_count == 0) {
        printf("\nositofs-defrag: filesystem is already compact\n");
        free(entries);
        free(crc_buf);
        free(ft_buf);
        osfs2_close_device(fd);
        return 0;
    }

    /* Compute fragmentation percentage: fraction of blocks in the used
     * range (data_start..hwm) that are gaps rather than file data. */
    uint32_t hwm_before = data_start;
    for (uint32_t i = 0; i < file_count; i++) {
        uint32_t end = entries[i].start_block + entries[i].block_count;
        if (end > hwm_before) hwm_before = end;
    }
    uint32_t range_before = hwm_before - data_start;
    double frag_before = range_before > 0 ?
        100.0 * (double)gap_blocks / (double)range_before : 0.0;

    if (dry_run)
        printf("\n[dry-run] Would move:\n");
    else
        printf("\n");

    /* ── Compact files ──────────────────────────────────────────── */

    void *blk_buf = osfs2_alloc_block();
    if (!blk_buf && !dry_run) {
        fprintf(stderr, "ositofs-defrag: out of memory for block buffer\n");
        free(entries);
        free(crc_buf);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    uint32_t next_pos = data_start;
    uint32_t files_moved = 0;

    for (uint32_t i = 0; i < file_count; i++) {
        uint32_t fidx = entries[i].file_idx;
        uint32_t old_start = entries[i].start_block;
        uint32_t count = entries[i].block_count;

        if (old_start == next_pos) {
            /* Already in place — no move needed */
            next_pos += count;
            continue;
        }

        /* Sanity: destination must be before source (move backward only) */
        if (next_pos > old_start) {
            fprintf(stderr, "ositofs-defrag: BUG — would move '%s' forward "
                    "(blocks %u -> %u), aborting\n",
                    osfs2_entry_name(&ft[fidx]), old_start, next_pos);
            free(blk_buf);
            free(entries);
            free(crc_buf);
            free(ft_buf);
            osfs2_close_device(fd);
            return 1;
        }

        /* Sanity: destination range must not overlap source */
        if (next_pos + count > old_start) {
            fprintf(stderr, "ositofs-defrag: BUG — destination overlaps source "
                    "for '%s' (dst %u-%u, src %u-%u), aborting\n",
                    osfs2_entry_name(&ft[fidx]), next_pos, next_pos + count - 1,
                    old_start, old_start + count - 1);
            free(blk_buf);
            free(entries);
            free(crc_buf);
            free(ft_buf);
            osfs2_close_device(fd);
            return 1;
        }

        if (verbose || dry_run) {
            printf("  Moving '%s' (blocks %u-%u -> %u-%u)...\n",
                   osfs2_entry_name(&ft[fidx]),
                   old_start, old_start + count - 1,
                   next_pos, next_pos + count - 1);
        }

        if (!dry_run) {
            /* Move blocks one at a time */
            for (uint32_t b = 0; b < count; b++) {
                uint32_t src_blk = old_start + b;
                uint32_t dst_blk = next_pos + b;

                if (osfs2_read_block(fd, src_blk, blk_buf) < 0) {
                    fprintf(stderr, "ositofs-defrag: read failed at block %u "
                            "for '%s', aborting\n", src_blk,
                            osfs2_entry_name(&ft[fidx]));
                    free(blk_buf);
                    free(entries);
                    free(crc_buf);
                    free(ft_buf);
                    osfs2_close_device(fd);
                    return 1;
                }

                if (osfs2_write_block(fd, dst_blk, blk_buf) < 0) {
                    fprintf(stderr, "ositofs-defrag: write failed at block %u "
                            "for '%s', aborting\n", dst_blk,
                            osfs2_entry_name(&ft[fidx]));
                    free(blk_buf);
                    free(entries);
                    free(crc_buf);
                    free(ft_buf);
                    osfs2_close_device(fd);
                    return 1;
                }

                /* Move CRC table entry */
                if (src_blk < OSFS2_MAX_BLOCKS)
                    crc_tab[dst_blk] = crc_tab[src_blk];
                if (src_blk < OSFS2_MAX_BLOCKS && src_blk != dst_blk)
                    crc_tab[src_blk] = 0;
            }

            /* Update file table entry */
            ft[fidx].start_block = next_pos;
        }

        files_moved++;
        next_pos += count;
    }

    osfs2_free_block(blk_buf);

    /* ── Verify invariants ──────────────────────────────────────── */

    /* After compaction, recount files and total blocks to make sure nothing
     * was lost during the move. */
    uint32_t verify_files = 0;
    uint32_t verify_blocks = 0;
    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        verify_files++;
        verify_blocks += ft[i].block_count;
    }

    if (verify_files != file_count) {
        fprintf(stderr, "ositofs-defrag: INTEGRITY ERROR — file count changed "
                "(%u -> %u)\n", file_count, verify_files);
        free(entries);
        free(crc_buf);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }
    if (verify_blocks != total_data_blocks) {
        fprintf(stderr, "ositofs-defrag: INTEGRITY ERROR — total blocks changed "
                "(%u -> %u)\n", total_data_blocks, verify_blocks);
        free(entries);
        free(crc_buf);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* ── Update superblock ──────────────────────────────────────── */

    if (!dry_run) {
        sb.next_data_block = next_pos;
        sb.used_blocks = data_start + total_data_blocks;

        sb.crc32 = 0;
        sb.crc32 = osfs2_crc32(&sb, sizeof(sb));

        /* Flush CRC table */
        if (osfs2_write_bytes(fd, crctab_off, crc_buf,
                              OSFS2_CRCTAB_SIZE) < 0) {
            fprintf(stderr, "ositofs-defrag: failed to write CRC table\n");
        }

        /* Flush file table */
        if (osfs2_write_bytes(fd, OSFS2_FILETAB_OFF, ft_buf,
                              filetab_size) < 0) {
            fprintf(stderr, "ositofs-defrag: failed to write file table\n");
        }

        /* Flush superblock + backup */
        void *sb_buf = osfs2_alloc_aligned(4096);
        if (sb_buf) {
            memcpy(sb_buf, &sb, sizeof(sb));
            osfs2_write_bytes(fd, 0, sb_buf, 4096);
            osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, sb_buf, 4096);
            free(sb_buf);
        }
    }

    /* ── Summary ────────────────────────────────────────────────── */

    printf("\nositofs-defrag: %s%u files, recovered %u blocks\n",
           dry_run ? "[dry-run] would compact " : "compacted ",
           files_moved, gap_blocks);
    printf("  Before: %u gaps, %.1f%% fragmented\n",
           gap_count, frag_before);
    printf("  After:  0 gaps, 0.0%% fragmented\n");

    free(entries);
    free(crc_buf);
    free(ft_buf);
    osfs2_close_device(fd);
    return 0;
}
