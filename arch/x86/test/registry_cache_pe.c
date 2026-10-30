/* Same binary on Windows and OsitoK. Only touch this process's fresh HKCU key. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef int LONG;
typedef int BOOL;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define HKCU ((HANDLE)(__INTPTR_TYPE__)(int)0x80000001U)
#define SENTINEL 0x13572468U
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API LONG CALL RegDisablePredefinedCache(void);
API LONG CALL RegDisablePredefinedCacheEx(void);
void *memcpy(void *out, const void *in, __SIZE_TYPE__ size)
{
    for (__SIZE_TYPE__ i = 0; i < size; i++)
        ((unsigned char *)out)[i] = ((const unsigned char *)in)[i];
    return out;
}
static LONG (CALL *disable)(void), (CALL *disable_ex)(void);
static LONG (CALL *create_key)(HANDLE, const char *, DWORD, char *, DWORD, DWORD, void *, HANDLE *, DWORD *);
static LONG (CALL *open_key)(HANDLE, const char *, DWORD, DWORD, HANDLE *);
static LONG (CALL *open_key_w)(HANDLE, const WORD *, DWORD, DWORD, HANDLE *);
static LONG (CALL *set_value)(HANDLE, const char *, DWORD, DWORD, const void *, DWORD);
static LONG (CALL *query_value)(HANDLE, const char *, void *, DWORD *, void *, DWORD *);
static LONG (CALL *delete_key)(HANDLE, const char *);
static LONG (CALL *close_key)(HANDLE);
static DWORD (CALL *process_id)(void);
static DWORD failures;
static void report(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void check(BOOL ok, const char *name)
{
    if (!ok) { failures++; report("REGISTRY-CACHE FAIL: "); report(name); report("\n"); }
}
static void verify_value(HANDLE key, DWORD expected)
{
    DWORD value = 0, size = sizeof(value), type = 0;
    LONG status = query_value(key, "sequence", 0, &type, &value, &size);
    check(!status && type == 4 && size == sizeof(value) && value == expected,
          "fresh registry value through retained/opened handle");
}
static void cache_calls(void)
{
    SetLastError(SENTINEL);
    LONG status = RegDisablePredefinedCache();
    check(!status && GetLastError() == SENTINEL, "direct HKCU disable ABI/LastError");
    SetLastError(SENTINEL);
    status = RegDisablePredefinedCacheEx();
    check(!status && GetLastError() == SENTINEL, "direct all-roots disable ABI/LastError");
    SetLastError(SENTINEL);
    status = disable();
    check(!status && GetLastError() == SENTINEL, "dynamic HKCU disable ABI/LastError");
    SetLastError(SENTINEL);
    status = disable_ex();
    check(!status && GetLastError() == SENTINEL, "dynamic all-roots disable ABI/LastError");
}
void mainCRTStartup(void)
{
    HANDLE adv = LoadLibraryA("advapi32.dll"), k32 = LoadLibraryA("kernel32.dll");
    disable = (void *)GetProcAddress(adv, "RegDisablePredefinedCache");
    disable_ex = (void *)GetProcAddress(adv, "RegDisablePredefinedCacheEx");
    if (!disable || !disable_ex) {
        report("REGISTRY-CACHE FAIL: missing cache-disable export\n");
        ExitProcess(1);
    }
#define GET(dst, name) do { dst = (void *)GetProcAddress(adv, name); check(dst != 0, name); } while (0)
    GET(create_key, "RegCreateKeyExA"); GET(open_key, "RegOpenKeyExA");
    GET(open_key_w, "RegOpenKeyExW"); GET(set_value, "RegSetValueExA");
    GET(query_value, "RegQueryValueExA"); GET(delete_key, "RegDeleteKeyA");
    GET(close_key, "RegCloseKey");
    process_id = (void *)GetProcAddress(k32, "GetCurrentProcessId");
    check(process_id != 0, "GetCurrentProcessId");
    if (failures) ExitProcess(1);
    cache_calls();
    char path[] = "Software\\OsitoRegistryCache00000000-64";
    DWORD end = sizeof(path) - 1, pid = process_id();
    if (sizeof(void *) == 4) { path[end - 2] = '3'; path[end - 1] = '2'; }
    for (DWORD i = 0; i < 8; i++) path[end - 11 + i] = "0123456789abcdef"[(pid >> (28 - 4 * i)) & 15];
    WORD wide[sizeof(path)];
    for (DWORD i = 0; i < sizeof(path); i++) wide[i] = (unsigned char)path[i];
    HANDLE key = 0; DWORD disposition = 0;
    LONG status = create_key(HKCU, path, 0, 0, 0, 0x2001f, 0, &key, &disposition);
    check(!status && key && disposition == 1, "fresh owned test key");
    if (status || !key || disposition != 1) {
        if (key) close_key(key);
        ExitProcess(1); /* Never change/delete an existing key. */
    }
    for (DWORD i = 1; i <= 64; i++) {
        check(!set_value(key, "sequence", 0, 4, &i, sizeof(i)), "write sequence");
        cache_calls();
        verify_value(key, i);
        HANDLE reopened = 0;
        status = i & 1 ? open_key(HKCU, path, 0, 0x20019, &reopened) :
                         open_key_w(HKCU, wide, 0, 0x20019, &reopened);
        check(!status && reopened, "reopen root A/W after disable");
        if (reopened) {
            verify_value(reopened, i);
            check(!close_key(reopened), "close reopened key");
        }
    }
    check(!close_key(key), "close retained key");
    check(!delete_key(HKCU, path), "remove own test key");
    cache_calls();
    HANDLE absent = 0;
    check(open_key(HKCU, path, 0, 0x20019, &absent) == 2 && !absent,
          "deleted key not resurrected by disable");
    if (absent) close_key(absent);
    report(failures ? "REGISTRY-CACHE FAILED\n" : "REGISTRY-CACHE PASS\n");
    ExitProcess(failures != 0);
}
