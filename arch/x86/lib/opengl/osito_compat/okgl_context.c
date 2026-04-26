/* okgl_context.c — W4.7 implementation of the okGL* context API.
 *
 * Layered above:
 *   - libvulkan.a              vkCreateInstance / vkEnumeratePhysicalDevices
 *   - libmesa_zink.a           okGLZinkCreateScreen (W4.4) → pipe_screen
 *   - libmesa_main.a           st_create_context / _mesa_make_current (W4.5)
 *
 * W4.6 wired pipe_screen + (already in this file) pipe_context + st_context
 * creation, but okGLMakeCurrent was a no-op when ctx->st was NULL because
 * the caller path silently fell through. W4.7 (T2/T3) makes pipe_context +
 * st_context creation REQUIRED — if either fails the whole context-create
 * fails — and converts okGLMakeCurrent to actually drive _mesa_make_current
 * with the embedded gl_context (st_context's first member is `gl_context *
 * ctx`). Return semantics also flipped: 0 = success, non-zero = failure
 * (matches okGL.h convention + hello-gl-clear's check).
 *
 * Design notes:
 *   * The pipe_screen + pipe_context tuple is created per-call. A future
 *     wave can promote the screen into a process-global singleton; the
 *     public API doesn't change.
 *   * Off-screen rendering is the only currently-supported mode
 *     (window_id is plumbed through but kopper / VkSurface attach is
 *     deferred to the same future wave).
 *   * st_create_context lives in libmesa_main.a. We declare it locally
 *     so we don't pull in mesa/main/ headers (which depend on a giant
 *     -include forest); the symbol resolution happens at link time.
 */

#include <stdint.h>
#include <stddef.h>

/* Public API */
#include "GL/okgl.h"

/* libc */
extern void *malloc(size_t);
extern void  free(void *);
extern int   printf(const char *, ...);
extern void *memset(void *, int, size_t);

/* From libvulkan.a (loader). We forward-declare here to keep the
 * include footprint of this TU tiny. */
typedef struct VkInstance_T          *VkInstance;
typedef struct VkPhysicalDevice_T    *VkPhysicalDevice;
typedef enum   VkResult { VK_SUCCESS = 0 } VkResult;

typedef struct {
    int sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t    applicationVersion;
    const char *pEngineName;
    uint32_t    engineVersion;
    uint32_t    apiVersion;
} VkApplicationInfo;

typedef struct {
    int sType;
    const void *pNext;
    uint32_t    flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t    enabledLayerCount;
    const char * const *ppEnabledLayerNames;
    uint32_t    enabledExtensionCount;
    const char * const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

#define OK_VK_STRUCTURE_TYPE_APPLICATION_INFO       0
#define OK_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO   1
#define OK_VK_API_VERSION_1_4                       (((1U<<22)|(4U<<12)|0U))

extern VkResult vkCreateInstance(const VkInstanceCreateInfo *, const void *, VkInstance *);
extern void     vkDestroyInstance(VkInstance, const void *);
extern VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);

/* From libmesa_zink.a (osito_compat/zink_screen_ositok.c). We pull in
 * the full p_screen.h so we can call screen->context_create() directly
 * (W4.7a). p_context.h gives us pipe_context::destroy (W4.7-T2). */
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
extern struct pipe_screen *
okGLZinkCreateScreen(VkInstance instance, VkPhysicalDevice phys);

/* The pipe_screen vtable's first useful entry is destroy(); after that
 * comes get_name, get_vendor, etc. context_create lives at offset
 * 18 ABI-wise but we don't need to know that — we walk through the
 * struct via the public header in real callers. For W4.6 we keep
 * this opaque and only call documented entry points. */

/* From libmesa_main.a — declared locally, resolved at link time. */
typedef int gl_api;
#define OK_API_OPENGL_COMPAT 1

struct gl_config;
struct gl_context;
struct gl_framebuffer;
struct st_context;
struct st_config_options;

extern struct st_context *
st_create_context(gl_api api, struct pipe_context *pipe,
                  const struct gl_config *visual,
                  struct st_context *share,
                  const struct st_config_options *options,
                  int /* bool */ no_error,
                  int /* bool */ has_egl_image_validate);

extern void st_destroy_context(struct st_context *st);

/* _mesa_make_current real signature (mesa/main/context.h):
 *   GLboolean _mesa_make_current(struct gl_context *ctx,
 *                                struct gl_framebuffer *draw,
 *                                struct gl_framebuffer *read);
 * Returns 1 on success, 0 on failure. We pass NULL framebuffers — Mesa
 * accepts that (off-screen path uses the dummy framebuffer).
 *
 * st_context's layout begins with `struct gl_context *ctx;` (verified
 * in mesa/src/mesa/state_tracker/st_context.h:128). We mirror just that
 * head locally so we can pluck out the embedded gl_context without
 * pulling the heavy state_tracker headers into this TU. */
struct ok_st_context_head {
    struct gl_context *ctx;
};

extern int /*GLboolean*/
_mesa_make_current(struct gl_context *ctx,
                   struct gl_framebuffer *drawBuffer,
                   struct gl_framebuffer *readBuffer);

/* OsitoK compositor flip — same syscall used by the SHM/SDL paths. */
#define SYS_GUI_FLIP 507
extern long syscall(long n, ...);

/* Internal context tuple. */
struct OK_GLContext {
    uint32_t            window_id;
    int                 width, height;
    VkInstance          instance;
    VkPhysicalDevice    phys;
    struct pipe_screen *screen;
    struct pipe_context *pipe;
    struct st_context  *st;
};

OK_GLContext *
okGLCreateContext(uint32_t window_id, int width, int height)
{
    OK_GLContext *ctx = (OK_GLContext *)malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->window_id = window_id;
    ctx->width     = width;
    ctx->height    = height;

    /* Step 1 — VkInstance via venus loader. */
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType            = OK_VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "okGL";
    app.apiVersion       = OK_VK_API_VERSION_1_4;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType            = OK_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    if (vkCreateInstance(&ici, NULL, &ctx->instance) != VK_SUCCESS) {
        printf("okGL: vkCreateInstance failed\n");
        free(ctx);
        return NULL;
    }

    /* Step 2 — first VkPhysicalDevice. */
    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &pd_count, NULL);
    if (pd_count == 0) {
        printf("okGL: no Vulkan physical device (host lacks virglrenderer?)\n");
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }
    VkPhysicalDevice pds[4];
    pd_count = 4;
    vkEnumeratePhysicalDevices(ctx->instance, &pd_count, pds);
    ctx->phys = pds[0];

    /* Step 3 — pipe_screen via zink. */
    ctx->screen = okGLZinkCreateScreen(ctx->instance, ctx->phys);
    if (!ctx->screen) {
        printf("okGL: okGLZinkCreateScreen returned NULL\n");
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    /* Step 4 — pipe_context (W4.7a). With p_screen.h included above
     * we can call context_create through the vtable directly. */
    ctx->pipe = ctx->screen->context_create(ctx->screen, NULL, 0);
    if (!ctx->pipe) {
        printf("okGL: pipe_screen->context_create failed\n");
        ctx->screen->destroy(ctx->screen);
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    /* Step 5 — st_create_context wires Mesa's dispatch table to ctx->pipe.
     * W4.7: REQUIRED — fail the whole context-create if it doesn't return.
     * (Previously gated on `if (ctx->pipe)` which was always-true given the
     * preceding NULL-check on ctx->pipe; the gate was dead code for W4.6
     * build-acceptance and is now removed for clarity.) */
    ctx->st = st_create_context(OK_API_OPENGL_COMPAT, ctx->pipe,
                                NULL, NULL, NULL, 0, 0);
    if (!ctx->st) {
        printf("okGL: st_create_context failed\n");
        /* pipe_context + pipe_screen destroy via vtable — both libGL.a
         * symbols. Errors here are best-effort cleanup. */
        if (ctx->pipe->destroy) ctx->pipe->destroy(ctx->pipe);
        if (ctx->screen->destroy) ctx->screen->destroy(ctx->screen);
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    return ctx;
}

int
okGLMakeCurrent(OK_GLContext *ctx)
{
    /* okGL convention: 0 = success, non-zero = failure (matches the
     * hello-gl-clear check `if (okGLMakeCurrent(ctx) != 0) SKIP`).
     * _mesa_make_current returns GLboolean: 1 = success, 0 = failure. */
    struct gl_context *gctx = NULL;
    if (ctx) {
        if (!ctx->st) return -1;  /* unconfigured context */
        gctx = ((struct ok_st_context_head *)ctx->st)->ctx;
    }
    int ok = _mesa_make_current(gctx, NULL, NULL);
    return ok ? 0 : -1;
}

int
okGLSwapBuffers(OK_GLContext *ctx)
{
    if (!ctx) return -1;
    if (ctx->window_id == 0) return 0;  /* off-screen — nothing to flip */
    /* Future: vkQueuePresentKHR via Zink's WSI plumbing.  For W4.6 we
     * issue a compositor flip; the surface contents are whatever the
     * SHM-backed back-buffer holds (glClear will paint into it once the
     * full pipe + WSI path lands). */
    syscall(SYS_GUI_FLIP, (long)ctx->window_id);
    return 0;
}

void
okGLDestroyContext(OK_GLContext *ctx)
{
    if (!ctx) return;
    /* Unbind first so Mesa doesn't dereference a stale gl_context after
     * st_destroy_context tears it down. */
    _mesa_make_current(NULL, NULL, NULL);
    if (ctx->st) st_destroy_context(ctx->st);
    /* W4.7: pipe_context and pipe_screen are vtable dispatches now that
     * p_screen.h is in scope. Some Gallium drivers tear pipe_context down
     * inside st_destroy_context — if our pipe survived, kill it explicitly. */
    if (ctx->pipe && ctx->pipe->destroy) ctx->pipe->destroy(ctx->pipe);
    if (ctx->screen && ctx->screen->destroy) ctx->screen->destroy(ctx->screen);
    if (ctx->instance) vkDestroyInstance(ctx->instance, NULL);
    free(ctx);
}
