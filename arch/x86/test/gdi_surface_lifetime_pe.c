/* Retained window DCs across backing-surface changes, through public Win32 ABI. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct { int x, y; } POINT;
typedef struct { int left, top, right, bottom; } RECT;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API void CALL ExitProcess(DWORD);

static HANDLE (CALL *create)(DWORD,const char*,const char*,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void*);
static BOOL (CALL *destroy)(HANDLE);
static HANDLE (CALL *get_dc)(HANDLE);
static HANDLE (CALL *get_window_dc)(HANDLE);
static int (CALL *release_dc)(HANDLE,HANDLE);
static BOOL (CALL *position)(HANDLE,HANDLE,int,int,int,int,DWORD);
static BOOL (CALL *client_rect)(HANDLE,RECT*);
static BOOL (CALL *window_rect)(HANDLE,RECT*);
static BOOL (CALL *client_to_screen)(HANDLE,POINT*);
static BOOL (CALL *paint)(HANDLE,int,int,int,int,DWORD);
static DWORD (CALL *pixel)(HANDLE,int,int);
static HANDLE (CALL *window_from_dc)(HANDLE);
static DWORD checks, failures;

static void report(const char *text)
{
    DWORD n = 0, written;
    while (text[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), text, n, &written, 0);
}
static void number(DWORD value)
{
    char buffer[16];
    unsigned at = sizeof(buffer) - 1;
    buffer[at] = 0;
    do { buffer[--at] = '0' + value % 10; value /= 10; } while (value);
    report(buffer + at);
}
static void check(BOOL value, const char *text)
{
    checks++;
    if (!value) { report("GDI SURFACE FAIL: "); report(text); report("\n"); failures++; }
}
#define LOAD(module, variable, name) do { \
    *(void **)&variable = GetProcAddress(module, name); \
    if (!variable) { report("GDI SURFACE MISSING: " name "\n"); ExitProcess(2); } } while (0)

void mainCRTStartup(void)
{
    HANDLE user = LoadLibraryA("user32.dll"), gdi = LoadLibraryA("gdi32.dll");
    LOAD(user, create, "CreateWindowExA"); LOAD(user, destroy, "DestroyWindow");
    LOAD(user, get_dc, "GetDC"); LOAD(user, get_window_dc, "GetWindowDC");
    LOAD(user, release_dc, "ReleaseDC"); LOAD(user, position, "SetWindowPos");
    LOAD(user, client_rect, "GetClientRect"); LOAD(user, window_rect, "GetWindowRect");
    LOAD(user, client_to_screen, "ClientToScreen"); LOAD(user, window_from_dc, "WindowFromDC");
    LOAD(gdi, paint, "PatBlt"); LOAD(gdi, pixel, "GetPixel");
    HANDLE window = create(0, "STATIC", "Retained DC", 0x00cf0000u,
                            40, 60, 120, 96, 0, 0, 0, 0);
    HANDLE client = window ? get_dc(window) : 0;
    HANDLE whole = window ? get_window_dc(window) : 0;
    check(window && client && whole, "create retained client/nonclient DCs");
    if (!window || !client || !whole) ExitProcess(1);
    for (unsigned i = 0; i < 32; i++) {
        int width = 80 + (i % 4) * 31, height = 64 + (i % 3) * 17;
        check(position(window, 0, 40, 60, width, height, 0x041cu), "resize");
        RECT area = {0}, frame = {0};
        POINT origin = {0};
        check(client_rect(window, &area) && area.right > 2 && area.bottom > 2, "client dimensions");
        check(window_rect(window, &frame) && client_to_screen(window, &origin), "client origin");
        check(window_from_dc(client) == window && window_from_dc(whole) == window, "retained DC owner");
        check(paint(whole, 0, 0, width, height, 0x00000042u), "clear whole window");
        check(paint(client, 0, 0, area.right, area.bottom, 0x00ff0062u), "paint resized client");
        check((pixel(client, area.right - 1, area.bottom - 1) & 0xffffffu) == 0xffffffu, "last client pixel");
        check((pixel(whole, 0, 0) & 0xffffffu) == 0, "client does not overwrite nonclient origin");
        check((pixel(whole, origin.x - frame.left, origin.y - frame.top) & 0xffffffu) == 0xffffffu,
              "client offset in whole surface");
    }
    check(release_dc(window, client) != 0, "release client DC");
    check(release_dc(window, whole) != 0, "release nonclient DC");
    check(destroy(window), "destroy retained window");
    /* Exceeds the 64-region SHM table: a leaked draw reference exhausts it. */
    for (unsigned i = 0; i < 96; i++) {
        window = create(0, "STATIC", "DC lifetime cycle", 0x80000000u,
                          40, 60, 12, 12, 0, 0, 0, 0);
        check(window != 0, "create cycle window");
        if (!window) break;
        client = get_dc(window);
        check(client != 0, "get cycle DC");
        check(paint(client, 0, 0, 12, 12, 0x00ff0062u), "paint before replacement");
        check(position(window, 0, 40, 60, 24, 20, 0x041cu), "grow cycle window");
        check(paint(client, 0, 0, 24, 20, 0x00000042u), "paint retained DC after growth");
        check((pixel(client, 23, 19) & 0xffffffu) == 0, "grown last pixel");
        check(release_dc(window, client) != 0, "release cycle DC");
        check(destroy(window), "destroy cycle window");
    }
    report(failures ? "GDI SURFACE CONTRACT FAIL: " : "GDI SURFACE CONTRACT PASS: ");
    number(checks); report(" checks\n");
    ExitProcess(failures ? 1 : 0);
}
