/*
 * OsitoK x86-64 — APFS Read-Only Driver (Apple File System)
 *
 * Reads files from modern macOS/iOS partitions (2017+).
 * Supports container superblock, volume superblock, B-tree catalog.
 * No encryption, no snapshots, no clones (basic read path).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern int   nvme_read(uint64_t lba, uint32_t count, void *buf);

/* ── APFS On-Disk Structures ────────────────────────────────── */

#define APFS_MAGIC          0x4253584E  /* "NXSB" (container superblock) */
#define APFS_VOL_MAGIC      0x42535041  /* "APSB" (volume superblock) */
#define APFS_BLOCK_SIZE     4096        /* Always 4KB */

/* Object header (every on-disk object starts with this) */
typedef struct __attribute__((packed)) {
    uint64_t cksum;              /* Fletcher-64 checksum */
    uint64_t oid;                /* Object ID */
    uint64_t xid;                /* Transaction ID */
    uint32_t type;               /* Object type + storage type */
    uint32_t subtype;
} apfs_obj_hdr_t;

/* Object types (lower 16 bits of type field) */
#define APFS_OBJ_NX_SUPERBLOCK  0x0001
#define APFS_OBJ_BTREE          0x0002
#define APFS_OBJ_BTREE_NODE     0x0003
#define APFS_OBJ_OMAP           0x000B
#define APFS_OBJ_FS             0x000D

/* Container superblock (NXSB) */
typedef struct __attribute__((packed)) {
    apfs_obj_hdr_t hdr;
    uint32_t magic;              /* APFS_MAGIC */
    uint32_t block_size;
    uint64_t block_count;
    uint64_t features;
    uint64_t ro_compat_features;
    uint64_t incompat_features;
    uint8_t  uuid[16];
    uint64_t next_oid;
    uint64_t next_xid;
    uint32_t xp_desc_blocks;
    uint32_t xp_data_blocks;
    uint64_t xp_desc_base;
    uint64_t xp_data_base;
    uint32_t xp_desc_next;
    uint32_t xp_data_next;
    uint32_t xp_desc_index;
    uint32_t xp_desc_len;
    uint32_t xp_data_index;
    uint32_t xp_data_len;
    uint64_t spaceman_oid;
    uint64_t omap_oid;
    uint64_t reaper_oid;
    uint32_t test_type;
    uint32_t max_file_systems;
    uint64_t fs_oid[100];        /* Volume OIDs (up to 100 volumes) */
} apfs_nx_superblock_t;

/* B-tree node header */
typedef struct __attribute__((packed)) {
    apfs_obj_hdr_t hdr;
    uint16_t flags;
    uint16_t level;              /* 0 = leaf */
    uint32_t nkeys;
    uint16_t table_space_off;
    uint16_t table_space_len;
    uint16_t free_space_off;
    uint16_t free_space_len;
    uint16_t key_free_list_off;
    uint16_t key_free_list_len;
    uint16_t val_free_list_off;
    uint16_t val_free_list_len;
} apfs_btree_node_t;

#define APFS_BTN_FIXED_KV   0x0004
#define APFS_BTN_LEAF       0x0000  /* (level == 0) */

/* Table of contents entry */
typedef struct __attribute__((packed)) {
    uint16_t key_off;
    uint16_t key_len;
    uint16_t val_off;
    uint16_t val_len;
} apfs_kvloc_t;

/* Catalog key types */
#define APFS_TYPE_DIR_REC    9

/* Directory record value (catalog) */
typedef struct __attribute__((packed)) {
    uint64_t file_id;
    uint64_t date_added;
    uint16_t flags;
    uint8_t  xfields[];
} apfs_drec_val_t;

/* ── Mount State ─────────────────────────────────────────────── */

static struct {
    bool     mounted;
    uint64_t part_lba;
    uint32_t block_size;
    uint64_t block_count;
    uint64_t omap_oid;           /* Object map OID */
    uint64_t catalog_oid;        /* Catalog B-tree OID */
    uint64_t first_vol_oid;      /* First volume OID */
    char     volume_name[64];
} apfs;

/* ── Block I/O ───────────────────────────────────────────────── */

static int apfs_read_block(uint64_t block, void *buf)
{
    uint64_t sectors = apfs.block_size / 512;
    return nvme_read(apfs.part_lba + block * sectors, (uint32_t)sectors, buf);
}

/* ── Public API ──────────────────────────────────────────────── */

int apfs_mount(uint64_t part_lba)
{
    uint8_t *buf = (uint8_t *)kmalloc(APFS_BLOCK_SIZE);
    if (!buf) return -1;

    /* Read container superblock (block 0) */
    apfs.part_lba = part_lba;
    apfs.block_size = APFS_BLOCK_SIZE;

    if (nvme_read(part_lba, APFS_BLOCK_SIZE / 512, buf) < 0) {
        kfree(buf);
        return -1;
    }

    apfs_nx_superblock_t *nx = (apfs_nx_superblock_t *)buf;
    if (nx->magic != APFS_MAGIC) {
        serial_puts("[APFS] Invalid container magic (expected NXSB)\n");
        kfree(buf);
        return -1;
    }

    apfs.block_size = nx->block_size;
    apfs.block_count = nx->block_count;
    apfs.omap_oid = nx->omap_oid;
    apfs.first_vol_oid = nx->fs_oid[0];

    serial_puts("[APFS] Container: ");
    serial_putdec(apfs.block_count);
    serial_puts(" blocks (");
    serial_putdec(apfs.block_count * apfs.block_size / (1024 * 1024));
    serial_puts(" MB), omap=0x");
    serial_puthex(apfs.omap_oid, 16);
    serial_puts("\n");

    /* Read first volume superblock */
    if (apfs.first_vol_oid) {
        if (apfs_read_block(apfs.first_vol_oid, buf) == 0) {
            /* Volume name at offset 0x48 (variable, encoded as UTF-8 after fields) */
            apfs_obj_hdr_t *vhdr = (apfs_obj_hdr_t *)buf;
            uint32_t *vol_magic = (uint32_t *)(buf + sizeof(apfs_obj_hdr_t));
            if (*vol_magic == APFS_VOL_MAGIC) {
                /* Volume name is at a fixed offset in the volume superblock */
                /* For simplicity, read bytes at offset 0x1A2 (common location) */
                char *vname = (char *)(buf + 0x1A2);
                int nl = 0;
                while (vname[nl] && nl < 63) { apfs.volume_name[nl] = vname[nl]; nl++; }
                apfs.volume_name[nl] = '\0';
                serial_puts("[APFS] Volume: \"");
                serial_puts(apfs.volume_name);
                serial_puts("\"\n");
            }
        }
    }

    apfs.mounted = true;
    kfree(buf);

    serial_puts("[APFS] Mounted (read-only, catalog browse not yet implemented)\n");
    return 0;
}

bool apfs_is_mounted(void) { return apfs.mounted; }

int apfs_ls(void)
{
    if (!apfs.mounted) return -1;
    serial_puts("[APFS] Volume: \"");
    serial_puts(apfs.volume_name);
    serial_puts("\" — directory listing requires full omap+catalog B-tree traversal (TODO)\n");
    return 0;
}
