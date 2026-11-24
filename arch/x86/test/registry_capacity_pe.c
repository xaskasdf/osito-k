/* Exercise installed-runtime hive sizes; remove every private test key. */
typedef unsigned int DWORD;
typedef int LONG;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API DWORD CALL GetCurrentProcessId(void);
API LONG CALL RegCreateKeyExA(void *, const char *, DWORD, char *, DWORD,
                            DWORD, void *, void **, DWORD *);
API LONG CALL RegOpenKeyExA(void *, const char *, DWORD, DWORD, void **);
API LONG CALL RegSetValueExA(void *, const char *, DWORD, DWORD,
                           const void *, DWORD);
API LONG CALL RegQueryValueExA(void *, const char *, DWORD *, DWORD *,
                             void *, DWORD *);
API LONG CALL RegCloseKey(void *);
API LONG CALL RegFlushKey(void *);
API LONG CALL RegDeleteKeyA(void *, const char *);
API LONG CALL RegDeleteTreeA(void *, const char *);
#define HKCU ((void *)(__INTPTR_TYPE__)(int)0x80000001U)
#define ACCESS 0xF003FU
#define SAY(s) do { DWORD written; WriteFile(GetStdHandle((DWORD)-11), \
    s, sizeof(s)-1, &written, 0); } while (0)
static unsigned checks, failures;
static void registry_capacity_check(int ok)
{
    checks++;
    if (!ok) { failures++; if (failures < 6) SAY("REGCAP FAIL\n"); }
}
void registry_capacity_entry(void)
{
    static const char hex[] = "0123456789abcdef";
    static char root[] = "Software\\OsitoRegistryCapacity-00000000";
    DWORD pid = GetCurrentProcessId();
    for (unsigned i = 0; i < 8; i++)
        root[sizeof(root)-2-i] = hex[(pid >> (i*4)) & 15];
    void *parent = 0;
    LONG status = RegCreateKeyExA(HKCU, root, 0, 0, 0, ACCESS, 0, &parent, 0);
    registry_capacity_check(status == 0);
    if (status) ExitProcess(1);
    for (unsigned i = 0; i < 256; i++) {
        char name[] = "Key00";
        name[3] = hex[i >> 4]; name[4] = hex[i & 15];
        void *key = 0;
        status = RegCreateKeyExA(parent, name, 0, 0, 0, ACCESS, 0, &key, 0);
        registry_capacity_check(status == 0);
        if (status) continue;
        DWORD values[2] = {i, i ^ 0xAA55AA55U};
        registry_capacity_check(RegSetValueExA(key, "First", 0, 4, &values[0], 4) == 0);
        registry_capacity_check(RegSetValueExA(key, "Second", 0, 4, &values[1], 4) == 0);
        registry_capacity_check(RegCloseKey(key) == 0);
    }
    registry_capacity_check(RegFlushKey(parent) == 0);
    for (unsigned i = 0; i < 256; i++) {
        char name[] = "Key00";
        name[3] = hex[i >> 4]; name[4] = hex[i & 15];
        void *key = 0;
        status = RegOpenKeyExA(parent, name, 0, ACCESS, &key);
        registry_capacity_check(status == 0);
        if (status) continue;
        for (unsigned j = 0; j < 2; j++) {
            DWORD data = 0, type = 0, size = 4;
            status = RegQueryValueExA(key, j ? "Second" : "First", 0, &type, &data, &size);
            registry_capacity_check(status == 0 && type == 4 && size == 4 &&
                                    data == (j ? (i ^ 0xAA55AA55U) : i));
        }
        registry_capacity_check(RegCloseKey(key) == 0);
    }
    registry_capacity_check(RegDeleteTreeA(parent, 0) == 0);
    registry_capacity_check(RegCloseKey(parent) == 0);
    registry_capacity_check(RegDeleteKeyA(HKCU, root) == 0);
    void *absent = 0;
    status = RegOpenKeyExA(HKCU, root, 0, ACCESS, &absent);
    registry_capacity_check(status == 2);
    if (!status) RegCloseKey(absent);
    registry_capacity_check(RegFlushKey(HKCU) == 0);
    registry_capacity_check(checks == 2055);
    if (!failures) SAY("REGCAP DONE 2056 checks\n");
    ExitProcess(failures ? 1 : 0);
}
