/* Runs unchanged on Windows and OsitoK. Only creates hidden, process-owned
 * windows and installs a hook on the calling thread. */
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef long LONG;
typedef void *HANDLE;
typedef HANDLE HWND;
typedef LONG LRESULT;
#define WINAPI __attribute__((stdcall))
#define IMPORT __declspec(dllimport)
typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, DWORD, LONG);
typedef LRESULT (WINAPI *HOOKPROC)(int, DWORD, LONG);
typedef struct {
    UINT cbSize, style;
    WNDPROC wndproc;
    int clsExtra, wndExtra;
    HANDLE instance, icon, cursor, background;
    const char *menu, *name;
    HANDLE smallIcon;
} WNDCLASSEXA;
typedef struct { LONG lp; DWORD wp; UINT message; HWND window; } CWPSTRUCT;
typedef struct {
    HWND window;
    UINT message;
    DWORD wp;
    LONG lp;
    DWORD time;
    LONG x, y;
    DWORD private_value;
} MSG;

IMPORT HANDLE WINAPI GetModuleHandleA(const char *);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT DWORD WINAPI GetCurrentThreadId(void);
IMPORT int WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT void WINAPI ExitProcess(UINT);
IMPORT unsigned short WINAPI RegisterClassExA(const WNDCLASSEXA *);
IMPORT HWND WINAPI CreateWindowExA(DWORD, const char *, const char *, DWORD,
                                   int, int, int, int, HWND, HANDLE, HANDLE, void *);
IMPORT int WINAPI DestroyWindow(HWND);
IMPORT int WINAPI IsWindow(HWND);
IMPORT LRESULT WINAPI DefWindowProcA(HWND, UINT, DWORD, LONG);
IMPORT LRESULT WINAPI SendMessageA(HWND, UINT, DWORD, LONG);
IMPORT HWND WINAPI SetFocus(HWND);
IMPORT HWND WINAPI GetFocus(void);
IMPORT int WINAPI PeekMessageA(MSG *, HWND, UINT, UINT, UINT);
IMPORT LRESULT WINAPI DispatchMessageA(const MSG *);
IMPORT LRESULT WINAPI CallWindowProcA(WNDPROC, HWND, UINT, DWORD, LONG);
IMPORT LRESULT WINAPI CallWindowProcW(WNDPROC, HWND, UINT, DWORD, LONG);
IMPORT LONG WINAPI SetWindowLongA(HWND, int, LONG);
IMPORT LONG WINAPI GetWindowLongA(HWND, int);
IMPORT HANDLE WINAPI SetWindowsHookExA(int, HOOKPROC, HANDLE, DWORD);
IMPORT int WINAPI UnhookWindowsHookEx(HANDLE);
IMPORT LRESULT WINAPI CallNextHookEx(HANDLE, int, DWORD, LONG);

#define WM_CREATE 1U
#define WM_COMMAND 0x0111U
#define WM_SYSCOMMAND 0x0112U
#define WM_SYSKEYDOWN 0x0104U
#define WM_CLOSE 0x0010U
#define SC_CLOSE 0xF060U
#define WM_CHAIN 0x8011U
#define WM_NEST  0x8012U
#define BM_CLICK 0x00F5U
#define GWL_WNDPROC (-4)
#define RESULT (-123L)
#define ARG_W 0xC1234567UL
#define ARG_L ((LONG)0x87654321UL)

static const char class_name[] = "OsitoSubclassContract";
static HANDLE instance;
static HWND button;
static WNDPROC previous_a, previous_b, previous_button;
static unsigned failures, checks, order, hook_calls, commands;
static int reject_create;
static unsigned system_commands, close_requests;
static int consume_system_command;

static void report(const char *text)
{
    DWORD count = 0, written;
    while (text[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11), text, count, &written, (void *)0);
}

static void report_hex(DWORD value)
{
    char text[10];
    for (unsigned i = 0; i < 8; i++)
        text[i] = "0123456789ABCDEF"[(value >> ((7 - i) * 4)) & 15];
    text[8] = '\n';
    text[9] = 0;
    report(text);
}

#define CHECK(condition, label) do { \
    checks++; \
    if (!(condition)) { failures++; report("[SUBCLASS] FAIL: " label "\n"); } \
} while (0)

static LRESULT WINAPI base_proc(HWND window, UINT message, DWORD wp, LONG lp)
{
    if (message == WM_CREATE && reject_create) return -1;
    if (message == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE) {
        system_commands++;
        if (consume_system_command) return 0;
    }
    if (message == WM_CLOSE) {
        close_requests++;
        return 0;
    }
    if (message == WM_CHAIN) {
        order = order * 10 + 3;
        CHECK(wp == ARG_W && lp == ARG_L, "forward all argument bits");
        return RESULT;
    }
    if (message == WM_NEST) {
        volatile DWORD canary = 0x5AA55AA5;
        LRESULT result = SendMessageA(window, WM_CHAIN, ARG_W, ARG_L);
        CHECK(canary == 0x5AA55AA5, "nested callback preserves stack");
        return result;
    }
    if (message == WM_COMMAND && (wp & 0xFFFF) == 101) {
        CHECK((HWND)lp == button && (wp >> 16) == 0,
              "native button notifies its parent");
        commands++;
        return 0;
    }
    return DefWindowProcA(window, message, wp, lp);
}

static LRESULT WINAPI subclass_a(HWND window, UINT message, DWORD wp, LONG lp)
{
    if (message == WM_CHAIN) order = order * 10 + 2;
    return CallWindowProcA(previous_a, window, message, wp, lp);
}

static LRESULT WINAPI subclass_b(HWND window, UINT message, DWORD wp, LONG lp)
{
    if (message == WM_CHAIN) order = order * 10 + 1;
    return CallWindowProcA(previous_b, window, message, wp, lp);
}

static LRESULT WINAPI button_proc(HWND window, UINT message, DWORD wp, LONG lp)
{
    return CallWindowProcA(previous_button, window, message, wp, lp);
}

static LRESULT WINAPI call_hook(int code, DWORD wp, LONG lp)
{
    const CWPSTRUCT *message = (const CWPSTRUCT *)lp;
    if (code >= 0 && message->message == WM_CHAIN) hook_calls++;
    return CallNextHookEx((HANDLE)0, code, wp, lp);
}

static HWND create_window(void)
{
    return CreateWindowExA(0, class_name, class_name, 0, 0, 0, 64, 64,
                           (HWND)0, (HANDLE)0, instance, (void *)0);
}

static void check_close_shortcut(HWND target, HWND root)
{
    MSG message = { 0 };
    PeekMessageA(&message, root, 0, 0, 0);
    SetFocus(target);
    CHECK(GetFocus() == target, "close shortcut has a focused input target");
    system_commands = close_requests = 0;
    consume_system_command = 1;
    SendMessageA(target, WM_SYSKEYDOWN, 0x73, 0x203E0001);
    CHECK(system_commands == 0 && close_requests == 0,
          "Alt-F4 posts rather than synchronously closes");
    int queued = PeekMessageA(&message, root, WM_SYSCOMMAND, WM_SYSCOMMAND, 1);
    if (!queued || message.window != root || message.wp != SC_CLOSE) {
        report("[SUBCLASS] queued/message/wp/window/root:\n");
        report_hex(queued);
        report_hex(message.message);
        report_hex(message.wp);
        report_hex((DWORD)message.window);
        report_hex((DWORD)root);
    }
    CHECK(queued && message.window == root && message.wp == SC_CLOSE,
          "Alt-F4 targets the root with SC_CLOSE");
    if (queued) DispatchMessageA(&message);
    CHECK(system_commands == 1 && close_requests == 0 && IsWindow(root),
          "application can consume the close system command");
    consume_system_command = 0;
    SendMessageA(root, WM_SYSCOMMAND, SC_CLOSE, 0);
    CHECK(close_requests == 1 && IsWindow(root),
          "application can cancel WM_CLOSE");
    SendMessageA(target, WM_SYSKEYDOWN, 0x73, 0x003E0001);
    CHECK(!PeekMessageA(&message, root, WM_SYSCOMMAND, WM_SYSCOMMAND, 1),
          "F4 without Alt does not request close");
}

void mainCRTStartup(void)
{
    report("[SUBCLASS] begin\n");
    instance = GetModuleHandleA((void *)0);
    WNDCLASSEXA cls = {sizeof(cls), 0, base_proc, 0, 0, 0, 0, 0, 0,
                       0, class_name, 0};
    cls.instance = instance;
    if (!RegisterClassExA(&cls)) ExitProcess(1);
    HWND first = create_window(), second = create_window();
    if (!first || !second) ExitProcess(2);
    previous_a = (WNDPROC)SetWindowLongA(first, GWL_WNDPROC, (LONG)subclass_a);
    previous_b = (WNDPROC)SetWindowLongA(first, GWL_WNDPROC, (LONG)subclass_b);
    CHECK(previous_a == base_proc && previous_b == subclass_a,
          "SetWindowLong returns the previous procedure");
    CHECK((WNDPROC)GetWindowLongA(first, GWL_WNDPROC) == subclass_b,
          "GetWindowLong returns the current procedure");
    HANDLE hook = SetWindowsHookExA(4, call_hook, 0, GetCurrentThreadId());
    CHECK(hook != (HANDLE)0, "install thread-local hook");
    for (unsigned i = 0; i < 32; i++) {
        order = hook_calls = 0;
        CHECK(SendMessageA(first, WM_CHAIN, ARG_W, ARG_L) == RESULT,
              "subclass chain preserves signed result");
        CHECK(order == 123, "subclass chain executes in order");
        CHECK(hook_calls == 1, "one hook notification per sent message");
    }
    order = hook_calls = 0;
    CHECK(CallWindowProcW(previous_a, first, WM_CHAIN, ARG_W, ARG_L) == RESULT,
          "CallWindowProcW invokes an unconverted numeric message");
    CHECK(order == 3 && hook_calls == 0,
          "direct previous-procedure call does not send a message");
    order = hook_calls = 0;
    CHECK(SendMessageA(first, WM_NEST, 0, 0) == RESULT && order == 123,
          "nested SendMessage traverses the installed chain");
    CHECK(hook_calls == 1, "nested chain does not duplicate hook calls");
    order = 0;
    CHECK(SendMessageA(second, WM_CHAIN, ARG_W, ARG_L) == RESULT && order == 3,
          "subclassing does not affect other windows of the class");
    report("[SUBCLASS] close: top-level\n");
    SetWindowLongA(first, -16, 0x00080000); /* WS_SYSMENU */
    check_close_shortcut(first, first);
    CHECK((WNDPROC)SetWindowLongA(first, GWL_WNDPROC, (LONG)previous_b) ==
              subclass_b, "remove top subclass");
    order = 0;
    CHECK(SendMessageA(first, WM_CHAIN, ARG_W, ARG_L) == RESULT && order == 23,
          "restored procedure remains callable");
    SetWindowLongA(first, GWL_WNDPROC, (LONG)previous_a);

    button = CreateWindowExA(0, "BUTTON", "probe", 0x40000000UL,
                            0, 0, 32, 24, first, (HANDLE)101, instance, 0);
    CHECK(button != (HWND)0, "create native control");
    if (button) {
        previous_button = (WNDPROC)SetWindowLongA(
            button, GWL_WNDPROC, (LONG)button_proc);
        CHECK(previous_button != (WNDPROC)0, "retrieve native control procedure");
        SendMessageA(button, BM_CLICK, 0, 0);
        CHECK(commands == 1, "forward native control behavior through subclass");
        report("[SUBCLASS] close: native button\n");
        check_close_shortcut(button, first);
        DestroyWindow(button);
    }
    cls.style = 0x0200; /* CS_NOCLOSE */
    cls.name = "OsitoNoCloseContract";
    CHECK(RegisterClassExA(&cls) != 0, "register no-close class");
    HWND no_close = CreateWindowExA(0, cls.name, cls.name, 0,
                                    0, 0, 32, 24, 0, 0, instance, 0);
    CHECK(no_close != (HWND)0, "create no-close window");
    if (no_close) {
        MSG message = { 0 };
        SetFocus(no_close);
        CHECK(GetFocus() == no_close, "no-close window has keyboard focus");
        SendMessageA(no_close, WM_SYSKEYDOWN, 0x73, 0x203E0001);
        CHECK(!PeekMessageA(&message, no_close, WM_SYSCOMMAND, WM_SYSCOMMAND, 1),
              "CS_NOCLOSE suppresses Alt-F4");
        DestroyWindow(no_close);
    }
    reject_create = 1;
    HWND rejected = create_window();
    CHECK(rejected == (HWND)0, "WM_CREATE minus one rejects window creation");
    if (rejected) DestroyWindow(rejected);
    if (hook) UnhookWindowsHookEx(hook);
    DestroyWindow(first);
    DestroyWindow(second);
    CHECK(!IsWindow(first) && !IsWindow(second), "destroy test windows");
    report(failures ? "[SUBCLASS] FAIL\n" : "[SUBCLASS] PASS\n");
    ExitProcess(failures ? 3 : 0);
}
