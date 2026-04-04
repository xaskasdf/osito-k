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

typedef struct {
    const char *local_path;
    char stored_name[OSFS2_NAME_LEN];
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
            strncpy(j->stored_name, stored, OSFS2_NAME_LEN - 1);
            j->stored_name[OSFS2_NAME_LEN - 1] = '\0';
        } else {
            char *tmp = strdup(local);
            strncpy(j->stored_name, basename(tmp), OSFS2_NAME_LEN - 1);
            j->stored_name[OSFS2_NAME_LEN - 1] = '\0';
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
        if (blk_idx < OSFS2_MAX_BLOCKS)
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
            strncpy(j->stored_name, name_override, OSFS2_NAME_LEN - 1);
            j->stored_name[OSFS2_NAME_LEN - 1] = '\0';
        } else {
            char *tmp = strdup(pos_files[i]);
            strncpy(j->stored_name, basename(tmp), OSFS2_NAME_LEN - 1);
            j->stored_name[OSFS2_NAME_LEN - 1] = '\0';
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

    void *crc_blk = osfs2_alloc_aligned(OSFS2_CRCTAB_SIZE);
    if (!crc_blk) goto fail;
    if (osfs2_read_bytes(fd, OSFS2_CRCTAB_OFF, crc_blk, OSFS2_CRCTAB_SIZE) < 0)
        goto fail_crc;

    uint32_t *crc_table = (uint32_t *)crc_blk;

    /* Read layer index (once, for GGUF files) */
    int any_gguf = 0;
    for (int i = 0; i < job_count; i++)
        if (jobs[i].is_gguf) { any_gguf = 1; break; }

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
                strcmp(ft[fi].name, jobs[i].stored_name) == 0) {
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

        /* Handle overwrite: delete existing in-memory */
        for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++) {
            if ((ft[fi].flags & OSFS2_FLAG_VALID) &&
                strcmp(ft[fi].name, job->stored_name) == 0) {
                printf("  Overwriting '%s' (freeing %u blocks)\n",
                       job->stored_name, ft[fi].block_count);
                uint32_t old_blocks = ft[fi].block_count;
                uint32_t old_top = ft[fi].start_block + old_blocks;

                /* Free layer index slot if GGUF */
                if (li && ft[fi].layer_index_slot != 0xFFFF &&
                    ft[fi].layer_index_slot < OSFS2_MAX_MODELS) {
                    li[ft[fi].layer_index_slot].num_layers = 0;
                }

                ft[fi].flags = 0;
                if (sb.file_count > 0) sb.file_count--;
                sb.used_blocks -= old_blocks;

                /* Reclaim if this was the topmost allocation */
                if (old_top == sb.next_data_block) {
                    uint32_t hwm = osfs2_data_start_blk(sb.block_size);
                    for (uint32_t j = 0; j < OSFS2_MAX_FILES; j++) {
                        if (!(ft[j].flags & OSFS2_FLAG_VALID)) continue;
                        uint32_t end = ft[j].start_block + ft[j].block_count;
                        if (end > hwm) hwm = end;
                    }
                    sb.next_data_block = hwm;
                }
                break;
            }
        }

        /* Check available space */
        uint32_t avail = sb.total_blocks - sb.next_data_block;
        if (blocks_needed > avail) {
            fprintf(stderr, "ositofs-write: not enough space for '%s' (%u blocks needed, %u available)\n",
                    job->stored_name, blocks_needed, avail);
            exit_code = 1;
            continue;  /* skip this file, try next */
        }

        /* Allocate data blocks (contiguous, next_data_block) */
        uint32_t start_block = sb.next_data_block;
        printf("  Writing %u data blocks starting at block %u...\n", blocks_needed, start_block);

        /* Write data blocks */
        uint32_t file_crc = 0;
        if (write_data_blocks(fd, job, start_block, blocks_needed, bs,
                              crc_table, &file_crc, ji, job_count) < 0) {
            fprintf(stderr, "ositofs-write: failed to write data for '%s', skipping\n",
                    job->stored_name);
            exit_code = 1;
            continue;
        }

        /* Find a free file table slot */
        uint32_t file_idx = OSFS2_MAX_FILES; /* sentinel */
        /* First: look for freed (invalid) slot within current range */
        for (uint32_t fi = 0; fi < OSFS2_MAX_FILES; fi++) {
            if (!(ft[fi].flags & OSFS2_FLAG_VALID)) {
                /* Check it's either within file_count or at file_count (append) */
                file_idx = fi;
                break;
            }
        }
        if (file_idx == OSFS2_MAX_FILES) {
            fprintf(stderr, "ositofs-write: file table full, skipping '%s'\n", job->stored_name);
            exit_code = 1;
            continue;
        }

        /* Fill file table entry */
        memset(&ft[file_idx], 0, sizeof(osfs2_file_t));
        strncpy(ft[file_idx].name, job->stored_name, OSFS2_NAME_LEN - 1);
        ft[file_idx].size = job->file_size;
        ft[file_idx].start_block = start_block;
        ft[file_idx].block_count = blocks_needed;
        ft[file_idx].crc32 = file_crc;
        ft[file_idx].flags = OSFS2_FLAG_VALID | (job->is_gguf ? OSFS2_FLAG_GGUF : OSFS2_FLAG_RAW);
        ft[file_idx].layer_index_slot = 0xFFFF;
        ft[file_idx].create_time = (uint32_t)time(NULL);
        ft[file_idx].modify_time = ft[file_idx].create_time;

        /* GGUF metadata */
        if (job->is_gguf) {
            ft[file_idx].quant_type = job->model_info.quant_type;
            ft[file_idx].num_layers = job->model_info.num_layers;
            ft[file_idx].hidden_size = job->model_info.hidden_size;
            ft[file_idx].vocab_size = job->model_info.vocab_size;
            ft[file_idx].head_count = job->model_info.head_count;
            ft[file_idx].kv_head_count = job->model_info.kv_head_count;
            ft[file_idx].context_length = job->model_info.context_length;
            strncpy(ft[file_idx].model_name, job->model_info.model_name,
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
                    ft[file_idx].layer_index_slot = slot;
                    printf("  Layer index slot %u (%u layers)\n",
                           slot, job->model_info.layer_count);
                }
            }
        }

        /* Update superblock in-memory */
        if (file_idx >= sb.file_count)
            sb.file_count = file_idx + 1;
        sb.next_data_block = start_block + blocks_needed;
        sb.used_blocks = sb.next_data_block;

        printf("  [OK] '%s' written: blocks %u-%u, CRC 0x%08X\n",
               job->stored_name, start_block, start_block + blocks_needed - 1, file_crc);
        success_count++;
    }

    /* ── Flush all metadata once ────────────────────────────────── */
    if (success_count > 0) {
        /* Write CRC table */
        if (osfs2_write_bytes(fd, OSFS2_CRCTAB_OFF, crc_blk, OSFS2_CRCTAB_SIZE) < 0) {
            fprintf(stderr, "ositofs-write: failed to write CRC table\n");
            goto fail_li;
        }

        /* Write layer index if any GGUF files were written */
        if (li_blk) {
            if (osfs2_write_bytes(fd, OSFS2_LAYERIDX_OFF, li_blk, OSFS2_LAYERIDX_SIZE) < 0) {
                fprintf(stderr, "ositofs-write: failed to write layer index\n");
                goto fail_li;
            }
        }

        /* Write file table */
        if (osfs2_write_bytes(fd, OSFS2_FILETAB_OFF, ft_blk, OSFS2_FILETAB_SIZE) < 0) {
            fprintf(stderr, "ositofs-write: failed to write file table\n");
            goto fail_li;
        }

        /* Write superblock + backup */
        void *sb_blk = osfs2_alloc_aligned(4096);
        if (!sb_blk) goto fail_li;
        memcpy(sb_blk, &sb, sizeof(sb));
        osfs2_super_t *sb_ptr = (osfs2_super_t *)sb_blk;
        sb_ptr->crc32 = 0;
        sb_ptr->crc32 = osfs2_crc32(sb_ptr, sizeof(*sb_ptr));
        if (osfs2_write_bytes(fd, 0, sb_blk, 4096) < 0) {
            free(sb_blk);
            goto fail_li;
        }
        osfs2_write_bytes(fd, OSFS2_SUPER_BACKUP_OFF, sb_blk, 4096);
        free(sb_blk);

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
    if (manifest_path) {
        for (int i = pos_count; i < job_count; i++) {
            free((void *)jobs[i].local_path);
        }
    }
    free(jobs);
    osfs2_close_device(fd);
    return 1;
}
