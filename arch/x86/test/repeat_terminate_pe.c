/* Repeated termination must not take over another thread's process cleanup. */
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
API HANDLE CALL GetProcessHeap(void);
API BOOL CALL HeapLock(HANDLE);
API BOOL CALL HeapUnlock(HANDLE);
API void *CALL HeapAlloc(HANDLE, DWORD, SIZE_T);
API BOOL CALL HeapFree(HANDLE, DWORD, void *);

#define SAY(literal) do { \
    DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11), literal, sizeof(literal) - 1, &written, NIL); \
} while (0)
#define REQUIRE(test, stage) do { \
    if (!(test)) { SAY("REPEAT-TERMINATE FAIL " stage "\r\n"); ExitProcess(1); } \
} while (0)
static const char ready_name[] = "Local\\OsitoRepeatTerminateReady";

static DWORD CALL repeat_guarded_worker(void *ready)
{
    HANDLE heap = GetProcessHeap();
    REQUIRE(HeapLock(heap), "child heap lock");
    REQUIRE(SetEvent(ready), "child ready");
    /* Osito must unwind this host lock even when the guest is cancelled.
     * A second TerminateProcess must leave that cleanup owner in charge. */
    Sleep(1500);
    REQUIRE(HeapUnlock(heap), "child heap unlock");
    for (;;) Sleep(10000);
}

void repeat_terminate_entry(void)
{
    char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++) {
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'C') {
            HANDLE ready = OpenEventA(2, 0, ready_name);
            REQUIRE(ready != NIL, "open ready");
            REQUIRE(CreateThread(NIL, 0, repeat_guarded_worker, ready, 0, NIL) != NIL,
                    "child worker");
            for (;;) Sleep(10000);
        }
    }
    char path[512];
    REQUIRE(GetModuleFileNameA(NIL, path, sizeof(path)) != 0, "module path");
    HANDLE ready = CreateEventA(NIL, 1, 0, ready_name);
    REQUIRE(ready != NIL, "ready event");
    for (DWORD round = 0; round < 4; round++) {
        REQUIRE(ResetEvent(ready), "reset ready");
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
        REQUIRE(CreateProcessA(path, command, NIL, NIL, 0, 0, NIL, NIL, startup, &process),
                "create child");
        REQUIRE(WaitForSingleObject(ready, 10000) == 0, "wait ready");
        REQUIRE(TerminateProcess(process.process, 83), "first terminate");
        Sleep(100);
        /* On Windows cleanup may already be complete, so the return value
         * of later requests can differ. They must never replace exit 83. */
        (void)TerminateProcess(process.process, 99);
        for (DWORD attempt = 0; attempt < 8; attempt++) {
            if (WaitForSingleObject(process.process, 10) == 0) break;
            (void)TerminateProcess(process.process, 100 + attempt);
        }
        REQUIRE(WaitForSingleObject(process.process, 10000) == 0, "termination wait");
        DWORD status = 0;
        REQUIRE(GetExitCodeProcess(process.process, &status) && status == 83, "exit status");
        void *memory = HeapAlloc(GetProcessHeap(), 8, 4096);
        REQUIRE(memory && HeapFree(GetProcessHeap(), 0, memory), "heap still usable");
        CloseHandle(process.thread); CloseHandle(process.process);
        SAY("REPEAT-TERMINATE ROUND PASS\r\n");
    }
    CloseHandle(ready);
    SAY("REPEAT-TERMINATE PASS\r\n");
    ExitProcess(0);
}
