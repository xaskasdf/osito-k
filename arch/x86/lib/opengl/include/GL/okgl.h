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
OK_GLContext *okGLCreateContext(uint32_t window_id, int width, int height);

/* Bind ctx to the calling thread.  Pass NULL to unbind.  After this
 * returns, GL calls on the calling thread route through ctx. */
int okGLMakeCurrent(OK_GLContext *ctx);

/* Present ctx's back-buffer to the bound window.  No-op for off-screen
 * contexts. */
int okGLSwapBuffers(OK_GLContext *ctx);

/* Destroy ctx.  After this returns ctx is invalid; never deref it. */
void okGLDestroyContext(OK_GLContext *ctx);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* _OK_GL_H_ */
