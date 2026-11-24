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

static const char *display_name(const osfs2_file_t *f)
{
    if (!(f->flags & OSFS2_FLAG_GGUF) &&
        !(f->flags & OSFS2_FLAG_INLINE) &&
        f->model_name[0])
        return f->model_name;
    return f->name;
}

static int entry_matches(const osfs2_file_t *f, const char *pattern)
{
    if (wildcard_match(pattern, f->name))
        return 1;
    const char *alias = display_name(f);
    return alias != f->name && wildcard_match(pattern, alias);
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

static void recompute_super(osfs2_super_t *sb, const osfs2_file_t *ft)
{
    uint32_t data_start = osfs2_layout_data_start_blk(sb);
    uint32_t files = 0;
    uint32_t used = data_start;
    uint32_t hwm = data_start;
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        files++;
        if (!(ft[i].flags & OSFS2_FLAG_INLINE) && ft[i].block_count) {
            used += ft[i].block_count;
            uint32_t end = ft[i].start_block + ft[i].block_count;
            if (end > hwm) hwm = end;
        }
    }
    sb->file_count = files;
    sb->used_blocks = used;
    sb->next_data_block = hwm;
    sb->crc32 = 0;
    sb->crc32 = osfs2_crc32(sb, sizeof(*sb));
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
    if (!dry_run && osfs2_journal_recover(fd, 1) < 0) {
        fprintf(stderr, "ositofs-delete: journal recovery failed\n");
        osfs2_close_device(fd);
        return 1;
    }

    /* Read and validate superblock */
    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        fprintf(stderr, "ositofs-delete: invalid superblock\n");
        osfs2_close_device(fd);
        return 1;
    }

    uint32_t max_files = osfs2_layout_max_files(&sb);
    uint32_t filetab_size = osfs2_layout_filetab_size(&sb);
    uint32_t layeridx_off = osfs2_layout_layeridx_off(&sb);

    /* Read file table */
    void *ft_buf = osfs2_alloc_aligned(filetab_size);
    if (!ft_buf) {
        fprintf(stderr, "ositofs-delete: out of memory\n");
        osfs2_close_device(fd);
        return 1;
    }

    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_buf, filetab_size) < 0) {
        fprintf(stderr, "ositofs-delete: failed to read file table\n");
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_buf;

    /* Scan file table for all matches */
    int *matches = (int *)malloc(max_files * sizeof(int));
    if (!matches) {
        fprintf(stderr, "ositofs-delete: out of memory\n");
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }
    int match_count = 0;

    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (wildcard_match(pattern, osfs2_entry_name(&ft[i])))
            matches[match_count++] = i;
    }

    if (match_count == 0) {
        fprintf(stderr, "ositofs-delete: no files matching '%s'\n", pattern);
        free(matches);
        osfs2_free_block(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Show matches */
    printf("Matched %d file%s:\n", match_count, match_count == 1 ? "" : "s");
    for (int m = 0; m < match_count; m++) {
        osfs2_file_t *f = &ft[matches[m]];
        printf("  %-48s ", osfs2_entry_name(f));
        osfs2_print_size(f->size);
        printf("  (blocks %u-%u)\n", f->start_block,
               f->start_block + f->block_count - 1);
    }

    /* Dry-run: just print and exit */
    if (dry_run) {
        printf("\n[dry-run] Would delete %d file%s\n",
               match_count, match_count == 1 ? "" : "s");
        free(matches);
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
        if (osfs2_layer_index_enabled && (f->flags & OSFS2_FLAG_GGUF) &&
            f->layer_index_slot != 0xFFFF) {
            li_blk = osfs2_alloc_aligned(OSFS2_LAYERIDX_SIZE);
            if (li_blk) {
                if (osfs2_read_bytes(fd, layeridx_off, li_blk,
                                     OSFS2_LAYERIDX_SIZE) < 0) {
                    free(li_blk);
                    li_blk = NULL;
                }
            }
            break;
        }
    }

    /* Delete each matched file as its own recoverable transaction. */
    uint32_t total_freed = 0;
    int deleted = 0;
    int journal_failed = 0;

    for (int m = 0; m < match_count; m++) {
        uint32_t slot = (uint32_t)matches[m];
        osfs2_file_t *f = &ft[slot];

        printf("Deleting '%s'...\n", osfs2_entry_name(f));

        osfs2_file_t before[OSFS2_JOURNAL_MAX_ENTRIES] = { *f };
        osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES] = {0};
        uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES] = { slot, 0 };
        osfs2_super_t before_sb = sb;
        osfs2_file_t saved = *f;
        memset(f, 0, sizeof(*f));
        osfs2_super_t after_sb = sb;
        recompute_super(&after_sb, ft);
        if (osfs2_journal_commit_entries(fd, OSFS2_JOURNAL_OP_DELETE,
                &before_sb, &after_sb, slots, before, after, 1) < 0) {
            *f = saved;
            fprintf(stderr, "ositofs-delete: journaled delete failed\n");
            journal_failed = 1;
            break;
        }
        sb = after_sb;

        /* Clean up layer index if GGUF file */
        if (li_blk && (saved.flags & OSFS2_FLAG_GGUF) &&
            saved.layer_index_slot < OSFS2_MAX_MODELS) {
            osfs2_layer_idx_t *li = (osfs2_layer_idx_t *)li_blk;
            memset(&li[saved.layer_index_slot], 0, sizeof(osfs2_layer_idx_t));
            li_dirty = 1;
        }

        total_freed += saved.block_count;
        deleted++;
    }

    /* Flush layer index if modified */
    if (li_blk && li_dirty) {
        if (osfs2_write_bytes(fd, layeridx_off, li_blk,
                              OSFS2_LAYERIDX_SIZE) < 0 || osfs2_sync(fd) < 0)
            journal_failed = 1;
    }
    free(li_blk);

    if (journal_failed) {
        free(ft_buf);
        osfs2_close_device(fd);
        return 1;
    }

    /* Summary */
    printf("\n[OK] Deleted %d file%s, freed %u block%s\n",
           deleted, deleted == 1 ? "" : "s",
           total_freed, total_freed == 1 ? "" : "s");

    free(matches);
    free(ft_buf);
    osfs2_close_device(fd);
    return 0;
}
