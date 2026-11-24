/* Public Win32 append contract, including independently opened writers. */
typedef unsigned long DWORD;
typedef long LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
#define NIL ((void *)0)
#define INVALID ((HANDLE)(UPTR)-1)
#define APPEND 4U
#define READ 0x80000000U
#define WRITE 0x40000000U
#define SYNCHRONIZE 0x100000U

typedef struct {
    UPTR internal, internal_high;
    DWORD offset, offset_high;
    HANDLE event;
} OVERLAPPED;

IMPORT HANDLE WINAPI CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMPORT BOOL WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, OVERLAPPED *);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, OVERLAPPED *);
IMPORT DWORD WINAPI SetFilePointer(HANDLE, LONG, LONG *, DWORD);
IMPORT DWORD WINAPI GetFileSize(HANDLE, DWORD *);
IMPORT BOOL WINAPI CloseHandle(HANDLE);
IMPORT BOOL WINAPI DeleteFileA(const char *);
IMPORT HANDLE WINAPI GetCurrentProcess(void);
IMPORT BOOL WINAPI DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
IMPORT BOOL WINAPI LockFileEx(HANDLE, DWORD, DWORD, DWORD, DWORD, OVERLAPPED *);
IMPORT BOOL WINAPI UnlockFileEx(HANDLE, DWORD, DWORD, DWORD, OVERLAPPED *);
IMPORT HANDLE WINAPI CreateEventA(void *, BOOL, BOOL, const char *);
IMPORT BOOL WINAPI SetEvent(HANDLE);
IMPORT HANDLE WINAPI CreateThread(void *, SIZE_T, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT void WINAPI ExitProcess(DWORD);

static DWORD failures;
static HANDLE gate;
static const char *path;
typedef struct { DWORD writer, sequence, first, second; } RECORD;
typedef struct { HANDLE file; DWORD writer; volatile DWORD errors; } WRITER;
static WRITER writers[4];
static RECORD contents[128];
static unsigned char seen[4][32];

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
        print("APPEND FAIL "); print(name); print("\r\n");
    }
}

static HANDLE open_file(DWORD access, DWORD disposition)
{
    HANDLE file = CreateFileA(path, access, 3, NIL, disposition, 0x80, NIL);
    check(file != INVALID, "open");
    return file;
}

static void write_text(HANDLE file, const char *text, DWORD length, OVERLAPPED *offset)
{
    DWORD written = 0;
    check(WriteFile(file, text, length, &written, offset) && written == length,
          "write");
}

static void verify(const char *expected, DWORD size)
{
    HANDLE file = open_file(READ, 3);
    if (file == INVALID) return;
    DWORD read = 0;
    char data[64];
    check(GetFileSize(file, NIL) == size, "append size");
    BOOL valid = ReadFile(file, data, sizeof(data), &read, NIL) && read == size;
    for (DWORD i = 0; valid && i < size; i++) valid = data[i] == expected[i];
    check(valid, "append contents");
    CloseHandle(file);
}

static DWORD WINAPI writer_thread(void *argument)
{
    WRITER *writer = argument;
    if (WaitForSingleObject(gate, 10000) != 0) { writer->errors++; return 1; }
    for (DWORD i = 0; i < 32; i++) {
        RECORD record = {writer->writer, i, 0x12340000U | i, 0x98760000U | writer->writer};
        DWORD written;
        if (!WriteFile(writer->file, &record, sizeof(record), &written, NIL) ||
            written != sizeof(record)) writer->errors++;
    }
    return 0;
}

void mainCRTStartup(void)
{
    path = sizeof(void *) == 8 ? "append64.tmp" : "append32.tmp";
    print(sizeof(void *) == 8 ? "APPEND PE64\r\n" : "APPEND PE32\r\n");
    HANDLE source = open_file(READ | WRITE, 2);
    if (source == INVALID) ExitProcess(2);
    write_text(source, "seed\n", 5, NIL);
    HANDLE first = open_file(APPEND, 3), second = open_file(APPEND, 3);
    if (first == INVALID || second == INVALID) ExitProcess(2);
    SetFilePointer(first, 0, NIL, 0);
    write_text(first, "A", 1, NIL);
    write_text(second, "B", 1, NIL);
    OVERLAPPED offset = {0};
    offset.offset = 1;
    write_text(first, "C", 1, &offset);
    verify("seed\nABC", 8);

    HANDLE duplicate = NIL;
    check(DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                          APPEND | SYNCHRONIZE, 0, 0), "duplicate reduced access");
    if (duplicate) {
        SetFilePointer(duplicate, 0, NIL, 0);
        write_text(duplicate, "D", 1, NIL);
        CloseHandle(duplicate);
        verify("seed\nABCD", 9);
    }

    OVERLAPPED lock = {0};
    lock.offset = 9;
    BOOL locked = LockFileEx(source, 3, 0, 1, 0, &lock);
    check(locked, "lock EOF");
    if (locked) {
        DWORD written = 0;
        BOOL ok = WriteFile(first, "X", 1, &written, NIL);
        check(!ok && GetLastError() == 33, "append respects range lock");
        check(UnlockFileEx(source, 0, 1, 0, &lock), "unlock EOF");
    }
    write_text(second, "E", 1, NIL);
    verify("seed\nABCDE", 10);
    SetFilePointer(source, 0, NIL, 0);
    write_text(source, "S", 1, NIL);
    verify("Seed\nABCDE", 10);
    offset.offset = offset.offset_high = 0xFFFFFFFFU;
    write_text(source, "F", 1, &offset);
    verify("Seed\nABCDEF", 11);
    HANDLE read_only = open_file(READ, 3);
    DWORD denied_written = 0;
    BOOL denied = WriteFile(read_only, "X", 1, &denied_written, NIL);
    check(!denied && GetLastError() == 5, "read-only handle cannot write");
    CloseHandle(read_only);
    verify("Seed\nABCDEF", 11);
    CloseHandle(first); CloseHandle(second); CloseHandle(source);

    source = open_file(READ | WRITE, 2);
    if (source == INVALID) ExitProcess(2);
    gate = CreateEventA(NIL, 1, 0, NIL);
    check(gate != NIL, "gate");
    HANDLE threads[4] = {0};
    for (DWORD i = 0; i < 4; i++) {
        writers[i].file = open_file(APPEND, 3);
        writers[i].writer = i;
        threads[i] = CreateThread(NIL, 0, writer_thread, &writers[i], 0, NIL);
        check(threads[i] != NIL, "thread");
    }
    SetEvent(gate);
    for (DWORD i = 0; i < 4; i++) {
        if (threads[i]) {
            if (WaitForSingleObject(threads[i], 60000) != 0) {
                print("APPEND FAIL writer timeout\r\n"); ExitProcess(3);
            }
            CloseHandle(threads[i]);
        }
        check(writers[i].errors == 0, "writer errors");
        CloseHandle(writers[i].file);
    }
    CloseHandle(gate);
    DWORD read = 0;
    check(GetFileSize(source, NIL) == sizeof(contents), "concurrent size");
    check(ReadFile(source, contents, sizeof(contents), &read, NIL) &&
          read == sizeof(contents), "concurrent read");
    BOOL valid = read == sizeof(contents);
    for (DWORD i = 0; valid && i < 128; i++) {
        RECORD *record = &contents[i];
        valid = record->writer < 4 && record->sequence < 32 &&
                record->first == (0x12340000U | record->sequence) &&
                record->second == (0x98760000U | record->writer);
        if (valid) {
            valid = !seen[record->writer][record->sequence];
            seen[record->writer][record->sequence] = 1;
        }
    }
    check(valid, "concurrent unique records");
    CloseHandle(source);
    check(DeleteFileA(path), "delete fixture");
    print(failures ? "APPEND FAILED\r\n" : "APPEND PASS\r\n");
    ExitProcess(failures ? 1 : 0);
}
