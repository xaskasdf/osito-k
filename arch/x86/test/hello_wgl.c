/* Minimal Win32 WGL smoke test. Build with a MinGW cross compiler. */

#include <windows.h>
#include <GL/gl.h>

#ifndef SMOKE_FRAMES
#define SMOKE_FRAMES 300
#endif

static LRESULT CALLBACK smoke_window_proc(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

void WINAPI WinMainCRTStartup(void)
{
    HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSA window_class = {0};
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = smoke_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = "OsitoWGLSmoke";
    if (!RegisterClassA(&window_class)) ExitProcess(10);

    HWND window = CreateWindowExA(0, window_class.lpszClassName,
        "OsitoK WGL smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 640, 480, NULL, NULL, instance, NULL);
    if (!window) ExitProcess(11);

    HDC dc = GetDC(window);
    if (!dc) ExitProcess(12);

    PIXELFORMATDESCRIPTOR format = {0};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
                     PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    format.iLayerType = PFD_MAIN_PLANE;

    int format_index = ChoosePixelFormat(dc, &format);
    if (!format_index || !SetPixelFormat(dc, format_index, &format))
        ExitProcess(13);

    HGLRC context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context)) ExitProcess(14);

    const GLubyte *version = glGetString(GL_VERSION);
    OutputDebugStringA(version ? "WGL smoke GL_VERSION: " :
                                 "WGL smoke GL_VERSION unavailable\n");
    if (version) {
        OutputDebugStringA((const char *)version);
        OutputDebugStringA("\n");
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    BOOL running = TRUE;
    for (int frame = 0; frame < SMOKE_FRAMES && running; frame++) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) running = FALSE;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (frame == 0) OutputDebugStringA("WGL smoke: before glViewport\n");
        glViewport(0, 0, 640, 480);
        if (frame == 0) OutputDebugStringA("WGL smoke: after glViewport\n");
        glClearColor(0.08f, 0.25f, 0.72f, 1.0f);
        if (frame == 0) OutputDebugStringA("WGL smoke: after glClearColor\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (frame == 0) OutputDebugStringA("WGL smoke: after glClear\n");
        SwapBuffers(dc);
        if (frame == 0) OutputDebugStringA("WGL smoke: after SwapBuffers\n");
        Sleep(16);
    }

    OutputDebugStringA("WGL smoke: before unbind\n");
    wglMakeCurrent(NULL, NULL);
    OutputDebugStringA("WGL smoke: after unbind\n");
    wglDeleteContext(context);
    OutputDebugStringA("WGL smoke: after delete context\n");
    ReleaseDC(window, dc);
    OutputDebugStringA("WGL smoke: after release DC\n");
    DestroyWindow(window);
    OutputDebugStringA("WGL smoke: after destroy window\n");
    ExitProcess(0);
}
