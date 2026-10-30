/* Public file-position contract used by Chromium's overlapped cache files. */
typedef unsigned long DWORD;
typedef long LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
typedef union { struct { DWORD low; LONG high; }; long long value; } LARGE;
typedef struct { UPTR internal, count; DWORD offset, high; HANDLE event; } OVERLAPPED;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
#define NIL ((void *)0)
#define INVALID ((HANDLE)(UPTR)-1)
#define READ 0x80000000U
#define WRITE 0x40000000U

IMPORT HANDLE WINAPI CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMPORT BOOL WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, OVERLAPPED *);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, OVERLAPPED *);
IMPORT DWORD WINAPI SetFilePointer(HANDLE, LONG, LONG *, DWORD);
IMPORT BOOL WINAPI SetFilePointerEx(HANDLE, LARGE, LARGE *, DWORD);
IMPORT BOOL WINAPI SetEndOfFile(HANDLE);
IMPORT DWORD WINAPI GetFileSize(HANDLE, DWORD *);
IMPORT BOOL WINAPI CloseHandle(HANDLE);
IMPORT BOOL WINAPI DeleteFileA(const char *);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT BOOL WINAPI DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
IMPORT HANDLE WINAPI CreateEventA(void *, BOOL, BOOL, const char *);
IMPORT BOOL WINAPI GetOverlappedResult(HANDLE, OVERLAPPED *, DWORD *, BOOL);
IMPORT HANDLE WINAPI CreateFileMappingA(HANDLE, void *, DWORD, DWORD, DWORD, const char *);
IMPORT void *WINAPI MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
IMPORT BOOL WINAPI FlushViewOfFile(const void *, SIZE_T);
IMPORT BOOL WINAPI UnmapViewOfFile(const void *);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT void WINAPI ExitProcess(DWORD);

static DWORD failures;

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void check(BOOL ok, const char *name)
{
    if (!ok) {
        failures++;
        print("FILE-POSITION FAIL "); print(name); print("\r\n");
    }
}

static void explicit_io(HANDLE file, BOOL write, unsigned char *byte)
{
    OVERLAPPED offset = {0};
    offset.offset = 64;
    offset.event = CreateEventA(NIL, 1, 0, NIL);
    check(offset.event != NIL, "overlapped event");
    DWORD done = 0;
    BOOL ok = write ? WriteFile(file, byte, 1, &done, &offset)
                    : ReadFile(file, byte, 1, &done, &offset);
    if (!ok && GetLastError() == 997)
        ok = GetOverlappedResult(file, &offset, &done, 1);
    check(ok && done == 1, write ? "explicit write" : "explicit read");
    check(SetFilePointer(file, 0, NIL, 1) == 37, "explicit I/O preserves cursor");
    if (offset.event) CloseHandle(offset.event);
}

void mainCRTStartup(void)
{
    const char *path = sizeof(void *) == 8 ? "position64.tmp" : "position32.tmp";
    print(sizeof(void *) == 8 ? "FILE-POSITION PE64\r\n" : "FILE-POSITION PE32\r\n");
    HANDLE file = CreateFileA(path, READ | WRITE, 7, NIL, 2, 0x40000080, NIL);
    check(file != INVALID, "open overlapped file");
    if (file == INVALID) ExitProcess(2);
    check(SetFilePointer(file, 8192, NIL, 0) == 8192, "seek overlapped handle");
    check(GetFileSize(file, NIL) == 0, "seek alone does not extend");
    check(SetEndOfFile(file), "extend overlapped file");
    check(GetFileSize(file, NIL) == 8192, "extended length");

    HANDLE other = CreateFileA(path, READ | WRITE, 7, NIL, 3, 0x80, NIL);
    check(other != INVALID, "open independent file");
    HANDLE mapping = CreateFileMappingA(file, NIL, 4, 0, 0, NIL);
    check(mapping != NIL, "map extended cache file");
    if (mapping) {
        unsigned char *view = MapViewOfFile(mapping, 6, 0, 0, 8192);
        check(view != NIL, "view extended cache file");
        if (view) {
            check(view[0] == 0 && view[8191] == 0, "zero-filled extension");
            view[256] = 0xA5;
            check(FlushViewOfFile(view, 0), "flush cache view");
            check(UnmapViewOfFile(view), "unmap cache view");
        }
        CloseHandle(mapping);
    }
    if (other != INVALID) {
        unsigned char byte = 0;
        DWORD done = 0;
        check(SetFilePointer(other, 256, NIL, 0) == 256, "seek independent file");
        check(ReadFile(other, &byte, 1, &done, NIL) && done == 1 && byte == 0xA5,
              "mapped data visible to independent read");
    }

    check(SetFilePointer(file, 37, NIL, 0) == 37, "set independent async cursor");
    HANDLE duplicate = NIL;
    check(DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate,
                          0, 0, 2), "duplicate overlapped handle");
    if (duplicate) {
        check(SetFilePointer(duplicate, 0, NIL, 1) == 37, "duplicate shares cursor");
        CloseHandle(duplicate);
    }
    if (other != INVALID)
        check(SetFilePointer(other, 0, NIL, 1) == 257, "independent cursor unchanged");
    unsigned char byte = 0x5A;
    explicit_io(file, 1, &byte);
    byte = 0;
    explicit_io(file, 0, &byte);
    check(byte == 0x5A, "explicit offset content");
    check(SetFilePointer(file, -1, NIL, 0) == 0xFFFFFFFFU && GetLastError() == 131,
          "negative seek rejected");
    check(SetFilePointer(file, 0, NIL, 1) == 37, "failed seek preserves cursor");

    LARGE distance, position;
    distance.value = 4096;
    check(SetFilePointerEx(file, distance, &position, 2) && position.value == 12288,
          "seek from EOF");
    check(GetFileSize(file, NIL) == 8192, "seek from EOF does not extend");
    check(SetEndOfFile(file) && GetFileSize(file, NIL) == 12288, "extend from explicit cursor");
    distance.value = -8192;
    check(SetFilePointerEx(file, distance, &position, 1) && position.value == 4096,
          "relative backward seek");
    check(SetEndOfFile(file) && GetFileSize(file, NIL) == 4096, "truncate from explicit cursor");
    distance.value = 0x100000010LL;
    check(SetFilePointerEx(file, distance, &position, 0) && position.value == distance.value,
          "64-bit cursor without allocation");
    check(GetFileSize(file, NIL) == 4096, "large seek does not allocate");
    if (other != INVALID) CloseHandle(other);
    CloseHandle(file);
    check(DeleteFileA(path), "cleanup");
    print(failures ? "FILE-POSITION FAILED\r\n" : "FILE-POSITION PASS\r\n");
    ExitProcess(failures ? 1 : 0);
}
