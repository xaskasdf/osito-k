/* Compare CRT path metadata with the live file descriptor and Win32 times. */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef unsigned long long U64;
typedef long long I64;
typedef __INTPTR_TYPE__ INTPTR;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API const char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API BOOL CALL CloseHandle(HANDLE);
API BOOL CALL DeleteFileA(const char *);
API DWORD CALL GetLastError(void);
API void CALL ExitProcess(DWORD);
typedef struct { DWORD low, high; } FILETIME;
API BOOL CALL SetFileTime(HANDLE, const FILETIME *, const FILETIME *, const FILETIME *);

typedef struct {
    DWORD dev;
    WORD ino, mode;
    short nlink, uid, gid;
    DWORD rdev;
} STAT_HEAD;
typedef struct { STAT_HEAD h; int size, access, write, creation; } STAT32;
typedef struct { STAT_HEAD h; I64 size; int access, write, creation; } STAT32I64;
typedef struct { STAT_HEAD h; int size; I64 access, write, creation; } STAT64I32;
typedef struct { STAT_HEAD h; I64 size, access, write, creation; } STAT64;
_Static_assert(sizeof(STAT32) == 36, "stat32 ABI");
_Static_assert(sizeof(STAT32I64) == 48, "stat32i64 ABI");
_Static_assert(sizeof(STAT64I32) == 48, "stat64i32 ABI");
_Static_assert(sizeof(STAT64) == 56, "stat64 ABI");
typedef union { STAT32 s32; STAT32I64 s32i64; STAT64I32 s64i32; STAT64 s64; BYTE raw[64]; } STAT_BUFFER;
typedef struct { U64 before; STAT_BUFFER value; U64 after; } GUARDED_STAT;
typedef int (*STAT_QUERY)(const void *, void *);
typedef int (*FSTAT_QUERY)(int, void *);
typedef int (*OPEN_HANDLE)(INTPTR, int);
typedef int (*CLOSE_FD)(int);
typedef int (*CHMOD)(const char *, int);

static unsigned checks, failures;
static void print(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}
static void number(unsigned n)
{
    char digits[12]; unsigned count = 0;
    do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
    while (count) { char one[2] = {digits[--count], 0}; print(one); }
}
static void check(BOOL ok, const char *name)
{
    checks++;
    if (ok) return;
    failures++;
    print("FAIL: "); print(name); print("\n");
}
static FILETIME from_unix(U64 seconds)
{
    U64 ticks = 116444736000000000ULL + seconds * 10000000ULL;
    FILETIME result = {(DWORD)ticks, (DWORD)(ticks >> 32)};
    return result;
}
static BOOL set_times(HANDLE file, U64 creation, U64 access, U64 write)
{
    FILETIME c = from_unix(creation), a = from_unix(access), w = from_unix(write);
    return SetFileTime(file, &c, &a, &w);
}
static void prepare(GUARDED_STAT *buffer)
{
    BYTE *raw = (BYTE *)buffer;
    for (SIZE_T i = 0; i < sizeof(*buffer); i++) raw[i] = 0xa5;
}
static void inspect(const GUARDED_STAT *buffer, unsigned layout,
                    U64 creation, U64 access, U64 write, const char *name)
{
    I64 c, a, w, size;
    unsigned bytes;
    switch (layout) {
    case 0:
        bytes = sizeof(STAT32); size = buffer->value.s32.size;
        c = buffer->value.s32.creation; a = buffer->value.s32.access; w = buffer->value.s32.write;
        break;
    case 1:
        bytes = sizeof(STAT32I64); size = buffer->value.s32i64.size;
        c = buffer->value.s32i64.creation; a = buffer->value.s32i64.access; w = buffer->value.s32i64.write;
        break;
    case 2:
        bytes = sizeof(STAT64I32); size = buffer->value.s64i32.size;
        c = buffer->value.s64i32.creation; a = buffer->value.s64i32.access; w = buffer->value.s64i32.write;
        break;
    default:
        bytes = sizeof(STAT64); size = buffer->value.s64.size;
        c = buffer->value.s64.creation; a = buffer->value.s64.access; w = buffer->value.s64.write;
        break;
    }
    BOOL guards = buffer->before == 0xa5a5a5a5a5a5a5a5ULL &&
                  buffer->after == 0xa5a5a5a5a5a5a5a5ULL;
    for (unsigned i = bytes; i < sizeof(buffer->value); i++)
        if (buffer->value.raw[i] != 0xa5) guards = 0;
    check(guards, "stat output bounds");
    check(size == 6, "stat file length");
    check((U64)c == creation && (U64)a == access && (U64)w == write, name);
    if ((U64)c != creation || (U64)a != access || (U64)w != write) {
        print("  creation low="); number((DWORD)c); print(" high="); number((DWORD)((U64)c >> 32));
        print(" access low="); number((DWORD)a); print(" high="); number((DWORD)((U64)a >> 32));
        print(" write low="); number((DWORD)w); print(" high="); number((DWORD)((U64)w >> 32)); print("\n");
    }
}

void mainCRTStartup(void)
{
    BOOL ucrt = 0, far = 0;
    const char *command = GetCommandLineA();
    for (; command && *command; command++) {
        if (command[0] == 'u' && command[1] == 'c' && command[2] == 'r' && command[3] == 't') ucrt = 1;
        if (command[0] == 'f' && command[1] == 'a' && command[2] == 'r') far = 1;
    }
    HANDLE module = LoadLibraryA(ucrt ? "ucrtbase.dll" : "msvcrt.dll");
    OPEN_HANDLE open_handle = (OPEN_HANDLE)GetProcAddress(module, "_open_osfhandle");
    CLOSE_FD close_fd = (CLOSE_FD)GetProcAddress(module, "_close");
    FSTAT_QUERY fstat_query = (FSTAT_QUERY)GetProcAddress(module, "_fstat64");
    CHMOD chmod_file = (CHMOD)GetProcAddress(module, "_chmod");
    const char *names[8] = {"_stat32", "_stat32i64", "_stat64i32", "_stat64",
                            "_wstat32", "_wstat32i64", "_wstat64i32", "_wstat64"};
    STAT_QUERY query[8];
    BOOL exports = module && open_handle && close_fd && fstat_query && chmod_file;
    for (unsigned i = 0; i < 8; i++) {
        query[i] = (STAT_QUERY)GetProcAddress(module, names[i]);
        if (ucrt || (i % 4) == 3) exports = exports && query[i];
    }
    check(exports, "required stat exports");
    if (!exports) ExitProcess(1);

    static const char name[] = "crt-stat-contract.tmp";
    static const WORD wide_name[] = {'c','r','t','-','s','t','a','t','-','c','o','n','t','r','a','c','t','.','t','m','p',0};
    HANDLE file = CreateFileA(name, 0xc0000000U, 3, 0, 1 /* CREATE_NEW */, 0x80, 0);
    check(file != (HANDLE)(INTPTR)-1, "create owned test file");
    if (file == (HANDLE)(INTPTR)-1) ExitProcess(1);
    DWORD written;
    check(WriteFile(file, "abcdef", 6, &written, 0) && written == 6, "write fixture");
    int fd = open_handle((INTPTR)file, 2 | 0x8000);
    check(fd >= 0, "adopt file descriptor");
    if (fd < 0) { CloseHandle(file); DeleteFileA(name); ExitProcess(1); }

    for (unsigned phase = 0; phase < (far ? 3U : 2U); phase++) {
        U64 creation = phase == 2 ? 0x100001234ULL : 1704067200ULL + phase * 100U;
        U64 access = creation + 86400, write = creation + 172800;
        BOOL set = set_times(file, creation, access, write);
        check(set, "set distinct timestamps");
        if (!set) continue;
        check(chmod_file(name, 0x180) == 0, "mode update preserves file creation");

        GUARDED_STAT result;
        prepare(&result);
        int rc = fstat_query(fd, &result.value);
        check(rc == 0, "fstat64 query");
        if (!rc) inspect(&result, 3, creation, access, write, "fstat64 timestamps");
        for (unsigned i = 0; i < 8; i++) {
            unsigned layout = i % 4;
            if (!query[i] || (phase == 2 && layout < 2)) continue;
            prepare(&result);
            rc = query[i](i < 4 ? (const void *)name : (const void *)wide_name, &result.value);
            check(rc == 0, names[i]);
            if (!rc) inspect(&result, layout, creation, access, write, names[i]);
        }
    }
    check(close_fd(fd) == 0, "close owned descriptor");
    check(DeleteFileA(name), "remove owned fixture");
    print("CRT-STAT "); number(checks); print(" checks, "); number(failures); print(" failures\n");
    ExitProcess(failures ? 1 : 0);
}
