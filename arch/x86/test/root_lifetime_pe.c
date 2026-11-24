/* Keep handles to an exited root process/thread while another shell-launched
 * root is alive. Old handles must remain signaled and retain their exit code. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
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
static DWORD (CALL *get_pid)(void), (CALL *get_tid)(void);
static DWORD (CALL *wait_one)(HANDLE, DWORD), (CALL *process_id)(HANDLE);
static DWORD (CALL *thread_id)(HANDLE), (CALL *module_name)(HANDLE, char *, DWORD);
static char *(CALL *command_line)(void);
static BOOL (CALL *exit_process)(HANDLE, DWORD *), (CALL *exit_thread)(HANDLE, DWORD *);
static HANDLE (CALL *create_event)(void *, BOOL, BOOL, const char *);
static HANDLE (CALL *open_event)(DWORD, BOOL, const char *);
static HANDLE (CALL *open_process)(DWORD, BOOL, DWORD);
static HANDLE (CALL *open_thread)(DWORD, BOOL, DWORD);
static BOOL (CALL *set_event)(HANDLE), (CALL *close_handle)(HANDLE);
static HANDLE (CALL *create_thread)(void *, SIZE, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
static BOOL (CALL *create_process)(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, STARTUP *, PROCESS *);
static STARTUP startup;
static PROCESS observer;
#if __SIZEOF_POINTER__ == 4
#define ABI_SUFFIX "32"
#else
#define ABI_SUFFIX "64"
#endif
static const char ready_name[] = "Local\\OsitoRootLifetimeReady" ABI_SUFFIX;
static const char begin_name[] = "Local\\OsitoRootLifetimeBegin" ABI_SUFFIX;
static const char done_name[] = "Local\\OsitoRootLifetimeDone" ABI_SUFFIX;

static void root_lifetime_report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}

static void root_lifetime_check(BOOL ok, const char *label)
{
    if (!ok) failures++;
    root_lifetime_report(ok ? "ROOTLIFE PASS " : "ROOTLIFE FAIL ");
    root_lifetime_report(label);
    root_lifetime_report("\n");
}

static DWORD CALL root_lifetime_worker(void *argument)
{
    return argument == (void *)(UPTR)0x7351 ? 0x2a : 1;
}

void root_lifetime_entry(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll");
#define LOAD(var, name) do { \
    var = GetProcAddress(kernel, name); \
    if (!var) { root_lifetime_report("ROOTLIFE MISSING " name "\n"); ExitProcess(2); } \
} while (0)
    LOAD(get_pid, "GetCurrentProcessId"); LOAD(get_tid, "GetCurrentThreadId");
    LOAD(wait_one, "WaitForSingleObject"); LOAD(process_id, "GetProcessId");
    LOAD(thread_id, "GetThreadId"); LOAD(module_name, "GetModuleFileNameA");
    LOAD(command_line, "GetCommandLineA"); LOAD(exit_process, "GetExitCodeProcess");
    LOAD(exit_thread, "GetExitCodeThread"); LOAD(create_event, "CreateEventA");
    LOAD(open_event, "OpenEventA"); LOAD(open_process, "OpenProcess");
    LOAD(open_thread, "OpenThread"); LOAD(set_event, "SetEvent");
    LOAD(close_handle, "CloseHandle"); LOAD(create_thread, "CreateThread");
    LOAD(create_process, "CreateProcessA");
#undef LOAD
    BOOL second = 0;
    DWORD parent_pid = 0, parent_tid = 0;
    for (const char *p = command_line(); *p; p++) {
        if (p[0] != '-' || p[1] != '-') continue;
        if (p[2] == 's') second = 1;
        if (p[2] != 'w' || p[3] != ' ') continue;
        for (DWORD i = 0; i < 8; i++) {
            BYTE a = (BYTE)p[4 + i], b = (BYTE)p[13 + i];
            parent_pid = (parent_pid << 4) | (a <= '9' ? a - '0' : a - 'a' + 10);
            parent_tid = (parent_tid << 4) | (b <= '9' ? b - '0' : b - 'a' + 10);
        }
        break;
    }
    if (parent_pid) {
        HANDLE ready = open_event(2, 0, ready_name);
        HANDLE begin = create_event(0, 1, 0, begin_name);
        HANDLE done = create_event(0, 1, 0, done_name);
        HANDLE parent = open_process(0x101000, 0, parent_pid);
        HANDLE primary = open_thread(0x100800, 0, parent_tid);
        DWORD code = 0;
        root_lifetime_check(ready && begin && done && parent && primary, "observer setup");
        root_lifetime_check(parent && wait_one(parent, 0) == 258, "live parent is unsignaled");
        root_lifetime_check(primary && wait_one(primary, 0) == 258, "live primary is unsignaled");
        root_lifetime_check(exit_process(parent, &code) && code == 259, "live parent exit status");
        root_lifetime_check(exit_thread(primary, &code) && code == 259, "live primary exit status");
        root_lifetime_check(ready && set_event(ready), "observer releases first root");
        root_lifetime_check(parent && wait_one(parent, 5000) == 0, "exited parent is signaled");
        root_lifetime_check(primary && wait_one(primary, 5000) == 0, "exited primary is signaled");
        root_lifetime_check(exit_process(parent, &code) && code == 41, "parent retains exit code 41");
        root_lifetime_check(exit_thread(primary, &code) && code == 41, "primary retains exit code 41");
        root_lifetime_check(process_id(parent) == parent_pid, "old process identity");
        root_lifetime_check(thread_id(primary) == parent_tid, "old thread identity");
        root_lifetime_report("ROOTLIFE BEFORE SECOND\n");
        root_lifetime_check(begin && wait_one(begin, 30000) == 0, "second root is active");
        root_lifetime_check(wait_one(parent, 0) == 0, "old process stays signaled during second root");
        root_lifetime_check(wait_one(primary, 0) == 0, "old primary stays signaled during second root");
        root_lifetime_check(exit_process(parent, &code) && code == 41, "old process exit code survives relaunch");
        root_lifetime_check(exit_thread(primary, &code) && code == 41, "old primary exit code survives relaunch");
        root_lifetime_check(process_id(parent) == parent_pid && thread_id(primary) == parent_tid,
                            "old identities survive relaunch");
        root_lifetime_check(close_handle(parent) && close_handle(primary), "release old process and thread handles");
        close_handle(ready); close_handle(begin);
        root_lifetime_report(failures ? "ROOTLIFE WATCH FAILED\n" : "ROOTLIFE WATCH DONE\n");
        set_event(done); close_handle(done);
        ExitProcess(failures ? 1 : 0);
    }
    if (second) {
        HANDLE begin = open_event(2, 0, begin_name);
        HANDLE done = open_event(0x100000, 0, done_name);
        HANDLE self = open_process(0x101000, 0, get_pid());
        DWORD code = 0;
        root_lifetime_check(begin && done && self, "second root setup");
        root_lifetime_check(set_event(begin) && wait_one(done, 30000) == 0, "observer released old handles");
        root_lifetime_check(wait_one(self, 0) == 258 && exit_process(self, &code) && code == 259,
                            "second process remains live after old handles close");
        DWORD tid = 0;
        HANDLE worker = create_thread(0, 0, root_lifetime_worker, (void *)(UPTR)0x7351, 0, &tid);
        root_lifetime_check(worker && tid && tid != get_tid(), "second root can create a worker");
        root_lifetime_check(worker && wait_one(worker, 10000) == 0 && exit_thread(worker, &code) && code == 0x2a,
                            "second root worker completes");
        root_lifetime_check(close_handle(worker), "second root worker handle closes");
        close_handle(begin); close_handle(done); close_handle(self);
        root_lifetime_report(failures ? "ROOTLIFE SECOND FAILED\n" : "ROOTLIFE SECOND DONE\n");
        ExitProcess(failures ? 1 : 0);
    }
    HANDLE ready = create_event(0, 1, 0, ready_name);
    char path[512], command[] = "rootlife --w 00000000 00000000";
    DWORD length = module_name(0, path, sizeof(path));
    DWORD pid = get_pid(), tid = get_tid();
    for (DWORD i = 0; i < 8; i++) {
        command[13 + i] = "0123456789abcdef"[(pid >> (28 - i * 4)) & 15];
        command[22 + i] = "0123456789abcdef"[(tid >> (28 - i * 4)) & 15];
    }
    startup.cb = sizeof(startup); startup.flags = 0x100;
    startup.input = GetStdHandle((DWORD)-10);
    startup.output = GetStdHandle((DWORD)-11);
    startup.error = GetStdHandle((DWORD)-12);
    root_lifetime_check(ready && length && length < sizeof(path), "first root setup");
    BOOL made = !failures && create_process(path, command, 0, 0, 1, 0, 0, 0, &startup, &observer);
    root_lifetime_check(made, "create handle observer");
    if (made) {
        root_lifetime_check(wait_one(ready, 20000) == 0, "observer pins handles before root exit");
        close_handle(observer.thread); close_handle(observer.process);
    }
    close_handle(ready);
    root_lifetime_report("ROOTLIFE FIRST EXIT\n");
    ExitProcess(failures ? 143 : 41);
}
