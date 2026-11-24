/*
 * OsitoK Windows Compatibility Layer — advapi32.dll Shim Implementation
 *
 * Kernel-global registry backed by a flat key-value store and a persistent,
 * versioned hive on OsitoFS.
 *
 * Registry paths are normalized to lowercase with backslashes and retain the
 * caller's WOW64 view. Platform metadata is initialized separately from the
 * application values written by installers.
 */

#include "advapi32_shim.h"
#include "handle.h"
#include "kernel32_shim.h"
#include "wow64.h"
#include "ntsyscall.h"
#include "ntprocess.h"
#include "ntdll_shim.h"
#include "scm.h"
#include "win32_abi.h"
#include "compat32.h"
#include "../fs/ositofs3.h"
#include "../fs/vfs.h"
#include "../kernel/smp.h"
#include "../lib/bearssl/inc/bearssl_hash.h"
#include "ositofs3_format.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void random_get_bytes(void *buf, uint32_t len);
extern PVOID WINAPI LocalAlloc(UINT uFlags, SIZE_T dwBytes);
extern PVOID WINAPI LocalFree(PVOID hMem);
extern void WINAPI SetLastError(DWORD dwErrCode);
extern HANDLE_TABLE g_handle_table;
extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
extern DWORD win32_current_process_id(void);
extern PVOID win32_current_thread_object(void);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);
extern uint64_t idt_get_ticks(void);
extern void sched_yield(void);
extern bool osfs2_is_mounted(void);
extern void *osfs2_find(const char *name);
extern uint64_t osfs2_file_size(void *file);
extern int osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern void *osfs2_create(const char *name, uint64_t size);
extern int osfs2_write(void *file, uint64_t offset, const void *buf,
                       uint64_t len);
extern int osfs2_truncate(void *file, uint64_t size);
extern int osfs2_rename(const char *from, const char *to, bool replace);
extern int osfs2_delete(const char *name);

/* ── String helpers ────────────────────────────────────────── */

static int reg_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void reg_strcpy(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static int reg_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void reg_memcpy(void *dst, const void *src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
}

static void reg_memset(void *dst, BYTE value, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    while (n--) *d++ = value;
}

static int reg_string_valid(const char *text, SIZE_T capacity)
{
    for (SIZE_T i = 0; i < capacity; i++)
        if (!text[i]) return 1;
    return 0;
}

/* ── Registry store ────────────────────────────────────────── */

/* Bounded hive capacity includes both WOW64 views and installed COM classes.
 * The compact version-1 disk format remains unchanged. */
#define MAX_REG_KEYS    2048
#define MAX_REG_VALUES  4096
#define MAX_REG_PATH    256
#define MAX_REG_DATA    512

#define REG_VIEW_SHARED 0
#define REG_VIEW_32     1
#define REG_VIEW_64     2

/* Old version-1 hives mixed A/W payloads without recording their encoding.
 * Keep those bytes opaque. New writes carry an encoding in a reserved byte
 * of the same disk record, so no heuristic can corrupt an existing value. */
#define REG_DATA_LEGACY 0
#define REG_DATA_ANSI   1
#define REG_DATA_UTF16  2

typedef struct {
    char    path[MAX_REG_PATH];     /* full normalized path e.g. "HKLM\\software\\foo" */
    int     used;
    ULONG   handle_id;              /* pseudo-handle ID for open keys */
    BYTE    view;                   /* WOW64 view retained by this handle */
} REG_KEY;

typedef struct {
    char    key_path[MAX_REG_PATH]; /* key this value belongs to */
    char    name[128];              /* value name (empty = default) */
    DWORD   type;                   /* REG_SZ, REG_DWORD, etc. */
    BYTE    data[MAX_REG_DATA];
    DWORD   data_len;
    int     used;
    BYTE    view;                   /* shared, 32-bit, or 64-bit value */
    BYTE    encoding;               /* string payload encoding, or legacy/raw */
} REG_VALUE;

static REG_KEY   reg_keys[MAX_REG_KEYS];
static REG_VALUE reg_values[MAX_REG_VALUES];
static ULONG     next_handle_id  = 0x90000001;
static spinlock_t reg_lock = SPINLOCK_INIT;
static spinlock_t reg_store_lock = SPINLOCK_INIT;
static volatile DWORD reg_init_state;
static DWORD reg_dirty_generation;
static DWORD reg_dirty_operations;
static uint64_t reg_last_flush_ticks;

#define REG_STORE_MAGIC       0x31474552U /* "REG1" */
#define REG_STORE_VERSION     1U
#define REG_STORE_PATH        "System\\Registry\\registry.dat"
#define REG_STORE_TEMP        "System\\Registry\\registry.dat.new"
#define REG_STORE_FLUSH_OPS   64U
#define REG_STORE_FLUSH_TICKS 100U

typedef struct __attribute__((packed)) {
    DWORD magic;
    DWORD version;
    DWORD header_size;
    DWORD key_record_size;
    DWORD value_record_size;
    DWORD key_count;
    DWORD value_count;
    DWORD image_size;
    DWORD crc32;
    DWORD reserved[3];
} REG_STORE_HEADER;

typedef struct __attribute__((packed)) {
    BYTE view;
    BYTE reserved[3];
    char path[MAX_REG_PATH];
} REG_STORE_KEY;

typedef struct __attribute__((packed)) {
    DWORD type;
    DWORD data_len;
    BYTE view;
    BYTE encoding;
    BYTE reserved[2];
    char key_path[MAX_REG_PATH];
    char name[128];
    BYTE data[MAX_REG_DATA];
} REG_STORE_VALUE;

#define REG_STORE_MAX_SIZE \
    (sizeof(REG_STORE_HEADER) + \
     MAX_REG_KEYS * sizeof(REG_STORE_KEY) + \
     MAX_REG_VALUES * sizeof(REG_STORE_VALUE))

_Static_assert(sizeof(REG_STORE_HEADER) == 48, "registry hive header layout");

static void reg_mark_dirty_locked(void)
{
    reg_dirty_generation++;
    if (!reg_dirty_generation) reg_dirty_generation = 1;
    if (reg_dirty_operations != 0xFFFFFFFFU) reg_dirty_operations++;
}

static REG_KEY *reg_key_from_handle(HKEY key)
{
    ULONG handle_id = (ULONG)(ULONG_PTR)key;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used && reg_keys[i].handle_id == handle_id)
            return &reg_keys[i];
    }
    return NULL;
}

static const char *reg_predefined_root_path(HKEY key)
{
    /* PE32 zero-extends predefined HKEYs; PE64 sign-extends them. */
    ULONG value = (ULONG)(ULONG_PTR)key;
    if (value == (ULONG)(ULONG_PTR)HKEY_LOCAL_MACHINE) return "hklm";
    if (value == (ULONG)(ULONG_PTR)HKEY_CURRENT_USER) return "hkcu";
    if (value == (ULONG)(ULONG_PTR)HKEY_CLASSES_ROOT) return "hkcr";
    if (value == (ULONG)(ULONG_PTR)HKEY_USERS) return "hku";
    if (value == (ULONG)(ULONG_PTR)HKEY_CURRENT_CONFIG) return "hkcc";
    return NULL;
}

static int reg_path_has_prefix(const char *path, const char *prefix)
{
    while (*prefix) {
        char a = *path++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return *path == 0 || *path == '\\';
}

/* Ordinary HKCU software settings are shared, including communication between
 * a 64-bit client and 32-bit applications. Only these per-user COM branches
 * are redirected on the Windows 7+ compatibility profile. */
static int reg_path_is_redirected(const char *path)
{
    return reg_path_has_prefix(path, "hklm\\software") ||
           reg_path_has_prefix(path, "hkcu\\software\\classes\\clsid") ||
           reg_path_has_prefix(path, "hkcu\\software\\classes\\directshow") ||
           reg_path_has_prefix(path, "hkcu\\software\\classes\\interface") ||
           reg_path_has_prefix(path, "hkcu\\software\\classes\\media type") ||
           reg_path_has_prefix(path, "hkcu\\software\\classes\\mediafoundation") ||
           reg_path_has_prefix(path, "hkcr");
}

static BYTE reg_default_view(void)
{
    return g_compat32_mode ? REG_VIEW_32 : REG_VIEW_64;
}

static LONG reg_select_view(HKEY root, const char *path, DWORD access,
                            BYTE *view)
{
    DWORD requested = access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY);
    if (requested == (KEY_WOW64_32KEY | KEY_WOW64_64KEY))
        return 87; /* ERROR_INVALID_PARAMETER */

    if (requested == KEY_WOW64_32KEY)
        *view = REG_VIEW_32;
    else if (requested == KEY_WOW64_64KEY)
        *view = REG_VIEW_64;
    else {
        REG_KEY *parent = reg_key_from_handle(root);
        *view = parent ? parent->view : reg_default_view();
    }

    (void)path;
    return ERROR_SUCCESS;
}

static int reg_view_visible(const char *path, BYTE stored, BYTE requested)
{
    if (!reg_path_is_redirected(path)) return 1;
    return stored == REG_VIEW_SHARED || stored == requested;
}

static BYTE reg_storage_view(const char *path, BYTE handle_view)
{
    return reg_path_is_redirected(path) ? handle_view : REG_VIEW_SHARED;
}

static REG_KEY *reg_ensure_key(const char *path, BYTE view)
{
    if (!path || reg_strlen(path) >= MAX_REG_PATH) return NULL;

    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used && reg_keys[i].view == view &&
            reg_stricmp(reg_keys[i].path, path) == 0)
            return &reg_keys[i];
    }

    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used) {
            reg_strcpy(reg_keys[i].path, path);
            reg_keys[i].used = 1;
            reg_keys[i].handle_id = next_handle_id++;
            reg_keys[i].view = view;
            return &reg_keys[i];
        }
    }
    return NULL;
}

/* ── Path normalization ────────────────────────────────────── */

/* Build full path: root prefix + subkey, lowercased, backslash-normalized */
static void build_path(char *out, HKEY root, const char *subkey)
{
    const char *prefix = reg_predefined_root_path(root);
    if (!prefix) {
        /* root is a previously opened key — find it by handle */
        for (int i = 0; i < MAX_REG_KEYS; i++) {
            if (reg_keys[i].used &&
                reg_keys[i].handle_id == (ULONG)(ULONG_PTR)root) {
                prefix = reg_keys[i].path;
                break;
            }
        }
        /* If not found, use raw hex */
        if (prefix == NULL) { prefix = "UNK"; }
    }

    int pos = 0;
    /* Copy prefix */
    for (const char *p = prefix; *p && pos < MAX_REG_PATH - 2; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '/') c = '\\';
        out[pos++] = c;
    }

    /* Add separator + subkey */
    if (subkey && subkey[0]) {
        out[pos++] = '\\';
        for (const char *p = subkey; *p && pos < MAX_REG_PATH - 1; p++) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c == '/') c = '\\';
            out[pos++] = c;
        }
    }
    /* A trailing separator does not change a registry key's identity. */
    while (pos && out[pos - 1] == '\\') pos--;
    out[pos] = 0;
}

/* ── Registry values ──────────────────────────────────────── */

static int reg_string_type(DWORD type)
{
    return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ;
}

static LONG reg_set_value_view(const char *key_path, const char *name,
                               DWORD type, const void *data, DWORD data_len,
                               BYTE view)
{
    const char *value_name = name ? name : "";
    if (!key_path || reg_strlen(key_path) >= MAX_REG_PATH ||
        reg_strlen(value_name) >= (int)sizeof(reg_values[0].name) ||
        data_len > MAX_REG_DATA || (data_len && !data))
        return ERROR_NOT_ENOUGH_MEMORY;

    view = reg_storage_view(key_path, view);

    /* A shared value under a redirected path must make the key visible from
     * both views. */
    if (view == REG_VIEW_SHARED && reg_path_is_redirected(key_path)) {
        if (!reg_ensure_key(key_path, REG_VIEW_32) ||
            !reg_ensure_key(key_path, REG_VIEW_64))
            return ERROR_NOT_ENOUGH_MEMORY;
    } else {
        if (!reg_ensure_key(key_path,
                            view == REG_VIEW_SHARED ? reg_default_view()
                                                    : view))
            return ERROR_NOT_ENOUGH_MEMORY;
    }

    /* Windows replaces an existing value in the selected view. */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used || reg_values[i].view != view ||
            reg_stricmp(reg_values[i].key_path, key_path) != 0 ||
            reg_stricmp(reg_values[i].name, value_name) != 0)
            continue;

        reg_values[i].type = type;
        reg_values[i].encoding = reg_string_type(type) ? REG_DATA_ANSI
                                                      : REG_DATA_LEGACY;
        reg_values[i].data_len =
            data_len < MAX_REG_DATA ? data_len : MAX_REG_DATA;
        if (data && reg_values[i].data_len)
            reg_memcpy(reg_values[i].data, data, reg_values[i].data_len);
        return ERROR_SUCCESS;
    }

    /* Find or create value */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used) {
            reg_strcpy(reg_values[i].key_path, key_path);
            reg_strcpy(reg_values[i].name, value_name);
            reg_values[i].type     = type;
            reg_values[i].encoding = reg_string_type(type) ? REG_DATA_ANSI
                                                          : REG_DATA_LEGACY;
            reg_values[i].data_len = data_len < MAX_REG_DATA ? data_len : MAX_REG_DATA;
            if (data && reg_values[i].data_len)
                reg_memcpy(reg_values[i].data, data, reg_values[i].data_len);
            reg_values[i].view = view;
            reg_values[i].used = 1;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_NOT_ENOUGH_MEMORY;
}

static LONG reg_set_value(const char *key_path, const char *name,
                          DWORD type, const void *data, DWORD data_len)
{
    return reg_set_value_view(key_path, name, type, data, data_len,
                              reg_default_view());
}

static void reg_init(void);

static int reg_store_view_valid(BYTE view)
{
    return view == REG_VIEW_SHARED || view == REG_VIEW_32 ||
           view == REG_VIEW_64;
}

static int reg_store_path_valid(const char *path, SIZE_T capacity)
{
    if (!path || !path[0] || !reg_string_valid(path, capacity)) return 0;
    return reg_path_has_prefix(path, "hklm") ||
           reg_path_has_prefix(path, "hkcu") ||
           reg_path_has_prefix(path, "hkcr") ||
           reg_path_has_prefix(path, "hku") ||
           reg_path_has_prefix(path, "hkcc");
}

static int reg_store_prepare_directories(void)
{
    if (!osfs3_is_mounted()) return 1;
    if (!osfs3_directory_exists_ci("System") &&
        osfs3_mkdir("System") < 0)
        return 0;
    if (!osfs3_directory_exists_ci("System\\Registry") &&
        osfs3_mkdir("System\\Registry") < 0)
        return 0;
    return 1;
}

static unsigned reg_shared_priority(BYTE view)
{
    /* Existing shared writes supersede incorrectly separated legacy copies.
     * Old hives have no timestamps; choose 64-bit, then 32-bit consistently. */
    return view == REG_VIEW_SHARED ? 3 : view;
}

static void reg_restore_shared_values(void)
{
    /* Old kernels stored HKCU\\Software values in separate ABI views. Resolve
     * them once while loading, so reads, enumeration, replacement and deletion
     * all operate on one value. Handles keep their view for COM descendants.
     * Loading itself does not overwrite the persisted hive. */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        REG_VALUE *value = &reg_values[i];
        if (!value->used || reg_path_is_redirected(value->key_path)) continue;
        for (int j = i + 1; j < MAX_REG_VALUES; j++) {
            REG_VALUE *other = &reg_values[j];
            if (!other->used || reg_stricmp(value->key_path, other->key_path) ||
                reg_stricmp(value->name, other->name)) continue;
            if (reg_shared_priority(other->view) > reg_shared_priority(value->view))
                *value = *other;
            other->used = 0;
        }
        value->view = REG_VIEW_SHARED;
    }
}

static int reg_store_load(void)
{
    void *file = osfs2_find(REG_STORE_PATH);
    if (!file) {
        serial_puts("[REG-STORE] no registry hive; starting empty\n");
        return 1;
    }

    uint64_t size64 = osfs2_file_size(file);
    if (size64 < sizeof(REG_STORE_HEADER) || size64 > REG_STORE_MAX_SIZE) {
        serial_puts("[REG-STORE] invalid hive size\n");
        return 0;
    }

    SIZE_T size = (SIZE_T)size64;
    BYTE *image = (BYTE *)kmalloc(size);
    if (!image || osfs2_read(file, 0, image, size) != (int)size) {
        if (image) kfree(image);
        serial_puts("[REG-STORE] hive read failed\n");
        return 0;
    }

    REG_STORE_HEADER *header = (REG_STORE_HEADER *)(void *)image;
    uint64_t expected_size = sizeof(*header) +
        (uint64_t)header->key_count * sizeof(REG_STORE_KEY) +
        (uint64_t)header->value_count * sizeof(REG_STORE_VALUE);
    DWORD stored_crc = header->crc32;
    header->crc32 = 0;
    DWORD calculated_crc = osfs3_crc32(image, size);
    header->crc32 = stored_crc;

    int valid = header->magic == REG_STORE_MAGIC &&
        header->version == REG_STORE_VERSION &&
        header->header_size == sizeof(*header) &&
        header->key_record_size == sizeof(REG_STORE_KEY) &&
        header->value_record_size == sizeof(REG_STORE_VALUE) &&
        header->key_count <= MAX_REG_KEYS &&
        header->value_count <= MAX_REG_VALUES &&
        header->image_size == size && expected_size == size &&
        stored_crc == calculated_crc;

    REG_STORE_KEY *keys = NULL;
    REG_STORE_VALUE *values = NULL;
    if (valid) {
        BYTE *cursor = image + sizeof(*header);
        keys = (REG_STORE_KEY *)(void *)cursor;
        cursor += (SIZE_T)header->key_count * sizeof(*keys);
        values = (REG_STORE_VALUE *)(void *)cursor;

        for (DWORD i = 0; i < header->key_count; i++) {
            if (!reg_store_view_valid(keys[i].view) ||
                !reg_store_path_valid(keys[i].path, sizeof(keys[i].path))) {
                valid = 0;
                break;
            }
        }
    }
    if (valid) {
        for (DWORD i = 0; i < header->value_count; i++) {
            if (!reg_store_view_valid(values[i].view) ||
                values[i].encoding > REG_DATA_UTF16 ||
                values[i].data_len > MAX_REG_DATA ||
                !reg_store_path_valid(values[i].key_path,
                                      sizeof(values[i].key_path)) ||
                !reg_string_valid(values[i].name,
                                  sizeof(values[i].name))) {
                valid = 0;
                break;
            }
        }
    }

    if (!valid) {
        kfree(image);
        serial_puts("[REG-STORE] invalid or corrupt registry hive\n");
        return 0;
    }

    reg_memset(reg_keys, 0, sizeof(reg_keys));
    reg_memset(reg_values, 0, sizeof(reg_values));
    next_handle_id = 0x90000001;
    for (DWORD i = 0; i < header->key_count; i++) {
        reg_strcpy(reg_keys[i].path, keys[i].path);
        reg_keys[i].view = keys[i].view;
        reg_keys[i].handle_id = next_handle_id++;
        reg_keys[i].used = 1;
    }
    for (DWORD i = 0; i < header->value_count; i++) {
        reg_strcpy(reg_values[i].key_path, values[i].key_path);
        reg_strcpy(reg_values[i].name, values[i].name);
        reg_values[i].type = values[i].type;
        reg_values[i].data_len = values[i].data_len;
        if (values[i].data_len)
            reg_memcpy(reg_values[i].data, values[i].data,
                       values[i].data_len);
        reg_values[i].view = values[i].view;
        reg_values[i].encoding = values[i].encoding;
        reg_values[i].used = 1;
    }
    reg_restore_shared_values();
    DWORD key_count = header->key_count;
    DWORD value_count = header->value_count;
    kfree(image);

    serial_puts("[REG-STORE] loaded keys=");
    serial_putdec(key_count);
    serial_puts(" values=");
    serial_putdec(value_count);
    serial_puts("\n");
    return 1;
}

static int reg_store_save(void)
{
    if (!osfs2_is_mounted() || !reg_store_prepare_directories()) return 0;

    BYTE *image = (BYTE *)kmalloc(REG_STORE_MAX_SIZE);
    if (!image) return 0;

    /* Hive publication holds this lock across filesystem I/O, which can
     * yield. INT 0x2E shims are cooperatively scheduled: a busy waiter would
     * prevent the suspended writer from ever releasing the lock. */
    while (!spin_trylock(&reg_store_lock)) sched_yield();
    spin_lock(&reg_lock);
    DWORD key_count = 0, value_count = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++)
        if (reg_keys[i].used) key_count++;
    for (int i = 0; i < MAX_REG_VALUES; i++)
        if (reg_values[i].used) value_count++;

    SIZE_T image_size = sizeof(REG_STORE_HEADER) +
        (SIZE_T)key_count * sizeof(REG_STORE_KEY) +
        (SIZE_T)value_count * sizeof(REG_STORE_VALUE);
    reg_memset(image, 0, image_size);
    REG_STORE_HEADER *header = (REG_STORE_HEADER *)(void *)image;
    header->magic = REG_STORE_MAGIC;
    header->version = REG_STORE_VERSION;
    header->header_size = sizeof(*header);
    header->key_record_size = sizeof(REG_STORE_KEY);
    header->value_record_size = sizeof(REG_STORE_VALUE);
    header->key_count = key_count;
    header->value_count = value_count;
    header->image_size = (DWORD)image_size;

    BYTE *cursor = image + sizeof(*header);
    REG_STORE_KEY *keys = (REG_STORE_KEY *)(void *)cursor;
    DWORD key_index = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used) continue;
        keys[key_index].view = reg_keys[i].view;
        reg_strcpy(keys[key_index].path, reg_keys[i].path);
        key_index++;
    }
    cursor += (SIZE_T)key_count * sizeof(*keys);
    REG_STORE_VALUE *values = (REG_STORE_VALUE *)(void *)cursor;
    DWORD value_index = 0;
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used) continue;
        values[value_index].type = reg_values[i].type;
        values[value_index].data_len = reg_values[i].data_len;
        values[value_index].view = reg_values[i].view;
        values[value_index].encoding = reg_values[i].encoding;
        reg_strcpy(values[value_index].key_path, reg_values[i].key_path);
        reg_strcpy(values[value_index].name, reg_values[i].name);
        if (reg_values[i].data_len)
            reg_memcpy(values[value_index].data, reg_values[i].data,
                       reg_values[i].data_len);
        value_index++;
    }
    DWORD generation = reg_dirty_generation;
    spin_unlock(&reg_lock);

    header->crc32 = 0;
    header->crc32 = osfs3_crc32(image, image_size);

    (void)osfs2_delete(REG_STORE_TEMP);
    void *file = osfs2_create(REG_STORE_TEMP, image_size);
    int saved = file &&
        osfs2_write(file, 0, image, image_size) >= 0 &&
        osfs2_truncate(file, image_size) >= 0 &&
        osfs2_rename(REG_STORE_TEMP, REG_STORE_PATH, true) >= 0;
    if (!saved) (void)osfs2_delete(REG_STORE_TEMP);

    if (saved) {
        spin_lock(&reg_lock);
        if (reg_dirty_generation == generation)
            reg_dirty_operations = 0;
        reg_last_flush_ticks = idt_get_ticks();
        spin_unlock(&reg_lock);
        serial_puts("[REG-STORE] saved keys=");
        serial_putdec(key_count);
        serial_puts(" values=");
        serial_putdec(value_count);
        serial_puts("\n");
    } else {
        serial_puts("[REG-STORE] hive publish failed\n");
    }
    spin_unlock(&reg_store_lock);
    kfree(image);
    return saved;
}

static void reg_store_maybe_flush(int force)
{
    DWORD dirty, operations;
    uint64_t last;
    spin_lock(&reg_lock);
    dirty = reg_dirty_generation;
    operations = reg_dirty_operations;
    last = reg_last_flush_ticks;
    spin_unlock(&reg_lock);
    if (!dirty || !operations) return;

    uint64_t now = idt_get_ticks();
    if (force || operations >= REG_STORE_FLUSH_OPS ||
        now - last >= REG_STORE_FLUSH_TICKS)
        (void)reg_store_save();
}

void advapi32_registry_flush(void)
{
    reg_init();
    reg_store_maybe_flush(1);
}

/* Public entry for the MSI installer: write a value at an already-normalized
 * lowercase backslash path (e.g. "hklm\\software\\app"). Ensures the store is
 * initialized first so installer-written rows are included in the hive. */
void advapi32_reg_install_set(const char *path_lc_backslash, const char *name,
                             DWORD type, const void *data, DWORD len)
{
    reg_init();
    spin_lock(&reg_lock);
    LONG status = reg_set_value(path_lc_backslash, name, type, data, len);
    if (status == ERROR_SUCCESS) reg_mark_dirty_locked();
    spin_unlock(&reg_lock);
    reg_store_maybe_flush(0);
}

static REG_VALUE *reg_find_value(const char *key_path, const char *name,
                                 BYTE view);

static void reg_seed_platform_defaults(void)
{
    /* The Win32 platform provides the core API namespace; legacy DirectX
     * setup supplies the optional side-by-side DLLs. This version identifies
     * the compatibility profile used by setup, not graphics device support.
     * Device creation/capability queries still report the backend's limits.
     * Never infer that an optional runtime has already been installed. */
    static const char *const paths[] = {
        "hklm\\software",
        "hklm\\software\\microsoft",
        "hklm\\software\\microsoft\\directx"
    };
    static const char version[] = "4.09.00.0904";
    const char *directx = paths[2];
    for (BYTE view = REG_VIEW_32; view <= REG_VIEW_64; view++) {
        int ready = 1;
        for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
            ULONG previous = next_handle_id;
            if (!reg_ensure_key(paths[i], view)) {
                ready = 0;
                break;
            }
            if (next_handle_id != previous) reg_mark_dirty_locked();
        }
        /* Preserve persisted overrides, even empty/differently typed values,
         * and values in a legacy shared view. Only missing values are seeded.
         * The initializer owns the store until reg_init_state becomes 2. */
        if (ready && !reg_find_value(directx, "Version", view)) {
            if (reg_set_value_view(directx, "Version", REG_SZ, version,
                                   sizeof(version), view) == ERROR_SUCCESS)
                reg_mark_dirty_locked();
            else
                ready = 0;
        }
        if (!ready)
            serial_puts("[REG-STORE] platform metadata exceeds hive capacity\n");
    }
}

static void reg_init(void)
{
    DWORD state = __atomic_load_n(&reg_init_state, __ATOMIC_ACQUIRE);
    if (state == 2) return;

    DWORD expected = 0;
    if (__atomic_compare_exchange_n(&reg_init_state, &expected, 1, FALSE,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        reg_memset(reg_keys, 0, sizeof(reg_keys));
        reg_memset(reg_values, 0, sizeof(reg_values));
        next_handle_id = 0x90000001;
        reg_dirty_generation = 0;
        reg_dirty_operations = 0;
        reg_last_flush_ticks = idt_get_ticks();
        if (osfs2_is_mounted()) (void)reg_store_load();
        else
            serial_puts("[REG-STORE] filesystem unavailable; volatile registry\n");
        reg_seed_platform_defaults();
        __atomic_store_n(&reg_init_state, 2, __ATOMIC_RELEASE);
        return;
    }

    while (__atomic_load_n(&reg_init_state, __ATOMIC_ACQUIRE) == 1)
        sched_yield(); /* The initializer may be suspended in hive I/O. */

}

/* ── Registry API implementations ──────────────────────────── */

static void reg_store_hkey(PHKEY out, HKEY value)
{
    if (g_compat32_mode)
        *(DWORD *)(void *)out = (DWORD)(ULONG_PTR)value;
    else
        *out = value;
}

static int reg_key_context(HKEY key, const char **path, BYTE *view)
{
    const char *root_path = reg_predefined_root_path(key);
    if (root_path) {
        *path = root_path;
    } else {
        REG_KEY *entry = reg_key_from_handle(key);
        if (!entry) return 0;
        *path = entry->path;
        *view = entry->view;
        return 1;
    }

    *view = reg_default_view();
    return 1;
}

static REG_VALUE *reg_find_value(const char *key_path, const char *name,
                                 BYTE view)
{
    BYTE exact_view = reg_storage_view(key_path, view);
    for (int pass = 0; pass < 2; pass++) {
        BYTE wanted = pass == 0 ? exact_view : REG_VIEW_SHARED;
        if (pass == 1 && wanted == exact_view) break;
        for (int i = 0; i < MAX_REG_VALUES; i++) {
            if (reg_values[i].used && reg_values[i].view == wanted &&
                reg_stricmp(reg_values[i].key_path, key_path) == 0 &&
                reg_stricmp(reg_values[i].name, name) == 0)
                return &reg_values[i];
        }
    }
    return NULL;
}

static int reg_is_vulkan_path(const char *path)
{
    return reg_path_has_prefix(path, "hklm\\software\\khronos\\vulkan") ||
           reg_path_has_prefix(path, "hkcu\\software\\khronos\\vulkan");
}

static void reg_trace_view(const char *operation, const char *path, BYTE view,
                           DWORD access)
{
    if (!reg_is_vulkan_path(path)) return;
    serial_puts("[REG-WOW64] ");
    serial_puts(operation);
    serial_puts(" view=");
    serial_puts(view == REG_VIEW_32 ? "32" : "64");
    serial_puts(" access=0x");
    serial_puthex(access, 8);
    serial_puts(" path=");
    serial_puts(path);
    serial_puts("\n");
}

LONG WINAPI RegOpenKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult)
{
    (void)ulOptions;
    reg_init();

    if (!phkResult) return ERROR_FILE_NOT_FOUND;
    reg_store_hkey(phkResult, NULL);

    char path[MAX_REG_PATH];
    spin_lock(&reg_lock);
    build_path(path, hKey, lpSubKey);
    BYTE view;
    LONG status = reg_select_view(hKey, path, samDesired, &view);
    if (status != ERROR_SUCCESS) {
        spin_unlock(&reg_lock);
        return status;
    }
    reg_trace_view("open", path, view, samDesired);

#ifndef OK_QUIET
    serial_puts("[REG] OpenKeyEx: ");
    serial_puts(path);
    serial_puts("\n");
#endif

    /* The store can contain a descendant without a materialized ancestor
     * (including hives saved by older kernels). Its ancestors still exist in
     * that view, even when no value was written on the ancestor itself.
     * Materialize a view-specific handle even for shared keys so descendants
     * inherit the caller's selected view. */
    int exists = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_path_has_prefix(reg_keys[i].path, path) &&
            reg_view_visible(reg_keys[i].path, reg_keys[i].view, view)) {
            exists = 1;
            break;
        }
    }

    if (!exists) {
        for (int i = 0; i < MAX_REG_VALUES; i++) {
            if (reg_values[i].used &&
                reg_path_has_prefix(reg_values[i].key_path, path) &&
                reg_view_visible(reg_values[i].key_path, reg_values[i].view, view)) {
                exists = 1;
                break;
            }
        }
    }

    if (exists) {
        REG_KEY *entry = reg_ensure_key(path, view);
        if (!entry) {
            spin_unlock(&reg_lock);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        HKEY result = (HKEY)(ULONG_PTR)entry->handle_id;
        spin_unlock(&reg_lock);
        reg_store_hkey(phkResult, result);
        return ERROR_SUCCESS;
    }

    spin_unlock(&reg_lock);

#ifndef OK_QUIET
    serial_puts("[REG]   not found\n");
#endif
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegOpenKeyA(HKEY hKey, PCSTR lpSubKey, PHKEY phkResult)
{
    return RegOpenKeyExA(hKey, lpSubKey, 0, 0, phkResult);
}

LONG WINAPI RegCreateKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD Reserved,
                            PSTR lpClass, DWORD dwOptions, DWORD samDesired,
                            PVOID lpSecurityAttributes, PHKEY phkResult,
                            DWORD *lpdwDisposition)
{
    (void)Reserved;
    (void)lpClass;
    (void)dwOptions;
    (void)lpSecurityAttributes;
    reg_init();

    if (!phkResult) return ERROR_FILE_NOT_FOUND;

    /* Try to open first */
    LONG result = RegOpenKeyExA(hKey, lpSubKey, 0, samDesired, phkResult);
    if (result == ERROR_SUCCESS) {
        if (lpdwDisposition) *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        return ERROR_SUCCESS;
    }

    /* Create a handle in the selected view. */
    char path[MAX_REG_PATH];
    BYTE view;
    spin_lock(&reg_lock);
    build_path(path, hKey, lpSubKey);
    LONG view_status = reg_select_view(hKey, path, samDesired, &view);
    if (view_status != ERROR_SUCCESS) {
        spin_unlock(&reg_lock);
        return view_status;
    }
    REG_KEY *entry = reg_ensure_key(path, view);
    if (entry) {
        HKEY created = (HKEY)(ULONG_PTR)entry->handle_id;
        reg_mark_dirty_locked();
        spin_unlock(&reg_lock);
        reg_trace_view("create", path, view, samDesired);
        reg_store_hkey(phkResult, created);
        if (lpdwDisposition) *lpdwDisposition = REG_CREATED_NEW_KEY;
        reg_store_maybe_flush(0);
        return ERROR_SUCCESS;
    }

    spin_unlock(&reg_lock);
    return ERROR_NOT_ENOUGH_MEMORY;
}

static LONG WINAPI RegCreateKeyA_k32(HKEY hKey, PCSTR lpSubKey,
                                     PHKEY phkResult)
{
    return RegCreateKeyExA(hKey, lpSubKey, 0, NULL, 0, KEY_ALL_ACCESS,
                           NULL, phkResult, NULL);
}

static LONG reg_copy_value_data(const REG_VALUE *value, int wide,
                                DWORD *type, BYTE *data, DWORD *size)
{
    if (data && !size) return 87; /* ERROR_INVALID_PARAMETER */
    if (type) *type = value->type;
    if (!size) return ERROR_SUCCESS;
    const BYTE *source = value->data;
    DWORD bytes = value->data_len;
    union { WCHAR wide[MAX_REG_DATA]; BYTE bytes[MAX_REG_DATA * 2]; } converted;
    if (bytes && reg_string_type(value->type) &&
        ((wide && value->encoding == REG_DATA_ANSI) ||
         (!wide && value->encoding == REG_DATA_UTF16))) {
        DWORD saved_error = GetLastError();
        int count;
        if (wide) {
            count = MultiByteToWideChar(0, 0, (PCSTR)value->data, bytes,
                                        converted.wide, MAX_REG_DATA);
            bytes = count > 0 ? (DWORD)count * sizeof(WCHAR) : 0;
        } else {
            count = WideCharToMultiByte(0, 0, (PCWSTR)value->data,
                                        bytes / sizeof(WCHAR),
                                        (PSTR)converted.bytes,
                                        sizeof(converted.bytes), NULL, NULL);
            bytes = count > 0 ? (DWORD)count : 0;
        }
        SetLastError(saved_error);
        if (count <= 0) return 1113; /* ERROR_NO_UNICODE_TRANSLATION */
        source = converted.bytes;
    }
    DWORD capacity = *size;
    *size = bytes;
    if (data) {
        if (capacity < bytes) return ERROR_MORE_DATA;
        reg_memcpy(data, source, bytes);
    }
    return ERROR_SUCCESS;
}

static LONG reg_query_value_encoded(HKEY hKey, PCSTR lpValueName,
                                     DWORD *lpType, BYTE *lpData,
                                     DWORD *lpcbData, int wide)
{
    reg_init();

    const char *key_path = NULL;
    REG_VALUE snapshot;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    const char *val_name = lpValueName ? lpValueName : "";

#ifndef OK_QUIET
    serial_puts("[REG] QueryValueEx: ");
    serial_puts(key_path);
    serial_puts(" \\ ");
    serial_puts(val_name);
    serial_puts("\n");
#endif

    REG_VALUE *value = reg_find_value(key_path, val_name, view);
    if (value) snapshot = *value;
    spin_unlock(&reg_lock);
    if (value)
        return reg_copy_value_data(&snapshot, wide, lpType, lpData, lpcbData);

#ifndef OK_QUIET
    serial_puts("[REG]   value not found\n");
#endif
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegQueryValueExA(HKEY hKey, PCSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    return reg_query_value_encoded(hKey, lpValueName, lpType, lpData,
                                    lpcbData, 0);
}

LONG WINAPI RegSetValueExA(HKEY hKey, PCSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData)
{
    (void)Reserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }
    const char *val_name = lpValueName ? lpValueName : "";
    LONG status = reg_set_value_view(key_path, val_name, dwType, lpData,
                                     cbData, view);
    if (status == ERROR_SUCCESS) reg_mark_dirty_locked();
    spin_unlock(&reg_lock);
    if (status == ERROR_SUCCESS) reg_store_maybe_flush(0);
    return status;
}

static LONG WINAPI RegDisablePredefinedCache(void)
{
    /* Neither HKCU nor the other predefined roots cache opened handles:
     * build_path/reg_key_context resolve the pseudo-handle on every use.
     * Both cache-disable APIs therefore already have their requested policy.
     * Do not initialize/flush the hive, close caller handles, or set LastError.
     * Per-user HKCU/HKU mapping remains a separate registry limitation. */
    return ERROR_SUCCESS;
}

LONG WINAPI RegCloseKey(HKEY hKey)
{
    /* Key objects persist; closing releases only the logical handle. */
    (void)hKey;
    reg_store_maybe_flush(0);
    return ERROR_SUCCESS;
}

LONG WINAPI RegFlushKey(HKEY hKey)
{
    reg_init();
    spin_lock(&reg_lock);
    const char *path = NULL;
    BYTE view = REG_VIEW_SHARED;
    int valid = reg_key_context(hKey, &path, &view);
    spin_unlock(&reg_lock);
    if (!valid) return ERROR_INVALID_HANDLE;
    (void)path;
    (void)view;
    return reg_store_save() ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

LONG WINAPI RegDeleteValueA(HKEY hKey, PCSTR lpValueName)
{
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    const char *val_name = lpValueName ? lpValueName : "";

    REG_VALUE *value = reg_find_value(key_path, val_name, view);
    if (value) {
        value->used = 0;
        reg_mark_dirty_locked();
        spin_unlock(&reg_lock);
        reg_store_maybe_flush(0);
        return ERROR_SUCCESS;
    }

    spin_unlock(&reg_lock);

    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegDeleteKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD samDesired,
                            DWORD Reserved)
{
    reg_init();
    if (Reserved != 0 || !lpSubKey || !*lpSubKey)
        return ERROR_INVALID_PARAMETER;

    const char *base = NULL;
    BYTE inherited_view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &base, &inherited_view) || !base) {
        spin_unlock(&reg_lock);
        return ERROR_INVALID_HANDLE;
    }

    char target[MAX_REG_PATH];
    build_path(target, hKey, lpSubKey);
    BYTE view;
    LONG view_status = reg_select_view(hKey, target, samDesired, &view);
    if (view_status != ERROR_SUCCESS) {
        spin_unlock(&reg_lock);
        return view_status;
    }

    /* RegDeleteKey removes values, but requires callers to remove every
     * subkey first. Descendant values imply a subkey in this flat store. */
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used ||
            !reg_view_visible(reg_keys[i].path, reg_keys[i].view, view) ||
            reg_stricmp(reg_keys[i].path, target) == 0)
            continue;
        if (reg_path_has_prefix(reg_keys[i].path, target)) {
            spin_unlock(&reg_lock);
            return ERROR_ACCESS_DENIED;
        }
    }
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used ||
            !reg_view_visible(reg_values[i].key_path,
                              reg_values[i].view, view) ||
            reg_stricmp(reg_values[i].key_path, target) == 0)
            continue;
        if (reg_path_has_prefix(reg_values[i].key_path, target)) {
            spin_unlock(&reg_lock);
            return ERROR_ACCESS_DENIED;
        }
    }

    int exists = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_view_visible(reg_keys[i].path, reg_keys[i].view, view) &&
            reg_stricmp(reg_keys[i].path, target) == 0) {
            exists = 1;
            break;
        }
    }
    if (!exists) {
        for (int i = 0; i < MAX_REG_VALUES; i++) {
            if (reg_values[i].used &&
                reg_view_visible(reg_values[i].key_path,
                                 reg_values[i].view, view) &&
                reg_stricmp(reg_values[i].key_path, target) == 0) {
                exists = 1;
                break;
            }
        }
    }
    if (!exists) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (reg_values[i].used &&
            reg_view_visible(reg_values[i].key_path,
                             reg_values[i].view, view) &&
            reg_stricmp(reg_values[i].key_path, target) == 0)
            reg_values[i].used = 0;
    }
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_view_visible(reg_keys[i].path, reg_keys[i].view, view) &&
            reg_stricmp(reg_keys[i].path, target) == 0)
            reg_keys[i].used = 0;
    }
    reg_mark_dirty_locked();
    spin_unlock(&reg_lock);
    reg_store_maybe_flush(0);
    return ERROR_SUCCESS;
}

LONG WINAPI RegDeleteKeyA(HKEY hKey, PCSTR lpSubKey)
{
    return RegDeleteKeyExA(hKey, lpSubKey, 0, 0);
}

static const char *reg_direct_child(const char *parent, const char *candidate)
{
    int parent_len = reg_strlen(parent);
    if (reg_strlen(candidate) <= parent_len || candidate[parent_len] != '\\')
        return NULL;

    for (int i = 0; i < parent_len; i++) {
        char a = parent[i], b = candidate[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return NULL;
    }

    const char *child = candidate + parent_len + 1;
    for (const char *p = child; *p; p++)
        if (*p == '\\') return NULL;
    return child;
}

static int reg_same_child_name(const char *a, const char *b)
{
    return reg_stricmp(a, b) == 0;
}

static REG_KEY *reg_child_at(const char *key_path, BYTE view, DWORD index,
                             const char **child_name)
{
    DWORD found = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used ||
            !reg_view_visible(reg_keys[i].path, reg_keys[i].view, view))
            continue;

        const char *child = reg_direct_child(key_path, reg_keys[i].path);
        if (!child) continue;

        int duplicate = 0;
        for (int j = 0; j < i; j++) {
            if (!reg_keys[j].used ||
                !reg_view_visible(reg_keys[j].path, reg_keys[j].view, view))
                continue;
            const char *previous =
                reg_direct_child(key_path, reg_keys[j].path);
            if (previous && reg_same_child_name(previous, child)) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;

        if (found++ == index) {
            if (child_name) *child_name = child;
            return &reg_keys[i];
        }
    }
    return NULL;
}

static REG_VALUE *reg_value_at(const char *key_path, BYTE view, DWORD index)
{
    DWORD found = 0;
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used ||
            reg_stricmp(reg_values[i].key_path, key_path) != 0 ||
            !reg_view_visible(key_path, reg_values[i].view, view))
            continue;

        /* A view-specific value shadows a shared value of the same name. */
        if (reg_values[i].view == REG_VIEW_SHARED &&
            reg_path_is_redirected(key_path)) {
            int shadowed = 0;
            for (int j = 0; j < MAX_REG_VALUES; j++) {
                if (reg_values[j].used && reg_values[j].view == view &&
                    reg_stricmp(reg_values[j].key_path, key_path) == 0 &&
                    reg_stricmp(reg_values[j].name,
                                reg_values[i].name) == 0) {
                    shadowed = 1;
                    break;
                }
            }
            if (shadowed) continue;
        }

        if (found++ == index) return &reg_values[i];
    }
    return NULL;
}

LONG WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, PSTR lpName,
                          DWORD *lpcchName, DWORD *lpReserved,
                          PSTR lpClass, DWORD *lpcchClass,
                          PVOID lpftLastWriteTime)
{
    (void)lpReserved;
    (void)lpClass;
    (void)lpcchClass;
    (void)lpftLastWriteTime;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    const char *child = NULL;
    if (!reg_child_at(key_path, view, dwIndex, &child)) {
        spin_unlock(&reg_lock);
        return ERROR_NO_MORE_ITEMS;
    }
    char child_copy[MAX_REG_PATH];
    reg_strcpy(child_copy, child);
    spin_unlock(&reg_lock);

    int child_len = reg_strlen(child_copy);
    if (lpName && lpcchName && *lpcchName > (DWORD)child_len) {
        reg_strcpy(lpName, child_copy);
        *lpcchName = child_len;
        return ERROR_SUCCESS;
    }
    if (lpcchName) *lpcchName = child_len + 1;
    return ERROR_MORE_DATA;
}

LONG WINAPI RegEnumKeyA(HKEY hKey, DWORD index, PSTR name, DWORD capacity)
{
    char child[MAX_REG_PATH];
    DWORD length = sizeof(child);
    LONG status = RegEnumKeyExA(hKey, index, child, &length, NULL,
                                NULL, NULL, NULL);
    if (status != ERROR_SUCCESS)
        return status == ERROR_FILE_NOT_FOUND ? 6 : status;
    if (!name) return 87;
    if (capacity > length) {
        reg_strcpy(name, child);
        return ERROR_SUCCESS;
    }
    /* The native ANSI legacy API writes a terminated, one-character-short
     * name when capacity equals the name length, still returning MORE_DATA.
     * Smaller buffers remain untouched. The wide API does not truncate. */
    if (capacity == length && capacity) {
        for (DWORD i = 0; i + 1 < capacity; i++) name[i] = child[i];
        name[capacity - 1] = 0;
    }
    return ERROR_MORE_DATA;
}

LONG WINAPI RegEnumKeyW(HKEY hKey, DWORD index, PWSTR name, DWORD capacity)
{
    LONG status = RegEnumKeyExW(hKey, index, name, &capacity, NULL,
                                NULL, NULL, NULL);
    return status == ERROR_FILE_NOT_FOUND ? 6 : status;
}

LONG WINAPI RegEnumValueA(HKEY hKey, DWORD dwIndex, PSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    REG_VALUE *value = reg_value_at(key_path, view, dwIndex);
    if (!value) {
        spin_unlock(&reg_lock);
        return ERROR_NO_MORE_ITEMS;
    }
    REG_VALUE snapshot = *value;
    spin_unlock(&reg_lock);

    int name_len = reg_strlen(snapshot.name);
    if (lpValueName && lpcchValueName) {
        if (*lpcchValueName <= (DWORD)name_len) {
            *lpcchValueName = name_len + 1;
            return ERROR_MORE_DATA;
        }
        reg_strcpy(lpValueName, snapshot.name);
        *lpcchValueName = name_len;
    }
    return reg_copy_value_data(&snapshot, 0, lpType, lpData, lpcbData);
}

LONG WINAPI RegEnumKeyExW(HKEY hKey, DWORD dwIndex, PWSTR lpName,
                          DWORD *lpcchName, DWORD *lpReserved,
                          PWSTR lpClass, DWORD *lpcchClass,
                          PVOID lpftLastWriteTime)
{
    char name[MAX_REG_PATH];
    DWORD capacity = sizeof(name);
    if (lpcchName && *lpcchName < capacity) capacity = *lpcchName;
    LONG status = RegEnumKeyExA(hKey, dwIndex, name, &capacity, lpReserved,
                                NULL, NULL, lpftLastWriteTime);
    if (status == ERROR_MORE_DATA) {
        if (lpcchName) *lpcchName = capacity;
        return status;
    }
    if (status != ERROR_SUCCESS) return status;
    if (!lpName || !lpcchName || *lpcchName <= capacity) {
        if (lpcchName) *lpcchName = capacity + 1;
        return ERROR_MORE_DATA;
    }
    for (DWORD i = 0; i < capacity; i++)
        lpName[i] = (WCHAR)(BYTE)name[i];
    lpName[capacity] = 0;
    *lpcchName = capacity;
    if (lpClass) {
        if (!lpcchClass || !*lpcchClass) return ERROR_MORE_DATA;
        lpClass[0] = 0;
    }
    if (lpcchClass) *lpcchClass = 0;
    return ERROR_SUCCESS;
}

LONG WINAPI RegQueryInfoKeyW(HKEY hKey, PWSTR lpClass, DWORD *lpcchClass,
                             DWORD *lpReserved, DWORD *lpcSubKeys,
                             DWORD *lpcbMaxSubKeyLen, DWORD *lpcbMaxClassLen,
                             DWORD *lpcValues, DWORD *lpcbMaxValueNameLen,
                             DWORD *lpcbMaxValueLen,
                             DWORD *lpcbSecurityDescriptor,
                             PVOID lpftLastWriteTime)
{
    (void)lpReserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return 6; /* ERROR_INVALID_HANDLE */
    }
    if (!key_path) {
        spin_unlock(&reg_lock);
        return 6; /* ERROR_INVALID_HANDLE */
    }

    DWORD subkeys = 0, max_subkey_len = 0;
    for (DWORD i = 0;; i++) {
        const char *child = NULL;
        if (!reg_child_at(key_path, view, i, &child)) break;
        DWORD child_len = (DWORD)reg_strlen(child);
        subkeys++;
        if (child_len > max_subkey_len) max_subkey_len = child_len;
    }

    DWORD values = 0, max_value_name_len = 0, max_value_len = 0;
    for (DWORD i = 0;; i++) {
        REG_VALUE *value = reg_value_at(key_path, view, i);
        if (!value) break;
        DWORD name_len = (DWORD)reg_strlen(value->name);
        values++;
        if (name_len > max_value_name_len) max_value_name_len = name_len;
        DWORD bytes = value->data_len;
        if (reg_string_type(value->type) && value->encoding == REG_DATA_ANSI)
            bytes *= sizeof(WCHAR);
        if (bytes > max_value_len) max_value_len = bytes;
    }
    spin_unlock(&reg_lock);

    /* A bad guest pointer can fault. Never expose reg_lock to that path. */
    if (lpClass) {
        if (!lpcchClass || *lpcchClass == 0) return ERROR_MORE_DATA;
        lpClass[0] = 0;
    }
    if (lpcchClass) *lpcchClass = 0;
    if (lpcbMaxClassLen) *lpcbMaxClassLen = 0;
    if (lpcbSecurityDescriptor) *lpcbSecurityDescriptor = 0;
    if (lpftLastWriteTime) {
        ((DWORD *)lpftLastWriteTime)[0] = 0;
        ((DWORD *)lpftLastWriteTime)[1] = 0;
    }
    if (lpcSubKeys) *lpcSubKeys = subkeys;
    if (lpcbMaxSubKeyLen) *lpcbMaxSubKeyLen = max_subkey_len;
    if (lpcValues) *lpcValues = values;
    if (lpcbMaxValueNameLen) *lpcbMaxValueNameLen = max_value_name_len;
    if (lpcbMaxValueLen) *lpcbMaxValueLen = max_value_len;
    return ERROR_SUCCESS;
}

LONG WINAPI RegQueryInfoKeyA(HKEY hKey, PSTR lpClass, DWORD *lpcchClass,
                             DWORD *lpReserved, DWORD *lpcSubKeys,
                             DWORD *lpcbMaxSubKeyLen, DWORD *lpcbMaxClassLen,
                             DWORD *lpcValues, DWORD *lpcbMaxValueNameLen,
                             DWORD *lpcbMaxValueLen,
                             DWORD *lpcbSecurityDescriptor,
                             PVOID lpftLastWriteTime)
{
    if (lpClass) {
        if (!lpcchClass || !*lpcchClass) return ERROR_MORE_DATA;
        lpClass[0] = 0;
    }
    return RegQueryInfoKeyW(hKey, NULL, lpcchClass, lpReserved, lpcSubKeys,
                            lpcbMaxSubKeyLen, lpcbMaxClassLen, lpcValues,
                            lpcbMaxValueNameLen, lpcbMaxValueLen,
                            lpcbSecurityDescriptor, lpftLastWriteTime);
}

LONG WINAPI RegDeleteTreeA(HKEY hKey, PCSTR lpSubKey)
{
    reg_init();
    const char *base = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &base, &view) || !base) {
        spin_unlock(&reg_lock);
        return 6; /* ERROR_INVALID_HANDLE */
    }

    char target[MAX_REG_PATH];
    if (lpSubKey && *lpSubKey)
        build_path(target, hKey, lpSubKey);
    else
        reg_strcpy(target, base);

    int removed = 0;
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used ||
            !reg_view_visible(reg_values[i].key_path,
                              reg_values[i].view, view))
            continue;
        if (reg_path_has_prefix(reg_values[i].key_path, target)) {
            reg_values[i].used = 0;
            removed = 1;
        }
    }
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used ||
            !reg_view_visible(reg_keys[i].path, reg_keys[i].view, view))
            continue;
        if (!reg_path_has_prefix(reg_keys[i].path, target)) continue;
        if ((!lpSubKey || !*lpSubKey) &&
            reg_stricmp(reg_keys[i].path, target) == 0)
            continue;
        reg_keys[i].used = 0;
        removed = 1;
    }
    if (removed) reg_mark_dirty_locked();
    spin_unlock(&reg_lock);
    if (removed) reg_store_maybe_flush(0);
    return removed ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

/* ── Wide (W) string helpers ───────────────────────────────── */

/* Convert UTF-16LE to ANSI (ASCII-range only) */
static void wide_to_ansi(char *dst, const WCHAR *src, int max)
{
    int i;
    if (!src) { dst[0] = 0; return; }
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = (char)(src[i] & 0xFF);
    dst[i] = 0;
}

/* ── Wide (W) registry API ────────────────────────────────── */

LONG WINAPI RegCreateKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD Reserved,
                            PWSTR lpClass, DWORD dwOptions, DWORD samDesired,
                            PVOID lpSecurityAttributes, PHKEY phkResult,
                            DWORD *lpdwDisposition)
{
    (void)lpClass;
    char ansi_subkey[MAX_REG_PATH];
    wide_to_ansi(ansi_subkey, lpSubKey, MAX_REG_PATH);
    serial_puts("[REG] RegCreateKeyExW -> A: ");
    serial_puts(ansi_subkey);
    serial_puts("\n");
    return RegCreateKeyExA(hKey, ansi_subkey, Reserved, NULL, dwOptions,
                           samDesired, lpSecurityAttributes, phkResult,
                           lpdwDisposition);
}

LONG WINAPI RegSetValueExW(HKEY hKey, PCWSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData)
{
    (void)Reserved;
    reg_init();
    char ansi_name[128];
    wide_to_ansi(ansi_name, lpValueName, 128);
#ifndef OK_QUIET
    serial_puts("[REG] RegSetValueExW -> A: ");
    serial_puts(ansi_name);
    serial_puts("\n");
#endif
    const char *path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }
    LONG status = reg_set_value_view(path, ansi_name, dwType, lpData, cbData, view);
    if (status == ERROR_SUCCESS) {
        REG_VALUE *value = reg_find_value(path, ansi_name, view);
        value->encoding = reg_string_type(dwType) ? REG_DATA_UTF16 : REG_DATA_LEGACY;
        reg_mark_dirty_locked();
    }
    spin_unlock(&reg_lock);
    if (status == ERROR_SUCCESS) reg_store_maybe_flush(0);
    return status;
}

LONG WINAPI RegOpenKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult)
{
    char ansi_subkey[MAX_REG_PATH];
    wide_to_ansi(ansi_subkey, lpSubKey, MAX_REG_PATH);
#ifndef OK_QUIET
    serial_puts("[REG] RegOpenKeyExW -> A: ");
    serial_puts(ansi_subkey);
    serial_puts("\n");
#endif
    return RegOpenKeyExA(hKey, ansi_subkey, ulOptions, samDesired, phkResult);
}

static LONG WINAPI RegOpenKeyW(HKEY key, PCWSTR subkey, PHKEY result)
{
    return RegOpenKeyExW(key, subkey, 0, 0, result);
}

LONG WINAPI RegQueryValueExW(HKEY hKey, PCWSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    char ansi_name[128];
    wide_to_ansi(ansi_name, lpValueName, 128);
#ifndef OK_QUIET
    serial_puts("[REG] RegQueryValueExW -> A: ");
    serial_puts(ansi_name);
    serial_puts("\n");
#endif
    return reg_query_value_encoded(hKey, ansi_name, lpType, lpData, lpcbData, 1);
}

LONG WINAPI RegEnumValueW(HKEY hKey, DWORD dwIndex, PWSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    spin_lock(&reg_lock);
    if (!reg_key_context(hKey, &key_path, &view)) {
        spin_unlock(&reg_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    REG_VALUE *value = reg_value_at(key_path, view, dwIndex);
    if (!value) {
        spin_unlock(&reg_lock);
        return ERROR_NO_MORE_ITEMS;
    }
    REG_VALUE snapshot = *value;
    spin_unlock(&reg_lock);

    DWORD name_len = (DWORD)reg_strlen(snapshot.name);
    if (lpValueName && lpcchValueName) {
        if (*lpcchValueName <= name_len) {
            *lpcchValueName = name_len + 1;
            return ERROR_MORE_DATA;
        }
        for (DWORD j = 0; j < name_len; j++)
            lpValueName[j] = (WCHAR)(BYTE)snapshot.name[j];
        lpValueName[name_len] = 0;
    }
    if (lpcchValueName) *lpcchValueName = name_len;
    return reg_copy_value_data(&snapshot, 1, lpType, lpData, lpcbData);
}

LONG WINAPI RegDeleteValueW(HKEY hKey, PCWSTR lpValueName)
{
    char ansi_name[128];
    wide_to_ansi(ansi_name, lpValueName, 128);
    return RegDeleteValueA(hKey, ansi_name);
}

LONG WINAPI RegDeleteKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD samDesired,
                            DWORD Reserved)
{
    char ansi_subkey[MAX_REG_PATH];
    wide_to_ansi(ansi_subkey, lpSubKey, MAX_REG_PATH);
    return RegDeleteKeyExA(hKey, ansi_subkey, samDesired, Reserved);
}

LONG WINAPI RegDeleteKeyW(HKEY hKey, PCWSTR lpSubKey)
{
    return RegDeleteKeyExW(hKey, lpSubKey, 0, 0);
}

/* ── User identity stubs (UT99 Core.dll) ──────────────────── */

static void reg_test_expect(BOOL condition, const char *name,
                            int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[REGTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int advapi32_registry_selftest(void)
{
    const struct {
        HKEY native_key;
        ULONG pe32_key;
        const char *path;
    } roots[] = {
        { HKEY_CLASSES_ROOT,   0x80000000U, "hkcr" },
        { HKEY_CURRENT_USER,   0x80000001U, "hkcu" },
        { HKEY_LOCAL_MACHINE,  0x80000002U, "hklm" },
        { HKEY_USERS,          0x80000003U, "hku" },
        { HKEY_CURRENT_CONFIG, 0x80000005U, "hkcc" },
    };
    const char *test_path = "Software\\OsitoK\\Tests\\RegistryRootAlias";
    const char *delete_path = "Software\\OsitoK\\Tests\\DeleteKeyViews";
    const char *value_name = "ViewValue";
    HKEY pe32_hklm = (HKEY)(ULONG_PTR)0x80000002U;
    HKEY key32 = NULL, opened32 = NULL;
    HKEY key64 = NULL, opened64 = NULL;
    HKEY delete32 = NULL, delete32_child = NULL;
    HKEY delete64 = NULL, opened_delete = NULL;
    DWORD value32 = 0x3211A5U, value64 = 0x6411A5U;
    DWORD disposition = 0, type = 0, size = sizeof(DWORD), observed = 0;
    LONG status;
    int checks = 0, failures = 0;

    serial_puts("[REGTEST] starting predefined-HKEY/WOW64 test\n");
    if (g_compat32_mode) {
        serial_puts("[REGTEST] FAIL: cannot run during a PE32 callback\n");
        return 1;
    }

    for (SIZE_T i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        HKEY pe32_key = (HKEY)(ULONG_PTR)roots[i].pe32_key;
        const char *native_path = reg_predefined_root_path(roots[i].native_key);
        const char *pe32_path = reg_predefined_root_path(pe32_key);
        reg_test_expect(native_path && pe32_path &&
                        reg_stricmp(native_path, roots[i].path) == 0 &&
                        reg_stricmp(pe32_path, roots[i].path) == 0,
                        "PE32 and PE64 predefined root aliases",
                        &checks, &failures);
    }

    status = RegCreateKeyExA(pe32_hklm, test_path, 0, NULL, 0,
                             KEY_READ | KEY_WRITE | KEY_WOW64_32KEY,
                             NULL, &key32, &disposition);
    reg_test_expect(status == ERROR_SUCCESS && key32 != NULL,
                    "create 32-bit view through zero-extended HKLM",
                    &checks, &failures);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExA(key32, value_name, 0, REG_DWORD,
                                (const BYTE *)&value32, sizeof(value32));
        reg_test_expect(status == ERROR_SUCCESS, "write 32-bit view",
                        &checks, &failures);
    }

    status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, test_path, 0,
                           KEY_READ | KEY_WOW64_32KEY, &opened32);
    reg_test_expect(status == ERROR_SUCCESS && opened32 != NULL,
                    "open 32-bit view through sign-extended HKLM",
                    &checks, &failures);
    if (status == ERROR_SUCCESS) {
        status = RegQueryValueExA(opened32, value_name, NULL, &type,
                                  (BYTE *)&observed, &size);
        reg_test_expect(status == ERROR_SUCCESS && type == REG_DWORD &&
                        size == sizeof(observed) && observed == value32,
                        "read 32-bit value across root aliases",
                        &checks, &failures);
    }

    status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, test_path, 0,
                           KEY_READ | KEY_WOW64_64KEY, &opened64);
    reg_test_expect(status == ERROR_FILE_NOT_FOUND && opened64 == NULL,
                    "32-bit value remains isolated from 64-bit view",
                    &checks, &failures);

    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, test_path, 0, NULL, 0,
                             KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                             NULL, &key64, &disposition);
    reg_test_expect(status == ERROR_SUCCESS && key64 != NULL,
                    "create 64-bit view through sign-extended HKLM",
                    &checks, &failures);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExA(key64, value_name, 0, REG_DWORD,
                                (const BYTE *)&value64, sizeof(value64));
        reg_test_expect(status == ERROR_SUCCESS, "write 64-bit view",
                        &checks, &failures);
    }

    opened64 = NULL;
    status = RegOpenKeyExA(pe32_hklm, test_path, 0,
                           KEY_READ | KEY_WOW64_64KEY, &opened64);
    reg_test_expect(status == ERROR_SUCCESS && opened64 != NULL,
                    "open 64-bit view through zero-extended HKLM",
                    &checks, &failures);
    if (status == ERROR_SUCCESS) {
        type = 0;
        size = sizeof(observed);
        observed = 0;
        status = RegQueryValueExA(opened64, value_name, NULL, &type,
                                  (BYTE *)&observed, &size);
        reg_test_expect(status == ERROR_SUCCESS && type == REG_DWORD &&
                        size == sizeof(observed) && observed == value64,
                        "read 64-bit value across root aliases",
                        &checks, &failures);
    }

    if (opened32) {
        type = 0;
        size = sizeof(observed);
        observed = 0;
        status = RegQueryValueExA(opened32, value_name, NULL, &type,
                                  (BYTE *)&observed, &size);
        reg_test_expect(status == ERROR_SUCCESS && observed == value32,
                        "32-bit value survives 64-bit write",
                        &checks, &failures);
    }

    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, delete_path, 0, NULL, 0,
                             KEY_READ | KEY_WRITE | KEY_WOW64_32KEY,
                             NULL, &delete32, &disposition);
    reg_test_expect(status == ERROR_SUCCESS && delete32 != NULL,
                    "create deletable 32-bit key", &checks, &failures);
    if (delete32) {
        status = RegCreateKeyExA(delete32, "Child", 0, NULL, 0,
                                 KEY_READ | KEY_WRITE | KEY_WOW64_32KEY,
                                 NULL, &delete32_child, &disposition);
        reg_test_expect(status == ERROR_SUCCESS && delete32_child != NULL,
                        "create child for delete guard", &checks, &failures);
    }

    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, delete_path, 0, NULL, 0,
                             KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                             NULL, &delete64, &disposition);
    reg_test_expect(status == ERROR_SUCCESS && delete64 != NULL,
                    "create matching 64-bit key", &checks, &failures);
    if (delete64)
        RegSetValueExA(delete64, value_name, 0, REG_DWORD,
                       (const BYTE *)&value64, sizeof(value64));

    status = RegDeleteKeyExA(HKEY_LOCAL_MACHINE, delete_path,
                             KEY_WOW64_32KEY, 1);
    reg_test_expect(status == ERROR_INVALID_PARAMETER,
                    "delete rejects nonzero reserved", &checks, &failures);
    status = RegDeleteKeyExA(HKEY_LOCAL_MACHINE, delete_path,
                             KEY_WOW64_32KEY, 0);
    reg_test_expect(status == ERROR_ACCESS_DENIED,
                    "delete rejects key with child", &checks, &failures);
    if (delete32) {
        status = RegDeleteKeyExA(delete32, "Child", KEY_WOW64_32KEY, 0);
        reg_test_expect(status == ERROR_SUCCESS,
                        "delete child key", &checks, &failures);
    }
    status = RegDeleteKeyExA(HKEY_LOCAL_MACHINE, delete_path,
                             KEY_WOW64_32KEY, 0);
    reg_test_expect(status == ERROR_SUCCESS,
                    "delete selected 32-bit view", &checks, &failures);

    opened_delete = NULL;
    status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, delete_path, 0,
                           KEY_READ | KEY_WOW64_32KEY, &opened_delete);
    reg_test_expect(status == ERROR_FILE_NOT_FOUND && opened_delete == NULL,
                    "deleted 32-bit view stays absent", &checks, &failures);
    status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, delete_path, 0,
                           KEY_READ | KEY_WOW64_64KEY, &opened_delete);
    reg_test_expect(status == ERROR_SUCCESS && opened_delete != NULL,
                    "64-bit view survives 32-bit delete", &checks, &failures);
    status = RegDeleteKeyExA(HKEY_LOCAL_MACHINE, delete_path,
                             KEY_WOW64_64KEY, 0);
    reg_test_expect(status == ERROR_SUCCESS,
                    "delete 64-bit view with values", &checks, &failures);

    if (key32) RegDeleteValueA(key32, value_name);
    if (key64) RegDeleteValueA(key64, value_name);
    if (opened32) RegCloseKey(opened32);
    if (opened64) RegCloseKey(opened64);
    if (key32) RegCloseKey(key32);
    if (key64) RegCloseKey(key64);

    serial_puts("[REGTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static BYTE WINAPI SystemFunction036(PVOID buffer, ULONG length)
{
    if (!buffer && length) return FALSE;
    random_get_bytes(buffer, length);
    return TRUE;
}

#define BCRYPT_RNG_USE_ENTROPY_IN_BUFFER 0x00000001U
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG  0x00000002U

static NTSTATUS WINAPI BCryptGenRandom(PVOID algorithm, BYTE *buffer,
                                       ULONG length, ULONG flags)
{
    if ((!buffer && length) ||
        (flags & ~(BCRYPT_RNG_USE_ENTROPY_IN_BUFFER |
                   BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
        (!algorithm && !(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return STATUS_INVALID_PARAMETER;

    random_get_bytes(buffer, length);
    return STATUS_SUCCESS;
}

/* Legacy CryptoAPI provider contexts. Steam still uses this API alongside
 * BCryptGenRandom. These handles model provider lifetime and ownership; key
 * container and key-pair operations are outside the implemented CSP surface. */
#define CRYPT_VERIFYCONTEXT              0xF0000000U
#define CRYPT_NEWKEYSET                  0x00000008U
#define CRYPT_DELETEKEYSET               0x00000010U
#define CRYPT_MACHINE_KEYSET             0x00000020U
#define CRYPT_SILENT                     0x00000040U
#define CRYPT_DEFAULT_CONTAINER_OPTIONAL 0x00000080U

#define PROV_RSA_FULL      1U
#define PROV_RSA_SCHANNEL 12U
#define PROV_RNG          21U
#define PROV_RSA_AES      24U

#define NTE_BAD_UID       0x80090001U
#define NTE_BAD_FLAGS     0x80090009U
#define NTE_NO_MEMORY     0x8009000EU
#define NTE_BAD_KEYSET    0x80090016U
#define NTE_PROV_TYPE_NOT_DEF 0x80090017U

#define MAX_CRYPT_PROVIDERS 128
#define CRYPT_HANDLE_PREFIX      0xA7C00000U
#define CRYPT_HANDLE_PREFIX_MASK 0xFFF00080U

typedef struct {
    HCRYPTPROV handle;
    DWORD owner_pid;
    DWORD provider_type;
    DWORD flags;
    USHORT generation;
    BOOL used;
} CRYPT_PROVIDER_CONTEXT;

static CRYPT_PROVIDER_CONTEXT crypt_providers[MAX_CRYPT_PROVIDERS];
static spinlock_t crypt_provider_lock = SPINLOCK_INIT;
static volatile ULONG crypt_acquire_log_count;
static volatile ULONG crypt_random_log_count;
static volatile ULONG crypt_release_log_count;

static uint64_t crypt_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&crypt_provider_lock);
    return flags;
}

static void crypt_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&crypt_provider_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static DWORD crypt_current_process_id(void)
{
    DWORD process_id = win32_current_process_id();
    return process_id ? process_id : 1;
}

static BOOL crypt_provider_type_supported(DWORD provider_type)
{
    return provider_type == PROV_RSA_FULL ||
           provider_type == PROV_RSA_SCHANNEL ||
           provider_type == PROV_RNG ||
           provider_type == PROV_RSA_AES;
}

static void crypt_store_provider(PHCRYPTPROV out, HCRYPTPROV provider)
{
    if (g_compat32_mode)
        *(DWORD *)(void *)out = (DWORD)provider;
    else
        *out = provider;
}

static int crypt_provider_index(HCRYPTPROV provider)
{
    DWORD value = (DWORD)provider;
    if ((value & CRYPT_HANDLE_PREFIX_MASK) != CRYPT_HANDLE_PREFIX)
        return -1;
    return (int)(value & 0x7FU);
}

static CRYPT_PROVIDER_CONTEXT *crypt_provider_lookup_locked(
    HCRYPTPROV provider, DWORD owner_pid)
{
    int index = crypt_provider_index(provider);
    if (index < 0 || index >= MAX_CRYPT_PROVIDERS)
        return NULL;
    CRYPT_PROVIDER_CONTEXT *context = &crypt_providers[index];
    if (!context->used || context->handle != provider ||
        context->owner_pid != owner_pid)
        return NULL;
    return context;
}

static BOOL crypt_acquire_context(PHCRYPTPROV out, BOOL named_container,
                                  BOOL named_provider, DWORD provider_type,
                                  DWORD flags)
{
    const DWORD allowed_flags = CRYPT_VERIFYCONTEXT | CRYPT_NEWKEYSET |
        CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET | CRYPT_SILENT |
        CRYPT_DEFAULT_CONTAINER_OPTIONAL;

    if (!out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    crypt_store_provider(out, 0);

    if (!crypt_provider_type_supported(provider_type)) {
        SetLastError(NTE_PROV_TYPE_NOT_DEF);
        return FALSE;
    }
    if ((flags & ~allowed_flags) ||
        ((flags & CRYPT_NEWKEYSET) &&
         (flags & (CRYPT_VERIFYCONTEXT | CRYPT_DELETEKEYSET))) ||
        ((flags & CRYPT_DELETEKEYSET) &&
         (flags & CRYPT_VERIFYCONTEXT))) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    if (flags & CRYPT_DELETEKEYSET) {
        /* Persisted key containers are not exposed by this provider yet. */
        SetLastError(NTE_BAD_KEYSET);
        return FALSE;
    }

    DWORD owner_pid = crypt_current_process_id();
    HCRYPTPROV provider = 0;
    uint64_t irq_flags = crypt_lock_irqsave();
    for (int i = 0; i < MAX_CRYPT_PROVIDERS; i++) {
        CRYPT_PROVIDER_CONTEXT *context = &crypt_providers[i];
        if (context->used)
            continue;

        USHORT generation = (USHORT)((context->generation + 1U) & 0x0FFFU);
        if (!generation) generation = 1;
        provider = (HCRYPTPROV)(CRYPT_HANDLE_PREFIX |
            ((DWORD)generation << 8) | (DWORD)i);
        context->handle = provider;
        context->owner_pid = owner_pid;
        context->provider_type = provider_type;
        context->flags = flags;
        context->generation = generation;
        context->used = TRUE;
        break;
    }
    crypt_unlock_irqrestore(irq_flags);

    if (!provider) {
        SetLastError(NTE_NO_MEMORY);
        return FALSE;
    }

    crypt_store_provider(out, provider);
    if (__atomic_fetch_add(&crypt_acquire_log_count, 1, __ATOMIC_RELAXED) < 64) {
        serial_puts("[ADVAPI-CRYPT] acquire pid=");
        serial_putdec(owner_pid);
        serial_puts(" type=");
        serial_putdec(provider_type);
        serial_puts(" flags=0x");
        serial_puthex(flags, 8);
        serial_puts(named_container ? " container=named" : " container=default");
        serial_puts(named_provider ? " provider=named" : " provider=default");
        serial_puts(" handle=0x");
        serial_puthex(provider, 8);
        serial_puts("\n");
    }
    return TRUE;
}

BOOL WINAPI CryptAcquireContextA(PHCRYPTPROV out, PCSTR container,
                                 PCSTR provider, DWORD provider_type,
                                 DWORD flags)
{
    return crypt_acquire_context(out, container && container[0],
                                 provider && provider[0], provider_type,
                                 flags);
}

BOOL WINAPI CryptAcquireContextW(PHCRYPTPROV out, PCWSTR container,
                                 PCWSTR provider, DWORD provider_type,
                                 DWORD flags)
{
    return crypt_acquire_context(out, container && container[0],
                                 provider && provider[0], provider_type,
                                 flags);
}

BOOL WINAPI CryptGenRandom(HCRYPTPROV provider, DWORD length, BYTE *buffer)
{
    if (!buffer && length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD owner_pid = crypt_current_process_id();
    uint64_t irq_flags = crypt_lock_irqsave();
    BOOL valid = crypt_provider_lookup_locked(provider, owner_pid) != NULL;
    crypt_unlock_irqrestore(irq_flags);
    if (!valid) {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    random_get_bytes(buffer, length);
    if (__atomic_fetch_add(&crypt_random_log_count, 1, __ATOMIC_RELAXED) < 64) {
        serial_puts("[ADVAPI-CRYPT] random pid=");
        serial_putdec(owner_pid);
        serial_puts(" handle=0x");
        serial_puthex(provider, 8);
        serial_puts(" bytes=");
        serial_putdec(length);
        serial_puts("\n");
    }
    return TRUE;
}

BOOL WINAPI CryptReleaseContext(HCRYPTPROV provider, DWORD flags)
{
    if (flags) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    DWORD owner_pid = crypt_current_process_id();
    uint64_t irq_flags = crypt_lock_irqsave();
    CRYPT_PROVIDER_CONTEXT *context =
        crypt_provider_lookup_locked(provider, owner_pid);
    if (context) {
        context->used = FALSE;
        context->handle = 0;
        context->owner_pid = 0;
        context->provider_type = 0;
        context->flags = 0;
    }
    crypt_unlock_irqrestore(irq_flags);
    if (!context) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (__atomic_fetch_add(&crypt_release_log_count, 1,
                           __ATOMIC_RELAXED) < 64) {
        serial_puts("[ADVAPI-CRYPT] release pid=");
        serial_putdec(owner_pid);
        serial_puts(" handle=0x");
        serial_puthex(provider, 8);
        serial_puts("\n");
    }
    return TRUE;
}

/* Hash state is process-owned and independent of application pointers. The
 * provider lock protects identities/state transitions, never a bulk digest. */
#define MAX_CRYPT_HASHES 256
enum { CRYPT_HASH_FREE, CRYPT_HASH_READY, CRYPT_HASH_BUSY };
typedef struct {
    ULONG_PTR handle;
    HCRYPTPROV provider;
    DWORD owner, algorithm, size, state;
    BOOL finished;
    union {
        br_md5_context md5;
        br_sha1_context sha1;
        br_sha256_context sha256;
        br_sha512_context sha512;
    } value;
} CRYPT_HASH_CONTEXT;
static CRYPT_HASH_CONTEXT crypt_hashes[MAX_CRYPT_HASHES];
static DWORD crypt_next_hash = 0xB1000000U;

static CRYPT_HASH_CONTEXT *crypt_hash_acquire(ULONG_PTR handle)
{
    if (!handle || handle > 0xffffffffU) return NULL;
    DWORD owner = crypt_current_process_id();
    for (;;) {
        uint64_t irq_flags = crypt_lock_irqsave();
        CRYPT_HASH_CONTEXT *found = NULL;
        BOOL busy = FALSE;
        for (unsigned i = 0; i < MAX_CRYPT_HASHES; i++) {
            CRYPT_HASH_CONTEXT *context = &crypt_hashes[i];
            if (context->state == CRYPT_HASH_FREE || context->handle != handle ||
                context->owner != owner) continue;
            if (context->state == CRYPT_HASH_READY) {
                context->state = CRYPT_HASH_BUSY;
                found = context;
            } else busy = TRUE;
            break;
        }
        crypt_unlock_irqrestore(irq_flags);
        if (found || !busy) return found;
        sched_yield();
    }
}

static BOOL crypt_hash_finish(CRYPT_HASH_CONTEXT *context, DWORD error)
{
    if (context) {
        uint64_t irq_flags = crypt_lock_irqsave();
        context->state = CRYPT_HASH_READY;
        crypt_unlock_irqrestore(irq_flags);
    }
    if (error) SetLastError(error);
    return error == 0;
}

static BOOL WINAPI CryptCreateHash(HCRYPTPROV provider, DWORD algorithm,
                                    ULONG_PTR key, DWORD flags, PVOID output)
{
    if (!win32_user_range_writable(output, g_compat32_mode ? 4 : 8, g_compat32_mode)) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    DWORD owner = crypt_current_process_id();
    uint64_t irq_flags = crypt_lock_irqsave();
    CRYPT_PROVIDER_CONTEXT *provider_context = crypt_provider_lookup_locked(provider, owner);
    DWORD error = 0, size = 0;
    CRYPT_HASH_CONTEXT *context = NULL;
    if (!provider_context) error = ERROR_INVALID_PARAMETER;
    else if (flags) error = NTE_BAD_FLAGS;
    else if (key) error = 0x80090003U; /* NTE_BAD_KEY: no keyed hash surface. */
    else {
        DWORD type = provider_context->provider_type;
        if (type == PROV_RSA_FULL || type == PROV_RSA_AES) {
            if (algorithm == 0x8003) size = 16; /* CALG_MD5 */
            if (algorithm == 0x8004) size = 20; /* CALG_SHA1 */
            if (type == PROV_RSA_AES) {
                if (algorithm == 0x800c) size = 32;
                if (algorithm == 0x800d) size = 48;
                if (algorithm == 0x800e) size = 64;
            }
        }
        if (!size) error = 0x80090008U; /* NTE_BAD_ALGID */
        else {
            for (unsigned i = 0; i < MAX_CRYPT_HASHES; i++)
                if (crypt_hashes[i].state == CRYPT_HASH_FREE) { context = &crypt_hashes[i]; break; }
            if (!context || crypt_next_hash == 0xffffffffU) error = NTE_NO_MEMORY;
        }
    }
    if (!error) {
        memset(context, 0, sizeof(*context));
        context->handle = ++crypt_next_hash;
        context->provider = provider;
        context->owner = owner;
        context->algorithm = algorithm;
        context->size = size;
        context->state = CRYPT_HASH_BUSY;
    }
    crypt_unlock_irqrestore(irq_flags);
    if (error) { SetLastError(error); return FALSE; }
    switch (algorithm) {
    case 0x8003: br_md5_init(&context->value.md5); break;
    case 0x8004: br_sha1_init(&context->value.sha1); break;
    case 0x800c: br_sha256_init(&context->value.sha256); break;
    case 0x800d: br_sha384_init(&context->value.sha512); break;
    case 0x800e: br_sha512_init(&context->value.sha512); break;
    }
    crypt_store_provider(output, context->handle);
    return crypt_hash_finish(context, 0);
}

static BOOL WINAPI CryptHashData(ULONG_PTR handle, const BYTE *data,
                                  DWORD length, DWORD flags)
{
    CRYPT_HASH_CONTEXT *context = crypt_hash_acquire(handle);
    if (!context) return crypt_hash_finish(NULL, ERROR_INVALID_PARAMETER);
    if (flags & ~1U) return crypt_hash_finish(context, NTE_BAD_FLAGS);
    /* The Microsoft CSP ignores CRYPT_USERDATA. A zero-byte update is also
     * accepted after finalization; it does not change the hash state. */
    if (!length) return crypt_hash_finish(context, 0);
    if (context->finished) return crypt_hash_finish(context, 0x8009000cU);
    if (!win32_user_range_readable(data, length, g_compat32_mode))
        return crypt_hash_finish(context, ERROR_INVALID_PARAMETER);
    for (DWORD offset = 0; offset < length;) {
        DWORD bytes = length - offset;
        if (bytes > 65536U) bytes = 65536U;
        switch (context->algorithm) {
        case 0x8003: br_md5_update(&context->value.md5, data + offset, bytes); break;
        case 0x8004: br_sha1_update(&context->value.sha1, data + offset, bytes); break;
        case 0x800c: br_sha256_update(&context->value.sha256, data + offset, bytes); break;
        case 0x800d: case 0x800e:
            br_sha512_update(&context->value.sha512, data + offset, bytes); break;
        }
        offset += bytes;
        if (offset < length) sched_yield();
    }
    return crypt_hash_finish(context, 0);
}

static BOOL WINAPI CryptGetHashParam(ULONG_PTR handle, DWORD parameter,
                                      BYTE *data, DWORD *length, DWORD flags)
{
    CRYPT_HASH_CONTEXT *context = crypt_hash_acquire(handle);
    if (!context) return crypt_hash_finish(NULL, ERROR_INVALID_PARAMETER);
    if (flags) return crypt_hash_finish(context, NTE_BAD_FLAGS);
    if (parameter != 1 && parameter != 2 && parameter != 4)
        return crypt_hash_finish(context, 0x8009000aU); /* NTE_BAD_TYPE */
    if (!win32_user_range_writable(length, sizeof(*length), g_compat32_mode))
        return crypt_hash_finish(context, ERROR_INVALID_PARAMETER);
    DWORD needed = parameter == 2 ? context->size : 4;
    DWORD capacity = *length;
    *length = needed;
    if (!data) return crypt_hash_finish(context, 0);
    if (capacity < needed) return crypt_hash_finish(context, ERROR_MORE_DATA);
    if (!win32_user_range_writable(data, needed, g_compat32_mode))
        return crypt_hash_finish(context, ERROR_INVALID_PARAMETER);
    if (parameter == 1) memcpy(data, &context->algorithm, sizeof(DWORD));
    else if (parameter == 4) memcpy(data, &context->size, sizeof(DWORD));
    else {
        switch (context->algorithm) {
        case 0x8003: br_md5_out(&context->value.md5, data); break;
        case 0x8004: br_sha1_out(&context->value.sha1, data); break;
        case 0x800c: br_sha256_out(&context->value.sha256, data); break;
        case 0x800d: br_sha384_out(&context->value.sha512, data); break;
        case 0x800e: br_sha512_out(&context->value.sha512, data); break;
        }
        context->finished = TRUE;
    }
    return crypt_hash_finish(context, 0);
}

static BOOL WINAPI CryptDestroyHash(ULONG_PTR handle)
{
    CRYPT_HASH_CONTEXT *context = crypt_hash_acquire(handle);
    if (!context) return crypt_hash_finish(NULL, ERROR_INVALID_PARAMETER);
    uint64_t irq_flags = crypt_lock_irqsave();
    memset(context, 0, sizeof(*context));
    crypt_unlock_irqrestore(irq_flags);
    return TRUE;
}

DWORD advapi32_crypto_release_process(DWORD process_id)
{
    if (!process_id) return 0;

    DWORD released = 0;
    uint64_t irq_flags = crypt_lock_irqsave();
    /* All owned workers have stopped before this hook. Hash contexts may
     * have been abandoned while busy, so reclaim every state for the owner. */
    for (unsigned i = 0; i < MAX_CRYPT_HASHES; i++)
        if (crypt_hashes[i].state != CRYPT_HASH_FREE && crypt_hashes[i].owner == process_id)
            memset(&crypt_hashes[i], 0, sizeof(crypt_hashes[i]));
    for (int i = 0; i < MAX_CRYPT_PROVIDERS; i++) {
        CRYPT_PROVIDER_CONTEXT *context = &crypt_providers[i];
        if (!context->used || context->owner_pid != process_id)
            continue;
        context->used = FALSE;
        context->handle = 0;
        context->owner_pid = 0;
        context->provider_type = 0;
        context->flags = 0;
        released++;
    }
    crypt_unlock_irqrestore(irq_flags);

    if (released) {
        serial_puts("[ADVAPI-CRYPT] process cleanup pid=");
        serial_putdec(process_id);
        serial_puts(" contexts=");
        serial_putdec(released);
        serial_puts("\n");
    }
    return released;
}

#define SECURITY_DESCRIPTOR_REVISION 1
#define ACL_REVISION                 2
#define SE_OWNER_DEFAULTED           0x0001
#define SE_GROUP_DEFAULTED           0x0002
#define SE_DACL_PRESENT              0x0004
#define SE_DACL_DEFAULTED            0x0008
#define SE_SACL_PRESENT              0x0010
#define SE_SACL_DEFAULTED            0x0020
#define SE_SELF_RELATIVE             0x8000

#define OWNER_SECURITY_INFORMATION          0x00000001U
#define GROUP_SECURITY_INFORMATION          0x00000002U
#define DACL_SECURITY_INFORMATION           0x00000004U
#define SACL_SECURITY_INFORMATION           0x00000008U
#define UNPROTECTED_DACL_SECURITY_INFORMATION 0x20000000U
#define PROTECTED_DACL_SECURITY_INFORMATION   0x80000000U

#define ERROR_NOT_SUPPORTED           50U
#define ERROR_FILENAME_EXCED_RANGE    206U
#define ERROR_NONE_MAPPED             1332U
#define ERROR_INVALID_ACL             1336U
#define ERROR_INVALID_SECURITY_DESCR  1338U
#define ERROR_PRIVILEGE_NOT_HELD      1314U

typedef struct {
    UCHAR revision;
    UCHAR sbz1;
    USHORT control;
    DWORD owner, group, sacl, dacl;
} SECURITY_DESCRIPTOR32;

typedef struct {
    UCHAR revision;
    UCHAR sbz1;
    USHORT control;
    PVOID owner, group, sacl, dacl;
} SECURITY_DESCRIPTOR64;

_Static_assert(sizeof(SECURITY_DESCRIPTOR32) == 20, "Win32 security descriptor layout");
_Static_assert(sizeof(SECURITY_DESCRIPTOR64) == 40, "Win64 security descriptor layout");

typedef struct {
    UCHAR revision;
    UCHAR sub_authority_count;
    UCHAR identifier_authority[6];
    DWORD sub_authority[2];
} BUILTIN_USERS_SID;

typedef struct {
    UCHAR revision;
    UCHAR sub_authority_count;
    UCHAR identifier_authority[6];
    DWORD sub_authority[5];
} LOCAL_USER_SID;

typedef struct {
    UCHAR revision;
    UCHAR sub_authority_count;
    UCHAR identifier_authority[6];
    DWORD sub_authority[1];
} LOCAL_INTEGRITY_SID;

typedef struct {
    UCHAR revision;
    UCHAR sbz1;
    USHORT size;
    USHORT ace_count;
    USHORT sbz2;
} ACL_HEADER;

typedef struct {
    DWORD acl_revision;
} ACL_REVISION_INFORMATION;

typedef struct {
    DWORD ace_count;
    DWORD acl_bytes_in_use;
    DWORD acl_bytes_free;
} ACL_SIZE_INFORMATION;

_Static_assert(sizeof(BUILTIN_USERS_SID) == 16, "built-in users SID layout");
_Static_assert(sizeof(LOCAL_USER_SID) == 28, "local user SID layout");
_Static_assert(sizeof(ACL_HEADER) == 8, "ACL header layout");
_Static_assert(sizeof(ACL_REVISION_INFORMATION) == 4,
               "ACL revision information layout");
_Static_assert(sizeof(ACL_SIZE_INFORMATION) == 12,
               "ACL size information layout");

static const LOCAL_USER_SID g_local_user_sid = {
    .revision = 1,
    .sub_authority_count = 5,
    .identifier_authority = {0, 0, 0, 0, 0, 5},
    .sub_authority = {21, 0x4F534954, 0x4F4B0001, 1, 1000},
};
static const LOCAL_INTEGRITY_SID g_local_integrity_sid = {
    .revision = 1,
    .sub_authority_count = 1,
    .identifier_authority = {0, 0, 0, 0, 0, 16},
    .sub_authority = {8192}, /* SECURITY_MANDATORY_MEDIUM_RID */
};
static const LOCAL_INTEGRITY_SID g_anonymous_sid = {
    .revision = 1,
    .sub_authority_count = 1,
    .identifier_authority = {0, 0, 0, 0, 0, 5},
    .sub_authority = {7}, /* SECURITY_ANONYMOUS_LOGON_RID */
};

typedef struct {
    PCVOID sid;
    DWORD sid_size;
    DWORD attributes;
} LOCAL_TOKEN_GROUP;

static const LOCAL_INTEGRITY_SID g_everyone_sid = {
    1, 1, {0, 0, 0, 0, 0, 1}, {0}
};
static const LOCAL_INTEGRITY_SID g_authenticated_users_sid = {
    1, 1, {0, 0, 0, 0, 0, 5}, {11}
};
static const BUILTIN_USERS_SID g_builtin_users_sid = {
    1, 2, {0, 0, 0, 0, 0, 5}, {32, 545}
};
static const BUILTIN_USERS_SID g_builtin_admin_sid = {
    1, 2, {0, 0, 0, 0, 0, 5}, {32, 544}
};
static const LOCAL_INTEGRITY_SID g_owner_rights_sid = {
    1, 1, {0, 0, 0, 0, 0, 3}, {4}
};
static const LOCAL_TOKEN_GROUP g_local_token_groups[] = {
    { &g_everyone_sid, sizeof(g_everyone_sid), 7 },
    { &g_authenticated_users_sid, sizeof(g_authenticated_users_sid), 7 },
    { &g_builtin_users_sid, sizeof(g_builtin_users_sid), 7 },
};

typedef struct {
    DWORD references;
    ACL_HEADER acl;
} TOKEN_DEFAULT_ACL;

typedef struct {
    DWORD token_type;
    DWORD impersonation_level;
    PCVOID user_sid;
    DWORD user_sid_size;
    ULONGLONG authentication_id;
    const LOCAL_TOKEN_GROUP *groups;
    DWORD group_count;
    DWORD user_attributes;
    const LOCAL_TOKEN_GROUP *restricted;
    DWORD restricted_count;
    DWORD filter_flags;
    BOOL has_restrictions;
    BOOL allocated;
    DWORD references;
    TOKEN_DEFAULT_ACL *default_dacl;
    ULONGLONG token_id;
    ULONGLONG modified_id;
    DWORD integrity_level;
} LOCAL_TOKEN_OBJECT;

/* Immutable identities may be shared by handles and thread impersonation
 * references. Duplicating changes type/level, not the source token. */
#define LOCAL_TOKEN_VARIANT(sid, group_list, count, type, level) { \
    .token_type = type, .impersonation_level = level, \
    .user_sid = &(sid), .user_sid_size = sizeof(sid), \
    .groups = group_list, .group_count = count, .integrity_level = 8192 }
#define LOCAL_TOKEN_VARIANTS(sid, groups, count) { \
    LOCAL_TOKEN_VARIANT(sid, groups, count, 1, 2), \
    LOCAL_TOKEN_VARIANT(sid, groups, count, 2, 0), \
    LOCAL_TOKEN_VARIANT(sid, groups, count, 2, 1), \
    LOCAL_TOKEN_VARIANT(sid, groups, count, 2, 2), \
    LOCAL_TOKEN_VARIANT(sid, groups, count, 2, 3) }
static const LOCAL_TOKEN_OBJECT g_local_tokens[2][5] = {
    LOCAL_TOKEN_VARIANTS(g_local_user_sid, g_local_token_groups, 3),
    LOCAL_TOKEN_VARIANTS(g_anonymous_sid, NULL, 0),
};
#undef LOCAL_TOKEN_VARIANTS
#undef LOCAL_TOKEN_VARIANT
#define g_local_primary_token       g_local_tokens[0][0]
#define g_local_impersonation_token g_local_tokens[0][3]
#define g_anonymous_token           g_local_tokens[1][3]
static DWORD g_token_live_objects;
static DWORD g_token_live_policies;
static ULONGLONG g_token_next_luid;
static spinlock_t g_token_policy_lock = SPINLOCK_INIT;

static uint64_t token_policy_lock(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&g_token_policy_lock);
    return flags;
}

static void token_policy_unlock(uint64_t flags)
{
    spin_unlock(&g_token_policy_lock);
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static void token_acl_release(TOKEN_DEFAULT_ACL *policy)
{
    if (policy && !__atomic_sub_fetch(&policy->references, 1, __ATOMIC_ACQ_REL)) {
        __atomic_sub_fetch(&g_token_live_policies, 1, __ATOMIC_RELAXED);
        kfree(policy);
    }
}

static TOKEN_DEFAULT_ACL *token_acl_reference(const LOCAL_TOKEN_OBJECT *token)
{
    uint64_t flags = token_policy_lock();
    TOKEN_DEFAULT_ACL *policy = token->default_dacl;
    if (policy) __atomic_add_fetch(&policy->references, 1, __ATOMIC_RELAXED);
    token_policy_unlock(flags);
    return policy;
}

void advapi32_token_retain(PCVOID object)
{
    LOCAL_TOKEN_OBJECT *token = (LOCAL_TOKEN_OBJECT *)object;
    if (token && token->allocated)
        __atomic_add_fetch(&token->references, 1, __ATOMIC_RELAXED);
}

void advapi32_token_release(PCVOID object)
{
    LOCAL_TOKEN_OBJECT *token = (LOCAL_TOKEN_OBJECT *)object;
    if (token && token->allocated &&
        !__atomic_sub_fetch(&token->references, 1, __ATOMIC_ACQ_REL)) {
        __atomic_sub_fetch(&g_token_live_objects, 1, __ATOMIC_RELAXED);
        token_acl_release(token->default_dacl);
        kfree(token);
    }
}

static LOCAL_TOKEN_OBJECT *token_copy(const LOCAL_TOKEN_OBJECT *source,
                                      const LOCAL_TOKEN_GROUP *restricted,
                                      DWORD restricted_count)
{
    ULONGLONG entries = (ULONGLONG)source->group_count + restricted_count;
    ULONGLONG size = sizeof(*source) + entries * sizeof(LOCAL_TOKEN_GROUP) +
                     source->user_sid_size;
    for (DWORD i = 0; i < source->group_count; i++) size += source->groups[i].sid_size;
    for (DWORD i = 0; i < restricted_count; i++) size += restricted[i].sid_size;
    /* TokenInformation lengths use DWORDs in both ABIs. */
    if (size > 0xffffffffU) { SetLastError(8); return NULL; }
    LOCAL_TOKEN_OBJECT *copy = kmalloc(size);
    if (!copy) { SetLastError(8); return NULL; }
    reg_memset(copy, 0, sizeof(*copy));
    copy->token_type = source->token_type;
    copy->impersonation_level = source->impersonation_level;
    copy->user_sid_size = source->user_sid_size;
    copy->authentication_id = source->authentication_id;
    copy->group_count = source->group_count;
    copy->user_attributes = source->user_attributes;
    copy->filter_flags = source->filter_flags;
    copy->has_restrictions = source->has_restrictions;
    uint64_t flags = token_policy_lock();
    copy->default_dacl = source->default_dacl;
    copy->integrity_level = source->integrity_level;
    if (copy->default_dacl)
        __atomic_add_fetch(&copy->default_dacl->references, 1, __ATOMIC_RELAXED);
    token_policy_unlock(flags);
    copy->token_id = __atomic_add_fetch(&g_token_next_luid, 1, __ATOMIC_RELAXED);
    copy->modified_id = __atomic_add_fetch(&g_token_next_luid, 1, __ATOMIC_RELAXED);
    copy->allocated = TRUE;
    copy->references = 1;
    __atomic_add_fetch(&g_token_live_objects, 1, __ATOMIC_RELAXED);
    LOCAL_TOKEN_GROUP *groups = (void *)(copy + 1);
    BYTE *sid = (void *)(groups + entries);
    copy->user_sid = sid;
    reg_memcpy(sid, source->user_sid, source->user_sid_size);
    sid += source->user_sid_size;
    copy->groups = groups;
    copy->restricted = groups + source->group_count;
    copy->restricted_count = restricted_count;
    for (ULONGLONG i = 0; i < entries; i++) {
        const LOCAL_TOKEN_GROUP *input = i < source->group_count
            ? &source->groups[i] : &restricted[i - source->group_count];
        groups[i] = *input;
        groups[i].sid = sid;
        reg_memcpy(sid, input->sid, input->sid_size);
        sid += input->sid_size;
    }
    return copy;
}

PVOID advapi32_token_create_primary(PCVOID object)
{
    const LOCAL_TOKEN_OBJECT *source = object ? object : &g_local_primary_token;
    LOCAL_TOKEN_OBJECT *copy = token_copy(source, source->restricted, source->restricted_count);
    if (copy) {
        copy->token_type = 1;
        copy->impersonation_level = 2;
    }
    return copy;
}

PVOID advapi32_token_create_admin_primary(void)
{
    static const LOCAL_TOKEN_GROUP groups[] = {
        { &g_everyone_sid, sizeof(g_everyone_sid), 7 },
        { &g_authenticated_users_sid, sizeof(g_authenticated_users_sid), 7 },
        { &g_builtin_users_sid, sizeof(g_builtin_users_sid), 7 },
        { &g_builtin_admin_sid, sizeof(g_builtin_admin_sid), 15 },
    };
    LOCAL_TOKEN_OBJECT source = g_local_primary_token;
    source.groups = groups;
    source.group_count = sizeof(groups) / sizeof(groups[0]);
    source.integrity_level = 12288; /* SECURITY_MANDATORY_HIGH_RID */
    return advapi32_token_create_primary(&source);
}

ULONGLONG advapi32_process_logon_id(void)
{
    /* The current token model has one local logon session. */
    return g_local_primary_token.authentication_id;
}

typedef struct {
    DWORD access_permissions;
    DWORD access_mode;
    DWORD inheritance;
    DWORD multiple_trustee;
    DWORD multiple_trustee_operation;
    DWORD trustee_form;
    DWORD trustee_type;
    DWORD trustee_name;
} EXPLICIT_ACCESS_W32;

typedef struct {
    DWORD access_permissions;
    DWORD access_mode;
    DWORD inheritance;
    DWORD padding0;
    PVOID multiple_trustee;
    DWORD multiple_trustee_operation;
    DWORD trustee_form;
    DWORD trustee_type;
    DWORD padding1;
    PWSTR trustee_name;
} EXPLICIT_ACCESS_W64;

_Static_assert(sizeof(EXPLICIT_ACCESS_W32) == 32,
               "Win32 explicit access layout");
_Static_assert(sizeof(EXPLICIT_ACCESS_W64) == 48,
               "Win64 explicit access layout");

static void WINAPI BuildExplicitAccessWithNameW_stub(
    PVOID explicit_access, PWSTR trustee_name, DWORD access_permissions,
    DWORD access_mode, DWORD inheritance)
{
    if (!explicit_access) return;

    if (g_compat32_mode) {
        EXPLICIT_ACCESS_W32 *entry = explicit_access;
        entry->access_permissions = access_permissions;
        entry->access_mode = access_mode;
        entry->inheritance = inheritance;
        entry->multiple_trustee = 0;
        entry->multiple_trustee_operation = 0; /* NO_MULTIPLE_TRUSTEE */
        entry->trustee_form = 1;               /* TRUSTEE_IS_NAME */
        entry->trustee_type = 0;               /* TRUSTEE_IS_UNKNOWN */
        entry->trustee_name = (DWORD)(ULONG_PTR)trustee_name;
    } else {
        EXPLICIT_ACCESS_W64 *entry = explicit_access;
        entry->access_permissions = access_permissions;
        entry->access_mode = access_mode;
        entry->inheritance = inheritance;
        entry->padding0 = 0;
        entry->multiple_trustee = NULL;
        entry->multiple_trustee_operation = 0;
        entry->trustee_form = 1;
        entry->trustee_type = 0;
        entry->padding1 = 0;
        entry->trustee_name = trustee_name;
    }
}

static void WINAPI BuildTrusteeWithSidW_stub(PVOID trustee, PVOID sid)
{
    if (!trustee) return;
    if (g_compat32_mode) {
        DWORD *fields = trustee;
        fields[0] = 0;
        fields[1] = 0; /* NO_MULTIPLE_TRUSTEE */
        fields[2] = 0; /* TRUSTEE_IS_SID */
        fields[3] = 0; /* TRUSTEE_IS_UNKNOWN */
        fields[4] = (DWORD)(ULONG_PTR)sid;
    } else {
        BYTE *fields = trustee;
        *(PVOID *)(fields + 0) = NULL;
        *(DWORD *)(fields + 8) = 0;
        *(DWORD *)(fields + 12) = 0;
        *(DWORD *)(fields + 16) = 0;
        *(PVOID *)(fields + 24) = sid;
    }
}

static DWORD WINAPI GetLengthSid_stub(PVOID sid)
{
    const UCHAR *bytes = (const UCHAR *)sid;
    if (!bytes || bytes[0] != 1 || bytes[1] > 15) {
        SetLastError(1337); /* ERROR_INVALID_SID */
        return 0;
    }
    return 8 + 4 * bytes[1];
}

static UCHAR *WINAPI GetSidSubAuthorityCount_stub(PVOID sid)
{
    BYTE *bytes = (BYTE *)sid;
    if (!bytes)
        return NULL;
    return &bytes[1];
}

static DWORD *WINAPI GetSidSubAuthority_stub(PVOID sid, DWORD index)
{
    BYTE *bytes = (BYTE *)sid;
    if (!bytes)
        return NULL;
    return (DWORD *)(void *)(bytes + 8 + index * sizeof(DWORD));
}

static BOOL WINAPI IsValidSid_stub(PVOID sid)
{
    return GetLengthSid_stub(sid) != 0;
}

static BOOL WINAPI CreateWellKnownSid_stub(DWORD sid_type, PVOID domain_sid,
                                            PVOID sid, DWORD *sid_size)
{
    const DWORD required = sizeof(BUILTIN_USERS_SID);
    (void)domain_sid;

    if (!sid_size || sid_type != 26) { /* WinBuiltinUsersSid */
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!sid || *sid_size < required) {
        *sid_size = required;
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    BUILTIN_USERS_SID *users = (BUILTIN_USERS_SID *)sid;
    users->revision = 1;
    users->sub_authority_count = 2;
    for (int i = 0; i < 5; i++) users->identifier_authority[i] = 0;
    users->identifier_authority[5] = 5; /* SECURITY_NT_AUTHORITY */
    users->sub_authority[0] = 32;       /* SECURITY_BUILTIN_DOMAIN_RID */
    users->sub_authority[1] = 545;      /* DOMAIN_ALIAS_RID_USERS */
    *sid_size = required;
    return TRUE;
}

static BOOL WINAPI InitializeAcl_stub(PVOID acl, DWORD acl_length, DWORD revision)
{
    if (!acl || acl_length < sizeof(ACL_HEADER) || acl_length > 0xffff ||
        (revision != ACL_REVISION && revision != 4)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    ACL_HEADER *header = (ACL_HEADER *)acl;
    header->revision = (UCHAR)revision;
    header->sbz1 = 0;
    header->size = (USHORT)acl_length;
    header->ace_count = 0;
    header->sbz2 = 0;
    return TRUE;
}

static BOOL acl_measure(const ACL_HEADER *header, DWORD *bytes_in_use)
{
    if (!header ||
        header->revision < ACL_REVISION || header->revision > 4 ||
        header->size < sizeof(*header)) {
        SetLastError(1336); /* ERROR_INVALID_ACL */
        return FALSE;
    }

    DWORD used = sizeof(*header);
    for (USHORT i = 0; i < header->ace_count; i++) {
        if (used > header->size - sizeof(DWORD)) {
            SetLastError(1336);
            return FALSE;
        }
        const BYTE *ace = (const BYTE *)header + used;
        USHORT ace_size = *(const USHORT *)(const void *)(ace + 2);
        if (ace_size < sizeof(DWORD) || ace_size > header->size - used) {
            SetLastError(1336);
            return FALSE;
        }
        used += ace_size;
    }

    if (bytes_in_use)
        *bytes_in_use = used;
    return TRUE;
}

static BOOL WINAPI IsValidAcl_stub(PVOID acl)
{
    return acl_measure((const ACL_HEADER *)acl, NULL);
}

static void acl_store_pointer(PVOID output, PVOID value)
{
    if (g_compat32_mode)
        *(DWORD *)(void *)output = (DWORD)(ULONG_PTR)value;
    else
        *(PVOID *)output = value;
}

static BOOL WINAPI GetAce_stub(PVOID acl, DWORD ace_index, PVOID ace_output)
{
    ACL_HEADER *header = (ACL_HEADER *)acl;
    if (!ace_output) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    acl_store_pointer(ace_output, NULL);
    if (!acl_measure(header, NULL))
        return FALSE;
    if (ace_index >= header->ace_count) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD offset = sizeof(*header);
    for (DWORD i = 0; i < ace_index; i++)
        offset += *(const USHORT *)((const BYTE *)header + offset + 2);
    acl_store_pointer(ace_output, (BYTE *)header + offset);
    return TRUE;
}

static BOOL WINAPI GetAclInformation_stub(PVOID acl, PVOID information,
                                           DWORD information_length,
                                           DWORD information_class)
{
    ACL_HEADER *header = (ACL_HEADER *)acl;
    DWORD used = 0;
    if (!acl_measure(header, &used))
        return FALSE;

    if (information_class == 1) { /* AclRevisionInformation */
        if (!information || information_length <
                                sizeof(ACL_REVISION_INFORMATION)) {
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return FALSE;
        }
        ((ACL_REVISION_INFORMATION *)information)->acl_revision =
            header->revision;
        return TRUE;
    }
    if (information_class == 2) { /* AclSizeInformation */
        if (!information || information_length < sizeof(ACL_SIZE_INFORMATION)) {
            SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
            return FALSE;
        }
        ACL_SIZE_INFORMATION *size = (ACL_SIZE_INFORMATION *)information;
        size->ace_count = header->ace_count;
        size->acl_bytes_in_use = used;
        size->acl_bytes_free = header->size - used;
        return TRUE;
    }

    SetLastError(87); /* ERROR_INVALID_PARAMETER */
    return FALSE;
}

static BOOL WINAPI AddAce_stub(PVOID acl, DWORD revision,
                                DWORD starting_ace_index, PVOID ace_list,
                                DWORD ace_list_length)
{
    ACL_HEADER *header = (ACL_HEADER *)acl;
    DWORD used = 0;
    if (!acl_measure(header, &used))
        return FALSE;
    if ((revision != ACL_REVISION && revision != 4) || !ace_list ||
        ace_list_length < sizeof(DWORD)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (ace_list_length > header->size - used) {
        SetLastError(1344); /* ERROR_ALLOTTED_SPACE_EXCEEDED */
        return FALSE;
    }

    DWORD source_offset = 0;
    DWORD source_count = 0;
    while (source_offset < ace_list_length) {
        if (ace_list_length - source_offset < sizeof(DWORD)) {
            SetLastError(87);
            return FALSE;
        }
        const BYTE *ace = (const BYTE *)ace_list + source_offset;
        USHORT ace_size = *(const USHORT *)(const void *)(ace + 2);
        if (ace_size < sizeof(DWORD) ||
            ace_size > ace_list_length - source_offset) {
            SetLastError(87);
            return FALSE;
        }
        source_offset += ace_size;
        source_count++;
    }
    if (source_offset != ace_list_length) {
        SetLastError(87);
        return FALSE;
    }
    if (source_count > 0xffffU - header->ace_count) {
        SetLastError(1344); /* ERROR_ALLOTTED_SPACE_EXCEEDED */
        return FALSE;
    }

    BYTE *copy = LocalAlloc(0, ace_list_length);
    if (!copy) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    reg_memcpy(copy, ace_list, ace_list_length);

    DWORD insert_offset = sizeof(*header);
    DWORD insertion_index = starting_ace_index;
    if (insertion_index > header->ace_count)
        insertion_index = header->ace_count;
    for (DWORD i = 0; i < insertion_index; i++)
        insert_offset += *(const USHORT *)((const BYTE *)header +
                                           insert_offset + 2);

    BYTE *bytes = (BYTE *)header;
    for (DWORD offset = used; offset > insert_offset; offset--)
        bytes[offset + ace_list_length - 1] = bytes[offset - 1];
    reg_memcpy(bytes + insert_offset, copy, ace_list_length);
    LocalFree(copy);

    header->ace_count = (USHORT)(header->ace_count + source_count);
    if (revision > header->revision)
        header->revision = (UCHAR)revision;
    return TRUE;
}

enum {
    ACL_GRANT_ACCESS       = 1,
    ACL_SET_ACCESS         = 2,
    ACL_DENY_ACCESS        = 3,
    ACL_REVOKE_ACCESS      = 4,
    ACL_SET_AUDIT_SUCCESS  = 5,
    ACL_SET_AUDIT_FAILURE  = 6,
};

typedef struct {
    DWORD access_permissions;
    DWORD access_mode;
    DWORD inheritance;
    DWORD trustee_form;
    PVOID trustee;
} ACL_EXPLICIT_ENTRY;

static void acl_read_explicit_entry(PVOID entries, ULONG index,
                                    ACL_EXPLICIT_ENTRY *out)
{
    if (g_compat32_mode) {
        const EXPLICIT_ACCESS_W32 *entry =
            &((const EXPLICIT_ACCESS_W32 *)entries)[index];
        out->access_permissions = entry->access_permissions;
        out->access_mode = entry->access_mode;
        out->inheritance = entry->inheritance;
        out->trustee_form = entry->trustee_form;
        out->trustee = (PVOID)(ULONG_PTR)entry->trustee_name;
    } else {
        const EXPLICIT_ACCESS_W64 *entry =
            &((const EXPLICIT_ACCESS_W64 *)entries)[index];
        out->access_permissions = entry->access_permissions;
        out->access_mode = entry->access_mode;
        out->inheritance = entry->inheritance;
        out->trustee_form = entry->trustee_form;
        out->trustee = entry->trustee_name;
    }
}

static PVOID acl_entry_sid(const ACL_EXPLICIT_ENTRY *entry)
{
    if (entry->trustee_form == 0) /* TRUSTEE_IS_SID */
        return entry->trustee;
    if (entry->trustee_form == 1) /* TRUSTEE_IS_NAME */
        return (PVOID)&g_local_user_sid;
    return NULL;
}

static DWORD acl_used_size(const ACL_HEADER *acl)
{
    DWORD used = 0;
    return acl_measure(acl, &used) ? used : 0;
}

static void acl_append_explicit_ace(BYTE *destination, DWORD *used,
                                    const ACL_EXPLICIT_ENTRY *entry,
                                    const void *sid, DWORD sid_size)
{
    BYTE *ace = destination + *used;
    if (entry->access_mode == ACL_DENY_ACCESS)
        ace[0] = 1; /* ACCESS_DENIED_ACE_TYPE */
    else if (entry->access_mode == ACL_SET_AUDIT_SUCCESS ||
             entry->access_mode == ACL_SET_AUDIT_FAILURE)
        ace[0] = 2; /* SYSTEM_AUDIT_ACE_TYPE */
    else
        ace[0] = 0; /* ACCESS_ALLOWED_ACE_TYPE */

    ace[1] = (BYTE)entry->inheritance;
    if (entry->access_mode == ACL_SET_AUDIT_SUCCESS)
        ace[1] |= 0x40; /* SUCCESSFUL_ACCESS_ACE_FLAG */
    else if (entry->access_mode == ACL_SET_AUDIT_FAILURE)
        ace[1] |= 0x80; /* FAILED_ACCESS_ACE_FLAG */

    *(USHORT *)(ace + 2) = (USHORT)(8 + sid_size);
    *(DWORD *)(ace + 4) = entry->access_permissions;
    reg_memcpy(ace + 8, sid, sid_size);
    *used += 8 + sid_size;
}

static DWORD WINAPI SetEntriesInAclW_stub(ULONG count, PVOID entries,
                                           PVOID old_acl, PVOID new_acl)
{
    if (!new_acl || (count && !entries))
        return 87; /* ERROR_INVALID_PARAMETER */

    const ACL_HEADER *old_header = (const ACL_HEADER *)old_acl;
    DWORD old_used = sizeof(ACL_HEADER);
    UCHAR revision = ACL_REVISION;
    if (old_header) {
        if (!IsValidAcl_stub((PVOID)old_header))
            return 1336; /* ERROR_INVALID_ACL */
        old_used = acl_used_size(old_header);
        if (!old_used)
            return 1336;
        revision = old_header->revision;
    }

    DWORD required = old_used;
    USHORT additions = 0;
    for (ULONG i = 0; i < count; i++) {
        ACL_EXPLICIT_ENTRY entry;
        acl_read_explicit_entry(entries, i, &entry);
        if (entry.access_mode == ACL_REVOKE_ACCESS)
            continue;
        if (entry.access_mode < ACL_GRANT_ACCESS ||
            entry.access_mode > ACL_SET_AUDIT_FAILURE)
            return 87;

        PVOID sid = acl_entry_sid(&entry);
        DWORD sid_size = GetLengthSid_stub(sid);
        if (!sid_size)
            return 1337; /* ERROR_INVALID_SID */
        if (required > 0xffffU - 8U - sid_size)
            return 1344; /* ERROR_ALLOTTED_SPACE_EXCEEDED */
        required += 8 + sid_size;
        additions++;
    }

    ACL_HEADER *acl = LocalAlloc(0x0040, required);
    if (!acl)
        return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    acl->revision = revision;
    acl->sbz1 = 0;
    acl->size = (USHORT)required;
    acl->ace_count = additions;
    acl->sbz2 = 0;

    DWORD used = sizeof(*acl);
    for (ULONG pass = 0; pass < 2; pass++) {
        for (ULONG i = 0; i < count; i++) {
            ACL_EXPLICIT_ENTRY entry;
            acl_read_explicit_entry(entries, i, &entry);
            if (entry.access_mode == ACL_REVOKE_ACCESS)
                continue;
            BOOL is_deny = entry.access_mode == ACL_DENY_ACCESS;
            if ((pass == 0) != is_deny)
                continue;
            PVOID sid = acl_entry_sid(&entry);
            DWORD sid_size = GetLengthSid_stub(sid);
            acl_append_explicit_ace((BYTE *)acl, &used, &entry, sid, sid_size);
        }
        if (pass == 0 && old_header && old_used > sizeof(*old_header)) {
            DWORD old_aces_size = old_used - sizeof(*old_header);
            reg_memcpy((BYTE *)acl + used,
                       (const BYTE *)old_header + sizeof(*old_header),
                       old_aces_size);
            used += old_aces_size;
            acl->ace_count += old_header->ace_count;
        }
    }

    if (g_compat32_mode)
        *(DWORD *)new_acl = (DWORD)(ULONG_PTR)acl;
    else
        *(PVOID *)new_acl = acl;
    return ERROR_SUCCESS;
}

static DWORD acl_explicit_count(const ACL_HEADER *acl, ULONG *entry_count,
                                SIZE_T *sid_bytes)
{
    DWORD offset = sizeof(*acl);
    ULONG count = 0;
    SIZE_T bytes = 0;

    for (USHORT i = 0; i < acl->ace_count; i++) {
        const BYTE *ace = (const BYTE *)acl + offset;
        USHORT ace_size = *(const USHORT *)(ace + 2);
        if (ace_size < 16 || (ace[0] != 0 && ace[0] != 1 && ace[0] != 2))
            return 1336; /* ERROR_INVALID_ACL */

        DWORD sid_size = GetLengthSid_stub((PVOID)(ace + 8));
        if (!sid_size || sid_size > (DWORD)ace_size - 8)
            return 1336;

        ULONG copies = 1;
        if (ace[0] == 2 && (ace[1] & 0xC0) == 0xC0)
            copies = 2; /* one success and one failure audit entry */
        if (count > 0xFFFFFFFFU - copies ||
            bytes > (SIZE_T)-1 - (SIZE_T)sid_size * copies)
            return 8; /* ERROR_NOT_ENOUGH_MEMORY */
        count += copies;
        bytes += (SIZE_T)sid_size * copies;
        offset += ace_size;
    }

    *entry_count = count;
    *sid_bytes = bytes;
    return ERROR_SUCCESS;
}

static void acl_write_explicit_entry(PVOID entries, ULONG index,
                                     DWORD permissions, DWORD mode,
                                     DWORD inheritance, PVOID sid)
{
    if (g_compat32_mode) {
        EXPLICIT_ACCESS_W32 *entry =
            &((EXPLICIT_ACCESS_W32 *)entries)[index];
        entry->access_permissions = permissions;
        entry->access_mode = mode;
        entry->inheritance = inheritance;
        entry->multiple_trustee = 0;
        entry->multiple_trustee_operation = 0;
        entry->trustee_form = 0; /* TRUSTEE_IS_SID */
        entry->trustee_type = 0; /* TRUSTEE_IS_UNKNOWN */
        entry->trustee_name = (DWORD)(ULONG_PTR)sid;
    } else {
        EXPLICIT_ACCESS_W64 *entry =
            &((EXPLICIT_ACCESS_W64 *)entries)[index];
        entry->access_permissions = permissions;
        entry->access_mode = mode;
        entry->inheritance = inheritance;
        entry->padding0 = 0;
        entry->multiple_trustee = NULL;
        entry->multiple_trustee_operation = 0;
        entry->trustee_form = 0;
        entry->trustee_type = 0;
        entry->padding1 = 0;
        entry->trustee_name = sid;
    }
}

static DWORD WINAPI GetExplicitEntriesFromAclA_stub(
    PVOID old_acl, ULONG *count, PVOID explicit_entries)
{
    if (!old_acl || !count || !explicit_entries)
        return 87; /* ERROR_INVALID_PARAMETER */
    *count = 0;
    if (g_compat32_mode)
        *(DWORD *)explicit_entries = 0;
    else
        *(PVOID *)explicit_entries = NULL;

    ACL_HEADER *acl = (ACL_HEADER *)old_acl;
    if (!IsValidAcl_stub(acl))
        return 1336; /* ERROR_INVALID_ACL */

    ULONG entry_count = 0;
    SIZE_T sid_bytes = 0;
    DWORD status = acl_explicit_count(acl, &entry_count, &sid_bytes);
    if (status != ERROR_SUCCESS || entry_count == 0)
        return status;

    SIZE_T entry_size = g_compat32_mode ? sizeof(EXPLICIT_ACCESS_W32)
                                        : sizeof(EXPLICIT_ACCESS_W64);
    if (entry_count > ((SIZE_T)-1 - sid_bytes) / entry_size)
        return 8;
    BYTE *block = LocalAlloc(0x0040,
                             entry_size * entry_count + sid_bytes);
    if (!block)
        return 8; /* ERROR_NOT_ENOUGH_MEMORY */

    BYTE *sid_output = block + entry_size * entry_count;
    DWORD offset = sizeof(*acl);
    ULONG output_index = 0;
    for (USHORT i = 0; i < acl->ace_count; i++) {
        const BYTE *ace = (const BYTE *)acl + offset;
        USHORT ace_size = *(const USHORT *)(ace + 2);
        DWORD sid_size = GetLengthSid_stub((PVOID)(ace + 8));
        DWORD permissions = *(const DWORD *)(ace + 4);
        DWORD inheritance = ace[1] & 0x1F;
        DWORD modes[2];
        ULONG mode_count = 1;
        if (ace[0] == 0)
            modes[0] = ACL_GRANT_ACCESS;
        else if (ace[0] == 1)
            modes[0] = ACL_DENY_ACCESS;
        else {
            modes[0] = (ace[1] & 0x40) ? ACL_SET_AUDIT_SUCCESS
                                        : ACL_SET_AUDIT_FAILURE;
            if ((ace[1] & 0xC0) == 0xC0) {
                modes[1] = ACL_SET_AUDIT_FAILURE;
                mode_count = 2;
            }
        }

        for (ULONG copy = 0; copy < mode_count; copy++) {
            reg_memcpy(sid_output, ace + 8, sid_size);
            acl_write_explicit_entry(block, output_index++, permissions,
                                     modes[copy], inheritance, sid_output);
            sid_output += sid_size;
        }
        offset += ace_size;
    }

    *count = entry_count;
    if (g_compat32_mode)
        *(DWORD *)explicit_entries = (DWORD)(ULONG_PTR)block;
    else
        *(PVOID *)explicit_entries = block;
    return ERROR_SUCCESS;
}

static BOOL WINAPI AddAccessAllowedAce_stub(PVOID acl, DWORD revision,
                                             DWORD access_mask, PVOID sid)
{
    ACL_HEADER *header = (ACL_HEADER *)acl;
    DWORD used = 0;
    if ((revision != ACL_REVISION && revision != 4)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!acl_measure(header, &used))
        return FALSE;
    DWORD sid_length = GetLengthSid_stub(sid);
    if (!sid_length)
        return FALSE;

    DWORD ace_size = 8 + sid_length;
    if (used + ace_size > header->size) {
        SetLastError(1344); /* ERROR_ALLOTTED_SPACE_EXCEEDED */
        return FALSE;
    }

    BYTE *ace = (BYTE *)acl + used;
    ace[0] = 0; /* ACCESS_ALLOWED_ACE_TYPE */
    ace[1] = 0;
    *(USHORT *)(ace + 2) = (USHORT)ace_size;
    *(DWORD *)(ace + 4) = access_mask;
    reg_memcpy(ace + 8, sid, sid_length);
    header->ace_count++;
    if (revision > header->revision)
        header->revision = (UCHAR)revision;
    return TRUE;
}

static DWORD acl_bounded_sid_length(const BYTE *ace, DWORD size, DWORD offset)
{
    if (offset > size || size - offset < 8 ||
        ace[offset] != 1 || ace[offset + 1] > 15)
        return 0;
    DWORD length = 8 + 4 * ace[offset + 1];
    return length <= size - offset ? length : 0;
}

static DWORD acl_mandatory_measure(const ACL_HEADER *acl, DWORD *used)
{
    if (!acl_measure(acl, used)) return ERROR_INVALID_ACL;
    DWORD offset = sizeof(*acl);
    for (DWORD i = 0; i < acl->ace_count; i++) {
        const BYTE *ace = (const BYTE *)acl + offset;
        DWORD size = *(const USHORT *)(ace + 2);
        DWORD sid_offset = 8;
        BOOL known = TRUE;
        switch (ace[0]) {
        case 0: case 1: case 2: case 3: /* Basic access/audit ACEs. */
        case 9: case 10: case 13: case 14: /* Callback ACEs. */
        case 17: case 19: case 20: /* Label, scoped policy, trust label. */
            break;
        case 4: { /* Compound ACE: server SID followed by client SID. */
            if (acl->revision < 3 || size < 24 ||
                *(const USHORT *)(ace + 8) != 1) return ERROR_INVALID_ACL;
            DWORD server = acl_bounded_sid_length(ace, size, 12);
            if (!server || size < 24 + server) return ERROR_INVALID_ACL;
            sid_offset = 12 + server;
            break;
        }
        case 5: case 6: case 7: case 8:
        case 11: case 12: case 15: case 16: { /* Object ACEs. */
            if (acl->revision < 4 || size < 24) return ERROR_INVALID_ACL;
            DWORD flags = *(const DWORD *)(ace + 8);
            sid_offset = 12 + ((flags & 1) ? 16 : 0) + ((flags & 2) ? 16 : 0);
            break;
        }
        case 18: /* Resource claims require a separate relative-claim parser. */
            return ERROR_NOT_SUPPORTED;
        case 21: { /* Access-filter ACE carries a conditional expression. */
            DWORD sid = acl_bounded_sid_length(ace, size, 8);
            if (!sid || size - 8 - sid < 6 ||
                *(const DWORD *)(ace + 8 + sid) != 0x78747261)
                return ERROR_INVALID_ACL;
            break;
        }
        default:
            known = FALSE; /* Preserve opaque ACEs; do not evaluate them. */
            break;
        }
        if (known && ((size & 3) ||
                      !acl_bounded_sid_length(ace, size, sid_offset)))
            return ERROR_INVALID_ACL;
        offset += size;
    }
    return ERROR_SUCCESS;
}

static BOOL WINAPI AddMandatoryAce_stub(PVOID acl, DWORD revision, DWORD flags,
                                        DWORD policy, PVOID label_sid)
{
    BYTE sid[68];
    ACL_HEADER captured;
    if (!win32_user_range_readable(label_sid, 8, g_compat32_mode)) {
        SetLastError(998); /* ERROR_NOACCESS */
        return FALSE;
    }
    reg_memcpy(sid, label_sid, 8);
    if (sid[0] != 1 || sid[1] > 15) {
        SetLastError(1337); /* ERROR_INVALID_SID */
        return FALSE;
    }
    DWORD sid_length = 8 + 4 * sid[1];
    if (!win32_user_range_readable(label_sid, sid_length, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    reg_memcpy(sid + 8, (const BYTE *)label_sid + 8, sid_length - 8);
    if (sid[2] || sid[3] || sid[4] || sid[5] || sid[6] || sid[7] != 16) {
        SetLastError(87); /* SECURITY_MANDATORY_LABEL_AUTHORITY required. */
        return FALSE;
    }
    if (!win32_user_range_readable(acl, sizeof(captured), g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    reg_memcpy(&captured, acl, sizeof(captured));
    if (revision > 4 || captured.revision > 4) {
        SetLastError(1306); /* ERROR_REVISION_MISMATCH */
        return FALSE;
    }
    if ((flags & ~0x1fU) || (policy & ~7U)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (((ULONG_PTR)acl & 1) || captured.revision < ACL_REVISION ||
        captured.size < sizeof(captured)) {
        SetLastError(ERROR_INVALID_ACL);
        return FALSE;
    }
    if (!win32_user_range_readable(acl, captured.size, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    /* Validate a bounded snapshot before touching the caller's ACL. The SID
     * is captured too, including when it aliases the destination buffer. */
    ACL_HEADER *copy = kmalloc(captured.size);
    if (!copy) { SetLastError(8); return FALSE; }
    reg_memcpy(copy, acl, captured.size);
    DWORD used = 0;
    DWORD error = copy->size != captured.size ? ERROR_INVALID_ACL :
                  acl_mandatory_measure(copy, &used);
    DWORD ace_size = 8 + sid_length;
    if (!error && ace_size > captured.size - used)
        error = 1344; /* ERROR_ALLOTTED_SPACE_EXCEEDED */
    if (!error &&
        (!win32_user_range_writable(acl, sizeof(captured), g_compat32_mode) ||
         !win32_user_range_writable((BYTE *)acl + used, ace_size, g_compat32_mode)))
        error = 998;
    if (!error) {
        BYTE *ace = (BYTE *)acl + used;
        ace[0] = 17; /* SYSTEM_MANDATORY_LABEL_ACE_TYPE */
        ace[1] = (BYTE)flags;
        *(USHORT *)(ace + 2) = (USHORT)ace_size;
        *(DWORD *)(ace + 4) = policy;
        reg_memcpy(ace + 8, sid, sid_length);
        ACL_HEADER *header = acl;
        header->ace_count = copy->ace_count + 1;
        header->revision = (BYTE)(revision > copy->revision ? revision : copy->revision);
    }
    kfree(copy);
    if (error) SetLastError(error);
    return error == 0;
}

static BOOL WINAPI InitializeSecurityDescriptor(PVOID descriptor, DWORD revision)
{
    if (!descriptor || revision != SECURITY_DESCRIPTOR_REVISION) {
        SetLastError(descriptor ? 1305 : 87); /* UNKNOWN_REVISION / INVALID_PARAMETER */
        return FALSE;
    }

    if (g_compat32_mode) {
        SECURITY_DESCRIPTOR32 *sd = (SECURITY_DESCRIPTOR32 *)descriptor;
        sd->revision = SECURITY_DESCRIPTOR_REVISION;
        sd->sbz1 = 0;
        sd->control = 0;
        sd->owner = sd->group = sd->sacl = sd->dacl = 0;
    } else {
        SECURITY_DESCRIPTOR64 *sd = (SECURITY_DESCRIPTOR64 *)descriptor;
        sd->revision = SECURITY_DESCRIPTOR_REVISION;
        sd->sbz1 = 0;
        sd->control = 0;
        sd->owner = sd->group = sd->sacl = sd->dacl = NULL;
    }
    return TRUE;
}

void WINAPI MapGenericMask(DWORD *access_mask,
                           const GENERIC_MAPPING *generic_mapping)
{
    if (!access_mask || !generic_mapping)
        return;

    DWORD original = *access_mask;
    DWORD mapped = original & ~(GENERIC_READ | GENERIC_WRITE |
                                GENERIC_EXECUTE | GENERIC_ALL);
    if (original & GENERIC_READ)
        mapped |= generic_mapping->GenericRead;
    if (original & GENERIC_WRITE)
        mapped |= generic_mapping->GenericWrite;
    if (original & GENERIC_EXECUTE)
        mapped |= generic_mapping->GenericExecute;
    if (original & GENERIC_ALL)
        mapped |= generic_mapping->GenericAll;
    *access_mask = mapped;
}

static BOOL WINAPI SetSecurityDescriptorDacl(PVOID descriptor, BOOL present,
                                              PVOID dacl, BOOL defaulted)
{
    if (!descriptor || *(UCHAR *)descriptor != SECURITY_DESCRIPTOR_REVISION) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    USHORT *control = (USHORT *)((BYTE *)descriptor + 2);
    *control &= ~(SE_DACL_PRESENT | SE_DACL_DEFAULTED);
    if (present)
        *control |= SE_DACL_PRESENT | (defaulted ? SE_DACL_DEFAULTED : 0);

    if (g_compat32_mode)
        ((SECURITY_DESCRIPTOR32 *)descriptor)->dacl = (DWORD)(ULONG_PTR)dacl;
    else
        ((SECURITY_DESCRIPTOR64 *)descriptor)->dacl = dacl;
    return TRUE;
}

static void security_store_pointer(PVOID target, PVOID value)
{
    if (!target) return;
    if (g_compat32_mode)
        *(DWORD *)target = (DWORD)(ULONG_PTR)value;
    else
        *(PVOID *)target = value;
}

static BOOL security_descriptor_set_identity(PVOID descriptor, PVOID sid,
                                              BOOL defaulted, BOOL owner)
{
    if (!win32_user_range_readable(descriptor, 4, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    if (*(BYTE *)descriptor != SECURITY_DESCRIPTOR_REVISION) {
        SetLastError(1305); /* ERROR_UNKNOWN_REVISION */
        return FALSE;
    }
    USHORT *control = (USHORT *)((BYTE *)descriptor + 2);
    if (*control & SE_SELF_RELATIVE) {
        SetLastError(ERROR_INVALID_SECURITY_DESCR);
        return FALSE;
    }
    SIZE_T size = g_compat32_mode ? sizeof(SECURITY_DESCRIPTOR32)
                                 : sizeof(SECURITY_DESCRIPTOR64);
    if (!win32_user_range_writable(descriptor, size, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    /* An absolute descriptor references the caller's SID; it does not copy
     * or dereference it. NULL clears the identity without clearing its
     * independently supplied defaulted flag. */
    if (g_compat32_mode) {
        SECURITY_DESCRIPTOR32 *sd = descriptor;
        if (owner) sd->owner = (DWORD)(ULONG_PTR)sid;
        else sd->group = (DWORD)(ULONG_PTR)sid;
    } else {
        SECURITY_DESCRIPTOR64 *sd = descriptor;
        if (owner) sd->owner = sid;
        else sd->group = sid;
    }
    USHORT flag = owner ? SE_OWNER_DEFAULTED : SE_GROUP_DEFAULTED;
    *control = (USHORT)((*control & ~flag) | (defaulted ? flag : 0));
    return TRUE;
}

static BOOL WINAPI SetSecurityDescriptorOwner(PVOID descriptor, PVOID owner,
                                                BOOL defaulted)
{
    return security_descriptor_set_identity(descriptor, owner, defaulted, TRUE);
}

static BOOL WINAPI SetSecurityDescriptorGroup(PVOID descriptor, PVOID group,
                                                BOOL defaulted)
{
    return security_descriptor_set_identity(descriptor, group, defaulted, FALSE);
}

static BOOL WINAPI AllocateAndInitializeSid_stub(
    const BYTE *identifier_authority, BYTE sub_authority_count,
    DWORD sub_authority0, DWORD sub_authority1, DWORD sub_authority2,
    DWORD sub_authority3, DWORD sub_authority4, DWORD sub_authority5,
    DWORD sub_authority6, DWORD sub_authority7, PVOID sid_out)
{
    if (!identifier_authority || !sid_out || sub_authority_count > 8) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    security_store_pointer(sid_out, NULL);

    DWORD sid_size = 8U + (DWORD)sub_authority_count * sizeof(DWORD);
    BYTE *sid = LocalAlloc(0x0040, sid_size);
    if (!sid) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    DWORD values[8];
    values[0] = sub_authority0;
    values[1] = sub_authority1;
    values[2] = sub_authority2;
    values[3] = sub_authority3;
    values[4] = sub_authority4;
    values[5] = sub_authority5;
    values[6] = sub_authority6;
    values[7] = sub_authority7;

    sid[0] = 1;
    sid[1] = sub_authority_count;
    reg_memcpy(sid + 2, identifier_authority, 6);
    for (BYTE i = 0; i < sub_authority_count; i++)
        *(DWORD *)(void *)(sid + 8 + (DWORD)i * sizeof(DWORD)) = values[i];

    security_store_pointer(sid_out, sid);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static PVOID WINAPI FreeSid_stub(PVOID sid)
{
    if (!sid) return NULL;
    PVOID result = LocalFree(sid);
    if (result) SetLastError(ERROR_INVALID_PARAMETER);
    return result;
}

static BOOL parse_sid_component(const WCHAR **cursor, ULONGLONG limit,
                                ULONGLONG *result)
{
    const WCHAR *p = *cursor;
    ULONGLONG value = 0;
    DWORD base = 10;
    DWORD digits = 0;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }

    for (;;) {
        DWORD digit;
        if (*p >= '0' && *p <= '9')
            digit = (DWORD)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')
            digit = (DWORD)(*p - 'a') + 10;
        else if (*p >= 'A' && *p <= 'F')
            digit = (DWORD)(*p - 'A') + 10;
        else
            break;
        if (digit >= base || value > (limit - digit) / base)
            return FALSE;
        value = value * base + digit;
        digits++;
        p++;
    }

    if (!digits) return FALSE;
    *cursor = p;
    *result = value;
    return TRUE;
}

static BOOL WINAPI ConvertStringSidToSidW_stub(PCWSTR string_sid,
                                                PVOID sid_out)
{
    DWORD sub_authorities[15];
    DWORD count = 0;
    ULONGLONG revision;
    ULONGLONG authority;
    const WCHAR *p = string_sid;

    if (!p || !sid_out) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    security_store_pointer(sid_out, NULL);

    if (p[0] == 'W' && p[1] == 'D' && !p[2]) {
        /* SDDL alias for the Everyone SID, S-1-1-0. */
        revision = 1;
        authority = 1;
        sub_authorities[count++] = 0;
    } else {
        if ((p[0] != 'S' && p[0] != 's') || p[1] != '-')
            goto invalid_sid;
        p += 2;
        if (!parse_sid_component(&p, 0xff, &revision) || revision != 1 ||
            *p != '-')
            goto invalid_sid;
        p++;
        if (!parse_sid_component(&p, 0xffffffffffffULL, &authority))
            goto invalid_sid;

        while (*p == '-') {
            ULONGLONG value;
            if (count == 15)
                goto invalid_sid;
            p++;
            if (!parse_sid_component(&p, 0xffffffffULL, &value))
                goto invalid_sid;
            sub_authorities[count++] = (DWORD)value;
        }
        if (*p)
            goto invalid_sid;
    }

    BYTE *sid = LocalAlloc(0x0040, 8 + count * sizeof(DWORD));
    if (!sid) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    sid[0] = (BYTE)revision;
    sid[1] = (BYTE)count;
    for (DWORD i = 0; i < 6; i++)
        sid[2 + i] = (BYTE)(authority >> (8 * (5 - i)));
    for (DWORD i = 0; i < count; i++)
        *(DWORD *)(sid + 8 + i * sizeof(DWORD)) = sub_authorities[i];

    security_store_pointer(sid_out, sid);
    SetLastError(0);
    return TRUE;

invalid_sid:
    SetLastError(1337); /* ERROR_INVALID_SID */
    return FALSE;
}

static DWORD sid_append_decimal(PWSTR output, DWORD position, ULONGLONG value)
{
    WCHAR digits[20];
    DWORD count = 0;
    do {
        digits[count++] = (WCHAR)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        output[position++] = digits[--count];
    return position;
}

static BOOL WINAPI ConvertSidToStringSidW_stub(PVOID sid_value, PVOID string_out)
{
    const BYTE *sid = (const BYTE *)sid_value;
    if (!string_out) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    security_store_pointer(string_out, NULL);
    if (!sid || !IsValidSid_stub(sid_value)) {
        SetLastError(1337); /* ERROR_INVALID_SID */
        return FALSE;
    }

    PWSTR output = LocalAlloc(0x0040, 192 * sizeof(WCHAR));
    if (!output) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    ULONGLONG authority = 0;
    for (DWORD i = 0; i < 6; i++)
        authority = (authority << 8) | sid[2 + i];

    DWORD position = 0;
    output[position++] = 'S';
    output[position++] = '-';
    position = sid_append_decimal(output, position, sid[0]);
    output[position++] = '-';
    position = sid_append_decimal(output, position, authority);
    for (DWORD i = 0; i < sid[1]; i++) {
        output[position++] = '-';
        DWORD sub_authority =
            *(const DWORD *)(sid + 8 + i * sizeof(DWORD));
        position = sid_append_decimal(output, position, sub_authority);
    }
    output[position] = 0;
    security_store_pointer(string_out, output);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ConvertSidToStringSidA_stub(PVOID sid_value, PVOID string_out)
{
    PVOID wide_value = NULL;
    if (!string_out) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    security_store_pointer(string_out, NULL);
    if (!ConvertSidToStringSidW_stub(sid_value, &wide_value))
        return FALSE;

    PWSTR wide = (PWSTR)wide_value;
    DWORD length = 0;
    while (wide[length]) length++;
    PSTR output = LocalAlloc(0x0040, length + 1);
    if (!output) {
        LocalFree(wide);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    for (DWORD i = 0; i <= length; i++)
        output[i] = (char)wide[i];
    LocalFree(wide);

    security_store_pointer(string_out, output);
    SetLastError(0);
    return TRUE;
}

static PVOID security_descriptor_part(PVOID descriptor, int part)
{
    SECURITY_DESCRIPTOR32 *sd32 = descriptor;
    if (sd32->control & SE_SELF_RELATIVE) {
        DWORD offset = (&sd32->owner)[part];
        return offset ? (BYTE *)descriptor + offset : NULL;
    }
    if (g_compat32_mode)
        return (PVOID)(ULONG_PTR)(&sd32->owner)[part];
    return (&((SECURITY_DESCRIPTOR64 *)descriptor)->owner)[part];
}

static BOOL WINAPI IsValidSecurityDescriptor_stub(PVOID descriptor)
{
    if (!descriptor) return FALSE;
    SECURITY_DESCRIPTOR32 *sd = (SECURITY_DESCRIPTOR32 *)descriptor;
    if (sd->revision != SECURITY_DESCRIPTOR_REVISION || sd->sbz1 != 0)
        return FALSE;

    if (sd->control & SE_SELF_RELATIVE) {
        for (int part = 0; part < 4; part++) {
            DWORD offset = (&sd->owner)[part];
            if (offset && (offset < sizeof(*sd) || (offset & 3U)))
                return FALSE;
        }
    }

    PVOID owner = security_descriptor_part(descriptor, 0);
    PVOID group = security_descriptor_part(descriptor, 1);
    PVOID sacl = security_descriptor_part(descriptor, 2);
    PVOID dacl = security_descriptor_part(descriptor, 3);
    if ((owner && !IsValidSid_stub(owner)) ||
        (group && !IsValidSid_stub(group)) ||
        ((sd->control & SE_SACL_PRESENT) && sacl &&
         !IsValidAcl_stub(sacl)) ||
        ((sd->control & SE_DACL_PRESENT) && dacl &&
         !IsValidAcl_stub(dacl)))
        return FALSE;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorControl_stub(PVOID descriptor,
                                                       USHORT *control,
                                                       DWORD *revision)
{
    if (!IsValidSecurityDescriptor_stub(descriptor) || !control || !revision)
        return FALSE;
    *control = *(USHORT *)((BYTE *)descriptor + 2);
    *revision = SECURITY_DESCRIPTOR_REVISION;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorOwner_stub(PVOID descriptor,
                                                     PVOID owner,
                                                     BOOL *defaulted)
{
    if (!IsValidSecurityDescriptor_stub(descriptor) || !owner || !defaulted)
        return FALSE;
    security_store_pointer(owner, security_descriptor_part(descriptor, 0));
    *defaulted = (*(USHORT *)((BYTE *)descriptor + 2) & SE_OWNER_DEFAULTED) != 0;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorGroup_stub(PVOID descriptor,
                                                     PVOID group,
                                                     BOOL *defaulted)
{
    if (!IsValidSecurityDescriptor_stub(descriptor) || !group || !defaulted)
        return FALSE;
    security_store_pointer(group, security_descriptor_part(descriptor, 1));
    *defaulted = (*(USHORT *)((BYTE *)descriptor + 2) & SE_GROUP_DEFAULTED) != 0;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorDacl_stub(PVOID descriptor,
                                                    BOOL *present, PVOID dacl,
                                                    BOOL *defaulted)
{
    if (!IsValidSecurityDescriptor_stub(descriptor) || !present || !dacl ||
        !defaulted)
        return FALSE;
    USHORT control = *(USHORT *)((BYTE *)descriptor + 2);
    *present = (control & SE_DACL_PRESENT) != 0;
    security_store_pointer(dacl, security_descriptor_part(descriptor, 3));
    *defaulted = (control & SE_DACL_DEFAULTED) != 0;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorSacl_stub(PVOID descriptor,
                                                    BOOL *present, PVOID sacl,
                                                    BOOL *defaulted)
{
    if (!IsValidSecurityDescriptor_stub(descriptor) || !present || !sacl ||
        !defaulted)
        return FALSE;
    USHORT control = *(USHORT *)((BYTE *)descriptor + 2);
    *present = (control & SE_SACL_PRESENT) != 0;
    security_store_pointer(sacl, security_descriptor_part(descriptor, 2));
    *defaulted = (control & SE_SACL_DEFAULTED) != 0;
    return TRUE;
}

#define SECURITY_CLASS_OWNER 0x01U
#define SECURITY_CLASS_GROUP 0x02U
#define SECURITY_CLASS_OTHER 0x04U

static ULONGLONG security_sid_authority(const BYTE *sid)
{
    ULONGLONG authority = 0;
    for (DWORD i = 0; i < 6; i++)
        authority = (authority << 8) | sid[2 + i];
    return authority;
}

static BOOL security_sid_has_subauthorities(const BYTE *sid, BYTE count,
                                             const DWORD *values)
{
    if (!sid || sid[1] != count) return FALSE;
    for (BYTE i = 0; i < count; i++) {
        DWORD value = *(const DWORD *)(const void *)(sid + 8 + i * 4U);
        if (value != values[i]) return FALSE;
    }
    return TRUE;
}

static BYTE security_sid_classes(PVOID sid_value)
{
    const BYTE *sid = (const BYTE *)sid_value;
    DWORD sid_size = GetLengthSid_stub(sid_value);
    if (!sid_size) return 0;

    if (sid_size == sizeof(g_local_user_sid)) {
        const BYTE *local = (const BYTE *)&g_local_user_sid;
        BOOL equal = TRUE;
        for (DWORD i = 0; i < sid_size; i++) {
            if (sid[i] != local[i]) {
                equal = FALSE;
                break;
            }
        }
        if (equal) return SECURITY_CLASS_OWNER;
    }

    ULONGLONG authority = security_sid_authority(sid);
    static const DWORD world[] = { 0 };
    static const DWORD creator_owner[] = { 0 };
    static const DWORD creator_group[] = { 1 };
    static const DWORD builtin_users[] = { 32, 545 };
    static const DWORD authenticated_users[] = { 11 };

    if (authority == 1 &&
        security_sid_has_subauthorities(sid, 1, world))
        return SECURITY_CLASS_OWNER | SECURITY_CLASS_GROUP |
               SECURITY_CLASS_OTHER;
    if (authority == 3 &&
        security_sid_has_subauthorities(sid, 1, creator_owner))
        return SECURITY_CLASS_OWNER;
    if (authority == 3 &&
        security_sid_has_subauthorities(sid, 1, creator_group))
        return SECURITY_CLASS_GROUP;
    if (authority == 5 &&
        security_sid_has_subauthorities(sid, 2, builtin_users))
        return SECURITY_CLASS_OWNER | SECURITY_CLASS_GROUP;
    if (authority == 5 &&
        security_sid_has_subauthorities(sid, 1, authenticated_users))
        return SECURITY_CLASS_OWNER | SECURITY_CLASS_GROUP |
               SECURITY_CLASS_OTHER;
    return 0;
}

static BYTE security_access_mask_to_mode(DWORD mask)
{
    if (mask & GENERIC_ALL) return 7;
    BYTE mode = 0;
    if (mask & (GENERIC_READ | FILE_READ_DATA | FILE_READ_EA |
                FILE_READ_ATTRIBUTES))
        mode |= 4;
    if (mask & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA |
                FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD))
        mode |= 2;
    if (mask & (GENERIC_EXECUTE | FILE_EXECUTE))
        mode |= 1;
    return mode;
}

static DWORD security_acl_to_mode(PVOID acl_value, uint16_t *mode)
{
    ACL_HEADER *acl = (ACL_HEADER *)acl_value;
    DWORD used;
    if (!mode || !acl_measure(acl, &used)) return ERROR_INVALID_ACL;

    BYTE decided[3] = { 0, 0, 0 };
    BYTE granted[3] = { 0, 0, 0 };
    DWORD offset = sizeof(*acl);
    const DWORD supported_mask = 0xF0000000U | 0x001F01FFU;
    for (USHORT i = 0; i < acl->ace_count; i++) {
        const BYTE *ace = (const BYTE *)acl + offset;
        USHORT ace_size = *(const USHORT *)(const void *)(ace + 2);
        if (ace_size < 16 || (ace[0] != 0 && ace[0] != 1))
            return ERROR_NOT_SUPPORTED;
        /* Inheritance cannot be retained in the inode mode. An ACE that is
         * merely marked as inherited still applies to this object. */
        if (ace[1] & ~0x10U) return ERROR_NOT_SUPPORTED;

        DWORD mask = *(const DWORD *)(const void *)(ace + 4);
        if (mask & ~supported_mask) return ERROR_NOT_SUPPORTED;
        PVOID sid = (PVOID)(ace + 8);
        DWORD sid_size = GetLengthSid_stub(sid);
        if (!sid_size || sid_size > (DWORD)ace_size - 8U)
            return ERROR_INVALID_ACL;
        BYTE classes = security_sid_classes(sid);
        if (!classes) return ERROR_NONE_MAPPED;

        BYTE rights = security_access_mask_to_mode(mask);
        for (BYTE class_index = 0; class_index < 3; class_index++) {
            BYTE class_bit = (BYTE)(1U << class_index);
            if (!(classes & class_bit)) continue;
            BYTE pending = rights & (BYTE)~decided[class_index];
            decided[class_index] |= pending;
            if (ace[0] == 0) granted[class_index] |= pending;
        }
        offset += ace_size;
    }
    if (offset != used) return ERROR_INVALID_ACL;

    *mode = (uint16_t)((granted[0] << 6) |
                       (granted[1] << 3) | granted[2]);
    return ERROR_SUCCESS;
}

static BOOL set_file_security_error(DWORD error)
{
    SetLastError(error);
    return FALSE;
}

static BOOL set_file_security_common(PCSTR file_name, DWORD security_info,
                                     PVOID descriptor)
{
    const DWORD known_information =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION |
        UNPROTECTED_DACL_SECURITY_INFORMATION |
        PROTECTED_DACL_SECURITY_INFORMATION;
    if (!file_name || !descriptor || !security_info ||
        (security_info & ~known_information))
        return set_file_security_error(ERROR_INVALID_PARAMETER);
    if (!IsValidSecurityDescriptor_stub(descriptor))
        return set_file_security_error(ERROR_INVALID_SECURITY_DESCR);
    if (security_info & SACL_SECURITY_INFORMATION)
        return set_file_security_error(ERROR_PRIVILEGE_NOT_HELD);
    if (security_info & (OWNER_SECURITY_INFORMATION |
                         GROUP_SECURITY_INFORMATION |
                         UNPROTECTED_DACL_SECURITY_INFORMATION |
                         PROTECTED_DACL_SECURITY_INFORMATION))
        return set_file_security_error(ERROR_NOT_SUPPORTED);
    if (!(security_info & DACL_SECURITY_INFORMATION))
        return set_file_security_error(ERROR_INVALID_PARAMETER);

    SECURITY_DESCRIPTOR32 *sd = (SECURITY_DESCRIPTOR32 *)descriptor;
    if (!(sd->control & SE_DACL_PRESENT))
        return set_file_security_error(ERROR_INVALID_SECURITY_DESCR);

    char normalized[260];
    if (!win32_resolve_file_path(file_name, normalized))
        return set_file_security_error(ERROR_FILENAME_EXCED_RANGE);
    vfs_node_t node;
    if (!vfs_find(normalized, VFS_MODE_WIN32, &node)) {
        if (win32_directory_exists_normalized(normalized))
            return set_file_security_error(ERROR_NOT_SUPPORTED);
        return set_file_security_error(ERROR_FILE_NOT_FOUND);
    }

    PVOID dacl = security_descriptor_part(descriptor, 3);
    uint16_t mode = 0777;
    if (dacl) {
        DWORD status = security_acl_to_mode(dacl, &mode);
        if (status != ERROR_SUCCESS)
            return set_file_security_error(status);
    }

    int result = vfs_set_mode(&node, mode);
    if (result == VFS_STATUS_NOT_SUPPORTED)
        return set_file_security_error(ERROR_NOT_SUPPORTED);
    if (result == VFS_STATUS_INVALID)
        return set_file_security_error(ERROR_INVALID_PARAMETER);
    if (result != VFS_STATUS_OK)
        return set_file_security_error(ERROR_ACCESS_DENIED);

    static uint32_t trace_count;
    uint32_t trace = __atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED);
    if (trace < 16) {
        serial_puts("[ADVAPI-FILE-SEC] path='");
        serial_puts(normalized);
        serial_puts("' mode=");
        serial_puthex(mode, 3);
        serial_puts("\n");
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static BOOL WINAPI SetFileSecurityA_stub(PCSTR file_name,
                                          DWORD security_info,
                                          PVOID descriptor)
{
    return set_file_security_common(file_name, security_info, descriptor);
}

static BOOL WINAPI SetFileSecurityW_stub(PCWSTR file_name,
                                          DWORD security_info,
                                          PVOID descriptor)
{
    if (!file_name)
        return set_file_security_error(ERROR_INVALID_PARAMETER);
    char narrow[260];
    DWORD length = 0;
    while (length < 259 && file_name[length]) {
        narrow[length] = (char)(file_name[length] & 0xff);
        length++;
    }
    if (file_name[length])
        return set_file_security_error(ERROR_FILENAME_EXCED_RANGE);
    narrow[length] = 0;
    return set_file_security_common(narrow, security_info, descriptor);
}

static DWORD WINAPI GetNamedSecurityInfoW_stub(
    PWSTR object_name, DWORD object_type, DWORD security_info, PVOID owner,
    PVOID group, PVOID dacl, PVOID sacl, PVOID descriptor)
{
    (void)object_type;
    (void)security_info;
    if (!object_name || !descriptor)
        return 87; /* ERROR_INVALID_PARAMETER */

    SIZE_T descriptor_size = sizeof(SECURITY_DESCRIPTOR32);
    BYTE *block = LocalAlloc(0x0040, descriptor_size + sizeof(ACL_HEADER));
    if (!block)
        return 8; /* ERROR_NOT_ENOUGH_MEMORY */

    SECURITY_DESCRIPTOR32 *sd = (SECURITY_DESCRIPTOR32 *)block;
    ACL_HEADER *acl = (ACL_HEADER *)(block + descriptor_size);
    sd->revision = SECURITY_DESCRIPTOR_REVISION;
    sd->control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
    sd->dacl = (DWORD)descriptor_size;
    InitializeAcl_stub(acl, sizeof(*acl), ACL_REVISION);

    security_store_pointer(owner, NULL);
    security_store_pointer(group, NULL);
    security_store_pointer(dacl, acl);
    security_store_pointer(sacl, NULL);
    security_store_pointer(descriptor, sd);
    return ERROR_SUCCESS;
}

static DWORD WINAPI GetSecurityInfo_stub(HANDLE object, DWORD object_type,
                                          DWORD security_info, PVOID owner,
                                          PVOID group, PVOID dacl, PVOID sacl,
                                          PVOID descriptor)
{
    (void)object;
    (void)object_type;
    (void)security_info;
    if (!descriptor) return 87; /* ERROR_INVALID_PARAMETER */

    SECURITY_DESCRIPTOR32 *sd = LocalAlloc(0x0040, sizeof(*sd));
    if (!sd) return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    sd->revision = SECURITY_DESCRIPTOR_REVISION;
    sd->control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
    security_store_pointer(owner, NULL);
    security_store_pointer(group, NULL);
    security_store_pointer(dacl, NULL);
    security_store_pointer(sacl, NULL);
    security_store_pointer(descriptor, sd);
    return ERROR_SUCCESS;
}

static DWORD WINAPI SetSecurityInfo_stub(HANDLE object, DWORD object_type,
                                          DWORD security_info, PVOID owner,
                                          PVOID group, PVOID dacl, PVOID sacl)
{
    (void)object; (void)object_type; (void)security_info;
    (void)owner; (void)group; (void)dacl; (void)sacl;
    return ERROR_SUCCESS;
}

static DWORD WINAPI SetNamedSecurityInfoW_stub(
    PWSTR object_name, DWORD object_type, DWORD security_info, PVOID owner,
    PVOID group, PVOID dacl, PVOID sacl)
{
    (void)object_type;
    (void)security_info;
    (void)owner;
    (void)group;
    (void)dacl;
    (void)sacl;
    return object_name ? ERROR_SUCCESS : 87; /* ERROR_INVALID_PARAMETER */
}

static DWORD WINAPI BuildSecurityDescriptorW_stub(
    PVOID owner, PVOID group, ULONG access_count, PVOID access_entries,
    ULONG audit_count, PVOID audit_entries, PVOID old_descriptor,
    DWORD *descriptor_size, PVOID *new_descriptor)
{
    (void)owner;
    (void)group;
    (void)access_count;
    (void)access_entries;
    (void)audit_count;
    (void)audit_entries;
    (void)old_descriptor;

    if (!descriptor_size || !new_descriptor)
        return 87; /* ERROR_INVALID_PARAMETER */

    /* ponytail: ACLs are metadata-only until the object manager enforces them. */
    SECURITY_DESCRIPTOR32 *sd = LocalAlloc(0x0040, sizeof(*sd));
    if (!sd)
        return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    sd->revision = SECURITY_DESCRIPTOR_REVISION;
    sd->control = SE_SELF_RELATIVE | SE_DACL_PRESENT;
    *descriptor_size = sizeof(*sd);
    if (g_compat32_mode)
        *(DWORD *)(void *)new_descriptor = (DWORD)(ULONG_PTR)sd;
    else
        *new_descriptor = sd;
    return ERROR_SUCCESS;
}

typedef struct {
    BYTE type;
    BYTE flags;
    DWORD mask;
    DWORD sid_size;
    BYTE sid[68];
} SDDL_ACE;

static int sddl_hex_digit(WCHAR ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static BOOL sddl_token_is(const WCHAR *token, DWORD length,
                          char first, char second)
{
    return length == 2 && token[0] == (WCHAR)first &&
           token[1] == (WCHAR)second;
}

static DWORD sddl_write_sid(BYTE *sid, ULONGLONG authority,
                            const DWORD *sub_authorities, BYTE count)
{
    sid[0] = 1;
    sid[1] = count;
    for (DWORD i = 0; i < 6; i++)
        sid[2 + i] = (BYTE)(authority >> (8 * (5 - i)));
    for (DWORD i = 0; i < count; i++)
        *(DWORD *)(sid + 8 + i * sizeof(DWORD)) = sub_authorities[i];
    return 8 + count * sizeof(DWORD);
}

static DWORD sddl_parse_sid(const WCHAR *token, DWORD length, BYTE *sid)
{
    DWORD sub_authorities[15];
    BYTE count = 0;
    ULONGLONG authority = 0;

    if (length >= 4 && token[0] == 'S' && token[1] == '-' &&
        token[2] == '1' && token[3] == '-') {
        DWORD position = 4;
        if (position == length) return 0;
        while (position < length && token[position] != '-') {
            if (token[position] < '0' || token[position] > '9') return 0;
            authority = authority * 10 + (token[position++] - '0');
            if (authority > 0xffffffffffffULL) return 0;
        }
        while (position < length) {
            if (token[position++] != '-' || position == length || count == 15)
                return 0;
            ULONGLONG value = 0;
            while (position < length && token[position] != '-') {
                if (token[position] < '0' || token[position] > '9') return 0;
                value = value * 10 + (token[position++] - '0');
                if (value > 0xffffffffULL) return 0;
            }
            sub_authorities[count++] = (DWORD)value;
        }
        return sddl_write_sid(sid, authority, sub_authorities, count);
    }

    if (sddl_token_is(token, length, 'W', 'D')) {
        authority = 1; sub_authorities[0] = 0; count = 1;
    } else if (sddl_token_is(token, length, 'A', 'C')) {
        authority = 15; sub_authorities[0] = 2; sub_authorities[1] = 1; count = 2;
    } else if (sddl_token_is(token, length, 'S', 'Y')) {
        authority = 5; sub_authorities[0] = 18; count = 1;
    } else if (sddl_token_is(token, length, 'B', 'A')) {
        authority = 5; sub_authorities[0] = 32; sub_authorities[1] = 544; count = 2;
    } else if (sddl_token_is(token, length, 'B', 'U')) {
        authority = 5; sub_authorities[0] = 32; sub_authorities[1] = 545; count = 2;
    } else if (sddl_token_is(token, length, 'A', 'U')) {
        authority = 5; sub_authorities[0] = 11; count = 1;
    } else if (sddl_token_is(token, length, 'I', 'U')) {
        authority = 5; sub_authorities[0] = 4; count = 1;
    } else {
        reg_memcpy(sid, &g_local_user_sid, sizeof(g_local_user_sid));
        return sizeof(g_local_user_sid);
    }
    return sddl_write_sid(sid, authority, sub_authorities, count);
}

static DWORD sddl_parse_rights(const WCHAR *token, DWORD length)
{
    if (length > 2 && token[0] == '0' && token[1] == 'x') {
        DWORD mask = 0;
        for (DWORD i = 2; i < length; i++) {
            int digit = sddl_hex_digit(token[i]);
            if (digit < 0) return 0;
            mask = (mask << 4) | (DWORD)digit;
        }
        return mask;
    }

    DWORD mask = 0;
    for (DWORD i = 0; i + 1 < length; i += 2) {
        const WCHAR *right = token + i;
        if (sddl_token_is(right, 2, 'G', 'A')) mask |= 0x10000000;
        else if (sddl_token_is(right, 2, 'G', 'R')) mask |= 0x80000000;
        else if (sddl_token_is(right, 2, 'G', 'W')) mask |= 0x40000000;
        else if (sddl_token_is(right, 2, 'G', 'X')) mask |= 0x20000000;
        else if (sddl_token_is(right, 2, 'F', 'A')) mask |= 0x001f01ff;
        else if (sddl_token_is(right, 2, 'F', 'R')) mask |= 0x00120089;
        else if (sddl_token_is(right, 2, 'F', 'W')) mask |= 0x00120116;
        else if (sddl_token_is(right, 2, 'F', 'X')) mask |= 0x001200a0;
        else if (sddl_token_is(right, 2, 'R', 'C')) mask |= 0x00020000;
        else if (sddl_token_is(right, 2, 'S', 'D')) mask |= 0x00010000;
        else if (sddl_token_is(right, 2, 'W', 'D')) mask |= 0x00040000;
        else if (sddl_token_is(right, 2, 'W', 'O')) mask |= 0x00080000;
        else if (sddl_token_is(right, 2, 'C', 'C')) mask |= 0x00000001;
        else if (sddl_token_is(right, 2, 'D', 'C')) mask |= 0x00000002;
        else if (sddl_token_is(right, 2, 'L', 'C')) mask |= 0x00000004;
        else if (sddl_token_is(right, 2, 'S', 'W')) mask |= 0x00000008;
        else if (sddl_token_is(right, 2, 'R', 'P')) mask |= 0x00000010;
        else if (sddl_token_is(right, 2, 'W', 'P')) mask |= 0x00000020;
        else if (sddl_token_is(right, 2, 'D', 'T')) mask |= 0x00000040;
        else if (sddl_token_is(right, 2, 'L', 'O')) mask |= 0x00000080;
        else if (sddl_token_is(right, 2, 'C', 'R')) mask |= 0x00000100;
    }
    return mask;
}

static BYTE sddl_parse_ace_flags(const WCHAR *token, DWORD length)
{
    BYTE flags = 0;
    for (DWORD i = 0; i + 1 < length; i += 2) {
        const WCHAR *flag = token + i;
        if (sddl_token_is(flag, 2, 'O', 'I')) flags |= 0x01;
        else if (sddl_token_is(flag, 2, 'C', 'I')) flags |= 0x02;
        else if (sddl_token_is(flag, 2, 'N', 'P')) flags |= 0x04;
        else if (sddl_token_is(flag, 2, 'I', 'O')) flags |= 0x08;
        else if (sddl_token_is(flag, 2, 'I', 'D')) flags |= 0x10;
        else if (sddl_token_is(flag, 2, 'S', 'A')) flags |= 0x40;
        else if (sddl_token_is(flag, 2, 'F', 'A')) flags |= 0x80;
    }
    return flags;
}

static BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(
    PCWSTR string_descriptor, DWORD revision, PVOID *descriptor,
    DWORD *descriptor_size)
{
    if (!string_descriptor || !descriptor) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (revision != SECURITY_DESCRIPTOR_REVISION) {
        SetLastError(1305); /* ERROR_UNKNOWN_REVISION */
        return FALSE;
    }

    const WCHAR *dacl = NULL;
    for (const WCHAR *cursor = string_descriptor; *cursor; cursor++) {
        if (cursor[0] == 'D' && cursor[1] == ':') {
            dacl = cursor + 2;
            break;
        }
    }

    SDDL_ACE aces[16];
    DWORD ace_count = 0;
    DWORD acl_size = sizeof(ACL_HEADER);
    USHORT control = SE_SELF_RELATIVE;
    if (dacl) {
        control |= SE_DACL_PRESENT;
        const WCHAR *cursor = dacl;
        while (*cursor && *cursor != '(') {
            if (*cursor == 'P') control |= 0x1000; /* SE_DACL_PROTECTED */
            cursor++;
        }
        while (*cursor == '(') {
            if (ace_count == 16) {
                SetLastError(1344); /* ERROR_ALLOTTED_SPACE_EXCEEDED */
                return FALSE;
            }
            cursor++;
            const WCHAR *fields[6];
            DWORD lengths[6];
            for (DWORD field = 0; field < 5; field++) {
                fields[field] = cursor;
                while (*cursor && *cursor != ';' && *cursor != ')') cursor++;
                if (*cursor != ';') {
                    SetLastError(1336); /* ERROR_INVALID_ACL */
                    return FALSE;
                }
                lengths[field] = (DWORD)(cursor - fields[field]);
                cursor++;
            }
            fields[5] = cursor;
            while (*cursor && *cursor != ')') cursor++;
            if (*cursor != ')') {
                SetLastError(1336);
                return FALSE;
            }
            lengths[5] = (DWORD)(cursor - fields[5]);
            cursor++;

            SDDL_ACE *ace = &aces[ace_count];
            if (lengths[0] == 1 && fields[0][0] == 'A') ace->type = 0;
            else if (lengths[0] == 1 && fields[0][0] == 'D') ace->type = 1;
            else continue;
            ace->flags = sddl_parse_ace_flags(fields[1], lengths[1]);
            ace->mask = sddl_parse_rights(fields[2], lengths[2]);
            ace->sid_size = sddl_parse_sid(fields[5], lengths[5], ace->sid);
            if (!ace->sid_size) {
                SetLastError(1337); /* ERROR_INVALID_SID */
                return FALSE;
            }
            acl_size += 8 + ace->sid_size;
            ace_count++;
        }
    }

    DWORD total_size = sizeof(SECURITY_DESCRIPTOR32) + (dacl ? acl_size : 0);
    SECURITY_DESCRIPTOR32 *sd = LocalAlloc(0x0040, total_size);
    if (!sd) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    sd->revision = SECURITY_DESCRIPTOR_REVISION;
    sd->control = control;

    if (dacl) {
        sd->dacl = sizeof(*sd);
        ACL_HEADER *acl = (ACL_HEADER *)((BYTE *)sd + sizeof(*sd));
        acl->revision = ACL_REVISION;
        acl->size = (USHORT)acl_size;
        acl->ace_count = (USHORT)ace_count;
        DWORD used = sizeof(*acl);
        for (DWORD i = 0; i < ace_count; i++) {
            BYTE *raw_ace = (BYTE *)acl + used;
            raw_ace[0] = aces[i].type;
            raw_ace[1] = aces[i].flags;
            *(USHORT *)(raw_ace + 2) = (USHORT)(8 + aces[i].sid_size);
            *(DWORD *)(raw_ace + 4) = aces[i].mask;
            reg_memcpy(raw_ace + 8, aces[i].sid, aces[i].sid_size);
            used += 8 + aces[i].sid_size;
        }
    }

    if (g_compat32_mode)
        *(DWORD *)(void *)descriptor = (DWORD)(ULONG_PTR)sd;
    else
        *descriptor = sd;
    if (descriptor_size)
        *descriptor_size = total_size;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI GetUserNameA(PSTR lpBuffer, DWORD *pcbBuffer)
{
    const char *name = "Player";
    DWORD len = 6;
    if (!lpBuffer || !pcbBuffer || *pcbBuffer <= len) {
        if (pcbBuffer) *pcbBuffer = len + 1;
        return FALSE;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = name[i];
    *pcbBuffer = len + 1;
    return TRUE;
}

BOOL WINAPI GetUserNameW(PWSTR lpBuffer, DWORD *pcbBuffer)
{
    static const WCHAR name[] = {'P','l','a','y','e','r',0};
    DWORD len = 6;
    if (!lpBuffer || !pcbBuffer || *pcbBuffer <= len) {
        if (pcbBuffer) *pcbBuffer = len + 1;
        return FALSE;
    }
    for (DWORD i = 0; i <= len; i++) lpBuffer[i] = name[i];
    *pcbBuffer = len + 1;
    return TRUE;
}

static BOOL WINAPI LookupAccountNameW_stub(
    PCWSTR system_name, PCWSTR account_name, PVOID sid, DWORD *sid_size,
    PWSTR domain_name, DWORD *domain_size, DWORD *sid_name_use)
{
    static const WCHAR domain[] = {'O','S','I','T','O',0};
    const DWORD required_sid = sizeof(LOCAL_USER_SID);
    const DWORD required_domain = sizeof(domain) / sizeof(domain[0]);
    (void)system_name;

    if (!account_name || !sid_size || !domain_size || !sid_name_use) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD supplied_sid = *sid_size;
    DWORD supplied_domain = *domain_size;
    *sid_size = required_sid;
    *domain_size = required_domain;
    if (!sid || supplied_sid < required_sid ||
        !domain_name || supplied_domain < required_domain) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    reg_memcpy(sid, &g_local_user_sid, sizeof(g_local_user_sid));
    for (DWORD i = 0; i < required_domain; i++)
        domain_name[i] = domain[i];
    *sid_name_use = 1; /* SidTypeUser */
    SetLastError(0);
    return TRUE;
}

static DWORD token_map_access(DWORD access)
{
    const GENERIC_MAPPING mapping = {0x2001a, 0x201e0, 0x20005, 0xf01ff};
    MapGenericMask(&access, &mapping);
    if (access & 0x02000000) /* MAXIMUM_ALLOWED for the local token owner */
        access = (access & ~0x02000000U) | mapping.GenericAll;
    return access;
}

NTSTATUS advapi32_open_token(HANDLE source, ACCESS_MASK access, ULONG attributes,
                            BOOL thread, BOOL open_as_self, PHANDLE output)
{
    if (!output) return STATUS_INVALID_PARAMETER;
    ULONG_PTR value = (ULONG_PTR)source;
    if (g_compat32_mode && value == 0xffffffffU) source = NT_CURRENT_PROCESS;
    if (g_compat32_mode && value == 0xfffffffeU) source = NT_CURRENT_THREAD;
    if (source == (thread ? NT_CURRENT_PROCESS : NT_CURRENT_THREAD))
        return STATUS_OBJECT_TYPE_MISMATCH;
    if (source != (thread ? NT_CURRENT_THREAD : NT_CURRENT_PROCESS) &&
        value > 0xffffffffU) return STATUS_INVALID_HANDLE;

    PVOID object = NULL;
    NTSTATUS status = thread ? nt_thread_reference_impersonation_token(source, &object)
                            : nt_process_reference_primary_token(source, &object);
    if (!NT_SUCCESS(status)) return status;
    const LOCAL_TOKEN_OBJECT *token = object;
    if (thread && token->impersonation_level == 0) {
        status = STATUS_CANT_OPEN_ANONYMOUS;
        goto done;
    }
    if (!thread || !open_as_self) {
        const LOCAL_TOKEN_OBJECT *subject = nt_thread_get_impersonation_token(
            win32_current_thread_object());
        BOOL bad_level = subject && subject->impersonation_level < 2;
        advapi32_token_release(subject);
        if (bad_level) { status = STATUS_BAD_IMPERSONATION_LEVEL; goto done; }
    }
    if (attributes & OBJ_OPENLINK) { status = STATUS_INVALID_PARAMETER; goto done; }
    /* The local token model has no SeSecurityPrivilege. Generic rights must
     * be mapped before validating the token object's supported access mask. */
    access = token_map_access(access);
    if (access & 0x01000000U) { status = STATUS_PRIVILEGE_NOT_HELD; goto done; }
    if (!access || (access & ~0x000f01ffU)) { status = STATUS_ACCESS_DENIED; goto done; }
    HANDLE handle = NULL;
    status = handle_alloc(&g_handle_table, OBJ_TYPE_TOKEN, access, object, &handle);
    if (NT_SUCCESS(status) && (attributes & OBJ_INHERIT)) {
        status = handle_update_flags_for_process(&g_handle_table, handle,
            win32_current_process_id(), HANDLE_USER_FLAG_INHERIT, HANDLE_USER_FLAG_INHERIT);
        if (!NT_SUCCESS(status)) handle_close(&g_handle_table, handle);
    }
    if (NT_SUCCESS(status)) *output = handle;
done:
    advapi32_token_release(object);
    return status;
}

static BOOL token_open_win32(HANDLE source, DWORD access, BOOL thread,
                            BOOL open_as_self, PHANDLE output)
{
    if (!output) { SetLastError(87); return FALSE; }
    if (!win32_user_range_writable(output, g_compat32_mode ? 4 : sizeof(HANDLE),
                                   g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    HANDLE handle = NULL;
    NTSTATUS status = advapi32_open_token(source, access, 0, thread, open_as_self, &handle);
    if (!NT_SUCCESS(status)) { SetLastError(RtlNtStatusToDosError(status)); return FALSE; }
    if (g_compat32_mode) *(DWORD *)(void *)output = (DWORD)(ULONG_PTR)handle;
    else *output = handle;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI OpenProcessToken_stub(HANDLE process, DWORD access, PHANDLE token)
{
    return token_open_win32(process, access, FALSE, FALSE, token);
}

static const LOCAL_TOKEN_OBJECT *token_query_access(HANDLE token,
                                                     DWORD required,
                                                     DWORD *granted)
{
    PVOID object = NULL;
    ACCESS_MASK access = 0;
    NTSTATUS status = handle_reference_token_for_process(
        &g_handle_table, token, win32_current_process_id(), required, &object, &access);
    if (!NT_SUCCESS(status)) {
        SetLastError(status == STATUS_ACCESS_DENIED ? 5 : 6);
        return NULL;
    }
    if (granted) *granted = access;
    return object;
}

static BOOL token_sid_equal(PCVOID left, PCVOID right)
{
    const BYTE *a = left, *b = right;
    if (!a || !b || a[0] != 1 || b[0] != 1 || a[1] != b[1] || a[1] > 15)
        return FALSE;
    for (DWORD i = 0; i < 8U + 4U * a[1]; i++)
        if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static BOOL token_has_sid(const LOCAL_TOKEN_OBJECT *token, PCVOID sid,
                          BOOL denying, BOOL owner)
{
    if (token_sid_equal(token->user_sid, sid))
        return !(token->user_attributes & 0x10) || (denying && !owner);
    for (DWORD i = 0; i < token->group_count; i++) {
        const LOCAL_TOKEN_GROUP *group = &token->groups[i];
        DWORD flags = group->attributes;
        if (owner && !(flags & 8)) continue; /* SE_GROUP_OWNER */
        if (flags & 0x10) { /* SE_GROUP_USE_FOR_DENY_ONLY */
            if (!denying || owner) continue;
        } else if (!(flags & 4)) {
            continue; /* SE_GROUP_ENABLED */
        }
        if (token_sid_equal(group->sid, sid)) return TRUE;
    }
    return FALSE;
}

static BOOL token_membership_enabled(const LOCAL_TOKEN_OBJECT *token, PCVOID sid)
{
    if (!token_has_sid(token, sid, FALSE, FALSE)) return FALSE;
    if (!token->restricted_count) return TRUE;
    for (DWORD i = 0; i < token->restricted_count; i++) {
        const LOCAL_TOKEN_GROUP *group = &token->restricted[i];
        if ((group->attributes & 4) && !(group->attributes & 0x10) &&
            token_sid_equal(group->sid, sid)) return TRUE;
    }
    return FALSE;
}

static BOOL WINAPI CheckTokenMembership(HANDLE token, PVOID sid_to_check,
                                          BOOL *is_member)
{
    DWORD saved_error = GetLastError();
    if (!win32_user_range_writable(is_member, sizeof(*is_member), g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    *is_member = FALSE;
    BYTE sid[68];
    if (!win32_user_range_readable(sid_to_check, 8, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    reg_memcpy(sid, sid_to_check, 8);
    if (sid[0] != 1 || sid[1] > 15) {
        SetLastError(1337);
        return FALSE;
    }
    DWORD size = 8 + 4 * sid[1];
    if (!win32_user_range_readable(sid_to_check, size, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    reg_memcpy(sid + 8, (const BYTE *)sid_to_check + 8, size - 8);

    HANDLE opened = NULL;
    BOOL implicit = token == NULL;
    BOOL primary_fallback = FALSE;
    if (implicit) {
        NTSTATUS status = advapi32_open_token(NT_CURRENT_THREAD, 8, 0,
                                              TRUE, FALSE, &opened);
        if (status == STATUS_NO_TOKEN) {
            status = advapi32_open_token(NT_CURRENT_PROCESS, 8, 0,
                                         FALSE, FALSE, &opened);
            primary_fallback = TRUE;
        }
        if (!NT_SUCCESS(status)) {
            SetLastError(RtlNtStatusToDosError(status));
            return FALSE;
        }
        token = opened;
    }
    const LOCAL_TOKEN_OBJECT *object = token_query_access(token, 8, NULL);
    if (opened) handle_close(&g_handle_table, opened);
    if (!object) return FALSE;

    DWORD error = 0;
    if (implicit && object->has_restrictions) {
        /* Token-object DACLs are not modeled yet. An already-open explicit
         * handle carries checked TOKEN_QUERY rights; acquiring one implicitly
         * while filtered must not silently bypass the missing access check. */
        error = ERROR_NOT_SUPPORTED;
    } else if (object->token_type != 2 && !primary_fallback) {
        error = 1309; /* ERROR_NO_IMPERSONATION_TOKEN */
    } else if (!primary_fallback && object->impersonation_level < 1) {
        error = 1346; /* ERROR_BAD_IMPERSONATION_LEVEL */
    } else {
        /* Membership is a read query. WRITE_RESTRICTED applies its second
         * SID pass only to write access; disabled/deny-only SIDs still fail. */
        *is_member = object->filter_flags & 8
            ? token_has_sid(object, sid, FALSE, FALSE)
            : token_membership_enabled(object, sid);
    }
    advapi32_token_release(object);
    SetLastError(error ? error : saved_error);
    return error == 0;
}

BOOL advapi32_effective_is_admin(void)
{
    PVOID object = NULL;
    NTSTATUS status = nt_thread_reference_impersonation_token(NT_CURRENT_THREAD, &object);
    if (status == STATUS_NO_TOKEN)
        status = nt_process_reference_primary_token(NT_CURRENT_PROCESS, &object);
    if (!NT_SUCCESS(status)) return FALSE;
    BOOL result = token_membership_enabled(object, &g_builtin_admin_sid);
    advapi32_token_release(object);
    return result;
}

static BOOL token_access_check(PVOID descriptor, const LOCAL_TOKEN_OBJECT *local,
                                     DWORD desired, const GENERIC_MAPPING *mapping,
                                     PVOID privileges, DWORD *privileges_length,
                                     DWORD *granted, BOOL *access_status)
{
    (void)privileges_length;
    if (local->token_type != 2) {
        SetLastError(1309); /* ERROR_NO_IMPERSONATION_TOKEN */
        return FALSE;
    }
    if (local->impersonation_level < 1) {
        SetLastError(1346); /* ERROR_BAD_IMPERSONATION_LEVEL */
        return FALSE;
    }
    if (desired & 0xf0000000U) {
        SetLastError(1360); /* ERROR_GENERIC_NOT_MAPPED */
        return FALSE;
    }
    DWORD saved_error = GetLastError();
    SetLastError(0);
    if (!IsValidSecurityDescriptor_stub(descriptor)) {
        DWORD error = GetLastError();
        SetLastError(error == 1336 ? error : 1338);
        return FALSE;
    }
    SetLastError(saved_error);
    if (
        !security_descriptor_part(descriptor, 0) ||
        !security_descriptor_part(descriptor, 1)) {
        SetLastError(1338); /* ERROR_INVALID_SECURITY_DESCR */
        return FALSE;
    }
    /* The current local tokens contain no privileges. In particular a NULL
     * DACL does not grant SeSecurityPrivilege. */
    if (desired & 0x01000000U) { /* ACCESS_SYSTEM_SECURITY */
        *granted = 0;
        *access_status = FALSE;
        SetLastError(1314); /* ERROR_PRIVILEGE_NOT_HELD */
        return TRUE;
    }
    USHORT control = ((SECURITY_DESCRIPTOR32 *)descriptor)->control;
    const ACL_HEADER *sacl = (control & SE_SACL_PRESENT)
        ? security_descriptor_part(descriptor, 2) : NULL;
    if (sacl && sacl->ace_count) {
        /* Mandatory labels/resource policies need a separate evaluator. */
        SetLastError(50); /* ERROR_NOT_SUPPORTED: never silently grant */
        return FALSE;
    }
    const ACL_HEADER *dacl = (control & SE_DACL_PRESENT)
        ? security_descriptor_part(descriptor, 3) : NULL;
    BOOL is_owner = token_has_sid(local, security_descriptor_part(descriptor, 0),
                                  FALSE, TRUE);
    BOOL owner_rights = FALSE;
    if (dacl) {
        DWORD offset = sizeof(*dacl);
        for (DWORD i = 0; i < dacl->ace_count; i++) {
            const BYTE *ace = (const BYTE *)dacl + offset;
            USHORT size = *(const USHORT *)(ace + 2);
            if (!(ace[1] & 8)) { /* INHERIT_ONLY_ACE does not apply */
                if (ace[0] > 1) {
                    SetLastError(50); /* object/callback ACEs are not evaluated */
                    return FALSE;
                }
                if (size < 16 || ace[8] != 1 || ace[9] > 15 ||
                    16U + 4U * ace[9] > size) {
                    SetLastError(1336); /* ERROR_INVALID_ACL */
                    return FALSE;
                }
                if (token_sid_equal(ace + 8, &g_owner_rights_sid))
                    owner_rights = TRUE;
            }
            offset += size; /* header and each ACE bound checked above */
        }
    }

    BOOL maximum = (desired & 0x02000000U) != 0;
    DWORD required = desired & ~0x02000000U;
    DWORD allowed = 0;
    for (DWORD pass = 0; pass < (local->restricted_count ? 2U : 1U); pass++) {
        LOCAL_TOKEN_OBJECT restricted = {
            .groups = local->restricted,
            .group_count = local->restricted_count,
        };
        const LOCAL_TOKEN_OBJECT *subject = pass ? &restricted : local;
        BOOL owner = is_owner && (!pass || token_has_sid(subject,
            security_descriptor_part(descriptor, 0), FALSE, FALSE));
        DWORD pass_allowed = owner && !owner_rights ? READ_CONTROL | WRITE_DAC : 0;
        DWORD denied = 0;
        DWORD write_mask = (mapping->GenericWrite & ~READ_CONTROL) |
                           DELETE | WRITE_DAC | WRITE_OWNER;
        DWORD pass_required = pass && (local->filter_flags & 8)
            ? required & write_mask : required;
        if (!dacl) {
            pass_allowed = maximum ? mapping->GenericAll | required : required;
        } else {
            DWORD offset = sizeof(*dacl);
            for (DWORD i = 0; i < dacl->ace_count; i++) {
                if (!maximum && !(pass_required & ~pass_allowed)) break;
                const BYTE *ace = (const BYTE *)dacl + offset;
                offset += *(const USHORT *)(ace + 2);
                if (ace[1] & 8) continue;
                BOOL matches = token_sid_equal(ace + 8, &g_owner_rights_sid)
                    ? owner : token_has_sid(subject, ace + 8, ace[0] == 1, FALSE);
                if (!matches) continue;
                DWORD mask = *(const DWORD *)(ace + 4);
                if (ace[0] == 1) {
                    denied |= mask & ~pass_allowed;
                    if (!maximum && (denied & pass_required)) break;
                } else {
                    pass_allowed |= mask & ~denied;
                }
            }
        }
        if (!pass) allowed = pass_allowed;
        else if (local->filter_flags & 8) {
            allowed &= pass_allowed | ~write_mask;
        } else allowed &= pass_allowed;
    }
    BOOL success = !(required & ~allowed) &&
                   (maximum ? allowed != 0 : required != 0);
    ((DWORD *)privileges)[1] = 0;
    *access_status = success;
    *granted = success ? (maximum ? allowed : required) : 0;
    if (!success) SetLastError(5);
    return TRUE;
}

static BOOL WINAPI AccessCheck_stub(PVOID descriptor, HANDLE token,
                                     DWORD desired, const GENERIC_MAPPING *mapping,
                                     PVOID privileges, DWORD *privileges_length,
                                     DWORD *granted, BOOL *access_status)
{
    if (!privileges_length || !granted || !access_status || !mapping) {
        SetLastError(87);
        return FALSE;
    }
    /* Buffer negotiation precedes token lookup on Windows. Even an invalid
     * handle clears PrivilegeCount once the buffer is large enough. */
    if (*privileges_length < 20) {
        *privileges_length = 20;
        SetLastError(122);
        return FALSE;
    }
    if (!privileges) { SetLastError(998); return FALSE; }
    *(DWORD *)privileges = 0;
    const LOCAL_TOKEN_OBJECT *local = token_query_access(token, 8, NULL);
    if (!local) return FALSE;
    BOOL result = token_access_check(descriptor, local, desired, mapping,
                                    privileges, privileges_length, granted, access_status);
    advapi32_token_release(local);
    return result;
}

static BOOL token_resolve_thread(HANDLE handle, PVOID *thread_object)
{
    if (!thread_object) return FALSE;

    ULONG_PTR value = (ULONG_PTR)handle;
    if (handle == NT_CURRENT_THREAD || (DWORD)value == 0xFFFFFFFEU) {
        *thread_object = win32_current_thread_object();
        return *thread_object != NULL;
    }

    return NT_SUCCESS(handle_lookup(&g_handle_table, handle, OBJ_TYPE_THREAD,
                                    thread_object));
}

static void token_store_handle(PHANDLE destination, HANDLE value)
{
    if (g_compat32_mode)
        *(DWORD *)(void *)destination = (DWORD)(ULONG_PTR)value;
    else
        *destination = value;
}

static BOOL WINAPI OpenThreadToken_stub(HANDLE thread, DWORD desired_access,
                                         BOOL open_as_self, PHANDLE token)
{
    return token_open_win32(thread, desired_access, TRUE, open_as_self, token);
}

NTSTATUS advapi32_reference_impersonation_token(HANDLE token, PVOID *object)
{
    if (!object) return STATUS_INVALID_PARAMETER;
    *object = NULL;
    if (token == NT_CURRENT_PROCESS || token == NT_CURRENT_THREAD)
        return STATUS_OBJECT_TYPE_MISMATCH;
    if ((ULONG_PTR)token > 0xffffffffU) return STATUS_INVALID_HANDLE;
    NTSTATUS status = handle_reference_token_for_process(&g_handle_table, token,
        win32_current_process_id(), 4, object, NULL); /* TOKEN_IMPERSONATE */
    if (NT_SUCCESS(status) && ((LOCAL_TOKEN_OBJECT *)*object)->token_type != 2) {
        advapi32_token_release(*object);
        *object = NULL;
        return STATUS_BAD_TOKEN_TYPE;
    }
    return status;
}

static BOOL WINAPI SetThreadToken_stub(PHANDLE thread, HANDLE token)
{
    HANDLE target = NT_CURRENT_THREAD;
    if (thread) {
        if (!win32_user_range_readable(thread, g_compat32_mode ? 4 : sizeof(HANDLE),
                                       g_compat32_mode)) {
            SetLastError(998);
            return FALSE;
        }
        target = g_compat32_mode
               ? (HANDLE)(ULONG_PTR)*(DWORD *)(void *)thread
               : *thread;
    }

    NTSTATUS status = nt_thread_assign_token_handle(target, token);
    if (!NT_SUCCESS(status)) {
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }

    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ImpersonateAnonymousToken_stub(HANDLE thread)
{
    PVOID thread_object = NULL;
    if (!token_resolve_thread(thread, &thread_object)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (!nt_thread_set_impersonation_token(thread_object,
                                           (PVOID)&g_anonymous_token)) {
        SetLastError(6);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI RevertToSelf_stub(void)
{
    PVOID thread_object = win32_current_thread_object();
    if (!nt_thread_set_impersonation_token(thread_object, NULL)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ImpersonateNamedPipeClient_stub(HANDLE pipe)
{
    PVOID object = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, pipe, OBJ_TYPE_FILE,
                                  &object))) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    PFILE_OBJECT file = (PFILE_OBJECT)object;
    if (!(file->flags & (FILE_OBJ_PIPE_READ | FILE_OBJ_PIPE_WRITE))) {
        SetLastError(6);
        return FALSE;
    }

    PVOID thread_object = win32_current_thread_object();
    if (!nt_thread_set_impersonation_token(
            thread_object, (PVOID)&g_local_impersonation_token)) {
        SetLastError(6);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL token_default_dacl_information(const LOCAL_TOKEN_OBJECT *token,
    PVOID information, DWORD length, DWORD *return_length)
{
    if (!win32_user_range_writable(return_length, sizeof(*return_length), g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    TOKEN_DEFAULT_ACL *policy = token_acl_reference(token);
    DWORD header = g_compat32_mode ? 4 : 8;
    DWORD size = policy ? policy->acl.size : 0;
    DWORD required = header + size;
    *return_length = required;
    DWORD error = 0;
    if (length < required) error = 122;
    else if (!win32_user_range_writable(information, required, g_compat32_mode)) error = 998;
    else {
        BYTE *bytes = information;
        security_store_pointer(bytes, policy ? bytes + header : NULL);
        if (policy) reg_memcpy(bytes + header, &policy->acl, size);
    }
    token_acl_release(policy);
    if (error) SetLastError(error);
    return error == 0;
}

static BOOL token_default_acl_valid(const ACL_HEADER *acl)
{
    if (!acl_measure(acl, NULL)) return FALSE;
    DWORD offset = sizeof(*acl);
    for (DWORD i = 0; i < acl->ace_count; i++) {
        const BYTE *ace = (const BYTE *)acl + offset;
        DWORD size = *(const USHORT *)(ace + 2);
        if (size & 3) return FALSE;
        /* Standard allow, deny, audit and alarm ACEs carry a SID at offset 8.
         * Other ACE layouts stay opaque here; AccessCheck rejects unsupported
         * evaluators instead of granting access on their behalf. */
        if (ace[0] <= 3 && (size < 16 || ace[8] != 1 || ace[9] > 15 ||
                            16U + 4U * ace[9] > size)) return FALSE;
        offset += size;
    }
    return TRUE;
}

static BOOL token_set_default_dacl(LOCAL_TOKEN_OBJECT *token, PVOID information, DWORD length)
{
    DWORD header = g_compat32_mode ? 4 : 8;
    if (length < header) { SetLastError(24); return FALSE; } /* ERROR_BAD_LENGTH */
    if (!win32_user_range_readable(information, header, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    const ACL_HEADER *input = g_compat32_mode
        ? (PCVOID)(ULONG_PTR)*(DWORD *)information : *(PCVOID *)information;
    TOKEN_DEFAULT_ACL *policy = NULL;
    if (input) {
        if (!win32_user_range_readable(input, sizeof(*input), g_compat32_mode)) {
            SetLastError(998);
            return FALSE;
        }
        ACL_HEADER captured;
        reg_memcpy(&captured, input, sizeof(captured));
        if (captured.size < sizeof(captured)) { SetLastError(1336); return FALSE; }
        if (!win32_user_range_readable(input, captured.size, g_compat32_mode)) {
            SetLastError(998);
            return FALSE;
        }
        policy = kmalloc(sizeof(*policy) - sizeof(policy->acl) + captured.size);
        if (!policy) { SetLastError(8); return FALSE; }
        policy->references = 1;
        reg_memcpy(&policy->acl, input, captured.size);
        if (policy->acl.size != captured.size || !token_default_acl_valid(&policy->acl)) {
            kfree(policy);
            SetLastError(1336);
            return FALSE;
        }
        __atomic_add_fetch(&g_token_live_policies, 1, __ATOMIC_RELAXED);
    }
    /* Readers pin an immutable ACL. Allocation, guest access and reclamation
     * stay outside the publication lock, including queries concurrent with set. */
    if (!token->allocated) { /* Only internal identity templates are static. */
        token_acl_release(policy);
        SetLastError(50);
        return FALSE;
    }
    uint64_t flags = token_policy_lock();
    TOKEN_DEFAULT_ACL *previous = token->default_dacl;
    token->default_dacl = policy;
    token->modified_id = __atomic_add_fetch(&g_token_next_luid, 1, __ATOMIC_RELAXED);
    token_policy_unlock(flags);
    token_acl_release(previous);
    return TRUE;
}

static BOOL token_integrity_information(const LOCAL_TOKEN_OBJECT *token,
    PVOID information, DWORD length, DWORD *returned)
{
    if (!win32_user_range_writable(returned, sizeof(*returned), g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    DWORD header = g_compat32_mode ? 8 : 16;
    *returned = header + sizeof(g_local_integrity_sid);
    if (length < *returned) { SetLastError(122); return FALSE; }
    if (!win32_user_range_writable(information, *returned, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    LOCAL_INTEGRITY_SID sid = g_local_integrity_sid;
    uint64_t flags = token_policy_lock();
    sid.sub_authority[0] = token->integrity_level;
    token_policy_unlock(flags);
    BYTE *bytes = information;
    reg_memset(bytes, 0, header);
    security_store_pointer(bytes, bytes + header);
    *(DWORD *)(bytes + (g_compat32_mode ? 4 : 8)) = 0x60;
    reg_memcpy(bytes + header, &sid, sizeof(sid));
    return TRUE;
}

static BOOL token_set_integrity(LOCAL_TOKEN_OBJECT *token, PVOID information, DWORD length)
{
    DWORD header = g_compat32_mode ? 8 : 16;
    if (length < header) { SetLastError(24); return FALSE; }
    if (!win32_user_range_readable(information, header, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    BYTE label[16] __attribute__((aligned(8)));
    reg_memcpy(label, information, header);
    DWORD attributes = *(DWORD *)(label + (g_compat32_mode ? 4 : 8));
    if (attributes & ~0xe000007fU) { SetLastError(87); return FALSE; }
    const BYTE *input = g_compat32_mode
        ? (PCVOID)(ULONG_PTR)*(DWORD *)label : *(PCVOID *)label;
    if (!win32_user_range_readable(input, 8, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    BYTE sid[68] __attribute__((aligned(4)));
    reg_memcpy(sid, input, 8);
    DWORD size = 8U + 4U * sid[1];
    if (sid[0] != 1 || size > sizeof(sid)) { SetLastError(1337); return FALSE; }
    if (!win32_user_range_readable(input, size, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    reg_memcpy(sid, input, size);
    if (sid[0] != 1 || size != 8U + 4U * sid[1]) { SetLastError(1337); return FALSE; }
    for (DWORD i = 2; i < 8; i++)
        if (sid[i] != (i == 7 ? 16 : 0)) {
            SetLastError(1299); /* ERROR_INVALID_LABEL */
            return FALSE;
        }
    /* NT normalizes an integrity SID to its final RID (zero if absent). */
    DWORD level = sid[1] ? *(DWORD *)(sid + size - 4) : 0;
    DWORD error = 0;
    uint64_t flags = token_policy_lock();
    if (!token->allocated) error = 50;
    else if (level > token->integrity_level) error = 1314; /* No SeTcbPrivilege. */
    else {
        token->integrity_level = level;
        token->modified_id = __atomic_add_fetch(&g_token_next_luid, 1, __ATOMIC_RELAXED);
    }
    token_policy_unlock(flags);
    if (error) SetLastError(error);
    return !error;
}

static BOOL token_information(const LOCAL_TOKEN_OBJECT *local_token,
                                             DWORD information_class,
                                             PVOID information,
                                             DWORD information_length,
                                             DWORD *return_length)
{
    if (information_class == 6)
        return token_default_dacl_information(local_token, information,
                                              information_length, return_length);
    if (information_class == 25)
        return token_integrity_information(local_token, information,
                                            information_length, return_length);
    if (!return_length) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    const LOCAL_TOKEN_GROUP *groups = information_class == 11
        ? local_token->restricted : local_token->groups;
    DWORD group_count = information_class == 11
        ? local_token->restricted_count : local_token->group_count;
    DWORD required = 0;
    DWORD value = 0;
    BOOL fixed_dword = FALSE;

    switch (information_class) {
    case 1:  /* TokenUser */
        required = (g_compat32_mode ? 8 : 16) + local_token->user_sid_size;
        break;
    case 2:  /* TokenGroups */
    case 11: /* TokenRestrictedSids */
        required = g_compat32_mode ? 4 : 8;
        required += group_count * (g_compat32_mode ? 8 : 16);
        for (DWORD i = 0; i < group_count; i++) required += groups[i].sid_size;
        if (information_class == 2)
            required += (g_compat32_mode ? 8 : 16) + sizeof(g_local_integrity_sid);
        break;
    case 3:  /* TokenPrivileges */
        /* Windows requires sizeof(TOKEN_PRIVILEGES), even with count zero. */
        required = sizeof(DWORD) * 4;
        break;
    case 4:  /* TokenOwner */
    case 5:  /* TokenPrimaryGroup */
        required = (g_compat32_mode ? 4 : 8) + local_token->user_sid_size;
        break;
    case 7:  /* TokenSource */
        required = 16;
        break;
    case 8:  /* TokenType */
        value = local_token->token_type;
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 9:  /* TokenImpersonationLevel */
        value = local_token->impersonation_level;
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 10: /* TokenStatistics */
        required = 56;
        break;
    case 12: /* TokenSessionId */
        value = 1;
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 21: /* TokenHasRestrictions */
        value = local_token->has_restrictions;
        required = 1; /* BOOLEAN, not BOOL */
        break;
    case 15: /* TokenSandBoxInert: no SYSTEM/TrustedInstaller identity */
    case 23: /* TokenVirtualizationAllowed */
    case 24: /* TokenVirtualizationEnabled */
    case 26: /* TokenUIAccess */
    case 29: /* TokenIsAppContainer */
        value = 0;
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 18: /* TokenElevationType */
        value = 1; /* TokenElevationTypeDefault */
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 20: /* TokenElevation */
        value = token_membership_enabled(local_token, &g_builtin_admin_sid);
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 27: /* TokenMandatoryPolicy */
        value = 1; /* TOKEN_MANDATORY_POLICY_NO_WRITE_UP */
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 28: /* TokenLogonSid: this local session has no logon SID group. */
        *return_length = 0;
        SetLastError(1168); /* ERROR_NOT_FOUND, not an invalid info class */
        return FALSE;
    case 31: /* TokenAppContainerSid */
        required = g_compat32_mode ? 4 : 8;
        break;
    default:
        *return_length = 0;
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    *return_length = required;
    if (!information || information_length < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    BYTE *bytes = (BYTE *)information;
    for (DWORD i = 0; i < required; i++) bytes[i] = 0;

    if (information_class == 21) {
        *bytes = (BYTE)value;
    } else if (fixed_dword) {
        *(DWORD *)(void *)bytes = value;
    } else if (information_class == 1) {
        DWORD header_size = g_compat32_mode ? 8 : 16;
        PVOID sid = bytes + header_size;
        security_store_pointer(bytes, sid);
        *(DWORD *)(void *)(bytes + (g_compat32_mode ? 4 : 8)) = local_token->user_attributes;
        reg_memcpy(sid, local_token->user_sid, local_token->user_sid_size);
    } else if (information_class == 2 || information_class == 11) {
        DWORD header = g_compat32_mode ? 4 : 8;
        DWORD stride = g_compat32_mode ? 8 : 16;
        DWORD count = group_count + (information_class == 2);
        BYTE *sid = bytes + header + stride * count;
        *(DWORD *)bytes = count;
        for (DWORD i = 0; i < group_count; i++) {
            const LOCAL_TOKEN_GROUP *group = &groups[i];
            BYTE *entry = bytes + header + i * stride;
            security_store_pointer(entry, sid);
            *(DWORD *)(entry + (g_compat32_mode ? 4 : 8)) = group->attributes;
            reg_memcpy(sid, group->sid, group->sid_size);
            sid += group->sid_size;
        }
        if (information_class == 2) {
            BYTE *entry = bytes + header + group_count * stride;
            LOCAL_INTEGRITY_SID integrity = g_local_integrity_sid;
            uint64_t flags = token_policy_lock();
            integrity.sub_authority[0] = local_token->integrity_level;
            token_policy_unlock(flags);
            security_store_pointer(entry, sid);
            *(DWORD *)(entry + stride / 2) = 0x60;
            reg_memcpy(sid, &integrity, sizeof(integrity));
        }
    } else if (information_class == 4 || information_class == 5) {
        DWORD header_size = g_compat32_mode ? 4 : 8;
        PVOID sid = bytes + header_size;
        security_store_pointer(bytes, sid);
        reg_memcpy(sid, local_token->user_sid, local_token->user_sid_size);
    } else if (information_class == 7) {
        const char source[] = "OsitoK";
        for (DWORD i = 0; i < sizeof(source) - 1; i++) bytes[i] = source[i];
        *(DWORD *)(void *)(bytes + 8) = win32_current_process_id();
    } else if (information_class == 10) {
        reg_memcpy(bytes, &local_token->token_id, sizeof(local_token->token_id));
        reg_memcpy(bytes + 8, &local_token->authentication_id,
                   sizeof(local_token->authentication_id));
        *(DWORD *)(void *)(bytes + 24) = local_token->token_type;
        *(DWORD *)(void *)(bytes + 28) = local_token->impersonation_level;
        uint64_t flags = token_policy_lock();
        DWORD charged = local_token->user_sid_size +
            (local_token->default_dacl ? local_token->default_dacl->acl.size : 0);
        ULONGLONG modified = local_token->modified_id;
        token_policy_unlock(flags);
        *(DWORD *)(void *)(bytes + 32) = charged;
        reg_memcpy(bytes + 48, &modified, sizeof(modified));
        *(DWORD *)(void *)(bytes + 40) = local_token->group_count + 1;
    }
    if (information_class != 3)
        SetLastError(0);
    return TRUE;
}

static BOOL WINAPI GetTokenInformation_stub(HANDLE token, DWORD information_class,
                                            PVOID information, DWORD information_length,
                                            DWORD *return_length)
{
    const LOCAL_TOKEN_OBJECT *local = token_query_access(
        token, information_class == 7 ? 0x10 : 8, NULL);
    if (!local) { if (return_length) *return_length = 0; return FALSE; }
    BOOL result = token_information(local, information_class, information,
                                    information_length, return_length);
    advapi32_token_release(local);
    return result;
}

typedef struct {
    DWORD low_part;
    LONG high_part;
} ADVAPI_LUID;

typedef struct {
    const char *name;
    DWORD value;
} ADVAPI_PRIVILEGE;

static const ADVAPI_PRIVILEGE advapi_privileges[] = {
    {"SeCreateTokenPrivilege", 2},
    {"SeAssignPrimaryTokenPrivilege", 3},
    {"SeLockMemoryPrivilege", 4},
    {"SeIncreaseQuotaPrivilege", 5},
    {"SeTcbPrivilege", 7},
    {"SeSecurityPrivilege", 8},
    {"SeTakeOwnershipPrivilege", 9},
    {"SeLoadDriverPrivilege", 10},
    {"SeSystemProfilePrivilege", 11},
    {"SeSystemtimePrivilege", 12},
    {"SeProfileSingleProcessPrivilege", 13},
    {"SeIncreaseBasePriorityPrivilege", 14},
    {"SeCreatePagefilePrivilege", 15},
    {"SeCreatePermanentPrivilege", 16},
    {"SeBackupPrivilege", 17},
    {"SeRestorePrivilege", 18},
    {"SeShutdownPrivilege", 19},
    {"SeDebugPrivilege", 20},
    {"SeAuditPrivilege", 21},
    {"SeSystemEnvironmentPrivilege", 22},
    {"SeChangeNotifyPrivilege", 23},
    {"SeRemoteShutdownPrivilege", 24},
    {"SeUndockPrivilege", 25},
    {"SeSyncAgentPrivilege", 26},
    {"SeEnableDelegationPrivilege", 27},
    {"SeManageVolumePrivilege", 28},
    {"SeImpersonatePrivilege", 29},
    {"SeCreateGlobalPrivilege", 30},
    {"SeTrustedCredManAccessPrivilege", 31},
    {"SeRelabelPrivilege", 32},
    {"SeIncreaseWorkingSetPrivilege", 33},
    {"SeTimeZonePrivilege", 34},
    {"SeCreateSymbolicLinkPrivilege", 35},
};

static BOOL WINAPI LookupPrivilegeValueA_stub(PCSTR system_name,
                                               PCSTR privilege_name,
                                               ADVAPI_LUID *luid)
{
    (void)system_name;
    if (!privilege_name || !luid) {
        SetLastError(87);
        return FALSE;
    }
    for (SIZE_T i = 0;
         i < sizeof(advapi_privileges) / sizeof(advapi_privileges[0]); i++) {
        if (reg_stricmp(privilege_name, advapi_privileges[i].name) != 0)
            continue;
        luid->low_part = advapi_privileges[i].value;
        luid->high_part = 0;
        SetLastError(0);
        return TRUE;
    }
    SetLastError(1313); /* ERROR_NO_SUCH_PRIVILEGE */
    return FALSE;
}

static BOOL WINAPI LookupPrivilegeValueW_stub(PCWSTR system_name,
                                               PCWSTR privilege_name,
                                               ADVAPI_LUID *luid)
{
    char system[64], privilege[96];
    wide_to_ansi(system, system_name, sizeof(system));
    wide_to_ansi(privilege, privilege_name, sizeof(privilege));
    return LookupPrivilegeValueA_stub(system_name ? system : NULL,
                                      privilege_name ? privilege : NULL, luid);
}

static BOOL WINAPI DuplicateTokenEx_stub(HANDLE existing_token,
                                          DWORD desired_access,
                                          PVOID token_attributes,
                                          DWORD impersonation_level,
                                          DWORD token_type,
                                          PHANDLE new_token)
{
    if (!new_token || impersonation_level > 3 ||
        (token_type != 1 && token_type != 2)) {
        SetLastError(87);
        return FALSE;
    }
    DWORD source_access = 0;
    const LOCAL_TOKEN_OBJECT *source = token_query_access(existing_token, 2,
                                                          &source_access);
    if (!source) return FALSE;
    if (source->token_type == 2 &&
        ((token_type == 2 && impersonation_level > source->impersonation_level) ||
         (token_type == 1 && source->impersonation_level < 2))) {
        advapi32_token_release(source);
        SetLastError(1346); /* ERROR_BAD_IMPERSONATION_LEVEL */
        return FALSE;
    }
    BOOL inherit = FALSE;
    if (token_attributes) {
        const BYTE *attributes = token_attributes;
        PVOID sd = g_compat32_mode
            ? (PVOID)(ULONG_PTR)*(const DWORD *)(attributes + 4)
            : *(PVOID const *)(attributes + 8);
        if (sd) { /* Token object ACLs are not implemented yet. */
            advapi32_token_release(source);
            SetLastError(50);
            return FALSE;
        }
        inherit = *(const DWORD *)(attributes + (g_compat32_mode ? 8 : 16)) != 0;
    }
    LOCAL_TOKEN_OBJECT *token_object = token_copy(source, source->restricted,
                                                 source->restricted_count);
    advapi32_token_release(source);
    if (!token_object) return FALSE;
    token_object->token_type = token_type;
    token_object->impersonation_level = token_type == 1 ? 2 : impersonation_level;
    DWORD access = desired_access ? token_map_access(desired_access) : source_access;
    HANDLE duplicate = NULL;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_TOKEN,
                                   access, (PVOID)token_object, &duplicate);
    advapi32_token_release(token_object);
    if (!NT_SUCCESS(status)) {
        SetLastError(8);
        return FALSE;
    }
    if (inherit)
        handle_update_flags_for_process(&g_handle_table, duplicate,
                                        win32_current_process_id(),
                                        HANDLE_USER_FLAG_INHERIT,
                                        HANDLE_USER_FLAG_INHERIT);
    if (g_compat32_mode)
        *(DWORD *)(void *)new_token = (DWORD)(ULONG_PTR)duplicate;
    else
        *new_token = duplicate;
    return TRUE;
}

/* Snapshot SID_AND_ATTRIBUTES using the caller's pointer width. All SIDs are
 * copied before filtering, so the resulting token owns no guest pointers. */
static BOOL token_capture_groups(PCVOID input, DWORD count, LOCAL_TOKEN_GROUP **result)
{
    *result = NULL;
    if (!count) return TRUE;
    SIZE_T stride = g_compat32_mode ? 8 : 16;
    ULONGLONG bytes = (ULONGLONG)count * (sizeof(LOCAL_TOKEN_GROUP) + 68);
    if (bytes > 0xffffffffU) { SetLastError(87); return FALSE; }
    if (!win32_user_range_readable(input, (SIZE_T)count * stride, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    LOCAL_TOKEN_GROUP *groups = kmalloc(bytes);
    if (!groups) { SetLastError(8); return FALSE; }
    BYTE *sids = (void *)(groups + count);
    for (DWORD i = 0; i < count; i++) {
        const BYTE *entry = (const BYTE *)input + i * stride;
        const BYTE *sid = g_compat32_mode
            ? (PCVOID)(ULONG_PTR)*(const DWORD *)entry : *(PCVOID const *)entry;
        if (!win32_user_range_readable(sid, 8, g_compat32_mode)) goto inaccessible;
        DWORD size = 8U + 4U * sid[1];
        if (sid[0] != 1 || size > 68) goto invalid;
        if (!win32_user_range_readable(sid, size, g_compat32_mode)) goto inaccessible;
        BYTE *copy = sids + i * 68U;
        reg_memcpy(copy, sid, size);
        if (copy[0] != 1 || 8U + 4U * copy[1] != size) goto invalid;
        groups[i] = (LOCAL_TOKEN_GROUP){copy, size, *(const DWORD *)(entry + stride / 2)};
    }
    *result = groups;
    return TRUE;
inaccessible:
    kfree(groups);
    SetLastError(998);
    return FALSE;
invalid:
    kfree(groups);
    SetLastError(1337); /* ERROR_INVALID_SID */
    return FALSE;
}

static BOOL token_create_restricted(HANDLE existing, DWORD flags,
    DWORD disable_count, PCVOID disable_sids, DWORD privilege_count,
    PCVOID delete_privileges, DWORD restrict_count, PCVOID restrict_sids, PHANDLE output)
{
    if (!output || (flags & ~15U)) { SetLastError(87); return FALSE; }
    if (flags & 4) { SetLastError(50); return FALSE; } /* LUA token policy */
    if (!win32_user_range_writable(output, g_compat32_mode ? 4 : 8, g_compat32_mode)) {
        SetLastError(998);
        return FALSE;
    }
    DWORD access;
    const LOCAL_TOKEN_OBJECT *source = token_query_access(existing, 2, &access);
    if (!source) return FALSE;
    LOCAL_TOKEN_GROUP *disabled = NULL, *restricted = NULL;
    LOCAL_TOKEN_OBJECT *copy = NULL;
    BOOL result = FALSE;
    if (privilege_count && !win32_user_range_readable(delete_privileges,
            (SIZE_T)privilege_count * 12U, g_compat32_mode)) {
        SetLastError(998);
        goto done;
    }
    if (source->restricted_count && !(source->filter_flags & 8) && (flags & 8)) {
        SetLastError(87); /* Filtering cannot weaken full to write-only. */
        goto done;
    }
    /* The local identity has no privileges to remove. DISABLE_MAX_PRIVILEGE
     * ignores PrivilegesToDelete, as Windows does. */
    if (!(flags & 1) && privilege_count) { SetLastError(87); goto done; }
    if (!token_capture_groups(disable_sids, disable_count, &disabled) ||
        !token_capture_groups(restrict_sids, restrict_count, &restricted)) goto done;
    DWORD kept = 0;
    for (DWORD i = 0; i < restrict_count; i++) {
        BOOL present = source->restricted_count == 0;
        for (DWORD j = 0; j < source->restricted_count; j++)
            if (token_sid_equal(restricted[i].sid, source->restricted[j].sid)) present = TRUE;
        if (present) {
            restricted[kept] = restricted[i];
            restricted[kept++].attributes = 7;
        }
    }
    if (source->restricted_count && !kept) { SetLastError(87); goto done; }
    copy = token_copy(source, restricted, kept);
    if (!copy) goto done;
    copy->has_restrictions = TRUE;
    /* An all-access restriction cannot be weakened to write-only by filtering
     * it again. SANDBOX_INERT is ignored for this non-SYSTEM local identity. */
    copy->filter_flags = source->restricted_count
        ? source->filter_flags & flags & 8U : flags & 8U;
    for (DWORD i = 0; i < disable_count; i++) {
        if (token_sid_equal(copy->user_sid, disabled[i].sid))
            copy->user_attributes = 0x10;
        LOCAL_TOKEN_GROUP *groups = (LOCAL_TOKEN_GROUP *)copy->groups;
        for (DWORD j = 0; j < copy->group_count; j++)
            if (token_sid_equal(groups[j].sid, disabled[i].sid))
                groups[j].attributes = 0x10;
    }
    HANDLE handle;
    if (!NT_SUCCESS(handle_alloc(&g_handle_table, OBJ_TYPE_TOKEN, access, copy, &handle))) {
        SetLastError(8);
        goto done;
    }
    token_store_handle(output, handle);
    result = TRUE;
done:
    advapi32_token_release(copy);
    if (restricted) kfree(restricted);
    if (disabled) kfree(disabled);
    advapi32_token_release(source);
    return result;
}

static BOOL WINAPI CreateRestrictedToken_impl(HANDLE existing, DWORD flags,
    DWORD disable_count, PCVOID disable_sids, DWORD privilege_count,
    PCVOID delete_privileges, DWORD restrict_count, PCVOID restrict_sids, PHANDLE output)
{
    BOOL result = token_create_restricted(existing, flags, disable_count, disable_sids,
        privilege_count, delete_privileges, restrict_count, restrict_sids, output);
    if (!result) {
        DWORD error = GetLastError();
        static DWORD failure_logs;
        if (__atomic_add_fetch(&failure_logs, 1, __ATOMIC_RELAXED) <= 32) {
            serial_puts("[TOKEN-FILTER] failed pid="); serial_putdec(win32_current_process_id());
            serial_puts(" error="); serial_putdec(error);
            serial_puts(" flags=0x"); serial_puthex(flags, 8);
            serial_puts(" disabled/restricted="); serial_putdec(disable_count);
            serial_puts("/"); serial_putdec(restrict_count);
            serial_puts("\n");
        }
        SetLastError(error);
    }
    return result;
}

static BOOL WINAPI IsTokenRestricted_impl(HANDLE handle)
{
    const LOCAL_TOKEN_OBJECT *token = token_query_access(handle, 8, NULL);
    if (!token) return FALSE;
    BOOL result = token->restricted_count != 0;
    advapi32_token_release(token);
    return result;
}

static BOOL WINAPI AdjustTokenPrivileges_stub(HANDLE token,
                                               BOOL disable_all,
                                               PVOID new_state,
                                               DWORD buffer_length,
                                               PVOID previous_state,
                                               DWORD *return_length)
{
    PVOID token_object = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, token, OBJ_TYPE_TOKEN,
                                  &token_object))) {
        if (return_length) *return_length = 0;
        SetLastError(6);
        return FALSE;
    }
    (void)token_object;
    if (!disable_all && !new_state) {
        if (return_length) *return_length = 0;
        SetLastError(87);
        return FALSE;
    }
    DWORD count = new_state ? *(DWORD *)new_state : 0;
    if (count > 128) {
        SetLastError(87);
        return FALSE;
    }
    DWORD required = sizeof(DWORD) + count * 12;
    if (return_length) *return_length = required;
    if (previous_state) {
        if (buffer_length < required) {
            SetLastError(122);
            return FALSE;
        }
        if (new_state) reg_memcpy(previous_state, new_state, required);
        else *(DWORD *)previous_state = 0;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetTokenInformation_stub(HANDLE token,
                                             DWORD information_class,
                                             PVOID information,
                                             DWORD information_length)
{
    const LOCAL_TOKEN_OBJECT *subject = token_query_access(token,
        information_class == 12 ? 0x100 : 0x80, NULL);
    if (!subject) return FALSE;
    if (information_class == 6) {
        BOOL result = token_set_default_dacl((LOCAL_TOKEN_OBJECT *)subject,
                                             information, information_length);
        advapi32_token_release(subject);
        return result;
    }
    if (information_class == 25) {
        BOOL result = token_set_integrity((LOCAL_TOKEN_OBJECT *)subject,
                                          information, information_length);
        advapi32_token_release(subject);
        return result;
    }
    BOOL restricted = subject->has_restrictions;
    advapi32_token_release(subject);
    if (restricted) {
        serial_puts("[ADVAPI32] SetTokenInformation: restricted token policy unsupported, class=");
        serial_putdec(information_class);
        serial_puts("\n");
        SetLastError(50);
        return FALSE;
    }
    DWORD minimum = sizeof(DWORD);
    switch (information_class) {
    case 4:  /* TokenOwner */
    case 5:  /* TokenPrimaryGroup */
        minimum = g_compat32_mode ? 4 : 8;
        break;
    case 12: /* TokenSessionId */
    case 15: /* TokenSandBoxInert */
    case 23: /* TokenVirtualizationAllowed */
    case 24: /* TokenVirtualizationEnabled */
    case 26: /* TokenUIAccess */
    case 27: /* TokenMandatoryPolicy */
    case 29: /* TokenIsAppContainer */
        minimum = sizeof(DWORD);
        break;
    default:
        SetLastError(87);
        return FALSE;
    }
    if (!information || information_length < minimum) {
        SetLastError(87);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI CopySid_stub(DWORD destination_length,
                                PVOID destination_sid, PVOID source_sid)
{
    DWORD source_length = GetLengthSid_stub(source_sid);
    if (!destination_sid || !source_length) return FALSE;
    if (destination_length < source_length) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    reg_memcpy(destination_sid, source_sid, source_length);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI EqualSid_stub(PVOID first_sid, PVOID second_sid)
{
    DWORD first_length = GetLengthSid_stub(first_sid);
    DWORD second_length = GetLengthSid_stub(second_sid);
    if (!first_length || first_length != second_length) return FALSE;
    const BYTE *first = (const BYTE *)first_sid;
    const BYTE *second = (const BYTE *)second_sid;
    for (DWORD i = 0; i < first_length; i++)
        if (first[i] != second[i]) return FALSE;
    SetLastError(0);
    return TRUE;
}

static BOOL token_primary_assignable(const LOCAL_TOKEN_OBJECT *subject,
                                     const LOCAL_TOKEN_OBJECT *caller)
{
    if (subject->authentication_id != caller->authentication_id ||
        !token_sid_equal(subject->user_sid, caller->user_sid)) return FALSE;
    if (!(subject->user_attributes & 0x10) && (caller->user_attributes & 0x10))
        return FALSE;
    for (DWORD i = 0; i < subject->group_count; i++) {
        const LOCAL_TOKEN_GROUP *group = &subject->groups[i];
        if ((group->attributes & 4) && !(group->attributes & 0x10) &&
            !token_has_sid(caller, group->sid, FALSE, FALSE)) return FALSE;
    }
    for (DWORD i = 0; i < caller->group_count; i++) {
        const LOCAL_TOKEN_GROUP *group = &caller->groups[i];
        if ((group->attributes & (4 | 0x10)) &&
            !token_has_sid(subject, group->sid, TRUE, FALSE)) return FALSE;
    }
    if (caller->restricted_count) {
        if (!subject->restricted_count ||
            (!(caller->filter_flags & 8) && (subject->filter_flags & 8))) return FALSE;
        for (DWORD i = 0; i < subject->restricted_count; i++) {
            BOOL found = FALSE;
            for (DWORD j = 0; j < caller->restricted_count; j++)
                if (token_sid_equal(subject->restricted[i].sid, caller->restricted[j].sid))
                    found = TRUE;
            if (!found) return FALSE;
        }
    }
    return TRUE;
}

static BOOL token_process_attributes(PCVOID attributes, BOOL *inherit)
{
    *inherit = FALSE;
    if (!attributes) return TRUE;
    DWORD size = g_compat32_mode ? 12 : 24;
    if (!win32_user_range_readable(attributes, size, g_compat32_mode)) {
        SetLastError(998); return FALSE;
    }
    const BYTE *bytes = attributes;
    if (*(const DWORD *)bytes < size) { SetLastError(87); return FALSE; }
    PCVOID descriptor = g_compat32_mode
        ? (PCVOID)(ULONG_PTR)*(const DWORD *)(bytes + 4)
        : *(PCVOID *)(bytes + 8);
    /* Process object ACL storage/enforcement is separate from token policy. */
    if (descriptor) { SetLastError(50); return FALSE; }
    *inherit = *(const DWORD *)(bytes + (g_compat32_mode ? 8 : 16)) != 0;
    return TRUE;
}

static BOOL WINAPI CreateProcessAsUserW_compat(
    HANDLE token, PCWSTR application_name, PWSTR command_line,
    PVOID process_attributes, PVOID thread_attributes, BOOL inherit_handles,
    DWORD creation_flags, PVOID environment, PCWSTR current_directory,
    PVOID startup_info, PVOID process_information)
{
    const LOCAL_TOKEN_OBJECT *subject = NULL;
    if (token) {
        subject = token_query_access(token, 0xb, NULL);
        if (!subject) return FALSE;
        PVOID caller = NULL;
        DWORD error = subject->token_type == 2 && subject->impersonation_level < 2
            ? 1346 : 0;
        if (!error) {
            NTSTATUS status = nt_process_reference_primary_token(NT_CURRENT_PROCESS, &caller);
            if (!NT_SUCCESS(status)) error = 6;
            /* No SE_ASSIGNPRIMARYTOKEN privilege exists in the local identity.
             * Same-logon copies and further restrictions cannot grant more. */
            else if (!token_primary_assignable(subject, caller)) error = 1314;
        }
        advapi32_token_release(caller);
        if (error) {
            advapi32_token_release(subject);
            SetLastError(error);
            return FALSE;
        }
    }
    BOOL result = FALSE, inherit_process, inherit_thread;
    if (!token_process_attributes(process_attributes, &inherit_process) ||
        !token_process_attributes(thread_attributes, &inherit_thread)) goto done;
    if (!win32_user_range_writable(process_information, g_compat32_mode ? 16 : 24,
                                   g_compat32_mode) ||
        !win32_user_range_readable(startup_info, g_compat32_mode ? 68 : 104,
                                  g_compat32_mode)) {
        SetLastError(998); goto done;
    }
    BYTE result_information[24] = {0};
    result = kernel32_create_process_token_w(subject, application_name, command_line,
        process_attributes, thread_attributes, inherit_handles, creation_flags,
        environment, current_directory, startup_info, result_information);
    if (result) {
        HANDLE process = g_compat32_mode
            ? (HANDLE)(ULONG_PTR)*(DWORD *)result_information : *(HANDLE *)result_information;
        HANDLE thread = g_compat32_mode
            ? (HANDLE)(ULONG_PTR)*((DWORD *)result_information + 1)
            : *((HANDLE *)result_information + 1);
        if (inherit_process) handle_update_flags_for_process(&g_handle_table, process,
            win32_current_process_id(), 1, 1);
        if (inherit_thread) handle_update_flags_for_process(&g_handle_table, thread,
            win32_current_process_id(), 1, 1);
        reg_memcpy(process_information, result_information, g_compat32_mode ? 16 : 24);
    }
done:
    advapi32_token_release(subject);
    return result;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

typedef ULONGLONG TRACEGUID_HANDLE;

typedef struct {
    LPCGUID guid;
    HANDLE reg_handle;
} TRACE_GUID_REGISTRATION64;

typedef struct {
    DWORD guid;
    DWORD reg_handle;
} TRACE_GUID_REGISTRATION32;

static ULONG WINAPI RegisterTraceGuidsW_stub(PVOID request_address,
                                              PVOID request_context,
                                              LPCGUID control_guid,
                                              ULONG guid_count,
                                              PVOID trace_guid_reg,
                                              PCWSTR mof_image_path,
                                              PCWSTR mof_resource_name,
                                              TRACEGUID_HANDLE *registration_handle)
{
    (void)request_context;
    (void)mof_image_path;
    (void)mof_resource_name;
    if (!request_address || !control_guid || !registration_handle)
        return 87; /* ERROR_INVALID_PARAMETER */

    /* ponytail: ETW has no controller yet, so providers stay disabled. */
    *registration_handle = 1;
    if (trace_guid_reg) {
        for (ULONG i = 0; i < guid_count; i++) {
            if (g_compat32_mode)
                ((TRACE_GUID_REGISTRATION32 *)trace_guid_reg)[i].reg_handle = 1;
            else
                ((TRACE_GUID_REGISTRATION64 *)trace_guid_reg)[i].reg_handle =
                    (HANDLE)(ULONG_PTR)1;
        }
    }
    return ERROR_SUCCESS;
}

static ULONG WINAPI UnregisterTraceGuids_stub(TRACEGUID_HANDLE registration_handle)
{
    return registration_handle ? ERROR_SUCCESS : 6; /* ERROR_INVALID_HANDLE */
}

#define ETW_MAX_SESSIONS 16
#define ETW_MAX_CONSUMERS 32
#define ETW_INVALID_HANDLE 0xFFFFFFFFFFFFFFFFULL

typedef struct {
    BOOL used;
    ULONGLONG handle;
    DWORD owner_process_id;
    BOOL enabled;
    char name[64];
} ETW_SESSION;

typedef struct {
    BOOL used;
    ULONGLONG handle;
    DWORD owner_process_id;
} ETW_CONSUMER;

static ETW_SESSION etw_sessions[ETW_MAX_SESSIONS];
static ETW_CONSUMER etw_consumers[ETW_MAX_CONSUMERS];
static DWORD etw_next_handle = 1;
static spinlock_t etw_lock = SPINLOCK_INIT;

static uint64_t etw_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&etw_lock);
    return flags;
}

static void etw_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&etw_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static ETW_SESSION *etw_find_session_locked(ULONGLONG handle,
                                            PCSTR session_name)
{
    for (int i = 0; i < ETW_MAX_SESSIONS; i++) {
        ETW_SESSION *session = &etw_sessions[i];
        if (!session->used) continue;
        if ((handle && session->handle == handle) ||
            (!handle && session_name &&
             reg_stricmp(session->name, session_name) == 0))
            return session;
    }
    return NULL;
}

/* Classic providers remain disabled until an ETW event sink is implemented.
 * Still expose their import: rejecting a message must not prevent the provider
 * DLL from loading. Do not inspect variadic payloads or report an event written
 * when there is nowhere to record it. LastError belongs to the caller. */
static ULONG etw_trace_message(ULONGLONG logger_handle)
{
    uint64_t flags = etw_lock_irqsave();
    BOOL exists = etw_find_session_locked(logger_handle, NULL) != NULL;
    etw_unlock_irqrestore(flags);
    return exists ? 50 : 6; /* ERROR_NOT_SUPPORTED / ERROR_INVALID_HANDLE */
}

static ULONG WINAPI TraceMessage_k32(DWORD handle_low, DWORD handle_high,
                                     ULONG message_flags, LPCGUID message_guid,
                                     ULONG message_number)
{
    (void)message_flags;
    (void)message_guid;
    (void)message_number;
    return etw_trace_message((ULONGLONG)handle_low |
                             ((ULONGLONG)handle_high << 32));
}

static ULONG WINAPI TraceMessage_k64(ULONGLONG logger_handle,
                                     ULONG message_flags, LPCGUID message_guid,
                                     ULONG message_number)
{
    (void)message_flags;
    (void)message_guid;
    (void)message_number;
    return etw_trace_message(logger_handle);
}

static ULONG WINAPI StartTraceA_stub(ULONGLONG *session_handle,
                                      PCSTR session_name, PVOID properties)
{
    (void)properties;
    if (!session_handle || !session_name || !*session_name)
        return 87; /* ERROR_INVALID_PARAMETER */

    int free_slot = -1;
    uint64_t flags = etw_lock_irqsave();
    for (int i = 0; i < ETW_MAX_SESSIONS; i++) {
        if (etw_sessions[i].used &&
            reg_stricmp(etw_sessions[i].name, session_name) == 0) {
            ULONGLONG existing = etw_sessions[i].handle;
            etw_unlock_irqrestore(flags);
            reg_memcpy(session_handle, &existing, sizeof(existing));
            return 183; /* ERROR_ALREADY_EXISTS */
        }
        if (!etw_sessions[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) {
        etw_unlock_irqrestore(flags);
        return 8; /* ERROR_NOT_ENOUGH_MEMORY */
    }

    ETW_SESSION *session = &etw_sessions[free_slot];
    session->used = TRUE;
    session->handle = 0xE7000000ULL | etw_next_handle++;
    session->owner_process_id = win32_current_process_id();
    session->enabled = FALSE;
    int length = reg_strlen(session_name);
    if (length > 63) length = 63;
    for (int i = 0; i < length; i++) session->name[i] = session_name[i];
    session->name[length] = 0;
    ULONGLONG result = session->handle;
    etw_unlock_irqrestore(flags);
    reg_memcpy(session_handle, &result, sizeof(result));
    return ERROR_SUCCESS;
}

static ULONG etw_stop_trace(ULONGLONG handle, PCSTR session_name,
                            PVOID properties)
{
    (void)properties;
    uint64_t flags = etw_lock_irqsave();
    ETW_SESSION *session = etw_find_session_locked(handle, session_name);
    if (!session) {
        etw_unlock_irqrestore(flags);
        return 1168; /* ERROR_NOT_FOUND */
    }
    session->used = FALSE;
    session->enabled = FALSE;
    session->name[0] = 0;
    etw_unlock_irqrestore(flags);
    return ERROR_SUCCESS;
}

static ULONG WINAPI StopTraceA_k32(DWORD handle_low, DWORD handle_high,
                                    PCSTR session_name, PVOID properties)
{
    ULONGLONG handle = (ULONGLONG)handle_low |
                       ((ULONGLONG)handle_high << 32);
    return etw_stop_trace(handle, session_name, properties);
}

static ULONG WINAPI StopTraceA_k64(ULONGLONG handle, PCSTR session_name,
                                    PVOID properties)
{
    return etw_stop_trace(handle, session_name, properties);
}

static ULONG etw_enable_trace(ULONG enable, ULONG enable_flag,
                              ULONG enable_level, LPCGUID control_guid,
                              ULONGLONG session_handle)
{
    (void)enable_flag;
    (void)enable_level;
    if (!control_guid) return 87;
    uint64_t flags = etw_lock_irqsave();
    ETW_SESSION *session =
        etw_find_session_locked(session_handle, NULL);
    if (!session) {
        etw_unlock_irqrestore(flags);
        return 6; /* ERROR_INVALID_HANDLE */
    }
    session->enabled = enable != 0;
    etw_unlock_irqrestore(flags);
    return ERROR_SUCCESS;
}

static ULONG WINAPI EnableTrace_k32(ULONG enable, ULONG enable_flag,
                                     ULONG enable_level, LPCGUID control_guid,
                                     DWORD handle_low, DWORD handle_high)
{
    ULONGLONG handle = (ULONGLONG)handle_low |
                       ((ULONGLONG)handle_high << 32);
    return etw_enable_trace(enable, enable_flag, enable_level, control_guid,
                            handle);
}

static ULONG WINAPI EnableTrace_k64(ULONG enable, ULONG enable_flag,
                                     ULONG enable_level, LPCGUID control_guid,
                                     ULONGLONG handle)
{
    return etw_enable_trace(enable, enable_flag, enable_level, control_guid,
                            handle);
}

static ULONGLONG WINAPI OpenTraceA_stub(PVOID log_file)
{
    if (!log_file) return ETW_INVALID_HANDLE;
    uint64_t flags = etw_lock_irqsave();
    int free_slot = -1;
    for (int i = 0; i < ETW_MAX_CONSUMERS; i++) {
        if (!etw_consumers[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        etw_unlock_irqrestore(flags);
        return ETW_INVALID_HANDLE;
    }
    ETW_CONSUMER *consumer = &etw_consumers[free_slot];
    consumer->used = TRUE;
    consumer->handle = 0xE7800000ULL | etw_next_handle++;
    consumer->owner_process_id = win32_current_process_id();
    ULONGLONG result = consumer->handle;
    etw_unlock_irqrestore(flags);
    return result;
}

static ULONG WINAPI ProcessTrace_stub(const ULONGLONG *handles,
                                       ULONG handle_count, PVOID start_time,
                                       PVOID end_time)
{
    (void)start_time;
    (void)end_time;
    if (!handles || !handle_count) return 87;

    uint64_t flags = etw_lock_irqsave();
    for (ULONG i = 0; i < handle_count; i++) {
        ULONGLONG handle;
        reg_memcpy(&handle, (const BYTE *)handles + i * sizeof(handle),
                   sizeof(handle));
        BOOL found = FALSE;
        for (int slot = 0; slot < ETW_MAX_CONSUMERS; slot++) {
            if (etw_consumers[slot].used &&
                etw_consumers[slot].handle == handle) {
                found = TRUE;
                break;
            }
        }
        if (!found) {
            etw_unlock_irqrestore(flags);
            return 6; /* ERROR_INVALID_HANDLE */
        }
    }
    etw_unlock_irqrestore(flags);
    return ERROR_SUCCESS; /* no events are produced by the kernel yet */
}

static ULONG etw_close_trace(ULONGLONG handle)
{
    uint64_t flags = etw_lock_irqsave();
    for (int i = 0; i < ETW_MAX_CONSUMERS; i++) {
        if (etw_consumers[i].used && etw_consumers[i].handle == handle) {
            etw_consumers[i].used = FALSE;
            etw_unlock_irqrestore(flags);
            return ERROR_SUCCESS;
        }
    }
    etw_unlock_irqrestore(flags);
    return 6; /* ERROR_INVALID_HANDLE */
}

static ULONG WINAPI CloseTrace_k32(DWORD handle_low, DWORD handle_high)
{
    return etw_close_trace((ULONGLONG)handle_low |
                           ((ULONGLONG)handle_high << 32));
}

static ULONG WINAPI CloseTrace_k64(ULONGLONG handle)
{
    return etw_close_trace(handle);
}

static ULONG WINAPI EventRegister_stub(LPCGUID provider_id,
                                        PVOID enable_callback,
                                        PVOID callback_context,
                                        ULONGLONG *registration_handle)
{
    (void)enable_callback;
    (void)callback_context;
    if (!provider_id || !registration_handle)
        return 87; /* ERROR_INVALID_PARAMETER */

    /* ponytail: ETW has no controller yet, so providers stay disabled. */
    *registration_handle = 1;
    return ERROR_SUCCESS;
}

static ULONG WINAPI EventSetInformation_stub(ULONGLONG registration_handle,
                                              DWORD information_class,
                                              PVOID information,
                                              ULONG information_length)
{
    (void)information_class;
    (void)information;
    (void)information_length;
    return registration_handle ? ERROR_SUCCESS : 6; /* ERROR_INVALID_HANDLE */
}

static ULONG WINAPI EventUnregister_stub(ULONGLONG registration_handle)
{
    return registration_handle ? ERROR_SUCCESS : 6; /* ERROR_INVALID_HANDLE */
}

static ULONG WINAPI EventWrite_stub(ULONGLONG registration_handle,
                                     PVOID event_descriptor,
                                     ULONG user_data_count,
                                     PVOID user_data)
{
    (void)event_descriptor;
    (void)user_data_count;
    (void)user_data;
    return registration_handle ? ERROR_SUCCESS : 6; /* ERROR_INVALID_HANDLE */
}

static ULONG WINAPI EventWriteTransfer_stub(ULONGLONG registration_handle,
                                             PVOID event_descriptor,
                                             LPCGUID activity_id,
                                             LPCGUID related_activity_id,
                                             ULONG user_data_count,
                                             PVOID user_data)
{
    (void)event_descriptor;
    (void)activity_id;
    (void)related_activity_id;
    (void)user_data_count;
    (void)user_data;
    return registration_handle ? ERROR_SUCCESS : 6; /* ERROR_INVALID_HANDLE */
}

/* Chromium delay-loads wevtapi.dll to inspect optional Windows event logs.
 * Expose an empty event stream so the feature degrades cleanly. */
#define EVT_RENDER_CONTEXT_HANDLE ((HANDLE)(ULONG_PTR)0xE7700001U)
#define EVT_QUERY_HANDLE          ((HANDLE)(ULONG_PTR)0xE7700002U)

static HANDLE WINAPI EvtCreateRenderContext_stub(DWORD value_path_count,
                                                  PCWSTR *value_paths,
                                                  DWORD flags)
{
    (void)value_path_count;
    (void)value_paths;
    (void)flags;
    SetLastError(ERROR_SUCCESS);
    return EVT_RENDER_CONTEXT_HANDLE;
}

static HANDLE WINAPI EvtQuery_stub(HANDLE session, PCWSTR path,
                                    PCWSTR query, DWORD flags)
{
    (void)session;
    (void)path;
    (void)query;
    (void)flags;
    SetLastError(ERROR_SUCCESS);
    return EVT_QUERY_HANDLE;
}

static BOOL WINAPI EvtNext_stub(HANDLE result_set, DWORD event_count,
                                HANDLE *events, DWORD timeout, DWORD flags,
                                DWORD *returned)
{
    (void)event_count;
    (void)events;
    (void)timeout;
    (void)flags;
    if (returned) *returned = 0;
    if (result_set != EVT_QUERY_HANDLE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(259); /* ERROR_NO_MORE_ITEMS */
    return FALSE;
}

static BOOL WINAPI EvtRender_stub(HANDLE context, HANDLE fragment,
                                  DWORD flags, DWORD buffer_size, PVOID buffer,
                                  DWORD *buffer_used, DWORD *property_count)
{
    (void)context;
    (void)fragment;
    (void)flags;
    (void)buffer_size;
    (void)buffer;
    if (buffer_used) *buffer_used = 0;
    if (property_count) *property_count = 0;
    SetLastError(13); /* ERROR_INVALID_DATA */
    return FALSE;
}

static BOOL WINAPI EvtClose_stub(HANDLE object)
{
    if (object != EVT_RENDER_CONTEXT_HANDLE && object != EVT_QUERY_HANDLE) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static LONG WINAPI RegNotifyChangeKeyValue_stub(HKEY key, BOOL watch_subtree,
                                                 DWORD notify_filter,
                                                 HANDLE event,
                                                 BOOL asynchronous)
{
    (void)watch_subtree;
    (void)notify_filter;
    (void)event;
    (void)asynchronous;
    return key ? ERROR_SUCCESS : 6; /* ERROR_INVALID_HANDLE */
}

static LONG WINAPI RegDisableReflectionKey_stub(HKEY key)
{
    reg_init();
    const char *path = NULL;
    BYTE view = REG_VIEW_SHARED;
    spin_lock(&reg_lock);
    int valid = reg_key_context(key, &path, &view);
    spin_unlock(&reg_lock);
    if (!valid)
        return 6; /* ERROR_INVALID_HANDLE */
    (void)path;
    (void)view;
    /* OsitoK exposes a unified registry view, so reflection is already off. */
    return ERROR_SUCCESS;
}

static uint64_t next_local_unique_id = 0x10000;

static BOOL WINAPI AllocateLocallyUniqueId(PVOID output)
{
    if (!output) {
        SetLastError(87);
        return FALSE;
    }
    uint64_t value = __atomic_fetch_add(&next_local_unique_id, 1,
                                        __ATOMIC_RELAXED);
    DWORD *parts = output;
    parts[0] = (DWORD)value;
    parts[1] = (DWORD)(value >> 32);
    return TRUE;
}

static const SHIM_EXPORT advapi32_exports[] = {
    { "AllocateLocallyUniqueId", (PVOID)AllocateLocallyUniqueId, 1, CC_STDCALL },
    { "RegOpenKeyA",        (PVOID)RegOpenKeyA,      3, CC_STDCALL },
    { "RegOpenKeyW",        (PVOID)RegOpenKeyW,      3, CC_STDCALL },
    { "RegOpenKeyExA",      (PVOID)RegOpenKeyExA,    5, CC_STDCALL },
    { "RegCreateKeyA",      (PVOID)RegCreateKeyA_k32, 3, CC_STDCALL },
    { "RegCreateKeyExA",    (PVOID)RegCreateKeyExA,  9, CC_STDCALL },
    { "RegQueryValueExA",   (PVOID)RegQueryValueExA, 6, CC_STDCALL },
    { "RegSetValueExA",     (PVOID)RegSetValueExA,   6, CC_STDCALL },
    { "RegCloseKey",        (PVOID)RegCloseKey,      1, CC_STDCALL },
    { "RegDisablePredefinedCache", (PVOID)RegDisablePredefinedCache, 0, CC_STDCALL },
    { "RegDisablePredefinedCacheEx", (PVOID)RegDisablePredefinedCache, 0, CC_STDCALL },
    { "RegFlushKey",        (PVOID)RegFlushKey,      1, CC_STDCALL },
    { "RegDeleteKeyA",      (PVOID)RegDeleteKeyA,    2, CC_STDCALL },
    { "RegDeleteKeyExA",    (PVOID)RegDeleteKeyExA,  4, CC_STDCALL },
    { "RegDeleteValueA",    (PVOID)RegDeleteValueA,  2, CC_STDCALL },
    { "RegEnumKeyExA",      (PVOID)RegEnumKeyExA,    8, CC_STDCALL },
    { "RegEnumKeyA",        (PVOID)RegEnumKeyA,      4, CC_STDCALL },
    { "RegEnumKeyW",        (PVOID)RegEnumKeyW,      4, CC_STDCALL },
    { "RegEnumValueA",      (PVOID)RegEnumValueA,    8, CC_STDCALL },
    { "RegQueryInfoKeyA",   (PVOID)RegQueryInfoKeyA, 12, CC_STDCALL },
    { "RegDeleteTreeA",     (PVOID)RegDeleteTreeA,   2, CC_STDCALL },
    { "RegOpenKeyExW",      (PVOID)RegOpenKeyExW,    5, CC_STDCALL },
    { "RegQueryValueExW",   (PVOID)RegQueryValueExW, 6, CC_STDCALL },
    { "RegEnumValueW",      (PVOID)RegEnumValueW,    8, CC_STDCALL },
    { "RegEnumKeyExW",      (PVOID)RegEnumKeyExW,    8, CC_STDCALL },
    { "RegQueryInfoKeyW",   (PVOID)RegQueryInfoKeyW, 12, CC_STDCALL },
    { "RegCreateKeyExW",    (PVOID)RegCreateKeyExW,  9, CC_STDCALL },
    { "RegSetValueExW",     (PVOID)RegSetValueExW,   6, CC_STDCALL },
    { "RegDeleteKeyW",      (PVOID)RegDeleteKeyW,    2, CC_STDCALL },
    { "RegDeleteKeyExW",    (PVOID)RegDeleteKeyExW,  4, CC_STDCALL },
    { "RegDeleteValueW",    (PVOID)RegDeleteValueW,  2, CC_STDCALL },
    { "RegNotifyChangeKeyValue", (PVOID)RegNotifyChangeKeyValue_stub, 5, CC_STDCALL },
    { "RegDisableReflectionKey", (PVOID)RegDisableReflectionKey_stub, 1, CC_STDCALL },
    { "GetUserNameA",       (PVOID)GetUserNameA,     2, CC_STDCALL },
    { "GetUserNameW",       (PVOID)GetUserNameW,     2, CC_STDCALL },
    { "LookupAccountNameW", (PVOID)LookupAccountNameW_stub, 7, CC_STDCALL },
    { "MapGenericMask",     (PVOID)MapGenericMask,    2, CC_STDCALL },
    { "AccessCheck",        (PVOID)AccessCheck_stub, 8, CC_STDCALL },
    { "InitializeSecurityDescriptor", (PVOID)InitializeSecurityDescriptor, 2, CC_STDCALL },
    { "CreateWellKnownSid", (PVOID)CreateWellKnownSid_stub, 4, CC_STDCALL },
    { "AllocateAndInitializeSid", (PVOID)AllocateAndInitializeSid_stub, 11, CC_STDCALL },
    { "FreeSid",            (PVOID)FreeSid_stub,       1, CC_STDCALL },
    { "ConvertStringSidToSidW", (PVOID)ConvertStringSidToSidW_stub, 2, CC_STDCALL },
    { "ConvertSidToStringSidA", (PVOID)ConvertSidToStringSidA_stub, 2, CC_STDCALL },
    { "ConvertSidToStringSidW", (PVOID)ConvertSidToStringSidW_stub, 2, CC_STDCALL },
    { "CopySid",            (PVOID)CopySid_stub,       3, CC_STDCALL },
    { "EqualSid",           (PVOID)EqualSid_stub,      2, CC_STDCALL },
    { "GetLengthSid",       (PVOID)GetLengthSid_stub, 1, CC_STDCALL },
    { "GetSidSubAuthority", (PVOID)GetSidSubAuthority_stub, 2, CC_STDCALL },
    { "GetSidSubAuthorityCount", (PVOID)GetSidSubAuthorityCount_stub, 1, CC_STDCALL },
    { "GetTokenInformation", (PVOID)GetTokenInformation_stub, 5, CC_STDCALL },
    { "OpenThreadToken",    (PVOID)OpenThreadToken_stub, 4, CC_STDCALL },
    { "SetThreadToken",     (PVOID)SetThreadToken_stub, 2, CC_STDCALL },
    { "ImpersonateAnonymousToken", (PVOID)ImpersonateAnonymousToken_stub, 1, CC_STDCALL },
    { "ImpersonateNamedPipeClient", (PVOID)ImpersonateNamedPipeClient_stub, 1, CC_STDCALL },
    { "RevertToSelf",       (PVOID)RevertToSelf_stub, 0, CC_STDCALL },
    { "LookupPrivilegeValueA", (PVOID)LookupPrivilegeValueA_stub, 3, CC_STDCALL },
    { "LookupPrivilegeValueW", (PVOID)LookupPrivilegeValueW_stub, 3, CC_STDCALL },
    { "DuplicateTokenEx",   (PVOID)DuplicateTokenEx_stub, 6, CC_STDCALL },
    { "CreateRestrictedToken", (PVOID)CreateRestrictedToken_impl, 9, CC_STDCALL },
    { "IsTokenRestricted", (PVOID)IsTokenRestricted_impl, 1, CC_STDCALL },
    { "AdjustTokenPrivileges", (PVOID)AdjustTokenPrivileges_stub, 6, CC_STDCALL },
    { "SetTokenInformation", (PVOID)SetTokenInformation_stub, 4, CC_STDCALL },
    { "IsValidSid",         (PVOID)IsValidSid_stub, 1, CC_STDCALL },
    { "InitializeAcl",      (PVOID)InitializeAcl_stub, 3, CC_STDCALL },
    { "IsValidAcl",         (PVOID)IsValidAcl_stub, 1, CC_STDCALL },
    { "GetAce",             (PVOID)GetAce_stub, 3, CC_STDCALL },
    { "AddAce",             (PVOID)AddAce_stub, 5, CC_STDCALL },
    { "GetAclInformation",  (PVOID)GetAclInformation_stub, 4, CC_STDCALL },
    { "AddAccessAllowedAce", (PVOID)AddAccessAllowedAce_stub, 4, CC_STDCALL },
    { "AddMandatoryAce",    (PVOID)AddMandatoryAce_stub, 5, CC_STDCALL },
    { "SetSecurityDescriptorDacl", (PVOID)SetSecurityDescriptorDacl, 4, CC_STDCALL },
    { "SetSecurityDescriptorOwner", (PVOID)SetSecurityDescriptorOwner, 3, CC_STDCALL },
    { "SetSecurityDescriptorGroup", (PVOID)SetSecurityDescriptorGroup, 3, CC_STDCALL },
    { "IsValidSecurityDescriptor", (PVOID)IsValidSecurityDescriptor_stub, 1, CC_STDCALL },
    { "GetSecurityDescriptorControl", (PVOID)GetSecurityDescriptorControl_stub, 3, CC_STDCALL },
    { "GetSecurityDescriptorOwner", (PVOID)GetSecurityDescriptorOwner_stub, 3, CC_STDCALL },
    { "GetSecurityDescriptorGroup", (PVOID)GetSecurityDescriptorGroup_stub, 3, CC_STDCALL },
    { "GetSecurityDescriptorDacl", (PVOID)GetSecurityDescriptorDacl_stub, 4, CC_STDCALL },
    { "GetSecurityDescriptorSacl", (PVOID)GetSecurityDescriptorSacl_stub, 4, CC_STDCALL },
    { "SetFileSecurityA",  (PVOID)SetFileSecurityA_stub, 3, CC_STDCALL },
    { "SetFileSecurityW",  (PVOID)SetFileSecurityW_stub, 3, CC_STDCALL },
    { "GetNamedSecurityInfoW", (PVOID)GetNamedSecurityInfoW_stub, 8, CC_STDCALL },
    { "SetNamedSecurityInfoW", (PVOID)SetNamedSecurityInfoW_stub, 7, CC_STDCALL },
    { "GetSecurityInfo",    (PVOID)GetSecurityInfo_stub, 8, CC_STDCALL },
    { "SetSecurityInfo",    (PVOID)SetSecurityInfo_stub, 7, CC_STDCALL },
    { "BuildExplicitAccessWithNameW", (PVOID)BuildExplicitAccessWithNameW_stub, 5, CC_STDCALL },
    { "BuildTrusteeWithSidW", (PVOID)BuildTrusteeWithSidW_stub, 2, CC_STDCALL },
    { "GetExplicitEntriesFromAclA", (PVOID)GetExplicitEntriesFromAclA_stub, 3, CC_STDCALL },
    { "SetEntriesInAclA",   (PVOID)SetEntriesInAclW_stub, 4, CC_STDCALL },
    { "SetEntriesInAclW",   (PVOID)SetEntriesInAclW_stub, 4, CC_STDCALL },
    { "BuildSecurityDescriptorW", (PVOID)BuildSecurityDescriptorW_stub, 9, CC_STDCALL },
    { "ConvertStringSecurityDescriptorToSecurityDescriptorW", (PVOID)ConvertStringSecurityDescriptorToSecurityDescriptorW, 4, CC_STDCALL },
    { "SystemFunction036",  (PVOID)SystemFunction036, 2, CC_STDCALL },
    { "ProcessPrng",        (PVOID)SystemFunction036, 2, CC_STDCALL },
    { "BCryptGenRandom",    (PVOID)BCryptGenRandom, 4, CC_STDCALL },
    { "CryptAcquireContextA", (PVOID)CryptAcquireContextA, 5, CC_STDCALL },
    { "CryptAcquireContextW", (PVOID)CryptAcquireContextW, 5, CC_STDCALL },
    { "CryptGenRandom",       (PVOID)CryptGenRandom,       3, CC_STDCALL },
    { "CryptReleaseContext",  (PVOID)CryptReleaseContext,  2, CC_STDCALL },
    { "CryptCreateHash",      (PVOID)CryptCreateHash,      5, CC_STDCALL },
    { "CryptHashData",        (PVOID)CryptHashData,        4, CC_STDCALL },
    { "CryptGetHashParam",    (PVOID)CryptGetHashParam,    5, CC_STDCALL },
    { "CryptDestroyHash",     (PVOID)CryptDestroyHash,     1, CC_STDCALL },
    { "RegisterTraceGuidsW", (PVOID)RegisterTraceGuidsW_stub, 8, CC_STDCALL },
    { "UnregisterTraceGuids", (PVOID)UnregisterTraceGuids_stub, 1, CC_STDCALL },
    { "TraceMessage",       (PVOID)TraceMessage_k32,       5, CC_CDECL },
    { "StartTraceA",        (PVOID)StartTraceA_stub,       3, CC_STDCALL },
    { "StopTraceA",         (PVOID)StopTraceA_k32,         4, CC_STDCALL },
    { "EnableTrace",        (PVOID)EnableTrace_k32,        6, CC_STDCALL },
    { "OpenTraceA",         (PVOID)OpenTraceA_stub,        1, CC_STDCALL },
    { "ProcessTrace",       (PVOID)ProcessTrace_stub,      4, CC_STDCALL },
    { "CloseTrace",         (PVOID)CloseTrace_k32,         2, CC_STDCALL },
    { "EventRegister",       (PVOID)EventRegister_stub,       4, CC_STDCALL },
    { "EventSetInformation", (PVOID)EventSetInformation_stub, 4, CC_STDCALL },
    { "EventUnregister",     (PVOID)EventUnregister_stub,     1, CC_STDCALL },
    { "EventWrite",          (PVOID)EventWrite_stub,          4, CC_STDCALL },
    { "EventWriteTransfer",  (PVOID)EventWriteTransfer_stub,  6, CC_STDCALL },
    { "EvtClose",            (PVOID)EvtClose_stub,             1, CC_STDCALL },
    { "EvtCreateRenderContext", (PVOID)EvtCreateRenderContext_stub, 3, CC_STDCALL },
    { "EvtNext",             (PVOID)EvtNext_stub,              6, CC_STDCALL },
    { "EvtQuery",            (PVOID)EvtQuery_stub,             4, CC_STDCALL },
    { "EvtRender",           (PVOID)EvtRender_stub,            7, CC_STDCALL },
    { "OpenSCManagerW",      (PVOID)OpenSCManagerW, 3, CC_STDCALL },
    { "OpenSCManagerA",      (PVOID)OpenSCManagerA, 3, CC_STDCALL },
    { "OpenServiceW",        (PVOID)OpenServiceW, 3, CC_STDCALL },
    { "OpenServiceA",        (PVOID)OpenServiceA, 3, CC_STDCALL },
    { "CreateServiceW",      (PVOID)CreateServiceW, 13, CC_STDCALL },
    { "ChangeServiceConfigW", (PVOID)ChangeServiceConfigW, 11, CC_STDCALL },
    { "ChangeServiceConfig2W", (PVOID)ChangeServiceConfig2W, 3, CC_STDCALL },
    { "DeleteService",       (PVOID)DeleteService, 1, CC_STDCALL },
    { "StartServiceW",       (PVOID)StartServiceW, 3, CC_STDCALL },
    { "ControlService",      (PVOID)ControlService, 3, CC_STDCALL },
    { "QueryServiceStatus",  (PVOID)QueryServiceStatus, 2, CC_STDCALL },
    { "QueryServiceStatusEx", (PVOID)QueryServiceStatusEx, 5, CC_STDCALL },
    { "QueryServiceConfigW", (PVOID)QueryServiceConfigW, 4, CC_STDCALL },
    { "QueryServiceConfigA", (PVOID)QueryServiceConfigA, 4, CC_STDCALL },
    { "CloseServiceHandle",  (PVOID)CloseServiceHandle, 1, CC_STDCALL },
    { "StartServiceCtrlDispatcherW", (PVOID)StartServiceCtrlDispatcherW, 1, CC_STDCALL },
    { "RegisterServiceCtrlHandlerW", (PVOID)RegisterServiceCtrlHandlerW, 2, CC_STDCALL },
    { "RegisterServiceCtrlHandlerExW", (PVOID)RegisterServiceCtrlHandlerExW, 3, CC_STDCALL },
    { "SetServiceStatus",    (PVOID)SetServiceStatus, 2, CC_STDCALL },
    { "QueryServiceObjectSecurity", (PVOID)QueryServiceObjectSecurity, 5, CC_STDCALL },
    { "SetServiceObjectSecurity", (PVOID)SetServiceObjectSecurity, 3, CC_STDCALL },
    { "RegisterEventSourceW", (PVOID)RegisterEventSourceW_scm, 2, CC_STDCALL },
    { "DeregisterEventSource", (PVOID)DeregisterEventSource_scm, 1, CC_STDCALL },
    { "ReportEventW",        (PVOID)ReportEventW_scm, 9, CC_STDCALL },
    { "OpenEventLogA",       (PVOID)OpenEventLogA_scm, 2, CC_STDCALL },
    { "ReadEventLogW",       (PVOID)ReadEventLogW_scm, 7, CC_STDCALL },
    { "CloseEventLog",       (PVOID)CloseEventLog_scm, 1, CC_STDCALL },
    { "OpenProcessToken",    (PVOID)OpenProcessToken_stub, 3, CC_STDCALL },
    { "CheckTokenMembership", (PVOID)CheckTokenMembership, 3, CC_STDCALL },
    { "CreateProcessAsUserW", (PVOID)CreateProcessAsUserW_compat, 11, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *advapi32_abi_table(int *count) {
    *count = (int)(sizeof(advapi32_exports)/sizeof(advapi32_exports[0]));
    return (const WIN32_EXPORT *)advapi32_exports;
}

static int advapi_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID advapi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    if (!g_compat32_mode && advapi_strcmp(func_name, "TraceMessage") == 0)
        return (PVOID)TraceMessage_k64;
    if (!g_compat32_mode && advapi_strcmp(func_name, "StopTraceA") == 0)
        return (PVOID)StopTraceA_k64;
    if (!g_compat32_mode && advapi_strcmp(func_name, "EnableTrace") == 0)
        return (PVOID)EnableTrace_k64;
    if (!g_compat32_mode && advapi_strcmp(func_name, "CloseTrace") == 0)
        return (PVOID)CloseTrace_k64;
    for (int i = 0; advapi32_exports[i].name; i++) {
        if (advapi_strcmp(func_name, advapi32_exports[i].name) == 0)
            return advapi32_exports[i].func;
    }
    return NULL;
}

PVOID advapi32_shim_init(void)
{
    reg_init();
    return (PVOID)advapi32_exports;
}
