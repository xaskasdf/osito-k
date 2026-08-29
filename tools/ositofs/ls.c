/*
 * ositofs-ls — List files on OsitoFS v2 partition
 *
 * Usage: ositofs-ls <device>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: ositofs-ls <device>\n");
        return 1;
    }

    int fd = osfs2_open_device(argv[1], 1);
    if (fd < 0) return 1;

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        osfs2_close_device(fd);
        return 1;
    }

    if (sb.file_count == 0) {
        printf("OsitoFS v2 [%s] — empty filesystem\n", sb.label);
        osfs2_close_device(fd);
        return 0;
    }

    /* Read file table (1MB at fixed offset) */
    void *ft_blk = osfs2_alloc_aligned(OSFS2_FILETAB_SIZE);
    if (!ft_blk) { osfs2_close_device(fd); return 1; }
    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_blk, OSFS2_FILETAB_SIZE) < 0) {
        free(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_blk;

    printf("OsitoFS v2 [%s] — %u file(s)\n\n", sb.label, sb.file_count);
    printf("%-40s %12s %8s  %-16s  %s\n", "NAME", "SIZE", "BLOCKS", "MODIFIED", "TYPE");
    printf("%-40s %12s %8s  %-16s  %s\n",
           "────────────────────────────────────────",
           "────────────",
           "────────",
           "────────────────",
           "────────────────");

    uint64_t total_size = 0;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;

        printf("%-40s ", osfs2_entry_name(&ft[i]));

        /* Size */
        char sizebuf[32];
        if (ft[i].size >= (uint64_t)1024 * 1024 * 1024)
            snprintf(sizebuf, sizeof(sizebuf), "%.1f GB",
                     (double)ft[i].size / (1024.0 * 1024.0 * 1024.0));
        else if (ft[i].size >= 1024 * 1024)
            snprintf(sizebuf, sizeof(sizebuf), "%.1f MB",
                     (double)ft[i].size / (1024.0 * 1024.0));
        else
            snprintf(sizebuf, sizeof(sizebuf), "%llu B", (unsigned long long)ft[i].size);
        printf("%12s ", sizebuf);

        printf("%8u  ", ft[i].block_count);

        if (ft[i].modify_time > 0) {
            time_t t = (time_t)ft[i].modify_time;
            struct tm *tm = localtime(&t);
            char tbuf[20];
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", tm);
            printf("%-16s  ", tbuf);
        } else {
            printf("%-16s  ", "-");
        }

        if (ft[i].flags & OSFS2_FLAG_GGUF)
            printf("%s %s", osfs2_quant_name(ft[i].quant_type), ft[i].model_name);
        else
            printf("raw");
        printf("\n");

        total_size += ft[i].size;
    }

    printf("\nTotal: "); osfs2_print_size(total_size);
    uint32_t data_start = osfs2_format_data_start_blk(sb.version,
                                                       sb.block_size);
    uint32_t data_blocks = sb.total_blocks - data_start;
    uint32_t used_data = sb.used_blocks > data_start ? sb.used_blocks - data_start : 0;
    printf(" in %u blocks (%u/%u data blocks used, %.1f%%, block_size=",
           used_data, used_data, data_blocks,
           100.0 * used_data / data_blocks);
    osfs2_print_size(sb.block_size);
    printf(")\n");

    free(ft_blk);
    osfs2_close_device(fd);
    return 0;
}
