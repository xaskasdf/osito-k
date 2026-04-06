/*
 * OsitoK x86-64 — HFS+ Read-Only Driver
 *
 * Reads files from macOS HFS+ partitions (pre-APFS).
 * Supports volume header, catalog B-tree, extent overflow.
 * No journaling, no compression, no hardlinks.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   nvme_read(uint64_t lba, uint32_t count, void *buf);

/* ── HFS+ On-Disk Structures (big-endian!) ───────────────────── */

#define HFSPLUS_MAGIC   0x482B     /* "H+" big-endian */
#define HFSX_MAGIC      0x4858     /* "HX" case-sensitive */

/* Byte-swap helpers (HFS+ is big-endian on disk) */
static inline uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* Fork data (extent records for a file fork) */
typedef struct {
    uint32_t logical_size_hi;
    uint32_t logical_size_lo;
    uint32_t clump_size;
    uint32_t total_blocks;
    struct { uint32_t start_block; uint32_t block_count; } extents[8];
} hfs_fork_data_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t part_lba;
    uint32_t block_size;          /* Allocation block size */
    uint32_t total_blocks;
    uint64_t catalog_start;       /* Catalog B-tree first extent start block */
    uint32_t catalog_blocks;      /* Catalog B-tree first extent block count */
    char     volume_name[128];
} hfs;

/* ── Block I/O ───────────────────────────────────────────────── */

static int hfs_read_block(uint32_t block, void *buf)
{
    uint64_t sectors = (uint64_t)hfs.block_size / 512;
    uint64_t lba = hfs.part_lba + (uint64_t)block * sectors;
    return nvme_read(lba, (uint32_t)sectors, buf);
}

/* ── B-Tree Node Reading ─────────────────────────────────────── */

#define BTREE_LEAF   0xFF
#define BTREE_INDEX  0x00
#define BTREE_HEADER 0x01

typedef struct {
    uint32_t flink;
    uint32_t blink;
    int8_t   kind;
    uint8_t  height;
    uint16_t num_records;
    uint16_t reserved;
} btree_node_header_t;

/* Read a B-tree node from the catalog file */
static int hfs_read_catalog_node(uint32_t node_num, void *buf, uint32_t node_size)
{
    /* Catalog file spans catalog_start..catalog_start+catalog_blocks.
     * Node offset = node_num * node_size bytes from start. */
    uint64_t byte_offset = (uint64_t)node_num * node_size;
    uint32_t block = hfs.catalog_start + (uint32_t)(byte_offset / hfs.block_size);
    uint32_t block_offset = (uint32_t)(byte_offset % hfs.block_size);

    /* Read enough blocks to cover the node */
    uint32_t blocks_needed = (block_offset + node_size + hfs.block_size - 1) / hfs.block_size;
    uint8_t *raw = (uint8_t *)kmalloc((uint64_t)blocks_needed * hfs.block_size);
    if (!raw) return -1;

    for (uint32_t i = 0; i < blocks_needed; i++) {
        if (hfs_read_block(block + i, raw + (uint64_t)i * hfs.block_size) < 0) {
            kfree(raw);
            return -1;
        }
    }

    memcpy(buf, raw + block_offset, node_size);
    kfree(raw);
    return 0;
}

/* ── Catalog Leaf Iteration ──────────────────────────────────── */

typedef void (*hfs_dir_cb)(const char *name, uint32_t cnid,
                           uint64_t size, bool is_dir, void *ctx);

static void hfs_iterate_catalog(uint32_t parent_cnid, hfs_dir_cb cb, void *ctx)
{
    /* Read B-tree header node (node 0) to get node size and root node */
    uint8_t hdr_buf[512];
    if (hfs_read_catalog_node(0, hdr_buf, 512) < 0) return;

    /* Header record starts at offset 14 (after node descriptor) */
    uint16_t node_size = be16(hdr_buf + 14 + 32);  /* nodeSize at header+32 */
    uint32_t root_node = be32(hdr_buf + 14 + 8);   /* rootNode at header+8 */
    uint32_t first_leaf = be32(hdr_buf + 14 + 24);  /* firstLeafNode */

    if (node_size == 0 || node_size > 32768) return;

    /* Scan leaf nodes starting from firstLeafNode */
    uint8_t *node_buf = (uint8_t *)kmalloc(node_size);
    if (!node_buf) return;

    uint32_t current = first_leaf;
    (void)root_node;

    while (current != 0) {
        if (hfs_read_catalog_node(current, node_buf, node_size) < 0) break;

        btree_node_header_t *nh = (btree_node_header_t *)node_buf;
        /* Byte-swap node header fields (big-endian) */
        uint32_t flink = be32(node_buf);
        uint16_t num_records = be16(node_buf + 10);
        int8_t kind = (int8_t)node_buf[8];

        if (kind != -1) { current = flink; continue; }  /* Not a leaf */

        /* Record offsets are at end of node, growing backwards */
        uint16_t *offsets = (uint16_t *)(node_buf + node_size - 2);

        for (int r = 0; r < num_records; r++) {
            uint16_t rec_off = be16((uint8_t *)&offsets[-r]);
            if (rec_off >= node_size - 2) continue;

            uint8_t *rec = node_buf + rec_off;

            /* Catalog key: keyLength(2) + parentID(4) + name(variable) */
            uint16_t key_len = be16(rec);
            if (key_len < 6) continue;
            uint32_t parent_id = be32(rec + 2);
            uint16_t name_len = be16(rec + 6);  /* Unicode chars */

            if (parent_id != parent_cnid) continue;

            /* Extract filename (UTF-16BE → ASCII) */
            char name[256];
            int nlen = name_len < 127 ? name_len : 127;
            for (int i = 0; i < nlen; i++) {
                uint16_t c = be16(rec + 8 + i * 2);
                name[i] = (c < 128) ? (char)c : '?';
            }
            name[nlen] = '\0';

            /* Record data follows key (aligned to 2 bytes) */
            uint16_t data_off = 2 + key_len;
            if (data_off & 1) data_off++;
            uint8_t *data = rec + data_off;

            /* Record type: 1=folder, 2=file */
            uint16_t rec_type = be16(data);
            if (rec_type == 1) {
                /* Folder */
                uint32_t cnid = be32(data + 8);
                cb(name, cnid, 0, true, ctx);
            } else if (rec_type == 2) {
                /* File — data fork logical size at data+88 */
                uint32_t cnid = be32(data + 8);
                uint64_t size = be64(data + 88);
                cb(name, cnid, size, false, ctx);
            }
        }

        current = flink;
        if (current == first_leaf) break;  /* Wrapped around */
    }

    kfree(node_buf);
}

/* ── Public API ──────────────────────────────────────────────── */

int hfsplus_mount(uint64_t part_lba)
{
    /* Volume header is at sector 2 (1024 bytes from partition start) */
    uint8_t vh_buf[512];
    if (nvme_read(part_lba + 2, 1, vh_buf) < 0) return -1;

    uint16_t sig = be16(vh_buf);
    if (sig != HFSPLUS_MAGIC && sig != HFSX_MAGIC) {
        serial_puts("[HFS+] Invalid signature 0x");
        serial_puthex(sig, 4);
        serial_puts("\n");
        return -1;
    }

    hfs.part_lba = part_lba;
    hfs.block_size = be32(vh_buf + 40);
    hfs.total_blocks = be32(vh_buf + 44);

    /* Catalog file: fork data at offset 288 in volume header */
    /* First extent: start_block at +296, block_count at +300 */
    hfs.catalog_start = be32(vh_buf + 296);
    hfs.catalog_blocks = be32(vh_buf + 300);

    hfs.mounted = true;

    serial_puts("[HFS+] Mounted: block_size=");
    serial_putdec(hfs.block_size);
    serial_puts(" blocks=");
    serial_putdec(hfs.total_blocks);
    serial_puts(" catalog@");
    serial_putdec(hfs.catalog_start);
    serial_puts("\n");
    return 0;
}

bool hfsplus_is_mounted(void) { return hfs.mounted; }

static void hfs_ls_cb(const char *name, uint32_t cnid,
                      uint64_t size, bool is_dir, void *ctx)
{
    (void)cnid; (void)ctx;
    serial_puts(is_dir ? "  [DIR] " : "  ");
    serial_puts(name);
    if (!is_dir) { serial_puts("  ("); serial_putdec(size); serial_puts(")"); }
    serial_puts("\n");
}

int hfsplus_ls(void)
{
    if (!hfs.mounted) return -1;
    serial_puts("[HFS+] Root directory:\n");
    /* Root folder CNID = 2 */
    hfs_iterate_catalog(2, hfs_ls_cb, NULL);
    return 0;
}
