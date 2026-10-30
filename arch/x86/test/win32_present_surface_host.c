/* Actual SHM + presentation shim; deterministic window retirement in flight.
 * Compile twice: TEST_WGL selects the WGL bridge instead of DirectDraw. */
#include "../include/types.h"
#include "../include/paging.h"
#include "../include/compositor_surface.h"
#undef PHYS_TO_VIRT
#undef VIRT_TO_PHYS
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(p) ((uint64_t)(uintptr_t)(p))
#define SHM_HOST_TEST
#include "../kernel/shm.c"
#ifndef PRESENT_TEST_SOURCE
#ifdef TEST_WGL
#define PRESENT_TEST_SOURCE "../win32/opengl32_shim.c"
#else
#define PRESENT_TEST_SOURCE "../win32/ddraw_shim.c"
#endif
#endif
#include PRESENT_TEST_SOURCE

extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern int posix_memalign(void **, size_t, size_t);
extern void abort(void) __attribute__((noreturn));
#define CHECK(value) do { checks++; if (!(value)) { \
    if (failures < 20) { printf("FAIL %d: %s\n", __LINE__, #value); } failures++; \
} } while (0)
#define WINDOW ((HANDLE)(ULONG_PTR)0xA0000042u)
#define OTHER_WINDOW ((HANDLE)(ULONG_PTR)0xA0000043u)
#define DC ((HANDLE)(ULONG_PTR)0xDC000001u)
#define BACKEND ((void *)(uintptr_t)0x1234)
#define BG 0x31223344u
static unsigned checks, failures, allocated, freed, pins, acquisitions;
static uint32_t live_handle, old_handle, live_pitch, live_rows, live_w, live_h;
static uint32_t live_left, live_top;
static uint32_t *live_pixels, *old_pixels, retired_pixels[45];
static uint32_t desktop[24], framebuffer_pixels[24];
static unsigned dirty, flips, flushes, reads, swaps, backend_creates, backend_resizes;
static int action, fail_acquire, malformed, baseline, display_enabled, fb_enabled;
static int backend_error, retire_on_backend, caps_enabled = 1;
static DWORD last_error;
static void *quarantine;
static BOOL had_retirement;
static int compat32;
static void retire_window(void);
int *proc_win32_compat32_mode_slot(void) { return &compat32; }

void serial_puts(const char *s)
{
#ifndef TEST_WGL
    if (action && !old_handle && !strcmp(s, "[DDRAW] present src ")) retire_window();
#else
    (void)s;
#endif
}
void serial_putdec(uint64_t v) { (void)v; }
void serial_puthex(uint64_t v, int n) { (void)v; (void)n; }
void *mem_alloc_aligned(uint64_t bytes, uint64_t alignment)
{
    void *p = NULL;
    if (posix_memalign(&p, alignment, bytes)) return NULL;
    allocated++;
    return p;
}
void *mem_alloc_pages(uint64_t count) { return mem_alloc_aligned(count * 4096, 4096); }
void mem_free_pages(void *p, uint64_t pages)
{
    (void)pages;
    freed++;
    if (p == old_pixels) {
        memcpy(retired_pixels, p, sizeof(retired_pixels));
        had_retirement = TRUE;
    }
    /* The failing baseline records early retirement without host UAF. */
    if (baseline && p == old_pixels) quarantine = p;
    else free(p);
}
uint32_t proc_exec_pid(void) { return 1; }
const char *proc_current_name(void) { return "present-lifetime"; }
DWORD win32_current_process_id(void) { return 1; }
void WINAPI SetLastError(DWORD error) { last_error = error; }
uint32_t *fb_get_base(void) { return fb_enabled ? framebuffer_pixels : NULL; }
uint32_t fb_get_width(void) { return 4; }
uint32_t fb_get_height(void) { return 4; }
uint32_t fb_get_pitch(void) { return 6; }
void fb_flush_all(void) { flushes++; }
uint32_t *display_get_back_buffer(void) { return display_enabled ? desktop : NULL; }
uint32_t display_get_width(void) { return 4; }
uint32_t display_get_height(void) { return 4; }
uint32_t display_get_pitch(void) { return 6; }
void display_flip(void) { flips++; }
BOOL user32_get_window_surface(HANDLE window, void **pixels, int *w, int *h, int *pitch)
{
    if (window != WINDOW || !live_handle || fail_acquire) return FALSE;
    *pixels = live_pixels + live_top * live_pitch + live_left;
    *w = live_w; *h = live_h; *pitch = live_pitch * 4;
    return TRUE;
}
BOOL user32_acquire_window_dc_surface(HANDLE window, BOOL whole,
                                       compositor_surface_ref_t *surface)
{
    CHECK(!whole);
    *surface = (compositor_surface_ref_t){0};
    if (window != WINDOW || !live_handle || fail_acquire) return FALSE;
    uint32_t *pixels = shm_map(live_handle);
    if (!pixels) return FALSE;
    surface->handle = live_handle;
    surface->pixels = pixels + live_top * live_pitch + live_left;
    surface->width = live_w; surface->height = live_h; surface->pitch = live_pitch;
    if (malformed == 1) surface->width = 0;
    if (malformed == 2) surface->pitch = live_w - 1;
    if (malformed == 3) surface->pixels = NULL;
    if (malformed == 4) surface->pitch = 0x20000000u;
    pins++; acquisitions++;
    return TRUE;
}
void compositor_release_surface(compositor_surface_ref_t *surface)
{
    uint32_t handle = surface->handle;
    *surface = (compositor_surface_ref_t){0};
    if (handle) { CHECK(pins == 1); pins--; shm_unmap(handle); }
}
void user32_mark_window_dirty(HANDLE window) { CHECK(window == WINDOW); dirty++; }
uint32_t user32_get_window_compositor_id(void *window)
{ CHECK(window == WINDOW); return live_handle ? 1 : 0; }
uint32_t compositor_create_window(uint32_t h, int16_t x, int16_t y,
    uint16_t w, uint16_t ht, uint32_t pid, const char *name)
{ (void)h; (void)x; (void)y; (void)w; (void)ht; (void)pid; (void)name; return 0; }
void compositor_set_fullscreen(uint32_t h, bool full) { (void)h; (void)full; }
void compositor_signal_dirty(uint32_t h) { (void)h; }
uint32_t compositor_find_window_by_shm(uint32_t h) { (void)h; return 0; }
bool compositor_is_running(void) { return false; }

static void create_surface(BOOL small)
{
    live_pitch = small ? 4 : 9; live_rows = small ? 4 : 5;
    live_w = small ? 2 : 5; live_h = small ? 2 : 3;
    live_left = small ? 1 : 2; live_top = 1;
    live_handle = shm_create_surface(live_pitch, live_rows, SHM_SURFACE_FLAGS);
    live_pixels = shm_map(live_handle);
    CHECK(live_handle && live_pixels);
    if (!live_pixels) abort();
    for (unsigned i = 0; i < live_pitch * live_rows; i++) live_pixels[i] = BG;
}
static void retire_window(void)
{
    old_handle = live_handle; old_pixels = live_pixels;
    if (action == 2) { live_handle = 0; live_pixels = NULL; }
    else create_surface(TRUE);
#ifndef TEST_WGL
    if (action == 3) ddraw_processes[0].hwnd = OTHER_WINDOW;
#endif
    shm_unmap(old_handle); shm_destroy(old_handle);
    CHECK(shm_get_size(old_handle) != 0);
}
static void setup(void)
{
    action = fail_acquire = malformed = backend_error = retire_on_backend = 0;
    old_handle = 0; old_pixels = NULL; had_retirement = FALSE;
    dirty = flips = flushes = reads = swaps = backend_creates = backend_resizes = 0;
    display_enabled = fb_enabled = caps_enabled = 1;
    for (unsigned i = 0; i < 24; i++) desktop[i] = framebuffer_pixels[i] = BG;
    create_surface(FALSE);
}
static void finish(void)
{
    CHECK(!pins && !shm_get_size(old_handle));
    if (live_handle) { shm_unmap(live_handle); shm_destroy(live_handle); live_handle = 0; }
    CHECK(!shm_get_active_count());
    if (quarantine) { free(quarantine); quarantine = NULL; }
}
static void check_retired(const uint32_t expected[15])
{
    CHECK(had_retirement);
    for (unsigned y = 0; y < 5; y++)
        for (unsigned x = 0; x < 9; x++) {
            uint32_t value = x >= 2 && x < 7 && y >= 1 && y < 4
                ? expected[(y - 1) * 5 + x - 2] : BG;
            CHECK(retired_pixels[y * 9 + x] == value);
        }
    if (live_handle)
        for (unsigned i = 0; i < live_pitch * live_rows; i++) CHECK(live_pixels[i] == BG);
    for (unsigned i = 0; i < 24; i++) CHECK(desktop[i] == BG && framebuffer_pixels[i] == BG);
    CHECK(!flips && !flushes);
}

#ifdef TEST_WGL
BOOL gdi32_rasterize_glyph(HDC hdc, WCHAR character, GDI_FONT_BITMAP *bitmap)
{ (void)hdc; (void)character; (void)bitmap; abort(); }
DWORD WINAPI GetLastError(void) { return last_error; }
HGDIOBJ WINAPI GetCurrentObject(HDC hdc, UINT type)
{ (void)hdc; (void)type; abort(); }
int win32_abi_lookup(const char *dll, const char *name, uint8_t *argc, uint8_t *cc)
{ (void)dll; (void)name; (void)argc; (void)cc; abort(); }
int32_t proc_current_pid(void) { return 2; }
int32_t proc_current_tgid(void) { return 1; }
HANDLE gdi32_window_from_dc(HANDLE hdc) { return hdc == DC ? WINDOW : NULL; }
uint32_t vg3d_caps(void) { return caps_enabled ? GPU_CAP_VENUS_READY : 0; }
void *dl_open_private(const char *name, uint32_t owner) { (void)name; (void)owner; abort(); }
void *dl_sym(void *handle, const char *name) { (void)handle; (void)name; abort(); }
int dl_discard(void *handle) { (void)handle; abort(); }
static int fake_readback(void *context, void *pixels, int w, int h, int pitch)
{
    CHECK(context == BACKEND && w == 5 && h == 3 && pitch == 36);
    reads++;
    if (action) retire_window();
    if (backend_error == 1) return -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            ((uint32_t *)((BYTE *)pixels + y * pitch))[x] = 0x4080c0u + y * 5 + x;
    return 0;
}
static int fake_swap(void *context)
{
    CHECK(context == BACKEND); CHECK(!action || !had_retirement);
    swaps++;
    return backend_error == 2 ? -1 : 0;
}
static void *fake_create(uint32_t id, int w, int h)
{
    CHECK(id == 1 && w == 5 && h == 3);
    backend_creates++;
    if (retire_on_backend) retire_window();
    return backend_error ? NULL : BACKEND;
}
static int fake_resize(void *context, uint32_t id, int w, int h)
{
    CHECK(context == BACKEND && id == 1 && w == 5 && h == 3);
    backend_resizes++;
    if (retire_on_backend) retire_window();
    return backend_error ? -1 : 0;
}
static int fake_current(void *context) { CHECK(!context || context == BACKEND); return 0; }
static void setup_wgl(void)
{
    setup();
    memset(contexts, 0, sizeof(contexts));
    memset(thread_bindings, 0, sizeof(thread_bindings));
    memset(module_instances, 0, sizeof(module_instances));
    OGL_MODULE_INSTANCE *module = &module_instances[0];
    module->used = 1; module->owner_tgid = 1;
    module->create_context = fake_create; module->resize_context = fake_resize;
    module->make_current = fake_current; module->readback = fake_readback;
    module->swap_buffers = fake_swap;
    contexts[0] = (OGL_CONTEXT){1, 1, DC, WINDOW, BACKEND, module, 5, 3};
    thread_bindings[0] = (OGL_THREAD_BINDING){1, 1, 2, (HGLRC)(ULONG_PTR)(OGL_CONTEXT_BASE + 1), DC, 0};
}
static void wgl_swap_contract(int retire, int error)
{
    setup_wgl(); action = retire; backend_error = error;
    CHECK(opengl32_swap_buffers(DC) == !error);
    CHECK(reads == 1 && swaps == (error != 1) && dirty == !error);
    uint32_t expected[15];
    for (unsigned i = 0; i < 15; i++) expected[i] = error == 1 ? BG : 0x4080c0u + i;
    check_retired(expected);
    finish();
}
static void wgl_control_contract(void)
{
    for (int create = 0; create < 2; create++)
        for (int error = 0; error < 2; error++) {
            setup_wgl(); action = 2; backend_error = error; retire_on_backend = 1;
            if (create) CHECK((ogl_wglCreateContext(DC) != NULL) == !error);
            else CHECK(ogl_wglMakeCurrent(DC, thread_bindings[0].context) == !error);
            CHECK(backend_creates == (unsigned)create && backend_resizes == (unsigned)!create);
            CHECK(had_retirement && !pins);
            finish();
        }
    for (int kind = 0; kind < 7; kind++) {
        setup_wgl();
        if (!kind) fail_acquire = 1;
        else if (kind <= 4) malformed = kind;
        else if (kind == 5) module_instances[0].readback = NULL;
        else module_instances[0].swap_buffers = NULL;
        CHECK(!opengl32_swap_buffers(DC));
        CHECK(!reads && !swaps && !dirty && !pins);
        finish();
    }
    setup_wgl(); caps_enabled = 0; last_error = 0;
    CHECK(!ogl_wglCreateContext(DC) && last_error == 50 && !pins);
    CHECK(!backend_creates);
    finish();
}
#else
static void setup_ddraw(void)
{
    setup();
    memset(ddraw_processes, 0, sizeof(ddraw_processes));
    ddraw_processes[0].used = 1; ddraw_processes[0].owner_pid = 1;
    ddraw_processes[0].hwnd = WINDOW;
    g_present_owner_pid = g_present_src_w = g_present_src_h = 0;
}
static void ddraw_copy_contract(int retire, unsigned bpp, BOOL bottom_up)
{
    setup_ddraw(); action = retire;
    static const uint32_t colors[6] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0, 0xffff00};
    static const uint16_t rgb565[6] = {0xf800, 0x07e0, 0x001f, 0xffff, 0, 0xffe0};
    uint32_t source32[10] = {0}, palette[256] = {0};
    uint16_t source16[10] = {0};
    uint8_t source8[10] = {0};
    for (unsigned y = 0; y < 2; y++)
        for (unsigned x = 0; x < 3; x++) {
            unsigned at = (bottom_up ? 1 - y : y) * 5 + x, color = y * 3 + x;
            source32[at] = colors[color]; source16[at] = rgb565[color];
            source8[at] = color; palette[color] = colors[color];
        }
    int pitch = bpp / 8 * 5, offset = bottom_up ? pitch : 0;
    const void *pixels = bpp == 32 ? (void *)source32 : bpp == 16 ? (void *)source16 : (void *)source8;
    ddraw_present_pixels((const BYTE *)pixels + offset, 3, 2, bpp,
                          bottom_up ? -pitch : pitch, palette);
    uint32_t expected[15];
    static const unsigned columns[5] = {0, 0, 1, 1, 2}, rows[3] = {0, 0, 1};
    for (unsigned y = 0; y < 3; y++)
        for (unsigned x = 0; x < 5; x++) expected[y * 5 + x] = colors[rows[y] * 3 + columns[x]];
    check_retired(expected);
    CHECK(dirty == 1);
    finish();
}
static void ddraw_control_contract(void)
{
    uint32_t source[4] = {0x123456, 0x123456, 0x123456, 0x123456};
    for (int kind = 0; kind < 4; kind++) {
        setup_ddraw();
        fail_acquire = !kind; malformed = kind;
        ddraw_present_pixels(source, 2, 2, 32, 8, NULL);
        CHECK(!dirty && !flips && !flushes && !pins);
        for (unsigned i = 0; i < 24; i++) CHECK(desktop[i] == BG && framebuffer_pixels[i] == BG);
        finish();
    }
    for (int display = 0; display < 2; display++) {
        setup_ddraw(); ddraw_processes[0].hwnd = NULL; display_enabled = display;
        unsigned before = acquisitions;
        ddraw_present_pixels(source, 2, 2, 32, 8, NULL);
        CHECK(acquisitions == before && !dirty && flips == (unsigned)display && flushes == (unsigned)!display);
        for (unsigned y = 0; y < 4; y++)
            for (unsigned x = 0; x < 6; x++) {
                uint32_t *target = display ? desktop : framebuffer_pixels;
                CHECK(target[y * 6 + x] == (x < 4 ? source[0] : BG));
            }
        finish();
    }
}
#endif

/* Export tables retained by ASan reference services outside the test paths. */
PVOID WINAPI VirtualAlloc(PVOID p, SIZE_T n, DWORD kind, DWORD flags)
{ (void)p; (void)n; (void)kind; (void)flags; abort(); }
BOOL WINAPI VirtualFree(PVOID p, SIZE_T n, DWORD kind)
{ (void)p; (void)n; (void)kind; abort(); }
uint32_t compat32_make_thunk_ex(uint64_t p, const char *name, uint8_t n, uint8_t convention)
{ (void)p; (void)name; (void)n; (void)convention; abort(); }
uint32_t compat32_callback_args(uint32_t p, int n, const uint32_t *args)
{ (void)p; (void)n; (void)args; abort(); }

int main(int argc, char **argv)
{
    (void)argv; baseline = argc > 1; shm_init();
#ifdef TEST_WGL
    if (baseline) wgl_swap_contract(1, 0);
    else {
        for (int retire = 1; retire <= 2; retire++)
            for (int error = 0; error < 3; error++) wgl_swap_contract(retire, error);
        wgl_control_contract();
    }
    const char *name = "WGL";
#else
    if (baseline) { ddraw_copy_contract(1, 32, FALSE); ddraw_control_contract(); }
    else {
        for (int retire = 1; retire <= 3; retire++)
            for (unsigned bpp = 8; bpp <= 32; bpp *= 2)
                for (int bottom_up = 0; bottom_up < 2; bottom_up++)
                    ddraw_copy_contract(retire, bpp, bottom_up);
        ddraw_control_contract();
    }
    const char *name = "DirectDraw";
#endif
    CHECK(allocated == freed && !pins && !shm_get_active_count());
    printf("%s surface lifetime: %u checks, %u failures; allocated=%u freed=%u pins=%u\n",
           name, checks, failures, allocated, freed, pins);
    return failures ? 1 : 0;
}
