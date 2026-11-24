/*
 * mkfs.ositofs — Format device with OsitoFS v2
 *
 * Usage: mkfs.ositofs <device> [--label <name>]
 *
 * Writes superblock, empty file table, empty CRC table,
 * and empty layer index.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

static void usage(void)
{
    fprintf(stderr, "Usage: mkfs.ositofs <device> [--label <name>] [--block-size <bytes>] [--file-slots <count>]\n");
    fprintf(stderr, "  --block-size: data block size (64K..1M, power of 2, default 1M)\n");
    fprintf(stderr, "  --file-slots: 4096..61440, multiple of 4096 (default 16384)\n");
    fprintf(stderr, "  --max-files: legacy alias for --file-slots\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *label = "osito-ai";
    uint32_t block_size = OSFS2_DEFAULT_BLOCK_SIZE;
    uint32_t file_slots = OSFS2_DEFAULT_MAX_FILES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
            block_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if ((strcmp(argv[i], "--file-slots") == 0 ||
                    strcmp(argv[i], "--max-files") == 0) &&
                   i + 1 < argc) {
            file_slots = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] != '-') {
            device = argv[i];
        } else {
            usage();
        }
    }
    if (!device) usage();

    if (!osfs2_valid_block_size(block_size)) {
        fprintf(stderr, "mkfs: invalid block size %u (must be power-of-2 in [%u, %u])\n",
                block_size, OSFS2_MIN_BLOCK_SIZE, OSFS2_MAX_BLOCK_SIZE);
        return 1;
    }
    if (!osfs2_valid_file_slots(file_slots)) {
        fprintf(stderr,
                "mkfs: invalid file slot count %u (must be %u..%u, multiple of %u)\n",
                file_slots, OSFS2_LEGACY_MAX_FILES, OSFS2_MAX_FILE_SLOTS,
                OSFS2_FILE_SLOT_GRANULE);
        return 1;
    }

    uint32_t version = OSFS2_VERSION;

    /* Set runtime block size before any allocations */
    osfs2_block_sz = block_size;

    int fd = osfs2_open_device(device, 0);
    if (fd < 0) return 1;

    uint64_t dev_size = osfs2_device_size(fd);
    uint32_t shift = osfs2_block_shift(block_size);
    uint32_t total_blocks = (uint32_t)(dev_size >> shift);
    uint32_t filetab_size = osfs2_layout_filetab_size_for_slots(file_slots);
    uint32_t crctab_off = osfs2_layout_crctab_off_for_slots(file_slots);
    uint32_t layeridx_off = osfs2_layout_layeridx_off_for_slots(file_slots);
    uint32_t data_off = osfs2_layout_data_off_for_slots(file_slots);
    uint32_t data_start =
        osfs2_layout_data_start_blk_for_slots(block_size, file_slots);

    osfs2_file_capacity = file_slots;
    osfs2_filetab_size = filetab_size;
    osfs2_crctab_offset = crctab_off;
    osfs2_layeridx_offset = layeridx_off;
    osfs2_crc_table_enabled = 1;
    osfs2_layer_index_enabled = 1;

    if (total_blocks < data_start + 1) {
        fprintf(stderr, "mkfs: device too small (need at least %u blocks for metadata + 1 data, have %u)\n",
                data_start + 1, total_blocks);
        osfs2_close_device(fd);
        return 1;
    }

    printf("mkfs.ositofs: formatting %s\n", device);
    printf("  Device size:  "); osfs2_print_size(dev_size); printf("\n");
    printf("  Block size:   "); osfs2_print_size(block_size); printf("\n");
    printf("  Total blocks: %u\n", total_blocks);
    printf("  Data blocks:  %u (starting at block %u)\n",
           total_blocks - data_start, data_start);
    printf("  File slots:   %u\n", file_slots);
    printf("  Label: %s\n", label);

    uint32_t meta_size = filetab_size;
    if (meta_size < OSFS2_SUPER_REGION_SIZE)
        meta_size = OSFS2_SUPER_REGION_SIZE;
    if (meta_size < OSFS2_CRCTAB_SIZE)
        meta_size = OSFS2_CRCTAB_SIZE;
    if (meta_size < OSFS2_LAYERIDX_SIZE)
        meta_size = OSFS2_LAYERIDX_SIZE;

    void *meta = osfs2_alloc_aligned(meta_size);
    if (!meta) { osfs2_close_device(fd); return 1; }

    /* ── Superblock (at offset 0, first 512 bytes of region) ───── */
    memset(meta, 0, meta_size);
    osfs2_super_t *sb = (osfs2_super_t *)meta;
    sb->magic = OSFS2_MAGIC;
    sb->version = version;
    sb->block_size = block_size;
    sb->total_blocks = total_blocks;
    sb->used_blocks = data_start;  /* metadata only */
    sb->file_count = 0;
    sb->next_data_block = data_start;
    osfs2_gen_uuid(sb->uuid);
    strncpy(sb->label, label, OSFS2_LABEL_LEN - 1);
    sb->create_time = (uint64_t)time(NULL);
    sb->layout_magic = OSFS2_LAYOUT_MAGIC;
    sb->file_table_slots = file_slots;
    sb->metadata_bytes = data_off;
    sb->crc32 = 0;
    sb->crc32 = osfs2_crc32(sb, sizeof(*sb));
    memcpy((uint8_t *)meta + OSFS2_SUPER_BACKUP_OFF, sb, sizeof(*sb));

    /* Write first 1MB region (superblock + padding) */
    if (osfs2_write_bytes(fd, 0, meta, OSFS2_SUPER_REGION_SIZE) < 0)
        goto fail;
    printf("  [OK] Superblock written (block_size=");
    osfs2_print_size(block_size); printf(")\n");

    /* ── File Table at offset 1MB (zeroed = no files) ────────── */
    memset(meta, 0, meta_size);
    if (osfs2_write_bytes(fd, OSFS2_FILETAB_OFF, meta, filetab_size) < 0)
        goto fail;
    printf("  [OK] File table written (%u slots)\n", file_slots);

    /* ── CRC Table at offset 2MB (zeroed) ────────────────────── */
    if (osfs2_crc_table_enabled) {
        if (osfs2_write_bytes(fd, crctab_off, meta,
                              OSFS2_CRCTAB_SIZE) < 0)
            goto fail;
        printf("  [OK] Block CRC table written\n");
    }

    /* ── Layer Index at offset 3MB (zeroed) ──────────────────── */
    if (osfs2_layer_index_enabled) {
        if (osfs2_write_bytes(fd, layeridx_off, meta,
                              OSFS2_LAYERIDX_SIZE) < 0)
            goto fail;
        printf("  [OK] Layer index written (%u slots)\n", OSFS2_MAX_MODELS);
    } else {
        printf("  [OK] Large-file layout (layer index disabled)\n");
    }

    printf("\nFormatting complete. Metadata overhead: ");
    osfs2_print_size(data_off);
    printf(" / ");
    osfs2_print_size(dev_size);
    printf(" (%.2f%%)\n",
           100.0 * data_off / dev_size);

    free(meta);
    osfs2_close_device(fd);
    return 0;

fail:
    free(meta);
    osfs2_close_device(fd);
    return 1;
}
