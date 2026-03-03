/*
 * ositofs-write — Write files to OsitoFS v2 partition
 *
 * Usage: ositofs-write <device> <file> [--name <name>]
 *
 * For GGUF files: auto-detects format, parses metadata, generates layer index.
 * Files are write-once, contiguous, first-fit allocation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>

#include "common.h"
#include "gguf.h"

static void usage(void)
{
    fprintf(stderr, "Usage: ositofs-write <device> <file> [--name <name>]\n");
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

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *filepath = NULL;
    const char *name_override = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name_override = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!device) device = argv[i];
            else if (!filepath) filepath = argv[i];
            else usage();
        } else {
            usage();
        }
    }
    if (!device || !filepath) usage();

    /* Get source file info */
    struct stat st;
    if (stat(filepath, &st) < 0) {
        fprintf(stderr, "ositofs-write: cannot stat %s: %s\n", filepath, strerror(errno));
        return 1;
    }
    uint64_t file_size = (uint64_t)st.st_size;

    /* Determine filename */
    char namebuf[OSFS2_NAME_LEN];
    if (name_override) {
        strncpy(namebuf, name_override, OSFS2_NAME_LEN - 1);
        namebuf[OSFS2_NAME_LEN - 1] = '\0';
    } else {
        char *tmp = strdup(filepath);
        strncpy(namebuf, basename(tmp), OSFS2_NAME_LEN - 1);
        namebuf[OSFS2_NAME_LEN - 1] = '\0';
        free(tmp);
    }

    printf("ositofs-write: %s -> %s\n", filepath, namebuf);
    printf("  File size: "); osfs2_print_size(file_size); printf("\n");

    /* Check if GGUF and parse metadata */
    int is_gguf = is_gguf_file(filepath);
    gguf_model_info_t model_info;
    memset(&model_info, 0, sizeof(model_info));

    if (is_gguf) {
        printf("  Detected GGUF format, parsing metadata...\n");
        if (gguf_parse(filepath, &model_info) < 0) {
            fprintf(stderr, "ositofs-write: GGUF parse failed, writing as raw file\n");
            is_gguf = 0;
        }
    }

    /* Open device and read superblock */
    int fd = osfs2_open_device(device, 0);
    if (fd < 0) return 1;

    osfs2_super_t sb;
    if (osfs2_read_super(fd, &sb) < 0) {
        osfs2_close_device(fd);
        return 1;
    }

    /* Calculate blocks needed */
    uint32_t blocks_needed = (uint32_t)((file_size + OSFS2_BLOCK_SIZE - 1) >> OSFS2_BLOCK_SHIFT);
    uint32_t avail = sb.total_blocks - sb.next_data_block;

    printf("  Blocks needed: %u, available: %u\n", blocks_needed, avail);
    if (blocks_needed > avail) {
        fprintf(stderr, "ositofs-write: not enough space (%u blocks needed, %u available)\n",
                blocks_needed, avail);
        osfs2_close_device(fd);
        return 1;
    }

    /* Check file table capacity */
    if (sb.file_count >= OSFS2_MAX_FILES) {
        fprintf(stderr, "ositofs-write: file table full (%u files)\n", sb.file_count);
        osfs2_close_device(fd);
        return 1;
    }

    /* Read file table block */
    void *ft_blk = osfs2_alloc_block();
    if (!ft_blk) { osfs2_close_device(fd); return 1; }
    if (osfs2_read_block(fd, OSFS2_FILETAB_BLK, ft_blk) < 0) goto fail;

    /* Check for duplicate name */
    osfs2_file_t *ft = (osfs2_file_t *)ft_blk;
    for (uint32_t i = 0; i < sb.file_count; i++) {
        if ((ft[i].flags & OSFS2_FLAG_VALID) && strcmp(ft[i].name, namebuf) == 0) {
            fprintf(stderr, "ositofs-write: file '%s' already exists (write-once FS)\n", namebuf);
            goto fail;
        }
    }

    /* Allocate data blocks (contiguous, first-fit = next_data_block) */
    uint32_t start_block = sb.next_data_block;

    printf("  Writing %u data blocks starting at block %u...\n", blocks_needed, start_block);

    /* Copy file data to device block by block */
    FILE *src = fopen(filepath, "rb");
    if (!src) {
        fprintf(stderr, "ositofs-write: cannot open %s: %s\n", filepath, strerror(errno));
        goto fail;
    }

    void *data_blk = osfs2_alloc_block();
    if (!data_blk) { fclose(src); goto fail; }

    /* Also read CRC table for per-block CRCs */
    void *crc_blk = osfs2_alloc_block();
    if (!crc_blk) { osfs2_free_block(data_blk); fclose(src); goto fail; }
    if (osfs2_read_block(fd, OSFS2_CRCTAB_BLK, crc_blk) < 0) {
        osfs2_free_block(crc_blk); osfs2_free_block(data_blk); fclose(src); goto fail;
    }
    uint32_t *crc_table = (uint32_t *)crc_blk;

    /* CRC32 of entire file */
    uint32_t file_crc = 0xFFFFFFFF;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        memset(data_blk, 0, OSFS2_BLOCK_SIZE);
        size_t to_read = OSFS2_BLOCK_SIZE;
        uint64_t remaining = file_size - (uint64_t)b * OSFS2_BLOCK_SIZE;
        if (remaining < to_read) to_read = (size_t)remaining;

        if (fread(data_blk, 1, to_read, src) != to_read) {
            fprintf(stderr, "ositofs-write: read error at block %u\n", b);
            osfs2_free_block(crc_blk); osfs2_free_block(data_blk); fclose(src); goto fail;
        }

        /* Update running CRC */
        const uint8_t *p = (const uint8_t *)data_blk;
        for (size_t i = 0; i < to_read; i++) {
            file_crc ^= p[i];
            for (int j = 0; j < 8; j++)
                file_crc = (file_crc >> 1) ^ (OSFS2_CRC32_POLY & (-(file_crc & 1)));
        }

        /* Block CRC */
        uint32_t blk_idx = start_block + b;
        if (blk_idx < OSFS2_MAX_BLOCKS)
            crc_table[blk_idx] = osfs2_crc32(data_blk, OSFS2_BLOCK_SIZE);

        if (osfs2_write_block(fd, start_block + b, data_blk) < 0) {
            osfs2_free_block(crc_blk); osfs2_free_block(data_blk); fclose(src); goto fail;
        }

        if ((b + 1) % 100 == 0 || b + 1 == blocks_needed)
            printf("\r  Block %u/%u", b + 1, blocks_needed);
    }
    printf("\n");

    file_crc = ~file_crc;

    fclose(src);
    osfs2_free_block(data_blk);

    /* Write updated CRC table */
    if (osfs2_write_block(fd, OSFS2_CRCTAB_BLK, crc_blk) < 0) {
        osfs2_free_block(crc_blk);
        goto fail;
    }
    osfs2_free_block(crc_blk);

    /* Fill file table entry */
    uint32_t file_idx = sb.file_count;
    memset(&ft[file_idx], 0, sizeof(osfs2_file_t));
    strncpy(ft[file_idx].name, namebuf, OSFS2_NAME_LEN - 1);
    ft[file_idx].size = file_size;
    ft[file_idx].start_block = start_block;
    ft[file_idx].block_count = blocks_needed;
    ft[file_idx].crc32 = file_crc;
    ft[file_idx].flags = OSFS2_FLAG_VALID | (is_gguf ? OSFS2_FLAG_GGUF : OSFS2_FLAG_RAW);
    ft[file_idx].layer_index_slot = 0xFFFF;

    if (is_gguf) {
        ft[file_idx].quant_type = model_info.quant_type;
        ft[file_idx].num_layers = model_info.num_layers;
        ft[file_idx].hidden_size = model_info.hidden_size;
        ft[file_idx].vocab_size = model_info.vocab_size;
        ft[file_idx].head_count = model_info.head_count;
        ft[file_idx].kv_head_count = model_info.kv_head_count;
        ft[file_idx].context_length = model_info.context_length;
        strncpy(ft[file_idx].model_name, model_info.model_name, OSFS2_MODEL_NAME_LEN - 1);

        /* Write layer index if we have layer offsets */
        if (model_info.layer_count > 0) {
            void *li_blk = osfs2_alloc_block();
            if (li_blk) {
                if (osfs2_read_block(fd, OSFS2_LAYERIDX_BLK, li_blk) == 0) {
                    osfs2_layer_idx_t *li = (osfs2_layer_idx_t *)li_blk;
                    /* Find free slot */
                    uint16_t slot = 0xFFFF;
                    for (uint16_t s = 0; s < OSFS2_MAX_MODELS; s++) {
                        if (li[s].num_layers == 0) { slot = s; break; }
                    }
                    if (slot != 0xFFFF) {
                        li[slot].num_layers = model_info.layer_count;
                        for (uint32_t l = 0; l < model_info.layer_count && l < OSFS2_MAX_LAYERS; l++)
                            li[slot].layer_offset[l] = model_info.layer_offsets[l];
                        ft[file_idx].layer_index_slot = slot;
                        osfs2_write_block(fd, OSFS2_LAYERIDX_BLK, li_blk);
                        printf("  Layer index written (slot %u, %u layers)\n",
                               slot, model_info.layer_count);
                    }
                }
                osfs2_free_block(li_blk);
            }
        }
    }

    /* Write updated file table */
    if (osfs2_write_block(fd, OSFS2_FILETAB_BLK, ft_blk) < 0) goto fail;

    /* Update superblock */
    sb.file_count = file_idx + 1;
    sb.next_data_block = start_block + blocks_needed;
    sb.used_blocks = sb.next_data_block;

    void *sb_blk = osfs2_alloc_block();
    if (!sb_blk) goto fail;
    memcpy(sb_blk, &sb, sizeof(sb));
    osfs2_super_t *sb_ptr = (osfs2_super_t *)sb_blk;
    sb_ptr->crc32 = 0;
    sb_ptr->crc32 = osfs2_crc32(sb_ptr, sizeof(*sb_ptr));
    if (osfs2_write_block(fd, OSFS2_SUPERBLOCK_BLK, sb_blk) < 0) {
        osfs2_free_block(sb_blk);
        goto fail;
    }
    osfs2_free_block(sb_blk);

    printf("  [OK] File '%s' written successfully\n", namebuf);
    printf("  Blocks: %u-%u, CRC: 0x%08X\n",
           start_block, start_block + blocks_needed - 1, file_crc);

    osfs2_free_block(ft_blk);
    osfs2_close_device(fd);
    return 0;

fail:
    osfs2_free_block(ft_blk);
    osfs2_close_device(fd);
    return 1;
}
