/* hello_gl.c — minimal okGL smoke test (W4.7c).
 *
 * Creates an off-screen 512x512 GL context, clears to red, finishes,
 * destroys the context. Does not present (window_id = 0). */

#include <GL/okgl.h>
#include <GL/gl.h>

int main(void) {
    extern char **environ;
    static char *software_env[] = {
        "LIBGL_ALWAYS_SOFTWARE=1",
        0,
    };
    environ = software_env;

    OK_GLContext *ctx = okGLCreateContext(0, 512, 512);
    if (!ctx) return 1;
    okGLMakeCurrent(ctx);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, 512, 512);
    glFinish();
    okGLDestroyContext(ctx);
    return 0;
}
