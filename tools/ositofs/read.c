/*
 * ositofs-read — Extract files from OsitoFS v2 partition
 *
 * Usage:
 *   ositofs-read <device> <pattern> [--output-dir <dir>]
 *   ositofs-read <device> <filename> [output-path]
 *
 * Supports wildcard patterns:
 *   *          — extract all files
 *   *.ext      — match by extension (case-insensitive)
 *   prefix*    — match by prefix
 *   exactname  — exact match (single file)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "common.h"

static int wildcard_match(const char *pattern, const char *name);

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

static void fprint_size(FILE *fp, uint64_t bytes)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024)
        fprintf(fp, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024)
        fprintf(fp, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        fprintf(fp, "%.1f KB", (double)bytes / 1024.0);
    else
        fprintf(fp, "%llu B", (unsigned long long)bytes);
}

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

/* Returns 1 if pattern contains wildcard characters */
static int is_wildcard(const char *pattern)
{
    return strchr(pattern, '*') != NULL;
}

/* ── Extract a single file to output path ────────────────────────────── */

static int extract_file(int fd, osfs2_file_t *f, const char *output)
{
    uint64_t file_size = f->size;
    uint32_t start_block = f->start_block;

    int out_fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("open output");
        return -1;
    }

    if (f->flags & OSFS2_FLAG_INLINE) {
        ssize_t written = write(out_fd, f->model_name, (size_t)file_size);
        close(out_fd);
        return written == (ssize_t)file_size ? 0 : -1;
    }

    void *data_blk = osfs2_alloc_block();
    if (!data_blk) {
        close(out_fd);
        return -1;
    }

    uint64_t remaining = file_size;
    uint32_t blk = start_block;
    int ok = 1;

    while (remaining > 0) {
        if (osfs2_read_block(fd, blk, data_blk) < 0) {
            fprintf(stderr, "ositofs-read: failed reading block %u\n", blk);
            ok = 0;
            break;
        }

        size_t to_write = remaining > osfs2_block_sz
                        ? osfs2_block_sz : (size_t)remaining;
        ssize_t n = write(out_fd, data_blk, to_write);
        if (n != (ssize_t)to_write) {
            perror("write output");
            ok = 0;
            break;
        }

        remaining -= to_write;
        blk++;
    }

    close(out_fd);
    osfs2_free_block(data_blk);
    return ok ? 0 : -1;
}

static int normalize_tree_path(const char *stored, char *out, size_t capacity)
{
    size_t write_pos = 0;
    const char *p = stored;
    while (*p == '/' || *p == '\\') p++;
    while (*p) {
        const char *component = p;
        size_t length = 0;
        while (*p && *p != '/' && *p != '\\') {
            p++;
            length++;
        }
        while (*p == '/' || *p == '\\') p++;
        if (!length || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.')
            return -1;
        if (write_pos + length + (write_pos != 0) >= capacity) return -1;
        if (write_pos) out[write_pos++] = '/';
        memcpy(out + write_pos, component, length);
        write_pos += length;
    }
    out[write_pos] = '\0';
    return write_pos ? 0 : -1;
}

static int mkdir_parents(const char *path)
{
    char copy[2048];
    size_t length = strlen(path);
    if (length >= sizeof(copy)) return -1;
    memcpy(copy, path, length + 1);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: ositofs-read <device> <pattern> [--output-dir <dir>]\n"
            "       ositofs-read <device> '*' --output-dir <dir> --tree\n"
            "       ositofs-read <device> <filename> [output-path]\n"
            "\n"
            "Patterns: *  *.ext  prefix*  exact-name\n");
        return 1;
    }

    const char *device     = argv[1];
    const char *pattern    = argv[2];
    const char *output_dir = NULL;
    const char *output_path = NULL;
    int tree_mode = 0;

    /* Parse optional arguments */
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "--tree") == 0) {
            tree_mode = 1;
        } else if (!output_path && i == 3 && !is_wildcard(pattern)) {
            /* Legacy: ositofs-read dev file.bin /tmp/out.bin */
            output_path = argv[i];
        } else {
            fprintf(stderr, "ositofs-read: unknown argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (tree_mode && !output_dir) output_dir = ".";
    if (output_dir && mkdir(output_dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir output directory");
        return 1;
    }

    int fd = osfs2_open_device(device, 1);
    if (fd < 0) return 1;

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        osfs2_close_device(fd);
        return 1;
    }

    uint32_t max_files = osfs2_layout_max_files(&sb);
    uint32_t filetab_size = osfs2_layout_filetab_size(&sb);

    /* Read file table */
    void *ft_blk = osfs2_alloc_aligned(filetab_size);
    if (!ft_blk) { osfs2_close_device(fd); return 1; }
    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_blk, filetab_size) < 0) {
        free(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    osfs2_file_t *ft = (osfs2_file_t *)ft_blk;

    /* Scan file table for all matches */
    int *matches = (int *)malloc(max_files * sizeof(int));
    if (!matches) {
        free(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }
    int match_count = 0;

    for (uint32_t i = 0; i < max_files; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        if (wildcard_match(pattern, osfs2_entry_name(&ft[i])))
            matches[match_count++] = (int)i;
    }

    if (match_count == 0) {
        fprintf(stderr, "ositofs-read: no files matching '%s'\n", pattern);
        free(matches);
        free(ft_blk);
        osfs2_close_device(fd);
        return 1;
    }

    /* Single file with explicit output path (legacy mode) */
    if (match_count == 1 && output_path) {
        osfs2_file_t *f = &ft[matches[0]];
        fprintf(stderr, "Extracting '%s' (", osfs2_entry_name(f));
        fprint_size(stderr, f->size);
        fprintf(stderr, ")...\n");

        int rc = extract_file(fd, f, output_path);
        if (rc == 0) {
            fprintf(stderr, "OK: %s -> %s (", osfs2_entry_name(f), output_path);
            fprint_size(stderr, f->size);
            fprintf(stderr, ")\n");
        }

        free(matches);
        free(ft_blk);
        osfs2_close_device(fd);
        return rc;
    }

    /* Batch extraction */
    int extracted = 0;
    int errors = 0;
    uint64_t total_bytes = 0;

    for (int m = 0; m < match_count; m++) {
        osfs2_file_t *f = &ft[matches[m]];

        /* Build output path */
        char out_path[1024];
        if (output_dir) {
            char relative[1024];
            const char *stored = osfs2_entry_name(f);
            if (tree_mode) {
                if (normalize_tree_path(stored, relative, sizeof(relative)) < 0) {
                    fprintf(stderr, "unsafe or long stored path: %s\n", stored);
                    errors++;
                    continue;
                }
                stored = relative;
            }
            snprintf(out_path, sizeof(out_path), "%s/%s", output_dir, stored);
        } else {
            snprintf(out_path, sizeof(out_path), "%s", osfs2_entry_name(f));
        }

        if (mkdir_parents(out_path) < 0) {
            fprintf(stderr, "cannot create parent path for %s\n", out_path);
            errors++;
            continue;
        }

        fprintf(stderr, "[%d/%d] Extracting '%s' (",
                m + 1, match_count, osfs2_entry_name(f));
        fprint_size(stderr, f->size);
        fprintf(stderr, ")...\n");

        int rc = extract_file(fd, f, out_path);
        if (rc == 0) {
            extracted++;
            total_bytes += f->size;
        } else {
            errors++;
            fprintf(stderr, "  FAILED: %s\n", osfs2_entry_name(f));
        }
    }

    /* Summary */
    fprintf(stderr, "\nExtracted %d file%s (",
            extracted, extracted == 1 ? "" : "s");
    fprint_size(stderr, total_bytes);
    fprintf(stderr, ")");
    if (errors > 0)
        fprintf(stderr, ", %d error%s", errors, errors == 1 ? "" : "s");
    fprintf(stderr, "\n");

    free(matches);
    free(ft_blk);
    osfs2_close_device(fd);
    return errors > 0 ? 1 : 0;
}
