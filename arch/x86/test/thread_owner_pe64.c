/* Process teardown must distinguish identical PEB VAs in different processes. */
typedef unsigned long DWORD;
typedef unsigned long long SIZE_T;
typedef int BOOL;
typedef void *HANDLE;
#define IMPORT __declspec(dllimport)

typedef struct {
    HANDLE process, thread;
    DWORD process_id, thread_id;
} PROCESS_INFORMATION64;

IMPORT void ExitProcess(DWORD code);
IMPORT HANDLE GetStdHandle(DWORD which);
IMPORT BOOL WriteFile(HANDLE file, const void *data, DWORD size,
                      DWORD *written, void *overlapped);
IMPORT HANDLE CreateThread(void *, SIZE_T, DWORD (*)(void *), void *, DWORD, DWORD *);
IMPORT HANDLE CreateEventA(void *, BOOL, BOOL, const char *);
IMPORT BOOL SetEvent(HANDLE);
IMPORT void Sleep(DWORD);
IMPORT DWORD WaitForSingleObject(HANDLE, DWORD);
IMPORT BOOL GetExitCodeThread(HANDLE, DWORD *);
IMPORT BOOL GetExitCodeProcess(HANDLE, DWORD *);
IMPORT BOOL CloseHandle(HANDLE);
IMPORT char *GetCommandLineA(void);
IMPORT DWORD GetCurrentProcessId(void);
IMPORT void *VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
IMPORT BOOL VirtualFree(void *, SIZE_T, DWORD);
IMPORT BOOL CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD,
                          void *, const char *, void *, PROCESS_INFORMATION64 *);

static void print(const char *text)
{
    DWORD length = 0, written = 0;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, (void *)0);
}

static void fail(DWORD step)
{
    char text[] = "THREAD-OWNER64 FAIL 00\r\n";
    text[21] = (char)('0' + step % 10);
    text[20] = (char)('0' + step / 10);
    print(text);
    ExitProcess(step);
    for (;;) { }
}

static SIZE_T peb_address(void)
{
    SIZE_T address;
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(address));
    return address;
}

static DWORD worker(void *event)
{
    return WaitForSingleObject(event, 15000) == 0 ? 77 : 78;
}

typedef struct {
    DWORD process_id;
    unsigned char *page;
} OWNER_PROBE;

static DWORD context_worker(void *argument)
{
    OWNER_PROBE *probe = argument;
    if (GetCurrentProcessId() != probe->process_id) return 11;
    if (!VirtualFree(probe->page, 4096, 0x4000)) return 12;
    if (VirtualAlloc(probe->page, 4096, 0x1000, 4) != probe->page) return 13;
    for (unsigned i = 0; i < 4096; i++)
        if (probe->page[i]) return 14;
    return VirtualFree(probe->page, 0, 0x8000) ? 0 : 15;
}

static BOOL start_child(char role, PROCESS_INFORMATION64 *child)
{
    char command[] = "ownchild.exe 0000000000000000 x";
    SIZE_T peb = peb_address();
    for (int i = 15; i >= 0; i--) {
        command[13 + i] = "0123456789abcdef"[peb & 15];
        peb >>= 4;
    }
    command[30] = role;
    unsigned char startup[104] = {0};
    *(DWORD *)startup = sizeof(startup);
    return CreateProcessA("ownchild.exe", command, (void *)0, (void *)0,
                          0, 0, (void *)0, (void *)0, startup, child);
}

void mainCRTStartup(void)
{
#ifdef OWNER_CHILD
    const char *arg = GetCommandLineA();
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    SIZE_T parent_peb = 0;
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = (unsigned char)arg[i];
        if (value >= '0' && value <= '9') value -= '0';
        else if (value >= 'a' && value <= 'f') value = value - 'a' + 10;
        else fail(1);
        parent_peb = (parent_peb << 4) | value;
    }
    if (parent_peb != peb_address()) fail(2);
    if (arg[16] == ' ' && arg[17] == 'h') {
        HANDLE ready = CreateEventA((void *)0, 1, 0, "owner-context-ready");
        HANDLE done = CreateEventA((void *)0, 1, 0, "owner-context-done");
        if (!ready || !done || !SetEvent(ready) ||
            WaitForSingleObject(done, 20000) != 0) fail(16);
        CloseHandle(ready);
        CloseHandle(done);
        ExitProcess(0);
    }
    if (arg[16] == ' ' && arg[17] == 'p') {
        OWNER_PROBE probe = { GetCurrentProcessId(),
            VirtualAlloc((void *)0, 4096, 0x3000, 4) };
        if (!probe.page) fail(17);
        for (unsigned i = 0; i < 4096; i++) probe.page[i] = 0x5a;
        HANDLE thread = CreateThread((void *)0, 0, context_worker, &probe,
                                     0, (void *)0);
        DWORD status = 0xffffffffU;
        if (!thread || WaitForSingleObject(thread, 10000) != 0 ||
            !GetExitCodeThread(thread, &status)) fail(18);
        if (status) fail(status);
        CloseHandle(thread);
        print("THREAD-OWNER64 WORKER-CONTEXT PASS\r\n");
        ExitProcess(0);
    }
    HANDLE event = CreateEventA((void *)0, 1, 0, (void *)0);
    HANDLE thread = event ? CreateThread((void *)0, 0, worker, event, 0, (void *)0) : (void *)0;
    if (!thread) fail(3);
    Sleep(50);
    print("THREAD-OWNER64 CHILD SAME-PEB\r\n");
    ExitProcess(0);
#else
    HANDLE event = CreateEventA((void *)0, 1, 0, (void *)0);
    HANDLE thread = event ? CreateThread((void *)0, 0, worker, event, 0, (void *)0) : (void *)0;
    if (!thread) fail(4);
    Sleep(50);
    char command[] = "ownchild.exe 0000000000000000";
    SIZE_T peb = peb_address();
    for (int i = 15; i >= 0; i--) {
        command[13 + i] = "0123456789abcdef"[peb & 15];
        peb >>= 4;
    }
    unsigned char startup[104] = {0};
    *(DWORD *)startup = sizeof(startup);
    PROCESS_INFORMATION64 child = {0};
    if (!CreateProcessA("ownchild.exe", command, (void *)0, (void *)0,
                        0, 0, (void *)0, (void *)0, startup, &child)) fail(5);
    DWORD status = 0xffffffffU;
    if (WaitForSingleObject(child.process, 10000) != 0 ||
        !GetExitCodeProcess(child.process, &status) || status != 0) fail(6);
    if (!GetExitCodeThread(thread, &status) || status != 259) fail(7);
    if (!SetEvent(event) || WaitForSingleObject(thread, 5000) != 0 ||
        !GetExitCodeThread(thread, &status) || status != 77) fail(8);
    if (!CloseHandle(child.thread) || !CloseHandle(child.process) ||
        !CloseHandle(thread) || !CloseHandle(event)) fail(9);
    HANDLE ready = CreateEventA((void *)0, 1, 0, "owner-context-ready");
    HANDLE done = CreateEventA((void *)0, 1, 0, "owner-context-done");
    PROCESS_INFORMATION64 hold = {0}, probe = {0};
    if (!ready || !done || !start_child('h', &hold) ||
        WaitForSingleObject(ready, 10000) != 0 ||
        !start_child('p', &probe)) fail(19);
    if (WaitForSingleObject(probe.process, 10000) != 0 ||
        !GetExitCodeProcess(probe.process, &status)) fail(20);
    DWORD probe_status = status;
    if (!SetEvent(done) || WaitForSingleObject(hold.process, 10000) != 0 ||
        !GetExitCodeProcess(hold.process, &status) || status) fail(21);
    CloseHandle(hold.thread);
    CloseHandle(hold.process);
    CloseHandle(probe.thread);
    CloseHandle(probe.process);
    CloseHandle(ready);
    CloseHandle(done);
    if (probe_status) fail(probe_status);
    print("THREAD-OWNER64 PASS\r\n");
    ExitProcess(0);
#endif
}
