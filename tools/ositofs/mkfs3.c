/*
 * mkfs.ositofs3 — Format device with OsitoFS v3
 *
 * Usage: mkfs.ositofs3 <device> [--label <name>]
 *
 * Block Layout:
 *   0: Superblock
 *   1: Inode Bitmap
 *   2: Block Bitmap
 *   3: Inode Table
 *   4: Root Directory Data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common_v3.h"

static void usage(void)
{
    fprintf(stderr, "Usage: mkfs.ositofs3 <device> [--label <name>]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *label = "ositok-root";

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

    int fd = osfs3_open_device(device, 0);
    if (fd < 0) return 1;

    uint64_t dev_size = osfs3_device_size(fd);
    uint32_t total_blocks = (uint32_t)(dev_size >> OSFS3_BLOCK_SHIFT);

    /* Metadata requires at least 4 blocks (SB, InoMap, BlkMap, InoTab) + 1 RootData */
    if (total_blocks < 5) {
        fprintf(stderr, "mkfs: device too small\n");
        osfs3_close_device(fd);
        return 1;
    }

    printf("mkfs.ositofs3: formatting %s\n", device);
    printf("  Device size: "); osfs3_print_size(dev_size); printf("\n");
    printf("  Total blocks: %u (1MB each)\n", total_blocks);
    printf("  Label: %s\n", label);

    void *blk = osfs3_alloc_block();
    if (!blk) { osfs3_close_device(fd); return 1; }

    /* ── Block 0: Superblock ─────────────────────────────────── */
    osfs3_super_t *sb = (osfs3_super_t *)blk;
    sb->magic = OSFS3_MAGIC;
    sb->version = OSFS3_VERSION;
    sb->block_size = OSFS3_BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->total_inodes = 4096; /* 1 block worth of inodes */
    sb->free_blocks = total_blocks - 5; 
    sb->free_inodes = sb->total_inodes - 1; /* Inode 1 is root */
    sb->first_data_block = 4;
    sb->root_inode = 1;
    osfs3_gen_uuid(sb->uuid);
    strncpy(sb->label, label, 31);
    sb->create_time = (uint64_t)time(NULL);
    sb->crc32 = 0;
    sb->crc32 = osfs3_crc32(sb, sizeof(*sb));

    if (osfs3_write_block(fd, 0, blk) < 0) goto fail;
    printf("  [OK] Superblock written\n");

    /* ── Block 1: Inode Bitmap ───────────────────────────────── */
    memset(blk, 0, OSFS3_BLOCK_SIZE);
    uint8_t *imap = (uint8_t *)blk;
    imap[0] = 0x03; /* Bit 0 (reserved), Bit 1 (root inode) */
    if (osfs3_write_block(fd, 1, blk) < 0) goto fail;
    printf("  [OK] Inode bitmap written\n");

    /* ── Block 2: Block Bitmap ───────────────────────────────── */
    memset(blk, 0, OSFS3_BLOCK_SIZE);
    uint8_t *bmap = (uint8_t *)blk;
    bmap[0] = 0x1F; /* Bits 0-4 used (SB, Imap, Bmap, InoTab, RootData) */
    if (osfs3_write_block(fd, 2, blk) < 0) goto fail;
    printf("  [OK] Block bitmap written\n");

    /* ── Block 3: Inode Table ────────────────────────────────── */
    memset(blk, 0, OSFS3_BLOCK_SIZE);
    osfs3_inode_t *inodes = (osfs3_inode_t *)blk;
    
    /* Root Inode (index 1) */
    inodes[1].mode = OSFS3_S_IFDIR | 0755;
    inodes[1].nlink = 2;
    inodes[1].size = 0; /* No entries yet, or empty data block */
    inodes[1].atime = inodes[1].mtime = inodes[1].ctime = sb->create_time;
    inodes[1].extent_count = 1;
    inodes[1].extents[0].start_block = 4;
    inodes[1].extents[0].block_count = 1;

    if (osfs3_write_block(fd, 3, blk) < 0) goto fail;
    printf("  [OK] Inode table written\n");

    /* ── Block 4: Root Directory Data ────────────────────────── */
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
    
    inodes[1].size = de->rec_len + 8; /* Just an example size */

    if (osfs3_write_block(fd, 4, blk) < 0) goto fail;
    printf("  [OK] Root directory initialized\n");

    printf("\nFormatting complete.\n");

    osfs3_free_block(blk);
    osfs3_close_device(fd);
    return 0;

fail:
    osfs3_free_block(blk);
    osfs3_close_device(fd);
    return 1;
}
