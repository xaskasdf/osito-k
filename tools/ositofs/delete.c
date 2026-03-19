/*
 * ositofs-delete — Delete a file from OsitoFS v2 partition
 *
 * Usage: ositofs-delete <device> <filename>
 *
 * Marks the file entry as invalid and updates superblock counters.
 * Freed blocks can be reused by subsequent writes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "common.h"

static void usage(void)
{
    fprintf(stderr, "Usage: ositofs-delete <device> <filename>\n");
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        usage();

    const char *device = argv[1];
    const char *name   = argv[2];

    int fd = osfs2_open_device(device, 0 /* read-write */);
    if (fd < 0) {
        fprintf(stderr, "ositofs-delete: cannot open %s\n", device);
        return 1;
    }

    /* Read and validate superblock */
    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        fprintf(stderr, "ositofs-delete: invalid superblock\n");
        osfs2_close_device(fd);
        return 1;
    }

    /* Read file table (block 1) */
    void *ft_buf = osfs2_alloc_block();
    if (!ft_buf) {
        fprintf(stderr, "ositofs-delete: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }

    if (osfs2_read_block(fd, OSFS2_FILETAB_BLK, ft_buf) < 0) {
        fprintf(stderr, "ositofs-delete: failed to read file table\n");
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Find the file by exact name */
    int found = -1;
    for (int i = 0; i < OSFS2_MAX_FILES; i++) {
        if ((ft[i].flags & OSFS2_FLAG_VALID) &&
            strcmp(ft[i].name, name) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        fprintf(stderr, "ositofs-delete: file '%s' not found\n", name);
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *f = &ft[found];
    printf("Deleting '%s' (", name);
    osfs2_print_size(f->size);
    printf(", blocks %u-%u)\n", f->start_block,
           f->start_block + f->block_count - 1);

    /* Mark entry as invalid */
    uint32_t freed = f->block_count;
    f->flags = 0;

    /* Update superblock */
    if (sb.file_count > 0)
        sb.file_count--;
    sb.used_blocks -= freed;

    /* Shrink next_data_block if we freed trailing blocks */
    uint32_t top = f->start_block + f->block_count;
    if (top == sb.next_data_block) {
        /* Scan backwards to find actual high-water mark */
        uint32_t hwm = OSFS2_DATA_START_BLK;
        for (int i = 0; i < OSFS2_MAX_FILES; i++) {
            if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
            uint32_t end = ft[i].start_block + ft[i].block_count;
            if (end > hwm) hwm = end;
        }
        sb.next_data_block = hwm;
    }

    /* Recalculate superblock CRC */
    sb.crc32 = 0;
    sb.crc32 = osfs2_crc32(&sb, sizeof(sb));

    /* Write back file table */
    if (osfs2_write_block(fd, OSFS2_FILETAB_BLK, ft_buf) < 0) {
        fprintf(stderr, "ositofs-delete: failed to write file table\n");
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Write back superblock */
    void *sb_buf = osfs2_alloc_block();
    if (sb_buf) {
        if (osfs2_read_block(fd, OSFS2_SUPERBLOCK_BLK, sb_buf) == 0) {
            memcpy(sb_buf, &sb, sizeof(sb));
            osfs2_write_block(fd, OSFS2_SUPERBLOCK_BLK, sb_buf);
        }
        osfs2_free_block(sb_buf);
    }

    printf("[OK] Deleted '%s'\n", name);

    osfs2_free_block(ft_buf);
    osfs2_close_device(fd);
    return 0;
}
