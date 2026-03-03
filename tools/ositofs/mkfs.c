/*
 * mkfs.ositofs — Format device with OsitoFS v2
 *
 * Usage: mkfs.ositofs <device> [--label <name>]
 *
 * Writes superblock, empty file table, empty CRC table,
 * and empty layer index. All metadata fits in 4 blocks (4MB).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

static void usage(void)
{
    fprintf(stderr, "Usage: mkfs.ositofs <device> [--label <name>]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *label = "osito-ai";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (argv[i][0] != '-') {
            device = argv[i];
        } else {
            usage();
        }
    }
    if (!device) usage();

    int fd = osfs2_open_device(device, 0);
    if (fd < 0) return 1;

    uint64_t dev_size = osfs2_device_size(fd);
    uint32_t total_blocks = (uint32_t)(dev_size >> OSFS2_BLOCK_SHIFT);

    if (total_blocks < OSFS2_DATA_START_BLK + 1) {
        fprintf(stderr, "mkfs: device too small (need at least %u MB, have %u MB)\n",
                OSFS2_DATA_START_BLK + 1, total_blocks);
        osfs2_close_device(fd);
        return 1;
    }

    printf("mkfs.ositofs: formatting %s\n", device);
    printf("  Device size: "); osfs2_print_size(dev_size); printf("\n");
    printf("  Total blocks: %u (1MB each)\n", total_blocks);
    printf("  Data blocks: %u (starting at block %u)\n",
           total_blocks - OSFS2_DATA_START_BLK, OSFS2_DATA_START_BLK);
    printf("  Label: %s\n", label);

    void *blk = osfs2_alloc_block();
    if (!blk) { osfs2_close_device(fd); return 1; }

    /* ── Block 0: Superblock ─────────────────────────────────── */
    osfs2_super_t *sb = (osfs2_super_t *)blk;
    sb->magic = OSFS2_MAGIC;
    sb->version = OSFS2_VERSION;
    sb->block_size = OSFS2_BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->used_blocks = OSFS2_DATA_START_BLK;  /* metadata only */
    sb->file_count = 0;
    sb->next_data_block = OSFS2_DATA_START_BLK;
    osfs2_gen_uuid(sb->uuid);
    strncpy(sb->label, label, OSFS2_LABEL_LEN - 1);
    sb->create_time = (uint64_t)time(NULL);
    sb->crc32 = 0;
    sb->crc32 = osfs2_crc32(sb, sizeof(*sb));

    if (osfs2_write_block(fd, OSFS2_SUPERBLOCK_BLK, blk) < 0)
        goto fail;
    printf("  [OK] Superblock written\n");

    /* ── Block 1: File Table (zeroed = no files) ─────────────── */
    memset(blk, 0, OSFS2_BLOCK_SIZE);
    if (osfs2_write_block(fd, OSFS2_FILETAB_BLK, blk) < 0)
        goto fail;
    printf("  [OK] File table written (%u slots)\n", OSFS2_MAX_FILES);

    /* ── Block 2: Block CRC Table (zeroed) ───────────────────── */
    if (osfs2_write_block(fd, OSFS2_CRCTAB_BLK, blk) < 0)
        goto fail;
    printf("  [OK] Block CRC table written\n");

    /* ── Block 3: Layer Index (zeroed) ───────────────────────── */
    if (osfs2_write_block(fd, OSFS2_LAYERIDX_BLK, blk) < 0)
        goto fail;
    printf("  [OK] Layer index written (%u slots)\n", OSFS2_MAX_MODELS);

    printf("\nFormatting complete. Metadata overhead: 4 MB / ");
    osfs2_print_size(dev_size);
    printf(" (%.2f%%)\n", 100.0 * 4.0 * OSFS2_BLOCK_SIZE / dev_size);

    osfs2_free_block(blk);
    osfs2_close_device(fd);
    return 0;

fail:
    osfs2_free_block(blk);
    osfs2_close_device(fd);
    return 1;
}
