/*
 * OsitoK Windows Compatibility Layer — advapi32.dll Shim Implementation
 *
 * In-memory registry backed by a flat key-value store.
 * Pre-populated with UT99-relevant defaults.
 *
 * Registry paths are normalized to lowercase with forward slashes.
 * Keys are stored as "HKLM/software/unreal technology/installed apps/..."
 */

#include "advapi32_shim.h"
#include "handle.h"
#include "ntsyscall.h"
#include "scm.h"
#include "win32_abi.h"
#include "../kernel/smp.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void random_get_bytes(void *buf, uint32_t len);
extern PVOID WINAPI LocalAlloc(UINT uFlags, SIZE_T dwBytes);
extern PVOID WINAPI LocalFree(PVOID hMem);
extern void WINAPI SetLastError(DWORD dwErrCode);
extern BOOL WINAPI CreateProcessW(PCWSTR lpApp, PWSTR lpCmd,
                                  PVOID process_attributes,
                                  PVOID thread_attributes,
                                  BOOL inherit_handles, DWORD creation_flags,
                                  PVOID environment, PCWSTR current_directory,
                                  PVOID startup_info,
                                  PVOID process_information);
extern HANDLE_TABLE g_handle_table;
extern BOOL nt_process_id(HANDLE handle, DWORD *process_id);
extern DWORD win32_current_process_id(void);
extern PVOID win32_current_thread_object(void);

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

/* ── Registry store ────────────────────────────────────────── */

#define MAX_REG_KEYS    128
#define MAX_REG_VALUES  256
#define MAX_REG_PATH    256
#define MAX_REG_DATA    512

#define REG_VIEW_SHARED 0
#define REG_VIEW_32     1
#define REG_VIEW_64     2

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
} REG_VALUE;

static REG_KEY   reg_keys[MAX_REG_KEYS];
static REG_VALUE reg_values[MAX_REG_VALUES];
static int       reg_initialized = 0;
static ULONG     next_handle_id  = 0x90000001;

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

static int reg_is_steam_service_path(const char *path)
{
    return reg_path_has_prefix(path, "hklm\\software\\valve\\steam");
}

static int reg_is_steam_pid_name(const char *name)
{
    return name && reg_stricmp(name, "SteamPID") == 0;
}

static void reg_trace_steam_pid(const char *operation, const char *path,
                                BYTE view, LONG status,
                                const REG_VALUE *value)
{
    serial_puts("[REG-STEAM] ");
    serial_puts(operation);
    serial_puts(" pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" path=");
    serial_puts(path);
    serial_puts(" view=");
    serial_puts(view == REG_VIEW_32 ? "32" : "64");
    serial_puts(" status=");
    serial_putdec((uint64_t)(uint32_t)status);
    if (value) {
        serial_puts(" type=");
        serial_putdec(value->type);
        serial_puts(" bytes=");
        serial_putdec(value->data_len);
        if (value->type == REG_DWORD && value->data_len >= sizeof(DWORD)) {
            DWORD data = 0;
            reg_memcpy(&data, value->data, sizeof(data));
            serial_puts(" value=");
            serial_putdec(data);
        }
    }
    serial_puts("\n");
}

static void reg_trace_invalid_steam_handle(const char *operation, HKEY key)
{
    serial_puts("[REG-STEAM] ");
    serial_puts(operation);
    serial_puts(" pid=");
    serial_putdec(win32_current_process_id());
    serial_puts(" handle=0x");
    serial_puthex((ULONG_PTR)key, g_compat32_mode ? 8 : 16);
    serial_puts(" status=");
    serial_putdec(ERROR_FILE_NOT_FOUND);
    serial_puts("\n");
}

/* Windows redirects the software hives according to the caller's ABI. HKCR
 * is a merged view over the corresponding Software\\Classes hives. */
static int reg_path_is_redirected(const char *path)
{
    return reg_path_has_prefix(path, "hklm\\software") ||
           reg_path_has_prefix(path, "hkcu\\software") ||
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
    out[pos] = 0;
}

/* ── Pre-populate UT99-relevant keys ──────────────────────── */

static void reg_set_value_view(const char *key_path, const char *name,
                               DWORD type, const void *data, DWORD data_len,
                               BYTE view)
{
    view = reg_storage_view(key_path, view);

    /* A shared value under a redirected path must make the key visible from
     * both views. This is used for the small set of compatibility defaults. */
    if (view == REG_VIEW_SHARED && reg_path_is_redirected(key_path)) {
        reg_ensure_key(key_path, REG_VIEW_32);
        reg_ensure_key(key_path, REG_VIEW_64);
    } else {
        reg_ensure_key(key_path,
                       view == REG_VIEW_SHARED ? reg_default_view() : view);
    }

    const char *value_name = name ? name : "";

    /* Windows replaces an existing value in the selected view. */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used || reg_values[i].view != view ||
            reg_stricmp(reg_values[i].key_path, key_path) != 0 ||
            reg_stricmp(reg_values[i].name, value_name) != 0)
            continue;

        reg_values[i].type = type;
        reg_values[i].data_len =
            data_len < MAX_REG_DATA ? data_len : MAX_REG_DATA;
        if (data && reg_values[i].data_len)
            reg_memcpy(reg_values[i].data, data, reg_values[i].data_len);
        return;
    }

    /* Find or create value */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used) {
            reg_strcpy(reg_values[i].key_path, key_path);
            reg_strcpy(reg_values[i].name, value_name);
            reg_values[i].type     = type;
            reg_values[i].data_len = data_len < MAX_REG_DATA ? data_len : MAX_REG_DATA;
            if (data && reg_values[i].data_len)
                reg_memcpy(reg_values[i].data, data, reg_values[i].data_len);
            reg_values[i].view = view;
            reg_values[i].used = 1;
            return;
        }
    }
}

static void reg_set_value(const char *key_path, const char *name,
                          DWORD type, const void *data, DWORD data_len)
{
    reg_set_value_view(key_path, name, type, data, data_len,
                       reg_default_view());
}

static void reg_init(void);

/* Public entry for the MSI installer: write a value at an already-normalized
 * lowercase backslash path (e.g. "hklm\\software\\app"). Ensures the store is
 * initialized first so installer-written rows survive alongside UT99 defaults. */
void advapi32_reg_install_set(const char *path_lc_backslash, const char *name,
                             DWORD type, const void *data, DWORD len)
{
    reg_init();
    reg_set_value(path_lc_backslash, name, type, data, len);
}

static void reg_init(void)
{
    if (reg_initialized) return;
    reg_initialized = 1;

    /* Zero everything */
    for (int i = 0; i < MAX_REG_KEYS; i++) reg_keys[i].used = 0;
    for (int i = 0; i < MAX_REG_VALUES; i++) reg_values[i].used = 0;

    /* UT99 install path (HKLM\SOFTWARE\Unreal Technology\Installed Apps\UnrealTournament) */
    {
        const char *key = "hklm\\software\\unreal technology\\installed apps\\unrealtournament";
        const char *path = "C:\\UnrealTournament";
        DWORD path_len = reg_strlen(path) + 1;
        reg_set_value_view(key, "folder", REG_SZ, path, path_len,
                           REG_VIEW_32);
    }

    /* CD key (empty — not needed for LAN/offline) */
    {
        const char *key = "hklm\\software\\unreal technology\\installed apps\\unrealtournament";
        const char *cdkey = "";
        reg_set_value_view(key, "cdkey", REG_SZ, cdkey, 1, REG_VIEW_32);
    }

    /* DirectX version hint */
    {
        const char *key = "hklm\\software\\microsoft\\directx";
        const char *ver = "4.09.00.0904";
        reg_set_value_view(key, "version", REG_SZ, ver,
                           reg_strlen(ver) + 1, REG_VIEW_SHARED);
    }

    serial_puts("[ADVAPI32] registry initialized with UT99 defaults\n");
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
    build_path(path, hKey, lpSubKey);
    BYTE view;
    LONG status = reg_select_view(hKey, path, samDesired, &view);
    if (status != ERROR_SUCCESS) return status;
    reg_trace_view("open", path, view, samDesired);

#ifndef OK_QUIET
    serial_puts("[REG] OpenKeyEx: ");
    serial_puts(path);
    serial_puts("\n");
#endif

    /* A key exists in this view if either a key object or a value is visible.
     * Materialize a view-specific handle even for shared keys so descendants
     * inherit the caller's selected view. */
    int exists = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_stricmp(reg_keys[i].path, path) == 0 &&
            (!reg_path_is_redirected(path) || reg_keys[i].view == view)) {
            exists = 1;
            break;
        }
    }

    if (!exists) {
        for (int i = 0; i < MAX_REG_VALUES; i++) {
            if (reg_values[i].used &&
                reg_stricmp(reg_values[i].key_path, path) == 0 &&
                reg_view_visible(path, reg_values[i].view, view)) {
                exists = 1;
                break;
            }
        }
    }

    if (exists) {
        REG_KEY *entry = reg_ensure_key(path, view);
        if (!entry) return ERROR_FILE_NOT_FOUND;
        reg_store_hkey(phkResult, (HKEY)(ULONG_PTR)entry->handle_id);
        if (reg_is_steam_service_path(path))
            reg_trace_steam_pid("open", path, view, ERROR_SUCCESS, NULL);
        return ERROR_SUCCESS;
    }

    if (reg_is_steam_service_path(path))
        reg_trace_steam_pid("open", path, view, ERROR_FILE_NOT_FOUND, NULL);

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

    char path[MAX_REG_PATH];
    build_path(path, hKey, lpSubKey);
    BYTE view;
    LONG view_status = reg_select_view(hKey, path, samDesired, &view);
    if (view_status != ERROR_SUCCESS) return view_status;
    reg_trace_view("create", path, view, samDesired);

    /* Try to open first */
    LONG result = RegOpenKeyExA(hKey, lpSubKey, 0, samDesired, phkResult);
    if (result == ERROR_SUCCESS) {
        if (lpdwDisposition) *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        return ERROR_SUCCESS;
    }

    /* Create a handle in the selected view. */
    REG_KEY *entry = reg_ensure_key(path, view);
    if (entry) {
        reg_store_hkey(phkResult, (HKEY)(ULONG_PTR)entry->handle_id);
        if (lpdwDisposition) *lpdwDisposition = REG_CREATED_NEW_KEY;
        return ERROR_SUCCESS;
    }

    return ERROR_FILE_NOT_FOUND;
}

static LONG WINAPI RegCreateKeyA_k32(HKEY hKey, PCSTR lpSubKey,
                                     PHKEY phkResult)
{
    return RegCreateKeyExA(hKey, lpSubKey, 0, NULL, 0, KEY_ALL_ACCESS,
                           NULL, phkResult, NULL);
}

LONG WINAPI RegQueryValueExA(HKEY hKey, PCSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    if (!reg_key_context(hKey, &key_path, &view)) {
        if (reg_is_steam_pid_name(lpValueName))
            reg_trace_invalid_steam_handle("query-invalid-handle", hKey);
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
    if (value) {
        if (reg_is_steam_service_path(key_path) &&
            reg_is_steam_pid_name(val_name))
            reg_trace_steam_pid("query", key_path, view, ERROR_SUCCESS,
                                value);
        if (lpType) *lpType = value->type;

        if (lpcbData) {
            if (lpData) {
                if (*lpcbData < value->data_len) {
                    *lpcbData = value->data_len;
                    return ERROR_MORE_DATA;
                }
                reg_memcpy(lpData, value->data, value->data_len);
            }
            *lpcbData = value->data_len;
        }
        return ERROR_SUCCESS;
    }

    if (reg_is_steam_service_path(key_path) &&
        reg_is_steam_pid_name(val_name))
        reg_trace_steam_pid("query", key_path, view, ERROR_FILE_NOT_FOUND,
                            NULL);

#ifndef OK_QUIET
    serial_puts("[REG]   value not found\n");
#endif
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegSetValueExA(HKEY hKey, PCSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData)
{
    (void)Reserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    if (!reg_key_context(hKey, &key_path, &view))
        return ERROR_FILE_NOT_FOUND;

    const char *val_name = lpValueName ? lpValueName : "";
    reg_set_value_view(key_path, val_name, dwType, lpData, cbData, view);
    if (reg_is_steam_service_path(key_path) &&
        reg_is_steam_pid_name(val_name)) {
        REG_VALUE *stored = reg_find_value(key_path, val_name, view);
        reg_trace_steam_pid("set", key_path, view,
                            stored ? ERROR_SUCCESS : 8,
                            stored);
    }
    return ERROR_SUCCESS;
}

LONG WINAPI RegCloseKey(HKEY hKey)
{
    /* We don't free key entries — they persist. Just no-op. */
    (void)hKey;
    return ERROR_SUCCESS;
}

LONG WINAPI RegDeleteValueA(HKEY hKey, PCSTR lpValueName)
{
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    if (!reg_key_context(hKey, &key_path, &view)) {
        if (reg_is_steam_pid_name(lpValueName))
            reg_trace_invalid_steam_handle("delete-invalid-handle", hKey);
        return ERROR_FILE_NOT_FOUND;
    }

    const char *val_name = lpValueName ? lpValueName : "";

    REG_VALUE *value = reg_find_value(key_path, val_name, view);
    if (value) {
        if (reg_is_steam_service_path(key_path) &&
            reg_is_steam_pid_name(val_name))
            reg_trace_steam_pid("delete", key_path, view, ERROR_SUCCESS,
                                value);
        value->used = 0;
        return ERROR_SUCCESS;
    }

    if (reg_is_steam_service_path(key_path) &&
        reg_is_steam_pid_name(val_name))
        reg_trace_steam_pid("delete", key_path, view,
                            ERROR_FILE_NOT_FOUND, NULL);

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
    if (!reg_key_context(hKey, &base, &inherited_view) || !base)
        return ERROR_INVALID_HANDLE;

    char target[MAX_REG_PATH];
    build_path(target, hKey, lpSubKey);
    BYTE view;
    LONG view_status = reg_select_view(hKey, target, samDesired, &view);
    if (view_status != ERROR_SUCCESS)
        return view_status;

    /* RegDeleteKey removes values, but requires callers to remove every
     * subkey first. Descendant values imply a subkey in this flat store. */
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used ||
            !reg_view_visible(reg_keys[i].path, reg_keys[i].view, view) ||
            reg_stricmp(reg_keys[i].path, target) == 0)
            continue;
        if (reg_path_has_prefix(reg_keys[i].path, target))
            return ERROR_ACCESS_DENIED;
    }
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used ||
            !reg_view_visible(reg_values[i].key_path,
                              reg_values[i].view, view) ||
            reg_stricmp(reg_values[i].key_path, target) == 0)
            continue;
        if (reg_path_has_prefix(reg_values[i].key_path, target))
            return ERROR_ACCESS_DENIED;
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
    if (!exists)
        return ERROR_FILE_NOT_FOUND;

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
    if (!reg_key_context(hKey, &key_path, &view))
        return ERROR_FILE_NOT_FOUND;

    const char *child = NULL;
    if (!reg_child_at(key_path, view, dwIndex, &child))
        return ERROR_NO_MORE_ITEMS;

    int child_len = reg_strlen(child);
    if (lpName && lpcchName && *lpcchName > (DWORD)child_len) {
        reg_strcpy(lpName, child);
        *lpcchName = child_len;
        return ERROR_SUCCESS;
    }
    if (lpcchName) *lpcchName = child_len + 1;
    return ERROR_MORE_DATA;
}

LONG WINAPI RegEnumValueA(HKEY hKey, DWORD dwIndex, PSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    if (!reg_key_context(hKey, &key_path, &view))
        return ERROR_FILE_NOT_FOUND;

    REG_VALUE *value = reg_value_at(key_path, view, dwIndex);
    if (!value) return ERROR_NO_MORE_ITEMS;

    int name_len = reg_strlen(value->name);
    if (lpValueName && lpcchValueName) {
        if (*lpcchValueName <= (DWORD)name_len) {
            *lpcchValueName = name_len + 1;
            return ERROR_MORE_DATA;
        }
        reg_strcpy(lpValueName, value->name);
        *lpcchValueName = name_len;
    }
    if (lpType) *lpType = value->type;
    if (lpcbData) {
        if (lpData && *lpcbData < value->data_len) {
            *lpcbData = value->data_len;
            return ERROR_MORE_DATA;
        }
        if (lpData) reg_memcpy(lpData, value->data, value->data_len);
        *lpcbData = value->data_len;
    }
    return ERROR_SUCCESS;
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
    if (!reg_key_context(hKey, &key_path, &view))
        return 6; /* ERROR_INVALID_HANDLE */
    if (!key_path) return 6; /* ERROR_INVALID_HANDLE */

    if (lpClass) {
        if (!lpcchClass || *lpcchClass == 0)
            return ERROR_MORE_DATA;
        lpClass[0] = 0;
    }
    if (lpcchClass) *lpcchClass = 0;
    if (lpcbMaxClassLen) *lpcbMaxClassLen = 0;
    if (lpcbSecurityDescriptor) *lpcbSecurityDescriptor = 0;
    if (lpftLastWriteTime) {
        ((DWORD *)lpftLastWriteTime)[0] = 0;
        ((DWORD *)lpftLastWriteTime)[1] = 0;
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
        if (value->data_len > max_value_len)
            max_value_len = value->data_len;
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
    if (!reg_key_context(hKey, &base, &view) || !base)
        return 6; /* ERROR_INVALID_HANDLE */

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
            if (reg_is_steam_service_path(reg_values[i].key_path) &&
                reg_is_steam_pid_name(reg_values[i].name))
                reg_trace_steam_pid("delete-tree", reg_values[i].key_path,
                                    reg_values[i].view, ERROR_SUCCESS,
                                    &reg_values[i]);
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
    char ansi_name[128];
    wide_to_ansi(ansi_name, lpValueName, 128);
#ifndef OK_QUIET
    serial_puts("[REG] RegSetValueExW -> A: ");
    serial_puts(ansi_name);
    serial_puts("\n");
#endif
    return RegSetValueExA(hKey, ansi_name, Reserved, dwType, lpData, cbData);
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

LONG WINAPI RegQueryValueExW(HKEY hKey, PCWSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    char ansi_name[128];
    wide_to_ansi(ansi_name, lpValueName, 128);
#ifndef OK_QUIET
    serial_puts("[REG] RegQueryValueExW -> A: ");
    serial_puts(ansi_name);
    serial_puts("\n");
#endif
    return RegQueryValueExA(hKey, ansi_name, lpReserved, lpType, lpData, lpcbData);
}

LONG WINAPI RegEnumValueW(HKEY hKey, DWORD dwIndex, PWSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    const char *key_path = NULL;
    BYTE view;
    if (!reg_key_context(hKey, &key_path, &view))
        return ERROR_FILE_NOT_FOUND;

    REG_VALUE *value = reg_value_at(key_path, view, dwIndex);
    if (!value) return ERROR_NO_MORE_ITEMS;

    DWORD name_len = (DWORD)reg_strlen(value->name);
    if (lpValueName && lpcchValueName) {
        if (*lpcchValueName <= name_len) {
            *lpcchValueName = name_len + 1;
            return ERROR_MORE_DATA;
        }
        for (DWORD j = 0; j < name_len; j++)
            lpValueName[j] = (WCHAR)(BYTE)value->name[j];
        lpValueName[name_len] = 0;
    }
    if (lpcchValueName) *lpcchValueName = name_len;
    if (lpType) *lpType = value->type;

    if (lpcbData) {
        if (lpData && *lpcbData < value->data_len) {
            *lpcbData = value->data_len;
            return ERROR_MORE_DATA;
        }
        if (lpData) reg_memcpy(lpData, value->data, value->data_len);
        *lpcbData = value->data_len;
    }
    return ERROR_SUCCESS;
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

#define ERROR_NOT_ENOUGH_MEMORY    8U
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

DWORD advapi32_crypto_release_process(DWORD process_id)
{
    if (!process_id) return 0;

    DWORD released = 0;
    uint64_t irq_flags = crypt_lock_irqsave();
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

_Static_assert(sizeof(BUILTIN_USERS_SID) == 16, "built-in users SID layout");
_Static_assert(sizeof(LOCAL_USER_SID) == 28, "local user SID layout");
_Static_assert(sizeof(ACL_HEADER) == 8, "ACL header layout");

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
    DWORD token_type;
    DWORD impersonation_level;
    PCVOID user_sid;
    DWORD user_sid_size;
} LOCAL_TOKEN_OBJECT;

static const LOCAL_TOKEN_OBJECT g_local_primary_token = {
    .token_type = 1,             /* TokenPrimary */
    .impersonation_level = 2,    /* SecurityImpersonation */
    .user_sid = &g_local_user_sid,
    .user_sid_size = sizeof(g_local_user_sid),
};
static const LOCAL_TOKEN_OBJECT g_local_impersonation_token = {
    .token_type = 2,             /* TokenImpersonation */
    .impersonation_level = 2,
    .user_sid = &g_local_user_sid,
    .user_sid_size = sizeof(g_local_user_sid),
};
static const LOCAL_TOKEN_OBJECT g_anonymous_token = {
    .token_type = 2,
    .impersonation_level = 2,
    .user_sid = &g_anonymous_sid,
    .user_sid_size = sizeof(g_anonymous_sid),
};

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
        revision != ACL_REVISION) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    ACL_HEADER *header = (ACL_HEADER *)acl;
    header->revision = ACL_REVISION;
    header->sbz1 = 0;
    header->size = (USHORT)acl_length;
    header->ace_count = 0;
    header->sbz2 = 0;
    return TRUE;
}

static BOOL WINAPI IsValidAcl_stub(PVOID acl)
{
    ACL_HEADER *header = (ACL_HEADER *)acl;
    if (!header ||
        (header->revision != ACL_REVISION && header->revision != 4) ||
        header->size < sizeof(*header)) {
        SetLastError(1336); /* ERROR_INVALID_ACL */
        return FALSE;
    }

    DWORD used = sizeof(*header);
    for (USHORT i = 0; i < header->ace_count; i++) {
        if (used + 4 > header->size) {
            SetLastError(1336);
            return FALSE;
        }
        USHORT ace_size = *(USHORT *)((BYTE *)acl + used + 2);
        if (ace_size < 4 || used + ace_size > header->size) {
            SetLastError(1336);
            return FALSE;
        }
        used += ace_size;
    }
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
    DWORD used = sizeof(*acl);
    for (USHORT i = 0; i < acl->ace_count; i++) {
        if (used + 4 > acl->size)
            return 0;
        USHORT ace_size = *(const USHORT *)((const BYTE *)acl + used + 2);
        if (ace_size < 4 || used + ace_size > acl->size)
            return 0;
        used += ace_size;
    }
    return used;
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
    DWORD sid_length = GetLengthSid_stub(sid);
    if (!header || revision != ACL_REVISION || header->revision != ACL_REVISION ||
        header->size < sizeof(*header) || !sid_length) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD used = sizeof(*header);
    for (USHORT i = 0; i < header->ace_count; i++) {
        if (used + 4 > header->size) {
            SetLastError(87);
            return FALSE;
        }
        USHORT ace_size = *(USHORT *)((BYTE *)acl + used + 2);
        if (ace_size < 4 || used + ace_size > header->size) {
            SetLastError(87);
            return FALSE;
        }
        used += ace_size;
    }

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
    return TRUE;
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
    return descriptor && *(UCHAR *)descriptor == SECURITY_DESCRIPTOR_REVISION;
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

static BOOL WINAPI OpenProcessToken_stub(HANDLE process,
                                          DWORD desired_access,
                                          PHANDLE token)
{
    if (!token) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    ULONG_PTR process_value = (ULONG_PTR)process;
    DWORD process_id = win32_current_process_id();
    if (process_value != (ULONG_PTR)NT_CURRENT_PROCESS &&
        (DWORD)process_value != 0xFFFFFFFFU &&
        !nt_process_id(process, &process_id)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    HANDLE new_token = NULL;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_TOKEN,
                                   desired_access,
                                   (PVOID)&g_local_primary_token,
                                   &new_token);
    if (!NT_SUCCESS(status)) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    if (g_compat32_mode)
        *(DWORD *)(void *)token = (DWORD)(ULONG_PTR)new_token;
    else
        *token = new_token;
    SetLastError(0);
    return TRUE;
}

static const LOCAL_TOKEN_OBJECT *token_object_info(PVOID object)
{
    if (object == &g_local_primary_token)
        return &g_local_primary_token;
    if (object == &g_local_impersonation_token)
        return &g_local_impersonation_token;
    if (object == &g_anonymous_token)
        return &g_anonymous_token;
    return NULL;
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
    (void)open_as_self;
    if (!token) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    PVOID thread_object = NULL;
    if (!token_resolve_thread(thread, &thread_object)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    PVOID token_object = nt_thread_get_impersonation_token(thread_object);
    if (!token_object) {
        SetLastError(1008); /* ERROR_NO_TOKEN */
        return FALSE;
    }

    HANDLE new_token = NULL;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_TOKEN,
                                   desired_access, token_object, &new_token);
    if (!NT_SUCCESS(status)) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    token_store_handle(token, new_token);
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetThreadToken_stub(PHANDLE thread, HANDLE token)
{
    HANDLE target = NT_CURRENT_THREAD;
    if (thread) {
        target = g_compat32_mode
               ? (HANDLE)(ULONG_PTR)*(DWORD *)(void *)thread
               : *thread;
    }

    PVOID thread_object = NULL;
    if (!token_resolve_thread(target, &thread_object)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    PVOID token_object = NULL;
    if (token && !NT_SUCCESS(handle_lookup(&g_handle_table, token,
                                           OBJ_TYPE_TOKEN, &token_object))) {
        SetLastError(6);
        return FALSE;
    }
    if (token_object && !token_object_info(token_object)) {
        SetLastError(6);
        return FALSE;
    }
    if (!nt_thread_set_impersonation_token(thread_object, token_object)) {
        SetLastError(6);
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

static BOOL WINAPI GetTokenInformation_stub(HANDLE token,
                                             DWORD information_class,
                                             PVOID information,
                                             DWORD information_length,
                                             DWORD *return_length)
{
    PVOID token_object = NULL;
    if (!return_length) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, token, OBJ_TYPE_TOKEN,
                                  &token_object))) {
        *return_length = 0;
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    const LOCAL_TOKEN_OBJECT *local_token = token_object_info(token_object);
    if (!local_token) {
        *return_length = 0;
        SetLastError(6);
        return FALSE;
    }
    DWORD required = 0;
    DWORD value = 0;
    BOOL fixed_dword = FALSE;

    switch (information_class) {
    case 1:  /* TokenUser */
        required = (g_compat32_mode ? 8 : 16) + local_token->user_sid_size;
        break;
    case 2:  /* TokenGroups */
    case 3:  /* TokenPrivileges */
        required = sizeof(DWORD);
        break;
    case 4:  /* TokenOwner */
    case 5:  /* TokenPrimaryGroup */
        required = (g_compat32_mode ? 4 : 8) + local_token->user_sid_size;
        break;
    case 6:  /* TokenDefaultDacl */
        required = g_compat32_mode ? 4 : 8;
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
    case 15: /* TokenSandBoxInert */
    case 21: /* TokenHasRestrictions */
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
        value = 1;
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
    case 25: /* TokenIntegrityLevel */
        required = (g_compat32_mode ? 8 : 16) +
                   sizeof(g_local_integrity_sid);
        break;
    case 27: /* TokenMandatoryPolicy */
        value = 1; /* TOKEN_MANDATORY_POLICY_NO_WRITE_UP */
        fixed_dword = TRUE;
        required = sizeof(DWORD);
        break;
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

    if (fixed_dword) {
        *(DWORD *)(void *)bytes = value;
    } else if (information_class == 1) {
        DWORD header_size = g_compat32_mode ? 8 : 16;
        PVOID sid = bytes + header_size;
        security_store_pointer(bytes, sid);
        *(DWORD *)(void *)(bytes + (g_compat32_mode ? 4 : 8)) = 0;
        reg_memcpy(sid, local_token->user_sid, local_token->user_sid_size);
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
        *(DWORD *)(void *)(bytes + 24) = local_token->token_type;
        *(DWORD *)(void *)(bytes + 28) = local_token->impersonation_level;
    } else if (information_class == 25) {
        DWORD header_size = g_compat32_mode ? 8 : 16;
        PVOID sid = bytes + header_size;
        security_store_pointer(bytes, sid);
        *(DWORD *)(void *)(bytes + (g_compat32_mode ? 4 : 8)) = 0x20;
        reg_memcpy(sid, &g_local_integrity_sid,
                   sizeof(g_local_integrity_sid));
    }
    SetLastError(0);
    return TRUE;
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
    (void)token_attributes;
    PVOID token_object = NULL;
    if (!new_token || impersonation_level > 3 ||
        (token_type != 1 && token_type != 2)) {
        SetLastError(87);
        return FALSE;
    }
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, existing_token,
                                  OBJ_TYPE_TOKEN, &token_object))) {
        SetLastError(6);
        return FALSE;
    }
    HANDLE duplicate = NULL;
    NTSTATUS status = handle_alloc(&g_handle_table, OBJ_TYPE_TOKEN,
                                   desired_access, token_object, &duplicate);
    if (!NT_SUCCESS(status)) {
        SetLastError(8);
        return FALSE;
    }
    if (g_compat32_mode)
        *(DWORD *)(void *)new_token = (DWORD)(ULONG_PTR)duplicate;
    else
        *new_token = duplicate;
    SetLastError(0);
    return TRUE;
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
    PVOID token_object = NULL;
    if (!NT_SUCCESS(handle_lookup(&g_handle_table, token, OBJ_TYPE_TOKEN,
                                  &token_object))) {
        SetLastError(6);
        return FALSE;
    }
    (void)token_object;
    DWORD minimum = sizeof(DWORD);
    switch (information_class) {
    case 4:  /* TokenOwner */
    case 5:  /* TokenPrimaryGroup */
    case 6:  /* TokenDefaultDacl */
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
    case 25: /* TokenIntegrityLevel */
        minimum = g_compat32_mode ? 8 : 16;
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

static BOOL WINAPI CreateProcessAsUserW_compat(
    HANDLE token, PCWSTR application_name, PWSTR command_line,
    PVOID process_attributes, PVOID thread_attributes, BOOL inherit_handles,
    DWORD creation_flags, PVOID environment, PCWSTR current_directory,
    PVOID startup_info, PVOID process_information)
{
    (void)token;
    serial_puts("[ADVAPI32] CreateProcessAsUserW -> current identity\n");
    return CreateProcessW(application_name, command_line,
                          process_attributes, thread_attributes,
                          inherit_handles, creation_flags, environment,
                          current_directory, startup_info,
                          process_information);
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
    const char *path = NULL;
    BYTE view = REG_VIEW_SHARED;
    if (!reg_key_context(key, &path, &view))
        return 6; /* ERROR_INVALID_HANDLE */
    (void)path;
    (void)view;
    /* OsitoK exposes a unified registry view, so reflection is already off. */
    return ERROR_SUCCESS;
}

static const SHIM_EXPORT advapi32_exports[] = {
    { "RegOpenKeyA",        (PVOID)RegOpenKeyA,      3, CC_STDCALL },
    { "RegOpenKeyExA",      (PVOID)RegOpenKeyExA,    5, CC_STDCALL },
    { "RegCreateKeyA",      (PVOID)RegCreateKeyA_k32, 3, CC_STDCALL },
    { "RegCreateKeyExA",    (PVOID)RegCreateKeyExA,  9, CC_STDCALL },
    { "RegQueryValueExA",   (PVOID)RegQueryValueExA, 6, CC_STDCALL },
    { "RegSetValueExA",     (PVOID)RegSetValueExA,   6, CC_STDCALL },
    { "RegCloseKey",        (PVOID)RegCloseKey,      1, CC_STDCALL },
    { "RegDeleteKeyA",      (PVOID)RegDeleteKeyA,    2, CC_STDCALL },
    { "RegDeleteKeyExA",    (PVOID)RegDeleteKeyExA,  4, CC_STDCALL },
    { "RegDeleteValueA",    (PVOID)RegDeleteValueA,  2, CC_STDCALL },
    { "RegEnumKeyExA",      (PVOID)RegEnumKeyExA,    8, CC_STDCALL },
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
    { "InitializeSecurityDescriptor", (PVOID)InitializeSecurityDescriptor, 2, CC_STDCALL },
    { "CreateWellKnownSid", (PVOID)CreateWellKnownSid_stub, 4, CC_STDCALL },
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
    { "AdjustTokenPrivileges", (PVOID)AdjustTokenPrivileges_stub, 6, CC_STDCALL },
    { "SetTokenInformation", (PVOID)SetTokenInformation_stub, 4, CC_STDCALL },
    { "IsValidSid",         (PVOID)IsValidSid_stub, 1, CC_STDCALL },
    { "InitializeAcl",      (PVOID)InitializeAcl_stub, 3, CC_STDCALL },
    { "IsValidAcl",         (PVOID)IsValidAcl_stub, 1, CC_STDCALL },
    { "AddAccessAllowedAce", (PVOID)AddAccessAllowedAce_stub, 4, CC_STDCALL },
    { "SetSecurityDescriptorDacl", (PVOID)SetSecurityDescriptorDacl, 4, CC_STDCALL },
    { "IsValidSecurityDescriptor", (PVOID)IsValidSecurityDescriptor_stub, 1, CC_STDCALL },
    { "GetSecurityDescriptorControl", (PVOID)GetSecurityDescriptorControl_stub, 3, CC_STDCALL },
    { "GetSecurityDescriptorOwner", (PVOID)GetSecurityDescriptorOwner_stub, 3, CC_STDCALL },
    { "GetSecurityDescriptorGroup", (PVOID)GetSecurityDescriptorGroup_stub, 3, CC_STDCALL },
    { "GetSecurityDescriptorDacl", (PVOID)GetSecurityDescriptorDacl_stub, 4, CC_STDCALL },
    { "GetSecurityDescriptorSacl", (PVOID)GetSecurityDescriptorSacl_stub, 4, CC_STDCALL },
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
    { "RegisterTraceGuidsW", (PVOID)RegisterTraceGuidsW_stub, 8, CC_STDCALL },
    { "UnregisterTraceGuids", (PVOID)UnregisterTraceGuids_stub, 1, CC_STDCALL },
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
    { "OpenServiceW",        (PVOID)OpenServiceW, 3, CC_STDCALL },
    { "CreateServiceW",      (PVOID)CreateServiceW, 13, CC_STDCALL },
    { "ChangeServiceConfigW", (PVOID)ChangeServiceConfigW, 11, CC_STDCALL },
    { "ChangeServiceConfig2W", (PVOID)ChangeServiceConfig2W, 3, CC_STDCALL },
    { "DeleteService",       (PVOID)DeleteService, 1, CC_STDCALL },
    { "StartServiceW",       (PVOID)StartServiceW, 3, CC_STDCALL },
    { "ControlService",      (PVOID)ControlService, 3, CC_STDCALL },
    { "QueryServiceStatus",  (PVOID)QueryServiceStatus, 2, CC_STDCALL },
    { "QueryServiceStatusEx", (PVOID)QueryServiceStatusEx, 5, CC_STDCALL },
    { "QueryServiceConfigW", (PVOID)QueryServiceConfigW, 4, CC_STDCALL },
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
