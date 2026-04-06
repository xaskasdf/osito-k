/*
 * OsitoK x86-64 — ISO 9660 Read-Only Driver
 *
 * Reads files from CD-ROM/DVD images (.iso).
 * Supports primary volume descriptor, directory traversal,
 * and file reading. No Joliet/Rock Ridge extensions (basic ISO).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Block device read — caller sets which device */
static int (*iso_read_sectors)(uint64_t lba, uint32_t count, void *buf);

/* ── ISO 9660 On-Disk Structures ─────────────────────────────── */

#define ISO_SECTOR_SIZE 2048
#define ISO_PVD_LBA     16   /* Primary Volume Descriptor at LBA 16 */

/* Volume Descriptor types */
#define ISO_VD_PRIMARY   1
#define ISO_VD_TERM      255

/* Both-endian 32-bit (stored as LE + BE) */
typedef struct __attribute__((packed)) {
    uint32_t le;
    uint32_t be;
} iso_733_t;

/* Both-endian 16-bit */
typedef struct __attribute__((packed)) {
    uint16_t le;
    uint16_t be;
} iso_723_t;

/* Date/time (7 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t year;    /* Years since 1900 */
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    int8_t  gmt_off; /* 15min intervals from GMT */
} iso_dir_date_t;

/* Directory record */
typedef struct __attribute__((packed)) {
    uint8_t       length;         /* Total length of this record */
    uint8_t       ext_attr_len;
    iso_733_t     extent;         /* LBA of file data */
    iso_733_t     size;           /* File size in bytes */
    iso_dir_date_t date;
    uint8_t       flags;          /* Bit 1 = directory */
    uint8_t       unit_size;
    uint8_t       interleave;
    iso_723_t     vol_seq;
    uint8_t       name_len;
    char          name[];         /* Variable length */
} iso_dir_record_t;

#define ISO_FLAG_DIR  0x02

/* Primary Volume Descriptor (subset of fields we need) */
typedef struct __attribute__((packed)) {
    uint8_t   type;               /* 1 = Primary */
    char      id[5];              /* "CD001" */
    uint8_t   version;
    uint8_t   unused1;
    char      system_id[32];
    char      volume_id[32];
    uint8_t   unused2[8];
    iso_733_t vol_space_size;     /* Volume size in logical blocks */
    uint8_t   unused3[32];
    iso_723_t vol_set_size;
    iso_723_t vol_seq_number;
    iso_723_t logical_block_size;
    iso_733_t path_table_size;
    uint32_t  path_table_le;
    uint32_t  opt_path_table_le;
    uint32_t  path_table_be;
    uint32_t  opt_path_table_be;
    uint8_t   root_dir_record[34]; /* Directory record for root */
} iso_pvd_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint32_t root_lba;    /* Root directory extent LBA */
    uint32_t root_size;   /* Root directory size in bytes */
    uint16_t block_size;  /* Logical block size (usually 2048) */
    char     volume_id[33];
} iso;

/* ── Directory reading ───────────────────────────────────────── */

typedef void (*iso_dir_cb)(const char *name, uint32_t lba,
                           uint32_t size, uint8_t flags, void *ctx);

static void iso_read_dir(uint32_t dir_lba, uint32_t dir_size,
                         iso_dir_cb cb, void *ctx)
{
    uint32_t sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    uint8_t *buf = (uint8_t *)kmalloc((uint64_t)sectors * ISO_SECTOR_SIZE);
    if (!buf) return;

    if (iso_read_sectors(dir_lba, sectors, buf) < 0) {
        kfree(buf);
        return;
    }

    uint32_t offset = 0;
    while (offset < dir_size) {
        iso_dir_record_t *rec = (iso_dir_record_t *)(buf + offset);
        if (rec->length == 0) {
            /* Skip to next sector boundary */
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        /* Extract name (strip ";1" version suffix) */
        char name[128];
        int nlen = rec->name_len;
        if (nlen > 127) nlen = 127;
        memcpy(name, rec->name, nlen);
        name[nlen] = '\0';

        /* Remove ";1" version */
        for (int i = nlen - 1; i >= 0; i--) {
            if (name[i] == ';') { name[i] = '\0'; break; }
        }
        /* Remove trailing '.' from directories */
        nlen = 0;
        while (name[nlen]) nlen++;
        if (nlen > 0 && name[nlen - 1] == '.') name[nlen - 1] = '\0';

        /* Skip "." and ".." entries */
        if (rec->name_len == 1 && (rec->name[0] == 0 || rec->name[0] == 1)) {
            offset += rec->length;
            continue;
        }

        cb(name, rec->extent.le, rec->size.le, rec->flags, ctx);
        offset += rec->length;
    }

    kfree(buf);
}

/* ── Case-insensitive comparison ─────────────────────────────── */

static int iso_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ── Public API: Mount ───────────────────────────────────────── */

int iso9660_mount(int (*read_fn)(uint64_t lba, uint32_t count, void *buf))
{
    iso_read_sectors = read_fn;

    /* Read Primary Volume Descriptor at LBA 16 */
    uint8_t pvd_buf[ISO_SECTOR_SIZE];
    if (iso_read_sectors(ISO_PVD_LBA, 1, pvd_buf) < 0) {
        serial_puts("[ISO9660] Failed to read PVD\n");
        return -1;
    }

    iso_pvd_t *pvd = (iso_pvd_t *)pvd_buf;

    /* Validate */
    if (pvd->type != ISO_VD_PRIMARY ||
        pvd->id[0] != 'C' || pvd->id[1] != 'D' ||
        pvd->id[2] != '0' || pvd->id[3] != '0' || pvd->id[4] != '1') {
        serial_puts("[ISO9660] Invalid PVD (no CD001 signature)\n");
        return -1;
    }

    /* Extract root directory record from PVD */
    iso_dir_record_t *root = (iso_dir_record_t *)pvd->root_dir_record;
    iso.root_lba  = root->extent.le;
    iso.root_size = root->size.le;
    iso.block_size = pvd->logical_block_size.le;

    /* Volume ID (space-padded, 32 chars) */
    memcpy(iso.volume_id, pvd->volume_id, 32);
    iso.volume_id[32] = '\0';
    /* Trim trailing spaces */
    for (int i = 31; i >= 0 && iso.volume_id[i] == ' '; i--)
        iso.volume_id[i] = '\0';

    iso.mounted = true;

    serial_puts("[ISO9660] Mounted: \"");
    serial_puts(iso.volume_id);
    serial_puts("\" root=LBA ");
    serial_putdec(iso.root_lba);
    serial_puts(" (");
    serial_putdec(iso.root_size);
    serial_puts(" bytes)\n");

    return 0;
}

bool iso9660_is_mounted(void) { return iso.mounted; }

/* ── Public API: List root directory ─────────────────────────── */

static void iso_ls_cb(const char *name, uint32_t lba,
                      uint32_t size, uint8_t flags, void *ctx)
{
    (void)lba; (void)ctx;
    if (flags & ISO_FLAG_DIR)
        serial_puts("  [DIR] ");
    else
        serial_puts("  ");
    serial_puts(name);
    if (!(flags & ISO_FLAG_DIR)) {
        serial_puts("  ("); serial_putdec(size); serial_puts(")");
    }
    serial_puts("\n");
}

int iso9660_ls(const char *path)
{
    if (!iso.mounted) return -1;
    (void)path;
    serial_puts("[ISO9660] Directory listing:\n");
    iso_read_dir(iso.root_lba, iso.root_size, iso_ls_cb, NULL);
    return 0;
}

/* ── Public API: Find file ───────────────────────────────────── */

typedef struct {
    const char *target;
    uint32_t lba, size;
    bool found;
} iso_find_ctx_t;

static void iso_find_cb(const char *name, uint32_t lba,
                        uint32_t size, uint8_t flags, void *ctx)
{
    iso_find_ctx_t *fc = (iso_find_ctx_t *)ctx;
    (void)flags;
    if (!fc->found && iso_stricmp(name, fc->target) == 0) {
        fc->lba = lba;
        fc->size = size;
        fc->found = true;
    }
}

int iso9660_find(const char *name, uint32_t *lba_out, uint32_t *size_out)
{
    if (!iso.mounted) return -1;
    iso_find_ctx_t ctx = { .target = name, .found = false };
    iso_read_dir(iso.root_lba, iso.root_size, iso_find_cb, &ctx);
    if (!ctx.found) return -1;
    if (lba_out) *lba_out = ctx.lba;
    if (size_out) *size_out = ctx.size;
    return 0;
}

/* ── Public API: Read file ───────────────────────────────────── */

int iso9660_read_file(const char *name, uint64_t offset, void *buf, uint64_t len)
{
    uint32_t lba, size;
    if (iso9660_find(name, &lba, &size) < 0) return -1;

    if (offset >= size) return 0;
    if (offset + len > size) len = size - offset;

    /* ISO files are stored contiguously — simple sector read */
    uint32_t start_sector = (uint32_t)(offset / ISO_SECTOR_SIZE);
    uint32_t sector_offset = (uint32_t)(offset % ISO_SECTOR_SIZE);
    uint32_t total_sectors = (uint32_t)((sector_offset + len + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE);

    uint8_t *tmp = (uint8_t *)kmalloc((uint64_t)total_sectors * ISO_SECTOR_SIZE);
    if (!tmp) return -1;

    if (iso_read_sectors(lba + start_sector, total_sectors, tmp) < 0) {
        kfree(tmp);
        return -1;
    }

    memcpy(buf, tmp + sector_offset, len);
    kfree(tmp);
    return (int)len;
}
