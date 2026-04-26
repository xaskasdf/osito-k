/* okgl_software.c — software-backed GL bypass for OsitoK.
 *
 * When Mesa+Zink can't bring up a real GL context (e.g. --no-gl boot,
 * no virglrenderer host), this file provides a tiny GL implementation
 * that paints directly into a SHM compositor surface. The supported
 * subset is enough for hello-gl-clear (and hello-gl-triangle with a
 * future CPU rasterizer).
 *
 * Symbols defined here OVERRIDE Mesa's via --allow-multiple-definition
 * + link order (libmesa_zink_loader.a / osito_compat objects come
 * BEFORE libGL.a in the link command).
 *
 * Coverage:
 *   okGLCreateContext   - allocates SHM surface + window via SYS_SHM_*
 *   okGLDestroyContext  - releases SHM
 *   okGLMakeCurrent     - sets thread-local current context
 *   okGLSwapBuffers     - SYS_GUI_FLIP
 *   glClearColor        - records RGBA in current ctx
 *   glClear             - fills SHM with current color
 *   glViewport          - records (no-op for clear)
 *   glFinish/glFlush    - no-ops
 *   glDrawArrays etc    - no-ops (defer to CPU rasterizer wave)
 */

#include <stdint.h>
#include <stddef.h>
#include "GL/okgl.h"

extern void *malloc(size_t);
extern void  free(void *);
extern int   printf(const char *, ...);
extern void *memset(void *, int, size_t);

/* ---- Syscalls (OsitoK libc exposes __syscallN, not syscall()) ---- */
extern long __syscall1(long, long);
extern long __syscall2(long, long, long);
extern long __syscall3(long, long, long, long);
#define SYS_NANOSLEEP      35L
#define SYS_SHM_MAP        501L
#define SYS_SHM_DESTROY    503L
#define SYS_SHM_MKSURFACE  506L
#define SYS_GUI_FLIP       507L
#define SHM_FMT_BGRA       0x41524742L

/* OsitoK libc lacks usleep — provide via nanosleep syscall (35).
 * Uses Linux ABI: timespec { sec, nsec }. */
struct ok_timespec { long tv_sec; long tv_nsec; };
int usleep(unsigned int us) {
    struct ok_timespec ts;
    ts.tv_sec  = (long)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    __syscall2(SYS_NANOSLEEP, (long)&ts, 0L);
    return 0;
}

/* ---- Context ---- */
struct OK_GLContext {
    uint32_t  shm_handle;
    uint32_t  width;
    uint32_t  height;
    uint32_t *fb;          /* mapped SHM, BGRA per pixel */
    /* GL state */
    float     clear_r, clear_g, clear_b, clear_a;
    int       vp_x, vp_y, vp_w, vp_h;
};

/* Single current context (single-threaded OsitoK userspace today). */
static OK_GLContext *g_current = (OK_GLContext *)0;

/* ---- okGL* lifecycle ---- */
OK_GLContext *
okGLCreateContext(uint32_t window_id, int width, int height) {
    (void)window_id;  /* SHM_MKSURFACE creates its own window id */
    if (width <= 0 || height <= 0) return (OK_GLContext *)0;

    OK_GLContext *c = (OK_GLContext *)malloc(sizeof(*c));
    if (!c) return (OK_GLContext *)0;
    memset(c, 0, sizeof(*c));
    c->width    = (uint32_t)width;
    c->height   = (uint32_t)height;
    c->vp_w     = width;
    c->vp_h     = height;
    c->clear_a  = 1.0f;

    long shm = __syscall3(SYS_SHM_MKSURFACE, (long)width, (long)height, SHM_FMT_BGRA);
    if (shm <= 0) {
        printf("okGL: SYS_SHM_MKSURFACE failed (%ld)\n", shm);
        free(c);
        return (OK_GLContext *)0;
    }
    c->shm_handle = (uint32_t)shm;

    long mapped = __syscall1(SYS_SHM_MAP, shm);
    if (mapped == 0) {
        printf("okGL: SYS_SHM_MAP failed for handle %u\n", c->shm_handle);
        __syscall1(SYS_SHM_DESTROY, shm);
        free(c);
        return (OK_GLContext *)0;
    }
    c->fb = (uint32_t *)(uintptr_t)mapped;

    return c;
}

int
okGLMakeCurrent(OK_GLContext *ctx) {
    g_current = ctx;
    return 0;  /* 0 = success, like POSIX */
}

int
okGLSwapBuffers(OK_GLContext *ctx) {
    if (!ctx) return -1;
    __syscall1(SYS_GUI_FLIP, (long)ctx->shm_handle);
    return 0;
}

void
okGLDestroyContext(OK_GLContext *ctx) {
    if (!ctx) return;
    if (g_current == ctx) g_current = (OK_GLContext *)0;
    if (ctx->shm_handle) syscall(SYS_SHM_DESTROY, (long)ctx->shm_handle);
    free(ctx);
}

/* ---- GL state setters ---- */
void glClearColor(float r, float g, float b, float a) {
    if (!g_current) return;
    g_current->clear_r = r;
    g_current->clear_g = g;
    g_current->clear_b = b;
    g_current->clear_a = a;
}

void glViewport(int x, int y, int w, int h) {
    if (!g_current) return;
    g_current->vp_x = x;
    g_current->vp_y = y;
    g_current->vp_w = w;
    g_current->vp_h = h;
}

/* ---- glClear: fill framebuffer with current color ---- */
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100

static inline uint8_t f2b(float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return (uint8_t)(f * 255.0f + 0.5f);
}

void glClear(unsigned int mask) {
    if (!g_current) return;
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    if (!g_current->fb) return;

    /* BGRA pixel: byte 0 = B, byte 1 = G, byte 2 = R, byte 3 = A.
     * As u32 little-endian: B | (G<<8) | (R<<16) | (A<<24). */
    uint32_t color = ((uint32_t)f2b(g_current->clear_b))
                   | ((uint32_t)f2b(g_current->clear_g) << 8)
                   | ((uint32_t)f2b(g_current->clear_r) << 16)
                   | ((uint32_t)f2b(g_current->clear_a) << 24);
    uint32_t *p = g_current->fb;
    uint32_t n = g_current->width * g_current->height;
    for (uint32_t i = 0; i < n; i++) p[i] = color;
}

/* ---- No-ops for the rest of the GL surface hello-gl-clear pulls ---- */
void glFinish(void) { }
void glFlush(void)  { }
void glClearDepth(double d)  { (void)d; }
void glClearStencil(int s)   { (void)s; }
void glEnable(unsigned int c)  { (void)c; }
void glDisable(unsigned int c) { (void)c; }
void glGetError(void) { }
