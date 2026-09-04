/*
 * OsitoK — DOS INT 21h API Services
 *
 * Implements the core DOS API functions accessed via INT 21h.
 * AH register selects the function. Maps file operations to OsitoFS.
 *
 * Phase 1: console I/O (01h-0Ch), version (30h), exit (4Ch)
 * Phase 2: file I/O (3Ch-42h), memory (48h-4Ah)
 */

#include "cpu8086.h"
#include "dos_hostmem.h"
#include "dos_audio.h"
#include "dos_find.h"
#include "dos_io.h"
#include "dos_loader.h"
#include "dos_mouse.h"
#include "dos_time.h"
#include "dos_vbe.h"
#include "../fs/ositofs3.h"
#include "../fs/ositofs_metadata.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void serial_putchar(char c);
extern int  disk_flush(void);

/* Console output — bridges to OsitoK's framebuffer */
extern void fb_putchar(char c);
extern int  kb_has_input(void);
extern char kb_getchar(void);

/* OsitoFS. The v2 entry points delegate to v3 when it is mounted. */
extern bool     osfs2_is_mounted(void);
extern void    *osfs2_find_ci(const char *name);
extern void    *osfs2_create(const char *name, uint64_t size);
extern int      osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int      osfs2_write(void *file, uint64_t offset, const void *buf,
                            uint64_t len);
extern int      osfs2_truncate(void *file, uint64_t size);
extern int      osfs2_delete(const char *name);
extern int      osfs2_rename(const char *from, const char *to, bool replace);
extern int      osfs2_file_retain(void *file);
extern void     osfs2_file_release(void *file);
extern uint64_t osfs2_file_size(void *file);
extern const char *osfs2_file_name(void *file);
extern bool     osfs2_directory_exists_ci(const char *directory);
extern uint32_t osfs2_free_blocks(void);
extern uint32_t osfs2_total_blocks(void);
extern uint32_t osfs2_get_block_size(void);

/* DOS memory manager */
extern uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs, uint16_t *largest);
extern int      dos_mem_free(dos_vm_t *vm, uint16_t segment);
extern int      dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size,
                               uint16_t *max_avail);
extern int      dos_mem_free_owner(dos_vm_t *vm, uint16_t owner);
extern void     dos_mem_init(dos_vm_t *vm);
extern int      dos_mem_selftest(void);

#ifndef DOS_DIAGNOSTICS
#define DOS_DIAGNOSTICS 0
#endif

enum {
    DOS_ERROR_CLASS_OUT_RESOURCE = 1,
    DOS_ERROR_CLASS_AUTHORIZATION = 3,
    DOS_ERROR_CLASS_APPLICATION = 7,
    DOS_ERROR_CLASS_NOT_FOUND = 8,
    DOS_ERROR_CLASS_BAD_FORMAT = 9,
    DOS_ERROR_CLASS_LOCKED = 10,
    DOS_ERROR_CLASS_ALREADY_EXISTS = 12,
    DOS_ERROR_CLASS_UNKNOWN = 13
};

enum {
    DOS_ERROR_ACTION_DELAY_RETRY = 2,
    DOS_ERROR_ACTION_USER = 3,
    DOS_ERROR_ACTION_ABORT = 4
};

enum {
    DOS_ERROR_LOCUS_UNKNOWN = 1,
    DOS_ERROR_LOCUS_DISK = 2,
    DOS_ERROR_LOCUS_MEMORY = 5
};

enum {
    DOS_ACCESS_READ = 0,
    DOS_ACCESS_WRITE = 1,
    DOS_ACCESS_READ_WRITE = 2,
    DOS_ACCESS_READ_NO_ATIME = 4
};

enum {
    DOS_SHARE_COMPATIBILITY = 0,
    DOS_SHARE_DENY_ALL = 1,
    DOS_SHARE_DENY_WRITE = 2,
    DOS_SHARE_DENY_READ = 3,
    DOS_SHARE_DENY_NONE = 4
};

enum {
    DOS_OPEN_ACCESS_MASK = 0x0007,
    DOS_OPEN_SHARE_MASK = 0x0070,
    DOS_OPEN_NO_INHERIT = 0x0080,
    DOS_OPEN_LARGE_FILE = 0x1000,
    DOS_OPEN_NO_CRITICAL = 0x2000,
    DOS_OPEN_SYNC = 0x4000,
    DOS_OPEN_VALID_MASK = DOS_OPEN_ACCESS_MASK | DOS_OPEN_SHARE_MASK |
                          DOS_OPEN_NO_INHERIT | DOS_OPEN_LARGE_FILE |
                          DOS_OPEN_NO_CRITICAL | DOS_OPEN_SYNC
};

enum {
    DOS_OPEN_EXISTING_FAIL = 0,
    DOS_OPEN_EXISTING_OPEN = 1,
    DOS_OPEN_EXISTING_REPLACE = 2,
    DOS_OPEN_RESULT_OPENED = 1,
    DOS_OPEN_RESULT_CREATED = 2,
    DOS_OPEN_RESULT_REPLACED = 3
};

enum {
    DOS_SYSVARS_SEG = 0x0050,
    DOS_SYSVARS_OFF = 0x0080,
    DOS_SYSVARS_SIZE = 0x006A,
    DOS_INDOS_OFF = 0x00F0
};

static bool dos_allocation_strategy_valid(uint8_t strategy)
{
    return strategy <= (DOS_ALLOC_UMB_FIRST | DOS_ALLOC_LAST_FIT) &&
           (strategy & DOS_ALLOC_FIT_MASK) <= DOS_ALLOC_LAST_FIT;
}

/* Materialize the stable DOS 4+ List-of-Lists prefix. Unsupported linked
 * subsystems terminate with FFFF:FFFF instead of exposing dangling pointers. */
static void dos_sync_system_variables(dos_vm_t *vm)
{
    if (!vm || !vm->mem) return;
    uint32_t base = dos_linear(DOS_SYSVARS_SEG, DOS_SYSVARS_OFF);
    if (base < 2u || base + DOS_SYSVARS_SIZE > vm->total_mem_size) return;

    for (uint32_t i = 0; i < DOS_SYSVARS_SIZE; i++)
        dos_mem_write8(vm, base + i, 0);

    dos_mem_write16(vm, base - 2u, vm->first_mcb);
    dos_mem_write32(vm, base + 0x00u, 0xFFFFFFFFu); /* DPB chain */
    dos_mem_write32(vm, base + 0x04u, 0xFFFFFFFFu); /* SFT chain */
    dos_mem_write32(vm, base + 0x08u, 0xFFFFFFFFu); /* CLOCK$ */
    dos_mem_write32(vm, base + 0x0Cu, 0xFFFFFFFFu); /* CON */
    dos_mem_write16(vm, base + 0x10u, 512u);
    dos_mem_write32(vm, base + 0x12u, 0xFFFFFFFFu); /* buffer info */
    dos_mem_write32(vm, base + 0x16u, 0xFFFFFFFFu); /* CDS array */
    dos_mem_write32(vm, base + 0x1Au, 0xFFFFFFFFu); /* FCB table */
    dos_mem_write8(vm, base + 0x20u, 0u);           /* block devices */
    dos_mem_write8(vm, base + 0x21u, 3u);           /* LASTDRIVE = C */

    /* Inline NUL character-device header. Strategy and interrupt offsets are
     * kernel-private because INT 21h handles devices without guest calls. */
    dos_mem_write32(vm, base + 0x22u, 0xFFFFFFFFu);
    dos_mem_write16(vm, base + 0x26u, 0x8004u);
    static const char nul_name[8] = {'N', 'U', 'L', ' ', ' ', ' ', ' ', ' '};
    for (unsigned i = 0; i < sizeof(nul_name); i++)
        dos_mem_write8(vm, base + 0x2Cu + i, (uint8_t)nul_name[i]);

    dos_mem_write32(vm, base + 0x37u, 0xFFFFFFFFu); /* SETVER */
    dos_mem_write32(vm, base + 0x3Bu, 0xFFFFFFFFu); /* A20 fixup */
    dos_mem_write16(vm, base + 0x3Du, vm->current_psp);
    dos_mem_write8(vm, base + 0x43u, 3u);           /* boot drive C */

    uint32_t xms_kb = vm->total_mem_size > 0x100000u
                    ? (vm->total_mem_size - 0x100000u) >> 10 : 0u;
    if (xms_kb > 0xFFFFu) xms_kb = 0xFFFFu;
    dos_mem_write16(vm, base + 0x45u, (uint16_t)xms_kb);
    dos_mem_write32(vm, base + 0x47u, 0xFFFFFFFFu); /* buffer chain */
    dos_mem_write32(vm, base + 0x4Du, 0xFFFFFFFFu); /* lookahead */
    dos_mem_write32(vm, base + 0x54u, 0xFFFFFFFFu); /* deblock buffer */
    dos_mem_write8(vm, base + 0x5Eu, vm->allocation_strategy);
    dos_mem_write8(vm, base + 0x63u, vm->uppermem_link);
    dos_mem_write16(vm, base + 0x66u, 0xFFFFu);     /* no UMB root */
    dos_mem_write16(vm, base + 0x68u, vm->first_mcb);
}

static void dos_publish_indos(dos_vm_t *vm)
{
    if (!vm || !vm->mem) return;
    uint32_t address = dos_linear(DOS_SYSVARS_SEG, DOS_INDOS_OFF);
    if (address < vm->total_mem_size)
        dos_mem_write8(vm, address, vm->indos_count);
}

static int dos_bind_device_handle(dos_vm_t *vm, int handle,
                                  uint8_t device_kind, uint16_t open_mode);
static int dos_clone_handle(dos_vm_t *vm, int source, int target,
                            bool replace);

static uint8_t dos_extended_error_locus(uint8_t function, uint16_t error)
{
    switch (error) {
    case DOS_ERR_FILE_NOT_FOUND:
    case DOS_ERR_PATH_NOT_FOUND:
    case DOS_ERR_INVALID_DRIVE:
    case DOS_ERR_NO_MORE_FILES:
    case DOS_ERR_SHARING_VIOLATION:
    case DOS_ERR_FILE_EXISTS:
    case DOS_ERR_CANNOT_MAKE:
        return DOS_ERROR_LOCUS_DISK;
    case DOS_ERR_NOT_ENOUGH_MEMORY:
    case DOS_ERR_INVALID_BLOCK:
    case DOS_ERR_BAD_ENVIRONMENT:
        return DOS_ERROR_LOCUS_MEMORY;
    case DOS_ERR_BAD_FORMAT:
        return DOS_ERROR_LOCUS_DISK;
    case DOS_ERR_INVALID_FUNCTION:
        return function == 0x58 ? DOS_ERROR_LOCUS_MEMORY
                                : DOS_ERROR_LOCUS_UNKNOWN;
    case DOS_ERR_ACCESS_DENIED:
        switch (function) {
        case 0x39:
        case 0x3A:
        case 0x3B:
        case 0x3C:
        case 0x3D:
        case 0x41:
        case 0x43:
        case 0x56:
        case 0x57:
        case 0x5A:
        case 0x5B:
        case 0x68:
        case 0x6A:
        case 0x6C:
            return DOS_ERROR_LOCUS_DISK;
        default:
            return DOS_ERROR_LOCUS_UNKNOWN;
        }
    default:
        return DOS_ERROR_LOCUS_UNKNOWN;
    }
}

static void dos_record_extended_error(dos_vm_t *vm, uint8_t function,
                                      uint16_t error)
{
    uint8_t error_class = DOS_ERROR_CLASS_UNKNOWN;
    uint8_t action = DOS_ERROR_ACTION_ABORT;

    switch (error) {
    case DOS_ERR_INVALID_FUNCTION:
    case DOS_ERR_INVALID_HANDLE:
    case DOS_ERR_INVALID_ACCESS:
        error_class = DOS_ERROR_CLASS_APPLICATION;
        break;
    case DOS_ERR_FILE_NOT_FOUND:
    case DOS_ERR_PATH_NOT_FOUND:
    case DOS_ERR_INVALID_DRIVE:
    case DOS_ERR_NO_MORE_FILES:
        error_class = DOS_ERROR_CLASS_NOT_FOUND;
        action = DOS_ERROR_ACTION_USER;
        break;
    case DOS_ERR_TOO_MANY_OPEN_FILES:
    case DOS_ERR_NOT_ENOUGH_MEMORY:
    case DOS_ERR_CANNOT_MAKE:
        error_class = DOS_ERROR_CLASS_OUT_RESOURCE;
        break;
    case DOS_ERR_ACCESS_DENIED:
        error_class = DOS_ERROR_CLASS_AUTHORIZATION;
        action = DOS_ERROR_ACTION_USER;
        break;
    case DOS_ERR_INVALID_BLOCK:
        error_class = DOS_ERROR_CLASS_APPLICATION;
        break;
    case DOS_ERR_INVALID_DATA:
    case DOS_ERR_BAD_ENVIRONMENT:
    case DOS_ERR_BAD_FORMAT:
        error_class = DOS_ERROR_CLASS_BAD_FORMAT;
        break;
    case DOS_ERR_SHARING_VIOLATION:
        error_class = DOS_ERROR_CLASS_LOCKED;
        action = DOS_ERROR_ACTION_DELAY_RETRY;
        break;
    case DOS_ERR_FILE_EXISTS:
        error_class = DOS_ERROR_CLASS_ALREADY_EXISTS;
        action = DOS_ERROR_ACTION_USER;
        break;
    default:
        break;
    }

    vm->extended_error = error;
    vm->extended_error_action = action;
    vm->extended_error_class = error_class;
    vm->extended_error_locus = dos_extended_error_locus(function, error);
    vm->extended_error_segment = 0;
    vm->extended_error_offset = 0;
}

static bool dos_int21_reports_carry_error(uint8_t function)
{
    switch (function) {
    case 0x39:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4E:
    case 0x4F:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x5A:
    case 0x5B:
    case 0x60:
    case 0x67:
    case 0x68:
    case 0x6A:
    case 0x6C:
        return true;
    default:
        return false;
    }
}

void dos_api_init(dos_vm_t *vm)
{
    if (!vm) return;
    vm->ctrl_break_enabled = false;
    vm->indos_count = 0;
    vm->extended_error = 0;
    vm->extended_error_action = 0;
    vm->extended_error_class = 0;
    vm->extended_error_locus = 0;
    vm->extended_error_segment = 0;
    vm->extended_error_offset = 0;
    vm->temp_file_serial = 0;
    vm->last_return_code = 0;
    vm->last_return_type = 0;
    vm->termination_type = 0;
    vm->process_terminated = false;
    vm->exec_depth = 0;
    vm->exec_context = NULL;
    vm->software_int_return_flags = 0;
    vm->software_int_frame_bytes = 0;
    vm->jft_external_segment = 0;
    vm->jft_external_psp = 0;
    vm->jft_active = false;
    dos_find_init(vm);
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
        vm->bootstrap_jft[i].sft_index = DOS_SFT_INVALID;
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++)
        vm->sft[i] = (dos_sft_entry_t){0};

    (void)dos_bind_device_handle(vm, 0, DOS_DEVICE_CON,
                                 DOS_ACCESS_READ);
    (void)dos_bind_device_handle(vm, 1, DOS_DEVICE_CON,
                                 DOS_ACCESS_WRITE);
    (void)dos_clone_handle(vm, 1, 2, false);
    (void)dos_bind_device_handle(vm, 3, DOS_DEVICE_AUX,
                                 DOS_ACCESS_READ_WRITE);
    (void)dos_bind_device_handle(vm, 4, DOS_DEVICE_PRN,
                                 DOS_ACCESS_WRITE);
    dos_publish_indos(vm);
    dos_sync_system_variables(vm);
}

/* ── Helper: read ASCIIZ string from DOS memory ────────────────── */

static void dos_read_asciiz(dos_vm_t *vm, uint16_t seg, uint16_t off,
                            char *buf, int maxlen)
{
    for (int i = 0; i < maxlen - 1; i++) {
        uint8_t ch = dos_mem_read8(vm, dos_addr(vm, seg,
                                                (uint16_t)(off + i)));
        if (ch == 0) { buf[i] = 0; return; }
        buf[i] = ch;
    }
    buf[maxlen - 1] = 0;
}

/* ── Helper: convert DOS path to OsitoFS path ──────────────────── */

static int dos_path_to_osfs(const char *path, uint8_t current_drive,
                            const char *current_dir, char *buf, int buflen)
{
    if (!path || !buf || buflen <= 0) return DOS_ERR_PATH_NOT_FOUND;

    int j = 0;
    bool absolute = false;

    if (path[0] && path[1] == ':') {
        char drive_letter = path[0];
        if (drive_letter >= 'a' && drive_letter <= 'z')
            drive_letter -= 'a' - 'A';
        if (drive_letter < 'A' || drive_letter > 'Z' ||
            (uint8_t)(drive_letter - 'A') != current_drive)
            return DOS_ERR_INVALID_DRIVE;
        path += 2;
    }

    if (*path == '\\' || *path == '/') {
        absolute = true;
        while (*path == '\\' || *path == '/') path++;
    }

    if (!absolute && current_dir) {
        for (int i = 0; current_dir[i]; i++) {
            if (j >= buflen - 1) return DOS_ERR_PATH_NOT_FOUND;
            buf[j++] = current_dir[i] == '\\' ? '/' : current_dir[i];
        }
    }

    while (*path) {
        while (*path == '\\' || *path == '/') path++;
        if (!*path) break;

        const char *component = path;
        int length = 0;
        while (path[length] && path[length] != '\\' && path[length] != '/')
            length++;

        if (length == 1 && component[0] == '.') {
            path += length;
            continue;
        }
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            while (j > 0 && buf[j - 1] != '/') j--;
            if (j > 0) j--;
            path += length;
            continue;
        }

        if (j && buf[j - 1] != '/') {
            if (j >= buflen - 1) return DOS_ERR_PATH_NOT_FOUND;
            buf[j++] = '/';
        }
        for (int i = 0; i < length; i++) {
            if (j >= buflen - 1) return DOS_ERR_PATH_NOT_FOUND;
            buf[j++] = component[i];
        }
        path += length;
    }

    buf[j] = 0;
    return 0;
}

static bool dos_string_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint8_t dos_ascii_upper(uint8_t ch)
{
    return ch >= 'a' && ch <= 'z' ? (uint8_t)(ch - ('a' - 'A')) : ch;
}

static bool dos_path_equal_ci(const char *left, const char *right)
{
    while (*left && *right) {
        bool left_separator = *left == '/' || *left == '\\';
        bool right_separator = *right == '/' || *right == '\\';
        if (left_separator || right_separator) {
            if (left_separator != right_separator) return false;
        } else if (dos_ascii_upper((uint8_t)*left) !=
                   dos_ascii_upper((uint8_t)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return !*left && !*right;
}

static uint8_t dos_device_from_path(const char *path)
{
    if (!path) return DOS_DEVICE_NONE;
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }

    unsigned length = 0;
    while (name[length] && name[length] != '.' && name[length] != ':')
        length++;
    if (name[length] == ':' && name[length + 1] != 0)
        return DOS_DEVICE_NONE;
    if (length == 3u && dos_ascii_upper((uint8_t)name[0]) == 'N' &&
        dos_ascii_upper((uint8_t)name[1]) == 'U' &&
        dos_ascii_upper((uint8_t)name[2]) == 'L')
        return DOS_DEVICE_NUL;
    return DOS_DEVICE_NONE;
}

enum {
    DOS_TRUENAME_CAPACITY = 128,
    DOS_TRUENAME_COMPONENT_CAPACITY = 13
};

static bool dos_is_path_separator(uint8_t ch)
{
    return ch == '\\' || ch == '/';
}

static int dos_truename_tail_error(const char *tail)
{
    while (tail && *tail) {
        if (dos_is_path_separator((uint8_t)*tail))
            return DOS_ERR_PATH_NOT_FOUND;
        tail++;
    }
    return DOS_ERR_FILE_NOT_FOUND;
}

static bool dos_truename_valid_character(uint8_t ch)
{
    if (ch < 0x20u) return false;
    switch (ch) {
    case '"':
    case '[':
    case ']':
    case ':':
    case '|':
    case '<':
    case '>':
    case '+':
    case '=':
    case ';':
    case ',':
        return false;
    default:
        return true;
    }
}

static bool dos_truename_pack_component(const char *source,
                                        uint32_t source_length,
                                        char *packed,
                                        uint32_t *packed_length,
                                        bool *has_wildcard)
{
    if (!source || !source_length || !packed || !packed_length ||
        !has_wildcard)
        return false;

    uint32_t length = 0;
    uint32_t remaining = 8;
    bool extension = false;
    bool wildcard = false;

    for (uint32_t i = 0; i < source_length; i++) {
        uint8_t ch = (uint8_t)source[i];
        if (ch == '*') {
            wildcard = true;
            while (remaining) {
                packed[length++] = '?';
                remaining--;
            }
            continue;
        }
        if (ch == '.') {
            if (extension) return false;
            if (i + 1u == source_length) break;
            if (!length) return false;
            extension = true;
            remaining = 3;
            packed[length++] = '.';
            continue;
        }
        if (!dos_truename_valid_character(ch)) return false;
        if (ch == '?') wildcard = true;
        if (remaining) {
            packed[length++] = (char)dos_ascii_upper(ch);
            remaining--;
        }
    }

    if (!length || packed[length - 1u] == '.') return false;
    packed[length] = 0;
    *packed_length = length;
    *has_wildcard = wildcard;
    return true;
}

static int dos_truename_append_path(const char *source, char *destination,
                                    uint32_t capacity, uint32_t root_length,
                                    uint32_t *destination_length,
                                    bool *wildcard_seen)
{
    if (!source || !destination || !destination_length || !wildcard_seen)
        return DOS_ERR_PATH_NOT_FOUND;

    uint32_t length = *destination_length;
    while (*source) {
        if (*wildcard_seen) return DOS_ERR_PATH_NOT_FOUND;

        bool had_separator = false;
        while (dos_is_path_separator((uint8_t)*source)) {
            had_separator = true;
            source++;
        }
        if (!*source) {
            if (had_separator && length > root_length &&
                destination[length - 1u] != '\\') {
                if (length + 1u >= capacity)
                    return DOS_ERR_PATH_NOT_FOUND;
                destination[length++] = '\\';
                destination[length] = 0;
            }
            break;
        }

        const char *component = source;
        uint32_t component_length = 0;
        while (source[component_length] &&
               !dos_is_path_separator((uint8_t)source[component_length]))
            component_length++;
        source += component_length;

        if (component_length == 1u && component[0] == '.')
            continue;
        if (component_length == 2u && component[0] == '.' &&
            component[1] == '.') {
            if (length <= root_length)
                return DOS_ERR_PATH_NOT_FOUND;
            while (length > root_length &&
                   destination[length - 1u] != '\\')
                length--;
            if (length > root_length) length--;
            destination[length] = 0;
            continue;
        }

        char packed[DOS_TRUENAME_COMPONENT_CAPACITY];
        uint32_t packed_length = 0;
        bool component_wildcard = false;
        if (!dos_truename_pack_component(component, component_length,
                                         packed, &packed_length,
                                         &component_wildcard))
            return dos_truename_tail_error(component);

        bool needs_separator = destination[length - 1u] != '\\';
        uint32_t required = packed_length + (needs_separator ? 1u : 0u);
        if (required >= capacity || length > capacity - required - 1u)
            return dos_truename_tail_error(component);
        if (needs_separator) destination[length++] = '\\';
        for (uint32_t i = 0; i < packed_length; i++)
            destination[length++] = packed[i];
        destination[length] = 0;
        *wildcard_seen = component_wildcard;
    }

    *destination_length = length;
    return 0;
}

static int dos_truename_unc(const char *source, char *destination,
                            uint32_t capacity, uint32_t *output_size)
{
    uint32_t length = 2;
    destination[0] = '\\';
    destination[1] = '\\';
    destination[2] = 0;
    source += 2;
    if (!*source || dos_is_path_separator((uint8_t)*source))
        return DOS_ERR_PATH_NOT_FOUND;

    while (*source && !dos_is_path_separator((uint8_t)*source)) {
        uint8_t ch = (uint8_t)*source++;
        if (!dos_truename_valid_character(ch) || ch == '*' || ch == '?')
            return DOS_ERR_PATH_NOT_FOUND;
        if (length + 1u >= capacity) return DOS_ERR_PATH_NOT_FOUND;
        destination[length++] = (char)dos_ascii_upper(ch);
    }
    destination[length] = 0;

    uint32_t root_length = length;
    bool wildcard_seen = false;
    int error = dos_truename_append_path(source, destination, capacity,
                                         root_length, &length,
                                         &wildcard_seen);
    if (error) return error;
    *output_size = length + 1u;
    return 0;
}

static int dos_truename_canonicalize(const char *source,
                                     uint8_t current_drive,
                                     const char *current_dir,
                                     char *destination, uint32_t capacity,
                                     uint32_t *output_size)
{
    if (!source || !destination || capacity < 4u || !output_size)
        return DOS_ERR_PATH_NOT_FOUND;
    if (!source[0]) return DOS_ERR_FILE_NOT_FOUND;

    const char *path = source;
    uint8_t drive = current_drive;
    if (path[1] == ':') {
        uint8_t drive_letter = dos_ascii_upper((uint8_t)path[0]);
        if (drive_letter < 'A' || drive_letter > 'Z')
            return DOS_ERR_PATH_NOT_FOUND;
        drive = (uint8_t)(drive_letter - 'A');
        path += 2;
    }
    if (current_drive > 25u || drive != current_drive)
        return DOS_ERR_PATH_NOT_FOUND;

    if (dos_is_path_separator((uint8_t)path[0]) &&
        dos_is_path_separator((uint8_t)path[1]))
        return dos_truename_unc(path, destination, capacity, output_size);

    bool has_separator = false;
    uint32_t path_length = 0;
    while (path[path_length]) {
        if (dos_is_path_separator((uint8_t)path[path_length]))
            has_separator = true;
        path_length++;
    }
    if (!has_separator && dos_device_from_path(path) != DOS_DEVICE_NONE) {
        if (path_length && path[path_length - 1u] == ':') path_length--;
        char packed[DOS_TRUENAME_COMPONENT_CAPACITY];
        uint32_t packed_length = 0;
        bool wildcard = false;
        if (!dos_truename_pack_component(path, path_length, packed,
                                         &packed_length, &wildcard))
            return DOS_ERR_FILE_NOT_FOUND;
        if (packed_length + 4u > capacity) return DOS_ERR_FILE_NOT_FOUND;
        destination[0] = (char)('A' + drive);
        destination[1] = ':';
        destination[2] = '/';
        for (uint32_t i = 0; i < packed_length; i++)
            destination[3u + i] = packed[i];
        destination[3u + packed_length] = 0;
        *output_size = 4u + packed_length;
        return 0;
    }

    destination[0] = (char)('A' + drive);
    destination[1] = ':';
    destination[2] = '\\';
    destination[3] = 0;
    uint32_t length = 3;
    const uint32_t root_length = 3;

    bool absolute = dos_is_path_separator((uint8_t)*path);
    while (absolute && dos_is_path_separator((uint8_t)*path)) path++;

    bool wildcard_seen = false;
    if (!absolute && current_dir && current_dir[0]) {
        const char *directory = current_dir;
        while (dos_is_path_separator((uint8_t)*directory)) directory++;
        int error = dos_truename_append_path(directory, destination, capacity,
                                             root_length, &length,
                                             &wildcard_seen);
        if (error || wildcard_seen) return DOS_ERR_PATH_NOT_FOUND;
    }

    int error = dos_truename_append_path(path, destination, capacity,
                                         root_length, &length,
                                         &wildcard_seen);
    if (error) return error;
    *output_size = length + 1u;
    return 0;
}

static int dos_truename_read_source(dos_vm_t *vm, uint16_t segment,
                                    uint16_t offset, char *destination,
                                    uint32_t capacity)
{
    bool path_separator_seen = false;
    for (uint32_t i = 0; i < capacity; i++) {
        uint32_t address = dos_addr(vm, segment, (uint16_t)(offset + i));
        if (address >= vm->total_mem_size)
            return DOS_ERR_PATH_NOT_FOUND;
        uint8_t ch = dos_mem_read8(vm, address);
        destination[i] = (char)ch;
        if (!ch) return 0;
        if (dos_is_path_separator(ch)) path_separator_seen = true;
    }
    return path_separator_seen ? DOS_ERR_PATH_NOT_FOUND
                               : DOS_ERR_FILE_NOT_FOUND;
}

static int dos_truename_write_result(dos_vm_t *vm, uint16_t segment,
                                     uint16_t offset, const char *source,
                                     uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t address = dos_addr(vm, segment, (uint16_t)(offset + i));
        if (address >= vm->total_mem_size)
            return DOS_ERR_PATH_NOT_FOUND;
    }
    for (uint32_t i = 0; i < size; i++) {
        uint32_t address = dos_addr(vm, segment, (uint16_t)(offset + i));
        dos_mem_write8(vm, address, (uint8_t)source[i]);
    }
    return 0;
}

static int dos_guest_buffer(dos_vm_t *vm, uint16_t segment, uint16_t offset,
                            uint32_t length, uint32_t *address)
{
    if (!vm || !vm->mem || !address) return -1;
    uint32_t linear = dos_addr(vm, segment, offset);
    if (linear > vm->total_mem_size ||
        length > vm->total_mem_size - linear)
        return -1;
    *address = linear;
    return 0;
}

static bool dos_psp_location(dos_vm_t *vm, uint16_t segment,
                             dos_psp_t **psp_out, uint32_t *address_out)
{
    if (!vm || !vm->mem || !segment) return false;
    uint32_t address = (uint32_t)segment << 4;
    if (address > vm->total_mem_size ||
        sizeof(dos_psp_t) > vm->total_mem_size - address)
        return false;
    if (psp_out) *psp_out = (dos_psp_t *)(vm->mem + address);
    if (address_out) *address_out = address;
    return true;
}

static bool dos_psp_jft(dos_vm_t *vm, uint16_t psp_segment,
                        dos_psp_t **psp_out, uint32_t *address_out,
                        uint16_t *count_out, uint16_t *segment_out,
                        uint16_t *offset_out)
{
    dos_psp_t *psp;
    if (!dos_psp_location(vm, psp_segment, &psp, NULL)) return false;

    uint16_t count = psp->jft_size;
    uint16_t offset = (uint16_t)psp->jft_ptr;
    uint16_t segment = (uint16_t)(psp->jft_ptr >> 16);
    if (count < DOS_PSP_JFT_ENTRIES || count > DOS_MAX_JFT_ENTRIES ||
        (uint32_t)offset + count > 0x10000u)
        return false;

    uint32_t address = dos_linear(segment, offset);
    if (address > vm->total_mem_size ||
        count > vm->total_mem_size - address)
        return false;

    if (psp_out) *psp_out = psp;
    if (address_out) *address_out = address;
    if (count_out) *count_out = count;
    if (segment_out) *segment_out = segment;
    if (offset_out) *offset_out = offset;
    return true;
}

static bool dos_current_jft(dos_vm_t *vm, dos_psp_t **psp_out,
                            uint32_t *address_out, uint16_t *count_out,
                            uint16_t *segment_out, uint16_t *offset_out);

static int dos_find_free_handle(dos_vm_t *vm)
{
    if (!vm) return -1;
    if (!vm->jft_active) {
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
            if (vm->bootstrap_jft[i].sft_index == DOS_SFT_INVALID)
                return (int)i;
        return -1;
    }

    uint32_t address;
    uint16_t count;
    if (!dos_current_jft(vm, NULL, &address, &count, NULL, NULL)) return -1;
    for (uint32_t i = 0; i < count; i++)
        if (dos_mem_read8(vm, address + i) == DOS_SFT_INVALID)
            return (int)i;
    return -1;
}

static int dos_find_free_sft(const dos_vm_t *vm)
{
    if (!vm) return -1;
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++)
        if (!vm->sft[i].used) return (int)i;
    return -1;
}

static bool dos_current_jft(dos_vm_t *vm, dos_psp_t **psp_out,
                            uint32_t *address_out, uint16_t *count_out,
                            uint16_t *segment_out, uint16_t *offset_out)
{
    if (!vm || !vm->jft_active) return false;
    return dos_psp_jft(vm, vm->current_psp, psp_out, address_out,
                       count_out, segment_out, offset_out);
}

static bool dos_jft_storage_owned(dos_vm_t *vm, uint16_t psp_segment,
                                  uint16_t table_segment,
                                  uint16_t table_offset)
{
    if (!vm || !vm->mem || !psp_segment || !table_segment || table_offset ||
        table_segment <= vm->first_mcb)
        return false;
    uint32_t address = (uint32_t)(table_segment - 1u) << 4;
    if (address > vm->total_mem_size ||
        sizeof(dos_mcb_t) > vm->total_mem_size - address)
        return false;
    const dos_mcb_t *mcb = (const dos_mcb_t *)(vm->mem + address);
    return (mcb->type == 'M' || mcb->type == 'Z') &&
           mcb->owner == psp_segment;
}

static void dos_refresh_external_jft(dos_vm_t *vm)
{
    uint16_t segment = 0, offset = 0;
    vm->jft_external_segment = 0;
    vm->jft_external_psp = 0;
    if (dos_current_jft(vm, NULL, NULL, NULL, &segment, &offset) &&
        dos_jft_storage_owned(vm, vm->current_psp, segment, offset)) {
        vm->jft_external_segment = segment;
        vm->jft_external_psp = vm->current_psp;
    }
}

static bool dos_jft_read(dos_vm_t *vm, uint16_t handle, uint8_t *index)
{
    if (!vm || !index) return false;
    if (!vm->jft_active) {
        if (handle >= DOS_PSP_JFT_ENTRIES) return false;
        *index = vm->bootstrap_jft[handle].sft_index;
        return true;
    }

    uint32_t address;
    uint16_t count;
    if (!dos_current_jft(vm, NULL, &address, &count, NULL, NULL) ||
        handle >= count)
        return false;
    *index = dos_mem_read8(vm, address + handle);
    return true;
}

static bool dos_jft_write(dos_vm_t *vm, uint16_t handle, uint8_t index)
{
    if (!vm) return false;
    if (!vm->jft_active) {
        if (handle >= DOS_PSP_JFT_ENTRIES) return false;
        vm->bootstrap_jft[handle].sft_index = index;
        return true;
    }

    uint32_t address;
    uint16_t count;
    if (!dos_current_jft(vm, NULL, &address, &count, NULL, NULL) ||
        handle >= count)
        return false;
    dos_mem_write8(vm, address + handle, index);
    return true;
}

static dos_sft_entry_t *dos_sft_from_index(dos_vm_t *vm, uint8_t index)
{
    if (!vm || index == DOS_SFT_INVALID || index >= DOS_MAX_SFT_ENTRIES ||
        !vm->sft[index].used || vm->sft[index].ref_count == 0)
        return NULL;
    return &vm->sft[index];
}

static bool dos_create_psp_copy(dos_vm_t *vm, uint16_t source_segment,
                                uint16_t target_segment, uint16_t mem_top,
                                bool create_child)
{
    dos_psp_t source_image;
    uint32_t source_address, target_address, source_jft_address;
    if (!dos_psp_location(vm, source_segment, NULL, &source_address) ||
        !dos_psp_location(vm, target_segment, NULL, &target_address) ||
        !dos_psp_jft(vm, source_segment, NULL, &source_jft_address,
                     NULL, NULL, NULL))
        return false;

    uint8_t *source_bytes = (uint8_t *)&source_image;
    for (uint32_t i = 0; i < sizeof(source_image); i++)
        source_bytes[i] = dos_mem_read8(vm, source_address + i);

    uint8_t source_jft[DOS_PSP_JFT_ENTRIES];
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
        source_jft[i] = dos_mem_read8(vm, source_jft_address + i);

    for (uint32_t i = 0; i < sizeof(source_image); i++)
        dos_mem_write8(vm, target_address + i, source_bytes[i]);
    dos_psp_t *target = (dos_psp_t *)(vm->mem + target_address);
    dos_psp_initialize_system_fields(
        vm, target, target_segment,
        create_child ? mem_top : source_image.mem_top);

    if (!create_child) {
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
            target->jft[i] = source_jft[i];
        return true;
    }

    target->parent_psp = source_segment;
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
        uint8_t index = source_jft[i];
        target->jft[i] = DOS_SFT_INVALID;
        dos_sft_entry_t *entry = dos_sft_from_index(vm, index);
        if (!entry || (entry->open_mode & DOS_OPEN_NO_INHERIT) ||
            entry->ref_count == 0xFFFFu)
            continue;
        target->jft[i] = index;
        entry->ref_count++;
    }
    return true;
}

static dos_sft_entry_t *dos_handle_sft(dos_vm_t *vm, int handle)
{
    uint8_t index;
    if (handle < 0 || handle >= (int)DOS_MAX_JFT_ENTRIES ||
        !dos_jft_read(vm, (uint16_t)handle, &index))
        return NULL;
    return dos_sft_from_index(vm, index);
}

static uint32_t dos_file_size32(void *file)
{
    uint64_t size = osfs2_file_size(file);
    return size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
}

static bool dos_file_is_read_only(void *file)
{
    uint8_t attributes = 0;
    return file &&
        osfs2_file_get_dos_attributes(file, &attributes) == 0 &&
        (attributes & OSFS_DOS_ATTR_READ_ONLY) != 0;
}

static uint8_t dos_open_access(uint16_t open_mode)
{
    uint8_t access = (uint8_t)(open_mode & DOS_OPEN_ACCESS_MASK);
    return access == DOS_ACCESS_READ_NO_ATIME ? DOS_ACCESS_READ : access;
}

static uint8_t dos_open_share(uint16_t open_mode)
{
    return (uint8_t)((open_mode & DOS_OPEN_SHARE_MASK) >> 4);
}

static bool dos_open_mode_valid(uint16_t open_mode)
{
    uint8_t access = (uint8_t)(open_mode & DOS_OPEN_ACCESS_MASK);
    return (open_mode & (uint16_t)~DOS_OPEN_VALID_MASK) == 0 &&
           (access <= DOS_ACCESS_READ_WRITE ||
            access == DOS_ACCESS_READ_NO_ATIME) &&
           dos_open_share(open_mode) <= DOS_SHARE_DENY_NONE;
}

static bool dos_access_reads(uint8_t access)
{
    return access == DOS_ACCESS_READ || access == DOS_ACCESS_READ_WRITE;
}

static bool dos_access_writes(uint8_t access)
{
    return access == DOS_ACCESS_WRITE || access == DOS_ACCESS_READ_WRITE;
}

static bool dos_share_denies(uint8_t share, uint8_t holder_access,
                             uint8_t requested_access)
{
    switch (share) {
    case DOS_SHARE_COMPATIBILITY:
        return holder_access == DOS_ACCESS_READ
            ? dos_access_writes(requested_access) : true;
    case DOS_SHARE_DENY_ALL:
        return true;
    case DOS_SHARE_DENY_WRITE:
        return dos_access_writes(requested_access);
    case DOS_SHARE_DENY_READ:
        return dos_access_reads(requested_access);
    case DOS_SHARE_DENY_NONE:
    default:
        return false;
    }
}

static bool dos_open_sharing_conflict(const dos_vm_t *vm, const void *file,
                                      uint16_t open_mode)
{
    uint8_t access = dos_open_access(open_mode);
    uint8_t share = dos_open_share(open_mode);

    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        const dos_sft_entry_t *entry = &vm->sft[i];
        if (!entry->used || !entry->ref_count || entry->is_device ||
            entry->osfs_file != file)
            continue;

        uint8_t existing_access = dos_open_access(entry->open_mode);
        uint8_t existing_share = dos_open_share(entry->open_mode);
        if (share == DOS_SHARE_COMPATIBILITY &&
            existing_share == DOS_SHARE_COMPATIBILITY &&
            entry->owner_psp == vm->current_psp)
            continue;
        if (dos_share_denies(existing_share, existing_access, access) ||
            dos_share_denies(share, access, existing_access))
            return true;
    }
    return false;
}

static int dos_bind_file_handle(dos_vm_t *vm, int handle, void *file,
                                uint16_t open_mode)
{
    uint8_t old_index;
    if (!vm || handle < 0 || handle >= (int)DOS_MAX_JFT_ENTRIES || !file ||
        !dos_jft_read(vm, (uint16_t)handle, &old_index) ||
        old_index != DOS_SFT_INVALID)
        return DOS_ERR_ACCESS_DENIED;
    if (!dos_open_mode_valid(open_mode)) return DOS_ERR_INVALID_ACCESS;
    int index = dos_find_free_sft(vm);
    if (index < 0) return DOS_ERR_TOO_MANY_OPEN_FILES;
    if (dos_open_sharing_conflict(vm, file, open_mode))
        return DOS_ERR_SHARING_VIOLATION;
    if (osfs2_file_retain(file) < 0) return DOS_ERR_ACCESS_DENIED;

    dos_sft_entry_t *entry = &vm->sft[index];
    entry->used = true;
    entry->ref_count = 1;
    entry->osfs_file = file;
    entry->position = 0;
    entry->file_size = dos_file_size32(file);
    entry->open_mode = open_mode;
    entry->owner_psp = vm->current_psp;
    entry->is_device = false;
    entry->device_kind = DOS_DEVICE_NONE;
    if (!dos_jft_write(vm, (uint16_t)handle, (uint8_t)index)) {
        osfs2_file_release(file);
        *entry = (dos_sft_entry_t){0};
        return DOS_ERR_ACCESS_DENIED;
    }
    return 0;
}

static int dos_bind_device_handle(dos_vm_t *vm, int handle,
                                  uint8_t device_kind, uint16_t open_mode)
{
    uint8_t old_index;
    if (!vm || handle < 0 || handle >= (int)DOS_MAX_JFT_ENTRIES ||
        device_kind == DOS_DEVICE_NONE ||
        !dos_jft_read(vm, (uint16_t)handle, &old_index) ||
        old_index != DOS_SFT_INVALID)
        return DOS_ERR_ACCESS_DENIED;
    if (!dos_open_mode_valid(open_mode)) return DOS_ERR_INVALID_ACCESS;
    int index = dos_find_free_sft(vm);
    if (index < 0) return DOS_ERR_TOO_MANY_OPEN_FILES;

    dos_sft_entry_t *entry = &vm->sft[index];
    entry->used = true;
    entry->ref_count = 1;
    entry->osfs_file = NULL;
    entry->position = 0;
    entry->file_size = 0;
    entry->open_mode = open_mode;
    entry->owner_psp = vm->current_psp;
    entry->is_device = true;
    entry->device_kind = device_kind;
    if (!dos_jft_write(vm, (uint16_t)handle, (uint8_t)index)) {
        *entry = (dos_sft_entry_t){0};
        return DOS_ERR_ACCESS_DENIED;
    }
    return 0;
}

static uint16_t dos_handle_device_info(const dos_sft_entry_t *entry)
{
    if (!entry || !entry->is_device) return 0;
    switch (entry->device_kind) {
    case DOS_DEVICE_CON:
        return 0x80D3u;
    case DOS_DEVICE_NUL:
        return 0x80C4u;
    case DOS_DEVICE_AUX:
    case DOS_DEVICE_PRN:
    default:
        return 0x80C0u;
    }
}

static void dos_release_file_handle(dos_vm_t *vm, int handle)
{
    uint8_t index;
    if (!vm || handle < 0 || handle >= (int)DOS_MAX_JFT_ENTRIES ||
        !dos_jft_read(vm, (uint16_t)handle, &index) ||
        !dos_jft_write(vm, (uint16_t)handle, DOS_SFT_INVALID))
        return;
    dos_sft_entry_t *entry = dos_sft_from_index(vm, index);
    if (!entry) return;

    entry->ref_count--;
    if (entry->ref_count != 0) return;
    if (!entry->is_device && entry->osfs_file)
        osfs2_file_release(entry->osfs_file);
    *entry = (dos_sft_entry_t){0};
}

static int dos_clone_handle(dos_vm_t *vm, int source, int target,
                            bool replace)
{
    uint8_t source_index, target_index;
    dos_sft_entry_t *entry = dos_handle_sft(vm, source);
    if (!entry || target < 0 || target >= (int)DOS_MAX_JFT_ENTRIES ||
        !dos_jft_read(vm, (uint16_t)source, &source_index) ||
        !dos_jft_read(vm, (uint16_t)target, &target_index))
        return -1;
    if (source == target) return 0;
    if (target_index != DOS_SFT_INVALID) {
        if (!replace) return -1;
        dos_release_file_handle(vm, target);
    }
    if (!entry->used || entry->ref_count == 0xFFFFu) return -1;

    if (!dos_jft_write(vm, (uint16_t)target, source_index)) return -1;
    entry->ref_count++;
    return 0;
}

static int dos_resize_jft(dos_vm_t *vm, uint16_t requested_count)
{
    if (requested_count == 0xFFFFu) return DOS_ERR_INVALID_FUNCTION;

    uint16_t new_count = requested_count < DOS_PSP_JFT_ENTRIES
                       ? DOS_PSP_JFT_ENTRIES : requested_count;
    dos_psp_t *psp;
    uint32_t old_address;
    uint16_t old_count, old_segment, old_offset;
    if (!dos_current_jft(vm, &psp, &old_address, &old_count,
                         &old_segment, &old_offset))
        return DOS_ERR_INVALID_FUNCTION;
    if (new_count == old_count) return 0;

    if (new_count < old_count) {
        for (uint32_t i = new_count; i < old_count; i++)
            if (dos_mem_read8(vm, old_address + i) != DOS_SFT_INVALID)
                return DOS_ERR_TOO_MANY_OPEN_FILES;
    }

    bool old_owned = dos_jft_storage_owned(vm, vm->current_psp,
                                           old_segment, old_offset);

    if (new_count == DOS_PSP_JFT_ENTRIES) {
        uint8_t inline_jft[DOS_PSP_JFT_ENTRIES];
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
            inline_jft[i] = dos_mem_read8(vm, old_address + i);
        if (old_owned && dos_mem_free(vm, old_segment) < 0)
            return DOS_ERR_NOT_ENOUGH_MEMORY;
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
            psp->jft[i] = inline_jft[i];
        psp->jft_size = DOS_PSP_JFT_ENTRIES;
        psp->jft_ptr = ((uint32_t)vm->current_psp << 16) |
                       __builtin_offsetof(dos_psp_t, jft);
        vm->jft_external_segment = 0;
        vm->jft_external_psp = 0;
        return 0;
    }

    uint16_t paragraphs = (uint16_t)(((uint32_t)new_count + 15u) >> 4);
    uint16_t new_segment = dos_mem_alloc(vm, paragraphs, NULL);
    if (!new_segment) return DOS_ERR_NOT_ENOUGH_MEMORY;
    uint32_t new_address = (uint32_t)new_segment << 4;
    for (uint32_t i = 0; i < ((uint32_t)paragraphs << 4); i++)
        dos_mem_write8(vm, new_address + i, DOS_SFT_INVALID);
    uint16_t copy_count = old_count < new_count ? old_count : new_count;
    for (uint32_t i = 0; i < copy_count; i++)
        dos_mem_write8(vm, new_address + i,
                       dos_mem_read8(vm, old_address + i));

    if (old_owned && dos_mem_free(vm, old_segment) < 0) {
        (void)dos_mem_free(vm, new_segment);
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    }

    psp->jft_size = new_count;
    psp->jft_ptr = (uint32_t)new_segment << 16;
    vm->jft_external_segment = new_segment;
    vm->jft_external_psp = vm->current_psp;
    return 0;
}

void dos_api_close_all(dos_vm_t *vm)
{
    if (!vm) return;
    dos_exec_cleanup(vm);
    uint16_t count = DOS_PSP_JFT_ENTRIES;
    uint16_t table_segment = 0, table_offset = 0;
    if (vm->jft_active &&
        !dos_current_jft(vm, NULL, NULL, &count,
                         &table_segment, &table_offset))
        count = 0;
    for (uint32_t i = 0; i < count; i++)
        dos_release_file_handle(vm, (int)i);

    /* A malformed or caller-rewritten JFT must not retain kernel file
     * references after the VM exits. Each SFT owns one OsitoFS retain. */
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        dos_sft_entry_t *entry = &vm->sft[i];
        if (entry->used && !entry->is_device && entry->osfs_file)
            osfs2_file_release(entry->osfs_file);
        *entry = (dos_sft_entry_t){0};
    }
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
        vm->bootstrap_jft[i].sft_index = DOS_SFT_INVALID;

    if (dos_jft_storage_owned(vm, vm->current_psp,
                              table_segment, table_offset))
        (void)dos_mem_free(vm, table_segment);
    vm->jft_external_segment = 0;
    vm->jft_external_psp = 0;
    vm->jft_active = false;
    dos_find_close_all(vm);
}

typedef struct {
    cpu8086_state_t *cpu_pointer;
    cpu8086_state_t cpu;
    dpmi_state_t dpmi;
    dos_sft_entry_t sft[DOS_MAX_SFT_ENTRIES];
    dos_search_t searches[DOS_MAX_SEARCHES];
    struct dos_vcpi_state *vcpi;
    void *jit;
    uint32_t ems_frame_bases[DOS_EMS_FRAME_PAGES];
    uint64_t native_resume_jmpbuf[9];
    uint64_t native_cr3;
    void *native_gdt;
    void *native_ldt;
    uint16_t current_psp;
    uint16_t dta_seg;
    uint16_t dta_off;
    uint16_t jft_external_segment;
    uint16_t jft_external_psp;
    uint16_t interpreter_stop_cs;
    uint16_t interpreter_stop_ip;
    uint16_t exec_depth;
    uint16_t next_search_token;
    uint32_t next_search_serial;
    uint32_t int22;
    uint32_t int23;
    uint32_t int24;
    uint8_t indos_count;
    uint8_t last_return_code;
    uint8_t last_return_type;
    uint8_t termination_type;
    uint8_t native_dispatch_depth;
    bool process_terminated;
    bool jft_active;
    bool native_idt_saved;
    bool native_ready;
    bool native_active;
    bool native_resume_armed;
    bool interpreter_stop_active;
    bool interpreter_stop_reached;
    bool backend_detached;
} dos_exec_parent_state_t;

struct dos_exec_context {
    struct dos_exec_context *previous;
    dos_exec_parent_state_t parent;
    uint32_t return_eip;
    uint32_t return_flags;
    uint16_t return_segment;
    uint16_t child_psp;
    uint16_t entry_segment;
    uint16_t entry_offset;
    uint8_t return_frame_bytes;
    bool child_active;
};

typedef struct {
    uint16_t environment;
    uint16_t command_offset;
    uint16_t command_segment;
    uint16_t fcb1_offset;
    uint16_t fcb1_segment;
    uint16_t fcb2_offset;
    uint16_t fcb2_segment;
} dos_exec_parameters_t;

static bool dos_exec_guest_buffer(dos_vm_t *vm, uint16_t segment,
                                  uint32_t offset, uint32_t size,
                                  uint32_t *address_out)
{
    if (!vm || !vm->mem || !size) return false;
    uint64_t address;
    if (vm->cpu && vm->cpu->protected_mode && vm->cpu->pm_cs_loaded) {
        dpmi_descriptor_t descriptor;
        if (!dpmi_guest_descriptor(vm, segment, &descriptor)) return false;
        uint32_t limit = dpmi_desc_get_limit(&descriptor);
        if (offset > limit || size - 1u > limit - offset) return false;
        address = (uint64_t)dpmi_desc_get_base(&descriptor) + offset;
    } else {
        if (offset > 0xFFFFu || size > 0x10000u - offset) return false;
        address = ((uint64_t)segment << 4) + offset;
    }
    if (address > vm->total_mem_size ||
        size > vm->total_mem_size - address)
        return false;
    if (address_out) *address_out = (uint32_t)address;
    return true;
}

static bool dos_exec_copy_from_guest(dos_vm_t *vm, uint16_t segment,
                                     uint16_t offset, void *destination,
                                     uint32_t size)
{
    uint32_t address;
    if (!destination || !dos_exec_guest_buffer(vm, segment, offset, size,
                                               &address))
        return false;
    uint8_t *bytes = (uint8_t *)destination;
    for (uint32_t i = 0; i < size; i++)
        bytes[i] = dos_mem_read8(vm, address + i);
    return true;
}

static bool dos_exec_copy_to_guest(dos_vm_t *vm, uint16_t segment,
                                   uint32_t offset, const void *source,
                                   uint32_t size)
{
    uint32_t address;
    if (!source || !dos_exec_guest_buffer(vm, segment, offset, size,
                                          &address))
        return false;
    const uint8_t *bytes = (const uint8_t *)source;
    for (uint32_t i = 0; i < size; i++)
        dos_mem_write8(vm, address + i, bytes[i]);
    return true;
}

static int dos_exec_read_path(dos_vm_t *vm, uint16_t segment,
                              uint16_t offset, char *path,
                              uint32_t capacity)
{
    if (!path || capacity < 2u) return DOS_ERR_PATH_NOT_FOUND;
    for (uint32_t i = 0; i + 1u < capacity; i++) {
        uint32_t address;
        if (!dos_exec_guest_buffer(vm, segment,
                                   (uint16_t)(offset + i), 1u, &address))
            return DOS_ERR_PATH_NOT_FOUND;
        path[i] = (char)dos_mem_read8(vm, address);
        if (!path[i]) return i ? 0 : DOS_ERR_FILE_NOT_FOUND;
        if ((uint32_t)offset + i == 0xFFFFu) break;
    }
    path[capacity - 1u] = 0;
    return DOS_ERR_PATH_NOT_FOUND;
}

static int dos_exec_stage_parameters(dos_vm_t *vm, uint16_t segment,
                                     uint16_t offset,
                                     dos_exec_parameters_t *parameters,
                                     uint8_t command_tail[128],
                                     uint8_t fcb1[16], uint8_t fcb2[16])
{
    uint8_t raw[14];
    if (!parameters ||
        !dos_exec_copy_from_guest(vm, segment, offset, raw, sizeof(raw)))
        return DOS_ERR_BAD_ENVIRONMENT;
    parameters->environment = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    parameters->command_offset =
        (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    parameters->command_segment =
        (uint16_t)(raw[4] | ((uint16_t)raw[5] << 8));
    parameters->fcb1_offset =
        (uint16_t)(raw[6] | ((uint16_t)raw[7] << 8));
    parameters->fcb1_segment =
        (uint16_t)(raw[8] | ((uint16_t)raw[9] << 8));
    parameters->fcb2_offset =
        (uint16_t)(raw[10] | ((uint16_t)raw[11] << 8));
    parameters->fcb2_segment =
        (uint16_t)(raw[12] | ((uint16_t)raw[13] << 8));

    if (!dos_exec_copy_from_guest(vm, parameters->command_segment,
                                  parameters->command_offset, command_tail,
                                  128u) || command_tail[0] > 126u)
        return DOS_ERR_BAD_ENVIRONMENT;
    command_tail[command_tail[0] + 1u] = 0x0Du;

    for (unsigned i = 0; i < 16u; i++) fcb1[i] = fcb2[i] = 0;
    if (parameters->fcb1_offset != 0xFFFFu &&
        !dos_exec_copy_from_guest(vm, parameters->fcb1_segment,
                                  parameters->fcb1_offset, fcb1, 16u))
        return DOS_ERR_BAD_ENVIRONMENT;
    if (parameters->fcb2_offset != 0xFFFFu &&
        !dos_exec_copy_from_guest(vm, parameters->fcb2_segment,
                                  parameters->fcb2_offset, fcb2, 16u))
        return DOS_ERR_BAD_ENVIRONMENT;
    return 0;
}

static int dos_exec_stage_jft(dos_vm_t *vm,
                              uint8_t inherited[DOS_PSP_JFT_ENTRIES])
{
    if (!vm || !inherited) return DOS_ERR_INVALID_DATA;
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
        uint8_t index = DOS_SFT_INVALID;
        if (!dos_jft_read(vm, (uint16_t)i, &index))
            return DOS_ERR_INVALID_DATA;
        dos_sft_entry_t *entry = dos_sft_from_index(vm, index);
        inherited[i] = entry && !(entry->open_mode & DOS_OPEN_NO_INHERIT)
                     ? index : DOS_SFT_INVALID;
    }
    return 0;
}

static int dos_exec_retain_inherited_handles(
    dos_vm_t *vm, const uint8_t inherited[DOS_PSP_JFT_ENTRIES])
{
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
        uint8_t index = inherited[i];
        if (index == DOS_SFT_INVALID) continue;
        dos_sft_entry_t *entry = dos_sft_from_index(vm, index);
        if (!entry || entry->ref_count == 0xFFFFu)
            return DOS_ERR_TOO_MANY_OPEN_FILES;
    }
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
        uint8_t index = inherited[i];
        if (index != DOS_SFT_INVALID) vm->sft[index].ref_count++;
    }
    return 0;
}

static void dos_exec_save_parent(dos_vm_t *vm,
                                 dos_exec_parent_state_t *parent)
{
    parent->cpu_pointer = vm->cpu;
    parent->cpu = *vm->cpu;
    parent->dpmi = vm->dpmi;
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++)
        parent->sft[i] = vm->sft[i];
    for (unsigned i = 0; i < DOS_MAX_SEARCHES; i++)
        parent->searches[i] = vm->searches[i];
    parent->vcpi = vm->vcpi;
    parent->jit = vm->jit;
    for (unsigned i = 0; i < DOS_EMS_FRAME_PAGES; i++)
        parent->ems_frame_bases[i] = vm->ems_frame_bases[i];
    for (unsigned i = 0; i < 9u; i++)
        parent->native_resume_jmpbuf[i] = vm->native_resume_jmpbuf[i];
    parent->native_cr3 = vm->native_cr3;
    parent->native_gdt = vm->native_gdt;
    parent->native_ldt = vm->native_ldt;
    parent->current_psp = vm->current_psp;
    parent->dta_seg = vm->dta_seg;
    parent->dta_off = vm->dta_off;
    parent->jft_external_segment = vm->jft_external_segment;
    parent->jft_external_psp = vm->jft_external_psp;
    parent->interpreter_stop_cs = vm->interpreter_stop_cs;
    parent->interpreter_stop_ip = vm->interpreter_stop_ip;
    parent->exec_depth = vm->exec_depth;
    parent->next_search_token = vm->next_search_token;
    parent->next_search_serial = vm->next_search_serial;
    parent->int22 = dos_mem_read32(vm, 0x22u * 4u);
    parent->int23 = dos_mem_read32(vm, 0x23u * 4u);
    parent->int24 = dos_mem_read32(vm, 0x24u * 4u);
    parent->indos_count = vm->indos_count;
    parent->last_return_code = vm->last_return_code;
    parent->last_return_type = vm->last_return_type;
    parent->termination_type = vm->termination_type;
    parent->native_dispatch_depth = vm->native_dispatch_depth;
    parent->process_terminated = vm->process_terminated;
    parent->jft_active = vm->jft_active;
    parent->native_idt_saved = vm->native_idt_saved;
    parent->native_ready = vm->native_ready;
    parent->native_active = vm->native_active;
    parent->native_resume_armed = vm->native_resume_armed;
    parent->interpreter_stop_active = vm->interpreter_stop_active;
    parent->interpreter_stop_reached = vm->interpreter_stop_reached;
    parent->backend_detached = vm->native_ready || vm->native_active ||
        vm->native_cr3 || vm->native_gdt || vm->native_ldt;
}

static int dos_exec_hold_parent_files(const dos_exec_parent_state_t *parent)
{
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        const dos_sft_entry_t *entry = &parent->sft[i];
        if (!entry->used || entry->is_device || !entry->osfs_file) continue;
        if (osfs2_file_retain(entry->osfs_file) >= 0) continue;

        for (unsigned held = 0; held < i; held++) {
            entry = &parent->sft[held];
            if (entry->used && !entry->is_device && entry->osfs_file)
                osfs2_file_release(entry->osfs_file);
        }
        return DOS_ERR_ACCESS_DENIED;
    }
    return 0;
}

static void dos_exec_release_parent_files(
    const dos_exec_parent_state_t *parent)
{
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        const dos_sft_entry_t *entry = &parent->sft[i];
        if (entry->used && !entry->is_device && entry->osfs_file)
            osfs2_file_release(entry->osfs_file);
    }
}

static void dos_exec_begin_child(dos_vm_t *vm,
                                 dos_exec_parent_state_t *parent)
{
    vm->vcpi = NULL;
    vm->jit = NULL;
    for (unsigned i = 0; i < DOS_EMS_FRAME_PAGES; i++)
        vm->ems_frame_bases[i] = 0;

    if (parent->backend_detached) {
        vm->native_cr3 = 0;
        vm->native_gdt = NULL;
        vm->native_ldt = NULL;
        vm->native_idt_saved = false;
        vm->native_ready = false;
        vm->native_active = false;
    }
    vm->native_resume_armed = false;
    vm->interpreter_stop_active = false;
    vm->interpreter_stop_reached = false;

    dpmi_init(vm);
    for (unsigned i = 0; i < DPMI_EXT_BITMAP_SIZE; i++)
        vm->dpmi.ext_page_bitmap[i] = parent->dpmi.ext_page_bitmap[i];
    dos_find_init(vm);
    vm->last_return_code = 0;
    vm->last_return_type = 0;
    vm->termination_type = 0;
    vm->process_terminated = false;
    vm->exec_depth = (uint16_t)(parent->exec_depth + 1u);
    vm->indos_count = 0;
    dos_publish_indos(vm);
}

static void dos_exec_restore_sft(dos_vm_t *vm,
                                 const dos_exec_parent_state_t *parent)
{
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        dos_sft_entry_t *current = &vm->sft[i];
        const dos_sft_entry_t *saved = &parent->sft[i];
        if (!saved->used) {
            if (current->used && !current->is_device && current->osfs_file)
                osfs2_file_release(current->osfs_file);
            *current = (dos_sft_entry_t){0};
            continue;
        }

        bool same_object = current->used &&
            current->is_device == saved->is_device &&
            current->owner_psp == saved->owner_psp &&
            (saved->is_device
                ? current->device_kind == saved->device_kind
                : current->osfs_file == saved->osfs_file);
        uint32_t position = saved->position;
        uint32_t file_size = saved->file_size;
        if (same_object) {
            position = current->position;
            file_size = current->is_device
                      ? 0 : dos_file_size32(current->osfs_file);
        } else if (current->used && !current->is_device &&
                   current->osfs_file) {
            osfs2_file_release(current->osfs_file);
        }
        *current = *saved;
        current->position = position;
        current->file_size = file_size;

        /* The live entry already owns a retain when it survived the child.
         * Otherwise the snapshot retain becomes the restored entry's retain. */
        if (same_object && !saved->is_device && saved->osfs_file)
            osfs2_file_release(saved->osfs_file);
    }
}

static void dos_exec_restore_parent(dos_vm_t *vm,
                                    dos_exec_parent_state_t *parent,
                                    uint16_t child_psp,
                                    uint64_t child_instructions)
{
    bool same_native_backend = vm->native_cr3 == parent->native_cr3 &&
        vm->native_gdt == parent->native_gdt &&
        vm->native_ldt == parent->native_ldt;
    if (!same_native_backend)
        dos_native_release_backend(vm);
    if (vm->vcpi && vm->vcpi != parent->vcpi) dos_vcpi_cleanup(vm);
    dos_exec_restore_sft(vm, parent);
    if (child_psp) (void)dos_mem_free_owner(vm, child_psp);

    dos_mem_write32(vm, 0x22u * 4u, parent->int22);
    dos_mem_write32(vm, 0x23u * 4u, parent->int23);
    dos_mem_write32(vm, 0x24u * 4u, parent->int24);
    vm->dpmi = parent->dpmi;
    vm->vcpi = parent->vcpi;
    vm->jit = parent->jit;
    for (unsigned i = 0; i < DOS_EMS_FRAME_PAGES; i++)
        vm->ems_frame_bases[i] = parent->ems_frame_bases[i];

    vm->native_cr3 = parent->native_cr3;
    vm->native_gdt = parent->native_gdt;
    vm->native_ldt = parent->native_ldt;
    vm->native_idt_saved = parent->native_idt_saved;
    vm->native_ready = parent->native_ready;
    vm->native_active = parent->native_active;
    vm->native_resume_armed = parent->native_resume_armed;
    for (unsigned i = 0; i < 9u; i++)
        vm->native_resume_jmpbuf[i] = parent->native_resume_jmpbuf[i];
    vm->native_dispatch_depth = parent->native_dispatch_depth;
    vm->interpreter_stop_cs = parent->interpreter_stop_cs;
    vm->interpreter_stop_ip = parent->interpreter_stop_ip;
    vm->interpreter_stop_active = parent->interpreter_stop_active;
    vm->interpreter_stop_reached = parent->interpreter_stop_reached;

    vm->current_psp = parent->current_psp;
    vm->dta_seg = parent->dta_seg;
    vm->dta_off = parent->dta_off;
    vm->jft_external_segment = parent->jft_external_segment;
    vm->jft_external_psp = parent->jft_external_psp;
    vm->jft_active = parent->jft_active;
    for (unsigned i = 0; i < DOS_MAX_SEARCHES; i++)
        vm->searches[i] = parent->searches[i];
    vm->next_search_token = parent->next_search_token;
    vm->next_search_serial = parent->next_search_serial;
    vm->indos_count = parent->indos_count;
    vm->last_return_code = parent->last_return_code;
    vm->last_return_type = parent->last_return_type;
    vm->termination_type = parent->termination_type;
    vm->process_terminated = parent->process_terminated;
    vm->exec_depth = parent->exec_depth;

    vm->cpu = parent->cpu_pointer;
    *vm->cpu = parent->cpu;
    vm->cpu->vm = vm;
    vm->cpu->insn_count += child_instructions;
    dos_sync_system_variables(vm);
    dos_publish_indos(vm);
}

static int dos_exec_finish_load_only(
    dos_vm_t *vm, const uint8_t *image, uint64_t file_size, int format,
    const char *canonical, const dos_load_spec_t *spec,
    const uint8_t inherited[DOS_PSP_JFT_ENTRIES],
    uint16_t block_segment, uint16_t block_offset)
{
    uint64_t context_pages =
        (sizeof(struct dos_exec_context) + 4095u) / 4096u;
    struct dos_exec_context *context =
        (struct dos_exec_context *)dos_host_alloc_pages(context_pages);
    if (!context) return DOS_ERR_NOT_ENOUGH_MEMORY;

    context->previous = vm->exec_context;
    context->child_psp = 0;
    context->entry_segment = 0;
    context->entry_offset = 0;
    context->return_segment = spec->termination_segment;
    context->return_eip = vm->cpu->eip;
    context->return_flags = vm->software_int_frame_bytes
                          ? vm->software_int_return_flags
                          : vm->cpu->eflags;
    context->return_frame_bytes = vm->software_int_frame_bytes;
    context->child_active = false;
    dos_exec_save_parent(vm, &context->parent);
    int error = dos_exec_hold_parent_files(&context->parent);
    if (error) {
        dos_host_free_pages(context, context_pages);
        return error;
    }

    cpu8086_state_t caller_cpu = *vm->cpu;
    uint16_t parent_psp = vm->current_psp;
    uint16_t parent_dta_seg = vm->dta_seg;
    uint16_t parent_dta_off = vm->dta_off;
    uint16_t parent_jft_segment = vm->jft_external_segment;
    uint16_t parent_jft_psp = vm->jft_external_psp;
    bool parent_jft_active = vm->jft_active;
    uint16_t child_psp = 0;

    error = format == DOS_FMT_MZ
          ? dos_load_mz_process(vm, image, file_size, canonical,
                                spec, &child_psp)
          : dos_load_com_process(vm, image, file_size, canonical,
                                 spec, &child_psp);
    bool handles_retained = false;
    uint8_t entry_state[8] = {0};
    uint16_t entry_values[4] = {0};
    if (!error) {
        error = dos_exec_retain_inherited_handles(vm, inherited);
        handles_retained = !error;
    }
    if (!error) {
        uint16_t initial_ax = vm->cpu->ax;
        cpu_push16(vm->cpu, initial_ax);
        uint16_t values[4] = {
            vm->cpu->sp, vm->cpu->ss, vm->cpu->ip, vm->cpu->cs
        };
        for (unsigned word = 0; word < 4u; word++) {
            entry_values[word] = values[word];
            entry_state[word * 2u] = (uint8_t)values[word];
            entry_state[word * 2u + 1u] = (uint8_t)(values[word] >> 8);
        }
    }

    *vm->cpu = caller_cpu;
    vm->cpu->vm = vm;
    if (!error &&
        !dos_exec_copy_to_guest(vm, block_segment,
                                (uint32_t)block_offset + 14u,
                                entry_state, sizeof(entry_state)))
        error = DOS_ERR_BAD_ENVIRONMENT;

    if (error) {
        if (handles_retained) {
            for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
                dos_release_file_handle(vm, (int)i);
        }
        if (child_psp) (void)dos_mem_free_owner(vm, child_psp);
        vm->current_psp = parent_psp;
        vm->dta_seg = parent_dta_seg;
        vm->dta_off = parent_dta_off;
        vm->jft_external_segment = parent_jft_segment;
        vm->jft_external_psp = parent_jft_psp;
        vm->jft_active = parent_jft_active;
        dos_exec_release_parent_files(&context->parent);
        dos_host_free_pages(context, context_pages);
    } else {
        context->child_psp = child_psp;
        context->entry_offset = entry_values[2];
        context->entry_segment = entry_values[3];
        vm->exec_context = context;
        vm->exec_depth = (uint16_t)(context->parent.exec_depth + 1u);
        vm->dta_seg = child_psp;
        vm->dta_off = 0x0080u;
        dos_refresh_external_jft(vm);
        dos_psp_t *child = NULL;
        if (dos_psp_location(vm, child_psp, &child, NULL)) {
            dos_mem_write32(vm, 0x22u * 4u, child->old_int22);
            dos_mem_write32(vm, 0x23u * 4u, child->old_int23);
            dos_mem_write32(vm, 0x24u * 4u, child->old_int24);
        }
        serial_puts("[DOS] EXEC loaded child PSP=");
        serial_puthex(child_psp, 4);
        serial_puts(" without execution\n");
    }
    dos_sync_system_variables(vm);
    return error;
}

static void dos_exec_finalize_load_return(dos_vm_t *vm)
{
    struct dos_exec_context *context = vm ? vm->exec_context : NULL;
    if (!context || context->child_psp != vm->current_psp || !vm->cpu)
        return;

    context->parent.cpu = *vm->cpu;
    context->parent.cpu.vm = vm;
    const uint32_t status_flags = FLAG_CF | FLAG_PF | FLAG_AF |
                                  FLAG_ZF | FLAG_SF | FLAG_OF;
    if (context->return_frame_bytes) {
        context->return_flags =
            (context->return_flags & ~status_flags) |
            (vm->cpu->eflags & status_flags) | FLAGS_FIXED;
    } else {
        context->return_flags = vm->cpu->eflags | FLAGS_FIXED;
    }

    /* The saved state resumes after this INT 21h has unwound. */
    if (context->parent.indos_count)
        context->parent.indos_count--;
    if (context->parent.native_dispatch_depth)
        context->parent.native_dispatch_depth--;
}

bool dos_exec_activate_loaded_child(dos_vm_t *vm)
{
    struct dos_exec_context *context = vm ? vm->exec_context : NULL;
    if (!context || context->child_active || !vm->cpu ||
        vm->current_psp != context->child_psp ||
        vm->cpu->cs != context->entry_segment ||
        vm->cpu->ip != context->entry_offset)
        return false;

    dos_exec_begin_child(vm, &context->parent);
    context->child_active = true;
    vm->native_resume_armed = true;
    vm->dta_seg = context->child_psp;
    vm->dta_off = 0x0080u;
    vm->jft_active = true;
    dos_refresh_external_jft(vm);
    dos_sync_system_variables(vm);
    serial_puts("[DOS] EXEC starting loaded child PSP=");
    serial_puthex(context->child_psp, 4);
    serial_puts("\n");
    return true;
}

static void dos_exec_restore_loaded_context(dos_vm_t *vm,
                                            bool resume_parent)
{
    struct dos_exec_context *context = vm->exec_context;
    cpu8086_state_t *child_cpu = vm->cpu;
    uint8_t return_code = vm->process_terminated
                        ? (uint8_t)child_cpu->exit_code : 0xFFu;
    uint8_t return_type = vm->process_terminated
                        ? vm->termination_type : 1u;
    uint64_t elapsed = child_cpu->insn_count >= context->parent.cpu.insn_count
                     ? child_cpu->insn_count - context->parent.cpu.insn_count
                     : child_cpu->insn_count;

    uint32_t int22 = ((uint32_t)context->return_segment << 16) |
                     (uint16_t)context->return_eip;
    uint32_t int23 = context->parent.int23;
    uint32_t int24 = context->parent.int24;
    dos_psp_t *child_psp = NULL;
    if (dos_psp_location(vm, context->child_psp, &child_psp, NULL)) {
        int22 = child_psp->old_int22;
        int23 = child_psp->old_int23;
        int24 = child_psp->old_int24;
    }

    struct dos_exec_context *previous = context->previous;
    uint16_t child_segment = context->child_psp;
    dos_exec_restore_parent(vm, &context->parent, child_segment, elapsed);
    vm->exec_context = previous;

    if (resume_parent) {
        cpu_stack_adjust(vm->cpu, context->return_frame_bytes);
        uint16_t target_segment = (uint16_t)(int22 >> 16);
        uint32_t target_offset = (uint16_t)int22;
        if (vm->cpu->protected_mode &&
            target_segment == context->return_segment &&
            target_offset == (uint16_t)context->return_eip)
            target_offset = context->return_eip;
        vm->cpu->cs = target_segment;
        vm->cpu->eip = target_offset;
        vm->cpu->eflags = context->return_flags | FLAGS_FIXED;
        vm->cpu->halted = false;
        vm->cpu->running = true;
        vm->cpu->exit_code = 0;
        vm->process_terminated = false;
        vm->last_return_code = return_code;
        vm->last_return_type = return_type;
        cpu8086_sync_cs(vm->cpu);
        dos_mem_write32(vm, 0x22u * 4u, int22);
        dos_mem_write32(vm, 0x23u * 4u, int23);
        dos_mem_write32(vm, 0x24u * 4u, int24);
        dos_sync_system_variables(vm);
        dos_publish_indos(vm);
        serial_puts("[DOS] EXEC returned child PSP=");
        serial_puthex(child_segment, 4);
        serial_puts(" to ");
        serial_puthex(target_segment, 4);
        serial_puts(":");
        serial_puthex(target_offset, 8);
        serial_puts("\n");
    }

    dos_host_free_pages(context,
                         (sizeof(struct dos_exec_context) + 4095u) / 4096u);
}

bool dos_exec_complete_termination(dos_vm_t *vm)
{
    if (!vm || !vm->cpu || vm->cpu->running || !vm->exec_context ||
        vm->exec_context->child_psp != vm->current_psp)
        return false;
    dos_exec_restore_loaded_context(vm, true);
    return true;
}

void dos_exec_cleanup(dos_vm_t *vm)
{
    if (!vm) return;
    while (vm->exec_context)
        dos_exec_restore_loaded_context(vm, false);
}

static int dos_exec_load_program(dos_vm_t *vm, uint16_t path_segment,
                                 uint16_t path_offset,
                                 uint16_t block_segment,
                                 uint16_t block_offset, bool execute)
{
    if (!vm || !vm->cpu || vm->exec_depth == 0xFFFFu)
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    if (!execute &&
        !dos_exec_guest_buffer(vm, block_segment, block_offset, 22u, NULL))
        return DOS_ERR_BAD_ENVIRONMENT;

    char path[128], os_path[128], resolved[128], canonical[128];
    uint8_t command_tail[128], fcb1[16], fcb2[16];
    uint8_t inherited[DOS_PSP_JFT_ENTRIES];
    dos_exec_parameters_t parameters;
    int error = dos_exec_read_path(vm, path_segment, path_offset, path,
                                   sizeof(path));
    if (!error)
        error = dos_exec_stage_parameters(vm, block_segment, block_offset,
                                          &parameters, command_tail,
                                          fcb1, fcb2);
    if (!error) error = dos_exec_stage_jft(vm, inherited);
    if (error) return error;

    uint16_t environment_source = parameters.environment;
    if (!environment_source) {
        uint32_t psp_address = (uint32_t)vm->current_psp << 4;
        if (!vm->current_psp || psp_address > vm->total_mem_size ||
            sizeof(dos_psp_t) > vm->total_mem_size - psp_address)
            return DOS_ERR_BAD_ENVIRONMENT;
        environment_source =
            ((dos_psp_t *)(vm->mem + psp_address))->env_seg;
    }

    error = dos_path_to_osfs(path, vm->current_drive, vm->current_dir,
                             os_path, sizeof(os_path));
    if (!error && os_path[0])
        error = dos_resolve_path(vm, os_path, false, resolved,
                                 sizeof(resolved));
    if (error || !os_path[0])
        return error ? error : DOS_ERR_FILE_NOT_FOUND;
    void *file = osfs2_find_ci(resolved);
    if (!file) return DOS_ERR_FILE_NOT_FOUND;

    uint64_t file_size = osfs2_file_size(file);
    if (!file_size) return DOS_ERR_BAD_FORMAT;
    if (file_size > DOS_TOTAL_MEM || file_size > 0x7FFFFFFFu)
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    uint64_t image_pages = (file_size + 4095u) / 4096u;
    uint8_t *image = (uint8_t *)dos_host_alloc_pages(image_pages);
    if (!image) return DOS_ERR_NOT_ENOUGH_MEMORY;
    if (osfs2_read(file, 0, image, file_size) != (int)file_size) {
        dos_host_free_pages(image, image_pages);
        return DOS_ERR_ACCESS_DENIED;
    }

    int format = dos_detect_format(image, file_size);

    uint32_t canonical_size = 0;
    error = dos_truename_canonicalize(path, vm->current_drive,
                                      vm->current_dir, canonical,
                                      sizeof(canonical), &canonical_size);
    if (error) {
        dos_host_free_pages(image, image_pages);
        return error;
    }

    dos_load_spec_t spec = {0};
    spec.parent_psp = vm->current_psp;
    spec.termination_offset = vm->cpu->ip;
    spec.termination_segment = vm->cpu->cs;
    spec.environment_source = environment_source;
    spec.jft = inherited;
    spec.command_tail = command_tail;
    spec.fcb1 = fcb1;
    spec.fcb2 = fcb2;
    spec.copy_environment = true;
    spec.initialize_cpu = true;
    spec.has_termination_address = true;

    if (!execute) {
        error = dos_exec_finish_load_only(vm, image, file_size, format,
                                          canonical, &spec, inherited,
                                          block_segment, block_offset);
        dos_host_free_pages(image, image_pages);
        return error;
    }

    uint64_t state_pages = (sizeof(dos_exec_parent_state_t) + 4095u) / 4096u;
    dos_exec_parent_state_t *parent =
        (dos_exec_parent_state_t *)dos_host_alloc_pages(state_pages);
    if (!parent) {
        dos_host_free_pages(image, image_pages);
        return DOS_ERR_NOT_ENOUGH_MEMORY;
    }
    dos_exec_save_parent(vm, parent);
    error = dos_exec_hold_parent_files(parent);
    if (error) {
        dos_host_free_pages(parent, state_pages);
        dos_host_free_pages(image, image_pages);
        return error;
    }
    dos_exec_begin_child(vm, parent);

    uint16_t child_psp = 0;
    if (format == DOS_FMT_MZ)
        error = dos_load_mz_process(vm, image, file_size, canonical,
                                    &spec, &child_psp);
    else
        error = dos_load_com_process(vm, image, file_size, canonical,
                                     &spec, &child_psp);
    dos_host_free_pages(image, image_pages);

    if (!error) error = dos_exec_retain_inherited_handles(vm, inherited);
    uint64_t child_instructions = 0;
    uint8_t return_code = 0;
    uint8_t return_type = 0;
    if (!error) {
        vm->dta_seg = child_psp;
        vm->dta_off = 0x0080u;
        dos_psp_t *child = NULL;
        if (dos_psp_location(vm, child_psp, &child, NULL)) {
            dos_mem_write32(vm, 0x22u * 4u, child->old_int22);
            dos_mem_write32(vm, 0x23u * 4u, child->old_int23);
            dos_mem_write32(vm, 0x24u * 4u, child->old_int24);
        }
        vm->termination_type = 0;
        vm->process_terminated = false;
        dos_sync_system_variables(vm);
        serial_puts("[DOS] EXEC child PSP=");
        serial_puthex(child_psp, 4);
        serial_puts("\n");
        int child_result = cpu8086_run(vm);
        child_instructions = vm->cpu->insn_count;
        if (vm->process_terminated) {
            return_code = (uint8_t)child_result;
            return_type = vm->termination_type;
        } else {
            return_code = 0xFFu;
            return_type = 1u;
        }
    }

    dos_exec_restore_parent(vm, parent, child_psp, child_instructions);
    if (!error) {
        vm->last_return_code = return_code;
        vm->last_return_type = return_type;
    }
    dos_host_free_pages(parent, state_pages);
    return error;
}

static int dos_exec_load_overlay(dos_vm_t *vm, uint16_t path_segment,
                                 uint16_t path_offset,
                                 uint16_t block_segment,
                                 uint16_t block_offset)
{
    char path[128], os_path[128], resolved[128];
    uint8_t raw[4];
    int error = dos_exec_read_path(vm, path_segment, path_offset, path,
                                   sizeof(path));
    if (!error &&
        !dos_exec_copy_from_guest(vm, block_segment, block_offset, raw,
                                  sizeof(raw)))
        error = DOS_ERR_BAD_ENVIRONMENT;
    if (!error)
        error = dos_path_to_osfs(path, vm->current_drive, vm->current_dir,
                                 os_path, sizeof(os_path));
    if (!error && os_path[0])
        error = dos_resolve_path(vm, os_path, false, resolved,
                                 sizeof(resolved));
    if (error || !os_path[0])
        return error ? error : DOS_ERR_FILE_NOT_FOUND;

    void *file = osfs2_find_ci(resolved);
    if (!file) return DOS_ERR_FILE_NOT_FOUND;
    uint64_t file_size = osfs2_file_size(file);
    if (!file_size) return DOS_ERR_BAD_FORMAT;
    if (file_size > DOS_TOTAL_MEM || file_size > 0x7FFFFFFFu)
        return DOS_ERR_NOT_ENOUGH_MEMORY;

    uint64_t image_pages = (file_size + 4095u) / 4096u;
    uint8_t *image = (uint8_t *)dos_host_alloc_pages(image_pages);
    if (!image) return DOS_ERR_NOT_ENOUGH_MEMORY;
    if (osfs2_read(file, 0, image, file_size) != (int)file_size) {
        dos_host_free_pages(image, image_pages);
        return DOS_ERR_ACCESS_DENIED;
    }

    uint16_t load_segment = (uint16_t)(raw[0] |
                                       ((uint16_t)raw[1] << 8));
    uint16_t relocation_factor =
        (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    error = dos_load_overlay(vm, image, file_size, load_segment,
                             relocation_factor);
    dos_host_free_pages(image, image_pages);
    return error;
}

static int dos_open_path(dos_vm_t *vm, const char *path, uint16_t open_mode,
                         uint16_t create_attributes,
                         uint8_t existing_action, bool create_if_missing,
                         uint16_t *opened_handle, uint16_t *open_result)
{
    if (!vm || !path || !opened_handle || !open_result)
        return DOS_ERR_ACCESS_DENIED;
    *opened_handle = 0;
    *open_result = 0;
    if (!dos_open_mode_valid(open_mode)) return DOS_ERR_INVALID_ACCESS;
    if (create_attributes & (uint16_t)~OSFS_DOS_ATTR_MASK)
        return DOS_ERR_ACCESS_DENIED;
    if (existing_action > DOS_OPEN_EXISTING_REPLACE)
        return DOS_ERR_INVALID_FUNCTION;

    char ospath[128], actual_path[128];
    int error = dos_path_to_osfs(path, vm->current_drive, vm->current_dir,
                                 ospath, sizeof(ospath));
    if (error || !ospath[0])
        return error ? error : DOS_ERR_PATH_NOT_FOUND;

    int handle = dos_find_free_handle(vm);
    if (handle < 0) return DOS_ERR_TOO_MANY_OPEN_FILES;

    uint8_t device_kind = dos_device_from_path(ospath);
    if (device_kind != DOS_DEVICE_NONE) {
        if (existing_action == DOS_OPEN_EXISTING_FAIL)
            return DOS_ERR_FILE_EXISTS;
        error = dos_bind_device_handle(vm, handle, device_kind, open_mode);
        if (error) return error;
        *opened_handle = (uint16_t)handle;
        *open_result = DOS_OPEN_RESULT_OPENED;
        return 0;
    }
    if (!osfs2_is_mounted()) return DOS_ERR_PATH_NOT_FOUND;

    error = dos_resolve_path(vm, ospath, create_if_missing, actual_path,
                             sizeof(actual_path));
    if (error) return error;
    if (!actual_path[0] || osfs2_directory_exists_ci(actual_path))
        return DOS_ERR_ACCESS_DENIED;

#if DOS_DIAGNOSTICS
    serial_puts("[DOS] Open '");
    serial_puts(path);
    serial_puts("' osfs='");
    serial_puts(actual_path);
    serial_puts("' mode=0x");
    serial_puthex(open_mode, 4);
    serial_puts("\n");
#endif

    void *file = osfs2_find_ci(actual_path);
    if (!file) {
        if (!create_if_missing) return DOS_ERR_FILE_NOT_FOUND;
        file = osfs2_create(actual_path, 0);
        if (!file) {
            if (existing_action == DOS_OPEN_EXISTING_FAIL &&
                osfs2_find_ci(actual_path))
                return DOS_ERR_FILE_EXISTS;
            return DOS_ERR_ACCESS_DENIED;
        }

        error = dos_bind_file_handle(vm, handle, file, open_mode);
        if (error) {
            (void)osfs2_delete(actual_path);
            return error;
        }
        if (osfs2_file_set_dos_attributes(
                file, (uint8_t)create_attributes | OSFS_DOS_ATTR_ARCHIVE) < 0) {
            dos_release_file_handle(vm, handle);
            (void)osfs2_delete(actual_path);
            return DOS_ERR_ACCESS_DENIED;
        }
        *opened_handle = (uint16_t)handle;
        *open_result = DOS_OPEN_RESULT_CREATED;
        return 0;
    }

    if (existing_action == DOS_OPEN_EXISTING_FAIL)
        return DOS_ERR_FILE_EXISTS;

    uint8_t old_attributes = 0;
    if (osfs2_file_get_dos_attributes(file, &old_attributes) < 0)
        return DOS_ERR_ACCESS_DENIED;
    if ((existing_action == DOS_OPEN_EXISTING_REPLACE ||
         dos_access_writes(dos_open_access(open_mode))) &&
        (old_attributes & OSFS_DOS_ATTR_READ_ONLY))
        return DOS_ERR_ACCESS_DENIED;
    if (existing_action == DOS_OPEN_EXISTING_REPLACE &&
        ((old_attributes ^ (uint8_t)create_attributes) &
         (OSFS_DOS_ATTR_HIDDEN | OSFS_DOS_ATTR_SYSTEM)))
        return DOS_ERR_ACCESS_DENIED;

    error = dos_bind_file_handle(vm, handle, file, open_mode);
    if (error) return error;
    if (existing_action == DOS_OPEN_EXISTING_REPLACE) {
        if (osfs2_truncate(file, 0) < 0 ||
            osfs2_file_set_dos_attributes(
                file, (uint8_t)create_attributes | OSFS_DOS_ATTR_ARCHIVE) < 0) {
            dos_release_file_handle(vm, handle);
            return DOS_ERR_ACCESS_DENIED;
        }
        *open_result = DOS_OPEN_RESULT_REPLACED;
    } else {
        *open_result = DOS_OPEN_RESULT_OPENED;
    }
    *opened_handle = (uint16_t)handle;
    return 0;
}

static char dos_hex_digit(uint8_t value)
{
    return value < 10u ? (char)('0' + value)
                       : (char)('A' + value - 10u);
}

static int dos_create_temp_file(dos_vm_t *vm, uint16_t segment,
                                uint16_t offset, uint16_t attributes,
                                uint16_t *opened_handle)
{
    char path[128];
    uint32_t address = 0;
    if (!opened_handle || dos_guest_buffer(vm, segment, offset, 1,
                                           &address) < 0)
        return DOS_ERR_ACCESS_DENIED;

    uint32_t available = vm->total_mem_size - address;
    uint32_t segment_available = 0x10000u - offset;
    if (available > segment_available) available = segment_available;
    uint32_t scan_limit = available;
    if (scan_limit > sizeof(path)) scan_limit = sizeof(path);

    uint32_t length = 0;
    while (length < scan_limit) {
        path[length] = (char)dos_mem_read8(vm, address + length);
        if (path[length] == 0) break;
        length++;
    }
    if (length == scan_limit)
        return scan_limit < sizeof(path) ? DOS_ERR_ACCESS_DENIED
                                         : DOS_ERR_PATH_NOT_FOUND;

    bool has_separator = length != 0 &&
        (path[length - 1] == '\\' || path[length - 1] == '/');
    uint32_t tail = length + (has_separator ? 0u : 1u);
    uint32_t total = tail + 8u + 1u;
    if (total > sizeof(path) || total > available)
        return DOS_ERR_PATH_NOT_FOUND;
    if (!has_separator) path[length] = '\\';

    for (uint32_t attempt = 0; attempt < 0x10000u; attempt++) {
        uint32_t token = ++vm->temp_file_serial;
        for (uint32_t i = 0; i < 8u; i++) {
            uint32_t shift = 28u - i * 4u;
            path[tail + i] = dos_hex_digit((uint8_t)(token >> shift) & 0x0Fu);
        }
        path[tail + 8u] = 0;
        for (uint32_t i = 0; i < total; i++)
            dos_mem_write8(vm, address + i, (uint8_t)path[i]);

        uint16_t open_result = 0;
        int error = dos_open_path(vm, path, DOS_ACCESS_READ_WRITE,
                                  attributes, DOS_OPEN_EXISTING_FAIL, true,
                                  opened_handle, &open_result);
        if (!error) return 0;
        if (error != DOS_ERR_FILE_EXISTS) return error;
    }
    return DOS_ERR_CANNOT_MAKE;
}

static int dos_seek_position(uint32_t current, uint32_t size, uint8_t origin,
                             int32_t offset, uint32_t *position)
{
    int64_t base;
    switch (origin) {
    case 0: base = 0; break;
    case 1: base = current; break;
    case 2: base = size; break;
    default: return DOS_ERR_INVALID_FUNCTION;
    }

    int64_t result = base + (int64_t)offset;
    if (result < 0 || result > UINT32_MAX)
        return DOS_ERR_INVALID_FUNCTION;
    *position = (uint32_t)result;
    return 0;
}

static int dos_disk_geometry(uint32_t block_size, uint32_t total_blocks,
                             uint32_t free_blocks, uint16_t *sectors_per_cluster,
                             uint16_t *free_clusters, uint16_t *bytes_per_sector,
                             uint16_t *total_clusters)
{
    if (!block_size || !total_blocks || !sectors_per_cluster ||
        !free_clusters || !bytes_per_sector || !total_clusters)
        return -1;

    const uint32_t sector_size = 512;
    uint32_t sectors = 1;
    uint64_t total_bytes = (uint64_t)block_size * total_blocks;
    uint64_t free_bytes = (uint64_t)block_size * free_blocks;
    while (sectors < 128 &&
           total_bytes / ((uint64_t)sector_size * sectors) > 0xFFFFU)
        sectors <<= 1;

    uint64_t cluster_size = (uint64_t)sector_size * sectors;
    uint64_t total = total_bytes / cluster_size;
    uint64_t free = free_bytes / cluster_size;
    if (total > 0xFFFFU) total = 0xFFFFU;
    if (free > total) free = total;

    *sectors_per_cluster = (uint16_t)sectors;
    *free_clusters = (uint16_t)free;
    *bytes_per_sector = (uint16_t)sector_size;
    *total_clusters = (uint16_t)total;
    return 0;
}

/* ── Helper: write char to console (handles CR/LF) ─────────────── */

static void dos_putchar(dos_vm_t *vm, char ch)
{
    (void)vm;
    fb_putchar(ch);
    serial_putchar(ch);  /* echo to serial for debugging */
}

/* ── INT 21h function dispatch ──────────────────────────────────── */

void dos_int21_dispatch(dos_vm_t *vm)
{
    if (!vm || !vm->cpu) return;
    cpu8086_state_t *cpu = vm->cpu;
    dos_psp_t *current_psp = NULL;
    if (dos_psp_location(vm, vm->current_psp, &current_psp, NULL)) {
        current_psp->last_ss_sp = ((uint32_t)cpu->ss << 16) |
                                  (uint16_t)cpu_stack_offset(cpu);
    }
    uint8_t ah = cpu->ah;
    uint8_t previous_indos = vm->indos_count;
    if (vm->indos_count != 0xFFu) vm->indos_count++;
    dos_publish_indos(vm);

    switch (ah) {

    /* ── AH=00h: Terminate ──────────────────────────────────────── */
    case 0x00:
        vm->termination_type = 0;
        vm->process_terminated = true;
        cpu->running = false;
        cpu->exit_code = 0;
        break;

    /* ── AH=01h: Read char with echo ────────────────────────────── */
    case 0x01: {
        char ch = kb_getchar();
        dos_putchar(vm, ch);
        cpu->al = (uint8_t)ch;
        break;
    }

    /* ── AH=02h: Write character ────────────────────────────────── */
    case 0x02:
        dos_putchar(vm, (char)cpu->dl);
        break;

    /* ── AH=06h: Direct console I/O ─────────────────────────────── */
    case 0x06:
        if (cpu->dl == 0xFF) {
            /* Input */
            if (kb_has_input()) {
                cpu->al = (uint8_t)kb_getchar();
                cpu->flags &= ~FLAG_ZF;
            } else {
                cpu->al = 0;
                cpu->flags |= FLAG_ZF;
            }
        } else {
            /* Output */
            dos_putchar(vm, (char)cpu->dl);
        }
        break;

    /* ── AH=07h/08h: Read char without echo ─────────────────────── */
    case 0x07:
    case 0x08: {
        cpu->al = (uint8_t)kb_getchar();
        break;
    }

    /* ── AH=09h: Write string ($ terminated) ────────────────────── */
    case 0x09: {
        uint16_t off = cpu->dx;
        for (uint32_t scanned = 0; scanned < 0x10000u; scanned++) {
            uint32_t address = dos_addr(vm, cpu->ds, off);
            if (address >= vm->total_mem_size) break;
            uint8_t ch = dos_mem_read8(vm, address);
            if (ch == '$') break;
            dos_putchar(vm, (char)ch);
            off++;
        }
        cpu->al = '$';
        break;
    }

    /* ── AH=0Ah: Buffered input ─────────────────────────────────── */
    case 0x0A: {
        uint32_t buf_addr = dos_addr(vm, cpu->ds, cpu->dx);
        uint8_t max_len = dos_mem_read8(vm, buf_addr);
        uint8_t count = 0;
        for (;;) {
            char ch = kb_getchar();
            if (ch == '\r' || ch == '\n') {
                dos_putchar(vm, '\r');
                dos_putchar(vm, '\n');
                break;
            }
            if (ch == 8 && count > 0) {  /* backspace */
                count--;
                dos_putchar(vm, '\b');
                dos_putchar(vm, ' ');
                dos_putchar(vm, '\b');
                continue;
            }
            if (count < max_len - 1) {
                dos_mem_write8(vm, buf_addr + 2 + count, (uint8_t)ch);
                dos_putchar(vm, ch);
                count++;
            }
        }
        dos_mem_write8(vm, buf_addr + 2 + count, 0x0D);
        dos_mem_write8(vm, buf_addr + 1, count);
        break;
    }

    /* ── AH=0Bh: Check stdin status ─────────────────────────────── */
    case 0x0B:
        cpu->al = kb_has_input() ? 0xFF : 0x00;
        break;

    /* ── AH=0Ch: Flush input + call function ────────────────────── */
    case 0x0C:
        /* Flush keyboard buffer */
        while (kb_has_input()) kb_getchar();
        /* Re-dispatch with AL as function */
        if (cpu->al == 0x01 || cpu->al == 0x06 || cpu->al == 0x07 ||
            cpu->al == 0x08 || cpu->al == 0x0A) {
            cpu->ah = cpu->al;
            dos_int21_dispatch(vm);
        }
        break;

    /* ── AH=0Dh: Disk Reset ─────────────────────────────────────── */
    /* MS-DOS flushes disk buffers. Our VM has no write-back cache,
     * so there's nothing to flush. Clear carry and return. */
    case 0x0D:
        cpu->flags &= ~0x0001; /* CF = 0 */
        break;

    /* AH=0Eh: Select default drive. Only C: is currently mounted. */
    case 0x0E:
        if (cpu->dl == 2) vm->current_drive = 2;
        cpu->al = 3;  /* highest available drive + 1 */
        break;

    /* ── AH=19h: Get current drive ──────────────────────────────── */
    case 0x19:
        cpu->al = vm->current_drive;  /* 2 = C: */
        break;

    /* ── AH=1Ah: Set DTA ────────────────────────────────────────── */
    case 0x1A:
        vm->dta_seg = cpu->ds;
        vm->dta_off = cpu->dx;
        break;

    /* ── AH=25h: Set interrupt vector ───────────────────────────── */
    case 0x25: {
        uint32_t ivt_addr = (uint32_t)cpu->al * 4;
        dos_mem_write16(vm, ivt_addr, cpu->dx);
        dos_mem_write16(vm, ivt_addr + 2, cpu->ds);
        break;
    }

    /* AH=26h: Copy the caller's PSP without creating a child process. */
    case 0x26:
        if (dos_create_psp_copy(vm, cpu->cs, cpu->dx, 0u, false) &&
            cpu->dx == vm->current_psp)
            dos_refresh_external_jft(vm);
        break;

    /* AH=2Ah: Get system date. AL=0 is Sunday. */
    case 0x2A: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        uint8_t day_of_week;
        if (!dos_clock_get(vm, &calendar, &hundredths) ||
            !dos_calendar_day_of_week(&calendar, &day_of_week)) {
            cpu->al = 0xFF;
            break;
        }
        cpu->cx = calendar.year;
        cpu->dh = calendar.month;
        cpu->dl = calendar.day;
        cpu->al = day_of_week;
        break;
    }

    /* AH=2Bh: Set system date. The clock remains private to this VM. */
    case 0x2B: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        if (!dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->al = 0xFF;
            break;
        }
        calendar.year = cpu->cx;
        calendar.month = cpu->dh;
        calendar.day = cpu->dl;
        cpu->al = dos_clock_set(vm, &calendar, hundredths) ? 0 : 0xFF;
        break;
    }

    /* AH=2Ch: Get system time, including hundredths in DL. */
    case 0x2C: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        if (!dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->al = 0;
            cpu->cx = 0;
            cpu->dx = 0;
            break;
        }
        cpu->ch = calendar.hour;
        cpu->cl = calendar.minute;
        cpu->dh = calendar.second;
        cpu->dl = hundredths;
        cpu->al = 0;
        break;
    }

    /* AH=2Dh: Set system time. */
    case 0x2D: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        if (!dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->al = 0xFF;
            break;
        }
        calendar.hour = cpu->ch;
        calendar.minute = cpu->cl;
        calendar.second = cpu->dh;
        cpu->al = dos_clock_set(vm, &calendar, cpu->dl) ? 0 : 0xFF;
        break;
    }

    /* ── AH=2Fh: Get DTA ────────────────────────────────────────── */
    case 0x2F:
        cpu->es = vm->dta_seg;
        cpu->bx = vm->dta_off;
        break;

    /* ── AH=30h: Get DOS version ────────────────────────────────── */
    case 0x30:
        cpu->al = 6;   /* major: DOS 6.22 */
        cpu->ah = 22;  /* minor */
        cpu->bx = 0;   /* OEM */
        cpu->cx = 0;
        break;

    /* ── AH=35h: Get interrupt vector ───────────────────────────── */
    case 0x35: {
        uint32_t ivt_addr = (uint32_t)cpu->al * 4;
        cpu->bx = dos_mem_read16(vm, ivt_addr);
        cpu->es = dos_mem_read16(vm, ivt_addr + 2);
        break;
    }

    /* AH=39h/3Ah: Create/remove a directory. */
    case 0x39: {
        char path[128], ospath[128], actual_path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        int error = dos_path_to_osfs(path, vm->current_drive,
                                     vm->current_dir, ospath,
                                     sizeof(ospath));
        if (!error && ospath[0])
            error = dos_resolve_path(vm, ospath, true, actual_path,
                                     sizeof(actual_path));
        if (error || !ospath[0]) {
            cpu->flags |= FLAG_CF;
            cpu->ax = error ? (uint16_t)error : DOS_ERR_ACCESS_DENIED;
        } else if (!osfs3_is_mounted()) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
        } else {
            int result = osfs3_mkdir(actual_path);
            if (result < 0) {
                cpu->flags |= FLAG_CF;
                cpu->ax = result == -3 ? DOS_ERR_PATH_NOT_FOUND
                                       : DOS_ERR_ACCESS_DENIED;
            } else {
                cpu->flags &= ~FLAG_CF;
            }
        }
        break;
    }

    case 0x3A: {
        char path[128], ospath[128], actual_path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        int error = dos_path_to_osfs(path, vm->current_drive,
                                     vm->current_dir, ospath,
                                     sizeof(ospath));
        if (!error && ospath[0])
            error = dos_resolve_path(vm, ospath, false, actual_path,
                                     sizeof(actual_path));
        if (error || !ospath[0]) {
            cpu->flags |= FLAG_CF;
            cpu->ax = error ? (uint16_t)error : DOS_ERR_ACCESS_DENIED;
        } else if (!osfs3_is_mounted() ||
                   dos_path_equal_ci(actual_path, vm->current_dir) ||
                   osfs3_rmdir(actual_path) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* AH=3Bh: Change current directory. */
    case 0x3B: {
        char path[128], ospath[128], actual_path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        int error = dos_path_to_osfs(path, vm->current_drive,
                                     vm->current_dir, ospath,
                                     sizeof(ospath));
        if (!error)
            error = dos_resolve_path(vm, ospath, false, actual_path,
                                     sizeof(actual_path));
        if (error ||
            (actual_path[0] &&
             !osfs2_directory_exists_ci(actual_path))) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_PATH_NOT_FOUND;
            break;
        }

        int length = 0;
        while (actual_path[length]) length++;
        if (length >= (int)sizeof(vm->current_dir)) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_PATH_NOT_FOUND;
            break;
        }
        for (int i = 0; i <= length; i++)
            vm->current_dir[i] = actual_path[i];
        cpu->flags &= ~FLAG_CF;
        break;
    }

    case 0x3C: {
        char path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        uint16_t handle = 0, open_result = 0;
        int error = dos_open_path(vm, path, DOS_ACCESS_READ_WRITE,
                                  cpu->cx,
                                  DOS_OPEN_EXISTING_REPLACE, true,
                                  &handle, &open_result);
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
            cpu->ax = handle;
        }
        break;
    }

    /* ── AH=3Dh: Open file ──────────────────────────────────────── */
    case 0x3D: {
        char path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        uint16_t handle = 0, open_result = 0;
        int error = dos_open_path(vm, path, cpu->al, 0,
                                  DOS_OPEN_EXISTING_OPEN, false,
                                  &handle, &open_result);
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
            cpu->ax = handle;
        }
        break;
    }

    /* ── AH=3Eh: Close file ─────────────────────────────────────── */
    case 0x3E: {
        uint16_t h = cpu->bx;
        if (!dos_handle_sft(vm, h)) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            break;
        }
        dos_release_file_handle(vm, h);
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=3Fh: Read file ──────────────────────────────────────── */
    case 0x3F: {
        uint16_t h = cpu->bx;
        uint16_t count = cpu->cx;
        dos_sft_entry_t *fh = dos_handle_sft(vm, h);

        if (!fh) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
#if DOS_DIAGNOSTICS
            static uint32_t bad_h_count = 0;
            bad_h_count++;
            if (bad_h_count < 8 || (bad_h_count & 0xFFFF) == 0) {
                serial_puts("[DOS] R BAD-HANDLE bx=0x");
                serial_puthex(h, 4);
                serial_puts(" cx=");      serial_putdec(count);
                serial_puts(" ds=0x");    serial_puthex(cpu->ds, 4);
                serial_puts(" dx=0x");    serial_puthex(cpu->dx, 4);
                serial_puts(" #");        serial_putdec(bad_h_count);
                serial_puts("\n");
            }
#endif
            break;
        }

        if (dos_open_access(fh->open_mode) == DOS_ACCESS_WRITE) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        if (fh->is_device) {
            if (fh->device_kind == DOS_DEVICE_CON) {
                uint32_t buf = 0;
                if (count && dos_guest_buffer(vm, cpu->ds, cpu->dx, count,
                                              &buf) < 0) {
                    cpu->flags |= FLAG_CF;
                    cpu->ax = DOS_ERR_ACCESS_DENIED;
                    break;
                }
                uint16_t total = 0;
                while (total < count) {
                    char ch = kb_getchar();
                    dos_mem_write8(vm, buf + total, (uint8_t)ch);
                    total++;
                    if (ch == '\r' || ch == '\n') break;
                }
                cpu->ax = total;
                cpu->flags &= ~FLAG_CF;
            } else if (fh->device_kind == DOS_DEVICE_NUL) {
                cpu->ax = 0;
                cpu->flags &= ~FLAG_CF;
            } else {
                cpu->flags |= FLAG_CF;
                cpu->ax = DOS_ERR_ACCESS_DENIED;
            }
            break;
        }
        if (!fh->osfs_file) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        fh->file_size = dos_file_size32(fh->osfs_file);
        uint32_t to_read = count;
        if (fh->position >= fh->file_size) {
            to_read = 0;
        } else if (to_read > fh->file_size - fh->position) {
            to_read = fh->file_size - fh->position;
        }

        uint32_t buf = 0;
        if (to_read && dos_guest_buffer(vm, cpu->ds, cpu->dx, to_read,
                                        &buf) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        uint32_t pos_before = fh->position;
        int total = to_read
            ? osfs2_read(fh->osfs_file, fh->position, vm->mem + buf, to_read)
            : 0;
        if (total < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        fh->position += (uint32_t)total;

#if DOS_DIAGNOSTICS
        /* Sampled trace: first 16 reads verbose, then every 64k. Shows
         * handle, source position, count requested, count actually
         * returned, and first 8 bytes of the data for diagnostic use. */
        static uint32_t read_count = 0;
        read_count++;
        if (read_count < 16 || (read_count & 0xFFFF) == 0) {
            serial_puts("[DOS] R h=");      serial_putdec(h);
            serial_puts(" pos=0x");         serial_puthex(pos_before, 8);
            serial_puts(" req=");           serial_putdec(count);
            serial_puts(" got=");           serial_putdec(total);
            serial_puts(" buf=0x");         serial_puthex(buf, 8);
            serial_puts(" data=");
            for (int i = 0; i < 8 && i < (int)total; i++) {
                serial_puts(" ");
                serial_puthex(dos_mem_read8(vm, buf + i), 2);
            }
            serial_puts(" #"); serial_putdec(read_count);
            serial_puts("\n");
        }
#else
        (void)pos_before;
#endif

        cpu->ax = (uint16_t)total;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=40h: Write file ─────────────────────────────────────── */
    case 0x40: {
        uint16_t h = cpu->bx;
        uint16_t count = cpu->cx;
        dos_sft_entry_t *fh = dos_handle_sft(vm, h);

        if (!fh) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            break;
        }

        if (dos_open_access(fh->open_mode) == DOS_ACCESS_READ) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        if (fh->is_device) {
            if (fh->device_kind == DOS_DEVICE_CON) {
                uint32_t buf = 0;
                if (count && dos_guest_buffer(vm, cpu->ds, cpu->dx, count,
                                              &buf) < 0) {
                    cpu->flags |= FLAG_CF;
                    cpu->ax = DOS_ERR_ACCESS_DENIED;
                    break;
                }
                for (uint16_t i = 0; i < count; i++)
                    dos_putchar(vm, (char)dos_mem_read8(vm, buf + i));
                cpu->ax = count;
                cpu->flags &= ~FLAG_CF;
            } else if (fh->device_kind == DOS_DEVICE_NUL) {
                cpu->ax = count;
                cpu->flags &= ~FLAG_CF;
            } else {
                cpu->flags |= FLAG_CF;
                cpu->ax = DOS_ERR_ACCESS_DENIED;
            }
            break;
        }
        if (!fh->osfs_file) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        if (count == 0) {
            if (osfs2_truncate(fh->osfs_file, fh->position) < 0) {
                cpu->flags |= FLAG_CF;
                cpu->ax = DOS_ERR_ACCESS_DENIED;
                break;
            }
            fh->file_size = fh->position;
            cpu->ax = 0;
            cpu->flags &= ~FLAG_CF;
            break;
        }

        if (fh->position > UINT32_MAX - count) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        uint32_t buf = 0;
        if (dos_guest_buffer(vm, cpu->ds, cpu->dx, count, &buf) < 0 ||
            osfs2_write(fh->osfs_file, fh->position, vm->mem + buf,
                        count) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        fh->position += count;
        if (fh->position > fh->file_size) fh->file_size = fh->position;
        cpu->ax = count;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* AH=41h: Delete file */
    case 0x41: {
        char path[128], ospath[128], actual_path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        int error = dos_path_to_osfs(path, vm->current_drive,
                                     vm->current_dir, ospath,
                                     sizeof(ospath));
        if (error || !ospath[0]) {
            cpu->flags |= FLAG_CF;
            cpu->ax = error ? (uint16_t)error : DOS_ERR_PATH_NOT_FOUND;
            break;
        }
        error = dos_resolve_path(vm, ospath, false, actual_path,
                                 sizeof(actual_path));
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
            break;
        }

        void *file = osfs2_find_ci(actual_path);
        if (!file) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_FILE_NOT_FOUND;
            break;
        }
        if (dos_file_is_read_only(file) || osfs2_delete(actual_path) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=42h: Seek (lseek) ──────────────────────────────────── */
    case 0x42: {
        uint16_t h = cpu->bx;
        dos_sft_entry_t *fh = dos_handle_sft(vm, h);
        if (!fh) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
#if DOS_DIAGNOSTICS
            static uint32_t bad_s_count = 0;
            bad_s_count++;
            if (bad_s_count < 8 || (bad_s_count & 0xFFFF) == 0) {
                serial_puts("[DOS] S BAD-HANDLE bx=0x");
                serial_puthex(h, 4);
                serial_puts(" al=");      serial_putdec(cpu->al);
                serial_puts(" cx:dx=0x"); serial_puthex(cpu->cx, 4);
                serial_puts(":0x");       serial_puthex(cpu->dx, 4);
                serial_puts(" #");        serial_putdec(bad_s_count);
                serial_puts("\n");
            }
#endif
            break;
        }

        if (fh->is_device || !fh->osfs_file) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            break;
        }
        fh->file_size = dos_file_size32(fh->osfs_file);

        int32_t offset = (int32_t)((uint32_t)cpu->cx << 16 | cpu->dx);
        uint32_t pos_before = fh->position;
        uint8_t whence = cpu->al;

        uint32_t new_position;
        int error = dos_seek_position(fh->position, fh->file_size, whence,
                                      offset, &new_position);
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
            break;
        }
        fh->position = new_position;

        cpu->dx = (uint16_t)(fh->position >> 16);
        cpu->ax = (uint16_t)(fh->position & 0xFFFF);
        cpu->flags &= ~FLAG_CF;

#if DOS_DIAGNOSTICS
        static uint32_t seek_count;
        seek_count++;
        if (seek_count < 16 || (seek_count & 0xFFFF) == 0) {
            serial_puts("[DOS] S h="); serial_putdec(h);
            serial_puts(" wh=");       serial_putdec(whence);
            serial_puts(" off=0x");    serial_puthex((uint32_t)offset, 8);
            serial_puts(" pos 0x");    serial_puthex(pos_before, 8);
            serial_puts(" -> 0x");     serial_puthex(fh->position, 8);
            serial_puts(" #");         serial_putdec(seek_count);
            serial_puts("\n");
        }
#else
        (void)pos_before;
#endif
        break;
    }

    /* AH=43h: Get/set file attributes. */
    case 0x43: {
        char path[128], ospath[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        int error = dos_path_to_osfs(path, vm->current_drive,
                                     vm->current_dir, ospath,
                                     sizeof(ospath));
        if (!error) {
            if (cpu->al == 0U) {
                uint16_t attributes = 0;
                error = dos_file_get_attributes(vm, ospath, &attributes);
                if (!error) cpu->cx = attributes;
            } else if (cpu->al == 1U) {
                error = dos_file_set_attributes(vm, ospath, cpu->cx);
            } else {
                error = DOS_ERR_INVALID_FUNCTION;
            }
        }
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* ── AH=44h: IOCTL ──────────────────────────────────────────── */
    case 0x44: {
        uint16_t h = cpu->bx;
        if (cpu->al == 0x00) {
            dos_sft_entry_t *entry = dos_handle_sft(vm, h);
            if (!entry) {
                cpu->flags |= FLAG_CF;
                cpu->ax = DOS_ERR_INVALID_HANDLE;
                break;
            }
            uint16_t info = dos_handle_device_info(entry);
            cpu->ax = info;
            cpu->dx = info;
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_FUNCTION;
        }
        break;
    }

    /* ── AH=45h: Duplicate file handle ──────────────────────────── */
    case 0x45: {
        if (!dos_handle_sft(vm, cpu->bx)) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            break;
        }
        int target = dos_find_free_handle(vm);
        if (target < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_TOO_MANY_OPEN_FILES;
            break;
        }
        if (dos_clone_handle(vm, cpu->bx, target, false) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            break;
        }
        cpu->ax = (uint16_t)target;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=46h: Force duplicate file handle ────────────────────── */
    case 0x46:
        if (dos_clone_handle(vm, cpu->bx, cpu->cx, true) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_HANDLE;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;

    /* ── AH=36h: Get disk free space ─────────────────────────────── */
    case 0x36: {
        if ((cpu->dl != 0 && cpu->dl != (uint8_t)(vm->current_drive + 1)) ||
            dos_disk_geometry(osfs2_get_block_size(), osfs2_total_blocks(),
                              osfs2_free_blocks(), &cpu->ax, &cpu->bx,
                              &cpu->cx, &cpu->dx) < 0) {
            cpu->ax = 0xFFFF;
        }
        break;
    }

    /* ── AH=38h: Get country info ──────────────────────────────── */
    case 0x38:
        cpu->flags &= ~FLAG_CF;
        cpu->bx = 1;  /* country code: USA */
        break;

    /* ── AH=47h: Get current directory ──────────────────────────── */
    case 0x47: {
        if (cpu->dl != 0 && cpu->dl != (uint8_t)(vm->current_drive + 1)) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_DRIVE;
            break;
        }
        int length = 0;
        while (vm->current_dir[length]) length++;
        uint32_t buf = 0;
        if (dos_guest_buffer(vm, cpu->ds, cpu->si, (uint32_t)length + 1,
                             &buf) < 0) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            break;
        }
        for (int i = 0; i < length; i++)
            dos_mem_write8(vm, buf + i,
                           vm->current_dir[i] == '/' ? '\\' : vm->current_dir[i]);
        dos_mem_write8(vm, buf + length, 0);
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=48h: Allocate memory ────────────────────────────────── */
    case 0x48: {
        uint16_t largest = 0;
#if DOS_DIAGNOSTICS
        /* Dump MCB chain before allocation */
        {
            uint16_t s = vm->first_mcb;
            serial_puts("[MCB] Chain before 48h alloc(");
            serial_putdec(cpu->bx);
            serial_puts("):");
            for (int i = 0; i < 10 && s; i++) {
                uint32_t a = (uint32_t)s << 4;
                dos_mcb_t *m = (dos_mcb_t *)(vm->mem + a);
                serial_puts(" [");
                serial_puthex(s, 4);
                serial_puts(" ");
                serial_puts(m->owner ? "USED" : "FREE");
                serial_puts(" sz=");
                serial_puthex(m->size, 4);
                serial_puts("]");
                if (m->type == 'Z') break;
                s += m->size + 1;
            }
            serial_puts("\n");
        }
#endif
        uint16_t seg = dos_mem_alloc(vm, cpu->bx, &largest);
#if DOS_DIAGNOSTICS
        serial_puts("[DOS] INT 21/48: alloc ");
        serial_putdec(cpu->bx);
        serial_puts(" para -> seg=");
        serial_puthex(seg, 4);
        serial_puts(seg ? " OK" : " FAIL");
        serial_puts("\n");
#endif
        if (seg) {
            cpu->ax = seg;
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->bx = largest;
            cpu->ax = DOS_ERR_NOT_ENOUGH_MEMORY;
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    /* ── AH=49h: Free memory ────────────────────────────────────── */
    case 0x49:
        if (dos_mem_free(vm, cpu->es) == 0) {
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->ax = DOS_ERR_INVALID_BLOCK;
            cpu->flags |= FLAG_CF;
        }
        break;

    /* ── AH=4Ah: Resize memory ──────────────────────────────────── */
    case 0x4A: {
#if DOS_DIAGNOSTICS
        serial_puts("[DOS] INT 21/4A: resize seg=");
        serial_puthex(cpu->es, 4);
        serial_puts(" to ");
        serial_putdec(cpu->bx);
        serial_puts(" para\n");
#endif
        uint16_t max_avail = 0;
        if (dos_mem_resize(vm, cpu->es, cpu->bx, &max_avail) == 0) {
            cpu->flags &= ~FLAG_CF;
        } else {
            cpu->bx = max_avail;
            cpu->ax = DOS_ERR_NOT_ENOUGH_MEMORY;
            cpu->flags |= FLAG_CF;
        }
        break;
    }

    /* AH=4Bh/AL=00h: load and synchronously execute a child process. */
    case 0x4B: {
        uint8_t exec_mode = cpu->al;
        int error;
        if (exec_mode == 0) {
            error = dos_exec_load_program(vm, cpu->ds, cpu->dx,
                                          cpu->es, cpu->bx, true);
        } else if (exec_mode == 1) {
            error = dos_exec_load_program(vm, cpu->ds, cpu->dx,
                                          cpu->es, cpu->bx, false);
        } else if (exec_mode == 3) {
            error = dos_exec_load_overlay(vm, cpu->ds, cpu->dx,
                                          cpu->es, cpu->bx);
        } else {
            error = DOS_ERR_INVALID_FUNCTION;
        }
        if (error) {
            cpu->ax = (uint16_t)error;
            cpu->flags |= FLAG_CF;
        } else {
            cpu->ax = 0;
            cpu->flags &= ~FLAG_CF;
            if (exec_mode == 1) dos_exec_finalize_load_return(vm);
        }
        break;
    }

    /* AH=4Eh/4Fh: Find first/next file using the current DTA. */
    case 0x4E: {
        char path[128], ospath[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        int error = dos_path_to_osfs(path, vm->current_drive,
                                     vm->current_dir, ospath,
                                     sizeof(ospath));
        if (!error) error = dos_find_first(vm, ospath, cpu->cx);
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    case 0x4F: {
        int error = dos_find_next(vm);
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* ── AH=4Ch: Exit with return code ──────────────────────────── */
    case 0x4C:
        serial_puts("[DOS] Exit(");
        serial_puthex(cpu->al, 2);
        serial_puts(") at #");
        serial_putdec(cpu->insn_count);
        serial_puts(" CS:EIP=");
        serial_puthex(cpu->cs, 4);
        serial_puts(":");
        serial_puthex(cpu->eip, 8);
        serial_puts(cpu->protected_mode ? " [PM]\n" : " [RM]\n");
        vm->termination_type = 0;
        vm->process_terminated = true;
        cpu->running = false;
        cpu->exit_code = cpu->al;
        break;

    /* AH=4Dh: return and consume the most recent child termination status. */
    case 0x4D:
        cpu->al = vm->last_return_code;
        cpu->ah = vm->last_return_type;
        vm->last_return_code = 0;
        vm->last_return_type = 0;
        break;

    /* AH=6Ch: Extended open/create. */
    case 0x6C: {
        uint8_t existing_action = (uint8_t)(cpu->dx & 0x0Fu);
        uint8_t missing_action = (uint8_t)((cpu->dx >> 4) & 0x0Fu);
        if (cpu->al != 0 || (cpu->dx & 0xFFE0u) != 0 ||
            existing_action > DOS_OPEN_EXISTING_REPLACE ||
            missing_action > 1u) {
            cpu->flags |= FLAG_CF;
            cpu->ax = DOS_ERR_INVALID_FUNCTION;
            break;
        }

        char path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->si, path, sizeof(path));
        uint16_t handle = 0, open_result = 0;
        int error = dos_open_path(vm, path, cpu->bx, cpu->cx,
                                  existing_action, missing_action == 1u,
                                  &handle, &open_result);
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
            cpu->ax = handle;
            cpu->cx = open_result;
        }
        break;
    }

    /* ── AH=62h: Get PSP ────────────────────────────────────────── */
    case 0x62:
        cpu->bx = vm->current_psp;
        break;

    /* ── AH=33h: Get/Set Ctrl+Break ──────────────────────────────── */
    case 0x33:
        switch (cpu->al) {
        case 0x00:
            cpu->dl = vm->ctrl_break_enabled ? 1u : 0u;
            break;
        case 0x01:
            vm->ctrl_break_enabled = (cpu->dl & 1u) != 0;
            cpu->dl = vm->ctrl_break_enabled ? 1u : 0u;
            break;
        case 0x02: {
            uint8_t previous = vm->ctrl_break_enabled ? 1u : 0u;
            vm->ctrl_break_enabled = (cpu->dl & 1u) != 0;
            cpu->dl = previous;
            break;
        }
        case 0x05:
            cpu->dl = 3u;
            break;
        case 0x06:
            cpu->bl = 6u;
            cpu->bh = 22u;
            cpu->dl = 0u;
            cpu->dh = 0u;
            break;
        default:
            cpu->al = 0xFFu;
            break;
        }
        break;

    /* ── AH=34h: Get InDOS flag pointer ───────────────────────────── */
    case 0x34:
        cpu->es = DOS_SYSVARS_SEG;
        cpu->bx = DOS_INDOS_OFF;
        break;

    /* ── AH=50h: Set PSP ──────────────────────────────────────────── */
    case 0x50:
        vm->current_psp = cpu->bx;
        vm->jft_active = true;
        dos_refresh_external_jft(vm);
        dos_sync_system_variables(vm);
        break;

    /* ── AH=51h: Get PSP ──────────────────────────────────────────── */
    case 0x51:
        cpu->bx = vm->current_psp;
        break;

    /* ── AH=52h: Get List of Lists (SYSVARS) ──────────────────────── */
    case 0x52:
        dos_sync_system_variables(vm);
        cpu->es = DOS_SYSVARS_SEG;
        cpu->bx = DOS_SYSVARS_OFF;
        break;

    /* AH=55h: Create a child PSP and make it the current process. */
    case 0x55: {
        uint16_t parent_psp = vm->current_psp;
        if (dos_create_psp_copy(vm, parent_psp, cpu->dx, cpu->si, true)) {
            vm->current_psp = cpu->dx;
            vm->jft_active = true;
            dos_refresh_external_jft(vm);
            dos_sync_system_variables(vm);
        }
        break;
    }

    /* AH=56h: Rename a file or directory without replacement. */
    case 0x56: {
        char from[128], to[128];
        char os_from[128] = {0}, os_to[128] = {0};
        char actual_from[128], actual_to[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, from, sizeof(from));
        dos_read_asciiz(vm, cpu->es, cpu->di, to, sizeof(to));
        int error = dos_path_to_osfs(from, vm->current_drive,
                                     vm->current_dir, os_from,
                                     sizeof(os_from));
        if (!error)
            error = dos_path_to_osfs(to, vm->current_drive,
                                     vm->current_dir, os_to,
                                     sizeof(os_to));
        if (!error && os_from[0] && os_to[0])
            error = dos_resolve_path(vm, os_from, false, actual_from,
                                     sizeof(actual_from));
        if (!error && os_from[0] && os_to[0])
            error = dos_resolve_path(vm, os_to, true, actual_to,
                                     sizeof(actual_to));
        if (!os_from[0] || !os_to[0]) error = DOS_ERR_PATH_NOT_FOUND;
        if (!error && (dos_device_from_path(actual_from) != DOS_DEVICE_NONE ||
                       dos_device_from_path(actual_to) != DOS_DEVICE_NONE))
            error = DOS_ERR_ACCESS_DENIED;
        if (!error && osfs2_rename(actual_from, actual_to, false) < 0)
            error = DOS_ERR_ACCESS_DENIED;
        if (error) {
            cpu->flags |= FLAG_CF;
            cpu->ax = (uint16_t)error;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* AH=57h: Get/Set file last-write time. */
    case 0x57: {
        if (cpu->al > 1U) {
            cpu->ax = DOS_ERR_INVALID_FUNCTION;
            cpu->flags |= FLAG_CF;
            break;
        }

        dos_sft_entry_t *entry = dos_handle_sft(vm, cpu->bx);
        if (!entry || entry->is_device || !entry->osfs_file) {
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            cpu->flags |= FLAG_CF;
            break;
        }

        if (cpu->al == 0U) {
            osfs_file_times_t times;
            if (osfs2_file_get_times(entry->osfs_file, &times) < 0 ||
                !dos_pack_datetime(times.modified, &cpu->dx, &cpu->cx)) {
                cpu->ax = DOS_ERR_INVALID_DATA;
                cpu->flags |= FLAG_CF;
                break;
            }
        } else {
            uint64_t modified;
            if (!dos_unpack_datetime(cpu->dx, cpu->cx, &modified)) {
                cpu->ax = DOS_ERR_INVALID_DATA;
                cpu->flags |= FLAG_CF;
                break;
            }
            osfs_file_times_t times = {0};
            times.modified = modified;
            if (osfs2_file_set_times(entry->osfs_file,
                                     OSFS_FILE_TIME_MODIFIED,
                                     &times) < 0) {
                cpu->ax = DOS_ERR_ACCESS_DENIED;
                cpu->flags |= FLAG_CF;
                break;
            }
        }
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* ── AH=58h: Get/Set memory allocation strategy ───────────────── */
    case 0x58:
        switch (cpu->al) {
        case 0x00:
            cpu->ax = vm->allocation_strategy;
            cpu->flags &= ~FLAG_CF;
            break;
        case 0x01:
            if (!dos_allocation_strategy_valid(cpu->bl)) {
                cpu->ax = DOS_ERR_INVALID_FUNCTION;
                cpu->flags |= FLAG_CF;
                break;
            }
            vm->allocation_strategy = cpu->bl;
            dos_sync_system_variables(vm);
            cpu->flags &= ~FLAG_CF;
            break;
        case 0x02:
            cpu->al = vm->uppermem_link;
            cpu->flags &= ~FLAG_CF;
            break;
        case 0x03:
        default:
            /* No UMB root exists yet, so link/unlink cannot succeed. */
            cpu->ax = DOS_ERR_INVALID_FUNCTION;
            cpu->flags |= FLAG_CF;
            break;
        }
        break;

    /* AH=59h: Return the most recent extended error classification. */
    case 0x59:
        cpu->ax = vm->extended_error;
        cpu->bl = vm->extended_error_action;
        cpu->bh = vm->extended_error_class;
        cpu->ch = vm->extended_error_locus;
        cpu->es = vm->extended_error_segment;
        cpu->di = vm->extended_error_offset;
        cpu->flags &= ~FLAG_CF;
        break;

    /* AH=5Ah: Create a uniquely named file in the supplied directory. */
    case 0x5A: {
        uint16_t handle = 0;
        int error = dos_create_temp_file(vm, cpu->ds, cpu->dx,
                                         cpu->cx, &handle);
        if (error) {
            cpu->ax = (uint16_t)error;
            cpu->flags |= FLAG_CF;
        } else {
            cpu->ax = handle;
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* AH=5Bh: Create a file only if its directory entry is new. */
    case 0x5B: {
        char path[128];
        dos_read_asciiz(vm, cpu->ds, cpu->dx, path, sizeof(path));
        uint16_t handle = 0, open_result = 0;
        int error = dos_open_path(vm, path, DOS_ACCESS_READ_WRITE,
                                  cpu->cx,
                                  DOS_OPEN_EXISTING_FAIL, true,
                                  &handle, &open_result);
        if (error) {
            cpu->ax = (uint16_t)error;
            cpu->flags |= FLAG_CF;
        } else {
            cpu->ax = handle;
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* AH=60h: Canonicalize a path into a fully qualified DOS name. */
    case 0x60: {
        char source[DOS_TRUENAME_CAPACITY];
        char canonical[DOS_TRUENAME_CAPACITY];
        uint32_t output_size = 0;
        uint16_t destination_offset = cpu->di;
        int error = dos_truename_read_source(
            vm, cpu->ds, cpu->si, source, sizeof(source));
        if (!error)
            error = dos_truename_canonicalize(
                source, vm->current_drive, vm->current_dir, canonical,
                sizeof(canonical), &output_size);
        if (!error)
            error = dos_truename_write_result(
                vm, cpu->es, destination_offset, canonical, output_size);
        if (error) {
            cpu->ax = (uint16_t)error;
            cpu->flags |= FLAG_CF;
        } else {
            cpu->ax = 0;
            cpu->di = (uint16_t)(destination_offset + output_size);
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* AH=67h: Set the current PSP's handle-table length. */
    case 0x67: {
        int error = dos_resize_jft(vm, cpu->bx);
        if (error) {
            cpu->ax = (uint16_t)error;
            cpu->flags |= FLAG_CF;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* AH=68h/6Ah: Commit a file's completed writes to stable storage. */
    case 0x68:
    case 0x6A: {
        dos_sft_entry_t *entry = dos_handle_sft(vm, cpu->bx);
        if (!entry) {
            cpu->ax = DOS_ERR_INVALID_HANDLE;
            cpu->flags |= FLAG_CF;
        } else if (!entry->is_device && disk_flush() < 0) {
            cpu->ax = DOS_ERR_ACCESS_DENIED;
            cpu->flags |= FLAG_CF;
        } else {
            cpu->flags &= ~FLAG_CF;
        }
        break;
    }

    /* ── Default: unhandled ─────────────────────────────────────── */
    default:
#if DOS_DIAGNOSTICS
        serial_puts("[DOS] Unhandled INT 21h AH=");
        serial_puthex(ah, 2);
        serial_puts("\n");
#endif
        /* DOS reports unsupported services as invalid functions. */
        cpu->flags |= FLAG_CF;
        cpu->ax = DOS_ERR_INVALID_FUNCTION;
        dos_record_extended_error(vm, ah, cpu->ax);
        break;
    }

    if ((cpu->flags & FLAG_CF) && dos_int21_reports_carry_error(ah))
        dos_record_extended_error(vm, ah, cpu->ax);

    vm->indos_count = previous_indos;
    dos_publish_indos(vm);
}

static void dos_selftest_write_asciiz(dos_vm_t *vm, uint16_t segment,
                                      const char *text)
{
    uint32_t address = dos_linear(segment, 0);
    uint32_t offset = 0;
    do {
        dos_mem_write8(vm, address + offset, (uint8_t)text[offset]);
    } while (text[offset++]);
}

static bool dos_selftest_build_psp(dos_vm_t *vm, uint16_t segment,
                                   bool inherit_handles)
{
    if (!vm || !vm->mem || !segment) return false;
    uint32_t address = (uint32_t)segment << 4;
    if (address > vm->total_mem_size ||
        sizeof(dos_psp_t) > vm->total_mem_size - address)
        return false;

    uint8_t inherited[DOS_PSP_JFT_ENTRIES];
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
        inherited[i] = DOS_SFT_INVALID;
        if (!inherit_handles) continue;
        if (!vm->jft_active) {
            inherited[i] = vm->bootstrap_jft[i].sft_index;
        } else if (!dos_jft_read(vm, (uint16_t)i, &inherited[i])) {
            return false;
        }
    }

    for (uint32_t i = 0; i < sizeof(dos_psp_t); i++)
        dos_mem_write8(vm, address + i, 0);
    dos_psp_t *psp = (dos_psp_t *)(vm->mem + address);
    psp->int20 = 0x20CDu;
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
        psp->jft[i] = inherited[i];
    psp->jft_size = DOS_PSP_JFT_ENTRIES;
    psp->jft_ptr = ((uint32_t)segment << 16) |
                   __builtin_offsetof(dos_psp_t, jft);
    return true;
}

static bool dos_selftest_psp_system_fields(const dos_psp_t *psp,
                                           uint16_t segment,
                                           uint16_t mem_top,
                                           uint32_t int22,
                                           uint32_t int23,
                                           uint32_t int24)
{
    if (!psp) return false;
    uint16_t cpm_offset = (uint16_t)psp->dos_call[1] |
                          ((uint16_t)psp->dos_call[2] << 8);
    uint16_t cpm_segment = (uint16_t)psp->dos_call[3] |
                           ((uint16_t)psp->dos_call[4] << 8);
    uint32_t cpm_linear = (((uint32_t)cpm_segment << 4) + cpm_offset) &
                          0x000FFFFFu;
    return psp->int20 == 0x20CDu && psp->mem_top == mem_top &&
           psp->dos_call[0] == 0x9Au && cpm_linear == 0x000000C0u &&
           psp->old_int22 == int22 && psp->old_int23 == int23 &&
           psp->old_int24 == int24 &&
           psp->jft_size == DOS_PSP_JFT_ENTRIES &&
           (uint16_t)psp->jft_ptr ==
               __builtin_offsetof(dos_psp_t, jft) &&
           (uint16_t)(psp->jft_ptr >> 16) == segment &&
           psp->reserved2[0] == 0xFFu &&
           psp->reserved2[1] == 0xFFu &&
           psp->reserved2[2] == 0xFFu &&
           psp->reserved2[3] == 0xFFu &&
           psp->dispatch[0] == 0xCDu &&
           psp->dispatch[1] == 0x21u &&
           psp->dispatch[2] == 0xCBu;
}

static int dos_psp_creation_selftest(void)
{
    enum {
        SOURCE_PSP = 0x1200u,
        COPIED_PSP = 0x2200u,
        CHILD_PSP = 0x2400u,
        SOURCE_MEM_TOP = 0x1800u,
        CHILD_MEM_TOP = 0x2A00u
    };
    const uint32_t int22 = 0xA2221111u;
    const uint32_t int23 = 0xA3332222u;
    const uint32_t int24 = 0xA4443333u;
    const uint64_t pages = (DOS_CONV_TOP + 4095u) / 4096u;
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(pages);
    if (!memory) return 1;
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;

    dos_vm_t vm = {0};
    cpu8086_state_t cpu = {0};
    vm.cpu = &cpu;
    vm.mem = memory;
    vm.total_mem_size = DOS_CONV_TOP;
    vm.current_drive = 2;
    cpu8086_init(&cpu, &vm);
    dos_mem_init(&vm);
    vm.current_psp = SOURCE_PSP;
    dos_api_init(&vm);

    int failures = 0;
    bool ready = dos_selftest_build_psp(&vm, SOURCE_PSP, true);
    if (!ready) {
        failures++;
    } else {
        vm.jft_active = true;
    }

    dos_psp_t *source = NULL;
    if (ready && !dos_psp_location(&vm, SOURCE_PSP, &source, NULL)) {
        failures++;
        ready = false;
    }
    if (ready) {
        source->mem_top = SOURCE_MEM_TOP;
        source->parent_psp = 0x0F00u;
        source->env_seg = 0x4321u;
        source->old_int22 = 0x01020304u;
        source->old_int23 = 0x05060708u;
        source->old_int24 = 0x090A0B0Cu;
        source->reserved2[0] = 0x12u;
        source->fcb1[0] = 0x34u;
        source->fcb2[0] = 0x56u;
        source->cmd_len = 3u;
        source->cmd_tail[0] = ' ';
        source->cmd_tail[1] = 'X';
        source->cmd_tail[2] = 'Y';
        source->cmd_tail[3] = 0x0Du;

        dos_mem_write32(&vm, 0x22u * 4u, int22);
        dos_mem_write32(&vm, 0x23u * 4u, int23);
        dos_mem_write32(&vm, 0x24u * 4u, int24);

        cpu.ax = 0x6700u;
        cpu.bx = 32u;
        cpu.flags |= FLAG_CF;
        dos_int21_dispatch(&vm);
        if ((cpu.flags & FLAG_CF) || dos_clone_handle(&vm, 1, 4, true) < 0) {
            failures++;
            ready = false;
        }
    }

    uint8_t source_handles[DOS_PSP_JFT_ENTRIES] = {0};
    uint16_t refs_before[DOS_MAX_SFT_ENTRIES] = {0};
    uint16_t source_external_segment = 0;
    if (ready) {
        uint8_t no_inherit_index = DOS_SFT_INVALID;
        if (!dos_jft_read(&vm, 3u, &no_inherit_index)) {
            failures++;
            ready = false;
        } else {
            dos_sft_entry_t *entry = dos_sft_from_index(
                &vm, no_inherit_index);
            if (!entry) {
                failures++;
                ready = false;
            } else {
                entry->open_mode |= DOS_OPEN_NO_INHERIT;
            }
        }
    }
    if (ready) {
        source_external_segment = (uint16_t)(source->jft_ptr >> 16);
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
            if (!dos_jft_read(&vm, (uint16_t)i, &source_handles[i])) {
                failures++;
                ready = false;
                break;
            }
        }
        for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++)
            refs_before[i] = vm.sft[i].ref_count;
    }

    if (ready) {
        cpu.cs = SOURCE_PSP;
        cpu.dx = COPIED_PSP;
        cpu.ax = 0x2600u;
        dos_int21_dispatch(&vm);

        dos_psp_t *copy = NULL;
        if (!dos_psp_location(&vm, COPIED_PSP, &copy, NULL) ||
            vm.current_psp != SOURCE_PSP ||
            vm.jft_external_segment != source_external_segment ||
            vm.jft_external_psp != SOURCE_PSP ||
            !dos_selftest_psp_system_fields(copy, COPIED_PSP,
                                            SOURCE_MEM_TOP,
                                            int22, int23, int24) ||
            copy->parent_psp != 0x0F00u ||
            copy->env_seg != 0x4321u || copy->fcb1[0] != 0x34u ||
            copy->fcb2[0] != 0x56u || copy->cmd_len != 3u ||
            copy->cmd_tail[1] != 'X') {
            failures++;
        } else {
            for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
                if (copy->jft[i] != source_handles[i]) {
                    failures++;
                    break;
                }
            }
        }
        for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
            if (vm.sft[i].ref_count != refs_before[i]) {
                failures++;
                break;
            }
        }
    }

    bool child_created = false;
    if (ready) {
        uint8_t expected_handles[DOS_PSP_JFT_ENTRIES];
        uint16_t expected_refs[DOS_MAX_SFT_ENTRIES];
        for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++)
            expected_refs[i] = refs_before[i];
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
            uint8_t index = source_handles[i];
            expected_handles[i] = DOS_SFT_INVALID;
            dos_sft_entry_t *entry = dos_sft_from_index(&vm, index);
            if (!entry || (entry->open_mode & DOS_OPEN_NO_INHERIT) ||
                expected_refs[index] == 0xFFFFu)
                continue;
            expected_handles[i] = index;
            expected_refs[index]++;
        }

        vm.dta_seg = 0x3333u;
        vm.dta_off = 0x0044u;
        cpu.dx = CHILD_PSP;
        cpu.si = CHILD_MEM_TOP;
        cpu.ax = 0x5500u;
        dos_int21_dispatch(&vm);
        child_created = vm.current_psp == CHILD_PSP;

        dos_psp_t *child = NULL;
        uint32_t sysvars = dos_linear(DOS_SYSVARS_SEG, DOS_SYSVARS_OFF);
        if (!child_created ||
            !dos_psp_location(&vm, CHILD_PSP, &child, NULL) ||
            vm.jft_external_segment != 0u ||
            vm.jft_external_psp != 0u ||
            vm.dta_seg != 0x3333u || vm.dta_off != 0x0044u ||
            dos_mem_read16(&vm, sysvars + 0x3Du) != CHILD_PSP ||
            !dos_selftest_psp_system_fields(child, CHILD_PSP,
                                            CHILD_MEM_TOP,
                                            int22, int23, int24) ||
            child->parent_psp != SOURCE_PSP ||
            child->env_seg != source->env_seg ||
            child->fcb1[0] != source->fcb1[0] ||
            child->fcb2[0] != source->fcb2[0] ||
            child->cmd_len != source->cmd_len ||
            child->cmd_tail[1] != source->cmd_tail[1]) {
            failures++;
        } else {
            for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
                if (child->jft[i] != expected_handles[i]) {
                    failures++;
                    break;
                }
            }
            for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
                if (vm.sft[i].ref_count != expected_refs[i]) {
                    failures++;
                    break;
                }
            }
        }
    }

    if (child_created) {
        for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++)
            dos_release_file_handle(&vm, (int)i);
    }
    vm.current_psp = SOURCE_PSP;
    vm.jft_active = true;
    dos_refresh_external_jft(&vm);
    dos_sync_system_variables(&vm);
    if (ready && child_created) {
        for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
            if (vm.sft[i].ref_count != refs_before[i]) {
                failures++;
                break;
            }
        }
    }

    dos_api_close_all(&vm);
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        if (vm.sft[i].used || vm.sft[i].ref_count) {
            failures++;
            break;
        }
    }
    dos_host_free_pages(memory, pages);
    return failures;
}

static bool dos_selftest_read_dta_name(dos_vm_t *vm, uint16_t segment,
                                       char name[13])
{
    uint32_t address = dos_linear(segment, 0) + 30U;
    for (uint32_t i = 0; i < 13U; i++) {
        name[i] = (char)dos_mem_read8(vm, address + i);
        if (!name[i]) return i != 0;
    }
    name[12] = 0;
    return false;
}

static int dos_find_api_contract_selftest(dos_vm_t *vm,
                                          cpu8086_state_t *cpu)
{
    enum {
        PATH_SEGMENT = 0x0200,
        TARGET_SEGMENT = 0x0210,
        DTA_SEGMENT = 0x0300
    };
    static const char *const long_paths[] = {
        "Qzx Find Contract Alpha.data",
        "Qzx Find Contract Beta.data"
    };
    static const uint32_t expected_sizes[] = {7U, 9U};
    static const char no_extension_path[] = "QZXNOEXT";
    static const char renamed_path[] = "QZXRENAM.TMP";
    static const char directory_path[] = "QZXDIR";
    static const char renamed_directory_path[] = "QZXREN";
    static const char child_path[] = "QZXREN/CHILD.TXT";
    static const char search_pattern[] = "QZX~????.DAT";
    int failures = 0;
    bool ready = true;

    for (uint32_t i = 0; i < 2U; i++)
        (void)osfs2_delete(long_paths[i]);
    (void)osfs2_delete(no_extension_path);
    (void)osfs2_delete(renamed_path);
    if (osfs3_is_mounted()) {
        (void)osfs2_delete(child_path);
        (void)osfs3_rmdir(renamed_directory_path);
        (void)osfs3_rmdir(directory_path);
    }

    uint64_t modified = 0;
    if (!dos_unpack_datetime(0x5021U, 0x645CU, &modified))
        ready = false;
    for (uint32_t i = 0; i < 2U; i++) {
        void *file = osfs2_create(long_paths[i], expected_sizes[i]);
        osfs_file_times_t times = {0};
        times.modified = modified;
        if (!file ||
            osfs2_file_set_times(file, OSFS_FILE_TIME_MODIFIED, &times) < 0)
            ready = false;
    }

    cpu->ds = DTA_SEGMENT;
    cpu->dx = 0;
    cpu->ax = 0x1A00U;
    dos_int21_dispatch(vm);

    char aliases[2][13] = {{0}};
    char resolved_paths[2][128] = {{0}};
    uint8_t seen = 0;
    if (!ready) {
        failures++;
    } else {
        dos_selftest_write_asciiz(vm, PATH_SEGMENT, search_pattern);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->cx = 0;
        cpu->ax = 0x4E00U;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);

        for (uint32_t result = 0; result < 2U; result++) {
            if (result) {
                cpu->ax = 0x4F00U;
                cpu->flags |= FLAG_CF;
                dos_int21_dispatch(vm);
            }
            if ((cpu->flags & FLAG_CF) ||
                !dos_selftest_read_dta_name(vm, DTA_SEGMENT,
                                            aliases[result])) {
                failures++;
                break;
            }

            uint32_t dta = dos_linear(DTA_SEGMENT, 0);
            char resolved[128];
            int resolve_error = dos_resolve_path(
                vm, aliases[result], false, resolved, sizeof(resolved));
            int matched = -1;
            for (uint32_t i = 0; i < 2U; i++)
                if (dos_string_equal(resolved, long_paths[i]))
                    matched = (int)i;
            if (resolve_error || matched < 0 ||
                (seen & (uint8_t)(1U << matched)) ||
                dos_mem_read8(vm, dta + 21U) != 0x20U ||
                dos_mem_read16(vm, dta + 22U) != 0x645CU ||
                dos_mem_read16(vm, dta + 24U) != 0x5021U ||
                dos_mem_read32(vm, dta + 26U) !=
                    expected_sizes[matched < 0 ? 0 : (uint32_t)matched]) {
                failures++;
            } else {
                seen |= (uint8_t)(1U << matched);
                uint32_t i = 0;
                while (resolved[i] && i + 1U < sizeof(resolved_paths[0])) {
                    resolved_paths[result][i] = resolved[i];
                    i++;
                }
                resolved_paths[result][i] = 0;
            }
        }
        if (seen != 0x03U ||
            dos_string_equal(aliases[0], aliases[1]))
            failures++;

        cpu->ax = 0x4F00U;
        cpu->flags &= ~FLAG_CF;
        dos_int21_dispatch(vm);
        if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_NO_MORE_FILES)
            failures++;

        if (aliases[0][0]) {
            dos_selftest_write_asciiz(vm, PATH_SEGMENT, aliases[0]);
            cpu->ds = PATH_SEGMENT;
            cpu->dx = 0;
            cpu->ax = 0x3D00U;
            cpu->flags |= FLAG_CF;
            dos_int21_dispatch(vm);
            if (cpu->flags & FLAG_CF) {
                failures++;
            } else {
                uint16_t handle = cpu->ax;
                cpu->ax = 0x3E00U;
                cpu->bx = handle;
                dos_int21_dispatch(vm);
                if (cpu->flags & FLAG_CF) failures++;
            }
        }

        for (uint32_t i = 0; i < 2U; i++) {
            if (!aliases[i][0]) continue;
            dos_selftest_write_asciiz(vm, PATH_SEGMENT, aliases[i]);
            cpu->ds = PATH_SEGMENT;
            cpu->dx = 0;
            cpu->ax = 0x4100U;
            cpu->flags |= FLAG_CF;
            dos_int21_dispatch(vm);
            if ((cpu->flags & FLAG_CF) || !resolved_paths[i][0] ||
                osfs2_find_ci(resolved_paths[i]))
                failures++;
        }
    }

    void *no_extension = osfs2_create(no_extension_path, 1U);
    if (!no_extension) {
        failures++;
    } else {
        dos_selftest_write_asciiz(vm, PATH_SEGMENT, "QZXNOEXT.*");
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->cx = 0;
        cpu->ax = 0x4E00U;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        char result[13];
        if ((cpu->flags & FLAG_CF) ||
            !dos_selftest_read_dta_name(vm, DTA_SEGMENT, result) ||
            !dos_string_equal(result, no_extension_path))
            failures++;

        dos_selftest_write_asciiz(vm, PATH_SEGMENT, no_extension_path);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->ax = 0x4301U;
        cpu->cx = OSFS_DOS_ATTR_READ_ONLY | OSFS_DOS_ATTR_HIDDEN |
                  OSFS_DOS_ATTR_SYSTEM;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if (cpu->flags & FLAG_CF) failures++;

        cpu->ax = 0x4300U;
        cpu->cx = 0;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) ||
            cpu->cx != (OSFS_DOS_ATTR_READ_ONLY | OSFS_DOS_ATTR_HIDDEN |
                        OSFS_DOS_ATTR_SYSTEM))
            failures++;

        cpu->ax = 0x3D01U;
        cpu->flags &= ~FLAG_CF;
        dos_int21_dispatch(vm);
        if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_ACCESS_DENIED)
            failures++;

        cpu->ax = 0x4100U;
        cpu->flags &= ~FLAG_CF;
        dos_int21_dispatch(vm);
        if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_ACCESS_DENIED ||
            !osfs2_find_ci(no_extension_path))
            failures++;

        dos_selftest_write_asciiz(vm, PATH_SEGMENT, no_extension_path);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->cx = 0;
        cpu->ax = 0x4E00U;
        cpu->flags &= ~FLAG_CF;
        dos_int21_dispatch(vm);
        if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_NO_MORE_FILES)
            failures++;

        cpu->cx = OSFS_DOS_ATTR_HIDDEN | OSFS_DOS_ATTR_SYSTEM;
        cpu->ax = 0x4E00U;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) ||
            dos_mem_read8(vm, dos_linear(DTA_SEGMENT, 0) + 21U) !=
                (OSFS_DOS_ATTR_READ_ONLY | OSFS_DOS_ATTR_HIDDEN |
                 OSFS_DOS_ATTR_SYSTEM))
            failures++;

        dos_selftest_write_asciiz(vm, PATH_SEGMENT, no_extension_path);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->ax = 0x4301U;
        cpu->cx = OSFS_DOS_ATTR_ARCHIVE;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        cpu->ax = 0x4300U;
        cpu->cx = 0;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) ||
            cpu->cx != OSFS_DOS_ATTR_ARCHIVE)
            failures++;

        dos_selftest_write_asciiz(vm, PATH_SEGMENT, no_extension_path);
        dos_selftest_write_asciiz(vm, TARGET_SEGMENT, renamed_path);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->es = TARGET_SEGMENT;
        cpu->di = 0;
        cpu->ax = 0x5600U;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) || osfs2_find_ci(no_extension_path) ||
            !osfs2_find_ci(renamed_path))
            failures++;
    }

    dos_selftest_write_asciiz(vm, PATH_SEGMENT, directory_path);
    cpu->ds = PATH_SEGMENT;
    cpu->dx = 0;
    cpu->ax = 0x3900U;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    if (!osfs3_is_mounted()) {
        if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_ACCESS_DENIED)
            failures++;
    } else {
        if (cpu->flags & FLAG_CF) {
            failures++;
        } else {
            cpu->ax = 0x4300U;
            cpu->flags |= FLAG_CF;
            dos_int21_dispatch(vm);
            if ((cpu->flags & FLAG_CF) || cpu->cx != 0x10U)
                failures++;
        }

        dos_selftest_write_asciiz(vm, PATH_SEGMENT, directory_path);
        dos_selftest_write_asciiz(vm, TARGET_SEGMENT,
                                  renamed_directory_path);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->es = TARGET_SEGMENT;
        cpu->di = 0;
        cpu->ax = 0x5600U;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) ||
            osfs3_directory_exists_ci(directory_path) ||
            !osfs3_directory_exists_ci(renamed_directory_path))
            failures++;

        void *child = osfs2_create(child_path, 1U);
        dos_selftest_write_asciiz(vm, PATH_SEGMENT,
                                  renamed_directory_path);
        cpu->ds = PATH_SEGMENT;
        cpu->dx = 0;
        cpu->ax = 0x3A00U;
        cpu->flags &= ~FLAG_CF;
        dos_int21_dispatch(vm);
        if (!child || !(cpu->flags & FLAG_CF) ||
            cpu->ax != DOS_ERR_ACCESS_DENIED)
            failures++;
        (void)osfs2_delete(child_path);

        cpu->ax = 0x3A00U;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) ||
            osfs3_directory_exists_ci(renamed_directory_path))
            failures++;
    }

    for (uint32_t i = 0; i < 2U; i++)
        (void)osfs2_delete(long_paths[i]);
    (void)osfs2_delete(no_extension_path);
    (void)osfs2_delete(renamed_path);
    if (osfs3_is_mounted()) {
        (void)osfs2_delete(child_path);
        (void)osfs3_rmdir(renamed_directory_path);
        (void)osfs3_rmdir(directory_path);
    }
    return failures;
}

static int dos_extended_open_contract_selftest(dos_vm_t *vm,
                                                cpu8086_state_t *cpu)
{
    enum { PATH_SEGMENT = 0x0240 };
    static const char path[] = "QZXOPEN6.TMP";
    const uint8_t protected_attributes = OSFS_DOS_ATTR_HIDDEN |
                                         OSFS_DOS_ATTR_SYSTEM;
    const uint8_t create_attributes = protected_attributes |
                                      OSFS_DOS_ATTR_READ_ONLY;
    const uint16_t deny_none_rw = (DOS_SHARE_DENY_NONE << 4) |
                                  DOS_ACCESS_READ_WRITE;
    const uint16_t noinherit_read = DOS_OPEN_NO_INHERIT |
                                    DOS_OPEN_LARGE_FILE |
                                    DOS_OPEN_NO_CRITICAL |
                                    DOS_OPEN_SYNC |
                                    (DOS_SHARE_DENY_NONE << 4) |
                                    DOS_ACCESS_READ;
    int failures = 0;
    uint16_t original_psp = vm->current_psp;

    void *file = osfs2_find_ci(path);
    if (file) {
        (void)osfs2_file_set_dos_attributes(file, 0);
        (void)osfs2_delete(path);
    }
    dos_selftest_write_asciiz(vm, PATH_SEGMENT, path);
    cpu->ds = PATH_SEGMENT;
    cpu->si = 0;

    cpu->ax = 0x6C00U;
    cpu->bx = deny_none_rw;
    cpu->cx = create_attributes;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_FILE_NOT_FOUND)
        failures++;

    cpu->ax = 0x6C00U;
    cpu->bx = deny_none_rw;
    cpu->cx = create_attributes;
    cpu->dx = 0x0010U;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    uint16_t handle = cpu->ax;
    dos_sft_entry_t *entry = dos_handle_sft(vm, handle);
    uint8_t stored_attributes = 0;
    file = osfs2_find_ci(path);
    if ((cpu->flags & FLAG_CF) || cpu->cx != DOS_OPEN_RESULT_CREATED ||
        !entry || entry->open_mode != deny_none_rw ||
        entry->owner_psp != original_psp || !file ||
        osfs2_file_get_dos_attributes(file, &stored_attributes) < 0 ||
        stored_attributes !=
            (create_attributes | OSFS_DOS_ATTR_ARCHIVE))
        failures++;
    if (!(cpu->flags & FLAG_CF) && entry) {
        cpu->ax = 0x4000U;
        cpu->bx = handle;
        cpu->cx = 1;
        cpu->dx = 0;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if ((cpu->flags & FLAG_CF) || cpu->ax != 1u) failures++;

        cpu->ax = 0x3E00U;
        cpu->bx = handle;
        cpu->flags |= FLAG_CF;
        dos_int21_dispatch(vm);
        if (cpu->flags & FLAG_CF) failures++;
    }

    cpu->ax = 0x6C00U;
    cpu->bx = deny_none_rw;
    cpu->cx = create_attributes;
    cpu->dx = 0x0010U;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_FILE_EXISTS)
        failures++;

    cpu->ax = 0x6C00U;
    cpu->bx = noinherit_read;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    handle = cpu->ax;
    entry = dos_handle_sft(vm, handle);
    if ((cpu->flags & FLAG_CF) || cpu->cx != DOS_OPEN_RESULT_OPENED ||
        !entry || entry->open_mode != noinherit_read ||
        entry->owner_psp != original_psp)
        failures++;
    if (!(cpu->flags & FLAG_CF) && entry) {
        cpu->ax = 0x3E00U;
        cpu->bx = handle;
        dos_int21_dispatch(vm);
    }

    const uint16_t no_atime_read = (DOS_SHARE_DENY_NONE << 4) |
                                    DOS_ACCESS_READ_NO_ATIME;
    cpu->ax = 0x6C00U;
    cpu->bx = no_atime_read;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    handle = cpu->ax;
    entry = dos_handle_sft(vm, handle);
    if ((cpu->flags & FLAG_CF) || !entry ||
        entry->open_mode != no_atime_read ||
        dos_open_access(entry->open_mode) != DOS_ACCESS_READ)
        failures++;
    if (!(cpu->flags & FLAG_CF) && entry)
        dos_release_file_handle(vm, handle);

    cpu->ax = 0x6C00U;
    cpu->bx = deny_none_rw;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_ACCESS_DENIED)
        failures++;

    file = osfs2_find_ci(path);
    if (!file || osfs2_file_set_dos_attributes(
            file, protected_attributes | OSFS_DOS_ATTR_ARCHIVE) < 0)
        failures++;

    cpu->ax = 0x6C00U;
    cpu->bx = deny_none_rw;
    cpu->cx = protected_attributes;
    cpu->dx = DOS_OPEN_EXISTING_REPLACE;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    handle = cpu->ax;
    entry = dos_handle_sft(vm, handle);
    file = osfs2_find_ci(path);
    stored_attributes = 0;
    if ((cpu->flags & FLAG_CF) || cpu->cx != DOS_OPEN_RESULT_REPLACED ||
        !entry || !file || osfs2_file_size(file) != 0 ||
        osfs2_file_get_dos_attributes(file, &stored_attributes) < 0 ||
        stored_attributes !=
            (protected_attributes | OSFS_DOS_ATTR_ARCHIVE))
        failures++;
    if (!(cpu->flags & FLAG_CF) && entry) {
        cpu->ax = 0x3E00U;
        cpu->bx = handle;
        dos_int21_dispatch(vm);
    }

    cpu->ax = 0x6C00U;
    cpu->bx = (DOS_SHARE_DENY_WRITE << 4) | DOS_ACCESS_READ;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    uint16_t shared_handle = cpu->ax;
    bool shared_opened = (cpu->flags & FLAG_CF) == 0;
    if (!shared_opened) failures++;

    cpu->ax = 0x6C00U;
    cpu->bx = (DOS_SHARE_DENY_NONE << 4) | DOS_ACCESS_WRITE;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) ||
        cpu->ax != DOS_ERR_SHARING_VIOLATION)
        failures++;
    if (shared_opened && dos_handle_sft(vm, shared_handle))
        dos_release_file_handle(vm, shared_handle);

    cpu->ax = 0x6C00U;
    cpu->bx = DOS_ACCESS_READ_WRITE;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    uint16_t compatibility_handle = cpu->ax;
    bool compatibility_opened = (cpu->flags & FLAG_CF) == 0;
    if (!compatibility_opened) failures++;

    cpu->ax = 0x6C00U;
    cpu->bx = DOS_ACCESS_READ;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags |= FLAG_CF;
    dos_int21_dispatch(vm);
    uint16_t same_psp_handle = cpu->ax;
    if (cpu->flags & FLAG_CF) {
        failures++;
    } else {
        dos_release_file_handle(vm, same_psp_handle);
    }

    uint16_t alternate_psp = (uint16_t)(original_psp + 0x20u);
    if (!dos_selftest_build_psp(vm, alternate_psp, false)) {
        failures++;
    } else {
        vm->current_psp = alternate_psp;
        cpu->ax = 0x6C00U;
        cpu->bx = DOS_ACCESS_READ;
        cpu->cx = 0;
        cpu->dx = DOS_OPEN_EXISTING_OPEN;
        cpu->flags &= ~FLAG_CF;
        dos_int21_dispatch(vm);
        if (!(cpu->flags & FLAG_CF) ||
            cpu->ax != DOS_ERR_SHARING_VIOLATION)
            failures++;
    }
    vm->current_psp = original_psp;
    dos_sync_system_variables(vm);
    if (compatibility_opened && dos_handle_sft(vm, compatibility_handle))
        dos_release_file_handle(vm, compatibility_handle);

    cpu->ax = 0x6C00U;
    cpu->bx = deny_none_rw;
    cpu->cx = 0;
    cpu->dx = 0x0003U;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_INVALID_FUNCTION)
        failures++;

    cpu->ax = 0x6C00U;
    cpu->bx = (5u << 4) | DOS_ACCESS_READ;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_INVALID_ACCESS)
        failures++;

    cpu->ax = 0x6C01U;
    cpu->bx = deny_none_rw;
    cpu->cx = 0;
    cpu->dx = DOS_OPEN_EXISTING_OPEN;
    cpu->flags &= ~FLAG_CF;
    dos_int21_dispatch(vm);
    if (!(cpu->flags & FLAG_CF) || cpu->ax != DOS_ERR_INVALID_FUNCTION)
        failures++;

    file = osfs2_find_ci(path);
    uint16_t handle_count = 0;
    (void)dos_current_jft(vm, NULL, NULL, &handle_count, NULL, NULL);
    for (uint32_t i = 5; i < handle_count; i++) {
        dos_sft_entry_t *open_entry = dos_handle_sft(vm, i);
        if (open_entry && !open_entry->is_device &&
            open_entry->osfs_file == file)
            dos_release_file_handle(vm, i);
    }
    if (file) (void)osfs2_file_set_dos_attributes(file, 0);
    (void)osfs2_delete(path);
    return failures;
}

static int dos_system_variables_selftest(void)
{
    const uint64_t pages = (DOS_CONV_TOP + 4095u) / 4096u;
    uint8_t *memory = (uint8_t *)dos_host_alloc_pages(pages);
    if (!memory) return 1;
    for (uint64_t i = 0; i < pages * 4096u; i++) memory[i] = 0;

    dos_vm_t vm = {0};
    cpu8086_state_t cpu = {0};
    vm.cpu = &cpu;
    vm.mem = memory;
    vm.total_mem_size = DOS_CONV_TOP;
    vm.current_drive = 2;
    cpu8086_init(&cpu, &vm);
    dos_mem_init(&vm);
    vm.current_psp = 0x1234;
    dos_api_init(&vm);

    int failures = 0;
    if (!dos_selftest_build_psp(&vm, vm.current_psp, true)) {
        failures++;
    } else {
        vm.jft_active = true;
    }

    cpu.ax = 0x5900;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 0u)
        failures++;

    cpu.ax = 0x2B00;
    cpu.cx = 2000;
    cpu.dx = 0x021D;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0u || !(cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x2D00;
    cpu.cx = 0x0C22;
    cpu.dx = 0x382A;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0u || (cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x2A5A;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.cx != 2000u || cpu.dh != 2u || cpu.dl != 29u ||
        cpu.al != 2u || !(cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x2C5A;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0u || cpu.ch != 12u || cpu.cl != 34u ||
        (cpu.dh != 56u && cpu.dh != 57u) || cpu.dl > 99u ||
        !(cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x2B00;
    cpu.cx = 2001;
    cpu.dx = 0x021D;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0xFFu || (cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x2B00;
    cpu.cx = 2100;
    cpu.dx = 0x0101;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0xFFu)
        failures++;

    cpu.ax = 0x2D00;
    cpu.cx = 0x1800;
    cpu.dx = 0;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0xFFu || !(cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x2A00;
    dos_int21_dispatch(&vm);
    if (cpu.cx != 2000u || cpu.dh != 2u || cpu.dl != 29u)
        failures++;

    cpu.ax = 0x5200;
    dos_int21_dispatch(&vm);
    uint32_t base = dos_linear(cpu.es, cpu.bx);
    static const char nul_name[8] = {'N', 'U', 'L', ' ', ' ', ' ', ' ', ' '};
    if (cpu.es != DOS_SYSVARS_SEG || cpu.bx != DOS_SYSVARS_OFF ||
        dos_mem_read16(&vm, base - 2u) != vm.first_mcb ||
        dos_mem_read32(&vm, base) != 0xFFFFFFFFu ||
        dos_mem_read16(&vm, base + 0x10u) != 512u ||
        dos_mem_read8(&vm, base + 0x20u) != 0u ||
        dos_mem_read8(&vm, base + 0x21u) != 3u ||
        dos_mem_read16(&vm, base + 0x26u) != 0x8004u ||
        dos_mem_read16(&vm, base + 0x3Du) != vm.current_psp ||
        dos_mem_read8(&vm, base + 0x43u) != 3u ||
        dos_mem_read8(&vm, base + 0x5Eu) != DOS_ALLOC_FIRST_FIT ||
        dos_mem_read8(&vm, base + 0x63u) != 0u ||
        dos_mem_read16(&vm, base + 0x66u) != 0xFFFFu)
        failures++;
    for (unsigned i = 0; i < sizeof(nul_name); i++) {
        if (dos_mem_read8(&vm, base + 0x2Cu + i) !=
            (uint8_t)nul_name[i]) {
            failures++;
            break;
        }
    }

    cpu.ax = 0x5801;
    cpu.bx = DOS_ALLOC_UMB_FIRST | DOS_ALLOC_LAST_FIT;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) ||
        vm.allocation_strategy !=
            (DOS_ALLOC_UMB_FIRST | DOS_ALLOC_LAST_FIT) ||
        dos_mem_read8(&vm, base + 0x5Eu) != vm.allocation_strategy)
        failures++;

    cpu.ax = 0x5800;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax !=
        (DOS_ALLOC_UMB_FIRST | DOS_ALLOC_LAST_FIT))
        failures++;

    cpu.ax = 0x5801;
    cpu.bx = 0x0043;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_FUNCTION ||
        vm.allocation_strategy !=
            (DOS_ALLOC_UMB_FIRST | DOS_ALLOC_LAST_FIT))
        failures++;

    cpu.ax = 0x5900;
    cpu.bx = 0;
    cpu.cx = 0xA55Au;
    cpu.es = 0xFFFFu;
    cpu.di = 0xFFFFu;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_FUNCTION ||
        cpu.bl != DOS_ERROR_ACTION_ABORT ||
        cpu.bh != DOS_ERROR_CLASS_APPLICATION ||
        cpu.ch != DOS_ERROR_LOCUS_MEMORY || cpu.cl != 0x5Au ||
        cpu.es != 0u || cpu.di != 0u)
        failures++;

    cpu.ax = 0x5802;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.al != 0u)
        failures++;

    cpu.ax = 0x5900;
    cpu.bx = 0;
    cpu.cx = 0;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_FUNCTION ||
        cpu.bl != DOS_ERROR_ACTION_ABORT ||
        cpu.bh != DOS_ERROR_CLASS_APPLICATION ||
        cpu.ch != DOS_ERROR_LOCUS_MEMORY)
        failures++;

    cpu.ax = 0x5803;
    cpu.bx = 1;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_FUNCTION)
        failures++;

    uint16_t jft_owner_psp = vm.current_psp;
    uint16_t largest_before_jft = 0;
    (void)dos_mem_alloc(&vm, 0xFFFFu, &largest_before_jft);
    dos_psp_t *active_psp = (dos_psp_t *)(vm.mem +
                                           ((uint32_t)vm.current_psp << 4));
    cpu.ax = 0x6700;
    cpu.bx = 32;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    uint16_t external_jft = (uint16_t)(active_psp->jft_ptr >> 16);
    uint32_t external_address = (uint32_t)external_jft << 4;
    if ((cpu.flags & FLAG_CF) || active_psp->jft_size != 32u ||
        (uint16_t)active_psp->jft_ptr != 0u || !external_jft ||
        vm.jft_external_segment != external_jft ||
        vm.jft_external_psp != vm.current_psp)
        failures++;
    for (uint32_t i = DOS_PSP_JFT_ENTRIES; i < 32u; i++) {
        if (dos_mem_read8(&vm, external_address + i) != DOS_SFT_INVALID) {
            failures++;
            break;
        }
    }

    if (!dos_selftest_build_psp(&vm, 0x2345u, false)) {
        failures++;
    } else {
        cpu.ax = 0x5000;
        cpu.bx = 0x2345u;
        dos_int21_dispatch(&vm);
        if (vm.current_psp != 0x2345u || vm.jft_external_segment != 0u ||
            vm.jft_external_psp != 0u)
            failures++;

        cpu.ax = 0x5000;
        cpu.bx = jft_owner_psp;
        dos_int21_dispatch(&vm);
        if (vm.current_psp != jft_owner_psp ||
            vm.jft_external_segment != external_jft ||
            vm.jft_external_psp != jft_owner_psp)
            failures++;
    }

    cpu.ax = 0x4600;
    cpu.bx = 1;
    cpu.cx = 31;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || dos_handle_sft(&vm, 31) !=
        dos_handle_sft(&vm, 1))
        failures++;

    cpu.ax = 0x6700;
    cpu.bx = DOS_PSP_JFT_ENTRIES;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_TOO_MANY_OPEN_FILES ||
        active_psp->jft_size != 32u)
        failures++;

    cpu.ax = 0x5900;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_TOO_MANY_OPEN_FILES ||
        cpu.bl != DOS_ERROR_ACTION_ABORT ||
        cpu.bh != DOS_ERROR_CLASS_OUT_RESOURCE ||
        cpu.ch != DOS_ERROR_LOCUS_UNKNOWN)
        failures++;

    dos_release_file_handle(&vm, 31);
    cpu.ax = 0x6700;
    cpu.bx = 0;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) ||
        active_psp->jft_size != DOS_PSP_JFT_ENTRIES ||
        (uint16_t)active_psp->jft_ptr !=
            __builtin_offsetof(dos_psp_t, jft) ||
        (uint16_t)(active_psp->jft_ptr >> 16) != vm.current_psp ||
        vm.jft_external_segment != 0u || vm.jft_external_psp != 0u)
        failures++;
    uint16_t largest_after_jft = 0;
    (void)dos_mem_alloc(&vm, 0xFFFFu, &largest_after_jft);
    if (largest_after_jft != largest_before_jft ||
        dos_jft_storage_owned(&vm, jft_owner_psp, external_jft, 0u))
        failures++;

    cpu.ax = 0x6700;
    cpu.bx = 0xFFFFu;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_FUNCTION)
        failures++;

    if (!dos_selftest_build_psp(&vm, 0x2345u, true))
        failures++;
    cpu.ax = 0x5000;
    cpu.bx = 0x2345;
    dos_int21_dispatch(&vm);
    if (vm.current_psp != 0x2345u ||
        dos_mem_read16(&vm, base + 0x3Du) != 0x2345u)
        failures++;

    cpu.ax = 0x3300;
    cpu.dx = 0xFFFF;
    dos_int21_dispatch(&vm);
    if (cpu.dl != 0u || vm.ctrl_break_enabled)
        failures++;

    cpu.ax = 0x3301;
    cpu.dl = 0xFFu;
    dos_int21_dispatch(&vm);
    if (cpu.dl != 1u || !vm.ctrl_break_enabled)
        failures++;

    cpu.ax = 0x3300;
    cpu.dl = 0u;
    dos_int21_dispatch(&vm);
    if (cpu.dl != 1u)
        failures++;

    cpu.ax = 0x3302;
    cpu.dl = 0u;
    dos_int21_dispatch(&vm);
    if (cpu.dl != 1u || vm.ctrl_break_enabled)
        failures++;

    cpu.ax = 0x3305;
    cpu.dl = 0u;
    dos_int21_dispatch(&vm);
    if (cpu.dl != 3u)
        failures++;

    cpu.ax = 0x3306;
    cpu.bx = 0u;
    cpu.dx = 0xFFFFu;
    dos_int21_dispatch(&vm);
    if (cpu.bx != 0x1606u || cpu.dx != 0u)
        failures++;

    cpu.ax = 0x3307;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if (cpu.al != 0xFFu || !(cpu.flags & FLAG_CF))
        failures++;

    cpu.ax = 0x3400;
    dos_int21_dispatch(&vm);
    if (cpu.es != DOS_SYSVARS_SEG || cpu.bx != DOS_INDOS_OFF ||
        vm.indos_count != 0u ||
        dos_mem_read8(&vm, dos_linear(cpu.es, cpu.bx)) != 0u)
        failures++;

    dos_sft_entry_t *stdin_entry = dos_handle_sft(&vm, 0);
    dos_sft_entry_t *stdout_entry = dos_handle_sft(&vm, 1);
    dos_sft_entry_t *stderr_entry = dos_handle_sft(&vm, 2);
    dos_sft_entry_t *aux_entry = dos_handle_sft(&vm, 3);
    dos_sft_entry_t *prn_entry = dos_handle_sft(&vm, 4);
    if (!stdin_entry || !stdin_entry->is_device ||
        stdin_entry->device_kind != DOS_DEVICE_CON ||
        dos_open_access(stdin_entry->open_mode) != DOS_ACCESS_READ ||
        !stdout_entry || !stdout_entry->is_device ||
        stdout_entry->device_kind != DOS_DEVICE_CON ||
        dos_open_access(stdout_entry->open_mode) != DOS_ACCESS_WRITE ||
        stderr_entry != stdout_entry || stdout_entry->ref_count != 2u ||
        !aux_entry || !aux_entry->is_device ||
        aux_entry->device_kind != DOS_DEVICE_AUX ||
        !prn_entry || !prn_entry->is_device ||
        prn_entry->device_kind != DOS_DEVICE_PRN)
        failures++;

    static const char nul_path[] = "C:\\TOOLS\\NUL.TXT";
    cpu.ds = 0x0200;
    cpu.dx = 0;
    uint32_t path_address = dos_linear(cpu.ds, cpu.dx);
    for (unsigned i = 0; i < sizeof(nul_path); i++)
        dos_mem_write8(&vm, path_address + i, (uint8_t)nul_path[i]);

    cpu.ax = 0x3D02;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    uint16_t nul_handle = cpu.ax;
    dos_sft_entry_t *nul_entry = dos_handle_sft(&vm, nul_handle);
    if ((cpu.flags & FLAG_CF) || nul_handle != 5u ||
        !nul_entry || !nul_entry->is_device ||
        nul_entry->device_kind != DOS_DEVICE_NUL ||
        dos_open_access(nul_entry->open_mode) != DOS_ACCESS_READ_WRITE ||
        nul_entry->ref_count != 1u)
        failures++;

    cpu.ax = 0x4400;
    cpu.bx = nul_handle;
    cpu.dx = 0;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 0x80C4u ||
        cpu.dx != 0x80C4u)
        failures++;

    cpu.ax = 0x4000;
    cpu.bx = nul_handle;
    cpu.cx = 7;
    cpu.dx = 0x0100;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 7u)
        failures++;

    cpu.ax = 0x3F00;
    cpu.bx = nul_handle;
    cpu.cx = 7;
    cpu.dx = 0x0100;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 0u)
        failures++;

    uint8_t nul_sft = DOS_SFT_INVALID;
    if (!dos_jft_read(&vm, nul_handle, &nul_sft)) failures++;
    nul_entry->position = 0x12345678u;
    cpu.ax = 0x4500;
    cpu.bx = nul_handle;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    uint16_t dup_handle = cpu.ax;
    uint8_t dup_sft = DOS_SFT_INVALID;
    if (!dos_jft_read(&vm, dup_handle, &dup_sft)) failures++;
    uint32_t psp_jft = ((uint32_t)vm.current_psp << 4) +
                       __builtin_offsetof(dos_psp_t, jft);
    if ((cpu.flags & FLAG_CF) || dup_handle != 6u ||
        dup_sft != nul_sft ||
        dos_handle_sft(&vm, dup_handle) != nul_entry ||
        nul_entry->ref_count != 2u ||
        dos_handle_sft(&vm, dup_handle)->position != 0x12345678u ||
        dos_mem_read8(&vm, psp_jft + nul_handle) != nul_sft ||
        dos_mem_read8(&vm, psp_jft + dup_handle) != nul_sft)
        failures++;

    cpu.ax = 0x3E00;
    cpu.bx = nul_handle;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    uint8_t closed_sft = 0;
    if (!dos_jft_read(&vm, nul_handle, &closed_sft)) failures++;
    if ((cpu.flags & FLAG_CF) || closed_sft != DOS_SFT_INVALID ||
        dos_mem_read8(&vm, psp_jft + nul_handle) != DOS_SFT_INVALID ||
        !nul_entry->used || nul_entry->ref_count != 1u ||
        dos_handle_sft(&vm, dup_handle) != nul_entry)
        failures++;

    static const char nul_create_path[] = "NUL:";
    cpu.ds = 0x0200;
    cpu.dx = 0;
    for (unsigned i = 0; i < sizeof(nul_create_path); i++)
        dos_mem_write8(&vm, path_address + i, (uint8_t)nul_create_path[i]);
    cpu.ax = 0x3C00;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    nul_handle = cpu.ax;
    dos_sft_entry_t *created_nul = dos_handle_sft(&vm, nul_handle);
    if ((cpu.flags & FLAG_CF) || nul_handle != 5u ||
        !created_nul || created_nul == nul_entry ||
        dos_open_access(created_nul->open_mode) != DOS_ACCESS_READ_WRITE ||
        created_nul->device_kind != DOS_DEVICE_NUL)
        failures++;

    cpu.ax = 0x3E00;
    cpu.bx = nul_handle;
    dos_int21_dispatch(&vm);
    if (created_nul->used)
        failures++;

    cpu.ax = 0x4500;
    cpu.bx = nul_handle;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_HANDLE)
        failures++;

    cpu.ax = 0x4600;
    cpu.bx = dup_handle;
    cpu.cx = DOS_PSP_JFT_ENTRIES;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_HANDLE ||
        dos_handle_sft(&vm, dup_handle) != nul_entry ||
        nul_entry->ref_count != 1u)
        failures++;

    cpu.ax = 0x4600;
    cpu.bx = dup_handle;
    cpu.cx = dup_handle;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || dos_handle_sft(&vm, dup_handle) != nul_entry ||
        nul_entry->ref_count != 1u)
        failures++;

    uint8_t console_sft = DOS_SFT_INVALID;
    if (!dos_jft_read(&vm, 2, &console_sft)) failures++;
    cpu.ax = 0x4600;
    cpu.bx = dup_handle;
    cpu.cx = 1;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || dos_handle_sft(&vm, 1) != nul_entry ||
        nul_entry->ref_count != 2u || dos_handle_sft(&vm, 2) != stdout_entry ||
        stdout_entry->ref_count != 1u)
        failures++;
    uint8_t stderr_sft = DOS_SFT_INVALID;
    if (!dos_jft_read(&vm, 2, &stderr_sft) || stderr_sft != console_sft)
        failures++;

    cpu.ax = 0x4000;
    cpu.bx = 1;
    cpu.cx = 7;
    cpu.flags |= FLAG_CF;
    dos_int21_dispatch(&vm);
    if ((cpu.flags & FLAG_CF) || cpu.ax != 7u)
        failures++;

    cpu.ax = 0x3E00;
    cpu.bx = 1;
    dos_int21_dispatch(&vm);
    cpu.ax = 0x3E00;
    cpu.bx = dup_handle;
    dos_int21_dispatch(&vm);
    if (nul_entry->used || dos_handle_sft(&vm, 1) ||
        dos_handle_sft(&vm, dup_handle))
        failures++;

    cpu.ax = 0x4400;
    cpu.bx = dup_handle;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_HANDLE ||
        vm.indos_count != 0u ||
        dos_mem_read8(&vm, dos_linear(DOS_SYSVARS_SEG, DOS_INDOS_OFF)) != 0u)
        failures++;

    cpu.ax = 0x5700;
    cpu.bx = 0;
    cpu.flags &= ~FLAG_CF;
    dos_int21_dispatch(&vm);
    if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_HANDLE)
        failures++;

    static const char time_path[] = "dos_filetime_contract.tmp";
    (void)osfs2_delete(time_path);
    void *time_file = osfs2_create(time_path, 0);
    bool time_bound = time_file &&
        dos_bind_file_handle(&vm, 5, time_file,
                             DOS_ACCESS_READ_WRITE) == 0;
    if (!time_bound) {
        failures++;
    } else {
        cpu.ax = 0x5701;
        cpu.bx = 5;
        cpu.cx = 0x645C;
        cpu.dx = 0x285D;
        cpu.flags |= FLAG_CF;
        dos_int21_dispatch(&vm);
        if (cpu.flags & FLAG_CF)
            failures++;

        cpu.ax = 0x5700;
        cpu.bx = 5;
        cpu.cx = 0;
        cpu.dx = 0;
        cpu.flags |= FLAG_CF;
        dos_int21_dispatch(&vm);
        if ((cpu.flags & FLAG_CF) || cpu.cx != 0x645CU ||
            cpu.dx != 0x285DU)
            failures++;

        osfs_file_times_t file_times;
        uint16_t stored_date = 0;
        uint16_t stored_time = 0;
        if (osfs2_file_get_times(time_file, &file_times) < 0 ||
            !dos_pack_datetime(file_times.modified, &stored_date,
                               &stored_time) ||
            stored_date != 0x285DU || stored_time != 0x645CU)
            failures++;

        if (dos_clone_handle(&vm, 5, 6, false) < 0) {
            failures++;
        } else {
            cpu.ax = 0x5701;
            cpu.bx = 6;
            cpu.cx = 0x0842;
            cpu.dx = 0x2A61;
            cpu.flags |= FLAG_CF;
            dos_int21_dispatch(&vm);
            cpu.ax = 0x5700;
            cpu.bx = 5;
            cpu.flags |= FLAG_CF;
            dos_int21_dispatch(&vm);
            if ((cpu.flags & FLAG_CF) || cpu.cx != 0x0842U ||
                cpu.dx != 0x2A61U)
                failures++;
            dos_release_file_handle(&vm, 6);
        }

        cpu.ax = 0x5701;
        cpu.bx = 5;
        cpu.cx = 0;
        cpu.dx = 0x29A1;
        cpu.flags &= ~FLAG_CF;
        dos_int21_dispatch(&vm);
        if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_DATA)
            failures++;

        cpu.ax = 0x5702;
        cpu.bx = 5;
        cpu.flags &= ~FLAG_CF;
        dos_int21_dispatch(&vm);
        if (!(cpu.flags & FLAG_CF) || cpu.ax != DOS_ERR_INVALID_FUNCTION)
            failures++;
        dos_release_file_handle(&vm, 5);
    }
    (void)osfs2_delete(time_path);

    failures += dos_find_api_contract_selftest(&vm, &cpu);
    failures += dos_extended_open_contract_selftest(&vm, &cpu);

    dos_api_close_all(&vm);
    for (unsigned i = 0; i < DOS_PSP_JFT_ENTRIES; i++) {
        if (vm.bootstrap_jft[i].sft_index != DOS_SFT_INVALID) {
            failures++;
            break;
        }
    }
    for (unsigned i = 0; i < DOS_MAX_SFT_ENTRIES; i++) {
        if (vm.sft[i].used) {
            failures++;
            break;
        }
    }
    dos_host_free_pages(memory, pages);
    return failures;
}

static void dos_selftest_accumulate(int *failures, const char *name,
                                    int result)
{
    if (result) {
        serial_puts("[DOS-TEST] ");
        serial_puts(name);
        serial_puts(" failed: ");
        serial_putdec((uint64_t)result);
        serial_puts("\n");
    }
    *failures += result;
}

int dos_api_selftest(void)
{
    int failures = 0;
    char path[128];
    uint32_t truename_size = 0;
    uint32_t position = 0;
    uint16_t sectors = 0, free_clusters = 0;
    uint16_t sector_size = 0, total_clusters = 0;

    if (dos_path_to_osfs("C:\\GAMES\\.\\DOOM\\..\\DOOM.EXE", 2,
                         "TOOLS", path, sizeof(path)) != 0 ||
        !dos_string_equal(path, "GAMES/DOOM.EXE"))
        failures++;
    if (dos_path_to_osfs("BIN\\APP.EXE", 2, "TOOLS", path,
                         sizeof(path)) != 0 ||
        !dos_string_equal(path, "TOOLS/BIN/APP.EXE"))
        failures++;
    if (dos_path_to_osfs("..\\DATA", 2, "TOOLS/BIN", path,
                         sizeof(path)) != 0 ||
        !dos_string_equal(path, "TOOLS/DATA"))
        failures++;
    if (dos_path_to_osfs("D:\\NO.EXE", 2, "", path,
                         sizeof(path)) != DOS_ERR_INVALID_DRIVE)
        failures++;

    if (dos_truename_canonicalize(
            "C:\\GAMES\\.\\DOOM\\..\\longfilename.extension", 2,
            "TOOLS", path, sizeof(path), &truename_size) != 0 ||
        !dos_string_equal(path, "C:\\GAMES\\LONGFILE.EXT") ||
        truename_size != sizeof("C:\\GAMES\\LONGFILE.EXT"))
        failures++;
    if (dos_truename_canonicalize("..\\data\\*.*", 2, "TOOLS/BIN",
                                   path, sizeof(path),
                                   &truename_size) != 0 ||
        !dos_string_equal(path, "C:\\TOOLS\\DATA\\????????.???") ||
        truename_size != sizeof("C:\\TOOLS\\DATA\\????????.???"))
        failures++;
    if (dos_truename_canonicalize("NUL.txt", 2, "TOOLS", path,
                                   sizeof(path), &truename_size) != 0 ||
        !dos_string_equal(path, "C:/NUL.TXT"))
        failures++;
    if (dos_truename_canonicalize(
            "\\\\server\\share\\dir\\..\\file.txt", 2, "TOOLS",
            path, sizeof(path), &truename_size) != 0 ||
        !dos_string_equal(path, "\\\\SERVER\\SHARE\\FILE.TXT"))
        failures++;
    if (dos_truename_canonicalize("..\\BAD", 2, "", path,
                                   sizeof(path), &truename_size) !=
            DOS_ERR_PATH_NOT_FOUND ||
        dos_truename_canonicalize("BAD|NAME", 2, "", path,
                                   sizeof(path), &truename_size) !=
            DOS_ERR_FILE_NOT_FOUND ||
        dos_truename_canonicalize("BAD|NAME\\TAIL", 2, "", path,
                                   sizeof(path), &truename_size) !=
            DOS_ERR_PATH_NOT_FOUND ||
        dos_truename_canonicalize("D:\\BAD", 2, "", path,
                                   sizeof(path), &truename_size) !=
            DOS_ERR_PATH_NOT_FOUND ||
        dos_truename_canonicalize("", 2, "", path, sizeof(path),
                                   &truename_size) != DOS_ERR_FILE_NOT_FOUND)
        failures++;
    if (dos_device_from_path("NUL") != DOS_DEVICE_NUL ||
        dos_device_from_path("TOOLS/Nul.txt") != DOS_DEVICE_NUL ||
        dos_device_from_path("TOOLS/NUL:") != DOS_DEVICE_NUL ||
        dos_device_from_path("TOOLS/ANNUL.TXT") != DOS_DEVICE_NONE ||
        dos_device_from_path("TOOLS/NUL:BAD") != DOS_DEVICE_NONE)
        failures++;

    if (dos_seek_position(100, 1000, 1, -50, &position) != 0 ||
        position != 50)
        failures++;
    if (dos_seek_position(100, 1000, 2, -1, &position) != 0 ||
        position != 999)
        failures++;
    if (dos_seek_position(0, 1000, 0, -1, &position) == 0)
        failures++;
    if (dos_seek_position(UINT32_MAX, 1000, 1, 1, &position) == 0)
        failures++;
    if (dos_seek_position(0, 0, 3, 0, &position) == 0)
        failures++;

    if (dos_disk_geometry(4096, 1024, 512, &sectors, &free_clusters,
                          &sector_size, &total_clusters) < 0 ||
        sectors != 1 || sector_size != 512 || free_clusters != 4096 ||
        total_clusters != 8192)
        failures++;

    dos_selftest_accumulate(&failures, "host memory", dos_hostmem_selftest());
    dos_selftest_accumulate(&failures, "memory", dos_mem_selftest());
    dos_selftest_accumulate(&failures, "system variables",
                            dos_system_variables_selftest());
    dos_selftest_accumulate(&failures, "PSP creation",
                            dos_psp_creation_selftest());
    dos_selftest_accumulate(&failures, "DPMI", dpmi_selftest());
    dos_selftest_accumulate(&failures, "BIOS memory",
                            dos_bios_memory_selftest());
    dos_selftest_accumulate(&failures, "BIOS services",
                            dos_bios_contract_selftest());
    dos_selftest_accumulate(&failures, "interrupts",
                            dos_interrupt_selftest());
    dos_selftest_accumulate(&failures, "VCPI", dos_vcpi_selftest());
    dos_selftest_accumulate(&failures, "audio", dos_audio_selftest());
    dos_selftest_accumulate(&failures, "I/O", dos_io_selftest());
    dos_selftest_accumulate(&failures, "mouse", dos_mouse_selftest());
    dos_selftest_accumulate(&failures, "VBE", dos_vbe_selftest());
    dos_selftest_accumulate(&failures, "find", dos_find_selftest());

    return failures;
}
