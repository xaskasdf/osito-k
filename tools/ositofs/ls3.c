/* List files and directories on an OsitoFS v3 volume. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_v3.h"

#define HOST_PATH_MAX 4096U

static int device_fd;
static osfs3_super_t superblock;
static osfs3_inode_t *inode_table;
static uint8_t *visited_directories;
static const char *match_text;
static uint32_t listed_entries;

static int alloc_aligned(void **out, size_t bytes)
{
    if (posix_memalign(out, 4096, bytes) != 0) return -1;
    memset(*out, 0, bytes);
    return 0;
}

static int dentry_valid(const osfs3_dentry_t *entry, uint32_t remaining)
{
    return remaining >= sizeof(*entry) && entry->rec_len >= sizeof(*entry) &&
           !(entry->rec_len & 3U) && entry->rec_len <= remaining &&
           entry->name_len <= entry->rec_len - sizeof(*entry);
}

static int inode_valid(uint32_t ino)
{
    return ino && ino < superblock.total_inodes && inode_table[ino].mode;
}

static unsigned char fold_ascii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    return c;
}

static int name_equal_ci(const char *stored, size_t stored_length,
                         const char *requested, size_t requested_length)
{
    if (stored_length != requested_length) return 0;
    for (size_t i = 0; i < stored_length; i++)
        if (fold_ascii((unsigned char)stored[i]) !=
            fold_ascii((unsigned char)requested[i]))
            return 0;
    return 1;
}

static int contains_ci(const char *text, const char *needle)
{
    if (!needle || !*needle) return 1;
    size_t text_length = strlen(text);
    size_t needle_length = strlen(needle);
    if (needle_length > text_length) return 0;
    for (size_t offset = 0; offset + needle_length <= text_length; offset++) {
        size_t index = 0;
        while (index < needle_length &&
               fold_ascii((unsigned char)text[offset + index]) ==
               fold_ascii((unsigned char)needle[index]))
            index++;
        if (index == needle_length) return 1;
    }
    return 0;
}

static int extent_valid(const osfs3_extent_t *extent)
{
    return extent->block_count &&
           extent->start_block >= superblock.first_data_block &&
           extent->start_block < superblock.total_blocks &&
           extent->block_count <=
               superblock.total_blocks - extent->start_block;
}

static uint32_t find_child(uint32_t dir_ino, const char *name)
{
    if (!inode_valid(dir_ino) || !name || !*name) return 0;
    const osfs3_inode_t *dir = &inode_table[dir_ino];
    if ((dir->mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR ||
        dir->extent_count > OSFS3_MAX_EXTENTS)
        return 0;

    void *buffer = osfs3_alloc_block();
    if (!buffer) return 0;
    uint32_t found = 0;
    size_t name_length = strlen(name);
    for (uint32_t extent_index = 0;
         extent_index < dir->extent_count && !found; extent_index++) {
        const osfs3_extent_t *extent = &dir->extents[extent_index];
        if (!extent_valid(extent)) break;
        for (uint32_t block_index = 0;
             block_index < extent->block_count && !found; block_index++) {
            if (osfs3_read_block(device_fd,
                                 extent->start_block + block_index,
                                 buffer) < 0)
                break;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                const osfs3_dentry_t *entry =
                    (const osfs3_dentry_t *)((const uint8_t *)buffer + offset);
                if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
                    break;
                if (entry->inode &&
                    name_equal_ci(entry->name, entry->name_len,
                                  name, name_length)) {
                    found = entry->inode;
                    break;
                }
                offset += entry->rec_len;
            }
        }
    }
    osfs3_free_block(buffer);
    return found;
}

static uint32_t lookup_path(const char *path)
{
    if (!path || !*path || strcmp(path, "/") == 0 ||
        strcmp(path, "\\") == 0)
        return superblock.root_inode;

    size_t length = strlen(path);
    if (length >= HOST_PATH_MAX) return 0;
    char normalized[HOST_PATH_MAX];
    memcpy(normalized, path, length + 1U);
    for (size_t i = 0; i < length; i++)
        if (normalized[i] == '\\') normalized[i] = '/';

    uint32_t current = superblock.root_inode;
    char *save = NULL;
    for (char *part = strtok_r(normalized, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        size_t part_length = strlen(part);
        if (!part_length || part_length > OSFS3_NAME_MAX ||
            strcmp(part, ".") == 0 || strcmp(part, "..") == 0)
            return 0;
        current = find_child(current, part);
        if (!current) return 0;
    }
    return current;
}

static char inode_type(const osfs3_inode_t *inode)
{
    switch (inode->mode & OSFS3_S_IFMT) {
    case OSFS3_S_IFDIR: return 'd';
    case OSFS3_S_IFREG: return 'f';
    case OSFS3_S_IFLNK: return 'l';
    default: return '?';
    }
}

static void print_entry(uint32_t ino, const char *path)
{
    if (!contains_ci(path, match_text)) return;
    const osfs3_inode_t *inode = &inode_table[ino];
    printf("%c %10llu %7u %s\n", inode_type(inode),
           (unsigned long long)inode->size, ino, path);
    listed_entries++;
}

static int join_path(char out[HOST_PATH_MAX], const char *parent,
                     const char *name)
{
    int length = strcmp(parent, "/") == 0
        ? snprintf(out, HOST_PATH_MAX, "/%s", name)
        : snprintf(out, HOST_PATH_MAX, "%s/%s", parent, name);
    return length < 0 || (uint32_t)length >= HOST_PATH_MAX ? -1 : 0;
}

static int list_directory(uint32_t ino, const char *path, int recursive,
                          uint32_t depth)
{
    if (!inode_valid(ino) || depth >= 64U || visited_directories[ino])
        return -1;
    const osfs3_inode_t *dir = &inode_table[ino];
    if ((dir->mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR ||
        dir->extent_count > OSFS3_MAX_EXTENTS)
        return -1;
    visited_directories[ino] = 1;

    void *buffer = osfs3_alloc_block();
    if (!buffer) return -1;
    int result = 0;
    for (uint32_t extent_index = 0;
         extent_index < dir->extent_count && result == 0; extent_index++) {
        const osfs3_extent_t *extent = &dir->extents[extent_index];
        if (!extent_valid(extent)) {
            result = -1;
            break;
        }
        for (uint32_t block_index = 0;
             block_index < extent->block_count && result == 0; block_index++) {
            if (osfs3_read_block(device_fd,
                                 extent->start_block + block_index,
                                 buffer) < 0) {
                result = -1;
                break;
            }
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                const osfs3_dentry_t *entry =
                    (const osfs3_dentry_t *)((const uint8_t *)buffer + offset);
                if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) {
                    result = -1;
                    break;
                }
                if (entry->inode &&
                    !(entry->name_len == 1 && entry->name[0] == '.') &&
                    !(entry->name_len == 2 && entry->name[0] == '.' &&
                      entry->name[1] == '.')) {
                    if (!inode_valid(entry->inode) || !entry->name_len ||
                        memchr(entry->name, '\0', entry->name_len) ||
                        memchr(entry->name, '/', entry->name_len) ||
                        memchr(entry->name, '\\', entry->name_len)) {
                        result = -1;
                        break;
                    }
                    char name[OSFS3_NAME_MAX + 1U];
                    memcpy(name, entry->name, entry->name_len);
                    name[entry->name_len] = '\0';
                    char child_path[HOST_PATH_MAX];
                    if (join_path(child_path, path, name) < 0) {
                        result = -1;
                        break;
                    }
                    print_entry(entry->inode, child_path);
                    if (recursive &&
                        (inode_table[entry->inode].mode & OSFS3_S_IFMT) ==
                            OSFS3_S_IFDIR &&
                        list_directory(entry->inode, child_path, 1,
                                       depth + 1U) < 0) {
                        result = -1;
                        break;
                    }
                }
                offset += entry->rec_len;
            }
        }
    }
    osfs3_free_block(buffer);
    return result;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: ositofs-ls3 <device> [--path <guest-path>] "
        "[-R|--recursive] [--match <text>]\n");
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *path = "/";
    int recursive = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc)
            path = argv[++i];
        else if ((strcmp(argv[i], "-R") == 0 ||
                  strcmp(argv[i], "--recursive") == 0))
            recursive = 1;
        else if (strcmp(argv[i], "--match") == 0 && i + 1 < argc)
            match_text = argv[++i];
        else if (argv[i][0] != '-' && !device)
            device = argv[i];
        else {
            usage();
            return 1;
        }
    }
    if (!device) {
        usage();
        return 1;
    }

    device_fd = osfs3_open_device(device, 1);
    if (device_fd < 0 || osfs3_read_super(device_fd, &superblock) < 0)
        return 1;
    size_t inode_bytes =
        (size_t)OSFS3_INODE_TABLE_BLOCKS(superblock.total_inodes) *
        OSFS3_BLOCK_SIZE;
    if (alloc_aligned((void **)&inode_table, inode_bytes) < 0 ||
        alloc_aligned((void **)&visited_directories,
                      (superblock.total_inodes + 4095U) & ~4095U) < 0 ||
        osfs3_read_bytes(device_fd,
            (uint64_t)OSFS3_INODE_TABLE_BLK * OSFS3_BLOCK_SIZE,
            inode_table, inode_bytes) < 0)
        return 1;

    uint32_t ino = lookup_path(path);
    if (!ino || !inode_valid(ino)) {
        fprintf(stderr, "ositofs-ls3: path not found: %s\n", path);
        return 1;
    }

    int result = 0;
    if ((inode_table[ino].mode & OSFS3_S_IFMT) == OSFS3_S_IFDIR)
        result = list_directory(ino, path, recursive, 0);
    else
        print_entry(ino, path);

    if (result < 0)
        fprintf(stderr, "ositofs-ls3: invalid directory data under %s\n", path);
    else
        fprintf(stderr, "ositofs-ls3: %u matching entr%s\n", listed_entries,
                listed_entries == 1 ? "y" : "ies");

    osfs3_close_device(device_fd);
    free(inode_table);
    free(visited_directories);
    return result < 0 ? 1 : 0;
}
