/* Same control/message probes on Windows and OsitoK, with either PE ABI. */
typedef unsigned int UINT, DWORD;
typedef int BOOL, LONG;
typedef unsigned short WCHAR;
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
    DWORD size; LONG width, height; unsigned short planes, bpp;
    DWORD compression, image_size; LONG xppm, yppm; DWORD used, important;
} BITMAPINFO;
typedef struct {
    UINT type, id, item, action, state;
    HANDLE window, dc; RECT rect; UPTR data;
} DRAWITEM;
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL GetModuleHandleA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API HANDLE CALL CreateThread(void *, UPTR, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
API HANDLE CALL CreateEventA(void *, BOOL, BOOL, const char *);
API BOOL CALL SetEvent(HANDLE);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL CloseHandle(HANDLE);
API BOOL CALL GetMessageW(void *, HANDLE, UINT, UINT);
API IPTR CALL DispatchMessageW(const void *);
API BOOL CALL PostThreadMessageW(DWORD, UINT, UPTR, IPTR);
API unsigned short CALL RegisterClassA(const WNDCLASS *);
API BOOL CALL UnregisterClassA(const char *, HANDLE);
API HANDLE CALL CreateWindowExA(DWORD, const char *, const char *, DWORD,
    int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
API HANDLE CALL CreateWindowExW(DWORD, const WCHAR *, const WCHAR *, DWORD,
    int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
API BOOL CALL DestroyWindow(HANDLE);
API BOOL CALL IsWindow(HANDLE);
API BOOL CALL IsWindowUnicode(HANDLE);
API BOOL CALL EnableWindow(HANDLE, BOOL);
API HANDLE CALL SetFocus(HANDLE);
API HANDLE CALL GetCapture(void);
API BOOL CALL ReleaseCapture(void);
API IPTR CALL DefWindowProcA(HANDLE, UINT, UPTR, IPTR);
API IPTR CALL SendMessageA(HANDLE, UINT, UPTR, IPTR);
API IPTR CALL SendMessageW(HANDLE, UINT, UPTR, IPTR);
API IPTR CALL CallWindowProcA(WNDPROC, HANDLE, UINT, UPTR, IPTR);
API BOOL CALL SetWindowTextA(HANDLE, const char *);
API int CALL GetWindowTextA(HANDLE, char *, int);
API int CALL GetWindowTextW(HANDLE, WCHAR *, int);
API int CALL GetWindowTextLengthA(HANDLE);
API int CALL GetWindowTextLengthW(HANDLE);
API BOOL CALL GetClientRect(HANDLE, RECT *);
API HANDLE CALL GetDC(HANDLE);
API int CALL ReleaseDC(HANDLE, HANDLE);
API BOOL CALL InvalidateRect(HANDLE, const RECT *, BOOL);
API BOOL CALL ValidateRect(HANDLE, const RECT *);
API BOOL CALL UpdateWindow(HANDLE);
API LONG CALL GetWindowLongA(HANDLE, int);
API LONG CALL SetWindowLongA(HANDLE, int, LONG);
API int CALL FillRect(HANDLE, const RECT *, HANDLE);
API HANDLE CALL CreateCompatibleDC(HANDLE);
API HANDLE CALL CreateDIBSection(HANDLE, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
API HANDLE CALL CreateBitmap(int, int, UINT, UINT, const void *);
API HANDLE CALL CreateSolidBrush(DWORD);
API DWORD CALL GetObjectType(HANDLE);
API HANDLE CALL SelectObject(HANDLE, HANDLE);
API BOOL CALL DeleteObject(HANDLE);
API BOOL CALL DeleteDC(HANDLE);
API HANDLE CALL GetStockObject(int);
API DWORD CALL SetTextColor(HANDLE, DWORD);
API DWORD CALL SetBkColor(HANDLE, DWORD);
API BOOL CALL GdiFlush(void);
API HANDLE CALL CreateRectRgn(int, int, int, int);
API int CALL SelectClipRgn(HANDLE, HANDLE);
API BOOL CALL BitBlt(HANDLE, int, int, int, int, HANDLE, int, int, DWORD);
static IPTR (CALL *set_long_ptr)(HANDLE, int, IPTR);
static unsigned checks, failures, clicks, colors, draws;
static UINT last_id, last_code;
static HANDLE last_window, parent, instance, ink, tint, dc, dib;
static DWORD *bits, previous[240*80];
static BOOL destroy_color, destroy_draw, ansi_payload, bleed;
static WNDPROC previous_proc;
static HANDLE worker_ready, worker_window;
static const char class_name[] = "OsitoControlContracts";

void *memset(void *out, int value, __SIZE_TYPE__ size)
{
    unsigned char *p = out;
    for (__SIZE_TYPE__ i = 0; i < size; i++) p[i] = (unsigned char)value;
    return out;
}
void *memcpy(void *out, const void *in, __SIZE_TYPE__ size)
{
    unsigned char *p = out; const unsigned char *q = in;
    for (__SIZE_TYPE__ i = 0; i < size; i++) p[i] = q[i];
    return out;
}
static void report(const char *text)
{
    DWORD count = 0, written;
    while (text[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11), text, count, &written, 0);
}
static void number(unsigned value)
{
    char digits[12]; UINT count = 0;
    do { digits[count++] = '0'+value%10; value /= 10; } while (value);
    while (count) { char s[2] = {digits[--count], 0}; report(s); }
}
static void check(BOOL pass, const char *name)
{
    checks++;
    if (!pass) { failures++; report("FAIL: "); report(name); report("\n"); }
}
static IPTR CALL owner(HANDLE window, UINT message, UPTR wp, IPTR lp)
{
    if (message == 0xC) ansi_payload = lp && ((const char *)lp)[0] == 'A' &&
        (unsigned char)((const char *)lp)[1] == 0xE9;
    if (message == 0x111 && lp) {
        last_id = (UINT)wp & 65535; last_code = (UINT)(wp >> 16);
        last_window = (HANDLE)lp;
        if (!last_code) clicks++;
        return 0;
    }
    if (message == 0x138) {
        colors++;
        SetTextColor((HANDLE)wp, 0x003355AA);
        SetBkColor((HANDLE)wp, 0x00664422);
        if (destroy_color) { destroy_color = 0; DestroyWindow((HANDLE)lp); }
        return (IPTR)tint;
    }
    if (message == 0x2B) {
        DRAWITEM *item = (DRAWITEM *)lp;
        draws++;
        check(item && item->id == wp &&
              (item->type == 4 || item->type == 5) && item->window && item->dc,
              "owner-draw structure ABI");
        if (item) {
            RECT area = item->rect;
            if (bleed) { area.right += 20; area.bottom += 20; }
            FillRect(item->dc, &area, ink);
            if (destroy_draw) { destroy_draw = 0; DestroyWindow(item->window); }
        }
        return 1;
    }
    return DefWindowProcA(window, message, wp, lp);
}
static IPTR CALL subclass(HANDLE window, UINT message, UPTR wp, IPTR lp)
{
    if (message == 0xC) ansi_payload = lp && ((const char *)lp)[0] == 'A' &&
        (unsigned char)((const char *)lp)[1] == 0xE9;
    return CallWindowProcA(previous_proc, window, message, wp, lp);
}
static HANDLE child(const char *cls, const char *text, UINT style, UINT id)
{
    HANDLE result = CreateWindowExA(0, cls, text, 0x50000000u|style,
        5, 5+(id%7)*30, 180, 24, parent, (HANDLE)(UPTR)id, instance, 0);
    check(result != 0, "create standard control");
    return result;
}
static void print(HANDLE control)
{
    GdiFlush();
    for (UINT i = 0; i < 240*80; i++) bits[i] = 0x00112233;
    SendMessageA(control, 0x318, (UPTR)dc, 4);
    GdiFlush();
}
static void text_contracts(void)
{
    HANDLE control = child("STATIC", "initial", 0, 101);
    char text[704]; WCHAR wide[704];
    for (int i = 0; i < 600; i++) text[i] = 'a'+i%26;
    text[600] = 0;
    check(SetWindowTextA(control, text), "SetWindowText sends WM_SETTEXT");
    check(GetWindowTextLengthA(control) == 600 && GetWindowTextLengthW(control) == 600,
          "caption is not truncated at the compositor title buffer");
    check(GetWindowTextW(control, wide, 704) == 600 && wide[599] == text[599] && !wide[600],
          "Unicode caption retrieval");
    char short_text[5] = {1,1,1,1,1};
    check(GetWindowTextA(control, short_text, 5) == 4 && short_text[4] == 0 &&
          short_text[3] == 'd', "ANSI caption bounded copy and terminator");
    const WCHAR unicode[] = {'A', 0x03A9, 0x20AC, 0};
    static const WCHAR static_class[] = {'S','T','A','T','I','C',0};
    HANDLE unicode_control = CreateWindowExW(0, static_class, unicode, 0x50000000u,
        5, 245, 180, 24, parent, (HANDLE)102, instance, 0);
    check(!IsWindowUnicode(control) && IsWindowUnicode(unicode_control),
          "window reports the native procedure character set");
    check(unicode_control && SendMessageW(unicode_control, 0xC, 0, (IPTR)unicode) &&
          SendMessageW(unicode_control, 0xD, 704, (IPTR)wide) == 3 &&
          wide[1] == unicode[1] && wide[2] == unicode[2], "native control Unicode text round trip");
    DestroyWindow(unicode_control);
    const WCHAR acp[] = {'A', 0xE9, 0};
    ansi_payload = 0;
    SendMessageW(parent, 0xC, 0, (IPTR)acp);
    check(ansi_payload && GetWindowTextW(parent, wide, 704) == 2 && wide[1] == 0xE9,
          "ANSI application procedure receives converted text");
    if (sizeof(void *) == 4) previous_proc = (WNDPROC)(UPTR)(UINT)SetWindowLongA(
        control, -4, (LONG)(UPTR)subclass);
    else previous_proc = (WNDPROC)set_long_ptr(control, -4, (IPTR)subclass);
    ansi_payload = 0;
    SendMessageW(control, 0xC, 0, (IPTR)acp);
    check(ansi_payload && GetWindowTextW(control, wide, 704) == 2 && wide[1] == 0xE9,
          "ANSI subclass and previous control procedure preserve message encoding");
    if (sizeof(void *) == 4) SetWindowLongA(control, -4, (LONG)(UPTR)previous_proc);
    else set_long_ptr(control, -4, (IPTR)previous_proc);
    SendMessageA(control, 0xC, 0, (IPTR)"Caption");
    print(control);
    check(colors && (bits[0] & 0xFFFFFF) == 0x00224466,
          "static background uses parent control-color brush");
    unsigned changed = 0;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 180; x++)
        if ((bits[y*240+x] & 0xFFFFFF) != 0x224466) changed++;
    check(changed > 0, "static caption paints glyphs");
    check(SendMessageA(control, 0x31, 0, 0) == 0, "initial control font is unset");
    HANDLE font = GetStockObject(13);
    SendMessageA(control, 0x30, (UPTR)font, 0);
    check((HANDLE)SendMessageA(control, 0x31, 0, 0) == font, "WM_SETFONT/GETFONT preserve caller handle");
    check((SendMessageA(control, 0x87, 0, 0) & 0x100) != 0, "static dialog-code classification");
    DestroyWindow(control);
    check(GetObjectType(font) == 6, "control destruction does not delete borrowed font");
}
static void buttons(void)
{
    HANDLE push = child("BUTTON", "&Next", 0x10000, 201);
    HANDLE automatic = child("BUTTON", "Auto", 3, 202);
    HANDLE manual = child("BUTTON", "Manual", 2, 203);
    HANDLE three = child("BUTTON", "Three", 6, 204);
    unsigned before = clicks;
    SendMessageA(automatic, 0xF5, 0, 0);
    check(SendMessageA(automatic, 0xF0, 0, 0) == 1 && clicks == before+1 &&
          last_id == 202 && last_window == automatic, "auto checkbox toggles before BN_CLICKED");
    SendMessageA(automatic, 0xF5, 0, 0);
    check(SendMessageA(automatic, 0xF0, 0, 0) == 0, "auto checkbox clears on second click");
    SendMessageA(manual, 0xF5, 0, 0);
    check(SendMessageA(manual, 0xF0, 0, 0) == 0, "manual checkbox does not auto toggle");
    SendMessageA(manual, 0xF1, 1, 0);
    check(SendMessageA(manual, 0xF0, 0, 0) == 1, "BM_SETCHECK updates checkbox");
    SendMessageA(push, 0xF1, 1, 0);
    check(SendMessageA(push, 0xF0, 0, 0) == 0, "BM_SETCHECK does not check push button");
    SendMessageA(three, 0xF5, 0, 0); SendMessageA(three, 0xF5, 0, 0);
    check(SendMessageA(three, 0xF0, 0, 0) == 2, "auto three-state reaches indeterminate");
    SendMessageA(three, 0xF5, 0, 0);
    check(SendMessageA(three, 0xF0, 0, 0) == 0, "auto three-state wraps to unchecked");
    HANDLE radio_a = child("BUTTON", "A", 9|0x20000, 205);
    HANDLE radio_b = child("BUTTON", "B", 9, 206);
    HANDLE radio_c = child("BUTTON", "C", 9|0x20000, 207);
    SendMessageA(radio_c, 0xF5, 0, 0); SendMessageA(radio_a, 0xF5, 0, 0);
    SendMessageA(radio_b, 0xF5, 0, 0);
    check(!SendMessageA(radio_a, 0xF0, 0, 0) && SendMessageA(radio_b, 0xF0, 0, 0) == 1 &&
          SendMessageA(radio_c, 0xF0, 0, 0) == 1, "auto radio only clears siblings in its dialog group");
    check(!(GetWindowLongA(radio_a, -16) & 0x10000) &&
          (GetWindowLongA(radio_b, -16) & 0x10000), "selected radio is the group tab stop");
    before = clicks;
    SendMessageA(push, 0x202, 0, 5|(5<<16));
    check(clicks == before, "button-up without a press does not click");
    SetFocus(push);
    SendMessageA(push, 0x201, 1, 5|(5<<16));
    check(GetCapture() == push && (SendMessageA(push, 0xF2, 0, 0) & 4), "button press captures pointer and sets pushed state");
    SendMessageA(push, 0x200, 1, 220|(50<<16));
    check(!(SendMessageA(push, 0xF2, 0, 0) & 4), "dragging outside clears pushed state");
    SendMessageA(push, 0x202, 0, 220|(50<<16));
    check(clicks == before && GetCapture() != push, "release outside cancels click and capture");
    SendMessageA(push, 0x201, 1, 5|(5<<16));
    SendMessageA(push, 0x202, 0, 5|(5<<16));
    check(clicks == before+1 && GetCapture() != push, "press and release inside sends one click");
    SendMessageA(push, 0x100, 32, 0); SendMessageA(push, 0x101, 32, 0);
    check(clicks == before+2, "space key pair clicks focused button");
    SendMessageA(push, 0xF4, 1, 0);
    check((GetWindowLongA(push, -16) & 15) == 1 &&
          (SendMessageA(push, 0x87, 0, 0) & 0x10), "BM_SETSTYLE updates default-button classification");
    print(automatic); memcpy(previous, bits, sizeof(previous));
    SendMessageA(automatic, 0xF1, 1, 0); print(automatic);
    unsigned difference = 0;
    for (unsigned i = 0; i < 240*80; i++) difference += (previous[i]&0xFFFFFF) != (bits[i]&0xFFFFFF);
    check(difference > 0, "checkbox checked state changes rendered pixels");
    DestroyWindow(push); DestroyWindow(automatic); DestroyWindow(manual); DestroyWindow(three);
    DestroyWindow(radio_a); DestroyWindow(radio_b); DestroyWindow(radio_c);
}
static void ownership(void)
{
    HANDLE own = child("BUTTON", "", 11, 301);
    unsigned before = draws;
    print(own);
    check(draws > before && (bits[0]&0xFFFFFF) == 0x55AA33, "owner-drawn button delegates pixels to parent");
    bleed = 1; print(own); bleed = 0;
    check((bits[181]&0xFFFFFF) == 0x112233 && (bits[25*240]&0xFFFFFF) == 0x112233,
          "owner drawing is clipped to the control client");
    HANDLE window_dc = GetDC(own);
    RECT client, update = {4, 4, 10, 10};
    GetClientRect(own, &client);
    FillRect(window_dc, &client, tint);
    ValidateRect(own, 0); InvalidateRect(own, &update, 0); UpdateWindow(own);
    BOOL copied = BitBlt(dc, 0, 0, client.right, client.bottom,
        window_dc, 0, 0, 0x00CC0020);
    GdiFlush();
    check(copied && (bits[0]&0xFFFFFF) == 0x224466 &&
          (bits[5*240+5]&0xFFFFFF) == 0x55AA33,
          "button painting preserves the BeginPaint update clip");
    ReleaseDC(own, window_dc);
    destroy_draw = 1; print(own);
    check(!IsWindow(own), "destruction from owner-draw callback is safe");
    own = child("STATIC", "", 13, 302);
    before = draws; print(own);
    check(draws > before && (bits[0]&0xFFFFFF) == 0x55AA33, "owner-drawn static delegates pixels to parent");
    DestroyWindow(own);
    HANDLE text = child("STATIC", "destroy", 0, 303);
    destroy_color = 1; print(text);
    check(!IsWindow(text), "destruction from control-color callback is safe");
    DWORD data[16*8];
    for (unsigned i = 0; i < 16*8; i++) data[i] = 0x002468AC;
    HANDLE bitmap = CreateBitmap(16, 8, 1, 32, data);
    HANDLE image = child("STATIC", "", 14, 304);
    check(SendMessageA(image, 0x172, 0, (IPTR)bitmap) == 0 &&
          (HANDLE)SendMessageA(image, 0x173, 0, 0) == bitmap, "static bitmap handle is retained and queryable");
    RECT rect; GetClientRect(image, &rect);
    check(rect.right == 16 && rect.bottom == 8, "static bitmap sizes to the image");
    print(image);
    check((bits[0]&0xFFFFFF) == 0x2468AC, "static bitmap pixels are presented");
    DestroyWindow(image);
    check(GetObjectType(bitmap) == 7, "static does not delete caller-owned bitmap");
    image = CreateWindowExA(0, "STATIC", "", 0x5000020Eu,
        5, 5, 12, 6, parent, (HANDLE)305, instance, 0);
    SendMessageA(image, 0x172, 0, (IPTR)bitmap);
    GetClientRect(image, &rect);
    check(rect.right == 12 && rect.bottom == 6, "centered static preserves caller geometry");
    HANDLE clip = CreateRectRgn(0, 0, rect.right, rect.bottom);
    SelectClipRgn(dc, clip); print(image);
    check((bits[12]&0xFFFFFF) == 0x112233 && (bits[6*240]&0xFFFFFF) == 0x112233,
          "static bitmap printing respects the caller DC clip");
    SelectClipRgn(dc, 0); DeleteObject(clip);
    DestroyWindow(image);
    DeleteObject(bitmap);
}

static DWORD CALL text_worker(void *unused)
{
    (void)unused;
    static const WCHAR cls[] = {'S','T','A','T','I','C',0};
    static const WCHAR name[] = {'w','o','r','k','e','r',0};
    worker_window = CreateWindowExW(0, cls, name, 0x80000000u,
        0, 0, 20, 20, 0, 0, instance, 0);
    SetEvent(worker_ready);
    unsigned char message[48];
    while (GetMessageW(message, 0, 0, 0) > 0) DispatchMessageW(message);
    if (worker_window) DestroyWindow(worker_window);
    return 0;
}

static void threaded_text(void)
{
    worker_ready = CreateEventA(0, 1, 0, 0);
    DWORD tid = 0;
    HANDLE thread = CreateThread(0, 0, text_worker, 0, 0, &tid);
    if (!thread || !worker_ready) { check(0, "start text worker"); return; }
    BOOL ready = WaitForSingleObject(worker_ready, 5000) == 0 && worker_window;
    check(ready, "worker owns a Unicode control");
    if (ready) {
        const WCHAR text[] = {'t',0x03A9,0}; WCHAR readback[16]; char ansi[16];
        check(SendMessageW(worker_window, 0xC, 0, (IPTR)text) &&
              SendMessageW(worker_window, 0xD, 16, (IPTR)readback) == 2 &&
              readback[1] == text[1], "cross-thread Unicode send keeps encoding and result");
        SendMessageA(worker_window, 0xC, 0, (IPTR)"worker text");
        check(GetWindowTextA(worker_window, ansi, 16) == 11 && ansi[10] == 't',
              "cross-thread ANSI send converts to Unicode procedure");
    }
    PostThreadMessageW(tid, 0x12, 0, 0);
    check(WaitForSingleObject(thread, 5000) == 0, "text worker shuts down and releases control");
    CloseHandle(thread); CloseHandle(worker_ready);
}
void mainCRTStartup(void)
{
    instance = GetModuleHandleA(0);
    set_long_ptr = GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowLongPtrA");
    if (sizeof(void *) == 8 && !set_long_ptr) ExitProcess(2);
    WNDCLASS cls = {0, owner, 0, 0, instance, 0, 0, (HANDLE)16, 0, class_name};
    if (!RegisterClassA(&cls)) ExitProcess(2);
    parent = CreateWindowExA(0, class_name, "Controls", 0x90000000u,
        30, 30, 500, 300, 0, 0, instance, 0);
    dc = CreateCompatibleDC(0);
    BITMAPINFO info = {40, 240, -80, 1, 32, 0, 0, 0, 0, 0, 0};
    dib = CreateDIBSection(dc, &info, 0, (void **)&bits, 0, 0);
    if (!parent || !dc || !dib) ExitProcess(2);
    HANDLE old = SelectObject(dc, dib);
    ink = CreateSolidBrush(0x0033AA55); tint = CreateSolidBrush(0x00664422);
    text_contracts(); buttons(); ownership(); threaded_text();
    DestroyWindow(parent); UnregisterClassA(class_name, instance);
    SelectObject(dc, old); DeleteObject(dib); DeleteDC(dc);
    DeleteObject(ink); DeleteObject(tint);
    report("[USER32-CONTROLS] checks="); number(checks);
    report(" failures="); number(failures); report(failures ? " FAIL\n" : " PASS\n");
    ExitProcess(failures ? 3 : 0);
}
