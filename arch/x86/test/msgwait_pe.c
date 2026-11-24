/* USER wait-any polling contracts on native Windows and OsitoK. */
typedef unsigned int DWORD, UINT;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API HANDLE CALL CreateEventA(void *, BOOL, BOOL, const char *);
API BOOL CALL SetEvent(HANDLE);
API BOOL CALL ResetEvent(HANDLE);
API BOOL CALL CloseHandle(HANDLE);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API DWORD CALL GetCurrentThreadId(void);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API char *CALL GetCommandLineA(void);

typedef struct {
    HANDLE hwnd; UINT message; UPTR wparam, lparam;
    DWORD time; LONG x, y; DWORD private_data;
} MSG;
typedef DWORD (CALL *MSGWAITEX)(DWORD, const HANDLE *, DWORD, DWORD, DWORD);
typedef DWORD (CALL *MSGWAIT)(DWORD, const HANDLE *, BOOL, DWORD, DWORD);
typedef BOOL (CALL *PEEK)(MSG *, HANDLE, UINT, UINT, UINT);
typedef BOOL (CALL *POST)(DWORD, UINT, UPTR, UPTR);
static MSGWAITEX wait_ex;
static MSGWAIT wait_plain;
static PEEK peek;
static POST post;
static DWORD checks, failures;

static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}
static void hex(DWORD value)
{
    char text[11] = "0x00000000";
    for (unsigned i = 0; i < 8; i++)
        text[2+i] = "0123456789ABCDEF"[(value >> (28-4*i)) & 15];
    report(text);
}
static void check(BOOL condition, const char *name, DWORD value)
{
    checks++;
    if (!condition) failures++;
    report(condition ? "MSGWAIT PASS " : "MSGWAIT FAIL ");
    report(name); report(" value="); hex(value); report("\n");
}
static void drain(void)
{
    MSG message;
    while (peek(&message, 0, 0, 0, 1)) { }
}

void mainCRTStartup(void)
{
    HANDLE user = LoadLibraryA("user32.dll");
    wait_ex = (MSGWAITEX)GetProcAddress(user, "MsgWaitForMultipleObjectsEx");
    wait_plain = (MSGWAIT)GetProcAddress(user, "MsgWaitForMultipleObjects");
    peek = (PEEK)GetProcAddress(user, "PeekMessageA");
    post = (POST)GetProcAddress(user, "PostThreadMessageA");
    if (!wait_ex || !wait_plain || !peek || !post) ExitProcess(3);
    drain();
    HANDLE events[2] = {CreateEventA(0, 0, 1, 0), CreateEventA(0, 0, 0, 0)};
    if (!events[0] || !events[1]) ExitProcess(4);
    DWORD result = wait_ex(1, events, 0, 0, 0);
    check(result == 0, "zero_timeout_signaled", result);
    result = WaitForSingleObject(events[0], 0);
    check(result == 258, "successful_wait_consumes_auto_event", result);
    ResetEvent(events[0]); SetEvent(events[1]);
    result = wait_ex(2, events, 0, 0, 0);
    check(result == 1, "zero_timeout_second_index", result);
    result = wait_ex(2, events, 0, 0, 0);
    check(result == 258, "zero_timeout_unsignaled", result);
    SetEvent(events[0]);
    result = wait_ex(1, events, 80, 0, 0);
    check(result == 0, "finite_timeout_signaled_control", result);
    HANDLE invalid = (HANDLE)(UPTR)0x12345678;
    SetLastError(0);
    result = wait_ex(1, &invalid, 0, 0, 0);
    DWORD error = GetLastError();
    check(result == (DWORD)-1 && error == 6, "zero_timeout_invalid_handle", result);

    SetEvent(events[0]);
    result = wait_plain(1, events, 0, 0, 0);
    check(result == 0, "plain_zero_timeout_signaled", result);
    ResetEvent(events[0]); ResetEvent(events[1]);
    check(post(GetCurrentThreadId(), 0x8002, 0, 0), "post_input_message", 0);
    result = wait_ex(1, events, 0, 8, 4);
    check(result == 1, "queue_without_ready_object", result);
    drain();
    result = wait_ex(0, 0, 0, 8, 4);
    check(result == 258, "empty_queue_zero_timeout", result);
    /* Optional bounded parking point for the read-only SSH snapshot test. */
    const char *command = GetCommandLineA();
    for (DWORD i = 0; command[i]; i++) {
        if (command[i] == ' ' && command[i+1] == 'h' &&
            command[i+2] == 'o' && command[i+3] == 'l' &&
            command[i+4] == 'd' && !command[i+5]) {
            report("MSGWAIT HOLD\n");
            WaitForSingleObject(events[1], 15000);
            break;
        }
    }
    CloseHandle(events[0]); CloseHandle(events[1]);
    report("MSGWAIT DONE checks="); hex(checks);
    report(" failures="); hex(failures); report("\n");
    ExitProcess(failures ? 1 : 0);
}
