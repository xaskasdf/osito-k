/*
 * ositofs-rename -- Rename a file on an OsitoFS v2 partition
 *
 * Usage:
 *   ositofs-rename <device> <old-name> <new-name>
 *
 * Metadata-only operation: updates the file entry's name field and
 * sets modify_time to current time.  No data blocks are touched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "common.h"

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-rename <device> <old-name> <new-name>\n");
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc != 4)
        usage();

    const char *device   = argv[1];
    const char *old_name = argv[2];
    const char *new_name = argv[3];

    /* Validate new name length */
    if (strlen(new_name) >= OSFS2_NAME_LEN) {
        fprintf(stderr, "ositofs-rename: new name too long (max %d chars)\n",
                OSFS2_NAME_LEN - 1);
        return 1;
    }

    int fd = osfs2_open_device(device, 0 /* read-write */);
    if (fd < 0) {
        fprintf(stderr, "ositofs-rename: cannot open %s\n", device);
        return 1;
    }

    /* Read and validate superblock */
    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        fprintf(stderr, "ositofs-rename: invalid superblock\n");
        osfs2_close_device(fd);
        return 1;
    }

    uint32_t max_files = osfs2_layout_max_files(&sb);
    uint32_t filetab_size = osfs2_layout_filetab_size(&sb);

    /* Read file table */
    void *ft_buf = osfs2_alloc_aligned(filetab_size);
    if (!ft_buf) {
        fprintf(stderr, "ositofs-rename: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }

    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, filetab_size) < 0) {
        fprintf(stderr, "ositofs-rename: failed to read file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Scan for old name (exact match among valid entries) */
    int found_idx = -1;
    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (strcmp(ft[i].name, old_name) == 0) {
            found_idx = (int)i;
            break;
        }
    }

    if (found_idx < 0) {
        fprintf(stderr, "ositofs-rename: file '%s' not found\n", old_name);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Check new name doesn't already exist */
    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (strcmp(ft[i].name, new_name) == 0) {
            fprintf(stderr, "ositofs-rename: file '%s' already exists\n", new_name);
            free(ft_buf);
            osfs2_close_device(fd);
            return 1;
        }
    }

    /* Rename: copy new name into file entry */
    osfs2_file_t *f = &ft[found_idx];
    memset(f->name, 0, OSFS2_NAME_LEN);
    strncpy(f->name, new_name, OSFS2_NAME_LEN - 1);

    /* Update modify_time */
    f->modify_time = (uint32_t)time(NULL);

    /* Write back file table */
    if (osfs2_write_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, filetab_size) < 0) {
        fprintf(stderr, "ositofs-rename: failed to write file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    printf("Renamed '%s' -> '%s'\n", old_name, new_name);

    free(ft_buf);
    osfs2_close_device(fd);
    return 0;
}
