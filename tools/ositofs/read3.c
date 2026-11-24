/* Recursive OsitoFS v3 extractor. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common_v3.h"

#define HOST_PATH_MAX 4096U

static int device_fd;
static osfs3_super_t superblock;
static osfs3_inode_t *inode_table;
static uint8_t *visited;
static uint32_t extracted_files;
static uint32_t extracted_directories;
static uint64_t extracted_bytes;

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
    if (!ino || ino >= superblock.total_inodes) return 0;
    uint16_t type = inode_table[ino].mode & OSFS3_S_IFMT;
    return type == OSFS3_S_IFREG || type == OSFS3_S_IFDIR;
}

static int name_equal_ci(const char *stored, size_t stored_length,
                         const char *requested, size_t requested_length)
{
    if (stored_length != requested_length) return 0;
    for (size_t i = 0; i < stored_length; i++) {
        unsigned char a = (unsigned char)stored[i];
        unsigned char b = (unsigned char)requested[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

static uint32_t map_file_block(const osfs3_inode_t *inode, uint32_t logical)
{
    if (inode->extent_count > OSFS3_MAX_EXTENTS) return 0;
    for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
        if (logical < inode->extents[extent].block_count)
            return inode->extents[extent].start_block + logical;
        logical -= inode->extents[extent].block_count;
    }
    return 0;
}

static int make_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    if (errno != EEXIST) {
        fprintf(stderr, "ositofs-read3: mkdir %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    struct stat metadata;
    if (stat(path, &metadata) < 0 || !S_ISDIR(metadata.st_mode)) {
        fprintf(stderr, "ositofs-read3: output path is not a directory: %s\n",
                path);
        return -1;
    }
    return 0;
}

static int join_path(char out[HOST_PATH_MAX], const char *parent,
                     const char *name)
{
    int length = snprintf(out, HOST_PATH_MAX, "%s%s%s", parent,
                          parent[0] && parent[strlen(parent) - 1] != '/' ? "/" : "",
                          name);
    return length < 0 || (uint32_t)length >= HOST_PATH_MAX ? -1 : 0;
}

static uint32_t find_child(uint32_t dir_ino, const char *name)
{
    if (!inode_valid(dir_ino) || !name || !*name) return 0;
    osfs3_inode_t *dir = &inode_table[dir_ino];
    if ((dir->mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR) return 0;

    void *buffer = osfs3_alloc_block();
    if (!buffer) return 0;
    uint32_t found = 0;
    size_t name_length = strlen(name);
    for (uint32_t extent = 0; extent < dir->extent_count && !found;
         extent++) {
        uint32_t start = dir->extents[extent].start_block;
        uint32_t count = dir->extents[extent].block_count;
        if (!count || start < superblock.first_data_block ||
            start >= superblock.total_blocks ||
            count > superblock.total_blocks - start)
            break;
        for (uint32_t index = 0; index < count && !found; index++) {
            if (osfs3_read_block(device_fd, start + index, buffer) < 0)
                break;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)((uint8_t *)buffer + offset);
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

static uint32_t lookup_path(const char *path, char basename[OSFS3_NAME_MAX + 1U])
{
    if (!path || !*path || strcmp(path, "/") == 0 ||
        strcmp(path, "\\") == 0) {
        basename[0] = '\0';
        return superblock.root_inode;
    }

    size_t length = strlen(path);
    if (length >= OSFS3_PATH_MAX) return 0;
    char normalized[OSFS3_PATH_MAX];
    memcpy(normalized, path, length + 1U);
    for (size_t i = 0; i < length; i++)
        if (normalized[i] == '\\') normalized[i] = '/';

    uint32_t current = superblock.root_inode;
    char *save = NULL;
    char *part = strtok_r(normalized, "/", &save);
    basename[0] = '\0';
    while (part) {
        size_t part_length = strlen(part);
        if (!part_length || part_length > OSFS3_NAME_MAX ||
            strcmp(part, ".") == 0 || strcmp(part, "..") == 0)
            return 0;
        current = find_child(current, part);
        if (!current) return 0;
        memcpy(basename, part, part_length + 1U);
        part = strtok_r(NULL, "/", &save);
    }
    return current;
}

static int extract_file(uint32_t ino, const char *path)
{
    osfs3_inode_t *inode = &inode_table[ino];
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output < 0) {
        fprintf(stderr, "ositofs-read3: create %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    void *buffer = osfs3_alloc_block();
    if (!buffer) {
        close(output);
        return -1;
    }
    uint64_t remaining = inode->size;
    uint32_t logical = 0;
    int result = 0;
    while (remaining) {
        uint32_t block = map_file_block(inode, logical++);
        if (!block || block < superblock.first_data_block ||
            block >= superblock.total_blocks ||
            osfs3_read_block(device_fd, block, buffer) < 0) {
            fprintf(stderr, "ositofs-read3: invalid data block for inode %u\n",
                    ino);
            result = -1;
            break;
        }
        size_t amount = remaining > OSFS3_BLOCK_SIZE
            ? OSFS3_BLOCK_SIZE : (size_t)remaining;
        size_t done = 0;
        while (done < amount) {
            ssize_t written = write(output, (uint8_t *)buffer + done,
                                    amount - done);
            if (written <= 0) {
                fprintf(stderr, "ositofs-read3: write %s: %s\n",
                        path, strerror(errno));
                result = -1;
                break;
            }
            done += (size_t)written;
        }
        if (result < 0) break;
        remaining -= amount;
    }
    if (close(output) < 0) result = -1;
    osfs3_free_block(buffer);
    if (result == 0) {
        extracted_files++;
        extracted_bytes += inode->size;
    }
    return result;
}

static int extract_node(uint32_t ino, const char *path, uint32_t depth)
{
    if (!inode_valid(ino) || depth >= 64 || visited[ino]) {
        fprintf(stderr, "ositofs-read3: invalid or cyclic inode %u\n", ino);
        return -1;
    }
    visited[ino] = 1;
    osfs3_inode_t *inode = &inode_table[ino];
    if ((inode->mode & OSFS3_S_IFMT) == OSFS3_S_IFREG)
        return extract_file(ino, path);
    if (make_directory(path) < 0) return -1;
    extracted_directories++;

    void *buffer = osfs3_alloc_block();
    if (!buffer) return -1;
    int result = 0;
    for (uint32_t extent = 0; extent < inode->extent_count && result == 0;
         extent++) {
        uint32_t start = inode->extents[extent].start_block;
        uint32_t count = inode->extents[extent].block_count;
        if (!count || start < superblock.first_data_block ||
            start >= superblock.total_blocks ||
            count > superblock.total_blocks - start) {
            result = -1;
            break;
        }
        for (uint32_t index = 0; index < count && result == 0; index++) {
            uint32_t block = start + index;
            if (osfs3_read_block(device_fd, block, buffer) < 0) {
                result = -1;
                break;
            }
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)((uint8_t *)buffer + offset);
                if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) {
                    result = -1;
                    break;
                }
                if (entry->inode &&
                    !(entry->name_len == 1 && entry->name[0] == '.') &&
                    !(entry->name_len == 2 && entry->name[0] == '.' &&
                      entry->name[1] == '.')) {
                    if (!entry->name_len ||
                        memchr(entry->name, '\0', entry->name_len) ||
                        memchr(entry->name, '/', entry->name_len) ||
                        memchr(entry->name, '\\', entry->name_len)) {
                        fprintf(stderr,
                            "ositofs-read3: unsafe name in directory inode %u\n",
                            ino);
                        result = -1;
                        break;
                    }
                    char name[OSFS3_NAME_MAX + 1U];
                    memcpy(name, entry->name, entry->name_len);
                    name[entry->name_len] = '\0';
                    char child[HOST_PATH_MAX];
                    if (join_path(child, path, name) < 0 ||
                        extract_node(entry->inode, child, depth + 1U) < 0) {
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

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *output = NULL;
    const char *input_path = "/";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (argv[i][0] != '-' && !device)
            device = argv[i];
        else {
            fprintf(stderr,
                    "Usage: ositofs-read3 <device> --output-dir <directory> "
                    "[--path <guest-path>]\n");
            return 1;
        }
    }
    if (!device || !output) {
        fprintf(stderr,
                "Usage: ositofs-read3 <device> --output-dir <directory> "
                "[--path <guest-path>]\n");
        return 1;
    }

    device_fd = osfs3_open_device(device, 1);
    if (device_fd < 0 || osfs3_read_super(device_fd, &superblock) < 0)
        return 1;
    size_t inode_bytes =
        (size_t)OSFS3_INODE_TABLE_BLOCKS(superblock.total_inodes) *
        OSFS3_BLOCK_SIZE;
    if (alloc_aligned((void **)&inode_table, inode_bytes) < 0 ||
        alloc_aligned((void **)&visited,
                      (superblock.total_inodes + 4095U) & ~4095U) < 0 ||
        osfs3_read_bytes(device_fd,
            (uint64_t)OSFS3_INODE_TABLE_BLK * OSFS3_BLOCK_SIZE,
            inode_table, inode_bytes) < 0)
        return 1;

    char basename[OSFS3_NAME_MAX + 1U];
    uint32_t input_ino = lookup_path(input_path, basename);
    if (!input_ino) {
        fprintf(stderr, "ositofs-read3: path not found: %s\n", input_path);
        return 1;
    }

    char destination[HOST_PATH_MAX];
    int result;
    if (!basename[0]) {
        if (snprintf(destination, sizeof(destination), "%s", output) < 0)
            return 1;
    } else {
        if (make_directory(output) < 0 ||
            join_path(destination, output, basename) < 0)
            return 1;
    }
    result = extract_node(input_ino, destination, 0);
    if (result == 0) {
        printf("Extracted %u files, %u directories, %llu bytes to %s\n",
               extracted_files, extracted_directories,
               (unsigned long long)extracted_bytes, output);
    }
    osfs3_close_device(device_fd);
    free(inode_table);
    free(visited);
    return result == 0 ? 0 : 1;
}
