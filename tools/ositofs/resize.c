/*
 * ositofs-resize — Grow an OsitoFS v2 image/partition in place.
 *
 * This updates the superblock block count after a smaller OsitoFS image has
 * been copied to a larger partition. Existing file extents are preserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-resize <device> [--label <name>]\n"
        "\n"
        "Updates total_blocks to match the current device/file size.\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *device;
    const char *label = NULL;

    if (argc < 2)
        usage();

    device = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else {
            usage();
        }
    }

    int fd = osfs2_open_device(device, 0);
    if (fd < 0)
        return 1;

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        fprintf(stderr, "ositofs-resize: invalid OsitoFS v2 superblock\n");
        osfs2_close_device(fd);
        return 1;
    }

    uint64_t dev_size = osfs2_device_size(fd);
    if (dev_size == 0) {
        fprintf(stderr, "ositofs-resize: cannot determine device size\n");
        osfs2_close_device(fd);
        return 1;
    }

    uint64_t new_blocks64 = dev_size / sb.block_size;
    if (new_blocks64 > OSFS2_MAX_BLOCKS) {
        fprintf(stderr,
                "ositofs-resize: target has %llu blocks, max supported is %u\n",
                (unsigned long long)new_blocks64, OSFS2_MAX_BLOCKS);
        osfs2_close_device(fd);
        return 1;
    }

    if (new_blocks64 < sb.used_blocks) {
        fprintf(stderr,
                "ositofs-resize: target too small (%llu blocks, %u used)\n",
                (unsigned long long)new_blocks64, sb.used_blocks);
        osfs2_close_device(fd);
        return 1;
    }

    printf("OsitoFS v2 resize: %s\n", device);
    printf("  block size:   %u bytes\n", sb.block_size);
    printf("  old blocks:   %u\n", sb.total_blocks);
    printf("  new blocks:   %llu\n", (unsigned long long)new_blocks64);

    sb.total_blocks = (uint32_t)new_blocks64;
    if (label) {
        memset(sb.label, 0, sizeof(sb.label));
        strncpy(sb.label, label, sizeof(sb.label) - 1);
    }

    sb.crc32 = 0;
    sb.crc32 = osfs2_crc32(&sb, sizeof(sb));

    void *blk = osfs2_alloc_aligned(4096);
    if (!blk) {
        osfs2_close_device(fd);
        return 1;
    }
    memcpy(blk, &sb, sizeof(sb));

    if (osfs2_write_bytes(fd, 0, blk, 4096) < 0 ||
        osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, blk, 4096) < 0) {
        fprintf(stderr, "ositofs-resize: failed to write superblock\n");
        free(blk);
        osfs2_close_device(fd);
        return 1;
    }

    free(blk);
    osfs2_close_device(fd);
    printf("  free blocks:  %u\n", sb.total_blocks - sb.used_blocks);
    return 0;
}
