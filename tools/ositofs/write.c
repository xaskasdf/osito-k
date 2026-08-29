/*
 * ositofs-write — Write files to OsitoFS v2 partition
 *
 * Usage: ositofs-write <device> <file1> [file2 ...] [--name <name>] [--overwrite]
 *                                                    [--from-list <manifest>]
 *
 * For GGUF files: auto-detects format, parses metadata, generates layer index.
 * Files are write-once, contiguous, first-fit allocation.
 *
 * Batch mode: multiple files are written in a single metadata flush.
 * --from-list reads a manifest (one file per line, optional stored name).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <time.h>

#include "common.h"
#include "gguf.h"

#define MAX_BATCH_FILES 4096

/*
 * Live-extent bitmap (one bit per block). Built from the in-memory file
 * table so the allocator never overlaps a live file — exactly the guard the
 * kernel driver uses. Needed because superblock.next_data_block can be stale
 * (observed on nvme_gcc.img: next_data_block=4 while 2034 blocks were live),
 * and appending blindly there clobbered a live extent.
 */
static uint8_t *g_blkmap;          /* malloc'd, total_blocks bits */
static uint32_t g_total_blocks;

static inline void blkmap_set(uint32_t b)   { if (b < g_total_blocks) g_blkmap[b>>3] |=  (uint8_t)(1u << (b & 7)); }
static inline void blkmap_clear(uint32_t b) { if (b < g_total_blocks) g_blkmap[b>>3] &= (uint8_t)~(1u << (b & 7)); }
static inline int  blkmap_test(uint32_t b)  { return (b >= g_total_blocks) ? 1 : ((g_blkmap[b>>3] >> (b & 7)) & 1); }

/* 1 iff [start,start+count) is fully in-bounds and every block is free. */
static int blkmap_range_free(uint32_t start, uint32_t count, uint32_t data_start)
{
    if (count == 0) return 0;
    if (start < data_start) return 0;
    if ((uint64_t)start + count > (uint64_t)g_total_blocks) return 0;
    for (uint32_t b = start; b < start + count; b++)
        if (blkmap_test(b)) return 0;
    return 1;
}

/* First-fit contiguous free run of `count` blocks. Returns start or 0 (none). */
static uint32_t blkmap_find_free(uint32_t count, uint32_t data_start)
{
    uint32_t run_start = 0, run_len = 0;
    for (uint32_t b = data_start; b < g_total_blocks; b++) {
        if (!blkmap_test(b)) {
            if (run_len == 0) run_start = b;
            if (++run_len == count) return run_start;
        } else run_len = 0;
    }
    return 0;
}

/* Rebuild g_blkmap from the file table: mark metadata + every live extent. */
static int blkmap_build(const osfs2_file_t *ft, uint32_t total_blocks, uint32_t data_start)
{
    g_total_blocks = total_blocks;
    g_blkmap = calloc((total_blocks + 7) / 8, 1);
    if (!g_blkmap) return -1;
    for (uint32_t b = 0; b < data_start; b++) blkmap_set(b);
    for (uint32_t i = 0; i < OSFS2_MAX_FILES; i++) {
        if (!(ft[i].flags & OSFS2_FLAG_VALID)) continue;
        for (uint32_t b = 0; b < ft[i].block_count; b++)
            blkmap_set(ft[i].start_block + b);
    }
    return 0;
}

static void recompute_super(osfs2_super_t *sb, const osfs2_file_t *ft)
{
    uint32_t data_start = osfs2_format_data_start_blk(sb->version,
                                                       sb->block_size);
    uint32_t files = 0, used = data_start, hwm = data_start;
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

typedef struct {
    const char *local_path;
    char stored_name[OSFS2_MODEL_NAME_LEN];
    uint64_t file_size;
    int is_gguf;
    gguf_model_info_t model_info;
} write_job_t;

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-write <device> <file1> [file2 ...] [options]\n"
        "\n"
        "Options:\n"
        "  --name <name>          Override stored filename (single file only)\n"
        "  --overwrite            Replace existing files with same name\n"
        "  --from-list <manifest> Read additional files from manifest\n"
        "\n"
        "Manifest format (one file per line):\n"
        "  <local_path> [stored_name]\n"
        "  # comments and empty lines are skipped\n");
    exit(1);
}

/* Check if file is GGUF by reading magic */
static int is_gguf_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, fp) != 1) { fclose(fp); return 0; }
    fclose(fp);
    return magic == GGUF_MAGIC;
}

/*
 * Parse a manifest file. Each non-empty, non-comment line:
 *   <local_path> [stored_name]
 * Returns 0 on success, -1 on error.
 */
static int parse_manifest(const char *path, write_job_t *jobs, int *count, int max)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "ositofs-write: cannot open manifest %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        /* Strip trailing newline/carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#') continue;

        /* Split into local_path and optional stored_name */
        char *local = line;
        char *stored = NULL;

        /* Skip leading whitespace */
        while (*local == ' ' || *local == '\t') local++;
        if (*local == '\0' || *local == '#') continue;

        /* Find separator (space or tab) between path and stored name */
        char *sep = local;
        /* Handle quoted paths */
        if (*sep == '"') {
            sep++;
            while (*sep && *sep != '"') sep++;
            if (*sep == '"') sep++;
            stored = sep;
            while (*stored == ' ' || *stored == '\t') stored++;
            if (*stored == '\0') stored = NULL;
            /* Remove quotes from local path */
            local++;
            char *qend = strchr(local, '"');
            if (qend) *qend = '\0';
        } else {
            while (*sep && *sep != ' ' && *sep != '\t') sep++;
            if (*sep) {
                *sep = '\0';
                stored = sep + 1;
                while (*stored == ' ' || *stored == '\t') stored++;
                if (*stored == '\0') stored = NULL;
            }
        }

        if (*count >= max) {
            fprintf(stderr, "ositofs-write: manifest %s:%d: too many files (max %d)\n",
                    path, lineno, max);
            fclose(fp);
            return -1;
        }

        /* Stat the file */
        struct stat st;
        if (stat(local, &st) < 0) {
            fprintf(stderr, "ositofs-write: manifest %s:%d: cannot stat '%s': %s\n",
                    path, lineno, local, strerror(errno));
            fclose(fp);
            return -1;
        }

        write_job_t *j = &jobs[*count];
        j->local_path = strdup(local);
        j->file_size = (uint64_t)st.st_size;

        if (stored && *stored) {
            /* Strip trailing whitespace from stored name */
            size_t slen = strlen(stored);
            while (slen > 0 && (stored[slen - 1] == ' ' || stored[slen - 1] == '\t'))
                stored[--slen] = '\0';
            if (strlen(stored) >= sizeof(j->stored_name)) {
                fprintf(stderr,
                        "ositofs-write: manifest %s:%d: stored name too long (max %d chars)\n",
                        path, lineno, OSFS2_MODEL_NAME_LEN - 1);
                fclose(fp);
                return -1;
            }
            strcpy(j->stored_name, stored);
        } else {
            char *tmp = strdup(local);
            const char *base = basename(tmp);
            if (strlen(base) >= sizeof(j->stored_name)) {
                fprintf(stderr,
                        "ositofs-write: manifest %s:%d: basename too long (max %d chars)\n",
                        path, lineno, OSFS2_MODEL_NAME_LEN - 1);
                free(tmp);
                fclose(fp);
                return -1;
            }
            strcpy(j->stored_name, base);
            free(tmp);
        }

        /* Check GGUF */
        j->is_gguf = is_gguf_file(j->local_path);
        memset(&j->model_info, 0, sizeof(j->model_info));
        if (j->is_gguf) {
            if (gguf_parse(j->local_path, &j->model_info) < 0) {
                fprintf(stderr, "ositofs-write: GGUF parse failed for '%s', writing as raw\n",
                        j->local_path);
                j->is_gguf = 0;
            }
        }

        (*count)++;
    }

    fclose(fp);
    return 0;
}

/*
 * Write data blocks for a single file job.
 * Updates crc_table in-memory. Returns file CRC on success, sets *out_crc.
 * Returns 0 on success, -1 on error.
 */
static int write_data_blocks(int fd, const write_job_t *job, uint32_t start_block,
                             uint32_t blocks_needed, uint32_t bs,
                             uint32_t *crc_table, uint32_t *out_crc,
                             int job_idx, int total_jobs)
{
    FILE *src = fopen(job->local_path, "rb");
    if (!src) {
        fprintf(stderr, "ositofs-write: cannot open %s: %s\n",
                job->local_path, strerror(errno));
        return -1;
    }

    void *data_blk = osfs2_alloc_block();
    if (!data_blk) { fclose(src); return -1; }

    uint32_t file_crc = 0xFFFFFFFF;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        memset(data_blk, 0, bs);
        size_t to_read = bs;
        uint64_t remaining = job->file_size - (uint64_t)b * bs;
        if (remaining < to_read) to_read = (size_t)remaining;

        if (fread(data_blk, 1, to_read, src) != to_read) {
            fprintf(stderr, "ositofs-write: read error at block %u of '%s'\n",
                    b, job->stored_name);
            osfs2_free_block(data_blk); fclose(src);
            return -1;
        }

        /* Streaming file CRC across all blocks (same poly as osfs2_crc32) */
        const uint8_t *p = (const uint8_t *)data_blk;
        for (size_t i = 0; i < to_read; i++) {
            file_crc ^= p[i];
            for (int j = 0; j < 8; j++)
                file_crc = (file_crc >> 1) ^ (OSFS2_CRC32_POLY & (-(file_crc & 1)));
        }

        /* Block CRC */
        uint32_t blk_idx = start_block + b;
        if (crc_table && blk_idx < OSFS2_MAX_BLOCKS)
            crc_table[blk_idx] = osfs2_crc32(data_blk, bs);

        if (osfs2_write_block(fd, start_block + b, data_blk) < 0) {
            osfs2_free_block(data_blk); fclose(src);
            return -1;
        }

        if ((b + 1) % 100 == 0 || b + 1 == blocks_needed) {
            if (total_jobs > 1)
                printf("\r  [%d/%d] Block %u/%u", job_idx + 1, total_jobs, b + 1, blocks_needed);
            else
                printf("\r  Block %u/%u", b + 1, blocks_needed);
        }
    }
    printf("\n");

    *out_crc = ~file_crc;

    osfs2_free_block(data_blk);
    fclose(src);
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *name_override = NULL;
    const char *manifest_path = NULL;
    int overwrite = 0;

    /* Collect positional file args */
    const char *pos_files[MAX_BATCH_FILES];
    int pos_count = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name_override = argv[++i];
        } else if (strcmp(argv[i], "--overwrite") == 0) {
            overwrite = 1;
        } else if (strcmp(argv[i], "--from-list") == 0 && i + 1 < argc) {
            manifest_path = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!device) {
                device = argv[i];
            } else {
                if (pos_count >= MAX_BATCH_FILES) {
                    fprintf(stderr, "ositofs-write: too many files (max %d)\n", MAX_BATCH_FILES);
                    return 1;
                }
                pos_files[pos_count++] = argv[i];
            }
        } else {
            usage();
        }
    }

    if (!device || (pos_count == 0 && !manifest_path)) usage();

    /* --name only valid with exactly 1 file */
    if (name_override && (pos_count != 1 || manifest_path)) {
        fprintf(stderr, "ositofs-write: --name can only be used with exactly one file\n");
        return 1;
    }

    /* ── Build job array ────────────────────────────────────────── */
    write_job_t *jobs = calloc(MAX_BATCH_FILES, sizeof(write_job_t));
    if (!jobs) {
        fprintf(stderr, "ositofs-write: out of memory\n");
        return 1;
    }
    int job_count = 0;

    /* Add positional files */
    for (int i = 0; i < pos_count; i++) {
        struct stat st;
        if (stat(pos_files[i], &st) < 0) {
            fprintf(stderr, "ositofs-write: cannot stat %s: %s\n",
                    pos_files[i], strerror(errno));
            free(jobs);
            return 1;
        }

        write_job_t *j = &jobs[job_count];
        j->local_path = pos_files[i];
        j->file_size = (uint64_t)st.st_size;

        if (name_override) {
            if (strlen(name_override) >= sizeof(j->stored_name)) {
                fprintf(stderr, "ositofs-write: stored name too long (max %d chars)\n",
                        OSFS2_MODEL_NAME_LEN - 1);
                free(jobs);
                return 1;
            }
            strcpy(j->stored_name, name_override);
        } else {
            char *tmp = strdup(pos_files[i]);
            const char *base = basename(tmp);
            if (strlen(base) >= sizeof(j->stored_name)) {
                fprintf(stderr, "ositofs-write: basename too long (max %d chars)\n",
                        OSFS2_MODEL_NAME_LEN - 1);
                free(tmp);
                free(jobs);
                return 1;
            }
            strcpy(j->stored_name, base);
            free(tmp);
        }

        /* Check GGUF */
        j->is_gguf = is_gguf_file(j->local_path);
        memset(&j->model_info, 0, sizeof(j->model_info));
        if (j->is_gguf) {
            printf("  Detected GGUF format for '%s', parsing metadata...\n", j->local_path);
            if (gguf_parse(j->local_path, &j->model_info) < 0) {
                fprintf(stderr, "ositofs-write: GGUF parse failed for '%s', writing as raw\n",
                        j->local_path);
                j->is_gguf = 0;
            }
        }

        job_count++;
    }

    /* Add files from manifest */
    if (manifest_path) {
        if (parse_manifest(manifest_path, jobs, &job_count, MAX_BATCH_FILES) < 0) {
            free(jobs);
            return 1;
        }
    }

    if (job_count == 0) {
        fprintf(stderr, "ositofs-write: no files to write\n");
        free(jobs);
        return 1;
    }

    /* Print job summary */
    if (job_count > 1)
        printf("ositofs-write: batch writing %d files\n", job_count);

    for (int i = 0; i < job_count; i++) {
        printf("ositofs-write: %s -> %s (", jobs[i].local_path, jobs[i].stored_name);
        osfs2_print_size(jobs[i].file_size);
        printf(")\n");
    }

    /* ── Open device, read superblock ───────────────────────────── */
    int fd = osfs2_open_device(device, 0);
    if (fd < 0) { free(jobs); return 1; }
    if (osfs2_journal_recover(fd, 1) < 0) {
        fprintf(stderr, "ositofs-write: journal recovery failed\n");
        osfs2_close_device(fd);
        free(jobs);
        return 1;
    }

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        osfs2_close_device(fd);
        free(jobs);
        return 1;
    }

    uint32_t bs = sb.block_size;
    uint32_t shift = osfs2_block_shift(bs);

    /* ── Read file table and CRC table (once) ───────────────────── */
    void *ft_blk = osfs2_alloc_aligned(OSFS2_FILETAB_SIZE);
    if (!ft_blk) { osfs2_close_device(fd); free(jobs); return 1; }
    if (osfs2_read_bytes(fd, OSFS2_FILETAB_OFF, ft_blk, OSFS2_FILETAB_SIZE) < 0)
        goto fail;

    osfs2_file_t *ft = (osfs2_file_t *)ft_blk;

    /* Build the live-extent bitmap so allocation never overlaps a live file,
     * even if superblock.next_data_block is stale. */
    if (blkmap_build(ft, sb.total_blocks,
                     osfs2_format_data_start_blk(sb.version, bs)) < 0) {
        fprintf(stderr, "ositofs-write: out of memory for block bitmap\n");
        goto fail;
    }

    void *crc_blk = osfs2_alloc_aligned(OSFS2_CRCTAB_SIZE);
    if (!crc_blk) goto fail;
    if (osfs2_crc_table_enabled &&
        osfs2_read_bytes(fd, OSFS2_CRCTAB_OFF, crc_blk,
                         OSFS2_CRCTAB_SIZE) < 0)
        goto fail_crc;
    if (!osfs2_crc_table_enabled)
        memset(crc_blk, 0, OSFS2_CRCTAB_SIZE);

    uint32_t *crc_table = (uint32_t *)crc_blk;

    /* Read layer index (once, for GGUF files) */
    int any_gguf = 0;
    for (int i = 0; i < job_count; i++)
        if (jobs[i].is_gguf) { any_gguf = 1; break; }
    if (any_gguf && !osfs2_layer_index_enabled) {
        fprintf(stderr, "ositofs-write: GGUF files require the standard layout\n");
        goto fail_crc;
    }

    void *li_blk = NULL;
    osfs2_layer_idx_t *li = NULL;
    if (any_gguf) {
        li_blk = osfs2_alloc_aligned(OSFS2_LAYERIDX_SIZE);
        if (li_blk) {
            if (osfs2_read_bytes(fd, OSFS2_LAYERIDX_OFF, li_blk, OSFS2_LAYERIDX_SIZE) < 0) {
                free(li_blk);
                li_blk = NULL;
            } else {
                li = (osfs2_layer_idx_t *)li_blk;
                uint8_t referenced[OSFS2_MAX_MODELS] = {0};
                for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++)
                    if ((ft[fi].flags & (OSFS2_FLAG_VALID | OSFS2_FLAG_GGUF)) ==
                            (OSFS2_FLAG_VALID | OSFS2_FLAG_GGUF) &&
                        ft[fi].layer_index_slot < OSFS2_MAX_MODELS)
                        referenced[ft[fi].layer_index_slot] = 1;
                for (uint32_t slot = 0; slot < OSFS2_MAX_MODELS; slot++)
                    if (!referenced[slot])
                        memset(&li[slot], 0, sizeof(osfs2_layer_idx_t));
            }
        }
    }

    /* ── Pre-validate ALL jobs ──────────────────────────────────── */

    /* Check for duplicate names within the batch */
    for (int i = 0; i < job_count; i++) {
        for (int j = i + 1; j < job_count; j++) {
            if (strcmp(jobs[i].stored_name, jobs[j].stored_name) == 0) {
                fprintf(stderr, "ositofs-write: duplicate name '%s' in batch (files '%s' and '%s')\n",
                        jobs[i].stored_name, jobs[i].local_path, jobs[j].local_path);
                goto fail_li;
            }
        }
    }

    /* Check space/slots */
    uint32_t new_files = 0; /* files that don't replace an existing one */

    for (int i = 0; i < job_count; i++) {
        /* Check if name exists in FS (and would be overwritten) */
        int exists = 0;
        for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++) {
            if ((ft[fi].flags & OSFS2_FLAG_VALID) &&
                strcmp(osfs2_entry_name(&ft[fi]), jobs[i].stored_name) == 0) {
                if (!overwrite) {
                    fprintf(stderr, "ositofs-write: file '%s' already exists (use --overwrite to replace)\n",
                            jobs[i].stored_name);
                    goto fail_li;
                }
                exists = 1;
                /* Freed blocks reduce needed total (approximately — reclaimed at top) */
                break;
            }
        }
        if (!exists) new_files++;
    }

    /* Count available file slots */
    {
        uint32_t valid_count = 0;
        for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++) {
            if (ft[fi].flags & OSFS2_FLAG_VALID) valid_count++;
        }
        if (valid_count + new_files > OSFS2_MAX_FILES) {
            fprintf(stderr, "ositofs-write: not enough file table slots (%u used, need %u more, max %u)\n",
                    valid_count, new_files, OSFS2_MAX_FILES);
            goto fail_li;
        }
    }

    /* Note: exact space check happens per-job after overwrites free blocks */

    /* ── Process each job ───────────────────────────────────────── */
    int success_count = 0;
    int exit_code = 0;

    for (int ji = 0; ji < job_count; ji++) {
        write_job_t *job = &jobs[ji];
        uint32_t blocks_needed = (uint32_t)((job->file_size + bs - 1) >> shift);

        if (job_count > 1)
            printf("\n[%d/%d] Writing '%s' (%u blocks)...\n", ji + 1, job_count,
                   job->stored_name, blocks_needed);
        else
            printf("  Blocks needed: %u\n", blocks_needed);

        int existing_idx = -1;
        for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++) {
            if ((ft[fi].flags & OSFS2_FLAG_VALID) &&
                strcmp(osfs2_entry_name(&ft[fi]), job->stored_name) == 0) {
                existing_idx = (int)fi;
                printf("  Replacing '%s' with copy-on-write (%u old blocks)\n",
                        job->stored_name, ft[fi].block_count);
                break;
            }
        }

        uint32_t start_block = 0;
        uint32_t file_crc = 0;

        if (blocks_needed > 0) {
            /* Allocate a contiguous run from the live-extent bitmap (first-fit).
             * This NEVER overlaps a live file. next_data_block is only a hint and
             * may be stale; the bitmap is authoritative. */
            uint32_t data_start_blk = osfs2_format_data_start_blk(
                sb.version, sb.block_size);
            start_block = blkmap_find_free(blocks_needed, data_start_blk);
            if (start_block && !blkmap_range_free(start_block, blocks_needed, data_start_blk))
                start_block = 0;  /* defensive */
            if (!start_block) {
                /* Try the next_data_block hint only if verified free, else
                 * re-derive a true high-water mark from the bitmap. */
                uint32_t hint = sb.next_data_block;
                if (hint < data_start_blk) hint = data_start_blk;
                if (blkmap_range_free(hint, blocks_needed, data_start_blk)) {
                    start_block = hint;
                } else {
                    uint32_t hwm = data_start_blk;
                    for (uint32_t b = data_start_blk; b < sb.total_blocks; b++)
                        if (blkmap_test(b)) hwm = b + 1;
                    if (blkmap_range_free(hwm, blocks_needed, data_start_blk))
                        start_block = hwm;
                }
            }
            if (!start_block) {
                fprintf(stderr, "ositofs-write: not enough contiguous space for '%s' (%u blocks needed)\n",
                        job->stored_name, blocks_needed);
                exit_code = 1;
                continue;  /* skip this file, try next */
            }

            /* Reserve the run in the bitmap immediately. */
            for (uint32_t b = 0; b < blocks_needed; b++)
                blkmap_set(start_block + b);

            printf("  Writing %u data blocks starting at block %u...\n", blocks_needed, start_block);

            /* Write data blocks */
            if (write_data_blocks(fd, job, start_block, blocks_needed, bs,
                                  crc_table, &file_crc, ji, job_count) < 0) {
                fprintf(stderr, "ositofs-write: failed to write data for '%s', skipping\n",
                        job->stored_name);
                for (uint32_t b = 0; b < blocks_needed; b++)
                    blkmap_clear(start_block + b);
                exit_code = 1;
                continue;
            }
        } else {
            printf("  Empty file: no data blocks\n");
        }

        /* Find a free file table slot */
        uint32_t file_idx = existing_idx >= 0
            ? (uint32_t)existing_idx : OSFS2_MAX_FILES;
        if (existing_idx < 0)
            for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++) {
                if (!(ft[fi].flags & OSFS2_FLAG_VALID)) {
                    file_idx = fi;
                    break;
                }
            }
        if (file_idx == OSFS2_MAX_FILES) {
            fprintf(stderr, "ositofs-write: file table full, skipping '%s'\n", job->stored_name);
            for (uint32_t b = 0; b < blocks_needed; b++)
                blkmap_clear(start_block + b);
            exit_code = 1;
            continue;
        }

        osfs2_file_t old_entry = ft[file_idx];
        osfs2_file_t new_entry;
        memset(&new_entry, 0, sizeof(new_entry));
        if (osfs2_set_entry_name(&new_entry, job->stored_name) < 0) {
            fprintf(stderr, "ositofs-write: cannot encode stored name '%s'\n",
                    job->stored_name);
            for (uint32_t b = 0; b < blocks_needed; b++)
                blkmap_clear(start_block + b);
            exit_code = 1;
            continue;
        }
        new_entry.size = job->file_size;
        new_entry.start_block = start_block;
        new_entry.block_count = blocks_needed;
        new_entry.crc32 = file_crc;
        new_entry.flags |= OSFS2_FLAG_VALID |
                           (job->is_gguf ? OSFS2_FLAG_GGUF : OSFS2_FLAG_RAW);
        new_entry.layer_index_slot = 0xFFFF;
        new_entry.create_time = (uint32_t)time(NULL);
        new_entry.modify_time = new_entry.create_time;

        /* GGUF metadata */
        if (job->is_gguf) {
            new_entry.quant_type = job->model_info.quant_type;
            new_entry.num_layers = job->model_info.num_layers;
            new_entry.hidden_size = job->model_info.hidden_size;
            new_entry.vocab_size = job->model_info.vocab_size;
            new_entry.head_count = job->model_info.head_count;
            new_entry.kv_head_count = job->model_info.kv_head_count;
            new_entry.context_length = job->model_info.context_length;
            strncpy(new_entry.model_name, job->model_info.model_name,
                    OSFS2_MODEL_NAME_LEN - 1);

            /* Write layer index if we have layer offsets */
            if (li && job->model_info.layer_count > 0) {
                uint16_t slot = 0xFFFF;
                for (uint16_t s = 0; s < OSFS2_MAX_MODELS; s++) {
                    if (li[s].num_layers == 0) { slot = s; break; }
                }
                if (slot != 0xFFFF) {
                    li[slot].num_layers = job->model_info.layer_count;
                    for (uint32_t l = 0; l < job->model_info.layer_count && l < OSFS2_MAX_LAYERS; l++)
                        li[slot].layer_offset[l] = job->model_info.layer_offsets[l];
                    new_entry.layer_index_slot = slot;
                    printf("  Layer index slot %u (%u layers)\n",
                           slot, job->model_info.layer_count);
                }
            }
        }

        osfs2_super_t before_sb = sb;
        ft[file_idx] = new_entry;
        osfs2_super_t after_sb = sb;
        recompute_super(&after_sb, ft);

        /* Side metadata and staged data must be durable before the entry that
         * exposes them is committed. Stale CRC/layer slots are harmless if an
         * uncommitted operation is interrupted. */
        if ((osfs2_crc_table_enabled &&
             osfs2_write_bytes(fd, OSFS2_CRCTAB_OFF, crc_blk,
                               OSFS2_CRCTAB_SIZE) < 0) ||
            (li_blk && osfs2_write_bytes(fd, OSFS2_LAYERIDX_OFF, li_blk,
                                         OSFS2_LAYERIDX_SIZE) < 0) ||
            osfs2_sync(fd) < 0) {
            ft[file_idx] = old_entry;
            for (uint32_t b = 0; b < blocks_needed; b++)
                blkmap_clear(start_block + b);
            fprintf(stderr, "ositofs-write: failed to stage metadata for '%s'\n",
                    job->stored_name);
            exit_code = 1;
            continue;
        }

        uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES] = { file_idx, 0 };
        osfs2_file_t before[OSFS2_JOURNAL_MAX_ENTRIES] = { old_entry };
        osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES] = { new_entry };
        if (osfs2_journal_commit_entries(fd, OSFS2_JOURNAL_OP_REPLACE,
                &before_sb, &after_sb, slots, before, after, 1) < 0) {
            fprintf(stderr, "ositofs-write: journaled commit failed for '%s'\n",
                    job->stored_name);
            exit_code = 1;
            break;
        }
        sb = after_sb;

        if ((old_entry.flags & OSFS2_FLAG_VALID) && old_entry.block_count) {
            for (uint32_t b = 0; b < old_entry.block_count; b++) {
                blkmap_clear(old_entry.start_block + b);
                if (old_entry.start_block + b < OSFS2_MAX_BLOCKS)
                    crc_table[old_entry.start_block + b] = 0;
            }
        }
        if (li && (old_entry.flags & OSFS2_FLAG_GGUF) &&
            old_entry.layer_index_slot != 0xFFFF &&
            old_entry.layer_index_slot < OSFS2_MAX_MODELS &&
            old_entry.layer_index_slot != new_entry.layer_index_slot)
            memset(&li[old_entry.layer_index_slot], 0,
                   sizeof(osfs2_layer_idx_t));

        if (blocks_needed > 0)
            printf("  [OK] '%s' written: blocks %u-%u, CRC 0x%08X\n",
                   job->stored_name, start_block, start_block + blocks_needed - 1, file_crc);
        else
            printf("  [OK] '%s' written: empty file, CRC 0x%08X\n",
                   job->stored_name, file_crc);
        success_count++;
    }

    /* Remove stale side metadata left by successfully replaced extents. */
    if (success_count > 0) {
        if (osfs2_crc_table_enabled &&
            osfs2_write_bytes(fd, OSFS2_CRCTAB_OFF, crc_blk,
                              OSFS2_CRCTAB_SIZE) < 0) {
            fprintf(stderr, "ositofs-write: failed to write CRC table\n");
            goto fail_li;
        }
        if (li_blk) {
            if (osfs2_write_bytes(fd, OSFS2_LAYERIDX_OFF, li_blk, OSFS2_LAYERIDX_SIZE) < 0) {
                fprintf(stderr, "ositofs-write: failed to write layer index\n");
                goto fail_li;
            }
        }
        if (osfs2_sync(fd) < 0) goto fail_li;

        if (job_count > 1) {
            printf("\nBatch complete: %d/%d files written successfully\n",
                   success_count, job_count);
        }
    } else {
        fprintf(stderr, "ositofs-write: no files written successfully\n");
        exit_code = 1;
    }

    if (li_blk) free(li_blk);
    free(crc_blk);
    free(ft_blk);
    free(g_blkmap); g_blkmap = NULL;
    /* Free strdup'd paths from manifest */
    if (manifest_path) {
        for (int i = pos_count; i < job_count; i++) {
            free((void *)jobs[i].local_path);
        }
    }
    free(jobs);
    osfs2_close_device(fd);
    return exit_code;

fail_li:
    if (li_blk) free(li_blk);
fail_crc:
    free(crc_blk);
fail:
    free(ft_blk);
    free(g_blkmap); g_blkmap = NULL;
    if (manifest_path) {
        for (int i = pos_count; i < job_count; i++) {
            free((void *)jobs[i].local_path);
        }
    }
    free(jobs);
    osfs2_close_device(fd);
    return 1;
}
