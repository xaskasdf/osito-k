/* No-renderer contract. Run with standard VGA, not Venus or another GL backend. */
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

typedef struct {
    unsigned short size, version;
    DWORD flags;
    unsigned char fields[20];
    DWORD layer_mask, visible_mask, damage_mask;
} PFD;
_Static_assert(sizeof(PFD) == 40, "pixel format layout");

typedef HANDLE (CALL *create_window_fn)(DWORD, const char *, const char *,
    DWORD, int, int, int, int, HANDLE, HANDLE, HANDLE, void *);
typedef HANDLE (CALL *get_dc_fn)(HANDLE);
typedef int (CALL *release_dc_fn)(HANDLE, HANDLE);
typedef BOOL (CALL *destroy_window_fn)(HANDLE);
typedef int (CALL *describe_fn)(HANDLE, int, DWORD, PFD *);
typedef BOOL (CALL *set_format_fn)(HANDLE, int, const PFD *);
typedef HANDLE (CALL *create_context_fn)(HANDLE);
typedef HANDLE (CALL *create_attribs_fn)(HANDLE, HANDLE, const int *);
typedef BOOL (CALL *delete_context_fn)(HANDLE);
typedef BOOL (CALL *make_current_fn)(HANDLE, HANDLE);
typedef HANDLE (CALL *get_current_fn)(void);
typedef void *(CALL *get_proc_fn)(const char *);
typedef const unsigned char *(CALL *get_string_fn)(UINT);
typedef BOOL (CALL *pixel_attrib_fn)(HANDLE, int, int, UINT,
                                    const int *, int *);

static UINT failures;
static void report(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, 0);
}
static void check(BOOL condition, const char *text)
{
    if (!condition) { failures++; report(text); report("\n"); }
}
static void *resolve(HANDLE module, const char *name)
{
    void *function = GetProcAddress(module, name);
    if (!function) {
        report("WGL-BACKEND FAIL missing "); report(name); report("\n");
        ExitProcess(2);
    }
    return function;
}

void mainCRTStartup(void)
{
    HANDLE user = LoadLibraryA("user32.dll");
    HANDLE gdi = LoadLibraryA("gdi32.dll");
    HANDLE gl = LoadLibraryA("opengl32.dll");
    if (!user || !gdi || !gl) ExitProcess(2);
    create_window_fn create_window = (create_window_fn)resolve(user, "CreateWindowExA");
    get_dc_fn get_dc = (get_dc_fn)resolve(user, "GetDC");
    release_dc_fn release_dc = (release_dc_fn)resolve(user, "ReleaseDC");
    destroy_window_fn destroy_window = (destroy_window_fn)resolve(user, "DestroyWindow");
    describe_fn describe = (describe_fn)resolve(gdi, "DescribePixelFormat");
    set_format_fn set_format = (set_format_fn)resolve(gdi, "SetPixelFormat");
    create_context_fn create_context = (create_context_fn)resolve(gl, "wglCreateContext");
    delete_context_fn delete_context = (delete_context_fn)resolve(gl, "wglDeleteContext");
    make_current_fn make_current = (make_current_fn)resolve(gl, "wglMakeCurrent");
    get_current_fn get_current = (get_current_fn)resolve(gl, "wglGetCurrentContext");
    get_current_fn get_current_dc = (get_current_fn)resolve(gl, "wglGetCurrentDC");
    get_proc_fn get_proc = (get_proc_fn)resolve(gl, "wglGetProcAddress");
    get_string_fn get_string = (get_string_fn)resolve(gl, "glGetString");
    create_attribs_fn create_attribs = (create_attribs_fn)get_proc("wglCreateContextAttribsARB");
    pixel_attrib_fn pixel_attrib = (pixel_attrib_fn)get_proc("wglGetPixelFormatAttribivARB");
    HANDLE window = create_window(0, "STATIC", "WGL backend contract",
        0x00CF0000, 20, 20, 160, 120, 0, 0, 0, 0);
    HANDLE dc = window ? get_dc(window) : 0;
    if (!dc) { report("WGL-BACKEND FAIL no window/DC\n"); ExitProcess(2); }
    PFD pfd;
    if (!describe(dc, 1, sizeof(pfd), &pfd)) ExitProcess(2);
    check(!(pfd.flags & 0x20), "FAIL advertised OpenGL without renderer");
    check(!(pfd.flags & 0x40), "FAIL advertised nonexistent GDI GL renderer");
    check((pfd.flags & 0x10) != 0, "FAIL missing GDI capability");
    check(set_format(dc, 1, &pfd), "FAIL set pixel format");
    check(get_current() == 0 && get_current_dc() == 0, "FAIL initial binding");
    check(get_string(0x1F02) == 0, "FAIL GL_VERSION without current context");
    SetLastError(0);
    HANDLE invalid = create_context(0);
    check(invalid == 0 && GetLastError() == 6, "FAIL invalid DC accepted");
    if (invalid) delete_context(invalid);
    for (UINT i = 0; i < 80; i++) {
        SetLastError(0);
        HANDLE context = create_context(dc);
        DWORD error = GetLastError();
        if (context) {
            report("WGL-BACKEND INVALID-SUCCESS\n");
            delete_context(context);
        }
        check(!context && error == 50, "FAIL no-backend context/error");
    }
    if (create_attribs) {
        const int attribs[] = {0x2091, 4, 0x2092, 5, 0};
        SetLastError(0);
        HANDLE context = create_attribs(dc, 0, attribs);
        check(!context && GetLastError() == 50, "FAIL ARB no-backend context/error");
        if (context) delete_context(context);
    }
    if (pixel_attrib) {
        int attributes[] = {0x2010, 0x2011}, values[] = {-1, -1};
        check(pixel_attrib(dc, 1, 0, 2, attributes, values) &&
              !values[0] && !values[1],
              "FAIL ARB advertised GL/double-buffer without renderer");
    }
    check(!get_current() && !get_current_dc(), "FAIL failed create changed binding");
    check(make_current(0, 0), "FAIL unbind without context");
    check(release_dc(window, dc) != 0 && destroy_window(window), "FAIL cleanup");
    report(failures ? "WGL-BACKEND FAIL\n" : "WGL-BACKEND PASS\n");
    ExitProcess(failures ? 1 : 0);
}
