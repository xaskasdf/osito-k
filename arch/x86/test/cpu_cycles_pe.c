/* Scheduler-backed Win32 cycle counts: no privileged instructions or hooks. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
#define SELF ((HANDLE)-1)
#define THREAD ((HANDLE)-2)
#define NIL ((void *)0)

typedef struct {
    HANDLE process, thread;
    DWORD pid, tid;
} PROCESS_INFORMATION;

typedef struct {
    DWORD cb;
    char *reserved, *desktop, *title;
    DWORD x, y, width, height, chars_x, chars_y, fill, flags;
    unsigned short show, reserved_bytes;
    void *reserved_data;
    HANDLE input, output, error;
} STARTUPINFO;

IMPORT void WINAPI ExitProcess(DWORD);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT DWORD WINAPI GetCurrentThreadId(void);
IMPORT BOOL WINAPI QueryProcessCycleTime(HANDLE, U64 *);
IMPORT BOOL WINAPI QueryThreadCycleTime(HANDLE, U64 *);
IMPORT DWORD WINAPI GetTickCount(void);
IMPORT void WINAPI Sleep(DWORD);
IMPORT HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD);
IMPORT HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD);
IMPORT BOOL WINAPI DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
IMPORT BOOL WINAPI CloseHandle(HANDLE);
IMPORT HANDLE WINAPI CreateThread(void *, SIZE_T, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
IMPORT DWORD WINAPI ResumeThread(HANDLE);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
IMPORT BOOL WINAPI CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD,
                                 void *, const char *, void *, PROCESS_INFORMATION *);
IMPORT BOOL WINAPI GetExitCodeProcess(HANDLE, DWORD *);
IMPORT BOOL WINAPI TerminateThread(HANDLE, DWORD);

static void print(const char *s)
{
    DWORD size = 0, written;
    while (s[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), s, size, &written, NIL);
}

static void fail(DWORD step)
{
    char s[] = "CPU-CYCLES FAIL 00\r\n";
    s[16] = (char)('0' + step / 10);
    s[17] = (char)('0' + step % 10);
    print(s);
    ExitProcess(step);
    for (;;) { }
}

static U64 tsc(void)
{
    unsigned lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((U64)hi << 32) | lo;
}

static void busy(DWORD ms)
{
    DWORD start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < ms)
        for (volatile unsigned i = 0; i < 10000; i++) { }
}

static U64 process_cycles(HANDLE h)
{
    U64 value = 0xfeedfacecafebeefULL;
    if (!QueryProcessCycleTime(h, &value) || value == 0xfeedfacecafebeefULL) fail(1);
    return value;
}

static U64 thread_cycles(HANDLE h)
{
    U64 value = 0xfeedfacecafebeefULL;
    if (!QueryThreadCycleTime(h, &value) || value == 0xfeedfacecafebeefULL) fail(2);
    return value;
}

static volatile DWORD worker_ready;

static DWORD WINAPI worker(void *arg)
{
    worker_ready = 1;
    if (arg) Sleep(10000);
    else busy(160);
    return 0;
}

void mainCRTStartup(void)
{
#ifdef CPU_CYCLES_CHILD
    U64 before = process_cycles(SELF);
    busy(240);
    if (process_cycles(SELF) <= before || !thread_cycles(THREAD)) fail(3);
    print("CPU-CYCLES CHILD PASS\r\n");
    ExitProcess(0);
#else
    (void)process_cycles(SELF);
    U64 value = 0xfeedfacecafebeefULL;
    if (QueryProcessCycleTime(SELF, NIL) || GetLastError() != 87) fail(4);
    if (QueryThreadCycleTime(THREAD, NIL) || GetLastError() != 87) fail(5);
    if (QueryProcessCycleTime(NIL, &value) || GetLastError() != 6 ||
        value != 0xfeedfacecafebeefULL) fail(6);
    if (QueryThreadCycleTime(SELF, &value) || GetLastError() != 6) fail(7);
    for (unsigned i = 0; i < 3; i++) {
        HANDLE p = OpenProcess(i == 0 ? 0 : i == 1 ? 0x400 : 0x1000,
                               0, GetCurrentProcessId());
        HANDLE t = OpenThread(i == 0 ? 0 : i == 1 ? 0x40 : 0x800,
                              0, GetCurrentThreadId());
        if (!p || !t) fail(8);
        if (!i) {
            if (QueryProcessCycleTime(p, &value) || GetLastError() != 5) fail(9);
            if (QueryThreadCycleTime(t, &value) || GetLastError() != 5) fail(10);
        } else {
            if (!process_cycles(p) || !thread_cycles(t)) fail(11);
        }
        if (!CloseHandle(p) || !CloseHandle(t)) fail(12);
    }
    HANDLE duplicate = NIL;
    if (!DuplicateHandle(SELF, SELF, SELF, &duplicate, 0x1000, 0, 0)) fail(13);
    U64 previous = process_cycles(duplicate);
    for (unsigned i = 0; i < 100; i++) {
        U64 next = process_cycles(duplicate);
        if (next < previous) fail(14);
        previous = next;
    }
    if (QueryThreadCycleTime(duplicate, &value) || GetLastError() != 6) fail(15);
    CloseHandle(duplicate);
    if (QueryProcessCycleTime(duplicate, &value) || GetLastError() != 6) fail(16);
    print("CPU-CYCLES ACCESS PASS\r\n");

    U64 before = thread_cycles(THREAD), wall = tsc();
    Sleep(400);
    U64 slept = thread_cycles(THREAD) - before;
    wall = tsc() - wall;
    if (!wall || slept >= wall / 2) fail(17);
    before = thread_cycles(THREAD);
    busy(300);
    if (thread_cycles(THREAD) <= before + slept) fail(18);
    print("CPU-CYCLES RUN-VS-WAIT PASS\r\n");

    U64 process_before = process_cycles(SELF), worker_total = 0;
    for (unsigned i = 0; i < 6; i++) {
        worker_ready = 0;
        HANDLE t = CreateThread(NIL, 0, worker, i == 5 ? (void *)1 : NIL, 4, NIL);
        if (!t || thread_cycles(t) != 0) fail(19);
        busy(80);
        if (thread_cycles(t) != 0) fail(20);
        if (ResumeThread(t) != 1) fail(21);
        if (i == 5) {
            for (unsigned n = 0; !worker_ready && n < 100; n++) Sleep(10);
            if (!worker_ready || !TerminateThread(t, 77)) fail(22);
        }
        if (WaitForSingleObject(t, 10000) != 0) fail(23);
        U64 stopped = thread_cycles(t);
        if (!stopped) fail(24);
        worker_total += stopped;
        Sleep(50);
        if (thread_cycles(t) != stopped) fail(25);
        U64 p = process_cycles(SELF);
        CloseHandle(t);
        if (process_cycles(SELF) < p) fail(26);
    }
    if (process_cycles(SELF) < process_before + worker_total) fail(27);
    print("CPU-CYCLES THREAD-LIFETIME PASS\r\n");

    PROCESS_INFORMATION child;
    STARTUPINFO startup;
    for (unsigned i = 0; i < sizeof(startup); i++)
        ((unsigned char *)&startup)[i] = 0;
    startup.cb = sizeof(startup);
#ifdef _WIN64
    char command[] = "cycleschild64.exe";
#else
    char command[] = "cycleschild32.exe";
#endif
    if (!CreateProcessA(NIL, command, NIL, NIL, 0, 4, NIL, NIL, &startup, &child)) fail(28);
    if (process_cycles(child.process) || thread_cycles(child.thread)) fail(29);
    busy(100);
    if (process_cycles(child.process) || thread_cycles(child.thread)) fail(30);
    if (ResumeThread(child.thread) != 1) fail(31);
    if (WaitForSingleObject(child.process, 15000) != 0) fail(32);
    DWORD code;
    if (!GetExitCodeProcess(child.process, &code) || code) fail(33);
    U64 stopped = process_cycles(child.process);
    if (!stopped || !thread_cycles(child.thread)) fail(34);
    CloseHandle(child.thread);
    busy(100);
    if (process_cycles(child.process) != stopped) fail(35);
    CloseHandle(child.process);
    print("CPU-CYCLES PROCESS-LIFETIME PASS\r\n");
    print("CPU-CYCLES PASS\r\n");
    ExitProcess(0);
#endif
}
