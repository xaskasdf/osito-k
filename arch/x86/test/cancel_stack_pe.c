/* Process cancellation must preserve peers' memory while host locks unwind. */
typedef unsigned long DWORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
API void CALL ExitProcess(DWORD);
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
API DWORD CALL GetTickCount(void);
API HANDLE CALL GetProcessHeap(void);
API BOOL CALL HeapLock(HANDLE);
API BOOL CALL HeapUnlock(HANDLE);
API void *CALL HeapAlloc(HANDLE, DWORD, SIZE_T);
API BOOL CALL HeapFree(HANDLE, DWORD, void *);
static HANDLE node_ready, never;
static volatile DWORD *volatile node;
static const char ready_name[] = "Local\\OsitoCancelStackReady";

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void require(BOOL ok, const char *stage)
{
    if (ok) return;
    print("CANCEL-STACK FAIL "); print(stage); print("\r\n");
    ExitProcess(1);
}

static DWORD CALL stack_owner(void *argument)
{
    (void)argument;
    volatile DWORD local_node = 0x13579bdf;
    node = &local_node;
    require(SetEvent(node_ready), "node ready");
    for (;;) WaitForSingleObject(never, 0xffffffffU);
}

static DWORD CALL guarded_peer(void *argument)
{
    require(WaitForSingleObject(node_ready, 10000) == 0, "node wait");
    HANDLE heap = GetProcessHeap();
    require(HeapLock(heap), "heap lock");
    require(SetEvent(argument), "guard ready");
    /* Windows stops this thread outright. Osito must let its host lock
     * unwind, without freeing the cancelled peer's shared stack first. */
    DWORD start = GetTickCount();
    do { Sleep(20); } while ((DWORD)(GetTickCount() - start) < 3000);
    require(node && *node == 0x13579bdf, "peer stack retained");
    *node = 0x2468ace0;
    require(HeapUnlock(heap), "heap unlock");
    for (;;) Sleep(10000);
}

static void child(void)
{
    HANDLE ready = OpenEventA(2, 0, ready_name);
    node_ready = CreateEventA(NIL, 1, 0, NIL);
    never = CreateEventA(NIL, 1, 0, NIL);
    require(ready && node_ready && never, "child events");
    require(CreateThread(NIL, 0, stack_owner, NIL, 0, NIL) != NIL, "node thread");
    require(CreateThread(NIL, 0, guarded_peer, ready, 0, NIL) != NIL, "guard thread");
    for (;;) Sleep(10000);
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++)
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'C') child();
    char path[512];
    require(GetModuleFileNameA(NIL, path, sizeof(path)) != 0, "module path");
    HANDLE ready = CreateEventA(NIL, 1, 0, ready_name);
    require(ready != NIL, "ready event");
    for (DWORD round = 0; round < 3; round++) {
        require(ResetEvent(ready), "reset ready");
        char command[540];
        DWORD p = 0, n = 0;
        command[p++] = '"';
        while (path[n]) command[p++] = path[n++];
        command[p++] = '"'; command[p++] = ' ';
        command[p++] = '-'; command[p++] = '-'; command[p++] = 'C'; command[p] = 0;
#ifdef _WIN64
        unsigned char startup[104] = {0};
#else
        unsigned char startup[68] = {0};
#endif
        *(DWORD *)startup = sizeof(startup);
        PROCESS_INFORMATION process;
        require(CreateProcessA(path, command, NIL, NIL, 0, 0, NIL, NIL, startup, &process), "create child");
        require(WaitForSingleObject(ready, 10000) == 0, "guard ready wait");
        require(TerminateProcess(process.process, 83), "terminate child");
        require(WaitForSingleObject(process.process, 10000) == 0, "termination wait");
        DWORD status = 0;
        require(GetExitCodeProcess(process.process, &status) && status == 83, "exit status");
        void *memory = HeapAlloc(GetProcessHeap(), 8, 4096);
        require(memory && HeapFree(GetProcessHeap(), 0, memory), "heap still usable");
        CloseHandle(process.thread); CloseHandle(process.process);
        print("CANCEL-STACK ROUND PASS\r\n");
    }
    CloseHandle(ready);
    print("CANCEL-STACK PASS\r\n");
    ExitProcess(0);
}
