/* Structural checker and conservative repair tool for OsitoFS v3. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common_v3.h"

#define FSCK_PATH_MAX 4096U

static int device_fd;
static osfs3_super_t superblock;
static uint8_t *inode_bitmap;
static uint8_t *block_bitmap;
static uint8_t *reachable;
static uint8_t *claimed_blocks;
static uint8_t *raw_claimed_blocks;
static osfs3_inode_t *inode_table;
static uint32_t errors;
static uint32_t fatal_errors;
static uint32_t repairs;
static uint32_t file_count;
static uint32_t directory_count;
static uint64_t file_bytes;
static int repair_mode;

static int bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static void bitmap_clear(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

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

static void report(const char *message, uint32_t value)
{
    fprintf(stderr, "fsck.ositofs3: %s: %u\n", message, value);
    errors++;
}

static void report_path(const char *message, uint32_t value,
                        const char *path)
{
    fprintf(stderr, "fsck.ositofs3: %s: %u (%s)\n",
            message, value, path ? path : "?");
    errors++;
}

static void report_fatal(const char *message, uint32_t value,
                         const char *path)
{
    report_path(message, value, path);
    fatal_errors++;
}

static uint32_t inode_blocks(const osfs3_inode_t *inode)
{
    uint32_t count = 0;
    if (inode->extent_count > OSFS3_MAX_EXTENTS) return 0;
    for (uint32_t i = 0; i < inode->extent_count; i++)
        count += inode->extents[i].block_count;
    return count;
}

static int inode_body_valid(uint32_t ino)
{
    if (!ino || ino >= superblock.total_inodes) return 0;
    osfs3_inode_t *inode = &inode_table[ino];
    uint16_t type = inode->mode & OSFS3_S_IFMT;
    if (type != OSFS3_S_IFREG && type != OSFS3_S_IFDIR) return 0;
    if (inode->extent_count > OSFS3_MAX_EXTENTS) return 0;

    uint64_t capacity = 0;
    for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
        uint32_t start = inode->extents[extent].start_block;
        uint32_t count = inode->extents[extent].block_count;
        if (!count || start < superblock.first_data_block ||
            start >= superblock.total_blocks ||
            count > superblock.total_blocks - start)
            return 0;
        capacity += (uint64_t)count * OSFS3_BLOCK_SIZE;
    }
    return inode->size <= capacity;
}

static unsigned char path_fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int dentry_name_equal_ci(const osfs3_dentry_t *entry,
                                const char *name, size_t name_length)
{
    if (entry->name_len != name_length) return 0;
    for (size_t i = 0; i < name_length; i++) {
        if (path_fold((unsigned char)entry->name[i]) !=
            path_fold((unsigned char)name[i]))
            return 0;
    }
    return 1;
}

static uint32_t find_child_ci(uint32_t dir_ino, const char *name,
                              size_t name_length)
{
    if (!inode_body_valid(dir_ino) || !name_length ||
        name_length > OSFS3_NAME_MAX)
        return 0;
    osfs3_inode_t *dir = &inode_table[dir_ino];
    if ((dir->mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR) return 0;

    void *buffer = osfs3_alloc_block();
    if (!buffer) return 0;
    uint32_t found = 0;
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
                    dentry_name_equal_ci(entry, name, name_length)) {
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

static uint32_t resolve_path_ci(const char *path)
{
    if (!path || !*path) return 0;
    const char *cursor = path;
    while (*cursor == '/' || *cursor == '\\') cursor++;
    if (!*cursor) return superblock.root_inode;

    uint32_t current = superblock.root_inode;
    while (*cursor) {
        const char *component = cursor;
        while (*cursor && *cursor != '/' && *cursor != '\\') cursor++;
        size_t length = (size_t)(cursor - component);
        if (!length || length > OSFS3_NAME_MAX ||
            (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.'))
            return 0;
        current = find_child_ci(current, component, length);
        if (!current) return 0;
        while (*cursor == '/' || *cursor == '\\') cursor++;
    }
    return current;
}

/* Clear a directory atomically at the namespace boundary. The normal fsck
 * reachability pass below then reclaims every detached inode and data block.
 * This is intended for offline cleanup of crash-only trees such as /Temp. */
static int prune_directory_contents(const char *path)
{
    uint32_t ino = resolve_path_ci(path);
    if (!ino || ino == superblock.root_inode || !inode_body_valid(ino) ||
        (inode_table[ino].mode & OSFS3_S_IFMT) != OSFS3_S_IFDIR) {
        fprintf(stderr, "fsck.ositofs3: prune target is not a directory: %s\n",
                path);
        return -1;
    }

    osfs3_inode_t *dir = &inode_table[ino];
    void *buffer = osfs3_alloc_block();
    if (!buffer) return -1;
    uint32_t detached = 0;
    int result = 0;
    for (uint32_t extent = 0; extent < dir->extent_count && result == 0;
         extent++) {
        uint32_t start = dir->extents[extent].start_block;
        uint32_t count = dir->extents[extent].block_count;
        if (!count || start < superblock.first_data_block ||
            start >= superblock.total_blocks ||
            count > superblock.total_blocks - start) {
            result = -1;
            break;
        }
        for (uint32_t index = 0; index < count; index++) {
            uint32_t block = start + index;
            if (osfs3_read_block(device_fd, block, buffer) < 0) {
                result = -1;
                break;
            }
            int modified = 0;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)((uint8_t *)buffer + offset);
                if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) {
                    result = -1;
                    break;
                }
                int is_dot = entry->name_len == 1 && entry->name[0] == '.';
                int is_dotdot = entry->name_len == 2 &&
                                entry->name[0] == '.' && entry->name[1] == '.';
                if (entry->inode && !is_dot && !is_dotdot) {
                    entry->inode = 0;
                    entry->type = OSFS3_DT_UNKNOWN;
                    detached++;
                    modified = 1;
                }
                offset += entry->rec_len;
            }
            if (result == 0 && modified &&
                osfs3_write_block(device_fd, block, buffer) < 0)
                result = -1;
            if (result < 0) break;
        }
    }
    osfs3_free_block(buffer);
    if (result < 0) {
        fprintf(stderr, "fsck.ositofs3: failed to prune directory: %s\n",
                path);
        return -1;
    }
    if (detached) repairs++;
    printf("fsck.ositofs3: detached %u entr%s from %s\n", detached,
           detached == 1 ? "y" : "ies", path);
    return 0;
}

static void build_raw_claim_map(void)
{
    for (uint32_t block = 0; block < superblock.first_data_block; block++)
        bitmap_set(raw_claimed_blocks, block);
    for (uint32_t ino = 1; ino < superblock.total_inodes; ino++) {
        if (!inode_body_valid(ino)) continue;
        osfs3_inode_t *inode = &inode_table[ino];
        for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
            uint32_t start = inode->extents[extent].start_block;
            uint32_t count = inode->extents[extent].block_count;
            for (uint32_t offset = 0; offset < count; offset++)
                bitmap_set(raw_claimed_blocks, start + offset);
        }
    }
}

static int lost_directory_block_valid(const void *buffer, uint32_t ino,
                                      uint32_t parent)
{
    const uint8_t *block = buffer;
    uint32_t offset = 0;
    while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
        const osfs3_dentry_t *entry =
            (const osfs3_dentry_t *)(block + offset);
        if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) return 0;
        if (offset == 0) {
            if (entry->inode != ino ||
                entry->rec_len != OSFS3_DIR_REC_LEN(1) ||
                entry->name_len != 1 || entry->type != OSFS3_DT_DIR ||
                entry->name[0] != '.')
                return 0;
        } else if (offset == OSFS3_DIR_REC_LEN(1)) {
            if (entry->inode != parent || entry->name_len != 2 ||
                entry->type != OSFS3_DT_DIR || entry->name[0] != '.' ||
                entry->name[1] != '.')
                return 0;
        }
        offset += entry->rec_len;
    }
    return offset == OSFS3_DIR_BLOCK_BYTES;
}

static int recover_lost_directory(uint32_t ino, uint32_t parent,
                                  const char *path)
{
    if (!ino || ino >= superblock.total_inodes || inode_body_valid(ino))
        return 0;

    void *buffer = osfs3_alloc_block();
    if (!buffer) return 0;
    uint32_t candidate = 0;
    for (uint32_t block = superblock.first_data_block;
         block < superblock.total_blocks; block++) {
        if (!bitmap_test(block_bitmap, block) ||
            bitmap_test(raw_claimed_blocks, block))
            continue;
        if (osfs3_read_block(device_fd, block, buffer) < 0) {
            fatal_errors++;
            osfs3_free_block(buffer);
            return 0;
        }
        if (!lost_directory_block_valid(buffer, ino, parent)) continue;
        if (candidate) {
            report_fatal("ambiguous lost directory", ino, path);
            osfs3_free_block(buffer);
            return 0;
        }
        candidate = block;
    }
    osfs3_free_block(buffer);
    if (!candidate) return 0;

    report_path("lost directory inode is recoverable", ino, path);
    osfs3_inode_t *inode = &inode_table[ino];
    memset(inode, 0, sizeof(*inode));
    inode->mode = OSFS3_S_IFDIR | 0755;
    inode->nlink = 2;
    inode->size = OSFS3_BLOCK_SIZE;
    inode->atime = inode->mtime = inode->ctime = superblock.create_time;
    inode->extent_count = 1;
    inode->extents[0].start_block = candidate;
    inode->extents[0].block_count = 1;
    bitmap_set(inode_bitmap, ino);
    bitmap_set(raw_claimed_blocks, candidate);
    if (repair_mode) {
        fprintf(stderr,
                "fsck.ositofs3: recovering directory %u at block %u (%s)\n",
                ino, candidate, path);
        repairs++;
    }
    return 1;
}

static int claim_inode_blocks(uint32_t ino)
{
    osfs3_inode_t *inode = &inode_table[ino];
    if (inode->extent_count > OSFS3_MAX_EXTENTS) {
        report("too many extents on inode", ino);
        fatal_errors++;
        return -1;
    }
    for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
        uint32_t start = inode->extents[extent].start_block;
        uint32_t count = inode->extents[extent].block_count;
        if (!count || start < superblock.first_data_block ||
            start >= superblock.total_blocks ||
            count > superblock.total_blocks - start) {
            report("invalid extent on inode", ino);
            fatal_errors++;
            return -1;
        }
        for (uint32_t block = start; block < start + count; block++) {
            if (!bitmap_test(block_bitmap, block))
                report("extent block missing from bitmap", block);
            if (bitmap_test(claimed_blocks, block)) {
                report("data block claimed twice", block);
                fatal_errors++;
            }
            bitmap_set(claimed_blocks, block);
        }
    }
    uint64_t capacity = (uint64_t)inode_blocks(inode) * OSFS3_BLOCK_SIZE;
    if (inode->size > capacity) {
        report("inode size exceeds extents", ino);
        fatal_errors++;
    }
    return 0;
}

static void check_inode(uint32_t ino, uint32_t expected_parent,
                        uint32_t depth, const char *path)
{
    if (!ino || ino >= superblock.total_inodes) {
        report_fatal("directory references invalid inode", ino, path);
        return;
    }
    if (!inode_body_valid(ino)) {
        report_fatal("unsupported or damaged inode", ino, path);
        return;
    }
    if (!bitmap_test(inode_bitmap, ino)) {
        report_path("reachable inode missing from bitmap", ino, path);
        if (repair_mode) {
            bitmap_set(inode_bitmap, ino);
            repairs++;
        }
    }
    if (bitmap_test(reachable, ino)) return;
    bitmap_set(reachable, ino);

    osfs3_inode_t *inode = &inode_table[ino];
    uint16_t type = inode->mode & OSFS3_S_IFMT;
    if (claim_inode_blocks(ino) < 0) return;
    if (type == OSFS3_S_IFREG) {
        file_count++;
        file_bytes += inode->size;
        return;
    }
    directory_count++;
    if (depth >= 64) {
        report_fatal("directory nesting exceeds limit", ino, path);
        return;
    }

    void *buffer = osfs3_alloc_block();
    if (!buffer) {
        report_fatal("cannot allocate directory buffer", ino, path);
        return;
    }
    int found_dot = 0;
    int found_dotdot = 0;
    for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
        for (uint32_t index = 0; index < inode->extents[extent].block_count;
             index++) {
            uint32_t block = inode->extents[extent].start_block + index;
            if (osfs3_read_block(device_fd, block, buffer) < 0) {
                report_fatal("cannot read directory block", block, path);
                continue;
            }
            int modified = 0;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)((uint8_t *)buffer + offset);
                if (!dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) {
                    report_fatal("invalid directory record in block",
                                 block, path);
                    break;
                }
                if (!entry->inode) {
                    offset += entry->rec_len;
                    continue;
                }

                int is_dot = entry->name_len == 1 && entry->name[0] == '.';
                int is_dotdot = entry->name_len == 2 && entry->name[0] == '.' &&
                                entry->name[1] == '.';
                if (is_dot) {
                    found_dot++;
                    if (entry->inode != ino || entry->type != OSFS3_DT_DIR) {
                        report_path("bad dot entry", ino, path);
                        if (repair_mode) {
                            entry->inode = ino;
                            entry->type = OSFS3_DT_DIR;
                            modified = 1;
                            repairs++;
                        }
                    }
                } else if (is_dotdot) {
                    found_dotdot++;
                    if (entry->inode != expected_parent ||
                        entry->type != OSFS3_DT_DIR) {
                        report_path("bad dotdot entry", ino, path);
                        if (repair_mode) {
                            entry->inode = expected_parent;
                            entry->type = OSFS3_DT_DIR;
                            modified = 1;
                            repairs++;
                        }
                    }
                } else {
                    int safe_name = entry->name_len != 0;
                    for (uint32_t c = 0; c < entry->name_len; c++) {
                        if (!entry->name[c] || entry->name[c] == '/' ||
                            entry->name[c] == '\\')
                            safe_name = 0;
                    }
                    char child_path[FSCK_PATH_MAX];
                    int length = -1;
                    if (safe_name) {
                        if (strcmp(path, "/") == 0)
                            length = snprintf(child_path, sizeof(child_path),
                                              "/%.*s",
                                              (int)entry->name_len,
                                              entry->name);
                        else
                            length = snprintf(child_path, sizeof(child_path),
                                              "%s/%.*s", path,
                                              (int)entry->name_len,
                                              entry->name);
                    }
                    if (!safe_name || length < 0 ||
                        (uint32_t)length >= sizeof(child_path)) {
                        report_path("unsafe directory name", ino, path);
                        if (repair_mode) {
                            entry->inode = 0;
                            entry->type = OSFS3_DT_UNKNOWN;
                            modified = 1;
                            repairs++;
                        } else {
                            fatal_errors++;
                        }
                        offset += entry->rec_len;
                        continue;
                    }

                    uint32_t child = entry->inode;
                    if ((child >= superblock.total_inodes ||
                         !inode_body_valid(child)) &&
                        !recover_lost_directory(child, ino, child_path)) {
                        report_path("dangling directory entry", child,
                                    child_path);
                        if (repair_mode) {
                            entry->inode = 0;
                            entry->type = OSFS3_DT_UNKNOWN;
                            modified = 1;
                            repairs++;
                        } else {
                            fatal_errors++;
                        }
                    } else {
                        uint8_t expected_type =
                            (inode_table[child].mode & OSFS3_S_IFMT) ==
                                    OSFS3_S_IFDIR
                                ? OSFS3_DT_DIR : OSFS3_DT_REG;
                        if (entry->type != expected_type) {
                            report_path("directory entry type mismatch",
                                        child, child_path);
                            if (repair_mode) {
                                entry->type = expected_type;
                                modified = 1;
                                repairs++;
                            }
                        }
                        check_inode(child, ino, depth + 1U, child_path);
                    }
                }
                offset += entry->rec_len;
            }
            if (modified &&
                osfs3_write_block(device_fd, block, buffer) < 0)
                report_fatal("cannot write repaired directory block",
                             block, path);
        }
    }
    if (found_dot != 1)
        report_fatal("directory dot count is not one", ino, path);
    if (found_dotdot != 1)
        report_fatal("directory dotdot count is not one", ino, path);
    osfs3_free_block(buffer);
}

static int persist_repairs(size_t inode_bytes)
{
    if (fsync(device_fd) < 0) return -1;
    if (osfs3_write_bytes(device_fd,
            (uint64_t)OSFS3_INODE_TABLE_BLK * OSFS3_BLOCK_SIZE,
            inode_table, inode_bytes) < 0 || fsync(device_fd) < 0)
        return -1;
    if (osfs3_write_block(device_fd, OSFS3_INODE_BITMAP_BLK,
                          inode_bitmap) < 0 ||
        osfs3_write_block(device_fd, OSFS3_BLOCK_BITMAP_BLK,
                          block_bitmap) < 0)
        return -1;

    void *super_block = osfs3_alloc_block();
    if (!super_block) return -1;
    int result = osfs3_read_block(
        device_fd, OSFS3_SUPERBLOCK_BLK, super_block);
    if (!result) {
        superblock.crc32 = 0;
        superblock.crc32 = osfs3_crc32(&superblock, sizeof(superblock));
        memcpy(super_block, &superblock, sizeof(superblock));
        result = osfs3_write_block(
            device_fd, OSFS3_SUPERBLOCK_BLK, super_block);
    }
    osfs3_free_block(super_block);
    return result < 0 || fsync(device_fd) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *prune_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repair") == 0)
            repair_mode = 1;
        else if (strcmp(argv[i], "--prune") == 0 && i + 1 < argc)
            prune_path = argv[++i];
        else if (argv[i][0] != '-' && !device)
            device = argv[i];
        else {
            fprintf(stderr,
                    "Usage: ositofs-fsck3 [--repair] [--prune <directory>] "
                    "<device>\n");
            return 1;
        }
    }
    if (!device || (prune_path && !repair_mode)) {
        fprintf(stderr,
                "Usage: ositofs-fsck3 [--repair] [--prune <directory>] "
                "<device>\n");
        return 1;
    }
    device_fd = osfs3_open_device(device, !repair_mode);
    if (device_fd < 0 || osfs3_read_super(device_fd, &superblock) < 0)
        return 1;

    uint32_t inode_blocks_count =
        OSFS3_INODE_TABLE_BLOCKS(superblock.total_inodes);
    size_t inode_bytes = (size_t)inode_blocks_count * OSFS3_BLOCK_SIZE;
    size_t inode_bitmap_bytes = (superblock.total_inodes + 7U) / 8U;
    if (alloc_aligned((void **)&inode_bitmap, OSFS3_BLOCK_SIZE) < 0 ||
        alloc_aligned((void **)&block_bitmap, OSFS3_BLOCK_SIZE) < 0 ||
        alloc_aligned((void **)&inode_table, inode_bytes) < 0 ||
        alloc_aligned((void **)&reachable,
                      (inode_bitmap_bytes + 4095U) & ~4095U) < 0 ||
        alloc_aligned((void **)&claimed_blocks, OSFS3_BLOCK_SIZE) < 0 ||
        alloc_aligned((void **)&raw_claimed_blocks, OSFS3_BLOCK_SIZE) < 0)
        return 1;
    if (osfs3_read_block(device_fd, OSFS3_INODE_BITMAP_BLK,
                         inode_bitmap) < 0 ||
        osfs3_read_block(device_fd, OSFS3_BLOCK_BITMAP_BLK,
                         block_bitmap) < 0 ||
        osfs3_read_bytes(device_fd,
            (uint64_t)OSFS3_INODE_TABLE_BLK * OSFS3_BLOCK_SIZE,
            inode_table, inode_bytes) < 0)
        return 1;

    if (prune_path && prune_directory_contents(prune_path) < 0) return 1;

    build_raw_claim_map();
    for (uint32_t block = 0; block < superblock.first_data_block; block++) {
        if (!bitmap_test(block_bitmap, block))
            report("metadata block missing from bitmap", block);
        bitmap_set(claimed_blocks, block);
    }
    check_inode(superblock.root_inode, superblock.root_inode, 0, "/");

    uint32_t used_inodes = 1;
    int inode_map_changed = !bitmap_test(inode_bitmap, 0);
    if (repair_mode) bitmap_set(inode_bitmap, 0);
    for (uint32_t ino = 0; ino < superblock.total_inodes; ino++) {
        if (!ino) continue;
        int allocated = bitmap_test(inode_bitmap, ino);
        int should_be_allocated = bitmap_test(reachable, ino);
        if (allocated && !should_be_allocated) {
            report("allocated inode is unreachable", ino);
            inode_map_changed = 1;
            if (repair_mode) {
                memset(&inode_table[ino], 0, sizeof(inode_table[ino]));
                bitmap_clear(inode_bitmap, ino);
                repairs++;
            }
        } else if (!allocated && should_be_allocated) {
            report("reachable inode is free", ino);
            inode_map_changed = 1;
            if (repair_mode) bitmap_set(inode_bitmap, ino);
        }
        if (should_be_allocated) used_inodes++;
    }
    uint32_t used_blocks = 0;
    int block_map_changed = 0;
    for (uint32_t block = 0; block < superblock.total_blocks; block++) {
        int allocated = bitmap_test(block_bitmap, block);
        int should_be_allocated = bitmap_test(claimed_blocks, block);
        if (should_be_allocated) used_blocks++;
        if (allocated != should_be_allocated) {
            report(allocated ? "allocated block is unclaimed"
                             : "claimed block is free", block);
            block_map_changed = 1;
        }
    }
    if (repair_mode && block_map_changed) {
        memcpy(block_bitmap, claimed_blocks, OSFS3_BLOCK_SIZE);
        repairs++;
    }
    if (repair_mode && inode_map_changed) repairs++;

    uint32_t expected_free_inodes = superblock.total_inodes - used_inodes;
    uint32_t expected_free_blocks = superblock.total_blocks - used_blocks;
    if (superblock.free_inodes != expected_free_inodes)
        report("superblock free inode count mismatch", superblock.free_inodes);
    if (superblock.free_blocks != expected_free_blocks)
        report("superblock free block count mismatch", superblock.free_blocks);
    if (repair_mode) {
        if (superblock.free_inodes != expected_free_inodes ||
            superblock.free_blocks != expected_free_blocks)
            repairs++;
        superblock.free_inodes = expected_free_inodes;
        superblock.free_blocks = expected_free_blocks;
    }

    if (repair_mode && repairs && !fatal_errors &&
        persist_repairs(inode_bytes) < 0) {
        fprintf(stderr, "fsck.ositofs3: failed to persist repairs\n");
        fatal_errors++;
    }

    printf("OsitoFS v3: %u files, %u directories, %llu bytes, "
           "%u free blocks, %u free inodes\n",
           file_count, directory_count, (unsigned long long)file_bytes,
           superblock.free_blocks, superblock.free_inodes);
    printf("fsck.ositofs3: %s (%u error%s)\n",
           fatal_errors ? "FAILED" :
           (repair_mode && repairs ? "repaired" :
            (errors ? "repairable" : "clean")),
           errors, errors == 1 ? "" : "s");
    if (repair_mode)
        printf("fsck.ositofs3: %u repair%s applied\n",
               repairs, repairs == 1 ? "" : "s");
    osfs3_close_device(device_fd);
    return fatal_errors || (!repair_mode && errors) ? 1 : 0;
}
