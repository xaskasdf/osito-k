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
    if (strlen(new_name) >= OSFS2_MODEL_NAME_LEN) {
        fprintf(stderr, "ositofs-rename: new name too long (max %d chars)\n",
                OSFS2_MODEL_NAME_LEN - 1);
        return 1;
    }

    int fd = osfs2_open_device(device, 0 /* read-write */);
    if (fd < 0) {
        fprintf(stderr, "ositofs-rename: cannot open %s\n", device);
        return 1;
    }
    if (osfs2_journal_recover(fd, 1) < 0) {
        fprintf(stderr, "ositofs-rename: journal recovery failed\n");
        osfs2_close_device(fd);
        return 1;
    }

    /* Read and validate superblock */
    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        fprintf(stderr, "ositofs-rename: invalid superblock\n");
        osfs2_close_device(fd);
        return 1;
    }

    /* Read file table (1MB at fixed offset) */
    void *ft_buf = osfs2_alloc_aligned(OSFS2_FILETAB_SIZE);
    if (!ft_buf) {
        fprintf(stderr, "ositofs-rename: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }

    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, OSFS2_FILETAB_SIZE) < 0) {
        fprintf(stderr, "ositofs-rename: failed to read file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Scan for old name (exact match among valid entries) */
    int found_idx = -1;
    for (int i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (strcmp(osfs2_entry_name(&ft[i]), old_name) == 0) {
            found_idx = i;
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
    for (int i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (strcmp(osfs2_entry_name(&ft[i]), new_name) == 0) {
            fprintf(stderr, "ositofs-rename: file '%s' already exists\n", new_name);
            free(ft_buf);
            osfs2_close_device(fd);
            return 1;
        }
    }

    osfs2_file_t before[OSFS2_JOURNAL_MAX_ENTRIES] = { ft[found_idx] };
    osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES] = { ft[found_idx] };
    uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES] = { (uint32_t)found_idx, 0 };
    if (osfs2_set_entry_name(&after[0], new_name) < 0) {
        fprintf(stderr, "ositofs-rename: cannot encode new name '%s'\n", new_name);
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Update modify_time */
    after[0].modify_time = (uint32_t)time(NULL);

    if (osfs2_journal_commit_entries(fd, OSFS2_JOURNAL_OP_RENAME,
            &sb, &sb, slots, before, after, 1) < 0) {
        fprintf(stderr, "ositofs-rename: journaled rename failed\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    printf("Renamed '%s' -> '%s'\n", old_name, new_name);

    free(ft_buf);
    osfs2_close_device(fd);
    return 0;
}
