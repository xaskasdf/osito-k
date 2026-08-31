/* okgl.h — OsitoK OpenGL context creation API.
 *
 * The native (non-X11/EGL/GLX) entry point applications use to obtain
 * a GL context backed by Mesa Zink + Vulkan + the OsitoK compositor.
 *
 * Usage pattern:
 *
 *     OK_GLContext *ctx = okGLCreateContext(window_id, 1024, 768);
 *     if (!ctx) abort();
 *     okGLMakeCurrent(ctx);
 *     glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
 *     glClear(GL_COLOR_BUFFER_BIT);
 *     okGLSwapBuffers(ctx);
 *     okGLDestroyContext(ctx);
 *
 * Window ids come from the OsitoK compositor (SYS_GUI_CREATE_WINDOW
 * family); window id 0 means "no compositor surface — render off-screen".
 *
 * This header is the only one applications include for context
 * management.  GL function prototypes come from the standard <GL/gl.h>
 * which Mesa ships.
 */
#ifndef _OK_GL_H_
#define _OK_GL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define OKGL_PUBLIC __attribute__((visibility("default")))
#else
#define OKGL_PUBLIC
#endif

/* Opaque context handle.  Internally points to a heap-allocated
 * OK_GLContext_Internal that owns the VkInstance / VkDevice / VkSurface
 * / pipe_screen / pipe_context / st_context tuple. */
typedef struct OK_GLContext OK_GLContext;

/* Create a new GL context bound to the given OsitoK compositor window.
 *
 * - window_id : 0 for off-screen, otherwise a compositor surface id
 * - width     : back-buffer width  in pixels
 * - height    : back-buffer height in pixels
 *
 * Returns the new context on success, NULL on failure (no Vulkan ICD,
 * no zink screen, OOM, ...).
 *
 * The first call performs lazy one-shot initialisation of the Vulkan
 * loader + zink screen.  Subsequent calls reuse the cached singletons. */
OKGL_PUBLIC OK_GLContext *okGLCreateContext(uint32_t window_id, int width,
                                            int height);

/* Bind ctx to the calling thread.  Pass NULL to unbind.  After this
 * returns, GL calls on the calling thread route through ctx. */
OKGL_PUBLIC int okGLMakeCurrent(OK_GLContext *ctx);

/* Rebind an existing context to a resized/recreated compositor window.
 * This invalidates the state-tracker drawable so its color attachment is
 * recreated at the new geometry before the next draw. */
OKGL_PUBLIC int okGLResizeContext(OK_GLContext *ctx, uint32_t window_id,
                                 int width, int height);

/* Present ctx's back-buffer to the bound window.  No-op for off-screen
 * contexts. */
OKGL_PUBLIC int okGLSwapBuffers(OK_GLContext *ctx);

/* Copy the current BGRA8 back-buffer into a compositor-owned surface.
 * `pitch` is the destination row stride in bytes. */
OKGL_PUBLIC int okGLReadback(OK_GLContext *ctx, void *pixels, int width,
                             int height, int pitch);

/* Destroy ctx.  After this returns ctx is invalid; never deref it. */
OKGL_PUBLIC void okGLDestroyContext(OK_GLContext *ctx);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* _OK_GL_H_ */
