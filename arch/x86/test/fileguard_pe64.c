/* QEMU-only regression. Internal SysV entry addresses come from the exact
 * kernel ELF. The actual file handles and child processes use Win32 APIs. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef void *HANDLE;
typedef int BOOL;
#define IMPORT __declspec(dllimport)
#define SYSV __attribute__((sysv_abi))

typedef struct {
    HANDLE process, thread;
    DWORD pid, tid;
} PROCESS_INFORMATION64;

IMPORT void ExitProcess(DWORD);
IMPORT HANDLE GetStdHandle(DWORD);
IMPORT DWORD GetCurrentProcessId(void);
IMPORT char *GetCommandLineA(void);
IMPORT BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMPORT BOOL LockFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
IMPORT BOOL UnlockFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
IMPORT DWORD GetLastError(void);
IMPORT BOOL CloseHandle(HANDLE);
IMPORT void *VirtualAlloc(void *, U64, DWORD, DWORD);
IMPORT BOOL VirtualFree(void *, U64, DWORD);
IMPORT BOOL CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD,
                          void *, const char *, void *, PROCESS_INFORMATION64 *);
IMPORT DWORD WaitForSingleObject(HANDLE, DWORD);
IMPORT BOOL GetExitCodeProcess(HANDLE, DWORD *);

static void print(const char *text)
{
    DWORD size = 0, written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), text, size, &written, (void *)0);
}

static void fail(unsigned code)
{
    char message[] = "FILEGUARD64 FAIL 00\r\n";
    message[16] = (char)('0' + code / 10);
    message[17] = (char)('0' + code % 10);
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static U64 argument(const char **text)
{
    while (**text == ' ') (*text)++;
    U64 value = 0;
    if (!**text) fail(1);
    while (**text && **text != ' ') {
        unsigned c = (unsigned char)*(*text)++;
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
        else fail(1);
        value = value * 16 + c;
    }
    return value;
}

static HANDLE open_file(DWORD disposition)
{
    HANDLE file = CreateFileA("fileguard-contract.tmp", 0xc0000000, 7,
                              (void *)0, disposition, 0x80, (void *)0);
    if (file == (HANDLE)-1) fail(2);
    return file;
}

void mainCRTStartup(void)
{
    const char *text = GetCommandLineA();
    while (*text && *text != ' ') text++;
    U64 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
#ifdef FILEGUARD_CHILD
    unsigned expect_conflict = (unsigned)argument(&text);
    if (argument(&text) == cr3) fail(3);
    HANDLE file = open_file(3);
    if (expect_conflict) {
        print("FILEGUARD64 CHILD DIFFERENT-CR3\r\n");
        if (LockFile(file, 0, 0, 4, 0) || GetLastError() != 33) fail(4);
    } else {
        if (!LockFile(file, 0, 0, 4, 0)) fail(5);
        if (!UnlockFile(file, 0, 0, 4, 0)) fail(6);
    }
    if (!CloseHandle(file)) fail(7);
    print("FILEGUARD64 CHILD PASS\r\n");
    ExitProcess(0);
#else
    void *table = (void *)argument(&text);
    long (SYSV *lookup)(void *, HANDLE, DWORD, int, void **) = (void *)argument(&text);
    long (SYSV *begin)(void *, DWORD, U64, U64, BOOL, void *) = (void *)argument(&text);
    void (SYSV *end)(void *) = (void *)argument(&text);
    HANDLE file = open_file(2);
    DWORD written;
    if (!WriteFile(file, "data", 4, &written, (void *)0) || written != 4) fail(8);
    void *object = (void *)0;
    DWORD pid = GetCurrentProcessId();
    if (lookup(table, file, pid, 1, &object) != 0 || !object) fail(9);
    /* Leave enough room for both the old inline node and the new token. */
    void *guard = VirtualAlloc((void *)0x6000000000ULL, 4096, 0x3000, 4);
    if (guard != (void *)0x6000000000ULL) fail(10);
    if (begin(object, pid, 0, 4, 1, guard) != 0) fail(11);
    print("FILEGUARD64 PRIVATE-GUARD PUBLISHED\r\n");
    for (unsigned stage = 0; stage < 2; stage++) {
        char command[] = "fgchild.exe 1 0000000000000000";
        command[12] = stage == 0 ? '1' : '0';
        U64 value = cr3;
        for (int i = 15; i >= 0; i--) {
            command[14 + i] = "0123456789abcdef"[value & 15];
            value >>= 4;
        }
        unsigned char startup[104] = {0};
        *(DWORD *)startup = sizeof(startup);
        PROCESS_INFORMATION64 child = {0};
        if (!CreateProcessA("fgchild.exe", command, (void *)0, (void *)0,
                            0, 0, (void *)0, (void *)0, startup, &child)) fail(12);
        DWORD status = ~0UL;
        if (WaitForSingleObject(child.process, 15000) != 0 ||
            !GetExitCodeProcess(child.process, &status) || status) fail(13);
        if (!CloseHandle(child.thread) || !CloseHandle(child.process)) fail(14);
        if (!stage) {
            end(guard);
            end(guard);
        }
    }
    /* Exit with one record whose private token was already unmapped. */
    if (begin(object, pid, 0, 4, 1, guard) != 0) fail(15);
    if (!VirtualFree(guard, 0, 0x8000)) fail(16);
    print("FILEGUARD64 PASS\r\n");
    ExitProcess(0);
#endif
}
