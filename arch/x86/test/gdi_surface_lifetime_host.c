/* Production GDI + SHM. USER32 publication is interrupted deterministically. */
#include "../include/types.h"
#include "../include/paging.h"
#include "../include/compositor_surface.h"
#undef PHYS_TO_VIRT
#undef VIRT_TO_PHYS
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(p) ((uint64_t)(uintptr_t)(p))
#define SHM_HOST_TEST
#define GDI_HOST_TEST
#include "../kernel/shm.c"
#ifndef GDI_SURFACE_TEST_SOURCE
#define GDI_SURFACE_TEST_SOURCE "../win32/gdi32_shim.c"
#endif
#include GDI_SURFACE_TEST_SOURCE

extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern int posix_memalign(void **, size_t, size_t);
extern void abort(void) __attribute__((noreturn));

#define WINDOW ((HANDLE)(ULONG_PTR)0xA0000042u)
#define BG 0xff202020u
#define COLOR 0xff4080c0u
#define CHECK(value) do { checks++; if (!(value)) { \
    if (failures < 20) { printf("FAIL %d: %s\n", __LINE__, #value); } \
    failures++; } } while (0)
static unsigned checks, failures, allocations, releases, pins, acquisitions;
static DWORD host_last_error;
static unsigned live_handle, old_handle, live_width, live_height;
static uint32_t *live_pixels, *old_pixels, retired_pixels[64];
static uint32_t source_pixels[64], copied_pixels[64];
static int action, on_acquire, on_trace, compat32, baseline, fail_acquire;
static void *quarantine;
static HDC window_dc, memory;
static BOOL had_retirement;
const uint8_t gui_font8x16[95][16] = {{0}};
uint32_t *fb_get_base(void) { return NULL; }
uint32_t fb_get_width(void) { return 0; }
uint32_t fb_get_height(void) { return 0; }
uint32_t fb_get_pitch(void) { return 0; }

static void retire_window(void);
void serial_puts(const char *s)
{
    if (on_trace && !strcmp(s, "[GDI-BLIT] dst=0x")) {
        on_trace = 0;
        retire_window();
    }
}
void serial_putdec(uint64_t v) { (void)v; }
void serial_puthex(uint64_t v, int n) { (void)v; (void)n; }
void *kmalloc(uint64_t n)
{
#ifdef GDI_HOST_TEST_ALLOCATE
    return GDI_HOST_TEST_ALLOCATE(n);
#else
    return malloc(n);
#endif
}
void kfree(void *p)
{
#ifdef GDI_HOST_TEST_FREE
    GDI_HOST_TEST_FREE(p);
#else
    free(p);
#endif
}
void *mem_alloc_aligned(uint64_t bytes, uint64_t alignment)
{
    void *p = NULL;
    if (posix_memalign(&p, alignment, bytes)) return NULL;
    allocations++;
    return p;
}
void mem_free_pages(void *p, uint64_t pages)
{
    (void)pages;
    releases++;
    if (p == old_pixels) {
        memcpy(retired_pixels, p, sizeof(retired_pixels));
        had_retirement = TRUE;
    }
    if (baseline && p == old_pixels) quarantine = p;
    else free(p);
}
uint32_t proc_exec_pid(void) { return 1; }
const char *proc_current_name(void) { return "gdi-lifetime"; }
DWORD WINAPI GetCurrentProcessId(void)
{
#ifdef GDI_HOST_TEST_PROCESS_ID
    return GDI_HOST_TEST_PROCESS_ID;
#else
    return 1;
#endif
}
DWORD WINAPI GetCurrentThreadId(void) { return 1; }
int win32_user_range_readable(const void *pointer, SIZE_T size, BOOL compat)
{
    (void)size; (void)compat;
    return pointer != NULL;
}
int win32_user_range_writable(void *pointer, SIZE_T size, BOOL compat)
{
    return win32_user_range_readable(pointer, size, compat);
}
bool win32_resolve_file_path(PCSTR path, char out[260])
{
    if (!path) return false;
    unsigned length = 0;
    while (path[length] && length < 259) {
        out[length] = path[length];
        length++;
    }
    out[length] = 0;
    return !path[length];
}
DWORD WINAPI GetSysColor(int index) { (void)index; return 0; }
void WINAPI SetLastError(DWORD value) { host_last_error = value; }
int *proc_win32_compat32_mode_slot(void) { return &compat32; }
HANDLE WINAPI GetDesktopWindow(void) { return (HANDLE)(ULONG_PTR)0xAFFFFFFFu; }
BOOL user32_get_window_dc_surface(HANDLE window, void **pixels, int *w,
                                   int *h, int *pitch, BOOL whole)
{
    (void)whole;
    if (window != WINDOW || !live_handle) return FALSE;
    *pixels = live_pixels; *w = live_width; *h = live_height; *pitch = live_width * 4;
    return TRUE;
}
BOOL user32_acquire_window_dc_surface(HANDLE window, BOOL whole,
                                       compositor_surface_ref_t *surface)
{
    (void)whole;
    *surface = (compositor_surface_ref_t){0};
    if (window != WINDOW || !live_handle || fail_acquire) return FALSE;
    surface->pixels = shm_map(live_handle);
    if (!surface->pixels) return FALSE;
    surface->handle = live_handle;
    surface->width = surface->pitch = live_width;
    surface->height = live_height;
    pins++; acquisitions++;
    if (on_acquire) { on_acquire = 0; retire_window(); }
    return TRUE;
}
void compositor_release_surface(compositor_surface_ref_t *surface)
{
    uint32_t handle = surface->handle;
    *surface = (compositor_surface_ref_t){0};
    if (handle) { CHECK(pins > 0); pins--; shm_unmap(handle); }
}
void user32_mark_window_dirty(HANDLE window) { CHECK(window == WINDOW); }
uint32_t compositor_create_window(uint32_t h, int16_t x, int16_t y,
    uint16_t w, uint16_t ht, uint32_t pid, const char *name)
{ (void)h; (void)x; (void)y; (void)w; (void)ht; (void)pid; (void)name; return 0; }
void compositor_set_fullscreen(uint32_t h, bool full) { (void)h; (void)full; }
void compositor_signal_dirty(uint32_t h) { (void)h; }
uint32_t compositor_find_window_by_shm(uint32_t h) { (void)h; return 0; }
bool compositor_is_running(void) { return false; }

/* ASan retains the export tables. Unrelated services must never be called. */
BOOL opengl32_has_accelerated_backend(void) { abort(); }
BOOL opengl32_swap_buffers(HANDLE dc) { (void)dc; abort(); }
void dll_debug_log_address(PVOID p) { (void)p; abort(); }
void *krealloc(void *p, uint64_t n) { (void)p; (void)n; abort(); }
bool vfs_find(const char *p, int m, vfs_node_t *n) { (void)p; (void)m; (void)n; abort(); }
int vfs_read(vfs_node_t *n, uint64_t o, void *b, uint64_t l)
{ (void)n; (void)o; (void)b; (void)l; abort(); }
bool win32_normalize_path(PCSTR p, char out[260]) { (void)p; (void)out; abort(); }
uint32_t compat32_callback_args(uint32_t fn, int n, const uint32_t *args)
{ (void)fn; (void)n; (void)args; abort(); }
BOOL WINAPI UnmapViewOfFile(PCVOID p) { (void)p; abort(); }
BOOL WINAPI VirtualFree(PVOID p, SIZE_T n, DWORD k)
{
    (void)n; (void)k;
#ifdef GDI_HOST_USER_MEMORY
    free(p);
    return TRUE;
#else
    (void)p; abort();
#endif
}
PVOID WINAPI VirtualAlloc(PVOID p, SIZE_T n, DWORD k, DWORD f)
{
    (void)p; (void)k; (void)f;
#ifdef GDI_HOST_USER_MEMORY
    void *memory = malloc(n);
    if (memory) memset(memory, 0, n);
    return memory;
#else
    (void)n; abort();
#endif
}
PVOID WINAPI MapViewOfFile(HANDLE f, DWORD a, DWORD h, DWORD l, SIZE_T n)
{ (void)f; (void)a; (void)h; (void)l; (void)n; abort(); }
int WINAPI MultiByteToWideChar(DWORD c, DWORD f, PCSTR s, int n, PWSTR d, int k)
{ (void)c; (void)f; (void)s; (void)n; (void)d; (void)k; abort(); }

static void create_surface(unsigned width, unsigned height)
{
    live_handle = shm_create_surface(width, height, SHM_SURFACE_FLAGS);
    live_pixels = shm_map(live_handle);
    live_width = width; live_height = height;
    CHECK(live_handle && live_pixels);
    if (!live_pixels) abort();
    for (unsigned i = 0; i < width * height; i++) live_pixels[i] = BG;
}
static void retire_window(void)
{
    old_handle = live_handle;
    old_pixels = live_pixels;
    if (action == 1) create_surface(2, 3);
    else { live_handle = 0; live_pixels = NULL; }
    shm_unmap(old_handle);
    shm_destroy(old_handle);
    CHECK(shm_get_size(old_handle) != 0);
    if (action == 2) gdi32_release_window_dc(WINDOW);
}
static void setup(BOOL clip)
{
    old_handle = 0; old_pixels = NULL; had_retirement = FALSE;
    create_surface(8, 8);
    window_dc = gdi32_alloc_window_dc(WINDOW);
    CHECK(window_dc != NULL);
    if (clip) {
        uint64_t token;
        GDI_RECT rectangle = {1, 1, 7, 7};
        CHECK(gdi32_push_paint_clip(window_dc, &rectangle, &token));
    }
    for (unsigned i = 0; i < 64; i++) {
        source_pixels[i] = COLOR;
        copied_pixels[i] = 0;
    }
    int index = alloc_dc();
    CHECK(index >= 0);
    GDI_DC *dc = &gdi_dcs[index];
    dc->surface = source_pixels; dc->width = dc->height = 8;
    dc->pitch = 32; dc->bpp = 32;
    memory = (HDC)(ULONG_PTR)(DC_TAG | (unsigned)index);
}
static void finish(void)
{
    CHECK(!pins && !shm_get_size(old_handle));
    if (live_handle) { shm_unmap(live_handle); shm_destroy(live_handle); live_handle = 0; }
    if (baseline) {
        /* The frozen source has privileged brush locks. This baseline only
         * reproduces replacement; its two unclipped DCs own no allocations. */
        memset(&gdi_dcs[DC_INDEX(window_dc)], 0, sizeof(GDI_DC));
        memset(&gdi_dcs[DC_INDEX(memory)], 0, sizeof(GDI_DC));
    } else {
        gdi32_release_window_dc(WINDOW);
        CHECK(DeleteDC(memory));
    }
    CHECK(!shm_get_active_count());
    if (quarantine) { free(quarantine); quarantine = NULL; }
}
static void draw_contract(unsigned kind, int retire, BOOL clip)
{
    unsigned before = failures, acquired = acquisitions;
    setup(clip);
    action = retire;
    if (baseline) { on_trace = 1; gdi_blit_trace_count = 0; }
    else on_acquire = 1;
    GDI_RECT rectangle = {0, 0, 8, 8};
    GDI_BITMAP_HEADER bitmap = {40, 8, -8, 1, 32, 0, 0, 0, 0, 0, 0};
    uint32_t expected = COLOR;
    switch (kind) {
    case 0: CHECK(BitBlt(window_dc, 0, 0, 8, 8, memory, 0, 0, ROP_SRCCOPY)); break;
    case 1: CHECK(gdi32_fill_rect(window_dc, &rectangle, STOCK_WHITE_BRUSH)); expected = 0xffffffffu; break;
    case 2:
        rectangle = (GDI_RECT){3, 3, 5, 5};
        CHECK(gdi32_draw_focus_rect(window_dc, &rectangle)); expected = BG ^ 0xffffffu; break;
    case 3: {
        GDI_DC *dc = dc_from_handle(window_dc);
        dc->text_align = 1; dc->bk_color = 0xffffffu;
        const WCHAR text[] = {'a', 'b'};
        CHECK(gdi_ext_text_out(window_dc, 0, 0, 2, &rectangle, text, 2, NULL));
        if (retire == 1) CHECK(dc->current_x == 16 && dc->current_y == 0);
        expected = 0xffffffffu; break;
    }
    case 4:
        CHECK(gdi32_copy_layered_pixels(window_dc, 0, 0, 8, 8, copied_pixels, TRUE));
        for (unsigned i = 0; i < 64; i++) CHECK(copied_pixels[i] == BG);
        expected = BG; break;
    case 5: gdi32_clear_window_coverage(window_dc); expected = 0; break;
    case 6: CHECK(StretchBlt_k32(window_dc, 0, 0, 8, 8, memory, 0, 0, 4, 4, ROP_SRCCOPY)); break;
    case 7: CHECK(StretchDIBits_k32(window_dc, 0, 0, 8, 8, 0, 0, 4, 4, source_pixels, &bitmap, 0, ROP_SRCCOPY) > 0); break;
    case 8: CHECK(SetDIBitsToDevice_k32(window_dc, 0, 0, 8, 8, 0, 0, 0, 8, source_pixels, &bitmap, 0) > 0); break;
    case 9: {
        GDI_TRIVERTEX vertices[] = {{0, 0, 0x4000, 0x8000, 0xc000, 0xffff},
                                   {8, 8, 0x4000, 0x8000, 0xc000, 0xffff}};
        GDI_GRADIENT_RECT mesh = {0, 1};
        CHECK(GradientFill_k32(window_dc, vertices, 2, &mesh, 1, 0)); break;
    }
    case 10: CHECK(TransparentBlt_k32(window_dc, 0, 0, 8, 8, memory, 0, 0, 8, 8, 0)); break;
    case 11: CHECK(AlphaBlend_k32(window_dc, 0, 0, 8, 8, memory, 0, 0, 8, 8, 0x01ff0000)); break;
    case 12: {
        DWORD (WINAPI *get_pixel)(HDC, int, int) =
            (DWORD (WINAPI *)(HDC, int, int))gdi32_resolve("GetPixel", 0, FALSE);
        CHECK(get_pixel && (get_pixel(window_dc, 3, 3) & 0xffffffu) == (BG & 0xffffffu));
        expected = BG; break;
    }
    case 13: {
        int index = alloc_bmp();
        CHECK(index >= 0);
        GDI_BITMAP *image = &gdi_bmps[index];
        image->pixels = source_pixels; image->width = image->height = 8;
        image->bpp = 32; image->pitch = 32;
        CHECK(gdi32_draw_icon_bitmap(window_dc, (HBITMAP)(ULONG_PTR)(BMP_TAG | (unsigned)index), NULL, 0, 0, 8, 8));
        memset(image, 0, sizeof(*image)); break;
    }
    case 14:
        CHECK(BitBlt(memory, 0, 0, 8, 8, window_dc, 0, 0, ROP_SRCCOPY));
        for (unsigned i = 0; i < 64; i++) CHECK(source_pixels[i] == BG);
        expected = BG; break;
    case 15:
        CHECK(BitBlt(window_dc, 0, 1, 8, 7, window_dc, 0, 0, ROP_SRCCOPY));
        expected = BG; break;
    case 16:
        CHECK(Arc(window_dc, 2, 3, 5, 6, 9, 4, 9, 4));
        expected = 0xff000000u; break;
    case 17:
        CHECK(SelectObject(window_dc, GetStockObject(8)) == STOCK_BLACK_PEN);
        CHECK(Pie(window_dc, 0, 0, 8, 8, 9, 4, 9, 4));
        expected = 0xffffffffu; break;
    }
    CHECK(!on_acquire && !on_trace && had_retirement);
    CHECK(retired_pixels[3 * 8 + 3] == expected);
    if (clip) CHECK(retired_pixels[0] == BG);
    if (!baseline) CHECK(acquisitions == acquired + 1);
    if (live_handle) {
        for (unsigned i = 0; i < 6; i++) CHECK(live_pixels[i] == BG);
        CHECK(BitBlt(window_dc, 0, 0, 8, 8, memory, 0, 0, ROP_SRCCOPY));
    }
    finish();
    if (before != failures) printf("draw kind=%u retire=%d clip=%d\n", kind, retire, clip);
}
static void early_returns(void)
{
    setup(FALSE);
    CHECK(!gdi32_fill_rect(window_dc, NULL, STOCK_WHITE_BRUSH));
    CHECK(!BitBlt(window_dc, 0, 0, 8, 8, NULL, 0, 0, ROP_SRCCOPY));
    CHECK(!gdi32_copy_layered_pixels(window_dc, 0, 0, 8, 8, NULL, TRUE));
    CHECK(!gdi_ext_text_out(window_dc, 0, 0, 0, NULL, NULL, 1, NULL));
    CHECK(!GradientFill_k32(window_dc, NULL, 0, NULL, 0, 0));
    CHECK(!AlphaBlend_k32(window_dc, 0, 0, 0, 8, memory, 0, 0, 8, 8, 0xff0000));
    CHECK(!pins);
    fail_acquire = 1;
    CHECK(!BitBlt(window_dc, 0, 0, 8, 8, memory, 0, 0, ROP_SRCCOPY));
    fail_acquire = 0;
    CHECK(!pins);
    for (unsigned i = 0; i < 64; i++) CHECK(live_pixels[i] == BG);
    finish();
}
int main(int argc, char **argv)
{
    (void)argv;
    shm_init();
    baseline = argc > 1;
    if (baseline) draw_contract(0, 1, FALSE);
    else {
        for (unsigned kind = 0; kind < 18; kind++)
            for (int retire = 1; retire <= 2; retire++)
                for (int clip = 0; clip < 2; clip++) draw_contract(kind, retire, clip);
        early_returns();
        /* Reject invalid early deletion according to the documented GDI
         * contract. Live and saved DC references must both pin the pen. */
        HDC first = CreateCompatibleDC(NULL), second = CreateCompatibleDC(NULL);
        HPEN pen = CreatePen(0, 1, 0x332211);
        CHECK(first && second && pen);
        CHECK(SelectObject(first, pen) == STOCK_BLACK_PEN);
        CHECK(SelectObject(second, pen) == STOCK_BLACK_PEN);
        CHECK(SaveDC(first) == 1 && SaveDC(first) == 2);
        CHECK(!DeleteObject(pen));
        CHECK(SelectObject(first, STOCK_BLACK_PEN) == pen);
        CHECK(DeleteDC(second) && !DeleteObject(pen));
        CHECK(RestoreDC(first, 1) && GetCurrentObject(first, 1) == pen);
        CHECK(!DeleteObject(pen));
        CHECK(DeleteDC(first) && DeleteObject(pen));
        CHECK(!gdi_pens);
        /* A pattern pen owns its bitmap snapshot independently of the
         * source handle and remains pinned by a saved DC selection. */
        compat32 = 0;
        DWORD pattern_bits[4] = {0x123456, 0xABCDEF, 0x2468AC, 0x13579B};
        DWORD pattern_colors[4] = {0x563412, 0xEFCDAB, 0xAC6824, 0x9B5713};
        DWORD blank[64] = {0};
        HBITMAP pattern = CreateBitmap(2, 2, 1, 32, pattern_bits);
        HBITMAP target = CreateBitmap(8, 8, 1, 32, blank);
        struct { DWORD style, color; ULONG_PTR hatch; } brush = {3, 0, (ULONG_PTR)pattern};
        pen = ExtCreatePen(0x10200, 8, &brush, 0, NULL);
        first = CreateCompatibleDC(NULL);
        CHECK(pattern && target && pen && first);
        CHECK(DeleteObject(pattern));
        HGDIOBJ original = SelectObject(first, target);
        CHECK(SelectObject(first, pen) == STOCK_BLACK_PEN && SaveDC(first) == 1);
        CHECK(!DeleteObject(pen));
        GDI_POINT line[] = {{0, 4}, {8, 4}};
        CHECK(Polyline_k32(first, line, 2));
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                CHECK(GetPixel_k32(first, x, y) == pattern_colors[(y % 2) * 2 + x % 2]);
        CHECK(SelectObject(first, STOCK_BLACK_PEN) == pen && !DeleteObject(pen));
        CHECK(RestoreDC(first, 1) && GetCurrentObject(first, 11) == pen);
        CHECK(SelectObject(first, original) == target);
        CHECK(DeleteDC(first) && DeleteObject(pen) && DeleteObject(target));
        CHECK(!gdi_pens);
        first = CreateCompatibleDC(NULL);
        /* Process cleanup also frees a selected extended pen and its copy. */
        pattern = CreateBitmap(2, 2, 1, 32, pattern_bits);
        brush.hatch = (ULONG_PTR)pattern;
        pen = ExtCreatePen(0x10200, 3, &brush, 0, NULL);
        CHECK(pen && DeleteObject(pattern));
        CHECK(SelectObject(first, pen) == STOCK_BLACK_PEN && SaveDC(first) == 1);
        gdi_dc_release_process(1);
        CHECK(!gdi_pens && !dc_from_handle(first));
    }
    CHECK(allocations == releases && !pins && !shm_get_active_count());
    printf("GDI surface lifetime: %u checks, %u failures; allocated=%u freed=%u pins=%u\n",
           checks, failures, allocations, releases, pins);
    return failures ? 1 : 0;
}
