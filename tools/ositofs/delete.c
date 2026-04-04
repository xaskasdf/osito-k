/*
 * ositofs-delete — Delete files from OsitoFS v2 partition
 *
 * Usage:
 *   ositofs-delete <device> <pattern> [--dry-run]
 *
 * Supports wildcard patterns:
 *   *          — delete all files
 *   *.ext      — match by extension (case-insensitive)
 *   prefix*    — match by prefix
 *   exactname  — exact match (single file)
 *
 * Marks matched file entries as invalid and updates superblock counters.
 * Freed blocks can be reused by subsequent writes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "common.h"

/* ── Simple wildcard match (same style as kernel osfs2_wildcard_match) ── */

static int wildcard_match(const char *pattern, const char *name)
{
    int plen = (int)strlen(pattern);
    int nlen = (int)strlen(name);

    /* "*" — match everything */
    if (plen == 1 && pattern[0] == '*')
        return 1;

    /* "*.ext" — match by extension (case-insensitive) */
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *ext = pattern + 1;  /* ".ext" */
        int elen = plen - 1;
        if (nlen < elen) return 0;
        for (int i = 0; i < elen; i++) {
            char a = name[nlen - elen + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
        }
        return 1;
    }

    /* "prefix*" — match by prefix */
    if (plen > 1 && pattern[plen - 1] == '*') {
        int prefix_len = plen - 1;
        if (nlen < prefix_len) return 0;
        return strncmp(pattern, name, prefix_len) == 0;
    }

    /* Exact match fallback */
    return strcmp(pattern, name) == 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-delete <device> <pattern> [--dry-run]\n"
        "\n"
        "Patterns: *  *.ext  prefix*  exact-name\n"
        "Options:  --dry-run  show what would be deleted without deleting\n");
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4)
        usage();

    const char *device  = argv[1];
    const char *pattern = argv[2];
    int dry_run = 0;

    if (argc == 4) {
        if (strcmp(argv[3], "--dry-run") == 0)
            dry_run = 1;
        else
            usage();
    }

    int fd = osfs2_open_device(device, dry_run /* readonly if dry-run */);
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

    /* Read file table (1MB at fixed offset) */
    void *ft_buf = osfs2_alloc_aligned(OSFS2_FILETAB_SIZE);
    if (!ft_buf) {
        fprintf(stderr, "ositofs-delete: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }

    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, OSFS2_FILETAB_SIZE) < 0) {
        fprintf(stderr, "ositofs-delete: failed to read file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Scan file table for all matches */
    int matches[OSFS2_MAX_FILES];
    int match_count = 0;

    for (int i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (wildcard_match(pattern, ft[i].name))
            matches[match_count++] = i;
    }

    if (match_count == 0) {
        fprintf(stderr, "ositofs-delete: no files matching '%s'\n", pattern);
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Show matches */
    printf("Matched %d file%s:\n", match_count, match_count == 1 ? "" : "s");
    for (int m = 0; m < match_count; m++) {
        osfs2_file_t *f = &ft[matches[m]];
        printf("  %-48s ", f->name);
        osfs2_print_size(f->size);
        printf("  (blocks %u-%u)\n", f->start_block,
               f->start_block + f->block_count - 1);
    }

    /* Dry-run: just print and exit */
    if (dry_run) {
        printf("\n[dry-run] Would delete %d file%s\n",
               match_count, match_count == 1 ? "" : "s");
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 0;
    }

    /* Read layer index for GGUF cleanup */
    void *li_blk = NULL;
    int li_dirty = 0;

    /* Check if any match is GGUF before allocating */
    for (int m = 0; m < match_count; m++) {
        osfs2_file_t *f = &ft[matches[m]];
        if ((f->flags & OSFS2_FLAG_GGUF) && f->layer_index_slot != 0xFFFF) {
            li_blk = osfs2_alloc_aligned(OSFS2_LAYERIDX_SIZE);
            if (li_blk) {
                if (osfs2_read_bytes(fd, OSFS2_LAYERIDX_OFF, li_blk,
                                     OSFS2_LAYERIDX_SIZE) < 0) {
                    free(li_blk);
                    li_blk = NULL;
                }
            }
            break;
        }
    }

    /* Delete each matched file */
    uint32_t total_freed = 0;
    int deleted = 0;

    for (int m = 0; m < match_count; m++) {
        osfs2_file_t *f = &ft[matches[m]];

        printf("Deleting '%s'...\n", f->name);

        /* Clean up layer index if GGUF file */
        if (li_blk && (f->flags & OSFS2_FLAG_GGUF) &&
            f->layer_index_slot != 0xFFFF) {
            osfs2_layer_idx_t *li = (osfs2_layer_idx_t *)li_blk;
            memset(&li[f->layer_index_slot], 0, sizeof(osfs2_layer_idx_t));
            li_dirty = 1;
        }

        /* Mark entry as invalid */
        total_freed += f->block_count;
        f->flags = 0;
        f->create_time = 0;
        f->modify_time = 0;
        deleted++;
    }

    /* Update superblock counters */
    if (sb.file_count >= (uint32_t)deleted)
        sb.file_count -= (uint32_t)deleted;
    else
        sb.file_count = 0;
    sb.used_blocks -= total_freed;

    /* Recalculate high-water mark once */
    uint32_t data_start = osfs2_data_start_blk(sb.block_size);
    uint32_t hwm = data_start;
    for (int i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        uint32_t end = ft[i].start_block + ft[i].block_count;
        if (end > hwm) hwm = end;
    }
    sb.next_data_block = hwm;

    /* Recalculate superblock CRC */
    sb.crc32 = 0;
    sb.crc32 = osfs2_crc32(&sb, sizeof(sb));

    /* Flush layer index if modified */
    if (li_blk && li_dirty) {
        osfs2_write_bytes(fd, OSFS2_LAYERIDX_OFF, li_blk, OSFS2_LAYERIDX_SIZE);
    }
    free(li_blk);

    /* Write back file table */
    if (osfs2_write_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, OSFS2_FILETAB_SIZE) < 0) {
        fprintf(stderr, "ositofs-delete: failed to write file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Write back superblock + backup */
    void *sb_buf = osfs2_alloc_aligned(4096);
    if (sb_buf) {
        memcpy(sb_buf, &sb, sizeof(sb));
        osfs2_write_bytes(fd, 0, sb_buf, 4096);
        osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, sb_buf, 4096);
        free(sb_buf);
    }

    /* Summary */
    printf("\n[OK] Deleted %d file%s, freed %u block%s\n",
           deleted, deleted == 1 ? "" : "s",
           total_freed, total_freed == 1 ? "" : "s");

    free(ft_buf);
    osfs2_close_device(fd);
    return 0;
}
