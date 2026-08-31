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
extern void *memcpy(void *, const void *, size_t);

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

/* From libmesa_zink.a (osito_compat/zink_screen_ositok.c). */
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "frontend/api.h"
#include "mesa/main/menums.h"
#include "state_tracker/st_context.h"
#include "util/u_inlines.h"
#include "util/u_atomic.h"
extern struct pipe_screen *
okGLZinkCreateScreen(VkInstance instance, VkPhysicalDevice phys);

/* OsitoK compositor flip — same syscall used by the SHM/SDL paths. */
#define SYS_GUI_FLIP 507
extern long syscall(long n, ...);

struct OK_GLContext;

struct ok_frontend_drawable {
    struct pipe_frontend_drawable base;
    struct OK_GLContext *owner;
};

/* Internal context tuple. */
struct OK_GLContext {
    uint32_t            window_id;
    int                 width, height;
    VkInstance          instance;
    VkPhysicalDevice    phys;
    struct pipe_screen *screen;
    struct pipe_context *pipe;
    struct st_context  *st;
    struct pipe_frontend_screen frontend_screen;
    struct st_visual visual;
    struct ok_frontend_drawable drawable;
    struct pipe_resource *color_resource;
    struct pipe_resource *readback_resource;
};

static uint32_t next_drawable_id = 1;

static int
ok_frontend_get_param(struct pipe_frontend_screen *fscreen,
                      enum st_manager_param param)
{
    (void)fscreen;
    (void)param;
    return 0;
}

static bool
ok_drawable_validate(struct st_context *st,
                     struct pipe_frontend_drawable *drawable,
                     const enum st_attachment_type *statts,
                     unsigned count,
                     struct pipe_resource **out,
                     struct pipe_resource **resolve)
{
    struct ok_frontend_drawable *ok_drawable =
        (struct ok_frontend_drawable *)drawable;
    OK_GLContext *ctx = ok_drawable->owner;

    (void)st;
    if (resolve) *resolve = NULL;

    for (unsigned i = 0; i < count; i++) {
        switch (statts[i]) {
        case ST_ATTACHMENT_FRONT_LEFT:
        case ST_ATTACHMENT_BACK_LEFT:
            if (!ctx->color_resource) {
                struct pipe_resource templ;
                memset(&templ, 0, sizeof(templ));
                templ.target = PIPE_TEXTURE_2D;
                templ.format = ctx->visual.color_format;
                templ.width0 = (uint32_t)ctx->width;
                templ.height0 = (uint16_t)ctx->height;
                templ.depth0 = 1;
                templ.array_size = 1;
                templ.last_level = 0;
                templ.nr_samples = ctx->visual.samples;
                templ.nr_storage_samples = ctx->visual.samples;
                templ.usage = PIPE_USAGE_DEFAULT;
                templ.bind = PIPE_BIND_RENDER_TARGET |
                             PIPE_BIND_SAMPLER_VIEW;

                ctx->color_resource =
                    ctx->screen->resource_create(ctx->screen, &templ);
                if (!ctx->color_resource) {
                    printf("okGL: failed to create drawable resource\n");
                    return false;
                }
            }
            pipe_resource_reference(&out[i], ctx->color_resource);
            break;
        default:
            printf("okGL: unsupported drawable attachment %d\n",
                   (int)statts[i]);
            return false;
        }
    }

    return true;
}

static bool
ok_drawable_flush_front(struct st_context *st,
                        struct pipe_frontend_drawable *drawable,
                        enum st_attachment_type statt)
{
    (void)st;
    (void)drawable;
    (void)statt;
    return true;
}

static bool
ok_drawable_flush_swapbuffers(struct st_context *st,
                              struct pipe_frontend_drawable *drawable)
{
    (void)st;
    (void)drawable;
    return true;
}

OK_GLContext *
okGLCreateContext(uint32_t window_id, int width, int height)
{
    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
        return NULL;

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
    printf("okGL: initial Vulkan pdev count=%u handle=%p\n",
           pd_count, (void *)ctx->phys);

    /* Step 3 — pipe_screen via zink. */
    ctx->screen = okGLZinkCreateScreen(ctx->instance, ctx->phys);
    if (!ctx->screen) {
        printf("okGL: okGLZinkCreateScreen returned NULL\n");
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    ctx->frontend_screen.screen = ctx->screen;
    ctx->frontend_screen.get_param = ok_frontend_get_param;

    ctx->visual.buffer_mask = ST_ATTACHMENT_BACK_LEFT_MASK;
    ctx->visual.color_format = PIPE_FORMAT_B8G8R8A8_UNORM;
    ctx->visual.depth_stencil_format = PIPE_FORMAT_NONE;
    ctx->visual.accum_format = PIPE_FORMAT_NONE;
    ctx->visual.samples = 0;

    ctx->drawable.owner = ctx;
    ctx->drawable.base.stamp = 1;
    ctx->drawable.base.ID = next_drawable_id++;
    if (ctx->drawable.base.ID == 0)
        ctx->drawable.base.ID = next_drawable_id++;
    ctx->drawable.base.fscreen = &ctx->frontend_screen;
    ctx->drawable.base.visual = &ctx->visual;
    ctx->drawable.base.flush_front = ok_drawable_flush_front;
    ctx->drawable.base.validate = ok_drawable_validate;
    ctx->drawable.base.flush_swapbuffers = ok_drawable_flush_swapbuffers;

    struct st_context_attribs attribs;
    memset(&attribs, 0, sizeof(attribs));
    attribs.profile = API_OPENGL_COMPAT;
    attribs.major = 1;
    attribs.minor = 0;
    attribs.visual = ctx->visual;

    enum st_context_error error = ST_CONTEXT_SUCCESS;
    ctx->st = st_api_create_context(&ctx->frontend_screen, &attribs,
                                    &error, NULL);
    if (!ctx->st) {
        printf("okGL: st_api_create_context failed (%d)\n", (int)error);
        st_screen_destroy(&ctx->frontend_screen);
        if (ctx->screen->destroy) ctx->screen->destroy(ctx->screen);
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }
    ctx->pipe = ctx->st->pipe;

    return ctx;
}

int
okGLMakeCurrent(OK_GLContext *ctx)
{
    if (ctx && !ctx->st) return -1;

    bool ok = ctx
        ? st_api_make_current(ctx->st, &ctx->drawable.base,
                             &ctx->drawable.base)
        : st_api_make_current(NULL, NULL, NULL);
    return ok ? 0 : -1;
}

int
okGLResizeContext(OK_GLContext *ctx, uint32_t window_id,
                  int width, int height)
{
    if (!ctx || width <= 0 || height <= 0 ||
        width > UINT16_MAX || height > UINT16_MAX)
        return -1;

    ctx->window_id = window_id;
    if (ctx->width == width && ctx->height == height)
        return 0;

    if (ctx->st)
        st_context_flush(ctx->st, ST_FLUSH_END_OF_FRAME,
                         NULL, NULL, NULL);

    pipe_resource_reference(&ctx->color_resource, NULL);
    pipe_resource_reference(&ctx->readback_resource, NULL);
    ctx->width = width;
    ctx->height = height;
    p_atomic_inc(&ctx->drawable.base.stamp);
    return 0;
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

int
okGLReadback(OK_GLContext *ctx, void *pixels, int width, int height,
             int pitch)
{
    if (!ctx || !ctx->st || !ctx->pipe || !ctx->color_resource || !pixels ||
        width <= 0 || height <= 0 || pitch < width * 4)
        return -1;

    unsigned copy_width = (unsigned)(width < ctx->width ? width : ctx->width);
    unsigned copy_height =
        (unsigned)(height < ctx->height ? height : ctx->height);
    uint64_t readback_size64 =
        (uint64_t)copy_width * (uint64_t)copy_height * 4u;
    if (readback_size64 == 0 || readback_size64 > UINT32_MAX)
        return -1;

    unsigned readback_size = (unsigned)readback_size64;
    if (!ctx->readback_resource ||
        ctx->readback_resource->width0 < readback_size) {
        pipe_resource_reference(&ctx->readback_resource, NULL);
        ctx->readback_resource = pipe_buffer_create(
            ctx->screen, PIPE_BIND_LINEAR, PIPE_USAGE_STAGING, readback_size);
        if (!ctx->readback_resource)
            return -1;
    }

    /* Submit rendering and the image-to-buffer copy in order. Mapping the
     * staging buffer below performs the single required completion wait. */
    st_context_flush(ctx->st, ST_FLUSH_END_OF_FRAME,
                     NULL, NULL, NULL);

    struct pipe_box box;
    memset(&box, 0, sizeof(box));
    box.width = (int)copy_width;
    box.height = (int)copy_height;
    box.depth = 1;
    ctx->pipe->resource_copy_region(
        ctx->pipe, ctx->readback_resource, 0, 0, 0, 0,
        ctx->color_resource, 0, &box);

    struct pipe_transfer *transfer = NULL;
    const uint8_t *source = (const uint8_t *)pipe_buffer_map(
        ctx->pipe, ctx->readback_resource, PIPE_MAP_READ, &transfer);
    if (!source || !transfer)
        return -1;

    uint8_t *destination = (uint8_t *)pixels;
    size_t source_stride = (size_t)copy_width * 4u;
    for (unsigned y = 0; y < copy_height; y++) {
        /* The state tracker renders this winsys drawable with a top-left
         * origin, matching the Osito compositor surface layout. */
        const uint8_t *source_row = source + (size_t)y * source_stride;
        memcpy(destination + (size_t)y * (size_t)pitch, source_row,
               (size_t)copy_width * 4U);
    }

    pipe_buffer_unmap(ctx->pipe, transfer);
    return 0;
}

void
okGLDestroyContext(OK_GLContext *ctx)
{
    if (!ctx) return;

    printf("okGL: destroy begin\n");
    if (ctx->st && st_api_get_current() == ctx->st) {
        printf("okGL: destroy unbind begin\n");
        st_api_make_current(NULL, NULL, NULL);
        printf("okGL: destroy unbind complete\n");
    }
    printf("okGL: destroy drawable begin\n");
    st_api_destroy_drawable(&ctx->drawable.base);
    printf("okGL: destroy drawable complete\n");
    if (ctx->st) {
        printf("okGL: destroy state tracker begin\n");
        st_destroy_context(ctx->st);
        printf("okGL: destroy state tracker complete\n");
        ctx->st = NULL;
        ctx->pipe = NULL;
    }
    printf("okGL: destroy color resource begin\n");
    pipe_resource_reference(&ctx->color_resource, NULL);
    pipe_resource_reference(&ctx->readback_resource, NULL);
    printf("okGL: destroy color resource complete\n");
    printf("okGL: destroy frontend screen begin\n");
    st_screen_destroy(&ctx->frontend_screen);
    printf("okGL: destroy frontend screen complete\n");
    if (ctx->screen && ctx->screen->destroy) {
        printf("okGL: destroy pipe screen begin\n");
        ctx->screen->destroy(ctx->screen);
        printf("okGL: destroy pipe screen complete\n");
    }
    if (ctx->instance) {
        printf("okGL: destroy Vulkan instance begin\n");
        vkDestroyInstance(ctx->instance, NULL);
        printf("okGL: destroy Vulkan instance complete\n");
    }
    free(ctx);
    printf("okGL: destroy complete\n");
}
