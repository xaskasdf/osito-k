/* Native Windows reference and PE32/PE64 Osito WGL layer contracts. */
typedef unsigned int DWORD, UINT;
typedef int BOOL;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void CALL ExitProcess(UINT);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API DWORD CALL GetLastError(void);
API void CALL SetLastError(DWORD);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
int _fltused;
void *memset(void *p, int value, __SIZE_TYPE__ count)
{ unsigned char *out = p; while (count--) *out++ = (unsigned char)value; return p; }
void *memcpy(void *p, const void *s, __SIZE_TYPE__ count)
{ unsigned char *out = p; const unsigned char *in = s; while (count--) *out++ = *in++; return p; }
typedef struct {
    unsigned short size, version;
    DWORD flags;
    unsigned char fields[20];
    DWORD layer_mask, visible_mask, damage_mask;
} PFD;
_Static_assert(sizeof(PFD) == 40, "PFD ABI");
typedef HANDLE (CALL *window_fn)(DWORD, const char *, const char *, DWORD,
    int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
typedef HANDLE (CALL *dc_fn)(HANDLE);
typedef int (CALL *release_fn)(HANDLE, HANDLE);
typedef BOOL (CALL *destroy_fn)(HANDLE);
typedef int (CALL *choose_fn)(HANDLE, const PFD *);
typedef int (CALL *describe_fn)(HANDLE, int, UINT, PFD *);
typedef BOOL (CALL *set_fn)(HANDLE, int, const PFD *);
typedef HANDLE (CALL *layer_context_fn)(HANDLE, int);
typedef BOOL (CALL *layer_describe_fn)(HANDLE, int, int, UINT, void *);
typedef int (CALL *layer_entries_fn)(HANDLE, int, int, int, const DWORD *);
typedef BOOL (CALL *layer_realize_fn)(HANDLE, int, BOOL);
typedef BOOL (CALL *layer_swap_fn)(HANDLE, UINT);
typedef BOOL (CALL *font_fn)(HANDLE, DWORD, DWORD, DWORD);
typedef BOOL (CALL *delete_fn)(HANDLE);
typedef BOOL (CALL *current_fn)(HANDLE, HANDLE);
typedef HANDLE (CALL *get_current_fn)(void);
typedef UINT (CALL *gl_error_fn)(void);
typedef unsigned char (CALL *is_list_fn)(UINT);
typedef void (CALL *delete_lists_fn)(UINT, int);

static unsigned failures, checks;
static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}
static void check(BOOL condition, const char *name)
{
    checks++;
    report(condition ? "WGLLAYER PASS " : "WGLLAYER FAIL ");
    report(name); report("\n");
    if (!condition) failures++;
}
static void hex(DWORD value)
{
    char text[9];
    for (int i = 0; i < 8; i++) text[i] = "0123456789abcdef"[(value >> ((7 - i) * 4)) & 15];
    text[8] = 0; report(text);
}
static void observation(BOOL value, const char *name)
{
    DWORD error = GetLastError();
    report("WGLLAYER OBS "); report(name); report(" value="); hex((DWORD)value);
    report(" error="); hex(error); report("\n");
}
static void *resolve(HANDLE module, const char *name)
{
    void *result = GetProcAddress(module, name);
    if (!result) { report("WGLLAYER FAIL missing "); report(name); report("\n"); ExitProcess(2); }
    return result;
}

void mainCRTStartup(void)
{
    HANDLE user = LoadLibraryA("user32.dll"), gdi = LoadLibraryA("gdi32.dll");
    HANDLE gl = LoadLibraryA("opengl32.dll");
    window_fn window = (window_fn)resolve(user, "CreateWindowExA");
    dc_fn get_dc = (dc_fn)resolve(user, "GetDC");
    release_fn release = (release_fn)resolve(user, "ReleaseDC");
    destroy_fn destroy = (destroy_fn)resolve(user, "DestroyWindow");
    choose_fn choose = (choose_fn)resolve(gdi, "ChoosePixelFormat");
    describe_fn describe = (describe_fn)resolve(gdi, "DescribePixelFormat");
    set_fn set = (set_fn)resolve(gdi, "SetPixelFormat");
    layer_context_fn create = (layer_context_fn)resolve(gl, "wglCreateLayerContext");
    layer_describe_fn layer = (layer_describe_fn)resolve(gl, "wglDescribeLayerPlane");
    layer_entries_fn entries = (layer_entries_fn)resolve(gl, "wglSetLayerPaletteEntries");
    layer_realize_fn realize = (layer_realize_fn)resolve(gl, "wglRealizeLayerPalette");
    layer_swap_fn swap = (layer_swap_fn)resolve(gl, "wglSwapLayerBuffers");
    font_fn font = (font_fn)resolve(gl, "wglUseFontBitmapsW");
    delete_fn remove = (delete_fn)resolve(gl, "wglDeleteContext");
    current_fn current = (current_fn)resolve(gl, "wglMakeCurrent");
    get_current_fn get_current = (get_current_fn)resolve(gl, "wglGetCurrentContext");
    get_current_fn get_current_dc = (get_current_fn)resolve(gl, "wglGetCurrentDC");
    gl_error_fn error = (gl_error_fn)resolve(gl, "glGetError");
    is_list_fn is_list = (is_list_fn)resolve(gl, "glIsList");
    delete_lists_fn delete_lists = (delete_lists_fn)resolve(gl, "glDeleteLists");
    HANDLE hwnd = window(0, "STATIC", "WGL layer contracts", 0x00CF0000,
        20, 20, 160, 120, 0, 0, 0, 0);
    HANDLE dc = hwnd ? get_dc(hwnd) : 0;
    if (!dc) { report("WGLLAYER FAIL window/DC\n"); ExitProcess(2); }
    PFD request = {0}, actual;
    request.size = sizeof(request); request.version = 1;
    request.flags = 0x25; request.fields[1] = 32; request.fields[15] = 24;
    int format = choose(dc, &request);
    check(format > 0 && describe(dc, format, sizeof(actual), &actual) > 0,
          "describe selected pixel format");
    check(actual.fields[19] == 0, "fixture has no overlays or underlays");
    check(set(dc, format, &actual), "set pixel format");
    unsigned char descriptor[64];
    DWORD colors[] = {0x102030, 0x405060};
    for (int plane = -1; plane <= 1; plane++) {
        memset(descriptor, 0xA5, sizeof(descriptor));
        check(!layer(dc, format, plane, sizeof(descriptor), descriptor),
              "absent layer descriptor rejected");
        BOOL unchanged = 1;
        for (unsigned i = 0; i < sizeof(descriptor); i++)
            if (descriptor[i] != 0xA5) unchanged = 0;
        check(unchanged, "failed descriptor preserves output");
        check(entries(dc, plane, 0, 2, colors) == 0, "absent layer palette rejected");
        check(!realize(dc, plane, 1), "absent layer realization rejected");
        if (plane) check(!create(dc, plane), "absent layer context rejected");
    }
    check(!font(dc, 'A', 1, 100), "font requires current GL context");
    check(!create(0, 0), "layer context rejects invalid DC");
    check(!font(0, 'A', 1, 100), "font rejects invalid DC");
    check(!get_current() && !get_current_dc(), "failures preserve empty binding");
    SetLastError(0); observation(swap(dc, 0), "zero-plane swap before context");
    SetLastError(0); observation(swap(dc, 2), "absent-plane swap before context");
    check(!swap(dc, 0) && !swap(dc, 2), "layer swaps require a current context");
    HANDLE context = create(dc, 0);
    if (actual.flags & 0x20) {
        check(context != 0, "main layer creates real advertised GL context");
        if (context) {
            check(current(dc, context), "bind main context");
            check(get_current() == context && get_current_dc() == dc, "current binding");
            check(font(dc, 32, 95, 100), "compile selected GDI font bitmaps");
            BOOL lists = 1;
            for (unsigned i = 0; i < 95; i++) if (!is_list(100 + i)) lists = 0;
            check(lists && error() == 0, "all glyph display lists exist without GL error");
            SetLastError(0); observation(swap(dc, 0), "zero-plane swap with context");
            SetLastError(0); observation(swap(dc, 2), "absent-plane swap with context");
            check(swap(dc, 0) && swap(dc, 2), "empty and absent planes have nothing to swap");
            check(swap(dc, 1), "swap main plane");
            delete_lists(100, 95);
            check(current(0, 0) && remove(context), "release real context");
        }
    } else {
        check(!context && GetLastError() == 50, "main layer reports unavailable renderer");
        check(!swap(dc, 1), "no renderer cannot swap main plane");
    }
    check(release(hwnd, dc) != 0 && destroy(hwnd), "release fixture");
    report("WGLLAYER DONE checks="); hex(checks); report(" failures="); hex(failures); report("\n");
    ExitProcess(failures ? 1 : 0);
}
