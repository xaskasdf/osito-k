/* Production GDI glyphs -> production WGL -> real host Mesa display lists.
 * EGL supplies the test context only; this does not qualify Osito's backend
 * creation, GPU driver or presentation path. No OpenGL calls are mocked. */
/* GDI uses its layout-compatible private RECT declaration for this weak
 * service. It is unused here; avoid a cross-TU prototype clash in the test. */
#define GetWindowRect gdi_test_GetWindowRect
#include "../win32/gdi32_shim.c"
#undef GetWindowRect
#include "../win32/opengl32_shim.c"
#include <EGL/egl.h>

extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern void abort(void) __attribute__((noreturn));
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static unsigned checks, failures;
static DWORD last_error;
static int compat32;
static const char *missing_symbol;
#define CHECK(x) do { checks++; if (!(x)) { \
    if (failures < 20) printf("FAIL %d: %s (error %u, gl %x)\n", \
        __LINE__, #x, last_error, glGetError()); failures++; } } while (0)

void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int width) { (void)n; (void)width; }
void *kmalloc(uint64_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
int *proc_win32_compat32_mode_slot(void) { return &compat32; }
DWORD WINAPI GetCurrentProcessId(void) { return 1; }
int32_t proc_current_pid(void) { return 2; }
int32_t proc_current_tgid(void) { return 1; }
void WINAPI SetLastError(DWORD error) { last_error = error; }
DWORD WINAPI GetLastError(void) { return last_error; }
DWORD WINAPI GetSysColor(int index) { (void)index; return 0; }
void user32_mark_window_dirty(HANDLE window) { (void)window; abort(); }
void compositor_release_surface(compositor_surface_ref_t *surface)
{ if (surface->handle) abort(); }
void *dl_sym(void *handle, const char *name)
{
    return missing_symbol && !strcmp(missing_symbol, name) ? NULL :
        dlsym(handle, name);
}

int main(void)
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor, count;
    EGLConfig config;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    const EGLint size[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    if (!eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(display, attributes, &config, 1, &count) || !count) {
        printf("FAIL real EGL context setup: %x\n", eglGetError()); return 2;
    }
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (!surface || !context || !eglMakeCurrent(display, surface, surface, context)) {
        printf("FAIL real EGL context binding: %x\n", eglGetError()); return 2;
    }
    void *module = dlopen("libGL.so.1", 2);
    if (!module) return 2;
    printf("WGL-FONT HOST GL %s / %s\n", glGetString(GL_VERSION),
        glGetString(GL_RENDERER));

    int slot = alloc_dc();
    CHECK(slot >= 0);
    if (slot < 0) return 2;
    HDC dc = (HDC)(ULONG_PTR)(DC_TAG | (unsigned)slot);
    GDI_DC *source = &gdi_dcs[slot];
    uint32_t untouched[64];
    for (unsigned i = 0; i < 64; i++) untouched[i] = 0xAB123456u;
    source->surface = untouched; source->width = 8; source->height = 8;
    source->pitch = 32; source->bpp = 32;
    source->viewport_x = 17; source->viewport_y = -8;
    source->text_color = 0x102030u;
    GDI_DC before = *source;
    GDI_FONT_BITMAP glyph;
    CHECK(!gdi32_rasterize_glyph(NULL, 'A', &glyph) && last_error == 6);
    CHECK(!gdi32_rasterize_glyph(dc, 'A', NULL) && last_error == 87);
    for (unsigned character = 32; character < 127; character++) {
        CHECK(gdi32_rasterize_glyph(dc, (WCHAR)character, &glyph));
        CHECK(glyph.width == 8 && glyph.height == 16 && !glyph.origin_x &&
              glyph.origin_y == 4 && glyph.advance_x == 8 && !glyph.advance_y);
        for (unsigned row = 0; row < 16; row++) {
            CHECK(glyph.bits[row * 4] == gui_font8x16[character - 32][15 - row]);
            CHECK(!glyph.bits[row * 4 + 1] && !glyph.bits[row * 4 + 2] &&
                  !glyph.bits[row * 4 + 3]);
        }
    }
    const WCHAR absent[] = {0, 31, 127, 0x100, 0xFFFF};
    for (unsigned i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        CHECK(gdi32_rasterize_glyph(dc, absent[i], &glyph));
        CHECK(!glyph.width && !glyph.height && !glyph.origin_x &&
              !glyph.origin_y && !glyph.advance_x && !glyph.advance_y);
        for (unsigned b = 0; b < sizeof(glyph.bits); b++) CHECK(!glyph.bits[b]);
    }
    CHECK(!memcmp(&before, source, sizeof(before)));
    for (unsigned i = 0; i < 64; i++) CHECK(untouched[i] == 0xAB123456u);
    CHECK(!ogl_wglUseFontBitmapsW(dc, 32, 95, 100) && last_error == 6);
    CHECK(!ogl_wglSwapLayerBuffers(dc, 0) && last_error == 6);
    CHECK(!ogl_wglSwapLayerBuffers(dc, 2) && last_error == 6);

    /* Bind an actual EGL context to the production owner/thread lookup. */
    contexts[0] = (OGL_CONTEXT){.used = 1, .owner_tgid = 1, .dc = dc,
        .backend = context, .module = &module_instances[0]};
    module_instances[0].used = 1; module_instances[0].owner_tgid = 1;
    module_instances[0].handle = module;
    thread_bindings[0] = (OGL_THREAD_BINDING){.used = 1, .tgid = 1, .tid = 2,
        .context = (HGLRC)(ULONG_PTR)(OGL_CONTEXT_BASE + 1), .dc = dc};
    CHECK(ogl_wglSwapLayerBuffers(dc, 0));
    CHECK(ogl_wglSwapLayerBuffers(dc, 2));
    CHECK(ogl_wglSwapLayerBuffers(dc, 0x10000u));
    CHECK(!ogl_wglUseFontBitmapsW(dc, 0xFFFFu, 2, 100) && last_error == 87);
    CHECK(!ogl_wglUseFontBitmapsW(dc, 0, 2, UINT32_MAX) && last_error == 87);
    CHECK(!ogl_wglUseFontBitmapsW(dc, 'A', 1, 0) && last_error == 87);
    CHECK(ogl_wglUseFontBitmapsW(dc, 0, 0, 0));
    missing_symbol = "glBitmap";
    CHECK(!ogl_wglUseFontBitmapsW(dc, 'A', 1, 100) && last_error == 50);
    CHECK(!glIsList(100));
    missing_symbol = NULL;
    glNewList(500, GL_COMPILE);
    CHECK(!ogl_wglUseFontBitmapsW(dc, 'A', 1, 100) && last_error == 170);
    glEndList();
    CHECK(glIsList(500) && !glIsList(100) && glGetError() == GL_NO_ERROR);
    GLint maximum, full_depth;
    glGetIntegerv(GL_MAX_CLIENT_ATTRIB_STACK_DEPTH, &maximum);
    for (GLint i = 0; i < maximum; i++) glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    CHECK(!ogl_wglUseFontBitmapsW(dc, 'A', 1, 100) && last_error == 8);
    glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &full_depth);
    CHECK(full_depth == maximum && !glIsList(100));
    for (GLint i = 0; i < maximum; i++) glPopClientAttrib();
    CHECK(glGetError() == GL_NO_ERROR);

    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 256, NULL, GL_STATIC_DRAW);
    const GLenum names[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
        GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_IMAGE_HEIGHT,
        GL_UNPACK_SKIP_IMAGES, GL_UNPACK_LSB_FIRST, GL_UNPACK_SWAP_BYTES};
    const GLint values[] = {8, 27, 3, 5, 31, 2, GL_TRUE, GL_TRUE};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        glPixelStorei(names[i], values[i]);
    CHECK(ogl_wglUseFontBitmapsW(dc, 32, 95, 100));
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        GLint value; glGetIntegerv(names[i], &value); CHECK(value == values[i]);
    }
    GLint binding, depth;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &binding);
    glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &depth);
    CHECK(binding == (GLint)buffer && !depth);
    for (unsigned i = 0; i < 95; i++) CHECK(glIsList(100 + i));

    /* Draw every glyph into real Mesa and compare each output pixel, including
     * blank padding, baseline placement and the raster advance for spaces. */
    glViewport(0, 0, 64, 64);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 64, 0, 64, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DITHER); glColor3f(1, 1, 1); glClearColor(0, 0, 0, 1);
    BYTE pixels[64 * 64 * 4];
    for (unsigned character = 32; character < 127; character++) {
        glClear(GL_COLOR_BUFFER_BIT);
        glRasterPos2i(16, 20); glCallList(100 + character - 32);
        GLfloat raster[4]; glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
        CHECK(raster[0] == 24 && raster[1] == 20);
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
            BYTE expected = x >= 16 && x < 24 && y >= 16 && y < 32 &&
                (gui_font8x16[character - 32][31 - y] & (0x80u >> (x - 16)))
                ? 255 : 0;
            unsigned p = (y * 64 + x) * 4;
            CHECK(pixels[p] == expected && pixels[p + 1] == expected &&
                  pixels[p + 2] == expected);
        }
    }
    for (unsigned i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        CHECK(ogl_wglUseFontBitmapsW(dc, absent[i], 1, 600 + i));
        CHECK(glIsList(600 + i));
        glClear(GL_COLOR_BUFFER_BIT);
        glRasterPos2i(16, 20); glCallList(600 + i);
        GLfloat raster[4]; glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
        CHECK(raster[0] == 16 && raster[1] == 20);
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (unsigned p = 0; p < 64 * 64; p++)
            CHECK(!pixels[p * 4] && !pixels[p * 4 + 1] && !pixels[p * 4 + 2]);
        glDeleteLists(600 + i, 1);
    }
    CHECK(!memcmp(&before, source, sizeof(before)));
    CHECK(glGetError() == GL_NO_ERROR);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); glDeleteBuffers(1, &buffer);
    glDeleteLists(100, 95); glDeleteLists(500, 1);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglDestroySurface(display, surface);
    eglTerminate(display); dlclose(module);
    printf("WGL-FONT HOST %s checks=%u failures=%u\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
