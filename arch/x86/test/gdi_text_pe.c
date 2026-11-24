/* Pixel and layout contracts, unchanged on native Windows and OsitoK. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct {
    DWORD size; LONG width, height; unsigned short planes, bpp;
    DWORD compression, image_size; LONG xppm, yppm; DWORD used, important;
} BITMAPINFO;
typedef struct {
    LONG height, ascent, descent, internal, external, average, maximum;
    LONG weight, overhang, aspect_x, aspect_y;
    WCHAR first, last, fallback, space;
    unsigned char italic, underline, strikeout, pitch, charset;
} TEXTMETRIC;
typedef struct { UINT size; int tab, left, right; UINT drawn; } DTP;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL GetModuleHandleA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API HANDLE CALL CreateCompatibleDC(HANDLE);
API HANDLE CALL CreateDIBSection(HANDLE, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
API HANDLE CALL SelectObject(HANDLE, HANDLE);
API HANDLE CALL GetStockObject(int);
API BOOL CALL DeleteObject(HANDLE);
API BOOL CALL DeleteDC(HANDLE);
API DWORD CALL SetTextColor(HANDLE, DWORD);
API DWORD CALL SetBkColor(HANDLE, DWORD);
API int CALL SetBkMode(HANDLE, int);
API UINT CALL SetTextAlign(HANDLE, UINT);
API BOOL CALL GetTextMetricsW(HANDLE, TEXTMETRIC *);
API BOOL CALL GetTextExtentPoint32W(HANDLE, const WCHAR *, int, POINT *);
API BOOL CALL TextOutW(HANDLE, int, int, const WCHAR *, int);
API BOOL CALL ExtTextOutA(HANDLE, int, int, UINT, const RECT *, const char *, UINT, const int *);
API BOOL CALL ExtTextOutW(HANDLE, int, int, UINT, const RECT *, const WCHAR *, UINT, const int *);
API BOOL CALL GdiFlush(void);
API HANDLE CALL CreateRectRgn(int, int, int, int);
API int CALL SelectClipRgn(HANDLE, HANDLE);
API BOOL CALL SetViewportOrgEx(HANDLE, int, int, POINT *);
API BOOL CALL MoveToEx(HANDLE, int, int, POINT *);
API int CALL DrawTextA(HANDLE, const char *, int, RECT *, UINT);
API int CALL DrawTextExA(HANDLE, char *, int, RECT *, UINT, DTP *);
API int CALL DrawTextExW(HANDLE, WCHAR *, int, RECT *, UINT, DTP *);
static int (CALL *draw_wide)(HANDLE, const WCHAR *, int, RECT *, UINT);
static BOOL (CALL *get_position)(HANDLE, POINT *);
static UINT (CALL *get_align)(HANDLE);
static HANDLE dc, reference;
static DWORD *bits, *reference_bits;
static TEXTMETRIC metrics;
static unsigned checks, failures;
static const WCHAR ab[] = {'A','B'};
#define WIDTH 320
#define HEIGHT 160
#define SENTINEL 0x00112233u
#define BG 0x00CCBBAAu

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
static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}
static void number(int value)
{
    char text[12]; unsigned used = 0, v;
    if (value < 0) { report("-"); v = 0u - (unsigned)value; }
    else v = value;
    do { text[used++] = '0' + v % 10; v /= 10; } while (v);
    while (used) { char digit[2] = {text[--used], 0}; report(digit); }
}
static void check(BOOL condition, const char *name)
{
    checks++;
    if (!condition) { failures++; report("FAIL: "); report(name); report("\n"); }
}
static void clear(void)
{
    GdiFlush();
    for (unsigned i = 0; i < WIDTH*HEIGHT; i++) bits[i] = reference_bits[i] = SENTINEL;
}
static BOOL same_pixels(void)
{
    GdiFlush();
    for (unsigned i = 0; i < WIDTH*HEIGHT; i++)
        if ((bits[i] & 0xFFFFFF) != (reference_bits[i] & 0xFFFFFF)) return 0;
    return 1;
}
static void ext_text(void)
{
    clear();
    check(ExtTextOutA(dc, 20, 20, 0, 0, "AB", 2, 0), "ExtTextOutA draws");
    TextOutW(reference, 20, 20, ab, 2);
    check(same_pixels(), "ANSI and Unicode text share pixels");
    int step[2] = {24, 31};
    clear();
    check(ExtTextOutW(dc, 20, 20, 0, 0, ab, 2, step), "explicit advance accepted");
    TextOutW(reference, 20, 20, ab, 1);
    TextOutW(reference, 44, 20, ab+1, 1);
    check(same_pixels(), "explicit character advances position glyphs");
    int pair[4] = {24, 9, 31, -3};
    clear();
    ExtTextOutW(dc, 20, 20, 0x2000, 0, ab, 2, pair);
    TextOutW(reference, 20, 20, ab, 1);
    TextOutW(reference, 44, 11, ab+1, 1);
    check(same_pixels(), "ETO_PDY advances x and y");

    RECT clip = {25, 22, 31, 29};
    clear();
    ExtTextOutW(dc, 20, 20, 4, &clip, ab, 2, 0);
    TextOutW(reference, 20, 20, ab, 2);
    GdiFlush();
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++)
        if (x < clip.left || x >= clip.right || y < clip.top || y >= clip.bottom)
            reference_bits[y*WIDTH+x] = SENTINEL;
    check(same_pixels(), "ETO_CLIPPED restricts only this text call");
    clear();
    check(ExtTextOutW(dc, 0, 0, 2, &clip, 0, 0, 0), "empty opaque text fills rectangle");
    for (int y = clip.top; y < clip.bottom; y++)
        for (int x = clip.left; x < clip.right; x++) reference_bits[y*WIDTH+x] = BG;
    check(same_pixels(), "ETO_OPAQUE works with transparent background mode");
    clear();
    SetViewportOrgEx(dc, 7, 11, 0);
    ExtTextOutW(dc, 0, 0, 2, &clip, 0, 0, 0);
    SetViewportOrgEx(dc, 0, 0, 0);
    for (int y = clip.top+11; y < clip.bottom+11; y++)
        for (int x = clip.left+7; x < clip.right+7; x++) reference_bits[y*WIDTH+x] = BG;
    check(same_pixels(), "opaque rectangle follows viewport origin");
    POINT extent;
    GetTextExtentPoint32W(dc, ab, 2, &extent);
    clear();
    SetTextAlign(dc, 6 | 24);
    ExtTextOutW(dc, 100, 80, 0, 0, ab, 2, 0);
    SetTextAlign(dc, 0);
    TextOutW(reference, 100-extent.x/2, 80-metrics.ascent, ab, 2);
    check(same_pixels(), "center and baseline use reference point");
    if (get_position) {
        POINT point;
        MoveToEx(dc, 20, 20, 0);
        SetTextAlign(dc, 1);
        clear();
        ExtTextOutW(dc, 100, 100, 0, 0, ab, 2, step);
        get_position(dc, &point);
        TextOutW(reference, 20, 20, ab, 1);
        TextOutW(reference, 44, 20, ab+1, 1);
        check(same_pixels() && point.x == 75 && point.y == 20,
              "TA_UPDATECP ignores arguments and advances current position");
        MoveToEx(dc, 100, 20, 0);
        SetTextAlign(dc, 3);
        ExtTextOutW(dc, 0, 0, 0, 0, ab, 2, step);
        get_position(dc, &point);
        check(point.x == 45 && point.y == 20, "right-aligned current position moves to start");
        MoveToEx(dc, 100, 20, 0);
        SetTextAlign(dc, 7);
        ExtTextOutW(dc, 0, 0, 0, 0, ab, 2, step);
        get_position(dc, &point);
        check(point.x == 100 && point.y == 20, "center alignment preserves current position");
        MoveToEx(dc, 20, 20, 0);
        SetTextAlign(dc, 1);
        ExtTextOutW(dc, 0, 0, 0x2000, 0, ab, 2, pair);
        get_position(dc, &point);
        check(point.x == 75 && point.y == 14, "ETO_PDY updates two-dimensional current position");
        SetTextAlign(dc, 0);
    }
    clear();
    SetBkMode(dc, 2);
    ExtTextOutW(dc, 20, 20, 0, 0, ab, 2, step);
    for (int y = 20; y < 20+metrics.height; y++)
        for (int x = 20; x < 75; x++) reference_bits[y*WIDTH+x] = BG;
    TextOutW(reference, 20, 20, ab, 1);
    TextOutW(reference, 44, 20, ab+1, 1);
    check(same_pixels(), "opaque text background covers the advance span");
    SetBkMode(dc, 1);
    clear();
    HANDLE region = CreateRectRgn(27, 24, 29, 27);
    SelectClipRgn(dc, region);
    ExtTextOutW(dc, 0, 0, 2, &clip, 0, 0, 0);
    for (int y = 24; y < 27; y++)
        for (int x = 27; x < 29; x++) reference_bits[y*WIDTH+x] = BG;
    check(same_pixels(), "opaque fill intersects the selected DC clip");
    SelectClipRgn(dc, 0); DeleteObject(region);
    clear();
    const char ansi[] = {'A', (char)0xE9, 'B'};
    const WCHAR unicode[] = {'A', 0xE9, 'B'};
    ExtTextOutA(dc, 20, 20, 0, 0, ansi, 3, 0);
    TextOutW(reference, 20, 20, unicode, 3);
    check(same_pixels(), "ANSI conversion follows the active codepage");
    static WCHAR oversized[8193];
    check(ExtTextOutW(dc, 0, 0, 0, 0, oversized, 8193, 0), "ExtTextOut accepts native Windows long runs");
    check(!ExtTextOutW(0, 0, 0, 0, 0, ab, 2, 0), "invalid text DC rejected");
}

static int extent(const char *text)
{
    WCHAR wide[128]; int length = 0;
    while (text[length] && length < 127) {
        wide[length] = (unsigned char)text[length]; length++;
    }
    POINT size;
    GetTextExtentPoint32W(dc, wide, length, &size);
    return size.x;
}
static void reference_text(const char *text, int x, int y)
{
    WCHAR wide[128]; int length = 0;
    while (text[length] && length < 127) {
        wide[length] = (unsigned char)text[length]; length++;
    }
    TextOutW(reference, x, y, wide, length);
}
static void clip_reference(RECT clip)
{
    GdiFlush();
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++)
        if (x < clip.left || x >= clip.right || y < clip.top || y >= clip.bottom)
            reference_bits[y*WIDTH+x] = SENTINEL;
}
static BOOL equal_text(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void layout(void)
{
    static const struct {
        const char *text, *widest; UINT flags; int width, lines;
    } cases[] = {
        {"ABC", "ABC", 0, 100, 1}, {"A\r\nB", "B", 0, 100, 2},
        {"A\n", "A", 0, 100, 2}, {"A\r\nB", "AB", 0x20, 100, 1},
        {"A\tB", "AB", 0, 100, 1}, {"A\tB", "B", 0x40, 100, 1},
        {"abc def ghi", "abc ", 0x10, 5, 3}, {"abcdefghij", "abcdefghij", 0x10, 3, 1},
        {"abc   def", "abc ", 0x10, 4, 2}, {"A&bc&&d", "Abc&d", 0, 100, 1},
        {"a&", "a", 0, 100, 1}, {"", "", 0, 100, 0}, {"", "", 0x20, 100, 1}
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        RECT rect = {10, 12, 10+cases[i].width*metrics.average, 112};
        clear();
        int height = DrawTextA(dc, cases[i].text, -1, &rect, cases[i].flags | 0x400);
        int width = extent(cases[i].widest);
        if (i == 5) width += 8*metrics.average;
        BOOL valid = height == (cases[i].lines ? cases[i].lines*metrics.height : 1) &&
            rect.right == 10+width && rect.bottom == 12+cases[i].lines*metrics.height;
        if (!valid) { report("LAYOUT case="); number(i); report(" h="); number(height);
            report(" w="); number(rect.right-rect.left); report("\n"); }
        check(valid, "CALCRECT dimensions follow line, word, tab and prefix rules");
        check(same_pixels(), "CALCRECT does not draw");
    }
    DTP params = {20, 4, 2, 3, 0};
    char text[] = "AB";
    RECT rect = {10, 12, 300, 112};
    int height = DrawTextExA(dc, text, -1, &rect, 0x420, &params);
    check(height == metrics.height && rect.right == 10+extent("AB")+5 &&
          params.drawn == 2, "DrawTextEx margins and length ABI");
    clear();
    rect = (RECT){20, 20, 100, 100};
    check(DrawTextA(dc, "AB", -1, &rect, 0x20) == metrics.height,
          "DrawTextA returns line height");
    reference_text("AB", 20, 20);
    check(same_pixels(), "DrawTextA emits text pixels");
    if (draw_wide) {
        clear();
        check(draw_wide(dc, ab, 2, &rect, 0x20) == metrics.height,
              "DrawTextW handles counted strings");
        reference_text("AB", 20, 20);
        check(same_pixels(), "DrawTextW agrees with ANSI rendering");
    }
    clear();
    WCHAR wide[] = {'A','B',0};
    int top = 20+(80-metrics.height)/2;
    height = DrawTextExW(dc, wide, -1, &rect, 0x25, 0);
    reference_text("AB", 20+(80-extent("AB"))/2, top);
    check(height == top+metrics.height-20 && same_pixels(), "centered line geometry and return value");
    clear();
    height = DrawTextA(dc, "AB", -1, &rect, 0x2A);
    reference_text("AB", 100-extent("AB"), 100-metrics.height);
    check(height == 80 && same_pixels(), "right and bottom alignment");
    clear();
    rect = (RECT){20, 20, 25, 30};
    DrawTextA(dc, "AB", -1, &rect, 0x20);
    reference_text("AB", 20, 20);
    clip_reference(rect);
    check(same_pixels(), "DrawText clips to caller rectangle");
    clear();
    DrawTextA(dc, "AB", -1, &rect, 0x120);
    reference_text("AB", 20, 20);
    check(same_pixels(), "DT_NOCLIP bypasses only layout rectangle");
    clear();
    rect = (RECT){20, 20, 20+extent("abc "), 120};
    height = DrawTextA(dc, "abc def", -1, &rect, 0x10);
    reference_text("abc", 20, 20);
    reference_text("def", 20, 20+metrics.height);
    check(height == 2*metrics.height && same_pixels(), "word wrapping positions each line");
    clear();
    rect = (RECT){20, 20, 300, 120};
    DrawTextA(dc, "A&bc&&d", -1, &rect, 0x100020);
    reference_text("Abc&d", 20, 20);
    check(same_pixels(), "DT_HIDEPREFIX processes mnemonics without underline");
    clear();
    DrawTextA(dc, "A&bc&&d", -1, &rect, 0x820);
    reference_text("A&bc&&d", 20, 20);
    check(same_pixels(), "DT_NOPREFIX keeps ampersands");
    clear();
    params = (DTP){20, 4, 2, 3, 0};
    char tabs[] = "A\tB";
    DrawTextExA(dc, tabs, -1, &rect, 0xE0, &params);
    reference_text("A", 22, 20);
    reference_text("B", 22+4*metrics.average, 20);
    check(params.drawn == 3 && same_pixels(), "DrawTextEx tab stops start at the left margin");

    char lines[] = "AB\r\nCD\r\nEF";
    params = (DTP){20, 8, 0, 0, 0};
    rect = (RECT){20, 20, 300, 20+metrics.height+1};
    height = DrawTextExA(dc, lines, -1, &rect, 0, &params);
    check(height == 2*metrics.height && params.drawn == 8,
          "partly visible last line counts consumed source characters");
    height = DrawTextExA(dc, lines, -1, &rect, 0x2000, &params);
    check(height == metrics.height && params.drawn == 4,
          "DT_EDITCONTROL omits partly visible final line");
    height = DrawTextExA(dc, lines, -1, &rect, 0x100, &params);
    check(height == 3*metrics.height && params.drawn == 10,
          "DT_NOCLIP processes full source");

    char ellipsis[64] = "ABCDEFG";
    rect = (RECT){20, 20, 20+extent("ABC..."), 100};
    clear();
    DrawTextA(dc, ellipsis, -1, &rect, 0x18020);
    reference_text("ABC...", 20, 20);
    check(equal_text(ellipsis, "ABC...") && same_pixels(), "end ellipsis modifies only opted-in string");
    char path[64] = "C:\\alpha\\file.txt";
    rect.right = 20+extent("C:\\...\\file.txt");
    clear();
    DrawTextA(dc, path, -1, &rect, 0x14020);
    reference_text("C:\\...\\file.txt", 20, 20);
    if (!equal_text(path, "C:\\...\\file.txt")) { report("PATH got="); report(path); report("\n"); }
    check(equal_text(path, "C:\\...\\file.txt") && same_pixels(), "path ellipsis preserves final component");
    clear();
    rect = (RECT){20, 20, 300, 100};
    DrawTextA(dc, "A&B", -1, &rect, 0x200020);
    GdiFlush();
    unsigned marked = 0, outside = 0;
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++) {
        if ((bits[y*WIDTH+x] & 0xFFFFFF) == SENTINEL) continue;
        marked++;
        if (x < 20+extent("A") || x >= 20+extent("AB") ||
            y < 20+metrics.ascent-1 || y >= 20+metrics.height) outside++;
    }
    check(marked && !outside, "DT_PREFIXONLY draws a mnemonic rule without glyphs");
    check(SetBkColor(dc, 0x00AABBCC) == 0x00AABBCC, "mnemonic drawing restores background color");
    static char large[9002];
    for (int i = 0; i < 9001; i++) large[i] = 'A';
    large[9001] = 0;
    rect = (RECT){0, 0, 300, 100};
    height = DrawTextA(dc, large, 9001, &rect, 0x420);
    check(height == metrics.height && rect.right == 9001*extent("A"),
          "DrawText measures beyond one ExtTextOut run");
    rect = (RECT){0, 0, 300, 100};
    clear();
    height = DrawTextA(dc, large, 9001, &rect, 0x20);
    WCHAR first[128];
    for (int i = 0; i < 128; i++) first[i] = 'A';
    TextOutW(reference, 0, 0, first, 128);
    clip_reference(rect);
    check(height == metrics.height && same_pixels(), "long DrawText input is drawn in bounded GDI runs");
    check(!DrawTextA(0, "AB", 2, &rect, 0), "invalid DrawText DC rejected");
    params.size = 1;
    check(!DrawTextExA(dc, text, -1, &rect, 0, &params), "invalid DRAWTEXTPARAMS size rejected");
    if (get_align) check(get_align(dc) == 0, "DrawText leaves text alignment unchanged");
}

static void modifiers(void)
{
    char multi[80] = "ABCDEFGHIJ\nKLMNOPQRST";
    char prefix[80] = "A&BCDEFG";
    char path[80] = "\\file.txt";
    char word[80] = "one two three";
    RECT rect = {20, 20, 20+extent("ABC..."), 120};
    char tail[24] = "KLMNOPQRST";
    int keep = 10;
    for (;;) {
        for (int i = 0; i < keep; i++) tail[i] = "KLMNOPQRST"[i];
        tail[keep] = tail[keep+1] = tail[keep+2] = '.'; tail[keep+3] = 0;
        if (extent(tail) <= rect.right-rect.left || !keep) break;
        keep--;
    }
    char expected[80] = "ABCDEFGHIJ\n";
    int index = 0;
    do { expected[11+index] = tail[index]; } while (tail[index++]);
    DrawTextA(dc, multi, -1, &rect, 0x18000);
    check(equal_text(multi, expected), "end ellipsis preserves earlier lines in modified source");
    DrawTextA(dc, prefix, -1, &rect, 0x18020);
    check(equal_text(prefix, "A&BC..."), "modified strings retain mnemonic markers");
    rect.right = 20+extent("...");
    DrawTextA(dc, path, -1, &rect, 0x14020);
    check(equal_text(path, "\\file.txt"), "path ellipsis does not discard an unshortenable final component");
    char plain[80] = "abcdefghij";
    rect.right = 20+extent("abc...");
    DrawTextA(dc, plain, -1, &rect, 0x14020);
    check(equal_text(plain, "abcdefghij"), "path ellipsis preserves a bare filename");
    rect.right = 20+extent("one ...");
    clear();
    DrawTextA(dc, word, -1, &rect, 0x50020);
    reference_text("one ...", 20, 20);
    check(equal_text(word, "one ...") && same_pixels(), "word ellipsis retains the separator before dots");
    clear();
    rect = (RECT){10, 10, 14, 14};
    SetBkMode(dc, 2);
    ExtTextOutW(dc, 20, 20, 2, &rect, ab, 2, 0);
    GdiFlush();
    check((bits[(20+metrics.height-1)*WIDTH+20] & 0xFFFFFF) == BG,
          "opaque background mode also fills text outside the opaque rectangle");
    SetBkMode(dc, 1);
}

void mainCRTStartup(void)
{
    BITMAPINFO info = {40, WIDTH, -HEIGHT, 1, 32, 0, 0, 0, 0, 0, 0};
    dc = CreateCompatibleDC(0); reference = CreateCompatibleDC(0);
    HANDLE bitmap = CreateDIBSection(dc, &info, 0, (void **)&bits, 0, 0);
    HANDLE reference_bitmap = CreateDIBSection(reference, &info, 0, (void **)&reference_bits, 0, 0);
    if (!dc || !reference || !bitmap || !reference_bitmap) ExitProcess(2);
    HANDLE old = SelectObject(dc, bitmap), old_ref = SelectObject(reference, reference_bitmap);
    SelectObject(dc, GetStockObject(13)); SelectObject(reference, GetStockObject(13));
    SetTextColor(dc, 0x00EEDDCC); SetTextColor(reference, 0x00EEDDCC);
    SetBkColor(dc, 0x00AABBCC); SetBkColor(reference, 0x00AABBCC);
    SetBkMode(dc, 1); SetBkMode(reference, 1);
    GetTextMetricsW(dc, &metrics);
    draw_wide = GetProcAddress(GetModuleHandleA("user32.dll"), "DrawTextW");
    get_position = GetProcAddress(GetModuleHandleA("gdi32.dll"), "GetCurrentPositionEx");
    get_align = GetProcAddress(GetModuleHandleA("gdi32.dll"), "GetTextAlign");
    check(draw_wide && get_position && get_align, "text query and Unicode exports present");
    ext_text();
    layout();
    modifiers();
    SelectObject(dc, old); SelectObject(reference, old_ref);
    DeleteObject(bitmap); DeleteObject(reference_bitmap);
    DeleteDC(dc); DeleteDC(reference);
    report("[GDI-TEXT] checks="); number(checks);
    report(" failures="); number(failures); report(failures ? " FAIL\n" : " PASS\n");
    ExitProcess(failures ? 3 : 0);
}
