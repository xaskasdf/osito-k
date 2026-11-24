/* Public USER32 contract: a hidden owner is not a hidden parent. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct { int x, y; } POINT;
typedef struct { HANDLE window; DWORD message; __UINTPTR_TYPE__ wparam;
    __INTPTR_TYPE__ lparam; DWORD time; POINT point; DWORD private_data; } MSG;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API void CALL ExitProcess(DWORD);
#define SAY(text) do { DWORD written; \
    WriteFile(GetStdHandle((DWORD)-11), text, sizeof(text)-1, &written, 0); } while (0)
#define CHECK(condition, text) do { if (condition) { SAY("OWNER-VIS PASS " text "\n"); } \
    else { SAY("OWNER-VIS FAIL " text "\n"); failures++; } } while (0)
#define LOAD(variable, name) do { *(void **)&variable = GetProcAddress(user, name); \
    if (!variable) { SAY("OWNER-VIS MISSING " name "\n"); ExitProcess(2); } } while (0)
#define SETTLE() do { for (int n=0;n<40;n++) { MSG message; \
    while (peek(&message,0,0,0,1)) dispatch(&message); sleep_ms(10); } } while (0)

void owner_visibility_entry(void)
{
    unsigned failures = 0;
    HANDLE user = LoadLibraryA("user32.dll");
    HANDLE (CALL *create)(DWORD,const char*,const char*,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void*);
    BOOL (CALL *destroy)(HANDLE), (CALL *show)(HANDLE,int), (CALL *visible)(HANDLE);
    HANDLE (CALL *at_point)(POINT);
    BOOL (CALL *position)(HANDLE,HANDLE,int,int,int,int,DWORD);
    BOOL (CALL *peek)(MSG*,HANDLE,DWORD,DWORD,DWORD);
    __INTPTR_TYPE__ (CALL *dispatch)(const MSG*);
    void (CALL *sleep_ms)(DWORD);
    LOAD(create, "CreateWindowExA"); LOAD(destroy, "DestroyWindow");
    LOAD(show, "ShowWindow"); LOAD(visible, "IsWindowVisible");
    LOAD(at_point, "WindowFromPoint"); LOAD(position, "SetWindowPos");
    LOAD(peek, "PeekMessageA"); LOAD(dispatch, "DispatchMessageA");
    *(void **)&sleep_ms = GetProcAddress(LoadLibraryA("kernel32.dll"), "Sleep");
    if (!sleep_ms) ExitProcess(2);
    HANDLE owner = create(0, "BUTTON", "hidden owner", 0x80000000u,
                          400,160,40,40,0,0,0,0);
    HANDLE popup = create(0x80, "BUTTON", "owned popup", 0x80000000u,
                          64,96,128,80,owner,0,0,0);
    CHECK(owner && popup, "create owner and popup");
    if (!owner || !popup) ExitProcess(1);
    position(popup, (HANDLE)(__INTPTR_TYPE__)-1,0,0,0,0,0x13);
    show(popup, 8);
    CHECK(!visible(owner) && visible(popup), "hidden owner preserves popup visibility");
    CHECK(at_point((POINT){150,150}) == popup, "hidden owner permits popup hit");
    HANDLE child = create(0, "BUTTON", "child", 0x50000000u,
                          4,4,40,24,popup,0,0,0);
    CHECK(child && at_point((POINT){72,104}) == child, "owned popup child hit");
    show(owner, 4);
    show(owner, 0);
    CHECK(visible(popup) && at_point((POINT){150,150}) == popup,
          "hiding shown owner preserves popup hit");
    show(popup, 0);
    CHECK(!visible(popup) && !visible(child) &&
          at_point((POINT){72,104}) != child,
          "hidden parent hides child");
    show(popup, 8);
    CHECK(visible(child) && at_point((POINT){72,104}) == child,
          "reshown popup restores child hit");
    show(owner, 6);
    SETTLE();
    HANDLE minimized_hit = at_point((POINT){72,104});
    CHECK(at_point((POINT){150,150}) != popup && minimized_hit != child,
          "minimized owner suppresses popup and descendants");
    show(owner, 9);
    SETTLE();
    CHECK(at_point((POINT){150,150}) == popup &&
          at_point((POINT){72,104}) == child,
          "restored owner restores popup and descendants");
    show(owner, 0);
    CHECK(at_point((POINT){150,150}) == popup,
          "rehidden owner keeps restored popup");
    destroy(owner);
    SAY("OWNER-VIS DONE\n");
    ExitProcess(failures ? 1 : 0);
}
