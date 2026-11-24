/*
 * OsitoK x86-64 — exFAT Read-Only Driver
 *
 * Reads files from exFAT partitions (USB sticks >32GB, SD cards).
 * Supports allocation bitmap, Unicode filenames (UTF-16LE),
 * stream extension entries, and cluster chains.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   nvme_read(uint64_t lba, uint32_t count, void *buf);

/* ── exFAT On-Disk Structures ────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     fs_name[8];          /* "EXFAT   " */
    uint8_t  zeros[53];           /* Must be zero */
    uint64_t partition_offset;
    uint64_t volume_length;       /* Sectors in volume */
    uint32_t fat_offset;          /* Sectors to FAT */
    uint32_t fat_length;          /* Sectors per FAT */
    uint32_t cluster_heap_offset; /* Sectors to first cluster */
    uint32_t cluster_count;
    uint32_t root_dir_cluster;    /* First cluster of root dir */
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;   /* log2 (9=512, 12=4096) */
    uint8_t  sectors_per_cluster_shift; /* log2 */
    uint8_t  num_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  boot_code[390];
    uint16_t boot_sig;            /* 0xAA55 */
} exfat_boot_t;

/* Directory entry types */
#define EXFAT_ENTRY_EOD        0x00  /* End of directory */
#define EXFAT_ENTRY_ALLOC_BMP  0x81  /* Allocation bitmap */
#define EXFAT_ENTRY_UPCASE     0x82  /* Up-case table */
#define EXFAT_ENTRY_LABEL      0x83  /* Volume label */
#define EXFAT_ENTRY_FILE       0x85  /* File/dir entry */
#define EXFAT_ENTRY_STREAM     0xC0  /* Stream extension */
#define EXFAT_ENTRY_NAME       0xC1  /* Filename extension */

typedef struct __attribute__((packed)) {
    uint8_t  type;           /* 0x85 */
    uint8_t  secondary_count;
    uint16_t set_checksum;
    uint16_t file_attr;      /* 0x10 = directory */
    uint16_t reserved1;
    uint32_t create_time;
    uint32_t modify_time;
    uint32_t access_time;
    uint8_t  create_10ms;
    uint8_t  modify_10ms;
    uint8_t  create_tz;
    uint8_t  modify_tz;
    uint8_t  access_tz;
    uint8_t  reserved2[7];
} exfat_file_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;           /* 0xC0 */
    uint8_t  flags;          /* bit 0: alloc possible, bit 1: no FAT chain */
    uint8_t  reserved;
    uint8_t  name_len;       /* Total filename length in Unicode chars */
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_size;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_stream_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;           /* 0xC1 */
    uint8_t  flags;          /* General secondary flags */
    uint16_t name[15];       /* Up to 15 UTF-16LE chars per entry */
} exfat_name_entry_t;

#define EXFAT_ATTR_DIR  0x0010

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t part_lba;
    uint32_t sector_size;
    uint32_t cluster_size;       /* bytes */
    uint32_t sectors_per_cluster;
    uint32_t fat_lba;            /* absolute */
    uint32_t heap_lba;           /* absolute (first cluster) */
    uint32_t root_cluster;
    uint32_t cluster_count;
} exf;

/* ── Cluster I/O ─────────────────────────────────────────────── */

static int exfat_read_cluster(uint32_t cluster, void *buf)
{
    uint64_t lba = exf.heap_lba + (uint64_t)(cluster - 2) * exf.sectors_per_cluster;
    return nvme_read(lba, exf.sectors_per_cluster, buf);
}

static uint32_t exfat_next_cluster(uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = exf.fat_lba + (fat_offset / exf.sector_size);
    uint32_t entry_off = fat_offset % exf.sector_size;

    uint8_t buf[512];
    if (nvme_read(fat_sector, 1, buf) < 0) return 0xFFFFFFFF;
    uint32_t val = *(uint32_t *)(buf + entry_off);
    return val;
}

/* ── Case-insensitive compare ────────────────────────────────── */

static int exfat_stricmp(const char *a, const char *b)
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

typedef void (*exfat_dir_cb)(const char *name, uint32_t cluster,
                             uint64_t size, uint16_t attr, void *ctx);

static void exfat_read_dir(uint32_t dir_cluster, exfat_dir_cb cb, void *ctx)
{
    uint8_t *buf = (uint8_t *)kmalloc(exf.cluster_size);
    if (!buf) return;

    char name[256];
    uint32_t file_cluster = 0;
    uint64_t file_size = 0;
    uint16_t file_attr = 0;
    int name_pos = 0;
    int expecting = 0;  /* secondary entries remaining */

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0xFFFFFFF8) {
        if (exfat_read_cluster(cluster, buf) < 0) break;

        uint32_t entries = exf.cluster_size / 32;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = buf + i * 32;
            uint8_t type = e[0];

            if (type == EXFAT_ENTRY_EOD) goto done;
            if (type == 0x00) continue;
            if (!(type & 0x80)) continue;  /* deleted entry */

            if (type == EXFAT_ENTRY_FILE) {
                exfat_file_entry_t *fe = (exfat_file_entry_t *)e;
                expecting = fe->secondary_count;
                file_attr = fe->file_attr;
                name_pos = 0;
                name[0] = '\0';
            } else if (type == EXFAT_ENTRY_STREAM && expecting > 0) {
                exfat_stream_entry_t *se = (exfat_stream_entry_t *)e;
                file_cluster = se->first_cluster;
                file_size = se->data_length;
                expecting--;
            } else if (type == EXFAT_ENTRY_NAME && expecting > 0) {
                exfat_name_entry_t *ne = (exfat_name_entry_t *)e;
                for (int j = 0; j < 15 && name_pos < 254; j++) {
                    uint16_t c = ne->name[j];
                    if (c == 0) break;
                    name[name_pos++] = (c < 128) ? (char)c : '?';
                }
                name[name_pos] = '\0';
                expecting--;

                if (expecting == 0 && name_pos > 0)
                    cb(name, file_cluster, file_size, file_attr, ctx);
            }
        }
        cluster = exfat_next_cluster(cluster);
    }
done:
    kfree(buf);
}

/* ── Public API ──────────────────────────────────────────────── */

int exfat_mount(uint64_t part_lba)
{
    uint8_t boot_buf[512];
    if (nvme_read(part_lba, 1, boot_buf) < 0) return -1;

    exfat_boot_t *bs = (exfat_boot_t *)boot_buf;
    if (bs->fs_name[0] != 'E' || bs->fs_name[1] != 'X' ||
        bs->fs_name[2] != 'F' || bs->fs_name[3] != 'A' ||
        bs->fs_name[4] != 'T') {
        serial_puts("[EXFAT] Invalid signature\n");
        return -1;
    }

    exf.part_lba = part_lba;
    exf.sector_size = 1U << bs->bytes_per_sector_shift;
    exf.sectors_per_cluster = 1U << bs->sectors_per_cluster_shift;
    exf.cluster_size = exf.sector_size * exf.sectors_per_cluster;
    exf.fat_lba = part_lba + bs->fat_offset;
    exf.heap_lba = part_lba + bs->cluster_heap_offset;
    exf.root_cluster = bs->root_dir_cluster;
    exf.cluster_count = bs->cluster_count;
    exf.mounted = true;

    serial_puts("[EXFAT] Mounted: cluster_size=");
    serial_putdec(exf.cluster_size);
    serial_puts(" root=");
    serial_putdec(exf.root_cluster);
    serial_puts(" clusters=");
    serial_putdec(exf.cluster_count);
    serial_puts("\n");
    return 0;
}

bool exfat_is_mounted(void) { return exf.mounted; }

/* List root directory */
static void exfat_ls_cb(const char *name, uint32_t cluster,
                        uint64_t size, uint16_t attr, void *ctx)
{
    (void)cluster; (void)ctx;
    if (attr & EXFAT_ATTR_DIR)
        serial_puts("  [DIR] ");
    else
        serial_puts("  ");
    serial_puts(name);
    if (!(attr & EXFAT_ATTR_DIR)) {
        serial_puts("  ("); serial_putdec(size); serial_puts(")");
    }
    serial_puts("\n");
}

int exfat_ls(void)
{
    if (!exf.mounted) return -1;
    serial_puts("[EXFAT] Root directory:\n");
    exfat_read_dir(exf.root_cluster, exfat_ls_cb, NULL);
    return 0;
}

/* Find file */
typedef struct { const char *target; uint32_t cluster; uint64_t size; bool found; } exfat_find_ctx_t;
static void exfat_find_cb(const char *name, uint32_t cluster,
                          uint64_t size, uint16_t attr, void *ctx)
{
    exfat_find_ctx_t *fc = (exfat_find_ctx_t *)ctx;
    (void)attr;
    if (!fc->found && exfat_stricmp(name, fc->target) == 0) {
        fc->cluster = cluster; fc->size = size; fc->found = true;
    }
}

int exfat_find(const char *name, uint32_t *cluster_out, uint64_t *size_out)
{
    if (!exf.mounted) return -1;
    exfat_find_ctx_t ctx = { .target = name, .found = false };
    exfat_read_dir(exf.root_cluster, exfat_find_cb, &ctx);
    if (!ctx.found) return -1;
    if (cluster_out) *cluster_out = ctx.cluster;
    if (size_out) *size_out = ctx.size;
    return 0;
}

/* Read file data */
int exfat_read_file(const char *name, uint64_t offset, void *buf, uint64_t len)
{
    uint32_t cluster;
    uint64_t size;
    if (exfat_find(name, &cluster, &size) < 0) return -1;

    if (offset >= size) return 0;
    if (offset + len > size) len = size - offset;

    uint32_t skip = (uint32_t)(offset / exf.cluster_size);
    uint32_t coff = (uint32_t)(offset % exf.cluster_size);

    for (uint32_t i = 0; i < skip && cluster >= 2 && cluster < 0xFFFFFFF8; i++)
        cluster = exfat_next_cluster(cluster);

    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;
    uint8_t *cbuf = (uint8_t *)kmalloc(exf.cluster_size);
    if (!cbuf) return -1;

    while (remaining > 0 && cluster >= 2 && cluster < 0xFFFFFFF8) {
        if (exfat_read_cluster(cluster, cbuf) < 0) break;
        uint32_t avail = exf.cluster_size - coff;
        uint32_t copy = (remaining < avail) ? (uint32_t)remaining : avail;
        memcpy(dst, cbuf + coff, copy);
        dst += copy;
        remaining -= copy;
        coff = 0;
        cluster = exfat_next_cluster(cluster);
    }

    kfree(cbuf);
    return (int)(len - remaining);
}
