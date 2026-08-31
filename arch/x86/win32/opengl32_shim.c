/* Win32 WGL/OpenGL bridge backed by the dynamically loaded Mesa/Zink module. */

#include "opengl32_shim.h"
#include "gdi32_shim.h"
#include "kernel32_shim.h"
#include "user32_shim.h"
#include "win32_abi.h"
#include "../include/dynlink.h"
#include "../include/paging.h"
#include "../include/sys/gpu_syscalls.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

/* Mesa exports a small GLES compatibility tail not declared by desktop GL. */
typedef int32_t GLclampx;

#if defined(__GNUC__)
#define OGL_SYSV_ABI __attribute__((sysv_abi))
#else
#define OGL_SYSV_ABI
#endif

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t value, int width);
extern void serial_putdec(uint64_t value);
extern void *mem_alloc_pages(uint64_t count);
extern void mem_free_pages(void *addr, uint64_t count);
extern int32_t proc_current_pid(void);
extern int32_t proc_current_tgid(void);
extern uint32_t compat32_make_thunk_ex(uint64_t target, const char *name,
                                        uint8_t num_args, uint8_t callconv);
extern uint32_t vg3d_caps(void) __attribute__((weak));

typedef HANDLE HGLRC;

#define OGL_CONTEXT_BASE 0xD1200000UL
#define OGL_MAX_CONTEXTS 64
#define OGL_MAX_THREAD_BINDINGS 1024
#define OGL_MAX_MODULE_INSTANCES 32
#define OGL_MAX_NATIVE_SYMBOLS 2048
#define OGL_STRING_PAGES 64
#define OGL_COMPAT_LARGE_STRING_OFFSET 4096U
#define OGL_COMPAT_LARGE_STRING_SIZE   (128U * 1024U)
#define OGL_COMPAT_SMALL_STRING_OFFSET \
    (OGL_COMPAT_LARGE_STRING_OFFSET + OGL_COMPAT_LARGE_STRING_SIZE)
#define OGL_COMPAT_SMALL_STRING_SIZE   1024U
#define OGL_COMPAT_SMALL_STRING_COUNT  \
    (((OGL_STRING_PAGES * 4096U) - OGL_COMPAT_SMALL_STRING_OFFSET) / \
     OGL_COMPAT_SMALL_STRING_SIZE)

#ifndef GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_NVX
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_NVX 0x9048U
#endif

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
#define WGL_FULL_ACCELERATION_ARB     0x2027
#define WGL_TYPE_RGBA_ARB             0x202B
#define WGL_FRAMEBUFFER_SRGB_ARB      0x20A9

typedef void *(OGL_SYSV_ABI *okgl_create_fn)(uint32_t, int, int);
typedef int (OGL_SYSV_ABI *okgl_make_current_fn)(void *);
typedef int (OGL_SYSV_ABI *okgl_resize_context_fn)(void *, uint32_t, int, int);
typedef int (OGL_SYSV_ABI *okgl_swap_buffers_fn)(void *);
typedef int (OGL_SYSV_ABI *okgl_readback_fn)(void *, void *, int, int, int);
typedef void (OGL_SYSV_ABI *okgl_destroy_fn)(void *);

typedef struct ogl_module_instance {
    volatile uint32_t used;
    uint32_t owner_tgid;
    void *handle;
    okgl_create_fn create_context;
    okgl_make_current_fn make_current;
    okgl_resize_context_fn resize_context;
    okgl_swap_buffers_fn swap_buffers;
    okgl_readback_fn readback;
    okgl_destroy_fn destroy_context;
    void *native_symbols[OGL_MAX_NATIVE_SYMBOLS];
} OGL_MODULE_INSTANCE;

typedef struct {
    volatile uint32_t used;
    uint32_t owner_tgid;
    BYTE *page;
    volatile uint32_t small_string_slot;
    volatile int large_string_lock;
} OGL_STRING_INSTANCE;

typedef struct {
    volatile uint32_t used;
    uint32_t owner_tgid;
    HDC dc;
    HWND window;
    void *backend;
    OGL_MODULE_INSTANCE *module;
    int width;
    int height;
} OGL_CONTEXT;

typedef struct {
    volatile uint32_t used;
    uint32_t tgid;
    uint32_t tid;
    HGLRC context;
    HDC dc;
    int swap_interval;
} OGL_THREAD_BINDING;

typedef struct {
    WIN32_EXPORT abi;
    PVOID compat32;
    BOOL core_11;
} OGL_GENERATED_EXPORT;

static OGL_CONTEXT contexts[OGL_MAX_CONTEXTS];
static OGL_THREAD_BINDING thread_bindings[OGL_MAX_THREAD_BINDINGS];
static OGL_MODULE_INSTANCE module_instances[OGL_MAX_MODULE_INSTANCES];
static OGL_STRING_INSTANCE string_instances[OGL_MAX_MODULE_INSTANCES];
static uint64_t swap_trace_count;
static unsigned unresolved_count;
static BYTE *string_page_phys;

static volatile int module_lock;
static volatile int string_instance_lock;

static uint32_t opengl_current_tgid(void);
static uint32_t opengl_current_tid(void);

static void module_spin_lock(void)
{
    while (__atomic_exchange_n(&module_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void module_spin_unlock(void)
{
    __atomic_store_n(&module_lock, 0, __ATOMIC_RELEASE);
}

static void string_instance_spin_lock(void)
{
    while (__atomic_exchange_n(&string_instance_lock, 1,
                               __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}

static void string_instance_spin_unlock(void)
{
    __atomic_store_n(&string_instance_lock, 0, __ATOMIC_RELEASE);
}

static OGL_MODULE_INSTANCE *opengl_find_module(uint32_t tgid)
{
    for (int i = 0; i < OGL_MAX_MODULE_INSTANCES; i++) {
        OGL_MODULE_INSTANCE *module = &module_instances[i];
        if (__atomic_load_n(&module->used, __ATOMIC_ACQUIRE) == 1 &&
            module->owner_tgid == tgid)
            return module;
    }
    return NULL;
}

static OGL_MODULE_INSTANCE *opengl_load_module_for(uint32_t tgid)
{
    if (!tgid) return NULL;

    OGL_MODULE_INSTANCE *module = opengl_find_module(tgid);
    if (module) return module;

    module_spin_lock();
    module = opengl_find_module(tgid);
    if (!module) {
        for (int i = 0; i < OGL_MAX_MODULE_INSTANCES; i++) {
            if (__atomic_load_n(&module_instances[i].used,
                                __ATOMIC_ACQUIRE) != 0)
                continue;
            module = &module_instances[i];
            memset(module, 0, sizeof(*module));
            module->owner_tgid = tgid;
            __atomic_store_n(&module->used, 2, __ATOMIC_RELEASE);
            break;
        }
    }

    if (module && __atomic_load_n(&module->used, __ATOMIC_ACQUIRE) == 2) {
        module->handle = dl_open_private("libGL-osito.so", tgid);
        if (module->handle) {
            module->create_context = (okgl_create_fn)
                dl_sym(module->handle, "okGLCreateContext");
            module->make_current = (okgl_make_current_fn)
                dl_sym(module->handle, "okGLMakeCurrent");
            module->resize_context = (okgl_resize_context_fn)
                dl_sym(module->handle, "okGLResizeContext");
            module->swap_buffers = (okgl_swap_buffers_fn)
                dl_sym(module->handle, "okGLSwapBuffers");
            module->readback = (okgl_readback_fn)
                dl_sym(module->handle, "okGLReadback");
            module->destroy_context = (okgl_destroy_fn)
                dl_sym(module->handle, "okGLDestroyContext");
        }

        if (module->handle && module->create_context &&
            module->make_current && module->resize_context &&
            module->swap_buffers && module->readback &&
            module->destroy_context) {
            __atomic_store_n(&module->used, 1, __ATOMIC_RELEASE);
            serial_puts("[OPENGL32] Mesa/Zink module loaded owner=");
            serial_putdec(tgid);
            serial_puts("\n");
        } else {
            if (module->handle) dl_discard(module->handle);
            memset(module, 0, sizeof(*module));
            __atomic_store_n(&module->used, 0, __ATOMIC_RELEASE);
            module = NULL;
            serial_puts("[OPENGL32] Mesa/Zink module unavailable owner=");
            serial_putdec(tgid);
            serial_puts("\n");
        }
    }
    module_spin_unlock();
    return module;
}

static int opengl_load_module(void)
{
    return opengl_load_module_for(opengl_current_tgid()) != NULL;
}

static void *opengl_native_symbol(unsigned index, const char *name)
{
    if (index >= OGL_MAX_NATIVE_SYMBOLS) return NULL;
    OGL_MODULE_INSTANCE *module =
        opengl_load_module_for(opengl_current_tgid());
    if (!module) return NULL;

    void *symbol = __atomic_load_n(&module->native_symbols[index],
                                   __ATOMIC_ACQUIRE);
    if (symbol) return symbol;

    symbol = dl_sym(module->handle, name);
    if (symbol) {
        void *expected = NULL;
        __atomic_compare_exchange_n(&module->native_symbols[index], &expected,
                                    symbol, FALSE, __ATOMIC_RELEASE,
                                    __ATOMIC_ACQUIRE);
        if (expected) symbol = expected;
    }
    return symbol;
}

BOOL opengl32_has_accelerated_backend(void)
{
    if (!vg3d_caps || !(vg3d_caps() & GPU_CAP_VENUS_READY)) return FALSE;
    return opengl_load_module() ? TRUE : FALSE;
}

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

static void initialize_string_page(BYTE *page)
{
    memset(page, 0, OGL_STRING_PAGES * 4096U);
    copy_string(page + 0, fallback_vendor);
    copy_string(page + 64, fallback_renderer);
    copy_string(page + 160, fallback_version);
    copy_string(page + 224, fallback_sl_version);
    copy_string(page + 288, fallback_extensions);
    copy_string(page + 352, fallback_wgl_extensions);
}

static void ensure_strings(void)
{
    if (__atomic_load_n(&string_page_phys, __ATOMIC_ACQUIRE)) return;

    BYTE *phys = (BYTE *)mem_alloc_pages(OGL_STRING_PAGES);
    if (!phys) return;
    BYTE *page = (BYTE *)PHYS_TO_VIRT(phys);
    initialize_string_page(page);

    BYTE *expected = NULL;
    if (!__atomic_compare_exchange_n(&string_page_phys, &expected, phys,
                                     FALSE, __ATOMIC_RELEASE,
                                     __ATOMIC_ACQUIRE))
        mem_free_pages(phys, OGL_STRING_PAGES);
}

static OGL_STRING_INSTANCE *find_string_instance(uint32_t tgid)
{
    for (int i = 0; i < OGL_MAX_MODULE_INSTANCES; i++) {
        OGL_STRING_INSTANCE *strings = &string_instances[i];
        if (__atomic_load_n(&strings->used, __ATOMIC_ACQUIRE) == 1 &&
            strings->owner_tgid == tgid)
            return strings;
    }
    return NULL;
}

static OGL_STRING_INSTANCE *compat_string_instance(void)
{
    uint32_t tgid = opengl_current_tgid();
    if (!tgid) return NULL;

    OGL_STRING_INSTANCE *strings = find_string_instance(tgid);
    if (strings) return strings;

    string_instance_spin_lock();
    strings = find_string_instance(tgid);
    if (!strings) {
        for (int i = 0; i < OGL_MAX_MODULE_INSTANCES; i++) {
            if (__atomic_load_n(&string_instances[i].used,
                                __ATOMIC_ACQUIRE) != 0)
                continue;
            strings = &string_instances[i];
            memset(strings, 0, sizeof(*strings));
            strings->owner_tgid = tgid;
            __atomic_store_n(&strings->used, 2, __ATOMIC_RELEASE);
            break;
        }

        if (strings &&
            __atomic_load_n(&strings->used, __ATOMIC_ACQUIRE) == 2) {
            BYTE *page = (BYTE *)VirtualAlloc(
                NULL, OGL_STRING_PAGES * 4096U,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (page && (ULONG_PTR)page <= 0xFFFFFFFFULL) {
                initialize_string_page(page);
                strings->page = page;
                __atomic_store_n(&strings->used, 1, __ATOMIC_RELEASE);
                serial_puts("[OPENGL32] mapped compat strings owner=");
                serial_putdec(tgid);
                serial_puts(" va=0x");
                serial_puthex((uint64_t)(ULONG_PTR)page, 8);
                serial_puts("\n");
            } else {
                if (page) VirtualFree(page, 0, MEM_RELEASE);
                memset(strings, 0, sizeof(*strings));
                __atomic_store_n(&strings->used, 0, __ATOMIC_RELEASE);
                strings = NULL;
                serial_puts("[OPENGL32] compat string allocation failed owner=");
                serial_putdec(tgid);
                serial_puts("\n");
            }
        }
    }
    string_instance_spin_unlock();
    return strings;
}

static BYTE *process_string_page(void)
{
    if (g_compat32_mode) {
        OGL_STRING_INSTANCE *strings = compat_string_instance();
        return strings ? strings->page : NULL;
    }

    ensure_strings();
    BYTE *phys = __atomic_load_n(&string_page_phys, __ATOMIC_ACQUIRE);
    if (!phys) return NULL;
    return (BYTE *)PHYS_TO_VIRT(phys);
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

static const char *opengl_compat_string(const char *source)
{
    if (!source) return NULL;
    if ((ULONG_PTR)source <= 0xFFFFFFFFULL) return source;

    OGL_STRING_INSTANCE *strings = compat_string_instance();
    if (!strings || !strings->page) return NULL;
    BYTE *page = strings->page;

    unsigned length = 0;
    while (length + 1U < OGL_COMPAT_LARGE_STRING_SIZE && source[length])
        length++;

    if (length + 1U > OGL_COMPAT_SMALL_STRING_SIZE) {
        while (__atomic_exchange_n(&strings->large_string_lock, 1,
                                   __ATOMIC_ACQUIRE))
            __asm__ volatile ("pause");
        BYTE *destination = page + OGL_COMPAT_LARGE_STRING_OFFSET;
        for (unsigned i = 0; i < length; i++)
            destination[i] = (BYTE)source[i];
        destination[length] = 0;
        __atomic_store_n(&strings->large_string_lock, 0, __ATOMIC_RELEASE);
        return (const char *)(page + OGL_COMPAT_LARGE_STRING_OFFSET);
    }

    uint32_t slot = __atomic_fetch_add(&strings->small_string_slot, 1,
                                       __ATOMIC_RELAXED);
    slot %= OGL_COMPAT_SMALL_STRING_COUNT;
    unsigned offset = OGL_COMPAT_SMALL_STRING_OFFSET +
                      slot * OGL_COMPAT_SMALL_STRING_SIZE;
    BYTE *destination = page + offset;
    for (unsigned i = 0; i < length; i++)
        destination[i] = (BYTE)source[i];
    destination[length] = 0;
    return (const char *)(page + offset);
}

static float opengl_float_from_slot(uint64_t slot)
{
    union { uint32_t bits; float value; } decoded;
    decoded.bits = (uint32_t)slot;
    return decoded.value;
}

static uint64_t opengl_u64_from_slots(uint64_t low, uint64_t high)
{
    return (uint64_t)(uint32_t)low | ((uint64_t)(uint32_t)high << 32);
}

static double opengl_double_from_slots(uint64_t low, uint64_t high)
{
    union { uint64_t bits; double value; } decoded;
    decoded.bits = opengl_u64_from_slots(low, high);
    return decoded.value;
}

#include "opengl32_generated.inc"

static uint32_t opengl_current_tgid(void)
{
    int32_t tgid = proc_current_tgid();
    return tgid > 0 ? (uint32_t)tgid : 0;
}

static uint32_t opengl_current_tid(void)
{
    int32_t tid = proc_current_pid();
    return tid > 0 ? (uint32_t)tid : 0;
}

static OGL_THREAD_BINDING *current_binding(BOOL create)
{
    uint32_t tgid = opengl_current_tgid();
    uint32_t tid = opengl_current_tid();
    if (!tgid || !tid) return NULL;

    for (int i = 0; i < OGL_MAX_THREAD_BINDINGS; i++) {
        OGL_THREAD_BINDING *binding = &thread_bindings[i];
        if (__atomic_load_n(&binding->used, __ATOMIC_ACQUIRE) == 1 &&
            binding->tgid == tgid && binding->tid == tid)
            return binding;
    }
    if (!create) return NULL;

    for (int i = 0; i < OGL_MAX_THREAD_BINDINGS; i++) {
        OGL_THREAD_BINDING *binding = &thread_bindings[i];
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&binding->used, &expected, 2, FALSE,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;
        binding->tgid = tgid;
        binding->tid = tid;
        binding->context = NULL;
        binding->dc = NULL;
        binding->swap_interval = 0;
        __atomic_store_n(&binding->used, 1, __ATOMIC_RELEASE);
        return binding;
    }
    return NULL;
}

static void release_binding(OGL_THREAD_BINDING *binding)
{
    if (!binding) return;
    binding->context = NULL;
    binding->dc = NULL;
    binding->swap_interval = 0;
    binding->tgid = 0;
    binding->tid = 0;
    __atomic_store_n(&binding->used, 0, __ATOMIC_RELEASE);
}

void opengl32_release_process(DWORD owner_tgid)
{
    if (!owner_tgid) return;

    unsigned dropped_contexts = 0;
    for (int i = 0; i < OGL_MAX_THREAD_BINDINGS; i++) {
        OGL_THREAD_BINDING *binding = &thread_bindings[i];
        if (__atomic_load_n(&binding->used, __ATOMIC_ACQUIRE) != 0 &&
            binding->tgid == owner_tgid)
            release_binding(binding);
    }

    /* Process teardown has already stopped its threads. Do not re-enter Mesa:
     * its worker queues and heap belong to the address space being discarded.
     * The kernel's VG3D owner cleanup releases host contexts and resources. */
    for (int i = 0; i < OGL_MAX_CONTEXTS; i++) {
        OGL_CONTEXT *context = &contexts[i];
        if (__atomic_load_n(&context->used, __ATOMIC_ACQUIRE) == 0 ||
            context->owner_tgid != owner_tgid)
            continue;
        __atomic_store_n(&context->used, 2, __ATOMIC_RELEASE);
        context->owner_tgid = 0;
        context->dc = NULL;
        context->window = NULL;
        context->backend = NULL;
        context->module = NULL;
        context->width = 0;
        context->height = 0;
        __atomic_store_n(&context->used, 0, __ATOMIC_RELEASE);
        dropped_contexts++;
    }

    module_spin_lock();
    OGL_MODULE_INSTANCE *module = opengl_find_module(owner_tgid);
    if (module) {
        __atomic_store_n(&module->used, 2, __ATOMIC_RELEASE);
        void *handle = module->handle;
        if (handle) dl_discard(handle);
        memset(module, 0, sizeof(*module));
        __atomic_store_n(&module->used, 0, __ATOMIC_RELEASE);
    }
    module_spin_unlock();

    BOOL released_strings = FALSE;
    string_instance_spin_lock();
    OGL_STRING_INSTANCE *strings = find_string_instance(owner_tgid);
    if (strings) {
        /* The process VMA teardown owns the VirtualAlloc mapping. This path
         * can also run from a reaper in another CR3, so only retire the
         * kernel-side lookup slot here. */
        __atomic_store_n(&strings->used, 2, __ATOMIC_RELEASE);
        memset(strings, 0, sizeof(*strings));
        __atomic_store_n(&strings->used, 0, __ATOMIC_RELEASE);
        released_strings = TRUE;
    }
    string_instance_spin_unlock();

    if (module || dropped_contexts || released_strings) {
        serial_puts("[OPENGL32] released owner=");
        serial_putdec(owner_tgid);
        serial_puts(" contexts=");
        serial_putdec(dropped_contexts);
        serial_puts("\n");
    }
}

static BOOL context_bound_to_other_thread(HGLRC context)
{
    uint32_t tgid = opengl_current_tgid();
    uint32_t tid = opengl_current_tid();
    for (int i = 0; i < OGL_MAX_THREAD_BINDINGS; i++) {
        OGL_THREAD_BINDING *binding = &thread_bindings[i];
        if (__atomic_load_n(&binding->used, __ATOMIC_ACQUIRE) == 1 &&
            binding->tgid == tgid && binding->tid != tid &&
            binding->context == context)
            return TRUE;
    }
    return FALSE;
}

static int context_index(HGLRC context)
{
    ULONG_PTR value = (ULONG_PTR)context;
    if (value <= OGL_CONTEXT_BASE ||
        value > OGL_CONTEXT_BASE + OGL_MAX_CONTEXTS)
        return -1;
    int index = (int)(value - OGL_CONTEXT_BASE - 1);
    return __atomic_load_n(&contexts[index].used, __ATOMIC_ACQUIRE) == 1 &&
           contexts[index].owner_tgid == opengl_current_tgid()
        ? index : -1;
}

static HGLRC WINAPI ogl_wglCreateContext(HDC hdc)
{
    uint32_t tgid = opengl_current_tgid();
    uint32_t tid = opengl_current_tid();
    if (!tgid || !tid) return NULL;

    serial_puts("[OPENGL32] wglCreateContext hdc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
    serial_puts(" tgid=");
    serial_putdec(tgid);
    serial_puts(" tid=");
    serial_putdec(tid);
    serial_puts("\n");

    for (int i = 0; i < OGL_MAX_CONTEXTS; i++) {
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&contexts[i].used, &expected, 2,
                                         FALSE, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;
        contexts[i].owner_tgid = tgid;

        HWND window = (HWND)gdi32_window_from_dc(hdc);
        serial_puts("[OPENGL32] context window=0x");
        serial_puthex((uint64_t)(ULONG_PTR)window, 8);
        serial_puts("\n");
        void *surface = NULL;
        int width = 800, height = 600, pitch = 0;
        if (window)
            user32_get_window_surface(window, &surface, &width, &height,
                                      &pitch);

        void *backend = NULL;
        OGL_MODULE_INSTANCE *module = NULL;
        if (vg3d_caps && (vg3d_caps() & GPU_CAP_VENUS_READY))
            module = opengl_load_module_for(tgid);
        if (module) {
            uint32_t window_id = user32_get_window_compositor_id(window);
            backend = module->create_context(window_id, width, height);
        }

        contexts[i].dc = hdc;
        contexts[i].window = window;
        contexts[i].backend = backend;
        contexts[i].module = module;
        contexts[i].width = width;
        contexts[i].height = height;
        __atomic_store_n(&contexts[i].used, 1, __ATOMIC_RELEASE);
        serial_puts(backend
            ? "[OPENGL32] created Mesa/Zink context\n"
            : "[OPENGL32] created software query context\n");
        return (HGLRC)(ULONG_PTR)(OGL_CONTEXT_BASE + i + 1);
    }
    return NULL;
}

static BOOL WINAPI ogl_wglDeleteContext(HGLRC context)
{
    int index = context_index(context);
    if (index < 0) return FALSE;
    if (context_bound_to_other_thread(context)) return FALSE;

    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    if (binding && binding->context == context) {
        OGL_MODULE_INSTANCE *module = contexts[index].module;
        if (contexts[index].backend && module && module->make_current)
            module->make_current(NULL);
        release_binding(binding);
    }

    uint32_t expected = 1;
    if (!__atomic_compare_exchange_n(&contexts[index].used, &expected, 2,
                                     FALSE, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return FALSE;
    OGL_MODULE_INSTANCE *module = contexts[index].module;
    if (contexts[index].backend && module && module->destroy_context) {
        serial_puts("[OPENGL32] destroy backend begin\n");
        module->destroy_context(contexts[index].backend);
        serial_puts("[OPENGL32] destroy backend complete\n");
    }
    contexts[index].owner_tgid = 0;
    contexts[index].dc = NULL;
    contexts[index].window = NULL;
    contexts[index].backend = NULL;
    contexts[index].module = NULL;
    contexts[index].width = 0;
    contexts[index].height = 0;
    __atomic_store_n(&contexts[index].used, 0, __ATOMIC_RELEASE);
    return TRUE;
}

static BOOL WINAPI ogl_wglMakeCurrent(HDC hdc, HGLRC context)
{
    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    if (!context) {
        HGLRC bound_context = binding ? binding->context : NULL;
        int current_index = context_index(bound_context);
        if (current_index >= 0 && contexts[current_index].backend) {
            OGL_MODULE_INSTANCE *module = contexts[current_index].module;
            if (module && module->make_current)
                module->make_current(NULL);
        }
        release_binding(binding);
        return TRUE;
    }
    int index = context_index(context);
    if (index < 0) return FALSE;
    if (context_bound_to_other_thread(context)) return FALSE;

    HWND window = (HWND)gdi32_window_from_dc(hdc);
    if (contexts[index].backend && window) {
        void *surface = NULL;
        int width = 0, height = 0, pitch = 0;
        if (!user32_get_window_surface(window, &surface, &width, &height,
                                       &pitch) ||
            (!contexts[index].module ||
             !contexts[index].module->resize_context ||
             contexts[index].module->resize_context(
                contexts[index].backend,
                user32_get_window_compositor_id(window), width, height) != 0))
            return FALSE;

        if (contexts[index].window != window ||
            contexts[index].width != width ||
            contexts[index].height != height) {
            serial_puts("[OPENGL32] drawable rebound window=0x");
            serial_puthex((uint64_t)(ULONG_PTR)window, 8);
            serial_puts(" size=");
            serial_putdec((uint32_t)width);
            serial_puts("x");
            serial_putdec((uint32_t)height);
            serial_puts("\n");
        }
        contexts[index].window = window;
        contexts[index].width = width;
        contexts[index].height = height;
    }

    if (contexts[index].backend) {
        OGL_MODULE_INSTANCE *module = contexts[index].module;
        if (!module || !module->make_current ||
            module->make_current(contexts[index].backend))
            return FALSE;
    }

    if (!binding) binding = current_binding(TRUE);
    if (!binding) {
        OGL_MODULE_INSTANCE *module = contexts[index].module;
        if (contexts[index].backend && module && module->make_current)
            module->make_current(NULL);
        return FALSE;
    }
    contexts[index].dc = hdc;
    binding->context = context;
    binding->dc = hdc;
    return TRUE;
}

static BOOL WINAPI ogl_wglShareLists(HGLRC source, HGLRC target)
{
    return context_index(source) >= 0 && context_index(target) >= 0;
}

static HGLRC WINAPI ogl_wglGetCurrentContext(void)
{
    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    return binding ? binding->context : NULL;
}

static HDC WINAPI ogl_wglGetCurrentDC(void)
{
    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    return binding ? binding->dc : NULL;
}

BOOL opengl32_swap_buffers(HANDLE hdc_handle)
{
    HDC hdc = (HDC)hdc_handle;
    uint64_t call = __atomic_add_fetch(&swap_trace_count, 1,
                                       __ATOMIC_RELAXED);
    BOOL trace = call <= 8 || (call & (call - 1)) == 0;
    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    HGLRC bound_context = binding ? binding->context : NULL;
    int index = context_index(bound_context);
    if (trace) {
        serial_puts("[OPENGL32-SWAP] enter call=0x");
        serial_puthex(call, 8);
        serial_puts(" hdc=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hdc, 16);
        serial_puts(" current=0x");
        serial_puthex((uint64_t)(ULONG_PTR)bound_context, 16);
        serial_puts("\n");
    }
    if (index < 0) {
        if (trace) serial_puts("[OPENGL32-SWAP] reject: no current context\n");
        return FALSE;
    }
    if (hdc && (!binding || binding->dc != hdc)) {
        if (trace) {
            serial_puts("[OPENGL32-SWAP] reject: dc mismatch expected=0x");
            serial_puthex((uint64_t)(ULONG_PTR)(binding ? binding->dc : NULL),
                          16);
            serial_puts("\n");
        }
        return FALSE;
    }

    OGL_CONTEXT *context = &contexts[index];
    if (!context->backend) {
        if (trace) serial_puts("[OPENGL32-SWAP] no backend\n");
        return TRUE;
    }

    void *pixels = NULL;
    int width = 0, height = 0, pitch = 0;
    HWND window = (HWND)gdi32_window_from_dc(hdc);
    if (!window) window = context->window;
    if (!window || !user32_get_window_surface(window, &pixels, &width,
                                               &height, &pitch)) {
        if (trace) serial_puts("[OPENGL32-SWAP] reject: no window surface\n");
        return FALSE;
    }

    OGL_MODULE_INSTANCE *module = context->module;
    if (!module || !module->readback || !module->swap_buffers) {
        if (trace) serial_puts("[OPENGL32-SWAP] reject: no owner module\n");
        return FALSE;
    }

    int result = module->readback(context->backend, pixels, width, height,
                                  pitch);
    if (result != 0) {
        if (trace) serial_puts("[OPENGL32-SWAP] reject: readback failed\n");
        return FALSE;
    }
    result = module->swap_buffers(context->backend);
    if (result != 0) {
        if (trace) serial_puts("[OPENGL32-SWAP] reject: backend swap failed\n");
        return FALSE;
    }
    user32_mark_window_dirty(window);
    if (trace) serial_puts("[OPENGL32-SWAP] success\n");
    return TRUE;
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
    return ogl_wglGetExtensionsStringARB(ogl_wglGetCurrentDC());
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
        case WGL_ACCELERATION_ARB:
            return opengl32_has_accelerated_backend()
                ? WGL_FULL_ACCELERATION_ARB : WGL_NO_ACCELERATION_ARB;
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
    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    if (!binding || !binding->context) return FALSE;
    binding->swap_interval = interval;
    return TRUE;
}

static int WINAPI ogl_wglGetSwapIntervalEXT(void)
{
    OGL_THREAD_BINDING *binding = current_binding(FALSE);
    return binding ? binding->swap_interval : 0;
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

static const WIN32_EXPORT opengl32_wgl_exports[] = {
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
};

static const WIN32_EXPORT opengl32_fallback_exports[] = {
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

static const OGL_GENERATED_EXPORT *opengl_generated_lookup(const char *name)
{
    unsigned low = 0;
    unsigned high = (unsigned)(sizeof(ogl_generated_exports) /
                               sizeof(ogl_generated_exports[0]));
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        int comparison = ogl_strcmp(name,
            ogl_generated_exports[middle].abi.name);
        if (comparison == 0) return &ogl_generated_exports[middle];
        if (comparison < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

static PVOID table_lookup(const WIN32_EXPORT *table, unsigned count,
                          const char *name)
{
    for (unsigned i = 0; i < count; i++)
        if (ogl_strcmp(name, table[i].name) == 0)
            return table[i].func;
    return NULL;
}

static PVOID opengl_lookup(const char *name)
{
    PVOID wgl = table_lookup(opengl32_wgl_exports,
        (unsigned)(sizeof(opengl32_wgl_exports) /
                   sizeof(opengl32_wgl_exports[0])), name);
    if (wgl) return wgl;

    const OGL_GENERATED_EXPORT *generated = opengl_generated_lookup(name);
    if (generated && opengl_load_module()) return generated->abi.func;

    return table_lookup(opengl32_fallback_exports,
        (unsigned)(sizeof(opengl32_fallback_exports) /
                   sizeof(opengl32_fallback_exports[0])), name);
}

void opengl32_shim_init(void)
{
    swap_trace_count = 0;
    unresolved_count = 0;
    ensure_strings();

    for (unsigned i = 0;
         i < sizeof(ogl_generated_exports) /
             sizeof(ogl_generated_exports[0]); i++) {
        win32_abi_register_compat32_bridge(
            ogl_generated_exports[i].abi.func,
            ogl_generated_exports[i].compat32);
    }
}

PVOID opengl32_resolve(const char *func_name, USHORT ordinal,
                       BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;

    PVOID wgl = table_lookup(opengl32_wgl_exports,
        (unsigned)(sizeof(opengl32_wgl_exports) /
                   sizeof(opengl32_wgl_exports[0])), func_name);
    if (wgl) return wgl;

    const OGL_GENERATED_EXPORT *generated =
        opengl_generated_lookup(func_name);
    if (generated && generated->core_11 && opengl_load_module())
        return generated->abi.func;

    return table_lookup(opengl32_fallback_exports,
        (unsigned)(sizeof(opengl32_fallback_exports) /
                   sizeof(opengl32_fallback_exports[0])), func_name);
}

const WIN32_EXPORT *opengl32_abi_table(int *count)
{
    enum {
        WGL_COUNT = sizeof(opengl32_wgl_exports) /
                    sizeof(opengl32_wgl_exports[0]),
        GL_COUNT = sizeof(ogl_generated_exports) /
                   sizeof(ogl_generated_exports[0]),
    };
    static WIN32_EXPORT combined[WGL_COUNT + GL_COUNT];
    static int initialized;

    if (!initialized) {
        for (unsigned i = 0; i < WGL_COUNT; i++)
            combined[i] = opengl32_wgl_exports[i];
        for (unsigned i = 0; i < GL_COUNT; i++)
            combined[WGL_COUNT + i] = ogl_generated_exports[i].abi;
        initialized = 1;
    }
    *count = WGL_COUNT + GL_COUNT;
    return combined;
}
