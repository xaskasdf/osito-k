/* Bounded child wait for authenticated inspection of a private PE stack. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
typedef struct {
    DWORD cb;
    char *reserved, *desktop, *title;
    DWORD x, y, width, height, columns, rows, fill, flags;
    unsigned short show, reserved_size;
    void *reserved_bytes;
    HANDLE input, output, error;
} STARTUPINFO;
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
API HANDLE CALL CreateEventA(void *, BOOL, BOOL, const char *);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
API BOOL CALL CloseHandle(HANDLE);

static void report(const char *text)
{
    DWORD count = 0, written;
    while (text[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11), text, count, &written, 0);
}

static void hex(UPTR value)
{
    char text[2 + sizeof(UPTR) * 2 + 1];
    text[0] = '0'; text[1] = 'x';
    for (unsigned i = 0; i < sizeof(UPTR) * 2; i++)
        text[2 + i] = "0123456789ABCDEF"[(value >> ((sizeof(UPTR) * 2 - i - 1) * 4)) & 15];
    text[sizeof(text) - 1] = 0;
    report(text);
}

__declspec(noinline) static DWORD stack_hold(HANDLE event)
{
    volatile DWORD result = WaitForSingleObject(event, 15000);
    return result;
}

void mainCRTStartup(void)
{
    const char *command = GetCommandLineA(), *flag = "--stack-child";
    BOOL child = 0;
    for (DWORD i = 0; command[i]; i++) {
        DWORD length = 0;
        while (flag[length] && command[i + length] == flag[length]) length++;
        if (!flag[length]) child = 1;
    }
    if (child) {
        HANDLE event = CreateEventA(0, 0, 0, 0);
        if (!event) ExitProcess(3);
        DWORD result = stack_hold(event);
        CloseHandle(event);
        ExitProcess(result == 258 ? 0 : 4);
    }
    char path[512], args[560];
    DWORD length = GetModuleFileNameA(0, path, sizeof(path));
    if (!length || length >= sizeof(path)) ExitProcess(5);
    args[0] = '"';
    for (DWORD i = 0; i < length; i++) args[i + 1] = path[i];
    const char *suffix = "\" --stack-child";
    DWORD used = length + 1;
    for (DWORD i = 0; suffix[i]; i++) args[used++] = suffix[i];
    args[used] = 0;
    static PROCESS_INFORMATION info;
    static STARTUPINFO startup;
    startup.cb = sizeof(startup);
    if (!CreateProcessA(path, args, 0, 0, 0, 0x08000000, 0, 0, &startup, &info)) ExitProcess(6);
    report("STACK-PROBE CHILD pid="); hex(info.pid);
    report(" tid="); hex(info.tid);
    report(" hold="); hex((UPTR)stack_hold); report("\n");
    DWORD exit_code = 0xFFFFFFFF;
    DWORD result = WaitForSingleObject(info.process, 20000);
    BOOL ok = result == 0 && GetExitCodeProcess(info.process, &exit_code) && exit_code == 0;
    CloseHandle(info.thread); CloseHandle(info.process);
    report(ok ? "STACK-PROBE DONE\n" : "STACK-PROBE FAIL\n");
    ExitProcess(ok ? 0 : 7);
}
