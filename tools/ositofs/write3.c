/*
 * ositofs-write3 — Write files to OsitoFS v3 partition
 *
 * Usage: ositofs-write3 <device> <source_file> --dest <path>
 *
 * Supports hierarchical directories, dynamic allocation, and extents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <time.h>

#include "common_v3.h"

static void usage(void)
{
    fprintf(stderr, "Usage: ositofs-write3 <device> <source_file> --dest <path>\n");
    exit(1);
}

/* ── Bitmap Helpers ─────────────────────────────────────────── */

static int get_free_bit(uint8_t *bitmap, uint32_t total_bits)
{
    for (uint32_t i = 0; i < (total_bits + 7) / 8; i++) {
        if (bitmap[i] != 0xFF) {
            for (int b = 0; b < 8; b++) {
                if (!(bitmap[i] & (1 << b))) {
                    uint32_t bit = i * 8 + b;
                    if (bit < total_bits) return bit;
                }
            }
        }
    }
    return -1;
}

static void set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

/* ── Global State ────────────────────────────────────────────── */
static int dev_fd = -1;
static osfs3_super_t sb;
static uint8_t *imap = NULL;
static uint8_t *bmap = NULL;
static osfs3_inode_t *itab = NULL;
static uint32_t itab_blocks = 0;
static size_t itab_bytes = 0;

/* ── Inode Management ────────────────────────────────────────── */

static int load_metadata(void)
{
    imap = osfs3_alloc_block();
    bmap = osfs3_alloc_block();
    itab_blocks = osfs3_inode_table_blocks(sb.total_inodes);
    itab_bytes = (size_t)itab_blocks * OSFS3_BLOCK_SIZE;
    if (posix_memalign((void **)&itab, 4096, itab_bytes) != 0)
        itab = NULL;
    if (!imap || !bmap || !itab) return -1;
    memset(itab, 0, itab_bytes);

    if (osfs3_read_block(dev_fd, OSFS3_INODE_BITMAP_BLK, imap) < 0) return -1;
    if (osfs3_read_block(dev_fd, OSFS3_BLOCK_BITMAP_BLK, bmap) < 0) return -1;
    for (uint32_t i = 0; i < itab_blocks; i++) {
        void *dst = (uint8_t *)itab + (size_t)i * OSFS3_BLOCK_SIZE;
        if (osfs3_read_block(dev_fd, OSFS3_INODE_TABLE_BLK + i, dst) < 0)
            return -1;
    }
    return 0;
}

static int save_metadata(void)
{
    sb.crc32 = 0;
    sb.crc32 = osfs3_crc32(&sb, sizeof(sb));
    
    void *sb_blk = osfs3_alloc_block();
    if (!sb_blk) return -1;
    memcpy(sb_blk, &sb, sizeof(sb));
    if (osfs3_write_block(dev_fd, 0, sb_blk) < 0) return -1;
    osfs3_free_block(sb_blk);

    if (osfs3_write_block(dev_fd, OSFS3_INODE_BITMAP_BLK, imap) < 0) return -1;
    if (osfs3_write_block(dev_fd, OSFS3_BLOCK_BITMAP_BLK, bmap) < 0) return -1;
    for (uint32_t i = 0; i < itab_blocks; i++) {
        void *src = (uint8_t *)itab + (size_t)i * OSFS3_BLOCK_SIZE;
        if (osfs3_write_block(dev_fd, OSFS3_INODE_TABLE_BLK + i, src) < 0)
            return -1;
    }
    return 0;
}

static uint32_t alloc_inode(uint16_t mode)
{
    int ino = get_free_bit(imap, sb.total_inodes);
    if (ino < 0) return 0;
    set_bit(imap, ino);
    
    memset(&itab[ino], 0, sizeof(osfs3_inode_t));
    itab[ino].mode = mode;
    itab[ino].nlink = (mode & OSFS3_S_IFDIR) ? 2 : 1;
    itab[ino].atime = itab[ino].mtime = itab[ino].ctime = time(NULL);
    sb.free_inodes--;
    return ino;
}

static uint32_t alloc_block(void)
{
    int blk = get_free_bit(bmap, sb.total_blocks);
    if (blk < 0) return 0;
    set_bit(bmap, blk);
    sb.free_blocks--;
    return blk;
}

/* ── Directory Operations ────────────────────────────────────── */

static int add_dentry(uint32_t dir_ino, uint32_t target_ino, const char *name, uint8_t type)
{
    osfs3_inode_t *dir = &itab[dir_ino];
    if (!(dir->mode & OSFS3_S_IFDIR)) return -1;

    uint32_t blk_num = dir->extents[0].start_block;
    void *blk = osfs3_alloc_block();
    if (osfs3_read_block(dev_fd, blk_num, blk) < 0) { osfs3_free_block(blk); return -1; }

    osfs3_dentry_t *de = (osfs3_dentry_t *)blk;
    uint32_t offset = 0;
    
    while (offset < OSFS3_BLOCK_SIZE) {
        if (de->rec_len == 0) break;
        uint32_t actual_rec_len = OSFS3_DIR_REC_LEN(de->name_len);
        
        if (de->rec_len >= actual_rec_len + OSFS3_DIR_REC_LEN(strlen(name))) {
            uint16_t old_rec_len = de->rec_len;
            de->rec_len = actual_rec_len;
            
            osfs3_dentry_t *new_de = (osfs3_dentry_t *)((char *)de + actual_rec_len);
            new_de->inode = target_ino;
            new_de->name_len = strlen(name);
            new_de->type = type;
            new_de->rec_len = old_rec_len - actual_rec_len;
            memcpy(new_de->name, name, new_de->name_len);
            
            osfs3_write_block(dev_fd, blk_num, blk);
            osfs3_free_block(blk);
            return 0;
        }
        
        offset += de->rec_len;
        if (offset >= OSFS3_BLOCK_SIZE) break;
        de = (osfs3_dentry_t *)((char *)de + de->rec_len);
    }

    osfs3_free_block(blk);
    return -1;
}

static uint32_t find_in_dir(uint32_t dir_ino, const char *name)
{
    osfs3_inode_t *dir = &itab[dir_ino];
    void *blk = osfs3_alloc_block();
    if (osfs3_read_block(dev_fd, dir->extents[0].start_block, blk) < 0) {
        osfs3_free_block(blk); return 0;
    }

    osfs3_dentry_t *de = (osfs3_dentry_t *)blk;
    uint32_t offset = 0;
    while (offset < OSFS3_BLOCK_SIZE) {
        if (de->rec_len == 0) break;
        if (de->inode != 0 && de->name_len == strlen(name) && 
            strncmp(de->name, name, de->name_len) == 0) {
            uint32_t found = de->inode;
            osfs3_free_block(blk);
            return found;
        }
        offset += de->rec_len;
        if (offset >= OSFS3_BLOCK_SIZE) break;
        de = (osfs3_dentry_t *)((char *)de + de->rec_len);
    }
    osfs3_free_block(blk);
    return 0;
}

/* ── Path Resolution ────────────────────────────────────────── */

static uint32_t resolve_path(const char *path, int create_dirs)
{
    if (!path || strcmp(path, "/") == 0 || strcmp(path, "") == 0) return sb.root_inode;

    char *p = strdup(path);
    char *token = strtok(p, "/");
    uint32_t current_ino = sb.root_inode;

    while (token) {
        uint32_t next_ino = find_in_dir(current_ino, token);
        
        if (!next_ino) {
            if (create_dirs) {
                next_ino = alloc_inode(OSFS3_S_IFDIR | 0755);
                if (!next_ino) {
                    free(p);
                    return 0;
                }
                uint32_t data_blk = alloc_block();
                if (!data_blk) {
                    free(p);
                    return 0;
                }
                itab[next_ino].extent_count = 1;
                itab[next_ino].extents[0].start_block = data_blk;
                itab[next_ino].extents[0].block_count = 1;
                
                void *blk = osfs3_alloc_block();
                osfs3_dentry_t *de = (osfs3_dentry_t *)blk;
                de->inode = next_ino; de->name_len = 1; de->type = OSFS3_DT_DIR;
                memcpy(de->name, ".", 1); de->rec_len = OSFS3_DIR_REC_LEN(1);
                osfs3_dentry_t *de2 = (osfs3_dentry_t *)((char *)de + de->rec_len);
                de2->inode = current_ino; de2->name_len = 2; de2->type = OSFS3_DT_DIR;
                memcpy(de2->name, "..", 2); de2->rec_len = OSFS3_BLOCK_SIZE - de->rec_len;
                osfs3_write_block(dev_fd, data_blk, blk);
                osfs3_free_block(blk);

                if (add_dentry(current_ino, next_ino, token, OSFS3_DT_DIR) < 0) {
                    free(p); return 0;
                }
            } else {
                free(p); return 0;
            }
        }
        current_ino = next_ino;
        token = strtok(NULL, "/");
    }
    free(p);
    return current_ino;
}

/* ── Main ────────────────────────────────────────────────────── */

#include <dirent.h>

/* ... existing includes ... */

static int is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int import_recursive(const char *host_path, const char *dest_path);

static int write_file_to_osfs(const char *src_file, const char *dest_path)
{
    struct stat st;
    if (stat(src_file, &st) < 0) { perror("stat"); return -1; }

    char *dpath = strdup(dest_path);
    char *fpath = strdup(dest_path);
    char *parent_dir = dirname(dpath);
    char *filename = basename(fpath);

    uint32_t parent_ino = resolve_path(parent_dir, 1);
    if (!parent_ino) { fprintf(stderr, "Could not resolve/create parent: %s\n", parent_dir); free(dpath); free(fpath); return -1; }

    if (find_in_dir(parent_ino, filename)) {
        // fprintf(stderr, "File '%s' already exists, skipping\n", filename);
        free(dpath); free(fpath); return 0;
    }

    uint32_t file_ino = alloc_inode(OSFS3_S_IFREG | 0644);
    if (!file_ino) {
        fprintf(stderr, "No free inodes for %s\n", dest_path);
        free(dpath); free(fpath);
        return -1;
    }
    itab[file_ino].size = st.st_size;
    
    uint32_t blocks_needed = (st.st_size + OSFS3_BLOCK_SIZE - 1) / OSFS3_BLOCK_SIZE;
    if (st.st_size > 0) {
        uint32_t start_blk = 0;
        uint32_t found_count = 0;
        for (uint32_t b = sb.first_data_block; b < sb.total_blocks; b++) {
            if (!(bmap[b / 8] & (1 << (b % 8)))) {
                if (found_count == 0) start_blk = b;
                found_count++;
                if (found_count == blocks_needed) break;
            } else found_count = 0;
        }
        if (found_count < blocks_needed) { fprintf(stderr, "No space for %u blocks\n", blocks_needed); return -1; }

        for (uint32_t b = start_blk; b < start_blk + blocks_needed; b++) set_bit(bmap, b);
        sb.free_blocks -= blocks_needed;

        FILE *f = fopen(src_file, "rb");
        void *blk = osfs3_alloc_block();
        for (uint32_t i = 0; i < blocks_needed; i++) {
            memset(blk, 0, OSFS3_BLOCK_SIZE);
            fread(blk, 1, OSFS3_BLOCK_SIZE, f);
            osfs3_write_block(dev_fd, start_blk + i, blk);
        }
        fclose(f);
        osfs3_free_block(blk);

        itab[file_ino].extents[0].start_block = start_blk;
        itab[file_ino].extents[0].block_count = blocks_needed;
        itab[file_ino].extent_count = 1;
    }

    add_dentry(parent_ino, file_ino, filename, OSFS3_DT_REG);
    free(dpath); free(fpath);
    return 0;
}

static int import_recursive(const char *host_path, const char *dest_path)
{
    if (!is_dir(host_path)) {
        return write_file_to_osfs(host_path, dest_path);
    }

    printf("Importing directory: %s -> %s\n", host_path, dest_path);
    
    /* Ensure destination directory exists */
    if (!resolve_path(dest_path, 1)) return -1;

    DIR *d = opendir(host_path);
    if (!d) return -1;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char sub_host[1024];
        char sub_dest[1024];
        snprintf(sub_host, sizeof(sub_host), "%s/%s", host_path, de->d_name);
        snprintf(sub_dest, sizeof(sub_dest), "%s/%s", dest_path, de->d_name);

        import_recursive(sub_host, sub_dest);
    }

    closedir(d);
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *src_path = NULL;
    const char *dest_path = NULL;
    int recursive = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
            dest_path = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0) {
            recursive = 1;
        } else if (argv[i][0] != '-') {
            if (!device) device = argv[i];
            else if (!src_path) src_path = argv[i];
        }
    }

    if (!device || !src_path || !dest_path) usage();

    dev_fd = osfs3_open_device(device, 0);
    if (dev_fd < 0 || osfs3_read_super(dev_fd, &sb) < 0 || load_metadata() < 0) return 1;

    if (recursive) {
        import_recursive(src_path, dest_path);
    } else {
        write_file_to_osfs(src_path, dest_path);
    }

    if (save_metadata() < 0) return 1;
    printf("Import complete.\n");

    osfs3_close_device(dev_fd);
    return 0;
}
