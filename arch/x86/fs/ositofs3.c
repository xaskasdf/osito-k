/*
 * OsitoFS v3 bare-metal driver.
 *
 * The complete inode table, path index, and allocation bitmaps are cached at
 * mount time. File data remains on disk and is addressed through direct
 * extents. Metadata mutations are serialized because Win32 exposes stable
 * inode pointers as file objects.
 */

#include "ositofs3.h"
#include "../include/paging.h"
#include "../../../include/common/ositofs3_format.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t value);

extern int disk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
extern int disk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len);
extern int disk_flush(void);
extern uint32_t disk_lba_size(void);
extern uint64_t disk_lba_count(void);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void mem_free_pages(void *addr, uint64_t count);
extern uint32_t ntp_get_utc(void) __attribute__((weak));
extern bool syscall_file_is_mapped(void *file) __attribute__((weak));

#define OSFS3_HASH_EMPTY UINT32_MAX
#define OSFS3_MAX_DEPTH  64U

typedef struct {
    uint32_t block;
    uint32_t offset;
    uint32_t previous_offset;
    bool has_previous;
} osfs3_dentry_location_t;

typedef struct {
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t next_sibling;
    uint32_t prev_sibling;
    uint32_t generation;
} osfs3_index_node_t;

static uint64_t partition_offset;
static osfs3_super_t superblock;
static bool mounted;

static void *inode_bitmap_phys;
static void *block_bitmap_phys;
static void *inode_table_phys;
static void *path_table_phys;
static void *path_known_phys;
static void *path_hash_phys;
static void *index_nodes_phys;
static void *open_refs_phys;
static void *file_revisions_phys;
static void *scratch_phys;

static uint8_t *inode_bitmap;
static uint8_t *block_bitmap;
static osfs3_inode_t *inode_table;
static char *path_table;
static uint8_t *path_known;
static uint32_t *path_hash;
static osfs3_index_node_t *index_nodes;
static uint32_t *file_open_refs;
static uint64_t *file_revisions;
static uint8_t *scratch_block;
static uint32_t path_hash_slots;
static uint32_t path_hash_mask;
static uint32_t regular_file_count;
static volatile uint32_t mutation_lock;
static uint64_t mutation_lock_irq_flags;
static bool path_index_ready;

static void osfs3_trace_create_failure(const char *path, const char *stage)
{
    static uint32_t logs;
    uint32_t index = __atomic_fetch_add(&logs, 1, __ATOMIC_RELAXED);
    if (index >= 128) return;
    serial_puts("[OSFS3-CREATE-FAIL] stage=");
    serial_puts(stage);
    serial_puts(" path='");
    serial_puts(path ? path : "<null>");
    serial_puts("' free_inodes=");
    serial_putdec(superblock.free_inodes);
    serial_puts(" free_blocks=");
    serial_putdec(superblock.free_blocks);
    serial_puts("\n");
}

static int osfs3_rebuild_paths_nolock(void);
static int osfs3_recover_metadata_nolock(void);
static int osfs3_persist_inode_nolock(uint32_t ino);
static int osfs3_relocate_contiguous_nolock(osfs3_inode_t *inode,
                                             uint32_t target_blocks);
static int osfs3_persist_file_update_nolock(uint32_t ino,
                                             osfs3_inode_t *inode,
                                             const osfs3_inode_t *original,
                                             bool relocated);
static void osfs3_rollback_growth_nolock(osfs3_inode_t *inode,
                                         const osfs3_inode_t *original);
static uint8_t osfs3_inode_dentry_type(const osfs3_inode_t *inode);

static void osfs3_spin_lock(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_exchange_n(&mutation_lock, 1U, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    mutation_lock_irq_flags = flags;
}

static void osfs3_spin_unlock(void)
{
    uint64_t flags = mutation_lock_irq_flags;
    __atomic_store_n(&mutation_lock, 0U, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static uint64_t osfs3_block_offset(uint32_t block)
{
    return (uint64_t)block << OSFS3_BLOCK_SHIFT;
}

static int osfs3_part_read(uint64_t offset, void *buf, uint64_t len)
{
    return disk_read_bytes(partition_offset + offset, buf, len);
}

static int osfs3_part_write(uint64_t offset, const void *buf, uint64_t len)
{
    return disk_write_bytes(partition_offset + offset, buf, len);
}

static int osfs3_read_block_raw(uint32_t block, void *buf)
{
    if (block >= superblock.total_blocks) return -1;
    return osfs3_part_read(osfs3_block_offset(block), buf, OSFS3_BLOCK_SIZE);
}

static int osfs3_write_block_raw(uint32_t block, const void *buf)
{
    if (block >= superblock.total_blocks) return -1;
    return osfs3_part_write(osfs3_block_offset(block), buf, OSFS3_BLOCK_SIZE);
}

static void *osfs3_cache_alloc(uint64_t bytes, void **phys_out)
{
    uint64_t rounded = (bytes + 4095U) & ~4095ULL;
    void *phys = mem_alloc_aligned(rounded, 4096);
    if (!phys) return NULL;
    void *virt = PHYS_TO_VIRT(phys);
    memset(virt, 0, rounded);
    *phys_out = phys;
    return virt;
}

static void osfs3_cache_free(void **phys, uint64_t bytes)
{
    if (!*phys) return;
    mem_free_pages(*phys, (bytes + 4095U) >> 12);
    *phys = NULL;
}

static void osfs3_release_cache(void)
{
    uint64_t inode_bytes = (uint64_t)
        OSFS3_INODE_TABLE_BLOCKS(superblock.total_inodes) * OSFS3_BLOCK_SIZE;
    osfs3_cache_free(&inode_bitmap_phys, OSFS3_BLOCK_SIZE);
    osfs3_cache_free(&block_bitmap_phys, OSFS3_BLOCK_SIZE);
    osfs3_cache_free(&inode_table_phys, inode_bytes);
    osfs3_cache_free(&path_table_phys,
        (uint64_t)superblock.total_inodes * OSFS3_PATH_MAX);
    osfs3_cache_free(&path_known_phys, superblock.total_inodes);
    osfs3_cache_free(&path_hash_phys,
        (uint64_t)path_hash_slots * sizeof(*path_hash));
    osfs3_cache_free(&index_nodes_phys,
        (uint64_t)superblock.total_inodes * sizeof(*index_nodes));
    osfs3_cache_free(&open_refs_phys,
        (uint64_t)superblock.total_inodes * sizeof(*file_open_refs));
    osfs3_cache_free(&file_revisions_phys,
        (uint64_t)superblock.total_inodes * sizeof(*file_revisions));
    osfs3_cache_free(&scratch_phys, OSFS3_BLOCK_SIZE);
    inode_bitmap = NULL;
    block_bitmap = NULL;
    inode_table = NULL;
    path_table = NULL;
    path_known = NULL;
    path_hash = NULL;
    index_nodes = NULL;
    file_open_refs = NULL;
    file_revisions = NULL;
    scratch_block = NULL;
    mounted = false;
    path_index_ready = false;
}

static inline bool osfs3_bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}

static inline void osfs3_bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

static inline void osfs3_bitmap_clear(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

static bool osfs3_inode_body_valid(uint32_t ino)
{
    if (!inode_table || ino == 0 || ino >= superblock.total_inodes)
        return false;

    const osfs3_inode_t *inode = &inode_table[ino];
    uint16_t type = inode->mode & OSFS3_S_IFMT;
    if (type != OSFS3_S_IFREG && type != OSFS3_S_IFDIR &&
        type != OSFS3_S_IFLNK)
        return false;
    if (inode->extent_count > OSFS3_MAX_EXTENTS)
        return false;

    uint64_t capacity = 0;
    for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
        uint32_t start = inode->extents[extent].start_block;
        uint32_t count = inode->extents[extent].block_count;
        if (!count || start < superblock.first_data_block ||
            start >= superblock.total_blocks ||
            count > superblock.total_blocks - start)
            return false;
        capacity += (uint64_t)count * OSFS3_BLOCK_SIZE;
    }

    if (type == OSFS3_S_IFLNK && !inode->extent_count)
        return inode->size < sizeof(inode->symlink_target);
    return inode->size <= capacity;
}

static bool osfs3_inode_valid(uint32_t ino)
{
    return ino > 0 && ino < superblock.total_inodes &&
           osfs3_bitmap_test(inode_bitmap, ino) &&
           osfs3_inode_body_valid(ino);
}

static bool osfs3_inode_is_dir(const osfs3_inode_t *inode)
{
    return inode && (inode->mode & OSFS3_S_IFMT) == OSFS3_S_IFDIR;
}

static bool osfs3_inode_is_file(const osfs3_inode_t *inode)
{
    return inode && (inode->mode & OSFS3_S_IFMT) == OSFS3_S_IFREG;
}

static osfs3_inode_t *osfs3_inode(uint32_t ino)
{
    return osfs3_inode_valid(ino) ? &inode_table[ino] : NULL;
}

static int osfs3_inode_number(const void *file)
{
    if (!inode_table || !file) return -1;
    uintptr_t base = (uintptr_t)inode_table;
    uintptr_t address = (uintptr_t)file;
    uintptr_t bytes = (uintptr_t)superblock.total_inodes * sizeof(*inode_table);
    if (address < base || address >= base + bytes ||
        (address - base) % sizeof(*inode_table))
        return -1;
    return (int)((address - base) / sizeof(*inode_table));
}

static uint64_t osfs3_now(void)
{
    return ntp_get_utc ? ntp_get_utc() : superblock.create_time;
}

static void osfs3_preserve_birth_time(osfs3_inode_t *inode)
{
    if (!inode || (inode->flags & OSFS3_INODE_FLAG_BTIME_VALID)) return;
    inode->birth_time = inode->ctime;
    inode->flags |= OSFS3_INODE_FLAG_BTIME_VALID;
}

static void osfs3_touch_directory_nolock(osfs3_inode_t *inode)
{
    uint64_t now = osfs3_now();
    osfs3_preserve_birth_time(inode);
    inode->mtime = now;
    inode->ctime = now;
}

static char osfs3_path_fold(char c)
{
    if (c == '/') return '\\';
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static bool osfs3_normalize_path(const char *input, char out[OSFS3_PATH_MAX])
{
    if (!input) return false;

    const char *p = input;
    if (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\')
        p += 4;
    else if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;
    if (p[0] && p[1] == ':') p += 2;

    uint16_t starts[OSFS3_MAX_DEPTH];
    uint32_t components = 0;
    uint32_t write = 0;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *start = p;
        uint32_t length = 0;
        while (*p && *p != '/' && *p != '\\') {
            p++;
            length++;
        }
        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            if (components) write = starts[--components];
            continue;
        }
        if (!length || length > OSFS3_NAME_MAX ||
            components >= OSFS3_MAX_DEPTH ||
            write + length + (write != 0) >= OSFS3_PATH_MAX)
            return false;
        starts[components++] = (uint16_t)write;
        if (write) out[write++] = '\\';
        memcpy(out + write, start, length);
        write += length;
    }
    out[write] = '\0';
    return true;
}

static bool osfs3_path_equal(const char *a, const char *b, bool ci)
{
    if (!ci) return strcmp(a, b) == 0;
    while (*a && *b && osfs3_path_fold(*a) == osfs3_path_fold(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static char *osfs3_inode_path(uint32_t ino)
{
    if (!path_table || ino >= superblock.total_inodes) return NULL;
    return path_table + (uint64_t)ino * OSFS3_PATH_MAX;
}

static const char *osfs3_inode_name(uint32_t ino)
{
    const char *path = osfs3_inode_path(ino);
    if (!path) return NULL;
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

static uint32_t osfs3_name_hash_value(uint32_t parent, const char *name,
                                       uint32_t length)
{
    uint32_t hash = 2166136261U;
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        hash ^= (uint8_t)(parent >> shift);
        hash *= 16777619U;
    }
    for (uint32_t i = 0; i < length; i++) {
        hash ^= (uint8_t)osfs3_path_fold(name[i]);
        hash *= 16777619U;
    }
    return hash;
}

static bool osfs3_name_equal_n(const char *stored, const char *name,
                                uint32_t length, bool ci)
{
    if (!stored || strlen(stored) != length) return false;
    for (uint32_t i = 0; i < length; i++) {
        char a = stored[i];
        char b = name[i];
        if (ci) {
            a = osfs3_path_fold(a);
            b = osfs3_path_fold(b);
        }
        if (a != b) return false;
    }
    return true;
}

static void osfs3_hash_clear(void)
{
    for (uint32_t i = 0; i < path_hash_slots; i++)
        path_hash[i] = OSFS3_HASH_EMPTY;
}

static int osfs3_hash_insert(uint32_t ino)
{
    if (ino == superblock.root_inode) return 0;
    const char *name = osfs3_inode_name(ino);
    uint32_t parent = index_nodes[ino].parent;
    if (!name || !*name || !parent) return -1;
    uint32_t length = (uint32_t)strlen(name);
    uint32_t slot = osfs3_name_hash_value(parent, name, length) &
                    path_hash_mask;
    for (uint32_t probe = 0; probe < path_hash_slots; probe++) {
        if (path_hash[slot] == OSFS3_HASH_EMPTY) {
            path_hash[slot] = ino;
            return 0;
        }
        slot = (slot + 1U) & path_hash_mask;
    }
    return -1;
}

static bool osfs3_hash_remove(uint32_t ino)
{
    if (ino == superblock.root_inode) return false;
    const char *name = osfs3_inode_name(ino);
    uint32_t parent = index_nodes[ino].parent;
    if (!name || !*name || !parent) return false;

    uint32_t slot = osfs3_name_hash_value(
        parent, name, (uint32_t)strlen(name)) & path_hash_mask;
    for (uint32_t probe = 0; probe < path_hash_slots; probe++) {
        uint32_t current = path_hash[slot];
        if (current == OSFS3_HASH_EMPTY) return false;
        if (current == ino) {
            path_hash[slot] = OSFS3_HASH_EMPTY;

            /* Closing a hole in an open-addressed table requires reinserting
             * the remainder of the cluster or lookups could stop too early. */
            uint32_t next = (slot + 1U) & path_hash_mask;
            while (path_hash[next] != OSFS3_HASH_EMPTY) {
                uint32_t displaced = path_hash[next];
                path_hash[next] = OSFS3_HASH_EMPTY;
                if (osfs3_hash_insert(displaced) < 0) return false;
                next = (next + 1U) & path_hash_mask;
            }
            return true;
        }
        slot = (slot + 1U) & path_hash_mask;
    }
    return false;
}

static uint32_t osfs3_hash_lookup_nolock(uint32_t parent, const char *name,
                                          uint32_t length, bool ci)
{
    if (!parent || !name || !length) return 0;
    uint32_t slot = osfs3_name_hash_value(parent, name, length) &
                    path_hash_mask;
    for (uint32_t probe = 0; probe < path_hash_slots; probe++) {
        uint32_t ino = path_hash[slot];
        if (ino == OSFS3_HASH_EMPTY) return 0;
        if (ino < superblock.total_inodes && path_known[ino] &&
            index_nodes[ino].parent == parent && osfs3_inode_valid(ino) &&
            osfs3_name_equal_n(osfs3_inode_name(ino), name, length, ci))
            return ino;
        slot = (slot + 1U) & path_hash_mask;
    }
    return 0;
}

static bool osfs3_link_child_raw_nolock(uint32_t parent, uint32_t child)
{
    if (!parent || !child || parent >= superblock.total_inodes ||
        child >= superblock.total_inodes || index_nodes[child].parent)
        return false;

    osfs3_index_node_t *parent_node = &index_nodes[parent];
    osfs3_index_node_t *child_node = &index_nodes[child];
    child_node->parent = parent;
    child_node->prev_sibling = parent_node->last_child;
    if (parent_node->last_child)
        index_nodes[parent_node->last_child].next_sibling = child;
    else
        parent_node->first_child = child;
    parent_node->last_child = child;
    return true;
}

static bool osfs3_unlink_child_raw_nolock(uint32_t child)
{
    if (!child || child >= superblock.total_inodes) return false;
    osfs3_index_node_t *child_node = &index_nodes[child];
    uint32_t parent = child_node->parent;
    if (!parent || parent >= superblock.total_inodes) return false;

    osfs3_index_node_t *parent_node = &index_nodes[parent];
    if (child_node->prev_sibling)
        index_nodes[child_node->prev_sibling].next_sibling =
            child_node->next_sibling;
    else
        parent_node->first_child = child_node->next_sibling;
    if (child_node->next_sibling)
        index_nodes[child_node->next_sibling].prev_sibling =
            child_node->prev_sibling;
    else
        parent_node->last_child = child_node->prev_sibling;
    child_node->parent = 0;
    child_node->next_sibling = 0;
    child_node->prev_sibling = 0;
    return true;
}

static void osfs3_bump_directory_generation_nolock(uint32_t ino)
{
    if (!ino || ino >= superblock.total_inodes) return;
    if (++index_nodes[ino].generation == 0)
        index_nodes[ino].generation = 1;
}

static bool osfs3_compose_child_path_nolock(
    uint32_t parent_ino, const char *name, char out[OSFS3_PATH_MAX])
{
    if (!name || !*name || parent_ino >= superblock.total_inodes ||
        !path_known[parent_ino])
        return false;

    const char *parent = osfs3_inode_path(parent_ino);
    uint32_t parent_len = (uint32_t)strlen(parent);
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t needed = parent_len + (parent_len != 0) + name_len;
    if (name_len > OSFS3_NAME_MAX || needed >= OSFS3_PATH_MAX) return false;

    if (parent_len) {
        memcpy(out, parent, parent_len);
        out[parent_len++] = '\\';
    }
    memcpy(out + parent_len, name, name_len);
    out[parent_len + name_len] = '\0';
    return true;
}

static int osfs3_index_add_child_nolock(uint32_t parent_ino, uint32_t ino,
                                         const char *name)
{
    char path[OSFS3_PATH_MAX];
    if (ino >= superblock.total_inodes || path_known[ino] ||
        !osfs3_inode_valid(ino) ||
        !osfs3_compose_child_path_nolock(parent_ino, name, path))
        return -1;

    strcpy(osfs3_inode_path(ino), path);
    path_known[ino] = 1;
    if (!osfs3_link_child_raw_nolock(parent_ino, ino)) {
        path_known[ino] = 0;
        osfs3_inode_path(ino)[0] = '\0';
        return -1;
    }
    if (osfs3_inode_is_file(&inode_table[ino])) regular_file_count++;
    if (osfs3_hash_insert(ino) == 0) {
        osfs3_bump_directory_generation_nolock(parent_ino);
        return 0;
    }

    if (osfs3_inode_is_file(&inode_table[ino]) && regular_file_count)
        regular_file_count--;
    osfs3_unlink_child_raw_nolock(ino);
    path_known[ino] = 0;
    osfs3_inode_path(ino)[0] = '\0';
    return -1;
}

static int osfs3_index_remove_nolock(uint32_t ino)
{
    if (ino >= superblock.total_inodes || !path_known[ino] ||
        !osfs3_hash_remove(ino))
        return -1;
    uint32_t parent = index_nodes[ino].parent;
    if (!osfs3_unlink_child_raw_nolock(ino)) return -1;
    if (osfs3_inode_is_file(&inode_table[ino]) && regular_file_count)
        regular_file_count--;
    path_known[ino] = 0;
    osfs3_inode_path(ino)[0] = '\0';
    memset(&index_nodes[ino], 0, sizeof(index_nodes[ino]));
    osfs3_bump_directory_generation_nolock(parent);
    return 0;
}

static bool osfs3_descendant_paths_fit_nolock(uint32_t parent,
                                               uint32_t parent_length,
                                               uint32_t depth)
{
    if (depth >= OSFS3_MAX_DEPTH) return false;
    for (uint32_t child = index_nodes[parent].first_child; child;
         child = index_nodes[child].next_sibling) {
        const char *name = osfs3_inode_name(child);
        if (!name) return false;
        uint32_t child_length = parent_length + (parent_length != 0) +
                                (uint32_t)strlen(name);
        if (child_length >= OSFS3_PATH_MAX ||
            !osfs3_descendant_paths_fit_nolock(
                child, child_length, depth + 1U))
            return false;
    }
    return true;
}

static int osfs3_update_descendant_paths_nolock(uint32_t parent,
                                                 uint32_t depth)
{
    if (depth >= OSFS3_MAX_DEPTH) return -1;
    for (uint32_t child = index_nodes[parent].first_child; child;
         child = index_nodes[child].next_sibling) {
        char name[OSFS3_NAME_MAX + 1U];
        const char *stored = osfs3_inode_name(child);
        if (!stored || strlen(stored) > OSFS3_NAME_MAX) return -1;
        strcpy(name, stored);
        char updated[OSFS3_PATH_MAX];
        if (!osfs3_compose_child_path_nolock(parent, name, updated))
            return -1;
        strcpy(osfs3_inode_path(child), updated);
        if (osfs3_update_descendant_paths_nolock(child, depth + 1U) < 0)
            return -1;
    }
    return 0;
}

static int osfs3_index_rename_nolock(uint32_t ino, uint32_t parent_ino,
                                      const char *name)
{
    if (ino >= superblock.total_inodes || !path_known[ino]) return -1;

    char new_root[OSFS3_PATH_MAX];
    if (!osfs3_compose_child_path_nolock(parent_ino, name, new_root))
        return -1;

    uint32_t new_length = (uint32_t)strlen(new_root);
    if (!osfs3_descendant_paths_fit_nolock(ino, new_length, 0)) return -1;

    uint32_t old_parent = index_nodes[ino].parent;
    if (!old_parent || !osfs3_hash_remove(ino)) return -1;
    if (old_parent != parent_ino) {
        if (!osfs3_unlink_child_raw_nolock(ino) ||
            !osfs3_link_child_raw_nolock(parent_ino, ino))
            return -1;
    }
    strcpy(osfs3_inode_path(ino), new_root);
    if (osfs3_update_descendant_paths_nolock(ino, 0) < 0 ||
        osfs3_hash_insert(ino) < 0)
        return -1;

    osfs3_bump_directory_generation_nolock(old_parent);
    if (parent_ino != old_parent)
        osfs3_bump_directory_generation_nolock(parent_ino);
    return 0;
}

static uint32_t osfs3_lookup_path_nolock(const char *path, bool ci)
{
    char normalized[OSFS3_PATH_MAX];
    if (!mounted || !osfs3_normalize_path(path, normalized)) return 0;
    if (!normalized[0]) return superblock.root_inode;

    uint32_t parent = superblock.root_inode;
    const char *component = normalized;
    while (*component) {
        const char *end = component;
        while (*end && *end != '\\') end++;
        uint32_t child = osfs3_hash_lookup_nolock(
            parent, component, (uint32_t)(end - component), ci);
        if (!child) return 0;
        parent = child;
        component = *end ? end + 1 : end;
    }
    return parent;
}

static bool osfs3_dentry_valid(const osfs3_dentry_t *entry,
                               uint32_t remaining)
{
    if (remaining < sizeof(*entry) || entry->rec_len < sizeof(*entry) ||
        (entry->rec_len & 3U) || entry->rec_len > remaining)
        return false;
    return entry->name_len <= entry->rec_len - sizeof(*entry);
}

typedef struct {
    uint32_t cleared_dentries;
    uint32_t restored_inodes;
    uint32_t recovered_directories;
    uint32_t corrected_entries;
} osfs3_repair_stats_t;

static bool osfs3_dentry_name_valid(const osfs3_dentry_t *entry)
{
    if (!entry->name_len || entry->name_len > OSFS3_NAME_MAX)
        return false;
    for (uint32_t i = 0; i < entry->name_len; i++) {
        if (!entry->name[i] || entry->name[i] == '/' ||
            entry->name[i] == '\\')
            return false;
    }
    return true;
}

static void osfs3_build_claimed_layout_nolock(void)
{
    memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
    for (uint32_t block = 0; block < superblock.first_data_block; block++)
        osfs3_bitmap_set(scratch_block, block);

    for (uint32_t ino = 1; ino < superblock.total_inodes; ino++) {
        if (!osfs3_inode_body_valid(ino)) continue;
        const osfs3_inode_t *inode = &inode_table[ino];
        for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
            uint32_t start = inode->extents[extent].start_block;
            uint32_t count = inode->extents[extent].block_count;
            for (uint32_t offset = 0; offset < count; offset++)
                osfs3_bitmap_set(scratch_block, start + offset);
        }
    }
}

static bool osfs3_lost_directory_block_valid(
    const uint8_t *block, uint32_t ino, uint32_t parent_ino)
{
    uint32_t offset = 0;
    bool first = true;
    while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
        const osfs3_dentry_t *entry =
            (const osfs3_dentry_t *)(block + offset);
        if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
            return false;
        if (first) {
            if (entry->inode != ino || entry->rec_len != OSFS3_DIR_REC_LEN(1) ||
                entry->name_len != 1 || entry->type != OSFS3_DT_DIR ||
                entry->name[0] != '.')
                return false;
            first = false;
        } else if (offset == OSFS3_DIR_REC_LEN(1)) {
            if (entry->inode != parent_ino || entry->name_len != 2 ||
                entry->type != OSFS3_DT_DIR || entry->name[0] != '.' ||
                entry->name[1] != '.')
                return false;
        }
        offset += entry->rec_len;
    }
    return !first && offset == OSFS3_DIR_BLOCK_BYTES;
}

static bool osfs3_recover_lost_directory_nolock(
    uint32_t ino, uint32_t parent_ino, osfs3_repair_stats_t *stats)
{
    if (!ino || ino >= superblock.total_inodes ||
        osfs3_inode_body_valid(ino))
        return false;

    uint32_t candidate = 0;
    uint8_t header[OSFS3_DIR_REC_LEN(1) + OSFS3_DIR_REC_LEN(2)];
    for (uint32_t block = superblock.first_data_block;
         block < superblock.total_blocks; block++) {
        if (!osfs3_bitmap_test(block_bitmap, block) ||
            osfs3_bitmap_test(scratch_block, block))
            continue;
        if (osfs3_part_read(osfs3_block_offset(block), header,
                            sizeof(header)) < 0)
            return false;
        const osfs3_dentry_t *dot = (const osfs3_dentry_t *)header;
        const osfs3_dentry_t *dotdot =
            (const osfs3_dentry_t *)(header + OSFS3_DIR_REC_LEN(1));
        if (dot->inode != ino || dot->rec_len != OSFS3_DIR_REC_LEN(1) ||
            dot->name_len != 1 || dot->type != OSFS3_DT_DIR ||
            dot->name[0] != '.' || dotdot->inode != parent_ino ||
            dotdot->name_len != 2 || dotdot->type != OSFS3_DT_DIR ||
            dotdot->name[0] != '.' || dotdot->name[1] != '.')
            continue;
        if (candidate) return false;
        candidate = block;
    }
    if (!candidate) return false;

    void *candidate_phys = NULL;
    uint8_t *candidate_block = osfs3_cache_alloc(
        OSFS3_BLOCK_SIZE, &candidate_phys);
    if (!candidate_block) return false;
    bool valid = osfs3_read_block_raw(candidate, candidate_block) == 0 &&
        osfs3_lost_directory_block_valid(
            candidate_block, ino, parent_ino);
    osfs3_cache_free(&candidate_phys, OSFS3_BLOCK_SIZE);
    if (!valid) return false;

    osfs3_inode_t *inode = &inode_table[ino];
    memset(inode, 0, sizeof(*inode));
    inode->mode = OSFS3_S_IFDIR | 0755;
    inode->nlink = 2;
    inode->size = OSFS3_BLOCK_SIZE;
    inode->atime = inode->mtime = inode->ctime = osfs3_now();
    inode->birth_time = inode->ctime;
    inode->flags |= OSFS3_INODE_FLAG_BTIME_VALID;
    inode->extent_count = 1;
    inode->extents[0].start_block = candidate;
    inode->extents[0].block_count = 1;
    osfs3_bitmap_set(inode_bitmap, ino);
    osfs3_bitmap_set(scratch_block, candidate);
    if (osfs3_persist_inode_nolock(ino) < 0 || disk_flush() < 0)
        return false;

    stats->recovered_directories++;
    serial_puts("[OsitoFS v3] recovered directory inode=");
    serial_putdec(ino);
    serial_puts(" block=");
    serial_putdec(candidate);
    serial_puts(" parent=");
    serial_putdec(parent_ino);
    serial_puts("\n");
    return true;
}

static int osfs3_scrub_directory_tree_nolock(
    uint32_t dir_ino, uint32_t parent_ino, uint32_t depth,
    osfs3_repair_stats_t *stats)
{
    if (depth >= OSFS3_MAX_DEPTH || !osfs3_inode_body_valid(dir_ino) ||
        !osfs3_inode_is_dir(&inode_table[dir_ino]))
        return -1;

    osfs3_inode_t *dir = &inode_table[dir_ino];
    void *directory_block_phys = NULL;
    uint8_t *directory_block = osfs3_cache_alloc(
        OSFS3_BLOCK_SIZE, &directory_block_phys);
    if (!directory_block) return -1;
    int result = 0;

    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t block_index = 0;
             block_index < dir->extents[extent].block_count; block_index++) {
            uint32_t block = dir->extents[extent].start_block + block_index;
            if (osfs3_read_block_raw(block, directory_block) < 0) {
                result = -1;
                goto out;
            }

            bool modified = false;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)(directory_block + offset);
                if (!osfs3_dentry_valid(entry,
                                         OSFS3_DIR_BLOCK_BYTES - offset)) {
                    result = -1;
                    goto out;
                }

                bool is_dot = entry->name_len == 1 && entry->name[0] == '.';
                bool is_dotdot = entry->name_len == 2 &&
                                 entry->name[0] == '.' &&
                                 entry->name[1] == '.';
                if (entry->inode && is_dot) {
                    if (entry->inode != dir_ino || entry->type != OSFS3_DT_DIR) {
                        entry->inode = dir_ino;
                        entry->type = OSFS3_DT_DIR;
                        stats->corrected_entries++;
                        modified = true;
                    }
                } else if (entry->inode && is_dotdot) {
                    if (entry->inode != parent_ino ||
                        entry->type != OSFS3_DT_DIR) {
                        entry->inode = parent_ino;
                        entry->type = OSFS3_DT_DIR;
                        stats->corrected_entries++;
                        modified = true;
                    }
                } else if (entry->inode) {
                    uint32_t child = entry->inode;
                    bool child_valid = child < superblock.total_inodes &&
                                       osfs3_inode_body_valid(child);
                    if (osfs3_dentry_name_valid(entry) && !child_valid &&
                        child < superblock.total_inodes)
                        child_valid = osfs3_recover_lost_directory_nolock(
                            child, dir_ino, stats);
                    if (!osfs3_dentry_name_valid(entry) || !child_valid) {
                        serial_puts("[OsitoFS v3] clearing dangling dentry parent=");
                        serial_putdec(dir_ino);
                        serial_puts(" target=");
                        serial_putdec(child);
                        serial_puts("\n");
                        entry->inode = 0;
                        entry->type = OSFS3_DT_UNKNOWN;
                        stats->cleared_dentries++;
                        modified = true;
                    } else {
                        if (!osfs3_bitmap_test(inode_bitmap, child)) {
                            osfs3_bitmap_set(inode_bitmap, child);
                            stats->restored_inodes++;
                        }
                        uint8_t expected =
                            osfs3_inode_dentry_type(&inode_table[child]);
                        if (entry->type != expected) {
                            entry->type = expected;
                            stats->corrected_entries++;
                            modified = true;
                        }
                        if (!path_known[child]) {
                            path_known[child] = 1;
                            if (osfs3_inode_is_dir(&inode_table[child]) &&
                                osfs3_scrub_directory_tree_nolock(
                                    child, dir_ino, depth + 1U, stats) < 0) {
                                result = -1;
                                goto out;
                            }
                        }
                    }
                }
                offset += entry->rec_len;
            }

            if (modified &&
                osfs3_write_block_raw(block, directory_block) < 0) {
                result = -1;
                goto out;
            }
        }
    }

out:
    osfs3_cache_free(&directory_block_phys, OSFS3_BLOCK_SIZE);
    return result;
}

static int osfs3_build_directory_paths(uint32_t dir_ino, uint32_t depth)
{
    if (depth >= OSFS3_MAX_DEPTH) return -1;
    osfs3_inode_t *dir = osfs3_inode(dir_ino);
    if (!osfs3_inode_is_dir(dir)) return -1;

    void *directory_block_phys = NULL;
    uint8_t *directory_block = osfs3_cache_alloc(
        OSFS3_BLOCK_SIZE, &directory_block_phys);
    if (!directory_block) return -1;
    int result = 0;

    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t block_index = 0;
             block_index < dir->extents[extent].block_count; block_index++) {
            uint32_t block = dir->extents[extent].start_block + block_index;
            if (osfs3_read_block_raw(block, directory_block) < 0) {
                result = -1;
                goto out;
            }

            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)(directory_block + offset);
                if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset)) {
                    result = -1;
                    goto out;
                }

                bool is_dot = entry->name_len == 1 && entry->name[0] == '.';
                bool is_dotdot = entry->name_len == 2 &&
                                 entry->name[0] == '.' &&
                                 entry->name[1] == '.';
                if (entry->inode && !is_dot && !is_dotdot &&
                    (entry->inode >= superblock.total_inodes ||
                     !osfs3_inode_valid(entry->inode))) {
                    serial_puts("[OsitoFS v3] dangling dentry in inode=");
                    serial_putdec(dir_ino);
                    serial_puts("\n");
                    result = -1;
                    goto out;
                }

                if (entry->inode && !is_dot && !is_dotdot) {
                    uint32_t child = entry->inode;
                    if (!path_known[child]) {
                        const char *parent = osfs3_inode_path(dir_ino);
                        char *child_path = osfs3_inode_path(child);
                        uint32_t parent_len = (uint32_t)strlen(parent);
                        uint32_t needed = parent_len + (parent_len != 0) +
                                          entry->name_len;
                        if (needed >= OSFS3_PATH_MAX) {
                            serial_puts("[OsitoFS v3] path too long, inode=");
                            serial_putdec(child);
                            serial_puts("\n");
                            result = -1;
                            goto out;
                        }
                        if (parent_len) {
                            memcpy(child_path, parent, parent_len);
                            child_path[parent_len++] = '\\';
                        }
                        memcpy(child_path + parent_len, entry->name,
                               entry->name_len);
                        child_path[parent_len + entry->name_len] = '\0';
                        path_known[child] = 1;
                        if (!osfs3_link_child_raw_nolock(dir_ino, child)) {
                            result = -1;
                            goto out;
                        }
                        if (osfs3_inode_is_file(&inode_table[child]))
                            regular_file_count++;
                        if (osfs3_inode_is_dir(&inode_table[child]) &&
                            osfs3_build_directory_paths(child, depth + 1U) < 0) {
                            result = -1;
                            goto out;
                        }
                    }
                }
                offset += entry->rec_len;
            }
        }
    }
out:
    osfs3_cache_free(&directory_block_phys, OSFS3_BLOCK_SIZE);
    return result;
}

static int osfs3_rebuild_paths_in_place_nolock(void)
{
    memset(path_table, 0,
           (uint64_t)superblock.total_inodes * OSFS3_PATH_MAX);
    memset(path_known, 0, superblock.total_inodes);
    memset(index_nodes, 0,
           (uint64_t)superblock.total_inodes * sizeof(*index_nodes));
    osfs3_hash_clear();
    regular_file_count = 0;

    path_known[superblock.root_inode] = 1;
    index_nodes[superblock.root_inode].parent = superblock.root_inode;
    index_nodes[superblock.root_inode].generation = 1;
    osfs3_inode_path(superblock.root_inode)[0] = '\0';
    if (osfs3_build_directory_paths(superblock.root_inode, 0) < 0)
        return -1;

    for (uint32_t ino = 1; ino < superblock.total_inodes; ino++) {
        if (path_known[ino] && osfs3_hash_insert(ino) < 0) return -1;
    }
    return 0;
}

static int osfs3_rebuild_paths_nolock(void)
{
    if (!path_index_ready) {
        int result = osfs3_rebuild_paths_in_place_nolock();
        if (!result) path_index_ready = true;
        return result;
    }

    uint64_t path_bytes =
        (uint64_t)superblock.total_inodes * OSFS3_PATH_MAX;
    uint64_t hash_bytes =
        (uint64_t)path_hash_slots * sizeof(*path_hash);
    uint64_t node_bytes =
        (uint64_t)superblock.total_inodes * sizeof(*index_nodes);
    void *new_path_phys = NULL;
    void *new_known_phys = NULL;
    void *new_hash_phys = NULL;
    void *new_nodes_phys = NULL;
    char *new_path_table = osfs3_cache_alloc(path_bytes, &new_path_phys);
    uint8_t *new_path_known = osfs3_cache_alloc(
        superblock.total_inodes, &new_known_phys);
    uint32_t *new_path_hash = osfs3_cache_alloc(hash_bytes, &new_hash_phys);
    osfs3_index_node_t *new_index_nodes = osfs3_cache_alloc(
        node_bytes, &new_nodes_phys);
    if (!new_path_table || !new_path_known || !new_path_hash ||
        !new_index_nodes) {
        osfs3_cache_free(&new_path_phys, path_bytes);
        osfs3_cache_free(&new_known_phys, superblock.total_inodes);
        osfs3_cache_free(&new_hash_phys, hash_bytes);
        osfs3_cache_free(&new_nodes_phys, node_bytes);
        return -1;
    }

    void *old_path_phys = path_table_phys;
    void *old_known_phys = path_known_phys;
    void *old_hash_phys = path_hash_phys;
    void *old_nodes_phys = index_nodes_phys;
    char *old_path_table = path_table;
    uint8_t *old_path_known = path_known;
    uint32_t *old_path_hash = path_hash;
    osfs3_index_node_t *old_index_nodes = index_nodes;
    uint32_t old_file_count = regular_file_count;

    path_table_phys = new_path_phys;
    path_known_phys = new_known_phys;
    path_hash_phys = new_hash_phys;
    index_nodes_phys = new_nodes_phys;
    path_table = new_path_table;
    path_known = new_path_known;
    path_hash = new_path_hash;
    index_nodes = new_index_nodes;

    if (osfs3_rebuild_paths_in_place_nolock() < 0) {
        osfs3_cache_free(&new_path_phys, path_bytes);
        osfs3_cache_free(&new_known_phys, superblock.total_inodes);
        osfs3_cache_free(&new_hash_phys, hash_bytes);
        osfs3_cache_free(&new_nodes_phys, node_bytes);
        path_table_phys = old_path_phys;
        path_known_phys = old_known_phys;
        path_hash_phys = old_hash_phys;
        index_nodes_phys = old_nodes_phys;
        path_table = old_path_table;
        path_known = old_path_known;
        path_hash = old_path_hash;
        index_nodes = old_index_nodes;
        regular_file_count = old_file_count;
        return -1;
    }

    osfs3_cache_free(&old_path_phys, path_bytes);
    osfs3_cache_free(&old_known_phys, superblock.total_inodes);
    osfs3_cache_free(&old_hash_phys, hash_bytes);
    osfs3_cache_free(&old_nodes_phys, node_bytes);
    return 0;
}

static bool osfs3_super_valid(const osfs3_super_t *value)
{
    if (value->magic != OSFS3_MAGIC || value->version != OSFS3_VERSION ||
        value->block_size != OSFS3_BLOCK_SIZE ||
        value->total_blocks <= value->first_data_block ||
        value->total_blocks > OSFS3_BITMAP_BITS ||
        value->total_inodes < 2 ||
        value->total_inodes > OSFS3_MAX_INODES ||
        value->first_data_block != OSFS3_FIRST_DATA_BLOCK(value->total_inodes) ||
        value->root_inode == 0 || value->root_inode >= value->total_inodes)
        return false;

    osfs3_super_t copy = *value;
    uint32_t saved_crc = copy.crc32;
    copy.crc32 = 0;
    return saved_crc == osfs3_crc32(&copy, sizeof(copy));
}

int osfs3_mount(uint64_t part_offset)
{
    if (inode_bitmap_phys || block_bitmap_phys || inode_table_phys ||
        path_table_phys || path_known_phys || path_hash_phys ||
        index_nodes_phys ||
        open_refs_phys || file_revisions_phys || scratch_phys)
        osfs3_release_cache();
    mounted = false;
    path_index_ready = false;
    partition_offset = part_offset;

    osfs3_super_t candidate;
    if (osfs3_part_read(0, &candidate, sizeof(candidate)) < 0)
        return -1;
    if (!osfs3_super_valid(&candidate)) {
        if (candidate.magic == OSFS3_MAGIC)
            serial_puts("[OsitoFS v3] invalid superblock\n");
        return -1;
    }

    uint64_t disk_bytes = (uint64_t)disk_lba_size() * disk_lba_count();
    uint64_t fs_bytes = (uint64_t)candidate.total_blocks * OSFS3_BLOCK_SIZE;
    if (part_offset > disk_bytes || fs_bytes > disk_bytes - part_offset)
        return -1;

    superblock = candidate;
    superblock.label[sizeof(superblock.label) - 1] = '\0';

    uint64_t inode_table_bytes =
        (uint64_t)OSFS3_INODE_TABLE_BLOCKS(superblock.total_inodes) *
        OSFS3_BLOCK_SIZE;
    path_hash_slots = 1;
    while (path_hash_slots < superblock.total_inodes * 2U)
        path_hash_slots <<= 1;
    path_hash_mask = path_hash_slots - 1U;

    inode_bitmap = osfs3_cache_alloc(OSFS3_BLOCK_SIZE, &inode_bitmap_phys);
    block_bitmap = osfs3_cache_alloc(OSFS3_BLOCK_SIZE, &block_bitmap_phys);
    inode_table = osfs3_cache_alloc(inode_table_bytes, &inode_table_phys);
    path_table = osfs3_cache_alloc(
        (uint64_t)superblock.total_inodes * OSFS3_PATH_MAX, &path_table_phys);
    path_known = osfs3_cache_alloc(superblock.total_inodes, &path_known_phys);
    path_hash = osfs3_cache_alloc(
        (uint64_t)path_hash_slots * sizeof(*path_hash), &path_hash_phys);
    index_nodes = osfs3_cache_alloc(
        (uint64_t)superblock.total_inodes * sizeof(*index_nodes),
        &index_nodes_phys);
    file_open_refs = osfs3_cache_alloc(
        (uint64_t)superblock.total_inodes * sizeof(*file_open_refs),
        &open_refs_phys);
    file_revisions = osfs3_cache_alloc(
        (uint64_t)superblock.total_inodes * sizeof(*file_revisions),
        &file_revisions_phys);
    scratch_block = osfs3_cache_alloc(OSFS3_BLOCK_SIZE, &scratch_phys);
    if (!inode_bitmap || !block_bitmap || !inode_table || !path_table ||
        !path_known || !path_hash || !index_nodes || !file_open_refs ||
        !file_revisions ||
        !scratch_block) {
        serial_puts("[OsitoFS v3] metadata cache allocation failed\n");
        osfs3_release_cache();
        return -1;
    }

    if (osfs3_read_block_raw(OSFS3_INODE_BITMAP_BLK, inode_bitmap) < 0 ||
        osfs3_read_block_raw(OSFS3_BLOCK_BITMAP_BLK, block_bitmap) < 0 ||
        osfs3_part_read(osfs3_block_offset(OSFS3_INODE_TABLE_BLK),
                        inode_table, inode_table_bytes) < 0 ||
        !osfs3_inode_body_valid(superblock.root_inode) ||
        !osfs3_inode_is_dir(&inode_table[superblock.root_inode])) {
        serial_puts("[OsitoFS v3] metadata load failed\n");
        osfs3_release_cache();
        return -1;
    }

    mounted = true;
    osfs3_repair_stats_t repair = {0};
    if (!osfs3_bitmap_test(inode_bitmap, superblock.root_inode)) {
        osfs3_bitmap_set(inode_bitmap, superblock.root_inode);
        repair.restored_inodes++;
    }
    osfs3_build_claimed_layout_nolock();
    memset(path_known, 0, superblock.total_inodes);
    path_known[superblock.root_inode] = 1;
    if (osfs3_scrub_directory_tree_nolock(
            superblock.root_inode, superblock.root_inode, 0, &repair) < 0 ||
        ((repair.cleared_dentries || repair.corrected_entries) &&
         disk_flush() < 0)) {
        serial_puts("[OsitoFS v3] directory repair failed\n");
        osfs3_release_cache();
        return -1;
    }
    if (osfs3_rebuild_paths_nolock() < 0) {
        serial_puts("[OsitoFS v3] path index rebuild failed\n");
        osfs3_release_cache();
        return -1;
    }
    serial_puts("[OsitoFS v3] dentry index ready parent+name slots=");
    serial_putdec(path_hash_slots);
    serial_puts("\n");
    if (osfs3_recover_metadata_nolock() < 0) {
        serial_puts("[OsitoFS v3] metadata recovery failed\n");
        osfs3_release_cache();
        return -1;
    }
    if (repair.cleared_dentries || repair.restored_inodes ||
        repair.recovered_directories || repair.corrected_entries) {
        serial_puts("[OsitoFS v3] directory repair cleared=");
        serial_putdec(repair.cleared_dentries);
        serial_puts(" restored=");
        serial_putdec(repair.restored_inodes);
        serial_puts(" recovered_dirs=");
        serial_putdec(repair.recovered_directories);
        serial_puts(" corrected=");
        serial_putdec(repair.corrected_entries);
        serial_puts("\n");
    }

    serial_puts("[OsitoFS v3] mounted label='");
    serial_puts(superblock.label);
    serial_puts("' files=");
    serial_putdec(regular_file_count);
    serial_puts(" free_blocks=");
    serial_putdec(superblock.free_blocks);
    serial_puts("\n");
    fb_puts(" OsitoFS v3 [");
    fb_puts(superblock.label);
    fb_puts("] - ");
    fb_putdec(regular_file_count);
    fb_puts(" files\n");
    return 0;
}

static int osfs3_persist_super_nolock(void)
{
    superblock.crc32 = 0;
    superblock.crc32 = osfs3_crc32(&superblock, sizeof(superblock));
    return osfs3_part_write(0, &superblock, sizeof(superblock));
}

static int osfs3_persist_inode_nolock(uint32_t ino)
{
    if (ino >= superblock.total_inodes) return -1;
    uint64_t offset = osfs3_block_offset(OSFS3_INODE_TABLE_BLK) +
                      (uint64_t)ino * sizeof(osfs3_inode_t);
    return osfs3_part_write(offset, &inode_table[ino], sizeof(osfs3_inode_t));
}

static int osfs3_persist_bitmaps_nolock(void)
{
    if (osfs3_write_block_raw(OSFS3_INODE_BITMAP_BLK, inode_bitmap) < 0)
        return -1;
    return osfs3_write_block_raw(OSFS3_BLOCK_BITMAP_BLK, block_bitmap);
}

static uint32_t osfs3_bitmap_count_bits(const uint8_t *bitmap, uint32_t bits)
{
    uint32_t count = 0;
    for (uint32_t bit = 0; bit < bits; bit++)
        if (osfs3_bitmap_test(bitmap, bit)) count++;
    return count;
}

static int osfs3_recover_metadata_nolock(void)
{
    uint32_t orphan_count = 0;
    uint32_t old_free_blocks = superblock.free_blocks;
    uint32_t old_free_inodes = superblock.free_inodes;
    bool bitmap_changed = false;

    /* At mount there are no open files, so scratch_block can temporarily hold
     * the authoritative block map reconstructed from reachable inodes. */
    memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
    for (uint32_t block = 0; block < superblock.first_data_block; block++)
        osfs3_bitmap_set(scratch_block, block);

    for (uint32_t ino = 1; ino < superblock.total_inodes; ino++) {
        bool allocated = osfs3_bitmap_test(inode_bitmap, ino);
        if (!path_known[ino]) {
            if (!allocated) continue;
            if (ino == superblock.root_inode) return -1;

            serial_puts("[OsitoFS v3] reclaiming orphan inode=");
            serial_putdec(ino);
            serial_puts(" size=");
            serial_putdec(inode_table[ino].size);
            serial_puts("\n");
            memset(&inode_table[ino], 0, sizeof(inode_table[ino]));
            osfs3_bitmap_clear(inode_bitmap, ino);
            file_open_refs[ino] = 1; /* Marks an inode that must be persisted. */
            orphan_count++;
            bitmap_changed = true;
            continue;
        }

        if (!allocated || !osfs3_inode_valid(ino) ||
            inode_table[ino].extent_count > OSFS3_MAX_EXTENTS) {
            serial_puts("[OsitoFS v3] invalid reachable inode=");
            serial_putdec(ino);
            serial_puts("\n");
            return -1;
        }

        osfs3_inode_t *inode = &inode_table[ino];
        uint64_t capacity = 0;
        for (uint32_t extent = 0; extent < inode->extent_count; extent++) {
            uint32_t start = inode->extents[extent].start_block;
            uint32_t count = inode->extents[extent].block_count;
            if (!count || start < superblock.first_data_block ||
                start >= superblock.total_blocks ||
                count > superblock.total_blocks - start) {
                serial_puts("[OsitoFS v3] invalid extent inode=");
                serial_putdec(ino);
                serial_puts("\n");
                return -1;
            }
            for (uint32_t offset = 0; offset < count; offset++) {
                uint32_t block = start + offset;
                if (osfs3_bitmap_test(scratch_block, block)) {
                    serial_puts("[OsitoFS v3] overlapping block=");
                    serial_putdec(block);
                    serial_puts(" inode=");
                    serial_putdec(ino);
                    serial_puts("\n");
                    return -1;
                }
                osfs3_bitmap_set(scratch_block, block);
            }
            capacity += (uint64_t)count * OSFS3_BLOCK_SIZE;
        }
        if (inode->size > capacity) {
            serial_puts("[OsitoFS v3] inode size exceeds extents inode=");
            serial_putdec(ino);
            serial_puts("\n");
            return -1;
        }
    }

    if (!osfs3_bitmap_test(inode_bitmap, 0)) {
        osfs3_bitmap_set(inode_bitmap, 0);
        bitmap_changed = true;
    }
    if (memcmp(block_bitmap, scratch_block, OSFS3_BLOCK_SIZE) != 0) {
        memcpy(block_bitmap, scratch_block, OSFS3_BLOCK_SIZE);
        bitmap_changed = true;
    }

    superblock.free_inodes = superblock.total_inodes -
        osfs3_bitmap_count_bits(inode_bitmap, superblock.total_inodes);
    superblock.free_blocks = superblock.total_blocks -
        osfs3_bitmap_count_bits(block_bitmap, superblock.total_blocks);
    bool counters_changed = superblock.free_inodes != old_free_inodes ||
                            superblock.free_blocks != old_free_blocks;
    if (!bitmap_changed && !counters_changed) return 0;

    /* Allocation maps become durable before stale inode bodies are cleared.
     * A reset at any point therefore cannot publish an inode that owns blocks
     * already returned to the allocator. */
    if (osfs3_persist_bitmaps_nolock() < 0 || disk_flush() < 0)
        return -1;
    for (uint32_t ino = 1; ino < superblock.total_inodes; ino++) {
        if (file_open_refs[ino] != 1) continue;
        if (osfs3_persist_inode_nolock(ino) < 0) return -1;
        file_open_refs[ino] = 0;
    }
    if (disk_flush() < 0 || osfs3_persist_super_nolock() < 0 ||
        disk_flush() < 0)
        return -1;

    serial_puts("[OsitoFS v3] metadata repaired orphans=");
    serial_putdec(orphan_count);
    serial_puts(" free_inodes=");
    serial_putdec(superblock.free_inodes);
    serial_puts(" free_blocks=");
    serial_putdec(superblock.free_blocks);
    serial_puts("\n");
    return 0;
}

static uint32_t osfs3_inode_block_count(const osfs3_inode_t *inode)
{
    uint32_t count = 0;
    if (!inode || inode->extent_count > OSFS3_MAX_EXTENTS) return 0;
    for (uint32_t i = 0; i < inode->extent_count; i++)
        count += inode->extents[i].block_count;
    return count;
}

static uint32_t osfs3_map_file_block(const osfs3_inode_t *inode,
                                     uint32_t logical_block)
{
    if (!inode || inode->extent_count > OSFS3_MAX_EXTENTS) return 0;
    for (uint32_t i = 0; i < inode->extent_count; i++) {
        if (logical_block < inode->extents[i].block_count)
            return inode->extents[i].start_block + logical_block;
        logical_block -= inode->extents[i].block_count;
    }
    return 0;
}

static uint32_t osfs3_find_free_run_nolock(uint32_t wanted,
                                           uint32_t *run_length)
{
    uint32_t best_start = 0;
    uint32_t best_length = 0;
    uint32_t start = 0;
    uint32_t length = 0;

    for (uint32_t block = superblock.first_data_block;
         block <= superblock.total_blocks; block++) {
        bool free = block < superblock.total_blocks &&
                    !osfs3_bitmap_test(block_bitmap, block);
        if (free) {
            if (!length) start = block;
            length++;
            if (length >= wanted) {
                *run_length = wanted;
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

    if (!best_length) return 0;
    *run_length = best_length > wanted ? wanted : best_length;
    return best_start;
}

static void osfs3_mark_run_nolock(uint32_t start, uint32_t count, bool used)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t block = start + i;
        bool was_used = osfs3_bitmap_test(block_bitmap, block);
        if (used && !was_used) {
            osfs3_bitmap_set(block_bitmap, block);
            if (superblock.free_blocks) superblock.free_blocks--;
        } else if (!used && was_used) {
            osfs3_bitmap_clear(block_bitmap, block);
            superblock.free_blocks++;
        }
    }
}

static int osfs3_zero_run_nolock(uint32_t start, uint32_t count)
{
    memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
    for (uint32_t i = 0; i < count; i++) {
        if (osfs3_write_block_raw(start + i, scratch_block) < 0) return -1;
    }
    return 0;
}

typedef struct {
    uint32_t start;
    uint32_t count;
} osfs3_allocated_run_t;

static int osfs3_ensure_blocks_nolock(osfs3_inode_t *inode,
                                      uint32_t target_blocks)
{
    uint32_t current = osfs3_inode_block_count(inode);
    if (target_blocks <= current) return 0;
    if (target_blocks > superblock.total_blocks - superblock.first_data_block)
        return -1;

    osfs3_inode_t original = *inode;
    osfs3_allocated_run_t allocated[OSFS3_MAX_EXTENTS + 1U];
    uint32_t allocated_count = 0;
    uint32_t missing = target_blocks - current;

    while (missing) {
        uint32_t start = 0;
        uint32_t count = 0;
        bool extend_last = false;

        if (inode->extent_count) {
            osfs3_extent_t *last = &inode->extents[inode->extent_count - 1U];
            start = last->start_block + last->block_count;
            while (count < missing && start + count < superblock.total_blocks &&
                   !osfs3_bitmap_test(block_bitmap, start + count))
                count++;
            extend_last = count != 0;
        }

        if (!count) {
            if (inode->extent_count >= OSFS3_MAX_EXTENTS) goto rollback;
            start = osfs3_find_free_run_nolock(missing, &count);
            if (!start || !count) goto rollback;
        }

        osfs3_mark_run_nolock(start, count, true);
        allocated[allocated_count].start = start;
        allocated[allocated_count].count = count;
        allocated_count++;

        if (extend_last) {
            inode->extents[inode->extent_count - 1U].block_count += count;
        } else {
            osfs3_extent_t *extent = &inode->extents[inode->extent_count++];
            extent->start_block = start;
            extent->block_count = count;
        }
        if (osfs3_zero_run_nolock(start, count) < 0) goto rollback;
        missing -= count;
    }
    return 0;

rollback:
    for (uint32_t i = 0; i < allocated_count; i++)
        osfs3_mark_run_nolock(allocated[i].start, allocated[i].count, false);
    *inode = original;
    return -1;
}

static void osfs3_release_tail_nolock(osfs3_inode_t *inode,
                                      uint32_t target_blocks)
{
    uint32_t current = osfs3_inode_block_count(inode);
    while (current > target_blocks && inode->extent_count) {
        osfs3_extent_t *last = &inode->extents[inode->extent_count - 1U];
        uint32_t remove = current - target_blocks;
        if (remove > last->block_count) remove = last->block_count;
        uint32_t release_start = last->start_block + last->block_count - remove;
        osfs3_mark_run_nolock(release_start, remove, false);
        last->block_count -= remove;
        current -= remove;
        if (!last->block_count) {
            memset(last, 0, sizeof(*last));
            inode->extent_count--;
        }
    }
}

static uint32_t osfs3_alloc_inode_nolock(uint16_t mode)
{
    for (uint32_t ino = 2; ino < superblock.total_inodes; ino++) {
        if (osfs3_bitmap_test(inode_bitmap, ino)) continue;
        osfs3_bitmap_set(inode_bitmap, ino);
        if (superblock.free_inodes) superblock.free_inodes--;
        osfs3_inode_t *inode = &inode_table[ino];
        memset(inode, 0, sizeof(*inode));
        inode->mode = mode;
        inode->nlink = (mode & OSFS3_S_IFMT) == OSFS3_S_IFDIR ? 2 : 1;
        inode->atime = inode->mtime = inode->ctime = osfs3_now();
        inode->birth_time = inode->ctime;
        inode->flags |= OSFS3_INODE_FLAG_BTIME_VALID;
        return ino;
    }
    return 0;
}

static void osfs3_release_inode_nolock(uint32_t ino)
{
    if (!osfs3_inode_valid(ino) || ino == superblock.root_inode) return;
    osfs3_inode_t *inode = &inode_table[ino];
    for (uint32_t i = 0; i < inode->extent_count; i++)
        osfs3_mark_run_nolock(inode->extents[i].start_block,
                              inode->extents[i].block_count, false);
    memset(inode, 0, sizeof(*inode));
    osfs3_bitmap_clear(inode_bitmap, ino);
    superblock.free_inodes++;
    file_open_refs[ino] = 0;
}

static bool osfs3_dentry_name_equal(const osfs3_dentry_t *entry,
                                     const char *name, bool ci)
{
    uint32_t length = (uint32_t)strlen(name);
    if (entry->name_len != length) return false;
    for (uint32_t i = 0; i < length; i++) {
        char a = entry->name[i];
        char b = name[i];
        if (ci) {
            a = osfs3_path_fold(a);
            b = osfs3_path_fold(b);
        }
        if (a != b) return false;
    }
    return true;
}

static int osfs3_find_dentry_nolock(
    uint32_t dir_ino, const char *name, bool ci, uint32_t *found_ino,
    osfs3_dentry_location_t *location)
{
    osfs3_inode_t *dir = osfs3_inode(dir_ino);
    if (found_ino) *found_ino = 0;
    if (!osfs3_inode_is_dir(dir) || !name || !*name) return -1;

    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t block_index = 0;
             block_index < dir->extents[extent].block_count; block_index++) {
            uint32_t block = dir->extents[extent].start_block + block_index;
            if (osfs3_read_block_raw(block, scratch_block) < 0) return -1;
            uint32_t offset = 0;
            uint32_t previous = 0;
            bool have_previous = false;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)(scratch_block + offset);
                if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
                    return -1;
                if (entry->inode && osfs3_dentry_name_equal(entry, name, ci)) {
                    if (location) {
                        location->block = block;
                        location->offset = offset;
                        location->previous_offset = previous;
                        location->has_previous = have_previous;
                    }
                    if (found_ino) *found_ino = entry->inode;
                    return 1;
                }
                previous = offset;
                have_previous = true;
                offset += entry->rec_len;
            }
        }
    }
    return 0;
}

static uint32_t osfs3_find_in_directory_nolock(
    uint32_t dir_ino, const char *name, bool ci,
    osfs3_dentry_location_t *location)
{
    if (!location && path_index_ready && name && *name)
        return osfs3_hash_lookup_nolock(
            dir_ino, name, (uint32_t)strlen(name), ci);
    uint32_t found = 0;
    return osfs3_find_dentry_nolock(
        dir_ino, name, ci, &found, location) == 1 ? found : 0;
}

static int osfs3_insert_in_loaded_block_nolock(uint32_t block,
                                                uint32_t target_ino,
                                                const char *name,
                                                uint8_t type)
{
    uint32_t name_length = (uint32_t)strlen(name);
    uint32_t needed = OSFS3_DIR_REC_LEN(name_length);
    uint32_t offset = 0;

    while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
        osfs3_dentry_t *entry =
            (osfs3_dentry_t *)(scratch_block + offset);
        if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
            return -1;

        uint32_t insertion_offset = 0;
        uint32_t available = 0;
        if (!entry->inode && entry->rec_len >= needed) {
            insertion_offset = offset;
            available = entry->rec_len;
        } else if (entry->inode) {
            uint32_t actual = OSFS3_DIR_REC_LEN(entry->name_len);
            if (entry->rec_len >= actual + needed) {
                insertion_offset = offset + actual;
                available = entry->rec_len - actual;
                entry->rec_len = (uint16_t)actual;
            }
        }

        if (available) {
            osfs3_dentry_t *insert =
                (osfs3_dentry_t *)(scratch_block + insertion_offset);
            uint32_t remainder = available - needed;
            memset(insert, 0, available);
            insert->inode = target_ino;
            insert->name_len = (uint8_t)name_length;
            insert->type = type;
            insert->rec_len = (uint16_t)(remainder >= sizeof(*insert)
                                           ? needed : available);
            memcpy(insert->name, name, name_length);
            if (remainder >= sizeof(*insert)) {
                osfs3_dentry_t *free_entry =
                    (osfs3_dentry_t *)((uint8_t *)insert + needed);
                free_entry->rec_len = (uint16_t)remainder;
            }
            return osfs3_write_block_raw(block, scratch_block);
        }
        offset += entry->rec_len;
    }
    return 1;
}

static uint8_t osfs3_inode_dentry_type(const osfs3_inode_t *inode)
{
    if (osfs3_inode_is_dir(inode)) return OSFS3_DT_DIR;
    if (osfs3_inode_is_file(inode)) return OSFS3_DT_REG;
    if ((inode->mode & OSFS3_S_IFMT) == OSFS3_S_IFLNK) return OSFS3_DT_LNK;
    return OSFS3_DT_UNKNOWN;
}

static int osfs3_add_dentry_nolock(uint32_t dir_ino, uint32_t target_ino,
                                   const char *name, uint8_t type)
{
    osfs3_inode_t *dir = osfs3_inode(dir_ino);
    if (!osfs3_inode_is_dir(dir) || !osfs3_inode_valid(target_ino) ||
        !name || !*name || strlen(name) > OSFS3_NAME_MAX)
        return -1;

    uint32_t existing = 0;
    int lookup = osfs3_find_dentry_nolock(
        dir_ino, name, true, &existing, NULL);
    if (lookup < 0 || lookup > 0) return -1;

    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t block_index = 0;
             block_index < dir->extents[extent].block_count; block_index++) {
            uint32_t block = dir->extents[extent].start_block + block_index;
            if (osfs3_read_block_raw(block, scratch_block) < 0) return -1;
            int result = osfs3_insert_in_loaded_block_nolock(
                block, target_ino, name, type);
            if (result < 0) return -1;
            if (!result) {
                osfs3_touch_directory_nolock(dir);
                return 0;
            }
        }
    }

    osfs3_inode_t original = *dir;
    uint32_t old_blocks = osfs3_inode_block_count(dir);
    bool relocated = false;
    if (osfs3_ensure_blocks_nolock(dir, old_blocks + 1U) < 0) {
        if (osfs3_relocate_contiguous_nolock(dir, old_blocks + 1U) < 0)
            return -1;
        relocated = true;
    }
    uint32_t block = osfs3_map_file_block(dir, old_blocks);
    if (!block) {
        osfs3_rollback_growth_nolock(dir, &original);
        return -1;
    }
    memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
    osfs3_dentry_t *entry = (osfs3_dentry_t *)scratch_block;
    entry->inode = target_ino;
    entry->rec_len = (uint16_t)OSFS3_DIR_BLOCK_BYTES;
    entry->name_len = (uint8_t)strlen(name);
    entry->type = type;
    memcpy(entry->name, name, entry->name_len);
    dir->size = (uint64_t)(old_blocks + 1U) * OSFS3_BLOCK_SIZE;
    osfs3_touch_directory_nolock(dir);
    if (osfs3_write_block_raw(block, scratch_block) < 0) {
        osfs3_rollback_growth_nolock(dir, &original);
        return -1;
    }

    if (osfs3_persist_file_update_nolock(
            dir_ino, dir, &original, relocated) < 0) {
        serial_puts("[OsitoFS v3] directory layout commit failed inode=");
        serial_putdec(dir_ino);
        serial_puts("\n");
        return -1;
    }
    return 0;
}

static int osfs3_remove_dentry_nolock(uint32_t dir_ino, const char *name,
                                      bool ci, uint32_t *removed_ino)
{
    osfs3_dentry_location_t location;
    uint32_t ino = osfs3_find_in_directory_nolock(
        dir_ino, name, ci, &location);
    if (!ino || osfs3_read_block_raw(location.block, scratch_block) < 0)
        return -1;

    osfs3_dentry_t *entry =
        (osfs3_dentry_t *)(scratch_block + location.offset);
    if (location.has_previous) {
        osfs3_dentry_t *previous =
            (osfs3_dentry_t *)(scratch_block + location.previous_offset);
        uint32_t combined = (uint32_t)previous->rec_len + entry->rec_len;
        if (combined <= OSFS3_DIR_BLOCK_BYTES - location.previous_offset)
            previous->rec_len = (uint16_t)combined;
        else
            entry->inode = 0;
    } else {
        entry->inode = 0;
    }
    if (osfs3_write_block_raw(location.block, scratch_block) < 0) return -1;
    osfs3_touch_directory_nolock(&inode_table[dir_ino]);
    if (removed_ino) *removed_ino = ino;
    return 0;
}

static bool osfs3_directory_empty_nolock(uint32_t dir_ino)
{
    osfs3_inode_t *dir = osfs3_inode(dir_ino);
    if (!osfs3_inode_is_dir(dir)) return false;
    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t block_index = 0;
             block_index < dir->extents[extent].block_count; block_index++) {
            uint32_t block = dir->extents[extent].start_block + block_index;
            if (osfs3_read_block_raw(block, scratch_block) < 0) return false;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)(scratch_block + offset);
                if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
                    return false;
                if (entry->inode &&
                    !(entry->name_len == 1 && entry->name[0] == '.') &&
                    !(entry->name_len == 2 && entry->name[0] == '.' &&
                      entry->name[1] == '.'))
                    return false;
                offset += entry->rec_len;
            }
        }
    }
    return true;
}

static int osfs3_split_parent(const char *path,
                              char parent[OSFS3_PATH_MAX],
                              char name[OSFS3_NAME_MAX + 1U])
{
    char normalized[OSFS3_PATH_MAX];
    if (!osfs3_normalize_path(path, normalized) || !normalized[0]) return -1;
    char *last = normalized;
    for (char *p = normalized; *p; p++)
        if (*p == '\\') last = p + 1;
    if (!*last || strlen(last) > OSFS3_NAME_MAX ||
        strcmp(last, ".") == 0 || strcmp(last, "..") == 0)
        return -1;
    strcpy(name, last);
    if (last == normalized) {
        parent[0] = '\0';
    } else {
        last[-1] = '\0';
        strcpy(parent, normalized);
    }
    return 0;
}

static int osfs3_initialize_directory_nolock(uint32_t ino,
                                              uint32_t parent_ino)
{
    osfs3_inode_t *inode = &inode_table[ino];
    if (osfs3_ensure_blocks_nolock(inode, 1) < 0) return -1;
    uint32_t block = osfs3_map_file_block(inode, 0);
    if (!block) return -1;

    memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
    osfs3_dentry_t *dot = (osfs3_dentry_t *)scratch_block;
    dot->inode = ino;
    dot->rec_len = (uint16_t)OSFS3_DIR_REC_LEN(1);
    dot->name_len = 1;
    dot->type = OSFS3_DT_DIR;
    dot->name[0] = '.';
    osfs3_dentry_t *dotdot =
        (osfs3_dentry_t *)(scratch_block + dot->rec_len);
    dotdot->inode = parent_ino;
    dotdot->rec_len = (uint16_t)(OSFS3_DIR_BLOCK_BYTES - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->type = OSFS3_DT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    inode->size = OSFS3_BLOCK_SIZE;
    return osfs3_write_block_raw(block, scratch_block);
}

static int osfs3_update_dotdot_nolock(uint32_t dir_ino,
                                      uint32_t parent_ino)
{
    osfs3_inode_t *dir = osfs3_inode(dir_ino);
    if (!osfs3_inode_is_dir(dir) || !dir->extent_count) return -1;
    uint32_t block = osfs3_map_file_block(dir, 0);
    if (!block || osfs3_read_block_raw(block, scratch_block) < 0) return -1;
    uint32_t offset = 0;
    while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
        osfs3_dentry_t *entry =
            (osfs3_dentry_t *)(scratch_block + offset);
        if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
            return -1;
        if (entry->name_len == 2 && entry->name[0] == '.' &&
            entry->name[1] == '.') {
            entry->inode = parent_ino;
            return osfs3_write_block_raw(block, scratch_block);
        }
        offset += entry->rec_len;
    }
    return -1;
}

static bool osfs3_file_busy_nolock(uint32_t ino)
{
    if (ino >= superblock.total_inodes) return true;
    if (file_open_refs[ino]) return true;
    return syscall_file_is_mapped &&
           syscall_file_is_mapped(&inode_table[ino]);
}

uint32_t osfs3_resolve_path(const char *path)
{
    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, false);
    osfs3_spin_unlock();
    return ino;
}

uint32_t osfs3_resolve_path_ci(const char *path)
{
    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, true);
    osfs3_spin_unlock();
    return ino;
}

bool osfs3_directory_exists_ci(const char *path)
{
    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, true);
    bool exists = ino && osfs3_inode_is_dir(osfs3_inode(ino));
    osfs3_spin_unlock();
    return exists;
}

bool osfs3_directory_has_children_ci(const char *path)
{
    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, true);
    bool has_children = ino && osfs3_inode_is_dir(osfs3_inode(ino)) &&
                        index_nodes[ino].first_child != 0;
    osfs3_spin_unlock();
    return has_children;
}

int osfs3_dir_cursor_open_ci(const char *path, osfs3_dir_cursor_t *cursor)
{
    if (!cursor) return -1;
    memset(cursor, 0, sizeof(*cursor));

    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, true);
    if (!ino || !osfs3_inode_is_dir(osfs3_inode(ino))) {
        osfs3_spin_unlock();
        return -1;
    }
    cursor->directory_inode = ino;
    cursor->next_inode = index_nodes[ino].first_child;
    cursor->generation = index_nodes[ino].generation;
    osfs3_spin_unlock();
    return 0;
}

int osfs3_dir_cursor_next(osfs3_dir_cursor_t *cursor,
                          char *name, uint32_t name_capacity,
                          bool *is_directory, uint64_t *size,
                          uint32_t *inode_index)
{
    if (!cursor || !name || !name_capacity) return -1;

    osfs3_spin_lock();
    uint32_t parent = cursor->directory_inode;
    if (!mounted || !parent || parent >= superblock.total_inodes ||
        !path_known[parent] ||
        !osfs3_inode_is_dir(osfs3_inode(parent))) {
        osfs3_spin_unlock();
        return -1;
    }

    if (cursor->generation != index_nodes[parent].generation) {
        cursor->next_inode = index_nodes[parent].first_child;
        cursor->generation = index_nodes[parent].generation;
    }

    while (cursor->next_inode) {
        uint32_t child = cursor->next_inode;
        if (child >= superblock.total_inodes) {
            osfs3_spin_unlock();
            return -1;
        }
        cursor->next_inode = index_nodes[child].next_sibling;
        if (!path_known[child] || index_nodes[child].parent != parent ||
            !osfs3_inode_valid(child))
            continue;

        const char *stored = osfs3_inode_name(child);
        uint32_t length = stored ? (uint32_t)strlen(stored) : 0;
        if (!stored || !length || length >= name_capacity) {
            osfs3_spin_unlock();
            return -1;
        }
        memcpy(name, stored, length + 1U);
        bool directory = osfs3_inode_is_dir(&inode_table[child]);
        if (is_directory) *is_directory = directory;
        if (size) *size = directory ? 0 : inode_table[child].size;
        if (inode_index) *inode_index = child;
        osfs3_spin_unlock();
        return 1;
    }

    osfs3_spin_unlock();
    return 0;
}

bool osfs3_is_dir(uint32_t ino)
{
    osfs3_spin_lock();
    bool result = mounted && osfs3_inode_is_dir(osfs3_inode(ino));
    osfs3_spin_unlock();
    return result;
}

void *osfs3_find(const char *path)
{
    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, false);
    void *result = ino && osfs3_inode_is_file(osfs3_inode(ino))
        ? &inode_table[ino] : NULL;
    osfs3_spin_unlock();
    return result;
}

void *osfs3_find_ci(const char *path)
{
    osfs3_spin_lock();
    uint32_t ino = osfs3_lookup_path_nolock(path, true);
    void *result = ino && osfs3_inode_is_file(osfs3_inode(ino))
        ? &inode_table[ino] : NULL;
    osfs3_spin_unlock();
    return result;
}

void *osfs3_get_node(int inode_index)
{
    if (!mounted || inode_index <= 0 ||
        inode_index >= (int)superblock.total_inodes)
        return NULL;
    uint32_t ino = (uint32_t)inode_index;
    return path_known[ino] && osfs3_inode_valid(ino)
        ? &inode_table[ino] : NULL;
}

void *osfs3_get_file(int inode_index)
{
    if (!mounted || inode_index <= 0 ||
        inode_index >= (int)superblock.total_inodes)
        return NULL;
    uint32_t ino = (uint32_t)inode_index;
    return path_known[ino] && osfs3_inode_valid(ino) &&
           osfs3_inode_is_file(&inode_table[ino])
        ? &inode_table[ino] : NULL;
}

void *osfs3_file_at(uint32_t ordinal)
{
    if (!mounted) return NULL;
    uint32_t current = 0;
    for (uint32_t ino = 1; ino < superblock.total_inodes; ino++) {
        if (!path_known[ino] || !osfs3_inode_valid(ino) ||
            !osfs3_inode_is_file(&inode_table[ino]))
            continue;
        if (current++ == ordinal) return &inode_table[ino];
    }
    return NULL;
}

static bool osfs3_wildcard_match(const char *pattern, const char *name)
{
    if (!pattern || !name) return false;
    if (pattern[0] == '*' && pattern[1] == '\0') return true;
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *extension = pattern + 1;
        uint32_t extension_len = (uint32_t)strlen(extension);
        uint32_t name_len = (uint32_t)strlen(name);
        if (name_len < extension_len) return false;
        return osfs3_path_equal(name + name_len - extension_len,
                                extension, true);
    }
    return osfs3_path_equal(pattern, name, true);
}

int osfs3_find_first(const char *pattern, int start_inode)
{
    if (!mounted || !pattern) return -1;
    if (start_inode < 1) start_inode = 1;
    for (int ino = start_inode; ino < (int)superblock.total_inodes; ino++) {
        if (!osfs3_get_file(ino)) continue;
        const char *name = osfs3_inode_path((uint32_t)ino);
        for (const char *p = name; *p; p++)
            if (*p == '\\' || *p == '/') name = p + 1;
        if (osfs3_wildcard_match(pattern, name)) return ino;
    }
    return -1;
}

int osfs3_file_retain(void *file)
{
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    if (!mounted || ino <= 0 || !osfs3_inode_valid((uint32_t)ino) ||
        file_open_refs[ino] == UINT32_MAX) {
        osfs3_spin_unlock();
        return -1;
    }
    file_open_refs[ino]++;
    osfs3_spin_unlock();
    return 0;
}

void osfs3_file_release(void *file)
{
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    if (ino > 0 && file_open_refs[ino]) file_open_refs[ino]--;
    osfs3_spin_unlock();
}

uint64_t osfs3_file_size(const void *file)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 && osfs3_inode_valid((uint32_t)ino)
        ? inode_table[ino].size : 0;
}

const char *osfs3_file_name(const void *file)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 && path_known[ino] ? osfs3_inode_path((uint32_t)ino) : NULL;
}

uint32_t osfs3_file_ctime(const void *file)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 ? (uint32_t)inode_table[ino].ctime : 0;
}

uint32_t osfs3_file_atime(const void *file)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 ? (uint32_t)inode_table[ino].atime : 0;
}

uint32_t osfs3_file_mtime(const void *file)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 ? (uint32_t)inode_table[ino].mtime : 0;
}

uint64_t osfs3_volume_id(void)
{
    osfs3_spin_lock();
    uint64_t hash = 0;
    if (mounted) {
        hash = 14695981039346656037ULL;
        for (uint32_t i = 0; i < 16; i++) {
            hash ^= superblock.uuid[i];
            hash *= 1099511628211ULL;
        }
        if (!(uint32_t)hash) hash ^= hash >> 32;
        if (!hash) hash = 1;
    }
    osfs3_spin_unlock();
    return hash;
}

uint64_t osfs3_file_id(const void *file)
{
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    uint64_t id = mounted && ino > 0 &&
        osfs3_inode_valid((uint32_t)ino) ? (uint64_t)(uint32_t)ino : 0;
    osfs3_spin_unlock();
    return id;
}

int osfs3_file_get_times(void *file, osfs_file_times_t *times)
{
    if (!times) return -1;
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    if (!mounted || ino <= 0 || !osfs3_inode_valid((uint32_t)ino)) {
        osfs3_spin_unlock();
        return -1;
    }
    const osfs3_inode_t *inode = &inode_table[ino];
    times->creation = (inode->flags & OSFS3_INODE_FLAG_BTIME_VALID)
        ? inode->birth_time : inode->ctime;
    times->access = inode->atime;
    times->modified = inode->mtime;
    times->changed = inode->ctime;
    osfs3_spin_unlock();
    return 0;
}

int osfs3_file_set_times(void *file, uint32_t mask,
                         const osfs_file_times_t *times)
{
    if (!times || !mask || (mask & ~OSFS_FILE_TIME_MASK)) return -1;
    osfs3_spin_lock();
    int ino_number = osfs3_inode_number(file);
    if (!mounted || ino_number <= 0 ||
        !osfs3_inode_valid((uint32_t)ino_number)) {
        osfs3_spin_unlock();
        return -1;
    }

    uint32_t ino = (uint32_t)ino_number;
    osfs3_inode_t *inode = &inode_table[ino];
    osfs3_inode_t original = *inode;
    if (mask & OSFS_FILE_TIME_CREATION) {
        inode->birth_time = times->creation;
        inode->flags |= OSFS3_INODE_FLAG_BTIME_VALID;
    }
    if (mask & OSFS_FILE_TIME_ACCESS) inode->atime = times->access;
    if (mask & OSFS_FILE_TIME_MODIFIED) inode->mtime = times->modified;
    if (mask & OSFS_FILE_TIME_CHANGED) inode->ctime = times->changed;
    if (memcmp(inode, &original, sizeof(*inode)) == 0) {
        osfs3_spin_unlock();
        return 0;
    }

    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    inode->crc32 = 0;
    int result = osfs3_persist_inode_nolock(ino);
    if (!result) result = disk_flush();
    if (result < 0) *inode = original;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

uint64_t osfs3_file_revision(const void *file)
{
    int ino = osfs3_inode_number(file);
    if (ino <= 0 || ino >= (int)superblock.total_inodes || !file_revisions)
        return 0;
    return __atomic_load_n(&file_revisions[ino], __ATOMIC_ACQUIRE);
}

uint64_t osfs3_file_byte_offset(const void *file)
{
    int ino = osfs3_inode_number(file);
    if (ino <= 0 || !osfs3_inode_valid((uint32_t)ino) ||
        inode_table[ino].extent_count != 1)
        return 0;
    return partition_offset +
           osfs3_block_offset(inode_table[ino].extents[0].start_block);
}

static int osfs3_read_snapshot(const osfs3_inode_t *inode, uint64_t offset,
                               void *buf, uint64_t len)
{
    if ((!buf && len) || len > 0x7FFFFFFFULL ||
        offset > inode->size || len > inode->size - offset)
        return -1;
    uint8_t *destination = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t position = offset + done;
        uint32_t logical = (uint32_t)(position >> OSFS3_BLOCK_SHIFT);
        uint32_t in_block = (uint32_t)(position & (OSFS3_BLOCK_SIZE - 1U));
        uint32_t physical = osfs3_map_file_block(inode, logical);
        if (!physical) return -1;
        uint64_t chunk = OSFS3_BLOCK_SIZE - in_block;
        if (chunk > len - done) chunk = len - done;
        if (osfs3_part_read(osfs3_block_offset(physical) + in_block,
                            destination + done, chunk) < 0)
            return -1;
        done += chunk;
    }
    return (int)len;
}

int osfs3_read_file(const void *file, uint64_t offset, void *buf, uint64_t len)
{
    osfs3_inode_t snapshot;
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    if (!mounted || ino <= 0 || !osfs3_inode_valid((uint32_t)ino) ||
        !osfs3_inode_is_file(&inode_table[ino])) {
        osfs3_spin_unlock();
        return -1;
    }
    snapshot = inode_table[ino];
    osfs3_spin_unlock();
    return osfs3_read_snapshot(&snapshot, offset, buf, len);
}

int osfs3_read(uint32_t ino, uint64_t offset, void *buf, uint64_t len)
{
    if (!mounted || !osfs3_inode_valid(ino)) return -1;
    return osfs3_read_file(&inode_table[ino], offset, buf, len);
}

int osfs3_read_file_block(const void *file, uint32_t block_index, void *buf)
{
    osfs3_inode_t snapshot;
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    if (!mounted || ino <= 0 || !buf || !osfs3_inode_valid((uint32_t)ino)) {
        osfs3_spin_unlock();
        return -1;
    }
    snapshot = inode_table[ino];
    osfs3_spin_unlock();
    uint32_t physical = osfs3_map_file_block(&snapshot, block_index);
    return physical ? osfs3_read_block_raw(physical, buf) : -1;
}

void osfs3_list_dir(uint32_t dir_ino)
{
    osfs3_spin_lock();
    osfs3_inode_t *dir = osfs3_inode(dir_ino);
    if (!mounted || !osfs3_inode_is_dir(dir)) {
        osfs3_spin_unlock();
        return;
    }
    serial_puts("[OsitoFS v3] directory ");
    serial_puts(osfs3_inode_path(dir_ino));
    serial_puts(":\n");
    for (uint32_t extent = 0; extent < dir->extent_count; extent++) {
        for (uint32_t block_index = 0;
             block_index < dir->extents[extent].block_count; block_index++) {
            uint32_t block = dir->extents[extent].start_block + block_index;
            if (osfs3_read_block_raw(block, scratch_block) < 0) break;
            uint32_t offset = 0;
            while (offset + sizeof(osfs3_dentry_t) <= OSFS3_DIR_BLOCK_BYTES) {
                osfs3_dentry_t *entry =
                    (osfs3_dentry_t *)(scratch_block + offset);
                if (!osfs3_dentry_valid(entry, OSFS3_DIR_BLOCK_BYTES - offset))
                    break;
                if (entry->inode) {
                    char name[OSFS3_NAME_MAX + 1U];
                    memcpy(name, entry->name, entry->name_len);
                    name[entry->name_len] = '\0';
                    serial_puts("  ");
                    serial_puts(entry->type == OSFS3_DT_DIR ? "<DIR> " : "      ");
                    serial_puts(name);
                    serial_puts("\n");
                }
                offset += entry->rec_len;
            }
        }
    }
    osfs3_spin_unlock();
}

static bool osfs3_inode_contains_block(const osfs3_inode_t *inode,
                                       uint32_t block)
{
    for (uint32_t i = 0; i < inode->extent_count; i++) {
        uint32_t start = inode->extents[i].start_block;
        if (block >= start && block - start < inode->extents[i].block_count)
            return true;
    }
    return false;
}

static void osfs3_rollback_growth_nolock(osfs3_inode_t *inode,
                                         const osfs3_inode_t *original)
{
    for (uint32_t i = 0; i < inode->extent_count; i++) {
        uint32_t start = inode->extents[i].start_block;
        uint32_t count = inode->extents[i].block_count;
        for (uint32_t block = start; block < start + count; block++) {
            if (!osfs3_inode_contains_block(original, block))
                osfs3_mark_run_nolock(block, 1, false);
        }
    }
    *inode = *original;
}

static bool osfs3_detach_new_inode_dentry_nolock(
    uint32_t parent_ino, const char *name, uint32_t ino)
{
    for (uint32_t attempt = 0; attempt < superblock.total_inodes; attempt++) {
        uint32_t found = 0;
        int status = osfs3_find_dentry_nolock(
            parent_ino, name, true, &found, NULL);
        if (status < 0) return false;
        if (!status || found != ino) {
            if (disk_flush() < 0) return false;
            status = osfs3_find_dentry_nolock(
                parent_ino, name, true, &found, NULL);
            return status >= 0 && (!status || found != ino);
        }

        if (osfs3_remove_dentry_nolock(parent_ino, name, true, NULL) < 0) {
            status = osfs3_find_dentry_nolock(
                parent_ino, name, true, &found, NULL);
            if (status < 0 || (status > 0 && found == ino)) return false;
        }
    }
    return false;
}

static void osfs3_release_obsolete_layout_nolock(
    const osfs3_inode_t *old_layout, const osfs3_inode_t *current_layout)
{
    for (uint32_t extent = 0; extent < old_layout->extent_count; extent++) {
        uint32_t start = old_layout->extents[extent].start_block;
        uint32_t count = old_layout->extents[extent].block_count;
        for (uint32_t offset = 0; offset < count; offset++) {
            uint32_t block = start + offset;
            if (!osfs3_inode_contains_block(current_layout, block))
                osfs3_mark_run_nolock(block, 1, false);
        }
    }
}

static void osfs3_rollback_new_inode_nolock(
    uint32_t parent_ino, const osfs3_inode_t *parent_original,
    uint32_t ino, const char *name)
{
    bool detached = osfs3_detach_new_inode_dentry_nolock(
        parent_ino, name, ino);
    bool parent_durable = detached &&
        osfs3_persist_bitmaps_nolock() == 0 &&
        osfs3_persist_super_nolock() == 0 && disk_flush() == 0 &&
        osfs3_persist_inode_nolock(parent_ino) == 0 && disk_flush() == 0;
    if (detached && parent_durable) {
        bool index_ok = !path_known[ino] ||
                        osfs3_index_remove_nolock(ino) == 0;
        osfs3_release_obsolete_layout_nolock(
            parent_original, &inode_table[parent_ino]);
        osfs3_release_inode_nolock(ino);
        if (osfs3_persist_bitmaps_nolock() < 0 ||
            osfs3_persist_super_nolock() < 0 || disk_flush() < 0 ||
            osfs3_persist_inode_nolock(ino) < 0 || disk_flush() < 0) {
            serial_puts("[OsitoFS v3] create rollback persistence failed\n");
        }
        if (!index_ok && osfs3_rebuild_paths_nolock() < 0)
            serial_puts("[OsitoFS v3] create rollback index rebuild failed\n");
    } else {
        serial_puts("[OsitoFS v3] create rollback left recoverable inode=");
        serial_putdec(ino);
        serial_puts("\n");
        if (osfs3_rebuild_paths_nolock() < 0)
            serial_puts("[OsitoFS v3] create rollback index rebuild failed\n");
    }
}

static int osfs3_relocate_contiguous_nolock(osfs3_inode_t *inode,
                                             uint32_t target_blocks)
{
    if (!target_blocks) return -1;

    uint32_t run_length = 0;
    uint32_t start = osfs3_find_free_run_nolock(target_blocks, &run_length);
    if (!start || run_length != target_blocks) return -1;

    uint32_t current_blocks = osfs3_inode_block_count(inode);
    osfs3_mark_run_nolock(start, target_blocks, true);
    for (uint32_t logical = 0; logical < target_blocks; logical++) {
        if (logical < current_blocks) {
            uint32_t source = osfs3_map_file_block(inode, logical);
            if (!source || osfs3_read_block_raw(source, scratch_block) < 0)
                goto rollback;
        } else {
            memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
        }
        if (osfs3_write_block_raw(start + logical, scratch_block) < 0)
            goto rollback;
    }

    memset(inode->extents, 0, sizeof(inode->extents));
    inode->extent_count = 1;
    inode->extents[0].start_block = start;
    inode->extents[0].block_count = target_blocks;
    return 0;

rollback:
    osfs3_mark_run_nolock(start, target_blocks, false);
    return -1;
}

static void osfs3_release_inode_blocks_nolock(const osfs3_inode_t *inode)
{
    for (uint32_t i = 0; i < inode->extent_count; i++)
        osfs3_mark_run_nolock(inode->extents[i].start_block,
                              inode->extents[i].block_count, false);
}

static int osfs3_persist_file_update_nolock(uint32_t ino,
                                             osfs3_inode_t *inode,
                                             const osfs3_inode_t *original,
                                             bool relocated)
{
    uint32_t old_blocks = osfs3_inode_block_count(original);
    uint32_t new_blocks = osfs3_inode_block_count(inode);

    if (relocated) {
        /* Keep both layouts allocated until the new inode is durable. */
        if (osfs3_persist_bitmaps_nolock() < 0 ||
            osfs3_persist_super_nolock() < 0 || disk_flush() < 0 ||
            osfs3_persist_inode_nolock(ino) < 0 || disk_flush() < 0)
            return -1;

        osfs3_release_inode_blocks_nolock(original);
        if (osfs3_persist_bitmaps_nolock() < 0 ||
            osfs3_persist_super_nolock() < 0 || disk_flush() < 0)
            return -1;
        return 0;
    }

    /* Growth allocates before publishing the inode; shrink publishes the
     * shorter inode before making its old tail available for reuse. */
    if (new_blocks > old_blocks &&
        (osfs3_persist_bitmaps_nolock() < 0 ||
         osfs3_persist_super_nolock() < 0 || disk_flush() < 0))
        return -1;
    if (osfs3_persist_inode_nolock(ino) < 0)
        return -1;
    if (new_blocks != old_blocks && disk_flush() < 0)
        return -1;
    if (new_blocks < old_blocks &&
        (osfs3_persist_bitmaps_nolock() < 0 ||
         osfs3_persist_super_nolock() < 0 || disk_flush() < 0))
        return -1;
    return 0;
}

static int osfs3_zero_file_range_nolock(const osfs3_inode_t *inode,
                                        uint64_t offset, uint64_t len)
{
    while (len) {
        uint32_t logical = (uint32_t)(offset >> OSFS3_BLOCK_SHIFT);
        uint32_t in_block = (uint32_t)(offset & (OSFS3_BLOCK_SIZE - 1U));
        uint32_t physical = osfs3_map_file_block(inode, logical);
        if (!physical) return -1;
        uint64_t chunk = OSFS3_BLOCK_SIZE - in_block;
        if (chunk > len) chunk = len;
        if (in_block || chunk != OSFS3_BLOCK_SIZE) {
            if (osfs3_read_block_raw(physical, scratch_block) < 0) return -1;
        } else {
            memset(scratch_block, 0, OSFS3_BLOCK_SIZE);
        }
        memset(scratch_block + in_block, 0, chunk);
        if (osfs3_write_block_raw(physical, scratch_block) < 0) return -1;
        offset += chunk;
        len -= chunk;
    }
    return 0;
}

static int osfs3_write_file_range_nolock(const osfs3_inode_t *inode,
                                         uint64_t offset, const void *buf,
                                         uint64_t len)
{
    const uint8_t *source = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t position = offset + done;
        uint32_t logical = (uint32_t)(position >> OSFS3_BLOCK_SHIFT);
        uint32_t in_block = (uint32_t)(position & (OSFS3_BLOCK_SIZE - 1U));
        uint32_t physical = osfs3_map_file_block(inode, logical);
        if (!physical) return -1;
        uint64_t chunk = OSFS3_BLOCK_SIZE - in_block;
        if (chunk > len - done) chunk = len - done;
        if (osfs3_part_write(osfs3_block_offset(physical) + in_block,
                             source + done, chunk) < 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static int osfs3_zero_old_tail_nolock(const osfs3_inode_t *inode,
                                      uint64_t old_size, uint64_t end)
{
    if (end <= old_size || !(old_size & (OSFS3_BLOCK_SIZE - 1U))) return 0;
    uint64_t block_end = (old_size + OSFS3_BLOCK_SIZE - 1U) &
                         ~(uint64_t)(OSFS3_BLOCK_SIZE - 1U);
    if (block_end > end) block_end = end;
    return osfs3_zero_file_range_nolock(inode, old_size, block_end - old_size);
}

void *osfs3_create(const char *path, uint64_t size)
{
    char parent_path[OSFS3_PATH_MAX];
    char name[OSFS3_NAME_MAX + 1U];
    osfs3_spin_lock();
    if (!mounted) {
        osfs3_trace_create_failure(path, "not-mounted");
        osfs3_spin_unlock();
        return NULL;
    }
    if (osfs3_split_parent(path, parent_path, name) < 0) {
        osfs3_trace_create_failure(path, "bad-path");
        osfs3_spin_unlock();
        return NULL;
    }
    if (osfs3_lookup_path_nolock(path, true)) {
        osfs3_trace_create_failure(path, "already-exists");
        osfs3_spin_unlock();
        return NULL;
    }
    uint32_t parent_ino = osfs3_lookup_path_nolock(parent_path, true);
    if (!parent_ino || !osfs3_inode_is_dir(osfs3_inode(parent_ino))) {
        osfs3_trace_create_failure(path, "parent-missing");
        osfs3_spin_unlock();
        return NULL;
    }
    osfs3_inode_t parent_original = inode_table[parent_ino];

    uint32_t ino = osfs3_alloc_inode_nolock(OSFS3_S_IFREG | 0644);
    if (!ino) {
        osfs3_trace_create_failure(path, "inode-allocation");
        osfs3_spin_unlock();
        return NULL;
    }
    osfs3_inode_t *inode = &inode_table[ino];
    uint64_t blocks64 = (size + OSFS3_BLOCK_SIZE - 1U) >> OSFS3_BLOCK_SHIFT;
    if (blocks64 > UINT32_MAX ||
        osfs3_ensure_blocks_nolock(inode, (uint32_t)blocks64) < 0) {
        osfs3_trace_create_failure(path, "block-allocation");
        osfs3_release_inode_nolock(ino);
        osfs3_spin_unlock();
        return NULL;
    }
    inode->size = size;

    /* Make the inode durable before publishing its directory entry. A reset
     * between these phases leaves an orphan that mount-time recovery reclaims. */
    if (osfs3_persist_bitmaps_nolock() < 0 ||
        osfs3_persist_super_nolock() < 0 || disk_flush() < 0 ||
        osfs3_persist_inode_nolock(ino) < 0 || disk_flush() < 0) {
        osfs3_trace_create_failure(path, "persist-new-inode");
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return NULL;
    }

    bool dentry_added =
        osfs3_add_dentry_nolock(parent_ino, ino, name, OSFS3_DT_REG) == 0;
    if (!dentry_added) {
        osfs3_trace_create_failure(path, "add-dentry");
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return NULL;
    }
    if (osfs3_persist_inode_nolock(parent_ino) < 0 || disk_flush() < 0) {
        osfs3_trace_create_failure(path, "persist-parent");
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return NULL;
    }
    if (osfs3_index_add_child_nolock(parent_ino, ino, name) < 0 &&
        osfs3_rebuild_paths_nolock() < 0) {
        osfs3_trace_create_failure(path, "path-index");
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return NULL;
    }
    void *result = &inode_table[ino];
    __atomic_add_fetch(&file_revisions[ino], 2, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

int osfs3_write_data(void *file, uint64_t offset, const void *buf,
                     uint64_t len)
{
    osfs3_spin_lock();
    int ino_number = osfs3_inode_number(file);
    if (!mounted || ino_number <= 0 || (!buf && len) ||
        !osfs3_inode_valid((uint32_t)ino_number) ||
        !osfs3_inode_is_file(&inode_table[ino_number]) ||
        offset > UINT64_MAX - len) {
        osfs3_spin_unlock();
        return -1;
    }
    if (!len) {
        osfs3_spin_unlock();
        return 0;
    }

    uint32_t ino = (uint32_t)ino_number;
    osfs3_inode_t *inode = &inode_table[ino];
    uint64_t capacity = (uint64_t)osfs3_inode_block_count(inode) <<
                        OSFS3_BLOCK_SHIFT;
    if (offset + len > capacity) {
        osfs3_spin_unlock();
        return -1;
    }

    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    int result = osfs3_write_file_range_nolock(inode, offset, buf, len);
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

int osfs3_set_size_reserved(void *file, uint64_t size)
{
    osfs3_spin_lock();
    int ino_number = osfs3_inode_number(file);
    if (!mounted || ino_number <= 0 ||
        !osfs3_inode_valid((uint32_t)ino_number) ||
        !osfs3_inode_is_file(&inode_table[ino_number])) {
        osfs3_spin_unlock();
        return -1;
    }

    uint32_t ino = (uint32_t)ino_number;
    osfs3_inode_t *inode = &inode_table[ino];
    uint64_t capacity = (uint64_t)osfs3_inode_block_count(inode) <<
                        OSFS3_BLOCK_SHIFT;
    if (size > capacity) {
        osfs3_spin_unlock();
        return -1;
    }

    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    osfs3_inode_t original = *inode;
    inode->size = size;
    uint64_t now = osfs3_now();
    osfs3_preserve_birth_time(inode);
    inode->mtime = now;
    inode->ctime = now;
    inode->crc32 = 0;
    int result = osfs3_persist_inode_nolock(ino);
    if (!result) result = disk_flush();
    if (result < 0) *inode = original;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

int osfs3_write_ex(void *file, uint64_t offset, const void *buf, uint64_t len,
                   uint32_t io_flags)
{
    if (io_flags & ~OSFS_IO_FLAG_MASK) return -1;
    osfs3_spin_lock();
    int ino_number = osfs3_inode_number(file);
    if (!mounted || ino_number <= 0 || (!buf && len) ||
        !osfs3_inode_valid((uint32_t)ino_number) ||
        !osfs3_inode_is_file(&inode_table[ino_number]) ||
        offset > UINT64_MAX - len) {
        osfs3_spin_unlock();
        return -1;
    }
    if (!len) {
        osfs3_spin_unlock();
        return 0;
    }

    uint32_t ino = (uint32_t)ino_number;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    osfs3_inode_t *inode = &inode_table[ino];
    osfs3_inode_t original = *inode;
    uint64_t end = offset + len;
    uint64_t blocks64 = (end + OSFS3_BLOCK_SIZE - 1U) >> OSFS3_BLOCK_SHIFT;
    bool relocated = false;
    int result = -1;
    if (blocks64 > UINT32_MAX) {
        goto done;
    }
    if (osfs3_ensure_blocks_nolock(inode, (uint32_t)blocks64) < 0) {
        if (osfs3_relocate_contiguous_nolock(
                inode, (uint32_t)blocks64) < 0) {
            osfs3_rollback_growth_nolock(inode, &original);
            goto done;
        }
        relocated = true;
    }
    if (
        (offset > original.size &&
          osfs3_zero_old_tail_nolock(inode, original.size, offset) < 0) ||
        osfs3_write_file_range_nolock(inode, offset, buf, len) < 0) {
        osfs3_rollback_growth_nolock(inode, &original);
        goto done;
    }
    if (end > inode->size) inode->size = end;
    uint64_t now = osfs3_now();
    if (!(io_flags & OSFS_IO_PRESERVE_MTIME)) inode->mtime = now;
    if (!(io_flags & OSFS_IO_PRESERVE_CTIME)) {
        osfs3_preserve_birth_time(inode);
        inode->ctime = now;
    }
    inode->crc32 = 0;
    result = osfs3_persist_file_update_nolock(ino, inode, &original,
                                               relocated);
    if (!result && relocated) {
        serial_puts("[OsitoFS v3] compacted '");
        serial_puts(osfs3_inode_path(ino));
        serial_puts("' extents=");
        serial_putdec(original.extent_count);
        serial_puts("->1 blocks=");
        serial_putdec((uint32_t)blocks64);
        serial_puts("\n");
    }
done:
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

int osfs3_write(void *file, uint64_t offset, const void *buf, uint64_t len)
{
    return osfs3_write_ex(file, offset, buf, len, 0);
}

int osfs3_truncate_ex(void *file, uint64_t size, uint32_t io_flags)
{
    if (io_flags & ~OSFS_IO_FLAG_MASK) return -1;
    osfs3_spin_lock();
    int ino_number = osfs3_inode_number(file);
    if (!mounted || ino_number <= 0 ||
        !osfs3_inode_valid((uint32_t)ino_number) ||
        !osfs3_inode_is_file(&inode_table[ino_number])) {
        osfs3_spin_unlock();
        return -1;
    }
    uint32_t ino = (uint32_t)ino_number;
    osfs3_inode_t *inode = &inode_table[ino];
    if (size == inode->size) {
        osfs3_spin_unlock();
        return 0;
    }

    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    osfs3_inode_t original = *inode;
    uint64_t blocks64 = (size + OSFS3_BLOCK_SIZE - 1U) >> OSFS3_BLOCK_SHIFT;
    bool relocated = false;
    int result = -1;
    if (blocks64 > UINT32_MAX) {
        goto done;
    }
    if (size > original.size) {
        if (osfs3_ensure_blocks_nolock(inode, (uint32_t)blocks64) < 0) {
            if (osfs3_relocate_contiguous_nolock(
                inode, (uint32_t)blocks64) < 0) {
                osfs3_rollback_growth_nolock(inode, &original);
                goto done;
            }
            relocated = true;
        }
        if (osfs3_zero_old_tail_nolock(inode, original.size, size) < 0) {
            osfs3_rollback_growth_nolock(inode, &original);
            goto done;
        }
    } else {
        if (blocks64 && (size & (OSFS3_BLOCK_SIZE - 1U))) {
            uint64_t tail = OSFS3_BLOCK_SIZE -
                            (size & (OSFS3_BLOCK_SIZE - 1U));
            if (osfs3_zero_file_range_nolock(inode, size, tail) < 0) {
                goto done;
            }
        }
        osfs3_release_tail_nolock(inode, (uint32_t)blocks64);
    }
    inode->size = size;
    uint64_t now = osfs3_now();
    if (!(io_flags & OSFS_IO_PRESERVE_MTIME)) inode->mtime = now;
    if (!(io_flags & OSFS_IO_PRESERVE_CTIME)) {
        osfs3_preserve_birth_time(inode);
        inode->ctime = now;
    }
    inode->crc32 = 0;
    result = osfs3_persist_file_update_nolock(ino, inode, &original,
                                               relocated);
done:
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

int osfs3_truncate(void *file, uint64_t size)
{
    return osfs3_truncate_ex(file, size, 0);
}

int osfs3_mkdir(const char *path)
{
    char parent_path[OSFS3_PATH_MAX];
    char name[OSFS3_NAME_MAX + 1U];
    osfs3_spin_lock();
    if (!mounted || osfs3_split_parent(path, parent_path, name) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    if (osfs3_lookup_path_nolock(path, true)) {
        osfs3_spin_unlock();
        return -2;
    }
    uint32_t parent_ino = osfs3_lookup_path_nolock(parent_path, true);
    if (!parent_ino || !osfs3_inode_is_dir(osfs3_inode(parent_ino))) {
        osfs3_spin_unlock();
        return -3;
    }
    osfs3_inode_t parent_original = inode_table[parent_ino];

    uint32_t ino = osfs3_alloc_inode_nolock(OSFS3_S_IFDIR | 0755);
    if (!ino || osfs3_initialize_directory_nolock(ino, parent_ino) < 0) {
        if (ino) osfs3_release_inode_nolock(ino);
        osfs3_spin_unlock();
        return -1;
    }

    if (osfs3_persist_bitmaps_nolock() < 0 ||
        osfs3_persist_super_nolock() < 0 || disk_flush() < 0 ||
        osfs3_persist_inode_nolock(ino) < 0 || disk_flush() < 0) {
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return -1;
    }

    bool dentry_added =
        osfs3_add_dentry_nolock(parent_ino, ino, name, OSFS3_DT_DIR) == 0;
    if (!dentry_added ||
        osfs3_persist_inode_nolock(parent_ino) < 0 ||
        disk_flush() < 0) {
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return -1;
    }
    if (osfs3_index_add_child_nolock(parent_ino, ino, name) < 0 &&
        osfs3_rebuild_paths_nolock() < 0) {
        osfs3_rollback_new_inode_nolock(
            parent_ino, &parent_original, ino, name);
        osfs3_spin_unlock();
        return -1;
    }
    osfs3_spin_unlock();
    return 0;
}

int osfs3_delete(const char *path)
{
    char parent_path[OSFS3_PATH_MAX];
    char name[OSFS3_NAME_MAX + 1U];
    osfs3_spin_lock();
    if (!mounted || osfs3_split_parent(path, parent_path, name) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    uint32_t parent_ino = osfs3_lookup_path_nolock(parent_path, true);
    uint32_t ino = parent_ino
        ? osfs3_find_in_directory_nolock(parent_ino, name, true, NULL) : 0;
    if (!ino || !osfs3_inode_is_file(osfs3_inode(ino))) {
        osfs3_spin_unlock();
        return -1;
    }
    if (osfs3_file_busy_nolock(ino)) {
        osfs3_spin_unlock();
        return -2;
    }
    if (osfs3_remove_dentry_nolock(parent_ino, name, true, NULL) < 0 ||
        osfs3_persist_inode_nolock(parent_ino) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    bool index_ok = osfs3_index_remove_nolock(ino) == 0;
    osfs3_release_inode_nolock(ino);
    if (osfs3_persist_inode_nolock(ino) < 0 ||
        osfs3_persist_bitmaps_nolock() < 0 ||
        osfs3_persist_super_nolock() < 0 ||
        (!index_ok && osfs3_rebuild_paths_nolock() < 0)) {
        osfs3_spin_unlock();
        return -1;
    }
    disk_flush();
    osfs3_spin_unlock();
    return 0;
}

int osfs3_rmdir(const char *path)
{
    char parent_path[OSFS3_PATH_MAX];
    char name[OSFS3_NAME_MAX + 1U];
    osfs3_spin_lock();
    if (!mounted || osfs3_split_parent(path, parent_path, name) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    uint32_t parent_ino = osfs3_lookup_path_nolock(parent_path, true);
    uint32_t ino = parent_ino
        ? osfs3_find_in_directory_nolock(parent_ino, name, true, NULL) : 0;
    if (!ino || ino == superblock.root_inode ||
        !osfs3_inode_is_dir(osfs3_inode(ino))) {
        osfs3_spin_unlock();
        return -1;
    }
    if (!osfs3_directory_empty_nolock(ino)) {
        osfs3_spin_unlock();
        return -2;
    }
    if (osfs3_file_busy_nolock(ino)) {
        osfs3_spin_unlock();
        return -3;
    }
    if (osfs3_remove_dentry_nolock(parent_ino, name, true, NULL) < 0 ||
        osfs3_persist_inode_nolock(parent_ino) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    bool index_ok = osfs3_index_remove_nolock(ino) == 0;
    osfs3_release_inode_nolock(ino);
    if (osfs3_persist_inode_nolock(ino) < 0 ||
        osfs3_persist_bitmaps_nolock() < 0 ||
        osfs3_persist_super_nolock() < 0 ||
        (!index_ok && osfs3_rebuild_paths_nolock() < 0)) {
        osfs3_spin_unlock();
        return -1;
    }
    disk_flush();
    osfs3_spin_unlock();
    return 0;
}

static int osfs3_rename_dentry_in_place_nolock(uint32_t dir_ino,
                                                const char *old_name,
                                                const char *new_name)
{
    osfs3_dentry_location_t location;
    if (!osfs3_find_in_directory_nolock(dir_ino, old_name, true, &location) ||
        osfs3_read_block_raw(location.block, scratch_block) < 0)
        return -1;
    osfs3_dentry_t *entry =
        (osfs3_dentry_t *)(scratch_block + location.offset);
    uint32_t new_length = (uint32_t)strlen(new_name);
    if (OSFS3_DIR_REC_LEN(new_length) > entry->rec_len) return 1;
    memset(entry->name, 0, entry->rec_len - sizeof(*entry));
    entry->name_len = (uint8_t)new_length;
    memcpy(entry->name, new_name, new_length);
    int result = osfs3_write_block_raw(location.block, scratch_block);
    if (!result)
        osfs3_touch_directory_nolock(&inode_table[dir_ino]);
    return result;
}

static bool osfs3_path_is_descendant_ci(const char *parent, const char *path)
{
    while (*parent && *path &&
           osfs3_path_fold(*parent) == osfs3_path_fold(*path)) {
        parent++;
        path++;
    }
    return !*parent && (*path == '\\' || *path == '/');
}

int osfs3_rename(const char *from, const char *to, bool replace)
{
    char from_normalized[OSFS3_PATH_MAX];
    char to_normalized[OSFS3_PATH_MAX];
    char from_parent_path[OSFS3_PATH_MAX];
    char to_parent_path[OSFS3_PATH_MAX];
    char from_name[OSFS3_NAME_MAX + 1U];
    char to_name[OSFS3_NAME_MAX + 1U];

    osfs3_spin_lock();
    if (!mounted || !osfs3_normalize_path(from, from_normalized) ||
        !osfs3_normalize_path(to, to_normalized) ||
        osfs3_split_parent(from, from_parent_path, from_name) < 0 ||
        osfs3_split_parent(to, to_parent_path, to_name) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    if (strcmp(from_normalized, to_normalized) == 0) {
        osfs3_spin_unlock();
        return 0;
    }

    uint32_t from_parent =
        osfs3_lookup_path_nolock(from_parent_path, true);
    uint32_t to_parent = osfs3_lookup_path_nolock(to_parent_path, true);
    uint32_t source = from_parent
        ? osfs3_find_in_directory_nolock(from_parent, from_name, true, NULL) : 0;
    if (!source || !to_parent ||
        !osfs3_inode_is_dir(osfs3_inode(to_parent))) {
        osfs3_spin_unlock();
        return -1;
    }
    osfs3_inode_t *source_inode = &inode_table[source];
    if (osfs3_inode_is_dir(source_inode) &&
        osfs3_path_is_descendant_ci(from_normalized, to_parent_path)) {
        osfs3_spin_unlock();
        return -1;
    }

    uint32_t target = osfs3_find_in_directory_nolock(
        to_parent, to_name, true, NULL);
    if (target == source) {
        int result = -1;
        if (from_parent == to_parent)
            result = osfs3_rename_dentry_in_place_nolock(
                from_parent, from_name, to_name);
        if (result == 1) {
            if (osfs3_remove_dentry_nolock(from_parent, from_name, true,
                                           NULL) == 0 &&
                osfs3_add_dentry_nolock(to_parent, source, to_name,
                    osfs3_inode_dentry_type(source_inode)) == 0)
                result = 0;
        }
        if (!result) {
            osfs3_preserve_birth_time(source_inode);
            source_inode->ctime = osfs3_now();
            bool index_ok =
                osfs3_index_rename_nolock(source, to_parent, to_name) == 0;
            result = osfs3_persist_inode_nolock(source) < 0 ||
                     osfs3_persist_inode_nolock(from_parent) < 0 ||
                     (!index_ok && osfs3_rebuild_paths_nolock() < 0)
                     ? -1 : 0;
            disk_flush();
        }
        osfs3_spin_unlock();
        return result;
    }

    if (target) {
        osfs3_inode_t *target_inode = &inode_table[target];
        if (!replace || osfs3_inode_is_dir(target_inode) !=
                        osfs3_inode_is_dir(source_inode)) {
            osfs3_spin_unlock();
            return -2;
        }
        if (osfs3_file_busy_nolock(target) ||
            (osfs3_inode_is_dir(target_inode) &&
             !osfs3_directory_empty_nolock(target))) {
            osfs3_spin_unlock();
            return -3;
        }
        if (osfs3_remove_dentry_nolock(to_parent, to_name, true, NULL) < 0) {
            osfs3_spin_unlock();
            return -1;
        }
    }

    uint8_t source_type = osfs3_inode_dentry_type(source_inode);
    if (osfs3_add_dentry_nolock(to_parent, source, to_name, source_type) < 0) {
        if (target)
            osfs3_add_dentry_nolock(to_parent, target, to_name,
                osfs3_inode_dentry_type(&inode_table[target]));
        osfs3_spin_unlock();
        return -1;
    }
    if (osfs3_remove_dentry_nolock(from_parent, from_name, true, NULL) < 0) {
        osfs3_remove_dentry_nolock(to_parent, to_name, true, NULL);
        if (target)
            osfs3_add_dentry_nolock(to_parent, target, to_name,
                osfs3_inode_dentry_type(&inode_table[target]));
        osfs3_spin_unlock();
        return -1;
    }

    if (osfs3_inode_is_dir(source_inode) && from_parent != to_parent &&
        osfs3_update_dotdot_nolock(source, to_parent) < 0) {
        osfs3_spin_unlock();
        return -1;
    }
    osfs3_preserve_birth_time(source_inode);
    source_inode->ctime = osfs3_now();
    bool index_ok = true;
    if (target && osfs3_index_remove_nolock(target) < 0) index_ok = false;
    if (osfs3_index_rename_nolock(source, to_parent, to_name) < 0)
        index_ok = false;
    if (target) osfs3_release_inode_nolock(target);

    if (osfs3_persist_inode_nolock(source) < 0 ||
        osfs3_persist_inode_nolock(from_parent) < 0 ||
        (to_parent != from_parent && osfs3_persist_inode_nolock(to_parent) < 0) ||
        (target && osfs3_persist_inode_nolock(target) < 0) ||
        osfs3_persist_bitmaps_nolock() < 0 ||
        osfs3_persist_super_nolock() < 0 ||
        (!index_ok && osfs3_rebuild_paths_nolock() < 0)) {
        osfs3_spin_unlock();
        return -1;
    }
    disk_flush();
    osfs3_spin_unlock();
    return 0;
}

uint64_t osfs3_get_size(uint32_t ino)
{
    return mounted && osfs3_inode_valid(ino) ? inode_table[ino].size : 0;
}

int osfs3_get_mode(uint32_t ino, uint16_t *mode)
{
    if (!mode) return -1;
    osfs3_spin_lock();
    if (!mounted || !osfs3_inode_valid(ino)) {
        osfs3_spin_unlock();
        return -1;
    }
    *mode = inode_table[ino].mode;
    osfs3_spin_unlock();
    return 0;
}

int osfs3_set_mode(uint32_t ino, uint16_t mode)
{
    if (mode & ~07777U) return -1;
    osfs3_spin_lock();
    if (!mounted || !osfs3_inode_valid(ino)) {
        osfs3_spin_unlock();
        return -1;
    }

    osfs3_inode_t *inode = &inode_table[ino];
    uint16_t updated = (uint16_t)((inode->mode & OSFS3_S_IFMT) | mode);
    if (updated == inode->mode) {
        osfs3_spin_unlock();
        return 0;
    }

    osfs3_inode_t original = *inode;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    inode->mode = updated;
    osfs3_preserve_birth_time(inode);
    inode->ctime = osfs3_now();
    inode->crc32 = 0;
    int result = osfs3_persist_inode_nolock(ino);
    if (!result) result = disk_flush();
    if (result < 0) *inode = original;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

int osfs3_file_get_mode(const void *file, uint16_t *mode)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 ? osfs3_get_mode((uint32_t)ino, mode) : -1;
}

int osfs3_file_set_mode(void *file, uint16_t mode)
{
    int ino = osfs3_inode_number(file);
    return ino > 0 ? osfs3_set_mode((uint32_t)ino, mode) : -1;
}

int osfs3_file_get_dos_attributes(const void *file, uint8_t *attributes)
{
    if (!attributes) return -1;
    osfs3_spin_lock();
    int ino = osfs3_inode_number(file);
    if (!mounted || ino <= 0 || !osfs3_inode_valid((uint32_t)ino)) {
        osfs3_spin_unlock();
        return -1;
    }

    const osfs3_inode_t *inode = &inode_table[ino];
    uint8_t value = 0;
    if (!(inode->mode & 0222U)) value |= OSFS_DOS_ATTR_READ_ONLY;
    if (inode->flags & OSFS3_INODE_FLAG_DOS_HIDDEN)
        value |= OSFS_DOS_ATTR_HIDDEN;
    if (inode->flags & OSFS3_INODE_FLAG_DOS_SYSTEM)
        value |= OSFS_DOS_ATTR_SYSTEM;
    if (osfs3_inode_is_file(inode) &&
        !(inode->flags & OSFS3_INODE_FLAG_DOS_NOARCH))
        value |= OSFS_DOS_ATTR_ARCHIVE;
    *attributes = value;
    osfs3_spin_unlock();
    return 0;
}

int osfs3_file_set_dos_attributes(void *file, uint8_t attributes)
{
    if (attributes & ~OSFS_DOS_ATTR_MASK) return -1;
    osfs3_spin_lock();
    int ino_number = osfs3_inode_number(file);
    if (!mounted || ino_number <= 0 ||
        !osfs3_inode_valid((uint32_t)ino_number)) {
        osfs3_spin_unlock();
        return -1;
    }

    uint32_t ino = (uint32_t)ino_number;
    osfs3_inode_t *inode = &inode_table[ino];
    uint16_t permissions = inode->mode & 07777U;
    if (attributes & OSFS_DOS_ATTR_READ_ONLY)
        permissions &= (uint16_t)~0222U;
    else if (!(permissions & 0222U))
        permissions |= 0200U;

    uint32_t flags = inode->flags &
        ~(OSFS3_INODE_FLAG_DOS_HIDDEN |
          OSFS3_INODE_FLAG_DOS_SYSTEM |
          OSFS3_INODE_FLAG_DOS_NOARCH);
    if (attributes & OSFS_DOS_ATTR_HIDDEN)
        flags |= OSFS3_INODE_FLAG_DOS_HIDDEN;
    if (attributes & OSFS_DOS_ATTR_SYSTEM)
        flags |= OSFS3_INODE_FLAG_DOS_SYSTEM;
    if (!(attributes & OSFS_DOS_ATTR_ARCHIVE))
        flags |= OSFS3_INODE_FLAG_DOS_NOARCH;

    uint16_t mode = (uint16_t)((inode->mode & OSFS3_S_IFMT) | permissions);
    if (mode == inode->mode && flags == inode->flags) {
        osfs3_spin_unlock();
        return 0;
    }

    osfs3_inode_t original = *inode;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_ACQ_REL);
    inode->mode = mode;
    inode->flags = flags;
    osfs3_preserve_birth_time(inode);
    inode->ctime = osfs3_now();
    inode->crc32 = 0;
    int result = osfs3_persist_inode_nolock(ino);
    if (!result) result = disk_flush();
    if (result < 0) *inode = original;
    __atomic_add_fetch(&file_revisions[ino], 1, __ATOMIC_RELEASE);
    osfs3_spin_unlock();
    return result;
}

bool osfs3_is_mounted(void)
{
    return mounted;
}

uint32_t osfs3_file_count(void)
{
    return mounted ? regular_file_count : 0;
}

uint32_t osfs3_free_blocks(void)
{
    return mounted ? superblock.free_blocks : 0;
}

uint32_t osfs3_total_blocks(void)
{
    return mounted && superblock.total_blocks > superblock.first_data_block
        ? superblock.total_blocks - superblock.first_data_block : 0;
}

uint32_t osfs3_max_files(void)
{
    return mounted ? superblock.total_inodes : 0;
}

uint32_t osfs3_block_size(void)
{
    return mounted ? OSFS3_BLOCK_SIZE : 0;
}

const char *osfs3_label(void)
{
    return mounted ? superblock.label : "";
}
