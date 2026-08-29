/* Format a device or image with OsitoFS v3. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common_v3.h"

static void usage(void)
{
    fprintf(stderr,
            "Usage: mkfs.ositofs3 <device> [--label <name>] "
            "[--inodes <count>]\n");
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
    uint32_t requested_inodes = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            label = argv[++i];
        else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 0);
            if (!end || *end || value < 2U || value > OSFS3_MAX_INODES)
                usage();
            requested_inodes = (uint32_t)value;
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
    uint64_t device_size = osfs3_device_size(fd);
    uint32_t total_blocks = (uint32_t)(device_size >> OSFS3_BLOCK_SHIFT);
    uint32_t total_inodes = requested_inodes
        ? requested_inodes : recommended_inode_count(total_blocks);
    uint32_t first_data = OSFS3_FIRST_DATA_BLOCK(total_inodes);
    uint32_t root_block = first_data;
    if ((device_size >> OSFS3_BLOCK_SHIFT) > OSFS3_BITMAP_BITS ||
        total_inodes < 2U || total_inodes > OSFS3_MAX_INODES ||
        total_blocks <= root_block) {
        fprintf(stderr, "mkfs.ositofs3: unsupported device size\n");
        osfs3_close_device(fd);
        return 1;
    }

    printf("mkfs.ositofs3: %s\n", device);
    printf("  size: ");
    osfs3_print_size(device_size);
    printf("\n  blocks: %u x %u KiB\n", total_blocks,
           OSFS3_BLOCK_SIZE / 1024U);
    printf("  inodes: %u (%u table blocks)\n", total_inodes,
           OSFS3_INODE_TABLE_BLOCKS(total_inodes));

    void *block = osfs3_alloc_block();
    void *inode_bitmap = osfs3_alloc_block();
    void *block_bitmap = osfs3_alloc_block();
    if (!block || !inode_bitmap || !block_bitmap) goto fail;

    osfs3_super_t super;
    memset(&super, 0, sizeof(super));
    super.magic = OSFS3_MAGIC;
    super.version = OSFS3_VERSION;
    super.block_size = OSFS3_BLOCK_SIZE;
    super.total_blocks = total_blocks;
    super.free_blocks = total_blocks - root_block - 1U;
    super.total_inodes = total_inodes;
    super.free_inodes = total_inodes - 2U; /* inode 0 and root are reserved */
    super.first_data_block = first_data;
    super.root_inode = 1;
    osfs3_gen_uuid(super.uuid);
    memcpy(super.label, label, strlen(label) + 1U);
    super.create_time = (uint64_t)time(NULL);
    super.crc32 = osfs3_crc32(&super, sizeof(super));

    memset(block, 0, OSFS3_BLOCK_SIZE);
    memcpy(block, &super, sizeof(super));
    if (osfs3_write_block(fd, OSFS3_SUPERBLOCK_BLK, block) < 0) goto fail;

    bitmap_set(inode_bitmap, 0);
    bitmap_set(inode_bitmap, 1);
    if (osfs3_write_block(fd, OSFS3_INODE_BITMAP_BLK, inode_bitmap) < 0)
        goto fail;

    for (uint32_t used = 0; used <= root_block; used++)
        bitmap_set(block_bitmap, used);
    if (osfs3_write_block(fd, OSFS3_BLOCK_BITMAP_BLK, block_bitmap) < 0)
        goto fail;

    uint32_t inode_blocks = OSFS3_INODE_TABLE_BLOCKS(total_inodes);
    for (uint32_t table_block = 0; table_block < inode_blocks; table_block++) {
        memset(block, 0, OSFS3_BLOCK_SIZE);
        if (table_block == 0) {
            osfs3_inode_t *inodes = (osfs3_inode_t *)block;
            osfs3_inode_t *root = &inodes[1];
            root->mode = OSFS3_S_IFDIR | 0755;
            root->nlink = 2;
            root->size = OSFS3_BLOCK_SIZE;
            root->atime = root->mtime = root->ctime = super.create_time;
            root->extent_count = 1;
            root->extents[0].start_block = root_block;
            root->extents[0].block_count = 1;
        }
        if (osfs3_write_block(fd, OSFS3_INODE_TABLE_BLK + table_block,
                              block) < 0)
            goto fail;
    }

    memset(block, 0, OSFS3_BLOCK_SIZE);
    osfs3_dentry_t *dot = (osfs3_dentry_t *)block;
    dot->inode = 1;
    dot->rec_len = (uint16_t)OSFS3_DIR_REC_LEN(1);
    dot->name_len = 1;
    dot->type = OSFS3_DT_DIR;
    dot->name[0] = '.';
    osfs3_dentry_t *dotdot =
        (osfs3_dentry_t *)((uint8_t *)block + dot->rec_len);
    dotdot->inode = 1;
    dotdot->rec_len = (uint16_t)(OSFS3_DIR_BLOCK_BYTES - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->type = OSFS3_DT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    if (osfs3_write_block(fd, root_block, block) < 0 || fsync(fd) < 0)
        goto fail;

    printf("  first data block: %u\n", first_data);
    printf("  label: %s\n", label);
    printf("Formatting complete.\n");
    osfs3_free_block(block);
    osfs3_free_block(inode_bitmap);
    osfs3_free_block(block_bitmap);
    osfs3_close_device(fd);
    return 0;

fail:
    osfs3_free_block(block);
    osfs3_free_block(inode_bitmap);
    osfs3_free_block(block_bitmap);
    osfs3_close_device(fd);
    return 1;
}
