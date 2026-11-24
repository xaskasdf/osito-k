/* Public SRW contracts, including cancelled contenders during child teardown. */
typedef unsigned long DWORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef __UINTPTR_TYPE__ WORD_PTR;
typedef void *HANDLE;
typedef int BOOL;
typedef unsigned char BOOLEAN;
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
API HANDLE CALL CreateThread(void *, SIZE_T, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
API HANDLE CALL CreateEventA(void *, BOOL, BOOL, const char *);
API HANDLE CALL OpenEventA(DWORD, BOOL, const char *);
API BOOL CALL SetEvent(HANDLE);
API BOOL CALL ResetEvent(HANDLE);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL CloseHandle(HANDLE);
API void CALL Sleep(DWORD);
API void CALL InitializeSRWLock(void *);
API void CALL AcquireSRWLockExclusive(void *);
API void CALL ReleaseSRWLockExclusive(void *);
API void CALL AcquireSRWLockShared(void *);
API void CALL ReleaseSRWLockShared(void *);
API BOOLEAN CALL TryAcquireSRWLockExclusive(void *);
API BOOLEAN CALL TryAcquireSRWLockShared(void *);
API void CALL InitializeConditionVariable(void *);
API BOOL CALL SleepConditionVariableSRW(void *, void *, DWORD, DWORD);
API void CALL WakeAllConditionVariable(void *);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
API BOOL CALL TerminateProcess(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
static WORD_PTR lock, condition;
static HANDLE ready, release_reader, writer_started, writer_acquired;
static volatile DWORD try_result, predicate, child_waiters;
static DWORD child_exclusive;
static const char ready_name[] = "Local\\OsitoSrwCancelReady";

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void require(BOOL ok, const char *stage)
{
    if (ok) return;
    print("SRW-MODES FAIL "); print(stage); print("\r\n");
    ExitProcess(1);
}

static void join(HANDLE thread)
{
    require(thread && WaitForSingleObject(thread, 5000) == 0, "join worker");
    require(CloseHandle(thread), "close worker");
}

static DWORD CALL try_worker(void *shared)
{
    try_result = shared ? TryAcquireSRWLockShared(&lock) : TryAcquireSRWLockExclusive(&lock);
    if (try_result) {
        if (shared) ReleaseSRWLockShared(&lock);
        else ReleaseSRWLockExclusive(&lock);
    }
    return 0;
}

static BOOL try_mode(BOOL shared)
{
    try_result = 2;
    join(CreateThread(NIL, 0, try_worker, shared ? (void *)1 : NIL, 0, NIL));
    require(try_result != 2, "try worker ran");
    return try_result;
}

static DWORD CALL reader_worker(void *unused)
{
    (void)unused;
    AcquireSRWLockShared(&lock);
    require(SetEvent(ready), "reader ready");
    require(WaitForSingleObject(release_reader, 5000) == 0, "release reader signal");
    ReleaseSRWLockShared(&lock);
    return 0;
}

static DWORD CALL writer_worker(void *unused)
{
    (void)unused;
    require(SetEvent(writer_started), "writer started");
    AcquireSRWLockExclusive(&lock);
    require(SetEvent(writer_acquired), "writer acquired");
    ReleaseSRWLockExclusive(&lock);
    return 0;
}

static DWORD CALL condition_reader(void *unused)
{
    (void)unused;
    AcquireSRWLockShared(&lock);
    require(SetEvent(ready), "condition reader ready");
    while (!predicate) {
        BOOL woke = SleepConditionVariableSRW(&condition, &lock, 3000, 1);
        require(woke || GetLastError() == 1460, "shared condition wait");
    }
    ReleaseSRWLockShared(&lock);
    return 0;
}

static DWORD CALL condition_writer(void *unused)
{
    (void)unused;
    AcquireSRWLockExclusive(&lock);
    predicate = 1;
    WakeAllConditionVariable(&condition);
    ReleaseSRWLockExclusive(&lock);
    return 0;
}

static DWORD CALL child_waiter(void *unused)
{
    (void)unused;
    if (__sync_add_and_fetch(&child_waiters, 1) == 2) {
        print(child_exclusive ? "SRW-MODES CANCEL EXCLUSIVE READY\r\n" :
                                "SRW-MODES CANCEL SHARED READY\r\n");
        require(SetEvent(ready), "child waiters ready");
    }
    if (child_exclusive) AcquireSRWLockExclusive(&lock);
    else AcquireSRWLockShared(&lock);
    require(0, "child waiter acquired held lock");
    return 1;
}

static void child(BOOL exclusive_waiters)
{
    child_exclusive = exclusive_waiters;
    ready = OpenEventA(2, 0, ready_name);
    require(ready != NIL, "open child ready");
    InitializeSRWLock(&lock);
    if (child_exclusive) AcquireSRWLockShared(&lock);
    else AcquireSRWLockExclusive(&lock);
    for (DWORD i = 0; i < 2; i++)
        require(CreateThread(NIL, 0, child_waiter, NIL, 0, NIL) != NIL, "create child waiter");
    for (;;) Sleep(10000);
}

void mainCRTStartup(void)
{
    char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++) {
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'S') child(0);
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'E') child(1);
    }
    InitializeSRWLock(&lock);
    InitializeConditionVariable(&condition);
    AcquireSRWLockShared(&lock);
    require(try_mode(1), "parallel try shared");
    require(!try_mode(0), "writer excluded by reader");
    ReleaseSRWLockShared(&lock);
    print("SRW-MODES TRY-SHARED PASS\r\n");

    AcquireSRWLockExclusive(&lock);
    require(!try_mode(1) && !try_mode(0), "exclusive owner excludes others");
    ReleaseSRWLockExclusive(&lock);
    require(try_mode(0), "writer enters empty lock");
    print("SRW-MODES TRY-EXCLUSIVE PASS\r\n");

    AcquireSRWLockShared(&lock);
    require(!SleepConditionVariableSRW(&condition, &lock, 0, 1) && GetLastError() == 1460,
            "shared condition timeout");
    require(try_mode(1) && !try_mode(0), "shared timeout reacquires shared");
    ReleaseSRWLockShared(&lock);
    print("SRW-MODES CV-SHARED-TIMEOUT PASS\r\n");

    AcquireSRWLockExclusive(&lock);
    require(!SleepConditionVariableSRW(&condition, &lock, 0, 0) && GetLastError() == 1460,
            "exclusive condition timeout");
    require(!try_mode(1) && !try_mode(0), "exclusive timeout reacquires exclusive");
    ReleaseSRWLockExclusive(&lock);
    print("SRW-MODES CV-EXCLUSIVE-TIMEOUT PASS\r\n");

    ready = CreateEventA(NIL, 1, 0, NIL);
    release_reader = CreateEventA(NIL, 1, 0, NIL);
    writer_started = CreateEventA(NIL, 1, 0, NIL);
    writer_acquired = CreateEventA(NIL, 1, 0, NIL);
    require(ready && release_reader && writer_started && writer_acquired, "reader writer events");
    AcquireSRWLockShared(&lock);
    HANDLE reader = CreateThread(NIL, 0, reader_worker, NIL, 0, NIL);
    require(reader && WaitForSingleObject(ready, 5000) == 0, "parallel blocking reader");
    HANDLE writer = CreateThread(NIL, 0, writer_worker, NIL, 0, NIL);
    require(writer && WaitForSingleObject(writer_started, 5000) == 0, "blocking writer startup");
    require(WaitForSingleObject(writer_acquired, 0) == 258, "writer waits for readers");
    ReleaseSRWLockShared(&lock);
    Sleep(20);
    require(WaitForSingleObject(writer_acquired, 0) == 258, "one reader remains");
    require(SetEvent(release_reader), "release last reader");
    join(reader); join(writer);
    require(WaitForSingleObject(writer_acquired, 0) == 0, "writer enters after last reader");
    print("SRW-MODES BLOCKING-READERS PASS\r\n");

    require(ResetEvent(ready), "reset condition ready");
    AcquireSRWLockShared(&lock);
    reader = CreateThread(NIL, 0, condition_reader, NIL, 0, NIL);
    require(reader && WaitForSingleObject(ready, 5000) == 0, "shared condition reader startup");
    writer = CreateThread(NIL, 0, condition_writer, NIL, 0, NIL);
    require(writer != NIL, "condition writer startup");
    ReleaseSRWLockShared(&lock);
    join(writer); join(reader);
    require(predicate == 1, "shared condition predicate");
    print("SRW-MODES CV-SHARED-WAKE PASS\r\n");
    CloseHandle(ready); CloseHandle(release_reader);
    CloseHandle(writer_started); CloseHandle(writer_acquired);

    char path[512];
    require(GetModuleFileNameA(NIL, path, sizeof(path)) != 0, "module path");
    ready = CreateEventA(NIL, 1, 0, ready_name);
    require(ready != NIL, "child ready event");
    for (DWORD mode = 0; mode < 2; mode++) {
        require(ResetEvent(ready), "reset child ready");
        char command[540];
        DWORD p = 0, n = 0;
        command[p++] = '"';
        while (path[n]) command[p++] = path[n++];
        command[p++] = '"'; command[p++] = ' ';
        command[p++] = '-'; command[p++] = '-';
        command[p++] = mode ? 'E' : 'S'; command[p] = 0;
#ifdef _WIN64
        unsigned char startup[104] = {0};
#else
        unsigned char startup[68] = {0};
#endif
        *(DWORD *)startup = sizeof(startup);
        PROCESS_INFORMATION process;
        require(CreateProcessA(path, command, NIL, NIL, 0, 0, NIL, NIL, startup, &process), "create child");
        require(WaitForSingleObject(ready, 5000) == 0, "child ready wait");
        Sleep(50);
        require(TerminateProcess(process.process, 83), "terminate child");
        require(WaitForSingleObject(process.process, 5000) == 0, "child termination wait");
        DWORD status = 0;
        require(GetExitCodeProcess(process.process, &status) && status == 83, "child status");
        CloseHandle(process.thread); CloseHandle(process.process);
        print(mode ? "SRW-MODES CANCEL-EXCLUSIVE PASS\r\n" : "SRW-MODES CANCEL-SHARED PASS\r\n");
    }
    CloseHandle(ready);
    print("SRW-MODES PASS\r\n");
    ExitProcess(0);
}
