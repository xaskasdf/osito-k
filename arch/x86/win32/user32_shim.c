/*
 * OsitoK Windows Compatibility Layer — user32.dll Shim Implementation
 *
 * All windows map to a single OsitoK framebuffer.
 * The message queue is a ring buffer fed by keyboard/mouse drivers.
 * In TEST_HARNESS mode, the queue is always empty (no real input).
 */

#include "user32_shim.h"
#include "user32_controls.h"
#include "gdi32_shim.h"
#include "kernel32_shim.h"
#include "compat32.h"
#include "win32_abi.h"
#include "dllloader.h"
#include "../kernel/smp.h"
#include "../include/boot_info.h"
#include "../include/paging.h"

_Static_assert(sizeof(POINT) == 8, "Win32 POINT ABI");
_Static_assert(sizeof(RECT) == 16, "Win32 RECT ABI");
_Static_assert(sizeof(WINDOWPLACEMENT) == 44, "Win32 WINDOWPLACEMENT ABI");
_Static_assert(sizeof(WINDOWPOS) == 40, "Win64 WINDOWPOS ABI");
_Static_assert(sizeof(MENUINFO) == 40, "Win64 MENUINFO ABI");
_Static_assert(sizeof(MENUITEMINFOA) == 80, "Win64 MENUITEMINFOA ABI");
_Static_assert(sizeof(MENUITEMINFOW) == 80, "Win64 MENUITEMINFOW ABI");
_Static_assert(sizeof(MOUSEINPUT) == 32, "Win64 MOUSEINPUT ABI");
_Static_assert(sizeof(KEYBDINPUT) == 24, "Win64 KEYBDINPUT ABI");
_Static_assert(sizeof(INPUT) == 40, "Win64 INPUT ABI");

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void WINAPI SetLastError(DWORD dwErrCode);
extern DWORD WINAPI GetLastError(void);
extern DWORD WINAPI GetCurrentProcessId(void);
extern DWORD WINAPI GetCurrentThreadId(void);
extern DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles,
                                            BOOL wait_all, DWORD timeout_ms);
extern DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD timeout_ms);
extern HANDLE WINAPI CreateEventA(PVOID attributes, BOOL manual_reset,
                                  BOOL initial_state, PCSTR name);
extern BOOL WINAPI CloseHandle(HANDLE handle);
extern PVOID WINAPI VirtualAlloc(PVOID address, SIZE_T size,
                                 DWORD allocation_type, DWORD protection);
extern void *kcalloc(uint64_t count, uint64_t size);
extern void kfree(void *pointer);
extern NTSTATUS ntsync_set_event_for_process(HANDLE event, ULONG owner_pid,
                                              LONG *previous_state);
extern NTSTATUS nt_close_handle_for_process(HANDLE handle, ULONG owner_pid);
extern NTSTATUS nt_vm_release_allocation_for_process(ULONG owner_pid,
                                                      PVOID allocation_base);
extern DWORD WINAPI shim_timeGetTime(void);
extern TEB *win64_current_teb(void);
extern uint32_t compat32_get_last_caller_eip(void);
extern void ddraw_present_hook(void) __attribute__((weak));
extern DWORD ddraw_present_poll_interval(void) __attribute__((weak));
extern void xhci_poll(void) __attribute__((weak));
extern bool compositor_is_running(void) __attribute__((weak));

static void msg_read_from(const void *src, MSG *out);

#ifndef U32_INPUT_DIAGNOSTICS
#define U32_INPUT_DIAGNOSTICS 0
#endif

#ifndef U32_API_DIAGNOSTICS
#define U32_API_DIAGNOSTICS 0
#endif

#define USER32_FALLBACK_SCREEN_WIDTH  640
#define USER32_FALLBACK_SCREEN_HEIGHT 480

static int current_mode_cx(void);
static int current_mode_cy(void);

/* ── OsitoK compositor integration (weak — NULL in test harness) ── */
extern uint32_t shm_create_surface(uint32_t w, uint32_t h, uint32_t flags)
    __attribute__((weak));
extern void    *shm_map(uint32_t handle)   __attribute__((weak));
extern void     shm_unmap(uint32_t handle) __attribute__((weak));
extern void     shm_destroy(uint32_t handle) __attribute__((weak));
extern uint32_t compositor_create_window(uint32_t shm_handle,
    int16_t x, int16_t y, uint16_t width, uint16_t height,
    uint32_t pid, const char *title) __attribute__((weak));
extern uint32_t compositor_create_window_inactive(uint32_t shm_handle,
    int16_t x, int16_t y, uint16_t width, uint16_t height,
    uint32_t pid, const char *title) __attribute__((weak));
extern void compositor_destroy_window(uint32_t window_id) __attribute__((weak));
extern void compositor_signal_dirty(uint32_t window_id)   __attribute__((weak));
extern void compositor_request_frame(void) __attribute__((weak));
extern void compositor_set_visible(uint32_t window_id, bool visible)
    __attribute__((weak));
extern void compositor_set_position(uint32_t window_id, int16_t x, int16_t y)
    __attribute__((weak));
extern void compositor_set_z_order(uint32_t window_id, uint32_t z_order)
    __attribute__((weak));
extern void compositor_set_clip_rect(uint32_t window_id, bool enabled,
    int16_t x, int16_t y, uint16_t width, uint16_t height)
    __attribute__((weak));
extern void compositor_set_size(uint32_t window_id, uint16_t width, uint16_t height)
    __attribute__((weak));
extern void compositor_set_title(uint32_t window_id, const char *title)
    __attribute__((weak));
extern void compositor_set_taskbar(uint32_t window_id, bool taskbar)
    __attribute__((weak));
extern void compositor_set_fullscreen(uint32_t window_id, bool fullscreen)
    __attribute__((weak));
extern void compositor_set_user32_managed(uint32_t window_id, bool managed)
    __attribute__((weak));
extern void compositor_focus_window(uint32_t window_id)
    __attribute__((weak));
extern bool compositor_get_fullscreen_content_rect(
    uint32_t window_id, int32_t *x, int32_t *y,
    uint32_t *width, uint32_t *height) __attribute__((weak));
extern bool compositor_replace_surface(uint32_t window_id, uint32_t shm_handle,
    uint16_t width, uint16_t height) __attribute__((weak));

/* SHM surface flags (matches shm.c) */
#define SHM_FLAG_CPU_WRITE    (1 << 0)
#define SHM_FLAG_CPU_READ     (1 << 1)

/* ── String helpers ────────────────────────────────────────── */

static int u32_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int u32_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void u32_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ── Window class registry ─────────────────────────────────── */

#define WNDCLASS_BLOCK_SIZE 64
#define WNDCLASS_MAX_BLOCKS 256
#define MAX_WNDCLASSES (WNDCLASS_BLOCK_SIZE * WNDCLASS_MAX_BLOCKS)
#define WNDCLASS_ATOM_BASE 0xC000U

typedef struct {
    char        class_name[128];
    WNDPROC     wndproc;
    DWORD       style;
    int         cbClsExtra;
    int         cbWndExtra;
    HINSTANCE   hInstance;
    HICON       hIcon;
    HCURSOR     hCursor;
    HBRUSH      hbrBackground;
    ULONG_PTR   menu_name;
    ULONG_PTR   class_name_ptr;
    HICON       hIconSm;
    DWORD       owner_pid;
    WORD        slot;
    WORD        atom;
    int         system_class;
    int         native_wndproc;
    BOOL        unicode;
    int         used;
} WNDCLASS_ENTRY;

typedef struct {
    WNDCLASS_ENTRY entries[WNDCLASS_BLOCK_SIZE];
} WNDCLASS_BLOCK;

static WNDCLASS_BLOCK *wndclass_blocks[WNDCLASS_MAX_BLOCKS];
static int wndclass_count = 0;

static LRESULT WINAPI system_class_wndproc(HWND hWnd, DWORD Msg,
                                            WPARAM wParam, LPARAM lParam);
static LRESULT WINAPI dialog_class_wndproc(HWND hWnd, DWORD Msg,
                                            WPARAM wParam, LPARAM lParam);
static void default_window_paint(HWND window);
static BOOL window_set_text(HWND window, PCVOID text, BOOL wide);
static HBRUSH WINAPI GetSysColorBrush_u32(int index);

/* USER32 registers these classes before application code can use them.  The
 * metadata matches 32-bit and 64-bit NT; cbWndExtra is ABI-stable for these
 * classes even though it contains pointer-backed control state. */
static WNDCLASS_ENTRY system_wndclasses[] = {
    { .class_name = "BUTTON",    .wndproc = system_class_wndproc,
      .style = 0x0000008B, .cbWndExtra = 8,  .atom = 0x0080,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "EDIT",      .wndproc = system_class_wndproc,
      .style = 0x00000088, .cbWndExtra = 8,  .atom = 0x0081,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "STATIC",    .wndproc = system_class_wndproc,
      .style = 0x00000088, .cbWndExtra = 8,  .atom = 0x0082,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "LISTBOX",   .wndproc = system_class_wndproc,
      .style = 0x00000088, .cbWndExtra = 8,  .atom = 0x0083,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "SCROLLBAR", .wndproc = system_class_wndproc,
      .style = 0x00000088, .cbWndExtra = 80, .atom = 0x0084,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "COMBOBOX",  .wndproc = system_class_wndproc,
      .style = 0x0000008B, .cbWndExtra = 8,  .atom = 0x0085,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "MDICLIENT", .wndproc = system_class_wndproc,
      .style = 0x00000000, .cbWndExtra = 16,
      .hbrBackground = (HBRUSH)(ULONG_PTR)13,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "COMBOLBOX", .wndproc = system_class_wndproc,
      .style = 0x00000808, .cbWndExtra = 8,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
    { .class_name = "#32770",    .wndproc = dialog_class_wndproc,
      .style = 0x00000808, .cbWndExtra = 30, .atom = 0x8002,
      .system_class = 1, .native_wndproc = 1, .used = 1 },
};

#define SYSTEM_WNDCLASS_COUNT \
    (sizeof(system_wndclasses) / sizeof(system_wndclasses[0]))

static WNDCLASS_ENTRY *find_system_class(const char *name)
{
    if (!name) return NULL;
    for (SIZE_T i = 0; i < SYSTEM_WNDCLASS_COUNT; i++)
        if (u32_stricmp(system_wndclasses[i].class_name, name) == 0)
            return &system_wndclasses[i];
    return NULL;
}

static WNDCLASS_ENTRY *find_system_class_by_atom(WORD atom)
{
    for (SIZE_T i = 0; i < SYSTEM_WNDCLASS_COUNT; i++)
        if (system_wndclasses[i].atom == atom)
            return &system_wndclasses[i];
    return NULL;
}

static WNDCLASS_ENTRY *find_class_for_pid(const char *name, DWORD pid)
{
    for (int i = 0; i < wndclass_count; i++) {
        WNDCLASS_BLOCK *block = wndclass_blocks[i / WNDCLASS_BLOCK_SIZE];
        WNDCLASS_ENTRY *entry = block
            ? &block->entries[i % WNDCLASS_BLOCK_SIZE] : NULL;
        if (entry && entry->used && entry->owner_pid == pid &&
            u32_stricmp(entry->class_name, name) == 0)
            return entry;
    }
    return NULL;
}

static WNDCLASS_ENTRY *find_class(const char *name)
{
    return find_class_for_pid(name, GetCurrentProcessId());
}

static WNDCLASS_ENTRY *lookup_class_for_pid(const char *name, DWORD pid)
{
    WNDCLASS_ENTRY *entry = find_class_for_pid(name, pid);
    return entry ? entry : find_system_class(name);
}

static WNDCLASS_ENTRY *find_class_by_atom_for_pid(WORD atom, DWORD pid)
{
    for (int i = 0; i < wndclass_count; i++) {
        WNDCLASS_BLOCK *block = wndclass_blocks[i / WNDCLASS_BLOCK_SIZE];
        WNDCLASS_ENTRY *entry = block
            ? &block->entries[i % WNDCLASS_BLOCK_SIZE] : NULL;
        if (entry && entry->used && entry->owner_pid == pid &&
            entry->atom == atom)
            return entry;
    }
    return NULL;
}

static WNDCLASS_ENTRY *find_class_by_atom(WORD atom)
{
    return find_class_by_atom_for_pid(atom, GetCurrentProcessId());
}

static WNDCLASS_ENTRY *lookup_class_by_atom_for_pid(WORD atom, DWORD pid)
{
    WNDCLASS_ENTRY *entry = find_class_by_atom_for_pid(atom, pid);
    return entry ? entry : find_system_class_by_atom(atom);
}

static WNDCLASS_ENTRY *lookup_class_by_atom(WORD atom)
{
    return lookup_class_by_atom_for_pid(atom, GetCurrentProcessId());
}

static WNDPROC class_wndproc_for_mode(WNDCLASS_ENTRY *entry)
{
    if (!entry || !entry->native_wndproc || !g_compat32_mode)
        return entry ? entry->wndproc : NULL;

    uint32_t thunk = compat32_make_thunk_ex(
        (uint64_t)(ULONG_PTR)entry->wndproc,
        "USER32!NativeClassWndProc", 4, CC_STDCALL);
    return (WNDPROC)(ULONG_PTR)thunk;
}

static WNDCLASS_ENTRY *alloc_wndclass(void)
{
    for (int i = 0; i < wndclass_count; i++) {
        WNDCLASS_BLOCK *block = wndclass_blocks[i / WNDCLASS_BLOCK_SIZE];
        WNDCLASS_ENTRY *entry = block
            ? &block->entries[i % WNDCLASS_BLOCK_SIZE] : NULL;
        if (entry && !entry->used) {
            memset(entry, 0, sizeof(*entry));
            entry->slot = (WORD)i;
            entry->atom = (WORD)(WNDCLASS_ATOM_BASE + i);
            return entry;
        }
    }
    if (wndclass_count >= MAX_WNDCLASSES) return NULL;

    int slot = wndclass_count;
    int block_index = slot / WNDCLASS_BLOCK_SIZE;
    WNDCLASS_BLOCK *block = wndclass_blocks[block_index];
    if (!block) {
        block = (WNDCLASS_BLOCK *)kcalloc(1, sizeof(*block));
        if (!block) return NULL;
        wndclass_blocks[block_index] = block;
        serial_puts("[USER32] class registry capacity=");
        serial_putdec((uint64_t)(block_index + 1) * WNDCLASS_BLOCK_SIZE);
        serial_puts("\n");
    }

    WNDCLASS_ENTRY *entry =
        &block->entries[slot % WNDCLASS_BLOCK_SIZE];
    wndclass_count++;
    memset(entry, 0, sizeof(*entry));
    entry->slot = (WORD)slot;
    entry->atom = (WORD)(WNDCLASS_ATOM_BASE + slot);
    return entry;
}

WORD user32_register_library_class(PCSTR class_name, DWORD style,
                                   int cb_cls_extra, int cb_wnd_extra,
                                   HBRUSH background, WNDPROC wndproc)
{
    if (!class_name || !class_name[0] || !wndproc)
        return 0;

    DWORD pid = GetCurrentProcessId();
    WNDCLASS_ENTRY *entry = find_class_for_pid(class_name, pid);
    if (entry)
        return entry->native_wndproc && entry->wndproc == wndproc
             ? entry->atom : 0;

    entry = alloc_wndclass();
    if (!entry)
        return 0;

    u32_strcpy(entry->class_name, class_name, sizeof(entry->class_name));
    entry->wndproc = wndproc;
    entry->style = style | CS_GLOBALCLASS;
    entry->cbClsExtra = cb_cls_extra;
    entry->cbWndExtra = cb_wnd_extra;
    entry->hbrBackground = background;
    entry->owner_pid = pid;
    entry->native_wndproc = 1;
    entry->used = 1;
    return entry->atom;
}

BOOL user32_unregister_library_class(PCSTR class_name, WNDPROC wndproc)
{
    if (!class_name || !wndproc)
        return FALSE;
    WNDCLASS_ENTRY *entry = find_class_for_pid(
        class_name, GetCurrentProcessId());
    if (!entry || !entry->native_wndproc || entry->wndproc != wndproc)
        return FALSE;
    entry->used = 0;
    return TRUE;
}

/* ── Window objects ────────────────────────────────────────── */

#define MAX_WINDOWS 256

static void menu_release_window_menus(HWND window, HMENU menu_bar,
                                      HMENU system_menu);
static void menu_release_process(DWORD pid);
static void menu_sync_system_window(HWND window);
static void icon_release_process(DWORD pid);
static void user_object_release_process(DWORD pid);

typedef struct {
    HWND        handle;
    char        class_name[128];
    char        title[256];
    PWSTR       text;
    U32_CONTROL *control;
    BOOL        unicode;
    WNDPROC     wndproc;
    DWORD       style;
    DWORD       ex_style;
    DWORD       layered_color_key;
    DWORD       layered_flags;
    BYTE        layered_alpha;
    int         x, y, width, height;
    HWND        parent;
    HWND        owner;
    HMENU       menu;
    HMENU       system_menu;
    PVOID       user_data;
    DWORD       owner_pid;
    DWORD       owner_tid;
    int         visible;
    int         message_only;
    int         destroying;
    UINT        show_cmd;
    POINT       min_position;
    POINT       max_position;
    RECT        normal_rect;
    uint32_t    z_order;
    uint32_t    render_z;
    int         paint_pending;
    int         erase_pending;
    RECT        update_rect;
    DWORD       mouse_track_flags;
    DWORD       mouse_hover_time;
    DWORD       mouse_hover_start;
    POINT       mouse_hover_origin;
    int         used;
    /* OsitoK compositor integration */
    uint32_t    shm_handle;        /* shared memory surface handle (0 = none) */
    uint32_t    compositor_id;     /* compositor window ID (0 = none) */
    void       *shm_pixels;        /* mapped shm surface pointer */
    int         surface_width;     /* allocated backing width / row stride */
    int         surface_height;    /* allocated backing height */
    int         directdraw_exclusive;
} WINDOW;

static WINDOW windows[MAX_WINDOWS];
static int window_count = 0;
static ULONG_PTR next_hwnd = 0xA0000001;
static int defer_sync_depth;
static unsigned show_trace_count;

typedef struct {
    BOOL used;
    BOOL modal;
    BOOL ended;
    BOOL owner_disabled;
    HWND window;
    HWND owner;
    DLGPROC proc;
    LONG_PTR result;
} U32_DIALOG_STATE;

static U32_DIALOG_STATE dialog_states[MAX_WINDOWS];

static U32_DIALOG_STATE *dialog_state_find(HWND window);
static void dialog_release_window(HWND window);

/* ChangeDisplaySettings owns a logical desktop mode. The physical GOP mode
 * remains fixed; the compositor presents/scales the selected logical mode. */
typedef struct {
    int active;
    int fullscreen;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t frequency;
    DWORD owner_pid;
} USER_DISPLAY_MODE;

static USER_DISPLAY_MODE user_display_mode;

#define MAX_DEFER_WINDOW_POS 8

typedef struct {
    HWND window;
    HWND insert_after;
    int x, y, width, height;
    UINT flags;
} DEFER_WINDOW_OP;

typedef struct {
    int used;
    HDWP handle;
    DWORD owner_pid;
    int count;
    DEFER_WINDOW_OP ops[MAX_WINDOWS];
} DEFER_WINDOW_SET;

static DEFER_WINDOW_SET defer_window_sets[MAX_DEFER_WINDOW_POS];
static ULONG_PTR next_hdwp = 0xD5000001;
static HANDLE process_dpi_context = (HANDLE)(LONG_PTR)-2;
static HANDLE thread_dpi_context;

#define MAX_WINDOW_PROPERTIES 1024
#define MAX_WINDOW_PROPERTY_NAME 128

typedef struct {
    int used;
    HWND window;
    WORD atom;
    WCHAR name[MAX_WINDOW_PROPERTY_NAME];
    HANDLE value;
} WINDOW_PROPERTY;

static WINDOW_PROPERTY window_properties[MAX_WINDOW_PROPERTIES];

static WINDOW *find_window(HWND hwnd);
static void release_window(WINDOW *w);
static void property_release_window(HWND window);
static void trace_window_release(const char *reason, const WINDOW *w,
                                 HWND root, uint64_t caller);
static void sort_window_handles_top_to_bottom(HWND *handles, int count);
static WINDOW *alloc_window(void)
{
    for (int i = 0; i < window_count; i++)
        if (!windows[i].used)
            return &windows[i];
    if (window_count >= MAX_WINDOWS) {
        serial_puts("[USER32] window table full\n");
        return NULL;
    }
    return &windows[window_count++];
}

/* ── Compositor integration API (called by ddraw_shim) ─────── */

/* Get the shm pixel buffer for a window (NULL if no compositor) */
void *user32_get_window_shm_pixels(HWND hwnd);
uint32_t user32_get_window_compositor_id(HWND hwnd);
BOOL user32_get_window_surface(HWND hwnd, void **pixels, int *width,
                               int *height, int *pitch);
void user32_mark_window_dirty(HWND hwnd);

void *user32_get_window_shm_pixels(HWND hwnd)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].handle == hwnd)
            return windows[i].shm_pixels;
    }
    return NULL;
}

uint32_t user32_get_window_compositor_id(HWND hwnd)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].handle == hwnd)
            return windows[i].compositor_id;
    }
    return 0;
}

BOOL user32_get_window_surface(HWND hwnd, void **pixels, int *width,
                               int *height, int *pitch)
{
    WINDOW *w = find_window(hwnd);
    if (!w || !w->shm_pixels || w->surface_width <= 0 || w->surface_height <= 0)
        return FALSE;

    int visible_width = w->width;
    int visible_height = w->height;
    if (visible_width > w->surface_width) visible_width = w->surface_width;
    if (visible_height > w->surface_height) visible_height = w->surface_height;
    if (visible_width < 0) visible_width = 0;
    if (visible_height < 0) visible_height = 0;

    if (pixels) *pixels = w->shm_pixels;
    if (width) *width = visible_width;
    if (height) *height = visible_height;
    if (pitch) *pitch = w->surface_width * 4;
    return TRUE;
}

void user32_mark_window_dirty(HWND hwnd)
{
    WINDOW *w = find_window(hwnd);
    if (w && w->compositor_id && compositor_signal_dirty)
        compositor_signal_dirty(w->compositor_id);
}

static void sync_window_surface(WINDOW *w, int width, int height)
{
    if (!w || !w->compositor_id)
        return;

    if (width <= 0 || height <= 0) {
        if (compositor_set_size)
            compositor_set_size(w->compositor_id, 0, 0);
        return;
    }

    if (width <= w->surface_width && height <= w->surface_height) {
        if (compositor_set_size)
            compositor_set_size(w->compositor_id, (uint16_t)width,
                                (uint16_t)height);
        return;
    }

    if (!shm_create_surface || !shm_map || !shm_unmap || !shm_destroy ||
        !compositor_replace_surface)
        return;

    uint32_t new_handle = shm_create_surface((uint32_t)width, (uint32_t)height,
        SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ);
    if (!new_handle)
        return;

    void *new_pixels = shm_map(new_handle);
    if (!new_pixels) {
        shm_destroy(new_handle);
        return;
    }

    if (!compositor_replace_surface(w->compositor_id, new_handle,
                                    (uint16_t)width, (uint16_t)height)) {
        shm_unmap(new_handle);
        shm_destroy(new_handle);
        return;
    }

    uint32_t old_handle = w->shm_handle;
    w->shm_handle = new_handle;
    w->shm_pixels = new_pixels;
    w->surface_width = width;
    w->surface_height = height;
    if (old_handle) {
        shm_unmap(old_handle);
        shm_destroy(old_handle);
    }
}

static WINDOW *find_window(HWND hwnd)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].handle == hwnd)
            return &windows[i];
    }
    return NULL;
}

static BOOL property_key_is_atom(const void *key)
{
    return ((ULONG_PTR)key >> 16) == 0;
}

static WCHAR property_fold_char(WCHAR value)
{
    if (value >= 'A' && value <= 'Z')
        return value + ('a' - 'A');
    return value;
}

static BOOL property_name_equals_a(const WINDOW_PROPERTY *property,
                                   PCSTR name)
{
    if (!property || property->atom || !name)
        return FALSE;
    for (int i = 0; i < MAX_WINDOW_PROPERTY_NAME; i++) {
        WCHAR actual = property->name[i];
        WCHAR wanted = (WCHAR)(BYTE)name[i];
        if (property_fold_char(actual) != property_fold_char(wanted))
            return FALSE;
        if (!actual)
            return TRUE;
    }
    return FALSE;
}

static BOOL property_name_equals_w(const WINDOW_PROPERTY *property,
                                   PCWSTR name)
{
    if (!property || property->atom || !name)
        return FALSE;
    for (int i = 0; i < MAX_WINDOW_PROPERTY_NAME; i++) {
        WCHAR actual = property->name[i];
        WCHAR wanted = name[i];
        if (property_fold_char(actual) != property_fold_char(wanted))
            return FALSE;
        if (!actual)
            return TRUE;
    }
    return FALSE;
}

static WINDOW_PROPERTY *property_find_a(HWND window, PCSTR name)
{
    BOOL atom_key = property_key_is_atom(name);
    WORD atom = (WORD)(ULONG_PTR)name;
    for (int i = 0; i < MAX_WINDOW_PROPERTIES; i++) {
        WINDOW_PROPERTY *property = &window_properties[i];
        if (!property->used || property->window != window)
            continue;
        if (atom_key ? property->atom == atom
                     : property_name_equals_a(property, name))
            return property;
    }
    return NULL;
}

static WINDOW_PROPERTY *property_find_w(HWND window, PCWSTR name)
{
    BOOL atom_key = property_key_is_atom(name);
    WORD atom = (WORD)(ULONG_PTR)name;
    for (int i = 0; i < MAX_WINDOW_PROPERTIES; i++) {
        WINDOW_PROPERTY *property = &window_properties[i];
        if (!property->used || property->window != window)
            continue;
        if (atom_key ? property->atom == atom
                     : property_name_equals_w(property, name))
            return property;
    }
    return NULL;
}

static WINDOW_PROPERTY *property_alloc(HWND window)
{
    for (int i = 0; i < MAX_WINDOW_PROPERTIES; i++) {
        if (!window_properties[i].used) {
            window_properties[i] = (WINDOW_PROPERTY){0};
            window_properties[i].used = 1;
            window_properties[i].window = window;
            return &window_properties[i];
        }
    }
    return NULL;
}

static BOOL property_store_name_a(WINDOW_PROPERTY *property, PCSTR name)
{
    if (property_key_is_atom(name)) {
        property->atom = (WORD)(ULONG_PTR)name;
        return property->atom != 0;
    }
    if (!name)
        return FALSE;
    for (int i = 0; i < MAX_WINDOW_PROPERTY_NAME; i++) {
        property->name[i] = (WCHAR)(BYTE)name[i];
        if (!name[i])
            return TRUE;
    }
    return FALSE;
}

static BOOL property_store_name_w(WINDOW_PROPERTY *property, PCWSTR name)
{
    if (property_key_is_atom(name)) {
        property->atom = (WORD)(ULONG_PTR)name;
        return property->atom != 0;
    }
    if (!name)
        return FALSE;
    for (int i = 0; i < MAX_WINDOW_PROPERTY_NAME; i++) {
        property->name[i] = name[i];
        if (!name[i])
            return TRUE;
    }
    return FALSE;
}

#if U32_API_DIAGNOSTICS
static void property_trace(const char *operation, HWND window,
                           const WINDOW_PROPERTY *property)
{
    static unsigned trace_count;
    if (trace_count++ >= 48)
        return;
    serial_puts("[USER32-PROP] ");
    serial_puts(operation);
    serial_puts(" hwnd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)window, 8);
    serial_puts(" key=");
    if (property && property->atom) {
        serial_puts("#");
        serial_puthex(property->atom, 4);
    } else if (property) {
        for (int i = 0; i < MAX_WINDOW_PROPERTY_NAME && property->name[i]; i++) {
            char text[2] = {
                property->name[i] < 0x80 ? (char)property->name[i] : '?', 0
            };
            serial_puts(text);
        }
    } else {
        serial_puts("<missing>");
    }
    serial_puts(" value=0x");
    serial_puthex(property ? (uint64_t)(ULONG_PTR)property->value : 0, 16);
    serial_puts("\n");
}
#else
#define property_trace(...) ((void)0)
#endif

static void property_release_window(HWND window)
{
    for (int i = 0; i < MAX_WINDOW_PROPERTIES; i++)
        if (window_properties[i].used &&
            window_properties[i].window == window)
            window_properties[i] = (WINDOW_PROPERTY){0};
}

static int hwnd_insert_token(HWND hwnd)
{
    ULONG_PTR value = (ULONG_PTR)hwnd;
    uint32_t low = (uint32_t)value;

    if (value == 0) return 0;
    if (value == (ULONG_PTR)(LONG_PTR)-1 || low == 0xFFFFFFFFU) return -1;
    if (value == (ULONG_PTR)(LONG_PTR)-2 || low == 0xFFFFFFFEU) return -2;
    if (value == 1) return 1;
    return 2;
}

static int hwnd_is_desktop(HWND hwnd)
{
    return (ULONG_PTR)hwnd == 0xD0000001U;
}

static int hwnd_is_message(HWND hwnd)
{
    ULONG_PTR value = (ULONG_PTR)hwnd;
    return value == (ULONG_PTR)(LONG_PTR)-3 ||
           (uint32_t)value == 0xFFFFFFFDU;
}

static int window_is_topmost(const WINDOW *w)
{
    return w && (w->ex_style & WS_EX_TOPMOST) != 0;
}

static void window_screen_origin(const WINDOW *w, int *screen_x, int *screen_y)
{
    int x = 0, y = 0;
    const WINDOW *current = w;

    for (int depth = 0; current && depth < MAX_WINDOWS; depth++) {
        x += current->x;
        y += current->y;
        if (!current->parent)
            break;
        WINDOW *parent = find_window(current->parent);
        if (!parent || parent == current)
            break;
        current = parent;
    }

    if (screen_x) *screen_x = x;
    if (screen_y) *screen_y = y;
}

static WINDOW *window_root(WINDOW *w, int *out_depth)
{
    WINDOW *current = w;
    int depth = 0;

    while (current && current->parent && depth < MAX_WINDOWS) {
        WINDOW *parent = find_window(current->parent);
        if (!parent || parent == current)
            break;
        current = parent;
        depth++;
    }

    if (out_depth) *out_depth = depth;
    return current;
}

static int root_is_above(const WINDOW *a, const WINDOW *b)
{
    int a_topmost = window_is_topmost(a);
    int b_topmost = window_is_topmost(b);
    if (a_topmost != b_topmost)
        return a_topmost > b_topmost;
    if (a->z_order != b->z_order)
        return a->z_order > b->z_order;
    return (ULONG_PTR)a->handle > (ULONG_PTR)b->handle;
}

static int sibling_is_above(const WINDOW *a, const WINDOW *b)
{
    if (!a->parent && !b->parent)
        return root_is_above(a, b);
    if (a->z_order != b->z_order)
        return a->z_order > b->z_order;
    return (ULONG_PTR)a->handle > (ULONG_PTR)b->handle;
}

static int same_z_group(const WINDOW *a, const WINDOW *b)
{
    if (!a || !b || a->parent != b->parent)
        return 0;
    if (!a->parent && window_is_topmost(a) != window_is_topmost(b))
        return 0;
    return 1;
}

static void sort_windows_bottom_to_top(WINDOW **list, int count)
{
    for (int i = 1; i < count; i++) {
        WINDOW *key = list[i];
        int j = i - 1;
        while (j >= 0 && sibling_is_above(list[j], key)) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
}

static void append_window_subtree(WINDOW *window, WINDOW **ordered,
                                  int *ordered_count, BYTE *visited)
{
    if (!window || !ordered || !ordered_count || !visited ||
        *ordered_count >= MAX_WINDOWS)
        return;

    int index = (int)(window - windows);
    if (index < 0 || index >= MAX_WINDOWS || visited[index])
        return;

    visited[index] = 1;
    ordered[(*ordered_count)++] = window;

    WINDOW *children[MAX_WINDOWS];
    int child_count = 0;
    for (int i = 0; i < window_count && child_count < MAX_WINDOWS; i++) {
        WINDOW *child = &windows[i];
        if (child->used && child->parent == window->handle)
            children[child_count++] = child;
    }
    sort_windows_bottom_to_top(children, child_count);

    for (int i = 0; i < child_count; i++)
        append_window_subtree(children[i], ordered, ordered_count, visited);
}

static void sync_compositor_z_orders(void)
{
    WINDOW *roots[MAX_WINDOWS];
    int root_count = 0;

    for (int i = 0; i < window_count; i++) {
        WINDOW *w = &windows[i];
        if (!w->used) continue;
        WINDOW *root = window_root(w, NULL);
        int known = 0;
        for (int j = 0; j < root_count; j++)
            if (roots[j] == root) { known = 1; break; }
        if (!known && root_count < MAX_WINDOWS)
            roots[root_count++] = root;
    }

    sort_windows_bottom_to_top(roots, root_count);

    WINDOW *ordered[MAX_WINDOWS];
    BYTE visited[MAX_WINDOWS] = {0};
    int ordered_count = 0;
    for (int i = 0; i < root_count; i++)
        append_window_subtree(roots[i], ordered, &ordered_count, visited);

    /* Malformed applications can leave an orphan or even a parent cycle.
     * Keep those windows deterministic without allowing them to duplicate an
     * already-flattened node. Normal Win32 trees never take this path. */
    WINDOW *remaining[MAX_WINDOWS];
    int remaining_count = 0;
    for (int i = 0; i < window_count && remaining_count < MAX_WINDOWS; i++) {
        if (windows[i].used && !visited[i])
            remaining[remaining_count++] = &windows[i];
    }
    sort_windows_bottom_to_top(remaining, remaining_count);
    for (int i = 0; i < remaining_count; i++)
        append_window_subtree(remaining[i], ordered, &ordered_count, visited);

    for (int i = 0; i < ordered_count; i++) {
        WINDOW *w = ordered[i];
        w->render_z = (uint32_t)(i + 1);
        if (w->compositor_id && compositor_set_z_order)
            compositor_set_z_order(w->compositor_id, w->render_z);
    }
}

static BOOL window_style_is_visible(const WINDOW *w)
{
    const WINDOW *current = w;
    for (int depth = 0; current && depth < MAX_WINDOWS; depth++) {
        if (!current->visible)
            return FALSE;
        if (!current->parent)
            return TRUE;
        if (hwnd_is_desktop(current->parent))
            return TRUE;
        WINDOW *parent = find_window(current->parent);
        if (!parent || parent == current)
            return FALSE;
        current = parent;
    }
    return FALSE;
}

BOOL user32_accessibility_snapshot(HWND handle,
                                   USER32_ACCESSIBLE_WINDOW_INFO *info)
{
    if (!info) return FALSE;
    *info = (USER32_ACCESSIBLE_WINDOW_INFO){0};

    WINDOW *w = find_window(handle);
    if (!w || w->destroying) return FALSE;

    int screen_x, screen_y;
    window_screen_origin(w, &screen_x, &screen_y);

    info->handle = w->handle;
    info->parent = w->parent;
    info->owner = w->owner;
    info->style = w->style;
    info->ex_style = w->ex_style;
    info->owner_pid = w->owner_pid;
    info->owner_tid = w->owner_tid;
    info->window_rect.left = screen_x;
    info->window_rect.top = screen_y;
    info->window_rect.right = screen_x + w->width;
    info->window_rect.bottom = screen_y + w->height;
    info->client_rect = info->window_rect;
    info->visible = !w->message_only && window_style_is_visible(w);
    info->message_only = w->message_only ? TRUE : FALSE;

    info->enabled = TRUE;
    WINDOW *current = w;
    for (int depth = 0; current && depth < MAX_WINDOWS; depth++) {
        if (current->style & WS_DISABLED) {
            info->enabled = FALSE;
            break;
        }
        current = current->parent ? find_window(current->parent) : NULL;
    }

    for (int i = 0; i + 1 < USER32_ACCESSIBILITY_TITLE_CAP &&
                    w->title[i]; i++)
        info->title[i] = (WCHAR)(BYTE)w->title[i];
    return TRUE;
}

UINT user32_accessibility_children(HWND parent, HWND *children, UINT capacity)
{
    if (!find_window(parent)) return 0;

    HWND matches[MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < window_count && count < MAX_WINDOWS; i++) {
        WINDOW *candidate = &windows[i];
        if (candidate->used && !candidate->destroying &&
            !candidate->message_only && candidate->parent == parent)
            matches[count++] = candidate->handle;
    }
    sort_window_handles_top_to_bottom(matches, count);

    if (children) {
        UINT copied = capacity < (UINT)count ? capacity : (UINT)count;
        for (UINT i = 0; i < copied; i++) children[i] = matches[i];
    }
    return (UINT)count;
}

static BOOL window_should_render(const WINDOW *w)
{
    const WINDOW *current = w;
    if (!w || w->message_only)
        return FALSE;
    if (!window_style_is_visible(w))
        return FALSE;

    for (int depth = 0; current && depth < MAX_WINDOWS; depth++) {
        if (current->style & WS_MINIMIZE)
            return FALSE;
        if (!current->parent)
            break;
        current = find_window(current->parent);
    }

    current = w;
    for (int depth = 0; current && current->owner && depth < MAX_WINDOWS; depth++) {
        current = find_window(current->owner);
        if (!current || !window_style_is_visible(current) ||
            (current->style & WS_MINIMIZE))
            return current ? FALSE : TRUE;
    }
    return TRUE;
}

static BOOL window_can_receive_input(const WINDOW *w)
{
    const WINDOW *current = w;

    if (!w || !w->used || w->destroying)
        return FALSE;
    for (int depth = 0; current && depth < MAX_WINDOWS; depth++) {
        if (current->style & WS_DISABLED)
            return FALSE;
        current = current->parent ? find_window(current->parent) : NULL;
    }
    return TRUE;
}

static BOOL window_can_activate(const WINDOW *w)
{
    return window_can_receive_input(w) && window_should_render(w) && !w->parent &&
           !(w->style & WS_CHILD) && !(w->ex_style & WS_EX_NOACTIVATE);
}

static BOOL window_is_taskbar_candidate(const WINDOW *w)
{
    if (!w || !w->used || w->destroying || w->message_only || !w->visible)
        return FALSE;
    if (w->parent || (w->style & WS_CHILD))
        return FALSE;
    if (w->ex_style & WS_EX_APPWINDOW)
        return TRUE;
    if (w->owner || (w->ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))
        return FALSE;
    return TRUE;
}

static BOOL window_parent_clip_rect(const WINDOW *w, RECT *clip)
{
    if (!w || !clip || !w->parent || hwnd_is_desktop(w->parent))
        return FALSE;

    WINDOW *parent = find_window(w->parent);
    if (!parent)
        return FALSE;

    int parent_x, parent_y;
    window_screen_origin(parent, &parent_x, &parent_y);
    clip->left = parent_x;
    clip->top = parent_y;
    clip->right = parent_x + parent->width;
    clip->bottom = parent_y + parent->height;

    for (int depth = 0; parent->parent && depth < MAX_WINDOWS; depth++) {
        if (hwnd_is_desktop(parent->parent))
            break;
        parent = find_window(parent->parent);
        if (!parent) break;
        window_screen_origin(parent, &parent_x, &parent_y);
        int left = parent_x;
        int top = parent_y;
        int right = parent_x + parent->width;
        int bottom = parent_y + parent->height;
        if (left > clip->left) clip->left = left;
        if (top > clip->top) clip->top = top;
        if (right < clip->right) clip->right = right;
        if (bottom < clip->bottom) clip->bottom = bottom;
    }
    if (clip->right < clip->left) clip->right = clip->left;
    if (clip->bottom < clip->top) clip->bottom = clip->top;
    return TRUE;
}

static BOOL window_should_be_fullscreen(const WINDOW *w)
{
    int screen_x, screen_y;

    if (!w || !w->used || w->destroying || w->message_only ||
        !window_should_render(w) || w->parent || (w->style & WS_CHILD))
        return FALSE;

    /* DirectDraw exclusive cooperative mode is independently sufficient to
     * own scanout. Legacy applications commonly call SetDisplayMode without
     * ChangeDisplaySettings, so tying this state only to the latter leaves a
     * live exclusive viewport composited as an ordinary desktop window. */
    if (w->directdraw_exclusive)
        return TRUE;

    if (!user_display_mode.active || !user_display_mode.fullscreen ||
        w->owner_pid != user_display_mode.owner_pid ||
        !(w->style & WS_POPUP))
        return FALSE;

    window_screen_origin(w, &screen_x, &screen_y);
    return screen_x == 0 && screen_y == 0 &&
           w->width == (int)user_display_mode.width &&
           w->height == (int)user_display_mode.height;
}

static void sync_all_window_compositor_state(void)
{
    if (defer_sync_depth > 0)
        return;
    for (int i = 0; i < window_count; i++) {
        WINDOW *w = &windows[i];
        if (!w->used || !w->compositor_id)
            continue;
        int screen_x, screen_y;
        window_screen_origin(w, &screen_x, &screen_y);
        if (compositor_set_position)
            compositor_set_position(w->compositor_id,
                                    (int16_t)screen_x, (int16_t)screen_y);
        if (compositor_set_visible)
            compositor_set_visible(w->compositor_id,
                                   window_should_render(w) != FALSE);
        if (compositor_set_title)
            compositor_set_title(w->compositor_id, w->title);
        if (compositor_set_taskbar)
            compositor_set_taskbar(w->compositor_id,
                                   window_is_taskbar_candidate(w) != FALSE);
        if (compositor_set_user32_managed)
            compositor_set_user32_managed(w->compositor_id, true);
        if (compositor_set_fullscreen)
            compositor_set_fullscreen(w->compositor_id,
                                      window_should_be_fullscreen(w) != FALSE);
        if (compositor_set_clip_rect) {
            RECT clip;
            BOOL enabled = window_parent_clip_rect(w, &clip);
            compositor_set_clip_rect(
                w->compositor_id, enabled != FALSE,
                enabled ? (int16_t)clip.left : 0,
                enabled ? (int16_t)clip.top : 0,
                enabled ? (uint16_t)(clip.right - clip.left) : 0,
                enabled ? (uint16_t)(clip.bottom - clip.top) : 0);
        }
    }
    sync_compositor_z_orders();
}

static void update_normal_rect(WINDOW *w)
{
    if (!w || (w->style & (WS_MINIMIZE | WS_MAXIMIZE)))
        return;
    w->normal_rect.left = w->x;
    w->normal_rect.top = w->y;
    w->normal_rect.right = w->x + w->width;
    w->normal_rect.bottom = w->y + w->height;
}

static BOOL place_window_in_z_order(WINDOW *w, HWND insert_after)
{
    if (!w) return FALSE;

    int token = hwnd_insert_token(insert_after);
    if (token == -1)
        w->ex_style |= WS_EX_TOPMOST;
    else if (token == -2 || token == 1)
        w->ex_style &= ~WS_EX_TOPMOST;

    WINDOW *ordered[MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < window_count; i++) {
        WINDOW *candidate = &windows[i];
        if (candidate->used && candidate != w && same_z_group(candidate, w))
            ordered[count++] = candidate;
    }
    sort_windows_bottom_to_top(ordered, count);

    int position = count;
    if (token == 1) {
        position = 0;
    } else if (token == 2) {
        WINDOW *after = find_window(insert_after);
        if (!after || !same_z_group(after, w)) {
            SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
            return FALSE;
        }
        position = count;
        for (int i = 0; i < count; i++) {
            if (ordered[i] == after) {
                position = i + 1;
                break;
            }
        }
    }

    for (int i = count; i > position; i--)
        ordered[i] = ordered[i - 1];
    ordered[position] = w;
    count++;
    for (int i = 0; i < count; i++)
        ordered[i]->z_order = (uint32_t)(i + 1);

    sync_compositor_z_orders();
    return TRUE;
}

/* ── Message queue ─────────────────────────────────────────── */

#define MSG_QUEUE_SIZE 256

/* ── Input state ──────────────────────────────────────────── */

/*
 * key_state[vk]: bit 0 = toggled, bit 7 = currently down.
 * async_pressed[vk]: set when key goes down, cleared by GetAsyncKeyState.
 */
static BYTE key_state[256];
static BOOL keyboard_alt_pending;
static BYTE async_pressed[256];
static BYTE key_state_at_msg[256]; /* snapshot at last PeekMessage/GetMessage retrieval */
static DWORD mouse_buttons = 0; /* bits 0-4: left, right, middle, X1, X2 */
static LPARAM message_extra_info;
static POINT cursor_pos = {
    USER32_FALLBACK_SCREEN_WIDTH / 2,
    USER32_FALLBACK_SCREEN_HEIGHT / 2
};

/* ── Message queue ────────────────────────────────────────── */

static MSG msg_queue[MSG_QUEUE_SIZE];
static DWORD msg_target_pid[MSG_QUEUE_SIZE];
static DWORD msg_target_tid[MSG_QUEUE_SIZE];
static LPARAM msg_extra_info[MSG_QUEUE_SIZE];
enum {
    MSG_SOURCE_POSTED = 0,
    MSG_SOURCE_INPUT = 1,
};
static BYTE msg_source[MSG_QUEUE_SIZE];
static uint64_t msg_input_sequence[MSG_QUEUE_SIZE];
static uint64_t next_input_sequence;

typedef struct {
    HWND window;
    int pointer_offset_x;
    int pointer_offset_y;
} NATIVE_MOVE_STATE;

static NATIVE_MOVE_STATE native_move;

#define INPUT_SEQUENCE_STATE_SLOTS 256
typedef struct {
    BOOL used;
    DWORD pid;
    DWORD tid;
    uint64_t sequence;
    BYTE last_source;
} INPUT_SEQUENCE_STATE;
static INPUT_SEQUENCE_STATE input_sequence_states[INPUT_SEQUENCE_STATE_SLOTS];
static int msg_head = 0, msg_tail = 0;
static int dispatch_depth = 0;
static DWORD queue_changed_status = 0;
static spinlock_t msg_queue_lock = SPINLOCK_INIT;

/* SendMessage to a window owned by another thread is not a normal posted
 * message.  The target thread executes it while pumping USER32 and the sender
 * waits for the returned LRESULT.  Keeping these calls separate also avoids
 * exposing implementation metadata through the public MSG structure. */
#define SENT_MESSAGE_SLOTS 256

enum {
    SENT_MESSAGE_FREE = 0,
    SENT_MESSAGE_PENDING,
    SENT_MESSAGE_PROCESSING,
    SENT_MESSAGE_DONE,
    SENT_MESSAGE_ABANDONED,
};

typedef struct {
    BYTE state;
    uint32_t generation;
    uint64_t sequence;
    DWORD sender_pid;
    DWORD sender_tid;
    DWORD target_pid;
    DWORD target_tid;
    HWND window;
    DWORD message;
    WPARAM wparam;
    LPARAM lparam;
    LRESULT result;
    BOOL unicode;
    HANDLE completion_event;
} SENT_MESSAGE;

static SENT_MESSAGE sent_messages[SENT_MESSAGE_SLOTS];
static spinlock_t sent_message_lock = SPINLOCK_INIT;
static uint32_t sent_message_generation;
static uint64_t sent_message_sequence;

#define MSG_WAIT_EVENT_SLOTS 256

typedef struct {
    BOOL used;
    DWORD pid;
    DWORD tid;
    HANDLE event;
} MSG_WAIT_EVENT;

static MSG_WAIT_EVENT msg_wait_events[MSG_WAIT_EVENT_SLOTS];
static spinlock_t msg_wait_event_lock = SPINLOCK_INIT;

#define USER_TIMER_SLOTS   256
#define USER_TIMER_MINIMUM 10U
#define USER_TIMER_MAXIMUM 0x7FFFFFFFU

typedef struct {
    BOOL used;
    BOOL pending;
    DWORD owner_pid;
    DWORD owner_tid;
    HWND window;
    ULONG_PTR id;
    UINT interval_ms;
    DWORD due_time;
    DWORD message_time;
    PVOID callback;
    BOOL posted;
    PVOID pending_callback;
} USER_TIMER;

static USER_TIMER user_timers[USER_TIMER_SLOTS];
static spinlock_t user_timer_lock = SPINLOCK_INIT;
static ULONG_PTR next_user_timer_id = 1;

#define USER_HOOK_SLOTS          256
#define USER_HOOK_CONTEXT_SLOTS  256
#define USER_HOOK_MAX_DEPTH      32
#define USER_HOOK_SCRATCH_STRIDE 64
#define USER_WINDOWPOS_MAX_DEPTH 16
#define USER_WINDOWPOS_STRIDE    32
#define USER_CREATE_MAX_DEPTH    16
#define USER_CREATE_STRIDE       48
#define USER_WINDOWPOS_OFFSET \
    (USER_HOOK_MAX_DEPTH * USER_HOOK_SCRATCH_STRIDE)
#define USER_CREATE_OFFSET \
    (USER_WINDOWPOS_OFFSET + USER_WINDOWPOS_MAX_DEPTH * USER_WINDOWPOS_STRIDE)

_Static_assert(USER_CREATE_OFFSET +
               USER_CREATE_MAX_DEPTH * USER_CREATE_STRIDE <= 0x1000,
               "User32 callback scratch exceeds one page");

typedef struct {
    BOOL used;
    HHOOK handle;
    int type;
    HOOKPROC callback;
    HINSTANCE module;
    DWORD owner_pid;
    DWORD installer_tid;
    DWORD target_tid;
    uint64_t sequence;
    BOOL compat32;
} USER_HOOK;

typedef struct {
    int type;
    uint64_t current_sequence;
} USER_HOOK_FRAME;

typedef struct {
    BOOL used;
    DWORD pid;
    DWORD tid;
    int depth;
    int windowpos_depth;
    int create_depth;
    BOOL unicode_message;
    PVOID scratch_page;
    USER_HOOK_FRAME frames[USER_HOOK_MAX_DEPTH];
} USER_HOOK_CONTEXT;

static USER_HOOK user_hooks[USER_HOOK_SLOTS];
static USER_HOOK_CONTEXT user_hook_contexts[USER_HOOK_CONTEXT_SLOTS];
static spinlock_t user_hook_lock = SPINLOCK_INIT;
static ULONG_PTR next_user_hook_handle = 0xD6000001;
static uint64_t next_user_hook_sequence = 1;

static inline uint64_t user_hook_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&user_hook_lock);
    return flags;
}

static inline void user_hook_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&user_hook_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static BOOL user_hook_type_valid(int type)
{
    return type >= WH_MSGFILTER && type <= WH_MOUSE_LL;
}

static USER_HOOK_CONTEXT *user_hook_context_get(BOOL create)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    USER_HOOK_CONTEXT *context = NULL;
    USER_HOOK_CONTEXT *free_context = NULL;
    uint64_t flags = user_hook_lock_irqsave();

    for (int i = 0; i < USER_HOOK_CONTEXT_SLOTS; i++) {
        USER_HOOK_CONTEXT *candidate = &user_hook_contexts[i];
        if (candidate->used && candidate->pid == pid &&
            candidate->tid == tid) {
            context = candidate;
            break;
        }
        if (!candidate->used && !free_context)
            free_context = candidate;
    }

    if (!context && create && free_context) {
        context = free_context;
        context->used = TRUE;
        context->pid = pid;
        context->tid = tid;
        context->depth = 0;
        context->windowpos_depth = 0;
        context->create_depth = 0;
        context->scratch_page = NULL;
    }

    user_hook_unlock_irqrestore(flags);
    return context;
}

static BOOL user_hook_has_match(int type)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    BOOL found = FALSE;
    uint64_t flags = user_hook_lock_irqsave();

    for (int i = 0; i < USER_HOOK_SLOTS; i++) {
        USER_HOOK *hook = &user_hooks[i];
        if (hook->used && hook->type == type && hook->owner_pid == pid &&
            (!hook->target_tid || hook->target_tid == tid)) {
            found = TRUE;
            break;
        }
    }

    user_hook_unlock_irqrestore(flags);
    return found;
}

static BOOL user_hook_find_next(int type, uint64_t before_sequence,
                                USER_HOOK *result)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    uint64_t best_sequence = 0;
    BOOL found = FALSE;
    uint64_t flags = user_hook_lock_irqsave();

    for (int i = 0; i < USER_HOOK_SLOTS; i++) {
        USER_HOOK *hook = &user_hooks[i];
        if (!hook->used || hook->type != type || hook->owner_pid != pid ||
            (hook->target_tid && hook->target_tid != tid) ||
            hook->sequence >= before_sequence ||
            hook->sequence <= best_sequence)
            continue;
        *result = *hook;
        best_sequence = hook->sequence;
        found = TRUE;
    }

    user_hook_unlock_irqrestore(flags);
    return found;
}

static LRESULT user_hook_call_next(USER_HOOK_CONTEXT *context, int code,
                                   WPARAM wParam, LPARAM lParam)
{
    if (!context || context->depth <= 0)
        return 0;

    USER_HOOK_FRAME *frame = &context->frames[context->depth - 1];
    USER_HOOK hook;
    if (!user_hook_find_next(frame->type, frame->current_sequence, &hook))
        return 0;

    uint64_t previous_sequence = frame->current_sequence;
    frame->current_sequence = hook.sequence;
    LRESULT result;
    if (hook.compat32) {
        uint32_t args[3] = {
            (uint32_t)code,
            (uint32_t)wParam,
            (uint32_t)lParam,
        };
        uint32_t stack_top = compat32_current_user_stack_top();
        result = (LRESULT)(int32_t)(stack_top
            ? compat32_callback_args_on_stack(
                  (uint32_t)(ULONG_PTR)hook.callback, 3, args, stack_top)
            : compat32_callback_args(
                  (uint32_t)(ULONG_PTR)hook.callback, 3, args));
    } else {
        result = hook.callback(code, wParam, lParam);
    }
    frame->current_sequence = previous_sequence;
    return result;
}

static LRESULT user_hook_dispatch(int type, int code,
                                  WPARAM wParam, LPARAM lParam)
{
    if (!user_hook_has_match(type))
        return 0;

    USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
    if (!context || context->depth >= USER_HOOK_MAX_DEPTH) {
        serial_puts("[USER32-HOOK] callback depth exhausted\n");
        return 0;
    }

    USER_HOOK_FRAME *frame = &context->frames[context->depth++];
    frame->type = type;
    frame->current_sequence = UINT64_MAX;
    LRESULT result = user_hook_call_next(context, code, wParam, lParam);
    context->depth--;
    return result;
}

static PVOID user32_callback_scratch_page(USER_HOOK_CONTEXT *context)
{
    if (!context)
        return NULL;
    if (!context->scratch_page) {
        PVOID page = VirtualAlloc(
            NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!page ||
            (g_compat32_mode &&
             (ULONG_PTR)page + 0x1000 > UINT32_MAX)) {
            return NULL;
        }
        context->scratch_page = page;
    }
    return context->scratch_page;
}

static PVOID user_hook_payload_for_next_frame(SIZE_T size)
{
    if (size > USER_HOOK_SCRATCH_STRIDE)
        return NULL;

    USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
    if (!context || context->depth >= USER_HOOK_MAX_DEPTH)
        return NULL;
    PVOID page = user32_callback_scratch_page(context);
    if (!page)
        return NULL;

    return (BYTE *)page +
           context->depth * USER_HOOK_SCRATCH_STRIDE;
}

HHOOK WINAPI SetWindowsHookExA(int idHook, HOOKPROC lpfn,
                               HINSTANCE hMod, DWORD dwThreadId)
{
    if (!user_hook_type_valid(idHook)) {
        SetLastError(1426); /* ERROR_INVALID_HOOK_FILTER */
        return NULL;
    }
    if (!lpfn) {
        SetLastError(1427); /* ERROR_INVALID_FILTER_PROC */
        return NULL;
    }

    USER_HOOK *slot = NULL;
    uint64_t flags = user_hook_lock_irqsave();
    for (int i = 0; i < USER_HOOK_SLOTS; i++) {
        if (!user_hooks[i].used) {
            slot = &user_hooks[i];
            break;
        }
    }
    if (!slot) {
        user_hook_unlock_irqrestore(flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }

    HHOOK handle = (HHOOK)next_user_hook_handle++;
    if (!next_user_hook_handle)
        next_user_hook_handle = 0xD6000001;
    slot->handle = handle;
    slot->type = idHook;
    slot->callback = lpfn;
    slot->module = hMod;
    slot->owner_pid = GetCurrentProcessId();
    slot->installer_tid = GetCurrentThreadId();
    slot->target_tid = dwThreadId;
    slot->sequence = next_user_hook_sequence++;
    slot->compat32 = g_compat32_mode ? TRUE : FALSE;
    slot->used = TRUE;
    user_hook_unlock_irqrestore(flags);

    serial_puts("[USER32-HOOK] set type=");
    serial_putdec((uint64_t)(int64_t)idHook);
    serial_puts(" handle=0x");
    serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
    serial_puts(" pid=");
    serial_putdec(GetCurrentProcessId());
    serial_puts(" tid=");
    serial_putdec(GetCurrentThreadId());
    serial_puts(" target=");
    serial_putdec(dwThreadId);
    serial_puts(" proc=0x");
    serial_puthex((uint64_t)(ULONG_PTR)lpfn, g_compat32_mode ? 8 : 16);
    serial_puts("\n");
    return handle;
}

HHOOK WINAPI SetWindowsHookExW(int idHook, HOOKPROC lpfn,
                               HINSTANCE hMod, DWORD dwThreadId)
{
    return SetWindowsHookExA(idHook, lpfn, hMod, dwThreadId);
}

BOOL WINAPI UnhookWindowsHookEx(HHOOK hhk)
{
    DWORD pid = GetCurrentProcessId();
    BOOL removed = FALSE;
    uint64_t flags = user_hook_lock_irqsave();
    for (int i = 0; i < USER_HOOK_SLOTS; i++) {
        if (user_hooks[i].used && user_hooks[i].handle == hhk &&
            user_hooks[i].owner_pid == pid) {
            user_hooks[i].used = FALSE;
            removed = TRUE;
            break;
        }
    }
    user_hook_unlock_irqrestore(flags);
    if (!removed)
        SetLastError(1404); /* ERROR_INVALID_HOOK_HANDLE */
    return removed;
}

LRESULT WINAPI CallNextHookEx(HHOOK hhk, int nCode,
                              WPARAM wParam, LPARAM lParam)
{
    (void)hhk; /* The parameter is ignored by Windows as well. */
    return user_hook_call_next(user_hook_context_get(FALSE),
                               nCode, wParam, lParam);
}

static void user_hook_release_process(DWORD pid)
{
    uint64_t flags = user_hook_lock_irqsave();
    for (int i = 0; i < USER_HOOK_SLOTS; i++)
        if (user_hooks[i].used && user_hooks[i].owner_pid == pid)
            user_hooks[i].used = FALSE;
    for (int i = 0; i < USER_HOOK_CONTEXT_SLOTS; i++) {
        if (user_hook_contexts[i].used &&
            user_hook_contexts[i].pid == pid) {
            user_hook_contexts[i].used = FALSE;
            user_hook_contexts[i].depth = 0;
            user_hook_contexts[i].windowpos_depth = 0;
            user_hook_contexts[i].create_depth = 0;
            user_hook_contexts[i].scratch_page = NULL;
        }
    }
    user_hook_unlock_irqrestore(flags);
}

static void user_hook_release_thread(DWORD pid, DWORD tid)
{
    PVOID scratch_page = NULL;
    uint64_t flags = user_hook_lock_irqsave();
    for (int i = 0; i < USER_HOOK_SLOTS; i++) {
        USER_HOOK *hook = &user_hooks[i];
        if (hook->used &&
            ((hook->owner_pid == pid && hook->installer_tid == tid) ||
             hook->target_tid == tid))
            hook->used = FALSE;
    }
    for (int i = 0; i < USER_HOOK_CONTEXT_SLOTS; i++) {
        USER_HOOK_CONTEXT *context = &user_hook_contexts[i];
        if (!context->used || context->pid != pid || context->tid != tid)
            continue;
        scratch_page = context->scratch_page;
        context->used = FALSE;
        context->depth = 0;
        context->windowpos_depth = 0;
        context->create_depth = 0;
        context->scratch_page = NULL;
    }
    user_hook_unlock_irqrestore(flags);

    if (scratch_page)
        (void)nt_vm_release_allocation_for_process(pid, scratch_page);
}

typedef struct {
    uint32_t lParam;
    uint32_t wParam;
    uint32_t message;
    uint32_t hwnd;
} CWPSTRUCT32;

typedef struct {
    uint32_t lResult;
    uint32_t lParam;
    uint32_t wParam;
    uint32_t message;
    uint32_t hwnd;
} CWPRETSTRUCT32;

static void user_hook_notify_getmessage(void *message, BOOL remove)
{
    MSG before = {0};
    MSG after = {0};
    msg_read_from(message, &before);
    LRESULT result = user_hook_dispatch(
        WH_GETMESSAGE, HC_ACTION, remove ? PM_REMOVE : PM_NOREMOVE,
        (LPARAM)(ULONG_PTR)message);
    msg_read_from(message, &after);

    if (before.hwnd != after.hwnd || before.message != after.message ||
        before.wParam != after.wParam || before.lParam != after.lParam ||
        before.message == WM_QUIT || after.message == WM_QUIT) {
        serial_puts("[USER32-HOOK-MSG] before=0x");
        serial_puthex(before.message, 4);
        serial_puts("/0x");
        serial_puthex((uint64_t)(uint32_t)before.wParam, 8);
        serial_puts(" after=0x");
        serial_puthex(after.message, 4);
        serial_puts("/0x");
        serial_puthex((uint64_t)(uint32_t)after.wParam, 8);
        serial_puts(" result=0x");
        serial_puthex((uint64_t)(ULONG_PTR)result, 8);
        serial_puts("\n");
    }
}

static void user_hook_notify_callwndproc(HWND hwnd, UINT message,
                                         WPARAM wParam, LPARAM lParam)
{
    if (!user_hook_has_match(WH_CALLWNDPROC))
        return;

    if (g_compat32_mode) {
        CWPSTRUCT32 *payload = (CWPSTRUCT32 *)
            user_hook_payload_for_next_frame(sizeof(*payload));
        if (!payload)
            return;
        payload->lParam = (uint32_t)lParam;
        payload->wParam = (uint32_t)wParam;
        payload->message = message;
        payload->hwnd = (uint32_t)(ULONG_PTR)hwnd;
        user_hook_dispatch(WH_CALLWNDPROC, HC_ACTION, 0,
                           (LPARAM)(ULONG_PTR)payload);
    } else {
        CWPSTRUCT payload = { lParam, wParam, message, hwnd };
        user_hook_dispatch(WH_CALLWNDPROC, HC_ACTION, 0,
                           (LPARAM)(ULONG_PTR)&payload);
    }
}

static void user_hook_notify_callwndprocret(LRESULT result, HWND hwnd,
                                            UINT message, WPARAM wParam,
                                            LPARAM lParam)
{
    if (!user_hook_has_match(WH_CALLWNDPROCRET))
        return;

    if (g_compat32_mode) {
        CWPRETSTRUCT32 *payload = (CWPRETSTRUCT32 *)
            user_hook_payload_for_next_frame(sizeof(*payload));
        if (!payload)
            return;
        payload->lResult = (uint32_t)result;
        payload->lParam = (uint32_t)lParam;
        payload->wParam = (uint32_t)wParam;
        payload->message = message;
        payload->hwnd = (uint32_t)(ULONG_PTR)hwnd;
        user_hook_dispatch(WH_CALLWNDPROCRET, HC_ACTION, 0,
                           (LPARAM)(ULONG_PTR)payload);
    } else {
        CWPRETSTRUCT payload = { result, lParam, wParam, message, hwnd };
        user_hook_dispatch(WH_CALLWNDPROCRET, HC_ACTION, 0,
                           (LPARAM)(ULONG_PTR)&payload);
    }
}

static DWORD user_timer_queue_status(void);
static BOOL user_timer_next_timeout(HWND window, DWORD min_message,
                                    DWORD max_message, DWORD *timeout_ms);
static int user_timer_retrieve(void *out, HWND window, DWORD min_message,
                               DWORD max_message, BOOL remove, MSG *retrieved);
static ULONG_PTR user_timer_set(HWND window, ULONG_PTR event_id,
                                UINT elapsed, PVOID callback);
static BOOL user_timer_kill(HWND window, ULONG_PTR event_id);
static void user_timer_release_window(HWND window);
static void user_timer_release_thread(DWORD pid, DWORD tid);
static void user_timer_release_process(DWORD pid);
static int sent_message_dispatch_current(void);
static BOOL sent_message_pending(DWORD pid, DWORD tid);
static void sent_message_release_thread(DWORD pid, DWORD tid);
static void sent_message_release_process(DWORD pid);

static inline uint64_t msg_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&msg_queue_lock);
    return flags;
}

static inline void msg_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&msg_queue_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static inline uint64_t sent_message_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&sent_message_lock);
    return flags;
}

static inline void sent_message_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&sent_message_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static BOOL sent_message_begin(const WINDOW *window, DWORD message,
                               WPARAM wparam, LPARAM lparam,
                               BOOL unicode,
                               HANDLE completion_event, int *slot_out,
                               uint32_t *generation_out)
{
    int slot = -1;
    uint64_t flags = sent_message_lock_irqsave();
    for (int i = 0; i < SENT_MESSAGE_SLOTS; i++) {
        if (sent_messages[i].state == SENT_MESSAGE_FREE) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        SENT_MESSAGE *sent = &sent_messages[slot];
        uint32_t generation = ++sent_message_generation;
        if (!generation)
            generation = ++sent_message_generation;
        sent->generation = generation;
        sent->sequence = ++sent_message_sequence;
        sent->sender_pid = GetCurrentProcessId();
        sent->sender_tid = GetCurrentThreadId();
        sent->target_pid = window->owner_pid;
        sent->target_tid = window->owner_tid;
        sent->window = window->handle;
        sent->message = message;
        sent->wparam = wparam;
        sent->lparam = lparam;
        sent->unicode = unicode;
        sent->result = 0;
        sent->completion_event = completion_event;
        sent->state = SENT_MESSAGE_PENDING;
        *slot_out = slot;
        *generation_out = generation;
    }
    sent_message_unlock_irqrestore(flags);
    return slot >= 0;
}

static BOOL sent_message_pending(DWORD pid, DWORD tid)
{
    BOOL pending = FALSE;
    uint64_t flags = sent_message_lock_irqsave();
    for (int i = 0; i < SENT_MESSAGE_SLOTS; i++) {
        SENT_MESSAGE *sent = &sent_messages[i];
        if (sent->state == SENT_MESSAGE_PENDING &&
            sent->target_pid == pid && sent->target_tid == tid) {
            pending = TRUE;
            break;
        }
    }
    sent_message_unlock_irqrestore(flags);
    return pending;
}

static BOOL sent_message_finish_wait(int slot, uint32_t generation,
                                     BOOL abandon, LRESULT *result)
{
    BOOL completed = FALSE;
    uint64_t flags = sent_message_lock_irqsave();
    SENT_MESSAGE *sent = &sent_messages[slot];
    if (sent->generation == generation) {
        if (sent->state == SENT_MESSAGE_DONE) {
            *result = sent->result;
            sent->state = SENT_MESSAGE_FREE;
            completed = TRUE;
        } else if (abandon) {
            sent->state = sent->state == SENT_MESSAGE_PROCESSING
                        ? SENT_MESSAGE_ABANDONED : SENT_MESSAGE_FREE;
        }
    }
    sent_message_unlock_irqrestore(flags);
    return completed;
}

static void sent_message_release_process(DWORD pid)
{
    for (int i = 0; i < SENT_MESSAGE_SLOTS; i++) {
        HANDLE event = NULL;
        DWORD sender_pid = 0;
        uint64_t flags = sent_message_lock_irqsave();
        SENT_MESSAGE *sent = &sent_messages[i];
        if (sent->state != SENT_MESSAGE_FREE) {
            if (sent->sender_pid == pid) {
                sent->state = sent->state == SENT_MESSAGE_PROCESSING
                            ? SENT_MESSAGE_ABANDONED : SENT_MESSAGE_FREE;
            } else if (sent->target_pid == pid) {
                sent->result = 0;
                sent->state = SENT_MESSAGE_DONE;
                event = sent->completion_event;
                sender_pid = sent->sender_pid;
            }
        }
        sent_message_unlock_irqrestore(flags);
        if (event)
            ntsync_set_event_for_process(event, sender_pid, NULL);
    }
}

static void sent_message_release_thread(DWORD pid, DWORD tid)
{
    for (int i = 0; i < SENT_MESSAGE_SLOTS; i++) {
        HANDLE event_to_close = NULL;
        HANDLE event_to_signal = NULL;
        DWORD sender_pid = 0;
        uint64_t flags = sent_message_lock_irqsave();
        SENT_MESSAGE *sent = &sent_messages[i];
        if (sent->state != SENT_MESSAGE_FREE) {
            if (sent->sender_pid == pid && sent->sender_tid == tid) {
                event_to_close = sent->completion_event;
                sent->state = sent->state == SENT_MESSAGE_PROCESSING
                            ? SENT_MESSAGE_ABANDONED : SENT_MESSAGE_FREE;
            } else if (sent->target_pid == pid && sent->target_tid == tid) {
                sent->result = 0;
                sent->state = SENT_MESSAGE_DONE;
                event_to_signal = sent->completion_event;
                sender_pid = sent->sender_pid;
            }
        }
        sent_message_unlock_irqrestore(flags);
        if (event_to_signal)
            ntsync_set_event_for_process(event_to_signal, sender_pid, NULL);
        if (event_to_close)
            (void)nt_close_handle_for_process(event_to_close, pid);
    }
}

static INPUT_SEQUENCE_STATE *msg_input_state_locked(DWORD pid, DWORD tid,
                                                    BOOL create)
{
    int free_slot = -1;
    for (int i = 0; i < INPUT_SEQUENCE_STATE_SLOTS; i++) {
        INPUT_SEQUENCE_STATE *state = &input_sequence_states[i];
        if (state->used && state->pid == pid && state->tid == tid)
            return state;
        if (!state->used && free_slot < 0)
            free_slot = i;
    }

    if (!create || free_slot < 0)
        return NULL;

    INPUT_SEQUENCE_STATE *state = &input_sequence_states[free_slot];
    state->used = TRUE;
    state->pid = pid;
    state->tid = tid;
    state->sequence = 0;
    state->last_source = MSG_SOURCE_POSTED;
    return state;
}

static uint64_t msg_last_input_sequence_locked(DWORD pid, DWORD tid)
{
    INPUT_SEQUENCE_STATE *state = msg_input_state_locked(pid, tid, FALSE);
    return state ? state->sequence : 0;
}

static void msg_note_retrieval_locked(DWORD pid, DWORD tid, BYTE source,
                                      uint64_t sequence)
{
    INPUT_SEQUENCE_STATE *state = msg_input_state_locked(pid, tid, TRUE);
    if (!state)
        return;
    state->last_source = source;
    if (source == MSG_SOURCE_INPUT || sequence)
        state->sequence = sequence;
}

static void msg_note_retrieval(DWORD pid, DWORD tid, BYTE source,
                               uint64_t sequence)
{
    uint64_t flags = msg_lock_irqsave();
    msg_note_retrieval_locked(pid, tid, source, sequence);
    msg_unlock_irqrestore(flags);
}

static BYTE msg_last_retrieved_source(DWORD pid, DWORD tid)
{
    uint64_t flags = msg_lock_irqsave();
    INPUT_SEQUENCE_STATE *state = msg_input_state_locked(pid, tid, FALSE);
    BYTE source = state ? state->last_source : MSG_SOURCE_POSTED;
    msg_unlock_irqrestore(flags);
    return source;
}

static uint64_t msg_last_input_sequence(DWORD pid, DWORD tid)
{
    uint64_t flags = msg_lock_irqsave();
    uint64_t sequence = msg_last_input_sequence_locked(pid, tid);
    msg_unlock_irqrestore(flags);
    return sequence;
}

static inline uint64_t msg_wait_event_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&msg_wait_event_lock);
    return flags;
}

static inline void msg_wait_event_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&msg_wait_event_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static HANDLE msg_wait_event_for_current_thread(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    int free_slot = -1;
    uint64_t irq_flags = msg_wait_event_lock_irqsave();

    for (int i = 0; i < MSG_WAIT_EVENT_SLOTS; i++) {
        MSG_WAIT_EVENT *entry = &msg_wait_events[i];
        if (entry->used && entry->pid == pid && entry->tid == tid) {
            HANDLE event = entry->event;
            msg_wait_event_unlock_irqrestore(irq_flags);
            return event;
        }
        if (!entry->used && free_slot < 0)
            free_slot = i;
    }
    msg_wait_event_unlock_irqrestore(irq_flags);

    if (free_slot < 0)
        return NULL;

    HANDLE created = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!created)
        return NULL;

    HANDLE result = created;
    BOOL keep_created = TRUE;
    irq_flags = msg_wait_event_lock_irqsave();
    free_slot = -1;
    for (int i = 0; i < MSG_WAIT_EVENT_SLOTS; i++) {
        MSG_WAIT_EVENT *entry = &msg_wait_events[i];
        if (entry->used && entry->pid == pid && entry->tid == tid) {
            result = entry->event;
            keep_created = FALSE;
            break;
        }
        if (!entry->used && free_slot < 0)
            free_slot = i;
    }
    if (keep_created && free_slot >= 0) {
        MSG_WAIT_EVENT *entry = &msg_wait_events[free_slot];
        entry->pid = pid;
        entry->tid = tid;
        entry->event = created;
        entry->used = TRUE;
    } else if (keep_created) {
        result = NULL;
        keep_created = FALSE;
    }
    msg_wait_event_unlock_irqrestore(irq_flags);

    if (!keep_created)
        CloseHandle(created);
    return result;
}

static void msg_wait_event_signal(DWORD target_pid, DWORD target_tid)
{
    HANDLE event = NULL;
    DWORD owner_pid = 0;
    uint64_t irq_flags = msg_wait_event_lock_irqsave();

    for (int i = 0; i < MSG_WAIT_EVENT_SLOTS; i++) {
        MSG_WAIT_EVENT *entry = &msg_wait_events[i];
        if (!entry->used || entry->tid != target_tid ||
            (target_pid && entry->pid != target_pid))
            continue;
        event = entry->event;
        owner_pid = entry->pid;
        break;
    }
    msg_wait_event_unlock_irqrestore(irq_flags);

    if (event)
        ntsync_set_event_for_process(event, owner_pid, NULL);
}

static void msg_wait_event_release_process(DWORD pid)
{
    uint64_t irq_flags = msg_wait_event_lock_irqsave();
    for (int i = 0; i < MSG_WAIT_EVENT_SLOTS; i++) {
        if (msg_wait_events[i].used && msg_wait_events[i].pid == pid)
            msg_wait_events[i].used = FALSE;
    }
    msg_wait_event_unlock_irqrestore(irq_flags);
}

static void msg_wait_event_release_thread(DWORD pid, DWORD tid)
{
    HANDLE event = NULL;
    uint64_t irq_flags = msg_wait_event_lock_irqsave();
    for (int i = 0; i < MSG_WAIT_EVENT_SLOTS; i++) {
        MSG_WAIT_EVENT *entry = &msg_wait_events[i];
        if (entry->used && entry->pid == pid && entry->tid == tid) {
            event = entry->event;
            *entry = (MSG_WAIT_EVENT){0};
            break;
        }
    }
    msg_wait_event_unlock_irqrestore(irq_flags);

    if (event)
        (void)nt_close_handle_for_process(event, pid);
}

#define QS_KEY             0x0001
#define QS_MOUSEMOVE       0x0002
#define QS_MOUSEBUTTON     0x0004
#define QS_POSTMESSAGE     0x0008
#define QS_TIMER           0x0010
#define QS_PAINT           0x0020
#define QS_SENDMESSAGE     0x0040
#define QS_HOTKEY          0x0080
#define QS_ALLPOSTMESSAGE  0x0100
#define QS_RAWINPUT        0x0400
#define QS_ALLINPUT        (QS_KEY | QS_MOUSEMOVE | QS_MOUSEBUTTON | \
                            QS_POSTMESSAGE | QS_TIMER | QS_PAINT | \
                            QS_SENDMESSAGE | QS_HOTKEY | \
                            QS_ALLPOSTMESSAGE | QS_RAWINPUT)

static DWORD message_queue_status(DWORD message)
{
    if (message >= 0x0100 && message <= 0x0108)
        return QS_KEY;
    if (message == WM_MOUSEMOVE)
        return QS_MOUSEMOVE;
    if (message >= 0x0201 && message <= 0x020E)
        return QS_MOUSEBUTTON;
    if (message == WM_TIMER)
        return QS_TIMER;
    if (message == WM_PAINT || message == WM_NCPAINT)
        return QS_PAINT;
    if (message == 0x0312) /* WM_HOTKEY */
        return QS_HOTKEY;
    if (message == 0x00FF) /* WM_INPUT */
        return QS_RAWINPUT;
    return QS_POSTMESSAGE | QS_ALLPOSTMESSAGE;
}

static DWORD current_queue_status(void)
{
    DWORD status = 0;
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    int tail = __atomic_load_n(&msg_tail, __ATOMIC_ACQUIRE);
    for (int i = msg_head; i != tail; i = (i + 1) % MSG_QUEUE_SIZE) {
        DWORD target_tid = __atomic_load_n(&msg_target_tid[i], __ATOMIC_RELAXED);
        DWORD target_pid = __atomic_load_n(&msg_target_pid[i], __ATOMIC_RELAXED);
        if (target_tid == tid && (!target_pid || target_pid == pid))
            status |= message_queue_status(
                __atomic_load_n(&msg_queue[i].message, __ATOMIC_RELAXED));
    }
    for (int i = 0; i < window_count; i++) {
        WINDOW *w = &windows[i];
        if (w->used && w->visible && w->paint_pending &&
            w->owner_pid == pid && w->owner_tid == tid)
            status |= QS_PAINT;
    }
    if (sent_message_pending(pid, tid))
        status |= QS_SENDMESSAGE;
    status |= user_timer_queue_status();
    return status;
}

static DWORD WINAPI GetQueueStatus_u32(UINT flags)
{
    DWORD current = current_queue_status() & flags;
    DWORD old_changed = __sync_fetch_and_and(&queue_changed_status, ~flags);
    DWORD changed = old_changed & flags;
    return (current << 16) | changed;
}

static WINDOW *paint_pending_window(HWND filter);
static int msg_queue_has_match(HWND hwnd, DWORD min_message, DWORD max_message);

static DWORD WINAPI MsgWaitForMultipleObjectsEx_u32(DWORD count,
                                                     const HANDLE *handles,
                                                     DWORD timeout_ms,
                                                     DWORD wake_mask,
                                                     DWORD flags)
{
    const DWORD wait_timeout = 0x00000102;
    const DWORD wait_failed = 0xFFFFFFFF;

    if (count > 63 || (count && !handles)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return wait_failed;
    }

    HANDLE queue_event = msg_wait_event_for_current_thread();
    DWORD start = shim_timeGetTime();
    for (;;) {
        extern void win32_main_termination_checkpoint(void);
        win32_main_termination_checkpoint();
        if (sent_message_dispatch_current() &&
            (wake_mask & QS_SENDMESSAGE))
            return count;
        if (wake_mask && (current_queue_status() & wake_mask))
            return count; /* WAIT_OBJECT_0 + message queue index */

        DWORD elapsed = (DWORD)(shim_timeGetTime() - start);
        if (!timeout_ms ||
            (timeout_ms != 0xFFFFFFFF && elapsed >= timeout_ms))
            return wait_timeout;

        DWORD wait_ms = timeout_ms == 0xFFFFFFFF
                      ? 0xFFFFFFFF : timeout_ms - elapsed;
        if (wake_mask & QS_TIMER) {
            DWORD timer_ms;
            if (user_timer_next_timeout(NULL, 0, 0, &timer_ms) &&
                (wait_ms == 0xFFFFFFFF || timer_ms < wait_ms))
                wait_ms = timer_ms;
        }
        if (!wait_ms)
            continue;

        BOOL include_queue = queue_event && wake_mask;
        DWORD wait_count = count + (include_queue ? 1U : 0U);
        if (!wait_count) {
            extern void sched_yield(void);
            sched_yield();
            continue;
        }

        DWORD result;
        if (g_compat32_mode) {
            uint32_t wait_handles[64];
            const uint32_t *source = (const uint32_t *)(const void *)handles;
            for (DWORD i = 0; i < count; i++)
                wait_handles[i] = source[i];
            if (include_queue)
                wait_handles[count] = (uint32_t)(ULONG_PTR)queue_event;
            result = WaitForMultipleObjects(
                wait_count, (const HANDLE *)(const void *)wait_handles,
                (flags & 1) != 0, wait_ms);
        } else {
            HANDLE wait_handles[64];
            for (DWORD i = 0; i < count; i++)
                wait_handles[i] = handles[i];
            if (include_queue)
                wait_handles[count] = queue_event;
            result = WaitForMultipleObjects(wait_count, wait_handles,
                                            (flags & 1) != 0, wait_ms);
        }

        if (result == wait_failed)
            return result;
        if (result == wait_timeout)
            continue;
        if ((flags & 1) == 0 && result < count)
            return result;
        /* The queue event is only a wakeup edge. Recheck the queue so a
         * coalesced or stale edge never appears as a real input message. */
    }
}

static DWORD WINAPI MsgWaitForMultipleObjects_u32(DWORD count,
                                                   const HANDLE *handles,
                                                   BOOL wait_all,
                                                   DWORD timeout_ms,
                                                   DWORD wake_mask)
{
    return MsgWaitForMultipleObjectsEx_u32(count, handles, timeout_ms,
                                           wake_mask, wait_all ? 1U : 0U);
}

static BOOL WINAPI WaitMessage_u32(void)
{
    return MsgWaitForMultipleObjectsEx_u32(0, NULL, 0xFFFFFFFF,
                                           QS_ALLINPUT, 0) == 0;
}

static BOOL WINAPI ShutdownBlockReasonCreate_u32(HWND window,
                                                  PCWSTR reason)
{
    WINDOW *target = find_window(window);
    if (!target) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }
    if (target->owner_pid != GetCurrentProcessId()) {
        SetLastError(5); /* ERROR_ACCESS_DENIED */
        return FALSE;
    }
    if (!reason) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD length = 0;
    while (reason[length] && length <= 256) length++;
    if (!length || length > 256) {
        SetLastError(87);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI ShutdownBlockReasonDestroy_u32(HWND window)
{
    WINDOW *target = find_window(window);
    if (!target) {
        SetLastError(1400);
        return FALSE;
    }
    if (target->owner_pid != GetCurrentProcessId()) {
        SetLastError(5);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

/* Millisecond time base for message timestamps. Shares winmm's rdtsc-based
 * clock (the APIC tick doesn't advance while a compat32 process runs, so
 * idt_get_ticks would freeze). NT stamps MSG.time when the message is POSTED,
 * and GetMessageTime() returns the stamp of the last retrieved message —
 * WinDrv stores it per mouse-button event (windrv.bin @0x11108457/0x1110847C/
 * 0x111084A1) for click/double-click discrimination, so a constant 0 corrupts
 * that logic. */
static DWORD g_last_msg_time = 0;   /* MSG.time of last dequeued message */
static DWORD g_last_input_time = 0;
static POINT g_last_msg_pos = { 0, 0 };

static BOOL message_filter_allows(DWORD message, DWORD min_message,
                                  DWORD max_message)
{
    return (!min_message && !max_message) ||
           (message >= min_message && message <= max_message);
}

static BOOL message_window_matches_filter(HWND message_window,
                                          HWND filter_window)
{
    if (filter_window == (HWND)(ULONG_PTR)-1)
        return message_window == NULL;
    if (!filter_window)
        return TRUE;
    return message_window == filter_window ||
           (message_window && IsChild(filter_window, message_window));
}

static int msg_matches_locked(int index, DWORD pid, DWORD tid, HWND hwnd,
                              DWORD min_message, DWORD max_message)
{
    MSG *msg = &msg_queue[index];

    if (msg_target_tid[index] != tid)
        return 0;
    if (msg_target_pid[index] && msg_target_pid[index] != pid)
        return 0;
    if (msg->message == WM_QUIT)
        return 1;
    if (!message_window_matches_filter(msg->hwnd, hwnd))
        return 0;
    if (!message_filter_allows(msg->message, min_message, max_message))
        return 0;
    return 1;
}

static int msg_find_locked(DWORD pid, DWORD tid, HWND hwnd,
                           DWORD min_message, DWORD max_message)
{
    for (int i = msg_head; i != msg_tail; i = (i + 1) % MSG_QUEUE_SIZE) {
        if (msg_matches_locked(i, pid, tid, hwnd, min_message, max_message))
            return i;
    }
    return -1;
}

static void msg_remove_locked(int index)
{
    int last = (msg_tail + MSG_QUEUE_SIZE - 1) % MSG_QUEUE_SIZE;
    while (index != last) {
        int next = (index + 1) % MSG_QUEUE_SIZE;
        msg_queue[index] = msg_queue[next];
        msg_target_pid[index] = msg_target_pid[next];
        msg_target_tid[index] = msg_target_tid[next];
        msg_extra_info[index] = msg_extra_info[next];
        msg_source[index] = msg_source[next];
        msg_input_sequence[index] = msg_input_sequence[next];
        index = next;
    }
    __atomic_store_n(&msg_tail, last, __ATOMIC_RELEASE);
}

static BOOL msg_enqueue_locked(HWND hwnd, DWORD message, WPARAM wp, LPARAM lp,
                               DWORD target_pid, DWORD target_tid,
                               LPARAM extra_info, DWORD timestamp,
                               LONG point_x, LONG point_y, BOOL coalesce,
                               BYTE source)
{
    if (coalesce && message == WM_MOUSEMOVE && msg_tail != msg_head) {
        int last = (msg_tail + MSG_QUEUE_SIZE - 1) % MSG_QUEUE_SIZE;
        if (msg_queue[last].message == WM_MOUSEMOVE &&
            msg_queue[last].hwnd == hwnd &&
            msg_target_pid[last] == target_pid &&
            msg_target_tid[last] == target_tid &&
            msg_source[last] == source) {
            msg_queue[last].wParam = wp;
            msg_queue[last].lParam = lp;
            msg_queue[last].time = timestamp;
            msg_queue[last].pt.x = point_x;
            msg_queue[last].pt.y = point_y;
            msg_extra_info[last] = extra_info;
            if (source == MSG_SOURCE_INPUT)
                msg_input_sequence[last] = ++next_input_sequence;
            return TRUE;
        }
    }

    int next = (msg_tail + 1) % MSG_QUEUE_SIZE;
    if (next == msg_head)
        return FALSE;

    msg_queue[msg_tail].hwnd    = hwnd;
    msg_queue[msg_tail].message = message;
    msg_queue[msg_tail].wParam  = wp;
    msg_queue[msg_tail].lParam  = lp;
    msg_queue[msg_tail].time    = timestamp;
    msg_queue[msg_tail].pt.x    = point_x;
    msg_queue[msg_tail].pt.y    = point_y;
    msg_target_pid[msg_tail] = target_pid;
    msg_target_tid[msg_tail] = target_tid;
    msg_extra_info[msg_tail] = extra_info;
    msg_source[msg_tail] = source;
    msg_input_sequence[msg_tail] = source == MSG_SOURCE_INPUT
        ? ++next_input_sequence : 0;
    __atomic_store_n(&msg_tail, next, __ATOMIC_RELEASE);
    return TRUE;
}

static BOOL msg_enqueue_target_info(HWND hwnd, DWORD message,
                                    WPARAM wp, LPARAM lp,
                                    DWORD target_pid, DWORD target_tid,
                                    LPARAM extra_info, DWORD timestamp,
                                    POINT point, BOOL coalesce, BYTE source)
{
    DWORD stamp = timestamp ? timestamp : shim_timeGetTime();
    uint64_t flags = msg_lock_irqsave();
    BOOL queued = msg_enqueue_locked(hwnd, message, wp, lp,
                                     target_pid, target_tid, extra_info,
                                     stamp, point.x, point.y, coalesce,
                                     source);
    msg_unlock_irqrestore(flags);

    if (!queued) {
        SetLastError(1816); /* ERROR_NOT_ENOUGH_QUOTA */
        return FALSE;
    }
    if (message == WM_QUIT) {
        serial_puts("[USER32-QUIT] enqueue pid=");
        serial_putdec(target_pid);
        serial_puts(" tid=");
        serial_putdec(target_tid);
        serial_puts(" code=");
        serial_putdec((uint64_t)(uint32_t)wp);
        serial_puts(" current_tid=");
        serial_putdec(GetCurrentThreadId());
        serial_puts(" caller=0x");
        serial_puthex(compat32_get_last_caller_eip(), 8);
        serial_puts("\n");
    }
    __sync_fetch_and_or(&queue_changed_status, message_queue_status(message));
    msg_wait_event_signal(target_pid, target_tid);
    return TRUE;
}

static BOOL msg_enqueue_target(HWND hwnd, DWORD message, WPARAM wp, LPARAM lp,
                               DWORD target_pid, DWORD target_tid)
{
    return msg_enqueue_target_info(hwnd, message, wp, lp,
                                   target_pid, target_tid,
                                   message_extra_info, 0, cursor_pos, TRUE,
                                   MSG_SOURCE_POSTED);
}

static BOOL msg_enqueue_source(HWND hwnd, DWORD message, WPARAM wp, LPARAM lp,
                               BYTE source)
{
    DWORD target_pid = GetCurrentProcessId();
    DWORD target_tid = GetCurrentThreadId();
    WINDOW *window = hwnd ? find_window(hwnd) : NULL;

    if (window) {
        target_pid = window->owner_pid;
        target_tid = window->owner_tid;
    } else if (hwnd && hwnd != (HWND)(ULONG_PTR)0xFFFF) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }
    if (source == MSG_SOURCE_POSTED)
        return msg_enqueue_target(hwnd, message, wp, lp,
                                  target_pid, target_tid);
    return msg_enqueue_target_info(hwnd, message, wp, lp,
                                   target_pid, target_tid,
                                   message_extra_info, 0, cursor_pos, TRUE,
                                   source);
}

static BOOL msg_enqueue(HWND hwnd, DWORD message, WPARAM wp, LPARAM lp)
{
    return msg_enqueue_source(hwnd, message, wp, lp, MSG_SOURCE_POSTED);
}

static BOOL msg_enqueue_input(HWND hwnd, DWORD message, WPARAM wp, LPARAM lp)
{
    return msg_enqueue_source(hwnd, message, wp, lp, MSG_SOURCE_INPUT);
}

/*
 * Write MSG to caller buffer using 32-bit or 64-bit layout.
 *
 * 64-bit MSG is 48 bytes (HWND=8, WPARAM=8, LPARAM=8).
 * 32-bit MSG is 28 bytes (HWND=4, WPARAM=4, LPARAM=4).
 * Writing 48 bytes to a 28-byte buffer overflows 20 bytes.
 *
 * 32-bit MSG layout:
 *   +0:  HWND(4) +4: message(4) +8: wParam(4) +12: lParam(4)
 *   +16: time(4) +20: pt.x(4) +24: pt.y(4)
 */
/*
 * Read MSG from a caller-provided buffer (may be 32-bit or 64-bit layout).
 * Extracts fields into local 64-bit MSG for internal use.
 */
static void msg_read_from(const void *src, MSG *out)
{
    if (g_compat32_mode) {
        const uint32_t *p = (const uint32_t *)src;
        out->hwnd    = (HWND)(ULONG_PTR)p[0];
        out->message = p[1];
        out->wParam  = (WPARAM)p[2];
        out->lParam  = (LPARAM)(int32_t)p[3];
        out->time    = p[4];
        out->pt.x    = (LONG)p[5];
        out->pt.y    = (LONG)p[6];
    } else {
        *out = *(const MSG *)src;
    }
}

static void msg_write_to(void *dest, HWND hwnd, DWORD message,
                          WPARAM wp, LPARAM lp, DWORD time,
                          LONG ptx, LONG pty)
{
    if (g_compat32_mode) {
        uint32_t *p = (uint32_t *)dest;
        p[0] = (uint32_t)(ULONG_PTR)hwnd;  /* HWND truncated to 32-bit */
        p[1] = message;
        p[2] = (uint32_t)wp;
        p[3] = (uint32_t)lp;
        p[4] = time;
        p[5] = (uint32_t)ptx;
        p[6] = (uint32_t)pty;
    } else {
        MSG *m = (MSG *)dest;
        m->hwnd    = hwnd;
        m->message = message;
        m->wParam  = wp;
        m->lParam  = lp;
        m->time    = time;
        m->pt.x    = ptx;
        m->pt.y    = pty;
    }
}

static int msg_retrieve(void *out, HWND hwnd, DWORD min_message,
                        DWORD max_message, BOOL remove, MSG *retrieved)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    MSG msg;
    LPARAM extra_info;
    BYTE source;
    uint64_t input_sequence;
    uint64_t flags = msg_lock_irqsave();
    int index = msg_find_locked(pid, tid, hwnd, min_message, max_message);
    if (index < 0) {
        msg_unlock_irqrestore(flags);
        return 0;
    }
    msg = msg_queue[index];
    if (msg.message == WM_QUIT) {
        serial_puts("[USER32-QUIT] dequeue slot=");
        serial_putdec(index);
        serial_puts(" pid=");
        serial_putdec(pid);
        serial_puts(" tid=");
        serial_putdec(tid);
        serial_puts(" target_pid=");
        serial_putdec(msg_target_pid[index]);
        serial_puts(" target_tid=");
        serial_putdec(msg_target_tid[index]);
        serial_puts(" code=");
        serial_putdec((uint64_t)(uint32_t)msg.wParam);
        serial_puts("\n");
    }
    extra_info = msg_extra_info[index];
    source = msg_source[index];
    input_sequence = msg_input_sequence[index];
    msg_note_retrieval_locked(pid, tid, source, input_sequence);
    if (remove)
        msg_remove_locked(index);
    msg_unlock_irqrestore(flags);

    msg_write_to(out, msg.hwnd, msg.message, msg.wParam, msg.lParam,
                 msg.time, msg.pt.x, msg.pt.y);
    if (retrieved)
        *retrieved = msg;
    g_last_msg_time = msg.time;
    g_last_msg_pos = msg.pt;
    message_extra_info = extra_info;
    for (int i = 0; i < 256; i++) key_state_at_msg[i] = key_state[i];

    if (msg.message >= WM_KEYDOWN && msg.message <= WM_SYSKEYUP) {
        static unsigned trace_count;
        if (trace_count++ < 96) {
            serial_puts("[KEY-MSG] retrieve pid=");
            serial_putdec(pid);
            serial_puts(" tid=");
            serial_putdec(tid);
            serial_puts(" hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)msg.hwnd, 8);
            serial_puts(" msg=0x");
            serial_puthex(msg.message, 4);
            serial_puts(" wp=0x");
            serial_puthex((uint64_t)msg.wParam, 8);
            serial_puts(" remove=");
            serial_putdec(remove ? 1 : 0);
            serial_puts("\n");
        }
    }
    return 1;
}

static int msg_queue_has_match(HWND hwnd, DWORD min_message, DWORD max_message)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    int tail = __atomic_load_n(&msg_tail, __ATOMIC_ACQUIRE);

    /* This is only a wakeup hint. Retrieval rechecks under the queue lock, so
     * a concurrent insert/remove may cause at most one harmless extra yield. */
    for (int i = msg_head; i != tail; i = (i + 1) % MSG_QUEUE_SIZE) {
        DWORD target_tid = __atomic_load_n(&msg_target_tid[i], __ATOMIC_RELAXED);
        DWORD target_pid = __atomic_load_n(&msg_target_pid[i], __ATOMIC_RELAXED);
        if (target_tid != tid || (target_pid && target_pid != pid))
            continue;

        MSG snapshot = msg_queue[i];
        if (snapshot.message == WM_QUIT)
            return 1;
        if (!message_window_matches_filter(snapshot.hwnd, hwnd))
            continue;
        if (!message_filter_allows(snapshot.message,
                                   min_message, max_message))
            continue;
        return 1;
    }
    return 0;
}

static void msg_purge_process(DWORD pid)
{
    uint64_t flags = msg_lock_irqsave();
    int i = msg_head;
    while (i != msg_tail) {
        if (msg_target_pid[i] == pid)
            msg_remove_locked(i);
        else
            i = (i + 1) % MSG_QUEUE_SIZE;
    }
    for (int i = 0; i < INPUT_SEQUENCE_STATE_SLOTS; i++) {
        if (input_sequence_states[i].used &&
            input_sequence_states[i].pid == pid)
            input_sequence_states[i].used = FALSE;
    }
    msg_unlock_irqrestore(flags);
}

static void msg_purge_thread(DWORD pid, DWORD tid)
{
    uint64_t flags = msg_lock_irqsave();
    int i = msg_head;
    while (i != msg_tail) {
        if (msg_target_pid[i] == pid && msg_target_tid[i] == tid)
            msg_remove_locked(i);
        else
            i = (i + 1) % MSG_QUEUE_SIZE;
    }
    for (int i = 0; i < INPUT_SEQUENCE_STATE_SLOTS; i++) {
        INPUT_SEQUENCE_STATE *state = &input_sequence_states[i];
        if (state->used && state->pid == pid && state->tid == tid)
            state->used = FALSE;
    }
    msg_unlock_irqrestore(flags);
}

static BOOL capture_routes_mouse_message(DWORD message)
{
    return message >= WM_MOUSEMOVE && message <= WM_MOUSEHWHEEL;
}

static BOOL mouse_message_uses_client_point(DWORD message)
{
    return capture_routes_mouse_message(message) &&
           message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL;
}

/* Hardware input is collected independently of a PE thread's message pump.
 * A fast click can therefore enqueue both edges before WM_LBUTTONDOWN runs.
 * Windows translates the next input edge after the handler has had a chance
 * to call SetCapture; retarget still-pending input here to preserve that
 * ordering contract. Posted application messages are deliberately excluded. */
static void msg_capture_pending_input(WINDOW *capture)
{
    int origin_x = 0;
    int origin_y = 0;
    int retargeted = 0;
    DWORD current_pid = GetCurrentProcessId();
    DWORD current_tid = GetCurrentThreadId();

    if (!capture)
        return;
    window_screen_origin(capture, &origin_x, &origin_y);

    uint64_t flags = msg_lock_irqsave();
    uint64_t retrieved_sequence =
        msg_last_input_sequence_locked(current_pid, current_tid);
    for (int i = msg_head; i != msg_tail; i = (i + 1) % MSG_QUEUE_SIZE) {
        MSG *message = &msg_queue[i];
        if (msg_source[i] != MSG_SOURCE_INPUT ||
            msg_input_sequence[i] <= retrieved_sequence ||
            !capture_routes_mouse_message(message->message))
            continue;

        message->hwnd = capture->handle;
        msg_target_pid[i] = capture->owner_pid;
        msg_target_tid[i] = capture->owner_tid;
        if (mouse_message_uses_client_point(message->message)) {
            LONG x = message->pt.x - origin_x;
            LONG y = message->pt.y - origin_y;
            message->lParam = (LPARAM)(WORD)x |
                              ((LPARAM)(WORD)y << 16);
        }
        retargeted++;
    }
    msg_unlock_irqrestore(flags);

    if (retargeted) {
        msg_wait_event_signal(capture->owner_pid, capture->owner_tid);
        static unsigned trace_count;
        if (trace_count++ < 32) {
            serial_puts("[CAP-QUEUE] hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)capture->handle, 8);
            serial_puts(" retargeted=");
            serial_putdec((uint64_t)retargeted);
            serial_puts("\n");
        }
    }
}

static void invalidate_window(WINDOW *w, const RECT *rect, BOOL erase)
{
    RECT dirty;

    if (!w) return;
    if (rect) {
        dirty = *rect;
        if (dirty.left < 0) dirty.left = 0;
        if (dirty.top < 0) dirty.top = 0;
        if (dirty.right > w->width) dirty.right = w->width;
        if (dirty.bottom > w->height) dirty.bottom = w->height;
    } else {
        dirty.left = 0;
        dirty.top = 0;
        dirty.right = w->width;
        dirty.bottom = w->height;
    }
    if (dirty.left >= dirty.right || dirty.top >= dirty.bottom) return;

    if (!w->paint_pending) {
        w->update_rect = dirty;
    } else {
        if (dirty.left < w->update_rect.left) w->update_rect.left = dirty.left;
        if (dirty.top < w->update_rect.top) w->update_rect.top = dirty.top;
        if (dirty.right > w->update_rect.right) w->update_rect.right = dirty.right;
        if (dirty.bottom > w->update_rect.bottom)
            w->update_rect.bottom = dirty.bottom;
    }
    w->paint_pending = 1;
    if (erase) w->erase_pending = 1;
    __sync_fetch_and_or(&queue_changed_status, QS_PAINT);
    msg_wait_event_signal(w->owner_pid, w->owner_tid);
}

static WINDOW *paint_pending_window(HWND filter)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    for (int i = 0; i < window_count; i++) {
        WINDOW *w = &windows[i];
        if (!w->used || !w->visible || !w->paint_pending || !w->wndproc)
            continue;
        if (w->owner_pid != pid || w->owner_tid != tid)
            continue;
        if (!message_window_matches_filter(w->handle, filter))
            continue;
        return w;
    }
    return NULL;
}

static int write_pending_paint(void *out, HWND filter,
                               DWORD min_message, DWORD max_message)
{
    if (!message_filter_allows(WM_PAINT, min_message, max_message))
        return 0;
    WINDOW *w = paint_pending_window(filter);
    if (!w) return 0;
    DWORD time = shim_timeGetTime();
    msg_write_to(out, w->handle, WM_PAINT, 0, 0,
                 time, 0, 0);
    g_last_msg_time = time;
    g_last_msg_pos.x = 0;
    g_last_msg_pos.y = 0;
    message_extra_info = 0;
    msg_note_retrieval(GetCurrentProcessId(), GetCurrentThreadId(),
                       MSG_SOURCE_POSTED, 0);
    for (int i = 0; i < 256; i++)
        key_state_at_msg[i] = key_state[i];
    return 1;
}

static inline uint64_t user_timer_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&user_timer_lock);
    return flags;
}

static inline void user_timer_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&user_timer_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static BOOL user_timer_time_reached(DWORD now, DWORD due_time)
{
    return (LONG)(now - due_time) >= 0;
}

static void user_timer_refresh_locked(USER_TIMER *timer, DWORD now)
{
    if (!timer->used || !user_timer_time_reached(now, timer->due_time))
        return;

    if (!timer->pending) {
        timer->pending = TRUE;
        timer->message_time = now;
        timer->pending_callback = timer->callback;
        __sync_fetch_and_or(&queue_changed_status, QS_TIMER);
    }

    DWORD elapsed = now - timer->due_time;
    uint64_t periods = (uint64_t)elapsed / timer->interval_ms + 1;
    timer->due_time += (DWORD)(periods * timer->interval_ms);
}

static BOOL user_timer_matches_locked(const USER_TIMER *timer,
                                      DWORD pid, DWORD tid, HWND window,
                                      DWORD min_message, DWORD max_message)
{
    return (timer->used || (timer->posted && timer->pending)) &&
           timer->owner_pid == pid && timer->owner_tid == tid &&
           message_filter_allows(WM_TIMER, min_message, max_message) &&
           message_window_matches_filter(timer->window, window);
}

static DWORD user_timer_queue_status(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    DWORD now = shim_timeGetTime();
    DWORD status = 0;
    uint64_t flags = user_timer_lock_irqsave();

    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        if ((!timer->used && !(timer->posted && timer->pending)) ||
            timer->owner_pid != pid || timer->owner_tid != tid)
            continue;
        user_timer_refresh_locked(timer, now);
        if (timer->pending)
            status |= QS_TIMER;
    }

    user_timer_unlock_irqrestore(flags);
    return status;
}

static BOOL user_timer_next_timeout(HWND window, DWORD min_message,
                                    DWORD max_message, DWORD *timeout_ms)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    DWORD now = shim_timeGetTime();
    DWORD nearest = 0xFFFFFFFFU;
    BOOL found = FALSE;
    uint64_t flags = user_timer_lock_irqsave();

    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        if (!user_timer_matches_locked(timer, pid, tid, window,
                                       min_message, max_message))
            continue;
        found = TRUE;
        user_timer_refresh_locked(timer, now);
        DWORD remaining = timer->pending ? 0 : timer->due_time - now;
        if (remaining < nearest)
            nearest = remaining;
    }

    user_timer_unlock_irqrestore(flags);
    if (found && timeout_ms)
        *timeout_ms = nearest;
    return found;
}

static int user_timer_retrieve(void *out, HWND window, DWORD min_message,
                               DWORD max_message, BOOL remove, MSG *retrieved)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    DWORD now = shim_timeGetTime();
    USER_TIMER snapshot = {0};
    BOOL found = FALSE;
    uint64_t flags = user_timer_lock_irqsave();

    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        if (!user_timer_matches_locked(timer, pid, tid, window,
                                       min_message, max_message))
            continue;
        user_timer_refresh_locked(timer, now);
        if (!timer->pending)
            continue;
        snapshot = *timer;
        if (remove) {
            timer->pending = FALSE;
            timer->posted = FALSE;
            timer->pending_callback = NULL;
        } else {
            /* Once returned by PM_NOREMOVE, this is a posted message. It
             * survives KillTimer and replacement until PM_REMOVE consumes it. */
            timer->posted = TRUE;
        }
        found = TRUE;
        break;
    }

    user_timer_unlock_irqrestore(flags);
    if (!found)
        return 0;

    msg_write_to(out, snapshot.window, WM_TIMER, snapshot.id,
                 (LPARAM)(ULONG_PTR)snapshot.pending_callback,
                 snapshot.message_time,
                 0, 0);
    if (retrieved) {
        retrieved->hwnd = snapshot.window;
        retrieved->message = WM_TIMER;
        retrieved->wParam = snapshot.id;
        retrieved->lParam =
            (LPARAM)(ULONG_PTR)snapshot.pending_callback;
        retrieved->time = snapshot.message_time;
        retrieved->pt.x = 0;
        retrieved->pt.y = 0;
    }
    g_last_msg_time = snapshot.message_time;
    g_last_msg_pos.x = 0;
    g_last_msg_pos.y = 0;
    message_extra_info = 0;
    msg_note_retrieval(pid, tid, MSG_SOURCE_POSTED, 0);
    for (int i = 0; i < 256; i++)
        key_state_at_msg[i] = key_state[i];
    return 1;
}

static BOOL user_timer_id_in_use_locked(DWORD pid, DWORD tid, ULONG_PTR id)
{
    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        if ((timer->used || (timer->posted && timer->pending)) &&
            !timer->window && timer->owner_pid == pid &&
            timer->owner_tid == tid && timer->id == id)
            return TRUE;
    }
    return FALSE;
}

static ULONG_PTR user_timer_set(HWND window, ULONG_PTR event_id,
                                UINT elapsed, PVOID callback)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    WINDOW *target = window ? find_window(window) : NULL;

    if (window && !target) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    if (target && (target->owner_pid != pid || target->owner_tid != tid)) {
        SetLastError(5); /* ERROR_ACCESS_DENIED */
        return 0;
    }
    if (elapsed < USER_TIMER_MINIMUM)
        elapsed = USER_TIMER_MINIMUM;
    else if (elapsed > USER_TIMER_MAXIMUM)
        elapsed = USER_TIMER_MAXIMUM;

    int slot = -1;
    int free_slot = -1;
    uint64_t flags = user_timer_lock_irqsave();
    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        BOOL occupied = timer->used || (timer->posted && timer->pending);
        if (!occupied) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (timer->owner_pid == pid && timer->owner_tid == tid &&
            timer->window == window && timer->id == event_id &&
            (timer->used || window)) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        slot = free_slot;
        if (!window && slot >= 0) {
            do {
                event_id = next_user_timer_id++;
                if (!next_user_timer_id)
                    next_user_timer_id = 1;
            } while (!event_id || user_timer_id_in_use_locked(pid, tid,
                                                               event_id));
        }
    }

    if (slot < 0) {
        user_timer_unlock_irqrestore(flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }

    USER_TIMER *timer = &user_timers[slot];
    BOOL retain_posted = timer->pending && timer->posted;
    timer->used = TRUE;
    if (!retain_posted) {
        timer->pending = FALSE;
        timer->posted = FALSE;
        timer->message_time = 0;
        timer->pending_callback = NULL;
    }
    timer->owner_pid = pid;
    timer->owner_tid = tid;
    timer->window = window;
    timer->id = event_id;
    timer->interval_ms = elapsed;
    timer->due_time = shim_timeGetTime() + elapsed;
    timer->callback = callback;
    user_timer_unlock_irqrestore(flags);

    msg_wait_event_signal(pid, tid);
    /* NT accepts ID zero for a window timer. The timer remains keyed by zero
     * (and must be removed with KillTimer(window, 0)), while SetTimer returns
     * a nonzero success value. */
    return window && !event_id ? 1 : event_id;
}

static BOOL user_timer_kill(HWND window, ULONG_PTR event_id)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    BOOL removed = FALSE;
    uint64_t flags = user_timer_lock_irqsave();

    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        if (timer->used && timer->owner_pid == pid &&
            timer->owner_tid == tid && timer->window == window &&
            timer->id == event_id) {
            timer->used = FALSE;
            if (!(timer->posted && timer->pending)) {
                timer->pending = FALSE;
                timer->posted = FALSE;
                timer->pending_callback = NULL;
            }
            removed = TRUE;
            break;
        }
    }

    user_timer_unlock_irqrestore(flags);
    if (removed)
        msg_wait_event_signal(pid, tid);
    return removed;
}

static void user_timer_release_window(HWND window)
{
    uint64_t flags = user_timer_lock_irqsave();
    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        if ((user_timers[i].used || user_timers[i].posted) &&
            user_timers[i].window == window) {
            user_timers[i].used = FALSE;
            user_timers[i].pending = FALSE;
            user_timers[i].posted = FALSE;
            user_timers[i].pending_callback = NULL;
        }
    }
    user_timer_unlock_irqrestore(flags);
}

static void user_timer_release_process(DWORD pid)
{
    uint64_t flags = user_timer_lock_irqsave();
    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        if ((user_timers[i].used || user_timers[i].posted) &&
            user_timers[i].owner_pid == pid) {
            user_timers[i].used = FALSE;
            user_timers[i].pending = FALSE;
            user_timers[i].posted = FALSE;
            user_timers[i].pending_callback = NULL;
        }
    }
    user_timer_unlock_irqrestore(flags);
}

static void user_timer_release_thread(DWORD pid, DWORD tid)
{
    uint64_t flags = user_timer_lock_irqsave();
    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        USER_TIMER *timer = &user_timers[i];
        if ((timer->used || timer->posted) && timer->owner_pid == pid &&
            timer->owner_tid == tid) {
            timer->used = FALSE;
            timer->pending = FALSE;
            timer->posted = FALSE;
            timer->pending_callback = NULL;
        }
    }
    user_timer_unlock_irqrestore(flags);
}

/*
 * PS/2 scancode set 1 → Windows virtual key code.
 * Index = scancode (0x00-0x58). Extended keys (0xE0 prefix) handled separately.
 */
static const BYTE scancode_to_vk[0x59] = {
    /*0x00*/ 0,       VK_ESCAPE, '1',      '2',      '3',      '4',      '5',      '6',
    /*0x08*/ '7',     '8',       '9',      '0',      VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
    /*0x10*/ 'Q',     'W',       'E',      'R',      'T',      'Y',      'U',      'I',
    /*0x18*/ 'O',     'P',       VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL, 'A',  'S',
    /*0x20*/ 'D',     'F',       'G',      'H',      'J',      'K',      'L',      VK_OEM_1,
    /*0x28*/ VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5, 'Z',    'X',      'C',      'V',
    /*0x30*/ 'B',     'N',       'M',      VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_RSHIFT, VK_MULTIPLY,
    /*0x38*/ VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1,  VK_F2,    VK_F3,    VK_F4,    VK_F5,
    /*0x40*/ VK_F6,   VK_F7,     VK_F8,    VK_F9,    VK_F10,   VK_NUMLOCK, VK_SCROLL, VK_NUMPAD7,
    /*0x48*/ VK_NUMPAD8, VK_NUMPAD9, VK_SUBTRACT, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_ADD, VK_NUMPAD1,
    /*0x50*/ VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL, 0, 0, 0, VK_F11,
    /*0x58*/ VK_F12
};

/* Extended scancodes (after 0xE0 prefix) */
static BYTE extended_scancode_to_vk(BYTE sc)
{
    switch (sc) {
    case 0x1C: return VK_RETURN;   /* Numpad Enter */
    case 0x1D: return VK_RCONTROL;
    case 0x35: return VK_DIVIDE;   /* Numpad / */
    case 0x38: return VK_RMENU;    /* Right Alt */
    case 0x47: return VK_HOME;
    case 0x48: return VK_UP;
    case 0x49: return VK_PRIOR;    /* Page Up */
    case 0x4B: return VK_LEFT;
    case 0x4D: return VK_RIGHT;
    case 0x4F: return VK_END;
    case 0x50: return VK_DOWN;
    case 0x51: return VK_NEXT;     /* Page Down */
    case 0x52: return VK_INSERT;
    case 0x53: return VK_DELETE;
    case 0x5B: return VK_LWIN;
    case 0x5C: return VK_RWIN;
    default:   return 0;
    }
}

/* ── Cursor state ──────────────────────────────────────────── */

#define U32_SHARED_CURSOR_A ((HCURSOR)(ULONG_PTR)0xC0000001ULL)
#define U32_SHARED_CURSOR_W ((HCURSOR)(ULONG_PTR)0xCCC00001ULL)
#define U32_SHARED_ICON_A   ((HICON)(ULONG_PTR)0xC0000002ULL)
#define U32_SHARED_ICON_W   ((HICON)(ULONG_PTR)0xC0000003ULL)

static HCURSOR current_cursor = NULL;

#define USER_ICON_SLOTS       256
#define USER_ICON_TAG         0xC1000000u
#define USER_CURSOR_TAG       0xC2000000u
#define USER_ICON_TAG_MASK    0xFF000000u
#define USER_ICON_INDEX_MASK  0x000000FFu
#define USER_ICON_GEN_MASK    0x0000FFFFu
#define USER_ICON_GEN_SHIFT   8

typedef struct {
    BOOL used;
    BOOL is_icon;
    USHORT generation;
    DWORD owner_pid;
    DWORD x_hotspot;
    DWORD y_hotspot;
    HBITMAP mask;
    HBITMAP color;
    HANDLE handle;
} USER_ICON;

static USER_ICON user_icons[USER_ICON_SLOTS];
static spinlock_t user_icon_lock = SPINLOCK_INIT;

extern HBITMAP gdi32_clone_bitmap(HBITMAP bitmap);
extern BOOL gdi32_draw_icon_bitmap(HDC hdc, HBITMAP color, HBITMAP mask,
                                   int x, int y, int width, int height);
extern HBITMAP WINAPI CreateBitmap(int width, int height, UINT planes,
                                   UINT bits_per_pixel, PVOID bits);
extern BOOL WINAPI DeleteObject(HANDLE object);

static inline uint64_t user_icon_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&user_icon_lock);
    return flags;
}

static inline void user_icon_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&user_icon_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static USER_ICON *icon_lookup_locked(HANDLE handle)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    ULONG_PTR tag = value & USER_ICON_TAG_MASK;
    if (tag != USER_ICON_TAG && tag != USER_CURSOR_TAG)
        return NULL;
    UINT index = (UINT)(value & USER_ICON_INDEX_MASK);
    if (index >= USER_ICON_SLOTS || !user_icons[index].used ||
        user_icons[index].handle != handle)
        return NULL;
    return &user_icons[index];
}

static HANDLE icon_create(BOOL is_icon, DWORD x_hotspot, DWORD y_hotspot,
                          HBITMAP mask, HBITMAP color, DWORD owner_pid)
{
    uint64_t flags = user_icon_lock_irqsave();
    for (UINT index = 0; index < USER_ICON_SLOTS; index++) {
        USER_ICON *entry = &user_icons[index];
        if (entry->used) continue;
        USHORT generation = (USHORT)((entry->generation + 1u) &
                                     USER_ICON_GEN_MASK);
        if (!generation) generation = 1;
        ULONG_PTR tag = is_icon ? USER_ICON_TAG : USER_CURSOR_TAG;
        HANDLE handle = (HANDLE)(tag |
            ((ULONG_PTR)generation << USER_ICON_GEN_SHIFT) | index);
        entry->used = TRUE;
        entry->is_icon = is_icon;
        entry->generation = generation;
        entry->owner_pid = owner_pid;
        entry->x_hotspot = x_hotspot;
        entry->y_hotspot = y_hotspot;
        entry->mask = mask;
        entry->color = color;
        entry->handle = handle;
        user_icon_unlock_irqrestore(flags);
        return handle;
    }
    user_icon_unlock_irqrestore(flags);
    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return NULL;
}

static BOOL icon_snapshot(HANDLE handle, USER_ICON *snapshot)
{
    uint64_t flags = user_icon_lock_irqsave();
    USER_ICON *entry = icon_lookup_locked(handle);
    if (entry && snapshot) *snapshot = *entry;
    user_icon_unlock_irqrestore(flags);
    return entry != NULL;
}

static BOOL icon_destroy(HANDLE handle, BOOL enforce_owner)
{
    HBITMAP mask = NULL;
    HBITMAP color = NULL;
    uint64_t flags = user_icon_lock_irqsave();
    USER_ICON *entry = icon_lookup_locked(handle);
    if (!entry || (enforce_owner && entry->owner_pid != GetCurrentProcessId())) {
        user_icon_unlock_irqrestore(flags);
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    mask = entry->mask;
    color = entry->color;
    entry->used = FALSE;
    entry->mask = NULL;
    entry->color = NULL;
    entry->handle = NULL;
    if (current_cursor == handle) current_cursor = NULL;
    user_icon_unlock_irqrestore(flags);
    if (mask) DeleteObject(mask);
    if (color) DeleteObject(color);
    return TRUE;
}

static void icon_release_process(DWORD pid)
{
    for (UINT index = 0; index < USER_ICON_SLOTS; index++) {
        HANDLE handle = NULL;
        uint64_t flags = user_icon_lock_irqsave();
        if (user_icons[index].used && user_icons[index].owner_pid == pid)
            handle = user_icons[index].handle;
        user_icon_unlock_irqrestore(flags);
        if (handle) icon_destroy(handle, FALSE);
    }
}

static void icon_release_all(void)
{
    for (UINT index = 0; index < USER_ICON_SLOTS; index++) {
        HANDLE handle = NULL;
        uint64_t flags = user_icon_lock_irqsave();
        if (user_icons[index].used)
            handle = user_icons[index].handle;
        user_icon_unlock_irqrestore(flags);
        if (handle) icon_destroy(handle, FALSE);
    }
}

static HWND  caret_hwnd = NULL;
static POINT caret_pos = { 0, 0 };
/* Zero is the implicit initial count. Retain only nonzero per-thread counts,
 * in kernel memory that remains readable by the compositor across CR3s. */
typedef struct USER_CURSOR_COUNT {
    struct USER_CURSOR_COUNT *next;
    DWORD pid, tid;
    int count;
} USER_CURSOR_COUNT;
static USER_CURSOR_COUNT *cursor_counts;
static spinlock_t cursor_count_lock = SPINLOCK_INIT;

static uint64_t cursor_count_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&cursor_count_lock);
    return flags;
}

static void cursor_count_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&cursor_count_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static int cursor_display_count(DWORD pid, DWORD tid)
{
    int count = 0;
    uint64_t flags = cursor_count_lock_irqsave();
    for (USER_CURSOR_COUNT *entry = cursor_counts; entry; entry = entry->next)
        if (entry->pid == pid && entry->tid == tid) {
            count = entry->count;
            break;
        }
    cursor_count_unlock_irqrestore(flags);
    return count;
}

static int cursor_change_count(DWORD pid, DWORD tid, int value, BOOL add)
{
    USER_CURSOR_COUNT *spare = NULL;
    for (;;) {
        uint64_t flags = cursor_count_lock_irqsave();
        USER_CURSOR_COUNT **link = &cursor_counts;
        while (*link && ((*link)->pid != pid || (*link)->tid != tid))
            link = &(*link)->next;
        if (!*link && value && !spare) {
            cursor_count_unlock_irqrestore(flags);
            spare = kcalloc(1, sizeof(*spare));
            if (!spare) {
                SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
                return 0;
            }
            spare->pid = pid;
            spare->tid = tid;
            continue;
        }
        if (!*link && spare) {
            *link = spare;
            spare = NULL;
        }
        USER_CURSOR_COUNT *entry = *link;
        int previous = entry ? entry->count : 0;
        int count = add
            ? (int)((DWORD)previous + (DWORD)value)
            : value;
        USER_CURSOR_COUNT *retired = NULL;
        if (entry) {
            entry->count = count;
            if (!count) {
                *link = entry->next;
                retired = entry;
            }
        }
        cursor_count_unlock_irqrestore(flags);
        if (retired) kfree(retired);
        if (spare) kfree(spare);
        if ((previous < 0) != (count < 0) && compositor_request_frame)
            compositor_request_frame();
        return count;
    }
}

static void cursor_release_counts(DWORD pid, DWORD tid)
{
    USER_CURSOR_COUNT *retired = NULL;
    uint64_t flags = cursor_count_lock_irqsave();
    USER_CURSOR_COUNT **link = &cursor_counts;
    while (*link) {
        USER_CURSOR_COUNT *entry = *link;
        if ((!pid || entry->pid == pid) && (!tid || entry->tid == tid)) {
            *link = entry->next;
            entry->next = retired;
            retired = entry;
        } else {
            link = &entry->next;
        }
    }
    cursor_count_unlock_irqrestore(flags);
    while (retired) {
        USER_CURSOR_COUNT *next = retired->next;
        kfree(retired);
        retired = next;
    }
    if (compositor_request_frame) compositor_request_frame();
}
static HWND  capture_hwnd = NULL;
static HWND  focus_hwnd   = NULL;   /* SetFocus / WM_SETFOCUS target */
static HWND  active_hwnd  = NULL;   /* active/foreground top-level window */
static BOOL  user32_foreground_active;
static int   clip_active  = 0;
static DWORD clip_owner_pid;
static DWORD clip_owner_tid;
static RECT  clip_rect    = {
    0, 0, USER32_FALLBACK_SCREEN_WIDTH, USER32_FALLBACK_SCREEN_HEIGHT
};

typedef struct {
    BOOL active;
    DWORD owner_pid;
    DWORD owner_tid;
    POINT anchor;
    DWORD last_warp_ms;
    UINT matching_warps;
} RELATIVE_POINTER_STATE;

static RELATIVE_POINTER_STATE relative_pointer;

/* Relative-delta tracker for absolute HID devices while an application uses
 * the classic clipped/hidden cursor-warp input model. */
static int   g_abs_prev_valid = 0;
static int   g_abs_prev_sx = 0, g_abs_prev_sy = 0;

static void update_mouse_tracking(HWND target, LPARAM pos_lp, int moved)
{
    DWORD now = shim_timeGetTime();

    for (int i = 0; i < window_count; i++) {
        WINDOW *w = &windows[i];
        if (!w->used || !w->mouse_track_flags) continue;

        if (target != w->handle) {
            if (w->mouse_track_flags & TME_LEAVE) {
                DWORD message = (w->mouse_track_flags & TME_NONCLIENT)
                    ? WM_NCMOUSELEAVE : WM_MOUSELEAVE;
                msg_enqueue(w->handle, message, 0, 0);
                w->mouse_track_flags &= ~TME_LEAVE;
                if (!(w->mouse_track_flags & (TME_HOVER | TME_LEAVE)))
                    w->mouse_track_flags &= ~TME_NONCLIENT;
            }
            if (w->mouse_track_flags & TME_HOVER) {
                w->mouse_hover_start = now;
                w->mouse_hover_origin = cursor_pos;
            }
            continue;
        }

        if (!(w->mouse_track_flags & TME_HOVER)) continue;
        LONG dx = cursor_pos.x - w->mouse_hover_origin.x;
        LONG dy = cursor_pos.y - w->mouse_hover_origin.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if ((moved && (dx > 4 || dy > 4)) || !w->mouse_hover_start) {
            w->mouse_hover_start = now;
            w->mouse_hover_origin = cursor_pos;
            continue;
        }
        if ((DWORD)(now - w->mouse_hover_start) >= w->mouse_hover_time) {
            DWORD message = (w->mouse_track_flags & TME_NONCLIENT)
                ? WM_NCMOUSEHOVER : WM_MOUSEHOVER;
            msg_enqueue(w->handle, message, (WPARAM)mouse_buttons, pos_lp);
            w->mouse_track_flags &= ~TME_HOVER;
            if (!(w->mouse_track_flags & (TME_HOVER | TME_LEAVE)))
                w->mouse_track_flags &= ~TME_NONCLIENT;
        }
    }
}

static HWND input_target(void);
static HWND mouse_input_target(void);

static int point_near(POINT point, POINT target, int tolerance)
{
    int dx = point.x - target.x;
    int dy = point.y - target.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= tolerance && dy <= tolerance;
}

static void confine_cursor_point(POINT *point)
{
    if (!point || !clip_active)
        return;
    if (point->x < clip_rect.left) point->x = clip_rect.left;
    if (point->y < clip_rect.top) point->y = clip_rect.top;
    if (point->x >= clip_rect.right) point->x = clip_rect.right - 1;
    if (point->y >= clip_rect.bottom) point->y = clip_rect.bottom - 1;
}

static void relative_pointer_reset(void)
{
    relative_pointer.active = FALSE;
    relative_pointer.owner_pid = 0;
    relative_pointer.owner_tid = 0;
    relative_pointer.anchor.x = 0;
    relative_pointer.anchor.y = 0;
    relative_pointer.last_warp_ms = 0;
    relative_pointer.matching_warps = 0;
    g_abs_prev_valid = 0;
}

static void log_input_prefix(const char *tag)
{
    serial_puts(tag);
    serial_puts(" focus=0x"); serial_puthex((uint64_t)(ULONG_PTR)focus_hwnd, 8);
    serial_puts(" active=0x"); serial_puthex((uint64_t)(ULONG_PTR)active_hwnd, 8);
    serial_puts(" target=0x"); serial_puthex((uint64_t)(ULONG_PTR)input_target(), 8);
    serial_puts(" cap=0x"); serial_puthex((uint64_t)(ULONG_PTR)capture_hwnd, 8);
    serial_puts(" cv="); serial_putdec((uint64_t)(int64_t)
        cursor_display_count(GetCurrentProcessId(), GetCurrentThreadId()));
    serial_puts(" clip="); serial_putdec((uint64_t)(int64_t)clip_active);
}

/* A hidden or clipped cursor is not by itself a request for relative input.
 * Enable tablet-to-relative translation only after the foreground owner
 * recenters the shared cursor, matching the classic Win32 warp input model. */
static int relative_pointer_mode_active(void)
{
    if (!relative_pointer.active)
        return 0;

    BOOL owns_clip = clip_active &&
        clip_owner_pid == relative_pointer.owner_pid;
    BOOL owns_hidden_cursor = cursor_display_count(
        relative_pointer.owner_pid, relative_pointer.owner_tid) < 0;
    if (!owns_clip && !owns_hidden_cursor)
        return 0;

    WINDOW *target = find_window(input_target());
    return !target || target->owner_pid == relative_pointer.owner_pid;
}

static void relative_pointer_note_warp(POINT point)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    DWORD now = shim_timeGetTime();
    BOOL same_warp = relative_pointer.owner_pid == pid &&
        relative_pointer.owner_tid == tid &&
        point_near(point, relative_pointer.anchor, 1) &&
        (DWORD)(now - relative_pointer.last_warp_ms) <= 250;

    relative_pointer.owner_pid = pid;
    relative_pointer.owner_tid = tid;
    relative_pointer.anchor = point;
    relative_pointer.last_warp_ms = now;
    relative_pointer.matching_warps = same_warp
        ? relative_pointer.matching_warps + 1 : 1;

    WINDOW *target = find_window(input_target());
    BOOL owns_target = !target || target->owner_pid == pid;
    BOOL owns_clip = clip_active && clip_owner_pid == pid;
    BOOL owns_hidden_cursor = cursor_display_count(pid, tid) < 0;
    BOOL centered = FALSE;

    if (owns_clip) {
        POINT center = {
            clip_rect.left + (clip_rect.right - clip_rect.left) / 2,
            clip_rect.top + (clip_rect.bottom - clip_rect.top) / 2,
        };
        centered = point_near(point, center, 2);
    } else if (owns_hidden_cursor && target) {
        int x, y;
        window_screen_origin(target, &x, &y);
        POINT center = { x + target->width / 2, y + target->height / 2 };
        centered = point_near(point, center, 2);
    }

    relative_pointer.active = owns_target &&
        ((owns_clip && centered) ||
         (owns_hidden_cursor &&
          (centered || relative_pointer.matching_warps >= 2)));
    if (!relative_pointer.active)
        g_abs_prev_valid = 0;
}

static void user32_reset_corrupt_window_state(const char *where)
{
    static int log_count = 0;
    if (log_count < 8) {
        serial_puts("[USER32] corrupt window state at ");
        serial_puts(where ? where : "?");
        serial_puts(": window_count=");
        serial_putdec((uint64_t)(int64_t)window_count);
        serial_puts(" reset\n");
        log_count++;
    }

    for (int i = 0; i < MAX_WINDOWS; i++)
        windows[i].used = 0;
    window_count = 0;
    focus_hwnd = NULL;
    active_hwnd = NULL;
    user32_foreground_active = FALSE;
    capture_hwnd = NULL;
    clip_active = 0;
    clip_owner_pid = 0;
    clip_owner_tid = 0;
    relative_pointer_reset();
    msg_head = msg_tail = 0;
    queue_changed_status = 0;
}

static int user32_window_state_sane(const char *where)
{
    if (window_count >= 0 && window_count <= MAX_WINDOWS)
        return 1;
    user32_reset_corrupt_window_state(where);
    return 0;
}

static int win32_input_active(void)
{
    if (!user32_window_state_sane("input"))
        return 0;
    return user32_foreground_active && input_target() != NULL;
}

static int u32_input_diagnostics_active(void)
{
    /* Compat diagnostics are scoped to the active PE32 input queue. */
    return U32_INPUT_DIAGNOSTICS && g_compat32_mode &&
           input_target() != NULL;
}

/* Route keyboard and relative pointer input using USER32 state. The focused
 * child wins, followed by the active top-level window and the topmost visible
 * window. This keeps routing independent of executable and class names. */
static HWND input_target(void)
{
    if (!user32_window_state_sane("target") ||
        !user32_foreground_active)
        return NULL;
    WINDOW *focused = find_window(focus_hwnd);
    if (window_can_receive_input(focused))
        return focused->handle;

    WINDOW *active = find_window(active_hwnd);
    if (window_can_activate(active))
        return active->handle;

    WINDOW *top = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WINDOW *candidate = &windows[i];
        if (!window_can_activate(candidate))
            continue;
        if (!top || root_is_above(candidate, top))
            top = candidate;
    }
    return top ? top->handle : NULL;
}

static LRESULT dispatch_wndproc(WNDPROC wndproc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam);

/* ── Default screen dimensions ─────────────────────────────── */

/* ── API Implementations ───────────────────────────────────── */

/*
 * Read WNDCLASSEXA from caller buffer (32-bit or 64-bit layout).
 *
 * 32-bit WNDCLASSEXA (48 bytes):
 *   +0: cbSize(4) +4: style(4) +8: lpfnWndProc(4) +12: cbClsExtra(4)
 *   +16: cbWndExtra(4) +20: hInstance(4) +24: hIcon(4) +28: hCursor(4)
 *   +32: hbrBackground(4) +36: lpszMenuName(4) +40: lpszClassName(4)
 *   +44: hIconSm(4)
 *
 * 64-bit WNDCLASSEXA (80 bytes):
 *   +0: cbSize(4) +4: style(4) +8: lpfnWndProc(8) +16: cbClsExtra(4)
 *   +20: cbWndExtra(4) +24: hInstance(8) +32: hIcon(8) +40: hCursor(8)
 *   +48: hbrBackground(8) +56: lpszMenuName(8) +64: lpszClassName(8)
 *   +72: hIconSm(8)
 */
static void wndclassex_read(const void *src, WNDCLASSEXA *out)
{
    if (g_compat32_mode) {
        const uint32_t *p = (const uint32_t *)src;
        out->cbSize        = p[0];
        out->style         = p[1];
        out->lpfnWndProc   = (WNDPROC)(ULONG_PTR)p[2];
        out->cbClsExtra    = (int)p[3];
        out->cbWndExtra    = (int)p[4];
        out->hInstance     = (HINSTANCE)(ULONG_PTR)p[5];
        out->hIcon         = (HICON)(ULONG_PTR)p[6];
        out->hCursor       = (HCURSOR)(ULONG_PTR)p[7];
        out->hbrBackground = (HBRUSH)(ULONG_PTR)p[8];
        out->lpszMenuName  = (PCSTR)(ULONG_PTR)p[9];
        out->lpszClassName = (PCSTR)(ULONG_PTR)p[10];
        out->hIconSm       = (HICON)(ULONG_PTR)p[11];
    } else {
        *out = *(const WNDCLASSEXA *)src;
    }
}

/*
 * Read WNDCLASSA from caller buffer (32-bit or 64-bit layout).
 *
 * 32-bit WNDCLASSA (40 bytes):
 *   +0: style(4) +4: lpfnWndProc(4) +8: cbClsExtra(4) +12: cbWndExtra(4)
 *   +16: hInstance(4) +20: hIcon(4) +24: hCursor(4) +28: hbrBackground(4)
 *   +32: lpszMenuName(4) +36: lpszClassName(4)
 */
static void wndclass_read(const void *src, WNDCLASSA *out)
{
    if (g_compat32_mode) {
        const uint32_t *p = (const uint32_t *)src;
        out->style         = p[0];
        out->lpfnWndProc   = (WNDPROC)(ULONG_PTR)p[1];
        out->cbClsExtra    = (int)p[2];
        out->cbWndExtra    = (int)p[3];
        out->hInstance     = (HINSTANCE)(ULONG_PTR)p[4];
        out->hIcon         = (HICON)(ULONG_PTR)p[5];
        out->hCursor       = (HCURSOR)(ULONG_PTR)p[6];
        out->hbrBackground = (HBRUSH)(ULONG_PTR)p[7];
        out->lpszMenuName  = (PCSTR)(ULONG_PTR)p[8];
        out->lpszClassName = (PCSTR)(ULONG_PTR)p[9];
    } else {
        *out = *(const WNDCLASSA *)src;
    }
}

WORD WINAPI RegisterClassExA(const WNDCLASSEXA *lpwcx)
{
    if (!lpwcx) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    WNDCLASSEXA wcx;
    wndclassex_read(lpwcx, &wcx);

    if (!wcx.lpszClassName || !wcx.lpszClassName[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    serial_puts("[USER32] RegisterClassExA: ");
    serial_puts(wcx.lpszClassName);
    serial_puts("\n");

    DWORD pid = GetCurrentProcessId();
    if (find_class_for_pid(wcx.lpszClassName, pid)) {
        SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }
    WNDCLASS_ENTRY *e = alloc_wndclass();
    if (!e) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    u32_strcpy(e->class_name, wcx.lpszClassName, 128);
    e->wndproc    = wcx.lpfnWndProc;
    e->style      = wcx.style;
    e->cbClsExtra = wcx.cbClsExtra;
    e->cbWndExtra = wcx.cbWndExtra;
    e->hInstance = wcx.hInstance;
    e->hIcon = wcx.hIcon;
    e->hCursor = wcx.hCursor;
    e->hbrBackground = wcx.hbrBackground;
    e->menu_name = (ULONG_PTR)wcx.lpszMenuName;
    e->class_name_ptr = (ULONG_PTR)wcx.lpszClassName;
    e->hIconSm = wcx.hIconSm;
    e->owner_pid  = pid;
    e->used       = 1;

    return e->atom;
}

WORD WINAPI RegisterClassA(const WNDCLASSA *lpwcx)
{
    if (!lpwcx) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    WNDCLASSA wca;
    wndclass_read(lpwcx, &wca);

    if (!wca.lpszClassName || !wca.lpszClassName[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    WNDCLASSEXA ex;
    BYTE *p = (BYTE *)&ex;
    for (SIZE_T i = 0; i < sizeof(ex); i++) p[i] = 0;
    ex.cbSize        = sizeof(WNDCLASSEXA);
    ex.style         = wca.style;
    ex.lpfnWndProc   = wca.lpfnWndProc;
    ex.cbClsExtra    = wca.cbClsExtra;
    ex.cbWndExtra    = wca.cbWndExtra;
    ex.hInstance     = wca.hInstance;
    ex.hIcon         = wca.hIcon;
    ex.hCursor       = wca.hCursor;
    ex.hbrBackground = wca.hbrBackground;
    ex.lpszMenuName  = wca.lpszMenuName;
    ex.lpszClassName = wca.lpszClassName;

    /* Call internal registration directly (not through thunk) */
    DWORD pid = GetCurrentProcessId();
    if (find_class_for_pid(ex.lpszClassName, pid)) {
        SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }
    WNDCLASS_ENTRY *e = alloc_wndclass();
    if (!e) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    u32_strcpy(e->class_name, ex.lpszClassName, 128);
    e->wndproc    = ex.lpfnWndProc;
    e->style      = ex.style;
    e->cbClsExtra = ex.cbClsExtra;
    e->cbWndExtra = ex.cbWndExtra;
    e->hInstance = ex.hInstance;
    e->hIcon = ex.hIcon;
    e->hCursor = ex.hCursor;
    e->hbrBackground = ex.hbrBackground;
    e->menu_name = (ULONG_PTR)ex.lpszMenuName;
    e->class_name_ptr = (ULONG_PTR)ex.lpszClassName;
    e->hIconSm = ex.hIconSm;
    e->owner_pid  = pid;
    e->used       = 1;
    return e->atom;
}

WORD WINAPI RegisterClassW(PVOID lpwc)
{
    if (!lpwc) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    WNDCLASSA wc;
    wndclass_read(lpwc, &wc);
    const uint16_t *class_name = (const uint16_t *)wc.lpszClassName;
    if (!class_name || !class_name[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    char class_a[128] = {0};
    int class_length;
    for (class_length = 0; class_length < 127 && class_name[class_length];
         class_length++)
        class_a[class_length] = (char)(class_name[class_length] & 0xFF);

    DWORD pid = GetCurrentProcessId();
    if (find_class_for_pid(class_a, pid)) {
        SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }

    WNDCLASS_ENTRY *e = alloc_wndclass();
    if (!e) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    u32_strcpy(e->class_name, class_a, sizeof(e->class_name));
    e->wndproc = wc.lpfnWndProc;
    e->style = wc.style;
    e->cbClsExtra = wc.cbClsExtra;
    e->cbWndExtra = wc.cbWndExtra;
    e->hInstance = wc.hInstance;
    e->hIcon = wc.hIcon;
    e->hCursor = wc.hCursor;
    e->hbrBackground = wc.hbrBackground;
    e->menu_name = (ULONG_PTR)wc.lpszMenuName;
    e->class_name_ptr = (ULONG_PTR)class_name;
    e->hIconSm = NULL;
    e->unicode = TRUE;
    e->owner_pid = pid;
    e->used = 1;

    serial_puts("[USER32] RegisterClassW: ");
    serial_puts(e->class_name);
    serial_puts("\n");
    return e->atom;
}

static WORD WINAPI GetClassWord_u32(HWND hwnd, int index)
{
    if (index != -32) return 0; /* GCW_ATOM */
    WINDOW *window = find_window(hwnd);
    WNDCLASS_ENTRY *entry = window
        ? lookup_class_for_pid(window->class_name, window->owner_pid) : NULL;
    return entry ? entry->atom : 0;
}

LONG_PTR WINAPI GetClassLongPtrW(HWND hwnd, int index)
{
    WINDOW *window = find_window(hwnd);
    if (!window) {
        SetLastError(1400);
        return 0;
    }
    WNDCLASS_ENTRY *entry =
        lookup_class_for_pid(window->class_name, window->owner_pid);
    if (!entry) return 0;

    switch (index) {
    case -34: return (LONG_PTR)(ULONG_PTR)entry->hIconSm;        /* GCLP_HICONSM */
    case -32: return entry->atom;                               /* GCW_ATOM */
    case -26: return entry->style;                              /* GCL_STYLE */
    case -24: return (LONG_PTR)(ULONG_PTR)entry->wndproc;        /* GCLP_WNDPROC */
    case -20: return entry->cbClsExtra;                         /* GCL_CBCLSEXTRA */
    case -18: return entry->cbWndExtra;                         /* GCL_CBWNDEXTRA */
    case -16: return (LONG_PTR)(ULONG_PTR)entry->hInstance;      /* GCLP_HMODULE */
    case -14: return (LONG_PTR)(ULONG_PTR)entry->hIcon;          /* GCLP_HICON */
    case -12: return (LONG_PTR)(ULONG_PTR)entry->hCursor;        /* GCLP_HCURSOR */
    case -10: return (LONG_PTR)(ULONG_PTR)entry->hbrBackground; /* GCLP_HBRBACKGROUND */
    case -8:  return (LONG_PTR)entry->menu_name;                 /* GCLP_MENUNAME */
    default:  return 0;
    }
}

LONG_PTR WINAPI SetClassLongPtrW(HWND hwnd, int index, LONG_PTR value)
{
    WINDOW *window = find_window(hwnd);
    if (!window) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    WNDCLASS_ENTRY *entry =
        find_class_for_pid(window->class_name, window->owner_pid);
    if (!entry) {
        SetLastError(1411); /* ERROR_CLASS_DOES_NOT_EXIST */
        return 0;
    }

    LONG_PTR previous;
    switch (index) {
    case -34:
        previous = (LONG_PTR)(ULONG_PTR)entry->hIconSm;
        entry->hIconSm = (HICON)(ULONG_PTR)value;
        break;
    case -26:
        previous = entry->style;
        entry->style = (DWORD)value;
        break;
    case -24:
        previous = (LONG_PTR)(ULONG_PTR)entry->wndproc;
        entry->wndproc = (WNDPROC)(ULONG_PTR)value;
        break;
    case -16:
        previous = (LONG_PTR)(ULONG_PTR)entry->hInstance;
        entry->hInstance = (HINSTANCE)(ULONG_PTR)value;
        break;
    case -14:
        previous = (LONG_PTR)(ULONG_PTR)entry->hIcon;
        entry->hIcon = (HICON)(ULONG_PTR)value;
        break;
    case -12:
        previous = (LONG_PTR)(ULONG_PTR)entry->hCursor;
        entry->hCursor = (HCURSOR)(ULONG_PTR)value;
        break;
    case -10:
        previous = (LONG_PTR)(ULONG_PTR)entry->hbrBackground;
        entry->hbrBackground = (HBRUSH)(ULONG_PTR)value;
        break;
    case -8:
        previous = (LONG_PTR)entry->menu_name;
        entry->menu_name = (ULONG_PTR)value;
        break;
    default:
        SetLastError(1413); /* ERROR_INVALID_INDEX */
        return 0;
    }
    return previous;
}

LONG WINAPI SetClassLongW(HWND hwnd, int index, LONG value)
{
    return (LONG)SetClassLongPtrW(hwnd, index, (LONG_PTR)value);
}

static LONG WINAPI SetClassLongA_u32(HWND hwnd, int index, LONG value)
{
    return SetClassLongW(hwnd, index, value);
}

static LONG_PTR WINAPI SetClassLongPtrA_u32(HWND hwnd, int index,
                                             LONG_PTR value)
{
    return SetClassLongPtrW(hwnd, index, value);
}

int WINAPI GetClassNameW(HWND hwnd, PWSTR class_name, int max_count)
{
    WINDOW *window = find_window(hwnd);
    if (!window || !class_name || max_count <= 0) {
        SetLastError(window ? 87 : 1400);
        return 0;
    }

    int length = 0;
    while (length + 1 < max_count && window->class_name[length]) {
        class_name[length] = (WCHAR)(BYTE)window->class_name[length];
        length++;
    }
    class_name[length] = 0;
    return length;
}

BOOL WINAPI UnregisterClassA(PCSTR lpClassName, HINSTANCE hInstance)
{
    (void)hInstance;
    if (!lpClassName) return FALSE;
    ULONG_PTR value = (ULONG_PTR)lpClassName;
    WNDCLASS_ENTRY *e = value <= 0xFFFF
        ? find_class_by_atom((WORD)value)
        : find_class(lpClassName);
    if (e) { e->used = 0; return TRUE; }
    return FALSE;
}

static BOOL WINAPI UnregisterClassW_k32(PCWSTR lpClassName,
                                         HINSTANCE hInstance)
{
    ULONG_PTR value = (ULONG_PTR)lpClassName;
    if (!value) return FALSE;
    if (value <= 0xFFFF) {
        WNDCLASS_ENTRY *e = find_class_by_atom((WORD)value);
        if (!e) return FALSE;
        e->used = 0;
        return TRUE;
    }

    char class_name[128];
    int i = 0;
    while (i < 127 && lpClassName[i]) {
        class_name[i] = (char)(lpClassName[i] & 0xFF);
        i++;
    }
    class_name[i] = 0;
    return UnregisterClassA(class_name, hInstance);
}

/* Deliver WM_SIZE to a window's 32-bit wndproc. Real Windows posts WM_SIZE
 * synchronously when a window is created with a size, resized, or shown — UE1's
 * UWindowsViewport learns SizeX/SizeY from this message (its WndProc WM_SIZE
 * handler calls ResizeViewport). Without it the viewport stays 0x0 → SoftDrv
 * SetRes(0,0) → DirectDraw SetDisplayMode(0,0) → zero-size surface → no frame.
 * compat32_callback_args handles the 64→32 mode switch (nesting-safe). */
/* WNDPROC addresses are valid only in their owning process address space. */
static void dispatch_window_message(WINDOW *w, DWORD message,
                                    WPARAM wParam, LPARAM lParam)
{
    if (!w || !w->wndproc)
        return;

    if (w->owner_pid == GetCurrentProcessId() &&
        w->owner_tid == GetCurrentThreadId()) {
        dispatch_wndproc(w->wndproc, w->handle, message, wParam, lParam);
        return;
    }

    if (message == WM_ACTIVATEAPP || message == WM_NCACTIVATE ||
        message == WM_ACTIVATE || message == WM_SETFOCUS ||
        message == WM_KILLFOCUS) {
        static unsigned trace_count;
        if (trace_count++ < 32) {
            serial_puts("[USER32-XPROC] queue hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)w->handle, 8);
            serial_puts(" msg=0x");
            serial_puthex(message, 4);
            serial_puts(" owner=");
            serial_putdec(w->owner_pid);
            serial_puts(":");
            serial_putdec(w->owner_tid);
            serial_puts(" current=");
            serial_putdec(GetCurrentProcessId());
            serial_puts(":");
            serial_putdec(GetCurrentThreadId());
            serial_puts("\n");
        }
    }
    msg_enqueue(w->handle, message, wParam, lParam);
}

static void dispatch_wm_size(WINDOW *w)
{
    if (!w || !w->wndproc) return;
    UINT size_type = (w->style & WS_MINIMIZE) ? SIZE_MINIMIZED :
                     ((w->style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : SIZE_RESTORED);
    int width = size_type == SIZE_MINIMIZED ? 0 : w->width;
    int height = size_type == SIZE_MINIMIZED ? 0 : w->height;
    if (size_type != SIZE_MINIMIZED && (width <= 0 || height <= 0)) return;
    serial_puts("[USER32] WM_SIZE hwnd=");
    serial_puthex((uint64_t)(ULONG_PTR)w->handle, 8);
    serial_puts(" ");
    serial_puthex(width, 4);
    serial_puts("x");
    serial_puthex(height, 4);
    serial_puts("\n");
    dispatch_window_message(
        w, WM_SIZE, size_type,
        ((uint32_t)width & 0xFFFF) | (((uint32_t)height & 0xFFFF) << 16));
}

typedef struct {
    uint32_t hwnd;
    uint32_t hwnd_insert_after;
    int32_t x;
    int32_t y;
    int32_t cx;
    int32_t cy;
    uint32_t flags;
} WINDOWPOS32;

_Static_assert(sizeof(WINDOWPOS32) == 28, "Win32 WINDOWPOS ABI");

static BOOL dispatch_windowpos_message(WINDOW *w, DWORD message,
                                       WINDOWPOS *position, BOOL copy_back)
{
    if (!w || !w->wndproc || !position ||
        w->owner_pid != GetCurrentProcessId() ||
        w->owner_tid != GetCurrentThreadId())
        return FALSE;

    if (!g_compat32_mode) {
        dispatch_wndproc(w->wndproc, w->handle, message, 0,
                         (LPARAM)(ULONG_PTR)position);
        return TRUE;
    }

    /* PE32 callbacks need a low, process-mapped WINDOWPOS layout. */
    USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
    PVOID scratch = user32_callback_scratch_page(context);
    if (!context || !scratch ||
        context->windowpos_depth >= USER_WINDOWPOS_MAX_DEPTH)
        return FALSE;
    volatile WINDOWPOS32 *wire = (volatile WINDOWPOS32 *)(
        (BYTE *)scratch + USER_WINDOWPOS_OFFSET +
        context->windowpos_depth * USER_WINDOWPOS_STRIDE);

    wire->hwnd = (uint32_t)(ULONG_PTR)position->hwnd;
    wire->hwnd_insert_after =
        (uint32_t)(ULONG_PTR)position->hwndInsertAfter;
    wire->x = position->x;
    wire->y = position->y;
    wire->cx = position->cx;
    wire->cy = position->cy;
    wire->flags = position->flags;

    context->windowpos_depth++;
    dispatch_wndproc(w->wndproc, w->handle, message, 0,
                     (LPARAM)(ULONG_PTR)wire);
    context->windowpos_depth--;

    if (copy_back) {
        position->hwndInsertAfter =
            (HWND)(ULONG_PTR)wire->hwnd_insert_after;
        position->x = wire->x;
        position->y = wire->y;
        position->cx = wire->cx;
        position->cy = wire->cy;
        position->flags = wire->flags;
    }
    return TRUE;
}

static void dispatch_focus_message(HWND target, DWORD message, HWND other)
{
    WINDOW *w = find_window(target);
    if (!w || !w->wndproc)
        return;

    dispatch_window_message(w, message, (WPARAM)(ULONG_PTR)other, 0);
}

static void dispatch_wm_deactivate(WINDOW *next)
{
    WINDOW *previous = find_window(active_hwnd);
    HWND old_focus = focus_hwnd;
    HWND next_handle = next ? next->handle : NULL;
    DWORD next_tid = next ? next->owner_tid : 0;

    active_hwnd = NULL;
    focus_hwnd = NULL;
    user32_foreground_active = FALSE;

    if (previous && previous->wndproc) {
        previous = window_root(previous, NULL);
        if (previous && previous->wndproc) {
            dispatch_window_message(previous, WM_ACTIVATEAPP, 0,
                                    (LPARAM)next_tid);
            dispatch_window_message(previous, WM_NCACTIVATE, 0, 0);
            dispatch_window_message(previous, WM_ACTIVATE, 0,
                                    (LPARAM)(ULONG_PTR)next_handle);
        }
    }
    if (old_focus)
        dispatch_focus_message(old_focus, WM_KILLFOCUS, next_handle);
}

/* Tell the engine its window is the active, focused foreground app. UE1's
 * UWindowsViewport gates realtime rendering on activation: without these
 * messages the viewport renders one init frame then idles (no per-frame
 * Repaint → no DDraw present). Real Windows delivers this sequence when a
 * window is shown and brought to the foreground. */
static void dispatch_wm_activate(WINDOW *w)
{
    if (!w || !w->wndproc) return;
    w = window_root(w, NULL);
    if (!w || !w->wndproc) return;

    WINDOW *previous = user32_foreground_active
        ? find_window(active_hwnd) : NULL;
    BOOL changed = !user32_foreground_active || previous != w;

    if (previous && previous != w && previous->wndproc) {
        dispatch_window_message(previous, WM_ACTIVATEAPP, 0,
                                (LPARAM)w->owner_tid);
        dispatch_window_message(previous, WM_NCACTIVATE, 0, 0);
        dispatch_window_message(previous, WM_ACTIVATE, 0,
                                (LPARAM)(ULONG_PTR)w->handle);
    }

    active_hwnd = w->handle;
    user32_foreground_active = TRUE;
    if (w->compositor_id && compositor_focus_window &&
        window_should_render(w))
        compositor_focus_window(w->compositor_id);
    /* Keep the shim focus state coherent with the messages we deliver: on NT
     * the window that receives WM_SETFOCUS IS the GetFocus() window. We used
     * to send WM_SETFOCUS here yet leave focus_hwnd NULL, so GetFocus()
     * contradicted the activation forever after — WinDrv gates its whole
     * in-game input path (UpdateInput key poll @0x11106F33, SetMouseCapture
     * OnlyFocus bail @0x1110665C) on GetFocus()==viewport hWnd. */
    if (changed) {
        dispatch_window_message(w, WM_ACTIVATEAPP, 1,
                                previous ? (LPARAM)previous->owner_tid : 0);
        dispatch_window_message(w, WM_NCACTIVATE, 1, 0);
        dispatch_window_message(w, WM_ACTIVATE, 1,
                                previous
                                    ? (LPARAM)(ULONG_PTR)previous->handle
                                    : 0);
    }

    /* WM_ACTIVATE may synchronously move focus to a child. Re-read the state
     * after the callback so activation cannot overwrite that choice. */
    WINDOW *focused = find_window(focus_hwnd);
    BOOL preserve_descendant = focused && focused != w &&
                               window_root(focused, NULL) == w;
    if (!preserve_descendant) {
        HWND old_focus = focus_hwnd;
        focus_hwnd = w->handle;
        if (old_focus && old_focus != w->handle)
            dispatch_focus_message(old_focus, WM_KILLFOCUS, w->handle);
        if (old_focus != w->handle)
            dispatch_focus_message(w->handle, WM_SETFOCUS, old_focus);
    }
    serial_puts(preserve_descendant
        ? "[USER32] dispatched WM_ACTIVATEAPP/ACTIVATE; kept child focus\n"
        : "[USER32] dispatched WM_ACTIVATEAPP/ACTIVATE/SETFOCUS\n");
}

static WINDOW *activation_candidate(HWND preferred)
{
    WINDOW *candidate = window_root(find_window(preferred), NULL);
    if (window_can_activate(candidate))
        return candidate;

    candidate = NULL;
    for (int i = 0; i < window_count; i++) {
        WINDOW *current = &windows[i];
        if (!window_can_activate(current))
            continue;
        if (!candidate || root_is_above(current, candidate))
            candidate = current;
    }
    return candidate;
}

static void repair_user32_activation(HWND preferred)
{
    if (!user32_foreground_active)
        return;

    WINDOW *active = window_root(find_window(active_hwnd), NULL);
    if (window_can_activate(active)) {
        WINDOW *focused = find_window(focus_hwnd);
        BOOL valid_focus = window_can_receive_input(focused) &&
                           window_should_render(focused) &&
                           window_root(focused, NULL) == active;
        if (!valid_focus) {
            HWND old_focus = focus_hwnd;
            focus_hwnd = active->handle;
            if (old_focus && old_focus != active->handle)
                dispatch_focus_message(old_focus, WM_KILLFOCUS,
                                       active->handle);
            if (old_focus != active->handle)
                dispatch_focus_message(active->handle, WM_SETFOCUS,
                                       old_focus);
        }
        if (active->compositor_id && compositor_focus_window)
            compositor_focus_window(active->compositor_id);
        return;
    }

    WINDOW *next = activation_candidate(preferred);
    dispatch_wm_deactivate(next);
    if (next)
        dispatch_wm_activate(next);
}

void user32_deactivate_compositor_windows(void)
{
    if (user32_foreground_active || active_hwnd || focus_hwnd)
        dispatch_wm_deactivate(NULL);
}

HWND WINAPI CreateWindowExA(DWORD dwExStyle, PCSTR lpClassName,
                            PCSTR lpWindowName, DWORD dwStyle,
                            int X, int Y, int nWidth, int nHeight,
                            HWND hWndParent, HMENU hMenu,
                            HINSTANCE hInstance, PVOID lpParam)
{
    (void)hInstance;

    DWORD pid = GetCurrentProcessId();
    ULONG_PTR class_value = (ULONG_PTR)lpClassName;
    int class_is_atom = lpClassName && class_value <= 0xFFFF;
    WNDCLASS_ENTRY *cls = NULL;
    if (lpClassName) {
        cls = class_is_atom
            ? lookup_class_by_atom_for_pid((WORD)class_value, pid)
            : lookup_class_for_pid(lpClassName, pid);
    }
    PCSTR class_name = cls ? cls->class_name
                           : (class_is_atom ? NULL : lpClassName);
    PCSTR window_name =
        lpWindowName && (ULONG_PTR)lpWindowName > 0xFFFF ? lpWindowName : NULL;

    serial_puts("[USER32] CreateWindowExA: ");
    if (class_is_atom) {
        serial_puts("#");
        serial_puthex(class_value, 4);
        if (class_name) {
            serial_puts(" (");
            serial_puts(class_name);
            serial_puts(")");
        }
    } else if (class_name) {
        serial_puts(class_name);
    }
    serial_puts(" \"");
    if (window_name) {
        serial_puts(window_name);
    } else if (lpWindowName) {
        serial_puts("#");
        serial_puthex((ULONG_PTR)lpWindowName, 4);
    }
    serial_puts("\"\n");

    /* MAKEINTATOM is a valid class argument. Resolve it before string access
     * and never substitute a different class when an explicit atom is bad. */
    if (!cls) {
        serial_puts(class_is_atom
            ? "[USER32]   unknown class atom\n"
            : "[USER32]   unknown window class\n");
        SetLastError(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return NULL;
    }

    WNDPROC wndproc = class_wndproc_for_mode(cls);
    if (!wndproc) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }

    /* CW_USEDEFAULT */
    if (X == (int)0x80000000) X = 0;
    if (Y == (int)0x80000000) Y = 0;
    if (nWidth == (int)0x80000000) nWidth = current_mode_cx();
    if (nHeight == (int)0x80000000) nHeight = current_mode_cy();

    WINDOW *relation = NULL;
    int message_only = hwnd_is_message(hWndParent);
    if (hWndParent && !message_only && !hwnd_is_desktop(hWndParent)) {
        relation = find_window(hWndParent);
        if (!relation) {
            SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
            return NULL;
        }
        if (relation->message_only)
            message_only = 1;
    }
    if ((dwStyle & WS_CHILD) && !hWndParent) {
        SetLastError(1400);
        return NULL;
    }

    WINDOW *w = alloc_window();
    if (!w) return NULL;
    w->handle   = (HWND)(ULONG_PTR)next_hwnd++;
    w->class_name[0] = 0;
    w->title[0] = 0;
    if (class_name) u32_strcpy(w->class_name, class_name, 128);
    if (window_name) u32_strcpy(w->title, window_name, 256);
    w->wndproc  = wndproc;
    w->unicode  = cls->unicode;
    w->style    = dwStyle;
    w->ex_style = dwExStyle;
    w->layered_color_key = 0;
    w->layered_flags = 0;
    w->layered_alpha = 255;
    w->x        = X;
    w->y        = Y;
    w->width    = nWidth;
    w->height   = nHeight;
    w->parent   = (dwStyle & WS_CHILD) && !message_only ? hWndParent : NULL;
    w->owner    = !(dwStyle & WS_CHILD) && !message_only &&
                  !hwnd_is_desktop(hWndParent) ? hWndParent : NULL;
    w->menu     = hMenu;
    w->system_menu = NULL;
    w->user_data = NULL;
    w->owner_pid = pid;
    w->owner_tid = GetCurrentThreadId();
    w->visible  = !message_only && (dwStyle & WS_VISIBLE) ? 1 : 0;
    w->message_only = message_only;
    w->destroying = 0;
    w->show_cmd = (dwStyle & WS_MINIMIZE) ? SW_SHOWMINIMIZED :
                  ((dwStyle & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    w->min_position.x = w->min_position.y = -1;
    w->max_position.x = w->max_position.y = -1;
    w->normal_rect.left = X;
    w->normal_rect.top = Y;
    w->normal_rect.right = X + nWidth;
    w->normal_rect.bottom = Y + nHeight;
    w->z_order = 0;
    w->render_z = 0;
    w->paint_pending = 0;
    w->erase_pending = 0;
    w->update_rect.left = w->update_rect.top = 0;
    w->update_rect.right = w->update_rect.bottom = 0;
    w->mouse_track_flags = 0;
    w->mouse_hover_time = 400;
    w->mouse_hover_start = 0;
    w->mouse_hover_origin.x = w->mouse_hover_origin.y = 0;
    w->used     = 1;
    w->shm_handle    = 0;
    w->compositor_id = 0;
    w->shm_pixels    = NULL;
    w->surface_width = 0;
    w->surface_height = 0;

    if (!window_set_text(w->handle, window_name, FALSE)) {
        release_window(w);
        return NULL;
    }
    if (cls->system_class && (cls->atom == 0x80 || cls->atom == 0x82)) {
        w->control = user32_control_create(cls->atom);
        if (!w->control) {
            release_window(w);
            SetLastError(8);
            return NULL;
        }
    }

    if (!message_only)
        place_window_in_z_order(w, (dwStyle & WS_CHILD) ? HWND_BOTTOM : HWND_TOP);

    /* OsitoK compositor: create backing shm surface + register window */
    if (!message_only && shm_create_surface &&
        (compositor_create_window_inactive || compositor_create_window) &&
        nWidth > 0 && nHeight > 0) {
        uint32_t sh = shm_create_surface((uint32_t)nWidth, (uint32_t)nHeight,
                                          SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ);
        if (sh) {
            w->shm_handle = sh;
            if (shm_map)
                w->shm_pixels = shm_map(sh);
            w->surface_width = nWidth;
            w->surface_height = nHeight;
            int screen_x, screen_y;
            window_screen_origin(w, &screen_x, &screen_y);
            if (compositor_create_window_inactive) {
                w->compositor_id = compositor_create_window_inactive(
                    sh, (int16_t)screen_x, (int16_t)screen_y,
                    (uint16_t)nWidth, (uint16_t)nHeight,
                    pid, window_name ? window_name : "");
            } else {
                w->compositor_id = compositor_create_window(
                    sh, (int16_t)screen_x, (int16_t)screen_y,
                    (uint16_t)nWidth, (uint16_t)nHeight,
                    pid, window_name ? window_name : "");
            }
            sync_all_window_compositor_state();
            serial_puts("[USER32]   compositor wid=");
            serial_puthex(w->compositor_id, 4);
            serial_puts(" shm=");
            serial_puthex(sh, 4);
            serial_puts("\n");
        }
    }

    serial_puts("[USER32]   hwnd=");
    serial_puthex((uint64_t)(ULONG_PTR)w->handle, 8);
    serial_puts(" size=");
    serial_puthex(nWidth, 4);
    serial_puts("x");
    serial_puthex(nHeight, 4);
    serial_puts("\n");

    /* WM_NCCREATE — the NT-correct window setup. Real Windows calls the
     * registered WndProc with WM_NCCREATE during CreateWindow, passing a
     * CREATESTRUCT whose lpCreateParams is the caller's `this` (lpParam, arg 12).
     * UT99's Window.dll WWindow::StaticProc reads lpCreateParams there, sets
     * WWindow->hWnd, and adds the WWindow to its global _Windows list. For every
     * later message StaticProc walks _Windows by hWnd to find the WWindow and
     * call its real WndProc. If we never send WM_NCCREATE, the WWindow is never
     * added to _Windows, so StaticProc can't map hwnd->WWindow and routes
     * WM_KEYDOWN to DefWindowProc — the game never receives keys and the menu
     * (Escape=ShowMenu) never opens. So send the real WM_NCCREATE rather than
     * poking WWindow->hWnd directly (which would also trip StaticProc's
     * check(!WWindow->hWnd) assertion). */
    {
        LRESULT nc_result = 1;
        LRESULT create_result = 0;
        if (!g_compat32_mode && w->wndproc) {
            CREATESTRUCTA cs = {
                .lpCreateParams = lpParam,
                .hInstance = hInstance,
                .hMenu = hMenu,
                .hwndParent = hWndParent,
                .cy = nHeight,
                .cx = nWidth,
                .y = Y,
                .x = X,
                .style = (LONG)dwStyle,
                .lpszName = lpWindowName,
                .lpszClass = lpClassName,
                .dwExStyle = dwExStyle,
            };
            nc_result = dispatch_wndproc(w->wndproc, w->handle, WM_NCCREATE, 0,
                                         (LPARAM)(ULONG_PTR)&cs);
            if (nc_result)
                create_result = dispatch_wndproc(w->wndproc, w->handle,
                                                 WM_CREATE, 0,
                                                 (LPARAM)(ULONG_PTR)&cs);
        }

        uint32_t wwindow_addr = (uint32_t)(ULONG_PTR)lpParam;
        if (g_compat32_mode && w->wndproc) {
            /* 32-bit CREATESTRUCTA (12 dwords) in PE32-accessible memory so the
             * 32-bit StaticProc can dereference lParam. */
            USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
            PVOID scratch = user32_callback_scratch_page(context);
            volatile uint32_t *cs = NULL;
            if (context && scratch &&
                context->create_depth < USER_CREATE_MAX_DEPTH) {
                cs = (volatile uint32_t *)(
                    (BYTE *)scratch + USER_CREATE_OFFSET +
                    context->create_depth * USER_CREATE_STRIDE);
            }
            if (cs) {
                cs[0]  = wwindow_addr;                       /* lpCreateParams */
                cs[1]  = (uint32_t)(ULONG_PTR)hInstance;     /* hInstance */
                cs[2]  = (uint32_t)(ULONG_PTR)hMenu;         /* hMenu */
                cs[3]  = (uint32_t)(ULONG_PTR)hWndParent;    /* hwndParent */
                cs[4]  = (uint32_t)nHeight;                  /* cy */
                cs[5]  = (uint32_t)nWidth;                   /* cx */
                cs[6]  = (uint32_t)Y;                        /* y */
                cs[7]  = (uint32_t)X;                        /* x */
                cs[8]  = dwStyle;                            /* style */
                cs[9]  = (uint32_t)(ULONG_PTR)lpWindowName;  /* lpszName */
                cs[10] = (uint32_t)(ULONG_PTR)lpClassName;   /* lpszClass */
                cs[11] = dwExStyle;                          /* dwExStyle */
                context->create_depth++;
                nc_result = dispatch_wndproc(w->wndproc, w->handle,
                                             WM_NCCREATE, 0,
                                             (LPARAM)(uintptr_t)cs);
                if (nc_result)
                    create_result = dispatch_wndproc(w->wndproc, w->handle,
                                                     WM_CREATE, 0,
                                                     (LPARAM)(uintptr_t)cs);
                context->create_depth--;
            } else {
                nc_result = 0;
            }
        }

        if (!nc_result || create_result == -1) {
            BOOL repair_activation = user32_foreground_active;
            HWND preferred = w->owner;
            if (nc_result && w->wndproc)
                dispatch_wndproc(w->wndproc, w->handle, WM_NCDESTROY, 0, 0);
            trace_window_release("create-failure", w, w->handle,
                                 (uint64_t)(ULONG_PTR)
                                 __builtin_return_address(0));
            release_window(w);
            sync_all_window_compositor_state();
            if (repair_activation)
                repair_user32_activation(preferred);
            return NULL;
        }
    }

    /* Real Windows sends WM_SIZE during CreateWindow when the window has a
     * non-zero size. UE1's viewport window is created already sized (e.g.
     * 640x480), so this is where it must learn SizeX/SizeY — there is no later
     * MoveWindow. Dispatched after the hWnd↔this association above so the
     * WndProc can resolve the window. */
    dispatch_wm_size(w);
    if (w->visible) {
        if (w->wndproc)
            dispatch_wndproc(w->wndproc, w->handle, WM_SHOWWINDOW, TRUE, 0);
        invalidate_window(w, NULL, TRUE);
        if (window_can_activate(w))
            dispatch_wm_activate(w);
    }

    return w->handle;
}

static void trace_window_release(const char *reason, const WINDOW *w,
                                 HWND root, uint64_t caller)
{
    if (!w) return;

    LOADED_MODULE *module = dll_find_module_by_address(
        (PVOID)(ULONG_PTR)caller);
    serial_puts("[USER32-WINDOW-RELEASE] reason=");
    serial_puts(reason ? reason : "unknown");
    serial_puts(" current_pid=");
    serial_putdec(GetCurrentProcessId());
    serial_puts(" current_tid=");
    serial_putdec(GetCurrentThreadId());
    serial_puts(" slot=");
    serial_putdec((uint64_t)(w - windows));
    serial_puts(" hwnd=");
    serial_puthex((uint64_t)(ULONG_PTR)w->handle, 16);
    serial_puts(" root=");
    serial_puthex((uint64_t)(ULONG_PTR)root, 16);
    serial_puts(" owner_pid=");
    serial_putdec(w->owner_pid);
    serial_puts(" owner_tid=");
    serial_putdec(w->owner_tid);
    serial_puts(" parent=");
    serial_puthex((uint64_t)(ULONG_PTR)w->parent, 16);
    serial_puts(" owner=");
    serial_puthex((uint64_t)(ULONG_PTR)w->owner, 16);
    serial_puts(" size=");
    serial_putdec((uint64_t)(uint32_t)w->width);
    serial_puts("x");
    serial_putdec((uint64_t)(uint32_t)w->height);
    serial_puts(" destroying=");
    serial_putdec(w->destroying ? 1 : 0);
    serial_puts(" visible=");
    serial_putdec(w->visible ? 1 : 0);
    serial_puts(" class='");
    serial_puts(w->class_name);
    serial_puts("' title='");
    serial_puts(w->title);
    serial_puts("' caller=");
    serial_puthex(caller, 16);
    serial_puts(" module=");
    if (module) {
        uint64_t base = (uint64_t)(ULONG_PTR)module->image.ImageBase;
        serial_puts(module->name);
        serial_puts("+");
        serial_puthex(caller - base, 8);
    } else {
        serial_puts("<none>");
    }
    serial_puts("\n");
}

static void release_window(WINDOW *w)
{
    if (!w || !w->used) return;
    user32_control_release(w->control);
    w->control = NULL;
    kfree(w->text);
    w->text = NULL;
    dialog_release_window(w->handle);
    extern void comctl32_release_window(DWORD owner_pid, HWND window);
    comctl32_release_window(w->owner_pid, w->handle);
    user_timer_release_window(w->handle);
    property_release_window(w->handle);
    menu_release_window_menus(w->handle,
        (w->style & WS_CHILD) ? NULL : w->menu, w->system_menu);
    if (w->compositor_id && compositor_destroy_window)
        compositor_destroy_window(w->compositor_id);
    gdi32_release_window_dc(w->handle);
    if (w->shm_handle && shm_unmap)
        shm_unmap(w->shm_handle);
    if (w->shm_handle && shm_destroy)
        shm_destroy(w->shm_handle);
    w->compositor_id = 0;
    w->shm_handle = 0;
    w->shm_pixels = NULL;
    w->surface_width = 0;
    w->surface_height = 0;
    w->paint_pending = 0;
    w->erase_pending = 0;
    w->mouse_track_flags = 0;
    w->mouse_hover_start = 0;
    if (focus_hwnd == w->handle) focus_hwnd = NULL;
    if (active_hwnd == w->handle) active_hwnd = NULL;
    if (capture_hwnd == w->handle) capture_hwnd = NULL;
    if (native_move.window == w->handle) {
        native_move.window = NULL;
        native_move.pointer_offset_x = 0;
        native_move.pointer_offset_y = 0;
    }
    if (caret_hwnd == w->handle) {
        caret_hwnd = NULL;
    }
    w->parent = NULL;
    w->owner = NULL;
    w->menu = NULL;
    w->system_menu = NULL;
    w->destroying = 0;
    w->used = 0;
}

static void destroy_window_tree(WINDOW *w, unsigned depth, HWND root,
                                uint64_t caller)
{
    if (!w || !w->used || w->destroying || depth >= MAX_WINDOWS)
        return;

    HWND handle = w->handle;
    HWND parent = w->parent;
    DWORD ex_style = w->ex_style;
    w->destroying = 1;
    w->visible = 0;
    w->style &= ~WS_VISIBLE;
    sync_all_window_compositor_state();

    if (w->wndproc)
        dispatch_wndproc(w->wndproc, handle, WM_DESTROY, 0, 0);

    HWND related[MAX_WINDOWS];
    int related_count = 0;
    for (int i = 0; i < window_count && related_count < MAX_WINDOWS; i++) {
        WINDOW *candidate = &windows[i];
        if (candidate->used && candidate != w && !candidate->destroying &&
            (candidate->parent == handle || candidate->owner == handle))
            related[related_count++] = candidate->handle;
    }

    for (int i = 0; i < related_count; i++) {
        WINDOW *candidate = find_window(related[i]);
        if (candidate && (candidate->parent == handle || candidate->owner == handle))
            destroy_window_tree(candidate, depth + 1, root, caller);
    }

    if (w->used && w->wndproc)
        dispatch_wndproc(w->wndproc, handle, WM_NCDESTROY, 0, 0);

    if (w->used) {
        trace_window_release(depth ? "destroy-related" : "destroy-direct",
                             w, root, caller);
        release_window(w);
    }

    if (parent && !(ex_style & 0x00000004U)) { /* WS_EX_NOPARENTNOTIFY */
        WINDOW *parent_window = find_window(parent);
        if (parent_window && parent_window->wndproc)
            dispatch_wndproc(parent_window->wndproc, parent, WM_PARENTNOTIFY,
                             WM_DESTROY, (LPARAM)(ULONG_PTR)handle);
    }
}

BOOL WINAPI DestroyWindow(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w || w->destroying) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }
    if (w->owner_tid != GetCurrentThreadId()) {
        SetLastError(5); /* ERROR_ACCESS_DENIED */
        return FALSE;
    }
    WINDOW *root = window_root(w, NULL);
    HWND preferred = root ? root->owner : NULL;
    BOOL repair_activation = user32_foreground_active;
    destroy_window_tree(w, 0, hWnd,
                        (uint64_t)(ULONG_PTR)__builtin_return_address(0));
    sync_all_window_compositor_state();
    if (repair_activation)
        repair_user32_activation(preferred);
    return TRUE;
}

void user32_release_thread(DWORD pid, DWORD tid)
{
    if (!pid || !tid)
        return;

    BOOL repair_activation = user32_foreground_active;
    WINDOW *active = window_root(find_window(active_hwnd), NULL);
    HWND preferred = active ? active->owner : NULL;

    if (clip_owner_pid == pid && clip_owner_tid == tid) {
        clip_active = 0;
        clip_owner_pid = 0;
        clip_owner_tid = 0;
    }
    cursor_release_counts(pid, tid);
    if (relative_pointer.owner_pid == pid &&
        relative_pointer.owner_tid == tid)
        relative_pointer_reset();

    msg_purge_thread(pid, tid);
    sent_message_release_thread(pid, tid);
    msg_wait_event_release_thread(pid, tid);
    user_timer_release_thread(pid, tid);
    user_hook_release_thread(pid, tid);

    for (int i = 0; i < window_count; i++) {
        WINDOW *window = &windows[i];
        if (!window->used || window->owner_pid != pid ||
            window->owner_tid != tid)
            continue;
        trace_window_release("thread-exit", window, window->handle,
                             (uint64_t)(ULONG_PTR)
                             __builtin_return_address(0));
        release_window(window);
    }

    /* Cross-thread ownership is legal. Detach surviving windows from handles
     * whose owning thread just exited so later traversal cannot dereference a
     * stale parent or owner. */
    for (int i = 0; i < window_count; i++) {
        WINDOW *window = &windows[i];
        if (!window->used)
            continue;
        if (window->parent && !find_window(window->parent))
            window->parent = NULL;
        if (window->owner && !find_window(window->owner))
            window->owner = NULL;
    }
    sync_all_window_compositor_state();
    if (repair_activation)
        repair_user32_activation(preferred);
}

void user32_release_process(DWORD pid)
{
    BOOL repair_activation = user32_foreground_active;
    WINDOW *active = window_root(find_window(active_hwnd), NULL);
    HWND preferred = active ? active->owner : NULL;
    extern void comctl32_release_process(DWORD owner_pid);
    comctl32_release_process(pid);
    if (clip_owner_pid == pid) {
        clip_active = 0;
        clip_owner_pid = 0;
        clip_owner_tid = 0;
    }
    cursor_release_counts(pid, 0);
    if (relative_pointer.owner_pid == pid)
        relative_pointer_reset();
    WINDOW *captured = capture_hwnd ? find_window(capture_hwnd) : NULL;
    WINDOW *moving = native_move.window ? find_window(native_move.window) : NULL;
    if (capture_hwnd && (!captured || captured->owner_pid == pid))
        capture_hwnd = NULL;
    if (native_move.window && (!moving || moving->owner_pid == pid)) {
        native_move.window = NULL;
        native_move.pointer_offset_x = 0;
        native_move.pointer_offset_y = 0;
    }
    msg_purge_process(pid);
    sent_message_release_process(pid);
    msg_wait_event_release_process(pid);
    user_timer_release_process(pid);
    user_hook_release_process(pid);
    icon_release_process(pid);
    for (int i = 0; i < window_count; i++)
        if (windows[i].used && windows[i].owner_pid == pid) {
            trace_window_release("process-exit", &windows[i],
                                 windows[i].handle,
                                 (uint64_t)(ULONG_PTR)
                                 __builtin_return_address(0));
            release_window(&windows[i]);
        }
    if (user_display_mode.active && user_display_mode.owner_pid == pid)
        memset(&user_display_mode, 0, sizeof(user_display_mode));
    sync_all_window_compositor_state();
    if (repair_activation)
        repair_user32_activation(preferred);
    for (int i = 0; i < wndclass_count; i++) {
        WNDCLASS_BLOCK *block = wndclass_blocks[i / WNDCLASS_BLOCK_SIZE];
        WNDCLASS_ENTRY *entry = block
            ? &block->entries[i % WNDCLASS_BLOCK_SIZE] : NULL;
        if (entry && entry->used && entry->owner_pid == pid)
            entry->used = 0;
    }
    for (int i = 0; i < MAX_DEFER_WINDOW_POS; i++)
        if (defer_window_sets[i].used && defer_window_sets[i].owner_pid == pid)
            defer_window_sets[i].used = 0;
    user_object_release_process(pid);
    menu_release_process(pid);
}

BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;

    BOOL repair_activation = user32_foreground_active;
    WINDOW *root = window_root(w, NULL);
    HWND preferred = root ? root->owner : NULL;
    int was_visible = w->visible;
    DWORD old_state = w->style & (WS_MINIMIZE | WS_MAXIMIZE);
    int activates = nCmdShow != SW_SHOWNOACTIVATE &&
                    nCmdShow != SW_SHOWMINNOACTIVE &&
                    nCmdShow != SW_SHOWNA;

    if (w->message_only)
        return was_visible;

    if (nCmdShow == SW_HIDE) {
        w->visible = 0;
    } else {
        w->visible = 1;
        if (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_MINIMIZE ||
            nCmdShow == SW_SHOWMINNOACTIVE || nCmdShow == SW_FORCEMINIMIZE) {
            update_normal_rect(w);
            w->style |= WS_MINIMIZE;
            w->style &= ~WS_MAXIMIZE;
            w->show_cmd = SW_SHOWMINIMIZED;
        } else if (nCmdShow == SW_SHOWMAXIMIZED) {
            update_normal_rect(w);
            w->style |= WS_MAXIMIZE;
            w->style &= ~WS_MINIMIZE;
            w->show_cmd = SW_SHOWMAXIMIZED;
            w->x = 0;
            w->y = 0;
            WINDOW *parent = find_window(w->parent);
            w->width = parent ? parent->width : GetSystemMetrics(SM_CXSCREEN);
            w->height = parent ? parent->height : GetSystemMetrics(SM_CYSCREEN);
            sync_window_surface(w, w->width, w->height);
        } else if (nCmdShow == SW_SHOWNORMAL || nCmdShow == SW_RESTORE ||
                   nCmdShow == SW_SHOWDEFAULT) {
            if (old_state) {
                w->x = w->normal_rect.left;
                w->y = w->normal_rect.top;
                w->width = w->normal_rect.right - w->normal_rect.left;
                w->height = w->normal_rect.bottom - w->normal_rect.top;
                sync_window_surface(w, w->width, w->height);
            }
            w->style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
            w->show_cmd = SW_SHOWNORMAL;
        }
    }

    if (w->visible)
        w->style |= WS_VISIBLE;
    else
        w->style &= ~WS_VISIBLE;

    sync_all_window_compositor_state();

    if (was_visible != w->visible && w->wndproc)
        dispatch_wndproc(w->wndproc, w->handle, WM_SHOWWINDOW,
                         w->visible ? TRUE : FALSE, 0);

    /* On first show, real Windows posts WM_SIZE to the wndproc. UE1's viewport
     * may rely on this (rather than the WM_SIZE during CreateWindow) to pick up
     * SizeX/SizeY before the render device is set up. compat32_callback_args
     * handles the 64→32 switch. */
    DWORD new_state = w->style & (WS_MINIMIZE | WS_MAXIMIZE);
    if (old_state != new_state)
        menu_sync_system_window(hWnd);
    if (was_visible != w->visible || old_state != new_state) {
        if (show_trace_count++ < 128) {
            serial_puts("[USER32] ShowWindow hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)hWnd, 8);
            serial_puts(" class=");
            serial_puts(w->class_name);
            serial_puts(" cmd=");
            serial_putdec((uint64_t)(uint32_t)nCmdShow);
            serial_puts(" visible=");
            serial_putdec((uint64_t)w->visible);
            serial_puts(" effective=");
            serial_putdec((uint64_t)(window_should_render(w) != FALSE));
            serial_puts(" parent=0x");
            serial_puthex((uint64_t)(ULONG_PTR)w->parent, 8);
            serial_puts(" owner=0x");
            serial_puthex((uint64_t)(ULONG_PTR)w->owner, 8);
            serial_puts("\n");
        }
    }
    if ((!was_visible && w->visible) || old_state != new_state) {
        dispatch_wm_size(w);
        if (!(w->style & WS_MINIMIZE))
            invalidate_window(w, NULL, TRUE);
    }

    if (repair_activation)
        repair_user32_activation(preferred);

    if (activates && window_can_activate(w)) {
        place_window_in_z_order(w, HWND_TOP);
        dispatch_wm_activate(w);
    }

    return was_visible;
}

BOOL WINAPI UpdateWindow(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;

    if (w->wndproc && w->visible && w->paint_pending)
        dispatch_wndproc(w->wndproc, hWnd, WM_PAINT, 0, 0);

    return TRUE;
}

static BOOL window_set_text(HWND hWnd, PCVOID input, BOOL wide)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    SIZE_T length = 0;
    if (input) {
        if (wide) {
            while (((PCWSTR)input)[length]) {
                if (++length >= 0x7FFFFFFEU) return FALSE;
            }
        } else {
            int count = MultiByteToWideChar(0, 0, input, -1, NULL, 0);
            if (count <= 0) return FALSE;
            length = (SIZE_T)count - 1;
        }
    }
    PWSTR text = kcalloc(length + 1, sizeof(WCHAR));
    if (!text) { SetLastError(8); return FALSE; }
    if (input && wide) memcpy(text, input, length * sizeof(WCHAR));
    else if (input && !MultiByteToWideChar(0, 0, input, -1, text, (int)length+1)) {
        kfree(text);
        return FALSE;
    }
    PWSTR previous = w->text;
    w->text = text;
    kfree(previous);
    /* Only the compositor/debug caption has a bounded presentation buffer. */
    int shown = length < 255 ? (int)length : 255;
    int bytes = shown ? WideCharToMultiByte(0, 0, text, shown,
        w->title, sizeof(w->title)-1, NULL, NULL) : 0;
    w->title[bytes > 0 ? bytes : 0] = 0;
    if (w->compositor_id && compositor_set_title)
        compositor_set_title(w->compositor_id, w->title);
    invalidate_window(w, NULL, TRUE);
    return TRUE;
}

PWSTR user32_copy_window_text(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return NULL;
    SIZE_T count = 0;
    if (w->text) while (w->text[count]) count++;
    PWSTR copy = kcalloc(count+1, sizeof(WCHAR));
    if (copy && count) memcpy(copy, w->text, count*sizeof(WCHAR));
    return copy;
}

static LRESULT window_text_message(HWND hWnd, DWORD message,
                                    WPARAM capacity, LPARAM buffer, BOOL wide)
{
    if (message == 0x000C) return window_set_text(hWnd, (PCVOID)buffer, wide);
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;
    static const WCHAR empty[] = {0};
    PCWSTR text = w->text ? w->text : empty;
    int length = 0;
    while (text[length]) length++;
    if (message == 0x000E) return wide ? length :
        (length ? WideCharToMultiByte(0, 0, text, length, NULL, 0, NULL, NULL) : 0);
    if (!buffer || !capacity || capacity > 0x7FFFFFFFU) return 0;
    if (wide) {
        if ((UINT)length >= capacity) length = (int)capacity-1;
        memcpy((PVOID)buffer, text, length*sizeof(WCHAR));
        ((PWSTR)buffer)[length] = 0;
        return length;
    }
    int out = 0;
    for (int i = 0; i < length; i++) {
        char bytes[8];
        int count = WideCharToMultiByte(0, 0, text+i, 1, bytes, sizeof(bytes), NULL, NULL);
        if (count <= 0 || (UINT)(out+count) >= capacity) break;
        memcpy((PSTR)buffer+out, bytes, count);
        out += count;
    }
    ((PSTR)buffer)[out] = 0;
    return out;
}

BOOL WINAPI SetWindowTextA(HWND hWnd, PCSTR lpString)
{
    return (BOOL)SendMessageA(hWnd, 0x000C, 0, (LPARAM)lpString);
}

static BOOL WINAPI SetWindowTextW_k32(HWND hWnd, PCWSTR lpString)
{
    return (BOOL)SendMessageW(hWnd, 0x000C, 0, (LPARAM)lpString);
}

int WINAPI GetWindowTextLengthA(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;

    return (int)(w->owner_pid == GetCurrentProcessId()
        ? SendMessageA(hWnd, 0x000E, 0, 0)
        : window_text_message(hWnd, 0x000E, 0, 0, FALSE));
}

int WINAPI GetWindowTextLengthW(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;
    return (int)(w->owner_pid == GetCurrentProcessId()
        ? SendMessageW(hWnd, 0x000E, 0, 0)
        : window_text_message(hWnd, 0x000E, 0, 0, TRUE));
}

BOOL WINAPI IsWindowUnicode(HWND hWnd)
{
    WINDOW *window = find_window(hWnd);
    return window ? window->unicode : FALSE;
}

int WINAPI GetWindowTextA(HWND hWnd, PSTR text, int max_count)
{
    WINDOW *w = find_window(hWnd);
    if (!text || max_count <= 0) return 0;
    text[0] = 0;
    if (!w) return 0;
    return (int)(w->owner_pid == GetCurrentProcessId()
        ? SendMessageA(hWnd, 0x000D, max_count, (LPARAM)text)
        : window_text_message(hWnd, 0x000D, max_count, (LPARAM)text, FALSE));
}

int WINAPI GetWindowTextW(HWND hWnd, PWSTR text, int max_count)
{
    WINDOW *w = find_window(hWnd);
    if (!text || max_count <= 0) return 0;
    text[0] = 0;
    if (!w) return 0;
    return (int)(w->owner_pid == GetCurrentProcessId()
        ? SendMessageW(hWnd, 0x000D, max_count, (LPARAM)text)
        : window_text_message(hWnd, 0x000D, max_count, (LPARAM)text, TRUE));
}

BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter,
                         int X, int Y, int cx, int cy, DWORD uFlags)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;

    WINDOWPOS position = {
        .hwnd = hWnd,
        .hwndInsertAfter = hWndInsertAfter,
        .x = X,
        .y = Y,
        .cx = cx,
        .cy = cy,
        .flags = uFlags,
    };
    BOOL sent_windowpos = FALSE;
    if (!(uFlags & SWP_NOSENDCHANGING))
        sent_windowpos = dispatch_windowpos_message(
            w, WM_WINDOWPOSCHANGING, &position, TRUE);

    hWndInsertAfter = position.hwndInsertAfter;
    X = position.x;
    Y = position.y;
    cx = position.cx;
    cy = position.cy;
    uFlags = position.flags;

    if (!(uFlags & SWP_NOZORDER) &&
        !place_window_in_z_order(w, hWndInsertAfter))
        return FALSE;

    int moved = 0, resized = 0;
    if (!(uFlags & SWP_NOMOVE)) {
        moved = w->x != X || w->y != Y;
        w->x = X;
        w->y = Y;
    }
    if (!(uFlags & SWP_NOSIZE)) {
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        resized = w->width != cx || w->height != cy;
        w->width = cx; w->height = cy;
        sync_window_surface(w, cx, cy);
    }

    if (moved || resized)
        update_normal_rect(w);

    if (uFlags & SWP_SHOWWINDOW)
        ShowWindow(hWnd, (uFlags & SWP_NOACTIVATE) ? SW_SHOWNA : SW_SHOW);
    else if (uFlags & SWP_HIDEWINDOW)
        ShowWindow(hWnd, SW_HIDE);

    sync_all_window_compositor_state();

    position.x = w->x;
    position.y = w->y;
    position.cx = w->width;
    position.cy = w->height;
    position.flags = uFlags;
    if (moved || resized ||
        (uFlags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED)))
        sent_windowpos = dispatch_windowpos_message(
            w, WM_WINDOWPOSCHANGED, &position, FALSE) || sent_windowpos;

    /* A pointer-bearing notification cannot be queued for a foreign owner.
     * Preserve the integral WM_SIZE fallback for that uncommon path. */
    if (resized && !sent_windowpos)
        dispatch_wm_size(w);
    if ((moved || resized) && !(uFlags & SWP_NOREDRAW) &&
        window_should_render(w))
        invalidate_window(w, NULL, TRUE);

    if (!(uFlags & SWP_NOACTIVATE) && w->visible && !w->parent)
        SetFocus(hWnd);
    return TRUE;
}

BOOL WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint)
{
    return SetWindowPos(hWnd, HWND_TOP, X, Y, nWidth, nHeight,
                        SWP_NOZORDER | (bRepaint ? 0 : SWP_NOREDRAW));
}

static DEFER_WINDOW_SET *find_defer_window_set(HDWP handle)
{
    for (int i = 0; i < MAX_DEFER_WINDOW_POS; i++)
        if (defer_window_sets[i].used && defer_window_sets[i].handle == handle)
            return &defer_window_sets[i];
    return NULL;
}

HDWP WINAPI BeginDeferWindowPos(int nNumWindows)
{
    if (nNumWindows < 0 || nNumWindows > MAX_WINDOWS) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    for (int i = 0; i < MAX_DEFER_WINDOW_POS; i++) {
        DEFER_WINDOW_SET *set = &defer_window_sets[i];
        if (set->used) continue;
        set->used = 1;
        set->handle = (HDWP)next_hdwp++;
        set->owner_pid = GetCurrentProcessId();
        set->count = 0;
        return set->handle;
    }
    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return NULL;
}

HDWP WINAPI DeferWindowPos(HDWP hWinPosInfo, HWND hWnd,
                           HWND hWndInsertAfter, int x, int y,
                           int cx, int cy, UINT uFlags)
{
    DEFER_WINDOW_SET *set = find_defer_window_set(hWinPosInfo);
    if (!set || set->owner_pid != GetCurrentProcessId() ||
        !find_window(hWnd) || set->count >= MAX_WINDOWS) {
        if (set) set->used = 0;
        SetLastError(87);
        return NULL;
    }

    DEFER_WINDOW_OP *op = &set->ops[set->count++];
    op->window = hWnd;
    op->insert_after = hWndInsertAfter;
    op->x = x;
    op->y = y;
    op->width = cx;
    op->height = cy;
    op->flags = uFlags;
    return hWinPosInfo;
}

BOOL WINAPI EndDeferWindowPos(HDWP hWinPosInfo)
{
    DEFER_WINDOW_SET *set = find_defer_window_set(hWinPosInfo);
    if (!set || set->owner_pid != GetCurrentProcessId()) {
        SetLastError(87);
        return FALSE;
    }

    BOOL success = TRUE;
    defer_sync_depth++;
    for (int i = 0; i < set->count; i++) {
        DEFER_WINDOW_OP *op = &set->ops[i];
        if (!SetWindowPos(op->window, op->insert_after,
                          op->x, op->y, op->width, op->height, op->flags))
            success = FALSE;
    }
    defer_sync_depth--;
    set->used = 0;
    sync_all_window_compositor_state();
    return success;
}

/* DirectDraw owns painting while a cooperative window is exclusive. Geometry
 * still follows the normal USER32 path so the owner receives the synchronous
 * WM_WINDOWPOSCHANGING/CHANGED sequence generated by Windows. */
BOOL user32_configure_directdraw_window(HWND hWnd, BOOL exclusive,
                                         BOOL allow_window_changes,
                                         int width, int height)
{
    WINDOW *win = find_window(hWnd);
    if (!win) return FALSE;
    if (exclusive && allow_window_changes && (width <= 0 || height <= 0))
        return FALSE;

    win->directdraw_exclusive = exclusive != FALSE;

    if (exclusive) {
        win->paint_pending = 0;
        win->erase_pending = 0;
        win->update_rect.left = win->update_rect.top = 0;
        win->update_rect.right = win->update_rect.bottom = 0;
    } else if (win->visible && !(win->style & WS_MINIMIZE)) {
        invalidate_window(win, NULL, TRUE);
    }

    if (!exclusive || !allow_window_changes) {
        sync_all_window_compositor_state();
        return TRUE;
    }

    BOOL result = SetWindowPos(hWnd, HWND_TOP, 0, 0, width, height,
                               SWP_NOZORDER | SWP_NOACTIVATE);
    if (result) {
        /* The primary surface, rather than GDI, supplies the exclusive frame. */
        win->paint_pending = 0;
        win->erase_pending = 0;
        win->update_rect.left = win->update_rect.top = 0;
        win->update_rect.right = win->update_rect.bottom = 0;
    }
    if (!result)
        sync_all_window_compositor_state();
    return result;
}

/* ── Message Loop ──────────────────────────────────────────── */

static DWORD service_message_pump(void)
{
    DWORD poll_interval = 0xFFFFFFFFU;

    if (ddraw_present_hook)
        ddraw_present_hook();
    if (ddraw_present_poll_interval) {
        DWORD ddraw_interval = ddraw_present_poll_interval();
        if (ddraw_interval && ddraw_interval < poll_interval)
            poll_interval = ddraw_interval;
    }

    BOOL compositor_active = compositor_is_running && compositor_is_running();
    if (xhci_poll && dispatch_depth == 0 && !compositor_active) {
        xhci_poll();
        if (poll_interval > 10)
            poll_interval = 10;
    }

    return poll_interval;
}

BOOL WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                          DWORD wMsgFilterMax, DWORD wRemoveMsg)
{

    if (!lpMsg) {
        SetLastError(998); /* ERROR_NOACCESS */
        return FALSE;
    }
    if (hWnd && hWnd != (HWND)(ULONG_PTR)-1 && !find_window(hWnd)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }

    /* Nonqueued SendMessage traffic runs on the owner thread before posted
     * messages are exposed to the application. */
    sent_message_dispatch_current();

    /* Software scanout and headless input are properties of the active
     * backends, not of the caller's pointer width. */
    (void)service_message_pump();

    /* First-ever PeekMessage: bootstrap the queued-state snapshot so GetKeyState
     * has a baseline (otherwise key_state_at_msg is zeroes → all keys "up"). */
    {
        static int boot_snap = 0;
        if (!boot_snap) {
            for (int ki = 0; ki < 256; ki++) key_state_at_msg[ki] = key_state[ki];
            boot_snap = 1;
        }
    }

    MSG retrieved;
    if (!msg_retrieve(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax,
                      (wRemoveMsg & PM_REMOVE) != 0, &retrieved)) {
        if (write_pending_paint(lpMsg, hWnd,
                                wMsgFilterMin, wMsgFilterMax)) {
            user_hook_notify_getmessage(
                lpMsg, (wRemoveMsg & PM_REMOVE) != 0);
            return TRUE;
        }
        if (user_timer_retrieve(lpMsg, hWnd,
                                wMsgFilterMin, wMsgFilterMax,
                                (wRemoveMsg & PM_REMOVE) != 0, &retrieved)) {
            user_hook_notify_getmessage(
                lpMsg, (wRemoveMsg & PM_REMOVE) != 0);
            return TRUE;
        }
        return FALSE;
    }

    /* Snapshot key_state at message-retrieval time (NT GetKeyState semantics).
     * Do this before retrieval so the snapshot reflects the state at the
     * moment this message was retrieved — matching what NT's GetKeyState
     * would return if called right after PeekMessage/GetMessage. */
    for (int ki = 0; ki < 256; ki++) key_state_at_msg[ki] = key_state[ki];

    /* ── Phase 1 diagnostic: log messages retrieved during capture ── */
    {
        static int diag_n = 0;
        if (u32_input_diagnostics_active() && diag_n < 180 &&
            relative_pointer_mode_active()) {
            MSG *src = &retrieved;
            serial_puts("[CAP-MSG] Peek: wm=0x");
            serial_puthex((uint64_t)src->message, 4);
            serial_puts(" hWnd=0x"); serial_puthex((uint64_t)(ULONG_PTR)src->hwnd, 8);
            if (src->message == WM_MOUSEMOVE || src->message == WM_KEYDOWN ||
                src->message == WM_KEYUP || src->message == WM_CHAR)
            {
                serial_puts(" wp=0x"); serial_puthex((uint64_t)(uint32_t)src->wParam, 8);
                serial_puts(" lp=0x"); serial_puthex((uint64_t)src->lParam, 8);
            }
            serial_puts(" focus=0x"); serial_puthex((uint64_t)(ULONG_PTR)focus_hwnd, 8);
            serial_puts(" target=0x"); serial_puthex((uint64_t)(ULONG_PTR)input_target(), 8);
            serial_puts(" cap=0x"); serial_puthex((uint64_t)(ULONG_PTR)capture_hwnd, 8);
            serial_puts("\n");
            diag_n++;
        }
    }

    user_hook_notify_getmessage(lpMsg, (wRemoveMsg & PM_REMOVE) != 0);
    return TRUE;
}

BOOL WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                         DWORD wMsgFilterMax)
{
    const DWORD wait_failed = 0xFFFFFFFFU;
    const DWORD infinite = 0xFFFFFFFFU;

    if (!lpMsg) {
        SetLastError(998); /* ERROR_NOACCESS */
        return (BOOL)-1;
    }
    if (hWnd && hWnd != (HWND)(ULONG_PTR)-1 && !find_window(hWnd)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return (BOOL)-1;
    }

    HANDLE queue_event = msg_wait_event_for_current_thread();
    for (;;) {
        extern void win32_main_termination_checkpoint(void);
        win32_main_termination_checkpoint();
        sent_message_dispatch_current();
        MSG retrieved;
        if (msg_retrieve(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax,
                         TRUE, &retrieved)) {
            user_hook_notify_getmessage(lpMsg, TRUE);
            msg_read_from(lpMsg, &retrieved);
            return retrieved.message == WM_QUIT ? FALSE : TRUE;
        }

        if (write_pending_paint(lpMsg, hWnd,
                                wMsgFilterMin, wMsgFilterMax)) {
            user_hook_notify_getmessage(lpMsg, TRUE);
            return TRUE;
        }
        if (user_timer_retrieve(lpMsg, hWnd,
                                wMsgFilterMin, wMsgFilterMax,
                                TRUE, &retrieved)) {
            user_hook_notify_getmessage(lpMsg, TRUE);
            return TRUE;
        }

#ifdef TEST_HARNESS
        /* Host harnesses have no scheduler or asynchronous input source. */
        msg_write_to(lpMsg, NULL, WM_QUIT, 0, 0, 0, 0, 0);
        return FALSE;
#else
        DWORD wait_ms = infinite;
        user_timer_next_timeout(hWnd, wMsgFilterMin,
                                wMsgFilterMax, &wait_ms);
        if (!wait_ms)
            continue;

        extern void sched_yield(void);
        DWORD pump_wait_ms = service_message_pump();

        if (!queue_event) {
            queue_event = msg_wait_event_for_current_thread();
            if (!queue_event) {
                sched_yield();
                continue;
            }
        }

        /* The auto-reset event is only a wakeup edge. Recheck every source
         * after obtaining it so a post racing event creation cannot be lost. */
        if (msg_queue_has_match(hWnd, wMsgFilterMin, wMsgFilterMax) ||
            (message_filter_allows(WM_PAINT,
                                   wMsgFilterMin, wMsgFilterMax) &&
             paint_pending_window(hWnd)))
            continue;
        DWORD next_timer_ms;
        if (user_timer_next_timeout(hWnd, wMsgFilterMin, wMsgFilterMax,
                                    &next_timer_ms)) {
            if (!next_timer_ms)
                continue;
            if (wait_ms == infinite || next_timer_ms < wait_ms)
                wait_ms = next_timer_ms;
        }
        if (pump_wait_ms != infinite &&
            (wait_ms == infinite || pump_wait_ms < wait_ms))
            wait_ms = pump_wait_ms;

        DWORD result = WaitForSingleObject(queue_event, wait_ms);
        if (result == wait_failed)
            return (BOOL)-1;
#endif
    }
}

BOOL WINAPI TranslateMessage(const MSG *lpMsg)
{
    /* Read MSG from caller buffer (handles 32-bit vs 64-bit layout) */
    MSG m;
    msg_read_from(lpMsg, &m);

    /* System keystrokes translate to WM_SYSCHAR, not ordinary text input.
     * VK_PACKET carries a UTF-16 code unit in the scan-code word. */
    BOOL queued = FALSE;
    WORD translated = 0;
    if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
        DWORD char_message = m.message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR;
        DWORD vk = (DWORD)m.wParam;
        if (vk == VK_PACKET) {
            WORD ch = (WORD)(((ULONG_PTR)m.lParam >> 16) & 0xFFFF);
            if (ch) {
                translated = ch;
                queued = msg_enqueue(m.hwnd, char_message,
                                     (WPARAM)ch, m.lParam);
            }
        } else {
            BYTE state[256];
            WORD ch = 0;
            GetKeyboardState(state);
            if (ToAscii(vk, (DWORD)((m.lParam >> 16) & 0xFF),
                        state, &ch, 0) > 0) {
                translated = ch;
                queued = msg_enqueue(m.hwnd, char_message,
                                     (WPARAM)ch, m.lParam);
            }
        }

        static unsigned trace_count;
        if (trace_count++ < 96) {
            serial_puts("[KEY-MSG] translate hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)m.hwnd, 8);
            serial_puts(" msg=0x");
            serial_puthex(m.message, 4);
            serial_puts(" vk=0x");
            serial_puthex((uint64_t)m.wParam, 4);
            serial_puts(" char=0x");
            serial_puthex(translated, 4);
            serial_puts(" queued=");
            serial_putdec(queued ? 1 : 0);
            serial_puts("\n");
        }
    }
    return TRUE;
}

static LRESULT invoke_wndproc(WNDPROC wndproc, HWND hWnd, DWORD Msg,
                              WPARAM wParam, LPARAM lParam)
{
    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(uintptr_t)hWnd,
            (uint32_t)Msg,
            (uint32_t)wParam,
            (uint32_t)lParam
        };
        uint32_t stack_top = compat32_current_user_stack_top();
        uint32_t result = stack_top
            ? compat32_callback_args_on_stack(
                (uint32_t)(uintptr_t)wndproc, 4, args, stack_top)
            : compat32_callback_args(
                (uint32_t)(uintptr_t)wndproc, 4, args);
        /* LRESULT is signed and pointer-sized in the calling application. */
        return (LRESULT)(int32_t)result;
    }
    return wndproc(hWnd, Msg, wParam, lParam);
}

static LRESULT dispatch_wndproc(WNDPROC wndproc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam)
{
    dispatch_depth++;
    user_hook_notify_callwndproc(hWnd, Msg, wParam, lParam);
    LRESULT ret = invoke_wndproc(wndproc, hWnd, Msg, wParam, lParam);
    user_hook_notify_callwndprocret(ret, hWnd, Msg, wParam, lParam);
    dispatch_depth--;
    return ret;
}

static LRESULT dispatch_wndproc_encoded(WNDPROC proc, HWND window, DWORD msg,
                                         WPARAM wp, LPARAM lp, BOOL unicode)
{
    USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
    if (!context) { SetLastError(8); return 0; }
    BOOL previous = context->unicode_message;
    context->unicode_message = unicode;
    LRESULT result = dispatch_wndproc(proc, window, msg, wp, lp);
    context->unicode_message = previous;
    return result;
}

static int sent_message_dispatch_current(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    int dispatched = 0;

    for (;;) {
        int slot = -1;
        uint64_t oldest = UINT64_MAX;
        SENT_MESSAGE request;
        uint64_t flags = sent_message_lock_irqsave();
        for (int i = 0; i < SENT_MESSAGE_SLOTS; i++) {
            SENT_MESSAGE *candidate = &sent_messages[i];
            if (candidate->state == SENT_MESSAGE_PENDING &&
                candidate->target_pid == pid &&
                candidate->target_tid == tid &&
                candidate->sequence < oldest) {
                oldest = candidate->sequence;
                slot = i;
            }
        }
        if (slot >= 0) {
            sent_messages[slot].state = SENT_MESSAGE_PROCESSING;
            request = sent_messages[slot];
        }
        sent_message_unlock_irqrestore(flags);
        if (slot < 0)
            break;

        LRESULT result = 0;
        WINDOW *window = find_window(request.window);
        if (window && window->owner_pid == pid && window->owner_tid == tid) {
            if (window->wndproc)
                result = dispatch_wndproc_encoded(window->wndproc, request.window,
                                          request.message, request.wparam,
                                          request.lparam, request.unicode);
            else
                result = DefWindowProcA(request.window, request.message,
                                        request.wparam, request.lparam);
        }

        HANDLE event = NULL;
        DWORD sender_pid = 0;
        flags = sent_message_lock_irqsave();
        SENT_MESSAGE *sent = &sent_messages[slot];
        if (sent->generation == request.generation) {
            if (sent->state == SENT_MESSAGE_ABANDONED) {
                sent->state = SENT_MESSAGE_FREE;
            } else if (sent->state == SENT_MESSAGE_PROCESSING) {
                sent->result = result;
                sent->state = SENT_MESSAGE_DONE;
                event = sent->completion_event;
                sender_pid = sent->sender_pid;
            }
        }
        sent_message_unlock_irqrestore(flags);

        if (event)
            ntsync_set_event_for_process(event, sender_pid, NULL);
        dispatched++;
    }
    return dispatched;
}

static LPARAM screen_point_lparam(POINT point)
{
    return (LPARAM)(WORD)point.x | ((LPARAM)(WORD)point.y << 16);
}

static void native_move_finish(POINT point, BOOL send_button_up)
{
    HWND handle = native_move.window;
    WINDOW *window = find_window(handle);
    native_move.window = NULL;

    if (window && window->owner_pid == GetCurrentProcessId() &&
        window->owner_tid == GetCurrentThreadId() && window->wndproc) {
        if (send_button_up)
            dispatch_wndproc(window->wndproc, handle, WM_NCLBUTTONUP,
                             HTCAPTION, screen_point_lparam(point));
        dispatch_wndproc(window->wndproc, handle, WM_EXITSIZEMOVE, 0, 0);
    }

    if (capture_hwnd == handle)
        ReleaseCapture();
}

static BOOL native_move_begin(HWND handle, POINT point)
{
    WINDOW *window = find_window(handle);
    if (!window)
        return FALSE;
    window = window_root(window, NULL);
    if (!window || window->owner_pid != GetCurrentProcessId() ||
        window->owner_tid != GetCurrentThreadId())
        return FALSE;

    if (native_move.window && native_move.window != window->handle)
        native_move_finish(point, FALSE);
    if (native_move.window == window->handle)
        return TRUE;

    int screen_x = 0;
    int screen_y = 0;
    window_screen_origin(window, &screen_x, &screen_y);
    native_move.window = window->handle;
    native_move.pointer_offset_x = point.x - screen_x;
    native_move.pointer_offset_y = point.y - screen_y;

    SetCapture(window->handle);
    if (GetCapture() != window->handle) {
        native_move.window = NULL;
        return FALSE;
    }

    if (window->wndproc)
        dispatch_wndproc(window->wndproc, window->handle,
                         WM_ENTERSIZEMOVE, 0, 0);
    return TRUE;
}

static void native_move_update(POINT point)
{
    WINDOW *window = find_window(native_move.window);
    if (!window) {
        native_move.window = NULL;
        return;
    }
    if (window->owner_pid != GetCurrentProcessId() ||
        window->owner_tid != GetCurrentThreadId())
        return;

    int x = point.x - native_move.pointer_offset_x;
    int y = point.y - native_move.pointer_offset_y;
    SetWindowPos(window->handle, HWND_TOP, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL dispatch_nonclient_press(const MSG *message, LRESULT *result)
{
    WINDOW *target = message ? find_window(message->hwnd) : NULL;
    WINDOW *host = target;
    if (target && target->parent) {
        WINDOW *parent = find_window(target->parent);
        if (parent && parent->wndproc)
            host = parent;
    }
    if (!host || !host->wndproc ||
        host->owner_pid != GetCurrentProcessId() ||
        host->owner_tid != GetCurrentThreadId())
        return FALSE;

    LPARAM screen_position = screen_point_lparam(message->pt);
    LRESULT hit = dispatch_wndproc(host->wndproc, host->handle,
                                   WM_NCHITTEST, 0, screen_position);
    if (hit != HTCAPTION)
        return FALSE;

    *result = dispatch_wndproc(host->wndproc, host->handle,
                               WM_NCLBUTTONDOWN, HTCAPTION,
                               screen_position);
    return TRUE;
}

LRESULT WINAPI DispatchMessageA(const MSG *lpMsg)
{
    /* Read MSG from caller buffer (handles 32-bit vs 64-bit layout) */
    MSG m;
    msg_read_from(lpMsg, &m);

    if (m.message == WM_QUIT)
        return 0;

    BOOL input_message =
        msg_last_retrieved_source(GetCurrentProcessId(),
                                  GetCurrentThreadId()) == MSG_SOURCE_INPUT;
    if (input_message && native_move.window) {
        if (m.message == WM_MOUSEMOVE) {
            native_move_update(m.pt);
            return 0;
        }
        if (m.message == WM_LBUTTONUP) {
            native_move_finish(m.pt, TRUE);
            return 0;
        }
    }

    if (input_message && m.message == WM_LBUTTONDOWN) {
        LRESULT result = 0;
        if (dispatch_nonclient_press(&m, &result))
            return result;
    }

    if (m.message >= WM_KEYDOWN && m.message <= WM_SYSKEYUP) {
        static unsigned trace_count;
        if (trace_count++ < 96) {
            serial_puts("[KEY-MSG] dispatch hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)m.hwnd, 8);
            serial_puts(" msg=0x");
            serial_puthex(m.message, 4);
            serial_puts(" wp=0x");
            serial_puthex((uint64_t)m.wParam, 8);
            serial_puts("\n");
        }
    }

    if (m.message == WM_TIMER && m.lParam) {
        if (g_compat32_mode) {
            extern uint32_t compat32_callback_args(uint32_t func, int nargs,
                                                   const uint32_t *args);
            uint32_t args[4] = {
                (uint32_t)(ULONG_PTR)m.hwnd,
                WM_TIMER,
                (uint32_t)m.wParam,
                m.time,
            };
            compat32_callback_args((uint32_t)(ULONG_PTR)m.lParam, 4, args);
        } else {
            typedef void (WINAPI *USER_TIMER_PROC)(HWND, UINT,
                                                   ULONG_PTR, DWORD);
            USER_TIMER_PROC callback =
                (USER_TIMER_PROC)(ULONG_PTR)m.lParam;
            callback(m.hwnd, WM_TIMER, m.wParam, m.time);
        }
        return 0;
    }

    {
        static int n = 0;
        if (u32_input_diagnostics_active() && n < 180 &&
            relative_pointer_mode_active() &&
            (m.message == WM_MOUSEMOVE || m.message == WM_KEYDOWN ||
             m.message == WM_KEYUP || m.message == WM_LBUTTONDOWN ||
             m.message == WM_LBUTTONUP || m.message == WM_RBUTTONDOWN ||
             m.message == WM_RBUTTONUP))
        {
            log_input_prefix("[DISPATCH]");
            serial_puts(" hwnd=0x"); serial_puthex((uint64_t)(ULONG_PTR)m.hwnd, 8);
            serial_puts(" msg=0x"); serial_puthex((uint64_t)m.message, 4);
            serial_puts(" wp=0x"); serial_puthex((uint64_t)(uint32_t)m.wParam, 8);
            serial_puts(" lp=0x"); serial_puthex((uint64_t)(uint32_t)m.lParam, 8);
            serial_puts("\n");
            n++;
        }
    }

    /* Dispatch to 32-bit WndProc via compat32_callback_args.
     * Previous code fell back to DefWindowProc with "can't call 32-bit
     * from 64-bit" — but compat32_callback_args handles the mode switch. */
    WINDOW *w = find_window(m.hwnd);
    if (w && w->wndproc) {
        return dispatch_wndproc(w->wndproc, m.hwnd, m.message,
                                m.wParam, m.lParam);
    }
    return DefWindowProcA(m.hwnd, m.message, m.wParam, m.lParam);
}

void WINAPI PostQuitMessage(int nExitCode)
{
    extern void serial_puts(const char *);
    extern void serial_puthex(uint64_t v, int n);
    extern void serial_putdec(uint64_t v);
    extern uint32_t compat32_get_last_caller_eip(void);
    uint32_t eip = compat32_get_last_caller_eip();
    serial_puts("[USER32] PostQuitMessage code=");
    serial_putdec((uint64_t)(uint32_t)nExitCode);
    serial_puts(" caller=0x");
    serial_puthex(eip, 8);
    serial_puts("\n");
    msg_enqueue_target(NULL, WM_QUIT, (WPARAM)nExitCode, 0,
                       GetCurrentProcessId(), GetCurrentThreadId());
}

BOOL WINAPI PostMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    return msg_enqueue(hWnd, Msg, wParam, lParam);
}

static BOOL send_message_wait(HWND hWnd, DWORD Msg, WPARAM wParam,
                              LPARAM lParam, DWORD timeout_ms,
                              BOOL block_reentrancy, BOOL unicode, LRESULT *result)
{
    WINDOW *window = find_window(hWnd);
    if (!window) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        *result = 0;
        return FALSE;
    }

    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    if (window->owner_pid == pid && window->owner_tid == tid) {
        *result = window->wndproc
                ? dispatch_wndproc_encoded(window->wndproc, hWnd, Msg, wParam, lParam, unicode)
                : DefWindowProcA(hWnd, Msg, wParam, lParam);
        return TRUE;
    }

    HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!event) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        *result = 0;
        return FALSE;
    }

    int slot;
    uint32_t generation;
    DWORD target_pid = window->owner_pid;
    DWORD target_tid = window->owner_tid;
    if (!sent_message_begin(window, Msg, wParam, lParam, unicode, event,
                            &slot, &generation)) {
        CloseHandle(event);
        SetLastError(1816); /* ERROR_NOT_ENOUGH_QUOTA */
        *result = 0;
        return FALSE;
    }

    __sync_fetch_and_or(&queue_changed_status, QS_SENDMESSAGE);
    msg_wait_event_signal(target_pid, target_tid);

    const DWORD infinite = 0xFFFFFFFFU;
    const DWORD wait_object_0 = 0;
    const DWORD wait_failed = 0xFFFFFFFFU;
    DWORD start = shim_timeGetTime();
    BOOL completed = FALSE;
    BOOL failed = FALSE;

    for (;;) {
        DWORD elapsed = (DWORD)(shim_timeGetTime() - start);
        DWORD slice = 10;
        if (timeout_ms != infinite) {
            if (elapsed >= timeout_ms)
                slice = 0;
            else if (timeout_ms - elapsed < slice)
                slice = timeout_ms - elapsed;
        }

        DWORD wait = WaitForSingleObject(event, slice);
        if (wait == wait_object_0) {
            completed = sent_message_finish_wait(slot, generation,
                                                  FALSE, result);
            if (completed)
                break;
        } else if (wait == wait_failed) {
            failed = TRUE;
            break;
        }

        /* NT lets a thread blocked in SendMessage receive sent messages of
         * its own unless SMTO_BLOCK was requested.  This prevents A->B->A
         * call chains from deadlocking. */
        if (!block_reentrancy)
            sent_message_dispatch_current();

        extern void win32_main_termination_checkpoint(void);
        win32_main_termination_checkpoint();

        if (timeout_ms != infinite &&
            (DWORD)(shim_timeGetTime() - start) >= timeout_ms)
            break;
    }

    if (!completed)
        completed = sent_message_finish_wait(slot, generation,
                                              TRUE, result);
    CloseHandle(event);

    if (!completed) {
        if (!failed)
            SetLastError(1460); /* ERROR_TIMEOUT */
        *result = 0;
        return FALSE;
    }
    return TRUE;
}

static PVOID message_convert_text(PCVOID text, BOOL input_wide, int *length)
{
    int count = input_wide
        ? WideCharToMultiByte(0, 0, text, -1, NULL, 0, NULL, NULL)
        : MultiByteToWideChar(0, 0, text, -1, NULL, 0);
    if (count <= 0) return NULL;
    PVOID copy = HeapAlloc(GetProcessHeap(), 0,
        (SIZE_T)count * (input_wide ? 1 : sizeof(WCHAR)));
    if (!copy) return NULL;
    int converted = input_wide
        ? WideCharToMultiByte(0, 0, text, -1, copy, count, NULL, NULL)
        : MultiByteToWideChar(0, 0, text, -1, copy, count);
    if (!converted) { HeapFree(GetProcessHeap(), 0, copy); return NULL; }
    *length = converted-1;
    return copy;
}

static LRESULT send_message_text(HWND window, DWORD message, WPARAM wp,
                                  LPARAM lp, BOOL wide)
{
    WINDOW *w = find_window(window);
    LRESULT result = 0;
    BOOL target_wide = w ? w->unicode : wide;
    if (target_wide == wide || message < 0xC || message > 0xE ||
        (message == 0xC && !lp)) {
        send_message_wait(window, message, wp, lp, 0xFFFFFFFFu, FALSE, wide, &result);
        return result;
    }
    HANDLE heap = GetProcessHeap();
    if (message == 0xC) {
        int length;
        PVOID text = message_convert_text((PCVOID)lp, wide, &length);
        if (!text) return 0;
        send_message_wait(window, message, wp, (LPARAM)text, 0xFFFFFFFFu,
            FALSE, target_wide, &result);
        HeapFree(heap, 0, text);
        return result;
    }
    WPARAM capacity = wp;
    if (message == 0xE) {
        if (!send_message_wait(window, message, 0, 0, 0xFFFFFFFFu,
                FALSE, target_wide, &result) || result <= 0 || result >= 0x7FFFFFFE)
            return result;
        capacity = (WPARAM)result+1;
    } else if (!lp || !capacity || capacity > 0x7FFFFFFFu) return 0;
    PVOID text = HeapAlloc(heap, 8, capacity * (target_wide ? sizeof(WCHAR) : 1));
    if (!text) return 0;
    BOOL completed = send_message_wait(window, 0xD, capacity, (LPARAM)text,
        0xFFFFFFFFu, FALSE, target_wide, &result);
    if (target_wide) ((PWSTR)text)[capacity-1] = 0;
    else ((PSTR)text)[capacity-1] = 0;
    int length = 0;
    PVOID converted = completed ? message_convert_text(text, target_wide, &length) : NULL;
    HeapFree(heap, 0, text);
    if (!converted) return 0;
    if (message == 0xD) {
        if ((WPARAM)length >= wp) length = (int)wp-1;
        memcpy((PVOID)lp, converted, (SIZE_T)length * (wide ? sizeof(WCHAR) : 1));
        if (wide) ((PWSTR)lp)[length] = 0;
        else ((PSTR)lp)[length] = 0;
    }
    HeapFree(heap, 0, converted);
    return length;
}

LRESULT WINAPI SendMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    return send_message_text(hWnd, Msg, wParam, lParam, FALSE);
}

LRESULT WINAPI DefWindowProcA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;

    switch (Msg) {
    case WM_NCCREATE:   return 1;       /* allow creation */
    case 0x000C: /* WM_SETTEXT */
    case 0x000D: /* WM_GETTEXT */
    case 0x000E: /* WM_GETTEXTLENGTH */
        return window_text_message(hWnd, Msg, wParam, lParam, FALSE);
    case WM_CREATE:     return 0;       /* success */
    case WM_CLOSE:      DestroyWindow(hWnd); return 0;
    case WM_DESTROY:    return 0;
    case WM_NCDESTROY:  return 0;
    case WM_ERASEBKGND: {
        WINDOW *window = find_window(hWnd);
        WNDCLASS_ENTRY *entry = window
            ? lookup_class_for_pid(window->class_name, window->owner_pid) : NULL;
        if (!entry || !entry->hbrBackground) return 0;
        RECT rect;
        if (!GetClientRect(hWnd, &rect)) return 0;
        return FillRect((HDC)(ULONG_PTR)wParam, &rect, entry->hbrBackground);
    }
    case 0x0132: /* WM_CTLCOLORMSGBOX */
    case 0x0133: /* WM_CTLCOLOREDIT */
    case 0x0134: /* WM_CTLCOLORLISTBOX */
    case 0x0135: /* WM_CTLCOLORBTN */
    case 0x0136: /* WM_CTLCOLORDLG */
    case 0x0138: { /* WM_CTLCOLORSTATIC */
        int background = Msg == 0x0133 || Msg == 0x0134 ? 5 : 15;
        SetTextColor((HDC)(ULONG_PTR)wParam, GetSysColor(8));
        SetBkColor((HDC)(ULONG_PTR)wParam, GetSysColor(background));
        return (LRESULT)(ULONG_PTR)GetSysColorBrush_u32(background);
    }
    case WM_NCHITTEST:  return HTCLIENT;
    case WM_NCLBUTTONDOWN:
        if (wParam == HTCAPTION) {
            POINT point = {
                .x = (short)(lParam & 0xFFFF),
                .y = (short)((lParam >> 16) & 0xFFFF),
            };
            native_move_begin(hWnd, point);
        }
        return 0;
    case WM_NCLBUTTONUP:
        return 0;
    case WM_NCACTIVATE: return 1;
    case WM_SETCURSOR:  return 1;
    case WM_ACTIVATE:   return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_F4 && ((ULONG_PTR)lParam & (1UL << 29))) {
            HWND root = GetAncestor(hWnd, GA_ROOT);
            WINDOW *window = find_window(root);
            WNDCLASS_ENTRY *window_class = window
                ? lookup_class_for_pid(window->class_name, window->owner_pid)
                : NULL;
            if (window &&
                (!window_class || !(window_class->style & CS_NOCLOSE)))
                PostMessageA(root, WM_SYSCOMMAND, SC_CLOSE, 0);
        }
        return 0;
    case WM_WINDOWPOSCHANGED: {
        WINDOW *w = find_window(hWnd);
        if (!w || !lParam)
            return 0;

        UINT flags;
        int x, y;
        if (g_compat32_mode) {
            const WINDOWPOS32 *position =
                (const WINDOWPOS32 *)(ULONG_PTR)lParam;
            flags = position->flags;
            x = position->x;
            y = position->y;
        } else {
            const WINDOWPOS *position =
                (const WINDOWPOS *)(ULONG_PTR)lParam;
            flags = position->flags;
            x = position->x;
            y = position->y;
        }

        /* NT generates WM_SIZE/WM_MOVE from DefWindowProc's handling of
         * WM_WINDOWPOSCHANGED, not directly from SetWindowPos. */
        if (!(flags & SWP_NOSIZE))
            dispatch_wm_size(w);
        if (!(flags & SWP_NOMOVE))
            dispatch_window_message(
                w, WM_MOVE, 0,
                ((uint32_t)x & 0xFFFFU) |
                (((uint32_t)y & 0xFFFFU) << 16));
        return 0;
    }
    case WM_SYSCOMMAND:
        switch ((UINT)wParam & 0xFFF0U) {
        case SC_CLOSE:
            SendMessageA(hWnd, WM_CLOSE, 0, 0);
            return 0;
        case SC_MINIMIZE:
            ShowWindow(hWnd, SW_MINIMIZE);
            return 0;
        case SC_MAXIMIZE:
            ShowWindow(hWnd, SW_SHOWMAXIMIZED);
            return 0;
        case SC_RESTORE:
            ShowWindow(hWnd, SW_RESTORE);
            return 0;
        case SC_MOVE:
            native_move_begin(hWnd, cursor_pos);
            return 0;
        case SC_SIZE:
            /* Interactive sizing is not implemented yet. */
            return 0;
        }
        break;
    case WM_PAINT:
        default_window_paint(hWnd);
        return 0;
    }

    return 0;
}

/* ── Window info ───────────────────────────────────────────── */

static U32_DIALOG_STATE *dialog_state_find(HWND window)
{
    if (!window) return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (dialog_states[i].used && dialog_states[i].window == window)
            return &dialog_states[i];
    return NULL;
}

static U32_DIALOG_STATE *dialog_state_allocate(BOOL modal, HWND owner,
                                                DLGPROC proc)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        U32_DIALOG_STATE *state = &dialog_states[i];
        if (state->used) continue;
        memset(state, 0, sizeof(*state));
        state->used = TRUE;
        state->modal = modal;
        state->owner = owner;
        state->proc = proc;
        state->result = -1;
        return state;
    }
    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return NULL;
}

static void dialog_state_clear(U32_DIALOG_STATE *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

static void dialog_release_window(HWND window)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        U32_DIALOG_STATE *state = &dialog_states[i];
        if (!state->used) continue;
        if (state->owner == window) {
            state->owner = NULL;
            state->owner_disabled = FALSE;
        }
        if (state->window != window) continue;
        if (state->modal) {
            if (!state->ended) state->result = -1;
            state->ended = TRUE;
            state->window = NULL;
        } else {
            dialog_state_clear(state);
        }
    }
}

static LRESULT dialog_default_proc(HWND hWnd, DWORD Msg,
                                   WPARAM wParam, LPARAM lParam)
{
    U32_DIALOG_STATE *state = dialog_state_find(hWnd);
    switch (Msg) {
    case WM_ERASEBKGND: {
        BOOL compat32 = g_compat32_mode;
        LRESULT result = SendMessageA(
            hWnd, 0x0136, wParam, (LPARAM)(ULONG_PTR)hWnd); /* WM_CTLCOLORDLG */
        /* LRESULT is signed; a PE32 brush handle is an unsigned 32-bit value. */
        HBRUSH brush = (HBRUSH)(compat32
            ? (ULONG_PTR)(uint32_t)result : (ULONG_PTR)result);
        RECT rect;
        if (!brush || !GetClientRect(hWnd, &rect)) return 0;
        return FillRect((HDC)(ULONG_PTR)wParam, &rect, brush);
    }
    case WM_INITDIALOG:
        return TRUE;
    case WM_CLOSE:
        if (state && state->modal)
            EndDialog(hWnd, IDCANCEL);
        else
            DestroyWindow(hWnd);
        return TRUE;
    case WM_COMMAND: {
        UINT id = (UINT)(wParam & 0xFFFFU);
        if (state && state->modal && (id == IDOK || id == IDCANCEL)) {
            EndDialog(hWnd, (LONG_PTR)id);
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            SendMessageA(hWnd, WM_COMMAND, IDCANCEL, 0);
            return TRUE;
        }
        if (wParam == VK_RETURN) {
            SendMessageA(hWnd, WM_COMMAND, IDOK, 0);
            return TRUE;
        }
        break;
    }
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

LRESULT WINAPI DefDlgProcA(HWND hDlg, DWORD msg, WPARAM wParam,
                            LPARAM lParam)
{
    return dialog_default_proc(hDlg, msg, wParam, lParam);
}

LRESULT WINAPI DefDlgProcW(HWND hDlg, DWORD msg, WPARAM wParam,
                            LPARAM lParam)
{
    return DefDlgProcA(hDlg, msg, wParam, lParam);
}

static LRESULT WINAPI dialog_class_wndproc(HWND hWnd, DWORD Msg,
                                            WPARAM wParam, LPARAM lParam)
{
    U32_DIALOG_STATE *state = dialog_state_find(hWnd);
    if (state && state->proc) {
        LRESULT handled = dispatch_wndproc((WNDPROC)state->proc, hWnd, Msg,
                                           wParam, lParam);
        if (handled)
            return handled;
    }
    return dialog_default_proc(hWnd, Msg, wParam, lParam);
}

static LRESULT WINAPI system_class_wndproc(HWND hWnd, DWORD Msg,
                                            WPARAM wParam, LPARAM lParam)
{
    WINDOW *window = find_window(hWnd);
    LRESULT result;
    if (window && window->control && user32_control_message(
            window->control, hWnd, Msg, wParam, lParam, &result)) return result;
    USER_HOOK_CONTEXT *context = user_hook_context_get(FALSE);
    if (context && context->unicode_message)
        return DefWindowProcW(hWnd, Msg, wParam, lParam);
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

BOOL WINAPI GetClientRect(HWND hWnd, LPRECT lpRect)
{
    WINDOW *w = find_window(hWnd);
    BOOL result = w && lpRect ? TRUE : FALSE;
    if (!result)
        return FALSE;
    lpRect->left   = 0;
    lpRect->top    = 0;
    lpRect->right  = w->width;
    lpRect->bottom = w->height;
    {
        static int n = 0;
        if (u32_input_diagnostics_active() && n < 32 &&
            relative_pointer_mode_active()) {
            extern uint32_t compat32_get_last_caller_eip(void);
            log_input_prefix("[MOUSE-RECT] GetClientRect");
            serial_puts(" hwnd=0x"); serial_puthex((uint64_t)(ULONG_PTR)hWnd, 8);
            serial_puts(" r=");
            serial_putdec((uint64_t)(uint32_t)lpRect->left); serial_puts(",");
            serial_putdec((uint64_t)(uint32_t)lpRect->top); serial_puts(",");
            serial_putdec((uint64_t)(uint32_t)lpRect->right); serial_puts(",");
            serial_putdec((uint64_t)(uint32_t)lpRect->bottom);
            serial_puts(" eip=0x"); serial_puthex(compat32_get_last_caller_eip(), 8);
            serial_puts("\n");
            n++;
        }
    }
    return TRUE;
}

BOOL WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !lpRect) return FALSE;
    int screen_x, screen_y;
    window_screen_origin(w, &screen_x, &screen_y);
    lpRect->left   = screen_x;
    lpRect->top    = screen_y;
    lpRect->right  = screen_x + w->width;
    lpRect->bottom = screen_y + w->height;
    return TRUE;
}

BOOL WINAPI GetWindowPlacement(HWND hWnd, LPWINDOWPLACEMENT placement)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !placement || placement->length != sizeof(WINDOWPLACEMENT)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    placement->flags = 0;
    placement->showCmd = (w->style & WS_MINIMIZE) ? SW_SHOWMINIMIZED :
                         ((w->style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED :
                                                    SW_SHOWNORMAL);
    placement->ptMinPosition = w->min_position;
    placement->ptMaxPosition = w->max_position;
    placement->rcNormalPosition = w->normal_rect;
    return TRUE;
}

BOOL WINAPI SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT *placement)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !placement || placement->length != sizeof(WINDOWPLACEMENT)) {
        SetLastError(87);
        return FALSE;
    }

    RECT normal = placement->rcNormalPosition;
    int width = normal.right - normal.left;
    int height = normal.bottom - normal.top;
    if (width < 0 || height < 0) {
        SetLastError(87);
        return FALSE;
    }

    if (!w->parent) {
        int screen_width = GetSystemMetrics(SM_CXSCREEN);
        int screen_height = GetSystemMetrics(SM_CYSCREEN);
        if (normal.right <= 0 || normal.left >= screen_width)
            normal.left = 0, normal.right = width;
        if (normal.bottom <= 0 || normal.top >= screen_height)
            normal.top = 0, normal.bottom = height;
    }

    w->min_position = placement->ptMinPosition;
    w->max_position = placement->ptMaxPosition;
    w->normal_rect = normal;

    UINT show_cmd = placement->showCmd;
    if (show_cmd == SW_SHOWNORMAL || show_cmd == SW_RESTORE ||
        show_cmd == SW_SHOWDEFAULT) {
        w->x = normal.left;
        w->y = normal.top;
        w->width = normal.right - normal.left;
        w->height = normal.bottom - normal.top;
        sync_window_surface(w, w->width, w->height);
    }

    ShowWindow(hWnd, (int)show_cmd);
    sync_all_window_compositor_state();
    return TRUE;
}

BOOL WINAPI IsRectEmpty(const RECT *lpRect)
{
    if (!lpRect) return TRUE;
    return lpRect->right <= lpRect->left ||
           lpRect->bottom <= lpRect->top;
}

BOOL WINAPI EqualRect(const RECT *a, const RECT *b)
{
    if (!a || !b) return FALSE;
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

BOOL WINAPI SetRect(LPRECT rect, int left, int top, int right, int bottom)
{
    if (!rect) return FALSE;
    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
    return TRUE;
}

BOOL WINAPI SetRectEmpty(LPRECT rect)
{
    if (!rect) return FALSE;
    rect->left = 0;
    rect->top = 0;
    rect->right = 0;
    rect->bottom = 0;
    return TRUE;
}

BOOL WINAPI OffsetRect(LPRECT rect, int dx, int dy)
{
    if (!rect) return FALSE;
    rect->left += dx;
    rect->right += dx;
    rect->top += dy;
    rect->bottom += dy;
    return TRUE;
}

BOOL WINAPI InflateRect(LPRECT rect, int dx, int dy)
{
    if (!rect) return FALSE;
    rect->left -= dx;
    rect->right += dx;
    rect->top -= dy;
    rect->bottom += dy;
    return TRUE;
}

BOOL WINAPI IntersectRect(LPRECT result, const RECT *a, const RECT *b)
{
    if (!result || !a || !b) return FALSE;
    result->left = a->left > b->left ? a->left : b->left;
    result->top = a->top > b->top ? a->top : b->top;
    result->right = a->right < b->right ? a->right : b->right;
    result->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (IsRectEmpty(result)) {
        result->left = result->top = result->right = result->bottom = 0;
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI PtInRect(const RECT *rect, POINT point)
{
    if (!rect || IsRectEmpty(rect)) return FALSE;
    return point.x >= rect->left && point.x < rect->right &&
           point.y >= rect->top && point.y < rect->bottom;
}

int WINAPI GetWindowRgn(HWND hWnd, HRGN hRgn)
{
    (void)hRgn;
    if (!find_window(hWnd)) return 0;

    /* Windows returns ERROR when a valid window has no custom region. */
    return 0;
}

int WINAPI SetWindowRgn(HWND hWnd, HRGN hRgn, BOOL bRedraw)
{
    (void)hRgn;
    (void)bRedraw;
    return find_window(hWnd) ? 1 : 0;
}

BOOL WINAPI AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu)
{
    (void)dwStyle;
    (void)bMenu;
    /* In our borderless framebuffer model, no adjustment needed */
    return lpRect ? TRUE : FALSE;
}

BOOL WINAPI AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle)
{
    (void)dwExStyle;
    return AdjustWindowRect(lpRect, dwStyle, bMenu);
}

/* ── SystemParametersInfo / Timer stubs ──────────────────────── */

#define SPI_GETNONCLIENTMETRICS 0x0029
#define SPI_GETWORKAREA         0x0030

typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    char lfFaceName[32];
} U32_LOGFONTA;

typedef struct {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
} U32_LOGFONTW;

typedef struct {
    UINT cbSize;
    int iBorderWidth;
    int iScrollWidth;
    int iScrollHeight;
    int iCaptionWidth;
    int iCaptionHeight;
    U32_LOGFONTA lfCaptionFont;
    int iSmCaptionWidth;
    int iSmCaptionHeight;
    U32_LOGFONTA lfSmCaptionFont;
    int iMenuWidth;
    int iMenuHeight;
    U32_LOGFONTA lfMenuFont;
    U32_LOGFONTA lfStatusFont;
    U32_LOGFONTA lfMessageFont;
    int iPaddedBorderWidth;
} U32_NONCLIENTMETRICSA;

typedef struct {
    UINT cbSize;
    int iBorderWidth;
    int iScrollWidth;
    int iScrollHeight;
    int iCaptionWidth;
    int iCaptionHeight;
    U32_LOGFONTW lfCaptionFont;
    int iSmCaptionWidth;
    int iSmCaptionHeight;
    U32_LOGFONTW lfSmCaptionFont;
    int iMenuWidth;
    int iMenuHeight;
    U32_LOGFONTW lfMenuFont;
    U32_LOGFONTW lfStatusFont;
    U32_LOGFONTW lfMessageFont;
    int iPaddedBorderWidth;
} U32_NONCLIENTMETRICSW;

_Static_assert(sizeof(U32_LOGFONTA) == 60, "LOGFONTA ABI changed");
_Static_assert(sizeof(U32_LOGFONTW) == 92, "LOGFONTW ABI changed");
_Static_assert(sizeof(U32_NONCLIENTMETRICSA) == 344,
               "NONCLIENTMETRICSA ABI changed");
_Static_assert(sizeof(U32_NONCLIENTMETRICSW) == 504,
               "NONCLIENTMETRICSW ABI changed");

#define U32_NONCLIENTMETRICSA_XP_SIZE 340u
#define U32_NONCLIENTMETRICSW_XP_SIZE 500u

static void spi_copy_bytes(PVOID dst, const void *src, UINT size)
{
    BYTE *out = (BYTE *)dst;
    const BYTE *in = (const BYTE *)src;
    while (size--) *out++ = *in++;
}

static void spi_init_font_a(U32_LOGFONTA *font, int height, int weight)
{
    static const char face[] = "Segoe UI";
    memset(font, 0, sizeof(*font));
    font->lfHeight = height;
    font->lfWeight = weight;
    font->lfCharSet = 1;          /* DEFAULT_CHARSET */
    font->lfQuality = 5;          /* CLEARTYPE_QUALITY */
    font->lfPitchAndFamily = 0x22; /* VARIABLE_PITCH | FF_SWISS */
    u32_strcpy(font->lfFaceName, face, 32);
}

static void spi_init_font_w(U32_LOGFONTW *font, int height, int weight)
{
    static const char face[] = "Segoe UI";
    memset(font, 0, sizeof(*font));
    font->lfHeight = height;
    font->lfWeight = weight;
    font->lfCharSet = 1;          /* DEFAULT_CHARSET */
    font->lfQuality = 5;          /* CLEARTYPE_QUALITY */
    font->lfPitchAndFamily = 0x22; /* VARIABLE_PITCH | FF_SWISS */
    for (int i = 0; face[i] && i < 31; i++)
        font->lfFaceName[i] = (WCHAR)(BYTE)face[i];
}

static void spi_trace_nonclient(char encoding, UINT size, PVOID pv_param,
                                PVOID local_metrics, uint64_t caller)
{
    static uint32_t trace_count;
    if (__atomic_fetch_add(&trace_count, 1, __ATOMIC_RELAXED) >= 64)
        return;
    extern int32_t proc_current_pid(void);
    extern uint64_t *tss_ist1_ptr;
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    serial_puts("[U32-SPI] GETNONCLIENTMETRICS encoding=");
    char text[2] = { encoding, 0 };
    serial_puts(text);
    serial_puts(" size=");
    serial_putdec(size);
    serial_puts(" win_pid=");
    serial_putdec(GetCurrentProcessId());
    serial_puts(" tid=");
    serial_putdec(GetCurrentThreadId());
    serial_puts(" kernel_pid=");
    serial_putdec((uint64_t)proc_current_pid());
    serial_puts(" pv=0x");
    serial_puthex((uint64_t)(ULONG_PTR)pv_param, 16);
    serial_puts(" local=0x");
    serial_puthex((uint64_t)(ULONG_PTR)local_metrics, 16);
    serial_puts(" rsp=0x");
    serial_puthex(rsp, 16);
    serial_puts(" ist1=0x");
    serial_puthex(tss_ist1_ptr ? *tss_ist1_ptr : 0, 16);
    serial_puts(" caller=0x");
    serial_puthex(caller, 16);
    serial_puts("\n");
}

static int screen_cx(void);   /* defined below (live GOP size) */
static int screen_cy(void);
static int current_mode_cx(void);
static int current_mode_cy(void);

BOOL WINAPI SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
    (void)fWinIni;
    if (uiAction == SPI_GETWORKAREA && pvParam) {
        /* Return screen rect as work area. Use the LIVE screen size (same
         * source as GetSystemMetrics) — the old compile-time
         * SCREEN_WIDTH/SCREEN_HEIGHT constants were the one remaining size
         * source that could disagree with every other metric, showing UE1 a
         * phantom desktop-size mismatch. */
        int32_t *rect = (int32_t *)pvParam;
        rect[0] = 0;              /* left */
        rect[1] = 0;              /* top */
        rect[2] = current_mode_cx();    /* right */
        rect[3] = current_mode_cy();    /* bottom */
        return TRUE;
    }
    if (uiAction == SPI_GETNONCLIENTMETRICS) {
        if (!pvParam || uiParam < U32_NONCLIENTMETRICSA_XP_SIZE) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }

        U32_NONCLIENTMETRICSA metrics;
        memset(&metrics, 0, sizeof(metrics));
        metrics.cbSize = uiParam;
        metrics.iBorderWidth = 1;
        metrics.iScrollWidth = 17;
        metrics.iScrollHeight = 17;
        metrics.iCaptionWidth = 18;
        metrics.iCaptionHeight = 18;
        metrics.iSmCaptionWidth = 16;
        metrics.iSmCaptionHeight = 16;
        metrics.iMenuWidth = 18;
        metrics.iMenuHeight = 18;
        metrics.iPaddedBorderWidth = 1;
        spi_init_font_a(&metrics.lfCaptionFont, -12, 700);
        spi_init_font_a(&metrics.lfSmCaptionFont, -11, 400);
        spi_init_font_a(&metrics.lfMenuFont, -12, 400);
        spi_init_font_a(&metrics.lfStatusFont, -12, 400);
        spi_init_font_a(&metrics.lfMessageFont, -12, 400);

        UINT write_size = uiParam < sizeof(metrics)
                        ? uiParam : (UINT)sizeof(metrics);
        spi_copy_bytes(pvParam, &metrics, write_size);
        spi_trace_nonclient('A', uiParam, pvParam, &metrics,
                            (uint64_t)__builtin_return_address(0));
        SetLastError(0);
        return TRUE;
    }
    return TRUE;
}

BOOL WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam,
                                  PVOID pvParam, UINT fWinIni)
{
    (void)fWinIni;
    if (uiAction == SPI_GETWORKAREA)
        return SystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
    if (uiAction == SPI_GETNONCLIENTMETRICS) {
        if (!pvParam || uiParam < U32_NONCLIENTMETRICSW_XP_SIZE) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }

        U32_NONCLIENTMETRICSW metrics;
        memset(&metrics, 0, sizeof(metrics));
        metrics.cbSize = uiParam;
        metrics.iBorderWidth = 1;
        metrics.iScrollWidth = 17;
        metrics.iScrollHeight = 17;
        metrics.iCaptionWidth = 18;
        metrics.iCaptionHeight = 18;
        metrics.iSmCaptionWidth = 16;
        metrics.iSmCaptionHeight = 16;
        metrics.iMenuWidth = 18;
        metrics.iMenuHeight = 18;
        metrics.iPaddedBorderWidth = 1;
        spi_init_font_w(&metrics.lfCaptionFont, -12, 700);
        spi_init_font_w(&metrics.lfSmCaptionFont, -11, 400);
        spi_init_font_w(&metrics.lfMenuFont, -12, 400);
        spi_init_font_w(&metrics.lfStatusFont, -12, 400);
        spi_init_font_w(&metrics.lfMessageFont, -12, 400);

        UINT write_size = uiParam < sizeof(metrics)
                        ? uiParam : (UINT)sizeof(metrics);
        spi_copy_bytes(pvParam, &metrics, write_size);
        spi_trace_nonclient('W', uiParam, pvParam, &metrics,
                            (uint64_t)__builtin_return_address(0));
        SetLastError(0);
        return TRUE;
    }
    return TRUE;
}

static BOOL WINAPI SetProcessDpiAwarenessContext_u32(HANDLE context)
{
    LONG_PTR value = (LONG_PTR)(ULONG_PTR)context;
    if (value > -1 || value < -5) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    process_dpi_context = context;
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI SetProcessDPIAware_u32(void)
{
    process_dpi_context = (HANDLE)(LONG_PTR)-2; /* SYSTEM_AWARE */
    SetLastError(0);
    return TRUE;
}

static HANDLE WINAPI SetThreadDpiAwarenessContext_u32(HANDLE context)
{
    LONG_PTR value = (LONG_PTR)(ULONG_PTR)context;
    if (value > -1 || value < -5) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    HANDLE previous = thread_dpi_context ? thread_dpi_context
                                         : process_dpi_context;
    thread_dpi_context = context;
    SetLastError(0);
    return previous;
}

static HANDLE WINAPI GetThreadDpiAwarenessContext_u32(void)
{
    return thread_dpi_context ? thread_dpi_context : process_dpi_context;
}

static UINT WINAPI GetDpiForWindow_u32(HWND window)
{
    if (!hwnd_is_desktop(window) && !find_window(window)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return 0;
    }
    SetLastError(0);
    return 96;
}

static UINT WINAPI GetDpiForSystem_u32(void)
{
    return 96;
}

static HANDLE WINAPI GetWindowDpiAwarenessContext_u32(HWND window)
{
    if (!hwnd_is_desktop(window) && !find_window(window)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return NULL;
    }
    SetLastError(0);
    return process_dpi_context;
}

static BOOL WINAPI EnableNonClientDpiScaling_u32(HWND window)
{
    if (!hwnd_is_desktop(window) && !find_window(window)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

static BOOL WINAPI AdjustWindowRectExForDpi_u32(LPRECT rect, DWORD style,
                                                 BOOL menu, DWORD ex_style,
                                                 UINT dpi)
{
    if (!dpi) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    return AdjustWindowRectEx(rect, style, menu, ex_style);
}

static BOOL WINAPI AreDpiAwarenessContextsEqual_u32(HANDLE first,
                                                      HANDLE second)
{
    return (LONG_PTR)(ULONG_PTR)first == (LONG_PTR)(ULONG_PTR)second;
}

static BOOL WINAPI IsValidDpiAwarenessContext_u32(HANDLE context)
{
    LONG_PTR value = (LONG_PTR)(ULONG_PTR)context;
    return value <= -1 && value >= -5;
}

static int WINAPI GetAwarenessFromDpiAwarenessContext_u32(HANDLE context)
{
    switch ((LONG_PTR)(ULONG_PTR)context) {
    case -1: /* UNAWARE */
    case -5: /* UNAWARE_GDISCALED */
        return 0;
    case -2: /* SYSTEM_AWARE */
        return 1;
    case -3: /* PER_MONITOR_AWARE */
    case -4: /* PER_MONITOR_AWARE_V2 */
        return 2;
    default:
        return -1; /* DPI_AWARENESS_INVALID */
    }
}

static BOOL WINAPI RegisterPointerDeviceNotifications_u32(HWND window,
                                                           BOOL notify_range)
{
    (void)window;
    (void)notify_range;
    return TRUE;
}

static BOOL WINAPI GetPointerDevices_u32(UINT *device_count,
                                          PVOID devices)
{
    (void)devices;
    if (!device_count) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    *device_count = 0;
    return TRUE;
}

static HANDLE WINAPI RegisterSuspendResumeNotification_u32(HANDLE recipient,
                                                             DWORD flags)
{
    (void)recipient;
    (void)flags;
    return (HANDLE)(ULONG_PTR)1;
}

static BOOL WINAPI UnregisterSuspendResumeNotification_u32(HANDLE notification)
{
    if (!notification) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

static HANDLE WINAPI RegisterPowerSettingNotification_u32(HANDLE recipient,
                                                           LPCGUID setting,
                                                           DWORD flags)
{
    (void)recipient;
    (void)flags;
    if (!setting) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    return (HANDLE)(ULONG_PTR)1;
}

static BOOL WINAPI UnregisterPowerSettingNotification_u32(HANDLE notification)
{
    if (!notification) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

static HANDLE WINAPI RegisterDeviceNotificationW_u32(HANDLE recipient,
                                                       PVOID filter,
                                                       DWORD flags)
{
    (void)recipient;
    (void)flags;
    if (!filter) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }
    /* Device arrival/removal events are not wired to the message queue yet. */
    return (HANDLE)(ULONG_PTR)2;
}

static BOOL WINAPI UnregisterDeviceNotification_u32(HANDLE notification)
{
    if (!notification) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    return TRUE;
}

ULONG_PTR WINAPI SetTimer(HWND hWnd, ULONG_PTR nIDEvent, UINT uElapse, void *lpTimerFunc)
{
    return user_timer_set(hWnd, nIDEvent, uElapse, lpTimerFunc);
}

static ULONG_PTR WINAPI SetCoalescableTimer_u32(HWND window,
                                                 ULONG_PTR event_id,
                                                 UINT elapsed,
                                                 PVOID timer_function,
                                                 ULONG tolerance)
{
    (void)tolerance;
    return SetTimer(window, event_id, elapsed, timer_function);
}

BOOL WINAPI KillTimer(HWND hWnd, ULONG_PTR uIDEvent)
{
    return user_timer_kill(hWnd, uIDEvent);
}

/* Report the physical display resolution as the desktop. fb_get_width/height
 * become the terminal surface dimensions after fb_redirect(), so they are only
 * a pre-compositor fallback and must not drive USER32 metrics after desktop. */
extern uint32_t display_get_width(void)  __attribute__((weak));
extern uint32_t display_get_height(void) __attribute__((weak));
extern uint32_t fb_get_width(void)  __attribute__((weak));
extern uint32_t fb_get_height(void) __attribute__((weak));
static int screen_cx(void)
{
    uint32_t w = display_get_width ? display_get_width() : 0;
    if (!w && fb_get_width) w = fb_get_width();
    return w ? (int)w : USER32_FALLBACK_SCREEN_WIDTH;
}
static int screen_cy(void)
{
    uint32_t h = display_get_height ? display_get_height() : 0;
    if (!h && fb_get_height) h = fb_get_height();
    return h ? (int)h : USER32_FALLBACK_SCREEN_HEIGHT;
}

/* NT semantics: a fullscreen-exclusive DirectDraw SetDisplayMode CHANGES the
 * desktop metrics (SM_CXSCREEN/HORZRES report the current mode). We must keep
 * reporting the GOP size during startup (UT99 filters out enumerated modes
 * larger than the "desktop"), so only switch to the ddraw mode once an actual
 * SetDisplayMode has been issued — ddraw_display_mode_active() is 0 until
 * then. Weak: user32 also serves PE apps that never touch ddraw. */
extern int  ddraw_display_mode_active(void) __attribute__((weak));
extern void ddraw_get_display_mode(uint32_t *w, uint32_t *h, uint32_t *bpp)
            __attribute__((weak));

static void current_mode_values(uint32_t *width, uint32_t *height,
                                uint32_t *bpp, uint32_t *frequency)
{
    uint32_t w = 0, h = 0, bits = 0, hz = 60;

    if (ddraw_display_mode_active && ddraw_get_display_mode &&
        ddraw_display_mode_active()) {
        ddraw_get_display_mode(&w, &h, &bits);
    } else if (user_display_mode.active) {
        w = user_display_mode.width;
        h = user_display_mode.height;
        bits = user_display_mode.bpp;
        hz = user_display_mode.frequency;
    }

    if (!w) w = (uint32_t)screen_cx();
    if (!h) h = (uint32_t)screen_cy();
    if (!bits) bits = 32;
    if (!hz) hz = 60;

    if (width) *width = w;
    if (height) *height = h;
    if (bpp) *bpp = bits;
    if (frequency) *frequency = hz;
}

BOOL user32_get_current_display_mode(uint32_t *width, uint32_t *height,
                                     uint32_t *bpp, uint32_t *frequency)
{
    current_mode_values(width, height, bpp, frequency);
    return TRUE;
}

static int current_mode_cx(void)
{
    uint32_t width = 0;
    current_mode_values(&width, NULL, NULL, NULL);
    return (int)width;
}
static int current_mode_cy(void)
{
    uint32_t height = 0;
    current_mode_values(NULL, &height, NULL, NULL);
    return (int)height;
}

int WINAPI GetSystemMetrics(int nIndex)
{
    switch (nIndex) {
    case SM_CXSCREEN:      return current_mode_cx();
    case SM_CYSCREEN:      return current_mode_cy();
    case SM_CXICON:
    case SM_CYICON:
    case SM_CXCURSOR:
    case SM_CYCURSOR:
        return 32;
    case SM_CXFULLSCREEN:  return current_mode_cx();
    case SM_CYFULLSCREEN:  return current_mode_cy();
    case SM_MOUSEPRESENT:
    case SM_MOUSEWHEELPRESENT:
        return 1;
    case SM_SWAPBUTTON:
    case SM_MOUSEHORIZONTALWHEELPRESENT:
        return 0;
    case SM_CMOUSEBUTTONS: return 3;
    case SM_CXSMICON:
    case SM_CYSMICON:
        return 16;
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
        return 0;
    case SM_CXVIRTUALSCREEN: return current_mode_cx();
    case SM_CYVIRTUALSCREEN: return current_mode_cy();
    case SM_CMONITORS:
    case SM_SAMEDISPLAYFORMAT:
        return 1;
    default:               return 0;
    }
}

int WINAPI GetSystemMetricsForDpi(int nIndex, UINT dpi)
{
    int value = GetSystemMetrics(nIndex);
    switch (nIndex) {
    case SM_CXICON:
    case SM_CYICON:
    case SM_CXCURSOR:
    case SM_CYCURSOR:
    case SM_CXSMICON:
    case SM_CYSMICON:
        if (dpi == 0)
            dpi = 96;
        value = (int)(((uint64_t)(uint32_t)value * dpi + 48) / 96);
        return value > 0 ? value : 1;
    default:
        return value;
    }
}

typedef struct {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
} MONITORINFO_K32;

typedef struct {
    MONITORINFO_K32 info;
    char device[32];
} MONITORINFOEXA_K32;

typedef struct {
    MONITORINFO_K32 info;
    WCHAR device[32];
} MONITORINFOEXW_K32;

_Static_assert(sizeof(MONITORINFOEXA_K32) == 72,
               "MONITORINFOEXA layout");
_Static_assert(sizeof(MONITORINFOEXW_K32) == 104,
               "MONITORINFOEXW layout");

static HANDLE primary_monitor(void)
{
    return (HANDLE)(ULONG_PTR)1;
}

HANDLE WINAPI MonitorFromWindow(HWND hWnd, DWORD dwFlags)
{
    return (find_window(hWnd) || dwFlags) ? primary_monitor() : NULL;
}

HANDLE WINAPI MonitorFromPoint(POINT pt, DWORD dwFlags)
{
    BOOL inside = pt.x >= 0 && pt.y >= 0 &&
                  pt.x < current_mode_cx() && pt.y < current_mode_cy();
    return (inside || dwFlags) ? primary_monitor() : NULL;
}

HANDLE WINAPI MonitorFromRect(const RECT *rect, DWORD dwFlags)
{
    BOOL intersects = rect && rect->right > 0 && rect->bottom > 0 &&
                      rect->left < current_mode_cx() &&
                      rect->top < current_mode_cy();
    return (intersects || dwFlags) ? primary_monitor() : NULL;
}

BOOL WINAPI GetMonitorInfoW(HANDLE hMonitor, PVOID lpmi)
{
    MONITORINFO_K32 *mi = (MONITORINFO_K32 *)lpmi;
    if (hMonitor != primary_monitor() || !mi ||
        mi->cbSize < sizeof(MONITORINFO_K32))
        return FALSE;

    mi->rcMonitor = (RECT){ 0, 0, current_mode_cx(), current_mode_cy() };
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = 1; /* MONITORINFOF_PRIMARY */
    if (mi->cbSize >= sizeof(MONITORINFOEXW_K32)) {
        MONITORINFOEXW_K32 *ex = (MONITORINFOEXW_K32 *)lpmi;
        static const char name[] = "\\\\.\\DISPLAY1";
        DWORD i = 0;
        memset(ex->device, 0, sizeof(ex->device));
        while (i + 1 < 32 && name[i]) {
            ex->device[i] = (WCHAR)(unsigned char)name[i];
            i++;
        }
    }
    return TRUE;
}

BOOL WINAPI GetMonitorInfoA(HANDLE hMonitor, PVOID lpmi)
{
    MONITORINFO_K32 *mi = (MONITORINFO_K32 *)lpmi;
    if (hMonitor != primary_monitor() || !mi ||
        mi->cbSize < sizeof(MONITORINFO_K32))
        return FALSE;

    mi->rcMonitor = (RECT){ 0, 0, current_mode_cx(), current_mode_cy() };
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = 1; /* MONITORINFOF_PRIMARY */
    if (mi->cbSize >= sizeof(MONITORINFOEXA_K32)) {
        MONITORINFOEXA_K32 *ex = (MONITORINFOEXA_K32 *)lpmi;
        static const char name[] = "\\\\.\\DISPLAY1";
        DWORD i = 0;
        memset(ex->device, 0, sizeof(ex->device));
        while (i + 1 < 32 && name[i]) {
            ex->device[i] = name[i];
            i++;
        }
    }
    return TRUE;
}

BOOL WINAPI EnumDisplayMonitors(HDC hdc, const RECT *clip,
                                MONITORENUMPROC callback, LPARAM data)
{
    static RECT monitor_rect;
    extern uint32_t compat32_callback_args(uint32_t func, int nargs,
                                           const uint32_t *args);

    if (!callback)
        return FALSE;

    monitor_rect = (RECT){ 0, 0, current_mode_cx(), current_mode_cy() };
    if (clip && (clip->right <= monitor_rect.left ||
                 clip->bottom <= monitor_rect.top ||
                 clip->left >= monitor_rect.right ||
                 clip->top >= monitor_rect.bottom))
        return TRUE;

    if (g_compat32_mode) {
        uint32_t args[4] = {
            (uint32_t)(ULONG_PTR)primary_monitor(),
            (uint32_t)(ULONG_PTR)hdc,
            (uint32_t)(ULONG_PTR)&monitor_rect,
            (uint32_t)data
        };
        return (BOOL)compat32_callback_args(
            (uint32_t)(ULONG_PTR)callback, 4, args);
    }

    return callback(primary_monitor(), hdc, &monitor_rect, data);
}

#define QDC_ALL_PATHS_U32          0x00000001u
#define QDC_ONLY_ACTIVE_PATHS_U32  0x00000002u
#define QDC_DATABASE_CURRENT_U32   0x00000004u
#define QDC_OPTION_MASK_U32        0x00000070u

typedef struct {
    DWORD low_part;
    LONG high_part;
} DISPLAYCONFIG_LUID_U32;

typedef struct {
    UINT numerator;
    UINT denominator;
} DISPLAYCONFIG_RATIONAL_U32;

typedef struct {
    UINT cx;
    UINT cy;
} DISPLAYCONFIG_REGION_U32;

typedef struct {
    ULONGLONG pixel_rate;
    DISPLAYCONFIG_RATIONAL_U32 hsync;
    DISPLAYCONFIG_RATIONAL_U32 vsync;
    DISPLAYCONFIG_REGION_U32 active_size;
    DISPLAYCONFIG_REGION_U32 total_size;
    UINT video_standard;
    LONG scanline_ordering;
} DISPLAYCONFIG_VIDEO_SIGNAL_INFO_U32;

typedef struct {
    UINT width;
    UINT height;
    LONG pixel_format;
    POINT position;
} DISPLAYCONFIG_SOURCE_MODE_U32;

typedef struct {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO_U32 signal;
} DISPLAYCONFIG_TARGET_MODE_U32;

typedef struct {
    LONG info_type;
    UINT id;
    DISPLAYCONFIG_LUID_U32 adapter_id;
    union {
        DISPLAYCONFIG_TARGET_MODE_U32 target_mode;
        DISPLAYCONFIG_SOURCE_MODE_U32 source_mode;
    } data;
} DISPLAYCONFIG_MODE_INFO_U32;

typedef struct {
    DISPLAYCONFIG_LUID_U32 adapter_id;
    UINT id;
    UINT mode_info_idx;
    UINT status_flags;
} DISPLAYCONFIG_PATH_SOURCE_INFO_U32;

typedef struct {
    DISPLAYCONFIG_LUID_U32 adapter_id;
    UINT id;
    UINT mode_info_idx;
    LONG output_technology;
    LONG rotation;
    LONG scaling;
    DISPLAYCONFIG_RATIONAL_U32 refresh_rate;
    LONG scanline_ordering;
    BOOL target_available;
    UINT status_flags;
} DISPLAYCONFIG_PATH_TARGET_INFO_U32;

typedef struct {
    DISPLAYCONFIG_PATH_SOURCE_INFO_U32 source;
    DISPLAYCONFIG_PATH_TARGET_INFO_U32 target;
    UINT flags;
} DISPLAYCONFIG_PATH_INFO_U32;

typedef struct {
    LONG type;
    UINT size;
    DISPLAYCONFIG_LUID_U32 adapter_id;
    UINT id;
} DISPLAYCONFIG_DEVICE_INFO_HEADER_U32;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER_U32 header;
    WCHAR gdi_device_name[32];
} DISPLAYCONFIG_SOURCE_DEVICE_NAME_U32;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER_U32 header;
    UINT flags;
    LONG output_technology;
    WORD edid_manufacturer_id;
    WORD edid_product_code_id;
    UINT connector_instance;
    WCHAR friendly_name[64];
    WCHAR device_path[128];
} DISPLAYCONFIG_TARGET_DEVICE_NAME_U32;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER_U32 header;
    UINT width;
    UINT height;
    DISPLAYCONFIG_TARGET_MODE_U32 target_mode;
} DISPLAYCONFIG_TARGET_PREFERRED_MODE_U32;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER_U32 header;
    WCHAR adapter_device_path[128];
} DISPLAYCONFIG_ADAPTER_NAME_U32;

typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER_U32 header;
    LONG base_output_technology;
} DISPLAYCONFIG_TARGET_BASE_TYPE_U32;

_Static_assert(sizeof(DISPLAYCONFIG_VIDEO_SIGNAL_INFO_U32) == 48,
               "DISPLAYCONFIG_VIDEO_SIGNAL_INFO layout");
_Static_assert(sizeof(DISPLAYCONFIG_MODE_INFO_U32) == 64,
               "DISPLAYCONFIG_MODE_INFO layout");
_Static_assert(sizeof(DISPLAYCONFIG_PATH_INFO_U32) == 72,
               "DISPLAYCONFIG_PATH_INFO layout");
_Static_assert(sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME_U32) == 84,
               "DISPLAYCONFIG_SOURCE_DEVICE_NAME layout");
_Static_assert(sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME_U32) == 420,
               "DISPLAYCONFIG_TARGET_DEVICE_NAME layout");

static const DISPLAYCONFIG_LUID_U32 display_adapter_luid = { 1, 0 };

static BOOL display_config_flags_valid(UINT flags)
{
    UINT query = flags & ~QDC_OPTION_MASK_U32;
    return query == QDC_ALL_PATHS_U32 ||
           query == QDC_ONLY_ACTIVE_PATHS_U32 ||
           query == QDC_DATABASE_CURRENT_U32;
}

static BOOL display_config_luid_valid(DISPLAYCONFIG_LUID_U32 luid)
{
    return luid.low_part == display_adapter_luid.low_part &&
           luid.high_part == display_adapter_luid.high_part;
}

static void display_config_copy_wide(WCHAR *destination, UINT capacity,
                                     const char *source)
{
    UINT i = 0;
    if (!destination || !capacity)
        return;
    while (source && source[i] && i + 1 < capacity) {
        destination[i] = (WCHAR)(BYTE)source[i];
        i++;
    }
    destination[i] = 0;
}

static void display_config_fill_target_mode(DISPLAYCONFIG_TARGET_MODE_U32 *mode)
{
    UINT width = (UINT)current_mode_cx();
    UINT height = (UINT)current_mode_cy();
    memset(mode, 0, sizeof(*mode));
    mode->signal.pixel_rate = (ULONGLONG)width * height * 60u;
    mode->signal.hsync = (DISPLAYCONFIG_RATIONAL_U32){ height * 60u, 1 };
    mode->signal.vsync = (DISPLAYCONFIG_RATIONAL_U32){ 60, 1 };
    mode->signal.active_size = (DISPLAYCONFIG_REGION_U32){ width, height };
    mode->signal.total_size = mode->signal.active_size;
    mode->signal.scanline_ordering = 1; /* progressive */
}

static LONG WINAPI GetDisplayConfigBufferSizes_u32(UINT flags,
                                                    UINT *path_count,
                                                    UINT *mode_count)
{
    if (!display_config_flags_valid(flags) || !path_count || !mode_count)
        return 87; /* ERROR_INVALID_PARAMETER */
    *path_count = 1;
    *mode_count = 2;
    return 0;
}

static LONG WINAPI QueryDisplayConfig_u32(
    UINT flags, UINT *path_count, DISPLAYCONFIG_PATH_INFO_U32 *paths,
    UINT *mode_count, DISPLAYCONFIG_MODE_INFO_U32 *modes,
    UINT *topology_id)
{
    UINT query = flags & ~QDC_OPTION_MASK_U32;
    if (!display_config_flags_valid(flags) || !path_count || !mode_count ||
        (query == QDC_DATABASE_CURRENT_U32 && !topology_id) ||
        (query != QDC_DATABASE_CURRENT_U32 && topology_id))
        return 87; /* ERROR_INVALID_PARAMETER */

    if (*path_count < 1 || *mode_count < 2 || !paths || !modes) {
        *path_count = 1;
        *mode_count = 2;
        return 122; /* ERROR_INSUFFICIENT_BUFFER */
    }

    memset(paths, 0, sizeof(*paths));
    memset(modes, 0, sizeof(*modes) * 2);

    paths[0].source.adapter_id = display_adapter_luid;
    paths[0].source.id = 0;
    paths[0].source.mode_info_idx = 0;
    paths[0].source.status_flags = 1; /* DISPLAYCONFIG_SOURCE_IN_USE */
    paths[0].target.adapter_id = display_adapter_luid;
    paths[0].target.id = 0;
    paths[0].target.mode_info_idx = 1;
    paths[0].target.output_technology = 17; /* indirect virtual */
    paths[0].target.rotation = 1;           /* identity */
    paths[0].target.scaling = 1;            /* identity */
    paths[0].target.refresh_rate =
        (DISPLAYCONFIG_RATIONAL_U32){ 60, 1 };
    paths[0].target.scanline_ordering = 1;  /* progressive */
    paths[0].target.target_available = TRUE;
    paths[0].target.status_flags = 1;       /* DISPLAYCONFIG_TARGET_IN_USE */
    paths[0].flags = 1;                     /* DISPLAYCONFIG_PATH_ACTIVE */

    modes[0].info_type = 1; /* DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE */
    modes[0].id = 0;
    modes[0].adapter_id = display_adapter_luid;
    modes[0].data.source_mode.width = (UINT)current_mode_cx();
    modes[0].data.source_mode.height = (UINT)current_mode_cy();
    modes[0].data.source_mode.pixel_format = 4; /* 32 bpp */
    modes[0].data.source_mode.position = (POINT){ 0, 0 };

    modes[1].info_type = 2; /* DISPLAYCONFIG_MODE_INFO_TYPE_TARGET */
    modes[1].id = 0;
    modes[1].adapter_id = display_adapter_luid;
    display_config_fill_target_mode(&modes[1].data.target_mode);

    *path_count = 1;
    *mode_count = 2;
    if (topology_id)
        *topology_id = 1; /* DISPLAYCONFIG_TOPOLOGY_INTERNAL */
    return 0;
}

static LONG WINAPI DisplayConfigGetDeviceInfo_u32(
    DISPLAYCONFIG_DEVICE_INFO_HEADER_U32 *header)
{
    if (!header || header->size < sizeof(*header) || header->id != 0 ||
        !display_config_luid_valid(header->adapter_id))
        return 87; /* ERROR_INVALID_PARAMETER */

    switch (header->type) {
    case 1: { /* DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME */
        if (header->size < sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME_U32))
            return 122;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME_U32 *name =
            (DISPLAYCONFIG_SOURCE_DEVICE_NAME_U32 *)header;
        memset(name->gdi_device_name, 0, sizeof(name->gdi_device_name));
        display_config_copy_wide(name->gdi_device_name, 32,
                                 "\\\\.\\DISPLAY1");
        return 0;
    }
    case 2: { /* DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME */
        if (header->size < sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME_U32))
            return 122;
        DISPLAYCONFIG_TARGET_DEVICE_NAME_U32 *name =
            (DISPLAYCONFIG_TARGET_DEVICE_NAME_U32 *)header;
        name->flags = 2; /* friendlyNameForced */
        name->output_technology = 17;
        name->edid_manufacturer_id = 0;
        name->edid_product_code_id = 0;
        name->connector_instance = 0;
        memset(name->friendly_name, 0, sizeof(name->friendly_name));
        memset(name->device_path, 0, sizeof(name->device_path));
        display_config_copy_wide(name->friendly_name, 64,
                                 "OsitoK Virtual Display");
        display_config_copy_wide(name->device_path, 128,
                                 "\\\\?\\DISPLAY#OSITOK#0");
        return 0;
    }
    case 3: { /* DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE */
        if (header->size < sizeof(DISPLAYCONFIG_TARGET_PREFERRED_MODE_U32))
            return 122;
        DISPLAYCONFIG_TARGET_PREFERRED_MODE_U32 *mode =
            (DISPLAYCONFIG_TARGET_PREFERRED_MODE_U32 *)header;
        mode->width = (UINT)current_mode_cx();
        mode->height = (UINT)current_mode_cy();
        display_config_fill_target_mode(&mode->target_mode);
        return 0;
    }
    case 4: { /* DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME */
        if (header->size < sizeof(DISPLAYCONFIG_ADAPTER_NAME_U32))
            return 122;
        DISPLAYCONFIG_ADAPTER_NAME_U32 *name =
            (DISPLAYCONFIG_ADAPTER_NAME_U32 *)header;
        memset(name->adapter_device_path, 0,
               sizeof(name->adapter_device_path));
        display_config_copy_wide(name->adapter_device_path, 128,
                                 "\\\\?\\PCI#VEN_1AF4&DEV_1050");
        return 0;
    }
    case 6: { /* DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_BASE_TYPE */
        if (header->size < sizeof(DISPLAYCONFIG_TARGET_BASE_TYPE_U32))
            return 122;
        DISPLAYCONFIG_TARGET_BASE_TYPE_U32 *type =
            (DISPLAYCONFIG_TARGET_BASE_TYPE_U32 *)header;
        type->base_output_technology = 17;
        return 0;
    }
    default:
        return 50; /* ERROR_NOT_SUPPORTED */
    }
}

static LONG WINAPI GetDpiForMonitor_u32(HANDLE monitor, UINT dpi_type,
                                         UINT *dpi_x, UINT *dpi_y)
{
    (void)dpi_type;
    if (monitor != primary_monitor() || !dpi_x || !dpi_y)
        return (LONG)0x80070057; /* E_INVALIDARG */
    *dpi_x = 96;
    *dpi_y = 96;
    return 0; /* S_OK */
}

static LONG WINAPI SetProcessDpiAwareness_u32(UINT awareness)
{
    return awareness <= 2 ? 0 : (LONG)0x80070057;
}

typedef struct {
    DWORD cb;
    WCHAR device_name[32];
    WCHAR device_string[128];
    DWORD state_flags;
    WCHAR device_id[128];
    WCHAR device_key[128];
} DISPLAY_DEVICEW_U32;

typedef struct {
    DWORD cb;
    char device_name[32];
    char device_string[128];
    DWORD state_flags;
    char device_id[128];
    char device_key[128];
} DISPLAY_DEVICEA_U32;

_Static_assert(sizeof(DISPLAY_DEVICEA_U32) == 424,
               "DISPLAY_DEVICEA layout");

static void display_copy_ascii(WCHAR *dst, DWORD capacity, const char *src)
{
    DWORD i = 0;
    if (!capacity)
        return;
    while (i + 1 < capacity && src[i]) {
        dst[i] = (WCHAR)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static BOOL display_name_matches(PCWSTR wide, const char *ascii)
{
    DWORD i = 0;
    if (!wide)
        return FALSE;
    while (ascii[i] && wide[i] == (WCHAR)(unsigned char)ascii[i])
        i++;
    return ascii[i] == 0 && wide[i] == 0;
}

static void display_copy_ansi(char *dst, DWORD capacity, const char *src)
{
    DWORD i = 0;
    if (!capacity)
        return;
    while (i + 1 < capacity && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static BOOL WINAPI EnumDisplayDevicesA_u32(const char *device,
                                            DWORD device_index,
                                            PVOID display_device, DWORD flags)
{
    (void)flags;
    DISPLAY_DEVICEA_U32 *out = (DISPLAY_DEVICEA_U32 *)display_device;
    static const char adapter_name[] = "\\\\.\\DISPLAY1";

    if (!out || out->cb < sizeof(*out)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (device_index != 0)
        return FALSE;
    if (device && u32_strcmp(device, adapter_name) != 0)
        return FALSE;

    memset(out, 0, sizeof(*out));
    out->cb = sizeof(*out);
    if (!device) {
        display_copy_ansi(out->device_name, 32, adapter_name);
        display_copy_ansi(out->device_string, 128, "OsitoK Display Adapter");
        out->state_flags = 0x00000015; /* attached, primary, VGA-compatible */
        display_copy_ansi(out->device_id, 128, "PCI\\VEN_1AF4&DEV_1050");
        display_copy_ansi(out->device_key, 128,
                          "\\Registry\\Machine\\System\\CurrentControlSet"
                          "\\Control\\Video\\{OSITOK}\\0000");
    } else {
        display_copy_ansi(out->device_name, 32,
                          "\\\\.\\DISPLAY1\\Monitor0");
        display_copy_ansi(out->device_string, 128, "Generic PnP Monitor");
        out->state_flags = 0x00000003; /* active and attached */
        display_copy_ansi(out->device_id, 128, "MONITOR\\OSITO0001");
        display_copy_ansi(out->device_key, 128,
                          "\\Registry\\Machine\\System\\CurrentControlSet"
                          "\\Enum\\DISPLAY\\OSITO0001");
    }
    return TRUE;
}

static BOOL WINAPI EnumDisplayDevicesW_u32(PCWSTR device, DWORD device_index,
                                            PVOID display_device, DWORD flags)
{
    (void)flags;
    DISPLAY_DEVICEW_U32 *out = (DISPLAY_DEVICEW_U32 *)display_device;
    static const char adapter_name[] = "\\\\.\\DISPLAY1";

    if (!out || out->cb < sizeof(*out)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    if (device_index != 0)
        return FALSE;

    if (device && !display_name_matches(device, adapter_name))
        return FALSE;

    memset(out, 0, sizeof(*out));
    out->cb = sizeof(*out);
    if (!device) {
        display_copy_ascii(out->device_name, 32, adapter_name);
        display_copy_ascii(out->device_string, 128, "OsitoK Display Adapter");
        out->state_flags = 0x00000015; /* attached, primary, VGA-compatible */
        display_copy_ascii(out->device_id, 128, "PCI\\VEN_1AF4&DEV_1050");
        display_copy_ascii(out->device_key, 128,
                           "\\Registry\\Machine\\System\\CurrentControlSet"
                           "\\Control\\Video\\{OSITOK}\\0000");
    } else {
        display_copy_ascii(out->device_name, 32,
                           "\\\\.\\DISPLAY1\\Monitor0");
        display_copy_ascii(out->device_string, 128, "Generic PnP Monitor");
        out->state_flags = 0x00000003; /* active and attached */
        display_copy_ascii(out->device_id, 128, "MONITOR\\OSITO0001");
        display_copy_ascii(out->device_key, 128,
                           "\\Registry\\Machine\\System\\CurrentControlSet"
                           "\\Enum\\DISPLAY\\OSITO0001");
    }
    return TRUE;
}

/* ── Display mode enumeration/change (for SoftDrv/DirectDraw) ──── */

typedef struct {
    char dmDeviceName[32];
    uint16_t dmSpecVersion;
    uint16_t dmDriverVersion;
    uint16_t dmSize;
    uint16_t dmDriverExtra;
    uint32_t dmFields;
    /* union { ... } — we just need position + display settings */
    int32_t  dmPositionX, dmPositionY;
    uint32_t dmDisplayOrientation;
    uint32_t dmDisplayFixedOutput;
    /* end union */
    int16_t  dmColor;
    int16_t  dmDuplex;
    int16_t  dmYResolution;
    int16_t  dmTTOption;
    int16_t  dmCollate;
    char     dmFormName[32];
    uint16_t dmLogPixels;
    uint32_t dmBitsPerPel;
    uint32_t dmPelsWidth;
    uint32_t dmPelsHeight;
    uint32_t dmDisplayFlags;
    uint32_t dmDisplayFrequency;
    uint32_t dmICMMethod;
    uint32_t dmICMIntent;
    uint32_t dmMediaType;
    uint32_t dmDitherType;
    uint32_t dmReserved1;
    uint32_t dmReserved2;
    uint32_t dmPanningWidth;
    uint32_t dmPanningHeight;
} DEVMODEA;

typedef struct {
    WCHAR dmDeviceName[32];
    uint16_t dmSpecVersion;
    uint16_t dmDriverVersion;
    uint16_t dmSize;
    uint16_t dmDriverExtra;
    uint32_t dmFields;
    int32_t  dmPositionX, dmPositionY;
    uint32_t dmDisplayOrientation;
    uint32_t dmDisplayFixedOutput;
    int16_t  dmColor;
    int16_t  dmDuplex;
    int16_t  dmYResolution;
    int16_t  dmTTOption;
    int16_t  dmCollate;
    WCHAR    dmFormName[32];
    uint16_t dmLogPixels;
    uint32_t dmBitsPerPel;
    uint32_t dmPelsWidth;
    uint32_t dmPelsHeight;
    uint32_t dmDisplayFlags;
    uint32_t dmDisplayFrequency;
    uint32_t dmICMMethod;
    uint32_t dmICMIntent;
    uint32_t dmMediaType;
    uint32_t dmDitherType;
    uint32_t dmReserved1;
    uint32_t dmReserved2;
    uint32_t dmPanningWidth;
    uint32_t dmPanningHeight;
} DEVMODEW;

_Static_assert(sizeof(DEVMODEA) == 156, "Win32 DEVMODEA ABI");
_Static_assert(sizeof(DEVMODEW) == 220, "Win32 DEVMODEW ABI");

#define DM_BITSPERPEL          0x00040000
#define DM_PELSWIDTH           0x00080000
#define DM_PELSHEIGHT          0x00100000
#define DM_DISPLAYFLAGS        0x00200000
#define DM_DISPLAYFREQUENCY    0x00400000

#define CDS_UPDATEREGISTRY     0x00000001
#define CDS_TEST               0x00000002
#define CDS_FULLSCREEN         0x00000004
#define CDS_GLOBAL             0x00000008
#define CDS_SET_PRIMARY        0x00000010
#define CDS_VIDEOPARAMETERS    0x00000020
#define CDS_ENABLE_UNSAFE      0x00000100
#define CDS_DISABLE_UNSAFE     0x00000200
#define CDS_RESET_EX           0x20000000
#define CDS_RESET              0x40000000
#define CDS_NORESET            0x10000000
#define CDS_SUPPORTED_FLAGS    (CDS_UPDATEREGISTRY | CDS_TEST | CDS_FULLSCREEN | \
                                CDS_GLOBAL | CDS_SET_PRIMARY | \
                                CDS_VIDEOPARAMETERS | CDS_ENABLE_UNSAFE | \
                                CDS_DISABLE_UNSAFE | CDS_RESET_EX | CDS_RESET | \
                                CDS_NORESET)

#define DISP_CHANGE_SUCCESSFUL  0
#define DISP_CHANGE_BADMODE    -2
#define DISP_CHANGE_BADFLAGS   -4
#define DISP_CHANGE_BADPARAM   -5
#define ENUM_CURRENT_SETTINGS  ((uint32_t)-1)
#define ENUM_REGISTRY_SETTINGS ((uint32_t)-2)

#define DEVMODEA_DISPLAY_SIZE 124
#define DEVMODEW_DISPLAY_SIZE 188

extern uint32_t display_get_mode_count(void) __attribute__((weak));
extern const boot_display_mode_t *display_get_mode(uint32_t index)
    __attribute__((weak));

static BOOL display_resolution_valid(uint32_t width, uint32_t height)
{
    return width > 0 && height > 0 && width <= 0x7FFFFFFFU &&
           height <= 0x7FFFFFFFU;
}

static BOOL display_backend_resolution_at(uint32_t wanted,
                                          uint32_t *width,
                                          uint32_t *height)
{
    uint32_t backend_count = display_get_mode_count
        ? display_get_mode_count() : 0;
    uint32_t unique = 0;

    if (display_get_mode) {
        for (uint32_t i = 0; i < backend_count; i++) {
            const boot_display_mode_t *mode = display_get_mode(i);
            if (!mode || !display_resolution_valid(mode->width, mode->height))
                continue;

            BOOL duplicate = FALSE;
            for (uint32_t previous = 0; previous < i; previous++) {
                const boot_display_mode_t *candidate =
                    display_get_mode(previous);
                if (candidate && candidate->width == mode->width &&
                    candidate->height == mode->height) {
                    duplicate = TRUE;
                    break;
                }
            }
            if (duplicate) continue;
            if (unique++ == wanted) {
                if (width) *width = mode->width;
                if (height) *height = mode->height;
                return TRUE;
            }
        }
    }

    /* Some display backends expose only the active scanout and no mode table.
     * Publish that mode once, unless the firmware table already contained it. */
    uint32_t active_width = (uint32_t)screen_cx();
    uint32_t active_height = (uint32_t)screen_cy();
    if (!display_resolution_valid(active_width, active_height))
        return FALSE;

    if (display_get_mode) {
        for (uint32_t i = 0; i < backend_count; i++) {
            const boot_display_mode_t *mode = display_get_mode(i);
            if (mode && mode->width == active_width &&
                mode->height == active_height)
                return FALSE;
        }
    }

    if (unique != wanted) return FALSE;
    if (width) *width = active_width;
    if (height) *height = active_height;
    return TRUE;
}

uint32_t user32_get_display_resolution_count(void)
{
    uint32_t count = 0;
    while (count < BOOT_MAX_DISPLAY_MODES + 1 &&
           display_backend_resolution_at(count, NULL, NULL))
        count++;
    return count;
}

BOOL user32_get_display_resolution(uint32_t index, uint32_t *width,
                                   uint32_t *height)
{
    if (!width || !height) return FALSE;
    return display_backend_resolution_at(index, width, height);
}

static BOOL display_mode_supported(uint32_t width, uint32_t height,
                                   uint32_t bpp, uint32_t frequency)
{
    if (bpp != 16 && bpp != 32)
        return FALSE;
    if (frequency != 0 && frequency != 1 && frequency != 60)
        return FALSE;

    uint32_t count = user32_get_display_resolution_count();
    for (uint32_t i = 0; i < count; i++) {
        uint32_t candidate_w = 0, candidate_h = 0;
        if (user32_get_display_resolution(i, &candidate_w, &candidate_h) &&
            candidate_w == width && candidate_h == height)
            return TRUE;
    }
    return FALSE;
}

static LONG apply_display_settings(uint32_t fields, uint32_t width,
                                   uint32_t height, uint32_t bpp,
                                   uint32_t frequency, uint32_t flags)
{
    uint32_t current_w, current_h, current_bpp, current_frequency;

    if (flags & ~CDS_SUPPORTED_FLAGS)
        return DISP_CHANGE_BADFLAGS;

    current_mode_values(&current_w, &current_h, &current_bpp,
                        &current_frequency);
    if (!(fields & DM_PELSWIDTH)) width = current_w;
    if (!(fields & DM_PELSHEIGHT)) height = current_h;
    if (!(fields & DM_BITSPERPEL)) bpp = current_bpp;
    if (!(fields & DM_DISPLAYFREQUENCY)) frequency = current_frequency;

    if (!display_mode_supported(width, height, bpp, frequency))
        return DISP_CHANGE_BADMODE;
    if (flags & (CDS_TEST | CDS_NORESET))
        return DISP_CHANGE_SUCCESSFUL;

    user_display_mode.active = 1;
    user_display_mode.fullscreen = (flags & CDS_FULLSCREEN) != 0;
    user_display_mode.width = width;
    user_display_mode.height = height;
    user_display_mode.bpp = bpp;
    user_display_mode.frequency = frequency > 1 ? frequency : 60;
    user_display_mode.owner_pid = GetCurrentProcessId();
    g_abs_prev_valid = 0;
    if (cursor_pos.x >= (LONG)width) cursor_pos.x = (LONG)width - 1;
    if (cursor_pos.y >= (LONG)height) cursor_pos.y = (LONG)height - 1;
    sync_all_window_compositor_state();

    serial_puts("[USER32] ChangeDisplaySettings -> ");
    serial_putdec(width); serial_puts("x"); serial_putdec(height);
    serial_puts("x"); serial_putdec(bpp);
    serial_puts(user_display_mode.fullscreen ? " fullscreen\n" : " desktop\n");
    return DISP_CHANGE_SUCCESSFUL;
}

static LONG restore_display_settings(uint32_t flags)
{
    if (flags & ~CDS_SUPPORTED_FLAGS)
        return DISP_CHANGE_BADFLAGS;
    if (flags & (CDS_TEST | CDS_NORESET))
        return DISP_CHANGE_SUCCESSFUL;

    memset(&user_display_mode, 0, sizeof(user_display_mode));
    g_abs_prev_valid = 0;
    sync_all_window_compositor_state();
    serial_puts("[USER32] ChangeDisplaySettings -> registry mode\n");
    return DISP_CHANGE_SUCCESSFUL;
}

LONG WINAPI ChangeDisplaySettingsA(DEVMODEA *dm, uint32_t flags)
{
    if (!dm)
        return restore_display_settings(flags);
    if (dm->dmSize < DEVMODEA_DISPLAY_SIZE)
        return DISP_CHANGE_BADPARAM;
    return apply_display_settings(dm->dmFields, dm->dmPelsWidth,
                                  dm->dmPelsHeight, dm->dmBitsPerPel,
                                  dm->dmDisplayFrequency, flags);
}

LONG WINAPI ChangeDisplaySettingsW(DEVMODEW *dm, uint32_t flags)
{
    if (!dm)
        return restore_display_settings(flags);
    if (dm->dmSize < DEVMODEW_DISPLAY_SIZE)
        return DISP_CHANGE_BADPARAM;
    return apply_display_settings(dm->dmFields, dm->dmPelsWidth,
                                  dm->dmPelsHeight, dm->dmBitsPerPel,
                                  dm->dmDisplayFrequency, flags);
}

BOOL WINAPI EnumDisplaySettingsA(const char *device, uint32_t mode, DEVMODEA *dm)
{
    (void)device;
    if (!dm) return FALSE;
    uint16_t caller_size = dm->dmSize;
    uint16_t output_size =
        (caller_size >= DEVMODEA_DISPLAY_SIZE && caller_size <= sizeof(*dm))
            ? caller_size : (uint16_t)sizeof(*dm);
    static const uint8_t bpps[] = { 16, 32 };

    /* LWJGL 2's current-mode query passes a full DEVMODEA buffer without
     * initializing dmSize. Windows accepts that legacy call pattern. */
    memset(dm, 0, output_size);
    dm->dmSize = output_size;
    dm->dmSpecVersion = 0x0401;
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                   DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    dm->dmDisplayFrequency = 60;

    if (mode == ENUM_CURRENT_SETTINGS) {
        current_mode_values(&dm->dmPelsWidth, &dm->dmPelsHeight,
                            &dm->dmBitsPerPel, &dm->dmDisplayFrequency);
        return TRUE;
    }
    if (mode == ENUM_REGISTRY_SETTINGS) {
        dm->dmBitsPerPel = 32;
        dm->dmPelsWidth = (uint32_t)screen_cx();
        dm->dmPelsHeight = (uint32_t)screen_cy();
        return TRUE;
    }

    /* index = res-major, bpp-minor */
    const uint32_t nres = user32_get_display_resolution_count();
    const uint32_t nbpp = sizeof(bpps) / sizeof(bpps[0]);
    if (mode >= nres * nbpp) return FALSE;
    uint32_t ri = mode / nbpp, bi = mode % nbpp;
    dm->dmBitsPerPel = bpps[bi];
    return user32_get_display_resolution(ri, &dm->dmPelsWidth,
                                         &dm->dmPelsHeight);
}

BOOL WINAPI EnumDisplaySettingsW(const WCHAR *device, uint32_t mode, DEVMODEW *dm)
{
    (void)device;
    if (!dm) return FALSE;
    uint16_t caller_size = dm->dmSize;
    uint16_t output_size =
        (caller_size >= DEVMODEW_DISPLAY_SIZE && caller_size <= sizeof(*dm))
            ? caller_size : (uint16_t)sizeof(*dm);
    static const uint8_t bpps[] = { 16, 32 };

    memset(dm, 0, output_size);
    dm->dmSize = output_size;
    dm->dmSpecVersion = 0x0401;
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                   DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    dm->dmDisplayFrequency = 60;

    if (mode == ENUM_CURRENT_SETTINGS) {
        current_mode_values(&dm->dmPelsWidth, &dm->dmPelsHeight,
                            &dm->dmBitsPerPel, &dm->dmDisplayFrequency);
        return TRUE;
    }
    if (mode == ENUM_REGISTRY_SETTINGS) {
        dm->dmBitsPerPel = 32;
        dm->dmPelsWidth = (uint32_t)screen_cx();
        dm->dmPelsHeight = (uint32_t)screen_cy();
        return TRUE;
    }

    const uint32_t nbpp = sizeof(bpps) / sizeof(bpps[0]);
    const uint32_t nres = user32_get_display_resolution_count();
    if (mode >= nres * nbpp) return FALSE;
    dm->dmBitsPerPel = bpps[mode % nbpp];
    return user32_get_display_resolution(mode / nbpp, &dm->dmPelsWidth,
                                         &dm->dmPelsHeight);
}

LONG WINAPI ChangeDisplaySettingsExA(const char *device, DEVMODEA *dm,
                                     HWND window, uint32_t flags, void *param)
{
    (void)device; (void)window; (void)param;
    return ChangeDisplaySettingsA(dm, flags);
}

LONG WINAPI ChangeDisplaySettingsExW(const WCHAR *device, DEVMODEW *dm,
                                     HWND window, uint32_t flags, void *param)
{
    (void)device; (void)window; (void)param;
    return ChangeDisplaySettingsW(dm, flags);
}

BOOL WINAPI EnumDisplaySettingsExA(const char *device, uint32_t mode,
                                   DEVMODEA *dm, uint32_t flags)
{
    (void)flags;
    return EnumDisplaySettingsA(device, mode, dm);
}

BOOL WINAPI EnumDisplaySettingsExW(const WCHAR *device, uint32_t mode,
                                   DEVMODEW *dm, uint32_t flags)
{
    (void)flags;
    return EnumDisplaySettingsW(device, mode, dm);
}

#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWL_USERDATA   (-21)
#define GWL_WNDPROC    (-4)
#define GWL_HWNDPARENT (-8)
#define GWL_ID         (-12)

LONG_PTR WINAPI GetWindowLongPtrA(HWND hWnd, int nIndex)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;
    switch (nIndex) {
    case GWL_STYLE:    return (LONG_PTR)w->style;
    case GWL_EXSTYLE:  return (LONG_PTR)w->ex_style;
    case GWL_USERDATA: return (LONG_PTR)w->user_data;
    case GWL_WNDPROC:  return (LONG_PTR)w->wndproc;
    case GWL_HWNDPARENT:
        return (LONG_PTR)(ULONG_PTR)(w->parent ? w->parent : w->owner);
    case GWL_ID:       return (LONG_PTR)(ULONG_PTR)w->menu;
    default:           return 0;
    }
}

LONG WINAPI GetWindowLongA(HWND hWnd, int nIndex)
{
    return (LONG)GetWindowLongPtrA(hWnd, nIndex);
}

LONG_PTR WINAPI SetWindowLongPtrA(HWND hWnd, int nIndex,
                                  LONG_PTR dwNewLong)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;
    LONG_PTR old = GetWindowLongPtrA(hWnd, nIndex);
    switch (nIndex) {
    case GWL_STYLE:
        w->style = (DWORD)dwNewLong;
        w->visible = (w->style & WS_VISIBLE) != 0;
        menu_sync_system_window(hWnd);
        sync_all_window_compositor_state();
        break;
    case GWL_EXSTYLE:
        w->ex_style = (DWORD)dwNewLong;
        place_window_in_z_order(w, HWND_TOP);
        break;
    case GWL_USERDATA: w->user_data = (PVOID)(ULONG_PTR)dwNewLong; break;
    case GWL_WNDPROC:
        w->wndproc = (WNDPROC)(ULONG_PTR)dwNewLong;
        w->unicode = FALSE;
        break;
    case GWL_ID:       w->menu = (HMENU)(ULONG_PTR)dwNewLong; break;
    case GWL_HWNDPARENT: {
        HWND relation = (HWND)(ULONG_PTR)dwNewLong;
        if (w->style & WS_CHILD) {
            SetParent(hWnd, relation);
        } else if (!relation || hwnd_is_desktop(relation) ||
                   (find_window(relation) && relation != hWnd)) {
            w->owner = (!relation || hwnd_is_desktop(relation)) ? NULL : relation;
            sync_all_window_compositor_state();
        } else {
            SetLastError(1400);
        }
        break;
    }
    }
    return old;
}

LONG WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
    return (LONG)SetWindowLongPtrA(hWnd, nIndex, dwNewLong);
}

BOOL WINAPI SetLayeredWindowAttributes(HWND hWnd, DWORD crKey,
                                       BYTE bAlpha, DWORD dwFlags)
{
    WINDOW *w = find_window(hWnd);
    if (!w || (dwFlags & ~3U) != 0 || (dwFlags & 3U) == 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    w->layered_color_key = crKey;
    w->layered_alpha = bAlpha;
    w->layered_flags = dwFlags;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI GetLayeredWindowAttributes(HWND hWnd, DWORD *crKey,
                                       BYTE *alpha, DWORD *flags)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !w->layered_flags) {
        SetLastError(w ? 87 : 1400);
        return FALSE;
    }
    if (crKey) *crKey = w->layered_color_key;
    if (alpha) *alpha = w->layered_alpha;
    if (flags) *flags = w->layered_flags;
    return TRUE;
}

HWND WINAPI GetForegroundWindow(void)
{
    if (!user32_foreground_active)
        return NULL;
    WINDOW *active = find_window(active_hwnd);
    if (active && !active->destroying)
        return window_root(active, NULL)->handle;
    return NULL;
}

HWND WINAPI WindowFromPoint(POINT point)
{
    WINDOW *hit = NULL;
    for (int i = 0; i < window_count; i++) {
        WINDOW *w = &windows[i];
        if (!w->used || !window_should_render(w))
            continue;
        int screen_x, screen_y;
        window_screen_origin(w, &screen_x, &screen_y);
        if (point.x >= screen_x && point.y >= screen_y &&
            point.x < screen_x + w->width && point.y < screen_y + w->height &&
            (!hit || w->render_z > hit->render_z))
            hit = w;
    }
    return hit ? hit->handle : NULL;
}

HWND WINAPI GetAncestor(HWND hWnd, UINT flags)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return NULL;
    if (flags == GA_PARENT) return w->parent;
    if (flags != GA_ROOT && flags != GA_ROOTOWNER) {
        SetLastError(87);
        return NULL;
    }

    w = window_root(w, NULL);
    if (flags == GA_ROOTOWNER) {
        for (int i = 0; i < MAX_WINDOWS && w && w->owner; i++) {
            WINDOW *owner = find_window(w->owner);
            if (!owner || owner == w) break;
            w = window_root(owner, NULL);
        }
    }
    return w ? w->handle : NULL;
}

static int is_top_level_window(const WINDOW *w)
{
    return w && w->used && !w->message_only && !w->parent &&
           !(w->style & WS_CHILD);
}

static int same_getwindow_group(const WINDOW *a, const WINDOW *b)
{
    if (!a || !b || a->parent != b->parent)
        return 0;
    if (!a->parent) {
        if (!is_top_level_window(a) || !is_top_level_window(b))
            return 0;
        if (window_is_topmost(a) != window_is_topmost(b))
            return 0;
    }
    return 1;
}

HWND WINAPI GetTopWindow(HWND hWnd)
{
    WINDOW *parent = hWnd ? find_window(hWnd) : NULL;
    if (hWnd && !parent && !hwnd_is_desktop(hWnd)) {
        SetLastError(1400);
        return NULL;
    }

    WINDOW *top = NULL;
    for (int i = 0; i < window_count; i++) {
        WINDOW *candidate = &windows[i];
        if (!candidate->used || candidate->message_only)
            continue;
        int matches = hWnd && !hwnd_is_desktop(hWnd)
            ? candidate->parent == hWnd
            : is_top_level_window(candidate);
        if (matches && (!top || sibling_is_above(candidate, top)))
            top = candidate;
    }
    return top ? top->handle : NULL;
}

HWND WINAPI GetWindow(HWND hWnd, UINT uCmd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        SetLastError(1400);
        return NULL;
    }

    if (uCmd == GW_OWNER)
        return w->owner;
    if (uCmd == GW_CHILD)
        return GetTopWindow(hWnd);
    if (uCmd == GW_ENABLEDPOPUP) {
        WINDOW *popup = NULL;
        for (int i = 0; i < window_count; i++) {
            WINDOW *candidate = &windows[i];
            if (candidate->used && candidate->owner == hWnd &&
                !(candidate->style & WS_DISABLED) &&
                (!popup || sibling_is_above(candidate, popup)))
                popup = candidate;
        }
        return popup ? popup->handle : hWnd;
    }
    if (uCmd > GW_HWNDPREV) {
        SetLastError(87);
        return NULL;
    }

    WINDOW *result = NULL;
    for (int i = 0; i < window_count; i++) {
        WINDOW *candidate = &windows[i];
        if (candidate == w || !candidate->used ||
            !same_getwindow_group(candidate, w))
            continue;

        if (uCmd == GW_HWNDFIRST) {
            if (!result || sibling_is_above(candidate, result)) result = candidate;
        } else if (uCmd == GW_HWNDLAST) {
            if (!result || sibling_is_above(result, candidate)) result = candidate;
        } else if (uCmd == GW_HWNDNEXT && sibling_is_above(w, candidate)) {
            if (!result || sibling_is_above(candidate, result)) result = candidate;
        } else if (uCmd == GW_HWNDPREV && sibling_is_above(candidate, w)) {
            if (!result || sibling_is_above(result, candidate)) result = candidate;
        }
    }

    if (uCmd == GW_HWNDFIRST || uCmd == GW_HWNDLAST) {
        if (!result || (uCmd == GW_HWNDFIRST && sibling_is_above(w, result)) ||
            (uCmd == GW_HWNDLAST && sibling_is_above(result, w)))
            result = w;
    }
    return result ? result->handle : NULL;
}

BOOL WINAPI BringWindowToTop(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        SetLastError(1400);
        return FALSE;
    }
    if (!place_window_in_z_order(w, HWND_TOP))
        return FALSE;

    /* Raising an owner also raises every directly owned popup while preserving
     * the popups' current order. Raise bottom-to-top so the former top popup
     * remains the top popup after the operation. */
    HWND owned[MAX_WINDOWS];
    int owned_count = 0;
    for (int i = 0; i < window_count && owned_count < MAX_WINDOWS; i++) {
        WINDOW *candidate = &windows[i];
        if (candidate->used && candidate->owner == hWnd)
            owned[owned_count++] = candidate->handle;
    }
    sort_window_handles_top_to_bottom(owned, owned_count);
    for (int i = owned_count - 1; i >= 0; i--) {
        WINDOW *popup = find_window(owned[i]);
        if (popup && popup->owner == hWnd)
            place_window_in_z_order(popup, HWND_TOP);
    }
    if (!w->parent)
        dispatch_wm_activate(w);
    return TRUE;
}

HWND WINAPI SetFocus(HWND hWnd)
{
    HWND old = focus_hwnd;
    /* [CAPDIAG — uncommitted] who flips focus (the capture-flap suspect) */
    {
        static int n = 0;
        if (u32_input_diagnostics_active() && hWnd != old && n++ < 40) {
            extern uint32_t compat32_get_last_caller_eip(void);
            serial_puts("[CAP] SetFocus(0x");
            serial_puthex((uint64_t)(ULONG_PTR)hWnd, 8);
            serial_puts(") was=0x"); serial_puthex((uint64_t)(ULONG_PTR)old, 8);
            serial_puts(" eip=0x"); serial_puthex(compat32_get_last_caller_eip(), 8);
            serial_puts("\n");
        }
    }
    WINDOW *target = hWnd ? find_window(hWnd) : NULL;
    /* Only track real windows we know about; NULL clears focus. */
    BOOL accepted = hWnd == NULL || target;
    if (accepted) {
        HWND current = focus_hwnd;
        if (target) {
            WINDOW *root = window_root(target, NULL);
            WINDOW *active = window_root(find_window(active_hwnd), NULL);
            if (!user32_foreground_active || active != root) {
                /* Seed the requested descendant before WM_ACTIVATE so a
                 * synchronous activation callback does not introduce an
                 * artificial root SETFOCUS/KILLFOCUS pair. */
                focus_hwnd = hWnd;
                dispatch_wm_activate(root);
            } else if (root && root->compositor_id &&
                       compositor_focus_window && window_should_render(root)) {
                compositor_focus_window(root->compositor_id);
            }
        }

        focus_hwnd = hWnd;
        /* NT delivers WM_KILLFOCUS to the loser and WM_SETFOCUS to the gainer.
         * UE1 re-arms input/capture on WM_SETFOCUS and releases on
         * WM_KILLFOCUS (ViewportWndProc focus cases near 0x1110715D/
         * 0x11107223); WinDrv itself calls SetFocus at OpenWindow
         * (0x11105AAD), in its WndProc (0x111071C5) and at ResizeViewport
         * (0x1110A25C), so mode changes depend on these messages flowing. */
        if (current != hWnd) {
            if (current)
                dispatch_focus_message(current, WM_KILLFOCUS, hWnd);
            if (hWnd)
                dispatch_focus_message(hWnd, WM_SETFOCUS, current);
        }
    }
    return old;
}
#define U32_DESKTOP_WINDOW_HANDLE       ((HWND)(ULONG_PTR)0xD0000001U)
#define U32_DEFAULT_WINSTA_HANDLE       ((HANDLE)(ULONG_PTR)0xD0000002U)
#define U32_DEFAULT_DESKTOP_HANDLE      ((HANDLE)(ULONG_PTR)0xD0000003U)
#define U32_USER_OBJECT_HANDLE_TAG      0xD1000000U
#define U32_USER_OBJECT_HANDLE_MASK     0xFFF00000U
#define U32_USER_OBJECT_CAP             64
#define U32_USER_OBJECT_OPEN_CAP        128
#define U32_PROCESS_STATION_CAP         64
#define U32_USER_OBJECT_NAME_CAP        64

#define U32_UOI_FLAGS                   1
#define U32_UOI_NAME                    2
#define U32_UOI_TYPE                    3
#define U32_WSF_VISIBLE                 0x0001U

typedef enum {
    U32_USER_OBJECT_NONE = 0,
    U32_USER_OBJECT_WINSTA,
    U32_USER_OBJECT_DESKTOP
} U32_USER_OBJECT_TYPE;

typedef struct {
    BOOL fInherit;
    BOOL fReserved;
    DWORD dwFlags;
} U32_USER_OBJECT_FLAGS;

typedef struct {
    BOOL used;
    USHORT generation;
    U32_USER_OBJECT_TYPE type;
    HANDLE handle;
    HANDLE station;
    DWORD flags;
    DWORD open_refs;
    BOOL inheritable;
    WCHAR name[U32_USER_OBJECT_NAME_CAP];
} U32_USER_OBJECT;

typedef struct {
    BOOL used;
    DWORD pid;
    HANDLE object;
    DWORD count;
} U32_USER_OBJECT_OPEN;

typedef struct {
    BOOL used;
    DWORD pid;
    HANDLE station;
} U32_PROCESS_STATION;

typedef struct {
    U32_USER_OBJECT_TYPE type;
    DWORD flags;
    BOOL inheritable;
    WCHAR name[U32_USER_OBJECT_NAME_CAP];
} U32_USER_OBJECT_VIEW;

_Static_assert(sizeof(U32_USER_OBJECT_FLAGS) == 12,
               "Win32 USEROBJECTFLAGS ABI");

static U32_USER_OBJECT user_objects[U32_USER_OBJECT_CAP];
static U32_USER_OBJECT_OPEN user_object_opens[U32_USER_OBJECT_OPEN_CAP];
static U32_PROCESS_STATION process_stations[U32_PROCESS_STATION_CAP];
static spinlock_t user_object_lock = SPINLOCK_INIT;

static BOOL user_object_handle_equal(HANDLE left, HANDLE right)
{
    return (uint32_t)(ULONG_PTR)left == (uint32_t)(ULONG_PTR)right;
}

static WCHAR user_object_fold_char(WCHAR value)
{
    if (value >= 'A' && value <= 'Z')
        return value + ('a' - 'A');
    return value;
}

static BOOL user_object_name_equal(PCWSTR left, PCWSTR right)
{
    if (!left || !right)
        return left == right;
    for (DWORD i = 0; i < U32_USER_OBJECT_NAME_CAP; i++) {
        WCHAR a = user_object_fold_char(left[i]);
        WCHAR b = user_object_fold_char(right[i]);
        if (a != b)
            return FALSE;
        if (!a)
            return TRUE;
    }
    return FALSE;
}

static BOOL user_object_copy_name(WCHAR *destination, PCWSTR source)
{
    DWORD i = 0;
    if (source) {
        while (i + 1 < U32_USER_OBJECT_NAME_CAP && source[i]) {
            destination[i] = source[i];
            i++;
        }
        if (source[i])
            return FALSE;
    }
    destination[i] = 0;
    return TRUE;
}

static void user_object_copy_ascii_name(WCHAR *destination,
                                        const char *source)
{
    DWORD i = 0;
    while (i + 1 < U32_USER_OBJECT_NAME_CAP && source[i]) {
        destination[i] = (WCHAR)(BYTE)source[i];
        i++;
    }
    destination[i] = 0;
}

static DWORD user_object_wide_length(PCWSTR value)
{
    DWORD length = 0;
    while (length < U32_USER_OBJECT_NAME_CAP && value[length])
        length++;
    return length;
}

static U32_USER_OBJECT *user_object_find_locked(HANDLE handle)
{
    uint32_t value = (uint32_t)(ULONG_PTR)handle;
    if ((value & U32_USER_OBJECT_HANDLE_MASK) != U32_USER_OBJECT_HANDLE_TAG)
        return NULL;

    uint32_t encoded_slot = value & 0xFFU;
    if (!encoded_slot || encoded_slot > U32_USER_OBJECT_CAP)
        return NULL;

    U32_USER_OBJECT *object = &user_objects[encoded_slot - 1];
    if (!object->used || !user_object_handle_equal(object->handle, handle))
        return NULL;
    return object;
}

static U32_USER_OBJECT_TYPE user_object_type_locked(HANDLE handle)
{
    if (user_object_handle_equal(handle, U32_DEFAULT_WINSTA_HANDLE))
        return U32_USER_OBJECT_WINSTA;
    if (user_object_handle_equal(handle, U32_DEFAULT_DESKTOP_HANDLE))
        return U32_USER_OBJECT_DESKTOP;
    U32_USER_OBJECT *object = user_object_find_locked(handle);
    return object ? object->type : U32_USER_OBJECT_NONE;
}

static HANDLE user_object_process_station_locked(DWORD pid)
{
    for (int i = 0; i < U32_PROCESS_STATION_CAP; i++)
        if (process_stations[i].used && process_stations[i].pid == pid)
            return process_stations[i].station;
    return U32_DEFAULT_WINSTA_HANDLE;
}

static BOOL user_object_has_station_association_locked(HANDLE station)
{
    for (int i = 0; i < U32_PROCESS_STATION_CAP; i++)
        if (process_stations[i].used &&
            user_object_handle_equal(process_stations[i].station, station))
            return TRUE;
    return FALSE;
}

static void user_object_maybe_destroy_locked(U32_USER_OBJECT *object)
{
    if (!object || !object->used || object->open_refs)
        return;
    if (object->type == U32_USER_OBJECT_WINSTA &&
        user_object_has_station_association_locked(object->handle))
        return;

    object->used = FALSE;
    object->handle = NULL;
    object->station = NULL;
    object->flags = 0;
    object->inheritable = FALSE;
    object->name[0] = 0;
}

static BOOL user_object_add_open_locked(DWORD pid, HANDLE handle)
{
    int free_slot = -1;
    for (int i = 0; i < U32_USER_OBJECT_OPEN_CAP; i++) {
        U32_USER_OBJECT_OPEN *open = &user_object_opens[i];
        if (!open->used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (open->pid == pid &&
            user_object_handle_equal(open->object, handle)) {
            if (open->count == (DWORD)-1)
                return FALSE;
            open->count++;
            return TRUE;
        }
    }
    if (free_slot < 0)
        return FALSE;

    user_object_opens[free_slot].used = TRUE;
    user_object_opens[free_slot].pid = pid;
    user_object_opens[free_slot].object = handle;
    user_object_opens[free_slot].count = 1;
    return TRUE;
}

static BOOL user_object_remove_open_locked(DWORD pid, HANDLE handle)
{
    for (int i = 0; i < U32_USER_OBJECT_OPEN_CAP; i++) {
        U32_USER_OBJECT_OPEN *open = &user_object_opens[i];
        if (!open->used || open->pid != pid ||
            !user_object_handle_equal(open->object, handle))
            continue;
        if (--open->count == 0)
            open->used = FALSE;
        return TRUE;
    }
    return FALSE;
}

static U32_USER_OBJECT *user_object_find_named_locked(
    U32_USER_OBJECT_TYPE type, PCWSTR name, HANDLE station)
{
    if (!name || !name[0])
        return NULL;
    for (int i = 0; i < U32_USER_OBJECT_CAP; i++) {
        U32_USER_OBJECT *object = &user_objects[i];
        if (!object->used || object->type != type ||
            !user_object_name_equal(object->name, name))
            continue;
        if (type != U32_USER_OBJECT_DESKTOP ||
            user_object_handle_equal(object->station, station))
            return object;
    }
    return NULL;
}

static U32_USER_OBJECT *user_object_allocate_locked(
    U32_USER_OBJECT_TYPE type, PCWSTR name, HANDLE station, DWORD flags,
    BOOL inheritable)
{
    for (int i = 0; i < U32_USER_OBJECT_CAP; i++) {
        U32_USER_OBJECT *object = &user_objects[i];
        if (object->used)
            continue;

        USHORT generation = (USHORT)((object->generation + 1U) & 0x0FFFU);
        if (!generation)
            generation = 1;
        object->generation = generation;
        object->type = type;
        object->handle = (HANDLE)(ULONG_PTR)(
            U32_USER_OBJECT_HANDLE_TAG | ((uint32_t)generation << 8) |
            (uint32_t)(i + 1));
        object->station = station;
        object->flags = flags;
        object->open_refs = 0;
        object->inheritable = inheritable;
        if (!user_object_copy_name(object->name, name)) {
            object->handle = NULL;
            object->type = U32_USER_OBJECT_NONE;
            return NULL;
        }
        object->used = TRUE;
        return object;
    }
    return NULL;
}

static BOOL user_object_attributes_inheritable(PVOID attributes)
{
    if (!attributes)
        return FALSE;
    if (g_compat32_mode) {
        const DWORD *fields = (const DWORD *)attributes;
        return fields[0] >= 12 && fields[2] != 0;
    }

    typedef struct {
        DWORD length;
        PVOID security_descriptor;
        BOOL inherit_handle;
    } U32_SECURITY_ATTRIBUTES64;
    const U32_SECURITY_ATTRIBUTES64 *security =
        (const U32_SECURITY_ATTRIBUTES64 *)attributes;
    return security->length >= sizeof(*security) && security->inherit_handle;
}

static BOOL user_object_snapshot(HANDLE handle, U32_USER_OBJECT_VIEW *view)
{
    if (!handle || !view)
        return FALSE;

    memset(view, 0, sizeof(*view));
    if (user_object_handle_equal(handle, U32_DEFAULT_WINSTA_HANDLE)) {
        view->type = U32_USER_OBJECT_WINSTA;
        view->flags = U32_WSF_VISIBLE;
        user_object_copy_ascii_name(view->name, "WinSta0");
        return TRUE;
    }
    if (user_object_handle_equal(handle, U32_DEFAULT_DESKTOP_HANDLE)) {
        view->type = U32_USER_OBJECT_DESKTOP;
        user_object_copy_ascii_name(view->name, "Default");
        return TRUE;
    }

    spin_lock(&user_object_lock);
    U32_USER_OBJECT *object = user_object_find_locked(handle);
    if (object) {
        view->type = object->type;
        view->flags = object->flags;
        view->inheritable = object->inheritable;
        user_object_copy_name(view->name, object->name);
    }
    spin_unlock(&user_object_lock);
    return object != NULL;
}

static void user_object_reset(void)
{
    user_object_lock = SPINLOCK_INIT;
    for (int i = 0; i < U32_USER_OBJECT_CAP; i++) {
        user_objects[i].used = FALSE;
        user_objects[i].open_refs = 0;
    }
    for (int i = 0; i < U32_USER_OBJECT_OPEN_CAP; i++)
        user_object_opens[i].used = FALSE;
    for (int i = 0; i < U32_PROCESS_STATION_CAP; i++)
        process_stations[i].used = FALSE;
}

static void user_object_release_process(DWORD pid)
{
    if (!pid)
        return;

    spin_lock(&user_object_lock);
    for (int i = 0; i < U32_PROCESS_STATION_CAP; i++)
        if (process_stations[i].used && process_stations[i].pid == pid)
            process_stations[i].used = FALSE;

    for (int i = 0; i < U32_USER_OBJECT_OPEN_CAP; i++) {
        U32_USER_OBJECT_OPEN *open = &user_object_opens[i];
        if (!open->used || open->pid != pid)
            continue;
        U32_USER_OBJECT *object = user_object_find_locked(open->object);
        if (object) {
            if (object->open_refs >= open->count)
                object->open_refs -= open->count;
            else
                object->open_refs = 0;
        }
        open->used = FALSE;
    }
    for (int i = 0; i < U32_USER_OBJECT_CAP; i++)
        user_object_maybe_destroy_locked(&user_objects[i]);
    spin_unlock(&user_object_lock);
}

HWND WINAPI GetDesktopWindow(void) { return U32_DESKTOP_WINDOW_HANDLE; }
HWND WINAPI GetShellWindow(void) { return GetDesktopWindow(); }

static HANDLE WINAPI GetProcessWindowStation_stub(void)
{
    DWORD pid = GetCurrentProcessId();
    spin_lock(&user_object_lock);
    HANDLE station = user_object_process_station_locked(pid);
    spin_unlock(&user_object_lock);
    return station;
}

static HANDLE WINAPI CreateWindowStationW_stub(PCWSTR name, DWORD flags,
                                                 DWORD access, PVOID attrs)
{
    (void)access;
    if (flags != 0) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    WCHAR checked_name[U32_USER_OBJECT_NAME_CAP];
    if (!user_object_copy_name(checked_name, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }

    static const WCHAR interactive_name[] = {
        'W', 'i', 'n', 'S', 't', 'a', '0', 0
    };
    DWORD pid = GetCurrentProcessId();
    BOOL inheritable = user_object_attributes_inheritable(attrs);
    BOOL existing = FALSE;
    HANDLE handle = NULL;

    spin_lock(&user_object_lock);
    U32_USER_OBJECT *object = NULL;
    if (name && user_object_name_equal(checked_name, interactive_name)) {
        handle = U32_DEFAULT_WINSTA_HANDLE;
        existing = TRUE;
    } else {
        object = user_object_find_named_locked(U32_USER_OBJECT_WINSTA,
                                               checked_name, NULL);
        if (object) {
            handle = object->handle;
            existing = TRUE;
        } else {
            object = user_object_allocate_locked(
                U32_USER_OBJECT_WINSTA, checked_name, NULL, 0,
                inheritable);
            if (object)
                handle = object->handle;
        }
    }

    if (!handle || !user_object_add_open_locked(pid, handle)) {
        if (object && !existing)
            user_object_maybe_destroy_locked(object);
        spin_unlock(&user_object_lock);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    if (object)
        object->open_refs++;
    spin_unlock(&user_object_lock);

    if (existing)
        SetLastError(183); /* ERROR_ALREADY_EXISTS */
    return handle;
}

static BOOL WINAPI SetProcessWindowStation_stub(HANDLE station)
{
    DWORD pid = GetCurrentProcessId();
    spin_lock(&user_object_lock);
    if (user_object_type_locked(station) != U32_USER_OBJECT_WINSTA) {
        spin_unlock(&user_object_lock);
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    int existing_slot = -1;
    int free_slot = -1;
    for (int i = 0; i < U32_PROCESS_STATION_CAP; i++) {
        if (process_stations[i].used && process_stations[i].pid == pid) {
            existing_slot = i;
            break;
        }
        if (!process_stations[i].used && free_slot < 0)
            free_slot = i;
    }

    if (user_object_handle_equal(station, U32_DEFAULT_WINSTA_HANDLE)) {
        if (existing_slot >= 0)
            process_stations[existing_slot].used = FALSE;
        spin_unlock(&user_object_lock);
        return TRUE;
    }
    if (existing_slot < 0)
        existing_slot = free_slot;
    if (existing_slot < 0) {
        spin_unlock(&user_object_lock);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }

    process_stations[existing_slot].used = TRUE;
    process_stations[existing_slot].pid = pid;
    process_stations[existing_slot].station = station;
    spin_unlock(&user_object_lock);
    return TRUE;
}

static BOOL user_object_close(HANDLE handle, U32_USER_OBJECT_TYPE type)
{
    DWORD pid = GetCurrentProcessId();
    spin_lock(&user_object_lock);
    if (user_object_type_locked(handle) != type) {
        spin_unlock(&user_object_lock);
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (type == U32_USER_OBJECT_WINSTA &&
        user_object_handle_equal(user_object_process_station_locked(pid),
                                 handle)) {
        spin_unlock(&user_object_lock);
        SetLastError(170); /* ERROR_BUSY */
        return FALSE;
    }
    if (type == U32_USER_OBJECT_DESKTOP &&
        user_object_handle_equal(handle, U32_DEFAULT_DESKTOP_HANDLE)) {
        spin_unlock(&user_object_lock);
        SetLastError(170); /* ERROR_BUSY */
        return FALSE;
    }
    if (!user_object_remove_open_locked(pid, handle)) {
        spin_unlock(&user_object_lock);
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }

    U32_USER_OBJECT *object = user_object_find_locked(handle);
    if (object && object->open_refs)
        object->open_refs--;
    user_object_maybe_destroy_locked(object);
    spin_unlock(&user_object_lock);
    return TRUE;
}

static BOOL WINAPI CloseWindowStation_stub(HANDLE station)
{
    return user_object_close(station, U32_USER_OBJECT_WINSTA);
}

static HANDLE WINAPI GetThreadDesktop_stub(DWORD thread_id)
{
    (void)thread_id;
    return U32_DEFAULT_DESKTOP_HANDLE;
}

static HANDLE WINAPI CreateDesktopW_stub(PCWSTR name, PCWSTR device,
                                          PVOID devmode, DWORD flags,
                                          DWORD access, PVOID attrs)
{
    (void)access;
    if (!name || !name[0] || device || devmode || (flags & ~1U)) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    WCHAR checked_name[U32_USER_OBJECT_NAME_CAP];
    if (!user_object_copy_name(checked_name, name)) {
        SetLastError(206); /* ERROR_FILENAME_EXCED_RANGE */
        return NULL;
    }

    static const WCHAR default_name[] = {
        'D', 'e', 'f', 'a', 'u', 'l', 't', 0
    };
    DWORD pid = GetCurrentProcessId();
    BOOL inheritable = user_object_attributes_inheritable(attrs);
    BOOL existing = FALSE;
    HANDLE handle = NULL;

    spin_lock(&user_object_lock);
    HANDLE station = user_object_process_station_locked(pid);
    U32_USER_OBJECT *object = NULL;
    if (user_object_handle_equal(station, U32_DEFAULT_WINSTA_HANDLE) &&
        user_object_name_equal(checked_name, default_name)) {
        handle = U32_DEFAULT_DESKTOP_HANDLE;
        existing = TRUE;
    } else {
        object = user_object_find_named_locked(U32_USER_OBJECT_DESKTOP,
                                               checked_name, station);
        if (object) {
            handle = object->handle;
            existing = TRUE;
        } else {
            object = user_object_allocate_locked(
                U32_USER_OBJECT_DESKTOP, checked_name, station, 0,
                inheritable);
            if (object)
                handle = object->handle;
        }
    }

    if (!handle || !user_object_add_open_locked(pid, handle)) {
        if (object && !existing)
            user_object_maybe_destroy_locked(object);
        spin_unlock(&user_object_lock);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    if (object)
        object->open_refs++;
    spin_unlock(&user_object_lock);

    if (existing)
        SetLastError(183); /* ERROR_ALREADY_EXISTS */
    return handle;
}

static BOOL WINAPI CloseDesktop_stub(HANDLE desktop)
{
    return user_object_close(desktop, U32_USER_OBJECT_DESKTOP);
}

static BOOL user_object_write_flags(const U32_USER_OBJECT_VIEW *view,
                                    PVOID info, DWORD length, DWORD *needed)
{
    *needed = sizeof(U32_USER_OBJECT_FLAGS);
    if (!info || length < sizeof(U32_USER_OBJECT_FLAGS)) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }

    U32_USER_OBJECT_FLAGS *output = (U32_USER_OBJECT_FLAGS *)info;
    output->fInherit = view->inheritable;
    output->fReserved = FALSE;
    output->dwFlags = view->flags;
    return TRUE;
}

static BOOL user_object_write_wide(PCWSTR value, PVOID info, DWORD length,
                                   DWORD *needed)
{
    DWORD characters = user_object_wide_length(value) + 1;
    DWORD required = characters * sizeof(WCHAR);
    *needed = required;
    if (!info || length < required) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    for (DWORD i = 0; i < characters; i++)
        ((WCHAR *)info)[i] = value[i];
    return TRUE;
}

static BOOL user_object_write_ansi(PCWSTR value, PVOID info, DWORD length,
                                   DWORD *needed)
{
    DWORD characters = user_object_wide_length(value) + 1;
    *needed = characters;
    if (!info || length < characters) {
        SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return FALSE;
    }
    for (DWORD i = 0; i < characters; i++) {
        WCHAR character = value[i];
        ((char *)info)[i] = character <= 0xFF ? (char)character : '?';
    }
    return TRUE;
}

static BOOL user_object_information(HANDLE object, int index, PVOID info,
                                    DWORD length, DWORD *needed, BOOL wide)
{
    if (!needed) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    U32_USER_OBJECT_VIEW view;
    if (!user_object_snapshot(object, &view)) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return FALSE;
    }
    if (index == U32_UOI_FLAGS)
        return user_object_write_flags(&view, info, length, needed);

    WCHAR type_name[U32_USER_OBJECT_NAME_CAP];
    PCWSTR value = NULL;
    if (index == U32_UOI_NAME) {
        value = view.name;
    } else if (index == U32_UOI_TYPE) {
        user_object_copy_ascii_name(
            type_name, view.type == U32_USER_OBJECT_WINSTA
                           ? "WindowStation" : "Desktop");
        value = type_name;
    } else {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    return wide ? user_object_write_wide(value, info, length, needed)
                : user_object_write_ansi(value, info, length, needed);
}

static BOOL WINAPI GetUserObjectInformationA_stub(HANDLE object, int index,
                                                   PVOID info, DWORD length,
                                                   DWORD *needed)
{
    return user_object_information(object, index, info, length, needed, FALSE);
}

static BOOL WINAPI GetUserObjectInformationW_stub(HANDLE object, int index,
                                                   PVOID info, DWORD length,
                                                   DWORD *needed)
{
    return user_object_information(object, index, info, length, needed, TRUE);
}
HWND WINAPI GetActiveWindow(void)
{
    if (!user32_foreground_active)
        return NULL;
    WINDOW *active = find_window(active_hwnd);
    return active ? window_root(active, NULL)->handle : NULL;
}

/* ── Cursor / Input ────────────────────────────────────────── */

BOOL WINAPI CreateCaret(HWND hWnd, HBITMAP hBitmap, int nWidth, int nHeight)
{
    (void)hBitmap;
    (void)nWidth;
    (void)nHeight;
    if (!hWnd || !find_window(hWnd)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }

    caret_hwnd = hWnd;
    caret_pos.x = caret_pos.y = 0;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI DestroyCaret(void)
{
    if (!caret_hwnd)
        return FALSE;
    caret_hwnd = NULL;
    return TRUE;
}

BOOL WINAPI SetCaretPos(int X, int Y)
{
    if (!caret_hwnd)
        return FALSE;
    caret_pos.x = X;
    caret_pos.y = Y;
    return TRUE;
}

BOOL WINAPI SetCursorPos(int X, int Y)
{
    POINT requested = { X, Y };
    confine_cursor_point(&requested);
    cursor_pos = requested;
    relative_pointer_note_warp(requested);
    {
        static int n = 0;
        if (u32_input_diagnostics_active() && n < 96 &&
            relative_pointer_mode_active()) {
            extern uint32_t compat32_get_last_caller_eip(void);
            log_input_prefix("[MOUSE-CURSOR] SetCursorPos");
            serial_puts(" x="); serial_putdec((uint64_t)(uint32_t)X);
            serial_puts(" y="); serial_putdec((uint64_t)(uint32_t)Y);
            serial_puts(" eip=0x"); serial_puthex(compat32_get_last_caller_eip(), 8);
            serial_puts("\n");
            n++;
        }
    }
    return TRUE;
}

BOOL WINAPI GetCursorPos(LPPOINT lpPoint)
{
    if (!lpPoint) return FALSE;
    *lpPoint = cursor_pos;
    {
        static int n = 0;
        if (u32_input_diagnostics_active() && n < 96 &&
            relative_pointer_mode_active()) {
            extern uint32_t compat32_get_last_caller_eip(void);
            log_input_prefix("[MOUSE-CURSOR] GetCursorPos");
            serial_puts(" x="); serial_putdec((uint64_t)(uint32_t)lpPoint->x);
            serial_puts(" y="); serial_putdec((uint64_t)(uint32_t)lpPoint->y);
            serial_puts(" eip=0x"); serial_puthex(compat32_get_last_caller_eip(), 8);
            serial_puts("\n");
            n++;
        }
    }
    return TRUE;
}

BOOL WINAPI GetCursorInfo(PVOID cursor_info)
{
    if (!cursor_info) {
        SetLastError(87);
        return FALSE;
    }

    WINDOW *target = find_window(mouse_input_target());
    BOOL visible = !target ||
        cursor_display_count(target->owner_pid, target->owner_tid) >= 0;
    if (g_compat32_mode) {
        uint32_t *info = (uint32_t *)cursor_info;
        if (info[0] != 20) {
            SetLastError(87);
            return FALSE;
        }
        info[1] = visible ? 1U : 0U; /* CURSOR_SHOWING */
        info[2] = (uint32_t)(ULONG_PTR)current_cursor;
        info[3] = (uint32_t)cursor_pos.x;
        info[4] = (uint32_t)cursor_pos.y;
    } else {
        typedef struct {
            DWORD cbSize;
            DWORD flags;
            HCURSOR cursor;
            POINT position;
        } CURSOR_INFO_NATIVE;
        CURSOR_INFO_NATIVE *info = (CURSOR_INFO_NATIVE *)cursor_info;
        if (info->cbSize != sizeof(*info)) {
            SetLastError(87);
            return FALSE;
        }
        info->flags = visible ? 1U : 0U;
        info->cursor = current_cursor;
        info->position = cursor_pos;
    }
    return TRUE;
}

BOOL WINAPI GetLastInputInfo(PVOID last_input_info)
{
    DWORD *info = (DWORD *)last_input_info;
    if (!info || info[0] != 8) {
        SetLastError(87);
        return FALSE;
    }
    info[1] = g_last_input_time;
    return TRUE;
}

int WINAPI ShowCursor(BOOL bShow)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    int count = cursor_change_count(pid, tid, bShow ? 1 : -1, TRUE);
    if (bShow && count >= 0 &&
        relative_pointer.owner_pid == pid &&
        relative_pointer.owner_tid == tid &&
        !(clip_active && clip_owner_pid == pid))
        relative_pointer_reset();
    return count;
}

bool user32_cursor_overlay_visible(uint32_t compositor_id)
{
    if (!compositor_id)
        return true;
    for (int i = 0; i < window_count; i++) {
        WINDOW *window = &windows[i];
        if (!window->used || window->compositor_id != compositor_id)
            continue;
        WINDOW *captured = find_window(capture_hwnd);
        if (captured && window_root(captured, NULL) == window)
            window = captured;
        return cursor_display_count(window->owner_pid, window->owner_tid) >= 0;
    }
    return true;
}

BOOL WINAPI ClipCursor(const RECT *lpRect)
{
    if (lpRect) {
        if (lpRect->right <= lpRect->left ||
            lpRect->bottom <= lpRect->top) {
            SetLastError(87); /* ERROR_INVALID_PARAMETER */
            return FALSE;
        }
        clip_rect = *lpRect;
        clip_active = 1;
        clip_owner_pid = GetCurrentProcessId();
        clip_owner_tid = GetCurrentThreadId();
        confine_cursor_point(&cursor_pos);
    } else {
        clip_active = 0;
        clip_owner_pid = 0;
        clip_owner_tid = 0;
    }
    relative_pointer_reset();
    return TRUE;
}

BOOL WINAPI GetClipCursor(RECT *lpRect)
{
    if (!lpRect) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    if (clip_active) {
        *lpRect = clip_rect;
    } else {
        lpRect->left = 0;
        lpRect->top = 0;
        lpRect->right = current_mode_cx();
        lpRect->bottom = current_mode_cy();
    }
    return TRUE;
}

BOOL WINAPI TrackMouseEvent(PVOID lpEventTrack)
{
    typedef struct {
        DWORD cbSize;
        DWORD dwFlags;
        uint32_t hwndTrack;
        DWORD dwHoverTime;
    } TRACKMOUSEEVENT32;

    if (!lpEventTrack) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    DWORD cb_size = *(const DWORD *)lpEventTrack;
    DWORD flags;
    HWND hwnd;
    DWORD hover_time;
    if (cb_size == sizeof(TRACKMOUSEEVENT32)) {
        TRACKMOUSEEVENT32 *event32 = (TRACKMOUSEEVENT32 *)lpEventTrack;
        flags = event32->dwFlags;
        hwnd = (HWND)(ULONG_PTR)event32->hwndTrack;
        hover_time = event32->dwHoverTime;
    } else if (cb_size >= sizeof(TRACKMOUSEEVENT)) {
        TRACKMOUSEEVENT *event64 = (TRACKMOUSEEVENT *)lpEventTrack;
        flags = event64->dwFlags;
        hwnd = event64->hwndTrack;
        hover_time = event64->dwHoverTime;
    } else {
        SetLastError(87);
        return FALSE;
    }

    WINDOW *w = find_window(hwnd);
    if (!w) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }

    {
        static UINT trace_count;
        if (trace_count++ < 32) {
            serial_puts("[USER32] TrackMouseEvent hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)hwnd, 8);
            serial_puts(" flags=0x");
            serial_puthex(flags, 8);
            serial_puts(" cb=");
            serial_putdec(cb_size);
            serial_puts("\n");
        }
    }

    if (flags & TME_QUERY) {
        if (cb_size == sizeof(TRACKMOUSEEVENT32)) {
            TRACKMOUSEEVENT32 *event32 = (TRACKMOUSEEVENT32 *)lpEventTrack;
            event32->dwFlags = w->mouse_track_flags;
            event32->dwHoverTime = w->mouse_hover_time;
        } else {
            TRACKMOUSEEVENT *event64 = (TRACKMOUSEEVENT *)lpEventTrack;
            event64->dwFlags = w->mouse_track_flags;
            event64->dwHoverTime = w->mouse_hover_time;
        }
        return TRUE;
    }

    DWORD requested = flags & (TME_HOVER | TME_LEAVE);
    if (flags & TME_CANCEL) {
        w->mouse_track_flags &= ~requested;
        if (!(w->mouse_track_flags & (TME_HOVER | TME_LEAVE)))
            w->mouse_track_flags &= ~TME_NONCLIENT;
        return TRUE;
    }
    if (!requested ||
        (flags & ~(TME_HOVER | TME_LEAVE | TME_NONCLIENT))) {
        SetLastError(87);
        return FALSE;
    }

    w->mouse_track_flags |= requested | (flags & TME_NONCLIENT);
    if (requested & TME_HOVER) {
        w->mouse_hover_time =
            (hover_time == HOVER_DEFAULT || !hover_time) ? 400 : hover_time;
        w->mouse_hover_start = shim_timeGetTime();
        w->mouse_hover_origin = cursor_pos;
    }
    return TRUE;
}

HWND WINAPI SetCapture(HWND hWnd)
{
    WINDOW *window = find_window(hWnd);
    if (!window) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return NULL;
    }
    if (window->owner_tid != GetCurrentThreadId()) {
        SetLastError(5); /* ERROR_ACCESS_DENIED */
        return NULL;
    }

    HWND old = capture_hwnd;
    capture_hwnd = hWnd;
    if (old && old != hWnd && native_move.window == old)
        native_move.window = NULL;
    if (old && old != hWnd && find_window(old))
        SendMessageA(old, WM_CAPTURECHANGED, 0, (LPARAM)hWnd);
    if (capture_hwnd == hWnd)
        msg_capture_pending_input(window);
    return old;
}

HWND WINAPI GetCapture(void)
{
    return capture_hwnd;
}

BOOL WINAPI ReleaseCapture(void)
{
    HWND old = capture_hwnd;
    capture_hwnd = NULL;
    if (native_move.window == old)
        native_move.window = NULL;
    if (old && find_window(old))
        SendMessageA(old, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

short WINAPI GetAsyncKeyState(int vKey)
{
    if (vKey < 0 || vKey > 255) return 0;
    short result = 0;
    if (key_state[vKey] & 0x80) result |= (short)0x8000; /* currently down */
    if (async_pressed[vKey])    { result |= 0x0001; async_pressed[vKey] = 0; }
    return result;
}

static HANDLE WINAPI GetKeyboardLayout_k32(DWORD thread_id)
{
    (void)thread_id;
    return (HANDLE)(ULONG_PTR)0x04090409; /* en-US */
}

static int WINAPI GetKeyboardLayoutList_k32(int capacity, HANDLE *layouts)
{
    if (capacity == 0)
        return 1;
    if (capacity < 0 || !layouts) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    layouts[0] = (HANDLE)(ULONG_PTR)0x04090409; /* en-US */
    return 1;
}

static UINT WINAPI ImmGetIMEFileNameW_k32(HANDLE layout, PWSTR filename,
                                           UINT capacity)
{
    (void)layout;
    if (filename && capacity)
        filename[0] = 0;
    return 0; /* The built-in en-US layout has no associated IME. */
}

static UINT WINAPI ImmGetIMEFileNameA_k32(HANDLE layout, PSTR filename,
                                           UINT capacity)
{
    (void)layout;
    if (filename && capacity)
        filename[0] = 0;
    return 0;
}

#define MAX_IMM_CONTEXTS 32

typedef struct {
    HANDLE handle;
    DWORD conversion;
    DWORD sentence;
    BOOL open;
    int used;
} IMM_CONTEXT_ENTRY;

typedef struct {
    HWND window;
    HANDLE context;
} IMM_ASSOCIATION;

static IMM_CONTEXT_ENTRY imm_contexts[MAX_IMM_CONTEXTS];
static IMM_ASSOCIATION imm_associations[MAX_WINDOWS];
static ULONG_PTR next_imm_context = 0x1A000001;

static IMM_CONTEXT_ENTRY *imm_find_context(HANDLE handle)
{
    for (int i = 0; i < MAX_IMM_CONTEXTS; i++)
        if (imm_contexts[i].used && imm_contexts[i].handle == handle)
            return &imm_contexts[i];
    return NULL;
}

static HANDLE WINAPI ImmCreateContext_k32(void)
{
    for (int i = 0; i < MAX_IMM_CONTEXTS; i++) {
        if (imm_contexts[i].used) continue;
        imm_contexts[i].handle = (HANDLE)next_imm_context++;
        imm_contexts[i].conversion = 0;
        imm_contexts[i].sentence = 0;
        imm_contexts[i].open = FALSE;
        imm_contexts[i].used = 1;
        return imm_contexts[i].handle;
    }
    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return NULL;
}

static BOOL WINAPI ImmDestroyContext_k32(HANDLE context)
{
    IMM_CONTEXT_ENTRY *entry = imm_find_context(context);
    if (!entry) return FALSE;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (imm_associations[i].context == context) {
            imm_associations[i].window = NULL;
            imm_associations[i].context = NULL;
        }
    }
    entry->used = 0;
    return TRUE;
}

static HANDLE WINAPI ImmGetContext_k32(HWND window)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (imm_associations[i].window == window)
            return imm_associations[i].context;
    return NULL;
}

static BOOL WINAPI ImmReleaseContext_k32(HWND window, HANDLE context)
{
    (void)window;
    (void)context;
    return TRUE;
}

static HANDLE WINAPI ImmAssociateContext_k32(HWND window, HANDLE context)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (imm_associations[i].window == window) {
            HANDLE previous = imm_associations[i].context;
            if (context) {
                imm_associations[i].context = context;
            } else {
                imm_associations[i].window = NULL;
                imm_associations[i].context = NULL;
            }
            return previous;
        }
        if (!imm_associations[i].window && free_slot < 0)
            free_slot = i;
    }

    if (window && context && free_slot >= 0) {
        imm_associations[free_slot].window = window;
        imm_associations[free_slot].context = context;
    }
    return NULL;
}

static BOOL WINAPI ImmGetOpenStatus_k32(HANDLE context)
{
    IMM_CONTEXT_ENTRY *entry = imm_find_context(context);
    return entry ? entry->open : FALSE;
}

static BOOL WINAPI ImmSetOpenStatus_k32(HANDLE context, BOOL open)
{
    IMM_CONTEXT_ENTRY *entry = imm_find_context(context);
    if (!entry) return FALSE;
    entry->open = open != FALSE;
    return TRUE;
}

static BOOL WINAPI ImmGetConversionStatus_k32(HANDLE context,
                                                DWORD *conversion,
                                                DWORD *sentence)
{
    IMM_CONTEXT_ENTRY *entry = imm_find_context(context);
    if (!entry) return FALSE;
    if (conversion) *conversion = entry->conversion;
    if (sentence) *sentence = entry->sentence;
    return TRUE;
}

static BOOL WINAPI ImmSetConversionStatus_k32(HANDLE context,
                                                DWORD conversion,
                                                DWORD sentence)
{
    IMM_CONTEXT_ENTRY *entry = imm_find_context(context);
    if (!entry) return FALSE;
    entry->conversion = conversion;
    entry->sentence = sentence;
    return TRUE;
}

static DWORD WINAPI ImmGetProperty_k32(HANDLE layout, DWORD index)
{
    (void)layout;
    (void)index;
    return 0; /* The built-in en-US layout exposes no IME properties. */
}

static LONG WINAPI ImmGetCompositionStringW_k32(HANDLE context, DWORD index,
                                                  PVOID buffer, DWORD bytes)
{
    (void)context;
    (void)index;
    (void)buffer;
    (void)bytes;
    return -1; /* IMM_ERROR_NODATA */
}

static BOOL WINAPI ImmSetCompositionStringW_k32(HANDLE context, DWORD index,
                                                  PCVOID composition,
                                                  DWORD composition_bytes,
                                                  PCVOID reading,
                                                  DWORD reading_bytes)
{
    (void)context;
    (void)index;
    (void)composition;
    (void)composition_bytes;
    (void)reading;
    (void)reading_bytes;
    return FALSE;
}

static DWORD WINAPI ImmGetCandidateListW_k32(HANDLE context, DWORD index,
                                               PVOID candidates, DWORD bytes)
{
    (void)context;
    (void)index;
    (void)candidates;
    (void)bytes;
    return 0;
}

static DWORD WINAPI ImmGetCandidateListCountW_k32(HANDLE context,
                                                    DWORD *list_count)
{
    (void)context;
    if (list_count) *list_count = 0;
    return 0;
}

static BOOL WINAPI ImmGetCompositionFontW_k32(HANDLE context, PVOID font)
{
    (void)context;
    (void)font;
    return FALSE;
}

static BOOL WINAPI ImmNotifyIME_k32(HANDLE context, DWORD action,
                                     DWORD index, DWORD value)
{
    (void)context;
    (void)action;
    (void)index;
    (void)value;
    return FALSE;
}

static BOOL WINAPI ImmSetCompositionWindow_k32(HANDLE context, PCVOID form)
{
    (void)context;
    (void)form;
    return FALSE;
}

static BOOL WINAPI ImmSetCandidateWindow_k32(HANDLE context, PCVOID form)
{
    (void)context;
    (void)form;
    return FALSE;
}

static DWORD WINAPI GetDoubleClickTime_k32(void)
{
    return 500;
}

static DWORD WINAPI GetCaretBlinkTime_k32(void)
{
    return 530;
}

static DWORD WINAPI GetGuiResources_k32(HANDLE process, DWORD flags)
{
    (void)process;
    switch (flags) {
    case 0: /* GR_GDIOBJECTS */
    case 1: /* GR_USEROBJECTS */
    case 2: /* GR_GDIOBJECTS_PEAK */
    case 4: /* GR_USEROBJECTS_PEAK */
        return 1;
    default:
        return 0;
    }
}

short WINAPI GetKeyState(int nVirtKey)
{
    if (nVirtKey < 0 || nVirtKey > 255) return 0;
    short result = 0;
    /* UE1/WinDrv polls GetKeyState for all keys once per frame. In this
     * single-foreground-thread shim, return the live state so movement keys
     * are visible outside individual WM_KEYDOWN dispatch. */
    if (key_state[nVirtKey] & 0x80) result |= (short)0x8000;
    if (key_state[nVirtKey] & 0x01) result |= 0x0001;
    return result;
}

BOOL WINAPI GetKeyboardState(BYTE *lpKeyState)
{
    if (!lpKeyState) return FALSE;
    for (int i = 0; i < 256; i++) lpKeyState[i] = key_state[i];
    return TRUE;
}

BOOL WINAPI SetKeyboardState(const BYTE *lpKeyState)
{
    if (!lpKeyState) return FALSE;
    for (int i = 0; i < 256; i++) key_state[i] = lpKeyState[i];
    return TRUE;
}

LPARAM WINAPI GetMessageExtraInfo(void)
{
    return message_extra_info;
}

LPARAM WINAPI SetMessageExtraInfo(LPARAM lParam)
{
    LPARAM previous = message_extra_info;
    message_extra_info = lParam;
    return previous;
}

typedef struct {
    DWORD type;
    union {
        struct {
            LONG dx;
            LONG dy;
            DWORD data;
            DWORD flags;
            DWORD time;
            ULONG_PTR extra_info;
        } mouse;
        struct {
            WORD vk;
            WORD scan;
            DWORD flags;
            DWORD time;
            ULONG_PTR extra_info;
        } keyboard;
        struct {
            DWORD message;
            WORD param_low;
            WORD param_high;
        } hardware;
    } value;
} SEND_INPUT_RECORD;

static WORD sendinput_read_u16(const BYTE *data)
{
    return (WORD)data[0] | ((WORD)data[1] << 8);
}

static DWORD sendinput_read_u32(const BYTE *data)
{
    return (DWORD)data[0] | ((DWORD)data[1] << 8) |
           ((DWORD)data[2] << 16) | ((DWORD)data[3] << 24);
}

static ULONG_PTR sendinput_read_uptr(const BYTE *data, BOOL compat32)
{
    ULONG_PTR value = sendinput_read_u32(data);
    if (!compat32)
        value |= (ULONG_PTR)sendinput_read_u32(data + 4) << 32;
    return value;
}

static BOOL sendinput_range_readable(const void *pointer, SIZE_T size)
{
    if (!size)
        return TRUE;
    if (!pointer)
        return FALSE;

    uint64_t first = (uint64_t)(ULONG_PTR)pointer;
    uint64_t last = first + size - 1;
    if (last < first)
        return FALSE;

    uint64_t first_upper = first >> 48;
    uint64_t last_upper = last >> 48;
    if (first_upper != ((first & (1ULL << 47)) ? 0xFFFFULL : 0) ||
        last_upper != ((last & (1ULL << 47)) ? 0xFFFFULL : 0))
        return FALSE;

#ifdef TEST_HARNESS
    return TRUE;
#else
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t page = first & ~0xFFFULL;
    uint64_t last_page = last & ~0xFFFULL;
    for (;;) {
        if (paging_translate_in_cr3(cr3, page) == UINT64_MAX)
            return FALSE;
        if (page == last_page)
            break;
        page += 0x1000;
    }
    return TRUE;
#endif
}

static void sendinput_parse_record(const BYTE *raw, BOOL compat32,
                                   SEND_INPUT_RECORD *record)
{
    int union_offset = compat32 ? 4 : 8;
    const BYTE *value = raw + union_offset;

    record->type = sendinput_read_u32(raw);
    if (record->type == INPUT_MOUSE) {
        record->value.mouse.dx = (LONG)sendinput_read_u32(value + 0);
        record->value.mouse.dy = (LONG)sendinput_read_u32(value + 4);
        record->value.mouse.data = sendinput_read_u32(value + 8);
        record->value.mouse.flags = sendinput_read_u32(value + 12);
        record->value.mouse.time = sendinput_read_u32(value + 16);
        record->value.mouse.extra_info =
            sendinput_read_uptr(value + (compat32 ? 20 : 24), compat32);
    } else if (record->type == INPUT_KEYBOARD) {
        record->value.keyboard.vk = sendinput_read_u16(value + 0);
        record->value.keyboard.scan = sendinput_read_u16(value + 2);
        record->value.keyboard.flags = sendinput_read_u32(value + 4);
        record->value.keyboard.time = sendinput_read_u32(value + 8);
        record->value.keyboard.extra_info =
            sendinput_read_uptr(value + (compat32 ? 12 : 16), compat32);
    } else if (record->type == INPUT_HARDWARE) {
        record->value.hardware.message = sendinput_read_u32(value + 0);
        record->value.hardware.param_low = sendinput_read_u16(value + 4);
        record->value.hardware.param_high = sendinput_read_u16(value + 6);
    }
}

static BYTE sendinput_scan_to_vk(WORD scan, BOOL extended)
{
    if ((scan & 0xFF00) == 0xE000) {
        extended = TRUE;
        scan &= 0xFF;
    }
    if (extended)
        return extended_scancode_to_vk((BYTE)scan);
    return scan < 0x59 ? scancode_to_vk[scan] : 0;
}

static BYTE sendinput_vk_to_scan(BYTE vk, BOOL *extended)
{
    *extended = FALSE;
    switch (vk) {
    case VK_SHIFT:   case VK_LSHIFT:   return 0x2A;
    case VK_RSHIFT:                    return 0x36;
    case VK_CONTROL: case VK_LCONTROL: return 0x1D;
    case VK_MENU:    case VK_LMENU:    return 0x38;
    case VK_RCONTROL: *extended = TRUE; return 0x1D;
    case VK_RMENU:    *extended = TRUE; return 0x38;
    case VK_DIVIDE:   *extended = TRUE; return 0x35;
    case VK_HOME:     *extended = TRUE; return 0x47;
    case VK_UP:       *extended = TRUE; return 0x48;
    case VK_PRIOR:    *extended = TRUE; return 0x49;
    case VK_LEFT:     *extended = TRUE; return 0x4B;
    case VK_RIGHT:    *extended = TRUE; return 0x4D;
    case VK_END:      *extended = TRUE; return 0x4F;
    case VK_DOWN:     *extended = TRUE; return 0x50;
    case VK_NEXT:     *extended = TRUE; return 0x51;
    case VK_INSERT:   *extended = TRUE; return 0x52;
    case VK_DELETE:   *extended = TRUE; return 0x53;
    case VK_LWIN:     *extended = TRUE; return 0x5B;
    case VK_RWIN:     *extended = TRUE; return 0x5C;
    default:
        for (int i = 0; i < 0x59; i++)
            if (scancode_to_vk[i] == vk)
                return (BYTE)i;
        return 0;
    }
}

static void sendinput_refresh_modifier(BYTE generic, BYTE left, BYTE right)
{
    BYTE down = (key_state[left] | key_state[right]) & 0x80;
    key_state[generic] = (key_state[generic] & 0x01) | down;
}

static void sendinput_update_key_state(BYTE vk, BOOL key_up)
{
    if (!vk || vk == VK_PACKET)
        return;

    BOOL was_down = (key_state[vk] & 0x80) != 0;
    if (key_up) {
        key_state[vk] &= (BYTE)~0x80;
    } else {
        key_state[vk] |= 0x80;
        async_pressed[vk] = 1;
        if (!was_down && (vk == VK_CAPITAL || vk == VK_NUMLOCK ||
                          vk == VK_SCROLL))
            key_state[vk] ^= 0x01;
    }

    if (vk == VK_LSHIFT || vk == VK_RSHIFT) {
        sendinput_refresh_modifier(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
        if (!key_up) async_pressed[VK_SHIFT] = 1;
    } else if (vk == VK_LCONTROL || vk == VK_RCONTROL) {
        sendinput_refresh_modifier(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
        if (!key_up) async_pressed[VK_CONTROL] = 1;
    } else if (vk == VK_LMENU || vk == VK_RMENU) {
        sendinput_refresh_modifier(VK_MENU, VK_LMENU, VK_RMENU);
        if (!key_up) async_pressed[VK_MENU] = 1;
    }
}

/* Shared by physical and injected input. Modifier state retains the side,
 * while window messages carry the generic VK and the physical scan code. */
static void keyboard_prepare_message(BYTE vk, WORD scan, BOOL extended,
                                      BOOL key_up, BOOL unicode, MSG *message)
{
    if (vk == VK_SHIFT) vk = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) vk = extended ? VK_RMENU : VK_LMENU;
    BYTE window_vk = vk;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) {
        window_vk = VK_SHIFT;
        extended = FALSE;
    } else if (vk == VK_LCONTROL || vk == VK_RCONTROL) {
        window_vk = VK_CONTROL;
    } else if (vk == VK_LMENU || vk == VK_RMENU) {
        window_vk = VK_MENU;
    }

    BOOL was_down = (key_state[vk] & 0x80) != 0;
    BOOL alt_before = (key_state[VK_MENU] & 0x80) != 0;
    BOOL control_before = (key_state[VK_CONTROL] & 0x80) != 0;
    BOOL system_key = FALSE;
    if (!unicode) {
        if (window_vk == VK_MENU) {
            system_key = key_up ? alt_before && keyboard_alt_pending
                                : !control_before;
            keyboard_alt_pending = !key_up && system_key;
        } else if (window_vk == VK_CONTROL) {
            system_key = key_up && alt_before;
            if (system_key) keyboard_alt_pending = FALSE;
        } else if (vk == VK_F10 || (alt_before && !control_before)) {
            system_key = TRUE;
            keyboard_alt_pending = FALSE;
        }
        if (!focus_hwnd) system_key = TRUE;
    }
    sendinput_update_key_state(vk, key_up);

    ULONG_PTR bits = 1 | ((ULONG_PTR)(unicode ? scan : scan & 0xFF) << 16);
    if (extended) bits |= 1UL << 24;
    if (!unicode && (key_state[VK_MENU] & 0x80)) bits |= 1UL << 29;
    if (was_down || key_up) bits |= 1UL << 30;
    if (key_up) bits |= 1UL << 31;
    message->message = key_up
        ? (system_key ? WM_SYSKEYUP : WM_KEYUP)
        : (system_key ? WM_SYSKEYDOWN : WM_KEYDOWN);
    message->wParam = window_vk;
    message->lParam = (LPARAM)bits;
}

static WORD sendinput_mouse_key_state(void)
{
    WORD state = 0;
    if (mouse_buttons & 1) state |= MK_LBUTTON;
    if (mouse_buttons & 2) state |= MK_RBUTTON;
    if (mouse_buttons & 4) state |= MK_MBUTTON;
    if (mouse_buttons & 8) state |= MK_XBUTTON1;
    if (mouse_buttons & 16) state |= MK_XBUTTON2;
    if (key_state[VK_SHIFT] & 0x80) state |= MK_SHIFT;
    if (key_state[VK_CONTROL] & 0x80) state |= MK_CONTROL;
    return state;
}

static void sendinput_set_mouse_button(DWORD mask, BYTE vk, BOOL down)
{
    if (down) {
        mouse_buttons |= mask;
        key_state[vk] |= 0x80;
        async_pressed[vk] = 1;
    } else {
        mouse_buttons &= ~mask;
        key_state[vk] &= (BYTE)~0x80;
    }
}

static int msg_free_slots_locked(void)
{
    if (msg_tail >= msg_head)
        return MSG_QUEUE_SIZE - 1 - (msg_tail - msg_head);
    return msg_head - msg_tail - 1;
}

static BOOL sendinput_move_will_coalesce_locked(HWND target,
                                                 DWORD target_pid,
                                                 DWORD target_tid)
{
    if (msg_tail == msg_head)
        return FALSE;
    int last = (msg_tail + MSG_QUEUE_SIZE - 1) % MSG_QUEUE_SIZE;
    return msg_queue[last].message == WM_MOUSEMOVE &&
           msg_queue[last].hwnd == target &&
           msg_target_pid[last] == target_pid &&
           msg_target_tid[last] == target_tid &&
           msg_source[last] == MSG_SOURCE_INPUT;
}

static int sendinput_record_slots_locked(const SEND_INPUT_RECORD *record,
                                         HWND target, DWORD target_pid,
                                         DWORD target_tid)
{
    if (record->type == INPUT_KEYBOARD || record->type == INPUT_HARDWARE)
        return 1;
    if (record->type != INPUT_MOUSE)
        return 0;

    DWORD flags = record->value.mouse.flags;
    int slots = 0;
    if (flags & MOUSEEVENTF_MOVE) {
        BOOL coalesce = !(flags & MOUSEEVENTF_MOVE_NOCOALESCE) &&
                        sendinput_move_will_coalesce_locked(
                            target, target_pid, target_tid);
        if (!coalesce) slots++;
    }
    if (flags & MOUSEEVENTF_LEFTDOWN) slots++;
    if (flags & MOUSEEVENTF_LEFTUP) slots++;
    if (flags & MOUSEEVENTF_RIGHTDOWN) slots++;
    if (flags & MOUSEEVENTF_RIGHTUP) slots++;
    if (flags & MOUSEEVENTF_MIDDLEDOWN) slots++;
    if (flags & MOUSEEVENTF_MIDDLEUP) slots++;
    WORD xbutton = (WORD)record->value.mouse.data;
    if ((xbutton == XBUTTON1 || xbutton == XBUTTON2) &&
        (flags & MOUSEEVENTF_XDOWN)) slots++;
    if ((xbutton == XBUTTON1 || xbutton == XBUTTON2) &&
        (flags & MOUSEEVENTF_XUP)) slots++;
    if (flags & MOUSEEVENTF_WHEEL) slots++;
    if (flags & MOUSEEVENTF_HWHEEL) slots++;
    return slots;
}

static BOOL sendinput_queue_locked(HWND target, DWORD message,
                                   WPARAM wparam, LPARAM lparam,
                                   DWORD target_pid, DWORD target_tid,
                                   LPARAM extra_info, DWORD timestamp,
                                   POINT screen_point, BOOL coalesce,
                                   DWORD *queue_status)
{
    if (!msg_enqueue_locked(target, message, wparam, lparam,
                            target_pid, target_tid, extra_info, timestamp,
                            screen_point.x, screen_point.y, coalesce,
                            MSG_SOURCE_INPUT))
        return FALSE;
    *queue_status |= message_queue_status(message);
    return TRUE;
}

UINT WINAPI SendInput(UINT cInputs, const INPUT *pInputs, int cbSize)
{
    BOOL compat32 = g_compat32_mode != 0;
    int expected_size = compat32 ? 28 : (int)sizeof(INPUT);

    if (cbSize != expected_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    if (!cInputs)
        return 0;

    SIZE_T input_bytes = (SIZE_T)cInputs * (SIZE_T)expected_size;
    if (!sendinput_range_readable(pInputs, input_bytes)) {
        SetLastError(998); /* ERROR_NOACCESS */
        return 0;
    }

    HWND target = input_target();
    DWORD target_pid = GetCurrentProcessId();
    DWORD target_tid = GetCurrentThreadId();
    WINDOW *window = target ? find_window(target) : NULL;
    if (window) {
        target_pid = window->owner_pid;
        target_tid = window->owner_tid;
    }

    UINT inserted = 0;
    DWORD changed_status = 0;
    DWORD failure_error = 0;
    BOOL queued_any = FALSE;
    BOOL injected_activity = FALSE;
    BOOL moved = FALSE;
    POINT final_client = cursor_pos;
    const BYTE *raw = (const BYTE *)(const void *)pInputs;

    uint64_t irq_flags = msg_lock_irqsave();
    for (UINT index = 0; index < cInputs; index++, raw += expected_size) {
        SEND_INPUT_RECORD record = { 0 };
        sendinput_parse_record(raw, compat32, &record);
        if (record.type > INPUT_HARDWARE) {
            failure_error = 87; /* ERROR_INVALID_PARAMETER */
            break;
        }

        int needed = sendinput_record_slots_locked(
            &record, target, target_pid, target_tid);
        if (needed > msg_free_slots_locked()) {
            failure_error = 1816; /* ERROR_NOT_ENOUGH_QUOTA */
            break;
        }

        DWORD timestamp = shim_timeGetTime();
        BOOL record_ok = TRUE;
        if (record.type == INPUT_KEYBOARD) {
            DWORD flags = record.value.keyboard.flags;
            BOOL key_up = (flags & KEYEVENTF_KEYUP) != 0;
            BOOL extended = (flags & KEYEVENTF_EXTENDEDKEY) != 0;
            BOOL unicode = (flags & KEYEVENTF_UNICODE) != 0;
            WORD scan = record.value.keyboard.scan;
            BYTE vk;

            if (unicode) {
                vk = VK_PACKET;
            } else if (flags & KEYEVENTF_SCANCODE) {
                vk = sendinput_scan_to_vk(scan, extended);
            } else {
                vk = (BYTE)record.value.keyboard.vk;
                if (!scan) {
                    BOOL mapped_extended;
                    scan = sendinput_vk_to_scan(vk, &mapped_extended);
                    extended = extended || mapped_extended;
                }
            }

            MSG key_message = { 0 };
            keyboard_prepare_message(vk, scan, extended, key_up, unicode,
                                      &key_message);
            POINT screen_point = cursor_pos;
            DWORD event_time = record.value.keyboard.time
                ? record.value.keyboard.time : timestamp;
            record_ok = sendinput_queue_locked(
                target, key_message.message, key_message.wParam,
                key_message.lParam,
                target_pid, target_tid,
                (LPARAM)record.value.keyboard.extra_info, event_time,
                screen_point, FALSE, &changed_status);
            injected_activity = TRUE;
        } else if (record.type == INPUT_MOUSE) {
            DWORD flags = record.value.mouse.flags;
            if (flags & MOUSEEVENTF_MOVE) {
                int width = current_mode_cx();
                int height = current_mode_cy();
                int64_t next_x;
                int64_t next_y;
                if (flags & MOUSEEVENTF_ABSOLUTE) {
                    DWORD normalized_x = (DWORD)record.value.mouse.dx;
                    DWORD normalized_y = (DWORD)record.value.mouse.dy;
                    if (normalized_x > 65535) normalized_x = 65535;
                    if (normalized_y > 65535) normalized_y = 65535;
                    next_x = width > 1
                        ? ((int64_t)normalized_x * (width - 1) + 32767) / 65535
                        : 0;
                    next_y = height > 1
                        ? ((int64_t)normalized_y * (height - 1) + 32767) / 65535
                        : 0;
                } else {
                    next_x = (int64_t)cursor_pos.x + record.value.mouse.dx;
                    next_y = (int64_t)cursor_pos.y + record.value.mouse.dy;
                }
                if (next_x < 0) next_x = 0;
                if (next_y < 0) next_y = 0;
                if (next_x >= width) next_x = width - 1;
                if (next_y >= height) next_y = height - 1;
                cursor_pos.x = (LONG)next_x;
                cursor_pos.y = (LONG)next_y;
                moved = TRUE;
            }

            POINT screen_point = cursor_pos;
            POINT client_point = screen_point;
            if (target)
                ScreenToClient(target, &client_point);
            final_client = client_point;
            LPARAM position = (LPARAM)(WORD)client_point.x |
                ((LPARAM)(WORD)client_point.y << 16);
            DWORD event_time = record.value.mouse.time
                ? record.value.mouse.time : timestamp;
            LPARAM extra_info = (LPARAM)record.value.mouse.extra_info;

#define SENDINPUT_MOUSE_MESSAGE(message_, wparam_, coalesce_)                 \
            do {                                                              \
                if (record_ok)                                                \
                    record_ok = sendinput_queue_locked(                       \
                        target, (message_), (wparam_), position,              \
                        target_pid, target_tid, extra_info, event_time,       \
                        screen_point, (coalesce_), &changed_status);          \
            } while (0)

            if (flags & MOUSEEVENTF_MOVE)
                SENDINPUT_MOUSE_MESSAGE(
                    WM_MOUSEMOVE, sendinput_mouse_key_state(),
                    !(flags & MOUSEEVENTF_MOVE_NOCOALESCE));
            if (flags & MOUSEEVENTF_LEFTDOWN) {
                sendinput_set_mouse_button(1, VK_LBUTTON, TRUE);
                SENDINPUT_MOUSE_MESSAGE(WM_LBUTTONDOWN,
                                        sendinput_mouse_key_state(), FALSE);
            }
            if (flags & MOUSEEVENTF_LEFTUP) {
                sendinput_set_mouse_button(1, VK_LBUTTON, FALSE);
                SENDINPUT_MOUSE_MESSAGE(WM_LBUTTONUP,
                                        sendinput_mouse_key_state(), FALSE);
            }
            if (flags & MOUSEEVENTF_RIGHTDOWN) {
                sendinput_set_mouse_button(2, VK_RBUTTON, TRUE);
                SENDINPUT_MOUSE_MESSAGE(WM_RBUTTONDOWN,
                                        sendinput_mouse_key_state(), FALSE);
            }
            if (flags & MOUSEEVENTF_RIGHTUP) {
                sendinput_set_mouse_button(2, VK_RBUTTON, FALSE);
                SENDINPUT_MOUSE_MESSAGE(WM_RBUTTONUP,
                                        sendinput_mouse_key_state(), FALSE);
            }
            if (flags & MOUSEEVENTF_MIDDLEDOWN) {
                sendinput_set_mouse_button(4, VK_MBUTTON, TRUE);
                SENDINPUT_MOUSE_MESSAGE(WM_MBUTTONDOWN,
                                        sendinput_mouse_key_state(), FALSE);
            }
            if (flags & MOUSEEVENTF_MIDDLEUP) {
                sendinput_set_mouse_button(4, VK_MBUTTON, FALSE);
                SENDINPUT_MOUSE_MESSAGE(WM_MBUTTONUP,
                                        sendinput_mouse_key_state(), FALSE);
            }

            WORD xbutton = (WORD)record.value.mouse.data;
            if ((flags & MOUSEEVENTF_XDOWN) &&
                (xbutton == XBUTTON1 || xbutton == XBUTTON2)) {
                DWORD mask = xbutton == XBUTTON1 ? 8 : 16;
                BYTE vk = xbutton == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                sendinput_set_mouse_button(mask, vk, TRUE);
                WPARAM wparam = sendinput_mouse_key_state() |
                    ((WPARAM)xbutton << 16);
                SENDINPUT_MOUSE_MESSAGE(WM_XBUTTONDOWN, wparam, FALSE);
            }
            if ((flags & MOUSEEVENTF_XUP) &&
                (xbutton == XBUTTON1 || xbutton == XBUTTON2)) {
                DWORD mask = xbutton == XBUTTON1 ? 8 : 16;
                BYTE vk = xbutton == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                sendinput_set_mouse_button(mask, vk, FALSE);
                WPARAM wparam = sendinput_mouse_key_state() |
                    ((WPARAM)xbutton << 16);
                SENDINPUT_MOUSE_MESSAGE(WM_XBUTTONUP, wparam, FALSE);
            }
            if (flags & MOUSEEVENTF_WHEEL) {
                WPARAM wparam = sendinput_mouse_key_state() |
                    ((WPARAM)(WORD)record.value.mouse.data << 16);
                SENDINPUT_MOUSE_MESSAGE(WM_MOUSEWHEEL, wparam, FALSE);
            }
            if (flags & MOUSEEVENTF_HWHEEL) {
                WPARAM wparam = sendinput_mouse_key_state() |
                    ((WPARAM)(WORD)record.value.mouse.data << 16);
                SENDINPUT_MOUSE_MESSAGE(WM_MOUSEHWHEEL, wparam, FALSE);
            }
#undef SENDINPUT_MOUSE_MESSAGE
            if (flags & (MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN |
                         MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTDOWN |
                         MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEDOWN |
                         MOUSEEVENTF_MIDDLEUP | MOUSEEVENTF_XDOWN |
                         MOUSEEVENTF_XUP | MOUSEEVENTF_WHEEL |
                         MOUSEEVENTF_HWHEEL))
                injected_activity = TRUE;
        } else {
            POINT screen_point = cursor_pos;
            LPARAM hardware_param =
                (LPARAM)record.value.hardware.param_low |
                ((LPARAM)record.value.hardware.param_high << 16);
            record_ok = sendinput_queue_locked(
                target, record.value.hardware.message, 0, hardware_param,
                target_pid, target_tid, 0, timestamp,
                screen_point, FALSE, &changed_status);
        }

        if (!record_ok) {
            failure_error = 1816; /* ERROR_NOT_ENOUGH_QUOTA */
            break;
        }
        if (needed)
            queued_any = TRUE;
        inserted++;
    }
    msg_unlock_irqrestore(irq_flags);

    if (changed_status)
        __sync_fetch_and_or(&queue_changed_status, changed_status);
    if (queued_any)
        msg_wait_event_signal(target_pid, target_tid);
    if (moved) {
        LPARAM position = (LPARAM)(WORD)final_client.x |
            ((LPARAM)(WORD)final_client.y << 16);
        update_mouse_tracking(target, position, TRUE);
    }
    if (injected_activity)
        g_last_input_time = shim_timeGetTime();
    if (failure_error)
        SetLastError(failure_error);
    return inserted;
}

DWORD WINAPI MapVirtualKeyA(DWORD uCode, DWORD uMapType)
{
    switch (uMapType) {
    case 0: /* MAPVK_VK_TO_VSC */
    case 4: { /* MAPVK_VK_TO_VSC_EX */
        BYTE vk = (BYTE)uCode;
        BOOL extended = FALSE;

        if (vk == VK_SHIFT) vk = VK_LSHIFT;
        else if (vk == VK_CONTROL) vk = VK_LCONTROL;
        else if (vk == VK_MENU) vk = VK_LMENU;

        BYTE scan = sendinput_vk_to_scan(vk, &extended);
        if (!scan) return 0;
        if (uMapType == 4 && extended)
            return 0xE000U | scan;
        return scan;
    }
    case 1: /* MAPVK_VSC_TO_VK */
    case 3: { /* MAPVK_VSC_TO_VK_EX */
        BYTE scan = (BYTE)uCode;
        BOOL extended = (uCode & 0xFF00U) == 0xE000U;
        BYTE vk = extended ? extended_scancode_to_vk(scan)
                           : (scan < 0x59 ? scancode_to_vk[scan] : 0);

        if (uMapType == 1) {
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
            else if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
            else if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
        }
        return vk;
    }
    case 2: /* MAPVK_VK_TO_CHAR */
        if (uCode >= 'A' && uCode <= 'Z') return uCode;
        if (uCode >= '0' && uCode <= '9') return uCode;
        if (uCode == VK_SPACE) return ' ';
        if (uCode == VK_OEM_MINUS) return '-';
        if (uCode == VK_OEM_PLUS) return '=';
        if (uCode == VK_OEM_1) return ';';
        if (uCode == VK_OEM_2) return '/';
        if (uCode == VK_OEM_3) return '`';
        if (uCode == VK_OEM_4) return '[';
        if (uCode == VK_OEM_5) return '\\';
        if (uCode == VK_OEM_6) return ']';
        if (uCode == VK_OEM_7) return '\'';
        if (uCode == VK_OEM_COMMA) return ',';
        if (uCode == VK_OEM_PERIOD) return '.';
        return 0;
    default:
        return 0;
    }
}

DWORD WINAPI MapVirtualKeyW(DWORD uCode, DWORD uMapType)
{
    return MapVirtualKeyA(uCode, uMapType);
}

DWORD WINAPI MapVirtualKeyExA(DWORD uCode, DWORD uMapType, HANDLE dwhkl)
{
    (void)dwhkl;
    return MapVirtualKeyA(uCode, uMapType);
}

DWORD WINAPI MapVirtualKeyExW(DWORD uCode, DWORD uMapType, HANDLE dwhkl)
{
    (void)dwhkl;
    return MapVirtualKeyW(uCode, uMapType);
}

int WINAPI GetKeyNameTextA(LONG lParam, PSTR lpString, int cchSize)
{
    if (!lpString || cchSize < 2) return 0;
    BYTE sc = (BYTE)((lParam >> 16) & 0xFF);
    BYTE vk = (sc < 0x59) ? scancode_to_vk[sc] : 0;
    if (vk >= 'A' && vk <= 'Z') {
        lpString[0] = (char)vk;
        lpString[1] = 0;
        return 1;
    }
    lpString[0] = '?';
    lpString[1] = 0;
    return 1;
}

int WINAPI ToAscii(DWORD uVirtKey, DWORD uScanCode, const BYTE *lpKeyState,
                   WORD *lpChar, DWORD uFlags)
{
    (void)uScanCode; (void)uFlags;
    if (!lpChar) return 0;
    BOOL shift = lpKeyState && (lpKeyState[VK_SHIFT] & 0x80);
    if (uVirtKey >= 'A' && uVirtKey <= 'Z') {
        *lpChar = shift ? (WORD)uVirtKey : (WORD)(uVirtKey + 32);
        return 1;
    }
    if (uVirtKey >= '0' && uVirtKey <= '9') {
        *lpChar = (WORD)uVirtKey;
        return 1;
    }
    if (uVirtKey == VK_SPACE)      { *lpChar = ' ';  return 1; }
    if (uVirtKey == VK_RETURN)     { *lpChar = '\r'; return 1; }
    if (uVirtKey == VK_BACK)       { *lpChar = '\b'; return 1; }
    if (uVirtKey == VK_TAB)        { *lpChar = '\t'; return 1; }
    if (uVirtKey == VK_ESCAPE)     { *lpChar = 0x1B; return 1; }
    if (uVirtKey == VK_OEM_MINUS)  { *lpChar = shift ? '_'  : '-';  return 1; }
    if (uVirtKey == VK_OEM_PLUS)   { *lpChar = shift ? '+'  : '=';  return 1; }
    if (uVirtKey == VK_OEM_COMMA)  { *lpChar = shift ? '<'  : ',';  return 1; }
    if (uVirtKey == VK_OEM_PERIOD) { *lpChar = shift ? '>'  : '.';  return 1; }
    if (uVirtKey == VK_OEM_1)      { *lpChar = shift ? ':'  : ';';  return 1; }
    if (uVirtKey == VK_OEM_2)      { *lpChar = shift ? '?'  : '/';  return 1; }
    if (uVirtKey == VK_OEM_3)      { *lpChar = shift ? '~'  : '`';  return 1; }
    if (uVirtKey == VK_OEM_4)      { *lpChar = shift ? '{'  : '[';  return 1; }
    if (uVirtKey == VK_OEM_5)      { *lpChar = shift ? '|'  : '\\'; return 1; }
    if (uVirtKey == VK_OEM_6)      { *lpChar = shift ? '}'  : ']';  return 1; }
    if (uVirtKey == VK_OEM_7)      { *lpChar = shift ? '"'  : '\''; return 1; }
    return 0;
}

int WINAPI ToAsciiEx(DWORD uVirtKey, DWORD uScanCode,
                     const BYTE *lpKeyState, WORD *lpChar,
                     DWORD uFlags, HANDLE dwhkl)
{
    (void)dwhkl;
    return ToAscii(uVirtKey, uScanCode, lpKeyState, lpChar, uFlags);
}

int WINAPI ToUnicode(DWORD uVirtKey, DWORD uScanCode,
                     const BYTE *lpKeyState, WCHAR *pwszBuff,
                     int cchBuff, DWORD uFlags)
{
    WORD ch;
    int result;

    if (!pwszBuff || cchBuff <= 0) return 0;
    result = ToAscii(uVirtKey, uScanCode, lpKeyState, &ch, uFlags);
    if (result <= 0) return result;

    pwszBuff[0] = (WCHAR)ch;
    return 1;
}

int WINAPI ToUnicodeEx(DWORD uVirtKey, DWORD uScanCode,
                       const BYTE *lpKeyState, WCHAR *pwszBuff,
                       int cchBuff, DWORD uFlags, HANDLE dwhkl)
{
    (void)dwhkl;
    return ToUnicode(uVirtKey, uScanCode, lpKeyState, pwszBuff,
                     cchBuff, uFlags);
}

/* ── Misc ──────────────────────────────────────────────────── */

int WINAPI MessageBoxA(HWND hWnd, PCSTR lpText, PCSTR lpCaption, DWORD uType)
{
    (void)hWnd;
    (void)uType;
    serial_puts("[MSGBOX] ");
    if (lpCaption) serial_puts(lpCaption);
    serial_puts(": ");
    if (lpText) serial_puts(lpText);
    serial_puts("\n");
    return 1; /* IDOK */
}

int WINAPI MessageBoxW(HWND hWnd, PCWSTR lpText, PCWSTR lpCaption, DWORD uType)
{
    (void)hWnd;
    (void)uType;
    serial_puts("[MSGBOX-W] ");
    /* Decode wide caption */
    if (lpCaption) {
        const WCHAR *w = lpCaption;
        char tmp[2] = {0, 0};
        while (*w) { tmp[0] = (*w <= 127) ? (char)*w : '?'; serial_puts(tmp); w++; }
    }
    serial_puts(": ");
    /* Decode wide text (limit 500 chars) */
    if (lpText) {
        const WCHAR *w = lpText;
        char tmp[2] = {0, 0};
        int n = 0;
        while (*w && n < 500) { tmp[0] = (*w <= 127) ? (char)*w : '?'; serial_puts(tmp); w++; n++; }
    }
    serial_puts("\n");
    return 1; /* IDOK */
}

static int WINAPI MessageBoxIndirectW_k32(const uint32_t *params)
{
    if (!params || params[0] < 24) return 0;
    return MessageBoxW((HWND)(ULONG_PTR)params[1],
                       (PCWSTR)(ULONG_PTR)params[3],
                       (PCWSTR)(ULONG_PTR)params[4], params[5]);
}

HCURSOR WINAPI LoadCursorA(HINSTANCE hInstance, PCSTR lpCursorName)
{
    (void)hInstance;
    (void)lpCursorName;
    return U32_SHARED_CURSOR_A;
}

HICON WINAPI LoadIconA(HINSTANCE hInstance, PCSTR lpIconName)
{
    (void)hInstance;
    (void)lpIconName;
    return U32_SHARED_ICON_A;
}

HICON WINAPI LoadIconW(HINSTANCE hInstance, PCWSTR lpIconName)
{
    (void)hInstance;
    (void)lpIconName;
    return U32_SHARED_ICON_W;
}

static BOOL icon_info_read(const ICONINFO *input, ICONINFO *output)
{
    if (!input || !output) return FALSE;
    if (g_compat32_mode) {
        const uint32_t *values = (const uint32_t *)input;
        output->fIcon = (BOOL)values[0];
        output->xHotspot = values[1];
        output->yHotspot = values[2];
        output->hbmMask = (HBITMAP)(ULONG_PTR)values[3];
        output->hbmColor = (HBITMAP)(ULONG_PTR)values[4];
    } else {
        *output = *input;
    }
    return TRUE;
}

static void icon_info_write(ICONINFO *output, const USER_ICON *icon,
                            HBITMAP mask, HBITMAP color)
{
    if (g_compat32_mode) {
        uint32_t *values = (uint32_t *)output;
        values[0] = (uint32_t)icon->is_icon;
        values[1] = icon->x_hotspot;
        values[2] = icon->y_hotspot;
        values[3] = (uint32_t)(ULONG_PTR)mask;
        values[4] = (uint32_t)(ULONG_PTR)color;
    } else {
        output->fIcon = icon->is_icon;
        output->xHotspot = icon->x_hotspot;
        output->yHotspot = icon->y_hotspot;
        output->hbmMask = mask;
        output->hbmColor = color;
    }
}

HICON WINAPI CreateIconIndirect(const ICONINFO *icon_info)
{
    ICONINFO source;
    if (!icon_info_read(icon_info, &source) || !source.hbmMask) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return NULL;
    }

    HBITMAP mask = gdi32_clone_bitmap(source.hbmMask);
    if (!mask) {
        SetLastError(6); /* ERROR_INVALID_HANDLE */
        return NULL;
    }
    HBITMAP color = NULL;
    if (source.hbmColor) {
        color = gdi32_clone_bitmap(source.hbmColor);
        if (!color) {
            DeleteObject(mask);
            SetLastError(6);
            return NULL;
        }
    }

    HANDLE handle = icon_create(source.fIcon != FALSE, source.xHotspot,
                                source.yHotspot, mask, color,
                                GetCurrentProcessId());
    if (!handle) {
        DeleteObject(mask);
        if (color) DeleteObject(color);
        return NULL;
    }
    serial_puts("[USER32] CreateIconIndirect -> 0x");
    serial_puthex((uint64_t)(ULONG_PTR)handle, 8);
    serial_puts("\n");
    return (HICON)handle;
}

HICON WINAPI CopyIcon(HICON icon)
{
    if (icon == U32_SHARED_ICON_A || icon == U32_SHARED_ICON_W)
        return (HICON)icon_create(TRUE, 0, 0, NULL, NULL,
                                  GetCurrentProcessId());
    if (icon == (HICON)U32_SHARED_CURSOR_A ||
        icon == (HICON)U32_SHARED_CURSOR_W)
        return (HICON)icon_create(FALSE, 0, 0, NULL, NULL,
                                  GetCurrentProcessId());

    USER_ICON source;
    if (!icon_snapshot(icon, &source)) {
        SetLastError(6);
        return NULL;
    }
    HBITMAP mask = source.mask ? gdi32_clone_bitmap(source.mask) : NULL;
    HBITMAP color = source.color ? gdi32_clone_bitmap(source.color) : NULL;
    if ((source.mask && !mask) || (source.color && !color)) {
        if (mask) DeleteObject(mask);
        if (color) DeleteObject(color);
        SetLastError(8);
        return NULL;
    }
    HANDLE copy = icon_create(source.is_icon, source.x_hotspot,
                              source.y_hotspot, mask, color,
                              GetCurrentProcessId());
    if (!copy) {
        if (mask) DeleteObject(mask);
        if (color) DeleteObject(color);
    }
    return (HICON)copy;
}

BOOL WINAPI GetIconInfo(HICON icon, ICONINFO *icon_info)
{
    if (!icon_info) {
        SetLastError(87);
        return FALSE;
    }

    USER_ICON source = {0};
    if (icon == U32_SHARED_ICON_A || icon == U32_SHARED_ICON_W) {
        source.is_icon = TRUE;
        source.mask = CreateBitmap(32, 32, 1, 1, NULL);
        source.color = CreateBitmap(32, 32, 1, 32, NULL);
        if (!source.mask || !source.color) {
            if (source.mask) DeleteObject(source.mask);
            if (source.color) DeleteObject(source.color);
            SetLastError(8);
            return FALSE;
        }
        icon_info_write(icon_info, &source, source.mask, source.color);
        return TRUE;
    }
    if (!icon_snapshot(icon, &source)) {
        SetLastError(6);
        return FALSE;
    }

    HBITMAP mask = source.mask ? gdi32_clone_bitmap(source.mask) : NULL;
    HBITMAP color = source.color ? gdi32_clone_bitmap(source.color) : NULL;
    if ((source.mask && !mask) || (source.color && !color)) {
        if (mask) DeleteObject(mask);
        if (color) DeleteObject(color);
        SetLastError(8);
        return FALSE;
    }
    icon_info_write(icon_info, &source, mask, color);
    return TRUE;
}

BOOL WINAPI DrawIconEx(HDC hdc, int x, int y, HICON icon, int width,
                       int height, UINT step, HBRUSH brush, UINT flags)
{
    (void)step;
    (void)brush;
    (void)flags;
    if (icon == U32_SHARED_ICON_A || icon == U32_SHARED_ICON_W ||
        icon == (HICON)U32_SHARED_CURSOR_A ||
        icon == (HICON)U32_SHARED_CURSOR_W)
        return TRUE;

    USER_ICON source;
    if (!icon_snapshot(icon, &source)) {
        SetLastError(6);
        return FALSE;
    }
    return gdi32_draw_icon_bitmap(hdc, source.color, source.mask,
                                  x, y, width, height);
}

BOOL WINAPI DestroyIcon(HICON icon)
{
    if (icon == U32_SHARED_ICON_A || icon == U32_SHARED_ICON_W)
        return TRUE;
    return icon_destroy(icon, TRUE);
}

BOOL user32_release_icon(HICON icon)
{
    if (icon == U32_SHARED_ICON_A || icon == U32_SHARED_ICON_W)
        return TRUE;
    return icon_destroy(icon, FALSE);
}

/* Forward to gdi32 real DC allocator */
extern HDC  gdi32_alloc_screen_dc(void);
extern HDC  gdi32_alloc_window_dc(HANDLE window);
extern HANDLE gdi32_window_from_dc(HDC hdc);
extern void gdi32_free_screen_dc(HDC hdc);

HDC WINAPI GetDC(HWND hWnd)
{
    /* A screen DC still belongs to the desktop window on NT.  ANGLE validates
     * its default EGL native display with WindowFromDC(GetDC(NULL)); retaining
     * a NULL owner here made every Win32 ANGLE backend reject the display. */
    HWND owner = hWnd ? hWnd : GetDesktopWindow();
    HDC hdc = gdi32_alloc_window_dc(owner);
    static UINT screen_dc_trace_count;
    if (!hWnd && screen_dc_trace_count++ < 8) {
        serial_puts("[USER32-DC] GetDC(NULL) hdc=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hdc, 8);
        serial_puts(" owner=0x");
        serial_puthex((uint64_t)(ULONG_PTR)owner, 8);
        serial_puts("\n");
    }
    return hdc;
}

HDC WINAPI GetWindowDC(HWND hWnd)
{
    if (!find_window(hWnd)) {
        SetLastError(1400);
        return NULL;
    }
    return GetDC(hWnd);
}

HWND WINAPI WindowFromDC(HDC hDC)
{
    HWND window = (HWND)gdi32_window_from_dc(hDC);
    static UINT trace_count;
    if (trace_count++ < 8) {
        serial_puts("[USER32-DC] WindowFromDC hdc=0x");
        serial_puthex((uint64_t)(ULONG_PTR)hDC, 8);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)(ULONG_PTR)window, 8);
        serial_puts("\n");
    }
    return window;
}

int WINAPI ReleaseDC(HWND hWnd, HDC hDC)
{
    (void)hWnd;
    gdi32_free_screen_dc(hDC);
    return 1;
}

BOOL WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase)
{
    if (!hWnd) {
        DWORD pid = GetCurrentProcessId();
        for (int i = 0; i < window_count; i++)
            if (windows[i].used && windows[i].owner_pid == pid)
                invalidate_window(&windows[i], lpRect, bErase);
        return TRUE;
    }

    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    invalidate_window(w, lpRect, bErase);
    return TRUE;
}

BOOL WINAPI RedrawWindow(HWND hWnd, const RECT *lprcUpdate,
                         HANDLE hrgnUpdate, UINT flags)
{
    (void)hrgnUpdate;
    if (!InvalidateRect(hWnd, lprcUpdate, (flags & 0x0004) != 0))
        return FALSE;
    return (flags & 0x0100) ? UpdateWindow(hWnd) : TRUE;
}

BOOL WINAPI SetForegroundWindow(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        SetLastError(1400);
        return FALSE;
    }

    WINDOW *root = window_root(w, NULL);
    if (root)
        place_window_in_z_order(root, HWND_TOP);
    dispatch_wm_activate(w);
    return TRUE;
}

BOOL user32_activate_compositor_window(uint32_t compositor_id)
{
    WINDOW *target = NULL;
    for (int i = 0; i < window_count; i++) {
        if (windows[i].used && windows[i].compositor_id == compositor_id) {
            target = window_root(&windows[i], NULL);
            break;
        }
    }
    if (!target || target->destroying || target->message_only ||
        target->parent || (target->style & (WS_CHILD | WS_DISABLED)) ||
        (target->ex_style & WS_EX_NOACTIVATE))
        return FALSE;

    if (target->style & WS_MINIMIZE)
        ShowWindow(target->handle, SW_RESTORE);
    else if (!target->visible)
        ShowWindow(target->handle, SW_SHOW);

    if (!window_can_activate(target))
        return FALSE;
    return SetForegroundWindow(target->handle);
}

BOOL WINAPI AllowSetForegroundWindow(DWORD dwProcessId)
{
    /* OsitoK has one interactive desktop and does not enforce Windows'
     * foreground-lock timeout. Granting either a concrete PID or ASFW_ANY is
     * therefore always valid; the caller still chooses the target HWND with
     * SetForegroundWindow. */
    (void)dwProcessId;
    return TRUE;
}

/* ── Dialog stubs ──────────────────────────────────────────── */

/* Dialog templates are packed resources whose variable-length fields are only
 * WORD-aligned. Keep reads byte-based so malformed resources cannot escape
 * their resource extent. */
#define U32_RT_DIALOG               ((PCWSTR)(ULONG_PTR)5)
#define U32_DIALOG_INDIRECT_LIMIT   (1024U * 1024U)
#define U32_DS_SETFONT              0x00000040U
#define U32_DS_CENTER               0x00000800U

typedef struct {
    const BYTE *current;
    SIZE_T remaining;
} U32_DIALOG_CURSOR;

typedef struct {
    BOOL present;
    BOOL ordinal;
    WORD ordinal_value;
    const BYTE *text;
    UINT text_length;
} U32_DIALOG_FIELD;

typedef struct {
    BOOL extended;
    DWORD style;
    DWORD ex_style;
    WORD item_count;
    int16_t x, y, cx, cy;
    U32_DIALOG_FIELD menu;
    U32_DIALOG_FIELD class_name;
    U32_DIALOG_FIELD title;
} U32_DIALOG_TEMPLATE;

typedef struct {
    DWORD style;
    DWORD ex_style;
    DWORD id;
    int16_t x, y, cx, cy;
    U32_DIALOG_FIELD class_name;
    U32_DIALOG_FIELD title;
    PCVOID creation_data;
    WORD creation_size;
} U32_DIALOG_ITEM;

static BOOL dialog_cursor_take(U32_DIALOG_CURSOR *cursor, SIZE_T size,
                               void *value)
{
    if (!cursor || size > cursor->remaining)
        return FALSE;
    if (value && size) memcpy(value, cursor->current, size);
    cursor->current += size;
    cursor->remaining -= size;
    return TRUE;
}

static BOOL dialog_cursor_word(U32_DIALOG_CURSOR *cursor, WORD *value)
{
    return dialog_cursor_take(cursor, sizeof(*value), value);
}

static BOOL dialog_cursor_dword(U32_DIALOG_CURSOR *cursor, DWORD *value)
{
    return dialog_cursor_take(cursor, sizeof(*value), value);
}

static BOOL dialog_cursor_align_dword(U32_DIALOG_CURSOR *cursor)
{
    SIZE_T padding = (SIZE_T)(-(ULONG_PTR)cursor->current) & 3U;
    return dialog_cursor_take(cursor, padding, NULL);
}

static BOOL dialog_cursor_field(U32_DIALOG_CURSOR *cursor,
                                U32_DIALOG_FIELD *field)
{
    WORD first;
    if (!field || !dialog_cursor_word(cursor, &first))
        return FALSE;
    memset(field, 0, sizeof(*field));
    if (!first)
        return TRUE;

    field->present = TRUE;
    if (first == 0xFFFFU) {
        field->ordinal = TRUE;
        return dialog_cursor_word(cursor, &field->ordinal_value);
    }

    field->text = cursor->current - sizeof(WORD);
    field->text_length = 1;
    for (;;) {
        WORD character;
        if (!dialog_cursor_word(cursor, &character))
            return FALSE;
        if (!character)
            return TRUE;
        if (field->text_length == 0xFFFFU)
            return FALSE;
        field->text_length++;
    }
}

static BOOL dialog_parse_template(U32_DIALOG_CURSOR *cursor,
                                  U32_DIALOG_TEMPLATE *dialog)
{
    if (!cursor || !dialog) return FALSE;
    memset(dialog, 0, sizeof(*dialog));

    U32_DIALOG_CURSOR original = *cursor;
    WORD version, signature;
    if (!dialog_cursor_word(cursor, &version) ||
        !dialog_cursor_word(cursor, &signature))
        return FALSE;

    if (signature == 0xFFFFU) {
        DWORD help_id;
        WORD count, x, y, cx, cy;
        if (version != 1 ||
            !dialog_cursor_dword(cursor, &help_id) ||
            !dialog_cursor_dword(cursor, &dialog->ex_style) ||
            !dialog_cursor_dword(cursor, &dialog->style) ||
            !dialog_cursor_word(cursor, &count) ||
            !dialog_cursor_word(cursor, &x) ||
            !dialog_cursor_word(cursor, &y) ||
            !dialog_cursor_word(cursor, &cx) ||
            !dialog_cursor_word(cursor, &cy))
            return FALSE;
        (void)help_id;
        dialog->extended = TRUE;
        dialog->item_count = count;
        dialog->x = (int16_t)x;
        dialog->y = (int16_t)y;
        dialog->cx = (int16_t)cx;
        dialog->cy = (int16_t)cy;
    } else {
        WORD count, x, y, cx, cy;
        *cursor = original;
        if (!dialog_cursor_dword(cursor, &dialog->style) ||
            !dialog_cursor_dword(cursor, &dialog->ex_style) ||
            !dialog_cursor_word(cursor, &count) ||
            !dialog_cursor_word(cursor, &x) ||
            !dialog_cursor_word(cursor, &y) ||
            !dialog_cursor_word(cursor, &cx) ||
            !dialog_cursor_word(cursor, &cy))
            return FALSE;
        dialog->item_count = count;
        dialog->x = (int16_t)x;
        dialog->y = (int16_t)y;
        dialog->cx = (int16_t)cx;
        dialog->cy = (int16_t)cy;
    }

    if (!dialog_cursor_field(cursor, &dialog->menu) ||
        !dialog_cursor_field(cursor, &dialog->class_name) ||
        !dialog_cursor_field(cursor, &dialog->title))
        return FALSE;

    if (dialog->style & U32_DS_SETFONT) {
        WORD point_size;
        U32_DIALOG_FIELD typeface;
        if (!dialog_cursor_word(cursor, &point_size))
            return FALSE;
        if (dialog->extended) {
            WORD weight;
            BYTE italic, charset;
            if (!dialog_cursor_word(cursor, &weight) ||
                !dialog_cursor_take(cursor, 1, &italic) ||
                !dialog_cursor_take(cursor, 1, &charset))
                return FALSE;
            (void)weight;
            (void)italic;
            (void)charset;
        }
        (void)point_size;
        if (!dialog_cursor_field(cursor, &typeface) || typeface.ordinal)
            return FALSE;
    }
    return TRUE;
}

static BOOL dialog_parse_item(U32_DIALOG_CURSOR *cursor, BOOL extended,
                              U32_DIALOG_ITEM *item)
{
    if (!cursor || !item || !dialog_cursor_align_dword(cursor))
        return FALSE;
    memset(item, 0, sizeof(*item));

    WORD x, y, cx, cy;
    if (extended) {
        DWORD help_id;
        if (!dialog_cursor_dword(cursor, &help_id) ||
            !dialog_cursor_dword(cursor, &item->ex_style) ||
            !dialog_cursor_dword(cursor, &item->style) ||
            !dialog_cursor_word(cursor, &x) ||
            !dialog_cursor_word(cursor, &y) ||
            !dialog_cursor_word(cursor, &cx) ||
            !dialog_cursor_word(cursor, &cy) ||
            !dialog_cursor_dword(cursor, &item->id))
            return FALSE;
        (void)help_id;
    } else {
        WORD id;
        if (!dialog_cursor_dword(cursor, &item->style) ||
            !dialog_cursor_dword(cursor, &item->ex_style) ||
            !dialog_cursor_word(cursor, &x) ||
            !dialog_cursor_word(cursor, &y) ||
            !dialog_cursor_word(cursor, &cx) ||
            !dialog_cursor_word(cursor, &cy) ||
            !dialog_cursor_word(cursor, &id))
            return FALSE;
        item->id = id;
    }
    item->x = (int16_t)x;
    item->y = (int16_t)y;
    item->cx = (int16_t)cx;
    item->cy = (int16_t)cy;

    if (!dialog_cursor_field(cursor, &item->class_name) ||
        !dialog_cursor_field(cursor, &item->title) ||
        !dialog_cursor_word(cursor, &item->creation_size))
        return FALSE;
    item->creation_data = item->creation_size ? cursor->current : NULL;
    return dialog_cursor_take(cursor, item->creation_size, NULL);
}

static PCSTR dialog_field_to_ansi(const U32_DIALOG_FIELD *field,
                                  char *buffer, SIZE_T capacity)
{
    if (!field || !field->present)
        return NULL;
    if (field->ordinal)
        return (PCSTR)(ULONG_PTR)field->ordinal_value;
    if (!buffer || !capacity)
        return NULL;

    SIZE_T copy = field->text_length;
    if (copy >= capacity) copy = capacity - 1;
    for (SIZE_T i = 0; i < copy; i++) {
        WORD character;
        memcpy(&character, field->text + i * sizeof(WORD), sizeof(character));
        buffer[i] = character <= 0xFFU ? (char)character : '?';
    }
    buffer[copy] = 0;
    return buffer;
}

static BOOL dialog_identifier_a_to_w(PCSTR identifier, WCHAR buffer[256],
                                     PCWSTR *wide)
{
    if (!identifier || !wide) {
        SetLastError(87);
        return FALSE;
    }
    if ((ULONG_PTR)identifier <= 0xFFFFU) {
        *wide = (PCWSTR)(ULONG_PTR)identifier;
        return TRUE;
    }
    SIZE_T length = 0;
    while (identifier[length] && length < 255) {
        buffer[length] = (WCHAR)(BYTE)identifier[length];
        length++;
    }
    if (identifier[length]) {
        SetLastError(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
        return FALSE;
    }
    buffer[length] = 0;
    *wide = buffer;
    return TRUE;
}

static int dialog_dlu_x(int value)
{
    return value * 2;
}

static int dialog_dlu_y(int value)
{
    return value * 2;
}

static HWND dialog_create_template(HINSTANCE instance, PCVOID template_data,
                                   SIZE_T template_size, HWND parent,
                                   DLGPROC proc, LPARAM init_param,
                                   BOOL modal, U32_DIALOG_STATE **state_out)
{
    if (state_out) *state_out = NULL;
    if (!template_data || template_size < 4) {
        SetLastError(87);
        return NULL;
    }

    U32_DIALOG_CURSOR cursor = {
        .current = (const BYTE *)template_data,
        .remaining = template_size,
    };
    U32_DIALOG_TEMPLATE dialog;
    if (!dialog_parse_template(&cursor, &dialog)) {
        SetLastError(1812); /* ERROR_RESOURCE_DATA_NOT_FOUND */
        return NULL;
    }
    if (dialog.item_count >= MAX_WINDOWS) {
        SetLastError(8);
        return NULL;
    }

    char dialog_class[128];
    char dialog_title[256];
    PCSTR class_name = dialog_field_to_ansi(&dialog.class_name,
                                             dialog_class,
                                             sizeof(dialog_class));
    PCSTR title = dialog_field_to_ansi(&dialog.title, dialog_title,
                                        sizeof(dialog_title));
    if (!class_name) class_name = "#32770";

    int width = dialog_dlu_x(dialog.cx);
    int height = dialog_dlu_y(dialog.cy);
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    int x = dialog_dlu_x(dialog.x);
    int y = dialog_dlu_y(dialog.y);
    if (dialog.style & U32_DS_CENTER) {
        x = (current_mode_cx() - width) / 2;
        y = (current_mode_cy() - height) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }

    U32_DIALOG_STATE *state = dialog_state_allocate(modal, parent, proc);
    if (!state) return NULL;

    BOOL requested_visible = (dialog.style & WS_VISIBLE) != 0;
    DWORD create_style = dialog.style & ~WS_VISIBLE;
    HWND window = CreateWindowExA(dialog.ex_style, class_name, title,
        create_style, x, y, width, height, parent, NULL, instance, NULL);
    if (!window) {
        dialog_state_clear(state);
        return NULL;
    }
    state->window = window;

    HWND first_enabled = NULL;
    HWND first_tab = NULL;
    for (WORD index = 0; index < dialog.item_count; index++) {
        U32_DIALOG_ITEM item;
        if (!dialog_parse_item(&cursor, dialog.extended, &item)) {
            SetLastError(1812);
            DestroyWindow(window);
            if (state->used) dialog_state_clear(state);
            return NULL;
        }

        char control_class[128];
        char control_title[256];
        PCSTR item_class = dialog_field_to_ansi(&item.class_name,
                                                 control_class,
                                                 sizeof(control_class));
        PCSTR item_title = dialog_field_to_ansi(&item.title, control_title,
                                                 sizeof(control_title));
        if (!item_class) {
            SetLastError(1812);
            DestroyWindow(window);
            if (state->used) dialog_state_clear(state);
            return NULL;
        }

        DWORD item_style = item.style | WS_CHILD;
        HWND control = CreateWindowExA(item.ex_style, item_class, item_title,
            item_style, dialog_dlu_x(item.x), dialog_dlu_y(item.y),
            dialog_dlu_x(item.cx), dialog_dlu_y(item.cy), window,
            (HMENU)(ULONG_PTR)item.id, instance,
            (PVOID)item.creation_data);
        if (!control) {
            DestroyWindow(window);
            if (state->used) dialog_state_clear(state);
            return NULL;
        }
        if (!(item_style & WS_DISABLED) && (item_style & WS_VISIBLE)) {
            if (!first_enabled) first_enabled = control;
            if (!first_tab && (item_style & WS_TABSTOP)) first_tab = control;
        }
    }

    HWND initial_focus = first_tab ? first_tab : first_enabled;
    LRESULT set_initial_focus = SendMessageA(
        window, WM_INITDIALOG, (WPARAM)(ULONG_PTR)initial_focus, init_param);
    if (set_initial_focus && initial_focus && find_window(initial_focus))
        SetFocus(initial_focus);

    if (find_window(window) && state->used && !state->ended &&
        (modal || requested_visible))
        ShowWindow(window, SW_SHOW);

    if (!find_window(window)) {
        if (state->used && !modal) dialog_state_clear(state);
        return NULL;
    }

    serial_puts("[USER32] dialog created hwnd=0x");
    serial_puthex((uint64_t)(ULONG_PTR)window, 8);
    serial_puts(" items=");
    serial_putdec(dialog.item_count);
    serial_puts(dialog.extended ? " format=extended\n" : " format=standard\n");
    if (state_out) *state_out = state;
    return window;
}

HWND WINAPI CreateDialogIndirectParamA(HINSTANCE hInstance,
                                        PCVOID lpTemplate,
                                        HWND hWndParent,
                                        DLGPROC lpDialogFunc,
                                        LPARAM dwInitParam)
{
    return dialog_create_template(hInstance, lpTemplate,
        U32_DIALOG_INDIRECT_LIMIT, hWndParent, lpDialogFunc, dwInitParam,
        FALSE, NULL);
}

HWND WINAPI CreateDialogIndirectParamW(HINSTANCE hInstance,
                                        PCVOID lpTemplate,
                                        HWND hWndParent,
                                        DLGPROC lpDialogFunc,
                                        LPARAM dwInitParam)
{
    return CreateDialogIndirectParamA(hInstance, lpTemplate, hWndParent,
                                      lpDialogFunc, dwInitParam);
}

HWND WINAPI CreateDialogParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                HWND hWndParent, DLGPROC lpDialogFunc,
                                LPARAM dwInitParam)
{
    PCVOID data;
    DWORD size;
    if (!kernel32_resource_data_w(hInstance, lpTemplateName, U32_RT_DIALOG,
                                  &data, &size))
        return NULL;
    return dialog_create_template(hInstance, data, size, hWndParent,
                                  lpDialogFunc, dwInitParam, FALSE, NULL);
}

HWND WINAPI CreateDialogParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                HWND hWndParent, DLGPROC lpDialogFunc,
                                LPARAM dwInitParam)
{
    WCHAR name_buffer[256];
    PCWSTR wide_name;
    if (!dialog_identifier_a_to_w(lpTemplateName, name_buffer, &wide_name))
        return NULL;
    return CreateDialogParamW(hInstance, wide_name, hWndParent,
                              lpDialogFunc, dwInitParam);
}

BOOL WINAPI EndDialog(HWND hDlg, LONG_PTR nResult)
{
    U32_DIALOG_STATE *state = dialog_state_find(hDlg);
    if (!state || !state->modal || state->ended) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }
    state->result = nResult;
    state->ended = TRUE;
    ShowWindow(hDlg, SW_HIDE);
    return TRUE;
}

static HWND dialog_next_tab_item(HWND dialog, HWND current)
{
    int current_index = -1;
    for (int i = 0; i < window_count; i++)
        if (windows[i].used && windows[i].handle == current)
            current_index = i;

    for (int step = 1; step <= window_count; step++) {
        int index = (current_index + step) % window_count;
        WINDOW *candidate = &windows[index];
        if (candidate->used && candidate->parent == dialog &&
            (candidate->style & WS_TABSTOP) &&
            (candidate->style & WS_VISIBLE) &&
            !(candidate->style & WS_DISABLED))
            return candidate->handle;
    }
    return NULL;
}

BOOL WINAPI IsDialogMessageA(HWND hDlg, LPMSG lpMsg)
{
    if (!dialog_state_find(hDlg) || !lpMsg) {
        SetLastError(!lpMsg ? 87 : 1400);
        return FALSE;
    }

    MSG message;
    msg_read_from(lpMsg, &message);
    if (message.hwnd != hDlg && !IsChild(hDlg, message.hwnd))
        return FALSE;

    if (message.message == WM_KEYDOWN) {
        if (message.wParam == VK_TAB) {
            HWND next = dialog_next_tab_item(hDlg, GetFocus());
            if (next) SetFocus(next);
            return TRUE;
        }
        if (message.wParam == VK_ESCAPE) {
            SendMessageA(hDlg, WM_COMMAND, IDCANCEL, 0);
            return TRUE;
        }
        if (message.wParam == VK_RETURN) {
            SendMessageA(hDlg, WM_COMMAND, IDOK, 0);
            return TRUE;
        }
    }

    TranslateMessage(lpMsg);
    DispatchMessageA(lpMsg);
    return TRUE;
}

BOOL WINAPI IsDialogMessageW(HWND hDlg, LPMSG lpMsg)
{
    return IsDialogMessageA(hDlg, lpMsg);
}

HWND WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem)
{
    if (!find_window(hDlg)) {
        SetLastError(1400);
        return NULL;
    }
    for (int i = 0; i < window_count; i++) {
        WINDOW *child = &windows[i];
        if (child->used && child->parent == hDlg &&
            (int)(ULONG_PTR)child->menu == nIDDlgItem)
            return child->handle;
    }
    return NULL;
}

BOOL WINAPI SetDlgItemTextA(HWND hDlg, int nIDDlgItem, PCSTR lpString)
{
    HWND item = GetDlgItem(hDlg, nIDDlgItem);
    if (!item) {
        SetLastError(1421); /* ERROR_CONTROL_ID_NOT_FOUND */
        return FALSE;
    }
    return SetWindowTextA(item, lpString);
}

static void dialog_uint_to_text(UINT value, BOOL is_signed, char text[12])
{
    UINT magnitude = value;
    int negative = is_signed && (value & 0x80000000U);
    if (negative) magnitude = 0U - value;

    char digits[10];
    int count = 0;
    do {
        digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude && count < (int)sizeof(digits));

    int out = 0;
    if (negative) text[out++] = '-';
    while (count) text[out++] = digits[--count];
    text[out] = 0;
}

BOOL WINAPI SetDlgItemInt(HWND hDlg, int nIDDlgItem, UINT value,
                          BOOL is_signed)
{
    char text[12];
    dialog_uint_to_text(value, is_signed, text);
    return SetDlgItemTextA(hDlg, nIDDlgItem, text);
}

UINT WINAPI GetDlgItemInt(HWND hDlg, int nIDDlgItem, BOOL *translated,
                          BOOL is_signed)
{
    if (translated) *translated = FALSE;

    HWND item = GetDlgItem(hDlg, nIDDlgItem);
    WINDOW *window = find_window(item);
    if (!window) return 0;

    const char *text = window->title;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;

    int negative = 0;
    if (*text == '+' || *text == '-') {
        negative = *text == '-';
        text++;
    }
    if (negative && !is_signed) return 0;

    uint64_t limit = is_signed
        ? (negative ? 0x80000000ULL : 0x7FFFFFFFULL)
        : 0xFFFFFFFFULL;
    uint64_t value = 0;
    int digits = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (limit - digit) / 10ULL) return 0;
        value = value * 10ULL + digit;
        digits++;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    if (!digits || *text) return 0;

    if (translated) *translated = TRUE;
    if (negative) return (UINT)(0U - (UINT)value);
    return (UINT)value;
}

int WINAPI GetDlgCtrlID(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        SetLastError(1400);
        return 0;
    }
    return (int)(ULONG_PTR)w->menu;
}

/* ── Window search ─────────────────────────────────────────── */

HWND WINAPI FindWindowExA(HWND hWndParent, HWND hWndChildAfter,
                           PCSTR lpszClass, PCSTR lpszWindow)
{
    PCSTR class_name = lpszClass;
    if (lpszClass && (ULONG_PTR)lpszClass <= 0xFFFF) {
        WNDCLASS_ENTRY *entry =
            lookup_class_by_atom((WORD)(ULONG_PTR)lpszClass);
        if (!entry) return NULL;
        class_name = entry->class_name;
    }

    if (hWndParent && !hwnd_is_message(hWndParent) &&
        !hwnd_is_desktop(hWndParent) && !find_window(hWndParent)) {
        SetLastError(1400);
        return NULL;
    }

    HWND candidates[MAX_WINDOWS];
    int candidate_count = 0;
    for (int i = 0; i < window_count && candidate_count < MAX_WINDOWS; i++) {
        WINDOW *w = &windows[i];
        if (!w->used) continue;
        BOOL in_group;
        if (hwnd_is_message(hWndParent))
            in_group = w->message_only && !w->parent;
        else if (!hWndParent || hwnd_is_desktop(hWndParent))
            in_group = is_top_level_window(w) ||
                       (hwnd_is_desktop(hWndParent) &&
                        w->parent == hWndParent);
        else
            in_group = w->parent == hWndParent;
        if (in_group)
            candidates[candidate_count++] = w->handle;
    }
    sort_window_handles_top_to_bottom(candidates, candidate_count);

    int after = hWndChildAfter == NULL;
    for (int i = 0; i < candidate_count; i++) {
        WINDOW *w = find_window(candidates[i]);
        if (!w) continue;
        if (!after) {
            if (w->handle == hWndChildAfter) after = 1;
            continue;
        }
        if (class_name && u32_stricmp(w->class_name, class_name) != 0)
            continue;
        if (lpszWindow && u32_stricmp(w->title, lpszWindow) != 0)
            continue;
        return w->handle;
    }
    if (hWndChildAfter && !after)
        SetLastError(1400);
    return NULL;
}

HWND WINAPI FindWindowExW(HWND hWndParent, HWND hWndChildAfter,
                           PCWSTR lpszClass, PCWSTR lpszWindow)
{
    char class_name[128] = {0};
    char window_name[256] = {0};
    PCSTR class_arg = (PCSTR)lpszClass;

    if (lpszClass && (ULONG_PTR)lpszClass > 0xFFFF) {
        for (int i = 0; i < 127 && lpszClass[i]; i++)
            class_name[i] = (char)(lpszClass[i] & 0xFF);
        class_arg = class_name;
    }
    if (lpszWindow)
        for (int i = 0; i < 255 && lpszWindow[i]; i++)
            window_name[i] = (char)(lpszWindow[i] & 0xFF);
    return FindWindowExA(hWndParent, hWndChildAfter, class_arg,
                         lpszWindow ? window_name : NULL);
}

HWND WINAPI FindWindowA(PCSTR lpszClass, PCSTR lpszWindow)
{
    return FindWindowExA(NULL, NULL, lpszClass, lpszWindow);
}

HWND WINAPI FindWindowW(PCWSTR lpszClass, PCWSTR lpszWindow)
{
    return FindWindowExW(NULL, NULL, lpszClass, lpszWindow);
}

/* ── Wide message loop delegates ───────────────────────────── */

BOOL WINAPI PeekMessageW(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                          DWORD wMsgFilterMax, DWORD wRemoveMsg)
{
    return PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
}

BOOL WINAPI GetMessageW(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                         DWORD wMsgFilterMax)
{
    return GetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
}

LRESULT WINAPI DispatchMessageW(const MSG *lpMsg)
{
    return DispatchMessageA(lpMsg);
}

LRESULT WINAPI SendMessageW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    return send_message_text(hWnd, Msg, wParam, lParam, TRUE);
}

LRESULT WINAPI SendMessageTimeoutW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam,
                                    DWORD fuFlags, DWORD uTimeout, ULONG_PTR *lpdwResult)
{
    LRESULT result = 0;
    BOOL completed = send_message_wait(hWnd, Msg, wParam, lParam, uTimeout,
                                       (fuFlags & 0x0001U) != 0, TRUE, &result);
    if (lpdwResult) {
        if (g_compat32_mode)
            *(uint32_t *)(void *)lpdwResult = (uint32_t)result;
        else
            *lpdwResult = (ULONG_PTR)result;
    }
    return completed ? 1 : 0;
}

/* ── Thread message stubs ──────────────────────────────────── */

BOOL WINAPI PostThreadMessageA(DWORD idThread, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    if (!idThread) {
        SetLastError(1444); /* ERROR_INVALID_THREAD_ID */
        return FALSE;
    }
    return msg_enqueue_target(NULL, Msg, wParam, lParam,
                              GetCurrentProcessId(), idThread);
}

BOOL WINAPI PostThreadMessageW(DWORD idThread, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    return PostThreadMessageA(idThread, Msg, wParam, lParam);
}

/* ── Window property stubs ─────────────────────────────────── */

HANDLE WINAPI GetPropA(HWND hWnd, PCSTR lpString)
{
    if (!find_window(hWnd) || !lpString)
        return NULL;
    WINDOW_PROPERTY *property = property_find_a(hWnd, lpString);
    property_trace(property ? "get-a" : "miss-a", hWnd, property);
    return property ? property->value : NULL;
}

HANDLE WINAPI GetPropW(HWND hWnd, PCWSTR lpString)
{
    if (!find_window(hWnd) || !lpString)
        return NULL;
    WINDOW_PROPERTY *property = property_find_w(hWnd, lpString);
    property_trace(property ? "get-w" : "miss-w", hWnd, property);
    return property ? property->value : NULL;
}

BOOL WINAPI SetPropA(HWND hWnd, PCSTR lpString, HANDLE hData)
{
    if (!find_window(hWnd)) {
        SetLastError(1400); /* ERROR_INVALID_WINDOW_HANDLE */
        return FALSE;
    }
    if (!lpString) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }
    WINDOW_PROPERTY *property = property_find_a(hWnd, lpString);
    BOOL allocated = FALSE;
    if (!property) {
        property = property_alloc(hWnd);
        allocated = TRUE;
    }
    if (!property) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    if (allocated && !property_store_name_a(property, lpString)) {
        *property = (WINDOW_PROPERTY){0};
        SetLastError(87);
        return FALSE;
    }
    property->value = hData;
    property_trace("set-a", hWnd, property);
    return TRUE;
}

BOOL WINAPI SetPropW(HWND hWnd, PCWSTR lpString, HANDLE hData)
{
    if (!find_window(hWnd)) {
        SetLastError(1400);
        return FALSE;
    }
    if (!lpString) {
        SetLastError(87);
        return FALSE;
    }
    WINDOW_PROPERTY *property = property_find_w(hWnd, lpString);
    BOOL allocated = FALSE;
    if (!property) {
        property = property_alloc(hWnd);
        allocated = TRUE;
    }
    if (!property) {
        SetLastError(8);
        return FALSE;
    }
    if (allocated && !property_store_name_w(property, lpString)) {
        *property = (WINDOW_PROPERTY){0};
        SetLastError(87);
        return FALSE;
    }
    property->value = hData;
    property_trace("set-w", hWnd, property);
    return TRUE;
}

HANDLE WINAPI RemovePropA(HWND hWnd, PCSTR lpString)
{
    if (!find_window(hWnd) || !lpString)
        return NULL;
    WINDOW_PROPERTY *property = property_find_a(hWnd, lpString);
    if (!property)
        return NULL;
    HANDLE value = property->value;
    property_trace("remove-a", hWnd, property);
    *property = (WINDOW_PROPERTY){0};
    return value;
}

HANDLE WINAPI RemovePropW(HWND hWnd, PCWSTR lpString)
{
    if (!find_window(hWnd) || !lpString)
        return NULL;
    WINDOW_PROPERTY *property = property_find_w(hWnd, lpString);
    if (!property)
        return NULL;
    HANDLE value = property->value;
    property_trace("remove-w", hWnd, property);
    *property = (WINDOW_PROPERTY){0};
    return value;
}

/* ── Window thread ─────────────────────────────────────────── */

DWORD WINAPI GetWindowThreadProcessId(HWND hWnd, DWORD *lpdwProcessId)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        if (lpdwProcessId) *lpdwProcessId = 0;
        return 0;
    }
    if (lpdwProcessId) *lpdwProcessId = w->owner_pid;
    return w->owner_tid;
}

static BOOL call_window_enum_proc(WNDENUMPROC callback, HWND window,
                                  LPARAM lParam)
{
    extern uint32_t compat32_callback_args(uint32_t func, int nargs,
                                           const uint32_t *args);

    if (g_compat32_mode) {
        uint32_t args[2] = {
            (uint32_t)(ULONG_PTR)window,
            (uint32_t)lParam
        };
        return (BOOL)compat32_callback_args(
            (uint32_t)(ULONG_PTR)callback, 2, args);
    }
    return callback(window, lParam);
}

static void sort_window_handles_top_to_bottom(HWND *handles, int count)
{
    for (int i = 1; i < count; i++) {
        HWND key = handles[i];
        WINDOW *key_window = find_window(key);
        int j = i - 1;
        while (j >= 0) {
            WINDOW *current = find_window(handles[j]);
            if (!current || !key_window || current->render_z >= key_window->render_z)
                break;
            handles[j + 1] = handles[j];
            j--;
        }
        handles[j + 1] = key;
    }
}

BOOL WINAPI EnumWindows(WNDENUMPROC lpfn, LPARAM lParam)
{
    HWND matches[MAX_WINDOWS];
    int match_count = 0;

    if (!lpfn) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    for (int i = 0; i < window_count && match_count < MAX_WINDOWS; i++) {
        WINDOW *w = &windows[i];
        if (is_top_level_window(w))
            matches[match_count++] = w->handle;
    }
    sort_window_handles_top_to_bottom(matches, match_count);

    for (int i = 0; i < match_count; i++) {
        WINDOW *w = find_window(matches[i]);
        if (!is_top_level_window(w))
            continue;
        if (!call_window_enum_proc(lpfn, w->handle, lParam))
            return FALSE;
    }
    return TRUE;
}

BOOL WINAPI EnumThreadWindows(DWORD dwThreadId, WNDENUMPROC lpfn,
                              LPARAM lParam)
{
    HWND matches[MAX_WINDOWS];
    int match_count = 0;

    if (!lpfn) {
        SetLastError(87);
        return FALSE;
    }

    for (int i = 0; i < window_count && match_count < MAX_WINDOWS; i++) {
        WINDOW *w = &windows[i];
        if (is_top_level_window(w) && w->owner_tid == dwThreadId)
            matches[match_count++] = w->handle;
    }
    sort_window_handles_top_to_bottom(matches, match_count);

    for (int i = 0; i < match_count; i++) {
        WINDOW *w = find_window(matches[i]);
        if (!is_top_level_window(w) || w->owner_tid != dwThreadId)
            continue;
        if (!call_window_enum_proc(lpfn, w->handle, lParam))
            return FALSE;
    }
    return TRUE;
}

/* ── OsitoK input injection ───────────────────────────────── */

/*
 * Called by OsitoK PS/2 keyboard IRQ handler.
 * scancode: PS/2 scancode set 1 (0x00-0x58, or 0xE0 prefix already stripped).
 * key_up: TRUE for break (release), FALSE for make (press).
 *
 * For extended keys (0xE0 prefix), the caller should strip the 0xE0
 * and pass the second byte with the is_extended flag behavior handled
 * by the caller setting scancode >= 0x80 conventions, or the caller
 * can directly call with the proper VK. We handle both patterns.
 */
/* Pointer input must be hit-tested independently from keyboard focus. Using
 * input_target() here made the focused parent receive every mouse event, so a
 * child such as Chrome_RenderWidgetHostHWND could never receive the click
 * that gives it focus. Capture and UT99 mouse-look remain explicit overrides. */
static HWND mouse_input_target(void)
{
    if (relative_pointer_mode_active())
        return input_target();
    if (capture_hwnd && find_window(capture_hwnd))
        return capture_hwnd;

    return WindowFromPoint(cursor_pos);
}

static LPARAM mouse_client_position(HWND target)
{
    POINT point = cursor_pos;
    if (target)
        ScreenToClient(target, &point);
    return ((LPARAM)(point.y & 0xFFFF) << 16) |
           (LPARAM)(point.x & 0xFFFF);
}

static void mouse_focus_target(HWND target, DWORD message)
{
    WINDOW *window = find_window(target);
    if (!window || (window->style & WS_DISABLED))
        return;

    HWND previous = focus_hwnd;
    if (previous != target)
        SetFocus(target);

    static unsigned trace_count;
    if (trace_count++ < 48) {
        serial_puts("[MOUSE-FOCUS] msg=0x");
        serial_puthex(message, 4);
        serial_puts(" target=0x");
        serial_puthex((uint64_t)(ULONG_PTR)target, 8);
        serial_puts(" previous=0x");
        serial_puthex((uint64_t)(ULONG_PTR)previous, 8);
        serial_puts(" class='");
        serial_puts(window->class_name);
        serial_puts("'\n");
    }
}

static BYTE prev_was_e0 = 0;

void win32_post_keyboard_event(BYTE scancode, BOOL key_up)
{
    if (!win32_input_active()) {
        prev_was_e0 = 0;
        return;
    }

    /* Handle 0xE0 prefix byte */
    if (scancode == 0xE0) {
        prev_was_e0 = 1;
        return;
    }

    BYTE vk;
    BOOL extended = prev_was_e0;
    prev_was_e0 = 0;

    if (extended) {
        vk = extended_scancode_to_vk(scancode);
    } else if (scancode < 0x59) {
        vk = scancode_to_vk[scancode];
    } else {
        return; /* unknown scancode */
    }
    g_last_input_time = shim_timeGetTime();

    if (vk == 0) return;

    MSG key_message = { 0 };
    keyboard_prepare_message(vk, scancode, extended, key_up, FALSE,
                              &key_message);

    /* Find active window for message target. MUST use the window's real handle
     * (windows[i].handle) — find_window() matches by handle, and Window.dll's
     * StaticProc maps hwnd->WWindow by the same handle. Using (i+1) here made
     * find_window() fail (handle 0xA00000xx != i+1), so DispatchMessage dropped
     * every key to DefWindowProc and the game never saw input.
     *
     * input_target() routes to the in-game VIEWPORT window when mouse-look is
     * active (so WASD/arrows reach UWindowsViewport::ViewportWndProc → the same
     * CauseInputEvent path ESC/Space already take), and to the focused UWindow
     * (menu/console) otherwise. This fixes B2: movement keys were landing on a
     * non-viewport window and never reaching the gameplay input. */
    HWND target = input_target();
    if (!target) return;

    msg_enqueue_input(target, key_message.message, key_message.wParam,
                       key_message.lParam);
}

/*
 * Called by OsitoK mouse IRQ handler (PS/2 or USB HID).
 * dx, dy: relative mouse movement (pixels).
 * buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
 * wheel_delta: mouse wheel delta (positive = up, negative = down).
 */
void win32_post_mouse_event(int dx, int dy, DWORD buttons, short wheel_delta)
{
    if (dx || dy || buttons != mouse_buttons || wheel_delta)
        g_last_input_time = shim_timeGetTime();
    /* Update cursor position */
    cursor_pos.x += dx;
    cursor_pos.y += dy;
    /* Clamp to screen */
    if (cursor_pos.x < 0) cursor_pos.x = 0;
    if (cursor_pos.y < 0) cursor_pos.y = 0;
    int screen_width = current_mode_cx();
    int screen_height = current_mode_cy();
    if (screen_width < 1) screen_width = 1;
    if (screen_height < 1) screen_height = 1;
    if (cursor_pos.x >= screen_width)  cursor_pos.x = screen_width - 1;
    if (cursor_pos.y >= screen_height) cursor_pos.y = screen_height - 1;
    confine_cursor_point(&cursor_pos);

    DWORD old_buttons = mouse_buttons;
    mouse_buttons = buttons;

    /* A warp-based consumer observes anchor plus the physical delta. */
    HWND target = mouse_input_target();

    /* ── Phase 1 diagnostic: WM_MOUSEMOVE routing during capture ── */
    {
        static int diag_n = 0;
        int cap = (capture_hwnd != NULL);
        int ml  = relative_pointer_mode_active();
        if (diag_n < 60 && (cap || ml)) {
            serial_puts("[CAP-MOUSE] dx="); serial_putdec((uint64_t)(int64_t)dx);
            serial_puts(" dy="); serial_putdec((uint64_t)(int64_t)dy);
            serial_puts(" x="); serial_putdec((uint64_t)cursor_pos.x);
            serial_puts(" y="); serial_putdec((uint64_t)cursor_pos.y);
            serial_puts(" target=0x"); serial_puthex((uint64_t)(ULONG_PTR)target, 8);
            serial_puts(" cap=0x"); serial_puthex((uint64_t)(ULONG_PTR)capture_hwnd, 8);
            serial_puts(" cv="); serial_putdec((uint64_t)(int64_t)
                cursor_display_count(GetCurrentProcessId(), GetCurrentThreadId()));
            serial_puts(" ml="); serial_putdec((uint64_t)(int64_t)ml);
            serial_puts("\n");
            diag_n++;
        }
    }

    LPARAM pos_lp = mouse_client_position(target);

    update_mouse_tracking(target, pos_lp, dx != 0 || dy != 0);

    /* Movement */
    if (target && (dx != 0 || dy != 0))
        msg_enqueue_input(target, WM_MOUSEMOVE,
                          sendinput_mouse_key_state(), pos_lp);

    /* Button state changes */
    if ((buttons & 1) && !(old_buttons & 1)) {
        mouse_focus_target(target, WM_LBUTTONDOWN);
        key_state[VK_LBUTTON] |= 0x80;
        if (target)
            msg_enqueue_input(target, WM_LBUTTONDOWN,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if (!(buttons & 1) && (old_buttons & 1)) {
        key_state[VK_LBUTTON] &= ~0x80;
        if (target)
            msg_enqueue_input(target, WM_LBUTTONUP,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if ((buttons & 2) && !(old_buttons & 2)) {
        mouse_focus_target(target, WM_RBUTTONDOWN);
        key_state[VK_RBUTTON] |= 0x80;
        if (target)
            msg_enqueue_input(target, WM_RBUTTONDOWN,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if (!(buttons & 2) && (old_buttons & 2)) {
        key_state[VK_RBUTTON] &= ~0x80;
        if (target)
            msg_enqueue_input(target, WM_RBUTTONUP,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if ((buttons & 4) && !(old_buttons & 4)) {
        mouse_focus_target(target, WM_MBUTTONDOWN);
        key_state[VK_MBUTTON] |= 0x80;
        if (target)
            msg_enqueue_input(target, WM_MBUTTONDOWN,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if (!(buttons & 4) && (old_buttons & 4)) {
        key_state[VK_MBUTTON] &= ~0x80;
        if (target)
            msg_enqueue_input(target, WM_MBUTTONUP,
                              sendinput_mouse_key_state(), pos_lp);
    }

    /* Wheel */
    if (target && wheel_delta != 0) {
        WPARAM wp = sendinput_mouse_key_state() |
                    ((WPARAM)(WORD)wheel_delta << 16);
        msg_enqueue_input(target, WM_MOUSEWHEEL, wp, pos_lp);
    }
}

static int map_fullscreen_axis(int physical, int offset,
                               uint32_t content_extent,
                               int logical_extent)
{
    if (logical_extent <= 1 || content_extent <= 1)
        return 0;

    int relative = physical - offset;
    if (relative <= 0)
        return 0;
    if ((uint32_t)relative >= content_extent)
        return logical_extent - 1;

    int value = (int)(((int64_t)relative * logical_extent) /
                      content_extent);
    return value < logical_extent ? value : logical_extent - 1;
}

static BOOL map_fullscreen_pointer(int physical_x, int physical_y,
                                   int *logical_x, int *logical_y)
{
    if (!logical_x || !logical_y ||
        !compositor_get_fullscreen_content_rect ||
        !user32_foreground_active)
        return FALSE;

    WINDOW *root = window_root(find_window(active_hwnd), NULL);
    if (!root || !root->compositor_id || root->width <= 0 ||
        root->height <= 0)
        return FALSE;

    int32_t left = 0;
    int32_t top = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!compositor_get_fullscreen_content_rect(root->compositor_id,
                                                &left, &top,
                                                &width, &height))
        return FALSE;

    *logical_x = map_fullscreen_axis(physical_x, left, width, root->width);
    *logical_y = map_fullscreen_axis(physical_y, top, height, root->height);
    return TRUE;
}

/* Desktop/UI bridge for a relative HID. input_events.c has already applied
 * acceleration and clamped screen_x/screen_y for the compositor. Seed the
 * relative emitter so it lands on that exact point while retaining raw HID
 * deltas for a detected cursor-warp input mode. */
void win32_post_mouse_screen(int screen_x, int screen_y,
                             int raw_dx, int raw_dy,
                             DWORD buttons, short wheel_delta)
{
    if (relative_pointer_mode_active()) {
        win32_post_mouse_event(raw_dx, raw_dy, buttons, wheel_delta);
        return;
    }

    int current_x = screen_x;
    int current_y = screen_y;
    int previous_x = screen_x - raw_dx;
    int previous_y = screen_y - raw_dy;
    if (map_fullscreen_pointer(screen_x, screen_y,
                               &current_x, &current_y)) {
        (void)map_fullscreen_pointer(screen_x - raw_dx,
                                     screen_y - raw_dy,
                                     &previous_x, &previous_y);
    }

    cursor_pos.x = previous_x;
    cursor_pos.y = previous_y;
    g_abs_prev_valid = 0;
    win32_post_mouse_event(current_x - previous_x,
                           current_y - previous_y,
                           buttons, wheel_delta);
}

/* Absolute-pointer path (QEMU usb-tablet / any HID_INPUT_ABS mouse). ax/ay are
 * raw logical coordinates in [lmin,lmax]. */
void win32_post_mouse_abs(int ax, int ay, int lmin, int lmax, DWORD buttons)
{
    g_last_input_time = shim_timeGetTime();
    int physical_width = screen_cx();
    int physical_height = screen_cy();
    if (physical_width < 1)
        physical_width = USER32_FALLBACK_SCREEN_WIDTH;
    if (physical_height < 1)
        physical_height = USER32_FALLBACK_SCREEN_HEIGHT;

    int tw = current_mode_cx(), th = current_mode_cy();
    if (tw < 1) tw = USER32_FALLBACK_SCREEN_WIDTH;
    if (th < 1) th = USER32_FALLBACK_SCREEN_HEIGHT;

    int range = lmax - lmin;
    if (range <= 0) range = 1;
    int physical_x = (int)(((int64_t)(ax - lmin) *
                            (physical_width - 1)) / range);
    int physical_y = (int)(((int64_t)(ay - lmin) *
                            (physical_height - 1)) / range);
    if (physical_x < 0) physical_x = 0;
    else if (physical_x >= physical_width) physical_x = physical_width - 1;
    if (physical_y < 0) physical_y = 0;
    else if (physical_y >= physical_height) physical_y = physical_height - 1;

    /* Invert the compositor's actual fullscreen projection. This preserves
     * correct hit testing through letterbox/pillarbox borders. The fallback
     * retains logical display-mode coordinates when no compositor is linked. */
    int sx = physical_x;
    int sy = physical_y;
    if (!map_fullscreen_pointer(physical_x, physical_y, &sx, &sy) &&
        (tw != physical_width || th != physical_height)) {
        sx = physical_width > 1
            ? (int)(((int64_t)physical_x * (tw - 1)) /
                    (physical_width - 1)) : 0;
        sy = physical_height > 1
            ? (int)(((int64_t)physical_y * (th - 1)) /
                    (physical_height - 1)) : 0;
    }
    if (sx < 0) sx = 0; else if (sx >= tw) sx = tw - 1;
    if (sy < 0) sy = 0; else if (sy >= th) sy = th - 1;

    int nx, ny, moved;
    int rel_dx = 0, rel_dy = 0;

    if (relative_pointer_mode_active()) {
        /* A cursor-warp consumer expects movement around its last anchor. An
         * absolute HID therefore needs conversion to sample deltas. */
        int dx = 0, dy = 0;
        if (g_abs_prev_valid) { dx = sx - g_abs_prev_sx; dy = sy - g_abs_prev_sy; }
        g_abs_prev_sx = sx; g_abs_prev_sy = sy; g_abs_prev_valid = 1;
        rel_dx = dx;
        rel_dy = dy;

        nx = cursor_pos.x + dx;
        ny = cursor_pos.y + dy;
        /* Keep within the active logical display; ClipCursor is applied below. */
        if (nx < 0) nx = 0; else if (nx >= tw) nx = tw - 1;
        if (ny < 0) ny = 0; else if (ny >= th) ny = th - 1;
        moved = (dx != 0 || dy != 0);
    } else {
        /* Ordinary UI consumes the absolute device coordinates directly. */
        g_abs_prev_valid = 0;
        nx = sx; ny = sy;
        moved = (nx != cursor_pos.x) || (ny != cursor_pos.y);
    }

    cursor_pos.x = nx;
    cursor_pos.y = ny;
    confine_cursor_point(&cursor_pos);
    nx = cursor_pos.x;
    ny = cursor_pos.y;
    HWND target = mouse_input_target();

    /* ── Phase 1 diagnostic: abs-path WM_MOUSEMOVE during capture ── */
    {
        static int diag_n = 0;
        int cap = (capture_hwnd != NULL);
        int ml  = relative_pointer_mode_active();
        if (u32_input_diagnostics_active() && diag_n < 160 && (cap || ml)) {
            serial_puts("[CAP-MOUSE-ABS] x="); serial_putdec((uint64_t)nx);
            serial_puts(" y="); serial_putdec((uint64_t)ny);
            serial_puts(" sx="); serial_putdec((uint64_t)sx);
            serial_puts(" sy="); serial_putdec((uint64_t)sy);
            serial_puts(" dx="); serial_putdec((uint64_t)(int64_t)rel_dx);
            serial_puts(" dy="); serial_putdec((uint64_t)(int64_t)rel_dy);
            serial_puts(" target=0x"); serial_puthex((uint64_t)(ULONG_PTR)target, 8);
            serial_puts(" cap=0x"); serial_puthex((uint64_t)(ULONG_PTR)capture_hwnd, 8);
            serial_puts(" moved="); serial_putdec((uint64_t)(int64_t)moved);
            serial_puts(" cv="); serial_putdec((uint64_t)(int64_t)
                cursor_display_count(GetCurrentProcessId(), GetCurrentThreadId()));
            serial_puts("\n");
            diag_n++;
        }
    }

    DWORD old_buttons = mouse_buttons;
    mouse_buttons = buttons;
    LPARAM pos_lp = mouse_client_position(target);

    update_mouse_tracking(target, pos_lp, moved);

    if (target && moved)
        msg_enqueue_input(target, WM_MOUSEMOVE,
                          sendinput_mouse_key_state(), pos_lp);
    if ((buttons & 1) && !(old_buttons & 1)) {
        mouse_focus_target(target, WM_LBUTTONDOWN);
        key_state[VK_LBUTTON] |= 0x80;
        if (target)
            msg_enqueue_input(target, WM_LBUTTONDOWN,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if (!(buttons & 1) && (old_buttons & 1)) {
        key_state[VK_LBUTTON] &= ~0x80;
        if (target)
            msg_enqueue_input(target, WM_LBUTTONUP,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if ((buttons & 2) && !(old_buttons & 2)) {
        mouse_focus_target(target, WM_RBUTTONDOWN);
        key_state[VK_RBUTTON] |= 0x80;
        if (target)
            msg_enqueue_input(target, WM_RBUTTONDOWN,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if (!(buttons & 2) && (old_buttons & 2)) {
        key_state[VK_RBUTTON] &= ~0x80;
        if (target)
            msg_enqueue_input(target, WM_RBUTTONUP,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if ((buttons & 4) && !(old_buttons & 4)) {
        mouse_focus_target(target, WM_MBUTTONDOWN);
        key_state[VK_MBUTTON] |= 0x80;
        if (target)
            msg_enqueue_input(target, WM_MBUTTONDOWN,
                              sendinput_mouse_key_state(), pos_lp);
    }
    if (!(buttons & 4) && (old_buttons & 4)) {
        key_state[VK_MBUTTON] &= ~0x80;
        if (target)
            msg_enqueue_input(target, WM_MBUTTONUP,
                              sendinput_mouse_key_state(), pos_lp);
    }
}

/* ── Clipboard stubs (UT99 Core.dll) ─────────────────────── */

#define U32_MAX_CLIPBOARD_FORMATS 64
#define U32_CLIPBOARD_NAME_CAP 128
#define U32_REGISTERED_FORMAT_BASE 0xC000U

typedef struct {
    BOOL used;
    WCHAR name[U32_CLIPBOARD_NAME_CAP];
    HANDLE data;
} U32_CLIPBOARD_FORMAT;

static U32_CLIPBOARD_FORMAT clipboard_formats[U32_MAX_CLIPBOARD_FORMATS];
static spinlock_t clipboard_lock = SPINLOCK_INIT;
static DWORD clipboard_sequence;

static inline uint64_t clipboard_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&clipboard_lock);
    return flags;
}

static inline void clipboard_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&clipboard_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static WCHAR clipboard_fold_char(WCHAR ch)
{
    return ch >= 'A' && ch <= 'Z' ? (WCHAR)(ch + ('a' - 'A')) : ch;
}

static BOOL clipboard_name_equal(PCWSTR a, PCWSTR b)
{
    while (*a && *b) {
        if (clipboard_fold_char(*a++) != clipboard_fold_char(*b++))
            return FALSE;
    }
    return *a == *b;
}

static U32_CLIPBOARD_FORMAT *clipboard_entry(UINT format)
{
    if (format < U32_REGISTERED_FORMAT_BASE)
        return NULL;
    UINT index = format - U32_REGISTERED_FORMAT_BASE;
    if (index >= U32_MAX_CLIPBOARD_FORMATS ||
        !clipboard_formats[index].used)
        return NULL;
    return &clipboard_formats[index];
}

UINT WINAPI RegisterClipboardFormatW(PCWSTR name)
{
    if (!name || !*name) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    uint64_t flags = clipboard_lock_irqsave();
    int free_slot = -1;
    for (int i = 0; i < U32_MAX_CLIPBOARD_FORMATS; i++) {
        U32_CLIPBOARD_FORMAT *entry = &clipboard_formats[i];
        if (!entry->used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (clipboard_name_equal(entry->name, name)) {
            clipboard_unlock_irqrestore(flags);
            return U32_REGISTERED_FORMAT_BASE + (UINT)i;
        }
    }

    if (free_slot < 0) {
        clipboard_unlock_irqrestore(flags);
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }

    U32_CLIPBOARD_FORMAT *entry = &clipboard_formats[free_slot];
    int i = 0;
    while (name[i] && i < U32_CLIPBOARD_NAME_CAP - 1) {
        entry->name[i] = name[i];
        i++;
    }
    entry->name[i] = 0;
    entry->data = NULL;
    entry->used = TRUE;
    clipboard_unlock_irqrestore(flags);
    return U32_REGISTERED_FORMAT_BASE + (UINT)free_slot;
}

UINT WINAPI RegisterClipboardFormatA(PCSTR name)
{
    if (!name || !*name) {
        SetLastError(87);
        return 0;
    }
    WCHAR wide[U32_CLIPBOARD_NAME_CAP];
    int i = 0;
    while (name[i] && i < U32_CLIPBOARD_NAME_CAP - 1) {
        wide[i] = (WCHAR)(BYTE)name[i];
        i++;
    }
    wide[i] = 0;
    return RegisterClipboardFormatW(wide);
}

BOOL WINAPI OpenClipboard(HANDLE hWndNewOwner)
{
    (void)hWndNewOwner;
    return TRUE;
}

BOOL WINAPI CloseClipboard(void)
{
    return TRUE;
}

BOOL WINAPI EmptyClipboard(void)
{
    uint64_t flags = clipboard_lock_irqsave();
    for (int i = 0; i < U32_MAX_CLIPBOARD_FORMATS; i++)
        clipboard_formats[i].data = NULL;
    clipboard_sequence++;
    clipboard_unlock_irqrestore(flags);
    return TRUE;
}

HANDLE WINAPI SetClipboardData(UINT format, HANDLE memory)
{
    uint64_t flags = clipboard_lock_irqsave();
    U32_CLIPBOARD_FORMAT *entry = clipboard_entry(format);
    if (!entry) {
        clipboard_unlock_irqrestore(flags);
        SetLastError(87);
        return NULL;
    }
    entry->data = memory;
    clipboard_sequence++;
    clipboard_unlock_irqrestore(flags);
    return memory;
}

HANDLE WINAPI GetClipboardData(UINT format)
{
    uint64_t flags = clipboard_lock_irqsave();
    U32_CLIPBOARD_FORMAT *entry = clipboard_entry(format);
    HANDLE data = entry ? entry->data : NULL;
    clipboard_unlock_irqrestore(flags);
    return data;
}

int WINAPI GetClipboardFormatNameW(UINT format, PWSTR buffer, int max_count)
{
    if (!buffer || max_count <= 0) {
        SetLastError(87);
        return 0;
    }
    uint64_t flags = clipboard_lock_irqsave();
    U32_CLIPBOARD_FORMAT *entry = clipboard_entry(format);
    int count = 0;
    if (entry) {
        while (entry->name[count] && count < max_count - 1) {
            buffer[count] = entry->name[count];
            count++;
        }
    }
    buffer[count] = 0;
    clipboard_unlock_irqrestore(flags);
    return count;
}

int WINAPI GetClipboardFormatNameA(UINT format, PSTR buffer, int max_count)
{
    if (!buffer || max_count <= 0) {
        SetLastError(87);
        return 0;
    }
    WCHAR wide[U32_CLIPBOARD_NAME_CAP];
    int count = GetClipboardFormatNameW(
        format, wide, max_count < U32_CLIPBOARD_NAME_CAP
                    ? max_count : U32_CLIPBOARD_NAME_CAP);
    for (int i = 0; i < count; i++)
        buffer[i] = wide[i] <= 0xFF ? (char)wide[i] : '?';
    buffer[count] = 0;
    return count;
}

UINT WINAPI EnumClipboardFormats(UINT format)
{
    uint64_t flags = clipboard_lock_irqsave();
    UINT start = format < U32_REGISTERED_FORMAT_BASE
               ? 0 : format - U32_REGISTERED_FORMAT_BASE + 1;
    UINT result = 0;
    for (UINT i = start; i < U32_MAX_CLIPBOARD_FORMATS; i++) {
        if (clipboard_formats[i].used && clipboard_formats[i].data) {
            result = U32_REGISTERED_FORMAT_BASE + i;
            break;
        }
    }
    clipboard_unlock_irqrestore(flags);
    return result;
}

DWORD WINAPI GetClipboardSequenceNumber(void)
{
    return __atomic_load_n(&clipboard_sequence, __ATOMIC_ACQUIRE);
}

BOOL WINAPI IsClipboardFormatAvailable(UINT format)
{
    uint64_t flags = clipboard_lock_irqsave();
    U32_CLIPBOARD_FORMAT *entry = clipboard_entry(format);
    BOOL available = entry && entry->data;
    clipboard_unlock_irqrestore(flags);
    return available;
}

/* ── Paint / Drawing (UT99 Window.dll / Chromium) ────────── */

typedef struct {
    uint32_t hdc;
    uint32_t fErase;
    RECT rcPaint;
    uint32_t fRestore;
    uint32_t fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT32_K32;

typedef struct {
    HDC hdc;
    uint32_t fErase;
    RECT rcPaint;
    uint32_t fRestore;
    uint32_t fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT64_K32;

_Static_assert(sizeof(PAINTSTRUCT32_K32) == 64,
               "32-bit PAINTSTRUCT layout changed");
_Static_assert(sizeof(PAINTSTRUCT64_K32) == 72,
               "64-bit PAINTSTRUCT layout changed");

HDC WINAPI BeginPaint(HWND hWnd, PVOID lpPaint)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !lpPaint) {
        SetLastError(!w ? 1400 : 87);
        return NULL;
    }
    BOOL compat32 = g_compat32_mode;
    HDC hdc = gdi32_alloc_window_dc(hWnd);
    RECT paint = { 0, 0, 0, 0 };
    BOOL erase = FALSE;
    if (!hdc) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    if (w->paint_pending) {
        paint = w->update_rect;
        erase = w->erase_pending ? TRUE : FALSE;
    }
    uint64_t token;
    if (!gdi32_push_paint_clip(hdc, (const GDI_RECT *)&paint, &token)) {
        gdi32_free_screen_dc(hdc);
        SetLastError(8);
        return NULL;
    }

    /* Consume this update before calling application code. A callback may
     * invalidate again, recurse into painting, or destroy its own window. */
    w->paint_pending = w->erase_pending = 0;
    memset(&w->update_rect, 0, sizeof(w->update_rect));
    if (erase && SendMessageA(hWnd, WM_ERASEBKGND, (WPARAM)hdc, 0))
        erase = FALSE;

    if (compat32) {
        PAINTSTRUCT32_K32 *ps = (PAINTSTRUCT32_K32 *)lpPaint;
        memset(ps, 0, sizeof(*ps));
        ps->hdc = (uint32_t)(ULONG_PTR)hdc;
        ps->fErase = erase;
        ps->rcPaint = paint;
        memcpy(ps->rgbReserved, &token, sizeof(token));
    } else {
        PAINTSTRUCT64_K32 *ps = (PAINTSTRUCT64_K32 *)lpPaint;
        memset(ps, 0, sizeof(*ps));
        ps->hdc = hdc;
        ps->fErase = erase;
        ps->rcPaint = paint;
        memcpy(ps->rgbReserved, &token, sizeof(token));
    }
    SetLastError(0);
    return hdc;
}

BOOL WINAPI EndPaint(HWND hWnd, PVOID lpPaint)
{
    HDC hdc = NULL;
    WINDOW *w = find_window(hWnd);
    uint64_t token = 0;

    if (lpPaint) {
        if (g_compat32_mode) {
            const PAINTSTRUCT32_K32 *ps = lpPaint;
            hdc = (HDC)(ULONG_PTR)ps->hdc;
            memcpy(&token, ps->rgbReserved, sizeof(token));
        } else {
            const PAINTSTRUCT64_K32 *ps = lpPaint;
            hdc = ps->hdc;
            memcpy(&token, ps->rgbReserved, sizeof(token));
        }
    }
    if (hdc && gdi32_window_from_dc(hdc) == hWnd) {
        /* An owned DC is shared across nested BeginPaint calls. EndPaint
         * releases its paint clip, not a saved application clip/state. */
        gdi32_pop_paint_clip(hdc, token, FALSE);
        gdi32_free_screen_dc(hdc);
    }
    if (w && w->compositor_id && compositor_signal_dirty)
        compositor_signal_dirty(w->compositor_id);
    return TRUE;
}

static void default_window_paint(HWND window)
{
    union {
        PAINTSTRUCT32_K32 narrow;
        PAINTSTRUCT64_K32 wide;
    } paint;
    if (BeginPaint(window, &paint)) EndPaint(window, &paint);
}

LRESULT WINAPI CallWindowProcA(PVOID lpPrevWndFunc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam)
{
    if (!lpPrevWndFunc)
        return DefWindowProcA(hWnd, Msg, wParam, lParam);

    if (Msg >= WM_KEYDOWN && Msg <= WM_SYSKEYUP) {
        static unsigned trace_count;
        if (trace_count++ < 96) {
            serial_puts("[KEY-MSG] call-prev hwnd=0x");
            serial_puthex((uint64_t)(ULONG_PTR)hWnd, 8);
            serial_puts(" msg=0x");
            serial_puthex(Msg, 4);
            serial_puts(" proc=0x");
            serial_puthex((uint64_t)(ULONG_PTR)lpPrevWndFunc, 16);
            serial_puts("\n");
        }
    }
    /* A subclass calls an earlier procedure, not a second message delivery.
     * Native class procedures already have callable PE32 thunks. */
    USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
    if (!context) { SetLastError(8); return 0; }
    BOOL previous = context->unicode_message;
    context->unicode_message = FALSE;
    dispatch_depth++;
    LRESULT result = invoke_wndproc((WNDPROC)lpPrevWndFunc, hWnd, Msg,
                                    wParam, lParam);
    dispatch_depth--;
    context->unicode_message = previous;
    return result;
}

LRESULT WINAPI CallWindowProcW(PVOID lpPrevWndFunc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam)
{
    if (!lpPrevWndFunc) return DefWindowProcW(hWnd, Msg, wParam, lParam);
    USER_HOOK_CONTEXT *context = user_hook_context_get(TRUE);
    if (!context) { SetLastError(8); return 0; }
    BOOL previous = context->unicode_message;
    context->unicode_message = TRUE;
    dispatch_depth++;
    LRESULT result = invoke_wndproc((WNDPROC)lpPrevWndFunc, hWnd, Msg, wParam, lParam);
    dispatch_depth--;
    context->unicode_message = previous;
    return result;
}

LRESULT WINAPI DefWindowProcW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == 0x000C || Msg == 0x000D || Msg == 0x000E)
        return window_text_message(hWnd, Msg, wParam, lParam, TRUE);
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

LRESULT WINAPI DefMDIChildProcA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd; (void)Msg; (void)wParam; (void)lParam;
    return 0;
}

LRESULT WINAPI DefMDIChildProcW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd; (void)Msg; (void)wParam; (void)lParam;
    return 0;
}

HWND WINAPI CreateWindowExW(DWORD dwExStyle, PCWSTR lpClassName,
                             PCWSTR lpWindowName, DWORD dwStyle,
                             int X, int Y, int nWidth, int nHeight,
                             HWND hWndParent, HMENU hMenu,
                             HINSTANCE hInstance, PVOID lpParam)
{
    /* Preserve MAKEINTATOM/MAKEINTRESOURCE values; only real pointers are
     * UTF-16 strings that may be converted and dereferenced. */
    char classA[128] = {0}, nameA[256] = {0};
    PCSTR class_arg = NULL;
    PCSTR name_arg = NULL;
    if (lpClassName) {
        if ((ULONG_PTR)lpClassName <= 0xFFFF) {
            class_arg = (PCSTR)lpClassName;
        } else {
            for (int i = 0; i < 127 && lpClassName[i]; i++)
                classA[i] = (char)(lpClassName[i] & 0xFF);
            class_arg = classA;
        }
    }
    if (lpWindowName) {
        if ((ULONG_PTR)lpWindowName <= 0xFFFF) {
            name_arg = (PCSTR)lpWindowName;
        } else {
            for (int i = 0; i < 255 && lpWindowName[i]; i++)
                nameA[i] = (char)(lpWindowName[i] & 0xFF);
            name_arg = nameA;
        }
    }
    HWND window = CreateWindowExA(dwExStyle, class_arg, name_arg, dwStyle,
        X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    WINDOW *entry = find_window(window);
    if (entry) {
        WNDCLASS_ENTRY *cls = lookup_class_for_pid(entry->class_name, entry->owner_pid);
        if (cls && cls->system_class) entry->unicode = TRUE;
    }
    if (window && lpWindowName && (ULONG_PTR)lpWindowName > 0xFFFF &&
        !window_set_text(window, lpWindowName, TRUE)) {
        DestroyWindow(window);
        return NULL;
    }
    return window;
}

WORD WINAPI RegisterClassExW(PVOID lpwcx)
{
    if (!lpwcx) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    /* Pointer layout matches WNDCLASSEXA; only the strings are wide. */
    WNDCLASSEXA wcx;
    wndclassex_read(lpwcx, &wcx);
    const uint16_t *class_name = (const uint16_t *)wcx.lpszClassName;

    char classA[128] = {0};
    if (class_name)
        for (int i = 0; i < 127 && class_name[i]; i++)
            classA[i] = (char)(class_name[i] & 0xFF);

    if (!classA[0]) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }

    serial_puts("[USER32] RegisterClassExW: ");
    serial_puts(classA);
    serial_puts(" wndproc=0x");
    serial_puthex((uint64_t)(uintptr_t)wcx.lpfnWndProc, 16);
    serial_puts("\n");

    DWORD pid = GetCurrentProcessId();
    if (find_class_for_pid(classA, pid)) {
        SetLastError(1410); /* ERROR_CLASS_ALREADY_EXISTS */
        return 0;
    }
    WNDCLASS_ENTRY *e = alloc_wndclass();
    if (!e) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    u32_strcpy(e->class_name, classA, 128);
    e->wndproc = wcx.lpfnWndProc;
    e->style = wcx.style;
    e->cbClsExtra = wcx.cbClsExtra;
    e->cbWndExtra = wcx.cbWndExtra;
    e->hInstance = wcx.hInstance;
    e->hIcon = wcx.hIcon;
    e->hCursor = wcx.hCursor;
    e->hbrBackground = wcx.hbrBackground;
    e->menu_name = (ULONG_PTR)wcx.lpszMenuName;
    e->class_name_ptr = (ULONG_PTR)class_name;
    e->hIconSm = wcx.hIconSm;
    e->owner_pid = pid;
    e->unicode = TRUE;
    e->used = 1;
    return e->atom;
}

static WNDCLASS_ENTRY *find_class_for_query(HINSTANCE instance,
                                             PCSTR class_name, WORD atom,
                                             BOOL class_is_atom)
{
    DWORD pid = GetCurrentProcessId();
    WNDCLASS_ENTRY *global = NULL;

    for (int i = 0; i < wndclass_count; i++) {
        WNDCLASS_BLOCK *block = wndclass_blocks[i / WNDCLASS_BLOCK_SIZE];
        WNDCLASS_ENTRY *entry = block
            ? &block->entries[i % WNDCLASS_BLOCK_SIZE] : NULL;
        if (!entry)
            continue;
        if (!entry->used || entry->owner_pid != pid)
            continue;
        if (class_is_atom) {
            if (entry->atom != atom)
                continue;
        } else if (u32_stricmp(entry->class_name, class_name) != 0) {
            continue;
        }
        if (entry->hInstance == instance)
            return entry;
        if ((entry->style & 0x00004000U) && !global) /* CS_GLOBALCLASS */
            global = entry;
    }

    if (global)
        return global;
    if (instance)
        return NULL;
    return class_is_atom ? find_system_class_by_atom(atom)
                         : find_system_class(class_name);
}

static void log_class_lookup_miss(PCSTR class_name, WORD atom,
                                  BOOL class_is_atom)
{
    serial_puts("[USER32] class lookup miss: ");
    if (class_is_atom) {
        serial_puts("atom #");
        serial_puthex(atom, 4);
    } else {
        serial_puts("'");
        serial_puts(class_name ? class_name : "<null>");
        serial_puts("'");
    }
    serial_puts(" pid=");
    serial_putdec(GetCurrentProcessId());
    serial_puts(" registered=");
    serial_putdec((uint64_t)wndclass_count);
    serial_puts("\n");
}

static HCURSOR class_cursor(WNDCLASS_ENTRY *entry)
{
    if (!entry || entry->hCursor || !entry->system_class)
        return entry ? entry->hCursor : NULL;
    ULONG_PTR resource = u32_stricmp(entry->class_name, "EDIT") == 0
        ? 32513U /* IDC_IBEAM */ : 32512U /* IDC_ARROW */;
    return LoadCursorA(NULL, (PCSTR)resource);
}

static BOOL write_class_info_ex(WNDCLASS_ENTRY *entry,
                                ULONG_PTR class_name, PVOID output)
{
    UINT expected_size = g_compat32_mode ? 48U : (UINT)sizeof(WNDCLASSEXA);
    if (!entry || !output || *(const UINT *)output != expected_size) {
        SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return FALSE;
    }

    WNDPROC wndproc = class_wndproc_for_mode(entry);
    if (!wndproc) {
        SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
        return FALSE;
    }
    HCURSOR cursor = class_cursor(entry);

    if (g_compat32_mode) {
        uint32_t *p = (uint32_t *)output;
        for (int i = 0; i < 12; i++) p[i] = 0;
        p[0] = expected_size;
        p[1] = entry->style;
        p[2] = (uint32_t)(ULONG_PTR)wndproc;
        p[3] = (uint32_t)entry->cbClsExtra;
        p[4] = (uint32_t)entry->cbWndExtra;
        p[5] = (uint32_t)(ULONG_PTR)entry->hInstance;
        p[6] = (uint32_t)(ULONG_PTR)entry->hIcon;
        p[7] = (uint32_t)(ULONG_PTR)cursor;
        p[8] = (uint32_t)(ULONG_PTR)entry->hbrBackground;
        p[9] = (uint32_t)entry->menu_name;
        p[10] = (uint32_t)class_name;
        p[11] = (uint32_t)(ULONG_PTR)entry->hIconSm;
    } else {
        WNDCLASSEXA *out = (WNDCLASSEXA *)output;
        BYTE *bytes = (BYTE *)out;
        for (SIZE_T i = 0; i < sizeof(*out); i++) bytes[i] = 0;
        out->cbSize = expected_size;
        out->style = entry->style;
        out->lpfnWndProc = wndproc;
        out->cbClsExtra = entry->cbClsExtra;
        out->cbWndExtra = entry->cbWndExtra;
        out->hInstance = entry->hInstance;
        out->hIcon = entry->hIcon;
        out->hCursor = cursor;
        out->hbrBackground = entry->hbrBackground;
        out->lpszMenuName = (PCSTR)entry->menu_name;
        out->lpszClassName = (PCSTR)class_name;
        out->hIconSm = entry->hIconSm;
    }
    return TRUE;
}

typedef struct {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    PCWSTR    lpszMenuName;
    PCWSTR    lpszClassName;
} WNDCLASSW_K32;

static BOOL WINAPI GetClassInfoW_k32(HINSTANCE hInstance, PCWSTR class_name,
                                      WNDCLASSW_K32 *out)
{
    ULONG_PTR value = (ULONG_PTR)class_name;
    if (!value || !out) {
        SetLastError(87);
        return FALSE;
    }
    BOOL is_atom = value <= 0xFFFF;
    char narrow[128] = {0};
    if (!is_atom) {
        int i = 0;
        while (i < 127 && class_name[i]) {
            narrow[i] = (char)(class_name[i] & 0xFF);
            i++;
        }
    }
    WNDCLASS_ENTRY *entry = find_class_for_query(
        hInstance, narrow, (WORD)value, is_atom);
    if (!entry) {
        log_class_lookup_miss(narrow, (WORD)value, is_atom);
        SetLastError(1411); /* ERROR_CLASS_DOES_NOT_EXIST */
        return FALSE;
    }

    WNDPROC wndproc = class_wndproc_for_mode(entry);
    if (!wndproc) {
        SetLastError(8);
        return FALSE;
    }
    HCURSOR cursor = class_cursor(entry);
    if (g_compat32_mode) {
        uint32_t *p = (uint32_t *)out;
        for (int i = 0; i < 10; i++) p[i] = 0;
        p[0] = entry->style;
        p[1] = (uint32_t)(ULONG_PTR)wndproc;
        p[2] = (uint32_t)entry->cbClsExtra;
        p[3] = (uint32_t)entry->cbWndExtra;
        p[4] = (uint32_t)(ULONG_PTR)entry->hInstance;
        p[5] = (uint32_t)(ULONG_PTR)entry->hIcon;
        p[6] = (uint32_t)(ULONG_PTR)cursor;
        p[7] = (uint32_t)(ULONG_PTR)entry->hbrBackground;
        p[8] = (uint32_t)entry->menu_name;
        p[9] = (uint32_t)value;
    } else {
        BYTE *bytes = (BYTE *)out;
        for (SIZE_T i = 0; i < sizeof(*out); i++) bytes[i] = 0;
        out->style = entry->style;
        out->lpfnWndProc = wndproc;
        out->cbClsExtra = entry->cbClsExtra;
        out->cbWndExtra = entry->cbWndExtra;
        out->hInstance = entry->hInstance;
        out->hIcon = entry->hIcon;
        out->hCursor = cursor;
        out->hbrBackground = entry->hbrBackground;
        out->lpszMenuName = (PCWSTR)entry->menu_name;
        out->lpszClassName = class_name;
    }
    return TRUE;
}

static BOOL WINAPI GetClassInfoA_k32(HINSTANCE hInstance, PCSTR class_name,
                                      WNDCLASSA *out)
{
    ULONG_PTR value = (ULONG_PTR)class_name;
    if (!value || !out) {
        SetLastError(87);
        return FALSE;
    }
    BOOL is_atom = value <= 0xFFFF;
    WNDCLASS_ENTRY *entry = find_class_for_query(
        hInstance, is_atom ? NULL : class_name, (WORD)value, is_atom);
    if (!entry) {
        log_class_lookup_miss(class_name, (WORD)value, is_atom);
        SetLastError(1411);
        return FALSE;
    }

    WNDPROC wndproc = class_wndproc_for_mode(entry);
    if (!wndproc) {
        SetLastError(8);
        return FALSE;
    }
    HCURSOR cursor = class_cursor(entry);
    if (g_compat32_mode) {
        uint32_t *p = (uint32_t *)out;
        for (int i = 0; i < 10; i++) p[i] = 0;
        p[0] = entry->style;
        p[1] = (uint32_t)(ULONG_PTR)wndproc;
        p[2] = (uint32_t)entry->cbClsExtra;
        p[3] = (uint32_t)entry->cbWndExtra;
        p[4] = (uint32_t)(ULONG_PTR)entry->hInstance;
        p[5] = (uint32_t)(ULONG_PTR)entry->hIcon;
        p[6] = (uint32_t)(ULONG_PTR)cursor;
        p[7] = (uint32_t)(ULONG_PTR)entry->hbrBackground;
        p[8] = (uint32_t)entry->menu_name;
        p[9] = (uint32_t)value;
    } else {
        BYTE *bytes = (BYTE *)out;
        for (SIZE_T i = 0; i < sizeof(*out); i++) bytes[i] = 0;
        out->style = entry->style;
        out->lpfnWndProc = wndproc;
        out->cbClsExtra = entry->cbClsExtra;
        out->cbWndExtra = entry->cbWndExtra;
        out->hInstance = entry->hInstance;
        out->hIcon = entry->hIcon;
        out->hCursor = cursor;
        out->hbrBackground = entry->hbrBackground;
        out->lpszMenuName = (PCSTR)entry->menu_name;
        out->lpszClassName = class_name;
    }
    return TRUE;
}

BOOL WINAPI GetClassInfoExA(HINSTANCE hInstance, PCSTR lpszClass, PVOID lpwcx)
{
    ULONG_PTR value = (ULONG_PTR)lpszClass;
    if (!value || !lpwcx) {
        SetLastError(87);
        return FALSE;
    }
    BOOL is_atom = value <= 0xFFFF;
    WNDCLASS_ENTRY *entry = find_class_for_query(
        hInstance, is_atom ? NULL : lpszClass, (WORD)value, is_atom);
    if (!entry) {
        log_class_lookup_miss(lpszClass, (WORD)value, is_atom);
        SetLastError(1411);
        return FALSE;
    }
    return write_class_info_ex(entry, value, lpwcx);
}

BOOL WINAPI GetClassInfoExW(HINSTANCE hInstance, PCWSTR lpszClass, PVOID lpwcx)
{
    ULONG_PTR value = (ULONG_PTR)lpszClass;
    if (!value || !lpwcx) {
        SetLastError(87);
        return FALSE;
    }
    BOOL is_atom = value <= 0xFFFF;
    char class_name[128] = {0};
    if (!is_atom) {
        for (int i = 0; i < 127 && lpszClass[i]; i++)
            class_name[i] = (char)(lpszClass[i] & 0xFF);
    }
    WNDCLASS_ENTRY *entry = find_class_for_query(
        hInstance, class_name, (WORD)value, is_atom);
    if (!entry) {
        log_class_lookup_miss(class_name, (WORD)value, is_atom);
        SetLastError(1411);
        return FALSE;
    }
    return write_class_info_ex(entry, value, lpwcx);
}
LONG WINAPI GetWindowLongW(HWND hWnd, int nIndex)
{
    return GetWindowLongA(hWnd, nIndex);
}

LONG WINAPI SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong)
{
    return (LONG)SetWindowLongPtrW(hWnd, nIndex, dwNewLong);
}

LONG_PTR WINAPI GetWindowLongPtrW(HWND hWnd, int nIndex)
{
    return GetWindowLongPtrA(hWnd, nIndex);
}

LONG_PTR WINAPI SetWindowLongPtrW(HWND hWnd, int nIndex,
                                   LONG_PTR dwNewLong)
{
    LONG_PTR result = SetWindowLongPtrA(hWnd, nIndex, dwNewLong);
    WINDOW *window = find_window(hWnd);
    if (window && nIndex == GWL_WNDPROC) window->unicode = TRUE;
    return result;
}

BOOL WINAPI IsWindow(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    return w ? TRUE : FALSE;
}

BOOL WINAPI IsIconic(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    return w && (w->style & WS_MINIMIZE) ? TRUE : FALSE;
}

BOOL WINAPI IsZoomed(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    return w && (w->style & WS_MAXIMIZE) ? TRUE : FALSE;
}

BOOL WINAPI IsWindowEnabled(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    return w && !(w->style & WS_DISABLED) ? TRUE : FALSE;
}

BOOL WINAPI IsChild(HWND hWndParent, HWND hWnd)
{
    WINDOW *child = find_window(hWnd);
    if (!find_window(hWndParent) || !child || hWndParent == hWnd)
        return FALSE;

    for (int i = 0; i < MAX_WINDOWS && child->parent; i++) {
        if (child->parent == hWndParent)
            return TRUE;
        child = find_window(child->parent);
        if (!child)
            break;
    }
    return FALSE;
}

HWND WINAPI GetParent(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        SetLastError(1400);
        return NULL;
    }
    if (w->message_only && !w->parent)
        return HWND_MESSAGE;
    if (w->parent)
        return w->parent;
    if (w->style & WS_POPUP)
        return w->owner;
    return NULL;
}

BOOL WINAPI EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc,
                            LPARAM lParam)
{
    if (!lpEnumFunc) {
        SetLastError(87);
        return FALSE;
    }
    if (!hWndParent)
        return EnumWindows(lpEnumFunc, lParam);
    if (!find_window(hWndParent) && !hwnd_is_desktop(hWndParent) &&
        !hwnd_is_message(hWndParent)) {
        SetLastError(1400);
        return FALSE;
    }

    HWND matches[MAX_WINDOWS];
    int match_count = 0;
    for (int i = 0; i < window_count && match_count < MAX_WINDOWS; i++) {
        WINDOW *candidate = &windows[i];
        if (!candidate->used) continue;
        BOOL matches_parent = hwnd_is_message(hWndParent)
            ? candidate->message_only
            : (hwnd_is_desktop(hWndParent)
                ? candidate->parent == hWndParent
                : IsChild(hWndParent, candidate->handle));
        if (matches_parent)
            matches[match_count++] = candidate->handle;
    }
    sort_window_handles_top_to_bottom(matches, match_count);

    for (int i = 0; i < match_count; i++) {
        WINDOW *candidate = find_window(matches[i]);
        if (!candidate) continue;
        BOOL still_child = hwnd_is_message(hWndParent)
            ? candidate->message_only
            : (hwnd_is_desktop(hWndParent)
                ? candidate->parent == hWndParent
                : IsChild(hWndParent, candidate->handle));
        if (still_child &&
            !call_window_enum_proc(lpEnumFunc, candidate->handle, lParam))
            return FALSE;
    }
    return TRUE;
}

/* Client→screen translation. Our windows are borderless (client rect ==
 * window rect), so the client origin in screen space is simply (w->x, w->y).
 * POINT is two LONGs (int32) — identical layout for 32-bit callers.
 * WinDrv's captured-mouse recenter does ClientToScreen+SetCursorPos
 * (windrv.bin 0x11108B0A-0x11108B18) and SetMouseCapture computes its rect via
 * GetClientRect+MapWindowPoints (0x11106682-0x111066A4); the old no-op stubs
 * were only correct because the game window happens to sit at 0,0 — make the
 * coordinate spaces correct by construction. */
BOOL WINAPI ClientToScreen(HWND hWnd, PVOID lpPoint)
{
    WINDOW *w = find_window(hWnd);
    LONG *pt = (LONG *)lpPoint;
    if (!pt || (!w && !hwnd_is_desktop(hWnd))) return FALSE;
    if (w) {
        int screen_x, screen_y;
        window_screen_origin(w, &screen_x, &screen_y);
        pt[0] += screen_x;
        pt[1] += screen_y;
    }
    return TRUE;
}

BOOL WINAPI ScreenToClient(HWND hWnd, PVOID lpPoint)
{
    WINDOW *w = find_window(hWnd);
    LONG *pt = (LONG *)lpPoint;
    if (!pt || (!w && !hwnd_is_desktop(hWnd))) return FALSE;
    if (w) {
        int screen_x, screen_y;
        window_screen_origin(w, &screen_x, &screen_y);
        pt[0] -= screen_x;
        pt[1] -= screen_y;
    }
    return TRUE;
}

BOOL WINAPI GetUpdateRect(HWND hWnd, PVOID lpRect, BOOL bErase)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    if (!w->paint_pending) {
        if (lpRect) memset(lpRect, 0, sizeof(RECT));
        return FALSE;
    }
    if (lpRect) *(RECT *)lpRect = w->update_rect;
    if (bErase && w->erase_pending) {
        HDC dc = gdi32_alloc_window_dc(hWnd);
        uint64_t token;
        if (dc && gdi32_push_paint_clip(
                dc, (const GDI_RECT *)&w->update_rect, &token)) {
            w->erase_pending = 0;
            LRESULT erased = SendMessageA(hWnd, WM_ERASEBKGND, (WPARAM)dc, 0);
            w = find_window(hWnd);
            if (w && !erased && w->paint_pending) w->erase_pending = 1;
            gdi32_pop_paint_clip(dc, token, TRUE);
        }
        if (dc) gdi32_free_screen_dc(dc);
    }
    return TRUE;
}

int WINAPI FillRect(HDC hDC, PVOID lprc, HBRUSH hbr)
{
    ULONG_PTR value = (ULONG_PTR)hbr;
    if (value >= 1 && value <= 31)
        hbr = (HBRUSH)(ULONG_PTR)(0xBC000000u | (value - 1));
    return gdi32_fill_rect(hDC, (const GDI_RECT *)lprc, (HGDIOBJ)hbr);
}

BOOL WINAPI DrawFocusRect(HDC hDC, PVOID lprc)
{
    return gdi32_draw_focus_rect(hDC, (const GDI_RECT *)lprc);
}

DWORD WINAPI GetSysColor(int nIndex)
{
    /* Default unthemed system palette, in COLORREF order (00BBGGRR). */
    static const DWORD colors[] = {
        0x00C8C8C8, 0x00000000, 0x00D1B499, 0x00DBCDBF,
        0x00F0F0F0, 0x00FFFFFF, 0x00646464, 0x00000000,
        0x00000000, 0x00000000, 0x00B4B4B4, 0x00FCF7F4,
        0x00ABABAB, 0x00D77800, 0x00FFFFFF, 0x00F0F0F0,
        0x00A0A0A0, 0x006D6D6D, 0x00000000, 0x00544E43,
        0x00FFFFFF, 0x00696969, 0x00E3E3E3, 0x00000000,
        0x00E1FFFF, 0x00000000, 0x00CC6600, 0x00EAD1B9,
        0x00F2E4D7, 0x00D77800, 0x00F0F0F0,
    };
    return (UINT)nIndex < sizeof(colors) / sizeof(colors[0])
        ? colors[nIndex] : 0;
}

static HBRUSH WINAPI GetSysColorBrush_u32(int index)
{
    return (UINT)index < 31
        ? (HBRUSH)(ULONG_PTR)(0xBC000000u | (UINT)index) : NULL;
}

/* ── Dialog box stubs ──────────────────────────────────────── */

static LONG_PTR dialog_box_template(HINSTANCE instance, PCVOID template_data,
                                    SIZE_T template_size, HWND parent,
                                    DLGPROC proc, LPARAM init_param)
{
    BOOL owner_disabled = parent && find_window(parent) &&
                          IsWindowEnabled(parent);
    if (owner_disabled)
        EnableWindow(parent, FALSE);

    U32_DIALOG_STATE *state = NULL;
    HWND dialog = dialog_create_template(instance, template_data,
        template_size, parent, proc, init_param, TRUE, &state);
    if (!dialog || !state) {
        if (owner_disabled && find_window(parent))
            EnableWindow(parent, TRUE);
        return -1;
    }
    state->owner_disabled = owner_disabled;

    BOOL preserve_quit = FALSE;
    int quit_code = 0;
    while (state->used && !state->ended && state->window) {
        MSG message64;
        uint32_t message32[7];
        LPMSG message_buffer = g_compat32_mode
            ? (LPMSG)(void *)message32 : &message64;
        BOOL status = GetMessageA(message_buffer, NULL, 0, 0);
        if (status == (BOOL)-1) {
            state->result = -1;
            state->ended = TRUE;
            break;
        }
        if (!status) {
            MSG quit_message;
            msg_read_from(message_buffer, &quit_message);
            preserve_quit = TRUE;
            quit_code = (int)quit_message.wParam;
            state->result = -1;
            state->ended = TRUE;
            break;
        }
        if (!IsDialogMessageA(dialog, message_buffer)) {
            TranslateMessage(message_buffer);
            DispatchMessageA(message_buffer);
        }
    }

    LONG_PTR result = state->result;
    HWND owner = state->owner;
    BOOL restore_owner = state->owner_disabled;
    if (state->window && find_window(state->window))
        DestroyWindow(state->window);
    if (restore_owner && owner && find_window(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    dialog_state_clear(state);
    if (preserve_quit)
        PostQuitMessage(quit_code);
    return result;
}

LONG_PTR WINAPI DialogBoxIndirectParamA(HINSTANCE hInstance,
                                         PCVOID lpTemplate,
                                         HWND hWndParent,
                                         DLGPROC lpDialogFunc,
                                         LPARAM dwInitParam)
{
    return dialog_box_template(hInstance, lpTemplate,
        U32_DIALOG_INDIRECT_LIMIT, hWndParent, lpDialogFunc, dwInitParam);
}

LONG_PTR WINAPI DialogBoxIndirectParamW(HINSTANCE hInstance,
                                         PCVOID lpTemplate,
                                         HWND hWndParent,
                                         DLGPROC lpDialogFunc,
                                         LPARAM dwInitParam)
{
    return DialogBoxIndirectParamA(hInstance, lpTemplate, hWndParent,
                                   lpDialogFunc, dwInitParam);
}

LONG_PTR WINAPI DialogBoxParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                 HWND hWndParent, DLGPROC lpDialogFunc,
                                 LPARAM dwInitParam)
{
    PCVOID data;
    DWORD size;
    if (!kernel32_resource_data_w(hInstance, lpTemplateName, U32_RT_DIALOG,
                                  &data, &size))
        return -1;
    return dialog_box_template(hInstance, data, size, hWndParent,
                               lpDialogFunc, dwInitParam);
}

LONG_PTR WINAPI DialogBoxParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                 HWND hWndParent, DLGPROC lpDialogFunc,
                                 LPARAM dwInitParam)
{
    WCHAR name_buffer[256];
    PCWSTR wide_name;
    if (!dialog_identifier_a_to_w(lpTemplateName, name_buffer, &wide_name))
        return -1;
    return DialogBoxParamW(hInstance, wide_name, hWndParent,
                           lpDialogFunc, dwInitParam);
}

/* ── Menu stubs ────────────────────────────────────────────── */

#define U32_MAX_MENUS       128
#define U32_MAX_MENU_ITEMS   96
#define U32_MENU_TEXT_CAP    192

typedef struct {
    UINT      type;
    UINT      state;
    UINT      state_override_mask;
    UINT      id;
    HMENU     submenu;
    HBITMAP   checked_bitmap;
    HBITMAP   unchecked_bitmap;
    HBITMAP   item_bitmap;
    ULONG_PTR data;
    ULONG_PTR legacy_type_data;
    WCHAR     text[U32_MENU_TEXT_CAP];
} U32_MENU_ITEM;

typedef struct {
    BOOL      used;
    BOOL      destroying;
    BOOL      popup;
    BOOL      system_menu;
    HMENU     handle;
    HMENU     parent_menu;
    HWND      system_owner;
    DWORD     owner_pid;
    DWORD     style;
    UINT      max_height;
    HBRUSH    background;
    DWORD     context_help_id;
    ULONG_PTR data;
    UINT      item_count;
    U32_MENU_ITEM items[U32_MAX_MENU_ITEMS];
} U32_MENU;

typedef struct {
    UINT      cb_size;
    UINT      mask;
    UINT      type;
    UINT      state;
    UINT      id;
    HMENU     submenu;
    HBITMAP   checked_bitmap;
    HBITMAP   unchecked_bitmap;
    ULONG_PTR data;
    PVOID     type_data;
    UINT      cch;
    HBITMAP   item_bitmap;
} U32_MENUITEMINFO_IO;

typedef struct {
    DWORD     cb_size;
    DWORD     mask;
    DWORD     style;
    UINT      max_height;
    HBRUSH    background;
    DWORD     context_help_id;
    ULONG_PTR data;
} U32_MENUINFO_IO;

static U32_MENU u32_menus[U32_MAX_MENUS];
static ULONG_PTR next_hmenu = 0xD6000001;
static spinlock_t u32_menu_lock = SPINLOCK_INIT;

static inline uint64_t menu_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&u32_menu_lock);
    return flags;
}

static inline void menu_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&u32_menu_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static U32_MENU *menu_find_locked(HMENU handle)
{
    if (!handle) return NULL;
    for (int i = 0; i < U32_MAX_MENUS; i++)
        if (u32_menus[i].used && u32_menus[i].handle == handle)
            return &u32_menus[i];
    return NULL;
}

static U32_MENU *menu_alloc_locked(BOOL popup, DWORD owner_pid)
{
    for (int i = 0; i < U32_MAX_MENUS; i++) {
        if (u32_menus[i].used) continue;
        u32_menus[i] = (U32_MENU){0};
        u32_menus[i].used = TRUE;
        u32_menus[i].popup = popup;
        u32_menus[i].owner_pid = owner_pid;
        u32_menus[i].handle = (HMENU)next_hmenu++;
        return &u32_menus[i];
    }
    return NULL;
}

static UINT menu_wide_length(PCWSTR text)
{
    UINT length = 0;
    if (!text) return 0;
    while (text[length]) length++;
    return length;
}

static void menu_copy_wide(WCHAR *dest, UINT capacity, PCWSTR source)
{
    UINT i = 0;
    if (source)
        while (i + 1 < capacity && source[i]) {
            dest[i] = source[i];
            i++;
        }
    if (capacity) dest[i] = 0;
}

static void menu_copy_ansi_to_wide(WCHAR *dest, UINT capacity, PCSTR source)
{
    UINT i = 0;
    if (source)
        while (i + 1 < capacity && source[i]) {
            dest[i] = (WCHAR)(BYTE)source[i];
            i++;
        }
    if (capacity) dest[i] = 0;
}

static BOOL menuiteminfo_read(const void *raw, U32_MENUITEMINFO_IO *out)
{
    if (!raw || !out) return FALSE;
    *out = (U32_MENUITEMINFO_IO){0};

    if (g_compat32_mode) {
        const uint32_t *p = (const uint32_t *)raw;
        if (p[0] != 48 && p[0] != 44) return FALSE;
        out->cb_size = p[0];
        out->mask = p[1];
        out->type = p[2];
        out->state = p[3];
        out->id = p[4];
        out->submenu = (HMENU)(ULONG_PTR)p[5];
        out->checked_bitmap = (HBITMAP)(ULONG_PTR)p[6];
        out->unchecked_bitmap = (HBITMAP)(ULONG_PTR)p[7];
        out->data = p[8];
        out->type_data = (PVOID)(ULONG_PTR)p[9];
        out->cch = p[10];
        if (p[0] >= 48)
            out->item_bitmap = (HBITMAP)(ULONG_PTR)p[11];
        return TRUE;
    }

    const MENUITEMINFOW *info = (const MENUITEMINFOW *)raw;
    if (info->cbSize != sizeof(MENUITEMINFOW) && info->cbSize != 72)
        return FALSE;
    out->cb_size = info->cbSize;
    out->mask = info->fMask;
    out->type = info->fType;
    out->state = info->fState;
    out->id = info->wID;
    out->submenu = info->hSubMenu;
    out->checked_bitmap = info->hbmpChecked;
    out->unchecked_bitmap = info->hbmpUnchecked;
    out->data = info->dwItemData;
    out->type_data = info->dwTypeData;
    out->cch = info->cch;
    if (info->cbSize >= sizeof(MENUITEMINFOW))
        out->item_bitmap = info->hbmpItem;
    return TRUE;
}

static void menuiteminfo_set_cch(void *raw, UINT cch)
{
    if (g_compat32_mode)
        ((uint32_t *)raw)[10] = cch;
    else
        ((MENUITEMINFOW *)raw)->cch = cch;
}

static void menuiteminfo_set_type_data(void *raw, ULONG_PTR value)
{
    if (g_compat32_mode)
        ((uint32_t *)raw)[9] = (uint32_t)value;
    else
        ((MENUITEMINFOW *)raw)->dwTypeData = (PWSTR)value;
}

static void menuiteminfo_write_fields(void *raw,
                                      const U32_MENUITEMINFO_IO *request,
                                      const U32_MENU_ITEM *item)
{
    UINT mask = request->mask;
    if (g_compat32_mode) {
        uint32_t *p = (uint32_t *)raw;
        if (mask & (MIIM_FTYPE | MIIM_TYPE)) p[2] = item->type;
        if (mask & MIIM_STATE) p[3] = item->state;
        if (mask & MIIM_ID) p[4] = item->id;
        if (mask & MIIM_SUBMENU) p[5] = (uint32_t)(ULONG_PTR)item->submenu;
        if (mask & MIIM_CHECKMARKS) {
            p[6] = (uint32_t)(ULONG_PTR)item->checked_bitmap;
            p[7] = (uint32_t)(ULONG_PTR)item->unchecked_bitmap;
        }
        if (mask & MIIM_DATA) p[8] = (uint32_t)item->data;
        if ((mask & MIIM_BITMAP) && request->cb_size >= 48)
            p[11] = (uint32_t)(ULONG_PTR)item->item_bitmap;
    } else {
        MENUITEMINFOW *info = (MENUITEMINFOW *)raw;
        if (mask & (MIIM_FTYPE | MIIM_TYPE)) info->fType = item->type;
        if (mask & MIIM_STATE) info->fState = item->state;
        if (mask & MIIM_ID) info->wID = item->id;
        if (mask & MIIM_SUBMENU) info->hSubMenu = item->submenu;
        if (mask & MIIM_CHECKMARKS) {
            info->hbmpChecked = item->checked_bitmap;
            info->hbmpUnchecked = item->unchecked_bitmap;
        }
        if (mask & MIIM_DATA) info->dwItemData = item->data;
        if ((mask & MIIM_BITMAP) && request->cb_size >= sizeof(*info))
            info->hbmpItem = item->item_bitmap;
    }
}

static BOOL menuinfo_read(const void *raw, U32_MENUINFO_IO *out)
{
    if (!raw || !out) return FALSE;
    *out = (U32_MENUINFO_IO){0};
    if (g_compat32_mode) {
        const uint32_t *p = (const uint32_t *)raw;
        if (p[0] != 28) return FALSE;
        out->cb_size = p[0];
        out->mask = p[1];
        out->style = p[2];
        out->max_height = p[3];
        out->background = (HBRUSH)(ULONG_PTR)p[4];
        out->context_help_id = p[5];
        out->data = p[6];
        return TRUE;
    }
    const MENUINFO *info = (const MENUINFO *)raw;
    if (info->cbSize != sizeof(MENUINFO)) return FALSE;
    out->cb_size = info->cbSize;
    out->mask = info->fMask;
    out->style = info->dwStyle;
    out->max_height = info->cyMax;
    out->background = info->hbrBack;
    out->context_help_id = info->dwContextHelpID;
    out->data = info->dwMenuData;
    return TRUE;
}

static void menuinfo_write(void *raw, const U32_MENUINFO_IO *request,
                           const U32_MENU *menu)
{
    if (g_compat32_mode) {
        uint32_t *p = (uint32_t *)raw;
        if (request->mask & MIM_STYLE) p[2] = menu->style;
        if (request->mask & MIM_MAXHEIGHT) p[3] = menu->max_height;
        if (request->mask & MIM_BACKGROUND)
            p[4] = (uint32_t)(ULONG_PTR)menu->background;
        if (request->mask & MIM_HELPID) p[5] = menu->context_help_id;
        if (request->mask & MIM_MENUDATA) p[6] = (uint32_t)menu->data;
    } else {
        MENUINFO *info = (MENUINFO *)raw;
        if (request->mask & MIM_STYLE) info->dwStyle = menu->style;
        if (request->mask & MIM_MAXHEIGHT) info->cyMax = menu->max_height;
        if (request->mask & MIM_BACKGROUND) info->hbrBack = menu->background;
        if (request->mask & MIM_HELPID)
            info->dwContextHelpID = menu->context_help_id;
        if (request->mask & MIM_MENUDATA) info->dwMenuData = menu->data;
    }
}

static int menu_find_item_locked(U32_MENU *menu, UINT item, BOOL by_position,
                                 U32_MENU **containing_menu, unsigned depth)
{
    if (!menu || depth >= U32_MAX_MENUS) return -1;
    if (by_position) {
        if (item >= menu->item_count) return -1;
        if (containing_menu) *containing_menu = menu;
        return (int)item;
    }
    for (UINT i = 0; i < menu->item_count; i++)
        if (menu->items[i].id == item) {
            if (containing_menu) *containing_menu = menu;
            return (int)i;
        }
    for (UINT i = 0; i < menu->item_count; i++) {
        U32_MENU *submenu = menu_find_locked(menu->items[i].submenu);
        int index = menu_find_item_locked(submenu, item, FALSE,
                                          containing_menu, depth + 1);
        if (index >= 0) return index;
    }
    return -1;
}

static BOOL menu_can_attach_submenu_locked(U32_MENU *parent,
                                           HMENU submenu_handle,
                                           HMENU current_submenu)
{
    if (!submenu_handle) return TRUE;
    U32_MENU *submenu = menu_find_locked(submenu_handle);
    if (!submenu || submenu == parent || submenu->owner_pid != parent->owner_pid)
        return FALSE;
    if (submenu->parent_menu && submenu->parent_menu != parent->handle &&
        submenu_handle != current_submenu)
        return FALSE;
    for (U32_MENU *cursor = parent; cursor; ) {
        if (cursor == submenu) return FALSE;
        cursor = menu_find_locked(cursor->parent_menu);
    }
    return TRUE;
}

static void menu_link_submenu_locked(U32_MENU *parent, U32_MENU_ITEM *item,
                                     HMENU submenu_handle)
{
    if (item->submenu == submenu_handle) return;
    U32_MENU *old_submenu = menu_find_locked(item->submenu);
    if (old_submenu && old_submenu->parent_menu == parent->handle)
        old_submenu->parent_menu = NULL;
    item->submenu = submenu_handle;
    U32_MENU *new_submenu = menu_find_locked(submenu_handle);
    if (new_submenu)
        new_submenu->parent_menu = parent->handle;
}

static void menu_clear_other_defaults(U32_MENU *menu, U32_MENU_ITEM *selected)
{
    for (UINT i = 0; i < menu->item_count; i++)
        if (&menu->items[i] != selected)
            menu->items[i].state &= ~MFS_DEFAULT;
}

static BOOL menu_apply_item_info_locked(U32_MENU *menu, U32_MENU_ITEM *item,
                                        const U32_MENUITEMINFO_IO *info,
                                        BOOL wide)
{
    if ((info->mask & MIIM_SUBMENU) &&
        !menu_can_attach_submenu_locked(menu, info->submenu, item->submenu)) {
        SetLastError(1401);
        return FALSE;
    }
    if (info->mask & (MIIM_FTYPE | MIIM_TYPE)) item->type = info->type;
    if (info->mask & MIIM_STATE) {
        item->state = info->state;
        item->state_override_mask |= MFS_DISABLED | MFS_CHECKED |
                                     MFS_HILITE | MFS_DEFAULT;
    }
    if (info->mask & MIIM_ID) item->id = info->id;
    if (info->mask & MIIM_SUBMENU)
        menu_link_submenu_locked(menu, item, info->submenu);
    if (info->mask & MIIM_CHECKMARKS) {
        item->checked_bitmap = info->checked_bitmap;
        item->unchecked_bitmap = info->unchecked_bitmap;
    }
    if (info->mask & MIIM_DATA) item->data = info->data;
    if (info->mask & MIIM_BITMAP) item->item_bitmap = info->item_bitmap;

    if ((info->mask & MIIM_STRING) ||
        ((info->mask & MIIM_TYPE) && !(item->type &
          (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR)))) {
        if (wide)
            menu_copy_wide(item->text, U32_MENU_TEXT_CAP,
                           (PCWSTR)info->type_data);
        else
            menu_copy_ansi_to_wide(item->text, U32_MENU_TEXT_CAP,
                                   (PCSTR)info->type_data);
    } else if (info->mask & MIIM_TYPE) {
        item->legacy_type_data = (ULONG_PTR)info->type_data;
    }
    if (item->type & MFT_SEPARATOR) item->text[0] = 0;
    if (item->state & MFS_DEFAULT)
        menu_clear_other_defaults(menu, item);
    return TRUE;
}

static void menu_destroy_locked(U32_MENU *menu, unsigned depth)
{
    if (!menu || !menu->used || menu->destroying || depth >= U32_MAX_MENUS)
        return;
    HMENU handle = menu->handle;
    menu->destroying = TRUE;
    for (UINT i = 0; i < menu->item_count; i++) {
        U32_MENU *submenu = menu_find_locked(menu->items[i].submenu);
        if (submenu) menu_destroy_locked(submenu, depth + 1);
    }
    for (int i = 0; i < U32_MAX_MENUS; i++) {
        U32_MENU *parent = &u32_menus[i];
        if (!parent->used || parent == menu) continue;
        if (parent->parent_menu == handle) parent->parent_menu = NULL;
        for (UINT j = 0; j < parent->item_count; j++)
            if (parent->items[j].submenu == handle)
                parent->items[j].submenu = NULL;
    }
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].used) continue;
        if (windows[i].menu == handle) windows[i].menu = NULL;
        if (windows[i].system_menu == handle) windows[i].system_menu = NULL;
    }
    *menu = (U32_MENU){0};
}

static void menu_set_auto_disabled(U32_MENU_ITEM *item, BOOL disabled)
{
    if (!item || (item->state_override_mask & MFS_DISABLED)) return;
    item->state &= ~MFS_DISABLED;
    if (disabled) item->state |= MFS_DISABLED;
}

static U32_MENU_ITEM *menu_find_direct_command(U32_MENU *menu, UINT command)
{
    for (UINT i = 0; menu && i < menu->item_count; i++)
        if (menu->items[i].id == command)
            return &menu->items[i];
    return NULL;
}

static void menu_sync_system_locked(U32_MENU *menu, WINDOW *window)
{
    if (!menu || !window || !menu->system_menu) return;
    BOOL minimized = (window->style & WS_MINIMIZE) != 0;
    BOOL maximized = (window->style & WS_MAXIMIZE) != 0;
    WNDCLASS_ENTRY *window_class =
        lookup_class_for_pid(window->class_name, window->owner_pid);
    menu_set_auto_disabled(menu_find_direct_command(menu, SC_RESTORE),
                           !minimized && !maximized);
    menu_set_auto_disabled(menu_find_direct_command(menu, SC_MOVE), maximized);
    menu_set_auto_disabled(menu_find_direct_command(menu, SC_SIZE),
        minimized || maximized || !(window->style & WS_THICKFRAME));
    menu_set_auto_disabled(menu_find_direct_command(menu, SC_MINIMIZE),
        minimized || !(window->style & WS_MINIMIZEBOX));
    menu_set_auto_disabled(menu_find_direct_command(menu, SC_MAXIMIZE),
        maximized || !(window->style & WS_MAXIMIZEBOX));
    menu_set_auto_disabled(menu_find_direct_command(menu, SC_CLOSE),
        !(window->style & WS_SYSMENU) ||
        (window_class && (window_class->style & CS_NOCLOSE)));
}

static void menu_append_system_item(U32_MENU *menu, UINT id, UINT type,
                                    UINT state, const char *text)
{
    if (!menu || menu->item_count >= U32_MAX_MENU_ITEMS) return;
    U32_MENU_ITEM *item = &menu->items[menu->item_count++];
    *item = (U32_MENU_ITEM){0};
    item->id = id;
    item->type = type;
    item->state = state;
    menu_copy_ansi_to_wide(item->text, U32_MENU_TEXT_CAP, text);
}

static U32_MENU *menu_create_system_locked(WINDOW *window)
{
    U32_MENU *menu = menu_alloc_locked(TRUE, window->owner_pid);
    if (!menu) return NULL;
    menu->system_menu = TRUE;
    menu->system_owner = window->handle;
    menu_append_system_item(menu, SC_RESTORE, MFT_STRING, 0, "&Restore");
    menu_append_system_item(menu, SC_MOVE, MFT_STRING, 0, "&Move");
    menu_append_system_item(menu, SC_SIZE, MFT_STRING, 0, "&Size");
    menu_append_system_item(menu, SC_MINIMIZE, MFT_STRING, 0, "Mi&nimize");
    menu_append_system_item(menu, SC_MAXIMIZE, MFT_STRING, 0, "Ma&ximize");
    menu_append_system_item(menu, 0, MFT_SEPARATOR, MFS_DISABLED, NULL);
    menu_append_system_item(menu, SC_CLOSE, MFT_STRING, MFS_DEFAULT, "&Close");
    menu_sync_system_locked(menu, window);
    return menu;
}

static void menu_release_window_menus(HWND window, HMENU menu_bar,
                                      HMENU system_menu)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *system = menu_find_locked(system_menu);
    if (system) menu_destroy_locked(system, 0);
    U32_MENU *bar = menu_find_locked(menu_bar);
    if (bar && menu_bar != system_menu) menu_destroy_locked(bar, 0);
    for (int i = 0; i < U32_MAX_MENUS; i++)
        if (u32_menus[i].used && u32_menus[i].system_owner == window)
            menu_destroy_locked(&u32_menus[i], 0);
    menu_unlock_irqrestore(flags);
}

static void menu_release_process(DWORD pid)
{
    uint64_t flags = menu_lock_irqsave();
    for (int i = 0; i < U32_MAX_MENUS; i++)
        if (u32_menus[i].used && u32_menus[i].owner_pid == pid)
            menu_destroy_locked(&u32_menus[i], 0);
    menu_unlock_irqrestore(flags);
}

static void menu_sync_system_window(HWND handle)
{
    WINDOW *window = find_window(handle);
    if (!window || !window->system_menu) return;
    uint64_t flags = menu_lock_irqsave();
    menu_sync_system_locked(menu_find_locked(window->system_menu), window);
    menu_unlock_irqrestore(flags);
}

HMENU WINAPI CreateMenu(void)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_alloc_locked(FALSE, GetCurrentProcessId());
    HMENU handle = menu ? menu->handle : NULL;
    menu_unlock_irqrestore(flags);
    if (!handle) SetLastError(8);
    return handle;
}

HMENU WINAPI CreatePopupMenu(void)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_alloc_locked(TRUE, GetCurrentProcessId());
    HMENU handle = menu ? menu->handle : NULL;
    menu_unlock_irqrestore(flags);
    if (!handle) SetLastError(8);
    return handle;
}

BOOL WINAPI DestroyMenu(HMENU handle)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle);
    if (!menu) {
        menu_unlock_irqrestore(flags);
        SetLastError(1401);
        return FALSE;
    }
    menu_destroy_locked(menu, 0);
    menu_unlock_irqrestore(flags);
    return TRUE;
}

BOOL WINAPI IsMenu(HMENU handle)
{
    uint64_t flags = menu_lock_irqsave();
    BOOL valid = menu_find_locked(handle) != NULL;
    menu_unlock_irqrestore(flags);
    return valid;
}

HMENU WINAPI GetSystemMenu(HWND handle, BOOL revert)
{
    WINDOW *window = find_window(handle);
    if (!window || window->message_only) {
        SetLastError(1400);
        return NULL;
    }
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *current = menu_find_locked(window->system_menu);
    if (revert) {
        if (current) menu_destroy_locked(current, 0);
        window->system_menu = NULL;
        menu_unlock_irqrestore(flags);
        return NULL;
    }
    if (!current) {
        current = menu_create_system_locked(window);
        window->system_menu = current ? current->handle : NULL;
    } else {
        menu_sync_system_locked(current, window);
    }
    HMENU result = current ? current->handle : NULL;
    menu_unlock_irqrestore(flags);
    if (!result) SetLastError(8);
    return result;
}

BOOL WINAPI EnableMenuItem(HMENU handle, UINT requested_item, UINT enable)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle), *containing = NULL;
    int index = menu_find_item_locked(menu, requested_item,
        (enable & MF_BYPOSITION) != 0, &containing, 0);
    if (index < 0) {
        menu_unlock_irqrestore(flags);
        SetLastError(menu ? 1456 : 1401);
        return (BOOL)-1;
    }
    U32_MENU_ITEM *item = &containing->items[index];
    UINT old_state = item->state & MFS_DISABLED;
    item->state &= ~MFS_DISABLED;
    if (enable & (MF_DISABLED | MF_GRAYED)) item->state |= MFS_DISABLED;
    item->state_override_mask |= MFS_DISABLED;
    menu_unlock_irqrestore(flags);
    return (BOOL)old_state;
}

static BOOL menu_insert_item(HMENU handle, UINT requested_item,
                             BOOL by_position, const void *raw_info, BOOL wide)
{
    U32_MENUITEMINFO_IO info;
    if (!menuiteminfo_read(raw_info, &info)) {
        SetLastError(87);
        return FALSE;
    }
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle);
    if (!menu) {
        menu_unlock_irqrestore(flags);
        SetLastError(1401);
        return FALSE;
    }
    if (menu->item_count >= U32_MAX_MENU_ITEMS) {
        menu_unlock_irqrestore(flags);
        SetLastError(8);
        return FALSE;
    }

    UINT position;
    if (by_position) {
        position = requested_item > menu->item_count
                 ? menu->item_count : requested_item;
    } else {
        U32_MENU *containing = NULL;
        int found = menu_find_item_locked(menu, requested_item, FALSE,
                                           &containing, 0);
        if (found < 0 || containing != menu) {
            menu_unlock_irqrestore(flags);
            SetLastError(1456);
            return FALSE;
        }
        position = (UINT)found;
    }

    U32_MENU_ITEM inserted = { .type = MFT_STRING, .state = MFS_ENABLED };
    if (!menu_apply_item_info_locked(menu, &inserted, &info, wide)) {
        menu_unlock_irqrestore(flags);
        return FALSE;
    }
    for (UINT i = menu->item_count; i > position; i--)
        menu->items[i] = menu->items[i - 1];
    menu->items[position] = inserted;
    menu->item_count++;
    U32_MENU *submenu = menu_find_locked(inserted.submenu);
    if (submenu) submenu->parent_menu = menu->handle;
    if (inserted.state & MFS_DEFAULT)
        menu_clear_other_defaults(menu, &menu->items[position]);
    menu_unlock_irqrestore(flags);
    return TRUE;
}

BOOL WINAPI InsertMenuItemA(HMENU menu, UINT item, BOOL by_position,
                            LPCMENUITEMINFOA info)
{
    return menu_insert_item(menu, item, by_position, info, FALSE);
}

BOOL WINAPI InsertMenuItemW(HMENU menu, UINT item, BOOL by_position,
                            LPCMENUITEMINFOW info)
{
    return menu_insert_item(menu, item, by_position, info, TRUE);
}

BOOL WINAPI GetMenuInfo(HMENU handle, LPMENUINFO raw_info)
{
    U32_MENUINFO_IO request;
    if (!menuinfo_read(raw_info, &request)) {
        SetLastError(87);
        return FALSE;
    }
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle);
    if (!menu) {
        menu_unlock_irqrestore(flags);
        SetLastError(1401);
        return FALSE;
    }
    menuinfo_write(raw_info, &request, menu);
    menu_unlock_irqrestore(flags);
    return TRUE;
}

static void menu_set_info_locked(U32_MENU *menu,
                                 const U32_MENUINFO_IO *info,
                                 unsigned depth)
{
    if (!menu || depth >= U32_MAX_MENUS) return;
    if (info->mask & MIM_STYLE) menu->style = info->style;
    if (info->mask & MIM_MAXHEIGHT) menu->max_height = info->max_height;
    if (info->mask & MIM_BACKGROUND) menu->background = info->background;
    if (info->mask & MIM_HELPID) menu->context_help_id = info->context_help_id;
    if (info->mask & MIM_MENUDATA) menu->data = info->data;
    if (info->mask & MIM_APPLYTOSUBMENUS)
        for (UINT i = 0; i < menu->item_count; i++)
            menu_set_info_locked(menu_find_locked(menu->items[i].submenu),
                                 info, depth + 1);
}

BOOL WINAPI SetMenuInfo(HMENU handle, LPCMENUINFO raw_info)
{
    U32_MENUINFO_IO info;
    if (!menuinfo_read(raw_info, &info)) {
        SetLastError(87);
        return FALSE;
    }
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle);
    if (!menu) {
        menu_unlock_irqrestore(flags);
        SetLastError(1401);
        return FALSE;
    }
    menu_set_info_locked(menu, &info, 0);
    menu_unlock_irqrestore(flags);
    return TRUE;
}

BOOL WINAPI SetMenuDefaultItem(HMENU handle, UINT requested_item,
                               UINT by_position)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle);
    if (!menu) {
        menu_unlock_irqrestore(flags);
        SetLastError(1401);
        return FALSE;
    }
    for (UINT i = 0; i < menu->item_count; i++)
        menu->items[i].state &= ~MFS_DEFAULT;
    if (requested_item == (UINT)-1) {
        menu_unlock_irqrestore(flags);
        return TRUE;
    }
    U32_MENU *containing = NULL;
    int index = menu_find_item_locked(menu, requested_item,
                                      by_position != 0, &containing, 0);
    if (index < 0) {
        menu_unlock_irqrestore(flags);
        SetLastError(1456);
        return FALSE;
    }
    containing->items[index].state |= MFS_DEFAULT;
    containing->items[index].state_override_mask |= MFS_DEFAULT;
    menu_unlock_irqrestore(flags);
    return TRUE;
}

static UINT menu_get_default_locked(U32_MENU *menu, UINT by_position,
                                    UINT flags, unsigned depth)
{
    if (!menu || depth >= U32_MAX_MENUS) return (UINT)-1;
    for (UINT i = 0; i < menu->item_count; i++) {
        U32_MENU_ITEM *item = &menu->items[i];
        if (!(item->state & MFS_DEFAULT)) continue;
        if ((item->state & MFS_DISABLED) && !(flags & GMDI_USEDISABLED))
            continue;
        if ((flags & GMDI_GOINTOPOPUPS) && item->submenu) {
            UINT nested = menu_get_default_locked(menu_find_locked(item->submenu),
                                                   by_position, flags, depth + 1);
            if (nested != (UINT)-1) return nested;
        }
        return by_position ? i : item->id;
    }
    return (UINT)-1;
}

UINT WINAPI GetMenuDefaultItem(HMENU handle, UINT by_position, UINT flags)
{
    uint64_t lock_flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle);
    UINT result = menu_get_default_locked(menu, by_position, flags, 0);
    menu_unlock_irqrestore(lock_flags);
    if (!menu) SetLastError(1401);
    else if (result == (UINT)-1) SetLastError(1456);
    return result;
}

HMENU WINAPI LoadMenuA(HINSTANCE hInstance, PCSTR lpMenuName)
{
    (void)hInstance; (void)lpMenuName;
    SetLastError(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return NULL;
}

HMENU WINAPI LoadMenuW(HINSTANCE hInstance, PCWSTR lpMenuName)
{
    (void)hInstance; (void)lpMenuName;
    SetLastError(1814);
    return NULL;
}

HMENU WINAPI GetSubMenu(HMENU hMenu, int nPos)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(hMenu);
    HMENU result = menu && nPos >= 0 && (UINT)nPos < menu->item_count
                 ? menu->items[nPos].submenu : NULL;
    menu_unlock_irqrestore(flags);
    if (!menu) SetLastError(1401);
    return result;
}

int WINAPI GetMenuItemCount(HMENU hMenu)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(hMenu);
    int count = menu ? (int)menu->item_count : -1;
    menu_unlock_irqrestore(flags);
    if (!menu) SetLastError(1401);
    return count;
}

UINT WINAPI GetMenuState(HMENU hMenu, UINT uId, UINT uFlags)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(hMenu), *containing = NULL;
    int index = menu_find_item_locked(menu, uId,
        (uFlags & MF_BYPOSITION) != 0, &containing, 0);
    if (index < 0) {
        menu_unlock_irqrestore(flags);
        SetLastError(menu ? 1456 : 1401);
        return (UINT)-1;
    }
    U32_MENU_ITEM *item = &containing->items[index];
    UINT state = item->type | item->state;
    U32_MENU *submenu = menu_find_locked(item->submenu);
    if (submenu)
        state |= MF_POPUP | ((submenu->item_count > 255 ? 255 :
                             submenu->item_count) << 8);
    menu_unlock_irqrestore(flags);
    return state;
}

static BOOL menu_get_item_info(HMENU handle, UINT requested_item,
                               BOOL by_position, void *raw_info, BOOL wide)
{
    U32_MENUITEMINFO_IO request;
    if (!menuiteminfo_read(raw_info, &request)) {
        SetLastError(87);
        return FALSE;
    }
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle), *containing = NULL;
    int index = menu_find_item_locked(menu, requested_item, by_position,
                                      &containing, 0);
    if (index < 0) {
        menu_unlock_irqrestore(flags);
        SetLastError(menu ? 1456 : 1401);
        return FALSE;
    }
    U32_MENU_ITEM *found = &containing->items[index];
    menuiteminfo_write_fields(raw_info, &request, found);
    if ((request.mask & MIIM_STRING) ||
        ((request.mask & MIIM_TYPE) && !(found->type &
          (MFT_BITMAP | MFT_OWNERDRAW | MFT_SEPARATOR)))) {
        UINT length = menu_wide_length(found->text);
        UINT copied = 0;
        if (request.type_data && request.cch) {
            UINT limit = request.cch - 1;
            copied = length < limit ? length : limit;
            if (wide) {
                PWSTR output = (PWSTR)request.type_data;
                for (UINT i = 0; i < copied; i++) output[i] = found->text[i];
                output[copied] = 0;
            } else {
                PSTR output = (PSTR)request.type_data;
                for (UINT i = 0; i < copied; i++)
                    output[i] = found->text[i] <= 0xFF
                              ? (char)found->text[i] : '?';
                output[copied] = 0;
            }
        }
        menuiteminfo_set_cch(raw_info, request.type_data ? copied : length);
    } else if (request.mask & MIIM_TYPE) {
        menuiteminfo_set_type_data(raw_info, found->legacy_type_data);
        menuiteminfo_set_cch(raw_info, 0);
    }
    menu_unlock_irqrestore(flags);
    return TRUE;
}

BOOL WINAPI GetMenuItemInfoA(HMENU menu, UINT item, BOOL by_position,
                             LPMENUITEMINFOA info)
{
    return menu_get_item_info(menu, item, by_position, info, FALSE);
}

BOOL WINAPI GetMenuItemInfoW(HMENU menu, UINT item, BOOL by_position,
                             LPMENUITEMINFOW info)
{
    return menu_get_item_info(menu, item, by_position, info, TRUE);
}

static BOOL menu_set_item_info(HMENU handle, UINT requested_item,
                               BOOL by_position, const void *raw_info,
                               BOOL wide)
{
    U32_MENUITEMINFO_IO info;
    if (!menuiteminfo_read(raw_info, &info)) {
        SetLastError(87);
        return FALSE;
    }
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle), *containing = NULL;
    int index = menu_find_item_locked(menu, requested_item, by_position,
                                      &containing, 0);
    if (index < 0) {
        menu_unlock_irqrestore(flags);
        SetLastError(menu ? 1456 : 1401);
        return FALSE;
    }
    BOOL result = menu_apply_item_info_locked(containing,
        &containing->items[index], &info, wide);
    menu_unlock_irqrestore(flags);
    return result;
}

BOOL WINAPI SetMenuItemInfoA(HMENU menu, UINT item, BOOL by_position,
                             LPCMENUITEMINFOA info)
{
    return menu_set_item_info(menu, item, by_position, info, FALSE);
}

BOOL WINAPI SetMenuItemInfoW(HMENU menu, UINT item, BOOL by_position,
                             LPCMENUITEMINFOW info)
{
    return menu_set_item_info(menu, item, by_position, info, TRUE);
}

DWORD WINAPI CheckMenuItem(HMENU handle, UINT requested_item, UINT check)
{
    uint64_t flags = menu_lock_irqsave();
    U32_MENU *menu = menu_find_locked(handle), *containing = NULL;
    int index = menu_find_item_locked(menu, requested_item,
        (check & MF_BYPOSITION) != 0, &containing, 0);
    if (index < 0) {
        menu_unlock_irqrestore(flags);
        SetLastError(menu ? 1456 : 1401);
        return (DWORD)-1;
    }
    U32_MENU_ITEM *item = &containing->items[index];
    DWORD previous = item->state & MFS_CHECKED;
    item->state &= ~MFS_CHECKED;
    if (check & MF_CHECKED) item->state |= MFS_CHECKED;
    item->state_override_mask |= MFS_CHECKED;
    menu_unlock_irqrestore(flags);
    return previous;
}

BOOL WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y,
                            int nReserved, HWND hWnd, PVOID prcRect)
{
    (void)uFlags; (void)x; (void)y; (void)nReserved; (void)prcRect;
    if (!IsMenu(hMenu)) {
        SetLastError(1401);
        return FALSE;
    }
    if (!find_window(hWnd)) {
        SetLastError(1400);
        return FALSE;
    }
    SendMessageA(hWnd, WM_INITMENU, (WPARAM)(ULONG_PTR)hMenu, 0);
    SendMessageA(hWnd, WM_INITMENUPOPUP, (WPARAM)(ULONG_PTR)hMenu, 0);
    return FALSE;
}

/* ── Cursor / misc stubs ───────────────────────────────────── */

HCURSOR WINAPI SetCursor(HCURSOR hCursor)
{
    HCURSOR previous = current_cursor;
    current_cursor = hCursor;
    return previous;
}

HCURSOR WINAPI GetCursor(void)
{
    return current_cursor;
}

HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, PCWSTR lpCursorName)
{
    (void)hInstance; (void)lpCursorName;
    return U32_SHARED_CURSOR_W;
}

BOOL WINAPI DestroyCursor(HCURSOR hCursor)
{
    if (hCursor == U32_SHARED_CURSOR_A || hCursor == U32_SHARED_CURSOR_W)
        return TRUE;
    if (!icon_destroy(hCursor, TRUE)) {
        SetLastError(1402); /* ERROR_INVALID_CURSOR_HANDLE */
        return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI LoadImageA(HINSTANCE hInst, PCSTR name, UINT type,
                          int cx, int cy, UINT fuLoad)
{
    (void)cx;
    (void)cy;
    (void)fuLoad;
    if (type == 1) /* IMAGE_ICON */
        return LoadIconA(hInst, name);
    if (type == 2) /* IMAGE_CURSOR */
        return LoadCursorA(hInst, name);
    SetLastError(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    return NULL;
}

HANDLE WINAPI CopyImage(HANDLE image, UINT type, int cx, int cy, UINT flags)
{
    (void)cx;
    (void)cy;
    if (!image) {
        SetLastError(6);
        return NULL;
    }
    if (flags & 0x00000004u) /* LR_COPYRETURNORG */
        return image;

    HANDLE copy = NULL;
    if (type == 0) { /* IMAGE_BITMAP */
        copy = gdi32_clone_bitmap((HBITMAP)image);
    } else if (type == 1 || type == 2) { /* IMAGE_ICON / IMAGE_CURSOR */
        copy = CopyIcon((HICON)image);
    } else {
        SetLastError(87);
        return NULL;
    }
    if (!copy) return NULL;

    if (flags & 0x00000008u) { /* LR_COPYDELETEORG */
        if (type == 0)
            DeleteObject(image);
        else if (type == 1)
            DestroyIcon((HICON)image);
        else
            DestroyCursor((HCURSOR)image);
    }
    return copy;
}

HANDLE WINAPI LoadImageW(HINSTANCE hInst, PCWSTR name, UINT type,
                          int cx, int cy, UINT fuLoad)
{
    (void)cx; (void)cy; (void)fuLoad;
    if (type == 1) /* IMAGE_ICON */
        return LoadIconW(hInst, name);
    if (type == 2) /* IMAGE_CURSOR */
        return LoadCursorW(hInst, name);
    SetLastError(1813);
    return NULL;
}

UINT WINAPI RegisterWindowMessageA(PCSTR lpString)
{
    (void)lpString;
    return 0xC000;
}

UINT WINAPI RegisterWindowMessageW(PCWSTR lpString)
{
    (void)lpString;
    return 0xC000;
}

BOOL WINAPI PostMessageW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    return msg_enqueue(hWnd, Msg, wParam, lParam);
}

/* ── Additional stubs ──────────────────────────────────────── */

BOOL WINAPI EnableWindow(HWND hWnd, BOOL bEnable)
{
    WINDOW *w = find_window(hWnd);
    if (!w) {
        SetLastError(1400);
        return FALSE;
    }
    BOOL was_disabled = (w->style & WS_DISABLED) != 0;
    BOOL will_disable = !bEnable;
    BOOL repair_activation = user32_foreground_active;
    WINDOW *root = window_root(w, NULL);
    HWND preferred = root ? root->owner : NULL;
    if (was_disabled != will_disable) {
        if (will_disable)
            w->style |= WS_DISABLED;
        else
            w->style &= ~WS_DISABLED;
        if (w->wndproc)
            dispatch_wndproc(w->wndproc, hWnd, WM_ENABLE, bEnable, 0);
        if (will_disable && repair_activation)
            repair_user32_activation(preferred);
    }
    return was_disabled;
}

HMENU WINAPI GetMenu(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    return w && !(w->style & WS_CHILD) ? w->menu : NULL;
}

DWORD WINAPI GetMessageTime(void)
{
    /* NT: the timestamp of the last message retrieved by Get/PeekMessage.
     * WinDrv stores this per mouse-button event for double-click detection
     * (windrv.bin 0x11108457/0x1110847C/0x111084A1) — a constant 0 broke it. */
    return g_last_msg_time;
}

DWORD WINAPI GetMessagePos(void)
{
    return (DWORD)(WORD)(int16_t)g_last_msg_pos.x |
           ((DWORD)(WORD)(int16_t)g_last_msg_pos.y << 16);
}

HWND WINAPI GetFocus(void)
{
    WINDOW *focused = find_window(focus_hwnd);
    HWND r = user32_foreground_active && focused && !focused->destroying
        ? focused->handle : NULL;
    {
        static uint32_t n = 0;
        if (u32_input_diagnostics_active() && (n++ & 0x3FF) == 0) {
            serial_puts("[CAP] GetFocus->0x");
            serial_puthex((uint64_t)(ULONG_PTR)r, 8);
            serial_puts("\n");
        }
    }
    return r;
}

BOOL WINAPI IsWindowVisible(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    return w && !w->message_only && window_style_is_visible(w);
}

int WINAPI MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints)
{
    /* NT: translate points from hWndFrom's client space to hWndTo's client
     * space (NULL = screen). Borderless model: a window's client origin in
     * screen space is (x, y). Return value packs the applied delta
     * (LOWORD=dx, HIWORD=dy) like real user32. Used by WinDrv SetMouseCapture
     * to convert its client rect to screen for the recenter math
     * (windrv.bin 0x11106682-0x111066A4) — the old no-op was only right
     * because the game window sits at 0,0. */
    LONG dx = 0, dy = 0;
    WINDOW *from = hWndFrom ? find_window(hWndFrom) : NULL;
    WINDOW *to   = hWndTo   ? find_window(hWndTo)   : NULL;
    if ((hWndFrom && !from && !hwnd_is_desktop(hWndFrom)) ||
        (hWndTo && !to && !hwnd_is_desktop(hWndTo))) {
        SetLastError(1400);
        return 0;
    }
    if (from) {
        int x, y;
        window_screen_origin(from, &x, &y);
        dx += x; dy += y;
    }
    if (to) {
        int x, y;
        window_screen_origin(to, &x, &y);
        dx -= x; dy -= y;
    }
    LONG *pt = (LONG *)lpPoints;
    if (pt) {
        for (UINT i = 0; i < cPoints; i++) {
            pt[i * 2 + 0] += dx;
            pt[i * 2 + 1] += dy;
        }
    }
    {
        static int n = 0;
        if (u32_input_diagnostics_active() && n < 32 &&
            relative_pointer_mode_active()) {
            extern uint32_t compat32_get_last_caller_eip(void);
            log_input_prefix("[MOUSE-RECT] MapWindowPoints");
            serial_puts(" from=0x"); serial_puthex((uint64_t)(ULONG_PTR)hWndFrom, 8);
            serial_puts(" to=0x"); serial_puthex((uint64_t)(ULONG_PTR)hWndTo, 8);
            serial_puts(" dx="); serial_putdec((uint64_t)(uint32_t)dx);
            serial_puts(" dy="); serial_putdec((uint64_t)(uint32_t)dy);
            serial_puts(" n="); serial_putdec((uint64_t)cPoints);
            if (pt && cPoints) {
                serial_puts(" p0=");
                serial_putdec((uint64_t)(uint32_t)pt[0]);
                serial_puts(",");
                serial_putdec((uint64_t)(uint32_t)pt[1]);
            }
            serial_puts(" eip=0x"); serial_puthex(compat32_get_last_caller_eip(), 8);
            serial_puts("\n");
            n++;
        }
    }
    return (int)((((uint32_t)dy & 0xFFFF) << 16) | ((uint32_t)dx & 0xFFFF));
}

BOOL WINAPI RegisterHotKey(HWND hWnd, int id, UINT fsModifiers, UINT vk)
{
    (void)hWnd; (void)id; (void)fsModifiers; (void)vk;
    return FALSE;
}

BOOL WINAPI SetMenu(HWND hWnd, HMENU hMenu)
{
    WINDOW *w = find_window(hWnd);
    if (!w || (w->style & WS_CHILD)) {
        SetLastError(w ? 87 : 1400);
        return FALSE;
    }
    if (hMenu) {
        uint64_t flags = menu_lock_irqsave();
        U32_MENU *menu = menu_find_locked(hMenu);
        BOOL valid = menu && !menu->system_menu;
        menu_unlock_irqrestore(flags);
        if (!valid) {
            SetLastError(1401);
            return FALSE;
        }
    }
    w->menu = hMenu;
    invalidate_window(w, NULL, TRUE);
    return TRUE;
}

HWND WINAPI SetParent(HWND hWndChild, HWND hWndNewParent)
{
    WINDOW *child = find_window(hWndChild);
    if (!child) {
        SetLastError(1400);
        return NULL;
    }

    int to_message = hwnd_is_message(hWndNewParent);
    HWND new_parent = hWndNewParent;
    if (!new_parent)
        new_parent = GetDesktopWindow();

    WINDOW *parent = NULL;
    if (!to_message && !hwnd_is_desktop(new_parent)) {
        parent = find_window(new_parent);
        if (!parent || new_parent == hWndChild || IsChild(hWndChild, new_parent)) {
            SetLastError(1400);
            return NULL;
        }
        if (parent->message_only)
            to_message = 1;
    }

    HWND previous = child->message_only && !child->parent
        ? HWND_MESSAGE
        : (child->parent ? child->parent : GetDesktopWindow());

    child->owner = NULL;
    child->message_only = to_message;
    child->parent = to_message ? NULL : new_parent;
    if (to_message)
        child->visible = 0;

    place_window_in_z_order(child, HWND_TOP);
    sync_all_window_compositor_state();
    if (!to_message && child->visible)
        invalidate_window(child, NULL, TRUE);
    return previous;
}

HWND WINAPI SetActiveWindow(HWND hWnd)
{
    HWND prev = GetActiveWindow();
    if (!hWnd) {
        user32_deactivate_compositor_windows();
        return prev;
    }
    WINDOW *w = find_window(hWnd);
    if (w)
        dispatch_wm_activate(w);
    return prev;
}

BOOL WINAPI UnregisterHotKey(HWND hWnd, int id)
{
    (void)hWnd; (void)id;
    return FALSE;
}

BOOL WINAPI ValidateRect(HWND hWnd, const RECT *lpRect)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    if (!w->paint_pending) return TRUE;
    if (!lpRect ||
        (lpRect->left <= w->update_rect.left &&
         lpRect->top <= w->update_rect.top &&
         lpRect->right >= w->update_rect.right &&
         lpRect->bottom >= w->update_rect.bottom)) {
        w->paint_pending = 0;
        w->erase_pending = 0;
        w->update_rect.left = w->update_rect.top = 0;
        w->update_rect.right = w->update_rect.bottom = 0;
    }
    return TRUE;
}

static HANDLE WINAPI SetWinEventHook_k32(DWORD event_min, DWORD event_max,
                                          HANDLE module, PVOID callback,
                                          DWORD process_id, DWORD thread_id,
                                          DWORD flags)
{
    (void)event_min; (void)event_max; (void)module; (void)callback;
    (void)process_id; (void)thread_id; (void)flags;
    return (HANDLE)(ULONG_PTR)0x5745;
}

static BOOL WINAPI UnhookWinEvent_k32(HANDLE hook)
{
    return hook != NULL;
}

static void WINAPI NotifyWinEvent_k32(DWORD event, HWND window,
                                      LONG object_id, LONG child_id)
{
    (void)event;
    (void)window;
    (void)object_id;
    (void)child_id;
}

static PSTR WINAPI CharNextA_k32(PCSTR current)
{
    return (PSTR)(current && *current ? current + 1 : current);
}

static PWSTR WINAPI CharNextW_k32(PCWSTR current)
{
    return (PWSTR)(current && *current ? current + 1 : current);
}

static PWSTR WINAPI CharPrevW_k32(PCWSTR start, PCWSTR current)
{
    return (PWSTR)(start && current && current > start ? current - 1 : start);
}

static int ws_put_uint(WCHAR *out, int pos, uint32_t value, unsigned base,
                       int width, int zero_pad, int negative, int upper)
{
    WCHAR rev[16];
    int digits = 0;
    do {
        uint32_t digit = value % base;
        rev[digits++] = (WCHAR)(digit < 10 ? '0' + digit
                                           : (upper ? 'A' : 'a') + digit - 10);
        value /= base;
    } while (value && digits < (int)(sizeof(rev) / sizeof(rev[0])));

    int total = digits + negative;
    if (negative && zero_pad && pos < 1023) out[pos++] = '-';
    while (total++ < width && pos < 1023) out[pos++] = zero_pad ? '0' : ' ';
    if (negative && !zero_pad && pos < 1023) out[pos++] = '-';
    while (digits && pos < 1023) out[pos++] = rev[--digits];
    return pos;
}

static int ws_put_uint_a(char *out, int pos, uint64_t value, unsigned base,
                         int width, int zero_pad, int negative, int upper)
{
    char rev[32];
    int digits = 0;
    do {
        uint64_t digit = value % base;
        rev[digits++] = (char)(digit < 10 ? '0' + digit
                                          : (upper ? 'A' : 'a') + digit - 10);
        value /= base;
    } while (value && digits < (int)sizeof(rev));

    int total = digits + negative;
    if (negative && zero_pad && pos < 1023) out[pos++] = '-';
    while (total++ < width && pos < 1023) out[pos++] = zero_pad ? '0' : ' ';
    if (negative && !zero_pad && pos < 1023) out[pos++] = '-';
    while (digits && pos < 1023) out[pos++] = rev[--digits];
    return pos;
}

/* wsprintfA/W are cdecl varargs. PE32 thunks cannot forward a va_list across
 * the mode switch, so the ABI descriptor supplies a bounded DWORD window and
 * these formatters consume only the arguments required by the format string. */
static int WINAPI wsprintfA_k32(PSTR out, PCSTR fmt,
                                 ULONG_PTR a0, ULONG_PTR a1,
                                 ULONG_PTR a2, ULONG_PTR a3,
                                 ULONG_PTR a4, ULONG_PTR a5,
                                 ULONG_PTR a6, ULONG_PTR a7,
                                 ULONG_PTR a8, ULONG_PTR a9)
{
    ULONG_PTR args[10] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    int arg = 0, pos = 0;
    if (!out || !fmt) return 0;

    while (*fmt && pos < 1023) {
        if (*fmt != '%') {
            out[pos++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out[pos++] = *fmt++;
            continue;
        }

        int zero_pad = 0, width = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        int long_long = 0;
        if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') {
            long_long = 1;
            fmt += 3;
        } else if (*fmt == 'h' || *fmt == 'l') {
            fmt++;
        }

        char spec = *fmt ? *fmt++ : 0;
        uint64_t raw = arg < 10 ? args[arg++] : 0;
        if (long_long) {
            uint64_t high = arg < 10 ? args[arg++] : 0;
            raw |= high << 32;
        }

        if (spec == 's') {
            PCSTR text = (PCSTR)(ULONG_PTR)raw;
            while (text && *text && pos < 1023) out[pos++] = *text++;
        } else if (spec == 'S') {
            PCWSTR text = (PCWSTR)(ULONG_PTR)raw;
            while (text && *text && pos < 1023) {
                WCHAR ch = *text++;
                out[pos++] = ch <= 0x7F ? (char)ch : '?';
            }
        } else if (spec == 'd' || spec == 'i') {
            int negative = long_long ? (int64_t)raw < 0 : (int32_t)raw < 0;
            uint64_t value = negative ? 0ULL - raw : raw;
            if (!long_long) value = (uint32_t)value;
            pos = ws_put_uint_a(out, pos, value, 10, width, zero_pad,
                                negative, 0);
        } else if (spec == 'u' || spec == 'x' || spec == 'X' ||
                   spec == 'o' || spec == 'p') {
            unsigned base = spec == 'u' ? 10 : (spec == 'o' ? 8 : 16);
            pos = ws_put_uint_a(out, pos, raw, base, width, zero_pad, 0,
                                spec == 'X');
        } else if (spec == 'c' || spec == 'C') {
            out[pos++] = (char)raw;
        } else {
            out[pos++] = '%';
            if (spec && pos < 1023) out[pos++] = spec;
        }
    }
    out[pos] = 0;
    return pos;
}

/* USER32's wsprintf is variadic cdecl despite living in a stdcall DLL. */
static int WINAPI wsprintfW_k32(PWSTR out, PCWSTR fmt,
                                 ULONG_PTR a0, ULONG_PTR a1,
                                 ULONG_PTR a2, ULONG_PTR a3,
                                 ULONG_PTR a4, ULONG_PTR a5,
                                 ULONG_PTR a6, ULONG_PTR a7,
                                 ULONG_PTR a8, ULONG_PTR a9)
{
    ULONG_PTR args[10] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    int arg = 0, pos = 0;
    if (!out || !fmt) return 0;

    while (*fmt && pos < 1023) {
        if (*fmt != '%') {
            out[pos++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out[pos++] = *fmt++;
            continue;
        }

        int zero_pad = 0, width = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        WCHAR spec = *fmt ? *fmt++ : 0;
        ULONG_PTR raw = arg < 10 ? args[arg++] : 0;

        if (spec == 's') {
            PCWSTR text = (PCWSTR)(ULONG_PTR)raw;
            while (text && *text && pos < 1023) out[pos++] = *text++;
        } else if (spec == 'S') {
            PCSTR text = (PCSTR)(ULONG_PTR)raw;
            while (text && *text && pos < 1023)
                out[pos++] = (WCHAR)(unsigned char)*text++;
        } else if (spec == 'd' || spec == 'i') {
            int negative = (int32_t)raw < 0;
            uint32_t value = negative ? 0u - raw : raw;
            pos = ws_put_uint(out, pos, value, 10, width, zero_pad,
                              negative, 0);
        } else if (spec == 'u' || spec == 'x' || spec == 'X') {
            pos = ws_put_uint(out, pos, raw, spec == 'u' ? 10 : 16,
                              width, zero_pad, 0, spec == 'X');
        } else if (spec == 'c') {
            out[pos++] = (WCHAR)raw;
        } else {
            out[pos++] = '%';
            if (spec && pos < 1023) out[pos++] = spec;
        }
    }
    out[pos] = 0;
    return pos;
}

static void WINAPI WTSFreeMemory_u32(PVOID memory)
{
    (void)memory;
}

static BOOL WINAPI WTSQuerySessionInformationW_u32(HANDLE server,
                                                    DWORD session_id,
                                                    DWORD info_class,
                                                    PWSTR *buffer,
                                                    DWORD *bytes_returned)
{
    (void)server;
    (void)session_id;
    (void)info_class;
    if (buffer) *buffer = NULL;
    if (bytes_returned) *bytes_returned = 0;
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return FALSE;
}

static BOOL WINAPI WTSRegisterSessionNotification_u32(HWND window, DWORD flags)
{
    (void)window;
    (void)flags;
    return TRUE;
}

static BOOL WINAPI WTSUnRegisterSessionNotification_u32(HWND window)
{
    (void)window;
    return TRUE;
}

static BOOL WINAPI DwmDefWindowProc_u32(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam,
                                        LRESULT *result)
{
    (void)window;
    (void)message;
    (void)wparam;
    (void)lparam;
    if (result) *result = 0;
    return FALSE;
}

static LONG WINAPI DwmExtendFrameIntoClientArea_u32(HWND window,
                                                     PCVOID margins)
{
    (void)window;
    (void)margins;
    return 0; /* S_OK: Osito's compositor already owns the frame. */
}

typedef struct {
    UINT numerator;
    UINT denominator;
} U32_UNSIGNED_RATIO;

typedef struct {
    UINT cb_size;
    U32_UNSIGNED_RATIO refresh_rate;
    ULONGLONG qpc_refresh_period;
    U32_UNSIGNED_RATIO compose_rate;
    ULONGLONG qpc_vblank;
    ULONGLONG refresh_count;
    UINT dx_refresh_count;
    ULONGLONG qpc_compose;
    ULONGLONG frame_count;
    UINT dx_present_count;
    ULONGLONG refresh_frame;
    ULONGLONG frame_submitted;
    UINT dx_present_submitted;
    ULONGLONG frame_confirmed;
    UINT dx_present_confirmed;
    ULONGLONG refresh_confirmed;
    UINT dx_refresh_confirmed;
    ULONGLONG frames_late;
    UINT frames_outstanding;
    ULONGLONG frame_displayed;
    ULONGLONG qpc_frame_displayed;
    ULONGLONG refresh_frame_displayed;
    ULONGLONG frame_complete;
    ULONGLONG qpc_frame_complete;
    ULONGLONG frame_pending;
    ULONGLONG qpc_frame_pending;
    ULONGLONG frames_displayed;
    ULONGLONG frames_complete;
    ULONGLONG frames_pending;
    ULONGLONG frames_available;
    ULONGLONG frames_dropped;
    ULONGLONG frames_missed;
    ULONGLONG refresh_next_displayed;
    ULONGLONG refresh_next_presented;
    ULONGLONG refreshes_displayed;
    ULONGLONG refreshes_presented;
    ULONGLONG refresh_started;
    ULONGLONG pixels_received;
    ULONGLONG pixels_drawn;
    ULONGLONG buffers_empty;
} U32_DWM_TIMING_INFO;

_Static_assert(sizeof(U32_DWM_TIMING_INFO) == 320,
               "Win64 DWM_TIMING_INFO ABI");

static LONG WINAPI DwmGetCompositionTimingInfo_u32(HWND window, PVOID timing)
{
    U32_DWM_TIMING_INFO *info = (U32_DWM_TIMING_INFO *)timing;
    if (!info || info->cb_size != sizeof(*info))
        return (LONG)0x80070057; /* E_INVALIDARG */
    if (window && !hwnd_is_desktop(window) && !find_window(window))
        return (LONG)0x80070006; /* E_HANDLE */

    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    ULONGLONG now = ((ULONGLONG)hi << 32) | lo;
    extern uint64_t idt_get_tsc_freq(void);
    ULONGLONG frequency = idt_get_tsc_freq();
    if (!frequency)
        frequency = 3000000000ULL;
    ULONGLONG period = frequency / 60;
    if (!period)
        period = 1;
    ULONGLONG vblank = now - now % period;
    ULONGLONG frame = vblank / period;

    memset(info, 0, sizeof(*info));
    info->cb_size = sizeof(*info);
    info->refresh_rate.numerator = 60;
    info->refresh_rate.denominator = 1;
    info->qpc_refresh_period = period;
    info->compose_rate = info->refresh_rate;
    info->qpc_vblank = vblank;
    info->refresh_count = frame;
    info->dx_refresh_count = (UINT)frame;
    info->qpc_compose = vblank;
    info->frame_count = frame;
    info->dx_present_count = (UINT)frame;
    info->refresh_frame = frame;
    info->frame_submitted = frame;
    info->dx_present_submitted = (UINT)frame;
    info->frame_confirmed = frame;
    info->dx_present_confirmed = (UINT)frame;
    info->refresh_confirmed = frame;
    info->dx_refresh_confirmed = (UINT)frame;
    info->frame_displayed = frame;
    info->qpc_frame_displayed = vblank;
    info->refresh_frame_displayed = frame;
    info->frame_complete = frame;
    info->qpc_frame_complete = vblank;
    info->frames_displayed = frame;
    info->frames_complete = frame;
    info->refresh_next_displayed = frame + 1;
    info->refresh_next_presented = frame + 1;
    info->refreshes_displayed = frame;
    info->refreshes_presented = frame;
    info->refresh_started = frame;
    info->buffers_empty = 2;

    static unsigned trace_count;
    if (trace_count++ < 8) {
        serial_puts("[DWM-TIMING] hwnd=0x");
        serial_puthex((uint64_t)(ULONG_PTR)window, 8);
        serial_puts(" qpc=0x");
        serial_puthex(vblank, 16);
        serial_puts(" period=");
        serial_putdec(period);
        serial_puts("\n");
    }
    return 0;
}

static LONG WINAPI DwmGetWindowAttribute_u32(HWND window, DWORD attribute,
                                              PVOID value, DWORD size)
{
    if (!value || !size)
        return (LONG)0x80070057; /* E_INVALIDARG */

    if (attribute == 9 && size >= sizeof(RECT)) /* EXTENDED_FRAME_BOUNDS */
        return GetWindowRect(window, (LPRECT)value) ? 0 : (LONG)0x80070057;

    if (attribute == 1 && size >= sizeof(BOOL)) { /* NCRENDERING_ENABLED */
        *(BOOL *)value = FALSE;
        return 0;
    }
    if (attribute == 14 && size >= sizeof(DWORD)) { /* CLOAKED */
        *(DWORD *)value = 0;
        return 0;
    }
    return (LONG)0x80004001; /* E_NOTIMPL */
}

static LONG WINAPI DwmIsCompositionEnabled_u32(BOOL *enabled)
{
    if (!enabled)
        return (LONG)0x80070057; /* E_INVALIDARG */

    /* OsitoK owns composition, but it does not implement the Windows DWM
     * contract.  Report that capability as unavailable so clients select
     * their documented non-DWM rendering path. */
    *enabled = FALSE;
    return 0;
}

static LONG WINAPI DwmSetWindowAttribute_u32(HWND window, DWORD attribute,
                                              PCVOID value, DWORD size)
{
    (void)window;
    (void)attribute;
    (void)value;
    (void)size;
    return 0;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct {
    HWND wanted[8];
    int wanted_count;
    uint32_t seen;
    int calls;
} WM_TEST_ENUM_CONTEXT;

static unsigned wm_test_destroy_count;
static unsigned wm_test_ncdestroy_count;
static unsigned wm_test_setfocus_count;
static unsigned wm_test_killfocus_count;
static unsigned wm_test_capturechanged_count;
static HWND wm_test_capturechanged_window;
static HWND wm_test_capturechanged_new;
static BOOL wm_test_caption_hit;
static unsigned wm_test_nclbuttondown_count;
static unsigned wm_test_entersizemove_count;
static unsigned wm_test_exitsizemove_count;
static unsigned wm_test_timer_count;
static HWND wm_test_timer_window;
static UINT wm_test_timer_message;
static ULONG_PTR wm_test_timer_id;
static DWORD wm_test_timer_time;

static void WINAPI wm_test_timer_proc(HWND window, UINT message,
                                      ULONG_PTR event_id, DWORD time)
{
    wm_test_timer_count++;
    wm_test_timer_window = window;
    wm_test_timer_message = message;
    wm_test_timer_id = event_id;
    wm_test_timer_time = time;
}

static LRESULT WINAPI wm_test_wndproc(HWND window, DWORD message,
                                       WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST && wm_test_caption_hit)
        return HTCAPTION;
    if (message == WM_DESTROY)
        wm_test_destroy_count++;
    else if (message == WM_NCDESTROY)
        wm_test_ncdestroy_count++;
    else if (message == WM_SETFOCUS)
        wm_test_setfocus_count++;
    else if (message == WM_KILLFOCUS)
        wm_test_killfocus_count++;
    else if (message == WM_CAPTURECHANGED) {
        wm_test_capturechanged_count++;
        wm_test_capturechanged_window = window;
        wm_test_capturechanged_new = (HWND)(ULONG_PTR)lparam;
    } else if (message == WM_NCLBUTTONDOWN)
        wm_test_nclbuttondown_count++;
    else if (message == WM_ENTERSIZEMOVE)
        wm_test_entersizemove_count++;
    else if (message == WM_EXITSIZEMOVE)
        wm_test_exitsizemove_count++;
    return DefWindowProcA(window, message, wparam, lparam);
}

static LRESULT WINAPI wm_test_previous_wndproc(HWND window, DWORD message,
                                                WPARAM wparam, LPARAM lparam)
{
    if (window && message == WM_USER + 73 &&
        wparam == (WPARAM)0x1234 && lparam == (LPARAM)0x5678)
        return (LRESULT)0x6A09;
    return (LRESULT)-1;
}

static BOOL WINAPI wm_test_enum_proc(HWND window, LPARAM parameter)
{
    WM_TEST_ENUM_CONTEXT *context =
        (WM_TEST_ENUM_CONTEXT *)(ULONG_PTR)parameter;
    if (!context)
        return FALSE;
    context->calls++;
    for (int i = 0; i < context->wanted_count; i++)
        if (context->wanted[i] == window)
            context->seen |= 1U << i;
    return TRUE;
}

static void wm_test_expect(BOOL condition, const char *name,
                           int *checks, int *failures)
{
    (*checks)++;
    if (condition)
        return;
    (*failures)++;
    serial_puts("[WMTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

static BOOL wm_test_rect_is(const RECT *rect, int left, int top,
                            int right, int bottom)
{
    return rect && rect->left == left && rect->top == top &&
           rect->right == right && rect->bottom == bottom;
}

static BOOL wm_test_wide_is(PCWSTR text, const char *ascii)
{
    if (!text || !ascii) return FALSE;
    while (*ascii && *text == (WCHAR)(BYTE)*ascii) {
        text++;
        ascii++;
    }
    return *text == 0 && *ascii == 0;
}

int user32_window_model_selftest(void)
{
    enum { WM_TEST_CLASS_BURST = 96 };
    static const char class_name[] = "OsitoWindowModelTest";
    static const WCHAR listbox_class_w[] = {
        'L', 'I', 'S', 'T', 'B', 'O', 'X', 0
    };
    static const WCHAR property_name_w[] = {
        'O', 's', 'i', 't', 'o', '.', 'P', 'r', 'o', 'p', 0
    };
    WNDCLASSA window_class = {
        .style = 0x0003,
        .lpfnWndProc = wm_test_wndproc,
        .cbClsExtra = 4,
        .cbWndExtra = 8,
        .hInstance = (HINSTANCE)(ULONG_PTR)0x12345678,
        .lpszClassName = class_name,
    };
    WNDCLASSEXA class_info = {0};
    WNDCLASSEXA system_class_info = {0};
    WNDCLASSEXA system_atom_info = {0};
    WNDCLASSA system_basic_info = {0};
    uint32_t system_class_info32[12] = {48};
    HWND parent = NULL, child_a = NULL, child_b = NULL, grandchild = NULL;
    HWND grandchild_b = NULL;
    HWND popup_a = NULL, popup_b = NULL, other = NULL, transient = NULL;
    HWND directdraw_window = NULL;
    HMENU popup_menu = NULL, popup_submenu = NULL;
    HMENU system_menu = NULL, reset_system_menu = NULL;
    int checks = 0, failures = 0;
    WORD atom = 0;
    WORD burst_atoms[WM_TEST_CLASS_BURST] = {0};
    char burst_names[WM_TEST_CLASS_BURST][32];
    char formatted_name[32];

    serial_puts("[WMTEST] starting USER32 window-model test\n");
    if (g_compat32_mode) {
        serial_puts("[WMTEST] FAIL: cannot run during a PE32 callback\n");
        return 1;
    }

    int formatted_length = wsprintfA_k32(
        formatted_name, "pid:%000008X", (ULONG_PTR)0x2A,
        0, 0, 0, 0, 0, 0, 0, 0, 0);
    wm_test_expect(formatted_length == 12 &&
                   u32_strcmp(formatted_name, "pid:0000002A") == 0,
                   "wsprintfA cdecl formatting", &checks, &failures);
    wm_test_expect(GetSystemMetrics(SM_XVIRTUALSCREEN) == 0 &&
                   GetSystemMetrics(SM_YVIRTUALSCREEN) == 0 &&
                   GetSystemMetrics(SM_CXVIRTUALSCREEN) ==
                       GetSystemMetrics(SM_CXSCREEN) &&
                   GetSystemMetrics(SM_CYVIRTUALSCREEN) ==
                       GetSystemMetrics(SM_CYSCREEN) &&
                   GetSystemMetrics(SM_CMONITORS) == 1 &&
                   GetSystemMetrics(SM_CXVIRTUALSCREEN) > 0 &&
                   GetSystemMetrics(SM_CYVIRTUALSCREEN) > 0,
                   "virtual-screen metrics", &checks, &failures);
    wm_test_expect(GetSystemMetrics(SM_CXCURSOR) == 32 &&
                   GetSystemMetrics(SM_CYCURSOR) == 32 &&
                   GetSystemMetrics(SM_MOUSEPRESENT) == 1 &&
                   GetSystemMetrics(SM_CMOUSEBUTTONS) == 3 &&
                   GetSystemMetrics(SM_MOUSEWHEELPRESENT) == 1 &&
                   GetSystemMetricsForDpi(SM_CXCURSOR, 192) == 64,
                   "mouse and cursor metrics", &checks, &failures);

    system_class_info.cbSize = sizeof(system_class_info);
    wm_test_expect(GetClassInfoExW(NULL, listbox_class_w,
                                  &system_class_info) &&
                   system_class_info.style == 0x00000088 &&
                   system_class_info.lpfnWndProc != NULL &&
                   system_class_info.cbClsExtra == 0 &&
                   system_class_info.cbWndExtra == 8 &&
                   system_class_info.hInstance == NULL &&
                   (ULONG_PTR)system_class_info.lpszClassName ==
                       (ULONG_PTR)listbox_class_w,
                   "predefined LISTBOX class metadata",
                   &checks, &failures);

    system_atom_info.cbSize = sizeof(system_atom_info);
    wm_test_expect(GetClassInfoExA(NULL, (PCSTR)(ULONG_PTR)0x0083,
                                  &system_atom_info) &&
                   system_atom_info.style == 0x00000088 &&
                   system_atom_info.lpfnWndProc != NULL &&
                   (ULONG_PTR)system_atom_info.lpszClassName == 0x0083,
                   "predefined LISTBOX class atom",
                   &checks, &failures);

    wm_test_expect(GetClassInfoA_k32(NULL, "LISTBOX",
                                    &system_basic_info) &&
                   system_basic_info.style == 0x00000088 &&
                   system_basic_info.lpfnWndProc != NULL &&
                   system_basic_info.cbWndExtra == 8 &&
                   system_basic_info.hInstance == NULL,
                   "GetClassInfoA system-class layout",
                   &checks, &failures);

    if (!compat32_is_initialized())
        compat32_init();
    g_compat32_mode = 1;
    BOOL compat_system_class = GetClassInfoExW(
        NULL, listbox_class_w, system_class_info32);
    g_compat32_mode = 0;
    wm_test_expect(compat_system_class && system_class_info32[0] == 48 &&
                   system_class_info32[1] == 0x00000088 &&
                   system_class_info32[2] != 0 &&
                   system_class_info32[3] == 0 &&
                   system_class_info32[4] == 8 &&
                   system_class_info32[5] == 0,
                   "PE32 WNDCLASSEX system-class layout and thunk",
                   &checks, &failures);

    SetLastError(0);
    HWND invalid_class_window = CreateWindowExA(
        0, "OsitoMissingWindowClass", "invalid", 0,
        0, 0, 32, 32, NULL, NULL, NULL, NULL);
    wm_test_expect(!invalid_class_window && GetLastError() == 1407,
                   "unknown class rejected without WndProc fallback",
                   &checks, &failures);

    wm_test_destroy_count = 0;
    wm_test_ncdestroy_count = 0;
    atom = RegisterClassA(&window_class);
    wm_test_expect(atom != 0, "RegisterClassA", &checks, &failures);
    if (!atom)
        goto cleanup;

    SetLastError(0);
    wm_test_expect(RegisterClassA(&window_class) == 0 &&
                   GetLastError() == 1410,
                   "duplicate class reports ERROR_CLASS_ALREADY_EXISTS",
                   &checks, &failures);

    int burst_registered = 0;
    for (int i = 0; i < WM_TEST_CLASS_BURST; i++) {
        WNDCLASSA burst_class = window_class;
        wsprintfA_k32(burst_names[i], "OsitoClassBurst%u",
                      (ULONG_PTR)i, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        burst_class.lpszClassName = burst_names[i];
        burst_atoms[i] = RegisterClassA(&burst_class);
        if (!burst_atoms[i])
            break;
        burst_registered++;
    }
    wm_test_expect(burst_registered == WM_TEST_CLASS_BURST,
                   "class registry grows beyond one 64-entry block",
                   &checks, &failures);
    for (int i = 0; i < burst_registered; i++)
        UnregisterClassA((PCSTR)(ULONG_PTR)burst_atoms[i],
                         window_class.hInstance);

    class_info.cbSize = sizeof(class_info);
    wm_test_expect(GetClassInfoExA(window_class.hInstance, class_name,
                                  &class_info) &&
                   class_info.cbSize == sizeof(class_info) &&
                   class_info.style == window_class.style &&
                   class_info.lpfnWndProc == window_class.lpfnWndProc &&
                   class_info.cbClsExtra == window_class.cbClsExtra &&
                   class_info.cbWndExtra == window_class.cbWndExtra &&
                   class_info.hInstance == window_class.hInstance &&
                   class_info.lpszClassName == window_class.lpszClassName,
                   "GetClassInfoExA preserves PE64 class metadata",
                   &checks, &failures);

    parent = CreateWindowExA(0, class_name, "wm-parent",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 80, 320, 240,
        NULL, NULL, NULL, NULL);
    child_a = CreateWindowExA(0, class_name, "wm-child-a",
        WS_CHILD | WS_VISIBLE, 10, 20, 80, 60,
        parent, (HMENU)(ULONG_PTR)101, NULL, NULL);
    child_b = CreateWindowExA(0, class_name, "wm-child-b",
        WS_CHILD | WS_VISIBLE, 30, 40, 70, 50,
        parent, (HMENU)(ULONG_PTR)102, NULL, NULL);
    grandchild = CreateWindowExA(0, class_name, "wm-grandchild",
        WS_CHILD | WS_VISIBLE, 5, 6, 20, 10,
        child_a, (HMENU)(ULONG_PTR)201, NULL, NULL);
    popup_a = CreateWindowExA(0, class_name, "wm-popup-a",
        WS_POPUP | WS_VISIBLE, 200, 160, 90, 70,
        parent, NULL, NULL, NULL);
    popup_b = CreateWindowExA(0, class_name, "wm-popup-b",
        WS_POPUP | WS_VISIBLE, 220, 180, 80, 60,
        parent, NULL, NULL, NULL);
    other = CreateWindowExA(0, class_name, "wm-other",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 400, 100, 120, 90,
        NULL, NULL, NULL, NULL);

    wm_test_expect(parent && child_a && child_b && grandchild &&
                   popup_a && popup_b && other,
                   "create hierarchy", &checks, &failures);
    if (!parent || !child_a || !child_b || !grandchild ||
        !popup_a || !popup_b || !other)
        goto cleanup;

    wm_test_expect(CallWindowProcW((PVOID)wm_test_previous_wndproc,
                                   parent, WM_USER + 73,
                                   (WPARAM)0x1234, (LPARAM)0x5678) ==
                       (LRESULT)0x6A09,
                   "CallWindowProcW invokes previous native WndProc",
                   &checks, &failures);

    HANDLE first_property =
        (HANDLE)(ULONG_PTR)0x1122334455667788ULL;
    HANDLE second_property =
        (HANDLE)(ULONG_PTR)0x8877665544332211ULL;
    wm_test_expect(SetPropA(parent, "Osito.Prop", first_property) &&
                   GetPropW(parent, property_name_w) == first_property,
                   "window property A/W lookup", &checks, &failures);
    wm_test_expect(SetPropW(parent, property_name_w, second_property) &&
                   GetPropA(parent, "osito.prop") == second_property,
                   "window property update", &checks, &failures);
    wm_test_expect(RemovePropA(parent, "OSITO.PROP") == second_property &&
                   GetPropW(parent, property_name_w) == NULL,
                   "window property removal", &checks, &failures);
    PCSTR atom_property = (PCSTR)(ULONG_PTR)0x4321;
    wm_test_expect(SetPropW(parent, (PCWSTR)atom_property, first_property) &&
                   GetPropA(parent, atom_property) == first_property &&
                   RemovePropW(parent, (PCWSTR)atom_property) == first_property,
                   "atom window property", &checks, &failures);

    MSG test_message = {0};
    DWORD child_message = WM_USER + 0x31;
    wm_test_expect(PostMessageA(grandchild, child_message,
                                (WPARAM)0x1234, (LPARAM)0x5678) &&
                   GetMessageA(&test_message, parent,
                               child_message, child_message) == TRUE &&
                   test_message.hwnd == grandchild &&
                   test_message.message == child_message &&
                   test_message.wParam == (WPARAM)0x1234 &&
                   test_message.lParam == (LPARAM)0x5678,
                   "GetMessage child-window filtering", &checks, &failures);
    wm_test_expect(GetMessageA(&test_message,
                               (HWND)(ULONG_PTR)0xDEADBEEFU,
                               child_message, child_message) == (BOOL)-1,
                   "GetMessage invalid HWND", &checks, &failures);

    PostQuitMessage(23);
    wm_test_expect(GetMessageA(&test_message, NULL,
                               child_message, child_message) == FALSE &&
                   test_message.message == WM_QUIT &&
                   test_message.wParam == (WPARAM)23,
                   "GetMessage WM_QUIT contract", &checks, &failures);

    ULONG_PTR window_timer = SetTimer(parent, 77, 1000, NULL);
    wm_test_expect(window_timer == 77 &&
                   SetTimer(parent, 77, 1, NULL) == 77,
                   "SetTimer replaces window timer", &checks, &failures);
    DWORD timer_wait_start = shim_timeGetTime();
    wm_test_expect(GetMessageA(&test_message, parent,
                               WM_TIMER, WM_TIMER) == TRUE &&
                   test_message.hwnd == parent &&
                   test_message.message == WM_TIMER &&
                   test_message.wParam == (WPARAM)77 &&
                   test_message.lParam == 0 &&
                   (DWORD)(test_message.time - timer_wait_start) < 1000,
                   "window WM_TIMER generation", &checks, &failures);
    wm_test_expect(KillTimer(parent, 77) && !KillTimer(parent, 77),
                   "KillTimer ownership and result", &checks, &failures);

    wm_test_timer_count = 0;
    wm_test_timer_window = (HWND)(ULONG_PTR)1;
    wm_test_timer_message = 0;
    wm_test_timer_id = 0;
    wm_test_timer_time = 0;
    ULONG_PTR callback_timer = SetTimer(NULL, 0, 1,
                                        (PVOID)wm_test_timer_proc);
    wm_test_expect(callback_timer != 0 &&
                   GetMessageA(&test_message, (HWND)(ULONG_PTR)-1,
                               WM_TIMER, WM_TIMER) == TRUE &&
                   test_message.hwnd == NULL &&
                   test_message.wParam == callback_timer &&
                   test_message.lParam ==
                       (LPARAM)(ULONG_PTR)wm_test_timer_proc,
                   "thread timer generated ID and message", &checks,
                   &failures);
    DispatchMessageA(&test_message);
    wm_test_expect(wm_test_timer_count == 1 &&
                   wm_test_timer_window == NULL &&
                   wm_test_timer_message == WM_TIMER &&
                   wm_test_timer_id == callback_timer &&
                   wm_test_timer_time == test_message.time,
                   "DispatchMessage TIMERPROC callback", &checks, &failures);
    wm_test_expect(KillTimer(NULL, callback_timer),
                   "KillTimer thread timer", &checks, &failures);
    DWORD no_timer_wait = 0;
    wm_test_expect(!user_timer_next_timeout(NULL, WM_TIMER, WM_TIMER,
                                            &no_timer_wait),
                   "no fabricated WM_TIMER after cancellation", &checks,
                   &failures);
    ULONG_PTR zero_window_timer = SetTimer(
        parent, 0, 1, (PVOID)wm_test_timer_proc);
    wm_test_expect(zero_window_timer == 1,
                   "window timer accepts ID zero", &checks, &failures);
    DWORD zero_timer_start = shim_timeGetTime();
    while ((DWORD)(shim_timeGetTime() - zero_timer_start) <
           USER_TIMER_MINIMUM) {
        extern void sched_yield(void);
        sched_yield();
    }
    wm_test_expect(PeekMessageA(&test_message, parent,
                                WM_TIMER, WM_TIMER, PM_NOREMOVE) == TRUE &&
                   test_message.hwnd == parent &&
                   test_message.message == WM_TIMER &&
                   test_message.wParam == 0 &&
                   test_message.lParam ==
                       (LPARAM)(ULONG_PTR)wm_test_timer_proc,
                   "window timer ID zero PM_NOREMOVE", &checks, &failures);
    DWORD posted_timer_time = test_message.time;
    wm_test_expect(SetTimer(parent, 0, 1000, NULL) == 1 &&
                   KillTimer(parent, 0) && !KillTimer(parent, 0),
                   "replace and cancel posted window timer", &checks,
                   &failures);
    wm_test_expect(PeekMessageA(&test_message, parent,
                                WM_TIMER, WM_TIMER, PM_REMOVE) == TRUE &&
                   test_message.hwnd == parent &&
                   test_message.wParam == 0 &&
                   test_message.lParam ==
                       (LPARAM)(ULONG_PTR)wm_test_timer_proc &&
                   test_message.time == posted_timer_time &&
                   !PeekMessageA(&test_message, parent,
                                 WM_TIMER, WM_TIMER, PM_REMOVE),
                   "posted WM_TIMER survives replacement and KillTimer",
                   &checks, &failures);

    wm_test_expect(GetParent(parent) == NULL, "top-level parent", &checks,
                   &failures);
    wm_test_expect(GetParent(child_a) == parent, "child parent", &checks,
                   &failures);
    wm_test_expect(GetParent(popup_a) == parent, "popup owner via GetParent",
                   &checks, &failures);
    wm_test_expect(GetWindow(popup_a, GW_OWNER) == parent, "GW_OWNER",
                   &checks, &failures);
    wm_test_expect(GetAncestor(grandchild, GA_ROOT) == parent, "GA_ROOT",
                   &checks, &failures);
    wm_test_expect(GetDlgItem(parent, 102) == child_b, "control id lookup",
                   &checks, &failures);
    wm_test_expect(GetDlgCtrlID(grandchild) == 201, "GetDlgCtrlID",
                   &checks, &failures);

    system_menu = GetSystemMenu(parent, FALSE);
    wm_test_expect(system_menu && IsMenu(system_menu) &&
                   GetSystemMenu(parent, FALSE) == system_menu,
                   "stable system-menu copy", &checks, &failures);
    wm_test_expect(GetMenuItemCount(system_menu) == 7 &&
                   GetMenuDefaultItem(system_menu, FALSE,
                                      GMDI_USEDISABLED) == SC_CLOSE,
                   "default system-menu contents", &checks, &failures);
    wm_test_expect((GetMenuState(system_menu, SC_RESTORE, MF_BYCOMMAND) &
                    MFS_DISABLED) == MFS_DISABLED,
                   "restore initially disabled", &checks, &failures);
    wm_test_expect(EnableMenuItem(system_menu, SC_CLOSE,
                                 MF_BYCOMMAND | MF_GRAYED) == MF_ENABLED &&
                   (GetMenuState(system_menu, SC_CLOSE, MF_BYCOMMAND) &
                    MFS_DISABLED) == MFS_DISABLED,
                   "system-menu state mutation", &checks, &failures);

    WCHAR close_text[16] = {0};
    MENUITEMINFOW close_info = {
        .cbSize = sizeof(MENUITEMINFOW),
        .fMask = MIIM_ID | MIIM_STATE | MIIM_STRING,
        .dwTypeData = close_text,
        .cch = 16,
    };
    wm_test_expect(GetMenuItemInfoW(system_menu, SC_CLOSE, FALSE,
                                    &close_info) &&
                   close_info.wID == SC_CLOSE &&
                   (close_info.fState & MFS_DISABLED) &&
                   wm_test_wide_is(close_text, "&Close"),
                   "GetMenuItemInfoW system command", &checks, &failures);

    wm_test_expect(GetSystemMenu(parent, TRUE) == NULL &&
                   !IsMenu(system_menu),
                   "GetSystemMenu revert destroys copy", &checks, &failures);
    reset_system_menu = GetSystemMenu(parent, FALSE);
    wm_test_expect(reset_system_menu && reset_system_menu != system_menu &&
                   (GetMenuState(reset_system_menu, SC_CLOSE, MF_BYCOMMAND) &
                    MFS_DISABLED) == 0,
                   "GetSystemMenu rebuilds defaults", &checks, &failures);

    popup_menu = CreatePopupMenu();
    popup_submenu = CreatePopupMenu();
    WCHAR open_text[] = { 'O', 'p', 'e', 'n', 0 };
    WCHAR child_text[] = { 'C', 'h', 'i', 'l', 'd', 0 };
    WCHAR more_text[] = { 'M', 'o', 'r', 'e', 0 };
    MENUITEMINFOW popup_item = {
        .cbSize = sizeof(MENUITEMINFOW),
        .fMask = MIIM_ID | MIIM_STRING | MIIM_DATA,
        .wID = 42,
        .dwItemData = 0x1234,
        .dwTypeData = open_text,
    };
    MENUITEMINFOW child_item = {
        .cbSize = sizeof(MENUITEMINFOW),
        .fMask = MIIM_ID | MIIM_STRING,
        .wID = 43,
        .dwTypeData = child_text,
    };
    MENUITEMINFOW submenu_item = {
        .cbSize = sizeof(MENUITEMINFOW),
        .fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU,
        .wID = 100,
        .hSubMenu = popup_submenu,
        .dwTypeData = more_text,
    };
    wm_test_expect(popup_menu && popup_submenu &&
                   InsertMenuItemW(popup_menu, (UINT)-1, TRUE, &popup_item) &&
                   InsertMenuItemW(popup_submenu, (UINT)-1, TRUE, &child_item) &&
                   InsertMenuItemW(popup_menu, (UINT)-1, TRUE, &submenu_item),
                   "popup-menu insertion", &checks, &failures);
    wm_test_expect(GetMenuItemCount(popup_menu) == 2 &&
                   GetSubMenu(popup_menu, 1) == popup_submenu &&
                   GetMenuState(popup_menu, 43, MF_BYCOMMAND) != (UINT)-1,
                   "submenu hierarchy and recursive lookup", &checks,
                   &failures);

    WCHAR queried_text[16] = {0};
    MENUITEMINFOW queried_item = {
        .cbSize = sizeof(MENUITEMINFOW),
        .fMask = MIIM_ID | MIIM_STRING | MIIM_DATA,
        .dwTypeData = queried_text,
        .cch = 16,
    };
    wm_test_expect(GetMenuItemInfoW(popup_menu, 42, FALSE, &queried_item) &&
                   queried_item.wID == 42 && queried_item.dwItemData == 0x1234 &&
                   wm_test_wide_is(queried_text, "Open"),
                   "popup item round trip", &checks, &failures);
    wm_test_expect(SetMenuDefaultItem(popup_menu, 42, FALSE) &&
                   GetMenuDefaultItem(popup_menu, FALSE, 0) == 42,
                   "default popup item", &checks, &failures);

    MENUINFO set_info = {
        .cbSize = sizeof(MENUINFO),
        .fMask = MIM_STYLE | MIM_MAXHEIGHT | MIM_MENUDATA |
                 MIM_APPLYTOSUBMENUS,
        .dwStyle = 0x20000000,
        .cyMax = 360,
        .dwMenuData = 0xABCD,
    };
    MENUINFO get_info = {
        .cbSize = sizeof(MENUINFO),
        .fMask = MIM_STYLE | MIM_MAXHEIGHT | MIM_MENUDATA,
    };
    MENUINFO child_get_info = {
        .cbSize = sizeof(MENUINFO),
        .fMask = MIM_STYLE | MIM_MAXHEIGHT | MIM_MENUDATA,
    };
    wm_test_expect(SetMenuInfo(popup_menu, &set_info) &&
                   GetMenuInfo(popup_menu, &get_info) &&
                   GetMenuInfo(popup_submenu, &child_get_info) &&
                   get_info.dwStyle == set_info.dwStyle &&
                   get_info.cyMax == 360 && get_info.dwMenuData == 0xABCD &&
                   child_get_info.dwStyle == set_info.dwStyle &&
                   child_get_info.cyMax == 360 &&
                   child_get_info.dwMenuData == 0xABCD,
                   "MENUINFO recursive application", &checks, &failures);
    wm_test_expect(DestroyMenu(popup_menu) && !IsMenu(popup_menu) &&
                   !IsMenu(popup_submenu),
                   "DestroyMenu recursively destroys submenus", &checks,
                   &failures);
    popup_menu = popup_submenu = NULL;

    RECT rect = {0};
    wm_test_expect(GetWindowRect(child_a, &rect) &&
                   wm_test_rect_is(&rect, 110, 100, 190, 160),
                   "child screen rectangle", &checks, &failures);
    wm_test_expect(GetWindowRect(grandchild, &rect) &&
                   wm_test_rect_is(&rect, 115, 106, 135, 116),
                   "nested child screen rectangle", &checks, &failures);
    POINT origin = {0, 0};
    MapWindowPoints(child_a, NULL, &origin, 1);
    wm_test_expect(origin.x == 110 && origin.y == 100,
                   "MapWindowPoints to screen", &checks, &failures);

    WM_TEST_ENUM_CONTEXT child_enum = {
        .wanted = { child_a, child_b, grandchild, popup_a },
        .wanted_count = 4,
    };
    wm_test_expect(EnumChildWindows(parent, wm_test_enum_proc,
                                    (LPARAM)(ULONG_PTR)&child_enum),
                   "EnumChildWindows return", &checks, &failures);
    wm_test_expect((child_enum.seen & 0x7U) == 0x7U &&
                   !(child_enum.seen & 0x8U),
                   "EnumChildWindows descendants only", &checks, &failures);

    WM_TEST_ENUM_CONTEXT top_enum = {
        .wanted = { parent, popup_a, popup_b, other, child_a },
        .wanted_count = 5,
    };
    wm_test_expect(EnumWindows(wm_test_enum_proc,
                               (LPARAM)(ULONG_PTR)&top_enum),
                   "EnumWindows return", &checks, &failures);
    wm_test_expect((top_enum.seen & 0x0FU) == 0x0FU &&
                   !(top_enum.seen & 0x10U),
                   "EnumWindows top-level only", &checks, &failures);
    wm_test_expect(FindWindowExA(GetDesktopWindow(), NULL, class_name,
                                "wm-parent") == parent,
                   "FindWindowExA desktop scope", &checks, &failures);

    wm_test_expect(BringWindowToTop(parent), "BringWindowToTop", &checks,
                   &failures);
    wm_test_expect(GetWindow(popup_b, GW_HWNDNEXT) == popup_a &&
                   GetWindow(popup_a, GW_HWNDNEXT) == parent,
                   "owned popup z-order", &checks, &failures);

    HDWP deferred = BeginDeferWindowPos(2);
    wm_test_expect(deferred != NULL, "BeginDeferWindowPos", &checks,
                   &failures);
    if (deferred) {
        deferred = DeferWindowPos(deferred, parent, HWND_TOP,
                                  140, 110, 0, 0,
                                  SWP_NOSIZE | SWP_NOZORDER |
                                  SWP_NOACTIVATE);
        if (deferred)
            deferred = DeferWindowPos(deferred, child_a, HWND_TOP,
                                      20, 25, 0, 0,
                                      SWP_NOSIZE | SWP_NOZORDER |
                                      SWP_NOACTIVATE);
        wm_test_expect(deferred && EndDeferWindowPos(deferred),
                       "deferred move transaction", &checks, &failures);
    }
    wm_test_expect(GetWindowRect(parent, &rect) &&
                   wm_test_rect_is(&rect, 140, 110, 460, 350),
                   "deferred parent geometry", &checks, &failures);
    wm_test_expect(GetWindowRect(grandchild, &rect) &&
                   wm_test_rect_is(&rect, 165, 141, 185, 151),
                   "descendant follows deferred move", &checks, &failures);
    POINT child_hit = { 166, 142 };
    wm_test_expect(WindowFromPoint(child_hit) == grandchild,
                   "WindowFromPoint selects deepest visible child",
                   &checks, &failures);

    grandchild_b = CreateWindowExA(0, class_name, "wm-grandchild-b",
        WS_CHILD | WS_VISIBLE, -5, -9, 20, 10,
        child_b, (HMENU)(ULONG_PTR)202, NULL, NULL);
    wm_test_expect(grandchild_b != NULL,
                   "create overlapping sibling-branch child",
                   &checks, &failures);
    if (grandchild_b) {
        wm_test_expect(SetWindowPos(child_b, HWND_TOP, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE |
                                        SWP_NOACTIVATE),
                       "raise overlapping sibling branch",
                       &checks, &failures);
        WINDOW *grandchild_window = find_window(grandchild);
        WINDOW *grandchild_b_window = find_window(grandchild_b);
        wm_test_expect(grandchild_window && grandchild_b_window &&
                       grandchild_window->render_z !=
                           grandchild_b_window->render_z,
                       "cross-branch render z-order is unique",
                       &checks, &failures);
        wm_test_expect(WindowFromPoint(child_hit) == grandchild_b,
                       "WindowFromPoint honors higher sibling subtree",
                       &checks, &failures);
        if (DestroyWindow(grandchild_b))
            grandchild_b = NULL;
        else
            wm_test_expect(FALSE, "destroy sibling-branch child",
                           &checks, &failures);
    }
    wm_test_destroy_count = 0;
    wm_test_ncdestroy_count = 0;

    wm_test_setfocus_count = 0;
    wm_test_killfocus_count = 0;
    SetFocus(child_a);
    wm_test_expect(wm_test_setfocus_count == 1 &&
                   wm_test_killfocus_count == 1,
                   "SetFocus messages are synchronous", &checks, &failures);
    wm_test_expect(GetFocus() == child_a &&
                   GetActiveWindow() == parent &&
                   GetForegroundWindow() == parent,
                   "focus child with active top-level", &checks, &failures);
    ShowWindow(parent, SW_SHOW);
    wm_test_expect(GetFocus() == child_a,
                   "top-level activation preserves child focus", &checks,
                   &failures);
    SetActiveWindow(parent);
    SetForegroundWindow(parent);
    BringWindowToTop(parent);
    wm_test_expect(GetFocus() == child_a &&
                   GetActiveWindow() == parent &&
                   GetForegroundWindow() == parent,
                   "foreground APIs preserve child focus", &checks,
                   &failures);

    ShowWindow(child_a, SW_HIDE);
    wm_test_expect(GetFocus() == parent && GetActiveWindow() == parent &&
                   input_target() == parent,
                   "hidden focused child repairs input target", &checks,
                   &failures);
    ShowWindow(child_a, SW_SHOWNA);
    SetFocus(child_a);

    ShowWindow(parent, SW_HIDE);
    wm_test_expect(!IsWindowVisible(child_a) &&
                   GetActiveWindow() == other && GetFocus() == other &&
                   input_target() == other,
                   "hidden active hierarchy transfers foreground", &checks,
                   &failures);
    ShowWindow(parent, SW_SHOWNA);
    wm_test_expect(IsWindowVisible(grandchild) &&
                   GetActiveWindow() == other && GetFocus() == other,
                   "nonactivating show preserves foreground", &checks,
                   &failures);

    WINDOWPLACEMENT placement = { .length = sizeof(WINDOWPLACEMENT) };
    wm_test_expect(GetWindowPlacement(parent, &placement) &&
                   wm_test_rect_is(&placement.rcNormalPosition,
                                   140, 110, 460, 350),
                   "GetWindowPlacement normal rectangle", &checks,
                   &failures);
    WINDOWPLACEMENT invalid_placement = {0};
    wm_test_expect(!GetWindowPlacement(parent, &invalid_placement),
                   "WINDOWPLACEMENT length validation", &checks, &failures);
    placement.showCmd = SW_SHOWMINIMIZED;
    wm_test_expect(SetWindowPlacement(parent, &placement) && IsIconic(parent),
                   "SetWindowPlacement minimize", &checks, &failures);
    placement.showCmd = SW_RESTORE;
    wm_test_expect(SetWindowPlacement(parent, &placement) && !IsIconic(parent),
                   "SetWindowPlacement restore", &checks, &failures);

    directdraw_window = CreateWindowExA(0, class_name, "wm-ddraw",
        WS_POPUP | WS_VISIBLE, 40, 50, 320, 200,
        NULL, NULL, NULL, NULL);
    wm_test_expect(directdraw_window != NULL,
                   "create DirectDraw cooperative window", &checks,
                   &failures);
    if (directdraw_window) {
        WINDOW *directdraw = find_window(directdraw_window);
        wm_test_expect(
            user32_configure_directdraw_window(directdraw_window, TRUE,
                                               FALSE, 0, 0) &&
            directdraw && directdraw->directdraw_exclusive &&
            directdraw->x == 40 && directdraw->y == 50 &&
            directdraw->width == 320 && directdraw->height == 200 &&
            window_should_be_fullscreen(directdraw),
            "exclusive DirectDraw mode without geometry change",
            &checks, &failures);
        wm_test_expect(
            user32_configure_directdraw_window(directdraw_window, TRUE,
                                               TRUE, 640, 480) &&
            directdraw->x == 0 && directdraw->y == 0 &&
            directdraw->width == 640 && directdraw->height == 480 &&
            window_should_be_fullscreen(directdraw),
            "exclusive DirectDraw mode updates viewport geometry",
            &checks, &failures);
        wm_test_expect(
            user32_configure_directdraw_window(directdraw_window, FALSE,
                                               FALSE, 0, 0) &&
            !directdraw->directdraw_exclusive &&
            !window_should_be_fullscreen(directdraw),
            "normal DirectDraw mode releases fullscreen ownership",
            &checks, &failures);
        if (DestroyWindow(directdraw_window))
            directdraw_window = NULL;
        else
            wm_test_expect(FALSE, "destroy DirectDraw cooperative window",
                           &checks, &failures);
        wm_test_destroy_count = 0;
        wm_test_ncdestroy_count = 0;
    }

    transient = CreateWindowExA(0, class_name, "wm-thread-transient",
        WS_POPUP | WS_VISIBLE, 260, 220, 80, 50,
        parent, NULL, NULL, NULL);
    wm_test_expect(transient != NULL && GetActiveWindow() == transient,
                   "visible owned popup becomes active", &checks, &failures);
    if (transient) {
        WINDOW *transient_window = find_window(transient);
        DWORD fake_tid = GetCurrentThreadId() ^ 0x40000000U;
        if (transient_window)
            transient_window->owner_tid = fake_tid;
        user32_release_thread(GetCurrentProcessId(), fake_tid);
        wm_test_expect(!IsWindow(transient) &&
                       GetActiveWindow() == parent && GetFocus() == parent &&
                       input_target() == parent,
                       "thread teardown restores owned-window foreground",
                       &checks, &failures);
        transient = NULL;
    }

    wm_test_expect(SetParent(child_b, NULL) == parent,
                   "SetParent previous parent", &checks, &failures);
    wm_test_expect(GetParent(child_b) == GetDesktopWindow(),
                   "SetParent desktop relation", &checks, &failures);
    wm_test_expect(GetWindowRect(child_b, &rect) &&
                   wm_test_rect_is(&rect, 30, 40, 100, 90),
                   "SetParent preserves local coordinates", &checks,
                   &failures);

    wm_test_expect(DestroyWindow(parent), "DestroyWindow owner tree", &checks,
                   &failures);
    wm_test_expect(!IsWindow(parent) && !IsWindow(child_a) &&
                   !IsWindow(grandchild) && !IsWindow(popup_a) &&
                   !IsWindow(popup_b) && IsWindow(child_b),
                   "recursive child and owned-window destruction", &checks,
                   &failures);
    wm_test_expect(!IsMenu(reset_system_menu),
                   "window destruction releases system menu", &checks,
                   &failures);
    wm_test_expect(wm_test_destroy_count == 5 &&
                   wm_test_ncdestroy_count == 5,
                   "WM_DESTROY/WM_NCDESTROY delivery", &checks, &failures);

cleanup:
    if (IsMenu(popup_menu)) DestroyMenu(popup_menu);
    if (IsMenu(popup_submenu)) DestroyMenu(popup_submenu);
    if (IsWindow(parent)) DestroyWindow(parent);
    if (IsWindow(child_a)) DestroyWindow(child_a);
    if (IsWindow(child_b)) DestroyWindow(child_b);
    if (IsWindow(grandchild)) DestroyWindow(grandchild);
    if (IsWindow(grandchild_b)) DestroyWindow(grandchild_b);
    if (IsWindow(popup_a)) DestroyWindow(popup_a);
    if (IsWindow(popup_b)) DestroyWindow(popup_b);
    if (IsWindow(directdraw_window)) DestroyWindow(directdraw_window);
    if (IsWindow(transient)) DestroyWindow(transient);
    if (IsWindow(other)) DestroyWindow(other);
    if (atom) UnregisterClassA(class_name, NULL);

    serial_puts("[WMTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

static void input_test_expect(BOOL condition, const char *name,
                              int *checks, int *failures)
{
    (*checks)++;
    if (condition)
        return;
    (*failures)++;
    serial_puts("[INPUTTEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

static BOOL input_test_take(HWND window, DWORD minimum, DWORD maximum,
                            MSG *message)
{
    return PeekMessageA(message, window, minimum, maximum, PM_REMOVE);
}

static void input_test_write_u16(BYTE *output, WORD value)
{
    output[0] = (BYTE)value;
    output[1] = (BYTE)(value >> 8);
}

static void input_test_write_u32(BYTE *output, DWORD value)
{
    output[0] = (BYTE)value;
    output[1] = (BYTE)(value >> 8);
    output[2] = (BYTE)(value >> 16);
    output[3] = (BYTE)(value >> 24);
}

static void input_test_keyboard_messages(HWND window, int *checks, int *failures)
{
    static const struct {
        BYTE scan, extended, up;
        DWORD message, vk, bits;
    } steps[] = {
        {0x1E, 0, 0, WM_KEYDOWN, 'A',        0x001E0001},
        {0x1E, 0, 0, WM_KEYDOWN, 'A',        0x401E0001},
        {0x1E, 0, 1, WM_KEYUP,   'A',        0xC01E0001},
        {0x38, 0, 0, WM_SYSKEYDOWN, VK_MENU, 0x20380001},
        {0x3E, 0, 0, WM_SYSKEYDOWN, VK_F4,   0x203E0001},
        {0x3E, 0, 0, WM_SYSKEYDOWN, VK_F4,   0x603E0001},
        {0x3E, 0, 1, WM_SYSKEYUP, VK_F4,     0xE03E0001},
        {0x38, 0, 1, WM_KEYUP, VK_MENU,      0xC0380001},
        {0x38, 1, 0, WM_SYSKEYDOWN, VK_MENU, 0x21380001},
        {0x38, 1, 1, WM_SYSKEYUP, VK_MENU,   0xC1380001},
        {0x1D, 0, 0, WM_KEYDOWN, VK_CONTROL, 0x001D0001},
        {0x38, 0, 0, WM_KEYDOWN, VK_MENU,    0x20380001},
        {0x12, 0, 0, WM_KEYDOWN, 'E',        0x20120001},
        {0x12, 0, 1, WM_KEYUP,   'E',        0xE0120001},
        {0x1D, 0, 1, WM_SYSKEYUP, VK_CONTROL,0xE01D0001},
        {0x38, 0, 1, WM_KEYUP, VK_MENU,      0xC0380001},
        {0x44, 0, 0, WM_SYSKEYDOWN, VK_F10,  0x00440001},
        {0x44, 0, 1, WM_SYSKEYUP, VK_F10,    0xC0440001},
        {0x2A, 0, 0, WM_KEYDOWN, VK_SHIFT,   0x002A0001},
        {0x36, 0, 0, WM_KEYDOWN, VK_SHIFT,   0x00360001},
        {0x2A, 0, 1, WM_KEYUP, VK_SHIFT,     0xC02A0001},
        {0x36, 0, 1, WM_KEYUP, VK_SHIFT,     0xC0360001},
        {0x1D, 1, 0, WM_KEYDOWN, VK_CONTROL, 0x011D0001},
        {0x1D, 1, 1, WM_KEYUP, VK_CONTROL,   0xC11D0001},
    };
    for (int injected = 0; injected < 2; injected++) {
        keyboard_alt_pending = FALSE;
        for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            if (injected) {
                INPUT input = { .type = INPUT_KEYBOARD };
                input.data.ki.wScan = steps[i].scan;
                input.data.ki.dwFlags = KEYEVENTF_SCANCODE |
                    (steps[i].extended ? KEYEVENTF_EXTENDEDKEY : 0) |
                    (steps[i].up ? KEYEVENTF_KEYUP : 0);
                input_test_expect(SendInput(1, &input, sizeof(input)) == 1,
                                  "inject keyboard sequence", checks, failures);
            } else {
                if (steps[i].extended)
                    win32_post_keyboard_event(0xE0, steps[i].up);
                win32_post_keyboard_event(steps[i].scan, steps[i].up);
            }
            MSG message = { 0 };
            BOOL found = input_test_take(window, WM_KEYDOWN, WM_SYSKEYUP,
                                          &message);
            input_test_expect(found && message.message == steps[i].message &&
                              message.wParam == steps[i].vk &&
                              (DWORD)message.lParam == steps[i].bits,
                              injected ? "injected key message contract"
                                       : "physical key message contract",
                              checks, failures);
            if (i == 20)
                input_test_expect((GetKeyState(VK_SHIFT) & 0x8000) &&
                                  !(GetKeyState(VK_LSHIFT) & 0x8000) &&
                                  (GetKeyState(VK_RSHIFT) & 0x8000),
                                  "releasing left Shift preserves right Shift",
                                  checks, failures);
        }
    }
    INPUT alt = { .type = INPUT_KEYBOARD };
    alt.data.ki.wVk = VK_MENU;
    MSG message = { 0 };
    input_test_expect(SendInput(1, &alt, sizeof(alt)) == 1 &&
                      input_test_take(window, WM_SYSKEYDOWN, WM_SYSKEYDOWN,
                                      &message) &&
                      message.wParam == VK_MENU &&
                      (DWORD)message.lParam == 0x20380001,
                      "generic injected Alt maps to its physical key",
                      checks, failures);
    alt.data.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt, sizeof(alt));
    input_test_expect(input_test_take(window, WM_SYSKEYUP, WM_SYSKEYUP,
                                      &message) &&
                      (DWORD)message.lParam == 0xC0380001,
                      "standalone Alt release has no down context",
                      checks, failures);
    message.hwnd = window;
    message.message = WM_SYSKEYDOWN;
    message.wParam = 'A';
    message.lParam = 0x201E0001;
    input_test_expect(TranslateMessage(&message) &&
                      input_test_take(window, WM_SYSCHAR, WM_SYSCHAR, &message) &&
                      message.wParam == 'a' && message.lParam == 0x201E0001,
                      "system character keeps its message type and context",
                      checks, failures);
    input_test_expect(!input_test_take(window, WM_CHAR, WM_CHAR, &message),
                      "system character does not leak into ordinary text",
                      checks, failures);
    HWND saved_focus = focus_hwnd;
    focus_hwnd = NULL;
    win32_post_keyboard_event(0x30, FALSE);
    win32_post_keyboard_event(0x30, TRUE);
    focus_hwnd = saved_focus;
    input_test_expect(input_test_take(window, WM_SYSKEYDOWN, WM_SYSKEYDOWN,
                                      &message) && message.wParam == 'B' &&
                      (DWORD)message.lParam == 0x00300001,
                      "unfocused active window receives system keydown",
                      checks, failures);
    input_test_expect(input_test_take(window, WM_SYSKEYUP, WM_SYSKEYUP,
                                      &message) && message.wParam == 'B',
                      "unfocused active window receives system keyup",
                      checks, failures);
}

int user32_input_selftest(void)
{
    static const char class_name[] = "OsitoInputTest";
    WNDCLASSA window_class = {
        .lpfnWndProc = wm_test_wndproc,
        .lpszClassName = class_name,
    };
    BYTE saved_keys[256];
    BYTE saved_async[256];
    BOOL saved_alt_pending = keyboard_alt_pending;
    DWORD saved_buttons = mouse_buttons;
    POINT saved_cursor = cursor_pos;
    HWND saved_capture = capture_hwnd;
    NATIVE_MOVE_STATE saved_native_move = native_move;
    DWORD test_pid = GetCurrentProcessId();
    DWORD test_tid = GetCurrentThreadId();
    BYTE saved_retrieved_source =
        msg_last_retrieved_source(test_pid, test_tid);
    uint64_t saved_input_sequence =
        msg_last_input_sequence(test_pid, test_tid);
    int saved_cursor_visible = cursor_display_count(test_pid, test_tid);
    int saved_abs_prev_valid = g_abs_prev_valid;
    int saved_abs_prev_sx = g_abs_prev_sx;
    int saved_abs_prev_sy = g_abs_prev_sy;
    RECT saved_clip_rect = clip_rect;
    int saved_clip_active = clip_active;
    DWORD saved_clip_owner_pid = clip_owner_pid;
    DWORD saved_clip_owner_tid = clip_owner_tid;
    RELATIVE_POINTER_STATE saved_relative_pointer = relative_pointer;
    LPARAM saved_extra = message_extra_info;
    DWORD saved_last_error = GetLastError();
    HWND saved_focus = focus_hwnd;
    HWND saved_active = active_hwnd;
    BOOL saved_foreground_active = user32_foreground_active;
    HWND window = NULL;
    HWND capture_window = NULL;
    WORD atom = 0;
    int checks = 0;
    int failures = 0;

    serial_puts("[INPUTTEST] starting SendInput contract test\n");
    if (g_compat32_mode) {
        serial_puts("[INPUTTEST] FAIL: cannot run during a PE32 callback\n");
        return 1;
    }

    input_test_expect(
        map_fullscreen_axis(0, 107, 1066, 640) == 0 &&
        map_fullscreen_axis(107, 107, 1066, 640) == 0 &&
        map_fullscreen_axis(640, 107, 1066, 640) == 320 &&
        map_fullscreen_axis(1172, 107, 1066, 640) == 639 &&
        map_fullscreen_axis(1279, 107, 1066, 640) == 639 &&
        map_fullscreen_axis(400, 0, 800, 480) == 240 &&
        map_fullscreen_axis(799, 0, 800, 480) == 479,
        "fullscreen pointer inverts aspect-fit projection",
        &checks, &failures);

    for (int i = 0; i < 256; i++) {
        saved_keys[i] = key_state[i];
        saved_async[i] = async_pressed[i];
        key_state[i] = 0;
        async_pressed[i] = 0;
    }

    atom = RegisterClassA(&window_class);
    input_test_expect(atom != 0, "RegisterClassA", &checks, &failures);
    if (!atom)
        goto cleanup;
    window = CreateWindowExA(0, class_name, "input-test",
                             WS_OVERLAPPED | WS_VISIBLE,
                             0, 0, current_mode_cx(), current_mode_cy(),
                             NULL, NULL, NULL, NULL);
    input_test_expect(window != NULL, "CreateWindowExA", &checks, &failures);
    if (!window)
        goto cleanup;
    capture_window = CreateWindowExA(0, class_name, "capture-test",
                                     WS_CHILD | WS_VISIBLE,
                                     500, 500, 8, 8, window, NULL,
                                     NULL, NULL);
    input_test_expect(capture_window != NULL, "create capture child",
                      &checks, &failures);
    if (!capture_window)
        goto cleanup;
    SetFocus(window);
    capture_hwnd = NULL;
    cursor_change_count(test_pid, test_tid, 0, FALSE);
    clip_active = 0;
    clip_owner_pid = 0;
    clip_owner_tid = 0;
    relative_pointer_reset();
    mouse_buttons = 0;
    ValidateRect(window, NULL);
    msg_purge_process(GetCurrentProcessId());

    wm_test_capturechanged_count = 0;
    wm_test_capturechanged_window = NULL;
    wm_test_capturechanged_new = NULL;
    input_test_expect(SetCapture(capture_window) == NULL &&
                      GetCapture() == capture_window &&
                      GetFocus() == window,
                      "SetCapture preserves keyboard focus",
                      &checks, &failures);
    input_test_expect(SetCapture(window) == capture_window &&
                      GetCapture() == window &&
                      wm_test_capturechanged_count == 1 &&
                      wm_test_capturechanged_window == capture_window &&
                      wm_test_capturechanged_new == window &&
                      GetFocus() == window,
                      "capture transfer notifies previous window",
                      &checks, &failures);
    input_test_expect(ReleaseCapture() && GetCapture() == NULL &&
                      wm_test_capturechanged_count == 2 &&
                      wm_test_capturechanged_window == window &&
                      wm_test_capturechanged_new == NULL &&
                      GetFocus() == window,
                      "ReleaseCapture preserves focus and notifies",
                      &checks, &failures);

    INPUT keys[2] = { 0 };
    keys[0].type = INPUT_KEYBOARD;
    keys[0].data.ki.wVk = 'A';
    keys[0].data.ki.dwExtraInfo = 0x1122334455667788ULL;
    keys[1] = keys[0];
    keys[1].data.ki.dwFlags = KEYEVENTF_KEYUP;
    keys[1].data.ki.dwExtraInfo = 0x8877665544332211ULL;
    input_test_expect(SendInput(2, keys, sizeof(INPUT)) == 2,
                      "PE64 keyboard pair accepted", &checks, &failures);

    MSG message = { 0 };
    input_test_expect(input_test_take(window, WM_KEYDOWN, WM_KEYDOWN,
                                      &message) &&
                      message.wParam == 'A' &&
                      (message.lParam & 0xFFFF) == 1,
                      "WM_KEYDOWN payload", &checks, &failures);
    input_test_expect((ULONG_PTR)GetMessageExtraInfo() ==
                          0x1122334455667788ULL,
                      "keydown dwExtraInfo", &checks, &failures);
    input_test_expect(TranslateMessage(&message) &&
                      input_test_take(window, WM_CHAR, WM_CHAR, &message) &&
                      message.wParam == 'a',
                      "regular key translates to WM_CHAR",
                      &checks, &failures);
    input_test_expect(input_test_take(window, WM_KEYUP, WM_KEYUP, &message) &&
                      message.wParam == 'A' &&
                      ((ULONG_PTR)message.lParam & 0xC0000000ULL) ==
                          0xC0000000ULL,
                      "WM_KEYUP transition bits", &checks, &failures);
    input_test_expect((ULONG_PTR)GetMessageExtraInfo() ==
                          0x8877665544332211ULL,
                      "keyup dwExtraInfo", &checks, &failures);
    short async_a = GetAsyncKeyState('A');
    input_test_expect((async_a & 0x8000) == 0 && (async_a & 1) != 0,
                      "keyboard state after down/up", &checks, &failures);

    key_state['C'] = 0;
    win32_post_keyboard_event(0x2E, FALSE);
    win32_post_keyboard_event(0x2E, TRUE);
    win32_post_keyboard_event(0x2E, FALSE);
    win32_post_keyboard_event(0x2E, TRUE);
    input_test_expect((GetKeyState('C') & 0x0001) == 0,
                      "ordinary hardware key does not toggle",
                      &checks, &failures);

    key_state[VK_CAPITAL] = 0;
    win32_post_keyboard_event(0x3A, FALSE);
    win32_post_keyboard_event(0x3A, TRUE);
    input_test_expect((GetKeyState(VK_CAPITAL) & 0x0001) != 0,
                      "Caps Lock hardware key toggles on",
                      &checks, &failures);
    win32_post_keyboard_event(0x3A, FALSE);
    win32_post_keyboard_event(0x3A, TRUE);
    input_test_expect((GetKeyState(VK_CAPITAL) & 0x0001) == 0,
                      "Caps Lock hardware key toggles off",
                      &checks, &failures);
    msg_purge_process(GetCurrentProcessId());

    INPUT unicode = { 0 };
    input_test_keyboard_messages(window, &checks, &failures);
    unicode.type = INPUT_KEYBOARD;
    unicode.data.ki.wScan = 0x03A9;
    unicode.data.ki.dwFlags = KEYEVENTF_UNICODE;
    unicode.data.ki.dwExtraInfo = 0xA55A;
    input_test_expect(SendInput(1, &unicode, sizeof(INPUT)) == 1,
                      "Unicode input accepted", &checks, &failures);
    input_test_expect(input_test_take(window, WM_KEYDOWN, WM_KEYDOWN,
                                      &message) &&
                      message.wParam == VK_PACKET,
                      "Unicode input uses VK_PACKET", &checks, &failures);
    input_test_expect(TranslateMessage(&message) &&
                      input_test_take(window, WM_CHAR, WM_CHAR, &message) &&
                      message.wParam == 0x03A9,
                      "VK_PACKET translates to UTF-16 WM_CHAR",
                      &checks, &failures);
    input_test_expect((ULONG_PTR)GetMessageExtraInfo() == 0xA55A,
                      "Unicode dwExtraInfo propagates to WM_CHAR",
                      &checks, &failures);

    BYTE input32[56];
    for (unsigned i = 0; i < sizeof(input32); i++) input32[i] = 0;
    input_test_write_u32(input32 + 0, INPUT_KEYBOARD);
    input_test_write_u16(input32 + 4, 'B');
    input_test_write_u32(input32 + 16, 0x1234ABCD);
    input_test_write_u32(input32 + 28, INPUT_KEYBOARD);
    input_test_write_u16(input32 + 32, 'B');
    input_test_write_u32(input32 + 36, KEYEVENTF_KEYUP);
    input_test_write_u32(input32 + 44, 0xDEADBEEF);
    g_compat32_mode = 1;
    UINT input32_count = SendInput(2, (const INPUT *)(const void *)input32, 28);
    g_compat32_mode = 0;
    input_test_expect(input32_count == 2, "PE32 INPUT layout accepted",
                      &checks, &failures);
    input_test_expect(input_test_take(window, WM_KEYDOWN, WM_KEYDOWN,
                                      &message) && message.wParam == 'B' &&
                      (ULONG_PTR)GetMessageExtraInfo() == 0x1234ABCD,
                      "PE32 keydown parsed", &checks, &failures);
    input_test_expect(input_test_take(window, WM_KEYUP, WM_KEYUP, &message) &&
                      message.wParam == 'B' &&
                      (ULONG_PTR)GetMessageExtraInfo() == 0xDEADBEEF,
                      "PE32 keyup parsed", &checks, &failures);

    SetCursorPos(10, 20);
    RECT requested_clip = { 3, 4, 123, 234 };
    RECT observed_clip = { 0 };
    input_test_expect(ClipCursor(&requested_clip) &&
                      GetClipCursor(&observed_clip) &&
                      observed_clip.left == requested_clip.left &&
                      observed_clip.top == requested_clip.top &&
                      observed_clip.right == requested_clip.right &&
                      observed_clip.bottom == requested_clip.bottom,
                      "ClipCursor/GetClipCursor round trip",
                      &checks, &failures);
    input_test_expect(!relative_pointer_mode_active(),
                      "confinement alone keeps absolute input",
                      &checks, &failures);
    SetCursorPos(1000, -10);
    POINT confined_cursor = { 0 };
    input_test_expect(GetCursorPos(&confined_cursor) &&
                      confined_cursor.x == requested_clip.right - 1 &&
                      confined_cursor.y == requested_clip.top,
                      "SetCursorPos obeys ClipCursor bounds",
                      &checks, &failures);
    POINT clip_center = {
        requested_clip.left +
            (requested_clip.right - requested_clip.left) / 2,
        requested_clip.top +
            (requested_clip.bottom - requested_clip.top) / 2,
    };
    SetCursorPos(clip_center.x, clip_center.y);
    input_test_expect(relative_pointer_mode_active(),
                      "center warp enables relative tablet translation",
                      &checks, &failures);
    input_test_expect(ClipCursor(NULL) && GetClipCursor(&observed_clip) &&
                      observed_clip.left == 0 && observed_clip.top == 0 &&
                      observed_clip.right == current_mode_cx() &&
                      observed_clip.bottom == current_mode_cy(),
                      "released clip reports desktop bounds",
                      &checks, &failures);
    input_test_expect(!relative_pointer_mode_active(),
                      "released confinement disables relative translation",
                      &checks, &failures);
    input_test_expect(ShowCursor(FALSE) == -1 &&
                      !relative_pointer_mode_active(),
                      "hidden cursor alone keeps absolute input",
                      &checks, &failures);
    struct {
        DWORD size, flags;
        HCURSOR cursor;
        POINT position;
    } cursor_info = { .size = sizeof(cursor_info) };
    input_test_expect(GetCursorInfo(&cursor_info) && cursor_info.flags == 0,
                      "native GetCursorInfo reports the hidden pointer owner",
                      &checks, &failures);
    DWORD cursor_info32[5] = { 20, 0xFFFFFFFFU, 0, 0, 0 };
    g_compat32_mode = 1;
    BOOL cursor_info32_ok = GetCursorInfo(cursor_info32);
    g_compat32_mode = 0;
    input_test_expect(cursor_info32_ok && cursor_info32[1] == 0,
                      "PE32 GetCursorInfo reports the same hidden owner",
                      &checks, &failures);
    WINDOW *cursor_window = find_window(window);
    if (cursor_window && cursor_window->compositor_id) {
        input_test_expect(!user32_cursor_overlay_visible(
                              cursor_window->compositor_id),
                          "hidden owner suppresses the compositor cursor",
                          &checks, &failures);
        DWORD owner_tid = cursor_window->owner_tid;
        cursor_window->owner_tid = ~owner_tid;
        input_test_expect(user32_cursor_overlay_visible(
                              cursor_window->compositor_id),
                          "another thread does not inherit a hidden cursor",
                          &checks, &failures);
        cursor_window->owner_tid = owner_tid;
    }
    input_test_expect(user32_cursor_overlay_visible(0) &&
                      user32_cursor_overlay_visible(0xFFFFFFFFU),
                      "desktop and stale surface IDs retain a cursor",
                      &checks, &failures);
    SetCursorPos(current_mode_cx() / 2, current_mode_cy() / 2);
    input_test_expect(relative_pointer_mode_active(),
                      "hidden center warp enables relative translation",
                      &checks, &failures);
    input_test_expect(ShowCursor(TRUE) == 0 &&
                      !relative_pointer_mode_active(),
                      "restored cursor disables relative translation",
                      &checks, &failures);
    input_test_expect(GetCursorInfo(&cursor_info) && cursor_info.flags == 1,
                      "GetCursorInfo reports restored visibility",
                      &checks, &failures);
    if (cursor_window && cursor_window->compositor_id)
        input_test_expect(user32_cursor_overlay_visible(
                              cursor_window->compositor_id),
                          "balanced ShowCursor restores the compositor cursor",
                          &checks, &failures);
    cursor_change_count(~test_pid, 1, -2, FALSE);
    cursor_change_count(~test_pid, 2, -1, FALSE);
    cursor_release_counts(~test_pid, 1);
    input_test_expect(cursor_display_count(~test_pid, 1) == 0 &&
                      cursor_display_count(~test_pid, 2) == -1,
                      "thread cursor cleanup preserves its sibling",
                      &checks, &failures);
    cursor_release_counts(~test_pid, 0);
    input_test_expect(cursor_display_count(~test_pid, 2) == 0,
                      "process cursor cleanup releases remaining counts",
                      &checks, &failures);
    ShowWindow(window, SW_SHOWNA);
    SetFocus(window);
    msg_purge_process(GetCurrentProcessId());
    SetCursorPos(10, 20);
    win32_post_mouse_screen(111, 77, 45, 31, 1, 0);
    POINT synced_cursor = { 0 };
    input_test_expect(GetCursorPos(&synced_cursor) &&
                      synced_cursor.x == 111 && synced_cursor.y == 77,
                      "hardware cursor follows compositor coordinates",
                      &checks, &failures);
    input_test_expect(input_test_take(window, WM_MOUSEMOVE, WM_MOUSEMOVE,
                                      &message) &&
                      (short)(message.lParam & 0xFFFF) == 111 &&
                      (short)((message.lParam >> 16) & 0xFFFF) == 77 &&
                      (message.wParam & MK_LBUTTON),
                      "hardware WM_MOUSEMOVE carries coordinates and buttons",
                      &checks, &failures);
    input_test_expect(input_test_take(window, WM_LBUTTONDOWN,
                                      WM_LBUTTONDOWN, &message) &&
                      (message.wParam & MK_LBUTTON),
                      "hardware click uses synchronized target",
                      &checks, &failures);
    win32_post_mouse_screen(111, 77, 0, 0, 0, 0);
    input_test_expect(input_test_take(window, WM_LBUTTONUP, WM_LBUTTONUP,
                                      &message) &&
                      !(message.wParam & MK_LBUTTON),
                      "hardware click release", &checks, &failures);

    ShowWindow(window, SW_HIDE);
    focus_hwnd = window; /* Reproduce a stale focus left by a hidden CEF HWND. */
    SetCursorPos(111, 77);
    input_test_expect(mouse_input_target() != window,
                      "hidden focused window excluded from pointer hit-test",
                      &checks, &failures);
    ShowWindow(window, SW_SHOWNA);
    SetFocus(window);

    /* Queue both physical edges before consuming the down event, matching a
     * fast click while a slow CEF pump is busy. SetCapture from the down
     * handler must redirect the still-pending up edge to the capture window. */
    msg_purge_process(GetCurrentProcessId());
    win32_post_mouse_screen(111, 77, 0, 0, 1, 0);
    win32_post_mouse_screen(111, 77, 0, 0, 0, 0);
    input_test_expect(input_test_take(window, WM_LBUTTONDOWN,
                                      WM_LBUTTONDOWN, &message),
                      "rapid hardware click down", &checks, &failures);
    SetCapture(capture_window);
    input_test_expect(input_test_take(capture_window, WM_LBUTTONUP,
                                      WM_LBUTTONUP, &message) &&
                      message.hwnd == capture_window &&
                      (short)(message.lParam & 0xFFFF) == 111 - 500 &&
                      (short)((message.lParam >> 16) & 0xFFFF) == 77 - 500,
                      "capture retargets queued hardware release",
                      &checks, &failures);
    input_test_expect(!relative_pointer_mode_active() &&
                      mouse_input_target() == capture_window,
                      "ordinary capture stays on absolute input path",
                      &checks, &failures);
    input_test_expect(!u32_input_diagnostics_active(),
                      "PE64 capture skips compat32 diagnostics",
                      &checks, &failures);
    msg_purge_process(test_pid);
    win32_post_mouse_screen(222, 144, 7, -3, 0, 0);
    POINT captured_cursor = { 0 };
    input_test_expect(GetCursorPos(&captured_cursor) &&
                      captured_cursor.x == 222 && captured_cursor.y == 144 &&
                      input_test_take(capture_window, WM_MOUSEMOVE,
                                      WM_MOUSEMOVE, &message) &&
                      message.hwnd == capture_window,
                      "captured desktop pointer retains absolute coordinates",
                      &checks, &failures);
    ReleaseCapture();

    SetCursorPos(10, 20);
    INPUT mouse = { 0 };
    mouse.type = INPUT_MOUSE;
    mouse.data.mi.dx = 15;
    mouse.data.mi.dy = -5;
    mouse.data.mi.mouseData = 120;
    mouse.data.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN |
                            MOUSEEVENTF_WHEEL;
    mouse.data.mi.dwExtraInfo = 0xCAFE;
    input_test_expect(SendInput(1, &mouse, sizeof(INPUT)) == 1,
                      "combined mouse input accepted", &checks, &failures);
    input_test_expect(input_test_take(window, WM_MOUSEMOVE,
                                      WM_MOUSEHWHEEL, &message) &&
                      message.message == WM_MOUSEMOVE &&
                      (short)(message.lParam & 0xFFFF) == 25 &&
                      (short)((message.lParam >> 16) & 0xFFFF) == 15,
                      "relative mouse coordinates", &checks, &failures);
    input_test_expect(GetMessagePos() == (DWORD)(25 | (15 << 16)),
                      "mouse MSG.pt/GetMessagePos", &checks, &failures);
    input_test_expect(input_test_take(window, WM_MOUSEMOVE,
                                      WM_MOUSEHWHEEL, &message) &&
                      message.message == WM_LBUTTONDOWN &&
                      (message.wParam & MK_LBUTTON),
                      "left-button state", &checks, &failures);
    input_test_expect(input_test_take(window, WM_MOUSEMOVE,
                                      WM_MOUSEHWHEEL, &message) &&
                      message.message == WM_MOUSEWHEEL &&
                      (short)(message.wParam >> 16) == 120 &&
                      (message.wParam & MK_LBUTTON),
                      "wheel delta and button state", &checks, &failures);
    input_test_expect((ULONG_PTR)GetMessageExtraInfo() == 0xCAFE,
                      "mouse dwExtraInfo", &checks, &failures);

    INPUT left_up = { 0 };
    left_up.type = INPUT_MOUSE;
    left_up.data.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    input_test_expect(SendInput(1, &left_up, sizeof(INPUT)) == 1 &&
                      input_test_take(window, WM_LBUTTONUP, WM_LBUTTONUP,
                                      &message) &&
                      !(message.wParam & MK_LBUTTON),
                      "left-button release", &checks, &failures);

    INPUT absolute[2] = { 0 };
    absolute[0].type = INPUT_MOUSE;
    absolute[0].data.mi.dwFlags = MOUSEEVENTF_MOVE |
        MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE_NOCOALESCE;
    absolute[1] = absolute[0];
    absolute[1].data.mi.dx = 65535;
    absolute[1].data.mi.dy = 65535;
    input_test_expect(SendInput(2, absolute, sizeof(INPUT)) == 2,
                      "absolute no-coalesce pair accepted",
                      &checks, &failures);
    input_test_expect(input_test_take(window, WM_MOUSEMOVE, WM_MOUSEMOVE,
                                      &message) &&
                      (short)(message.lParam & 0xFFFF) == 0 &&
                      (short)((message.lParam >> 16) & 0xFFFF) == 0,
                      "absolute origin mapping", &checks, &failures);
    input_test_expect(input_test_take(window, WM_MOUSEMOVE, WM_MOUSEMOVE,
                                      &message) &&
                      (short)(message.lParam & 0xFFFF) ==
                          current_mode_cx() - 1 &&
                      (short)((message.lParam >> 16) & 0xFFFF) ==
                          current_mode_cy() - 1,
                      "absolute endpoint and no-coalesce",
                      &checks, &failures);

    INPUT xbuttons[2] = { 0 };
    xbuttons[0].type = INPUT_MOUSE;
    xbuttons[0].data.mi.mouseData = XBUTTON1;
    xbuttons[0].data.mi.dwFlags = MOUSEEVENTF_XDOWN;
    xbuttons[1] = xbuttons[0];
    xbuttons[1].data.mi.dwFlags = MOUSEEVENTF_XUP;
    input_test_expect(SendInput(2, xbuttons, sizeof(INPUT)) == 2,
                      "XBUTTON pair accepted", &checks, &failures);
    input_test_expect(input_test_take(window, WM_XBUTTONDOWN,
                                      WM_XBUTTONDOWN, &message) &&
                      (WORD)(message.wParam >> 16) == XBUTTON1 &&
                      (message.wParam & MK_XBUTTON1),
                      "WM_XBUTTONDOWN payload", &checks, &failures);
    input_test_expect(input_test_take(window, WM_XBUTTONUP,
                                      WM_XBUTTONUP, &message) &&
                      (WORD)(message.wParam >> 16) == XBUTTON1 &&
                      !(message.wParam & MK_XBUTTON1),
                      "WM_XBUTTONUP payload", &checks, &failures);

    INPUT hardware = { 0 };
    hardware.type = INPUT_HARDWARE;
    hardware.data.hi.uMsg = WM_USER + 17;
    hardware.data.hi.wParamL = 0x1122;
    hardware.data.hi.wParamH = 0x3344;
    input_test_expect(SendInput(1, &hardware, sizeof(INPUT)) == 1 &&
                      input_test_take(window, WM_USER + 17, WM_USER + 17,
                                      &message) &&
                      (ULONG_PTR)message.lParam == 0x33441122,
                      "HARDWAREINPUT payload", &checks, &failures);

    input_test_expect(SendInput(1, &unicode, 28) == 0,
                      "PE64 rejects PE32 cbSize", &checks, &failures);
    INPUT invalid = { .type = 99 };
    input_test_expect(SendInput(1, &invalid, sizeof(INPUT)) == 0,
                      "invalid INPUT type rejected", &checks, &failures);

    ShowWindow(window, SW_SHOWNA);
    SetWindowPos(window, HWND_TOP, 100, 80, 300, 200,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetFocus(window);
    capture_hwnd = NULL;
    native_move.window = NULL;
    mouse_buttons = 0;
    msg_purge_process(GetCurrentProcessId());
    wm_test_caption_hit = TRUE;
    wm_test_nclbuttondown_count = 0;
    wm_test_entersizemove_count = 0;
    wm_test_exitsizemove_count = 0;

    SetCursorPos(140, 100);
    DWORD foreign_tid = test_tid ^ 0x80000000U;
    if (!foreign_tid || foreign_tid == test_tid)
        foreign_tid = test_tid + 1;
    msg_note_retrieval(test_pid, test_tid, MSG_SOURCE_INPUT, 0xCAFE);
    msg_note_retrieval(test_pid, foreign_tid, MSG_SOURCE_POSTED, 0);
    input_test_expect(
        msg_last_retrieved_source(test_pid, test_tid) == MSG_SOURCE_INPUT &&
        msg_last_retrieved_source(test_pid, foreign_tid) == MSG_SOURCE_POSTED,
        "retrieved input source is thread-local", &checks, &failures);
    msg_purge_process(test_pid);

    input_test_expect(PostMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON,
                                   (LPARAM)(140 | (100 << 16))),
                      "posted caption-shaped click queued",
                      &checks, &failures);
    BOOL posted_down = input_test_take(window, WM_LBUTTONDOWN,
                                       WM_LBUTTONDOWN, &message);
    if (posted_down)
        DispatchMessageA(&message);
    input_test_expect(posted_down && !native_move.window &&
                      GetCapture() == NULL &&
                      wm_test_nclbuttondown_count == 0,
                      "posted click does not enter native move loop",
                      &checks, &failures);
    capture_hwnd = NULL;
    native_move.window = NULL;
    native_move.pointer_offset_x = 0;
    native_move.pointer_offset_y = 0;
    wm_test_nclbuttondown_count = 0;
    wm_test_entersizemove_count = 0;
    wm_test_exitsizemove_count = 0;
    msg_purge_process(test_pid);

    win32_post_mouse_screen(140, 100, 0, 0, 1, 0);
    BOOL move_down = input_test_take(window, WM_LBUTTONDOWN,
                                     WM_LBUTTONDOWN, &message);
    if (move_down)
        DispatchMessageA(&message);
    input_test_expect(move_down && native_move.window == window &&
                      GetCapture() == window &&
                      wm_test_nclbuttondown_count == 1 &&
                      wm_test_entersizemove_count == 1,
                      "caption press enters native move loop",
                      &checks, &failures);

    win32_post_mouse_screen(180, 130, 40, 30, 1, 0);
    BOOL move_motion = input_test_take(window, WM_MOUSEMOVE,
                                       WM_MOUSEMOVE, &message);
    if (move_motion)
        DispatchMessageA(&message);
    RECT moved_rect = {0};
    input_test_expect(move_motion && GetWindowRect(window, &moved_rect) &&
                      wm_test_rect_is(&moved_rect, 140, 110, 440, 310),
                      "captured pointer moves top-level window",
                      &checks, &failures);

    win32_post_mouse_screen(180, 130, 0, 0, 0, 0);
    BOOL move_up = input_test_take(window, WM_LBUTTONUP,
                                   WM_LBUTTONUP, &message);
    if (move_up)
        DispatchMessageA(&message);
    input_test_expect(move_up && !native_move.window &&
                      GetCapture() == NULL &&
                      wm_test_exitsizemove_count == 1,
                      "caption release exits native move loop",
                      &checks, &failures);
    wm_test_caption_hit = FALSE;

    HCURSOR shared_cursor = LoadCursorW(NULL, (PCWSTR)(ULONG_PTR)32512);
    SetLastError(0xA55A);
    input_test_expect(DestroyCursor(shared_cursor) &&
                      GetLastError() == 0xA55A,
                      "shared cursor destroy is a preserving no-op",
                      &checks, &failures);
    SetLastError(0xA55A);
    input_test_expect(!DestroyCursor(NULL) && GetLastError() == 1402,
                      "invalid cursor rejected with Win32 error",
                      &checks, &failures);

cleanup:
    msg_purge_process(GetCurrentProcessId());
    if (capture_window) DestroyWindow(capture_window);
    if (window) DestroyWindow(window);
    if (atom) UnregisterClassA(class_name, NULL);
    for (int i = 0; i < 256; i++) {
        key_state[i] = saved_keys[i];
        async_pressed[i] = saved_async[i];
    }
    mouse_buttons = saved_buttons;
    keyboard_alt_pending = saved_alt_pending;
    cursor_pos = saved_cursor;
    capture_hwnd = saved_capture;
    native_move = saved_native_move;
    msg_note_retrieval(test_pid, test_tid, saved_retrieved_source,
                       saved_input_sequence);
    cursor_change_count(test_pid, test_tid, saved_cursor_visible, FALSE);
    g_abs_prev_valid = saved_abs_prev_valid;
    g_abs_prev_sx = saved_abs_prev_sx;
    g_abs_prev_sy = saved_abs_prev_sy;
    clip_rect = saved_clip_rect;
    clip_active = saved_clip_active;
    clip_owner_pid = saved_clip_owner_pid;
    clip_owner_tid = saved_clip_owner_tid;
    relative_pointer = saved_relative_pointer;
    message_extra_info = saved_extra;
    focus_hwnd = saved_focus && find_window(saved_focus) ? saved_focus : NULL;
    active_hwnd = saved_active && find_window(saved_active)
        ? saved_active : NULL;
    user32_foreground_active = saved_foreground_active && active_hwnd;
    g_compat32_mode = 0;
    SetLastError(saved_last_error);

    serial_puts("[INPUTTEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

typedef struct {
    BYTE data[512];
    SIZE_T length;
    BOOL failed;
} DIALOG_TEST_BUILDER;

static void dialog_test_put(DIALOG_TEST_BUILDER *builder, PCVOID data,
                            SIZE_T size)
{
    if (!builder || builder->failed ||
        size > sizeof(builder->data) - builder->length) {
        if (builder) builder->failed = TRUE;
        return;
    }
    memcpy(builder->data + builder->length, data, size);
    builder->length += size;
}

static void dialog_test_byte(DIALOG_TEST_BUILDER *builder, BYTE value)
{
    dialog_test_put(builder, &value, sizeof(value));
}

static void dialog_test_word(DIALOG_TEST_BUILDER *builder, WORD value)
{
    dialog_test_put(builder, &value, sizeof(value));
}

static void dialog_test_dword(DIALOG_TEST_BUILDER *builder, DWORD value)
{
    dialog_test_put(builder, &value, sizeof(value));
}

static void dialog_test_string(DIALOG_TEST_BUILDER *builder,
                               const char *text)
{
    if (!text) {
        dialog_test_word(builder, 0);
        return;
    }
    while (*text)
        dialog_test_word(builder, (WORD)(BYTE)*text++);
    dialog_test_word(builder, 0);
}

static void dialog_test_ordinal(DIALOG_TEST_BUILDER *builder, WORD ordinal)
{
    dialog_test_word(builder, 0xFFFFU);
    dialog_test_word(builder, ordinal);
}

static void dialog_test_align(DIALOG_TEST_BUILDER *builder)
{
    while (builder->length & 3U)
        dialog_test_byte(builder, 0);
}

static void dialog_test_build_extended(DIALOG_TEST_BUILDER *builder)
{
    memset(builder, 0, sizeof(*builder));
    dialog_test_word(builder, 1);
    dialog_test_word(builder, 0xFFFFU);
    dialog_test_dword(builder, 0); /* help ID */
    dialog_test_dword(builder, 0); /* extended style */
    dialog_test_dword(builder, WS_POPUP | WS_CAPTION | WS_SYSMENU |
                               WS_VISIBLE | U32_DS_SETFONT | U32_DS_CENTER);
    dialog_test_word(builder, 2); /* controls */
    dialog_test_word(builder, 0);
    dialog_test_word(builder, 0);
    dialog_test_word(builder, 100);
    dialog_test_word(builder, 50);
    dialog_test_word(builder, 0); /* menu */
    dialog_test_word(builder, 0); /* default dialog class */
    dialog_test_string(builder, "Dialog contract");
    dialog_test_word(builder, 8);   /* point size */
    dialog_test_word(builder, 400); /* weight */
    dialog_test_byte(builder, 0);   /* italic */
    dialog_test_byte(builder, 1);   /* charset */
    dialog_test_string(builder, "MS Shell Dlg");

    dialog_test_align(builder);
    dialog_test_dword(builder, 0);
    dialog_test_dword(builder, 0);
    dialog_test_dword(builder, WS_CHILD | WS_VISIBLE);
    dialog_test_word(builder, 4);
    dialog_test_word(builder, 4);
    dialog_test_word(builder, 90);
    dialog_test_word(builder, 12);
    dialog_test_dword(builder, 100);
    dialog_test_ordinal(builder, 0x0082); /* STATIC */
    dialog_test_string(builder, "Ready");
    dialog_test_word(builder, 0);

    dialog_test_align(builder);
    dialog_test_dword(builder, 0);
    dialog_test_dword(builder, 0);
    dialog_test_dword(builder, WS_CHILD | WS_VISIBLE | WS_TABSTOP);
    dialog_test_word(builder, 35);
    dialog_test_word(builder, 25);
    dialog_test_word(builder, 30);
    dialog_test_word(builder, 14);
    dialog_test_dword(builder, IDOK);
    dialog_test_ordinal(builder, 0x0080); /* BUTTON */
    dialog_test_string(builder, "OK");
    dialog_test_word(builder, 0);
}

static void dialog_test_build_standard(DIALOG_TEST_BUILDER *builder)
{
    memset(builder, 0, sizeof(*builder));
    dialog_test_dword(builder, WS_POPUP);
    dialog_test_dword(builder, 0);
    dialog_test_word(builder, 0);
    dialog_test_word(builder, 2);
    dialog_test_word(builder, 3);
    dialog_test_word(builder, 40);
    dialog_test_word(builder, 20);
    dialog_test_word(builder, 0);
    dialog_test_word(builder, 0);
    dialog_test_string(builder, "Standard dialog");
}

static int dialog_test_init_count;
static int dialog_test_command_count;
static BOOL dialog_test_focus_valid;
static HWND dialog_test_modal_owner;
static BOOL dialog_test_owner_disabled;
static BOOL dialog_test_end_succeeded;

static ULONG_PTR WINAPI dialog_test_modeless_proc(HWND dialog, DWORD message,
                                                   WPARAM wparam,
                                                   LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        dialog_test_init_count++;
        dialog_test_focus_valid =
            (HWND)(ULONG_PTR)wparam == GetDlgItem(dialog, IDOK) &&
            lparam == (LPARAM)0x12345678;
        return TRUE;
    }
    if (message == WM_COMMAND && (UINT)(wparam & 0xFFFFU) == IDOK) {
        dialog_test_command_count++;
        return lparam == (LPARAM)(ULONG_PTR)GetDlgItem(dialog, IDOK);
    }
    return FALSE;
}

static ULONG_PTR WINAPI dialog_test_modal_proc(HWND dialog, DWORD message,
                                                WPARAM wparam,
                                                LPARAM lparam)
{
    (void)wparam;
    if (message == WM_INITDIALOG) {
        dialog_test_init_count++;
        dialog_test_owner_disabled = dialog_test_modal_owner &&
            !IsWindowEnabled(dialog_test_modal_owner) &&
            lparam == (LPARAM)0x55AA;
        dialog_test_end_succeeded = EndDialog(dialog, 42);
        return TRUE;
    }
    return FALSE;
}

static void dialog_test_expect(BOOL condition, const char *name,
                               int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[DIALOG-TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int user32_dialog_selftest(void)
{
    int checks = 0, failures = 0;
    DIALOG_TEST_BUILDER extended;
    DIALOG_TEST_BUILDER standard;
    HWND modeless = NULL;
    HWND standard_window = NULL;
    HWND owner = NULL;

    serial_puts("[DIALOG-TEST] starting USER32 dialog test\n");
    if (g_compat32_mode) {
        serial_puts("[DIALOG-TEST] FAIL: PE32 callback active\n");
        return 1;
    }

    dialog_test_build_extended(&extended);
    dialog_test_expect(!extended.failed, "extended template builder",
                       &checks, &failures);

    dialog_test_init_count = 0;
    dialog_test_command_count = 0;
    dialog_test_focus_valid = FALSE;
    modeless = CreateDialogIndirectParamW(
        NULL, extended.data, NULL, dialog_test_modeless_proc,
        (LPARAM)0x12345678);
    HWND static_control = modeless ? GetDlgItem(modeless, 100) : NULL;
    HWND button = modeless ? GetDlgItem(modeless, IDOK) : NULL;
    WCHAR class_name[16] = {0};
    WCHAR static_text[16] = {0};
    RECT client = {0};
    if (modeless) {
        GetClassNameW(modeless, class_name,
                      (int)(sizeof(class_name) / sizeof(class_name[0])));
        if (static_control)
            GetWindowTextW(static_control, static_text,
                           (int)(sizeof(static_text) /
                                 sizeof(static_text[0])));
        GetClientRect(modeless, &client);
    }
    dialog_test_expect(modeless && dialog_state_find(modeless) &&
                       dialog_test_init_count == 1 &&
                       dialog_test_focus_valid && GetFocus() == button,
                       "modeless creation, init and focus",
                       &checks, &failures);
    dialog_test_expect(wm_test_wide_is(class_name, "#32770") &&
                       static_control && button &&
                       GetDlgCtrlID(static_control) == 100 &&
                       GetDlgCtrlID(button) == IDOK &&
                       wm_test_wide_is(static_text, "Ready"),
                       "resource classes, IDs and titles",
                       &checks, &failures);
    dialog_test_expect(client.right == 200 && client.bottom == 100 &&
                       IsWindowVisible(modeless),
                       "dialog-unit geometry and visibility",
                       &checks, &failures);
    if (button) SendMessageA(button, BM_CLICK, 0, 0);
    dialog_test_expect(dialog_test_command_count == 1,
                       "button command notification",
                       &checks, &failures);
    SetLastError(0);
    dialog_test_expect(!EndDialog(modeless, 9) && GetLastError() == 1400,
                       "EndDialog rejects modeless windows",
                       &checks, &failures);
    if (modeless) DestroyWindow(modeless);
    dialog_test_expect(!IsWindow(modeless) && !IsWindow(static_control) &&
                       !IsWindow(button) && !dialog_state_find(modeless),
                       "modeless tree and state teardown",
                       &checks, &failures);
    modeless = NULL;

    dialog_test_build_standard(&standard);
    standard_window = CreateDialogIndirectParamA(
        NULL, standard.data, NULL, NULL, 0);
    dialog_test_expect(!standard.failed && standard_window &&
                       dialog_state_find(standard_window),
                       "standard template parsing", &checks, &failures);
    if (standard_window) DestroyWindow(standard_window);
    standard_window = NULL;

    BYTE truncated[8] = { 1, 0, 0xFF, 0xFF, 0, 0, 0, 0 };
    SetLastError(0);
    dialog_test_expect(!dialog_create_template(
                           NULL, truncated, sizeof(truncated), NULL, NULL, 0,
                           FALSE, NULL) && GetLastError() == 1812,
                       "truncated template rejected", &checks, &failures);

    owner = CreateWindowExA(0, "STATIC", "dialog-owner",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            10, 10, 160, 100, NULL, NULL, NULL, NULL);
    dialog_test_modal_owner = owner;
    dialog_test_owner_disabled = FALSE;
    dialog_test_end_succeeded = FALSE;
    dialog_test_init_count = 0;
    LONG_PTR modal_result = owner ? DialogBoxIndirectParamW(
        NULL, extended.data, owner, dialog_test_modal_proc,
        (LPARAM)0x55AA) : -1;
    dialog_test_expect(owner && modal_result == 42 &&
                       dialog_test_init_count == 1 &&
                       dialog_test_owner_disabled &&
                       dialog_test_end_succeeded &&
                       IsWindowEnabled(owner),
                       "modal result and owner restoration",
                       &checks, &failures);

    if (owner) DestroyWindow(owner);
    dialog_test_modal_owner = NULL;
    serial_puts("[DIALOG-TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

typedef struct { const char *name; PVOID func; uint8_t argc; uint8_t cc; } SHIM_EXPORT;

static const SHIM_EXPORT user32_exports[] = {
    /* Window class */
    { "RegisterClassExA",   (PVOID)RegisterClassExA, 1, CC_STDCALL },
    { "RegisterClassA",     (PVOID)RegisterClassA, 1, CC_STDCALL },
    { "RegisterClassW",     (PVOID)RegisterClassW, 1, CC_STDCALL },
    { "UnregisterClassA",   (PVOID)UnregisterClassA, 2, CC_STDCALL },
    { "UnregisterClassW",   (PVOID)UnregisterClassW_k32, 2, CC_STDCALL },
    { "GetClassInfoA",      (PVOID)GetClassInfoA_k32, 3, CC_STDCALL },
    { "GetClassInfoW",      (PVOID)GetClassInfoW_k32, 3, CC_STDCALL },
    { "GetClassWord",       (PVOID)GetClassWord_u32, 2, CC_STDCALL },
    { "GetClassLongPtrW",   (PVOID)GetClassLongPtrW, 2, CC_STDCALL },
    { "SetClassLongA",      (PVOID)SetClassLongA_u32, 3, CC_STDCALL },
    { "SetClassLongW",      (PVOID)SetClassLongW, 3, CC_STDCALL },
    { "SetClassLongPtrA",   (PVOID)SetClassLongPtrA_u32, 3, CC_STDCALL },
    { "SetClassLongPtrW",   (PVOID)SetClassLongPtrW, 3, CC_STDCALL },
    { "GetClassNameW",      (PVOID)GetClassNameW, 3, CC_STDCALL },
    /* Window creation */
    { "CreateWindowExA",    (PVOID)CreateWindowExA, 12, CC_STDCALL },
    { "DestroyWindow",      (PVOID)DestroyWindow, 1, CC_STDCALL },
    { "ShowWindow",         (PVOID)ShowWindow, 2, CC_STDCALL },
    { "UpdateWindow",       (PVOID)UpdateWindow, 1, CC_STDCALL },
    { "SetWindowTextA",     (PVOID)SetWindowTextA, 2, CC_STDCALL },
    { "SetWindowTextW",     (PVOID)SetWindowTextW_k32, 2, CC_STDCALL },
    { "GetWindowTextLengthA", (PVOID)GetWindowTextLengthA, 1, CC_STDCALL },
    { "GetWindowTextLengthW", (PVOID)GetWindowTextLengthW, 1, CC_STDCALL },
    { "GetWindowTextW",     (PVOID)GetWindowTextW, 3, CC_STDCALL },
    { "GetWindowTextA",     (PVOID)GetWindowTextA, 3, CC_STDCALL },
    { "IsWindowUnicode",    (PVOID)IsWindowUnicode, 1, CC_STDCALL },
    { "SetWindowPos",       (PVOID)SetWindowPos, 7, CC_STDCALL },
    { "MoveWindow",         (PVOID)MoveWindow, 6, CC_STDCALL },
    { "BeginDeferWindowPos",(PVOID)BeginDeferWindowPos, 1, CC_STDCALL },
    { "DeferWindowPos",     (PVOID)DeferWindowPos, 8, CC_STDCALL },
    { "EndDeferWindowPos",  (PVOID)EndDeferWindowPos, 1, CC_STDCALL },
    { "SetLayeredWindowAttributes", (PVOID)SetLayeredWindowAttributes, 4,
      CC_STDCALL },
    { "GetLayeredWindowAttributes", (PVOID)GetLayeredWindowAttributes, 4,
      CC_STDCALL },
    /* Message loop */
    { "PeekMessageA",       (PVOID)PeekMessageA, 5, CC_STDCALL },
    { "GetMessageA",        (PVOID)GetMessageA, 4, CC_STDCALL },
    { "GetQueueStatus",     (PVOID)GetQueueStatus_u32, 1, CC_STDCALL },
    { "MsgWaitForMultipleObjects", (PVOID)MsgWaitForMultipleObjects_u32, 5, CC_STDCALL },
    { "MsgWaitForMultipleObjectsEx", (PVOID)MsgWaitForMultipleObjectsEx_u32, 5, CC_STDCALL },
    { "WaitMessage",        (PVOID)WaitMessage_u32, 0, CC_STDCALL },
    { "ShutdownBlockReasonCreate", (PVOID)ShutdownBlockReasonCreate_u32, 2, CC_STDCALL },
    { "ShutdownBlockReasonDestroy", (PVOID)ShutdownBlockReasonDestroy_u32, 1, CC_STDCALL },
    { "TranslateMessage",   (PVOID)TranslateMessage, 1, CC_STDCALL },
    { "DispatchMessageA",   (PVOID)DispatchMessageA, 1, CC_STDCALL },
    { "SetWindowsHookExA",  (PVOID)SetWindowsHookExA, 4, CC_STDCALL },
    { "SetWindowsHookExW",  (PVOID)SetWindowsHookExW, 4, CC_STDCALL },
    { "UnhookWindowsHookEx",(PVOID)UnhookWindowsHookEx, 1, CC_STDCALL },
    { "CallNextHookEx",     (PVOID)CallNextHookEx, 4, CC_STDCALL },
    { "PostQuitMessage",    (PVOID)PostQuitMessage, 1, CC_STDCALL },
    { "PostMessageA",       (PVOID)PostMessageA, 4, CC_STDCALL },
    { "SendMessageA",       (PVOID)SendMessageA, 4, CC_STDCALL },
    { "DefWindowProcA",     (PVOID)DefWindowProcA, 4, CC_STDCALL },
    { "DefDlgProcA",        (PVOID)DefDlgProcA, 4, CC_STDCALL },
    { "DefDlgProcW",        (PVOID)DefDlgProcW, 4, CC_STDCALL },
    { "IsDialogMessageA",   (PVOID)IsDialogMessageA, 2, CC_STDCALL },
    { "IsDialogMessageW",   (PVOID)IsDialogMessageW, 2, CC_STDCALL },
    /* Window info */
    { "GetClientRect",      (PVOID)GetClientRect, 2, CC_STDCALL },
    { "GetWindowRect",      (PVOID)GetWindowRect, 2, CC_STDCALL },
    { "GetWindowPlacement", (PVOID)GetWindowPlacement, 2, CC_STDCALL },
    { "SetWindowPlacement", (PVOID)SetWindowPlacement, 2, CC_STDCALL },
    { "IsRectEmpty",        (PVOID)IsRectEmpty, 1, CC_STDCALL },
    { "EqualRect",          (PVOID)EqualRect, 2, CC_STDCALL },
    { "SetRect",            (PVOID)SetRect, 5, CC_STDCALL },
    { "SetRectEmpty",       (PVOID)SetRectEmpty, 1, CC_STDCALL },
    { "OffsetRect",         (PVOID)OffsetRect, 3, CC_STDCALL },
    { "InflateRect",        (PVOID)InflateRect, 3, CC_STDCALL },
    { "IntersectRect",      (PVOID)IntersectRect, 3, CC_STDCALL },
    { "PtInRect",           (PVOID)PtInRect, 3, CC_STDCALL },
    { "GetWindowRgn",       (PVOID)GetWindowRgn, 2, CC_STDCALL },
    { "SetWindowRgn",       (PVOID)SetWindowRgn, 3, CC_STDCALL },
    { "AdjustWindowRect",   (PVOID)AdjustWindowRect, 3, CC_STDCALL },
    { "AdjustWindowRectEx", (PVOID)AdjustWindowRectEx, 4, CC_STDCALL },
    { "SystemParametersInfoA",(PVOID)SystemParametersInfoA, 4, CC_STDCALL },
    { "SystemParametersInfoW",(PVOID)SystemParametersInfoW, 4, CC_STDCALL },
    { "SetProcessDPIAware", (PVOID)SetProcessDPIAware_u32, 0, CC_STDCALL },
    { "SetProcessDpiAwarenessContext", (PVOID)SetProcessDpiAwarenessContext_u32, 1, CC_STDCALL },
    { "SetThreadDpiAwarenessContext", (PVOID)SetThreadDpiAwarenessContext_u32, 1, CC_STDCALL },
    { "GetThreadDpiAwarenessContext", (PVOID)GetThreadDpiAwarenessContext_u32, 0, CC_STDCALL },
    { "GetDpiForWindow",    (PVOID)GetDpiForWindow_u32, 1, CC_STDCALL },
    { "GetDpiForSystem",    (PVOID)GetDpiForSystem_u32, 0, CC_STDCALL },
    { "GetWindowDpiAwarenessContext", (PVOID)GetWindowDpiAwarenessContext_u32, 1, CC_STDCALL },
    { "GetAwarenessFromDpiAwarenessContext", (PVOID)GetAwarenessFromDpiAwarenessContext_u32, 1, CC_STDCALL },
    { "EnableNonClientDpiScaling", (PVOID)EnableNonClientDpiScaling_u32, 1, CC_STDCALL },
    { "AdjustWindowRectExForDpi", (PVOID)AdjustWindowRectExForDpi_u32, 5, CC_STDCALL },
    { "AreDpiAwarenessContextsEqual", (PVOID)AreDpiAwarenessContextsEqual_u32, 2, CC_STDCALL },
    { "IsValidDpiAwarenessContext", (PVOID)IsValidDpiAwarenessContext_u32, 1, CC_STDCALL },
    { "RegisterPointerDeviceNotifications", (PVOID)RegisterPointerDeviceNotifications_u32, 2, CC_STDCALL },
    { "GetPointerDevices",  (PVOID)GetPointerDevices_u32, 2, CC_STDCALL },
    { "RegisterPowerSettingNotification", (PVOID)RegisterPowerSettingNotification_u32, 3, CC_STDCALL },
    { "UnregisterPowerSettingNotification", (PVOID)UnregisterPowerSettingNotification_u32, 1, CC_STDCALL },
    { "RegisterDeviceNotificationW", (PVOID)RegisterDeviceNotificationW_u32, 3, CC_STDCALL },
    { "UnregisterDeviceNotification", (PVOID)UnregisterDeviceNotification_u32, 1, CC_STDCALL },
    { "RegisterSuspendResumeNotification", (PVOID)RegisterSuspendResumeNotification_u32, 2, CC_STDCALL },
    { "UnregisterSuspendResumeNotification", (PVOID)UnregisterSuspendResumeNotification_u32, 1, CC_STDCALL },
    { "SetTimer",            (PVOID)SetTimer, 4, CC_STDCALL },
    { "SetCoalescableTimer", (PVOID)SetCoalescableTimer_u32, 5, CC_STDCALL },
    { "KillTimer",           (PVOID)KillTimer, 2, CC_STDCALL },
    { "GetSystemMetrics",    (PVOID)GetSystemMetrics, 1, CC_STDCALL },
    { "GetSystemMetricsForDpi", (PVOID)GetSystemMetricsForDpi, 2, CC_STDCALL },
    { "MonitorFromWindow",   (PVOID)MonitorFromWindow, 2, CC_STDCALL },
    /* POINT is passed by value as two 32-bit stack slots in PE32. */
    { "MonitorFromPoint",    (PVOID)MonitorFromPoint, 3, CC_STDCALL },
    { "MonitorFromRect",     (PVOID)MonitorFromRect, 2, CC_STDCALL },
    { "GetMonitorInfoA",     (PVOID)GetMonitorInfoA, 2, CC_STDCALL },
    { "GetMonitorInfoW",     (PVOID)GetMonitorInfoW, 2, CC_STDCALL },
    { "EnumDisplayMonitors", (PVOID)EnumDisplayMonitors, 4, CC_STDCALL },
    { "EnumDisplayDevicesA", (PVOID)EnumDisplayDevicesA_u32, 4, CC_STDCALL },
    { "EnumDisplayDevicesW", (PVOID)EnumDisplayDevicesW_u32, 4, CC_STDCALL },
    { "GetDisplayConfigBufferSizes", (PVOID)GetDisplayConfigBufferSizes_u32, 3, CC_STDCALL },
    { "QueryDisplayConfig",  (PVOID)QueryDisplayConfig_u32, 6, CC_STDCALL },
    { "DisplayConfigGetDeviceInfo", (PVOID)DisplayConfigGetDeviceInfo_u32, 1, CC_STDCALL },
    { "GetDpiForMonitor",    (PVOID)GetDpiForMonitor_u32, 4, CC_STDCALL },
    { "SetProcessDpiAwareness", (PVOID)SetProcessDpiAwareness_u32, 1, CC_STDCALL },
    { "ChangeDisplaySettingsA",   (PVOID)ChangeDisplaySettingsA, 2, CC_STDCALL },
    { "ChangeDisplaySettingsW",   (PVOID)ChangeDisplaySettingsW, 2, CC_STDCALL },
    { "ChangeDisplaySettingsExA", (PVOID)ChangeDisplaySettingsExA, 5, CC_STDCALL },
    { "ChangeDisplaySettingsExW", (PVOID)ChangeDisplaySettingsExW, 5, CC_STDCALL },
    { "EnumDisplaySettingsA",     (PVOID)EnumDisplaySettingsA, 3, CC_STDCALL },
    { "EnumDisplaySettingsW",     (PVOID)EnumDisplaySettingsW, 3, CC_STDCALL },
    { "EnumDisplaySettingsExA",   (PVOID)EnumDisplaySettingsExA, 4, CC_STDCALL },
    { "EnumDisplaySettingsExW",   (PVOID)EnumDisplaySettingsExW, 4, CC_STDCALL },
    { "GetWindowLongA",     (PVOID)GetWindowLongA, 2, CC_STDCALL },
    { "SetWindowLongA",     (PVOID)SetWindowLongA, 3, CC_STDCALL },
    { "GetWindowLongPtrA",  (PVOID)GetWindowLongPtrA, 2, CC_STDCALL },
    { "SetWindowLongPtrA",  (PVOID)SetWindowLongPtrA, 3, CC_STDCALL },
    { "GetForegroundWindow",(PVOID)GetForegroundWindow, 0, CC_STDCALL },
    { "FindWindowA",        (PVOID)FindWindowA, 2, CC_STDCALL },
    { "FindWindowW",        (PVOID)FindWindowW, 2, CC_STDCALL },
    { "WindowFromPoint",    (PVOID)WindowFromPoint, 2, CC_STDCALL },
    { "GetAncestor",        (PVOID)GetAncestor, 2, CC_STDCALL },
    { "GetWindow",          (PVOID)GetWindow, 2, CC_STDCALL },
    { "GetTopWindow",       (PVOID)GetTopWindow, 1, CC_STDCALL },
    { "BringWindowToTop",   (PVOID)BringWindowToTop, 1, CC_STDCALL },
    { "SetFocus",           (PVOID)SetFocus, 1, CC_STDCALL },
    { "GetDesktopWindow",   (PVOID)GetDesktopWindow, 0, CC_STDCALL },
    { "GetShellWindow",     (PVOID)GetShellWindow,   0, CC_STDCALL },
    { "CloseDesktop",       (PVOID)CloseDesktop_stub, 1, CC_STDCALL },
    { "CloseWindowStation", (PVOID)CloseWindowStation_stub, 1, CC_STDCALL },
    { "GetProcessWindowStation", (PVOID)GetProcessWindowStation_stub, 0, CC_STDCALL },
    { "CreateWindowStationW", (PVOID)CreateWindowStationW_stub, 4, CC_STDCALL },
    { "GetThreadDesktop",   (PVOID)GetThreadDesktop_stub, 1, CC_STDCALL },
    { "SetProcessWindowStation", (PVOID)SetProcessWindowStation_stub, 1, CC_STDCALL },
    { "CreateDesktopW",     (PVOID)CreateDesktopW_stub, 6, CC_STDCALL },
    { "GetUserObjectInformationA", (PVOID)GetUserObjectInformationA_stub, 5, CC_STDCALL },
    { "GetUserObjectInformationW", (PVOID)GetUserObjectInformationW_stub, 5, CC_STDCALL },
    { "GetActiveWindow",    (PVOID)GetActiveWindow, 0, CC_STDCALL },
    /* Cursor / input */
    { "CreateCaret",       (PVOID)CreateCaret, 4, CC_STDCALL },
    { "DestroyCaret",      (PVOID)DestroyCaret, 0, CC_STDCALL },
    { "SetCaretPos",       (PVOID)SetCaretPos, 2, CC_STDCALL },
    { "SetCursorPos",       (PVOID)SetCursorPos, 2, CC_STDCALL },
    { "GetCursorPos",       (PVOID)GetCursorPos, 1, CC_STDCALL },
    { "GetCursorInfo",      (PVOID)GetCursorInfo, 1, CC_STDCALL },
    { "GetLastInputInfo",   (PVOID)GetLastInputInfo, 1, CC_STDCALL },
    { "ShowCursor",         (PVOID)ShowCursor, 1, CC_STDCALL },
    { "ClipCursor",         (PVOID)ClipCursor, 1, CC_STDCALL },
    { "GetClipCursor",      (PVOID)GetClipCursor, 1, CC_STDCALL },
    { "TrackMouseEvent",    (PVOID)TrackMouseEvent, 1, CC_STDCALL },
    { "SetCapture",         (PVOID)SetCapture, 1, CC_STDCALL },
    { "GetCapture",         (PVOID)GetCapture, 0, CC_STDCALL },
    { "ReleaseCapture",     (PVOID)ReleaseCapture, 0, CC_STDCALL },
    { "SendInput",          (PVOID)SendInput, 3, CC_STDCALL },
    { "GetAsyncKeyState",   (PVOID)GetAsyncKeyState, 1, CC_STDCALL },
    { "GetKeyboardLayout",  (PVOID)GetKeyboardLayout_k32, 1, CC_STDCALL },
    { "GetKeyboardLayoutList", (PVOID)GetKeyboardLayoutList_k32, 2, CC_STDCALL },
    { "ImmGetIMEFileNameW", (PVOID)ImmGetIMEFileNameW_k32, 3, CC_STDCALL },
    { "ImmGetIMEFileNameA", (PVOID)ImmGetIMEFileNameA_k32, 3, CC_STDCALL },
    { "ImmCreateContext",   (PVOID)ImmCreateContext_k32, 0, CC_STDCALL },
    { "ImmDestroyContext",  (PVOID)ImmDestroyContext_k32, 1, CC_STDCALL },
    { "ImmGetContext",      (PVOID)ImmGetContext_k32, 1, CC_STDCALL },
    { "ImmReleaseContext",  (PVOID)ImmReleaseContext_k32, 2, CC_STDCALL },
    { "ImmAssociateContext",(PVOID)ImmAssociateContext_k32, 2, CC_STDCALL },
    { "ImmGetOpenStatus",   (PVOID)ImmGetOpenStatus_k32, 1, CC_STDCALL },
    { "ImmSetOpenStatus",   (PVOID)ImmSetOpenStatus_k32, 2, CC_STDCALL },
    { "ImmGetConversionStatus",
      (PVOID)ImmGetConversionStatus_k32, 3, CC_STDCALL },
    { "ImmSetConversionStatus",
      (PVOID)ImmSetConversionStatus_k32, 3, CC_STDCALL },
    { "ImmGetProperty",     (PVOID)ImmGetProperty_k32, 2, CC_STDCALL },
    { "ImmGetCompositionStringW",
      (PVOID)ImmGetCompositionStringW_k32, 4, CC_STDCALL },
    { "ImmSetCompositionStringW",
      (PVOID)ImmSetCompositionStringW_k32, 6, CC_STDCALL },
    { "ImmGetCandidateListW",(PVOID)ImmGetCandidateListW_k32, 4, CC_STDCALL },
    { "ImmGetCandidateListCountW",
      (PVOID)ImmGetCandidateListCountW_k32, 2, CC_STDCALL },
    { "ImmGetCompositionFontW",
      (PVOID)ImmGetCompositionFontW_k32, 2, CC_STDCALL },
    { "ImmNotifyIME",       (PVOID)ImmNotifyIME_k32, 4, CC_STDCALL },
    { "ImmSetCompositionWindow",
      (PVOID)ImmSetCompositionWindow_k32, 2, CC_STDCALL },
    { "ImmSetCandidateWindow",
      (PVOID)ImmSetCandidateWindow_k32, 2, CC_STDCALL },
    { "GetDoubleClickTime", (PVOID)GetDoubleClickTime_k32, 0, CC_STDCALL },
    { "GetCaretBlinkTime",  (PVOID)GetCaretBlinkTime_k32, 0, CC_STDCALL },
    { "GetGuiResources",    (PVOID)GetGuiResources_k32, 2, CC_STDCALL },
    { "GetKeyState",        (PVOID)GetKeyState, 1, CC_STDCALL },
    { "GetKeyboardState",   (PVOID)GetKeyboardState, 1, CC_STDCALL },
    { "SetKeyboardState",   (PVOID)SetKeyboardState, 1, CC_STDCALL },
    { "GetMessageExtraInfo",(PVOID)GetMessageExtraInfo, 0, CC_STDCALL },
    { "SetMessageExtraInfo",(PVOID)SetMessageExtraInfo, 1, CC_STDCALL },
    { "MapVirtualKeyA",     (PVOID)MapVirtualKeyA, 2, CC_STDCALL },
    { "MapVirtualKeyW",     (PVOID)MapVirtualKeyW, 2, CC_STDCALL },
    { "MapVirtualKeyExA",   (PVOID)MapVirtualKeyExA, 3, CC_STDCALL },
    { "MapVirtualKeyExW",   (PVOID)MapVirtualKeyExW, 3, CC_STDCALL },
    { "GetKeyNameTextA",    (PVOID)GetKeyNameTextA, 3, CC_STDCALL },
    { "ToAscii",            (PVOID)ToAscii, 5, CC_STDCALL },
    { "ToAsciiEx",          (PVOID)ToAsciiEx, 6, CC_STDCALL },
    { "ToUnicode",          (PVOID)ToUnicode, 6, CC_STDCALL },
    { "ToUnicodeEx",        (PVOID)ToUnicodeEx, 7, CC_STDCALL },
    /* Misc */
    { "MessageBoxA",        (PVOID)MessageBoxA, 4, CC_STDCALL },
    { "MessageBoxW",        (PVOID)MessageBoxW, 4, CC_STDCALL },
    { "MessageBoxIndirectW",(PVOID)MessageBoxIndirectW_k32, 1, CC_STDCALL },
    { "wsprintfA",          (PVOID)wsprintfA_k32, 12, CC_CDECL },
    { "wsprintfW",          (PVOID)wsprintfW_k32, 12, CC_CDECL },
    { "LoadCursorA",        (PVOID)LoadCursorA, 2, CC_STDCALL },
    { "LoadIconA",          (PVOID)LoadIconA, 2, CC_STDCALL },
    { "LoadIconW",          (PVOID)LoadIconW, 2, CC_STDCALL },
    { "CreateIconIndirect", (PVOID)CreateIconIndirect, 1, CC_STDCALL },
    { "CopyIcon",           (PVOID)CopyIcon, 1, CC_STDCALL },
    { "GetIconInfo",        (PVOID)GetIconInfo, 2, CC_STDCALL },
    { "DrawIconEx",         (PVOID)DrawIconEx, 9, CC_STDCALL },
    { "DestroyIcon",        (PVOID)DestroyIcon,     1, CC_STDCALL },
    { "GetDC",              (PVOID)GetDC, 1, CC_STDCALL },
    { "GetWindowDC",        (PVOID)GetWindowDC, 1, CC_STDCALL },
    { "WindowFromDC",       (PVOID)WindowFromDC, 1, CC_STDCALL },
    { "ReleaseDC",          (PVOID)ReleaseDC, 2, CC_STDCALL },
    { "InvalidateRect",     (PVOID)InvalidateRect, 3, CC_STDCALL },
    { "RedrawWindow",       (PVOID)RedrawWindow, 4, CC_STDCALL },
    { "SetForegroundWindow",(PVOID)SetForegroundWindow, 1, CC_STDCALL },
    { "AllowSetForegroundWindow", (PVOID)AllowSetForegroundWindow, 1, CC_STDCALL },
    /* Dialog */
    { "CreateDialogParamA", (PVOID)CreateDialogParamA, 5, CC_STDCALL },
    { "CreateDialogParamW", (PVOID)CreateDialogParamW, 5, CC_STDCALL },
    { "CreateDialogIndirectParamA", (PVOID)CreateDialogIndirectParamA,
      5, CC_STDCALL },
    { "CreateDialogIndirectParamW", (PVOID)CreateDialogIndirectParamW,
      5, CC_STDCALL },
    { "EndDialog",          (PVOID)EndDialog, 2, CC_STDCALL },
    { "GetDlgItem",         (PVOID)GetDlgItem, 2, CC_STDCALL },
    { "GetDlgItemInt",      (PVOID)GetDlgItemInt, 4, CC_STDCALL },
    { "SetDlgItemInt",      (PVOID)SetDlgItemInt, 4, CC_STDCALL },
    { "SetDlgItemTextA",    (PVOID)SetDlgItemTextA, 3, CC_STDCALL },
    { "GetDlgCtrlID",       (PVOID)GetDlgCtrlID, 1, CC_STDCALL },
    /* Window search */
    { "FindWindowExA",      (PVOID)FindWindowExA, 4, CC_STDCALL },
    { "FindWindowExW",      (PVOID)FindWindowExW, 4, CC_STDCALL },
    /* Wide message loop */
    { "PeekMessageW",       (PVOID)PeekMessageW, 5, CC_STDCALL },
    { "GetMessageW",        (PVOID)GetMessageW, 4, CC_STDCALL },
    { "DispatchMessageW",   (PVOID)DispatchMessageW, 1, CC_STDCALL },
    { "SendMessageW",       (PVOID)SendMessageW, 4, CC_STDCALL },
    { "SendMessageTimeoutW",(PVOID)SendMessageTimeoutW, 7, CC_STDCALL },
    /* Thread messages */
    { "PostThreadMessageA", (PVOID)PostThreadMessageA, 4, CC_STDCALL },
    { "PostThreadMessageW", (PVOID)PostThreadMessageW, 4, CC_STDCALL },
    /* Window properties */
    { "GetPropA",           (PVOID)GetPropA, 2, CC_STDCALL },
    { "GetPropW",           (PVOID)GetPropW, 2, CC_STDCALL },
    { "SetPropA",           (PVOID)SetPropA, 3, CC_STDCALL },
    { "SetPropW",           (PVOID)SetPropW, 3, CC_STDCALL },
    { "RemovePropA",        (PVOID)RemovePropA, 2, CC_STDCALL },
    { "RemovePropW",        (PVOID)RemovePropW, 2, CC_STDCALL },
    /* Window thread */
    { "GetWindowThreadProcessId", (PVOID)GetWindowThreadProcessId, 2, CC_STDCALL },
    { "EnumWindows",        (PVOID)EnumWindows, 2, CC_STDCALL },
    { "EnumThreadWindows",  (PVOID)EnumThreadWindows, 3, CC_STDCALL },
    /* Clipboard */
    { "OpenClipboard",      (PVOID)OpenClipboard, 1, CC_STDCALL },
    { "CloseClipboard",     (PVOID)CloseClipboard, 0, CC_STDCALL },
    { "EmptyClipboard",     (PVOID)EmptyClipboard, 0, CC_STDCALL },
    { "SetClipboardData",   (PVOID)SetClipboardData, 2, CC_STDCALL },
    { "GetClipboardData",   (PVOID)GetClipboardData, 1, CC_STDCALL },
    { "RegisterClipboardFormatA", (PVOID)RegisterClipboardFormatA, 1, CC_STDCALL },
    { "RegisterClipboardFormatW", (PVOID)RegisterClipboardFormatW, 1, CC_STDCALL },
    { "GetClipboardFormatNameA", (PVOID)GetClipboardFormatNameA, 3, CC_STDCALL },
    { "GetClipboardFormatNameW", (PVOID)GetClipboardFormatNameW, 3, CC_STDCALL },
    { "EnumClipboardFormats", (PVOID)EnumClipboardFormats, 1, CC_STDCALL },
    { "GetClipboardSequenceNumber", (PVOID)GetClipboardSequenceNumber, 0, CC_STDCALL },
    { "IsClipboardFormatAvailable", (PVOID)IsClipboardFormatAvailable, 1, CC_STDCALL },
    /* Paint / drawing */
    { "BeginPaint",         (PVOID)BeginPaint, 2, CC_STDCALL },
    { "EndPaint",           (PVOID)EndPaint, 2, CC_STDCALL },
    { "CallWindowProcA",    (PVOID)CallWindowProcA, 5, CC_STDCALL },
    { "CallWindowProcW",    (PVOID)CallWindowProcW, 5, CC_STDCALL },
    { "DefWindowProcW",     (PVOID)DefWindowProcW, 4, CC_STDCALL },
    { "DefMDIChildProcA",   (PVOID)DefMDIChildProcA, 4, CC_STDCALL },
    { "DefMDIChildProcW",   (PVOID)DefMDIChildProcW, 4, CC_STDCALL },
    { "CreateWindowExW",    (PVOID)CreateWindowExW, 12, CC_STDCALL },
    { "RegisterClassExW",   (PVOID)RegisterClassExW, 1, CC_STDCALL },
    { "GetClassInfoExA",    (PVOID)GetClassInfoExA, 3, CC_STDCALL },
    { "GetClassInfoExW",    (PVOID)GetClassInfoExW, 3, CC_STDCALL },
    { "GetWindowLongW",     (PVOID)GetWindowLongW, 2, CC_STDCALL },
    { "SetWindowLongW",     (PVOID)SetWindowLongW, 3, CC_STDCALL },
    { "GetWindowLongPtrW",  (PVOID)GetWindowLongPtrW, 2, CC_STDCALL },
    { "SetWindowLongPtrW",  (PVOID)SetWindowLongPtrW, 3, CC_STDCALL },
    { "IsWindow",           (PVOID)IsWindow, 1, CC_STDCALL },
    { "IsIconic",           (PVOID)IsIconic, 1, CC_STDCALL },
    { "IsZoomed",           (PVOID)IsZoomed, 1, CC_STDCALL },
    { "IsWindowEnabled",    (PVOID)IsWindowEnabled, 1, CC_STDCALL },
    { "IsChild",            (PVOID)IsChild, 2, CC_STDCALL },
    { "GetParent",          (PVOID)GetParent, 1, CC_STDCALL },
    { "EnumChildWindows",   (PVOID)EnumChildWindows, 3, CC_STDCALL },
    { "ClientToScreen",     (PVOID)ClientToScreen, 2, CC_STDCALL },
    { "ScreenToClient",     (PVOID)ScreenToClient, 2, CC_STDCALL },
    { "GetUpdateRect",      (PVOID)GetUpdateRect, 3, CC_STDCALL },
    { "FillRect",           (PVOID)FillRect, 3, CC_STDCALL },
    { "DrawFocusRect",      (PVOID)DrawFocusRect, 2, CC_STDCALL },
    { "DrawTextA",          (PVOID)DrawTextA, 5, CC_STDCALL },
    { "DrawTextW",          (PVOID)DrawTextW, 5, CC_STDCALL },
    { "DrawTextExA",        (PVOID)DrawTextExA, 6, CC_STDCALL },
    { "DrawTextExW",        (PVOID)DrawTextExW, 6, CC_STDCALL },
    { "GetSysColor",        (PVOID)GetSysColor, 1, CC_STDCALL },
    { "GetSysColorBrush",   (PVOID)GetSysColorBrush_u32, 1, CC_STDCALL },
    /* Dialog box */
    { "DialogBoxParamA",    (PVOID)DialogBoxParamA, 5, CC_STDCALL },
    { "DialogBoxParamW",    (PVOID)DialogBoxParamW, 5, CC_STDCALL },
    { "DialogBoxIndirectParamA", (PVOID)DialogBoxIndirectParamA,
      5, CC_STDCALL },
    { "DialogBoxIndirectParamW", (PVOID)DialogBoxIndirectParamW,
      5, CC_STDCALL },
    /* Menu */
    { "CreateMenu",         (PVOID)CreateMenu, 0, CC_STDCALL },
    { "CreatePopupMenu",    (PVOID)CreatePopupMenu, 0, CC_STDCALL },
    { "DestroyMenu",        (PVOID)DestroyMenu, 1, CC_STDCALL },
    { "IsMenu",             (PVOID)IsMenu, 1, CC_STDCALL },
    { "GetSystemMenu",      (PVOID)GetSystemMenu, 2, CC_STDCALL },
    { "EnableMenuItem",     (PVOID)EnableMenuItem, 3, CC_STDCALL },
    { "InsertMenuItemA",    (PVOID)InsertMenuItemA, 4, CC_STDCALL },
    { "InsertMenuItemW",    (PVOID)InsertMenuItemW, 4, CC_STDCALL },
    { "GetMenuInfo",        (PVOID)GetMenuInfo, 2, CC_STDCALL },
    { "SetMenuInfo",        (PVOID)SetMenuInfo, 2, CC_STDCALL },
    { "SetMenuDefaultItem", (PVOID)SetMenuDefaultItem, 3, CC_STDCALL },
    { "GetMenuDefaultItem", (PVOID)GetMenuDefaultItem, 3, CC_STDCALL },
    { "LoadMenuA",          (PVOID)LoadMenuA, 2, CC_STDCALL },
    { "LoadMenuW",          (PVOID)LoadMenuW, 2, CC_STDCALL },
    { "GetSubMenu",         (PVOID)GetSubMenu, 2, CC_STDCALL },
    { "GetMenuItemCount",   (PVOID)GetMenuItemCount, 1, CC_STDCALL },
    { "GetMenuState",       (PVOID)GetMenuState, 3, CC_STDCALL },
    { "GetMenuItemInfoA",   (PVOID)GetMenuItemInfoA, 4, CC_STDCALL },
    { "GetMenuItemInfoW",   (PVOID)GetMenuItemInfoW, 4, CC_STDCALL },
    { "SetMenuItemInfoA",   (PVOID)SetMenuItemInfoA, 4, CC_STDCALL },
    { "SetMenuItemInfoW",   (PVOID)SetMenuItemInfoW, 4, CC_STDCALL },
    { "CheckMenuItem",      (PVOID)CheckMenuItem, 3, CC_STDCALL },
    { "TrackPopupMenu",     (PVOID)TrackPopupMenu, 7, CC_STDCALL },
    /* String navigation */
    { "CharNextA",          (PVOID)CharNextA_k32, 1, CC_STDCALL },
    { "CharNextW",          (PVOID)CharNextW_k32, 1, CC_STDCALL },
    { "CharPrevW",          (PVOID)CharPrevW_k32, 2, CC_STDCALL },
    /* Cursor / misc */
    { "GetCursor",          (PVOID)GetCursor, 0, CC_STDCALL },
    { "SetCursor",          (PVOID)SetCursor, 1, CC_STDCALL },
    { "LoadCursorW",        (PVOID)LoadCursorW, 2, CC_STDCALL },
    { "DestroyCursor",      (PVOID)DestroyCursor, 1, CC_STDCALL },
    { "LoadImageA",         (PVOID)LoadImageA, 6, CC_STDCALL },
    { "LoadImageW",         (PVOID)LoadImageW, 6, CC_STDCALL },
    { "CopyImage",          (PVOID)CopyImage, 5, CC_STDCALL },
    { "RegisterWindowMessageA", (PVOID)RegisterWindowMessageA, 1, CC_STDCALL },
    { "RegisterWindowMessageW", (PVOID)RegisterWindowMessageW, 1, CC_STDCALL },
    { "PostMessageW",       (PVOID)PostMessageW, 4, CC_STDCALL },
    /* Additional stubs */
    { "EnableWindow",           (PVOID)EnableWindow, 2, CC_STDCALL },
    { "GetMenu",                (PVOID)GetMenu, 1, CC_STDCALL },
    { "GetMessageTime",         (PVOID)GetMessageTime, 0, CC_STDCALL },
    { "GetMessagePos",          (PVOID)GetMessagePos, 0, CC_STDCALL },
    { "GetFocus",               (PVOID)GetFocus, 0, CC_STDCALL },
    { "IsWindowVisible",        (PVOID)IsWindowVisible, 1, CC_STDCALL },
    { "MapWindowPoints",        (PVOID)MapWindowPoints, 4, CC_STDCALL },
    { "RegisterHotKey",         (PVOID)RegisterHotKey, 4, CC_STDCALL },
    { "SetMenu",                (PVOID)SetMenu, 2, CC_STDCALL },
    { "SetParent",              (PVOID)SetParent, 2, CC_STDCALL },
    { "SetActiveWindow",        (PVOID)SetActiveWindow, 1, CC_STDCALL },
    { "SystemParametersInfoW",  (PVOID)SystemParametersInfoW, 4, CC_STDCALL },
    { "UnregisterHotKey",       (PVOID)UnregisterHotKey, 2, CC_STDCALL },
    { "ValidateRect",           (PVOID)ValidateRect, 2, CC_STDCALL },
    { "SetWinEventHook",        (PVOID)SetWinEventHook_k32, 7, CC_STDCALL },
    { "UnhookWinEvent",         (PVOID)UnhookWinEvent_k32, 1, CC_STDCALL },
    { "NotifyWinEvent",         (PVOID)NotifyWinEvent_k32, 4, CC_STDCALL },
    { "WTSFreeMemory",          (PVOID)WTSFreeMemory_u32, 1, CC_STDCALL },
    { "WTSQuerySessionInformationW", (PVOID)WTSQuerySessionInformationW_u32, 5, CC_STDCALL },
    { "WTSRegisterSessionNotification", (PVOID)WTSRegisterSessionNotification_u32, 2, CC_STDCALL },
    { "WTSUnRegisterSessionNotification", (PVOID)WTSUnRegisterSessionNotification_u32, 1, CC_STDCALL },
    { "DwmDefWindowProc",         (PVOID)DwmDefWindowProc_u32, 5, CC_STDCALL },
    { "DwmExtendFrameIntoClientArea", (PVOID)DwmExtendFrameIntoClientArea_u32, 2, CC_STDCALL },
    { "DwmGetCompositionTimingInfo", (PVOID)DwmGetCompositionTimingInfo_u32, 2, CC_STDCALL },
    { "DwmGetWindowAttribute",    (PVOID)DwmGetWindowAttribute_u32, 4, CC_STDCALL },
    { "DwmIsCompositionEnabled",  (PVOID)DwmIsCompositionEnabled_u32, 1, CC_STDCALL },
    { "DwmSetWindowAttribute",    (PVOID)DwmSetWindowAttribute_u32, 4, CC_STDCALL },
    { NULL, NULL, 0, CC_STDCALL }
};

const WIN32_EXPORT *user32_abi_table(int *count)
{
    *count = (int)(sizeof(user32_exports) / sizeof(user32_exports[0]));
    return (const WIN32_EXPORT *)user32_exports;
}

PVOID user32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal) return NULL;
    for (int i = 0; user32_exports[i].name; i++) {
        if (u32_strcmp(func_name, user32_exports[i].name) == 0)
            return user32_exports[i].func;
    }
    return NULL;
}

PVOID user32_shim_init(void)
{
    icon_release_all();
    user_icon_lock = SPINLOCK_INIT;
    wndclass_count = 0;
    window_count = 0;
    memset(&user_display_mode, 0, sizeof(user_display_mode));
    defer_sync_depth = 0;
    show_trace_count = 0;
    next_hdwp = 0xD5000001;
    for (int i = 0; i < MAX_DEFER_WINDOW_POS; i++)
        defer_window_sets[i].used = 0;
    process_dpi_context = (HANDLE)(LONG_PTR)-2;
    thread_dpi_context = NULL;
    user_object_reset();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].used = 0;
        dialog_states[i].used = FALSE;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        imm_associations[i].window = NULL;
        imm_associations[i].context = NULL;
    }
    for (int i = 0; i < MAX_IMM_CONTEXTS; i++)
        imm_contexts[i].used = 0;
    next_imm_context = 0x1A000001;
    msg_head = msg_tail = 0;
    queue_changed_status = 0;
    msg_queue_lock = SPINLOCK_INIT;
    msg_wait_event_lock = SPINLOCK_INIT;
    for (int i = 0; i < MSG_WAIT_EVENT_SLOTS; i++)
        msg_wait_events[i].used = FALSE;
    user_timer_lock = SPINLOCK_INIT;
    next_user_timer_id = 1;
    for (int i = 0; i < USER_TIMER_SLOTS; i++) {
        user_timers[i].used = FALSE;
        user_timers[i].pending = FALSE;
    }
    user_hook_lock = SPINLOCK_INIT;
    next_user_hook_handle = 0xD6000001;
    next_user_hook_sequence = 1;
    for (int i = 0; i < USER_HOOK_SLOTS; i++)
        user_hooks[i].used = FALSE;
    for (int i = 0; i < USER_HOOK_CONTEXT_SLOTS; i++) {
        user_hook_contexts[i].used = FALSE;
        user_hook_contexts[i].depth = 0;
        user_hook_contexts[i].windowpos_depth = 0;
        user_hook_contexts[i].create_depth = 0;
        user_hook_contexts[i].scratch_page = NULL;
    }
    for (int i = 0; i < MSG_QUEUE_SIZE; i++) {
        msg_target_pid[i] = 0;
        msg_target_tid[i] = 0;
        msg_extra_info[i] = 0;
        msg_source[i] = MSG_SOURCE_POSTED;
        msg_input_sequence[i] = 0;
    }
    next_input_sequence = 0;
    for (int i = 0; i < INPUT_SEQUENCE_STATE_SLOTS; i++)
        input_sequence_states[i].used = FALSE;
    g_last_msg_time = 0;
    g_last_input_time = shim_timeGetTime();
    g_last_msg_pos.x = g_last_msg_pos.y = 0;
    for (int i = 0; i < 256; i++) { key_state[i] = 0; async_pressed[i] = 0; }
    mouse_buttons = 0;
    message_extra_info = 0;
    prev_was_e0 = 0;
    keyboard_alt_pending = FALSE;
    /* Re-exec resets input and activation state. */
    focus_hwnd = NULL;
    active_hwnd = NULL;
    user32_foreground_active = FALSE;
    capture_hwnd = NULL;
    native_move.window = NULL;
    native_move.pointer_offset_x = 0;
    native_move.pointer_offset_y = 0;
    clip_active = 0;
    clip_owner_pid = 0;
    clip_owner_tid = 0;
    relative_pointer_reset();
    cursor_release_counts(0, 0);
    current_cursor = NULL;
    int cursor_width = current_mode_cx();
    int cursor_height = current_mode_cy();
    if (cursor_width < 1) cursor_width = USER32_FALLBACK_SCREEN_WIDTH;
    if (cursor_height < 1) cursor_height = USER32_FALLBACK_SCREEN_HEIGHT;
    cursor_pos.x = cursor_width / 2;
    cursor_pos.y = cursor_height / 2;
    clip_rect = (RECT){ 0, 0, cursor_width, cursor_height };
    caret_hwnd = NULL;
    caret_pos.x = caret_pos.y = 0;
    return (PVOID)user32_exports;
}
