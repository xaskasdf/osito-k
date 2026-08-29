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
typedef ULONG_PTR HCRYPTPROV;
typedef HCRYPTPROV *PHCRYPTPROV;

/* Predefined root keys */
/* Windows sign-extends predefined HKEY values in 64-bit processes. */
#define HKEY_CLASSES_ROOT        ((HKEY)(ULONG_PTR)(LONG)0x80000000U)
#define HKEY_CURRENT_USER        ((HKEY)(ULONG_PTR)(LONG)0x80000001U)
#define HKEY_LOCAL_MACHINE       ((HKEY)(ULONG_PTR)(LONG)0x80000002U)
#define HKEY_USERS               ((HKEY)(ULONG_PTR)(LONG)0x80000003U)
#define HKEY_CURRENT_CONFIG      ((HKEY)(ULONG_PTR)(LONG)0x80000005U)

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
#define KEY_WOW64_64KEY         0x0100
#define KEY_WOW64_32KEY         0x0200
#define KEY_READ                0x20019
#define KEY_WRITE               0x20006
#define KEY_ALL_ACCESS          0xF003F

/* Registry create disposition */
#define REG_CREATED_NEW_KEY         1
#define REG_OPENED_EXISTING_KEY     2

/* Error codes */
#define ERROR_SUCCESS               0
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_ACCESS_DENIED          5
#define ERROR_INVALID_HANDLE         6
#define ERROR_INVALID_PARAMETER      87
#define ERROR_MORE_DATA             234
#define ERROR_NO_MORE_ITEMS         259

/* ── Registry API ──────────────────────────────────────────── */

LONG WINAPI RegOpenKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult);
LONG WINAPI RegOpenKeyA(HKEY hKey, PCSTR lpSubKey, PHKEY phkResult);
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
LONG WINAPI RegDeleteKeyA(HKEY hKey, PCSTR lpSubKey);
LONG WINAPI RegDeleteKeyExA(HKEY hKey, PCSTR lpSubKey, DWORD samDesired,
                            DWORD Reserved);
LONG WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, PSTR lpName,
                          DWORD *lpcchName, DWORD *lpReserved,
                          PSTR lpClass, DWORD *lpcchClass,
                          PVOID lpftLastWriteTime);
LONG WINAPI RegEnumValueA(HKEY hKey, DWORD dwIndex, PSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData);
LONG WINAPI RegQueryInfoKeyA(HKEY hKey, PSTR lpClass, DWORD *lpcchClass,
                             DWORD *lpReserved, DWORD *lpcSubKeys,
                             DWORD *lpcbMaxSubKeyLen, DWORD *lpcbMaxClassLen,
                             DWORD *lpcValues, DWORD *lpcbMaxValueNameLen,
                             DWORD *lpcbMaxValueLen,
                             DWORD *lpcbSecurityDescriptor,
                             PVOID lpftLastWriteTime);
LONG WINAPI RegDeleteTreeA(HKEY hKey, PCSTR lpSubKey);

/* Wide (W) variants — delegate to ANSI counterparts */
LONG WINAPI RegOpenKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD ulOptions,
                          DWORD samDesired, PHKEY phkResult);
LONG WINAPI RegQueryValueExW(HKEY hKey, PCWSTR lpValueName, DWORD *lpReserved,
                             DWORD *lpType, BYTE *lpData, DWORD *lpcbData);
LONG WINAPI RegEnumKeyExW(HKEY hKey, DWORD dwIndex, PWSTR lpName,
                          DWORD *lpcchName, DWORD *lpReserved,
                          PWSTR lpClass, DWORD *lpcchClass,
                          PVOID lpftLastWriteTime);
LONG WINAPI RegEnumValueW(HKEY hKey, DWORD dwIndex, PWSTR lpValueName,
                          DWORD *lpcchValueName, DWORD *lpReserved,
                          DWORD *lpType, BYTE *lpData, DWORD *lpcbData);
LONG WINAPI RegQueryInfoKeyW(HKEY hKey, PWSTR lpClass, DWORD *lpcchClass,
                             DWORD *lpReserved, DWORD *lpcSubKeys,
                             DWORD *lpcbMaxSubKeyLen, DWORD *lpcbMaxClassLen,
                             DWORD *lpcValues, DWORD *lpcbMaxValueNameLen,
                             DWORD *lpcbMaxValueLen,
                             DWORD *lpcbSecurityDescriptor,
                             PVOID lpftLastWriteTime);
LONG WINAPI RegCreateKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD Reserved,
                            PWSTR lpClass, DWORD dwOptions, DWORD samDesired,
                            PVOID lpSecurityAttributes, PHKEY phkResult,
                            DWORD *lpdwDisposition);
LONG WINAPI RegSetValueExW(HKEY hKey, PCWSTR lpValueName, DWORD Reserved,
                           DWORD dwType, const BYTE *lpData, DWORD cbData);
LONG WINAPI RegDeleteValueW(HKEY hKey, PCWSTR lpValueName);
LONG WINAPI RegDeleteKeyW(HKEY hKey, PCWSTR lpSubKey);
LONG WINAPI RegDeleteKeyExW(HKEY hKey, PCWSTR lpSubKey, DWORD samDesired,
                            DWORD Reserved);

/* Installer hook: write a value at an already-normalized lowercase backslash
 * registry path (e.g. "hklm\\software\\app"). Used by the MSI Registry table. */
void advapi32_reg_install_set(const char *path_lc_backslash, const char *name,
                             DWORD type, const void *data, DWORD len);

/* Legacy CryptoAPI provider contexts. The current provider surface supports
 * random generation and keeps contexts isolated by Win32 process. */
BOOL WINAPI CryptAcquireContextA(PHCRYPTPROV phProv, PCSTR container,
                                 PCSTR provider, DWORD provider_type,
                                 DWORD flags);
BOOL WINAPI CryptAcquireContextW(PHCRYPTPROV phProv, PCWSTR container,
                                 PCWSTR provider, DWORD provider_type,
                                 DWORD flags);
BOOL WINAPI CryptGenRandom(HCRYPTPROV provider, DWORD length, BYTE *buffer);
BOOL WINAPI CryptReleaseContext(HCRYPTPROV provider, DWORD flags);
DWORD advapi32_crypto_release_process(DWORD process_id);
int advapi32_registry_selftest(void);

/* ── Shim init / resolve ───────────────────────────────────── */

PVOID advapi32_shim_init(void);
PVOID advapi32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal);

#endif /* ADVAPI32_SHIM_H */
