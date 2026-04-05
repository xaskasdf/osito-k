/*
 * OsitoK x86-64 — FAT32 Read-Only Driver
 *
 * Reads files from FAT32 partitions (USB sticks, existing disks).
 * Supports long filenames (LFN), subdirectories, cluster chains.
 * Read-only — no write, create, or delete operations.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern int nvme_read(uint64_t lba, uint32_t count, void *buf);
extern int nvme_is_ready(void);
extern uint32_t nvme_lba_size(void);

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* ── FAT32 On-Disk Structures ────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    char     name[11];            /* 8.3 format, space-padded */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  ctime_tenths;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
} fat32_dirent_t;

/* LFN entry (attr = 0x0F) */
typedef struct __attribute__((packed)) {
    uint8_t  order;          /* Sequence (0x40|N for last, N for others) */
    uint16_t name1[5];       /* Characters 1-5 */
    uint8_t  attr;           /* Always 0x0F */
    uint8_t  type;           /* 0 for LFN */
    uint8_t  checksum;
    uint16_t name2[6];       /* Characters 6-11 */
    uint16_t cluster_lo;     /* Always 0 */
    uint16_t name3[2];       /* Characters 12-13 */
} fat32_lfn_t;

/* Directory entry attributes */
#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME    0x08
#define FAT_ATTR_DIR       0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

/* FAT cluster values */
#define FAT32_EOC          0x0FFFFFF8  /* End of chain (>= this value) */
#define FAT32_FREE         0x00000000
#define FAT32_BAD          0x0FFFFFF7

/* ── FAT32 Mount State ───────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t part_lba;          /* Partition start LBA on disk */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;       /* bytes per cluster */
    uint32_t fat_lba;            /* First FAT start LBA (absolute) */
    uint32_t fat_sectors;        /* Sectors per FAT */
    uint32_t data_lba;           /* First data cluster start LBA */
    uint32_t root_cluster;       /* Root directory first cluster */
    uint32_t total_clusters;
} fat;

/* ── Sector I/O ──────────────────────────────────────────────── */

/* Read sectors from the FAT32 partition (LBA relative to partition start) */
static int fat32_read_sectors(uint64_t lba, uint32_t count, void *buf)
{
    return nvme_read(fat.part_lba + lba, count, buf);
}

/* ── FAT table lookup ────────────────────────────────────────── */

/* Read the next cluster in chain from the FAT table */
static uint32_t fat32_next_cluster(uint32_t cluster)
{
    /* Each FAT entry is 4 bytes. Compute which sector of the FAT contains it. */
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat.fat_lba + (fat_offset / fat.bytes_per_sector);
    uint32_t entry_offset = fat_offset % fat.bytes_per_sector;

    uint8_t buf[512];
    if (fat32_read_sectors(fat_sector - fat.part_lba, 1, buf) < 0)
        return FAT32_EOC;

    uint32_t val = *(uint32_t *)(buf + entry_offset);
    return val & 0x0FFFFFFF;  /* Mask upper 4 bits (reserved) */
}

/* ── Cluster → LBA conversion ────────────────────────────────── */

static uint64_t cluster_to_lba(uint32_t cluster)
{
    return fat.data_lba + (uint64_t)(cluster - 2) * fat.sectors_per_cluster;
}

/* ── LFN decoding ────────────────────────────────────────────── */

/* Extract UTF-16LE characters from LFN entry into ASCII name buffer */
static void lfn_extract(const fat32_lfn_t *lfn, char *name, int slot)
{
    int base = (slot - 1) * 13;  /* 13 chars per LFN entry */
    for (int i = 0; i < 5; i++) {
        uint16_t c = lfn->name1[i];
        if (c == 0 || c == 0xFFFF) return;
        name[base + i] = (c < 128) ? (char)c : '?';
    }
    for (int i = 0; i < 6; i++) {
        uint16_t c = lfn->name2[i];
        if (c == 0 || c == 0xFFFF) return;
        name[base + 5 + i] = (c < 128) ? (char)c : '?';
    }
    for (int i = 0; i < 2; i++) {
        uint16_t c = lfn->name3[i];
        if (c == 0 || c == 0xFFFF) return;
        name[base + 11 + i] = (c < 128) ? (char)c : '?';
    }
}

/* Decode 8.3 short name to readable string */
static void short_name_decode(const char *raw, char *out)
{
    int p = 0;
    /* Name (8 chars, right-trimmed) */
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[p++] = raw[i];
    /* Extension (3 chars) */
    if (raw[8] != ' ') {
        out[p++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[p++] = raw[i];
    }
    out[p] = '\0';
}

/* Case-insensitive comparison */
static int fat32_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ── Directory reading ───────────────────────────────────────── */

/* Callback for directory iteration */
typedef void (*fat32_dir_cb)(const char *name, uint32_t cluster,
                             uint32_t size, uint8_t attr, void *ctx);

/* Iterate entries in a directory cluster chain */
static void fat32_read_dir(uint32_t dir_cluster, fat32_dir_cb cb, void *ctx)
{
    uint8_t *buf = (uint8_t *)kmalloc(fat.cluster_size);
    if (!buf) return;

    char lfn_name[256];
    int lfn_slots = 0;
    memset(lfn_name, 0, sizeof(lfn_name));

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(cluster);
        if (fat32_read_sectors(lba - fat.part_lba, fat.sectors_per_cluster, buf) < 0)
            break;

        uint32_t entries = fat.cluster_size / 32;
        for (uint32_t i = 0; i < entries; i++) {
            fat32_dirent_t *d = (fat32_dirent_t *)(buf + i * 32);

            if (d->name[0] == 0x00) goto done;       /* End of directory */
            if ((uint8_t)d->name[0] == 0xE5) {       /* Deleted entry */
                lfn_slots = 0;
                continue;
            }

            if (d->attr == FAT_ATTR_LFN) {
                /* LFN entry */
                fat32_lfn_t *lfn = (fat32_lfn_t *)(buf + i * 32);
                int slot = lfn->order & 0x3F;
                if (lfn->order & 0x40) {
                    /* Last LFN entry — start fresh */
                    memset(lfn_name, 0, sizeof(lfn_name));
                    lfn_slots = slot;
                }
                lfn_extract(lfn, lfn_name, slot);
                continue;
            }

            /* Skip volume label and hidden entries */
            if (d->attr & FAT_ATTR_VOLUME) { lfn_slots = 0; continue; }

            /* Regular file or directory */
            uint32_t first_cluster = ((uint32_t)d->cluster_hi << 16) | d->cluster_lo;
            char name[256];
            if (lfn_slots > 0) {
                memcpy(name, lfn_name, sizeof(name));
            } else {
                short_name_decode(d->name, name);
            }
            lfn_slots = 0;

            cb(name, first_cluster, d->size, d->attr, ctx);
        }

        cluster = fat32_next_cluster(cluster);
    }

done:
    kfree(buf);
}

/* ── Public API: Mount ───────────────────────────────────────── */

int fat32_mount(uint64_t part_lba)
{
    serial_puts("[FAT32] Mounting partition at LBA ");
    serial_putdec(part_lba);
    serial_puts("...\n");

    /* Read BPB (first sector of partition) */
    uint8_t bpb_buf[512];
    if (nvme_read(part_lba, 1, bpb_buf) < 0) {
        serial_puts("[FAT32] Failed to read BPB\n");
        return -1;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)bpb_buf;

    /* Validate */
    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 4096) {
        serial_puts("[FAT32] Unsupported sector size\n");
        return -1;
    }
    if (bpb->fat_size_32 == 0) {
        serial_puts("[FAT32] Not a FAT32 volume (fat_size_32=0)\n");
        return -1;
    }

    fat.part_lba = part_lba;
    fat.bytes_per_sector = bpb->bytes_per_sector;
    fat.sectors_per_cluster = bpb->sectors_per_cluster;
    fat.cluster_size = fat.bytes_per_sector * fat.sectors_per_cluster;
    fat.fat_lba = part_lba + bpb->reserved_sectors;
    fat.fat_sectors = bpb->fat_size_32;
    fat.data_lba = fat.fat_lba + (uint64_t)bpb->num_fats * bpb->fat_size_32;
    fat.root_cluster = bpb->root_cluster;
    fat.total_clusters = (bpb->total_sectors_32 - (fat.data_lba - part_lba)) /
                         fat.sectors_per_cluster;
    fat.mounted = true;

    serial_puts("[FAT32] Mounted: cluster_size=");
    serial_putdec(fat.cluster_size);
    serial_puts(" root=");
    serial_putdec(fat.root_cluster);
    serial_puts(" clusters=");
    serial_putdec(fat.total_clusters);
    serial_puts("\n");

    fb_puts(" FAT32: mounted\n");
    return 0;
}

bool fat32_is_mounted(void) { return fat.mounted; }

/* ── Public API: List directory ──────────────────────────────── */

static void ls_callback(const char *name, uint32_t cluster,
                        uint32_t size, uint8_t attr, void *ctx)
{
    (void)cluster; (void)ctx;
    if (attr & FAT_ATTR_DIR) {
        serial_puts("  [DIR] ");
        fb_puts("  [DIR] ");
    } else {
        serial_puts("  ");
        fb_puts("  ");
    }
    serial_puts(name);
    fb_puts(name);
    if (!(attr & FAT_ATTR_DIR)) {
        serial_puts("  (");
        serial_putdec(size);
        serial_puts(" bytes)");
    }
    serial_puts("\n");
    fb_puts("\n");
}

int fat32_ls(const char *path)
{
    if (!fat.mounted) return -1;

    /* For now: NULL or "/" or "" = root directory */
    uint32_t dir_cluster = fat.root_cluster;
    (void)path;  /* TODO: subdirectory traversal */

    serial_puts("[FAT32] Directory listing:\n");
    fat32_read_dir(dir_cluster, ls_callback, NULL);
    return 0;
}

/* ── Public API: Find file ───────────────────────────────────── */

typedef struct {
    const char *target;
    uint32_t    found_cluster;
    uint32_t    found_size;
    bool        found;
} find_ctx_t;

static void find_callback(const char *name, uint32_t cluster,
                          uint32_t size, uint8_t attr, void *ctx)
{
    find_ctx_t *fc = (find_ctx_t *)ctx;
    (void)attr;
    if (!fc->found && fat32_stricmp(name, fc->target) == 0) {
        fc->found_cluster = cluster;
        fc->found_size = size;
        fc->found = true;
    }
}

int fat32_find(const char *name, uint32_t *cluster_out, uint32_t *size_out)
{
    if (!fat.mounted) return -1;

    find_ctx_t ctx = { .target = name, .found = false };
    fat32_read_dir(fat.root_cluster, find_callback, &ctx);

    if (!ctx.found) return -1;
    if (cluster_out) *cluster_out = ctx.found_cluster;
    if (size_out) *size_out = ctx.found_size;
    return 0;
}

/* ── Public API: Read file ───────────────────────────────────── */

int fat32_read_file(const char *name, uint64_t offset, void *buf, uint64_t len)
{
    if (!fat.mounted) return -1;

    uint32_t cluster, size;
    if (fat32_find(name, &cluster, &size) < 0)
        return -1;

    if (offset >= size) return 0;
    if (offset + len > size) len = size - offset;

    /* Skip clusters until we reach the offset */
    uint32_t skip_clusters = (uint32_t)(offset / fat.cluster_size);
    uint32_t cluster_offset = (uint32_t)(offset % fat.cluster_size);

    for (uint32_t i = 0; i < skip_clusters; i++) {
        cluster = fat32_next_cluster(cluster);
        if (cluster >= FAT32_EOC) return -1;
    }

    /* Read data cluster by cluster */
    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;
    uint8_t *cbuf = (uint8_t *)kmalloc(fat.cluster_size);
    if (!cbuf) return -1;

    while (remaining > 0 && cluster >= 2 && cluster < FAT32_EOC) {
        uint64_t lba = cluster_to_lba(cluster);
        if (fat32_read_sectors(lba - fat.part_lba, fat.sectors_per_cluster, cbuf) < 0)
            break;

        uint32_t avail = fat.cluster_size - cluster_offset;
        uint32_t copy = (remaining < avail) ? (uint32_t)remaining : avail;
        memcpy(dst, cbuf + cluster_offset, copy);

        dst += copy;
        remaining -= copy;
        cluster_offset = 0;  /* Only first cluster has offset */
        cluster = fat32_next_cluster(cluster);
    }

    kfree(cbuf);
    return (int)(len - remaining);
}
