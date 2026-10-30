/* Production GDI and console redirect, with independently padded surfaces. */
#include "../include/types.h"
#ifndef GDI_SCREEN_TEST_SOURCE
#define GDI_SCREEN_TEST_SOURCE "../win32/gdi32_shim.c"
#endif
#include GDI_SCREEN_TEST_SOURCE
#include "../kernel/framebuffer.c"

extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern void abort(void) __attribute__((noreturn));

#define COUNT 1024
#define GUARD 8
#define SENTINEL 0x5AA55AA5u
#define WINDOW ((HANDLE)(ULONG_PTR)0xA0000042u)
#define CHECK(x) do { checks++; if (!(x)) { \
    if (failures < 20) printf("FAIL %d: %s\n", __LINE__, #x); \
    failures++; } } while (0)
static unsigned checks, failures, presentations, window_dirty;
static uint32_t console_pixels[COUNT], shadow_pixels[COUNT], terminal_pixels[COUNT];
static uint32_t screen_a[COUNT], screen_b[COUNT], window_pixels[COUNT];
static uint32_t source_pixels[COUNT], readback[COUNT];
static uint32_t *display_pixels;
static uint32_t screen_w, screen_h, screen_pitch, logical_bpp = 32;
static int window_ready, compat32;
static DWORD test_last_error;

const uint8_t gui_font8x16[95][16] = {{0}};
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int digits) { (void)n; (void)digits; }
void *kmalloc(uint64_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
int *proc_win32_compat32_mode_slot(void) { return &compat32; }
DWORD WINAPI GetCurrentProcessId(void) { return 1; }
DWORD WINAPI GetCurrentThreadId(void) { return 1; }
DWORD WINAPI GetSysColor(int index) { (void)index; return 0; }
void WINAPI SetLastError(DWORD error) { test_last_error = error; }
DWORD WINAPI GetLastError(void) { return test_last_error; }
HANDLE WINAPI GetDesktopWindow(void) { return (HANDLE)(ULONG_PTR)0xAFFFFFFFu; }
uint32_t *display_get_back_buffer(void) { return display_pixels; }
uint32_t display_get_width(void) { return screen_w; }
uint32_t display_get_height(void) { return screen_h; }
uint32_t display_get_pitch(void) { return screen_pitch; }
void display_force_refresh(void) { presentations++; }
BOOL user32_get_current_display_mode(uint32_t *w, uint32_t *h,
                                     uint32_t *bpp, uint32_t *hz)
{
    if (w) *w = screen_w;
    if (h) *h = screen_h;
    if (bpp) *bpp = logical_bpp;
    if (hz) *hz = 60;
    return TRUE;
}
BOOL user32_get_window_dc_surface(HANDLE window, void **pixels, int *w,
                                   int *h, int *pitch, BOOL whole)
{
    (void)whole;
    if (window != WINDOW || !window_ready) return FALSE;
    *pixels = window_pixels + GUARD;
    *w = 5; *h = 4; *pitch = 8 * 4;
    return TRUE;
}
void user32_mark_window_dirty(HANDLE window)
{
    CHECK(window == WINDOW);
    window_dirty++;
}

/* ASan retains export tables; these unrelated services must never be called. */
BOOL opengl32_has_accelerated_backend(void) { abort(); }
BOOL opengl32_swap_buffers(HANDLE dc) { (void)dc; abort(); }
void dll_debug_log_address(PVOID p) { (void)p; abort(); }
void *krealloc(void *p, uint64_t n) { (void)p; (void)n; abort(); }
bool vfs_find(const char *path, int mode, vfs_node_t *out)
{ (void)path; (void)mode; (void)out; abort(); }
int vfs_read(vfs_node_t *node, uint64_t off, void *buf, uint64_t len)
{ (void)node; (void)off; (void)buf; (void)len; abort(); }
bool win32_normalize_path(PCSTR path, char out[260])
{ (void)path; (void)out; abort(); }
uint32_t compat32_callback_args(uint32_t fn, int count, const uint32_t *args)
{ (void)fn; (void)count; (void)args; abort(); }
BOOL WINAPI UnmapViewOfFile(PCVOID p) { (void)p; abort(); }
BOOL WINAPI VirtualFree(PVOID p, SIZE_T n, DWORD kind)
{ (void)p; (void)n; (void)kind; abort(); }
PVOID WINAPI VirtualAlloc(PVOID p, SIZE_T n, DWORD kind, DWORD flags)
{ (void)p; (void)n; (void)kind; (void)flags; abort(); }
PVOID WINAPI MapViewOfFile(HANDLE file, DWORD access, DWORD hi, DWORD lo, SIZE_T n)
{ (void)file; (void)access; (void)hi; (void)lo; (void)n; abort(); }
int WINAPI MultiByteToWideChar(DWORD cp, DWORD flags, PCSTR src, int n,
                                PWSTR out, int count)
{ (void)cp; (void)flags; (void)src; (void)n; (void)out; (void)count; abort(); }

static void fill(uint32_t *pixels)
{
    for (unsigned i = 0; i < COUNT; i++) pixels[i] = SENTINEL;
}

static HDC memory_dc(uint32_t *pixels, int w, int h, int pitch)
{
    int index = alloc_dc();
    CHECK(index >= 0);
    if (index < 0) return NULL;
    gdi_dcs[index].surface = pixels;
    gdi_dcs[index].width = w;
    gdi_dcs[index].height = h;
    gdi_dcs[index].pitch = pitch * 4;
    gdi_dcs[index].bpp = 32;
    return (HDC)(ULONG_PTR)(DC_TAG | (unsigned)index);
}

static void contents(uint32_t *pixels, unsigned w, unsigned h, unsigned pitch)
{
    for (unsigned i = 0; i < COUNT; i++) {
        unsigned at = i - GUARD, y = at / pitch, x = at % pitch;
        uint32_t expected = i >= GUARD && y < h && x < w
            ? source_pixels[y * 32 + x] : SENTINEL;
        CHECK(pixels[i] == expected);
    }
}

static void untouched(uint32_t *pixels)
{
    for (unsigned i = 0; i < COUNT; i++) CHECK(pixels[i] == SENTINEL);
}

int main(void)
{
    fill(console_pixels); fill(shadow_pixels); fill(terminal_pixels);
    fill(screen_a); fill(screen_b); fill(window_pixels); fill(readback);
    for (unsigned i = 0; i < COUNT; i++) source_pixels[i] = 0xFF000000u | i;
    fb_init(console_pixels + GUARD, 13, 8, 16);
    fb_enable_shadow(shadow_pixels + GUARD);
    HDC source = memory_dc(source_pixels, 24, 20, 32);
    HDC screen = CreateDCA("DISPLAY", NULL, NULL, NULL);
    GDI_DC *dc = dc_from_handle(screen);
    CHECK(dc && dc->width == 13 && dc->height == 8 && dc->pitch == 64);
    CHECK(BitBlt(screen, 0, 0, 13, 8, source, 0, 0, ROP_SRCCOPY));
    contents(shadow_pixels, 13, 8, 16);
    contents(console_pixels, 13, 8, 16);
    CHECK(presentations == 0);

    /* Retain the pre-desktop DC across console redirection. */
    fb_redirect(terminal_pixels + GUARD, 12, 6, 16);
    CHECK(fb_is_redirected());
    display_pixels = screen_a + GUARD;
    screen_w = 20; screen_h = 10; screen_pitch = 24;
    dc = dc_from_handle(screen);
    CHECK(dc && dc->surface == display_pixels && dc->width == 20 &&
          dc->height == 10 && dc->pitch == 96);
    CHECK(BitBlt(screen, 0, 0, 20, 10, source, 0, 0, ROP_SRCCOPY));
    contents(screen_a, 20, 10, 24);
    untouched(terminal_pixels);
    CHECK(presentations == 1);
    HDC desktop = gdi32_alloc_window_dc(GetDesktopWindow());
    CHECK(gdi32_window_from_dc(desktop) == GetDesktopWindow());
    gdi32_free_screen_dc(desktop);
    CHECK(gdi32_alloc_window_dc(GetDesktopWindow()) == desktop);

    /* A logical 16-bit mode still has a 32-bit native screen buffer. */
    logical_bpp = 16;
    display_pixels = screen_b + GUARD;
    screen_w = 9; screen_h = 7; screen_pitch = 12;
    CHECK(GetDeviceCaps(desktop, BITSPIXEL) == 16);
    CHECK(BitBlt(desktop, 0, 0, 9, 7, source, 0, 0, ROP_SRCCOPY));
    dc = dc_from_handle(desktop);
    CHECK(dc && dc->bpp == 32 && dc->pitch == 48 && dc->width == 9 && dc->height == 7);
    contents(screen_b, 9, 7, 12);
    untouched(terminal_pixels);
    CHECK(presentations == 2 && window_dirty == 0);

    /* Reading a screen DC must use its native pitch too. */
    HDC destination = memory_dc(readback + GUARD, 9, 7, 12);
    CHECK(BitBlt(destination, 0, 0, 9, 7, desktop, 0, 0, ROP_SRCCOPY));
    contents(readback, 9, 7, 12);
    CHECK(presentations == 2);

    /* Unavailable surfaces fail closed; they do not target terminal or screen. */
    HDC window = gdi32_alloc_window_dc(WINDOW);
    dc = dc_from_handle(window);
    CHECK(dc && !dc->is_screen && !dc->surface);
    CHECK(!BitBlt(window, 0, 0, 5, 4, source, 0, 0, ROP_SRCCOPY));
    window_ready = 1;
    CHECK(BitBlt(window, 0, 0, 5, 4, source, 0, 0, ROP_SRCCOPY));
    contents(window_pixels, 5, 4, 8);
    CHECK(window_dirty == 1 && presentations == 2);
    window_ready = 0;
    CHECK(!BitBlt(window, 0, 0, 5, 4, source, 0, 0, ROP_SRCCOPY));

    screen_pitch = 8;
    CHECK(!dc_from_handle(desktop)->surface);
    screen_pitch = 0xFFFFFFFFu; screen_h = 0xFFFFFFFFu;
    CHECK(!dc_from_handle(desktop)->surface);
    display_pixels = NULL;
    CHECK(!dc_from_handle(desktop)->surface);
    CHECK(!BitBlt(desktop, 0, 0, 5, 4, source, 0, 0, ROP_SRCCOPY));
    untouched(terminal_pixels);
    printf("GDI screen: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
