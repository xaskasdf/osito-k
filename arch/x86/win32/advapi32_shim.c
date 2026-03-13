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

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

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

typedef struct {
    char    path[MAX_REG_PATH];     /* full normalized path e.g. "HKLM\\software\\foo" */
    int     used;
    ULONG   handle_id;              /* pseudo-handle ID for open keys */
} REG_KEY;

typedef struct {
    char    key_path[MAX_REG_PATH]; /* key this value belongs to */
    char    name[128];              /* value name (empty = default) */
    DWORD   type;                   /* REG_SZ, REG_DWORD, etc. */
    BYTE    data[MAX_REG_DATA];
    DWORD   data_len;
    int     used;
} REG_VALUE;

static REG_KEY   reg_keys[MAX_REG_KEYS];
static REG_VALUE reg_values[MAX_REG_VALUES];
static int       reg_initialized = 0;
static ULONG     next_handle_id  = 0x90000001;

/* ── Path normalization ────────────────────────────────────── */

/* Build full path: root prefix + subkey, lowercased, backslash-normalized */
static void build_path(char *out, HKEY root, const char *subkey)
{
    const char *prefix;
    if (root == HKEY_LOCAL_MACHINE)      prefix = "HKLM";
    else if (root == HKEY_CURRENT_USER)  prefix = "HKCU";
    else if (root == HKEY_CLASSES_ROOT)  prefix = "HKCR";
    else if (root == HKEY_USERS)         prefix = "HKU";
    else if (root == HKEY_CURRENT_CONFIG)prefix = "HKCC";
    else {
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

static void reg_set_value(const char *key_path, const char *name,
                          DWORD type, const void *data, DWORD data_len)
{
    /* Find or create the key */
    int ki = -1;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used && reg_stricmp(reg_keys[i].path, key_path) == 0) {
            ki = i;
            break;
        }
    }
    if (ki < 0) {
        for (int i = 0; i < MAX_REG_KEYS; i++) {
            if (!reg_keys[i].used) {
                ki = i;
                reg_strcpy(reg_keys[ki].path, key_path);
                reg_keys[ki].used = 1;
                reg_keys[ki].handle_id = 0;
                break;
            }
        }
    }

    /* Find or create value */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used) {
            reg_strcpy(reg_values[i].key_path, key_path);
            reg_strcpy(reg_values[i].name, name ? name : "");
            reg_values[i].type     = type;
            reg_values[i].data_len = data_len < MAX_REG_DATA ? data_len : MAX_REG_DATA;
            reg_memcpy(reg_values[i].data, data, reg_values[i].data_len);
            reg_values[i].used = 1;
            return;
        }
    }
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
        reg_set_value(key, "folder", REG_SZ, path, path_len);
    }

    /* CD key (empty — not needed for LAN/offline) */
    {
        const char *key = "hklm\\software\\unreal technology\\installed apps\\unrealtournament";
        const char *cdkey = "";
        reg_set_value(key, "cdkey", REG_SZ, cdkey, 1);
    }

    /* DirectX version hint */
    {
        const char *key = "hklm\\software\\microsoft\\directx";
        const char *ver = "4.09.00.0904";
        reg_set_value(key, "version", REG_SZ, ver, reg_strlen(ver) + 1);
    }

    serial_puts("[ADVAPI32] registry initialized with UT99 defaults\n");
}

/* ── Registry API implementations ──────────────────────────── */

LONG WINAPI RegOpenKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult)
{
    (void)ulOptions;
    (void)samDesired;
    reg_init();

    if (!phkResult) return ERROR_FILE_NOT_FOUND;

    char path[MAX_REG_PATH];
    build_path(path, hKey, lpSubKey);

    serial_puts("[REG] OpenKeyEx: ");
    serial_puts(path);
    serial_puts("\n");

    /* Search for a key with this path, or any values under this path */
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used && reg_stricmp(reg_keys[i].path, path) == 0) {
            /* Assign a handle ID if not already */
            if (reg_keys[i].handle_id == 0)
                reg_keys[i].handle_id = next_handle_id++;
            *phkResult = (HKEY)(ULONG_PTR)reg_keys[i].handle_id;
            return ERROR_SUCCESS;
        }
    }

    /* Check if any values exist under this path */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (reg_values[i].used && reg_stricmp(reg_values[i].key_path, path) == 0) {
            /* Create a key entry for it */
            for (int k = 0; k < MAX_REG_KEYS; k++) {
                if (!reg_keys[k].used) {
                    reg_strcpy(reg_keys[k].path, path);
                    reg_keys[k].used = 1;
                    reg_keys[k].handle_id = next_handle_id++;
                    *phkResult = (HKEY)(ULONG_PTR)reg_keys[k].handle_id;
                    return ERROR_SUCCESS;
                }
            }
        }
    }

    serial_puts("[REG]   not found\n");
    *phkResult = NULL;
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegCreateKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD Reserved,
                            PSTR lpClass, DWORD dwOptions, DWORD samDesired,
                            PVOID lpSecurityAttributes, PHKEY phkResult,
                            DWORD *lpdwDisposition)
{
    (void)Reserved;
    (void)lpClass;
    (void)dwOptions;
    (void)samDesired;
    (void)lpSecurityAttributes;
    reg_init();

    if (!phkResult) return ERROR_FILE_NOT_FOUND;

    char path[MAX_REG_PATH];
    build_path(path, hKey, lpSubKey);

    /* Try to open first */
    LONG result = RegOpenKeyExA(hKey, lpSubKey, 0, KEY_ALL_ACCESS, phkResult);
    if (result == ERROR_SUCCESS) {
        if (lpdwDisposition) *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        return ERROR_SUCCESS;
    }

    /* Create new key */
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used) {
            reg_strcpy(reg_keys[i].path, path);
            reg_keys[i].used = 1;
            reg_keys[i].handle_id = next_handle_id++;
            *phkResult = (HKEY)(ULONG_PTR)reg_keys[i].handle_id;
            if (lpdwDisposition) *lpdwDisposition = REG_CREATED_NEW_KEY;
            return ERROR_SUCCESS;
        }
    }

    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegQueryValueExA(HKEY hKey, PCSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    /* Find the key path from handle */
    const char *key_path = NULL;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_keys[i].handle_id == (ULONG)(ULONG_PTR)hKey) {
            key_path = reg_keys[i].path;
            break;
        }
    }

    if (!key_path) return ERROR_FILE_NOT_FOUND;

    const char *val_name = lpValueName ? lpValueName : "";

    serial_puts("[REG] QueryValueEx: ");
    serial_puts(key_path);
    serial_puts(" \\ ");
    serial_puts(val_name);
    serial_puts("\n");

    /* Search values */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (reg_values[i].used &&
            reg_stricmp(reg_values[i].key_path, key_path) == 0 &&
            reg_stricmp(reg_values[i].name, val_name) == 0) {

            if (lpType) *lpType = reg_values[i].type;

            if (lpcbData) {
                if (lpData) {
                    if (*lpcbData < reg_values[i].data_len) {
                        *lpcbData = reg_values[i].data_len;
                        return ERROR_MORE_DATA;
                    }
                    reg_memcpy(lpData, reg_values[i].data, reg_values[i].data_len);
                }
                *lpcbData = reg_values[i].data_len;
            }

            return ERROR_SUCCESS;
        }
    }

    serial_puts("[REG]   value not found\n");
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegSetValueExA(HKEY hKey, PCSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData)
{
    (void)Reserved;
    reg_init();

    /* Find key path */
    const char *key_path = NULL;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_keys[i].handle_id == (ULONG)(ULONG_PTR)hKey) {
            key_path = reg_keys[i].path;
            break;
        }
    }

    if (!key_path) return ERROR_FILE_NOT_FOUND;

    const char *val_name = lpValueName ? lpValueName : "";

    /* Update existing or create new */
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (reg_values[i].used &&
            reg_stricmp(reg_values[i].key_path, key_path) == 0 &&
            reg_stricmp(reg_values[i].name, val_name) == 0) {
            /* Update */
            reg_values[i].type = dwType;
            reg_values[i].data_len = cbData < MAX_REG_DATA ? cbData : MAX_REG_DATA;
            if (lpData)
                reg_memcpy(reg_values[i].data, lpData, reg_values[i].data_len);
            return ERROR_SUCCESS;
        }
    }

    /* Create new value */
    reg_set_value(key_path, val_name, dwType, lpData, cbData);
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
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_keys[i].handle_id == (ULONG)(ULONG_PTR)hKey) {
            key_path = reg_keys[i].path;
            break;
        }
    }

    if (!key_path) return ERROR_FILE_NOT_FOUND;

    const char *val_name = lpValueName ? lpValueName : "";

    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (reg_values[i].used &&
            reg_stricmp(reg_values[i].key_path, key_path) == 0 &&
            reg_stricmp(reg_values[i].name, val_name) == 0) {
            reg_values[i].used = 0;
            return ERROR_SUCCESS;
        }
    }

    return ERROR_FILE_NOT_FOUND;
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

    /* Find key path */
    const char *key_path = NULL;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_keys[i].handle_id == (ULONG)(ULONG_PTR)hKey) {
            key_path = reg_keys[i].path;
            break;
        }
    }

    if (!key_path) return ERROR_FILE_NOT_FOUND;

    int path_len = reg_strlen(key_path);

    /* Count subkeys: keys whose path starts with key_path + "\\" and has no further "\\" */
    DWORD found = 0;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used) continue;
        if (reg_strlen(reg_keys[i].path) <= path_len) continue;

        /* Check prefix match */
        int match = 1;
        for (int j = 0; j < path_len; j++) {
            char ca = key_path[j], cb = reg_keys[i].path[j];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) { match = 0; break; }
        }
        if (!match || reg_keys[i].path[path_len] != '\\') continue;

        /* Check no further backslash after the separator */
        const char *sub = &reg_keys[i].path[path_len + 1];
        int has_sub = 0;
        for (const char *p = sub; *p; p++) {
            if (*p == '\\') { has_sub = 1; break; }
        }
        if (has_sub) continue;

        /* This is a direct child subkey */
        if (found == dwIndex) {
            int sublen = reg_strlen(sub);
            if (lpName && lpcchName && *lpcchName > (DWORD)sublen) {
                reg_strcpy(lpName, sub);
                *lpcchName = sublen;
                return ERROR_SUCCESS;
            }
            return ERROR_MORE_DATA;
        }
        found++;
    }

    return ERROR_NO_MORE_ITEMS;
}

LONG WINAPI RegEnumValueA(HKEY hKey, DWORD dwIndex, PSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    (void)lpReserved;
    reg_init();

    /* Find key path */
    const char *key_path = NULL;
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (reg_keys[i].used &&
            reg_keys[i].handle_id == (ULONG)(ULONG_PTR)hKey) {
            key_path = reg_keys[i].path;
            break;
        }
    }

    if (!key_path) return ERROR_FILE_NOT_FOUND;

    /* Find the Nth value for this key */
    DWORD found = 0;
    for (int i = 0; i < MAX_REG_VALUES; i++) {
        if (!reg_values[i].used) continue;
        if (reg_stricmp(reg_values[i].key_path, key_path) != 0) continue;

        if (found == dwIndex) {
            int nlen = reg_strlen(reg_values[i].name);
            if (lpValueName && lpcchValueName) {
                if (*lpcchValueName <= (DWORD)nlen) return ERROR_MORE_DATA;
                reg_strcpy(lpValueName, reg_values[i].name);
                *lpcchValueName = nlen;
            }
            if (lpType) *lpType = reg_values[i].type;
            if (lpcbData) {
                if (lpData) {
                    if (*lpcbData < reg_values[i].data_len) {
                        *lpcbData = reg_values[i].data_len;
                        return ERROR_MORE_DATA;
                    }
                    reg_memcpy(lpData, reg_values[i].data, reg_values[i].data_len);
                }
                *lpcbData = reg_values[i].data_len;
            }
            return ERROR_SUCCESS;
        }
        found++;
    }

    return ERROR_NO_MORE_ITEMS;
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
    serial_puts("[REG] RegSetValueExW -> A: ");
    serial_puts(ansi_name);
    serial_puts("\n");
    return RegSetValueExA(hKey, ansi_name, Reserved, dwType, lpData, cbData);
}

LONG WINAPI RegOpenKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult)
{
    char ansi_subkey[MAX_REG_PATH];
    wide_to_ansi(ansi_subkey, lpSubKey, MAX_REG_PATH);
    serial_puts("[REG] RegOpenKeyExW -> A: ");
    serial_puts(ansi_subkey);
    serial_puts("\n");
    return RegOpenKeyExA(hKey, ansi_subkey, ulOptions, samDesired, phkResult);
}

LONG WINAPI RegQueryValueExW(HKEY hKey, PCWSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData)
{
    char ansi_name[128];
    wide_to_ansi(ansi_name, lpValueName, 128);
    serial_puts("[REG] RegQueryValueExW -> A: ");
    serial_puts(ansi_name);
    serial_puts("\n");
    return RegQueryValueExA(hKey, ansi_name, lpReserved, lpType, lpData, lpcbData);
}

/* ── User identity stubs (UT99 Core.dll) ──────────────────── */

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

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT advapi32_exports[] = {
    { "RegOpenKeyExA",      (PVOID)RegOpenKeyExA },
    { "RegCreateKeyExA",    (PVOID)RegCreateKeyExA },
    { "RegQueryValueExA",   (PVOID)RegQueryValueExA },
    { "RegSetValueExA",     (PVOID)RegSetValueExA },
    { "RegCloseKey",        (PVOID)RegCloseKey },
    { "RegDeleteValueA",    (PVOID)RegDeleteValueA },
    { "RegEnumKeyExA",      (PVOID)RegEnumKeyExA },
    { "RegEnumValueA",      (PVOID)RegEnumValueA },
    { "RegOpenKeyExW",      (PVOID)RegOpenKeyExW },
    { "RegQueryValueExW",   (PVOID)RegQueryValueExW },
    { "RegCreateKeyExW",    (PVOID)RegCreateKeyExW },
    { "RegSetValueExW",     (PVOID)RegSetValueExW },
    { "GetUserNameA",       (PVOID)GetUserNameA },
    { "GetUserNameW",       (PVOID)GetUserNameW },
    { NULL, NULL }
};

static int advapi_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID advapi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
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
