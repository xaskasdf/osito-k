/* A child keeps named objects and a pending wait across two shell launches. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef int LONG;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
typedef struct {
    DWORD cb; char *reserved, *desktop, *title;
    DWORD x, y, width, height, columns, rows, fill, flags;
    WORD show, reserved_bytes; BYTE *reserved_data;
    HANDLE input, output, error;
} STARTUP;
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS;
static DWORD failures;
static DWORD (CALL *wait_one)(HANDLE, DWORD), (CALL *module_name)(HANDLE, char *, DWORD);
static char *(CALL *command_line)(void);
static HANDLE (CALL *create_event)(void *, BOOL, BOOL, const char *);
static HANDLE (CALL *open_event)(DWORD, BOOL, const char *);
static HANDLE (CALL *create_mutex)(void *, BOOL, const char *);
static HANDLE (CALL *open_mutex)(DWORD, BOOL, const char *);
static HANDLE (CALL *open_mutex_w)(DWORD, BOOL, const WORD *);
static HANDLE (CALL *create_semaphore)(void *, LONG, LONG, const char *);
static HANDLE (CALL *create_semaphore_w)(void *, LONG, LONG, const WORD *);
static HANDLE (CALL *open_semaphore)(DWORD, BOOL, const char *);
static HANDLE (CALL *open_semaphore_w)(DWORD, BOOL, const WORD *);
static DWORD (CALL *get_error)(void);
static BOOL (CALL *handle_info)(HANDLE, DWORD *);
static BOOL (CALL *release_mutex)(HANDLE), (CALL *release_semaphore)(HANDLE, LONG, LONG *);
static BOOL (CALL *set_event)(HANDLE), (CALL *close_handle)(HANDLE);
static BOOL (CALL *create_process)(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, STARTUP *, PROCESS *);
static STARTUP startup;
static PROCESS observer;
#if __SIZEOF_POINTER__ == 4
#define ABI_SUFFIX "32"
#else
#define ABI_SUFFIX "64"
#endif
static const char ready_name[] = "Local\\OsitoSyncLifetimeReady" ABI_SUFFIX;
static const char begin_name[] = "Local\\OsitoSyncLifetimeBegin" ABI_SUFFIX;
static const char done_name[] = "Local\\OsitoSyncLifetimeDone" ABI_SUFFIX;
static const char mutex_name[] = "Local\\OsitoSyncLifetimeMutex" ABI_SUFFIX;
static const char semaphore_name[] = "Local\\OsitoSyncLifetimeSemaphore" ABI_SUFFIX;

static void sync_lifetime_report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}

static void sync_lifetime_check(BOOL ok, const char *label)
{
    if (!ok) failures++;
    sync_lifetime_report(ok ? "SYNCLIFE PASS " : "SYNCLIFE FAIL ");
    sync_lifetime_report(label);
    sync_lifetime_report("\n");
}

void sync_lifetime_entry(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll");
#define LOAD(var, name) do { \
    var = GetProcAddress(kernel, name); \
    if (!var) { sync_lifetime_report("SYNCLIFE MISSING " name "\n"); ExitProcess(2); } \
} while (0)
    LOAD(wait_one, "WaitForSingleObject"); LOAD(module_name, "GetModuleFileNameA");
    LOAD(command_line, "GetCommandLineA"); LOAD(create_event, "CreateEventA");
    LOAD(open_event, "OpenEventA"); LOAD(set_event, "SetEvent");
    LOAD(close_handle, "CloseHandle"); LOAD(create_process, "CreateProcessA");
    LOAD(create_mutex, "CreateMutexA"); LOAD(open_mutex, "OpenMutexA");
    LOAD(release_mutex, "ReleaseMutex"); LOAD(create_semaphore, "CreateSemaphoreA");
    LOAD(open_semaphore, "OpenSemaphoreA"); LOAD(release_semaphore, "ReleaseSemaphore");
    LOAD(open_mutex_w, "OpenMutexW"); LOAD(open_semaphore_w, "OpenSemaphoreW");
    LOAD(create_semaphore_w, "CreateSemaphoreW"); LOAD(get_error, "GetLastError");
    LOAD(handle_info, "GetHandleInformation");
#undef LOAD
    WORD wide_mutex[80], wide_semaphore[80];
    for (DWORD i = 0; i < sizeof(mutex_name); i++) wide_mutex[i] = (BYTE)mutex_name[i];
    for (DWORD i = 0; i < sizeof(semaphore_name); i++) wide_semaphore[i] = (BYTE)semaphore_name[i];
    BOOL child = 0, second = 0;
    for (const char *p = command_line(); *p; p++) {
        if (p[0] == '-' && p[1] == '-') {
            if (p[2] == 'c') child = 1;
            if (p[2] == 's') second = 1;
        }
    }
    if (child) {
        HANDLE ready = open_event(2, 0, ready_name);
        HANDLE begin = create_event(0, 1, 0, begin_name);
        HANDLE done = create_event(0, 1, 0, done_name);
        HANDLE mutex = create_mutex(0, 1, mutex_name);
        HANDLE semaphore = create_semaphore(0, 2, 3, semaphore_name);
        sync_lifetime_check(ready && begin && done && mutex && semaphore, "child creates named objects");
        HANDLE alias = create_semaphore_w(0, 0, 9, wide_semaphore);
        sync_lifetime_check(alias && get_error() == 183, "CreateSemaphoreW reopens existing object");
        LONG previous = -1;
        sync_lifetime_check(alias && release_semaphore(alias, 1, &previous) && previous == 2,
                            "reopen preserves original semaphore count");
        sync_lifetime_check(alias && wait_one(alias, 0) == 0 && close_handle(alias),
                            "wide semaphore alias shares state and closes");
        sync_lifetime_check(mutex && wait_one(mutex, 0) == 0, "child owns mutex recursively");
        sync_lifetime_check(ready && set_event(ready), "child releases first root");
        sync_lifetime_report("SYNCLIFE BEFORE SECOND\n");
        sync_lifetime_check(begin && wait_one(begin, 30000) == 0, "pending child wait survives next launch");
        sync_lifetime_check(wait_one(begin, 0) == 0, "manual event stays signaled");
        sync_lifetime_check(wait_one(semaphore, 0) == 0, "child consumes first restored count");
        sync_lifetime_check(wait_one(semaphore, 0) == 0, "child consumes second restored count");
        sync_lifetime_check(wait_one(semaphore, 0) == 258, "shared semaphore reaches zero");
        sync_lifetime_check(release_mutex(mutex), "child releases first recursion");
        sync_lifetime_check(release_mutex(mutex), "child releases final recursion");
        sync_lifetime_check(close_handle(ready) && close_handle(begin) && close_handle(mutex) && close_handle(semaphore),
                            "child closes object handles");
        sync_lifetime_report(failures ? "SYNCLIFE CHILD FAILED\n" : "SYNCLIFE CHILD DONE\n");
        set_event(done); close_handle(done);
        ExitProcess(failures ? 1 : 0);
    }
    if (second) {
        HANDLE begin = open_event(2, 0, begin_name);
        HANDLE done = open_event(0x100000, 0, done_name);
        HANDLE mutex = open_mutex(0x100001, 0, mutex_name);
        HANDLE semaphore = open_semaphore(0x100002, 0, semaphore_name);
        sync_lifetime_check(begin && done && mutex && semaphore, "second root reopens live objects");
        if (!(begin && done && mutex && semaphore)) ExitProcess(2);
        HANDLE wide_m = open_mutex_w(0x100001, 1, wide_mutex);
        HANDLE wide_s = open_semaphore_w(0x100002, 1, wide_semaphore);
        sync_lifetime_check(wide_m && wide_s, "wide opens share named objects");
        DWORD flags = 0;
        sync_lifetime_check(handle_info(wide_m, &flags) && (flags & 1), "wide mutex handle is inheritable");
        sync_lifetime_check(handle_info(wide_s, &flags) && (flags & 1), "wide semaphore handle is inheritable");
        sync_lifetime_check(handle_info(mutex, &flags) && !(flags & 1) &&
                            handle_info(semaphore, &flags) && !(flags & 1), "ANSI opens remain non-inheritable");
        sync_lifetime_check(wait_one(wide_m, 0) == 258, "mutex still belongs to child");
        sync_lifetime_check(wait_one(wide_s, 0) == 0, "initial semaphore first count preserved");
        sync_lifetime_check(wait_one(semaphore, 0) == 0, "initial semaphore second count preserved");
        sync_lifetime_check(wait_one(semaphore, 0) == 258, "initial semaphore exhausts at two");
        sync_lifetime_check(close_handle(wide_m) && close_handle(wide_s), "wide object handles close independently");
        LONG previous = -1;
        sync_lifetime_check(release_semaphore(semaphore, 2, &previous) && previous == 0,
                            "second root replenishes shared count");
        sync_lifetime_check(set_event(begin), "second root signals pending child wait");
        sync_lifetime_check(wait_one(done, 20000) == 0, "child completion crosses launches");
        sync_lifetime_check(wait_one(mutex, 0) == 0, "released mutex transfers to second root");
        sync_lifetime_check(release_mutex(mutex), "second root releases mutex");
        sync_lifetime_check(close_handle(begin) && close_handle(done) && close_handle(mutex) && close_handle(semaphore),
                            "second root closes object handles");
        sync_lifetime_report(failures ? "SYNCLIFE SECOND FAILED\n" : "SYNCLIFE SECOND DONE\n");
        ExitProcess(failures ? 1 : 0);
    }
    sync_lifetime_check(!open_mutex(0x100000, 0, mutex_name) && get_error() == 2,
                        "opening missing mutex does not create it");
    sync_lifetime_check(!open_semaphore(0x100000, 0, semaphore_name) && get_error() == 2,
                        "opening missing semaphore does not create it");
    HANDLE ready = create_event(0, 1, 0, ready_name);
    char path[512], command[] = "synclife --child";
    DWORD length = module_name(0, path, sizeof(path));
    startup.cb = sizeof(startup); startup.flags = 0x100;
    startup.input = GetStdHandle((DWORD)-10);
    startup.output = GetStdHandle((DWORD)-11);
    startup.error = GetStdHandle((DWORD)-12);
    sync_lifetime_check(ready && length && length < sizeof(path), "first root setup");
    BOOL made = !failures && create_process(path, command, 0, 0, 1, 0, 0, 0, &startup, &observer);
    sync_lifetime_check(made, "first root creates observer");
    if (made) {
        sync_lifetime_check(wait_one(ready, 20000) == 0, "objects published before root exits");
        close_handle(observer.thread); close_handle(observer.process);
    }
    close_handle(ready);
    sync_lifetime_report("SYNCLIFE FIRST EXIT\n");
    ExitProcess(failures ? 143 : 41);
}
