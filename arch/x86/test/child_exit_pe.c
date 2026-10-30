/* Terminating a child must not strand a host heap lock used by other PEs. */
typedef unsigned long DWORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
API void CALL ExitProcess(DWORD);
API DWORD CALL GetLastError(void);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
API BOOL CALL TerminateProcess(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
API HANDLE CALL CreateThread(void *, SIZE_T, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
API HANDLE CALL CreateEventA(void *, BOOL, BOOL, const char *);
API HANDLE CALL OpenEventA(DWORD, BOOL, const char *);
API BOOL CALL SetEvent(HANDLE);
API BOOL CALL ResetEvent(HANDLE);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL CloseHandle(HANDLE);
API void CALL Sleep(DWORD);
API HANDLE CALL GetProcessHeap(void);
API BOOL CALL HeapLock(HANDLE);
API BOOL CALL HeapUnlock(HANDLE);
API void *CALL HeapAlloc(HANDLE, DWORD, SIZE_T);
API BOOL CALL HeapFree(HANDLE, DWORD, void *);
API void CALL InitializeCriticalSection(void *);
API void CALL EnterCriticalSection(void *);
static HANDLE trigger, started;
static volatile DWORD allocated;
static unsigned long long child_critical_section[5];
static volatile DWORD critical_waiters_ready;
static const char ready_name[] = "Local\\OsitoChildExitReady";

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void require(BOOL ok, const char *stage)
{
    if (ok) return;
    print("CHILD-EXIT FAIL "); print(stage); print("\r\n");
    ExitProcess(1);
}

static DWORD CALL worker(void *argument)
{
    require(SetEvent(started), "worker started");
    require(WaitForSingleObject(trigger, 10000) == 0, "worker trigger");
    if (argument) ExitProcess(83);
    void *memory = HeapAlloc(GetProcessHeap(), 8, 4096);
    allocated = memory && HeapFree(GetProcessHeap(), 0, memory);
    return 0;
}

static HANDLE begin_worker(BOOL exits)
{
    started = CreateEventA(NIL, 1, 0, NIL);
    trigger = CreateEventA(NIL, 1, 0, NIL);
    require(started && trigger, "worker events");
    HANDLE thread = CreateThread(NIL, 0, worker, exits ? (void *)1 : NIL, 0, NIL);
    require(thread && WaitForSingleObject(started, 10000) == 0, "worker startup");
    return thread;
}

static DWORD CALL locked_worker(void *argument)
{
    HANDLE heap = GetProcessHeap();
    require(HeapLock(heap), "worker heap lock");
    require(SetEvent(argument), "locked worker ready");
    Sleep(1500);
    require(HeapUnlock(heap), "worker heap unlock");
    for (;;) Sleep(10000);
}

static DWORD CALL critical_wait_worker(void *argument)
{
    if (__sync_add_and_fetch(&critical_waiters_ready, 1) == 2) {
        print("CHILD-EXIT CS WAIT READY\r\n");
        require(SetEvent(argument), "critical waiters ready");
    }
    EnterCriticalSection(child_critical_section);
    require(0, "owned critical section unexpectedly acquired");
    return 1;
}

static void child(DWORD mode)
{
    HANDLE ready = OpenEventA(2, 0, ready_name);
    require(ready != NIL, "open ready event");
    if (mode == 3) {
        InitializeCriticalSection(child_critical_section);
        EnterCriticalSection(child_critical_section);
        for (DWORD i = 0; i < 2; i++)
            require(CreateThread(NIL, 0, critical_wait_worker, ready, 0, NIL) != NIL,
                    "critical waiter startup");
        for (;;) Sleep(10000);
    }
    if (mode == 2) {
        require(CreateThread(NIL, 0, locked_worker, ready, 0, NIL) != NIL,
                "locked worker startup");
        for (;;) Sleep(10000);
    }
    if (mode == 1) (void)begin_worker(1);
    HANDLE heap = GetProcessHeap();
    require(HeapLock(heap), "child heap lock");
    require(SetEvent(ready), "child ready");
    if (mode == 1) require(SetEvent(trigger), "exit worker trigger");
    Sleep(1500);
    require(HeapUnlock(heap), "child heap unlock");
    for (;;) Sleep(10000);
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++) {
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'E') child(0);
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'W') child(1);
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'L') child(2);
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'C') child(3);
    }
    char path[512];
    require(GetModuleFileNameA(NIL, path, sizeof(path)) != 0, "module path");
    HANDLE ready = CreateEventA(NIL, 1, 0, ready_name);
    require(ready != NIL, "ready event");
    for (DWORD mode = 0; mode < 4; mode++) {
        allocated = 0;
        require(ResetEvent(ready), "reset ready");
        HANDLE probe = begin_worker(0);
        char command[540];
        DWORD p = 0, n = 0;
        command[p++] = '"';
        while (path[n]) command[p++] = path[n++];
        command[p++] = '"'; command[p++] = ' ';
        command[p++] = '-'; command[p++] = '-';
        command[p++] = mode == 3 ? 'C' : mode == 2 ? 'L' : mode ? 'W' : 'E'; command[p] = 0;
#ifdef _WIN64
        unsigned char startup[104] = {0};
#else
        unsigned char startup[68] = {0};
#endif
        *(DWORD *)startup = sizeof(startup);
        PROCESS_INFORMATION process;
        require(CreateProcessA(path, command, NIL, NIL, 0, 0, NIL, NIL, startup, &process), "create child");
        require(WaitForSingleObject(ready, 10000) == 0, "child ready wait");
        if (mode == 3) Sleep(100);
        if (mode != 1) require(TerminateProcess(process.process, 83), "terminate child");
        require(WaitForSingleObject(process.process, 10000) == 0, "child termination wait");
        DWORD status = 0;
        require(GetExitCodeProcess(process.process, &status) && status == 83, "child status");
        require(SetEvent(trigger), "heap probe trigger");
        require(WaitForSingleObject(probe, 5000) == 0, "heap probe timed out");
        require(allocated, "heap probe allocation");
        CloseHandle(process.thread); CloseHandle(process.process);
        CloseHandle(probe); CloseHandle(trigger); CloseHandle(started);
        print(mode == 3 ? "CHILD-EXIT CS-WAIT PASS\r\n" :
              mode == 2 ? "CHILD-EXIT WORKER-LOCK PASS\r\n" :
              mode ? "CHILD-EXIT WORKER PASS\r\n" : "CHILD-EXIT EXTERNAL PASS\r\n");
    }
    CloseHandle(ready);
    print("CHILD-EXIT PASS\r\n");
    ExitProcess(0);
}
