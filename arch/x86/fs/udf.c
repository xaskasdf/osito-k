/*
 * OsitoK x86-64 — UDF Read-Only Driver (ECMA-167 / ISO 13346)
 *
 * Reads files from DVD/Blu-ray media and .iso images with UDF filesystem.
 * Supports Anchor Volume Descriptor, partition map, FID chain.
 * Minimal: no extended attributes, no named streams.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Block device read — set by udf_mount */
static int (*udf_read_sectors)(uint64_t lba, uint32_t count, void *buf);

/* ── UDF On-Disk Structures ──────────────────────────────────── */

#define UDF_SECTOR_SIZE   2048
#define UDF_AVDP_LBA      256   /* Anchor Volume Descriptor Pointer */

/* Descriptor tag (16 bytes, starts every descriptor) */
typedef struct __attribute__((packed)) {
    uint16_t tag_id;
    uint16_t descriptor_version;
    uint8_t  tag_checksum;
    uint8_t  reserved;
    uint16_t tag_serial;
    uint16_t desc_crc;
    uint16_t desc_crc_length;
    uint32_t tag_location;
} udf_tag_t;

/* Tag IDs */
#define UDF_TAG_PVD         1   /* Primary Volume Descriptor */
#define UDF_TAG_AVDP        2   /* Anchor Volume Descriptor Pointer */
#define UDF_TAG_PD          5   /* Partition Descriptor */
#define UDF_TAG_LVD         6   /* Logical Volume Descriptor */
#define UDF_TAG_FSD         256 /* File Set Descriptor */
#define UDF_TAG_FID         257 /* File Identifier Descriptor */
#define UDF_TAG_FE          261 /* File Entry */

/* Extent (8 bytes: length + location) */
typedef struct __attribute__((packed)) {
    uint32_t length;
    uint32_t location;
} udf_extent_t;

/* Long allocation descriptor (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t length;
    uint32_t block;
    uint16_t partition;
    uint8_t  impl_use[6];
} udf_long_ad_t;

/* Short allocation descriptor (8 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t length;       /* Upper 2 bits = type, lower 30 = length */
    uint32_t position;     /* Logical block within partition */
} udf_short_ad_t;

/* ICB Tag (20 bytes, in File Entry) */
typedef struct __attribute__((packed)) {
    uint32_t prior_entries;
    uint16_t strategy_type;
    uint16_t strategy_param;
    uint16_t max_entries;
    uint8_t  reserved;
    uint8_t  file_type;    /* 4=dir, 5=file */
    uint8_t  parent_icb[6];
    uint16_t flags;        /* bit 0-2: alloc type (0=short, 1=long, 3=inline) */
} udf_icb_tag_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint32_t part_start;     /* Partition start LBA */
    uint32_t root_block;     /* Root directory FE block (partition-relative) */
    uint32_t root_part;      /* Root directory partition number */
    char     volume_id[33];
} udf;

/* ── Helpers ─────────────────────────────────────────────────── */

static int udf_read_block(uint32_t lba, void *buf)
{
    return udf_read_sectors(lba, 1, buf);
}

/* Read a block relative to the partition */
static int udf_read_part_block(uint32_t block, void *buf)
{
    return udf_read_block(udf.part_start + block, buf);
}

/* ── Directory reading ───────────────────────────────────────── */

typedef void (*udf_dir_cb)(const char *name, uint32_t block,
                           uint32_t size, uint8_t file_type, void *ctx);

static void udf_read_dir(uint32_t dir_block, uint32_t dir_len,
                         udf_dir_cb cb, void *ctx)
{
    uint32_t sectors = (dir_len + UDF_SECTOR_SIZE - 1) / UDF_SECTOR_SIZE;
    uint8_t *buf = (uint8_t *)kmalloc((uint64_t)sectors * UDF_SECTOR_SIZE);
    if (!buf) return;

    for (uint32_t i = 0; i < sectors; i++) {
        if (udf_read_part_block(dir_block + i, buf + i * UDF_SECTOR_SIZE) < 0) {
            kfree(buf);
            return;
        }
    }

    uint32_t offset = 0;
    while (offset + 38 < dir_len) {
        udf_tag_t *tag = (udf_tag_t *)(buf + offset);
        if (tag->tag_id != UDF_TAG_FID) break;

        uint16_t impl_len = *(uint16_t *)(buf + offset + 36);
        uint8_t  name_len = *(uint8_t *)(buf + offset + 19);
        uint8_t  file_chars = *(uint8_t *)(buf + offset + 18);

        /* File ICB location */
        udf_long_ad_t *icb = (udf_long_ad_t *)(buf + offset + 20);

        /* Extract filename (after implementation use area) */
        uint8_t *name_raw = buf + offset + 38 + impl_len;
        char name[256];
        int nlen = 0;

        if (name_len > 0 && !(file_chars & 0x08)) {  /* Skip parent entry */
            if (name_raw[0] == 8) {
                /* CS0 encoding: byte 0 = encoding type, rest = ASCII */
                for (int i = 1; i < name_len && nlen < 254; i++)
                    name[nlen++] = (char)name_raw[i];
            } else if (name_raw[0] == 16) {
                /* CS0 encoding: UTF-16BE */
                for (int i = 1; i + 1 < name_len && nlen < 254; i += 2) {
                    uint16_t c = ((uint16_t)name_raw[i] << 8) | name_raw[i+1];
                    name[nlen++] = (c < 128) ? (char)c : '?';
                }
            }
            name[nlen] = '\0';

            if (nlen > 0) {
                /* Read file entry to get size */
                uint32_t fe_block = icb->block;
                uint8_t fe_buf[UDF_SECTOR_SIZE];
                uint32_t fsize = 0;
                uint8_t ftype = 5;  /* assume file */

                if (udf_read_part_block(fe_block, fe_buf) == 0) {
                    udf_tag_t *fe_tag = (udf_tag_t *)fe_buf;
                    if (fe_tag->tag_id == UDF_TAG_FE) {
                        udf_icb_tag_t *icbt = (udf_icb_tag_t *)(fe_buf + 16);
                        ftype = icbt->file_type;
                        /* Info length at offset 56 (uint64_t) */
                        fsize = *(uint32_t *)(fe_buf + 56);
                    }
                }
                cb(name, fe_block, fsize, ftype, ctx);
            }
        }

        /* Advance: 38 + impl_len + name_len, rounded up to 4 bytes */
        uint32_t entry_len = 38 + impl_len + name_len;
        entry_len = (entry_len + 3) & ~3;
        offset += entry_len;
    }

    kfree(buf);
}

/* ── Public API ──────────────────────────────────────────────── */

int udf_mount(int (*read_fn)(uint64_t lba, uint32_t count, void *buf))
{
    udf_read_sectors = read_fn;

    /* Read Anchor Volume Descriptor Pointer at LBA 256 */
    uint8_t avdp_buf[UDF_SECTOR_SIZE];
    if (udf_read_block(UDF_AVDP_LBA, avdp_buf) < 0) return -1;

    udf_tag_t *avdp_tag = (udf_tag_t *)avdp_buf;
    if (avdp_tag->tag_id != UDF_TAG_AVDP) {
        serial_puts("[UDF] No AVDP at LBA 256\n");
        return -1;
    }

    /* Main VDS extent */
    udf_extent_t *mvds = (udf_extent_t *)(avdp_buf + 16);
    uint32_t vds_lba = mvds->location;
    uint32_t vds_len = mvds->length / UDF_SECTOR_SIZE;

    /* Scan Volume Descriptor Sequence for PD and LVD */
    uint8_t vds_buf[UDF_SECTOR_SIZE];
    for (uint32_t i = 0; i < vds_len && i < 16; i++) {
        if (udf_read_block(vds_lba + i, vds_buf) < 0) continue;
        udf_tag_t *tag = (udf_tag_t *)vds_buf;

        if (tag->tag_id == UDF_TAG_PVD) {
            /* Volume ID at offset 24, dstring 32 bytes */
            uint8_t *vid = vds_buf + 24;
            int vlen = vid[31];  /* length in last byte of dstring */
            if (vlen > 30) vlen = 30;
            for (int j = 0; j < vlen; j++)
                udf.volume_id[j] = (char)vid[j];
            udf.volume_id[vlen] = '\0';
        }
        if (tag->tag_id == UDF_TAG_PD) {
            /* Partition start location at offset 188 */
            udf.part_start = *(uint32_t *)(vds_buf + 188);
        }
        if (tag->tag_id == UDF_TAG_LVD) {
            /* FSD location: long_ad at offset 248 */
            udf_long_ad_t *fsd_ad = (udf_long_ad_t *)(vds_buf + 248);
            /* Read FSD to find root directory */
            uint8_t fsd_buf[UDF_SECTOR_SIZE];
            if (udf_read_part_block(fsd_ad->block, fsd_buf) == 0) {
                udf_tag_t *fsd_tag = (udf_tag_t *)fsd_buf;
                if (fsd_tag->tag_id == UDF_TAG_FSD) {
                    /* Root directory ICB at offset 400 */
                    udf_long_ad_t *root_icb = (udf_long_ad_t *)(fsd_buf + 400);
                    udf.root_block = root_icb->block;
                    udf.root_part = root_icb->partition;
                }
            }
        }
    }

    if (udf.part_start == 0) {
        serial_puts("[UDF] No partition descriptor found\n");
        return -1;
    }

    udf.mounted = true;
    serial_puts("[UDF] Mounted: \"");
    serial_puts(udf.volume_id);
    serial_puts("\" part@");
    serial_putdec(udf.part_start);
    serial_puts(" root=");
    serial_putdec(udf.root_block);
    serial_puts("\n");
    return 0;
}

bool udf_is_mounted(void) { return udf.mounted; }

static void udf_ls_cb(const char *name, uint32_t block,
                       uint32_t size, uint8_t ftype, void *ctx)
{
    (void)block; (void)ctx;
    serial_puts(ftype == 4 ? "  [DIR] " : "  ");
    serial_puts(name);
    if (ftype != 4) { serial_puts("  ("); serial_putdec(size); serial_puts(")"); }
    serial_puts("\n");
}

int udf_ls(void)
{
    if (!udf.mounted) return -1;
    /* Read root directory FE to get allocation info */
    uint8_t fe_buf[UDF_SECTOR_SIZE];
    if (udf_read_part_block(udf.root_block, fe_buf) < 0) return -1;

    udf_tag_t *tag = (udf_tag_t *)fe_buf;
    if (tag->tag_id != UDF_TAG_FE) return -1;

    uint32_t dir_len = *(uint32_t *)(fe_buf + 56);

    /* Get allocation descriptors (after extended attributes) */
    uint32_t ea_len = *(uint32_t *)(fe_buf + 168);
    uint32_t ad_off = 176 + ea_len;
    udf_short_ad_t *sad = (udf_short_ad_t *)(fe_buf + ad_off);
    uint32_t dir_block = sad->position;

    serial_puts("[UDF] Root directory:\n");
    udf_read_dir(dir_block, dir_len, udf_ls_cb, NULL);
    return 0;
}
