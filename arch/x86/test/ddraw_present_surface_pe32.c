/* Osito's window presentation contract through the legacy PE32 COM ABI. */
typedef unsigned int DWORD;
typedef int BOOL, HRESULT;
typedef void *HANDLE;
typedef struct { void **vtable; } COM;
typedef struct { int left, top, right, bottom; } RECT;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void CALL ExitProcess(DWORD);
static DWORD checks, failures;
static void report(const char *text)
{
    DWORD n = 0, written;
    while (text[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), text, n, &written, 0);
}
static void number(DWORD value)
{
    char text[12]; unsigned at = sizeof(text) - 1; text[at] = 0;
    do { text[--at] = '0' + value % 10; value /= 10; } while (value);
    report(text + at);
}
static void check(BOOL condition, const char *name)
{
    checks++;
    if (!condition) { failures++; report("DDRAW SURFACE FAIL: "); report(name); report("\n"); }
}
#define LOAD(module, variable, name) do { \
    *(void **)&variable = GetProcAddress(module, name); \
    if (!variable) { report("DDRAW SURFACE MISSING: " name "\n"); ExitProcess(2); } } while (0)
static HANDLE (CALL *create_window)(DWORD,const char*,const char*,DWORD,int,int,int,int,HANDLE,HANDLE,HANDLE,void*);
static BOOL (CALL *destroy_window)(HANDLE);
static HANDLE (CALL *get_dc)(HANDLE), (CALL *get_window_dc)(HANDLE);
static int (CALL *release_dc)(HANDLE,HANDLE);
static BOOL (CALL *position)(HANDLE,HANDLE,int,int,int,int,DWORD);
static BOOL (CALL *client_rect)(HANDLE,RECT*);
static BOOL (CALL *paint)(HANDLE,int,int,int,int,DWORD);
static DWORD (CALL *pixel)(HANDLE,int,int);
typedef HRESULT (CALL *CREATE_DD)(void *, COM **, void *);
typedef HRESULT (CALL *COOPERATE)(COM *, HANDLE, DWORD);
typedef HRESULT (CALL *CREATE_SURFACE)(COM *, DWORD *, COM **, void *);
typedef HRESULT (CALL *BLT)(COM *, RECT *, COM *, RECT *, DWORD, DWORD *);
typedef DWORD (CALL *RELEASE)(COM *);

void mainCRTStartup(void)
{
    HANDLE user = LoadLibraryA("user32.dll"), gdi = LoadLibraryA("gdi32.dll");
    HANDLE dd_module = LoadLibraryA("ddraw.dll");
    LOAD(user, create_window, "CreateWindowExA"); LOAD(user, destroy_window, "DestroyWindow");
    LOAD(user, get_dc, "GetDC"); LOAD(user, get_window_dc, "GetWindowDC");
    LOAD(user, release_dc, "ReleaseDC"); LOAD(user, position, "SetWindowPos");
    LOAD(user, client_rect, "GetClientRect"); LOAD(gdi, paint, "PatBlt"); LOAD(gdi, pixel, "GetPixel");
    CREATE_DD create;
    LOAD(dd_module, create, "DirectDrawCreate");
    COM *dd = 0;
    check(create(0, &dd, 0) == 0 && dd, "create DirectDraw");
    if (!dd) ExitProcess(1);
    COOPERATE cooperate = (COOPERATE)dd->vtable[20];
    CREATE_SURFACE create_surface = (CREATE_SURFACE)dd->vtable[6];
    HANDLE bootstrap = create_window(0, "STATIC", "DDraw surface test", 0x00cf0000u,
                                       40, 60, 120, 96, 0, 0, 0, 0);
    check(bootstrap && cooperate(dd, bootstrap, 8) == 0, "normal cooperative window");
    DWORD desc[27] = {0}; desc[0] = sizeof(desc); desc[1] = 1; desc[26] = 0x200;
    COM *primary = 0;
    check(create_surface(dd, desc, &primary, 0) == 0 && primary, "create primary");
    check(destroy_window(bootstrap), "destroy bootstrap");
    if (!primary) ExitProcess(1);
    BLT blit = (BLT)primary->vtable[5];
    DWORD fill[25] = {0}; fill[0] = sizeof(fill);
    /* More than 64 backing allocations: leaked presents exhaust the SHM table. */
    for (unsigned i = 0; i < 96; i++) {
        HANDLE window = create_window(0, "STATIC", "Retained DirectDraw target", 0x00cf0000u,
                                         40, 60, 120, 96, 0, 0, 0, 0);
        HANDLE client = window ? get_dc(window) : 0;
        HANDLE whole = window ? get_window_dc(window) : 0;
        check(window && client && whole, "create window/DCs");
        if (!window || !client || !whole) break;
        check(cooperate(dd, window, 8) == 0, "rebind target");
        for (unsigned phase = 0; phase < 2; phase++) {
            int width = 80 + (i % 3) * 17 + phase * 47;
            int height = 64 + (i % 4) * 11 + phase * 23;
            RECT area = {0};
            check(position(window, 0, 40, 60, width, height, 0x041cu), "resize target");
            check(client_rect(window, &area) && area.right > 0 && area.bottom > 0, "client bounds");
            check(paint(whole, 0, 0, width, height, 0x00000042u), "clear nonclient");
            fill[20] = phase ? 0x606060u : 0xffffffu;
            check(blit(primary, 0, 0, 0, 0x400, fill) == 0, "present fill");
            check((pixel(client, area.right - 1, area.bottom - 1) & 0xffffffu) == fill[20], "last resized client pixel");
            check((pixel(whole, 0, 0) & 0xffffffu) == 0, "preserve nonclient pixels");
        }
        check(release_dc(window, client) && release_dc(window, whole), "release DCs");
        check(destroy_window(window), "destroy target");
        check(blit(primary, 0, 0, 0, 0x400, fill) == 0, "present after target destruction");
    }
    check(((RELEASE)primary->vtable[2])(primary) == 0, "release primary");
    check(((RELEASE)dd->vtable[2])(dd) == 0, "release DirectDraw");
    report(failures ? "DDRAW SURFACE CONTRACT FAIL: " : "DDRAW SURFACE CONTRACT PASS: ");
    number(checks); report(" checks\n");
    ExitProcess(failures ? 1 : 0);
}
