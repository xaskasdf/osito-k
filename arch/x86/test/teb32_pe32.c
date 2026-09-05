/* The same binary exercises real PE32 segment accesses on Windows and OsitoK. */
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef void *HANDLE;
typedef HANDLE HWND;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
typedef LONG (WINAPI *WNDPROC)(HWND, UINT, DWORD, LONG);
typedef struct {
    UINT size, style;
    WNDPROC procedure;
    int class_extra, window_extra;
    HANDLE instance, icon, cursor, background;
    const char *menu, *name;
    HANDLE small_icon;
} WNDCLASSEXA;

IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT int WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT void WINAPI ExitProcess(UINT);
IMPORT DWORD WINAPI GetCurrentThreadId(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT HANDLE WINAPI GetModuleHandleA(const char *);
IMPORT HANDLE WINAPI CreateEventA(void *, int, int, const char *);
IMPORT int WINAPI SetEvent(HANDLE);
IMPORT HANDLE WINAPI CreateThread(void *, DWORD,
                                  DWORD (WINAPI *)(void *), void *, DWORD,
                                  DWORD *);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
IMPORT int WINAPI CloseHandle(HANDLE);
IMPORT void WINAPI Sleep(DWORD);
IMPORT DWORD WINAPI TlsAlloc(void);
IMPORT int WINAPI TlsFree(DWORD);
IMPORT int WINAPI TlsSetValue(DWORD, void *);
IMPORT void *WINAPI TlsGetValue(DWORD);
IMPORT void WINAPI SetLastError(DWORD);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT unsigned short WINAPI RegisterClassExA(const WNDCLASSEXA *);
IMPORT HWND WINAPI CreateWindowExA(DWORD, const char *, const char *, DWORD,
                                   int, int, int, int, HWND, HANDLE, HANDLE, void *);
IMPORT int WINAPI DestroyWindow(HWND);
IMPORT LONG WINAPI DefWindowProcA(HWND, UINT, DWORD, LONG);
IMPORT LONG WINAPI SendMessageA(HWND, UINT, DWORD, LONG);

enum { WORKERS = 4, ROUNDS = 32, WM_PROBE = 0x8013 };
typedef struct {
    DWORD self, tid, marker;
    unsigned failures;
    HANDLE done;
} thread_result;
static thread_result main_result, results[WORKERS];
static DWORD tls_slot;
static HANDLE start_gate, finish_gate;

static void report(const char *text)
{
    DWORD size = 0, written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), text, size, &written, 0);
}

static void check_teb(thread_result *result)
{
    DWORD self, pid, tid, head, peb, error;
    __asm__ volatile (
        "movl %%fs:0x18, %0\n"
        "movl %%fs:0x20, %1\n"
        "movl %%fs:0x24, %2\n"
        : "=r"(self), "=r"(pid), "=r"(tid) : : "memory");
    __asm__ volatile (
        "movl %%fs:0x00, %0\n"
        "movl %%fs:0x30, %1\n"
        : "=r"(head), "=r"(peb) : : "memory");
    if (!self || self != result->self || tid != result->tid ||
        tid != GetCurrentThreadId() || pid != GetCurrentProcessId() || !peb)
        result->failures++;
    if (self && *(volatile DWORD *)self != head) result->failures++;
    if (TlsGetValue(tls_slot) != (void *)result->marker) result->failures++;
    SetLastError(result->marker);
    __asm__ volatile ("movl %%fs:0x34, %0" : "=r"(error) : : "memory");
    if (error != result->marker || GetLastError() != result->marker)
        result->failures++;
}

static void start_thread_result(thread_result *result)
{
    __asm__ volatile ("movl %%fs:0x18, %0" : "=r"(result->self) : : "memory");
    result->tid = GetCurrentThreadId();
    if (TlsGetValue(tls_slot) != 0 ||
        !TlsSetValue(tls_slot, (void *)result->marker)) result->failures++;
    check_teb(result);
}

static DWORD WINAPI worker(void *argument)
{
    thread_result *result = argument;
    if (WaitForSingleObject(start_gate, 10000) != 0) ExitProcess(4);
    start_thread_result(result);
    for (unsigned i = 0; i < ROUNDS; i++) {
        Sleep(1);
        check_teb(result);
    }
    SetEvent(result->done);
    if (WaitForSingleObject(finish_gate, 10000) != 0) ExitProcess(4);
    return result->failures;
}

static LONG WINAPI window_proc(HWND window, UINT message, DWORD wp, LONG lp)
{
    if (message == WM_PROBE) {
        check_teb(&main_result);
        Sleep(1);
        check_teb(&main_result);
        return 0x12345678;
    }
    return DefWindowProcA(window, message, wp, lp);
}

void mainCRTStartup(void)
{
    tls_slot = TlsAlloc();
    if (tls_slot == 0xFFFFFFFFUL) ExitProcess(2);
    main_result.marker = 0x12340000;
    start_thread_result(&main_result);
    start_gate = CreateEventA(0, 1, 0, 0);
    finish_gate = CreateEventA(0, 1, 0, 0);
    if (!start_gate || !finish_gate) ExitProcess(2);
    HANDLE threads[WORKERS] = {0};
    for (unsigned i = 0; i < WORKERS; i++) {
        results[i].marker = 0x12340001 + i;
        results[i].done = CreateEventA(0, 1, 0, 0);
        if (!results[i].done) ExitProcess(2);
        threads[i] = CreateThread(0, 0, worker, &results[i], 0, 0);
        if (!threads[i]) ExitProcess(2);
    }
    SetEvent(start_gate);

    WNDCLASSEXA cls = {0};
    cls.size = sizeof(cls);
    cls.procedure = window_proc;
    cls.instance = GetModuleHandleA(0);
    cls.name = "OsitoTebContract";
    if (!RegisterClassExA(&cls)) ExitProcess(2);
    HWND window = CreateWindowExA(0, cls.name, cls.name, 0, 0, 0, 32, 32,
                                  0, 0, cls.instance, 0);
    if (!window) ExitProcess(2);
    for (unsigned i = 0; i < ROUNDS; i++) {
        if (SendMessageA(window, WM_PROBE, 0, 0) != 0x12345678)
            main_result.failures++;
        check_teb(&main_result);
    }
    if (!DestroyWindow(window)) main_result.failures++;

    unsigned failures = main_result.failures;
    for (unsigned i = 0; i < WORKERS; i++) {
        if (WaitForSingleObject(results[i].done, 10000) != 0) ExitProcess(4);
        failures += results[i].failures;
        if (!results[i].self || results[i].self == main_result.self ||
            results[i].tid == main_result.tid) failures++;
        for (unsigned j = 0; j < i; j++)
            if (results[i].self == results[j].self ||
                results[i].tid == results[j].tid) failures++;
    }
    SetEvent(finish_gate);
    for (unsigned i = 0; i < WORKERS; i++) {
        if (WaitForSingleObject(threads[i], 10000) != 0) ExitProcess(4);
        CloseHandle(threads[i]);
        CloseHandle(results[i].done);
    }
    CloseHandle(start_gate);
    CloseHandle(finish_gate);
    if (!TlsFree(tls_slot)) failures++;
    report(failures ? "[TEB32] FAIL\n" : "[TEB32] PASS\n");
    ExitProcess(failures ? 3 : 0);
}
