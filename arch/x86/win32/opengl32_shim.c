/*
 * Minimal opengl32.dll/WGL implementation for capability probes.
 *
 * This provides a coherent software query context for SDL/GLEW tools such as
 * Steam's gldriverquery. It deliberately does not advertise accelerated
 * rendering; the native Mesa/Zink stack is still a userspace library and is
 * not linked into the Win32 kernel shim.
 */

#include "opengl32_shim.h"
#include "gdi32_shim.h"
#include "win32_abi.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);
extern uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                        uint8_t num_args, uint8_t callconv);

typedef HANDLE HGLRC;

#define OGL_CONTEXT_BASE 0xD1200000UL
#define OGL_MAX_CONTEXTS 16

#define GL_VENDOR                              0x1F00U
#define GL_RENDERER                            0x1F01U
#define GL_VERSION                             0x1F02U
#define GL_EXTENSIONS                          0x1F03U
#define GL_SHADING_LANGUAGE_VERSION            0x8B8CU
#define GL_MAJOR_VERSION                       0x821BU
#define GL_MINOR_VERSION                       0x821CU
#define GL_NUM_EXTENSIONS                      0x821DU
#define GL_CONTEXT_PROFILE_MASK                0x9126U
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT   0x00000002U
#define GL_MAX_TEXTURE_SIZE                    0x0D33U
#define GL_MAX_VIEWPORT_DIMS                   0x0D3AU
#define GL_MAX_VERTEX_ATTRIBS                  0x8869U
#define GL_MAX_TEXTURE_IMAGE_UNITS             0x8872U
#define GL_MAX_DRAW_BUFFERS                    0x8824U
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS    0x8B4DU
#define GL_MAX_RENDERBUFFER_SIZE               0x84E8U
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_NVX 0x9048U
#define GL_TEXTURE_FREE_MEMORY_ATI             0x87FCU

#define WGL_NUMBER_PIXEL_FORMATS_ARB  0x2000
#define WGL_DRAW_TO_WINDOW_ARB        0x2001
#define WGL_ACCELERATION_ARB          0x2003
#define WGL_SUPPORT_OPENGL_ARB        0x2010
#define WGL_DOUBLE_BUFFER_ARB         0x2011
#define WGL_PIXEL_TYPE_ARB            0x2013
#define WGL_COLOR_BITS_ARB            0x2014
#define WGL_RED_BITS_ARB              0x2015
#define WGL_GREEN_BITS_ARB            0x2017
#define WGL_BLUE_BITS_ARB             0x2019
#define WGL_ALPHA_BITS_ARB            0x201B
#define WGL_DEPTH_BITS_ARB            0x2022
#define WGL_STENCIL_BITS_ARB          0x2023
#define WGL_NO_ACCELERATION_ARB       0x2025
#define WGL_TYPE_RGBA_ARB             0x202B
#define WGL_FRAMEBUFFER_SRGB_ARB      0x20A9

static BYTE context_used[OGL_MAX_CONTEXTS];
static HDC context_dc[OGL_MAX_CONTEXTS];
static HGLRC current_context;
static HDC current_dc;
static int swap_interval;
static unsigned unresolved_count;
static BYTE *string_page_phys;

static const char fallback_vendor[] = "OsitoK";
static const char fallback_renderer[] = "OsitoK WGL software query context";
static const char fallback_version[] = "2.1 OsitoK";
static const char fallback_sl_version[] = "1.20 OsitoK";
static const char fallback_extensions[] = "";
static const char fallback_wgl_extensions[] =
    "WGL_ARB_create_context WGL_ARB_create_context_profile "
    "WGL_EXT_swap_control";

static void copy_string(BYTE *dst, const char *src)
{
    while ((*dst++ = (BYTE)*src++) != 0) { }
}

static void ensure_strings(void)
{
    if (__atomic_load_n(&string_page_phys, __ATOMIC_ACQUIRE)) return;

    BYTE *phys = (BYTE *)mem_alloc_pages(1);
    if (!phys) return;
    BYTE *page = (BYTE *)PHYS_TO_VIRT(phys);
    for (int i = 0; i < 4096; i++) page[i] = 0;
    copy_string(page + 0, fallback_vendor);
    copy_string(page + 64, fallback_renderer);
    copy_string(page + 160, fallback_version);
    copy_string(page + 224, fallback_sl_version);
    copy_string(page + 288, fallback_extensions);
    copy_string(page + 352, fallback_wgl_extensions);

    BYTE *expected = NULL;
    if (!__atomic_compare_exchange_n(&string_page_phys, &expected, phys,
                                     FALSE, __ATOMIC_RELEASE,
                                     __ATOMIC_ACQUIRE))
        mem_free_pages(phys, 1);
}

static BYTE *process_string_page(void)
{
    ensure_strings();
    BYTE *phys = __atomic_load_n(&string_page_phys, __ATOMIC_ACQUIRE);
    if (!phys) return NULL;
    return g_compat32_mode ? phys : (BYTE *)PHYS_TO_VIRT(phys);
}

static const BYTE *query_string(unsigned name)
{
    BYTE *string_page = process_string_page();
    switch (name) {
        case GL_VENDOR:
            return string_page ? string_page + 0 : NULL;
        case GL_RENDERER:
            return string_page ? string_page + 64 : NULL;
        case GL_VERSION:
            return string_page ? string_page + 160 : NULL;
        case GL_SHADING_LANGUAGE_VERSION:
            return string_page ? string_page + 224 : NULL;
        case GL_EXTENSIONS:
            return string_page ? string_page + 288 : NULL;
        default:
            return NULL;
    }
}

static int context_index(HGLRC context)
{
    ULONG_PTR value = (ULONG_PTR)context;
    if (value <= OGL_CONTEXT_BASE ||
        value > OGL_CONTEXT_BASE + OGL_MAX_CONTEXTS)
        return -1;
    int index = (int)(value - OGL_CONTEXT_BASE - 1);
    return context_used[index] ? index : -1;
}

static HGLRC WINAPI ogl_wglCreateContext(HDC hdc)
{
    for (int i = 0; i < OGL_MAX_CONTEXTS; i++) {
        if (context_used[i]) continue;
        context_used[i] = 1;
        context_dc[i] = hdc;
        serial_puts("[OPENGL32] created software query context\n");
        return (HGLRC)(ULONG_PTR)(OGL_CONTEXT_BASE + i + 1);
    }
    return NULL;
}

static BOOL WINAPI ogl_wglDeleteContext(HGLRC context)
{
    int index = context_index(context);
    if (index < 0) return FALSE;
    if (current_context == context) {
        current_context = NULL;
        current_dc = NULL;
    }
    context_used[index] = 0;
    context_dc[index] = NULL;
    return TRUE;
}

static BOOL WINAPI ogl_wglMakeCurrent(HDC hdc, HGLRC context)
{
    if (!context) {
        current_context = NULL;
        current_dc = NULL;
        return TRUE;
    }
    int index = context_index(context);
    if (index < 0) return FALSE;
    context_dc[index] = hdc;
    current_context = context;
    current_dc = hdc;
    return TRUE;
}

static BOOL WINAPI ogl_wglShareLists(HGLRC source, HGLRC target)
{
    return context_index(source) >= 0 && context_index(target) >= 0;
}

static HGLRC WINAPI ogl_wglGetCurrentContext(void)
{
    return current_context;
}

static HDC WINAPI ogl_wglGetCurrentDC(void)
{
    return current_dc;
}

static HGLRC WINAPI ogl_wglCreateContextAttribsARB(HDC hdc, HGLRC share,
                                                    const int *attribs)
{
    (void)share;
    (void)attribs;
    return ogl_wglCreateContext(hdc);
}

static const char *WINAPI ogl_wglGetExtensionsStringARB(HDC hdc)
{
    (void)hdc;
    BYTE *string_page = process_string_page();
    return string_page ? (const char *)(string_page + 352) : NULL;
}

static const char *WINAPI ogl_wglGetExtensionsStringEXT(void)
{
    return ogl_wglGetExtensionsStringARB(current_dc);
}

static BOOL WINAPI ogl_wglChoosePixelFormatARB(HDC hdc,
                                                const int *int_attribs,
                                                const float *float_attribs,
                                                UINT max_formats,
                                                int *formats,
                                                UINT *format_count)
{
    (void)hdc;
    (void)int_attribs;
    (void)float_attribs;
    if (format_count) *format_count = max_formats ? 1U : 0U;
    if (formats && max_formats) formats[0] = 1;
    return max_formats ? TRUE : FALSE;
}

static int pixel_attrib_value(int attrib)
{
    switch (attrib) {
        case WGL_NUMBER_PIXEL_FORMATS_ARB: return 1;
        case WGL_DRAW_TO_WINDOW_ARB:       return TRUE;
        case WGL_ACCELERATION_ARB:         return WGL_NO_ACCELERATION_ARB;
        case WGL_SUPPORT_OPENGL_ARB:       return TRUE;
        case WGL_DOUBLE_BUFFER_ARB:        return TRUE;
        case WGL_PIXEL_TYPE_ARB:           return WGL_TYPE_RGBA_ARB;
        case WGL_COLOR_BITS_ARB:           return 32;
        case WGL_RED_BITS_ARB:             return 8;
        case WGL_GREEN_BITS_ARB:           return 8;
        case WGL_BLUE_BITS_ARB:            return 8;
        case WGL_ALPHA_BITS_ARB:           return 8;
        case WGL_DEPTH_BITS_ARB:           return 24;
        case WGL_STENCIL_BITS_ARB:         return 8;
        case WGL_FRAMEBUFFER_SRGB_ARB:      return FALSE;
        default:                            return 0;
    }
}

static BOOL WINAPI ogl_wglGetPixelFormatAttribivARB(HDC hdc, int format,
                                                     int layer, UINT count,
                                                     const int *attribs,
                                                     int *values)
{
    (void)hdc;
    (void)format;
    (void)layer;
    if (!attribs || !values) return FALSE;
    for (UINT i = 0; i < count; i++) values[i] = pixel_attrib_value(attribs[i]);
    return TRUE;
}

static BOOL WINAPI ogl_wglGetPixelFormatAttribfvARB(HDC hdc, int format,
                                                     int layer, UINT count,
                                                     const int *attribs,
                                                     float *values)
{
    (void)hdc;
    (void)format;
    (void)layer;
    if (!attribs || !values) return FALSE;
    for (UINT i = 0; i < count; i++)
        values[i] = (float)pixel_attrib_value(attribs[i]);
    return TRUE;
}

static BOOL WINAPI ogl_wglSwapIntervalEXT(int interval)
{
    swap_interval = interval;
    return TRUE;
}

static int WINAPI ogl_wglGetSwapIntervalEXT(void)
{
    return swap_interval;
}

static const BYTE *WINAPI ogl_glGetString(unsigned name)
{
    return query_string(name);
}

static const BYTE *WINAPI ogl_glGetStringi(unsigned name, UINT index)
{
    (void)name;
    (void)index;
    return NULL;
}

static void WINAPI ogl_glGetIntegerv(unsigned name, int *value)
{
    if (!value) return;
    switch (name) {
        case GL_MAJOR_VERSION:                    value[0] = 2; break;
        case GL_MINOR_VERSION:                    value[0] = 1; break;
        case GL_NUM_EXTENSIONS:                   value[0] = 0; break;
        case GL_CONTEXT_PROFILE_MASK:
            value[0] = GL_CONTEXT_COMPATIBILITY_PROFILE_BIT; break;
        case GL_MAX_TEXTURE_SIZE:                 value[0] = 8192; break;
        case GL_MAX_VERTEX_ATTRIBS:               value[0] = 16; break;
        case GL_MAX_TEXTURE_IMAGE_UNITS:          value[0] = 16; break;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: value[0] = 32; break;
        case GL_MAX_DRAW_BUFFERS:                 value[0] = 4; break;
        case GL_MAX_RENDERBUFFER_SIZE:            value[0] = 8192; break;
        case GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_NVX:
            value[0] = 8 * 1024 * 1024; break;
        case GL_MAX_VIEWPORT_DIMS:
            value[0] = 8192; value[1] = 8192; break;
        case GL_TEXTURE_FREE_MEMORY_ATI:
            value[0] = 8 * 1024 * 1024;
            value[1] = value[2] = value[3] = 0;
            break;
        default: value[0] = 0; break;
    }
}

static void WINAPI ogl_glGetInteger64v(unsigned name, int64_t *value)
{
    int values[4] = {0, 0, 0, 0};
    ogl_glGetIntegerv(name, values);
    if (value) *value = values[0];
}

static void WINAPI ogl_glGetBooleanv(unsigned name, BYTE *value)
{
    int values[4] = {0, 0, 0, 0};
    ogl_glGetIntegerv(name, values);
    if (value) *value = values[0] ? 1 : 0;
}

static void WINAPI ogl_glGetFloatv(unsigned name, float *value)
{
    int values[4] = {0, 0, 0, 0};
    ogl_glGetIntegerv(name, values);
    if (value) *value = (float)values[0];
}

static void WINAPI ogl_glGetDoublev(unsigned name, double *value)
{
    int values[4] = {0, 0, 0, 0};
    ogl_glGetIntegerv(name, values);
    if (value) *value = (double)values[0];
}

static UINT WINAPI ogl_glGetError(void)
{
    return 0;
}

static BYTE WINAPI ogl_glIsEnabled(unsigned capability)
{
    (void)capability;
    return 0;
}

static void WINAPI ogl_glEnable(unsigned capability)
{
    (void)capability;
}

static void WINAPI ogl_glDisable(unsigned capability)
{
    (void)capability;
}

static void WINAPI ogl_glViewport(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void WINAPI ogl_glScissor(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void WINAPI ogl_glClearColor(float red, float green, float blue,
                                     float alpha)
{
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
}

static void WINAPI ogl_glClearDepth(double depth)
{
    (void)depth;
}

static void WINAPI ogl_glClearStencil(int value)
{
    (void)value;
}

static void WINAPI ogl_glClear(unsigned mask)
{
    (void)mask;
}

static void WINAPI ogl_glDepthFunc(unsigned function)
{
    (void)function;
}

static void WINAPI ogl_glDepthMask(BYTE enabled)
{
    (void)enabled;
}

static void WINAPI ogl_glCullFace(unsigned mode)
{
    (void)mode;
}

static void WINAPI ogl_glFrontFace(unsigned mode)
{
    (void)mode;
}

static void WINAPI ogl_glColorMask(BYTE red, BYTE green, BYTE blue, BYTE alpha)
{
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
}

static void WINAPI ogl_glBlendFunc(unsigned source, unsigned destination)
{
    (void)source;
    (void)destination;
}

static void WINAPI ogl_glFinish(void) { }
static void WINAPI ogl_glFlush(void) { }

static PVOID opengl_lookup(const char *name);

static PVOID WINAPI ogl_wglGetProcAddress(PCSTR name)
{
    if (!name) return NULL;
    PVOID function = opengl_lookup(name);
    if (!function) {
        if (unresolved_count++ < 96) {
            serial_puts("[OPENGL32] unresolved: ");
            serial_puts(name);
            serial_puts("\n");
        }
        return NULL;
    }

    if (g_compat32_mode) {
        uint8_t argc = 0;
        uint8_t cc = CC_STDCALL;
        if (!win32_abi_lookup("opengl32.dll", name, &argc, &cc))
            return NULL;
        uint32_t thunk = compat32_make_thunk_ex(
            (uint64_t)(ULONG_PTR)function, name, argc, cc);
        return thunk ? (PVOID)(ULONG_PTR)thunk : NULL;
    }
    return function;
}

static const WIN32_EXPORT opengl32_exports[] = {
    WX_STD("wglGetProcAddress",             ogl_wglGetProcAddress, 1),
    WX_STD("wglCreateContext",              ogl_wglCreateContext, 1),
    WX_STD("wglDeleteContext",              ogl_wglDeleteContext, 1),
    WX_STD("wglMakeCurrent",                ogl_wglMakeCurrent, 2),
    WX_STD("wglShareLists",                 ogl_wglShareLists, 2),
    WX_STD("wglGetCurrentContext",          ogl_wglGetCurrentContext, 0),
    WX_STD("wglGetCurrentDC",               ogl_wglGetCurrentDC, 0),
    WX_STD("wglCreateContextAttribsARB",    ogl_wglCreateContextAttribsARB, 3),
    WX_STD("wglGetExtensionsStringARB",     ogl_wglGetExtensionsStringARB, 1),
    WX_STD("wglGetExtensionsStringEXT",     ogl_wglGetExtensionsStringEXT, 0),
    WX_STD("wglChoosePixelFormatARB",       ogl_wglChoosePixelFormatARB, 6),
    WX_STD("wglGetPixelFormatAttribivARB",  ogl_wglGetPixelFormatAttribivARB, 6),
    WX_STD("wglGetPixelFormatAttribfvARB",  ogl_wglGetPixelFormatAttribfvARB, 6),
    WX_STD("wglSwapIntervalEXT",            ogl_wglSwapIntervalEXT, 1),
    WX_STD("wglGetSwapIntervalEXT",         ogl_wglGetSwapIntervalEXT, 0),
    WX_STD("glGetString",                   ogl_glGetString, 1),
    WX_STD("glGetStringi",                  ogl_glGetStringi, 2),
    WX_STD("glGetIntegerv",                 ogl_glGetIntegerv, 2),
    WX_STD("glGetInteger64v",               ogl_glGetInteger64v, 2),
    WX_STD("glGetBooleanv",                 ogl_glGetBooleanv, 2),
    WX_STD("glGetFloatv",                   ogl_glGetFloatv, 2),
    WX_STD("glGetDoublev",                  ogl_glGetDoublev, 2),
    WX_STD("glGetError",                    ogl_glGetError, 0),
    WX_STD("glIsEnabled",                   ogl_glIsEnabled, 1),
    WX_STD("glEnable",                     ogl_glEnable, 1),
    WX_STD("glDisable",                    ogl_glDisable, 1),
    WX_STD("glViewport",                   ogl_glViewport, 4),
    WX_STD("glScissor",                    ogl_glScissor, 4),
    WX_STD("glClearColor",                 ogl_glClearColor, 4),
    WX_STD("glClearDepth",                 ogl_glClearDepth, 2),
    WX_STD("glClearStencil",               ogl_glClearStencil, 1),
    WX_STD("glClear",                      ogl_glClear, 1),
    WX_STD("glDepthFunc",                  ogl_glDepthFunc, 1),
    WX_STD("glDepthMask",                  ogl_glDepthMask, 1),
    WX_STD("glCullFace",                   ogl_glCullFace, 1),
    WX_STD("glFrontFace",                  ogl_glFrontFace, 1),
    WX_STD("glColorMask",                  ogl_glColorMask, 4),
    WX_STD("glBlendFunc",                  ogl_glBlendFunc, 2),
    WX_STD("glFinish",                     ogl_glFinish, 0),
    WX_STD("glFlush",                       ogl_glFlush, 0),
};

static int ogl_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static PVOID opengl_lookup(const char *name)
{
    for (unsigned i = 0;
         i < sizeof(opengl32_exports) / sizeof(opengl32_exports[0]); i++) {
        if (ogl_strcmp(name, opengl32_exports[i].name) == 0)
            return opengl32_exports[i].func;
    }
    return NULL;
}

void opengl32_shim_init(void)
{
    for (int i = 0; i < OGL_MAX_CONTEXTS; i++) {
        context_used[i] = 0;
        context_dc[i] = NULL;
    }
    current_context = NULL;
    current_dc = NULL;
    swap_interval = 0;
    unresolved_count = 0;
    ensure_strings();
}

PVOID opengl32_resolve(const char *func_name, USHORT ordinal,
                       BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;
    return opengl_lookup(func_name);
}

const WIN32_EXPORT *opengl32_abi_table(int *count)
{
    *count = (int)(sizeof(opengl32_exports) / sizeof(opengl32_exports[0]));
    return opengl32_exports;
}
