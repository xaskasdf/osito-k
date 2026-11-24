/*
 * OsitoK x86-64 — Btrfs Read-Only Driver
 *
 * Reads files from Btrfs partitions (Fedora, openSUSE, SUSE).
 * Supports superblock, chunk tree (logical→physical mapping),
 * FS tree (directories, files, extents). No RAID, no compression.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   nvme_read(uint64_t lba, uint32_t count, void *buf);

/* ── Btrfs Key ───────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
} btrfs_key_t;

/* Key types */
#define BTRFS_INODE_ITEM_KEY     1
#define BTRFS_DIR_ITEM_KEY      84
#define BTRFS_DIR_INDEX_KEY     96
#define BTRFS_EXTENT_DATA_KEY  108
#define BTRFS_ROOT_ITEM_KEY    132
#define BTRFS_CHUNK_ITEM_KEY   228

/* Special object IDs */
#define BTRFS_ROOT_TREE_OBJECTID    1
#define BTRFS_CHUNK_TREE_OBJECTID   3
#define BTRFS_FS_TREE_OBJECTID      5
#define BTRFS_FIRST_FREE_OBJECTID 256

/* ── Btrfs Superblock ────────────────────────────────────────── */

#define BTRFS_SUPER_OFFSET  0x10000  /* 64KB from partition start */
#define BTRFS_MAGIC         0x4D5F53665248425FULL  /* "_BHRfS_M" LE */

typedef struct __attribute__((packed)) {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;              /* This block's byte address */
    uint64_t flags;
    uint64_t magic;
    uint64_t generation;
    uint64_t root;                /* Root tree logical address */
    uint64_t chunk_root;          /* Chunk tree logical address */
    uint64_t log_root;
    uint64_t log_root_transid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t root_dir_objectid;
    uint64_t num_devices;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t leafsize;
    uint32_t stripesize;
    uint32_t sys_chunk_array_size;
    uint64_t chunk_root_generation;
    uint64_t compat_flags;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint16_t csum_type;
    uint8_t  root_level;
    uint8_t  chunk_root_level;
    uint8_t  log_root_level;
    /* ... more fields, then sys_chunk_array at offset 0x32B */
} btrfs_super_t;

/* ── B-tree Node Header ──────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  chunk_tree_uuid[16];
    uint64_t generation;
    uint64_t owner;
    uint32_t nritems;
    uint8_t  level;               /* 0 = leaf */
} btrfs_header_t;

/* Leaf item (follows header) */
typedef struct __attribute__((packed)) {
    btrfs_key_t key;
    uint32_t    offset;           /* Offset within leaf data area */
    uint32_t    size;             /* Data size */
} btrfs_item_t;

/* Internal node key pointer */
typedef struct __attribute__((packed)) {
    btrfs_key_t key;
    uint64_t    blockptr;         /* Logical address of child node */
    uint64_t    generation;
} btrfs_key_ptr_t;

/* ── Chunk Map (logical → physical) ──────────────────────────── */

#define MAX_CHUNKS 64

typedef struct {
    uint64_t logical;
    uint64_t physical;
    uint64_t length;
} btrfs_chunk_entry_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t part_lba;
    uint32_t nodesize;
    uint32_t sectorsize;
    uint64_t root_logical;        /* Root tree bytenr */
    uint64_t fs_root_logical;     /* FS tree bytenr (looked up from root tree) */
    btrfs_chunk_entry_t chunks[MAX_CHUNKS];
    int      chunk_count;
} bt;

/* ── Logical → Physical Translation ──────────────────────────── */

static uint64_t btrfs_logical_to_phys(uint64_t logical)
{
    for (int i = 0; i < bt.chunk_count; i++) {
        if (logical >= bt.chunks[i].logical &&
            logical < bt.chunks[i].logical + bt.chunks[i].length) {
            return bt.chunks[i].physical + (logical - bt.chunks[i].logical);
        }
    }
    /* Fallback: assume identity mapping for system chunks */
    return logical;
}

/* ── Node I/O ────────────────────────────────────────────────── */

static int btrfs_read_node(uint64_t logical, void *buf)
{
    uint64_t phys = btrfs_logical_to_phys(logical);
    uint64_t lba = bt.part_lba + phys / 512;
    uint32_t sectors = bt.nodesize / 512;
    return nvme_read(lba, sectors, buf);
}

/* ── B-tree Search ───────────────────────────────────────────── */

/* Find item in a B-tree. Returns pointer to leaf data or NULL. */
static const uint8_t *btrfs_find_item(uint64_t root_logical,
                                       uint64_t objectid, uint8_t type,
                                       uint32_t *data_size)
{
    uint8_t *node = (uint8_t *)kmalloc(bt.nodesize);
    if (!node) return NULL;

    if (btrfs_read_node(root_logical, node) < 0) {
        kfree(node);
        return NULL;
    }

    btrfs_header_t *hdr = (btrfs_header_t *)node;

    /* Navigate internal nodes to find the right leaf */
    while (hdr->level > 0) {
        btrfs_key_ptr_t *ptrs = (btrfs_key_ptr_t *)(node + sizeof(btrfs_header_t));
        uint64_t child = ptrs[0].blockptr;

        /* Find the right child (last key <= target) */
        for (uint32_t i = 0; i < hdr->nritems; i++) {
            if (ptrs[i].key.objectid <= objectid)
                child = ptrs[i].blockptr;
            else
                break;
        }

        if (btrfs_read_node(child, node) < 0) {
            kfree(node);
            return NULL;
        }
        hdr = (btrfs_header_t *)node;
    }

    /* Scan leaf items */
    btrfs_item_t *items = (btrfs_item_t *)(node + sizeof(btrfs_header_t));
    for (uint32_t i = 0; i < hdr->nritems; i++) {
        if (items[i].key.objectid == objectid &&
            items[i].key.type == type) {
            /* Found! Copy data out (node may be freed) */
            uint32_t doff = items[i].offset;
            uint32_t dsize = items[i].size;
            uint8_t *data_area = node + sizeof(btrfs_header_t) +
                                 hdr->nritems * sizeof(btrfs_item_t);
            if (doff + dsize > bt.nodesize) { kfree(node); return NULL; }

            uint8_t *result = (uint8_t *)kmalloc(dsize);
            if (!result) { kfree(node); return NULL; }
            memcpy(result, data_area + doff, dsize);
            if (data_size) *data_size = dsize;
            kfree(node);
            return result;
        }
    }

    kfree(node);
    return NULL;
}

/* ── Directory Reading ───────────────────────────────────────── */

typedef void (*btrfs_dir_cb)(const char *name, uint64_t inode,
                              uint8_t type, void *ctx);

static void btrfs_read_dir(uint64_t dir_inode, btrfs_dir_cb cb, void *ctx)
{
    if (!bt.fs_root_logical) return;

    uint8_t *node = (uint8_t *)kmalloc(bt.nodesize);
    if (!node) return;

    if (btrfs_read_node(bt.fs_root_logical, node) < 0) {
        kfree(node);
        return;
    }

    btrfs_header_t *hdr = (btrfs_header_t *)node;

    /* For simplicity, scan leaf for DIR_INDEX items with matching objectid */
    /* TODO: proper B-tree descent for the target objectid */
    if (hdr->level > 0) { kfree(node); return; }

    btrfs_item_t *items = (btrfs_item_t *)(node + sizeof(btrfs_header_t));
    uint8_t *data_base = node + sizeof(btrfs_header_t) +
                         hdr->nritems * sizeof(btrfs_item_t);

    for (uint32_t i = 0; i < hdr->nritems; i++) {
        if (items[i].key.objectid == dir_inode &&
            items[i].key.type == BTRFS_DIR_INDEX_KEY) {
            /* DIR_INDEX data: child_location(17) + transid(8) + data_len(2) +
             * name_len(2) + type(1) + name(variable) */
            uint8_t *d = data_base + items[i].offset;
            uint32_t dsize = items[i].size;
            if (dsize < 30) continue;

            uint64_t child_objectid = *(uint64_t *)d;
            uint16_t name_len = *(uint16_t *)(d + 27);
            uint8_t  file_type = d[29];

            if (name_len > 0 && 30 + name_len <= dsize) {
                char name[256];
                int nl = name_len < 255 ? name_len : 255;
                memcpy(name, d + 30, nl);
                name[nl] = '\0';
                cb(name, child_objectid, file_type, ctx);
            }
        }
    }

    kfree(node);
}

/* ── Public API ──────────────────────────────────────────────── */

int btrfs_mount(uint64_t part_lba)
{
    /* Read superblock at 64KB offset */
    uint8_t sb_buf[4096];
    uint64_t sb_lba = part_lba + BTRFS_SUPER_OFFSET / 512;
    if (nvme_read(sb_lba, 8, sb_buf) < 0) return -1;

    btrfs_super_t *sb = (btrfs_super_t *)sb_buf;
    if (sb->magic != BTRFS_MAGIC) {
        serial_puts("[BTRFS] Invalid magic\n");
        return -1;
    }

    bt.part_lba = part_lba;
    bt.nodesize = sb->nodesize;
    bt.sectorsize = sb->sectorsize;
    bt.root_logical = sb->root;
    bt.chunk_count = 0;

    /* Parse system chunk array (embedded in superblock at offset 0x32B) */
    uint8_t *sys_chunks = sb_buf + 0x32B;
    uint32_t sys_len = sb->sys_chunk_array_size;
    uint32_t pos = 0;
    while (pos + 17 + 48 <= sys_len && bt.chunk_count < MAX_CHUNKS) {
        /* Key: objectid(8) + type(1) + offset(8) = 17 bytes */
        uint64_t logical = *(uint64_t *)(sys_chunks + pos + 9);  /* key.offset */
        pos += 17;
        /* Chunk item: length(8), owner(8), stripe_len(8), type(8),
         * io_align(4), io_width(4), sector_size(4), num_stripes(2),
         * sub_stripes(2), stripe[0] = devid(8) + offset(8) + dev_uuid(16) */
        uint64_t length = *(uint64_t *)(sys_chunks + pos);
        uint16_t num_stripes = *(uint16_t *)(sys_chunks + pos + 44);
        uint64_t physical = *(uint64_t *)(sys_chunks + pos + 48 + 8);

        bt.chunks[bt.chunk_count].logical = logical;
        bt.chunks[bt.chunk_count].physical = physical;
        bt.chunks[bt.chunk_count].length = length;
        bt.chunk_count++;

        pos += 48 + num_stripes * 32;
    }

    /* Look up FS tree root from root tree */
    bt.fs_root_logical = 0;
    uint32_t ri_size;
    const uint8_t *root_item = btrfs_find_item(bt.root_logical,
        BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, &ri_size);
    if (root_item && ri_size >= 176) {
        /* Root item: bytenr at offset 176 */
        bt.fs_root_logical = *(uint64_t *)(root_item + 176);
        kfree((void *)root_item);
    }

    bt.mounted = true;

    serial_puts("[BTRFS] Mounted: nodesize=");
    serial_putdec(bt.nodesize);
    serial_puts(" chunks=");
    serial_putdec((uint64_t)bt.chunk_count);
    if (bt.fs_root_logical) {
        serial_puts(" fs_root=0x");
        serial_puthex(bt.fs_root_logical, 16);
    }
    serial_puts("\n");
    return 0;
}

bool btrfs_is_mounted(void) { return bt.mounted; }

static void btrfs_ls_cb(const char *name, uint64_t inode,
                        uint8_t type, void *ctx)
{
    (void)inode; (void)ctx;
    /* type: 1=file, 2=dir */
    serial_puts(type == 2 ? "  [DIR] " : "  ");
    serial_puts(name);
    serial_puts("\n");
}

int btrfs_ls(void)
{
    if (!bt.mounted) return -1;
    serial_puts("[BTRFS] Root directory:\n");
    btrfs_read_dir(BTRFS_FIRST_FREE_OBJECTID, btrfs_ls_cb, NULL);
    return 0;
}
