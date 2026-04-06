/*
 * OsitoK x86-64 — NTFS Read-Only Driver
 *
 * Reads files from NTFS partitions (Windows disks).
 * Supports MFT parsing, resident/non-resident attributes,
 * run list decoding, and B+ tree directory index.
 * No compression, encryption, or sparse file support.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   nvme_read(uint64_t lba, uint32_t count, void *buf);

/* ── NTFS Boot Sector ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];              /* "NTFS    " */
    uint16_t bytes_per_sector;    /* Usually 512 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;    /* 0 for NTFS */
    uint8_t  unused1[5];
    uint8_t  media;
    uint16_t unused2;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t unused3;
    uint32_t unused4;
    uint64_t total_sectors;
    uint64_t mft_lcn;             /* MFT start cluster */
    uint64_t mft_mirror_lcn;
    int8_t   clusters_per_mft;    /* Negative = 2^abs(val) bytes */
    uint8_t  unused5[3];
    int8_t   clusters_per_index;
    uint8_t  unused6[3];
    uint64_t volume_serial;
    uint32_t checksum;
} ntfs_boot_t;

/* ── MFT Entry ───────────────────────────────────────────────── */

#define NTFS_MFT_MAGIC  0x454C4946  /* "FILE" */

typedef struct __attribute__((packed)) {
    uint32_t magic;               /* "FILE" */
    uint16_t update_seq_offset;
    uint16_t update_seq_count;
    uint64_t lsn;
    uint16_t seq_number;
    uint16_t hard_link_count;
    uint16_t first_attr_offset;
    uint16_t flags;               /* 0x01=in_use, 0x02=directory */
    uint32_t used_size;
    uint32_t alloc_size;
    uint64_t base_record;
    uint16_t next_attr_id;
} ntfs_mft_entry_t;

#define NTFS_MFT_FLAG_INUSE  0x01
#define NTFS_MFT_FLAG_DIR    0x02

/* ── Attributes ──────────────────────────────────────────────── */

#define NTFS_ATTR_FILENAME      0x30
#define NTFS_ATTR_DATA          0x80
#define NTFS_ATTR_INDEX_ROOT    0x90
#define NTFS_ATTR_INDEX_ALLOC   0xA0
#define NTFS_ATTR_END           0xFFFFFFFF

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t instance;
    /* Resident: */
    /* uint32_t value_length; uint16_t value_offset; */
    /* Non-resident: */
    /* uint64_t start_vcn, end_vcn; uint16_t run_offset; ... */
} ntfs_attr_header_t;

/* Resident attribute (follows header) */
typedef struct __attribute__((packed)) {
    uint32_t value_length;
    uint16_t value_offset;
    uint16_t flags;
} ntfs_attr_resident_t;

/* Non-resident attribute (follows header) */
typedef struct __attribute__((packed)) {
    uint64_t start_vcn;
    uint64_t end_vcn;
    uint16_t run_offset;
    uint16_t compression_unit;
    uint32_t padding;
    uint64_t alloc_size;
    uint64_t real_size;
    uint64_t init_size;
} ntfs_attr_nonresident_t;

/* $FILE_NAME attribute content */
typedef struct __attribute__((packed)) {
    uint64_t parent_ref;
    uint64_t created;
    uint64_t modified;
    uint64_t mft_modified;
    uint64_t accessed;
    uint64_t alloc_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t  name_length;         /* In UTF-16LE chars */
    uint8_t  namespace;           /* 0=POSIX, 1=Win32, 2=DOS, 3=Win32+DOS */
    uint16_t name[];              /* UTF-16LE filename */
} ntfs_filename_attr_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t part_lba;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t sectors_per_cluster;
    uint32_t mft_entry_size;
    uint64_t mft_lcn;            /* MFT start cluster */
    uint8_t *mft_buf;            /* Buffer for one MFT entry */
} nt;

/* ── Cluster I/O ─────────────────────────────────────────────── */

static int ntfs_read_clusters(uint64_t lcn, uint32_t count, void *buf)
{
    uint64_t lba = nt.part_lba + lcn * nt.sectors_per_cluster;
    return nvme_read(lba, count * nt.sectors_per_cluster, buf);
}

/* ── MFT Entry Reading ───────────────────────────────────────── */

static int ntfs_read_mft(uint64_t mft_num, void *buf)
{
    /* MFT entries are sequential starting at mft_lcn */
    uint64_t byte_offset = mft_num * nt.mft_entry_size;
    uint64_t cluster = nt.mft_lcn + byte_offset / nt.cluster_size;
    uint32_t cluster_off = (uint32_t)(byte_offset % nt.cluster_size);

    uint8_t *cbuf = (uint8_t *)kmalloc(nt.cluster_size);
    if (!cbuf) return -1;

    if (ntfs_read_clusters(cluster, 1, cbuf) < 0) {
        kfree(cbuf);
        return -1;
    }

    memcpy(buf, cbuf + cluster_off, nt.mft_entry_size);
    kfree(cbuf);

    /* Apply fixup (update sequence) */
    ntfs_mft_entry_t *entry = (ntfs_mft_entry_t *)buf;
    if (entry->magic != NTFS_MFT_MAGIC) return -1;

    uint16_t *usa = (uint16_t *)((uint8_t *)buf + entry->update_seq_offset);
    uint16_t usa_val = usa[0];
    for (int i = 1; i < entry->update_seq_count; i++) {
        uint16_t *fixup = (uint16_t *)((uint8_t *)buf + i * nt.sector_size - 2);
        if (*fixup != usa_val) return -1;  /* Fixup mismatch */
        *fixup = usa[i];
    }

    return 0;
}

/* ── Attribute Iteration ─────────────────────────────────────── */

/* Find an attribute by type in an MFT entry. Returns pointer or NULL. */
static ntfs_attr_header_t *ntfs_find_attr(void *mft_buf, uint32_t attr_type)
{
    ntfs_mft_entry_t *entry = (ntfs_mft_entry_t *)mft_buf;
    uint8_t *p = (uint8_t *)mft_buf + entry->first_attr_offset;
    uint8_t *end = (uint8_t *)mft_buf + entry->used_size;

    while (p + 4 <= end) {
        ntfs_attr_header_t *attr = (ntfs_attr_header_t *)p;
        if (attr->type == NTFS_ATTR_END || attr->length == 0) break;
        if (attr->type == attr_type) return attr;
        p += attr->length;
    }
    return NULL;
}

/* Get resident attribute value pointer and length */
static const uint8_t *ntfs_attr_value(ntfs_attr_header_t *attr, uint32_t *len_out)
{
    if (attr->non_resident) return NULL;
    ntfs_attr_resident_t *res = (ntfs_attr_resident_t *)((uint8_t *)attr + 16);
    if (len_out) *len_out = res->value_length;
    return (const uint8_t *)attr + res->value_offset;
}

/* ── Run List Decoding ───────────────────────────────────────── */

/* Decode a non-resident data run list into (LCN, length) pairs.
 * Run list format: header byte = (offset_bytes << 4 | length_bytes),
 * followed by length (LE) then offset (LE, signed delta from previous). */

#define MAX_RUNS 64

typedef struct {
    uint64_t lcn;
    uint64_t length;  /* in clusters */
} ntfs_run_t;

static int ntfs_decode_runs(ntfs_attr_header_t *attr,
                            ntfs_run_t *runs, int max_runs)
{
    if (!attr->non_resident) return 0;

    ntfs_attr_nonresident_t *nr = (ntfs_attr_nonresident_t *)((uint8_t *)attr + 16);
    uint8_t *p = (uint8_t *)attr + nr->run_offset;
    uint8_t *end = (uint8_t *)attr + attr->length;

    int count = 0;
    int64_t prev_lcn = 0;

    while (p < end && *p != 0 && count < max_runs) {
        uint8_t header = *p++;
        int len_bytes = header & 0x0F;
        int off_bytes = (header >> 4) & 0x0F;

        if (len_bytes == 0) break;
        if (p + len_bytes + off_bytes > end) break;

        /* Read length (unsigned) */
        uint64_t length = 0;
        for (int i = 0; i < len_bytes; i++)
            length |= (uint64_t)*p++ << (i * 8);

        /* Read offset (signed, delta from previous) */
        int64_t offset = 0;
        if (off_bytes > 0) {
            for (int i = 0; i < off_bytes; i++)
                offset |= (int64_t)*p++ << (i * 8);
            /* Sign-extend */
            if (offset & (1LL << (off_bytes * 8 - 1)))
                offset |= ~0ULL << (off_bytes * 8);
        }

        prev_lcn += offset;
        runs[count].lcn = (uint64_t)prev_lcn;
        runs[count].length = length;
        count++;
    }

    return count;
}

/* ── Directory Reading (via $INDEX_ROOT) ─────────────────────── */

typedef void (*ntfs_dir_cb)(const char *name, uint64_t mft_ref,
                            uint64_t size, bool is_dir, void *ctx);

static void ntfs_read_dir(uint64_t dir_mft, ntfs_dir_cb cb, void *ctx)
{
    uint8_t *mft_buf = (uint8_t *)kmalloc(nt.mft_entry_size);
    if (!mft_buf) return;

    if (ntfs_read_mft(dir_mft, mft_buf) < 0) {
        kfree(mft_buf);
        return;
    }

    /* Find $INDEX_ROOT attribute (resident B+ tree root) */
    ntfs_attr_header_t *idx = ntfs_find_attr(mft_buf, NTFS_ATTR_INDEX_ROOT);
    if (!idx || idx->non_resident) {
        kfree(mft_buf);
        return;
    }

    uint32_t val_len;
    const uint8_t *val = ntfs_attr_value(idx, &val_len);
    if (!val || val_len < 32) { kfree(mft_buf); return; }

    /* Index root header: type(4) + collation(4) + alloc_size(4) +
     * clusters_per_index(1) + pad(3) = 16 bytes.
     * Then index node header: offset(4) + total(4) + alloc(4) + flags(1) = 13 bytes.
     * Index entries start at offset 16 + index_node.offset */
    uint32_t node_offset = *(uint32_t *)(val + 16);  /* offset to first entry */
    const uint8_t *entries = val + 16 + node_offset;
    const uint8_t *entries_end = val + val_len;

    while (entries + 16 <= entries_end) {
        uint64_t mft_ref = *(uint64_t *)entries & 0x0000FFFFFFFFFFFF;
        uint16_t entry_len = *(uint16_t *)(entries + 8);
        uint16_t stream_len = *(uint16_t *)(entries + 10);
        uint32_t entry_flags = *(uint32_t *)(entries + 12);

        if (entry_len == 0 || entry_len > 4096) break;
        if (entry_flags & 2) break;  /* Last entry (no more) */

        if (stream_len >= sizeof(ntfs_filename_attr_t) && mft_ref > 0) {
            const ntfs_filename_attr_t *fn =
                (const ntfs_filename_attr_t *)(entries + 16);
            /* Only show Win32 or Win32+DOS names (skip DOS-only short names) */
            if (fn->namespace != 2) {
                char name[256];
                int nlen = fn->name_length < 255 ? fn->name_length : 255;
                for (int i = 0; i < nlen; i++)
                    name[i] = (fn->name[i] < 128) ? (char)fn->name[i] : '?';
                name[nlen] = '\0';

                bool is_dir = (fn->flags & 0x10000000) != 0;
                cb(name, mft_ref, fn->real_size, is_dir, ctx);
            }
        }

        entries += entry_len;
    }

    kfree(mft_buf);
}

/* ── Public API ──────────────────────────────────────────────── */

int ntfs_mount(uint64_t part_lba)
{
    uint8_t boot_buf[512];
    if (nvme_read(part_lba, 1, boot_buf) < 0) return -1;

    ntfs_boot_t *bs = (ntfs_boot_t *)boot_buf;
    if (bs->oem[0] != 'N' || bs->oem[1] != 'T' ||
        bs->oem[2] != 'F' || bs->oem[3] != 'S') {
        serial_puts("[NTFS] Invalid OEM signature\n");
        return -1;
    }

    nt.part_lba = part_lba;
    nt.sector_size = bs->bytes_per_sector;
    nt.sectors_per_cluster = bs->sectors_per_cluster;
    nt.cluster_size = nt.sector_size * nt.sectors_per_cluster;
    nt.mft_lcn = bs->mft_lcn;

    /* MFT entry size: if clusters_per_mft < 0, size = 2^abs(val) */
    if (bs->clusters_per_mft < 0)
        nt.mft_entry_size = 1U << (uint32_t)(-bs->clusters_per_mft);
    else
        nt.mft_entry_size = (uint32_t)bs->clusters_per_mft * nt.cluster_size;

    nt.mft_buf = (uint8_t *)kmalloc(nt.mft_entry_size);
    if (!nt.mft_buf) return -1;

    nt.mounted = true;

    serial_puts("[NTFS] Mounted: cluster_size=");
    serial_putdec(nt.cluster_size);
    serial_puts(" mft_entry=");
    serial_putdec(nt.mft_entry_size);
    serial_puts(" mft@LCN ");
    serial_putdec(nt.mft_lcn);
    serial_puts("\n");
    return 0;
}

bool ntfs_is_mounted(void) { return nt.mounted; }

/* List root directory (MFT entry 5 = root) */
static void ntfs_ls_cb(const char *name, uint64_t mft_ref,
                       uint64_t size, bool is_dir, void *ctx)
{
    (void)mft_ref; (void)ctx;
    if (is_dir)
        serial_puts("  [DIR] ");
    else
        serial_puts("  ");
    serial_puts(name);
    if (!is_dir) {
        serial_puts("  ("); serial_putdec(size); serial_puts(")");
    }
    serial_puts("\n");
}

int ntfs_ls(void)
{
    if (!nt.mounted) return -1;
    serial_puts("[NTFS] Root directory:\n");
    ntfs_read_dir(5, ntfs_ls_cb, NULL);  /* MFT entry 5 = root dir */
    return 0;
}

/* Find file in root */
typedef struct {
    const char *target;
    uint64_t    mft_ref;
    uint64_t    size;
    bool        found;
} ntfs_find_ctx_t;

static int ntfs_stricmp(const char *a, const char *b)
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

static void ntfs_find_cb(const char *name, uint64_t mft_ref,
                         uint64_t size, bool is_dir, void *ctx)
{
    ntfs_find_ctx_t *fc = (ntfs_find_ctx_t *)ctx;
    (void)is_dir;
    if (!fc->found && ntfs_stricmp(name, fc->target) == 0) {
        fc->mft_ref = mft_ref;
        fc->size = size;
        fc->found = true;
    }
}

int ntfs_find(const char *name, uint64_t *mft_out, uint64_t *size_out)
{
    if (!nt.mounted) return -1;
    ntfs_find_ctx_t ctx = { .target = name, .found = false };
    ntfs_read_dir(5, ntfs_find_cb, &ctx);
    if (!ctx.found) return -1;
    if (mft_out) *mft_out = ctx.mft_ref;
    if (size_out) *size_out = ctx.size;
    return 0;
}

/* Read file data via $DATA attribute run list */
int ntfs_read_file(const char *name, uint64_t offset, void *buf, uint64_t len)
{
    uint64_t mft_ref, file_size;
    if (ntfs_find(name, &mft_ref, &file_size) < 0) return -1;

    if (offset >= file_size) return 0;
    if (offset + len > file_size) len = file_size - offset;

    /* Read file's MFT entry */
    uint8_t *mft_buf = (uint8_t *)kmalloc(nt.mft_entry_size);
    if (!mft_buf) return -1;

    if (ntfs_read_mft(mft_ref, mft_buf) < 0) {
        kfree(mft_buf);
        return -1;
    }

    /* Find $DATA attribute */
    ntfs_attr_header_t *data_attr = ntfs_find_attr(mft_buf, NTFS_ATTR_DATA);
    if (!data_attr) { kfree(mft_buf); return -1; }

    if (!data_attr->non_resident) {
        /* Resident $DATA: small file stored inline in MFT */
        uint32_t val_len;
        const uint8_t *val = ntfs_attr_value(data_attr, &val_len);
        if (!val) { kfree(mft_buf); return -1; }
        if (offset >= val_len) { kfree(mft_buf); return 0; }
        if (offset + len > val_len) len = val_len - offset;
        memcpy(buf, val + offset, len);
        kfree(mft_buf);
        return (int)len;
    }

    /* Non-resident $DATA: decode run list and read clusters */
    ntfs_run_t runs[MAX_RUNS];
    int nruns = ntfs_decode_runs(data_attr, runs, MAX_RUNS);
    kfree(mft_buf);

    if (nruns <= 0) return -1;

    /* Walk runs to find offset and read data */
    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;
    uint64_t file_pos = 0;

    uint8_t *cbuf = (uint8_t *)kmalloc(nt.cluster_size);
    if (!cbuf) return -1;

    for (int r = 0; r < nruns && remaining > 0; r++) {
        uint64_t run_bytes = runs[r].length * nt.cluster_size;

        if (file_pos + run_bytes <= offset) {
            file_pos += run_bytes;
            continue;  /* Skip this run entirely */
        }

        /* Read clusters from this run */
        for (uint64_t c = 0; c < runs[r].length && remaining > 0; c++) {
            uint64_t cluster_start = file_pos + c * nt.cluster_size;
            uint64_t cluster_end = cluster_start + nt.cluster_size;

            if (cluster_end <= offset) continue;
            if (cluster_start >= offset + len) break;

            if (ntfs_read_clusters(runs[r].lcn + c, 1, cbuf) < 0) break;

            uint32_t coff = 0;
            if (cluster_start < offset) coff = (uint32_t)(offset - cluster_start);
            uint32_t avail = nt.cluster_size - coff;
            uint32_t copy = (remaining < avail) ? (uint32_t)remaining : avail;

            memcpy(dst, cbuf + coff, copy);
            dst += copy;
            remaining -= copy;
        }
        file_pos += run_bytes;
    }

    kfree(cbuf);
    return (int)(len - remaining);
}
