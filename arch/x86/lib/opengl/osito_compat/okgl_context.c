/* okgl_context.c — W4.6 implementation of the okGL* context API.
 *
 * Layered above:
 *   - libvulkan.a              vkCreateInstance / vkEnumeratePhysicalDevices
 *   - libmesa_zink.a           okGLZinkCreateScreen (W4.4) → pipe_screen
 *   - libmesa_main.a           st_create_context / st_make_current (W4.5)
 *
 * Build-only acceptance for W4.6: the call chain is wired but a full
 * runtime exercise is deferred — the W4.6 hello-gl tests link against
 * this file but the actual end-to-end paint via zink+venus depends on
 * the compositor surface extension landing in a future wave.
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
 * (W4.7a). */
#include "pipe/p_screen.h"
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

/* We'd normally call _mesa_make_current here to bind the dispatch
 * table; that requires pulling in mesa/main/context.h. For W4.6 build
 * acceptance we declare it locally. The actual runtime wiring is
 * deferred. */
extern int /*GLboolean*/
_mesa_make_current(void *ctx, void *drawBuffer, void *readBuffer);

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
     * Build-only for W4.6: skip when pipe is NULL to avoid a crash, but
     * keep the symbol reference so the linker pulls in libmesa_main.a. */
    if (ctx->pipe) {
        ctx->st = st_create_context(OK_API_OPENGL_COMPAT, ctx->pipe,
                                    NULL, NULL, NULL, 0, 0);
        if (!ctx->st) {
            printf("okGL: st_create_context failed\n");
            /* destroy via screen->destroy() — same offset story; deferred */
            vkDestroyInstance(ctx->instance, NULL);
            free(ctx);
            return NULL;
        }
    }

    return ctx;
}

int
okGLMakeCurrent(OK_GLContext *ctx)
{
    if (!ctx) {
        /* Unbind. */
        return _mesa_make_current(NULL, NULL, NULL);
    }
    if (!ctx->st) {
        /* W4.6 build-acceptance path — st_context wasn't created.
         * Not an error, but glClear / glDrawArrays will be no-ops. */
        return 0;
    }
    return _mesa_make_current(ctx->st, NULL, NULL);
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
    if (ctx->st) st_destroy_context(ctx->st);
    /* pipe_screen->destroy() and pipe_context->destroy() are vtable
     * dispatches; deferred to W4.7 when we include p_screen.h here. */
    if (ctx->instance) vkDestroyInstance(ctx->instance, NULL);
    free(ctx);
}
