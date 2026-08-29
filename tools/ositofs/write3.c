/* Import files and directory trees into an OsitoFS v3 image. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common_v3.h"

static int device_fd = -1;
static osfs3_super_t superblock;
static uint8_t *inode_bitmap;
static uint8_t *block_bitmap;
static osfs3_inode_t *inode_table;
static uint8_t *block_buffer;
static uint32_t next_inode_hint = 2;
static uint32_t next_block_hint;
static uint32_t imported_files;
static uint32_t imported_dirs;
static uint64_t imported_bytes;
static int replace_existing;

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-write3 <device> <source> --dest <path> "
        "[-r] [--replace]\n");
    exit(1);
}

static int bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

/* ── Global State ────────────────────────────────────────────── */
static int dev_fd = -1;
static osfs3_super_t sb;
static uint8_t *imap = NULL;
static uint8_t *bmap = NULL;
static osfs3_inode_t *itab = NULL;
static uint32_t itab_blocks = 0;
static size_t itab_bytes = 0;

static int alloc_aligned(void **out, size_t bytes)
{
    if (posix_memalign(out, 4096, bytes) != 0) return -1;
    memset(*out, 0, bytes);
    return 0;
}

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

static uint32_t find_free_run(uint32_t wanted, uint32_t *actual)
{
    uint32_t best_start = 0;
    uint32_t best_length = 0;
    uint32_t start = 0;
    uint32_t length = 0;
    uint32_t begin = next_block_hint < superblock.total_blocks
        ? next_block_hint : superblock.first_data_block;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t first = pass == 0 ? begin : superblock.first_data_block;
        uint32_t last = pass == 0 ? superblock.total_blocks : begin;
        for (uint32_t block = first; block <= last; block++) {
            int free = block < last && !bitmap_test(block_bitmap, block);
            if (free) {
                if (!length) start = block;
                length++;
                if (length >= wanted) {
                    *actual = wanted;
                    return start;
                }
            } else {
                if (length > best_length) {
                    best_start = start;
                    best_length = length;
                }
                length = 0;
            }
        }
    }
    if (!best_length) return 0;
    *actual = best_length > wanted ? wanted : best_length;
    return best_start;
}

static void mark_run(uint32_t start, uint32_t count, int used)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t block = start + i;
        int was_used = bitmap_test(block_bitmap, block);
        if (used && !was_used) {
            bitmap_set(block_bitmap, block);
            superblock.free_blocks--;
        } else if (!used && was_used) {
            bitmap_clear(block_bitmap, block);
            superblock.free_blocks++;
        }
    }
    if (used && start + count > next_block_hint)
        next_block_hint = start + count;
}

static int zero_run(uint32_t start, uint32_t count)
{
    memset(block_buffer, 0, OSFS3_BLOCK_SIZE);
    for (uint32_t i = 0; i < count; i++)
        if (osfs3_write_block(device_fd, start + i, block_buffer) < 0)
            return -1;
    return 0;
}

static int ensure_blocks(osfs3_inode_t *inode, uint32_t target)
{
    uint32_t current = inode_block_count(inode);
    while (current < target) {
        uint32_t missing = target - current;
        uint32_t start = 0;
        uint32_t count = 0;
        int extend = 0;
        if (inode->extent_count) {
            osfs3_extent_t *last = &inode->extents[inode->extent_count - 1U];
            start = last->start_block + last->block_count;
            while (count < missing && start + count < superblock.total_blocks &&
                   !bitmap_test(block_bitmap, start + count))
                count++;
            extend = count != 0;
        }
        if (!count) {
            if (inode->extent_count >= OSFS3_MAX_EXTENTS) return -1;
            start = find_free_run(missing, &count);
            if (!start || !count) return -1;
        }
        mark_run(start, count, 1);
        if (extend)
            inode->extents[inode->extent_count - 1U].block_count += count;
        else {
            inode->extents[inode->extent_count].start_block = start;
            inode->extents[inode->extent_count].block_count = count;
            inode->extent_count++;
        }
        if (zero_run(start, count) < 0) return -1;
        current += count;
    }
    return 0;
}

static int trim_blocks(osfs3_inode_t *inode, uint32_t target)
{
    uint32_t current = inode_block_count(inode);
    while (current > target && inode->extent_count) {
        osfs3_extent_t *last = &inode->extents[inode->extent_count - 1U];
        uint32_t count = current - target;
        if (count > last->block_count) count = last->block_count;
        uint32_t start = last->start_block + last->block_count - count;
        if (zero_run(start, count) < 0) return -1;
        mark_run(start, count, 0);
        last->block_count -= count;
        current -= count;
        if (!last->block_count) {
            memset(last, 0, sizeof(*last));
            inode->extent_count--;
        }
    }
    return current == target ? 0 : -1;
}

static uint32_t alloc_inode(uint16_t mode, const struct stat *metadata)
{
    for (uint32_t scanned = 0; scanned < superblock.total_inodes; scanned++) {
        uint32_t ino = next_inode_hint + scanned;
        if (ino >= superblock.total_inodes) ino = 2 + (ino - superblock.total_inodes);
        if (ino < 2 || bitmap_test(inode_bitmap, ino)) continue;
        bitmap_set(inode_bitmap, ino);
        superblock.free_inodes--;
        next_inode_hint = ino + 1;
        osfs3_inode_t *inode = &inode_table[ino];
        memset(inode, 0, sizeof(*inode));
        inode->mode = mode;
        inode->nlink = (mode & OSFS3_S_IFMT) == OSFS3_S_IFDIR ? 2 : 1;
        inode->atime = metadata ? (uint64_t)metadata->st_atime : (uint64_t)time(NULL);
        inode->mtime = metadata ? (uint64_t)metadata->st_mtime : inode->atime;
        inode->ctime = metadata ? (uint64_t)metadata->st_ctime : inode->atime;
        return ino;
    }
    return 0;
}

static int dentry_valid(const osfs3_dentry_t *entry, uint32_t remaining)
{
    return remaining >= sizeof(*entry) && entry->rec_len >= sizeof(*entry) &&
           !(entry->rec_len & 3U) && entry->rec_len <= remaining &&
           entry->name_len <= entry->rec_len - sizeof(*entry);
}

static int dentry_name_equal(const osfs3_dentry_t *entry, const char *name)
{
    size_t length = strlen(name);
    return entry->name_len == length &&
           memcmp(entry->name, name, length) == 0;
}

static uint32_t find_in_dir(uint32_t dir_ino, const char *name)
{
    osfs3_inode_t *dir = &inode_table[dir_ino];
    if ((dir->mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR) return 0;
    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t index = 0; index < dir->extents[extent].block_count;
             index++) {
            uint32_t block = dir->extents[extent].start_block + index;
            if (osfs3_read_block(device_fd, block, block_buffer) < 0) return 0;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)(block_buffer + offset);
                if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
                    return 0;
                if (entry->inode && dentry_name_equal(entry, name))
                    return entry->inode;
                offset += entry->rec_len;
            }
        }
    }
    return 0;
}

static int insert_loaded_dentry(uint32_t block, uint32_t target,
                                const char *name, uint8_t type)
{
    uint32_t name_length = (uint32_t)strlen(name);
    uint32_t needed = OSFS3_DIR_REC_LEN(name_length);
    uint32_t offset = 0;
    while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
        osfs3_dentry_t *entry =
            (osfs3_dentry_t *)(block_buffer + offset);
        if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) return -1;
        uint32_t insertion = 0;
        uint32_t available = 0;
        if (!entry->inode && entry->rec_len >= needed) {
            insertion = offset;
            available = entry->rec_len;
        } else if (entry->inode) {
            uint32_t actual = OSFS3_DIR_REC_LEN(entry->name_len);
            if (entry->rec_len >= actual + needed) {
                insertion = offset + actual;
                available = entry->rec_len - actual;
                entry->rec_len = (uint16_t)actual;
            }
        }
        if (available) {
            osfs3_dentry_t *added =
                (osfs3_dentry_t *)(block_buffer + insertion);
            uint32_t remainder = available - needed;
            memset(added, 0, available);
            added->inode = target;
            added->name_len = (uint8_t)name_length;
            added->type = type;
            added->rec_len = (uint16_t)(remainder >= sizeof(*added)
                                          ? needed : available);
            memcpy(added->name, name, name_length);
            if (remainder >= sizeof(*added)) {
                osfs3_dentry_t *free_entry =
                    (osfs3_dentry_t *)((uint8_t *)added + needed);
                free_entry->rec_len = (uint16_t)remainder;
            }
            return osfs3_write_block(device_fd, block, block_buffer);
        }
        offset += entry->rec_len;
    }
    return 1;
}

static int add_dentry(uint32_t dir_ino, uint32_t target, const char *name,
                      uint8_t type)
{
    osfs3_inode_t *dir = &inode_table[dir_ino];
    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t index = 0; index < dir->extents[extent].block_count;
             index++) {
            uint32_t block = dir->extents[extent].start_block + index;
            if (osfs3_read_block(device_fd, block, block_buffer) < 0) return -1;
            int result = insert_loaded_dentry(block, target, name, type);
            if (result <= 0) return result;
        }
    }
    uint32_t old_blocks = inode_block_count(dir);
    if (ensure_blocks(dir, old_blocks + 1U) < 0) return -1;
    uint32_t block = map_file_block(dir, old_blocks);
    memset(block_buffer, 0, OSFS3_BLOCK_SIZE);
    osfs3_dentry_t *entry = (osfs3_dentry_t *)block_buffer;
    entry->inode = target;
    entry->rec_len = (uint16_t)OSFS3_DIR_BLOCK_BYTES;
    entry->name_len = (uint8_t)strlen(name);
    entry->type = type;
    memcpy(entry->name, name, entry->name_len);
    dir->size = (uint64_t)(old_blocks + 1U) * OSFS3_BLOCK_SIZE;
    return osfs3_write_block(device_fd, block, block_buffer);
}

static uint32_t create_directory(uint32_t parent, const char *name)
{
    uint32_t ino = alloc_inode(OSFS3_S_IFDIR | 0755, NULL);
    if (!ino) return 0;
    osfs3_inode_t *inode = &inode_table[ino];
    if (ensure_blocks(inode, 1) < 0) return 0;
    uint32_t block = map_file_block(inode, 0);
    memset(block_buffer, 0, OSFS3_BLOCK_SIZE);
    osfs3_dentry_t *dot = (osfs3_dentry_t *)block_buffer;
    dot->inode = ino;
    dot->rec_len = (uint16_t)OSFS3_DIR_REC_LEN(1);
    dot->name_len = 1;
    dot->type = OSFS3_DT_DIR;
    dot->name[0] = '.';
    osfs3_dentry_t *dotdot =
        (osfs3_dentry_t *)(block_buffer + dot->rec_len);
    dotdot->inode = parent;
    dotdot->rec_len = (uint16_t)(OSFS3_DIR_BLOCK_BYTES - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->type = OSFS3_DT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    inode->size = OSFS3_BLOCK_SIZE;
    if (osfs3_write_block(device_fd, block, block_buffer) < 0 ||
        add_dentry(parent, ino, name, OSFS3_DT_DIR) < 0)
        return 0;
    imported_dirs++;
    return ino;
}

static int normalize_path(const char *path, char out[OSFS3_PATH_MAX])
{
    uint32_t write = 0;
    const char *p = path;
    while (*p == '/' || *p == '\\') p++;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            while (*p == '/' || *p == '\\') p++;
            if (*p && write) out[write++] = '/';
            continue;
        }
        if (write + 1 >= OSFS3_PATH_MAX) return -1;
        out[write++] = *p++;
    }
    if (write && out[write - 1] == '/') write--;
    out[write] = '\0';
    return 0;
}

static uint32_t resolve_path(const char *path, int create_dirs)
{
    char normalized[OSFS3_PATH_MAX];
    if (normalize_path(path, normalized) < 0) return 0;
    if (!normalized[0]) return superblock.root_inode;
    uint32_t current = superblock.root_inode;
    char *cursor = normalized;
    while (*cursor) {
        char *separator = strchr(cursor, '/');
        if (separator) *separator = '\0';
        if (!*cursor || strlen(cursor) > OSFS3_NAME_MAX) return 0;
        uint32_t next = find_in_dir(current, cursor);
        if (!next && create_dirs) next = create_directory(current, cursor);
        if (!next || (inode_table[next].mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR)
            return 0;
        current = next;
        if (!separator) break;
        cursor = separator + 1;
    }
    return current;
}

static int split_destination(const char *path, char parent[OSFS3_PATH_MAX],
                             char name[OSFS3_NAME_MAX + 1U])
{
    char normalized[OSFS3_PATH_MAX];
    if (normalize_path(path, normalized) < 0 || !normalized[0]) return -1;
    char *last = strrchr(normalized, '/');
    const char *base = last ? last + 1 : normalized;
    if (!*base || strlen(base) > OSFS3_NAME_MAX) return -1;
    strcpy(name, base);
    if (last) {
        *last = '\0';
        strcpy(parent, normalized);
    } else {
        parent[0] = '\0';
    }
    return 0;
}

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

    uint32_t ino = existing;
    if (ino && (inode_table[ino].mode & OSFS3_S_IFMT) != OSFS3_S_IFREG) {
        fprintf(stderr, "ositofs-write3: replacement is not a regular file: %s\n",
                destination);
        return -1;
    }
    if (!ino) ino = alloc_inode(OSFS3_S_IFREG | 0644, &metadata);
    if (!ino) return -1;
    osfs3_inode_t *inode = &inode_table[ino];
    uint64_t blocks64 = ((uint64_t)metadata.st_size + OSFS3_BLOCK_SIZE - 1U) >>
                        OSFS3_BLOCK_SHIFT;
    if (blocks64 > UINT32_MAX)
        return -1;

    int source = open(host_path, O_RDONLY);
    if (source < 0) return -1;
    uint32_t target_blocks = (uint32_t)blocks64;
    if (ensure_blocks(inode, target_blocks) < 0 ||
        trim_blocks(inode, target_blocks) < 0) {
        close(source);
        return -1;
    }
    uint64_t remaining = (uint64_t)metadata.st_size;
    uint32_t logical = 0;
    while (remaining) {
        size_t wanted = remaining > OSFS3_BLOCK_SIZE
            ? OSFS3_BLOCK_SIZE : (size_t)remaining;
        memset(block_buffer, 0, OSFS3_BLOCK_SIZE);
        size_t done = 0;
        while (done < wanted) {
            ssize_t amount = read(source, block_buffer + done, wanted - done);
            if (amount <= 0) {
                close(source);
                return -1;
            }
            done += (size_t)amount;
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
        remaining -= wanted;
    }
    close(source);
    inode->size = (uint64_t)metadata.st_size;
    inode->atime = (uint64_t)metadata.st_atime;
    inode->mtime = (uint64_t)metadata.st_mtime;
    inode->ctime = (uint64_t)metadata.st_ctime;
    if (!existing && add_dentry(parent, ino, name, OSFS3_DT_REG) < 0)
        return -1;
    imported_files++;
    imported_bytes += inode->size;
    if (!(imported_files % 250U))
        fprintf(stderr, "  imported %u files...\n", imported_files);
    return 0;
}

static int import_recursive(const char *host_path, const char *destination)
{
    struct stat metadata;
    if (lstat(host_path, &metadata) < 0) return -1;
    if (S_ISREG(metadata.st_mode)) return import_file(host_path, destination);
    if (!S_ISDIR(metadata.st_mode)) return 0;
    if (!resolve_path(destination, 1)) return -1;

    DIR *directory = opendir(host_path);
    if (!directory) return -1;
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        char child_host[4096];
        char child_destination[OSFS3_PATH_MAX];
        if (snprintf(child_host, sizeof(child_host), "%s/%s", host_path,
                     entry->d_name) >= (int)sizeof(child_host) ||
            snprintf(child_destination, sizeof(child_destination), "%s%s%s",
                     destination,
                     destination[0] && destination[strlen(destination) - 1] != '/'
                         ? "/" : "",
                     entry->d_name) >= (int)sizeof(child_destination) ||
            import_recursive(child_host, child_destination) < 0) {
            result = -1;
            break;
        }
    }
    closedir(directory);
    return result;
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *source = NULL;
    const char *destination = NULL;
    int recursive = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc)
            destination = argv[++i];
        else if (strcmp(argv[i], "-r") == 0)
            recursive = 1;
        else if (strcmp(argv[i], "--replace") == 0)
            replace_existing = 1;
        else if (argv[i][0] != '-' && !device)
            device = argv[i];
        else if (argv[i][0] != '-' && !source)
            source = argv[i];
        else
            usage();
    }
    if (!device || !source || !destination) usage();

    device_fd = osfs3_open_device(device, 0);
    if (device_fd < 0 || osfs3_read_super(device_fd, &superblock) < 0 ||
        load_metadata() < 0)
        return 1;
    int result = recursive
        ? import_recursive(source, destination)
        : import_file(source, destination);
    if (result == 0) result = save_metadata();
    if (result == 0) {
        fprintf(stderr,
            "Imported %u files, %u directories, %llu bytes.\n",
            imported_files, imported_dirs,
            (unsigned long long)imported_bytes);
    }
    osfs3_close_device(device_fd);
    free(inode_bitmap);
    free(block_bitmap);
    free(inode_table);
    free(block_buffer);
    return result == 0 ? 0 : 1;
}
