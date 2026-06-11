/*
 * OsitoK Windows Compatibility Layer — advapi32.dll Shim
 *
 * Provides Registry API (RegOpenKeyEx, RegQueryValueEx, etc.)
 * backed by a simple in-memory key-value store.
 *
 * UT99 reads registry for install path, CD key, and configuration.
 * We pre-populate keys with sensible defaults.
 */

#ifndef ADVAPI32_SHIM_H
#define ADVAPI32_SHIM_H

#include "nttypes.h"

/* ── Registry types ────────────────────────────────────────── */

typedef HANDLE HKEY;
typedef HKEY  *PHKEY;

/* Predefined root keys */
#define HKEY_CLASSES_ROOT        ((HKEY)(ULONG_PTR)0x80000000)
#define HKEY_CURRENT_USER        ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_LOCAL_MACHINE       ((HKEY)(ULONG_PTR)0x80000002)
#define HKEY_USERS               ((HKEY)(ULONG_PTR)0x80000003)
#define HKEY_CURRENT_CONFIG      ((HKEY)(ULONG_PTR)0x80000005)

/* Registry value types */
#define REG_NONE                    0
#define REG_SZ                      1
#define REG_EXPAND_SZ               2
#define REG_BINARY                  3
#define REG_DWORD                   4
#define REG_DWORD_LITTLE_ENDIAN     4
#define REG_DWORD_BIG_ENDIAN        5
#define REG_LINK                    6
#define REG_MULTI_SZ                7
#define REG_QWORD                   11

/* Registry access rights */
#define KEY_QUERY_VALUE         0x0001
#define KEY_SET_VALUE           0x0002
#define KEY_CREATE_SUB_KEY      0x0004
#define KEY_ENUMERATE_SUB_KEYS  0x0008
#define KEY_NOTIFY              0x0010
#define KEY_CREATE_LINK         0x0020
#define KEY_READ                0x20019
#define KEY_WRITE               0x20006
#define KEY_ALL_ACCESS          0xF003F

/* Registry create disposition */
#define REG_CREATED_NEW_KEY         1
#define REG_OPENED_EXISTING_KEY     2

/* Error codes */
#define ERROR_SUCCESS               0
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_MORE_DATA             234
#define ERROR_NO_MORE_ITEMS         259

/* ── Registry API ──────────────────────────────────────────── */

LONG WINAPI RegOpenKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult);
LONG WINAPI RegCreateKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD Reserved,
                            PSTR lpClass, DWORD dwOptions, DWORD samDesired,
                            PVOID lpSecurityAttributes, PHKEY phkResult,
                            DWORD *lpdwDisposition);
LONG WINAPI RegQueryValueExA(HKEY hKey, PCSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData);
LONG WINAPI RegSetValueExA(HKEY hKey, PCSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData);
LONG WINAPI RegCloseKey(HKEY hKey);
LONG WINAPI RegDeleteValueA(HKEY hKey, PCSTR lpValueName);
LONG WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, PSTR lpName,
                          DWORD *lpcchName, DWORD *lpReserved,
                          PSTR lpClass, DWORD *lpcchClass,
                          PVOID lpftLastWriteTime);
LONG WINAPI RegEnumValueA(HKEY hKey, DWORD dwIndex, PSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData);

/* Wide (W) variants — delegate to ANSI counterparts */
LONG WINAPI RegOpenKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult);
LONG WINAPI RegQueryValueExW(HKEY hKey, PCWSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData);
LONG WINAPI RegCreateKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD Reserved,
                            PWSTR lpClass, DWORD dwOptions, DWORD samDesired,
                            PVOID lpSecurityAttributes, PHKEY phkResult,
                            DWORD *lpdwDisposition);
LONG WINAPI RegSetValueExW(HKEY hKey, PCWSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData);

/* Installer hook: write a value at an already-normalized lowercase backslash
 * registry path (e.g. "hklm\\software\\app"). Used by the MSI Registry table. */
void advapi32_reg_install_set(const char *path_lc_backslash, const char *name,
                             DWORD type, const void *data, DWORD len);

/* ── Shim init / resolve ───────────────────────────────────── */

PVOID advapi32_shim_init(void);
PVOID advapi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* ADVAPI32_SHIM_H */
