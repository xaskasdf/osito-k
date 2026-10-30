/* Public Win32 cache/position/EOF/locking contract and small-read workload. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport) __stdcall
API HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API HANDLE CreateFileW(const unsigned short *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
API DWORD SetFilePointer(HANDLE, long, long *, DWORD);
API BOOL SetEndOfFile(HANDLE);
API BOOL FlushFileBuffers(HANDLE);
API BOOL CloseHandle(HANDLE);
API BOOL LockFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
API BOOL UnlockFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
API DWORD GetLastError(void);
API BOOL DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
API HANDLE GetCurrentProcess(void);
API HANDLE GetStdHandle(DWORD);
API char *GetCommandLineA(void);
API void ExitProcess(unsigned);

#define FILE_BYTES (2U * 1024 * 1024)
static __declspec(align(4096)) unsigned char buffer[65536];
static const char path[] = "filecache-test.bin";
static const unsigned short path_w[] = L"filecache-test.bin";

static void text(const char *s)
{
    DWORD n = 0, written;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &written, 0);
}

static void check(BOOL valid, unsigned code)
{
    if (valid) return;
    char digits[] = "000\n";
    digits[0] += code / 100;
    digits[1] += code / 10 % 10;
    digits[2] += code % 10;
    text("FILE-CACHE FAIL "); text(digits);
    ExitProcess(code);
}

static int mode(const char *name)
{
    const char *cmd = GetCommandLineA();
    while (*cmd) {
        if (*cmd++ != ' ') continue;
        unsigned i = 0;
        while (name[i] && name[i] == cmd[i]) i++;
        if (!name[i] && (!cmd[i] || cmd[i] == ' ')) return 1;
    }
    return 0;
}

static unsigned char expected(unsigned off)
{ return (unsigned char)(off ^ (off >> 8) ^ (off >> 16)); }

static HANDLE open_file(DWORD disposition, DWORD flags, int wide)
{
    HANDLE f = wide ? CreateFileW(path_w, 0xc0000000U, 7, 0, disposition, flags, 0)
                    : CreateFileA(path, 0xc0000000U, 7, 0, disposition, flags, 0);
    check(f && f != (HANDLE)(UPTR)-1, 1);
    return f;
}

static void seek(HANDLE f, unsigned off)
{ check(SetFilePointer(f, (long)off, 0, 0) == off, 2); }

static void read_bytes(HANDLE f, unsigned length, unsigned expected_count)
{
    DWORD count = 0xdead;
    check(ReadFile(f, buffer, length, &count, 0) && count == expected_count, 3);
}

void mainCRTStartup(void)
{
    DWORD count;
    if (mode("prepare")) {
        HANDLE f = open_file(2, 0x80, 0);
        for (unsigned off = 0; off < FILE_BYTES; off += sizeof(buffer)) {
            for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = expected(off + i);
            check(WriteFile(f, buffer, sizeof(buffer), &count, 0) &&
                  count == sizeof(buffer), 4);
        }
        check(FlushFileBuffers(f) && CloseHandle(f), 5);
        text("FILE-CACHE PREPARED\n");
    } else if (mode("contract")) {
        HANDLE f = open_file(3, 0x08000080, 0);
        HANDLE peer = open_file(3, 0x80, 1);
        read_bytes(f, 2048, 2048);
        read_bytes(f, 2048, 2048);
        check(SetFilePointer(f, 0, 0, 1) == 4096, 6);
        seek(peer, 1000);
        unsigned char changed = 0x5a;
        check(WriteFile(peer, &changed, 1, &count, 0) && count == 1, 7);
        seek(f, 0);
        read_bytes(f, 2048, 2048);
        check(buffer[1000] == changed, 8);
        check(LockFile(peer, 0, 0, 512, 0), 9);
        seek(f, 0);
        check(!ReadFile(f, buffer, 512, &count, 0) && GetLastError() == 33, 10);
        check(UnlockFile(peer, 0, 0, 512, 0), 11);
        read_bytes(f, 512, 512);
        seek(peer, 513);
        check(SetEndOfFile(peer), 12);
        seek(f, 0);
        read_bytes(f, 2048, 513);
        for (unsigned i = 0; i < 513; i++) check(buffer[i] == expected(i), 13);
        read_bytes(f, 2048, 0);
        read_bytes(f, 0, 0);
        seek(peer, 65536);
        check(SetEndOfFile(peer), 14);
        seek(f, 511);
        read_bytes(f, 4096, 4096);
        check(buffer[0] == expected(511) && buffer[1] == expected(512), 15);
        for (unsigned i = 2; i < 4096; i++) check(buffer[i] == 0, 16);
        HANDLE duplicate;
        check(DuplicateHandle(GetCurrentProcess(), f, GetCurrentProcess(),
                              &duplicate, 0, 0, 2), 17);
        check(CloseHandle(f), 18);
        check(SetFilePointer(duplicate, 0, 0, 1) == 4607, 19);
        read_bytes(duplicate, 2048, 2048);
        for (unsigned i = 0; i < 2048; i++) check(buffer[i] == 0, 20);
        check(CloseHandle(duplicate) && CloseHandle(peer), 21);
        f = open_file(3, 0x80, 0);
        read_bytes(f, 2048, 2048);
        check(buffer[1000] == 0 && CloseHandle(f), 22);
        text("FILE-CACHE CONTRACT PASS\n");
    } else {
        DWORD flags = 0x80;
        if (mode("unbuffered")) flags |= 0x20000000;
        if (mode("random")) flags |= 0x10000000;
        HANDLE f = open_file(3, flags, mode("wide"));
        for (unsigned off = 0; off < FILE_BYTES; off += 2048) {
            read_bytes(f, 2048, 2048);
            for (unsigned i = 0; i < 2048; i++) check(buffer[i] == expected(off + i), 23);
        }
        read_bytes(f, 2048, 0);
        if (!mode("exit-open")) check(CloseHandle(f), 24);
        text("FILE-CACHE STREAM PASS\n");
    }
    ExitProcess(0);
}
