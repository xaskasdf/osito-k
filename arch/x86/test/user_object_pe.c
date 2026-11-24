/* Same USER object naming/lifetime probe on Windows and OsitoK, PE32/PE64. */
typedef unsigned int DWORD, UINT;
typedef unsigned short WCHAR;
typedef int BOOL;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);

void *memcpy(void *destination, const void *source, __SIZE_TYPE__ length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (__SIZE_TYPE__ i = 0; i < length; i++) out[i] = in[i];
    return destination;
}

static HANDLE (CALL *create_station)(const WCHAR *, DWORD, DWORD, void *);
static HANDLE (CALL *get_station)(void);
static BOOL (CALL *set_station)(HANDLE);
static BOOL (CALL *close_station)(HANDLE);
static HANDLE (CALL *create_desktop)(const WCHAR *, const WCHAR *, void *, DWORD,
                                    DWORD, void *);
static BOOL (CALL *close_desktop)(HANDLE);
static BOOL (CALL *information_w)(HANDLE, int, void *, DWORD, DWORD *);
static BOOL (CALL *information_a)(HANDLE, int, void *, DWORD, DWORD *);
static DWORD (CALL *get_pid)(void);
static DWORD checks, failures;

static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}

static void check(BOOL condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        report("USER-OBJECT FAIL: "); report(name); report("\n");
    }
}

static DWORD wide_length(const WCHAR *value)
{
    DWORD size = 0;
    while (value[size]) size++;
    return size;
}

static BOOL equal(const WCHAR *left, const WCHAR *right)
{
    DWORD i = 0;
    while (left[i] && left[i] == right[i]) i++;
    return left[i] == right[i];
}

static BOOL name(HANDLE object, WCHAR *buffer)
{
    DWORD needed = 0;
    check(!information_w(object, 2, 0, 0, &needed) &&
          GetLastError() == 122, "name size probe");
    check(needed > sizeof(WCHAR) && needed <= 128 && !(needed & 1),
          "name is nonempty UTF-16 including terminator");
    if (needed <= sizeof(WCHAR) || needed > 128 || (needed & 1)) return 0;
    buffer[0] = 0x5a5a;
    DWORD actual = 0;
    check(!information_w(object, 2, buffer, needed - 1, &actual) &&
          GetLastError() == 122 && actual == needed && buffer[0] == 0x5a5a,
          "short name buffer unchanged");
    BOOL ok = information_w(object, 2, buffer, needed, &actual);
    check(ok && actual == needed && buffer[needed / 2 - 1] == 0,
          "exact name buffer");
    if (!ok) return 0;
    char ansi[64];
    actual = 0;
    check(information_a(object, 2, ansi, sizeof(ansi), &actual) &&
          actual * 2 == needed, "ANSI name size");
    for (DWORD i = 0; i < needed / 2; i++)
        check((unsigned char)ansi[i] == buffer[i], "ANSI and Unicode names agree");
    return 1;
}

void mainCRTStartup(void)
{
    HANDLE library = LoadLibraryA("user32.dll");
    create_station = GetProcAddress(library, "CreateWindowStationW");
    get_station = GetProcAddress(library, "GetProcessWindowStation");
    set_station = GetProcAddress(library, "SetProcessWindowStation");
    close_station = GetProcAddress(library, "CloseWindowStation");
    create_desktop = GetProcAddress(library, "CreateDesktopW");
    close_desktop = GetProcAddress(library, "CloseDesktop");
    information_w = GetProcAddress(library, "GetUserObjectInformationW");
    information_a = GetProcAddress(library, "GetUserObjectInformationA");
    get_pid = GetProcAddress(LoadLibraryA("kernel32.dll"), "GetCurrentProcessId");
    if (!create_station || !get_station || !set_station || !close_station ||
        !create_desktop || !close_desktop || !information_w || !information_a ||
        !get_pid) {
        report("USER-OBJECT MISSING EXPORT\n"); ExitProcess(2);
    }

    const DWORD station_access = 0x000f037f;
    HANDLE original = get_station();
    WCHAR original_name[64], first_name[64], second_name[64], after_name[64];
    check(original && name(original, original_name), "original station named");
    HANDLE first = create_station(0, 0, station_access, 0);
    HANDLE second = create_station(L"", 0, station_access, 0);
    check(first && second, "NULL and empty station names accepted");
    BOOL first_named = first && name(first, first_name);
    BOOL second_named = second && name(second, second_name);
    if (first_named && second_named) {
        check(equal(first_name, second_name), "same logon yields same generated name");
        check(!equal(first_name, original_name), "generated station is not interactive station");
        report("Generated station: ");
        char text[65];
        DWORD i = 0;
        while (first_name[i] && i < 64) { text[i] = (char)first_name[i]; i++; }
        text[i] = 0;
        report(text); report("\n");
        HANDLE reopened = create_station(first_name, 0, station_access, 0);
        if (!reopened && GetLastError() == 5)
            report("USER-OBJECT SKIP: explicit names require administrator rights\n");
        else
            check(reopened && name(reopened, after_name) && equal(first_name, after_name),
                  "generated name can be reopened explicitly");
        if (reopened) check(close_station(reopened), "close explicit generated reference");
    }
    HANDLE duplicate = create_station(0, 1, station_access, 0);
    check(!duplicate, "create-only rejects existing generated station");
    if (duplicate) close_station(duplicate);
    if (first) check(close_station(first), "close first generated reference");
    if (second_named) check(name(second, after_name) && equal(second_name, after_name),
                            "second reference survives first close");

    if (second && set_station(second)) {
        check(1, "associate generated station");
        HANDLE desktop = create_desktop(L"OsitoUserObjectContract", 0, 0, 0,
                                        0x000f01ff, 0);
        check(desktop && name(desktop, after_name) &&
              equal(after_name, L"OsitoUserObjectContract"), "alternate desktop name");
        check(set_station(original), "restore original station");
        if (desktop) check(close_desktop(desktop), "close alternate desktop");
    } else {
        check(0, "associate generated station");
    }
    if (second) check(close_station(second), "close last generated reference");
    check(get_station() == original, "original association retained");

    WCHAR explicit_name[] = L"OsitoStation-00000000";
    DWORD pid = get_pid();
    for (DWORD i = 0; i < 8; i++)
        explicit_name[13 + i] = L"0123456789abcdef"[(pid >> (28 - i * 4)) & 15];
    first = create_station(explicit_name, 1, station_access, 0);
    if (!first && GetLastError() == 5)
        report("USER-OBJECT SKIP: named create-only requires administrator rights\n");
    else
        check(first && name(first, first_name) && equal(first_name, explicit_name),
              "named create-only preserves requested name");
    if (first) {
        duplicate = create_station(explicit_name, 1, station_access, 0);
        check(!duplicate, "create-only rejects existing explicit station");
        if (duplicate) close_station(duplicate);
        check(close_station(first), "close named station");
    }
    first = create_station(L"invalid\\station", 0, station_access, 0);
    check(!first, "backslash rejected");
    if (first) close_station(first);
    for (DWORD i = 0; i < 128; i++) {
        HANDLE temporary = create_station(0, 0, station_access, 0);
        check(temporary && close_station(temporary), "generated station lifetime cycling");
    }
    report(failures ? "USER-OBJECT FAILED\n" : "USER-OBJECT PASS\n");
    ExitProcess(failures ? 1 : 0);
}
