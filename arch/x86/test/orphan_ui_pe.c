/* A child creates its GUI before the parent exits. Returning to the shell
 * must release the parent's resources without resetting the child's USER32. */
typedef unsigned int DWORD, UINT;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
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
typedef IPTR (CALL *WNDPROC)(HANDLE, UINT, UPTR, IPTR);
typedef struct {
    UINT style; WNDPROC proc; int cls_extra, wnd_extra;
    HANDLE instance, icon, cursor, background; const char *menu, *name;
} WNDCLASS;
typedef struct {
    HANDLE window; UINT message; UPTR wp; IPTR lp;
    DWORD time; int x, y; DWORD private_data;
} MSG;
static DWORD failures, timer_count, posted_count;
static DWORD (CALL *get_pid)(void), (CALL *wait_one)(HANDLE, DWORD);
static DWORD (CALL *module_name)(HANDLE, char *, DWORD);
static char *(CALL *command_line)(void);
static HANDLE (CALL *module_handle)(const char *);
static HANDLE (CALL *create_event)(void *, BOOL, BOOL, const char *);
static HANDLE (CALL *open_event)(DWORD, BOOL, const char *);
static HANDLE (CALL *open_process)(DWORD, BOOL, DWORD);
static BOOL (CALL *set_event)(HANDLE), (CALL *close_handle)(HANDLE);
static void (CALL *sleep_ms)(DWORD);
static BOOL (CALL *create_process)(const char *, char *, void *, void *,
                                  BOOL, DWORD, void *, const char *,
                                  STARTUP *, PROCESS *);
static WORD (CALL *register_class)(const WNDCLASS *);
static BOOL (CALL *class_info)(HANDLE, const char *, WNDCLASS *);
static HANDLE (CALL *create_window)(DWORD, const char *, const char *, DWORD,
    int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
static BOOL (CALL *is_window)(HANDLE), (CALL *destroy_window)(HANDLE);
static BOOL (CALL *set_text)(HANDLE, const char *);
static int (CALL *get_text)(HANDLE, char *, int);
static IPTR (CALL *default_proc)(HANDLE, UINT, UPTR, IPTR);
static IPTR (CALL *send_message)(HANDLE, UINT, UPTR, IPTR);
static BOOL (CALL *post_message)(HANDLE, UINT, UPTR, IPTR);
static BOOL (CALL *peek_message)(MSG *, HANDLE, UINT, UINT, UINT);
static IPTR (CALL *dispatch)(const MSG *);
static UPTR (CALL *set_timer)(HANDLE, UPTR, UINT, void *);
static BOOL (CALL *kill_timer)(HANDLE, UPTR);
static HANDLE window;
static STARTUP startup;
static PROCESS child;
static WNDCLASS cls, queried;
static const char class_name[] = "OsitoOrphanUiFixture";

static void orphan_ui_report(const char *text)
{
    DWORD size = 0, written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), text, size, &written, 0);
}

static void orphan_ui_check(BOOL ok, const char *label)
{
    if (!ok) failures++;
    orphan_ui_report(ok ? "ORPHANUI PASS " : "ORPHANUI FAIL ");
    orphan_ui_report(label);
    orphan_ui_report("\n");
}

static IPTR CALL orphan_ui_proc(HANDLE hwnd, UINT message, UPTR wp, IPTR lp)
{
    if (message == 0x113 && wp == 7) { timer_count++; return 0; }
    if (message == 0x8001) return (IPTR)(wp ^ (UPTR)lp);
    if (message == 0x8002) { posted_count++; return 0; }
    return default_proc(hwnd, message, wp, lp);
}

static void orphan_ui_pump(void)
{
    MSG message;
    for (UINT round = 0; round < 100; round++) {
        while (peek_message(&message, 0, 0, 0, 1)) dispatch(&message);
        sleep_ms(10);
    }
}

void orphan_ui_entry(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll"), user = LoadLibraryA("user32.dll");
#define LOAD(dll, var, name) do { \
    var = GetProcAddress(dll, name); \
    if (!var) { orphan_ui_report("ORPHANUI MISSING " name "\n"); ExitProcess(2); } \
} while (0)
    LOAD(kernel, get_pid, "GetCurrentProcessId");
    LOAD(kernel, wait_one, "WaitForSingleObject");
    LOAD(kernel, module_name, "GetModuleFileNameA");
    LOAD(kernel, command_line, "GetCommandLineA");
    LOAD(kernel, module_handle, "GetModuleHandleA");
    LOAD(kernel, create_event, "CreateEventA");
    LOAD(kernel, open_event, "OpenEventA");
    LOAD(kernel, open_process, "OpenProcess");
    LOAD(kernel, set_event, "SetEvent");
    LOAD(kernel, close_handle, "CloseHandle");
    LOAD(kernel, sleep_ms, "Sleep");
    LOAD(kernel, create_process, "CreateProcessA");
    LOAD(user, register_class, "RegisterClassA");
    LOAD(user, class_info, "GetClassInfoA");
    LOAD(user, create_window, "CreateWindowExA");
    LOAD(user, is_window, "IsWindow");
    LOAD(user, destroy_window, "DestroyWindow");
    LOAD(user, set_text, "SetWindowTextA");
    LOAD(user, get_text, "GetWindowTextA");
    LOAD(user, default_proc, "DefWindowProcA");
    LOAD(user, send_message, "SendMessageA");
    LOAD(user, post_message, "PostMessageA");
    LOAD(user, peek_message, "PeekMessageA");
    LOAD(user, dispatch, "DispatchMessageA");
    LOAD(user, set_timer, "SetTimer");
    LOAD(user, kill_timer, "KillTimer");
#undef LOAD
    DWORD parent_pid = 0;
    for (const char *p = command_line(); *p; p++) {
        if (p[0] != '-' || p[1] != '-' || p[2] != 'c' || p[3] != ' ') continue;
        for (UINT i = 0; i < 8; i++) {
            BYTE c = (BYTE)p[4 + i];
            parent_pid = (parent_pid << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
        }
        break;
    }
    char event_name[] = "Local\\OsitoOrphanUi-00000000";
    DWORD owner = parent_pid ? parent_pid : get_pid();
    for (UINT i = 0; i < 8; i++)
        event_name[sizeof(event_name) - 9 + i] = "0123456789abcdef"[(owner >> (28 - i * 4)) & 15];
    if (!parent_pid) {
        HANDLE ready = create_event(0, 1, 0, event_name);
        char path[512], command[] = "orphanui --c 00000000";
        DWORD length = module_name(0, path, sizeof(path));
        for (UINT i = 0; i < 8; i++)
            command[sizeof(command) - 9 + i] = "0123456789abcdef"[(owner >> (28 - i * 4)) & 15];
        startup.cb = sizeof(startup);
        startup.flags = 0x100;
        startup.input = GetStdHandle((DWORD)-10);
        startup.output = GetStdHandle((DWORD)-11);
        startup.error = GetStdHandle((DWORD)-12);
        orphan_ui_check(ready && length && length < sizeof(path), "parent setup");
        BOOL made = !failures && create_process(path, command, 0, 0, 1, 0, 0, 0, &startup, &child);
        orphan_ui_check(made, "create independent GUI child");
        if (made) {
            orphan_ui_check(wait_one(ready, 20000) == 0, "child GUI ready before parent exit");
            close_handle(child.thread); close_handle(child.process);
        }
        close_handle(ready);
        orphan_ui_report("ORPHANUI PARENT EXIT\n");
        ExitProcess(failures ? 1 : 0);
    }
    HANDLE ready = open_event(2, 0, event_name);
    HANDLE parent = open_process(0x100000, 0, parent_pid);
    orphan_ui_check(ready && parent, "child observes parent lifetime");
    cls.proc = orphan_ui_proc;
    cls.instance = module_handle(0);
    cls.name = class_name;
    orphan_ui_check(register_class(&cls) != 0, "register child class");
    /* Hidden windows avoid moving focus on the Windows reference host. */
    window = create_window(0, class_name, "before", 0, 40, 40, 180, 100,
                           0, 0, cls.instance, 0);
    orphan_ui_check(window && is_window(window), "create child window");
    orphan_ui_check(set_timer(window, 7, 50, 0) == 7, "create child timer");
    orphan_ui_check(ready && set_event(ready), "release parent after GUI setup");
    orphan_ui_check(parent && wait_one(parent, 20000) == 0, "parent has exited");
    close_handle(parent); close_handle(ready);
    sleep_ms(750); /* Parent exit is signaled before the shell returns. */
    orphan_ui_check(is_window(window), "window survives shell return");
    orphan_ui_check(class_info(cls.instance, class_name, &queried) &&
                    queried.proc == orphan_ui_proc, "class survives shell return");
    char title[16];
    orphan_ui_check(set_text(window, "after") && get_text(window, title, sizeof(title)) == 5 &&
                    title[0] == 'a' && title[4] == 'r', "window text after parent exit");
    orphan_ui_check(send_message(window, 0x8001, 0x1234, 0x5678) == (0x1234 ^ 0x5678),
                    "child callback after parent exit");
    orphan_ui_check(post_message(window, 0x8002, 0, 0), "post child message after parent exit");
    orphan_ui_pump();
    orphan_ui_check(posted_count == 1, "dispatch child message after parent exit");
    orphan_ui_check(timer_count > 0, "timer survives parent exit");
    orphan_ui_check(kill_timer(window, 7), "release child timer");
    orphan_ui_check(destroy_window(window) && !is_window(window), "release child window");
    orphan_ui_report(failures ? "ORPHANUI CHILD FAILED\n" : "ORPHANUI CHILD DONE\n");
    ExitProcess(failures ? 1 : 0);
}
