/*
 * ositofs-info — Show OsitoFS v2 filesystem info
 *
 * Usage: ositofs-info <device>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

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
    osfs2_close_device(fd);

    printf("OsitoFS v2 — Filesystem Info\n");
    printf("════════════════════════════════════════\n");
    printf("Device:         %s\n", argv[1]);
    printf("Device size:    "); osfs2_print_size(dev_size); printf("\n");
    printf("Magic:          0x%08X (%s)\n", sb.magic,
           sb.magic == OSFS2_MAGIC ? "OK" : "BAD");
    printf("Version:        %u\n", sb.version);
    printf("Block size:     %u bytes (1 MB)\n", sb.block_size);
    printf("Total blocks:   %u\n", sb.total_blocks);
    printf("Used blocks:    %u (metadata: %u, data: %u)\n",
           sb.used_blocks, OSFS2_DATA_START_BLK,
           sb.used_blocks > OSFS2_DATA_START_BLK ?
               sb.used_blocks - OSFS2_DATA_START_BLK : 0);
    printf("Free blocks:    %u\n", sb.total_blocks - sb.used_blocks);
    printf("File count:     %u / %u\n", sb.file_count, OSFS2_MAX_FILES);
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
    uint32_t data_blocks = sb.total_blocks - OSFS2_DATA_START_BLK;
    uint32_t used_data = sb.next_data_block > OSFS2_DATA_START_BLK ?
                         sb.next_data_block - OSFS2_DATA_START_BLK : 0;
    uint32_t free_data = data_blocks - used_data;
    printf("  Data area:    %u MB (%u blocks)\n", data_blocks, data_blocks);
    printf("  Used:         %u MB (%.1f%%)\n", used_data,
           100.0 * used_data / data_blocks);
    printf("  Free:         %u MB (%.1f%%)\n", free_data,
           100.0 * free_data / data_blocks);
    printf("  Metadata:     4 MB (blocks 0-3)\n");

    return 0;
}
