/*
 * ositofs-read — Extract a file from OsitoFS v2 partition
 *
 * Usage: ositofs-read <device> <filename> [output-path]
 *
 * If output-path is omitted, writes to ./<filename>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "common.h"

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: ositofs-read <device> <filename> [output-path]\n");
        return 1;
    }

    const char *device = argv[1];
    const char *filename = argv[2];
    const char *output = argc > 3 ? argv[3] : filename;

    int fd = osfs2_open_device(device, 1);
    if (fd < 0) return 1;

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        osfs2_close_device(fd);
        return 1;
    }

    /* Read file table */
    void *ft_blk = osfs2_alloc_block();
    if (!ft_blk) { osfs2_close_device(fd); return 1; }
    if (osfs2_read_block(fd, OSFS2_FILETAB_BLK, ft_blk) < 0) {
        osfs2_free_block(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_blk;

    /* Find the file */
    int found = -1;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (strcmp(ft[i].name, filename) == 0) {
            found = (int)i;
            break;
        }
    }

    if (found < 0) {
        fprintf(stderr, "ositofs-read: file '%s' not found\n", filename);
        osfs2_free_block(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *f = &ft[found];
    uint64_t file_size = f->size;
    uint32_t start_block = f->start_block;

    fprintf(stderr, "Extracting '%s' (%llu bytes, block %u)...\n",
            filename, (unsigned long long)file_size, start_block);

    /* Open output file */
    int out_fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("open output");
        osfs2_free_block(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    /* Read data blocks and write to output */
    void *data_blk = osfs2_alloc_block();
    if (!data_blk) {
        close(out_fd);
        osfs2_free_block(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    uint64_t remaining = file_size;
    uint32_t blk = start_block;

    while (remaining > 0) {
        if (osfs2_read_block(fd, blk, data_blk) < 0) {
            fprintf(stderr, "ositofs-read: failed reading block %u\n", blk);
            break;
        }

        size_t to_write = remaining > OSFS2_BLOCK_SIZE ? OSFS2_BLOCK_SIZE : (size_t)remaining;
        ssize_t n = write(out_fd, data_blk, to_write);
        if (n != (ssize_t)to_write) {
            perror("write output");
            break;
        }

        remaining -= to_write;
        blk++;
    }

    close(out_fd);
    osfs2_free_block(data_blk);
    osfs2_free_block(ft_blk);
    osfs2_close_device(fd);

    if (remaining == 0) {
        fprintf(stderr, "OK: %s → %s (%llu bytes)\n", filename, output,
                (unsigned long long)file_size);
        return 0;
    }
    return 1;
}
