/*
 * OsitoK x86-64 — OsitoFS v3 Bare-Metal Driver
 *
 * Hierarchical filesystem with inodes and extents.
 */

#include "../include/types.h"
#include "../../../include/common/ositofs3_format.h"

/* ── External Declarations ──────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern int nvme_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
extern int nvme_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len);
extern int nvme_flush(void);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── Driver State ────────────────────────────────────────────── */

static uint64_t      partition_offset;
static osfs3_super_t superblock;
static bool           mounted = false;

static uint8_t      *inode_bitmap = NULL;
static uint8_t      *block_bitmap = NULL;
static osfs3_inode_t *inode_table = NULL; /* Cached first block of inodes */

/* ── Helpers ─────────────────────────────────────────────────── */

static int osfs3_part_read(uint64_t offset, void *buf, uint64_t len)
{
    return nvme_read_bytes(partition_offset + offset, buf, len);
}

static int osfs3_read_block(uint32_t block, void *buf)
{
    uint64_t offset = (uint64_t)block << OSFS3_BLOCK_SHIFT;
    return osfs3_part_read(offset, buf, OSFS3_BLOCK_SIZE);
}

static osfs3_inode_t *osfs3_get_inode(uint32_t ino)
{
    if (!mounted || ino == 0 || ino >= superblock.total_inodes) return NULL;
    
    /* For now, we only handle inodes in the first block of the table (Ino 0-4095) */
    return &inode_table[ino];
}

/* ── Directory Traversal ─────────────────────────────────────── */

static uint32_t osfs3_find_in_dir(uint32_t dir_ino, const char *name)
{
    osfs3_inode_t *dir = osfs3_get_inode(dir_ino);
    if (!dir || !(dir->mode & OSFS3_S_IFDIR)) return 0;

    /* Use a smaller, stack-allocated or reusable buffer if possible. 
     * For now, let's keep the allocation but ensure it's handled carefully. */
    void *blk = mem_alloc_aligned(4096, 4096); /* Read only the first 4KB of dentry data */
    if (!blk) return 0;

    /* Most directories fit in the first 4KB */
    if (osfs3_part_read((uint64_t)dir->extents[0].start_block << OSFS3_BLOCK_SHIFT, blk, 4096) < 0) {
        mem_free_pages(blk, 1);
        return 0;
    }

    osfs3_dentry_t *de = (osfs3_dentry_t *)blk;
    uint32_t offset = 0;
    uint32_t name_len = strlen(name);
    uint32_t found_ino = 0;

    while (offset < 4096) {
        if (de->rec_len == 0 || de->rec_len > 1024) break;
        
        if (de->inode != 0 && de->name_len == name_len) {
            bool match = true;
            for (uint32_t i = 0; i < name_len; i++) {
                if (de->name[i] != name[i]) { match = false; break; }
            }
            if (match) { found_ino = de->inode; break; }
        }
        
        offset += de->rec_len;
        de = (osfs3_dentry_t *)((char *)de + de->rec_len);
    }

    mem_free_pages(blk, 1);
    return found_ino;
}

/* ── Path Resolution ────────────────────────────────────────── */

uint32_t osfs3_resolve_path(const char *path)
{
    if (!mounted || !path) return 0;
    if (path[0] == '\0') return superblock.root_inode;

    /* Handle absolute path skip */
    const char *curr = path;
    if (*curr == '/') curr++;

    uint32_t current_ino = superblock.root_inode;
    char component[OSFS3_NAME_MAX + 1];

    while (*curr) {
        /* Extract next component */
        int i = 0;
        while (*curr && *curr != '/' && i < OSFS3_NAME_MAX) {
            component[i++] = *curr++;
        }
        component[i] = '\0';

        if (i > 0) {
            current_ino = osfs3_find_in_dir(current_ino, component);
            if (!current_ino) return 0;
        }

        if (*curr == '/') curr++;
    }

    return current_ino;
}

/* ── Mount ───────────────────────────────────────────────────── */

int osfs3_mount(uint64_t part_offset)
{
    partition_offset = part_offset;
    mounted = false;

    serial_puts("[OsitoFS v3] Mounting at offset ");
    serial_puthex(part_offset, 16);
    serial_puts("\n");

    /* Read superblock */
    if (osfs3_part_read(0, &superblock, sizeof(superblock)) < 0) return -1;

    if (superblock.magic != OSFS3_MAGIC) {
        serial_puts("[OsitoFS v3] Bad magic\n");
        return -1;
    }

    /* Load bitmaps and first inode block */
    inode_bitmap = (uint8_t *)mem_alloc_aligned(OSFS3_BLOCK_SIZE, 4096);
    block_bitmap = (uint8_t *)mem_alloc_aligned(OSFS3_BLOCK_SIZE, 4096);
    inode_table  = (osfs3_inode_t *)mem_alloc_aligned(OSFS3_BLOCK_SIZE, 4096);

    if (osfs3_read_block(1, inode_bitmap) < 0 ||
        osfs3_read_block(2, block_bitmap) < 0 ||
        osfs3_read_block(3, inode_table) < 0) {
        return -1;
    }

    mounted = true;
    
    fb_puts("\n OsitoFS v3 [");
    fb_puts(superblock.label);
    fb_puts("] — Hierarchical FS Ready\n");

    return 0;
}

/* ── File I/O ────────────────────────────────────────────────── */

int osfs3_read(uint32_t ino, uint64_t offset, void *buf, uint64_t len)
{
    osfs3_inode_t *inode = osfs3_get_inode(ino);
    if (!inode || (inode->mode & OSFS3_S_IFDIR)) return -1;
    if (offset >= inode->size) return 0;
    if (offset + len > inode->size) len = inode->size - offset;
    
    uint64_t bytes_read = 0;
    uint8_t *ptr = (uint8_t *)buf;

    /* Simplified extent reading with chunking (max 64KB per NVMe call) */
    while (bytes_read < len) {
        uint64_t chunk = len - bytes_read;
        if (chunk > 65536) chunk = 65536;

        uint64_t abs_offset = ((uint64_t)inode->extents[0].start_block << OSFS3_BLOCK_SHIFT) + offset + bytes_read;
        if (nvme_read_bytes(partition_offset + abs_offset, ptr + bytes_read, chunk) < 0) {
            return bytes_read > 0 ? (int)bytes_read : -1;
        }
        bytes_read += chunk;
    }

    return (int)bytes_read;
}

/* ── List Directory ────────────────────────────────────────── */

void osfs3_list_dir(uint32_t dir_ino)
{
    osfs3_inode_t *dir = osfs3_get_inode(dir_ino);
    if (!dir || !(dir->mode & OSFS3_S_IFDIR)) return;

    void *blk = mem_alloc_aligned(4096, 4096);
    if (!blk) return;

    if (osfs3_part_read((uint64_t)dir->extents[0].start_block << OSFS3_BLOCK_SHIFT, blk, 4096) < 0) {
        mem_free_pages(blk, 1);
        return;
    }

    osfs3_dentry_t *de = (osfs3_dentry_t *)blk;
    uint32_t offset = 0;

    serial_puts("[OsitoFS v3] Directory listing:\n");
    while (offset < 4096) {
        if (de->rec_len == 0 || de->rec_len > 1024) break;
        if (de->inode != 0) {
            osfs3_inode_t *ino = osfs3_get_inode(de->inode);
            serial_puts("  ");
            if (ino && (ino->mode & OSFS3_S_IFDIR)) serial_puts("<DIR> ");
            else serial_puts("      ");
            
            /* Print name */
            char name[OSFS3_NAME_MAX + 1];
            uint32_t nlen = de->name_len;
            if (nlen > OSFS3_NAME_MAX) nlen = OSFS3_NAME_MAX;
            memcpy(name, de->name, nlen);
            name[nlen] = '\0';
            serial_puts(name);
            
            if (ino) {
                serial_puts("  size=");
                serial_putdec(ino->size);
            }
            serial_puts("\n");
        }
        offset += de->rec_len;
        de = (osfs3_dentry_t *)((char *)de + de->rec_len);
    }

    mem_free_pages(blk, 1);
}

uint64_t osfs3_get_size(uint32_t ino)
{
    osfs3_inode_t *inode = osfs3_get_inode(ino);
    return inode ? inode->size : 0;
}

bool osfs3_is_dir(uint32_t ino)
{
    osfs3_inode_t *inode = osfs3_get_inode(ino);
    return inode && (inode->mode & OSFS3_S_IFDIR);
}

bool osfs3_is_mounted(void)
{
    return mounted;
}
