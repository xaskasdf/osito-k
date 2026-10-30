/* Public Win32 file contract and read timings; run only on a test filesystem. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport) __stdcall
API HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
API DWORD GetFileSize(HANDLE, DWORD *);
API DWORD SetFilePointer(HANDLE, long, long *, DWORD);
API BOOL FlushFileBuffers(HANDLE);
API BOOL CloseHandle(HANDLE);
API HANDLE GetStdHandle(DWORD);
API BOOL QueryPerformanceCounter(U64 *);
API BOOL QueryPerformanceFrequency(U64 *);
API void ExitProcess(unsigned);

#define FILE_BYTES (2U * 1024 * 1024)
static unsigned char buffer[65536];
static char line[240];
static unsigned line_size;

static void text(const char *s)
{ while (*s && line_size + 1 < sizeof(line)) line[line_size++] = *s++; }
static void number(DWORD value)
{
    char digits[10];
    unsigned n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) line[line_size++] = digits[--n];
}
static void hex(U64 value)
{
    text("0x");
    for (int i = 60; i >= 0; i -= 4)
        line[line_size++] = "0123456789abcdef"[(unsigned)(value >> i) & 15];
}
static void emit(void)
{
    DWORD written;
    text("\n");
    WriteFile(GetStdHandle((DWORD)-11), line, line_size, &written, 0);
    line_size = 0;
}
static void check(BOOL valid, unsigned code)
{
    if (valid) return;
    line_size = 0; text("USB-READ FAIL code="); number(code); emit();
    ExitProcess(code);
}
static unsigned char expected(unsigned offset)
{ return (unsigned char)(offset ^ (offset >> 8) ^ (offset >> 16)); }

void mainCRTStartup(void)
{
    U64 frequency;
    check(QueryPerformanceFrequency(&frequency) && frequency, 1);
    HANDLE file = CreateFileA("usb-read-batch-fixture.bin", 0xc0000000U,
                              0, 0, 4, 0x80, 0);
    check(file && file != (HANDLE)(UPTR)-1, 2);
    DWORD high = 0, size = GetFileSize(file, &high);
    check(!high && (size == 0 || size == FILE_BYTES), 3);
    DWORD count;
    if (!size) {
        for (unsigned off = 0; off < FILE_BYTES; off += sizeof(buffer)) {
            for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = expected(off + i);
            check(WriteFile(file, buffer, sizeof(buffer), &count, 0) &&
                  count == sizeof(buffer), 4);
        }
        check(FlushFileBuffers(file), 5);
    }
    check(GetFileSize(file, &high) == FILE_BYTES && !high, 6);
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned mode = 0; mode < 2; mode++) {
            DWORD chunk = mode ? 65536 : 8192;
            U64 elapsed = 0;
            check(SetFilePointer(file, 0, 0, 0) == 0, 7);
            for (unsigned off = 0; off < FILE_BYTES; off += chunk) {
                U64 start, end;
                check(QueryPerformanceCounter(&start), 8);
                check(ReadFile(file, buffer, chunk, &count, 0) && count == chunk, 9);
                check(QueryPerformanceCounter(&end) && end >= start, 10);
                elapsed += end - start;
                for (unsigned i = 0; i < count; i++)
                    check(buffer[i] == expected(off + i), 11);
            }
            check(ReadFile(file, buffer, chunk, &count, 0) && !count, 12);
            text("USB-READ PASS round="); number(round);
            text(" chunk="); number(chunk);
            text(" bytes="); number(FILE_BYTES);
            text(" ticks="); hex(elapsed);
            text(" frequency="); hex(frequency); emit();
        }
    }
    check(SetFilePointer(file, 511, 0, 0) == 511, 13);
    check(ReadFile(file, buffer, sizeof(buffer), &count, 0) && count == sizeof(buffer), 14);
    for (unsigned i = 0; i < count; i++) check(buffer[i] == expected(511 + i), 15);
    check(SetFilePointer(file, FILE_BYTES - 513, 0, 0) == FILE_BYTES - 513, 16);
    check(ReadFile(file, buffer, sizeof(buffer), &count, 0) && count == 513, 17);
    for (unsigned i = 0; i < count; i++) check(buffer[i] == expected(FILE_BYTES - 513 + i), 18);
    check(CloseHandle(file), 19);
    text("USB-READ ALL PASS"); emit();
    ExitProcess(0);
}
