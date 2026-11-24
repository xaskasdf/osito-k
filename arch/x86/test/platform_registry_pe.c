/* Read-only platform metadata probe; also runs on native Windows. */
typedef unsigned int DWORD;
typedef int LONG;
typedef __WCHAR_TYPE__ WCHAR;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API LONG CALL RegOpenKeyExA(void *, const char *, DWORD, DWORD, void **);
API LONG CALL RegOpenKeyExW(void *, const WCHAR *, DWORD, DWORD, void **);
API LONG CALL RegQueryValueExA(void *, const char *, DWORD *, DWORD *, void *, DWORD *);
API LONG CALL RegQueryValueExW(void *, const WCHAR *, DWORD *, DWORD *, void *, DWORD *);
API LONG CALL RegCloseKey(void *);
#define HKLM ((void *)(__INTPTR_TYPE__)(int)0x80000002U)
static unsigned failures;

static void platform_registry_write(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}

static void platform_registry_check(int ok, const char *label)
{
    if (!ok) failures++;
    platform_registry_write(ok ? "PLATFORMREG PASS " : "PLATFORMREG FAIL ");
    platform_registry_write(label);
    platform_registry_write("\n");
}

void platform_registry_entry(void)
{
    static const DWORD views[] = {0, 0x200, 0x100};
    static const char version[] = "4.09.00.0904";
    for (unsigned i = 0; i < sizeof(views)/sizeof(views[0]); i++) {
        struct { void *key; DWORD guard; } opened = {0, 0xAA55AA55};
        LONG status = RegOpenKeyExA(HKLM, "sOfTwArE\\MiCrOsOfT\\DiReCtX", 0,
                                   0x20019 | views[i], &opened.key);
        platform_registry_check(!status && opened.key && opened.guard == 0xAA55AA55,
                                "open selected view");
        if (status) continue;
        DWORD type = 0, size = 0;
        status = RegQueryValueExA(opened.key, "Version", 0, &type, 0, &size);
        platform_registry_check(!status && type == 1 && size == sizeof(version),
                                "ANSI size includes terminator");
        struct { char text[32]; DWORD guard; } ansi = {{0}, 0x13579BDF};
        size = 4;
        status = RegQueryValueExA(opened.key, "Version", 0, &type, ansi.text, &size);
        platform_registry_check(status == 234 && size == sizeof(version) &&
                                ansi.guard == 0x13579BDF, "short buffer");
        size = sizeof(ansi.text);
        status = RegQueryValueExA(opened.key, "vErSiOn", 0, &type, ansi.text, &size);
        int same = 1;
        for (unsigned j = 0; j < sizeof(version); j++)
            if (ansi.text[j] != version[j]) same = 0;
        platform_registry_check(!status && type == 1 && size == sizeof(version) &&
                                same && ansi.guard == 0x13579BDF, "ANSI value");
        struct { WCHAR text[32]; DWORD guard; } wide = {{0}, 0x2468ACE0};
        size = 0;
        status = RegQueryValueExW(opened.key, L"Version", 0, &type, 0, &size);
        platform_registry_check(!status && type == 1 && size == sizeof(version)*2,
                                "UTF16 byte size");
        size = sizeof(wide.text);
        status = RegQueryValueExW(opened.key, L"Version", 0, &type, wide.text, &size);
        same = 1;
        for (unsigned j = 0; j < sizeof(version); j++)
            if (wide.text[j] != (WCHAR)version[j]) same = 0;
        platform_registry_check(!status && type == 1 && size == sizeof(version)*2 &&
                                same && wide.guard == 0x2468ACE0, "UTF16 value");
        size = sizeof(ansi.text);
        status = RegQueryValueExA(opened.key, "OsitoPlatformAbsentValue_924713", 0,
                                  &type, ansi.text, &size);
        platform_registry_check(status == 2, "unprovided value stays absent");
        platform_registry_check(!RegCloseKey(opened.key), "close ANSI handle");
        opened.key = 0;
        status = RegOpenKeyExW(HKLM, L"Software\\Microsoft\\DirectX", 0,
                               0x20019 | views[i], &opened.key);
        platform_registry_check(!status && opened.key && opened.guard == 0xAA55AA55,
                                "wide open selected view");
        if (!status) platform_registry_check(!RegCloseKey(opened.key), "close wide handle");
        void *parent = 0, *child = 0;
        status = RegOpenKeyExA(HKLM, "Software\\Microsoft", 0,
                               0x20019 | views[i], &parent);
        platform_registry_check(!status && parent, "parent exists");
        if (!status) {
            status = RegOpenKeyExA(parent, "DirectX", 0, 0x20019, &child);
            platform_registry_check(!status && child, "relative open inherits view");
            if (!status) RegCloseKey(child);
            RegCloseKey(parent);
        }
    }
    platform_registry_write("PLATFORMREG DONE\n");
    ExitProcess(failures ? 1 : 0);
}
