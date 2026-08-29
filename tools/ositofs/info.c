/*
 * ositofs-info — Show OsitoFS v2 filesystem info
 *
 * Usage: ositofs-info <device>
 *
 * Shows: superblock details, capacity, fragmentation analysis,
 *        file statistics, timestamp stats, and GGUF model summary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

/* For qsort: sort file regions by start_block ascending */
typedef struct {
    uint32_t start_block;
    uint32_t block_count;
    uint32_t file_idx;   /* index into file table */
} region_t;

static int region_cmp(const void *a, const void *b)
{
    const region_t *ra = (const region_t *)a;
    const region_t *rb = (const region_t *)b;
    if (ra->start_block < rb->start_block) return -1;
    if (ra->start_block > rb->start_block) return  1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: ositofs-info <device>\n");
        return 1;
    }

    int fd = osfs2_open_device(argv[1], 1);
    if (fd < 0) return 1;

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        osfs2_close_device(fd);
        return 1;
    }

    uint64_t dev_size = osfs2_device_size(fd);
    uint32_t max_files = osfs2_layout_max_files(&sb);
    uint32_t filetab_size = osfs2_layout_filetab_size(&sb);
    uint32_t metadata_bytes = osfs2_layout_data_off(&sb);

    printf("OsitoFS v2 — Filesystem Info\n");
    printf("════════════════════════════════════════\n");
    printf("Device:         %s\n", argv[1]);
    printf("Device size:    "); osfs2_print_size(dev_size); printf("\n");
    printf("Magic:          0x%08X (%s)\n", sb.magic,
           sb.magic == OSFS2_MAGIC ? "OK" : "BAD");
    printf("Version:        %u\n", sb.version);
    printf("Block size:     "); osfs2_print_size(sb.block_size); printf("\n");
    printf("Total blocks:   %u\n", sb.total_blocks);
    uint32_t data_start = osfs2_layout_data_start_blk(&sb);
    printf("Used blocks:    %u (metadata: %u, data: %u)\n",
           sb.used_blocks, data_start,
           sb.used_blocks > data_start ?
               sb.used_blocks - data_start : 0);
    printf("Free blocks:    %u\n", sb.total_blocks - sb.used_blocks);
    printf("File count:     %u / %u\n", sb.file_count, max_files);
    printf("Next data blk:  %u\n", sb.next_data_block);
    printf("Label:          %s\n", sb.label);

    printf("UUID:           ");
    for (int i = 0; i < 16; i++) {
        printf("%02x", sb.uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) printf("-");
    }
    printf("\n");

    time_t t = (time_t)sb.create_time;
    char timebuf[64];
    struct tm *tm = localtime(&t);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);
    printf("Created:        %s\n", timebuf);
    printf("CRC32:          0x%08X\n", sb.crc32);

    printf("\nCapacity:\n");
    uint32_t data_blks = sb.total_blocks - data_start;
    uint32_t used_data = sb.next_data_block > data_start ?
                         sb.next_data_block - data_start : 0;
    uint32_t free_data = data_blks - used_data;
    printf("  Data area:    "); osfs2_print_size((uint64_t)data_blks * sb.block_size);
    printf(" (%u blocks)\n", data_blks);
    printf("  Used:         "); osfs2_print_size((uint64_t)used_data * sb.block_size);
    printf(" (%.1f%%)\n", data_blks ? 100.0 * used_data / data_blks : 0.0);
    printf("  Free:         "); osfs2_print_size((uint64_t)free_data * sb.block_size);
    printf(" (%.1f%%)\n", data_blks ? 100.0 * free_data / data_blks : 0.0);
    printf("  Metadata:     "); osfs2_print_size(metadata_bytes);
    printf(" (offset 0-%uMB)\n", metadata_bytes / (1024 * 1024));

    /* ── Read file table for detailed analysis ──────────────────── */

    void *ft_blk = osfs2_alloc_aligned(filetab_size);
    if (!ft_blk) {
        osfs2_close_device(fd);
        return 1;
    }
    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_blk, filetab_size) < 0) {
        free(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_blk;
    osfs2_close_device(fd);

    /* Collect valid files and their block regions */
    uint32_t valid_count = 0;
    region_t *regions = (region_t *)malloc(max_files * sizeof(region_t));
    if (!regions) {
        free(ft_blk);
        return 1;
    }

    uint32_t raw_count = 0, gguf_count = 0;
    uint64_t raw_bytes = 0, gguf_bytes = 0, total_size = 0;
    uint32_t smallest_idx = 0, largest_idx = 0;
    uint64_t smallest_size = UINT64_MAX, largest_size = 0;
    uint32_t oldest_idx = 0, newest_idx = 0;
    uint32_t oldest_time = UINT32_MAX, newest_time = 0;

    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;

        if (regions) {
            regions[valid_count].start_block = ft[i].start_block;
            regions[valid_count].block_count = ft[i].block_count;
            regions[valid_count].file_idx    = i;
        }
        valid_count++;

        /* File type stats */
        if (ft[i].flags & OSFS2_FLAG_GGUF) {
            gguf_count++;
            gguf_bytes += ft[i].size;
        } else {
            raw_count++;
            raw_bytes += ft[i].size;
        }
        total_size += ft[i].size;

        /* Size extremes */
        if (ft[i].size < smallest_size) {
            smallest_size = ft[i].size;
            smallest_idx  = i;
        }
        if (ft[i].size > largest_size) {
            largest_size = ft[i].size;
            largest_idx  = i;
        }

        /* Timestamp extremes (use modify_time, fall back to create_time) */
        uint32_t ts = ft[i].modify_time ? ft[i].modify_time : ft[i].create_time;
        if (ts > 0 && ts < oldest_time) {
            oldest_time = ts;
            oldest_idx  = i;
        }
        if (ts > newest_time) {
            newest_time = ts;
            newest_idx  = i;
        }
    }

    /* ── Fragmentation Analysis ─────────────────────────────────── */

    printf("\nFragmentation:\n");
    if (valid_count == 0) {
        printf("  (no files — nothing to analyze)\n");
    } else {
        /* Sort regions by start_block */
        qsort(regions, valid_count, sizeof(region_t), region_cmp);

        uint32_t gap_count = 0;
        uint32_t largest_free_blks = 0;
        uint32_t total_data_end = data_start + data_blks;

        /* Gap before first allocation */
        if (regions[0].start_block > data_start) {
            uint32_t gap = regions[0].start_block - data_start;
            gap_count++;
            if (gap > largest_free_blks) largest_free_blks = gap;
        }

        /* Gaps between allocations */
        for (uint32_t i = 1; i < valid_count; i++) {
            uint32_t prev_end = regions[i - 1].start_block +
                                regions[i - 1].block_count;
            if (regions[i].start_block > prev_end) {
                uint32_t gap = regions[i].start_block - prev_end;
                gap_count++;
                if (gap > largest_free_blks) largest_free_blks = gap;
            }
        }

        /* Gap after last allocation to end of data area */
        uint32_t last_end = regions[valid_count - 1].start_block +
                            regions[valid_count - 1].block_count;
        if (last_end < total_data_end) {
            uint32_t gap = total_data_end - last_end;
            gap_count++;
            if (gap > largest_free_blks) largest_free_blks = gap;
        }

        double frag_pct = 0.0;
        if (gap_count + valid_count > 0)
            frag_pct = 100.0 * gap_count / (gap_count + valid_count);

        printf("  Allocated regions: %u\n", valid_count);
        printf("  Gaps:              %u\n", gap_count);
        printf("  Largest free:      %u blocks (", largest_free_blks);
        osfs2_print_size((uint64_t)largest_free_blks * sb.block_size);
        printf(")\n");
        printf("  Fragmentation:     %.1f%%\n", frag_pct);
    }

    /* ── File Statistics ────────────────────────────────────────── */

    printf("\nFiles:\n");
    if (valid_count == 0) {
        printf("  (empty filesystem)\n");
    } else {
        printf("  Total:    %u\n", valid_count);
        printf("  Raw:      %u (", raw_count);
        osfs2_print_size(raw_bytes);
        printf(")\n");
        printf("  GGUF:     %u (", gguf_count);
        osfs2_print_size(gguf_bytes);
        printf(")\n");
        printf("  Smallest: %s (", osfs2_entry_name(&ft[smallest_idx]));
        osfs2_print_size(smallest_size);
        printf(")\n");
        printf("  Largest:  %s (", osfs2_entry_name(&ft[largest_idx]));
        osfs2_print_size(largest_size);
        printf(")\n");
        printf("  Avg size: ");
        osfs2_print_size(total_size / valid_count);
        printf("\n");
    }

    /* ── Timestamp Statistics ───────────────────────────────────── */

    printf("\nTimestamps:\n");
    if (valid_count == 0 || oldest_time == UINT32_MAX) {
        printf("  (no timestamps recorded)\n");
    } else {
        char tbuf[64];

        if (oldest_time > 0 && oldest_time != UINT32_MAX) {
            time_t ot = (time_t)oldest_time;
            struct tm *otm = localtime(&ot);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", otm);
            printf("  Oldest:   %s (%s)\n", tbuf,
                   osfs2_entry_name(&ft[oldest_idx]));
        }

        if (newest_time > 0) {
            time_t nt = (time_t)newest_time;
            struct tm *ntm = localtime(&nt);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", ntm);
            printf("  Newest:   %s (%s)\n", tbuf,
                   osfs2_entry_name(&ft[newest_idx]));
        }
    }

    /* ── Model Summary (GGUF files only) ────────────────────────── */

    if (gguf_count > 0) {
        printf("\nModels:\n");
        for (uint32_t i = 0; i < max_files; i++) {
            if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
            if (!(ft[i].flags & OSFS2_FLAG_GGUF))  continue;

            printf("  %-28s %-8s", osfs2_entry_name(&ft[i]),
                   osfs2_quant_name(ft[i].quant_type));

            if (ft[i].num_layers > 0 || ft[i].hidden_size > 0 ||
                ft[i].vocab_size > 0) {
                printf("  %uL/%uH/%uV",
                       ft[i].num_layers, ft[i].hidden_size,
                       ft[i].vocab_size);
            }

            if (ft[i].model_name[0])
                printf("  %s", ft[i].model_name);

            printf("\n");
        }
    }

    free(regions);
    free(ft_blk);
    return 0;
}
