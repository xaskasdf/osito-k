#include "cpu8086.h"
#include "dos_find.h"
#include "dos_time.h"
#include "../fs/ositofs3.h"
#include "../fs/ositofs_metadata.h"

extern bool osfs2_is_mounted(void);
extern void *osfs2_find_exact_ci(const char *name);
extern void *osfs2_get_file(int index);
extern uint32_t osfs2_max_files(void);
extern const char *osfs2_file_name(void *file);
extern uint64_t osfs2_file_size(void *file);
extern bool osfs2_directory_exists_ci(const char *directory);
extern int osfs2_directory_representative_ci(const char *directory);
extern const char *osfs2_label(void);

enum {
    DOS_FIND_ERR_FILE_NOT_FOUND = 2,
    DOS_FIND_ERR_PATH_NOT_FOUND = 3,
    DOS_FIND_ERR_ACCESS_DENIED = 5,
    DOS_FIND_ERR_NO_MORE_FILES = 18
};

enum {
    DOS_ATTR_READ_ONLY = 0x01,
    DOS_ATTR_HIDDEN = 0x02,
    DOS_ATTR_SYSTEM = 0x04,
    DOS_ATTR_VOLUME = 0x08,
    DOS_ATTR_DIRECTORY = 0x10,
    DOS_ATTR_ARCHIVE = 0x20
};

enum {
    DOS_DTA_SEARCH_MAGIC = 0x5344534FU,
    DOS_DTA_SEARCH_VERSION = 1,
    DOS_DTA_RESULT_OFFSET = 21,
    DOS_DTA_ATTRIBUTE_OFFSET = 21,
    DOS_DTA_TIME_OFFSET = 22,
    DOS_DTA_DATE_OFFSET = 24,
    DOS_DTA_SIZE_OFFSET = 26,
    DOS_DTA_NAME_OFFSET = 30,
    DOS_DTA_SIZE = 43,
    DOS_DTA_STATE_VOLUME_DONE = 0x01
};

typedef struct {
    bool use_osfs3;
    int32_t next_index;
    osfs3_dir_cursor_t osfs3;
    char directory[DOS_SEARCH_PATH_MAX];
} dos_dir_iter_t;

typedef struct {
    char name[DOS_SEARCH_PATH_MAX];
    bool is_directory;
    uint64_t size;
    uint64_t identity;
    void *node;
} dos_dir_entry_t;

static bool dos_find_sep(char ch)
{
    return ch == '/' || ch == '\\';
}

static uint8_t dos_find_upper(uint8_t ch)
{
    return ch >= 'a' && ch <= 'z' ? (uint8_t)(ch - ('a' - 'A')) : ch;
}

static bool dos_find_name_equal(const char *left, const char *right)
{
    while (*left && *right &&
           dos_find_upper((uint8_t)*left) ==
               dos_find_upper((uint8_t)*right)) {
        left++;
        right++;
    }
    return !*left && !*right;
}

static uint64_t dos_find_name_hash(const char *name)
{
    uint64_t hash = 1469598103934665603ULL;
    while (*name) {
        hash ^= dos_find_upper((uint8_t)*name++);
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

static bool dos_find_copy(char *destination, uint32_t capacity,
                          const char *source)
{
    if (!destination || !capacity || !source) return false;
    uint32_t length = 0;
    while (source[length]) {
        if (length + 1U >= capacity) return false;
        destination[length] = source[length];
        length++;
    }
    destination[length] = 0;
    return true;
}

static bool dos_find_join(const char *directory, const char *child,
                          char *path, uint32_t capacity)
{
    uint32_t length = 0;
    if (!path || !capacity || !directory || !child) return false;
    while (directory[length]) {
        if (length + 1U >= capacity) return false;
        path[length] = directory[length];
        length++;
    }
    if (length) {
        if (length + 1U >= capacity) return false;
        path[length++] = '/';
    }
    for (uint32_t i = 0; child[i]; i++) {
        if (length + 1U >= capacity) return false;
        path[length++] = child[i];
    }
    path[length] = 0;
    return true;
}

static bool dos_find_path_child(const char *directory, const char *stored,
                                char child[DOS_SEARCH_PATH_MAX],
                                bool *is_directory)
{
    const char *path = stored;
    const char *prefix = directory;
    while (dos_find_sep(*path)) path++;
    while (*prefix) {
        if (!*path) return false;
        bool prefix_separator = dos_find_sep(*prefix);
        bool path_separator = dos_find_sep(*path);
        if (prefix_separator || path_separator) {
            if (prefix_separator != path_separator) return false;
        } else if (dos_find_upper((uint8_t)*prefix) !=
                   dos_find_upper((uint8_t)*path)) {
            return false;
        }
        prefix++;
        path++;
    }
    if (*directory) {
        if (!dos_find_sep(*path)) return false;
        while (dos_find_sep(*path)) path++;
    }
    if (!*path) return false;

    uint32_t length = 0;
    while (*path && !dos_find_sep(*path)) {
        if (length + 1U >= DOS_SEARCH_PATH_MAX) return false;
        child[length++] = *path++;
    }
    child[length] = 0;
    if (!length) return false;
    *is_directory = dos_find_sep(*path);
    return true;
}

static bool dos_find_v2_seen_before(const char *directory,
                                    const char *child, int32_t before)
{
    for (int32_t index = 0; index < before; index++) {
        void *file = osfs2_get_file(index);
        const char *stored = file ? osfs2_file_name(file) : NULL;
        char prior[DOS_SEARCH_PATH_MAX];
        bool ignored;
        if (stored && dos_find_path_child(directory, stored, prior, &ignored) &&
            dos_find_name_equal(prior, child))
            return true;
    }
    return false;
}

static int dos_dir_iter_open(const char *directory, dos_dir_iter_t *iterator)
{
    if (!directory || !iterator || !osfs2_is_mounted()) return -1;
    memset(iterator, 0, sizeof(*iterator));
    if (!dos_find_copy(iterator->directory, sizeof(iterator->directory),
                       directory))
        return -1;
    iterator->use_osfs3 = osfs3_is_mounted();
    if (iterator->use_osfs3)
        return osfs3_dir_cursor_open_ci(directory, &iterator->osfs3);
    if (*directory && !osfs2_directory_exists_ci(directory)) return -1;
    return 0;
}

static int dos_dir_iter_next(dos_dir_iter_t *iterator,
                             dos_dir_entry_t *entry)
{
    if (!iterator || !entry) return -1;
    memset(entry, 0, sizeof(*entry));

    if (iterator->use_osfs3) {
        uint32_t inode = 0;
        int result = osfs3_dir_cursor_next(
            &iterator->osfs3, entry->name, sizeof(entry->name),
            &entry->is_directory, &entry->size, &inode);
        if (result <= 0) return result;
        entry->node = osfs3_get_node((int)inode);
        entry->identity = inode;
        return 1;
    }

    int32_t limit = (int32_t)osfs2_max_files();
    while (iterator->next_index < limit) {
        int32_t index = iterator->next_index++;
        void *file = osfs2_get_file(index);
        const char *stored = file ? osfs2_file_name(file) : NULL;
        bool implied_directory;
        if (!stored || !dos_find_path_child(iterator->directory, stored,
                                             entry->name,
                                             &implied_directory))
            continue;

        char full_path[DOS_SEARCH_PATH_MAX];
        if (!dos_find_join(iterator->directory, entry->name, full_path,
                           sizeof(full_path)))
            continue;
        if (implied_directory) {
            int representative =
                osfs2_directory_representative_ci(full_path);
            if ((representative >= 0 && representative != index) ||
                (representative < 0 &&
                 dos_find_v2_seen_before(iterator->directory, entry->name,
                                         index)))
                continue;
        }
        entry->is_directory = implied_directory ||
            osfs2_directory_exists_ci(full_path);
        entry->size = entry->is_directory ? 0 : osfs2_file_size(file);
        entry->identity = entry->is_directory
            ? dos_find_name_hash(full_path) : osfs2_file_id(file);
        entry->node = entry->is_directory ? NULL : file;
        return 1;
    }
    return 0;
}

static bool dos_short_character(uint8_t ch)
{
    ch = dos_find_upper(ch);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch >= 0x80)
        return true;
    static const char allowed[] = "$%'-_@~`!(){}^#&";
    for (uint32_t i = 0; allowed[i]; i++)
        if (ch == (uint8_t)allowed[i]) return true;
    return false;
}

static bool dos_name_is_short(const char *name, const char **dot_out)
{
    const char *dot = NULL;
    uint32_t base_length = 0;
    uint32_t extension_length = 0;
    if (!name || !*name || dos_find_name_equal(name, ".") ||
        dos_find_name_equal(name, ".."))
        return false;
    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            if (dot) return false;
            dot = p;
            continue;
        }
        if (!dos_short_character((uint8_t)*p)) return false;
        if (dot) extension_length++;
        else base_length++;
    }
    if (!base_length || base_length > 8U || extension_length > 3U)
        return false;
    if (dot_out) *dot_out = dot;
    return true;
}

static void dos_make_short_name(const char *name, uint64_t identity,
                                char output[13])
{
    const char *dot = NULL;
    uint32_t out = 0;
    if (dos_name_is_short(name, &dot)) {
        for (const char *p = name; *p && p != dot; p++)
            output[out++] = (char)dos_find_upper((uint8_t)*p);
        if (dot && dot[1]) {
            output[out++] = '.';
            for (const char *p = dot + 1; *p; p++)
                output[out++] = (char)dos_find_upper((uint8_t)*p);
        }
        output[out] = 0;
        return;
    }

    const char *last_dot = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.') last_dot = p;
    const char *base_end = last_dot && last_dot != name ? last_dot
                                                        : name + strlen(name);
    for (const char *p = name; p < base_end && out < 3U; p++) {
        uint8_t ch = dos_find_upper((uint8_t)*p);
        if (dos_short_character(ch)) output[out++] = (char)ch;
    }
    while (out < 3U) output[out++] = '_';
    output[out++] = '~';

    static const char base36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint32_t suffix = (uint32_t)((identity ? identity :
                                  dos_find_name_hash(name)) % 1679616ULL);
    for (int shift = 3; shift >= 0; shift--) {
        uint32_t divisor = 1;
        for (int i = 0; i < shift; i++) divisor *= 36U;
        output[out++] = base36[(suffix / divisor) % 36U];
    }

    if (last_dot && last_dot[1]) {
        uint32_t extension = 0;
        uint32_t dot_index = out++;
        output[dot_index] = '.';
        for (const char *p = last_dot + 1; *p && extension < 3U; p++) {
            uint8_t ch = dos_find_upper((uint8_t)*p);
            if (dos_short_character(ch)) {
                output[out++] = (char)ch;
                extension++;
            }
        }
        if (!extension) out = dot_index;
    }
    output[out] = 0;
}

static bool dos_wildcard_match(const char *pattern, const char *name)
{
    if (dos_find_name_equal(pattern, "*") ||
        dos_find_name_equal(pattern, "*.*"))
        return true;

    const char *star = NULL;
    const char *retry = NULL;
    while (*name) {
        if (*pattern == '?' ||
            (*pattern != '*' &&
             dos_find_upper((uint8_t)*pattern) ==
                 dos_find_upper((uint8_t)*name))) {
            pattern++;
            name++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = name;
        } else if (star) {
            pattern = star + 1;
            name = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') pattern++;
    if (pattern[0] == '.' && pattern[1] == '*' && !pattern[2])
        return true;
    return !*pattern;
}

static uint8_t dos_entry_attributes(const dos_dir_entry_t *entry)
{
    uint8_t attributes = 0;
    bool loaded = entry->node &&
        osfs2_file_get_dos_attributes(entry->node, &attributes) == 0;
    if (entry->is_directory) {
        attributes &= (uint8_t)~DOS_ATTR_ARCHIVE;
        attributes |= DOS_ATTR_DIRECTORY;
    } else if (!loaded) {
        attributes |= DOS_ATTR_ARCHIVE;
    }
    if (entry->name[0] == '.') attributes |= DOS_ATTR_HIDDEN;
    return attributes;
}

static bool dos_attributes_match(uint8_t entry, uint16_t requested)
{
    uint8_t opt_in = DOS_ATTR_HIDDEN | DOS_ATTR_SYSTEM | DOS_ATTR_DIRECTORY;
    return !(entry & opt_in & ~(uint8_t)requested);
}

static bool dos_dta_address(dos_vm_t *vm, uint32_t *address)
{
    if (!vm || !vm->mem || !address) return false;
    uint32_t value = dos_addr(vm, vm->dta_seg, vm->dta_off);
    if (value > vm->total_mem_size ||
        DOS_DTA_SIZE > vm->total_mem_size - value)
        return false;
    *address = value;
    return true;
}

static void dos_dta_write_cursor(dos_vm_t *vm, uint32_t address,
                                 const dos_search_t *search,
                                 const dos_dir_iter_t *iterator,
                                 uint8_t state)
{
    dos_mem_write32(vm, address, DOS_DTA_SEARCH_MAGIC);
    dos_mem_write8(vm, address + 4U,
                   (uint8_t)(search - vm->searches));
    dos_mem_write8(vm, address + 5U,
                   search->use_osfs3 ? DOS_DTA_SEARCH_VERSION | 0x80U
                                     : DOS_DTA_SEARCH_VERSION);
    dos_mem_write16(vm, address + 6U, search->token);
    if (search->use_osfs3) {
        dos_mem_write32(vm, address + 8U, iterator->osfs3.directory_inode);
        dos_mem_write32(vm, address + 12U, iterator->osfs3.next_inode);
        dos_mem_write32(vm, address + 16U, iterator->osfs3.generation);
    } else {
        dos_mem_write32(vm, address + 8U, (uint32_t)iterator->next_index);
        dos_mem_write32(vm, address + 12U, 0);
        dos_mem_write32(vm, address + 16U, 0);
    }
    dos_mem_write8(vm, address + 20U, state);
}

static dos_search_t *dos_dta_search(dos_vm_t *vm, uint32_t address,
                                    dos_dir_iter_t *iterator,
                                    uint8_t *state)
{
    if (dos_mem_read32(vm, address) != DOS_DTA_SEARCH_MAGIC) return NULL;
    uint8_t slot = dos_mem_read8(vm, address + 4U);
    uint8_t version = dos_mem_read8(vm, address + 5U);
    uint16_t token = dos_mem_read16(vm, address + 6U);
    if (slot >= DOS_MAX_SEARCHES ||
        (version & 0x7FU) != DOS_DTA_SEARCH_VERSION)
        return NULL;
    dos_search_t *search = &vm->searches[slot];
    if (!search->used || search->token != token ||
        search->use_osfs3 != ((version & 0x80U) != 0))
        return NULL;

    memset(iterator, 0, sizeof(*iterator));
    iterator->use_osfs3 = search->use_osfs3;
    if (!dos_find_copy(iterator->directory, sizeof(iterator->directory),
                       search->directory))
        return NULL;
    if (search->use_osfs3) {
        iterator->osfs3.directory_inode = dos_mem_read32(vm, address + 8U);
        iterator->osfs3.next_inode = dos_mem_read32(vm, address + 12U);
        iterator->osfs3.generation = dos_mem_read32(vm, address + 16U);
    } else {
        iterator->next_index = (int32_t)dos_mem_read32(vm, address + 8U);
    }
    *state = dos_mem_read8(vm, address + 20U);
    return search;
}

static void dos_search_finish(dos_vm_t *vm, uint32_t address,
                              dos_search_t *search)
{
    if (search) search->used = false;
    dos_mem_write32(vm, address, 0);
}

static dos_search_t *dos_search_allocate(dos_vm_t *vm, uint32_t address)
{
    dos_dir_iter_t old_iterator;
    uint8_t old_state;
    dos_search_t *old = dos_dta_search(vm, address, &old_iterator, &old_state);
    if (old) old->used = false;

    int slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint32_t i = 0; i < DOS_MAX_SEARCHES; i++) {
        if (!vm->searches[i].used) {
            slot = (int)i;
            break;
        }
        if (vm->searches[i].serial < oldest) {
            oldest = vm->searches[i].serial;
            slot = (int)i;
        }
    }
    if (slot < 0) return NULL;

    dos_search_t *search = &vm->searches[slot];
    *search = (dos_search_t){0};
    vm->next_search_token++;
    if (!vm->next_search_token) vm->next_search_token++;
    vm->next_search_serial++;
    search->used = true;
    search->token = vm->next_search_token;
    search->serial = vm->next_search_serial;
    return search;
}

static void dos_fill_dta(dos_vm_t *vm, uint32_t address,
                         const dos_dir_entry_t *entry,
                         const char short_name[13], uint8_t attributes)
{
    for (uint32_t i = DOS_DTA_RESULT_OFFSET; i < DOS_DTA_SIZE; i++)
        dos_mem_write8(vm, address + i, 0);
    dos_mem_write8(vm, address + DOS_DTA_ATTRIBUTE_OFFSET, attributes);

    uint16_t date = 0x0021U;
    uint16_t time = 0;
    osfs_file_times_t times;
    if (entry && entry->node &&
        osfs2_file_get_times(entry->node, &times) == 0)
        (void)dos_pack_datetime(times.modified, &date, &time);
    dos_mem_write16(vm, address + DOS_DTA_TIME_OFFSET, time);
    dos_mem_write16(vm, address + DOS_DTA_DATE_OFFSET, date);
    uint64_t size = entry ? entry->size : 0;
    dos_mem_write32(vm, address + DOS_DTA_SIZE_OFFSET,
                    size > UINT32_MAX ? UINT32_MAX : (uint32_t)size);
    for (uint32_t i = 0; short_name[i] && i < 12U; i++)
        dos_mem_write8(vm, address + DOS_DTA_NAME_OFFSET + i,
                       (uint8_t)short_name[i]);
}

void dos_find_init(dos_vm_t *vm)
{
    if (!vm) return;
    for (uint32_t i = 0; i < DOS_MAX_SEARCHES; i++)
        vm->searches[i] = (dos_search_t){0};
    vm->next_search_token = 0;
    vm->next_search_serial = 0;
}

void dos_find_close_all(dos_vm_t *vm)
{
    dos_find_init(vm);
}

int dos_find_first(dos_vm_t *vm, const char *path, uint16_t attributes)
{
    uint32_t address;
    if (!path || !*path || !dos_dta_address(vm, &address))
        return DOS_FIND_ERR_PATH_NOT_FOUND;

    const char *last_separator = NULL;
    for (const char *p = path; *p; p++)
        if (dos_find_sep(*p)) last_separator = p;
    const char *pattern = last_separator ? last_separator + 1 : path;
    if (!*pattern) return DOS_FIND_ERR_PATH_NOT_FOUND;

    char directory[DOS_SEARCH_PATH_MAX];
    uint32_t directory_length = last_separator
        ? (uint32_t)(last_separator - path) : 0;
    if (directory_length >= sizeof(directory))
        return DOS_FIND_ERR_PATH_NOT_FOUND;
    memcpy(directory, path, directory_length);
    directory[directory_length] = 0;

    char resolved_directory[DOS_SEARCH_PATH_MAX];
    int error = dos_resolve_path(vm, directory, false, resolved_directory,
                                 sizeof(resolved_directory));
    if (error) return DOS_FIND_ERR_PATH_NOT_FOUND;

    dos_dir_iter_t iterator;
    if (dos_dir_iter_open(resolved_directory, &iterator) < 0)
        return DOS_FIND_ERR_PATH_NOT_FOUND;

    dos_search_t *search = dos_search_allocate(vm, address);
    if (!search) return DOS_FIND_ERR_NO_MORE_FILES;
    search->use_osfs3 = iterator.use_osfs3;
    search->attributes = attributes & 0x3FU;
    if (!dos_find_copy(search->directory, sizeof(search->directory),
                       resolved_directory) ||
        !dos_find_copy(search->pattern, sizeof(search->pattern), pattern)) {
        search->used = false;
        return DOS_FIND_ERR_PATH_NOT_FOUND;
    }
    dos_dta_write_cursor(vm, address, search, &iterator, 0);
    return dos_find_next(vm);
}

int dos_find_next(dos_vm_t *vm)
{
    uint32_t address;
    if (!dos_dta_address(vm, &address)) return DOS_FIND_ERR_NO_MORE_FILES;

    dos_dir_iter_t iterator;
    uint8_t state;
    dos_search_t *search = dos_dta_search(vm, address, &iterator, &state);
    if (!search) return DOS_FIND_ERR_NO_MORE_FILES;

    if (search->attributes & DOS_ATTR_VOLUME) {
        if ((state & DOS_DTA_STATE_VOLUME_DONE) || search->directory[0]) {
            dos_search_finish(vm, address, search);
            return DOS_FIND_ERR_NO_MORE_FILES;
        }
        state |= DOS_DTA_STATE_VOLUME_DONE;
        dos_dta_write_cursor(vm, address, search, &iterator, state);
        const char *label = osfs2_label();
        if (!label || !*label) {
            dos_search_finish(vm, address, search);
            return DOS_FIND_ERR_NO_MORE_FILES;
        }
        char short_name[13];
        dos_make_short_name(label, osfs2_volume_id(), short_name);
        if (!dos_wildcard_match(search->pattern, short_name) &&
            !dos_wildcard_match(search->pattern, label)) {
            dos_search_finish(vm, address, search);
            return DOS_FIND_ERR_NO_MORE_FILES;
        }
        dos_fill_dta(vm, address, NULL, short_name, DOS_ATTR_VOLUME);
        return 0;
    }

    dos_dir_entry_t entry;
    while (dos_dir_iter_next(&iterator, &entry) > 0) {
        dos_dta_write_cursor(vm, address, search, &iterator, state);
        char short_name[13];
        dos_make_short_name(entry.name, entry.identity, short_name);
        if (!dos_wildcard_match(search->pattern, short_name) &&
            !dos_wildcard_match(search->pattern, entry.name))
            continue;
        uint8_t entry_attributes = dos_entry_attributes(&entry);
        if (!dos_attributes_match(entry_attributes, search->attributes))
            continue;
        dos_fill_dta(vm, address, &entry, short_name, entry_attributes);
        return 0;
    }

    dos_search_finish(vm, address, search);
    return DOS_FIND_ERR_NO_MORE_FILES;
}

int dos_resolve_path(dos_vm_t *vm, const char *path,
                     bool allow_missing_leaf, char *resolved,
                     uint32_t resolved_capacity)
{
    if (!vm || !path || !resolved || !resolved_capacity)
        return DOS_FIND_ERR_PATH_NOT_FOUND;
    resolved[0] = 0;
    if (!*path) return 0;
    if (!osfs2_is_mounted()) return DOS_FIND_ERR_PATH_NOT_FOUND;

    void *file = osfs2_find_exact_ci(path);
    if (file) {
        const char *stored = osfs2_file_name(file);
        return stored && dos_find_copy(resolved, resolved_capacity, stored)
            ? 0 : DOS_FIND_ERR_PATH_NOT_FOUND;
    }
    if (osfs2_directory_exists_ci(path))
        return dos_find_copy(resolved, resolved_capacity, path)
            ? 0 : DOS_FIND_ERR_PATH_NOT_FOUND;

    if (allow_missing_leaf) {
        const char *leaf = path;
        const char *last_separator = NULL;
        for (const char *p = path; *p; p++)
            if (dos_find_sep(*p)) last_separator = p;
        if (last_separator) leaf = last_separator + 1;

        bool may_be_alias = false;
        for (const char *p = leaf; *p; p++)
            if (*p == '~') may_be_alias = true;
        if (!may_be_alias) {
            char parent[DOS_SEARCH_PATH_MAX];
            uint32_t parent_length = last_separator
                ? (uint32_t)(last_separator - path) : 0;
            if (parent_length < sizeof(parent)) {
                memcpy(parent, path, parent_length);
                parent[parent_length] = 0;
                if (!parent[0] || osfs2_directory_exists_ci(parent))
                    return dos_find_copy(resolved, resolved_capacity, path)
                        ? 0 : DOS_FIND_ERR_PATH_NOT_FOUND;
            }
        }
    }

    const char *component = path;
    while (*component) {
        while (dos_find_sep(*component)) component++;
        if (!*component) break;
        const char *end = component;
        while (*end && !dos_find_sep(*end)) end++;
        bool last = !*end;
        uint32_t length = (uint32_t)(end - component);
        if (!length || length >= DOS_SEARCH_PATH_MAX)
            return DOS_FIND_ERR_PATH_NOT_FOUND;
        char requested[DOS_SEARCH_PATH_MAX];
        memcpy(requested, component, length);
        requested[length] = 0;

        dos_dir_iter_t iterator;
        if (dos_dir_iter_open(resolved, &iterator) < 0)
            return DOS_FIND_ERR_PATH_NOT_FOUND;
        dos_dir_entry_t entry;
        bool found = false;
        while (dos_dir_iter_next(&iterator, &entry) > 0) {
            char short_name[13];
            dos_make_short_name(entry.name, entry.identity, short_name);
            if (dos_find_name_equal(requested, entry.name) ||
                dos_find_name_equal(requested, short_name)) {
                found = true;
                break;
            }
        }

        char joined[DOS_SEARCH_PATH_MAX];
        if (!found) {
            if (!last || !allow_missing_leaf)
                return last ? DOS_FIND_ERR_FILE_NOT_FOUND
                            : DOS_FIND_ERR_PATH_NOT_FOUND;
            if (!dos_find_join(resolved, requested, joined, sizeof(joined)))
                return DOS_FIND_ERR_PATH_NOT_FOUND;
        } else {
            if (!last && !entry.is_directory)
                return DOS_FIND_ERR_PATH_NOT_FOUND;
            if (!dos_find_join(resolved, entry.name, joined, sizeof(joined)))
                return DOS_FIND_ERR_PATH_NOT_FOUND;
        }
        if (!dos_find_copy(resolved, resolved_capacity, joined))
            return DOS_FIND_ERR_PATH_NOT_FOUND;
        component = end;
    }
    return 0;
}

static int dos_find_resolved_node(const char *path, void **node,
                                  bool *is_directory)
{
    if (!path || !node || !is_directory) return -1;
    *node = osfs2_find_exact_ci(path);
    if (*node) {
        *is_directory = false;
        return 0;
    }

    if (osfs3_is_mounted()) {
        uint32_t inode = osfs3_resolve_path_ci(path);
        *node = inode ? osfs3_get_node((int)inode) : NULL;
        if (*node) {
            *is_directory = osfs3_is_dir(inode);
            return 0;
        }
    } else if (!*path || osfs2_directory_exists_ci(path)) {
        *is_directory = true;
        return 0;
    }
    return -1;
}

static const char *dos_find_basename(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (dos_find_sep(*p)) name = p + 1;
    return name;
}

int dos_file_get_attributes(dos_vm_t *vm, const char *path,
                            uint16_t *attributes)
{
    if (!attributes) return DOS_FIND_ERR_ACCESS_DENIED;
    char resolved[DOS_SEARCH_PATH_MAX];
    int error = dos_resolve_path(vm, path, false, resolved,
                                 sizeof(resolved));
    if (error) return error;

    void *node = NULL;
    bool directory = false;
    if (dos_find_resolved_node(resolved, &node, &directory) < 0)
        return DOS_FIND_ERR_FILE_NOT_FOUND;
    uint8_t value = 0;
    if (node && osfs2_file_get_dos_attributes(node, &value) < 0)
        return DOS_FIND_ERR_ACCESS_DENIED;
    if (directory) {
        value &= (uint8_t)~DOS_ATTR_ARCHIVE;
        value |= DOS_ATTR_DIRECTORY;
    }
    if (dos_find_basename(resolved)[0] == '.') value |= DOS_ATTR_HIDDEN;
    *attributes = value;
    return 0;
}

int dos_file_set_attributes(dos_vm_t *vm, const char *path,
                            uint16_t attributes)
{
    if (attributes & ~OSFS_DOS_ATTR_MASK)
        return DOS_FIND_ERR_ACCESS_DENIED;
    char resolved[DOS_SEARCH_PATH_MAX];
    int error = dos_resolve_path(vm, path, false, resolved,
                                 sizeof(resolved));
    if (error) return error;
    if (!resolved[0]) return DOS_FIND_ERR_ACCESS_DENIED;

    void *node = NULL;
    bool directory = false;
    if (dos_find_resolved_node(resolved, &node, &directory) < 0)
        return DOS_FIND_ERR_FILE_NOT_FOUND;
    if (directory) attributes &= (uint16_t)~DOS_ATTR_ARCHIVE;
    if (!node || osfs2_file_set_dos_attributes(
            node, (uint8_t)attributes) < 0)
        return DOS_FIND_ERR_ACCESS_DENIED;
    return 0;
}

int dos_find_selftest(void)
{
    int failures = 0;
    char short_name[13];
    dos_make_short_name("readme.txt", 1, short_name);
    if (!dos_find_name_equal(short_name, "README.TXT")) failures++;
    dos_make_short_name("Long File Name.data", 1, short_name);
    if (!dos_find_name_equal(short_name, "LON~0001.DAT")) failures++;
    if (!dos_wildcard_match("*.DAT", short_name) ||
        !dos_wildcard_match("*.*", "NOEXT") ||
        !dos_wildcard_match("NOEXT.*", "NOEXT") ||
        dos_wildcard_match("A?.TXT", "ABC.TXT"))
        failures++;
    char child[DOS_SEARCH_PATH_MAX];
    bool directory;
    if (!dos_find_path_child("GAMES", "games/UT99/System/core.dll",
                             child, &directory) ||
        !dos_find_name_equal(child, "UT99") || !directory ||
        dos_find_path_child("OTHER", "games/UT99/System/core.dll",
                            child, &directory))
        failures++;
    return failures;
}
