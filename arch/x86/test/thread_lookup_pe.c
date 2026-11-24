/* Exercise main/worker identity after scheduler-slot reuse on Windows and
 * Osito-K. Timing is evidence, not a machine-dependent correctness limit. */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef __SIZE_TYPE__ SIZE;
typedef __UINTPTR_TYPE__ UPTR;
typedef unsigned long long U64;
typedef int BOOL;
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
typedef struct { DWORD pid, parent_tid, tls, round; } PROBE;
static DWORD (CALL *get_pid)(void), (CALL *get_tid)(void);
static DWORD (CALL *get_error)(void), (CALL *tls_alloc)(void);
static DWORD (CALL *wait_one)(HANDLE, DWORD), (CALL *thread_id)(HANDLE);
static DWORD (CALL *resume)(HANDLE), (CALL *module_name)(HANDLE, char *, DWORD);
static BOOL (CALL *tls_set)(DWORD, void *), (CALL *tls_free)(DWORD);
static void *(CALL *tls_get)(DWORD);
static void (CALL *set_error)(DWORD), (CALL *sleep_ms)(DWORD);
static BOOL (CALL *close_handle)(HANDLE), (CALL *thread_exit)(HANDLE, DWORD *);
static BOOL (CALL *process_exit)(HANDLE, DWORD *), (CALL *counter)(U64 *);
static BOOL (CALL *frequency)(U64 *);
static HANDLE (CALL *create_thread)(void *, SIZE, DWORD (CALL *)(void *),
                                   void *, DWORD, DWORD *);
static BOOL (CALL *create_process)(const char *, char *, void *, void *,
                                  BOOL, DWORD, void *, const char *,
                                  STARTUP *, PROCESS *);
static char *(CALL *command_line)(void);
static DWORD checks, failures;
static STARTUP startup;
static PROCESS child;

static void context_report(const char *text)
{
    DWORD size = 0, written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), text, size, &written, 0);
}

static void context_check(BOOL condition, const char *label)
{
    checks++;
    if (!condition) failures++;
    context_report(condition ? "THREADLOOKUP PASS " : "THREADLOOKUP FAIL ");
    context_report(label);
    context_report("\n");
}

static BOOL context_identity(DWORD pid, DWORD tid, DWORD tls, void *value)
{
    for (DWORD i = 0; i < 24; i++) {
        set_error(0x13572468);
        DWORD actual_pid = get_pid(), actual_tid = get_tid();
        DWORD pseudo_tid = thread_id((HANDLE)(UPTR)-2), error = get_error();
        void *actual_value = tls_get(tls);
        if (actual_pid != pid || actual_tid != tid || pseudo_tid != tid ||
            error != 0x13572468 || actual_value != value) {
            DWORD values[] = {pid, actual_pid, tid, actual_tid, pseudo_tid,
                              error, (DWORD)(UPTR)value, (DWORD)(UPTR)actual_value};
            context_report("THREADLOOKUP IDENTITY expected/actual-pid expected/actual/pseudo-tid error expected/actual-tls");
            for (DWORD field = 0; field < 8; field++) {
                char text[10]; text[0] = ' '; text[9] = 0;
                for (int digit = 8; digit > 0; digit--) {
                    text[digit] = "0123456789abcdef"[values[field] & 15];
                    values[field] >>= 4;
                }
                context_report(text);
            }
            context_report("\n");
            return 0;
        }
        if (!(i % 8)) sleep_ms(0);
    }
    return 1;
}

static BOOL context_benchmark(DWORD pid, DWORD tid, const char *label)
{
    U64 start = 0, end = 0, hz = 0;
    BOOL valid = counter(&start) && frequency(&hz) && hz;
    for (DWORD i = 0; i < 20000; i++)
        if (get_pid() != pid || get_tid() != tid) valid = 0;
    valid = counter(&end) && valid && end >= start;
    U64 values[2] = {end - start, hz};
    context_report("THREADLOOKUP TIMING ");
    context_report(label);
    for (DWORD i = 0; i < 2; i++) {
        char text[18];
        text[0] = ' '; text[17] = 0;
        for (int j = 16; j > 0; j--) {
            text[j] = "0123456789abcdef"[values[i] & 15];
            values[i] >>= 4;
        }
        context_report(text);
    }
    context_report(" iterations=20000\n");
    return valid;
}

static DWORD CALL context_worker(void *argument)
{
    PROBE *probe = argument;
    DWORD tid = get_tid();
    void *value = (void *)(UPTR)(0x1000 + probe->round);
    if (!tid || tid == probe->parent_tid || tls_get(probe->tls))
        return 1;
    if (!tls_set(probe->tls, value) ||
        !context_identity(probe->pid, tid, probe->tls, value))
        return 2;
    if (!probe->round && !context_benchmark(probe->pid, tid, "worker"))
        return 3;
    return 0x7000 + probe->round;
}

void context_entry(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll");
#define LOAD(var, name) do { \
    var = GetProcAddress(kernel, name); \
    if (!var) { context_report("THREADLOOKUP MISSING " name "\n"); ExitProcess(2); } \
} while (0)
    LOAD(get_pid, "GetCurrentProcessId"); LOAD(get_tid, "GetCurrentThreadId");
    LOAD(get_error, "GetLastError"); LOAD(set_error, "SetLastError");
    LOAD(tls_alloc, "TlsAlloc"); LOAD(tls_free, "TlsFree");
    LOAD(tls_get, "TlsGetValue"); LOAD(tls_set, "TlsSetValue");
    LOAD(wait_one, "WaitForSingleObject"); LOAD(thread_id, "GetThreadId");
    LOAD(resume, "ResumeThread"); LOAD(sleep_ms, "Sleep");
    LOAD(close_handle, "CloseHandle"); LOAD(thread_exit, "GetExitCodeThread");
    LOAD(process_exit, "GetExitCodeProcess"); LOAD(create_thread, "CreateThread");
    LOAD(create_process, "CreateProcessA"); LOAD(module_name, "GetModuleFileNameA");
    LOAD(command_line, "GetCommandLineA");
    LOAD(counter, "QueryPerformanceCounter"); LOAD(frequency, "QueryPerformanceFrequency");
#undef LOAD
    BOOL is_child = 0;
    for (const char *p = command_line(); *p; p++)
        if (p[0] == '-' && p[1] == '-' && p[2] == 'c') is_child = 1;
    DWORD pid = get_pid(), tid = get_tid(), tls = tls_alloc();
    context_check(pid && tid && tls != 0xffffffff, "primary identity and TLS allocation");
    if (tls == 0xffffffff) ExitProcess(3);
    void *primary_value = (void *)(UPTR)0x1357;
    context_check(tls_set(tls, primary_value), "set primary TLS");
    for (DWORD i = 0; i < (is_child ? 4U : 24U); i++) {
        PROBE probe = {pid, tid, tls, i};
        DWORD worker_tid = 0, code = 0;
        HANDLE thread = create_thread(0, 0, context_worker, &probe, 4, &worker_tid);
        context_check(thread && worker_tid && worker_tid != tid,
                      "create suspended worker");
        if (!thread) break;
        context_check(thread_id(thread) == worker_tid, "published worker identity");
        context_check(resume(thread) == 1, "resume suspended worker");
        DWORD wait = wait_one(thread, 20000);
        context_check(wait == 0 && thread_exit(thread, &code) && code == 0x7000 + i,
                      "worker identity, TLS and return code");
        if (wait != 0) ExitProcess(4);
        context_check(close_handle(thread), "close worker");
        context_check(context_identity(pid, tid, tls, primary_value),
                      "primary identity and TLS after worker exit");
        sleep_ms(10); /* Let the scheduler reclaim slots between generations. */
    }
    context_check(context_benchmark(pid, tid, "primary"), "primary API loop");
    if (!is_child) {
        char path[512];
        DWORD length = module_name(0, path, sizeof(path));
        context_check(length && length < sizeof(path), "find own executable");
        if (length && length < sizeof(path)) {
            startup.cb = sizeof(startup);
            for (DWORD i = 0; i < 3; i++) {
                char command[] = "threadlookup --child";
                child.process = child.thread = 0;
                BOOL made = create_process(path, command, 0, 0, 0, 0, 0, 0,
                                           &startup, &child);
                context_check(made && child.pid != pid && child.tid != tid,
                              "new process identity");
                if (!made) break;
                DWORD code = 0;
                DWORD wait = wait_one(child.process, 30000);
                context_check(wait == 0 && process_exit(child.process, &code) && !code,
                              "child main and worker identities");
                if (wait != 0) ExitProcess(5);
                context_check(close_handle(child.thread) && close_handle(child.process),
                              "close child handles");
                context_check(context_identity(pid, tid, tls, primary_value),
                              "parent identity and TLS after child exit");
                sleep_ms(10);
            }
        }
    }
    context_check(tls_free(tls), "free primary TLS");
    context_report(is_child ? "THREADLOOKUP CHILD DONE\n" : "THREADLOOKUP DONE\n");
    ExitProcess(failures ? 1 : 0);
}
