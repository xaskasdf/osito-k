/*
 * ositofs-fsck — Filesystem consistency checker for OsitoFS v2
 *
 * Usage: ositofs-fsck <device> [--repair] [--verbose]
 *
 * Checks:
 *   1. Superblock validation (primary vs backup)
 *   2. File table scan (valid entries, bounds, overlaps, duplicates)
 *   3. Block accounting (used_blocks, next_data_block, orphans)
 *   4. Block CRC verification (--verbose only)
 *   5. Layer index validation (slot bounds, orphaned slots)
 *
 * With --repair: fixes superblock, file_count, used_blocks,
 * next_data_block, and clears orphaned layer index slots.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "common.h"

/* ── Superblock validation (raw, bypasses osfs2_read_super) ──── */

static int validate_super_raw(const osfs2_super_t *sb)
{
    if (sb->magic != OSFS2_MAGIC) return -1;
    if (sb->version != OSFS2_VERSION) return -1;
    osfs2_super_t tmp;
    memcpy(&tmp, sb, sizeof(tmp));
    tmp.crc32 = 0;
    uint32_t calc = osfs2_crc32(&tmp, sizeof(tmp));
    if (calc != sb->crc32) return -1;
    if (!osfs2_valid_block_size(sb->block_size)) return -1;
    return 0;
}

/* ── Comparison helper for qsort ─────────────────────────────── */

typedef struct {
    uint32_t start;
    uint32_t count;
    uint32_t file_idx;
} block_range_t;

static int cmp_range(const void *a, const void *b)
{
    const block_range_t *ra = (const block_range_t *)a;
    const block_range_t *rb = (const block_range_t *)b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: ositofs-fsck <device> [--repair] [--verbose]\n");
        return 1;
    }

    const char *device = argv[1];
    int repair = 0, verbose = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--repair") == 0) repair = 1;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else {
            fprintf(stderr, "ositofs-fsck: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    int errors = 0;
    int repaired = 0;

    /* Open device (read-write only if --repair) */
    int fd = osfs2_open_device(device, repair ? 0 : 1);
    if (fd < 0) return 1;

    printf("ositofs-fsck: checking %s\n", device);

    /* ── 1. Superblock validation ────────────────────────────── */

    void *sb_buf = osfs2_alloc_aligned(4096);
    void *sb_bak_buf = osfs2_alloc_aligned(4096);
    if (!sb_buf || !sb_bak_buf) {
        fprintf(stderr, "ositofs-fsck: out of memory\n");
        free(sb_buf); free(sb_bak_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_super_t sb_primary, sb_backup;
    int primary_ok = 0, backup_ok = 0;

    if (osfs2_read_bytes(fd, 0, sb_buf, 4096) == 0) {
        memcpy(&sb_primary, sb_buf, sizeof(sb_primary));
        primary_ok = (validate_super_raw(&sb_primary) == 0);
    }
    if (osfs2_read_bytes(fd, OSFS2_SUPER_BACKUP_OFF, sb_bak_buf, 4096) == 0) {
        memcpy(&sb_backup, sb_bak_buf, sizeof(sb_backup));
        backup_ok = (validate_super_raw(&sb_backup) == 0);
    }

    if (primary_ok && backup_ok) {
        if (memcmp(&sb_primary, &sb_backup, sizeof(osfs2_super_t)) == 0)
            printf("  Superblock: OK (primary and backup match)\n");
        else {
            printf("  Superblock: WARNING (primary and backup differ)\n");
            errors++;
        }
    } else if (primary_ok && !backup_ok) {
        printf("  Superblock: WARNING (backup CORRUPT, primary OK)\n");
        errors++;
        if (repair) {
            osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, sb_buf, 4096);
            printf("    -> repaired: copied primary to backup\n");
            repaired++;
        }
    } else if (!primary_ok && backup_ok) {
        printf("  Superblock: PRIMARY CORRUPT (backup OK)\n");
        errors++;
        if (repair) {
            osfs2_write_bytes(fd, 0, sb_bak_buf, 4096);
            printf("    -> repaired: copied backup to primary\n");
            repaired++;
        }
    } else {
        printf("  Superblock: BOTH CORRUPT\n");
        fprintf(stderr, "ositofs-fsck: fatal — no valid superblock found\n");
        free(sb_buf); free(sb_bak_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Use whichever superblock is valid as our working copy */
    osfs2_super_t sb;
    if (primary_ok)
        memcpy(&sb, &sb_primary, sizeof(sb));
    else
        memcpy(&sb, &sb_backup, sizeof(sb));

    osfs2_block_sz = sb.block_size;
    uint32_t data_start = osfs2_data_start_blk(sb.block_size);

    free(sb_buf); sb_buf = NULL;
    free(sb_bak_buf); sb_bak_buf = NULL;

    /* ── 2. File table scan ──────────────────────────────────── */

    void *ft_buf = osfs2_alloc_aligned(OSFS2_FILETAB_SIZE);
    if (!ft_buf) {
        fprintf(stderr, "ositofs-fsck: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }
    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, OSFS2_FILETAB_SIZE) < 0) {
        fprintf(stderr, "ositofs-fsck: failed to read file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Count valid entries and collect block ranges */
    uint32_t actual_file_count = 0;
    block_range_t *ranges = calloc(OSFS2_MAX_FILES, sizeof(block_range_t));
    uint32_t range_count = 0;

    int ft_errors = 0;

    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        actual_file_count++;

        /* Bounds check: start_block */
        if (ft[i].start_block < data_start) {
            printf("  File [%u] '%s': start_block %u < data_start %u\n",
                   i, ft[i].name, ft[i].start_block, data_start);
            ft_errors++;
        }

        /* Bounds check: end within device */
        uint32_t end = ft[i].start_block + ft[i].block_count;
        if (end > sb.total_blocks) {
            printf("  File [%u] '%s': end block %u > total_blocks %u\n",
                   i, ft[i].name, end, sb.total_blocks);
            ft_errors++;
        }

        /* Null-terminated name check */
        int name_ok = 0;
        for (int j = 0; j < OSFS2_NAME_LEN; j++) {
            if (ft[i].name[j] == '\0') { name_ok = 1; break; }
        }
        if (!name_ok) {
            printf("  File [%u]: name not null-terminated\n", i);
            ft_errors++;
        }

        /* Size vs block_count consistency */
        if (ft[i].block_count > 0) {
            uint64_t max_size = (uint64_t)ft[i].block_count * sb.block_size;
            uint64_t min_size = (uint64_t)(ft[i].block_count - 1) * sb.block_size + 1;
            if (ft[i].size > max_size) {
                printf("  File [%u] '%s': size %llu exceeds %u blocks capacity (%llu)\n",
                       i, ft[i].name, (unsigned long long)ft[i].size,
                       ft[i].block_count, (unsigned long long)max_size);
                ft_errors++;
            }
            (void)min_size; /* size can be smaller (padding is fine) */
        } else if (ft[i].size > 0) {
            printf("  File [%u] '%s': nonzero size but block_count=0\n",
                   i, ft[i].name);
            ft_errors++;
        }

        /* Collect range for overlap check */
        if (ft[i].block_count > 0) {
            ranges[range_count].start = ft[i].start_block;
            ranges[range_count].count = ft[i].block_count;
            ranges[range_count].file_idx = i;
            range_count++;
        }
    }

    /* File count check */
    if (actual_file_count == sb.file_count) {
        printf("  File table: %u valid files (file_count=%u OK)\n",
               actual_file_count, sb.file_count);
    } else {
        printf("  File table: %u valid files (file_count=%u MISMATCH)\n",
               actual_file_count, sb.file_count);
        errors++;
        if (repair) {
            sb.file_count = actual_file_count;
            printf("    -> repaired: file_count set to %u\n", actual_file_count);
            repaired++;
        }
    }
    errors += ft_errors;

    /* Duplicate filename check */
    int dup_errors = 0;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        for (uint32_t j = i + 1; j < OSFS2_MAX_FILES; j++) {
            if (!(ft[j].flags & OSFS2_FLAG_VALID)) continue;
            if (strcmp(ft[i].name, ft[j].name) == 0) {
                printf("  DUPLICATE filename '%s' at slots %u and %u\n",
                       ft[i].name, i, j);
                dup_errors++;
            }
        }
    }
    if (dup_errors > 0) {
        printf("  Duplicate filenames: %d found\n", dup_errors);
        errors += dup_errors;
    }

    /* ── Block range overlap check ───────────────────────────── */

    qsort(ranges, range_count, sizeof(block_range_t), cmp_range);

    int overlap_errors = 0;
    for (uint32_t i = 1; i < range_count; i++) {
        uint32_t prev_end = ranges[i - 1].start + ranges[i - 1].count;
        if (ranges[i].start < prev_end) {
            printf("  OVERLAP: files [%u] '%s' (blocks %u-%u) and [%u] '%s' (blocks %u-%u)\n",
                   ranges[i - 1].file_idx,
                   ft[ranges[i - 1].file_idx].name,
                   ranges[i - 1].start,
                   ranges[i - 1].start + ranges[i - 1].count - 1,
                   ranges[i].file_idx,
                   ft[ranges[i].file_idx].name,
                   ranges[i].start,
                   ranges[i].start + ranges[i].count - 1);
            overlap_errors++;
        }
    }
    if (overlap_errors == 0)
        printf("  Block ranges: OK (no overlaps)\n");
    else {
        printf("  Block ranges: %d overlap(s) found\n", overlap_errors);
        errors += overlap_errors;
    }

    /* ── 3. Block accounting ─────────────────────────────────── */

    /* Compute actual used blocks (metadata blocks + data blocks from files) */
    uint32_t actual_used = data_start; /* metadata always counts */
    for (uint32_t i = 0; i < range_count; i++)
        actual_used += ranges[i].count;

    if (sb.used_blocks == actual_used) {
        printf("  Block accounting: used_blocks=%u (expected %u) OK\n",
               sb.used_blocks, actual_used);
    } else {
        printf("  Block accounting: used_blocks=%u (expected %u) MISMATCH\n",
               sb.used_blocks, actual_used);
        errors++;
        if (repair) {
            sb.used_blocks = actual_used;
            printf("    -> repaired: used_blocks set to %u\n", actual_used);
            repaired++;
        }
    }

    /* Compute actual high-water mark */
    uint32_t actual_hwm = data_start;
    for (uint32_t i = 0; i < range_count; i++) {
        uint32_t end = ranges[i].start + ranges[i].count;
        if (end > actual_hwm) actual_hwm = end;
    }

    if (sb.next_data_block == actual_hwm) {
        printf("  High-water mark: next_data_block=%u (expected %u) OK\n",
               sb.next_data_block, actual_hwm);
    } else {
        printf("  High-water mark: next_data_block=%u (expected %u) MISMATCH\n",
               sb.next_data_block, actual_hwm);
        errors++;
        if (repair) {
            sb.next_data_block = actual_hwm;
            printf("    -> repaired: next_data_block set to %u\n", actual_hwm);
            repaired++;
        }
    }

    /* Orphaned blocks: blocks between data_start and hwm not covered by any file.
     * We already have the sorted ranges, so walk them and count gaps. */
    uint32_t orphaned = 0;
    if (range_count > 0) {
        /* Gap before first file */
        if (ranges[0].start > data_start)
            orphaned += ranges[0].start - data_start;
        /* Gaps between files */
        for (uint32_t i = 1; i < range_count; i++) {
            uint32_t prev_end = ranges[i - 1].start + ranges[i - 1].count;
            if (ranges[i].start > prev_end)
                orphaned += ranges[i].start - prev_end;
        }
    }
    printf("  Orphaned blocks: %u\n", orphaned);
    if (orphaned > 0) errors += (int)orphaned; /* each orphaned block is an issue */

    free(ranges);

    /* ── 4. Block CRC verification (--verbose) ───────────────── */

    if (verbose) {
        void *crc_buf = osfs2_alloc_aligned(OSFS2_CRCTAB_SIZE);
        if (!crc_buf) {
            fprintf(stderr, "ositofs-fsck: out of memory for CRC table\n");
        } else if (osfs2_read_bytes(fd, OSFS2_CRCTAB_OFF, crc_buf,
                                    OSFS2_CRCTAB_SIZE) < 0) {
            fprintf(stderr, "ositofs-fsck: failed to read CRC table\n");
            free(crc_buf);
            crc_buf = NULL;
        }

        if (crc_buf) {
            uint32_t *crc_tab = (uint32_t *)crc_buf;
            void *data_blk = osfs2_alloc_block();
            uint32_t files_checked = 0, crc_errors = 0;

            if (!data_blk) {
                fprintf(stderr, "ositofs-fsck: out of memory for data block\n");
            } else {
                for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
                    if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;

                    int file_ok = 1;
                    for (uint32_t b = 0; b < ft[i].block_count; b++) {
                        uint32_t blk = ft[i].start_block + b;
                        if (blk >= OSFS2_MAX_BLOCKS) {
                            printf("  CRC: file '%s' block %u out of CRC table range\n",
                                   ft[i].name, blk);
                            file_ok = 0;
                            break;
                        }

                        uint32_t stored_crc = crc_tab[blk];
                        if (stored_crc == 0) continue; /* no CRC recorded */

                        if (osfs2_read_block(fd, blk, data_blk) < 0) {
                            printf("  CRC: file '%s' block %u read error\n",
                                   ft[i].name, blk);
                            file_ok = 0;
                            break;
                        }

                        /* For the last block, only hash up to file size */
                        uint64_t remaining = ft[i].size -
                            (uint64_t)b * osfs2_block_sz;
                        size_t hash_len = (remaining < osfs2_block_sz) ?
                            (size_t)remaining : osfs2_block_sz;

                        uint32_t calc = osfs2_crc32(data_blk, hash_len);
                        if (calc != stored_crc) {
                            printf("  CRC MISMATCH: file '%s' block %u "
                                   "(stored=0x%08X, calc=0x%08X)\n",
                                   ft[i].name, blk, stored_crc, calc);
                            file_ok = 0;
                        }
                    }
                    files_checked++;
                    if (!file_ok) crc_errors++;
                }
                osfs2_free_block(data_blk);
            }

            printf("  Block CRCs: %u/%u files verified",
                   files_checked - crc_errors, files_checked);
            if (crc_errors > 0) {
                printf(" (%u with errors)", crc_errors);
                errors += crc_errors;
            }
            printf("\n");
            free(crc_buf);
        }
    }

    /* ── 5. Layer index validation ───────────────────────────── */

    void *li_buf = osfs2_alloc_aligned(OSFS2_LAYERIDX_SIZE);
    if (!li_buf) {
        fprintf(stderr, "ositofs-fsck: out of memory for layer index\n");
    } else if (osfs2_read_bytes(fd, OSFS2_LAYERIDX_OFF, li_buf,
                                OSFS2_LAYERIDX_SIZE) < 0) {
        fprintf(stderr, "ositofs-fsck: failed to read layer index\n");
        free(li_buf);
        li_buf = NULL;
    }

    if (li_buf) {
        osfs2_layer_idx_t *li = (osfs2_layer_idx_t *)li_buf;

        /* Track which slots are referenced by valid files */
        uint8_t slot_used[OSFS2_MAX_MODELS];
        memset(slot_used, 0, sizeof(slot_used));

        uint32_t slots_referenced = 0;
        int li_errors = 0;

        for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
            if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
            if (ft[i].layer_index_slot == 0xFFFF) continue;

            uint16_t slot = ft[i].layer_index_slot;
            if (slot >= OSFS2_MAX_MODELS) {
                printf("  Layer index: file '%s' slot %u out of range [0, %u)\n",
                       ft[i].name, slot, OSFS2_MAX_MODELS);
                li_errors++;
                continue;
            }

            slot_used[slot] = 1;
            slots_referenced++;

            /* Validate layer offsets are within file size */
            uint32_t nl = li[slot].num_layers;
            if (nl > OSFS2_MAX_LAYERS) {
                printf("  Layer index: slot %u has num_layers=%u > MAX_LAYERS=%u\n",
                       slot, nl, OSFS2_MAX_LAYERS);
                li_errors++;
            } else {
                for (uint32_t l = 0; l < nl; l++) {
                    if (li[slot].layer_offset[l] > ft[i].size) {
                        printf("  Layer index: slot %u layer %u offset %llu > "
                               "file '%s' size %llu\n",
                               slot, l,
                               (unsigned long long)li[slot].layer_offset[l],
                               ft[i].name,
                               (unsigned long long)ft[i].size);
                        li_errors++;
                    }
                }
            }
        }

        /* Find orphaned layer index slots (have data but no file references them) */
        uint32_t orphaned_slots = 0;
        for (uint32_t s = 0; s < OSFS2_MAX_MODELS; s++) {
            if (slot_used[s]) continue;
            /* Check if slot has any non-zero content */
            int has_data = 0;
            if (li[s].num_layers > 0) has_data = 1;
            if (!has_data) {
                for (uint32_t l = 0; l < OSFS2_MAX_LAYERS; l++) {
                    if (li[s].layer_offset[l] != 0) { has_data = 1; break; }
                }
            }
            if (has_data) {
                orphaned_slots++;
                if (verbose)
                    printf("  Layer index: orphaned slot %u (num_layers=%u)\n",
                           s, li[s].num_layers);
                if (repair) {
                    memset(&li[s], 0, sizeof(osfs2_layer_idx_t));
                }
            }
        }

        printf("  Layer index: %u slots used, %u orphaned\n",
               slots_referenced, orphaned_slots);

        if (orphaned_slots > 0) {
            errors += orphaned_slots;
            if (repair) {
                if (osfs2_write_bytes(fd, OSFS2_LAYERIDX_OFF, li_buf,
                                      OSFS2_LAYERIDX_SIZE) == 0) {
                    printf("    -> repaired: cleared %u orphaned slot(s)\n",
                           orphaned_slots);
                    repaired++;
                }
            }
        }
        errors += li_errors;

        free(li_buf);
    }

    /* ── Write repaired superblock ───────────────────────────── */

    if (repair && repaired > 0) {
        sb.crc32 = 0;
        sb.crc32 = osfs2_crc32(&sb, sizeof(sb));

        void *wb = osfs2_alloc_aligned(4096);
        if (wb) {
            memset(wb, 0, 4096);
            memcpy(wb, &sb, sizeof(sb));
            osfs2_write_bytes(fd, 0, wb, 4096);
            osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, wb, 4096);
            free(wb);
        }
    }

    /* ── Summary ─────────────────────────────────────────────── */

    free(ft_buf);
    osfs2_close_device(fd);

    if (errors == 0) {
        printf("ositofs-fsck: filesystem is clean\n");
        return 0;
    }

    if (repair && repaired > 0) {
        printf("ositofs-fsck: %d error(s) found, %d repaired\n",
               errors, repaired);
    } else if (repair) {
        printf("ositofs-fsck: %d error(s) found, none could be auto-repaired\n",
               errors);
    } else {
        printf("ositofs-fsck: %d error(s) found (use --repair to fix)\n",
               errors);
    }
    return 1;
}
