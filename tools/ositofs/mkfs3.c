/*
 * mkfs.ositofs3 — Format device with OsitoFS v3
 *
 * Usage: mkfs.ositofs3 <device> [--label <name>] [--inodes <count>]
 *
 * Block Layout:
 *   0: Superblock
 *   1: Inode Bitmap
 *   2: Block Bitmap
 *   3..K: Inode Table
 *   K+1: Root Directory Data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common_v3.h"

static void usage(void)
{
    fprintf(stderr, "Usage: mkfs.ositofs3 <device> [--label <name>] [--inodes <count>]\n");
    fprintf(stderr, "  --inodes: total inodes, multiple of %u (default %u)\n",
            (uint32_t)OSFS3_INODES_PER_BLOCK, OSFS3_DEFAULT_INODES);
    exit(1);
}

static uint32_t recommended_inode_count(uint32_t total_blocks)
{
    uint32_t inodes = total_blocks;
    if (inodes < OSFS3_DEFAULT_INODES) inodes = OSFS3_DEFAULT_INODES;
    if (inodes > OSFS3_MAX_INODES) inodes = OSFS3_MAX_INODES;

    while (inodes > 2U &&
           OSFS3_FIRST_DATA_BLOCK(inodes) + 1U >= total_blocks)
        inodes /= 2U;
    return inodes;
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *label = "ositok-root";
    uint32_t total_inodes = OSFS3_DEFAULT_INODES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            label = argv[++i];
        } else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            total_inodes = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] != '-') {
            device = argv[i];
        } else {
            usage();
        }
        else if (argv[i][0] != '-' && !device)
            device = argv[i];
        else
            usage();
    }
    if (!device || strlen(label) >= sizeof(((osfs3_super_t *)0)->label))
        usage();

    int fd = osfs3_open_device(device, 0);
    if (fd < 0) return 1;

    uint64_t dev_size = osfs3_device_size(fd);
    uint32_t total_blocks = (uint32_t)(dev_size >> OSFS3_BLOCK_SHIFT);
    uint32_t inode_table_blocks = osfs3_inode_table_blocks(total_inodes);
    uint32_t root_data_block = osfs3_first_data_block_for_inodes(total_inodes);

    if (!osfs3_valid_inode_count(total_inodes)) {
        fprintf(stderr,
                "mkfs: invalid inode count %u (must be %u..%u, multiple of %u)\n",
                total_inodes, (uint32_t)OSFS3_INODES_PER_BLOCK, osfs3_max_inodes(),
                (uint32_t)OSFS3_INODES_PER_BLOCK);
        osfs3_close_device(fd);
        return 1;
    }
    if (total_blocks > osfs3_max_blocks()) {
        fprintf(stderr, "mkfs: device has %u blocks, bitmap supports max %u\n",
                total_blocks, osfs3_max_blocks());
        osfs3_close_device(fd);
        return 1;
    }

    /* Metadata requires SB, inode bitmap, block bitmap, inode table, root dir. */
    if (total_blocks < root_data_block + 1) {
        fprintf(stderr, "mkfs: device too small\n");
        osfs3_close_device(fd);
        return 1;
    }

    printf("mkfs.ositofs3: formatting %s\n", device);
    printf("  Device size: "); osfs3_print_size(dev_size); printf("\n");
    printf("  Total blocks: %u (1MB each)\n", total_blocks);
    printf("  Inodes: %u (%u inode-table blocks)\n",
           total_inodes, inode_table_blocks);
    printf("  Data blocks: %u (starting at block %u)\n",
           total_blocks - root_data_block, root_data_block);
    printf("  Label: %s\n", label);

    void *block = osfs3_alloc_block();
    void *inode_bitmap = osfs3_alloc_block();
    void *block_bitmap = osfs3_alloc_block();
    if (!block || !inode_bitmap || !block_bitmap) goto fail;

    /* ── Block 0: Superblock ─────────────────────────────────── */
    osfs3_super_t *sb = (osfs3_super_t *)blk;
    sb->magic = OSFS3_MAGIC;
    sb->version = OSFS3_VERSION;
    sb->block_size = OSFS3_BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->total_inodes = total_inodes;
    sb->free_blocks = total_blocks - (root_data_block + 1);
    sb->free_inodes = sb->total_inodes - 1; /* Inode 1 is root */
    sb->first_data_block = root_data_block;
    sb->root_inode = 1;
    osfs3_gen_uuid(sb->uuid);
    strncpy(sb->label, label, 31);
    sb->create_time = (uint64_t)time(NULL);
    sb->crc32 = 0;
    sb->crc32 = osfs3_crc32(sb, sizeof(*sb));

    memset(block, 0, OSFS3_BLOCK_SIZE);
    memcpy(block, &super, sizeof(super));
    if (osfs3_write_block(fd, OSFS3_SUPERBLOCK_BLK, block) < 0) goto fail;

    bitmap_set(inode_bitmap, 0);
    bitmap_set(inode_bitmap, 1);
    if (osfs3_write_block(fd, OSFS3_INODE_BITMAP_BLK, inode_bitmap) < 0)
        goto fail;

    /* ── Block 2: Block Bitmap ───────────────────────────────── */
    memset(blk, 0, OSFS3_BLOCK_SIZE);
    uint8_t *bmap = (uint8_t *)blk;
    for (uint32_t b = 0; b <= root_data_block; b++)
        bmap[b / 8] |= (uint8_t)(1u << (b % 8));
    if (osfs3_write_block(fd, 2, blk) < 0) goto fail;
    printf("  [OK] Block bitmap written\n");

    /* ── Blocks 3..K: Inode Table ─────────────────────────────── */
    memset(blk, 0, OSFS3_BLOCK_SIZE);
    osfs3_inode_t *inodes = (osfs3_inode_t *)blk;
    
    /* Root Inode (index 1) */
    inodes[1].mode = OSFS3_S_IFDIR | 0755;
    inodes[1].nlink = 2;
    inodes[1].size = OSFS3_BLOCK_SIZE;
    inodes[1].atime = inodes[1].mtime = inodes[1].ctime = sb->create_time;
    inodes[1].extent_count = 1;
    inodes[1].extents[0].start_block = root_data_block;
    inodes[1].extents[0].block_count = 1;

    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        if (i > 0)
            memset(blk, 0, OSFS3_BLOCK_SIZE);
        if (osfs3_write_block(fd, OSFS3_INODE_TABLE_BLK + i, blk) < 0)
            goto fail;
    }
    printf("  [OK] Inode table written\n");

    /* ── Root Directory Data ─────────────────────────────────── */
    memset(blk, 0, OSFS3_BLOCK_SIZE);
    /* In a directory block, we could pre-initialize '.' and '..' */
    osfs3_dentry_t *de = (osfs3_dentry_t *)blk;
    
    /* Entry: . */
    de->inode = 1;
    de->name_len = 1;
    de->type = OSFS3_DT_DIR;
    memcpy(de->name, ".", 1);
    de->rec_len = OSFS3_DIR_REC_LEN(1);
    
    /* Entry: .. */
    osfs3_dentry_t *de2 = (osfs3_dentry_t *)((char *)de + de->rec_len);
    de2->inode = 1;
    de2->name_len = 2;
    de2->type = OSFS3_DT_DIR;
    memcpy(de2->name, "..", 2);
    de2->rec_len = OSFS3_BLOCK_SIZE - de->rec_len; /* Fill the rest of the block */
    
    if (osfs3_write_block(fd, root_data_block, blk) < 0) goto fail;
    printf("  [OK] Root directory initialized\n");

    printf("\nFormatting complete.\n");

    osfs3_free_block(blk);
    osfs3_close_device(fd);
    return 0;

fail:
    osfs3_free_block(block);
    osfs3_free_block(inode_bitmap);
    osfs3_free_block(block_bitmap);
    osfs3_close_device(fd);
    return 1;
}
