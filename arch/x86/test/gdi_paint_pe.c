/* Run the same DIB/brush contracts as PE32 and PE64 on Windows and OsitoK. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct {
    DWORD size; LONG width, height; unsigned short planes, bpp;
    DWORD compression, image_size; LONG xppm, yppm; DWORD used, important;
} BITMAPINFO;
typedef struct {
    LONG type, width, height, pitch; unsigned short planes, bpp; void *bits;
} BITMAP;
typedef struct { UINT style; DWORD color; UPTR hatch; } LOGBRUSH;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL GetModuleHandleA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API HANDLE CALL CreateCompatibleDC(HANDLE);
API HANDLE CALL CreateDIBSection(HANDLE, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
API HANDLE CALL CreateBitmap(int, int, UINT, UINT, const void *);
API HANDLE CALL SelectObject(HANDLE, HANDLE);
API HANDLE CALL GetCurrentObject(HANDLE, UINT);
API HANDLE CALL GetStockObject(int);
API HANDLE CALL CreateSolidBrush(DWORD);
API HANDLE CALL CreatePatternBrush(HANDLE);
API BOOL CALL DeleteObject(HANDLE);
API BOOL CALL DeleteDC(HANDLE);
API int CALL GetObjectA(HANDLE, int, void *);
API DWORD CALL GetObjectType(HANDLE);
API BOOL CALL PatBlt(HANDLE, int, int, int, int, DWORD);
API HANDLE CALL CreateRectRgn(int, int, int, int);
API int CALL SelectClipRgn(HANDLE, HANDLE);
API BOOL CALL SetViewportOrgEx(HANDLE, int, int, void *);
API BOOL CALL SetBrushOrgEx(HANDLE, int, int, void *);
API DWORD CALL SetTextColor(HANDLE, DWORD);
API DWORD CALL SetBkColor(HANDLE, DWORD);
API int CALL FillRect(HANDLE, const RECT *, HANDLE);
API BOOL CALL DrawFocusRect(HANDLE, const RECT *);
API DWORD CALL GetSysColor(int);
API BOOL CALL GdiFlush(void);

void *memset(void *destination, int value, __SIZE_TYPE__ size)
{
    unsigned char *bytes = destination;
    for (__SIZE_TYPE__ i = 0; i < size; i++) bytes[i] = (unsigned char)value;
    return destination;
}

static UINT checks, failures;
static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}
static void check(BOOL condition, const char *name)
{
    checks++;
    if (!condition) { failures++; report(name); report("\n"); }
}
static DWORD rgb(DWORD color)
{
    return ((color & 255) << 16) | (color & 0xFF00) | ((color >> 16) & 255);
}
static void clear(DWORD *pixels, UINT count)
{
    for (UINT i = 0; i < count; i++) pixels[i] = 0x00563412;
}
static void formats(void)
{
    HANDLE dc = CreateCompatibleDC(0);
    BITMAPINFO info = {40, 3, 2, 1, 24, 0, 0, 0, 0, 0, 0};
    unsigned char *bits;
    HANDLE bitmap = CreateDIBSection(dc, &info, 0, (void **)&bits, 0, 0);
    if (!dc || !bitmap) ExitProcess(2);
    HANDLE old = SelectObject(dc, bitmap);
    for (UINT i = 0; i < 24; i++) bits[i] = 0x5A;
    HANDLE brush = CreateSolidBrush(0x00123456);
    RECT rect = {0, 0, 2, 1};
    check(FillRect(dc, &rect, brush), "24bpp fill succeeds");
    GdiFlush();
    check(bits[12] == 0x12 && bits[13] == 0x34 && bits[14] == 0x56 &&
          bits[15] == 0x12 && bits[16] == 0x34 && bits[17] == 0x56 &&
          bits[18] == 0x5A && bits[23] == 0x5A && bits[0] == 0x5A,
          "24bpp fill respects bottom-up rows, width and padding");
    SelectObject(dc, old);
    check(DeleteObject(brush) && DeleteObject(bitmap) && DeleteDC(dc),
          "24bpp resources released");
}

void mainCRTStartup(void)
{
    HANDLE dc = CreateCompatibleDC(0);
    BITMAPINFO info = {40, 16, -16, 1, 32, 0, 0, 0, 0, 0, 0};
    DWORD *bits;
    HANDLE bitmap = CreateDIBSection(dc, &info, 0, (void **)&bits, 0, 0);
    if (!dc || !bitmap) ExitProcess(2);
    HANDLE old_bitmap = SelectObject(dc, bitmap);
    HANDLE red = CreateSolidBrush(0x000000FF), blue = CreateSolidBrush(0x00FF0000);
    if (!red || !blue) ExitProcess(2);
    check(red != blue, "brushes have distinct identities");
    LOGBRUSH logical = {99, 99, 99};
    check(GetObjectA(red, sizeof(logical), &logical) == sizeof(logical) &&
          logical.style == 0 && logical.color == 0xFF,
          "LOGBRUSH has the caller ABI layout and original color");
    check(GetObjectType(red) == 2 && GetObjectType(GetStockObject(5)) == 2,
          "brush object type includes stock brushes");
    HANDLE old_brush = SelectObject(dc, red);
    RECT rect = {2, 3, 5, 7}, all = {0, 0, 16, 16};
    clear(bits, 256);
    check(FillRect(dc, &rect, blue), "FillRect returns success");
    GdiFlush();
    check((bits[3*16+2] & 0xFFFFFF) == 0x0000FF &&
          (bits[6*16+4] & 0xFFFFFF) == 0x0000FF &&
          bits[3*16+5] == 0x00563412 && bits[7*16+2] == 0x00563412,
          "FillRect draws the brush and excludes right/bottom edges");
    check(GetCurrentObject(dc, 2) == red, "FillRect preserves selected brush");
    check(PatBlt(dc, 0, 0, 1, 1, 0x00F00021), "PATCOPY succeeds");
    GdiFlush();
    check((bits[0] & 0xFFFFFF) == 0xFF0000, "PATCOPY uses selected brush");
    SetViewportOrgEx(dc, 1, 2, 0);
    RECT unit = {0, 0, 1, 1};
    check(FillRect(dc, &unit, blue), "translated fill succeeds");
    GdiFlush();
    check((bits[2*16+1] & 0xFFFFFF) == 0x0000FF, "fill uses viewport origin");
    SetViewportOrgEx(dc, 0, 0, 0);
    HANDLE region = CreateRectRgn(4, 4, 6, 6);
    check(SelectClipRgn(dc, region) == 2, "select clip rectangle");
    DeleteObject(region);
    clear(bits, 256);
    check(FillRect(dc, &all, blue), "clipped fill succeeds");
    GdiFlush();
    check((bits[4*16+4] & 0xFFFFFF) == 0x0000FF &&
          bits[4*16+6] == 0x00563412 && bits[0] == 0x00563412,
          "DC owns a copy of the deleted clip region");
    region = CreateRectRgn(0, 0, 0, 0);
    check(SelectClipRgn(dc, region) == 1, "empty clip is distinct from no clip");
    DeleteObject(region);
    clear(bits, 256);
    FillRect(dc, &all, red);
    GdiFlush();
    check(bits[0] == 0x00563412, "empty clip writes no pixels");
    check(SelectClipRgn(dc, 0) != 0, "remove clip");

    unsigned char mono[256] = {0x80, 0, 0x01, 0};
    HANDLE source = CreateBitmap(8, 2, 1, 1, mono);
    BITMAP description = {0};
    check(GetObjectA(source, sizeof(description), &description) == sizeof(description)
          && description.bpp == 1 && description.pitch == 2,
          "CreateBitmap preserves monochrome WORD-aligned rows");
    HANDLE pattern = CreatePatternBrush(source);
    check(pattern != 0 && pattern != red, "pattern brush allocated");
    check(DeleteObject(source), "source bitmap can be deleted independently");
    SetTextColor(dc, 0x000000FF);
    SetBkColor(dc, 0x0000FF00);
    SetBrushOrgEx(dc, 1, 1, 0);
    check(FillRect(dc, &all, pattern), "pattern fill succeeds");
    GdiFlush();
    check((bits[1*16+1] & 0xFFFFFF) == 0x00FF00 &&
          (bits[1*16+2] & 0xFFFFFF) == 0xFF0000 &&
          (bits[2*16+8] & 0xFFFFFF) == 0x00FF00,
          "monochrome pattern uses DC colors and brush origin");
    DeleteObject(pattern);
    check(!GetObjectA(pattern, sizeof(logical), &logical), "deleted brush is invalid");
    HANDLE (CALL *system_brush)(int) = (HANDLE (CALL *)(int))GetProcAddress(
        GetModuleHandleA("user32.dll"), "GetSysColorBrush");
    if (system_brush) {
        HANDLE system = system_brush(5);
        check(system && system == system_brush(5),
              "system brushes are cached");
        FillRect(dc, &all, system);
        GdiFlush();
        check((bits[0] & 0xFFFFFF) == rgb(GetSysColor(5)), "system brush color");
    } else check(0, "GetSysColorBrush export exists");
    FillRect(dc, &all, (HANDLE)(UPTR)6);
    GdiFlush();
    check((bits[0] & 0xFFFFFF) == rgb(GetSysColor(5)), "COLOR_WINDOW+1 brush");

    clear(bits, 256);
    check(DrawFocusRect(dc, &rect), "focus rectangle drawn");
    GdiFlush();
    unsigned changed = 0;
    for (UINT i = 0; i < 256; i++) changed += (bits[i] & 0xFFFFFF) != 0x563412;
    check(changed && bits[4*16+3] == 0x00563412, "focus draws only border pixels");
    check(DrawFocusRect(dc, &rect), "focus rectangle erased");
    GdiFlush();
    for (UINT i = 0; i < 256; i++)
        if ((bits[i] & 0xFFFFFF) != 0x563412) { check(0, "focus XOR restores pixels"); break; }
    SelectObject(dc, old_brush);
    check(DeleteObject(red) && DeleteObject(blue), "unselected brushes released");
    SelectObject(dc, old_bitmap);
    check(DeleteObject(bitmap) && DeleteDC(dc), "DC and DIB released");
    formats();
    report(failures ? "[GDI-PAINT] FAIL\n" : "[GDI-PAINT] PASS\n");
    ExitProcess(failures ? 3 : 0);
}
