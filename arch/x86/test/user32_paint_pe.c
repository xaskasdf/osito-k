/* Identical USER32 paint-cycle probe for Windows and OsitoK, PE32 and PE64. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef __INTPTR_TYPE__ IPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { LONG left, top, right, bottom; } RECT;
typedef IPTR (CALL *WNDPROC)(HANDLE, UINT, UPTR, IPTR);
typedef struct {
    UINT style; WNDPROC proc; int cls_extra, wnd_extra;
    HANDLE instance, icon, cursor, background;
    const char *menu, *name;
} WNDCLASS;
typedef struct {
    HANDLE dc; BOOL erase; RECT rect; BOOL restore, incremental;
    unsigned char reserved[32];
} PAINTSTRUCT;
typedef struct {
    DWORD size; LONG width, height; unsigned short planes, bpp;
    DWORD compression, image_size; LONG xppm, yppm; DWORD used, important;
} BITMAPINFO;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL GetModuleHandleA(const char *);
API unsigned short CALL RegisterClassA(const WNDCLASS *);
API BOOL CALL UnregisterClassA(const char *, HANDLE);
API HANDLE CALL CreateWindowExA(DWORD, const char *, const char *, DWORD,
                               int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
API BOOL CALL DestroyWindow(HANDLE);
API IPTR CALL DefWindowProcA(HANDLE, UINT, UPTR, IPTR);
API IPTR CALL SendMessageA(HANDLE, UINT, UPTR, IPTR);
API BOOL CALL InvalidateRect(HANDLE, const RECT *, BOOL);
API BOOL CALL ValidateRect(HANDLE, const RECT *);
API BOOL CALL GetUpdateRect(HANDLE, RECT *, BOOL);
API BOOL CALL UpdateWindow(HANDLE);
API HANDLE CALL CreateDialogIndirectParamA(HANDLE, const void *, HANDLE, WNDPROC, IPTR);
API DWORD CALL GetSysColor(int);
API HANDLE CALL BeginPaint(HANDLE, PAINTSTRUCT *);
API BOOL CALL EndPaint(HANDLE, const PAINTSTRUCT *);
API HANDLE CALL GetDC(HANDLE);
API int CALL ReleaseDC(HANDLE, HANDLE);
API int CALL FillRect(HANDLE, const RECT *, HANDLE);
API HANDLE CALL CreateSolidBrush(DWORD);
API HANDLE CALL CreateCompatibleDC(HANDLE);
API HANDLE CALL CreateDIBSection(HANDLE, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
API HANDLE CALL SelectObject(HANDLE, HANDLE);
API HANDLE CALL CreateRectRgn(int, int, int, int);
API int CALL SelectClipRgn(HANDLE, HANDLE);
API BOOL CALL BitBlt(HANDLE, int, int, int, int, HANDLE, int, int, DWORD);
API BOOL CALL DeleteObject(HANDLE);
API BOOL CALL DeleteDC(HANDLE);
API BOOL CALL GdiFlush(void);
API DWORD CALL GetObjectType(HANDLE);

void *memset(void *destination, int value, __SIZE_TYPE__ size)
{
    unsigned char *bytes = destination;
    for (__SIZE_TYPE__ i = 0; i < size; i++) bytes[i] = (unsigned char)value;
    return destination;
}
void *memcpy(void *destination, const void *source, __SIZE_TYPE__ size)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (__SIZE_TYPE__ i = 0; i < size; i++) out[i] = in[i];
    return destination;
}
static unsigned checks, failures, erases, paints;
static int erase_policy, paint_action, reinvalidate, nested_paint, destroy_on_erase;
static BOOL region_in_erase;
static BOOL destroyed;
static PAINTSTRUCT observed;
static HANDLE background, sentinel, ink, capture_dc, source_dc;
static DWORD *capture_bits;
static unsigned dialog_colors;
static HANDLE dialog_brush;
static const RECT all = {0, 0, 160, 128};
static const RECT dirty = {10, 12, 30, 36}, again = {70, 72, 90, 96};

static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}
static void number(unsigned value)
{
    char text[12]; unsigned used = 0;
    do { text[used++] = '0' + value % 10; value /= 10; } while (value);
    while (used) { char digit[2] = {text[--used], 0}; report(digit); }
}
static void check(BOOL condition, const char *name)
{
    checks++;
    if (!condition) { failures++; report("FAIL: "); report(name); report("\n"); }
}
static BOOL equal(RECT a, RECT b)
{
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}
static DWORD rgb(DWORD color)
{
    return ((color & 255) << 16) | (color & 0xFF00) | ((color >> 16) & 255);
}
static DWORD pixel(HANDLE window, int x, int y)
{
    HANDLE dc = GetDC(window);
    capture_bits[0] = 0xABCDEF;
    check(BitBlt(capture_dc, 0, 0, 1, 1, dc, x, y, 0x00CC0020), "capture pixel");
    GdiFlush();
    ReleaseDC(window, dc);
    return capture_bits[0] & 0xFFFFFF;
}
static IPTR CALL wndproc(HANDLE window, UINT msg, UPTR wp, IPTR lp)
{
    if (msg == 0x14) {
        erases++;
        region_in_erase = GetUpdateRect(window, 0, 0);
        if (nested_paint) {
            nested_paint = 0;
            PAINTSTRUCT nested;
            HANDLE dc = BeginPaint(window, &nested);
            RECT empty = {0};
            check(dc && equal(nested.rect, empty), "nested BeginPaint sees consumed update");
            FillRect(dc, &all, ink);
            EndPaint(window, &nested);
        }
        if (destroy_on_erase) {
            destroy_on_erase = 0;
            destroyed = 1;
            DestroyWindow(window);
            return 1;
        }
        if (reinvalidate) {
            reinvalidate = 0;
            InvalidateRect(window, &again, 1);
        }
        if (erase_policy == 1) return 0;
        if (erase_policy == 2) { FillRect((HANDLE)wp, &all, ink); return 1; }
    }
    if (msg == 0xF) {
        paints++;
        if (paint_action == 3) return DefWindowProcA(window, msg, wp, lp);
        PAINTSTRUCT ps;
        HANDLE dc = BeginPaint(window, &ps);
        check(dc != 0 && dc == ps.dc, "BeginPaint returns PAINTSTRUCT HDC");
        observed = ps;
        if (paint_action == 1 || paint_action == 2) SelectClipRgn(dc, 0);
        if (paint_action == 1 || paint_action == 4) FillRect(dc, &all, ink);
        if (paint_action == 2)
            BitBlt(dc, 0, 0, 160, 128, source_dc, 0, 0, 0x00CC0020);
        BOOL ended = EndPaint(window, &ps);
        if (!destroyed) check(ended, "EndPaint succeeds");
        return 0;
    }
    return DefWindowProcA(window, msg, wp, lp);
}
static void reset(HANDLE window)
{
    ValidateRect(window, 0);
    HANDLE dc = GetDC(window);
    SelectClipRgn(dc, 0);
    FillRect(dc, &all, sentinel);
    GdiFlush();
    ReleaseDC(window, dc);
    erases = paints = 0;
    erase_policy = paint_action = reinvalidate = nested_paint = destroy_on_erase = 0;
    region_in_erase = 0;
    destroyed = 0;
}
static HANDLE create(const char *name)
{
    return CreateWindowExA(0x08000080, name, "Paint contract", 0x90000000,
                          80, 120, 160, 128, 0, 0, GetModuleHandleA(0), 0);
}

static IPTR CALL dialog_proc(HANDLE window, UINT msg, UPTR wp, IPTR lp)
{
    if (msg == 0x0136) {
        dialog_colors++;
        check(wp && (HANDLE)lp == window, "WM_CTLCOLORDLG carries dialog and DC");
        return (IPTR)dialog_brush;
    }
    return 0;
}

static void dialog_paint(void)
{
    const struct __attribute__((packed)) {
        DWORD style, extended;
        unsigned short controls;
        short x, y, width, height;
        unsigned short menu, cls, title;
    } template = {0x90000000, 0x08000080, 0, 80, 120, 80, 64, 0, 0, 0};
    HANDLE dialog = CreateDialogIndirectParamA(GetModuleHandleA(0), &template,
                                               0, dialog_proc, 0);
    if (!dialog) ExitProcess(2);
    UpdateWindow(dialog);
    reset(dialog);
    dialog_colors = 0;
    InvalidateRect(dialog, &dirty, 1);
    UpdateWindow(dialog);
    check(dialog_colors == 1, "dialog default paint queries background brush");
    check(pixel(dialog, 15, 16) == rgb(GetSysColor(15)) &&
          pixel(dialog, 5, 5) == rgb(0x00112233), "default dialog brush respects paint clip");
    dialog_brush = ink;
    InvalidateRect(dialog, &dirty, 1);
    UpdateWindow(dialog);
    check(dialog_colors == 2, "dialog background callback receives next paint");
    check(pixel(dialog, 15, 16) == rgb(0x00DDAA77) &&
          pixel(dialog, 5, 5) == rgb(0x00112233), "dialog uses application brush without deleting it");
    DestroyWindow(dialog);
    dialog_brush = 0;
}

void mainCRTStartup(void)
{
    background = CreateSolidBrush(0x006B4523);
    sentinel = CreateSolidBrush(0x00112233);
    ink = CreateSolidBrush(0x00DDAA77);
    capture_dc = CreateCompatibleDC(0);
    source_dc = CreateCompatibleDC(0);
    BITMAPINFO info = {40, 160, -128, 1, 32, 0, 0, 0, 0, 0, 0};
    DWORD *source_bits;
    HANDLE cap_bitmap = CreateDIBSection(capture_dc, &info, 0, (void **)&capture_bits, 0, 0);
    HANDLE src_bitmap = CreateDIBSection(source_dc, &info, 0, (void **)&source_bits, 0, 0);
    if (!background || !sentinel || !ink || !cap_bitmap || !src_bitmap) ExitProcess(2);
    HANDLE cap_old = SelectObject(capture_dc, cap_bitmap);
    HANDLE src_old = SelectObject(source_dc, src_bitmap);
    for (UINT i = 0; i < 160*128; i++) source_bits[i] = rgb(0x00DDAA77);
    WNDCLASS cls = {0x20, wndproc, 0, 0, GetModuleHandleA(0), 0, 0,
                    background, 0, "OsitoPaintContract"};
    if (!RegisterClassA(&cls)) ExitProcess(2);
    HANDLE window = create(cls.name);
    if (!window) ExitProcess(2);
    UpdateWindow(window);

    reset(window);
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(paints == 1 && erases == 1, "BeginPaint sends one erase callback");
    check(!region_in_erase, "update region is consumed before erase callback");
    check(!observed.erase && equal(observed.rect, dirty), "erased PAINTSTRUCT and client bounds");
    check(pixel(window, 15, 16) == rgb(0x006B4523), "class brush erases dirty pixels");
    check(pixel(window, 5, 5) == rgb(0x00112233), "erase excludes valid pixels");
    RECT query = {1, 2, 3, 4}, empty = {0};
    check(!GetUpdateRect(window, &query, 0) && equal(query, empty), "empty update query zeros RECT");

    reset(window);
    InvalidateRect(window, &dirty, 0);
    UpdateWindow(window);
    check(erases == 0 && !observed.erase, "erase-false invalidation does not erase");
    reset(window);
    erase_policy = 1;
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(erases == 1 && observed.erase, "unhandled erase leaves PAINTSTRUCT fErase set");
    check(pixel(window, 15, 16) == rgb(0x00112233), "unhandled erase leaves pixels intact");
    reset(window);
    erase_policy = 2;
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(erases == 1 && !observed.erase, "application erase suppresses further erasing");
    check(pixel(window, 15, 16) == rgb(0x00DDAA77) &&
          pixel(window, 5, 5) == rgb(0x00112233), "application erase uses update clip");

    for (int action = 1; action <= 2; action++) {
        reset(window);
        paint_action = action;
        InvalidateRect(window, &dirty, 0);
        UpdateWindow(window);
        check(pixel(window, 15, 16) == rgb(0x00DDAA77) &&
              pixel(window, 5, 5) == rgb(0x00112233), "fill/blit cannot escape paint clip");
    }
    reset(window);
    paint_action = 1;
    SendMessageA(window, 0xF, 0, 0);
    check(equal(observed.rect, empty) && !observed.erase && erases == 0,
          "BeginPaint without update produces an empty paint region");
    check(pixel(window, 5, 5) == rgb(0x00112233), "empty paint DC draws nothing");

    reset(window);
    reinvalidate = 1;
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(GetUpdateRect(window, &query, 0) && equal(query, again),
          "callback reinvalidation survives BeginPaint/EndPaint");
    UpdateWindow(window);
    check(erases == 2 && !GetUpdateRect(window, 0, 0), "next paint handles reinvalidated region");

    reset(window);
    InvalidateRect(window, &dirty, 1);
    check(GetUpdateRect(window, &query, 1) && equal(query, dirty) && erases == 1,
          "GetUpdateRect erase dispatches without validating");
    UpdateWindow(window);
    check(erases == 1 && !observed.erase, "query erase is not repeated by BeginPaint");

    reset(window);
    paint_action = 3;
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(erases == 1 && !GetUpdateRect(window, 0, 0), "default WM_PAINT performs pending erase");
    check(pixel(window, 15, 16) == rgb(0x006B4523), "default WM_PAINT draws class background");

    reset(window);
    HANDLE own = GetDC(window);
    HANDLE region = CreateRectRgn(20, 0, 160, 128);
    SelectClipRgn(own, region);
    DeleteObject(region);
    paint_action = 4;
    InvalidateRect(window, &dirty, 0);
    UpdateWindow(window);
    SelectClipRgn(own, 0);
    check(pixel(window, 15, 16) == rgb(0x00112233) &&
          pixel(window, 25, 16) == rgb(0x00DDAA77), "paint clip intersects application clip");
    FillRect(own, &all, ink);
    check(pixel(window, 5, 5) == rgb(0x00DDAA77), "EndPaint removes only transient paint clip");
    ReleaseDC(window, own);

    reset(window);
    nested_paint = 1;
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(pixel(window, 15, 16) == rgb(0x006B4523) &&
          pixel(window, 5, 5) == rgb(0x006B4523), "nested EndPaint releases owned-DC paint clip");

    own = GetDC(window);
    check(GetObjectType(own) == 3, "window owns a display DC");
    DestroyWindow(window);
    check(!BeginPaint(window, &observed), "invalid window has no paint DC");
    UnregisterClassA(cls.name, cls.instance);
    cls.background = 0;
    if (!RegisterClassA(&cls)) ExitProcess(2);
    window = create(cls.name);
    reset(window);
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(observed.erase, "class without background brush leaves erasing to app");
    reset(window);
    destroy_on_erase = 1;
    InvalidateRect(window, &dirty, 1);
    UpdateWindow(window);
    check(!BeginPaint(window, &observed), "destroying during erase retires paint state");
    UnregisterClassA(cls.name, cls.instance);
    dialog_paint();
    SelectObject(capture_dc, cap_old);
    SelectObject(source_dc, src_old);
    DeleteObject(cap_bitmap); DeleteObject(src_bitmap);
    DeleteDC(capture_dc); DeleteDC(source_dc);
    DeleteObject(background); DeleteObject(sentinel); DeleteObject(ink);
    report("[USER32-PAINT] checks="); number(checks);
    report(" failures="); number(failures);
    report(failures ? " FAIL\n" : " PASS\n");
    ExitProcess(failures ? 3 : 0);
}
