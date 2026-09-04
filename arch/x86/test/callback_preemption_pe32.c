/* PE32 callbacks must remain preemptible, including nested window procedures
 * and callbacks suspended while another thread enters its own callback. */
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef unsigned long long QWORD;
typedef void *HANDLE;
typedef HANDLE HWND;
typedef LONG LRESULT;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)

typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, DWORD, LONG);
typedef struct {
    UINT cbSize, style;
    WNDPROC wndproc;
    int clsExtra, wndExtra;
    HANDLE instance, icon, cursor, background;
    const char *menu, *name;
    HANDLE smallIcon;
} WNDCLASSEXA;

IMPORT HANDLE WINAPI GetModuleHandleA(const char *);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT int WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT void WINAPI ExitProcess(UINT);
IMPORT void WINAPI Sleep(DWORD);
IMPORT HANDLE WINAPI CreateThread(void *, DWORD, DWORD (WINAPI *)(void *),
                                  void *, DWORD, DWORD *);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
IMPORT int WINAPI CloseHandle(HANDLE);
IMPORT unsigned short WINAPI RegisterClassExA(const WNDCLASSEXA *);
IMPORT HWND WINAPI CreateWindowExA(DWORD, const char *, const char *, DWORD,
                                   int, int, int, int, HWND, HANDLE, HANDLE, void *);
IMPORT int WINAPI DestroyWindow(HWND);
IMPORT LRESULT WINAPI DefWindowProcA(HWND, UINT, DWORD, LONG);
IMPORT LRESULT WINAPI SendMessageA(HWND, UINT, DWORD, LONG);

#define WM_TEST_SPIN  0x8011U
#define WM_TEST_NEST  0x8012U
#define WM_TEST_SLEEP 0x8013U
#define RESULT_MARK  0x13572468L

static const char class_name[] = "CallbackPreemptionTest";
static HANDLE instance;
static volatile DWORD heartbeat, ready, stop_worker, worker_command;
static volatile DWORD worker_entered, worker_returned;
static QWORD spin_budget;
static unsigned failures;

static void report(const char *text)
{
    DWORD size = 0, written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), text, size, &written, (void *)0);
}

#define CHECK(condition, label) do { \
    if (!(condition)) { failures++; report("[CB-PREEMPT] FAIL: " label "\n"); } \
} while (0)

static QWORD read_tsc(void)
{
    DWORD lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((QWORD)hi << 32) | lo;
}

/* No system calls or cooperative yields: only the timer can run the worker. */
static int spin_for_heartbeat(void)
{
    DWORD first = heartbeat;
    QWORD start = read_tsc();
    while ((DWORD)(heartbeat - first) < 2) {
        if (read_tsc() - start >= spin_budget)
            return 0;
        __asm__ volatile ("pause");
    }
    return 1;
}

static LRESULT WINAPI window_proc(HWND window, UINT message,
                                   DWORD wparam, LONG lparam)
{
    volatile DWORD canary[4] = {0xA5123456, 0xFEDC0001, 0x1234FFEE, 0x5AA55AA5};
    if (message == WM_TEST_SPIN) {
        CHECK(spin_for_heartbeat(), "worker runs during CPU-bound callback");
        CHECK(canary[0] == 0xA5123456 && canary[3] == 0x5AA55AA5,
              "callback stack survives preemption");
        return RESULT_MARK;
    }
    if (message == WM_TEST_NEST) {
        worker_command = 1;
        Sleep(10);
        CHECK(SendMessageA(window, WM_TEST_SPIN, 0, 0) == RESULT_MARK,
              "nested callback result");
        CHECK(canary[1] == 0xFEDC0001 && canary[2] == 0x1234FFEE,
              "outer callback stack survives nesting");
        return RESULT_MARK;
    }
    if (message == WM_TEST_SLEEP) {
        worker_entered++;
        Sleep(80);
        CHECK(canary[0] == 0xA5123456 && canary[2] == 0x1234FFEE,
              "sleeping callback keeps its private stack");
        worker_returned++;
        return RESULT_MARK;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND create_window(void)
{
    return CreateWindowExA(0, class_name, class_name, 0, 0, 0, 64, 64,
                           (HWND)0, (HANDLE)0, instance, (void *)0);
}

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    HWND window = create_window();
    if (!window) { ready = 2; return 1; }
    ready = 1;
    while (!stop_worker) {
        if (worker_command) {
            worker_command = 0;
            CHECK(SendMessageA(window, WM_TEST_SLEEP, 0, 0) == RESULT_MARK,
                  "worker callback result");
        }
        Sleep(2);
        heartbeat++;
    }
    DestroyWindow(window);
    return 0;
}

void mainCRTStartup(void)
{
    report("[CB-PREEMPT] begin\n");
    instance = GetModuleHandleA((void *)0);
    WNDCLASSEXA cls = {sizeof(cls), 0, window_proc, 0, 0, 0, 0, 0, 0,
                       0, class_name, 0};
    cls.instance = instance;
    if (!RegisterClassExA(&cls)) ExitProcess(1);
    HWND window = create_window();
    if (!window) ExitProcess(2);
    QWORD start = read_tsc();
    Sleep(50);
    spin_budget = (read_tsc() - start) * 20;
    HANDLE thread = CreateThread(0, 0, worker, 0, 0, 0);
    if (!thread) ExitProcess(3);
    for (unsigned i = 0; i < 100 && !ready; i++) Sleep(5);
    CHECK(ready == 1, "worker window created");
    if (ready == 1) {
        CHECK(spin_for_heartbeat(), "baseline timer preemption");
        CHECK(SendMessageA(window, WM_TEST_SPIN, 0, 0) == RESULT_MARK,
              "callback return value");
        for (unsigned i = 0; i < 4; i++) {
            CHECK(SendMessageA(window, WM_TEST_NEST, 0, 0) == RESULT_MARK,
                  "outer callback result");
            CHECK(spin_for_heartbeat(), "timer stays enabled after callbacks");
        }
        CHECK(worker_entered == 4 && worker_returned == 4,
              "overlapping callbacks complete on both threads");
    }
    stop_worker = 1;
    CHECK(WaitForSingleObject(thread, 2000) == 0, "worker exits");
    CloseHandle(thread);
    DestroyWindow(window);
    report(failures ? "[CB-PREEMPT] FAIL\n" : "[CB-PREEMPT] PASS\n");
    ExitProcess(failures ? 4 : 0);
}
