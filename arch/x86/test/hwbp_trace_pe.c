/* Execution-breakpoint return, scalar width and private-CR3 scope fixture. */
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
typedef struct {
    DWORD cb;
    char *reserved, *desktop, *title;
    DWORD x, y, width, height, chars_x, chars_y, fill, flags;
    WORD show, reserved_size;
    void *reserved_bytes;
    HANDLE input, output, error;
} STARTUPINFO;
API void CALL ExitProcess(DWORD);
API void CALL Sleep(DWORD);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, STARTUPINFO *, PROCESS_INFORMATION *);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
API BOOL CALL CloseHandle(HANDLE);

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static BOOL option(const char *text)
{
    const char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++) {
        if (i && args[i - 1] != ' ') continue;
        DWORD n = 0;
        while (text[n] && args[i + n] == text[n]) n++;
        if (!text[n] && (!args[i + n] || args[i + n] == ' ')) return 1;
    }
    return 0;
}

static __attribute__((noinline)) void sample(void)
{
    __asm__ volatile (
#ifdef _WIN64
        "movabs $0xfedcba98140000a5, %%rdi\n"
        "movabs $0xabcdef0012345678, %%rax\n"
#else
        "movl $0x140000a5, %%edi\n"
        "movl $0x12345678, %%eax\n"
#endif
        ".globl hwbp_trace_point\n"
        "hwbp_trace_point:\n"
        ".rept 16\n"
        "nop\n"
        ".endr\n"
        ::: "edi", "eax", "memory"
    );
}

static void private_child(void)
{
    char path[512], command[540];
    STARTUPINFO startup = {0};
    PROCESS_INFORMATION child = {0};
    DWORD size = GetModuleFileNameA(NIL, path, sizeof(path));
    if (!size || size >= sizeof(path)) goto fail;
    DWORD p = 0;
    command[p++] = '"';
    for (DWORD i = 0; i < size; i++) command[p++] = path[i];
    command[p++] = '"';
    const char suffix[] = " --child";
    for (DWORD i = 0; i < sizeof(suffix); i++) command[p++] = suffix[i];
    startup.cb = sizeof(startup);
    if (!CreateProcessA(path, command, NIL, NIL, 0, 0, NIL, NIL, &startup, &child))
        goto fail;
    if (WaitForSingleObject(child.process, 10000) != 0) goto fail;
    DWORD status;
    if (!GetExitCodeProcess(child.process, &status) || status) goto fail;
    CloseHandle(child.thread);
    CloseHandle(child.process);
    print("HWBP-FIXTURE CHILD DONE\r\n");
    return;
fail:
    print("HWBP-FIXTURE FAIL child\r\n");
    ExitProcess(1);
}

void mainCRTStartup(void)
{
    if (option("--child")) {
        for (unsigned i = 0; i < 3; i++) sample();
        ExitProcess(0);
    }
    if (option("--scope")) private_child();
    print("HWBP-FIXTURE READY\r\n");
    Sleep(5000);
    for (unsigned i = 0; i < 3; i++) {
        sample();
        Sleep(100);
    }
    print("HWBP-FIXTURE DONE\r\n");
    ExitProcess(0);
}
