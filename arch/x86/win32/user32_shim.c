/*
 * OsitoK Windows Compatibility Layer — user32.dll Shim Implementation
 *
 * All windows map to a single OsitoK framebuffer.
 * The message queue is a ring buffer fed by keyboard/mouse drivers.
 * In TEST_HARNESS mode, the queue is always empty (no real input).
 */

#include "user32_shim.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* ── OsitoK compositor integration (weak — NULL in test harness) ── */
extern uint32_t shm_create_surface(uint32_t w, uint32_t h, uint32_t flags)
    __attribute__((weak));
extern void    *shm_map(uint32_t handle)   __attribute__((weak));
extern void     shm_unmap(uint32_t handle) __attribute__((weak));
extern void     shm_destroy(uint32_t handle) __attribute__((weak));
extern uint32_t compositor_create_window(uint32_t shm_handle,
    int16_t x, int16_t y, uint16_t width, uint16_t height,
    uint32_t pid, const char *title) __attribute__((weak));
extern void compositor_destroy_window(uint32_t window_id) __attribute__((weak));
extern void compositor_signal_dirty(uint32_t window_id)   __attribute__((weak));

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

#define MAX_WNDCLASSES 64

typedef struct {
    char        class_name[128];
    WNDPROC     wndproc;
    DWORD       style;
    int         cbWndExtra;
    int         used;
} WNDCLASS_ENTRY;

static WNDCLASS_ENTRY wndclasses[MAX_WNDCLASSES];
static int wndclass_count = 0;

static WNDCLASS_ENTRY *find_class(const char *name)
{
    for (int i = 0; i < wndclass_count; i++) {
        if (wndclasses[i].used && u32_stricmp(wndclasses[i].class_name, name) == 0)
            return &wndclasses[i];
    }
    return NULL;
}

/* ── Window objects ────────────────────────────────────────── */

#define MAX_WINDOWS 16

typedef struct {
    HWND        handle;
    char        class_name[128];
    char        title[256];
    WNDPROC     wndproc;
    DWORD       style;
    DWORD       ex_style;
    int         x, y, width, height;
    HWND        parent;
    PVOID       user_data;
    int         visible;
    int         used;
    /* OsitoK compositor integration */
    uint32_t    shm_handle;        /* shared memory surface handle (0 = none) */
    uint32_t    compositor_id;     /* compositor window ID (0 = none) */
    void       *shm_pixels;        /* mapped shm surface pointer */
} WINDOW;

static WINDOW windows[MAX_WINDOWS];
static int window_count = 0;
static ULONG_PTR next_hwnd = 0xA0000001;

/* ── Compositor integration API (called by ddraw_shim) ─────── */

/* Get the shm pixel buffer for a window (NULL if no compositor) */
void *user32_get_window_shm_pixels(HWND hwnd);
uint32_t user32_get_window_compositor_id(HWND hwnd);

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

static WINDOW *find_window(HWND hwnd)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].handle == hwnd)
            return &windows[i];
    }
    return NULL;
}

/* ── Message queue ─────────────────────────────────────────── */

#define MSG_QUEUE_SIZE 256

static MSG msg_queue[MSG_QUEUE_SIZE];
static int msg_head = 0, msg_tail = 0;
static int quit_posted = 0;
static int quit_code = 0;

static int msg_queue_empty(void)
{
    return msg_head == msg_tail;
}

static void msg_enqueue(HWND hwnd, DWORD message, WPARAM wp, LPARAM lp)
{
    int next = (msg_tail + 1) % MSG_QUEUE_SIZE;
    if (next == msg_head) return; /* full — drop */
    msg_queue[msg_tail].hwnd    = hwnd;
    msg_queue[msg_tail].message = message;
    msg_queue[msg_tail].wParam  = wp;
    msg_queue[msg_tail].lParam  = lp;
    msg_queue[msg_tail].time    = 0;
    msg_queue[msg_tail].pt.x    = 0;
    msg_queue[msg_tail].pt.y    = 0;
    msg_tail = next;
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
    extern int g_compat32_mode;
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
    extern int g_compat32_mode;
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

static int msg_dequeue(void *out)
{
    if (msg_head == msg_tail) return 0;
    MSG *src = &msg_queue[msg_head];
    msg_write_to(out, src->hwnd, src->message, src->wParam, src->lParam,
                 src->time, src->pt.x, src->pt.y);
    msg_head = (msg_head + 1) % MSG_QUEUE_SIZE;
    return 1;
}

/* ── Input state ──────────────────────────────────────────── */

/*
 * key_state[vk]: bit 0 = toggled, bit 7 = currently down.
 * async_pressed[vk]: set when key goes down, cleared by GetAsyncKeyState.
 */
static BYTE key_state[256];
static BYTE async_pressed[256];
static DWORD mouse_buttons = 0; /* bit 0=left, 1=right, 2=middle */

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

static POINT cursor_pos = { 320, 240 };
static int   cursor_visible = 1;
static HWND  capture_hwnd = NULL;

/* ── Default screen dimensions ─────────────────────────────── */

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 600

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
    extern int g_compat32_mode;
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
    extern int g_compat32_mode;
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
    if (!lpwcx) return 0;

    WNDCLASSEXA wcx;
    wndclassex_read(lpwcx, &wcx);

    if (!wcx.lpszClassName) return 0;

    serial_puts("[USER32] RegisterClassExA: ");
    serial_puts(wcx.lpszClassName);
    serial_puts("\n");

    if (wndclass_count >= MAX_WNDCLASSES) return 0;

    WNDCLASS_ENTRY *e = &wndclasses[wndclass_count];
    u32_strcpy(e->class_name, wcx.lpszClassName, 128);
    e->wndproc    = wcx.lpfnWndProc;
    e->style      = wcx.style;
    e->cbWndExtra = wcx.cbWndExtra;
    e->used       = 1;
    wndclass_count++;

    return (WORD)wndclass_count; /* atom */
}

WORD WINAPI RegisterClassA(const WNDCLASSA *lpwcx)
{
    if (!lpwcx) return 0;

    WNDCLASSA wca;
    wndclass_read(lpwcx, &wca);

    if (!wca.lpszClassName) return 0;

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
    if (wndclass_count >= MAX_WNDCLASSES) return 0;
    WNDCLASS_ENTRY *e = &wndclasses[wndclass_count];
    u32_strcpy(e->class_name, ex.lpszClassName, 128);
    e->wndproc    = ex.lpfnWndProc;
    e->style      = ex.style;
    e->cbWndExtra = ex.cbWndExtra;
    e->used       = 1;
    wndclass_count++;
    return (WORD)wndclass_count;
}

BOOL WINAPI UnregisterClassA(PCSTR lpClassName, HINSTANCE hInstance)
{
    (void)hInstance;
    if (!lpClassName) return FALSE;
    WNDCLASS_ENTRY *e = find_class(lpClassName);
    if (e) { e->used = 0; return TRUE; }
    return FALSE;
}

HWND WINAPI CreateWindowExA(DWORD dwExStyle, PCSTR lpClassName,
                            PCSTR lpWindowName, DWORD dwStyle,
                            int X, int Y, int nWidth, int nHeight,
                            HWND hWndParent, HMENU hMenu,
                            HINSTANCE hInstance, PVOID lpParam)
{
    (void)hMenu;
    (void)hInstance;

    serial_puts("[USER32] CreateWindowExA: ");
    if (lpClassName) serial_puts(lpClassName);
    serial_puts(" \"");
    if (lpWindowName) serial_puts(lpWindowName);
    serial_puts("\"\n");

    if (window_count >= MAX_WINDOWS) return NULL;

    /* Find window class.
     * RegisterClassExW stores class names with potential wide→narrow corruption
     * (first char can differ). If exact match fails, use the most recently
     * registered class as fallback — classes are typically registered right
     * before their windows are created. */
    WNDPROC wndproc = NULL;
    if (lpClassName) {
        WNDCLASS_ENTRY *cls = find_class(lpClassName);
        if (cls) {
            wndproc = cls->wndproc;
        } else if (wndclass_count > 0) {
            /* Fallback: use last registered class */
            wndproc = wndclasses[wndclass_count - 1].wndproc;
        }
    }

    /* CW_USEDEFAULT */
    if (X == (int)0x80000000) X = 0;
    if (Y == (int)0x80000000) Y = 0;
    if (nWidth == (int)0x80000000) nWidth = SCREEN_WIDTH;
    if (nHeight == (int)0x80000000) nHeight = SCREEN_HEIGHT;

    WINDOW *w = &windows[window_count];
    w->handle   = (HWND)(ULONG_PTR)next_hwnd++;
    if (lpClassName) u32_strcpy(w->class_name, lpClassName, 128);
    if (lpWindowName) u32_strcpy(w->title, lpWindowName, 256);
    w->wndproc  = wndproc;
    w->style    = dwStyle;
    w->ex_style = dwExStyle;
    w->x        = X;
    w->y        = Y;
    w->width    = nWidth;
    w->height   = nHeight;
    w->parent   = hWndParent;
    w->user_data = NULL;
    w->visible  = (dwStyle & WS_VISIBLE) ? 1 : 0;
    w->used     = 1;
    w->shm_handle    = 0;
    w->compositor_id = 0;
    w->shm_pixels    = NULL;

    /* OsitoK compositor: create backing shm surface + register window */
    if (shm_create_surface && compositor_create_window && nWidth > 0 && nHeight > 0) {
        uint32_t sh = shm_create_surface((uint32_t)nWidth, (uint32_t)nHeight,
                                          SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ);
        if (sh) {
            w->shm_handle = sh;
            if (shm_map)
                w->shm_pixels = shm_map(sh);
            w->compositor_id = compositor_create_window(sh,
                (int16_t)X, (int16_t)Y, (uint16_t)nWidth, (uint16_t)nHeight,
                0, lpWindowName ? lpWindowName : "");
            serial_puts("[USER32]   compositor wid=");
            serial_puthex(w->compositor_id, 4);
            serial_puts(" shm=");
            serial_puthex(sh, 4);
            serial_puts("\n");
        }
    }

    window_count++;

    serial_puts("[USER32]   hwnd=");
    serial_puthex((uint64_t)(ULONG_PTR)w->handle, 8);
    serial_puts(" size=");
    serial_puthex(nWidth, 4);
    serial_puts("x");
    serial_puthex(nHeight, 4);
    serial_puts("\n");

    /* Send WM_NCCREATE + WM_CREATE via compat32 callback.
     * The WndProc is 32-bit code — can't call it directly from 64-bit. */
    if (wndproc) {
        extern uint32_t compat32_callback_args(uint32_t func_addr, int nargs, const uint32_t *args);
        CREATESTRUCTA cs;
        BYTE *p = (BYTE *)&cs;
        for (SIZE_T i = 0; i < sizeof(cs); i++) p[i] = 0;
        cs.lpCreateParams = lpParam;
        cs.hInstance  = hInstance;
        cs.hwndParent = hWndParent;
        cs.cx = nWidth;
        cs.cy = nHeight;
        cs.x  = X;
        cs.y  = Y;
        cs.style = (LONG)dwStyle;
        cs.lpszName  = lpWindowName;
        cs.lpszClass = lpClassName;
        cs.dwExStyle = dwExStyle;

        uint32_t wndproc32 = (uint32_t)(ULONG_PTR)wndproc;
        uint32_t nccreate_args[4] = {
            (uint32_t)(ULONG_PTR)w->handle, 0x0081 /*WM_NCCREATE*/, 0,
            (uint32_t)(ULONG_PTR)&cs };
        uint32_t create_args[4] = {
            (uint32_t)(ULONG_PTR)w->handle, 0x0001 /*WM_CREATE*/, 0,
            (uint32_t)(ULONG_PTR)&cs };

        compat32_callback_args(wndproc32, 4, nccreate_args);
        compat32_callback_args(wndproc32, 4, create_args);
    }

    return w->handle;
}

BOOL WINAPI DestroyWindow(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;

    if (w->wndproc) {
        w->wndproc(hWnd, WM_DESTROY, 0, 0);
        w->wndproc(hWnd, WM_NCDESTROY, 0, 0);
    }

    /* Clean up compositor resources */
    if (w->compositor_id && compositor_destroy_window)
        compositor_destroy_window(w->compositor_id);
    if (w->shm_handle && shm_unmap)
        shm_unmap(w->shm_handle);
    if (w->shm_handle && shm_destroy)
        shm_destroy(w->shm_handle);
    w->compositor_id = 0;
    w->shm_handle = 0;
    w->shm_pixels = NULL;

    w->used = 0;
    return TRUE;
}

BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;

    int was_visible = w->visible;
    w->visible = (nCmdShow != SW_HIDE) ? 1 : 0;

    if (w->wndproc)
        w->wndproc(hWnd, WM_SHOWWINDOW, w->visible, 0);

    return was_visible;
}

BOOL WINAPI UpdateWindow(HWND hWnd)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;

    if (w->wndproc && w->visible)
        w->wndproc(hWnd, WM_PAINT, 0, 0);

    return TRUE;
}

BOOL WINAPI SetWindowTextA(HWND hWnd, PCSTR lpString)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    if (lpString) u32_strcpy(w->title, lpString, 256);
    return TRUE;
}

BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter,
                         int X, int Y, int cx, int cy, DWORD uFlags)
{
    (void)hWndInsertAfter;
    (void)uFlags;
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    w->x = X; w->y = Y; w->width = cx; w->height = cy;
    return TRUE;
}

BOOL WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return FALSE;
    w->x = X; w->y = Y; w->width = nWidth; w->height = nHeight;
    if (bRepaint && w->wndproc && w->visible)
        w->wndproc(hWnd, WM_PAINT, 0, 0);
    return TRUE;
}

/* ── Message Loop ──────────────────────────────────────────── */

BOOL WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                          DWORD wMsgFilterMax, DWORD wRemoveMsg)
{
    (void)hWnd;
    (void)wMsgFilterMin;
    (void)wMsgFilterMax;

    static int peek_log_count = 0;
    if (peek_log_count < 3) {
        serial_puts("[USER32] PeekMessageA called\n");
        peek_log_count++;
    }

    if (quit_posted && msg_queue_empty()) {
        msg_write_to(lpMsg, NULL, WM_QUIT, (WPARAM)quit_code, 0, 0, 0, 0);
        if (wRemoveMsg & PM_REMOVE)
            quit_posted = 0;
        return TRUE;
    }

    if (msg_queue_empty()) return FALSE;

    if (wRemoveMsg & PM_REMOVE) {
        return msg_dequeue(lpMsg) ? TRUE : FALSE;
    } else {
        /* Peek without removing */
        MSG *src = &msg_queue[msg_head];
        msg_write_to(lpMsg, src->hwnd, src->message, src->wParam,
                     src->lParam, src->time, src->pt.x, src->pt.y);
        return TRUE;
    }
}

BOOL WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, DWORD wMsgFilterMin,
                         DWORD wMsgFilterMax)
{
    (void)hWnd;
    (void)wMsgFilterMin;
    (void)wMsgFilterMax;

    /* Block until a message is available */
    /* In a real kernel, this would yield. In test mode, immediately check. */
    if (quit_posted) {
        msg_write_to(lpMsg, NULL, WM_QUIT, (WPARAM)quit_code, 0, 0, 0, 0);
        return FALSE; /* WM_QUIT → return FALSE to exit loop */
    }

    if (msg_dequeue(lpMsg))
        return TRUE;

#ifdef TEST_HARNESS
    /* In test harness, return WM_QUIT to prevent infinite loop */
    msg_write_to(lpMsg, NULL, WM_QUIT, 0, 0, 0, 0, 0);
    return FALSE;
#else
    /* On bare metal, yield CPU and retry — real apps expect GetMessage to block.
     * Post WM_TIMER periodically so the app's message loop keeps running. */
    msg_write_to(lpMsg, NULL, 0x0113 /* WM_TIMER */, 1, 0, 0, 0, 0);
    /* Brief yield — sti;hlt;cli lets pending interrupts fire */
    __asm__ volatile ("sti; hlt; cli" ::: "memory");
    return TRUE;
#endif
}

BOOL WINAPI TranslateMessage(const MSG *lpMsg)
{
    /* Read MSG from caller buffer (handles 32-bit vs 64-bit layout) */
    MSG m;
    msg_read_from(lpMsg, &m);

    /* Generate WM_CHAR from WM_KEYDOWN — simplified */
    if (m.message == WM_KEYDOWN) {
        DWORD vk = (DWORD)m.wParam;
        if (vk >= 0x20 && vk <= 0x7E) {
            msg_enqueue(m.hwnd, WM_CHAR, m.wParam, m.lParam);
        }
    }
    return TRUE;
}

LRESULT WINAPI DispatchMessageA(const MSG *lpMsg)
{
    /* Read MSG from caller buffer (handles 32-bit vs 64-bit layout) */
    MSG m;
    msg_read_from(lpMsg, &m);

    if (m.message == WM_QUIT)
        return 0;

    WINDOW *w = find_window(m.hwnd);
    if (w && w->wndproc) {
        return w->wndproc(m.hwnd, m.message, m.wParam, m.lParam);
    }

    return DefWindowProcA(m.hwnd, m.message, m.wParam, m.lParam);
}

void WINAPI PostQuitMessage(int nExitCode)
{
    quit_posted = 1;
    quit_code = nExitCode;
}

BOOL WINAPI PostMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    msg_enqueue(hWnd, Msg, wParam, lParam);
    return TRUE;
}

LRESULT WINAPI SendMessageA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    WINDOW *w = find_window(hWnd);
    if (w && w->wndproc)
        return w->wndproc(hWnd, Msg, wParam, lParam);
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

LRESULT WINAPI DefWindowProcA(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd;
    (void)wParam;
    (void)lParam;

    switch (Msg) {
    case WM_NCCREATE:   return 1;       /* allow creation */
    case WM_CREATE:     return 0;       /* success */
    case WM_CLOSE:      DestroyWindow(hWnd); return 0;
    case WM_DESTROY:    return 0;
    case WM_NCDESTROY:  return 0;
    case WM_ERASEBKGND: return 1;       /* "erased" */
    case WM_NCHITTEST:  return 1;       /* HTCLIENT */
    case WM_NCACTIVATE: return 1;
    case WM_SETCURSOR:  return 1;
    case WM_ACTIVATE:   return 0;
    case WM_PAINT:      return 0;
    }

    return 0;
}

/* ── Window info ───────────────────────────────────────────── */

BOOL WINAPI GetClientRect(HWND hWnd, LPRECT lpRect)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !lpRect) return FALSE;
    lpRect->left   = 0;
    lpRect->top    = 0;
    lpRect->right  = w->width;
    lpRect->bottom = w->height;
    return TRUE;
}

BOOL WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect)
{
    WINDOW *w = find_window(hWnd);
    if (!w || !lpRect) return FALSE;
    lpRect->left   = w->x;
    lpRect->top    = w->y;
    lpRect->right  = w->x + w->width;
    lpRect->bottom = w->y + w->height;
    return TRUE;
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

#define SPI_GETWORKAREA 48

BOOL WINAPI SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
    (void)uiParam; (void)fWinIni;
    if (uiAction == SPI_GETWORKAREA && pvParam) {
        /* Return screen rect as work area */
        int32_t *rect = (int32_t *)pvParam;
        rect[0] = 0;              /* left */
        rect[1] = 0;              /* top */
        rect[2] = SCREEN_WIDTH;   /* right */
        rect[3] = SCREEN_HEIGHT;  /* bottom */
        return TRUE;
    }
    return TRUE;
}

static ULONG_PTR g_timer_id = 1;

ULONG_PTR WINAPI SetTimer(HWND hWnd, ULONG_PTR nIDEvent, UINT uElapse, void *lpTimerFunc)
{
    (void)hWnd; (void)uElapse; (void)lpTimerFunc;
    return nIDEvent ? nIDEvent : g_timer_id++;
}

BOOL WINAPI KillTimer(HWND hWnd, ULONG_PTR uIDEvent)
{
    (void)hWnd; (void)uIDEvent;
    return TRUE;
}

int WINAPI GetSystemMetrics(int nIndex)
{
    switch (nIndex) {
    case SM_CXSCREEN:      return SCREEN_WIDTH;
    case SM_CYSCREEN:      return SCREEN_HEIGHT;
    case SM_CXFULLSCREEN:  return SCREEN_WIDTH;
    case SM_CYFULLSCREEN:  return SCREEN_HEIGHT;
    default:               return 0;
    }
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
} DEVMODEA;

#define DM_BITSPERPEL  0x40000
#define DM_PELSWIDTH   0x80000
#define DM_PELSHEIGHT  0x100000
#define DISP_CHANGE_SUCCESSFUL 0
#define ENUM_CURRENT_SETTINGS  ((uint32_t)-1)

LONG WINAPI ChangeDisplaySettingsA(DEVMODEA *dm, uint32_t flags)
{
    (void)dm; (void)flags;
    /* Always succeed — we use the GOP framebuffer as-is */
    return DISP_CHANGE_SUCCESSFUL;
}

LONG WINAPI ChangeDisplaySettingsW(void *dm, uint32_t flags)
{
    (void)dm; (void)flags;
    return DISP_CHANGE_SUCCESSFUL;
}

BOOL WINAPI EnumDisplaySettingsA(const char *device, uint32_t mode, DEVMODEA *dm)
{
    (void)device;
    if (!dm) return FALSE;

    /* Return our single supported mode */
    memset(dm, 0, sizeof(*dm));
    dm->dmSize = sizeof(*dm);
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    dm->dmBitsPerPel = 32;
    dm->dmPelsWidth = SCREEN_WIDTH;
    dm->dmPelsHeight = SCREEN_HEIGHT;
    dm->dmDisplayFrequency = 60;

    /* Only mode index 0 and ENUM_CURRENT_SETTINGS are valid */
    if (mode == 0 || mode == ENUM_CURRENT_SETTINGS)
        return TRUE;
    return FALSE;
}

BOOL WINAPI EnumDisplaySettingsW(const void *device, uint32_t mode, void *dm)
{
    return EnumDisplaySettingsA((const char *)device, mode, (DEVMODEA *)dm);
}

#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWL_USERDATA   (-21)
#define GWL_WNDPROC    (-4)

LONG WINAPI GetWindowLongA(HWND hWnd, int nIndex)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;
    switch (nIndex) {
    case GWL_STYLE:    return (LONG)w->style;
    case GWL_EXSTYLE:  return (LONG)w->ex_style;
    case GWL_USERDATA: return (LONG)(ULONG_PTR)w->user_data;
    case GWL_WNDPROC:  return (LONG)(ULONG_PTR)w->wndproc;
    default:           return 0;
    }
}

LONG WINAPI SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
{
    WINDOW *w = find_window(hWnd);
    if (!w) return 0;
    LONG old = GetWindowLongA(hWnd, nIndex);
    switch (nIndex) {
    case GWL_STYLE:    w->style = (DWORD)dwNewLong; break;
    case GWL_EXSTYLE:  w->ex_style = (DWORD)dwNewLong; break;
    case GWL_USERDATA: w->user_data = (PVOID)(ULONG_PTR)dwNewLong; break;
    case GWL_WNDPROC:  w->wndproc = (WNDPROC)(ULONG_PTR)dwNewLong; break;
    }
    return old;
}

HWND WINAPI GetForegroundWindow(void)
{
    /* Return the first visible window */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].visible)
            return windows[i].handle;
    }
    return NULL;
}

HWND WINAPI SetFocus(HWND hWnd) { return hWnd; }
HWND WINAPI GetDesktopWindow(void) { return (HWND)(ULONG_PTR)0xD0000001; }
HWND WINAPI GetActiveWindow(void) { return GetForegroundWindow(); }

/* ── Cursor / Input ────────────────────────────────────────── */

BOOL WINAPI SetCursorPos(int X, int Y)
{
    cursor_pos.x = X;
    cursor_pos.y = Y;
    return TRUE;
}

BOOL WINAPI GetCursorPos(LPPOINT lpPoint)
{
    if (!lpPoint) return FALSE;
    *lpPoint = cursor_pos;
    return TRUE;
}

int WINAPI ShowCursor(BOOL bShow)
{
    if (bShow) cursor_visible++;
    else       cursor_visible--;
    return cursor_visible;
}

BOOL WINAPI ClipCursor(const RECT *lpRect)
{
    (void)lpRect;
    return TRUE;
}

HWND WINAPI SetCapture(HWND hWnd)
{
    HWND old = capture_hwnd;
    capture_hwnd = hWnd;
    return old;
}

BOOL WINAPI ReleaseCapture(void)
{
    capture_hwnd = NULL;
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

short WINAPI GetKeyState(int nVirtKey)
{
    if (nVirtKey < 0 || nVirtKey > 255) return 0;
    short result = 0;
    if (key_state[nVirtKey] & 0x80) result |= (short)0x8000;
    if (key_state[nVirtKey] & 0x01) result |= 0x0001; /* toggle state */
    return result;
}

BOOL WINAPI GetKeyboardState(BYTE *lpKeyState)
{
    if (!lpKeyState) return FALSE;
    for (int i = 0; i < 256; i++) lpKeyState[i] = key_state[i];
    return TRUE;
}

DWORD WINAPI MapVirtualKeyA(DWORD uCode, DWORD uMapType)
{
    switch (uMapType) {
    case 0: /* VK → scancode */
        for (int i = 0; i < 0x59; i++)
            if (scancode_to_vk[i] == (BYTE)uCode) return (DWORD)i;
        return 0;
    case 1: /* scancode → VK */
        if (uCode < 0x59) return scancode_to_vk[uCode];
        return 0;
    case 2: /* VK → char (unshifted) */
        if (uCode >= 'A' && uCode <= 'Z') return uCode + 32; /* lowercase */
        if (uCode >= '0' && uCode <= '9') return uCode;
        if (uCode == VK_SPACE) return ' ';
        return 0;
    default:
        return 0;
    }
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
    if (uVirtKey == VK_SPACE)  { *lpChar = ' '; return 1; }
    if (uVirtKey == VK_RETURN) { *lpChar = '\r'; return 1; }
    return 0;
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

HCURSOR WINAPI LoadCursorA(HINSTANCE hInstance, PCSTR lpCursorName)
{
    (void)hInstance;
    (void)lpCursorName;
    return (HCURSOR)(ULONG_PTR)0xC0000001;
}

HICON WINAPI LoadIconA(HINSTANCE hInstance, PCSTR lpIconName)
{
    (void)hInstance;
    (void)lpIconName;
    return (HICON)(ULONG_PTR)0xC0000002;
}

HICON WINAPI LoadIconW(HINSTANCE hInstance, PCWSTR lpIconName)
{
    (void)hInstance;
    (void)lpIconName;
    return (HICON)(ULONG_PTR)0xC0000003;
}

HDC WINAPI GetDC(HWND hWnd)
{
    (void)hWnd;
    return (HDC)(ULONG_PTR)0xDC000001;
}

int WINAPI ReleaseDC(HWND hWnd, HDC hDC)
{
    (void)hWnd;
    (void)hDC;
    return 1;
}

BOOL WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase)
{
    (void)hWnd;
    (void)lpRect;
    (void)bErase;
    return TRUE;
}

BOOL WINAPI SetForegroundWindow(HWND hWnd)
{
    (void)hWnd;
    return TRUE;
}

/* ── Dialog stubs ──────────────────────────────────────────── */

HWND WINAPI CreateDialogParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    (void)hInstance; (void)lpTemplateName; (void)hWndParent;
    (void)lpDialogFunc; (void)dwInitParam;
    serial_puts("[USER32] CreateDialogParamA: stub NULL\n");
    return NULL;
}

HWND WINAPI CreateDialogParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    (void)hInstance; (void)lpTemplateName; (void)hWndParent;
    (void)lpDialogFunc; (void)dwInitParam;
    serial_puts("[USER32] CreateDialogParamW: stub NULL\n");
    return NULL;
}

BOOL WINAPI EndDialog(HWND hDlg, LONG_PTR nResult)
{
    (void)hDlg; (void)nResult;
    return TRUE;
}

HWND WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem)
{
    (void)hDlg; (void)nIDDlgItem;
    return NULL;
}

/* ── Window search stubs ───────────────────────────────────── */

HWND WINAPI FindWindowExA(HWND hWndParent, HWND hWndChildAfter,
                           PCSTR lpszClass, PCSTR lpszWindow)
{
    (void)hWndParent; (void)hWndChildAfter;
    (void)lpszClass; (void)lpszWindow;
    return NULL;
}

HWND WINAPI FindWindowExW(HWND hWndParent, HWND hWndChildAfter,
                           PCWSTR lpszClass, PCWSTR lpszWindow)
{
    (void)hWndParent; (void)hWndChildAfter;
    (void)lpszClass; (void)lpszWindow;
    return NULL;
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
    (void)hWnd; (void)Msg; (void)wParam; (void)lParam;
    return 0;
}

LRESULT WINAPI SendMessageTimeoutW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam,
                                    DWORD fuFlags, DWORD uTimeout, ULONG_PTR *lpdwResult)
{
    (void)hWnd; (void)Msg; (void)wParam; (void)lParam;
    (void)fuFlags; (void)uTimeout;
    if (lpdwResult) *lpdwResult = 0;
    return 0;
}

/* ── Thread message stubs ──────────────────────────────────── */

BOOL WINAPI PostThreadMessageA(DWORD idThread, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)idThread; (void)Msg; (void)wParam; (void)lParam;
    return TRUE;
}

BOOL WINAPI PostThreadMessageW(DWORD idThread, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)idThread; (void)Msg; (void)wParam; (void)lParam;
    return TRUE;
}

/* ── Window property stubs ─────────────────────────────────── */

HANDLE WINAPI GetPropA(HWND hWnd, PCSTR lpString)
{
    (void)hWnd; (void)lpString;
    return NULL;
}

HANDLE WINAPI GetPropW(HWND hWnd, PCWSTR lpString)
{
    (void)hWnd; (void)lpString;
    return NULL;
}

BOOL WINAPI SetPropA(HWND hWnd, PCSTR lpString, HANDLE hData)
{
    (void)hWnd; (void)lpString; (void)hData;
    return TRUE;
}

BOOL WINAPI SetPropW(HWND hWnd, PCWSTR lpString, HANDLE hData)
{
    (void)hWnd; (void)lpString; (void)hData;
    return TRUE;
}

HANDLE WINAPI RemovePropA(HWND hWnd, PCSTR lpString)
{
    (void)hWnd; (void)lpString;
    return NULL;
}

HANDLE WINAPI RemovePropW(HWND hWnd, PCWSTR lpString)
{
    (void)hWnd; (void)lpString;
    return NULL;
}

/* ── Window thread ─────────────────────────────────────────── */

DWORD WINAPI GetWindowThreadProcessId(HWND hWnd, DWORD *lpdwProcessId)
{
    (void)hWnd;
    if (lpdwProcessId) *lpdwProcessId = 1;
    return 1;
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
static BYTE prev_was_e0 = 0;

void win32_post_keyboard_event(BYTE scancode, BOOL key_up)
{
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

    if (vk == 0) return;

    /* Update key state */
    if (key_up) {
        key_state[vk] &= ~0x80;
        /* Also update generic modifier */
        if (vk == VK_LSHIFT || vk == VK_RSHIFT)
            key_state[VK_SHIFT] = key_state[VK_LSHIFT] | key_state[VK_RSHIFT];
        if (vk == VK_LCONTROL || vk == VK_RCONTROL)
            key_state[VK_CONTROL] = key_state[VK_LCONTROL] | key_state[VK_RCONTROL];
        if (vk == VK_LMENU || vk == VK_RMENU)
            key_state[VK_MENU] = key_state[VK_LMENU] | key_state[VK_RMENU];
    } else {
        key_state[vk] |= 0x80;
        key_state[vk] ^= 0x01; /* toggle */
        async_pressed[vk] = 1;
        /* Update generic modifier */
        if (vk == VK_LSHIFT || vk == VK_RSHIFT)
            key_state[VK_SHIFT] |= 0x80;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL)
            key_state[VK_CONTROL] |= 0x80;
        if (vk == VK_LMENU || vk == VK_RMENU)
            key_state[VK_MENU] |= 0x80;
    }

    /* Find active window for message target */
    HWND target = NULL;
    for (int i = window_count - 1; i >= 0; i--) {
        if (windows[i].used) { target = (HWND)(ULONG_PTR)(i + 1); break; }
    }

    /* Build lParam: scancode in bits 16-23, extended flag in bit 24,
     * previous state in bit 30, transition state in bit 31 */
    LPARAM lp = ((LPARAM)scancode << 16) | 1; /* repeat count = 1 */
    if (extended) lp |= (1 << 24);
    if (key_up) {
        lp |= ((LPARAM)1 << 30); /* was down */
        lp |= ((LPARAM)1 << 31); /* transition: going up */
    }

    DWORD msg = key_up ? WM_KEYUP : WM_KEYDOWN;
    msg_enqueue(target, msg, (WPARAM)vk, lp);
}

/*
 * Called by OsitoK mouse IRQ handler (PS/2 or USB HID).
 * dx, dy: relative mouse movement (pixels).
 * buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
 * wheel_delta: mouse wheel delta (positive = up, negative = down).
 */
void win32_post_mouse_event(int dx, int dy, DWORD buttons, short wheel_delta)
{
    /* Update cursor position */
    cursor_pos.x += dx;
    cursor_pos.y += dy;
    /* Clamp to screen */
    if (cursor_pos.x < 0) cursor_pos.x = 0;
    if (cursor_pos.y < 0) cursor_pos.y = 0;
    if (cursor_pos.x >= SCREEN_WIDTH)  cursor_pos.x = SCREEN_WIDTH - 1;
    if (cursor_pos.y >= SCREEN_HEIGHT) cursor_pos.y = SCREEN_HEIGHT - 1;

    DWORD old_buttons = mouse_buttons;
    mouse_buttons = buttons;

    HWND target = NULL;
    for (int i = window_count - 1; i >= 0; i--) {
        if (windows[i].used) { target = (HWND)(ULONG_PTR)(i + 1); break; }
    }
    if (capture_hwnd) target = capture_hwnd;

    LPARAM pos_lp = ((LPARAM)(cursor_pos.y & 0xFFFF) << 16) |
                     (LPARAM)(cursor_pos.x & 0xFFFF);

    /* Movement */
    if (dx != 0 || dy != 0)
        msg_enqueue(target, WM_MOUSEMOVE, 0, pos_lp);

    /* Button state changes */
    if ((buttons & 1) && !(old_buttons & 1)) {
        key_state[VK_LBUTTON] |= 0x80;
        msg_enqueue(target, WM_LBUTTONDOWN, MK_LBUTTON, pos_lp);
    }
    if (!(buttons & 1) && (old_buttons & 1)) {
        key_state[VK_LBUTTON] &= ~0x80;
        msg_enqueue(target, WM_LBUTTONUP, 0, pos_lp);
    }
    if ((buttons & 2) && !(old_buttons & 2)) {
        key_state[VK_RBUTTON] |= 0x80;
        msg_enqueue(target, WM_RBUTTONDOWN, MK_RBUTTON, pos_lp);
    }
    if (!(buttons & 2) && (old_buttons & 2)) {
        key_state[VK_RBUTTON] &= ~0x80;
        msg_enqueue(target, WM_RBUTTONUP, 0, pos_lp);
    }
    if ((buttons & 4) && !(old_buttons & 4)) {
        key_state[VK_MBUTTON] |= 0x80;
        msg_enqueue(target, WM_MBUTTONDOWN, MK_MBUTTON, pos_lp);
    }
    if (!(buttons & 4) && (old_buttons & 4)) {
        key_state[VK_MBUTTON] &= ~0x80;
        msg_enqueue(target, WM_MBUTTONUP, 0, pos_lp);
    }

    /* Wheel */
    if (wheel_delta != 0) {
        WPARAM wp = ((WPARAM)(short)wheel_delta << 16);
        msg_enqueue(target, WM_MOUSEWHEEL, wp, pos_lp);
    }
}

/* ── Clipboard stubs (UT99 Core.dll) ─────────────────────── */

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
    return TRUE;
}

HANDLE WINAPI SetClipboardData(UINT uFormat, HANDLE hMem)
{
    (void)uFormat; (void)hMem;
    return hMem;
}

HANDLE WINAPI GetClipboardData(UINT uFormat)
{
    (void)uFormat;
    return NULL;
}

/* ── Paint / Drawing stubs (UT99 Window.dll / Core.dll) ──── */

HDC WINAPI BeginPaint(HWND hWnd, PVOID lpPaint)
{
    (void)hWnd;
    if (lpPaint) {
        BYTE *p = (BYTE *)lpPaint;
        for (int i = 0; i < 60; i++) p[i] = 0;
    }
    return (HDC)(ULONG_PTR)0xDC000001;
}

BOOL WINAPI EndPaint(HWND hWnd, PVOID lpPaint)
{
    (void)hWnd; (void)lpPaint;
    return TRUE;
}

LRESULT WINAPI CallWindowProcA(PVOID lpPrevWndFunc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam)
{
    uint32_t func = (uint32_t)(ULONG_PTR)lpPrevWndFunc;
    /* Validate: PE code addresses are above 0x10000000; anything below
     * is kernel/thunk/stack memory and would crash if called. */
    if (!func || func < 0x10000000) return 0;
    extern uint32_t compat32_callback_args(uint32_t func_addr, int nargs, const uint32_t *args);
    uint32_t args[4] = {
        (uint32_t)(ULONG_PTR)hWnd, (uint32_t)Msg,
        (uint32_t)wParam, (uint32_t)lParam
    };
    return (LRESULT)compat32_callback_args(func, 4, args);
}

LRESULT WINAPI CallWindowProcW(PVOID lpPrevWndFunc, HWND hWnd, DWORD Msg,
                                WPARAM wParam, LPARAM lParam)
{
    return CallWindowProcA(lpPrevWndFunc, hWnd, Msg, wParam, lParam);
}

LRESULT WINAPI DefWindowProcW(HWND hWnd, DWORD Msg, WPARAM wParam, LPARAM lParam)
{
    (void)hWnd; (void)Msg; (void)wParam; (void)lParam;
    return 0;
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
    /* Convert wide strings to narrow and delegate to A version */
    char classA[128] = {0}, nameA[256] = {0};
    if (lpClassName) {
        for (int i = 0; i < 127 && lpClassName[i]; i++)
            classA[i] = (char)(lpClassName[i] & 0xFF);
    }
    if (lpWindowName) {
        for (int i = 0; i < 255 && lpWindowName[i]; i++)
            nameA[i] = (char)(lpWindowName[i] & 0xFF);
    }
    return CreateWindowExA(dwExStyle, classA, nameA, dwStyle,
                           X, Y, nWidth, nHeight,
                           hWndParent, hMenu, hInstance, lpParam);
}

WORD WINAPI RegisterClassExW(PVOID lpwcx)
{
    /* The WNDCLASSEXW structure has wide string pointers.
     * Extract class name and WndProc, create a narrow entry. */
    if (!lpwcx) return 0;

    /* WNDCLASSEXW is passed as a 32-bit pointer to the struct.
     * Read fields at byte offsets to be safe: */
    uint8_t *raw = (uint8_t *)lpwcx;
    uint32_t cb_size = *(uint32_t *)(raw + 0);
    uint32_t wndproc_addr = *(uint32_t *)(raw + 8);
    uint32_t classname_ptr = *(uint32_t *)(raw + 40);

    /* Note: classname wide string has corrupted first char due to unknown
     * alignment issue in compat32 struct passing. The WndProc fallback in
     * CreateWindowExA handles this — classes matched by last-registered. */

    char classA[128] = {0};
    if (classname_ptr) {
        const uint16_t *ws = (const uint16_t *)(uintptr_t)classname_ptr;
        for (int i = 0; i < 127 && ws[i]; i++)
            classA[i] = (char)(ws[i] & 0xFF);
    }

    serial_puts("[USER32] RegisterClassExW: ");
    serial_puts(classA);
    serial_puts(" wndproc=0x");
    serial_puthex(wndproc_addr, 8);
    serial_puts("\n");

    if (wndclass_count >= MAX_WNDCLASSES) return 0;
    WNDCLASS_ENTRY *e = &wndclasses[wndclass_count];
    u32_strcpy(e->class_name, classA, 128);
    e->wndproc = (WNDPROC)(uintptr_t)wndproc_addr;
    e->used = 1;
    wndclass_count++;
    return (WORD)(0xC100 + wndclass_count);
}

BOOL WINAPI GetClassInfoExA(HINSTANCE hInstance, PCSTR lpszClass, PVOID lpwcx)
{
    (void)hInstance;
    if (!lpszClass) return FALSE;
    /* Fill WNDCLASSEX with defaults if provided */
    if (lpwcx) {
        uint8_t *p = (uint8_t *)lpwcx;
        for (int i = 0; i < 48; i++) p[i] = 0; /* zero WNDCLASSEXA */
        *(uint32_t *)p = 48; /* cbSize */
    }
    /* Always return TRUE — simulate all system window classes exist.
     * The engine checks base classes (WinBase, etc.) before RegisterClassExW. */
    return TRUE;
}

BOOL WINAPI GetClassInfoExW(HINSTANCE hInstance, PCWSTR lpszClass, PVOID lpwcx)
{
    (void)hInstance;
    if (!lpszClass) return FALSE;
    if (lpwcx) {
        uint8_t *p = (uint8_t *)lpwcx;
        for (int i = 0; i < 48; i++) p[i] = 0;
        *(uint32_t *)p = 48; /* cbSize */
    }
    return TRUE;
}

LONG WINAPI GetWindowLongW(HWND hWnd, int nIndex)
{
    (void)hWnd; (void)nIndex;
    return 0;
}

LONG WINAPI SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong)
{
    (void)hWnd; (void)nIndex; (void)dwNewLong;
    return 0;
}

BOOL WINAPI IsWindow(HWND hWnd)
{
    (void)hWnd;
    return TRUE;
}

BOOL WINAPI IsIconic(HWND hWnd)
{
    (void)hWnd;
    return FALSE;
}

BOOL WINAPI IsZoomed(HWND hWnd)
{
    (void)hWnd;
    return FALSE;
}

BOOL WINAPI IsWindowEnabled(HWND hWnd)
{
    (void)hWnd;
    return TRUE;
}

HWND WINAPI GetParent(HWND hWnd)
{
    (void)hWnd;
    return NULL;
}

BOOL WINAPI EnumChildWindows(HWND hWndParent, PVOID lpEnumFunc, LPARAM lParam)
{
    (void)hWndParent; (void)lpEnumFunc; (void)lParam;
    return TRUE;
}

BOOL WINAPI ClientToScreen(HWND hWnd, PVOID lpPoint)
{
    (void)hWnd; (void)lpPoint;
    return TRUE;
}

BOOL WINAPI ScreenToClient(HWND hWnd, PVOID lpPoint)
{
    (void)hWnd; (void)lpPoint;
    return TRUE;
}

BOOL WINAPI GetUpdateRect(HWND hWnd, PVOID lpRect, BOOL bErase)
{
    (void)hWnd; (void)lpRect; (void)bErase;
    return FALSE;
}

int WINAPI FillRect(HDC hDC, PVOID lprc, HBRUSH hbr)
{
    (void)hDC; (void)lprc; (void)hbr;
    return 1;
}

BOOL WINAPI DrawFocusRect(HDC hDC, PVOID lprc)
{
    (void)hDC; (void)lprc;
    return TRUE;
}

int WINAPI DrawTextA(HDC hdc, PCSTR lpchText, int cchText, PVOID lprc, UINT format)
{
    (void)hdc; (void)lpchText; (void)cchText; (void)lprc; (void)format;
    return 0;
}

int WINAPI DrawTextExA(HDC hdc, PSTR lpchText, int cchText, PVOID lprc,
                        UINT format, PVOID lpdtp)
{
    (void)hdc; (void)lpchText; (void)cchText; (void)lprc;
    (void)format; (void)lpdtp;
    return 0;
}

int WINAPI DrawTextExW(HDC hdc, PWSTR lpchText, int cchText, PVOID lprc,
                        UINT format, PVOID lpdtp)
{
    (void)hdc; (void)lpchText; (void)cchText; (void)lprc;
    (void)format; (void)lpdtp;
    return 0;
}

DWORD WINAPI GetSysColor(int nIndex)
{
    (void)nIndex;
    return 0;
}

/* ── Dialog box stubs ──────────────────────────────────────── */

LONG_PTR WINAPI DialogBoxParamA(HINSTANCE hInstance, PCSTR lpTemplateName,
                                 HWND hWndParent, PVOID lpDialogFunc, LPARAM dwInitParam)
{
    (void)hInstance; (void)lpTemplateName; (void)hWndParent;
    (void)lpDialogFunc; (void)dwInitParam;
    serial_puts("[USER32] DialogBoxParamA: stub -1\n");
    return -1;
}

LONG_PTR WINAPI DialogBoxParamW(HINSTANCE hInstance, PCWSTR lpTemplateName,
                                 HWND hWndParent, PVOID lpDialogFunc, LPARAM dwInitParam)
{
    (void)hInstance; (void)lpTemplateName; (void)hWndParent;
    (void)lpDialogFunc; (void)dwInitParam;
    serial_puts("[USER32] DialogBoxParamW: stub -1\n");
    return -1;
}

/* ── Menu stubs ────────────────────────────────────────────── */

HMENU WINAPI LoadMenuA(HINSTANCE hInstance, PCSTR lpMenuName)
{
    (void)hInstance; (void)lpMenuName;
    return NULL;
}

HMENU WINAPI LoadMenuW(HINSTANCE hInstance, PCWSTR lpMenuName)
{
    (void)hInstance; (void)lpMenuName;
    return NULL;
}

HMENU WINAPI GetSubMenu(HMENU hMenu, int nPos)
{
    (void)hMenu; (void)nPos;
    return NULL;
}

int WINAPI GetMenuItemCount(HMENU hMenu)
{
    (void)hMenu;
    return 0;
}

UINT WINAPI GetMenuState(HMENU hMenu, UINT uId, UINT uFlags)
{
    (void)hMenu; (void)uId; (void)uFlags;
    return (UINT)-1;
}

BOOL WINAPI GetMenuItemInfoA(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii)
{
    (void)hmenu; (void)item; (void)fByPosition; (void)lpmii;
    return FALSE;
}

BOOL WINAPI GetMenuItemInfoW(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii)
{
    (void)hmenu; (void)item; (void)fByPosition; (void)lpmii;
    return FALSE;
}

BOOL WINAPI SetMenuItemInfoA(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii)
{
    (void)hmenu; (void)item; (void)fByPosition; (void)lpmii;
    return TRUE;
}

BOOL WINAPI SetMenuItemInfoW(HMENU hmenu, UINT item, BOOL fByPosition, PVOID lpmii)
{
    (void)hmenu; (void)item; (void)fByPosition; (void)lpmii;
    return TRUE;
}

DWORD WINAPI CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck)
{
    (void)hMenu; (void)uIDCheckItem; (void)uCheck;
    return 0;
}

BOOL WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y,
                            int nReserved, HWND hWnd, PVOID prcRect)
{
    (void)hMenu; (void)uFlags; (void)x; (void)y;
    (void)nReserved; (void)hWnd; (void)prcRect;
    return FALSE;
}

/* ── Cursor / misc stubs ───────────────────────────────────── */

HCURSOR WINAPI SetCursor(HCURSOR hCursor)
{
    (void)hCursor;
    return NULL;
}

HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, PCWSTR lpCursorName)
{
    (void)hInstance; (void)lpCursorName;
    return (HCURSOR)(ULONG_PTR)0xCCC00001;
}

HANDLE WINAPI LoadImageA(HINSTANCE hInst, PCSTR name, UINT type,
                          int cx, int cy, UINT fuLoad)
{
    (void)hInst; (void)name; (void)type;
    (void)cx; (void)cy; (void)fuLoad;
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
    msg_enqueue(hWnd, Msg, wParam, lParam);
    return TRUE;
}

/* ── Additional stubs ──────────────────────────────────────── */

BOOL WINAPI EnableWindow(HWND hWnd, BOOL bEnable)
{
    (void)hWnd; (void)bEnable;
    return FALSE; /* window was previously enabled */
}

HMENU WINAPI GetMenu(HWND hWnd)
{
    (void)hWnd;
    return NULL;
}

DWORD WINAPI GetMessageTime(void)
{
    return 0;
}

HWND WINAPI GetFocus(void)
{
    return NULL;
}

BOOL WINAPI IsWindowVisible(HWND hWnd)
{
    (void)hWnd;
    return TRUE;
}

int WINAPI MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints)
{
    (void)hWndFrom; (void)hWndTo; (void)lpPoints; (void)cPoints;
    return 0;
}

BOOL WINAPI RegisterHotKey(HWND hWnd, int id, UINT fsModifiers, UINT vk)
{
    (void)hWnd; (void)id; (void)fsModifiers; (void)vk;
    return FALSE;
}

BOOL WINAPI SetMenu(HWND hWnd, HMENU hMenu)
{
    (void)hWnd; (void)hMenu;
    return FALSE;
}

HWND WINAPI SetParent(HWND hWndChild, HWND hWndNewParent)
{
    (void)hWndChild; (void)hWndNewParent;
    return NULL;
}

HWND WINAPI SetActiveWindow(HWND hWnd)
{
    (void)hWnd;
    return NULL;
}

BOOL WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam,
                                   PVOID pvParam, UINT fWinIni)
{
    (void)uiAction; (void)uiParam; (void)pvParam; (void)fWinIni;
    return FALSE;
}

BOOL WINAPI UnregisterHotKey(HWND hWnd, int id)
{
    (void)hWnd; (void)id;
    return FALSE;
}

BOOL WINAPI ValidateRect(HWND hWnd, const RECT *lpRect)
{
    (void)hWnd; (void)lpRect;
    return TRUE;
}

/* ── Export table ──────────────────────────────────────────── */

typedef struct { const char *name; PVOID func; } SHIM_EXPORT;

static const SHIM_EXPORT user32_exports[] = {
    /* Window class */
    { "RegisterClassExA",   (PVOID)RegisterClassExA },
    { "RegisterClassA",     (PVOID)RegisterClassA },
    { "UnregisterClassA",   (PVOID)UnregisterClassA },
    /* Window creation */
    { "CreateWindowExA",    (PVOID)CreateWindowExA },
    { "DestroyWindow",      (PVOID)DestroyWindow },
    { "ShowWindow",         (PVOID)ShowWindow },
    { "UpdateWindow",       (PVOID)UpdateWindow },
    { "SetWindowTextA",     (PVOID)SetWindowTextA },
    { "SetWindowPos",       (PVOID)SetWindowPos },
    { "MoveWindow",         (PVOID)MoveWindow },
    /* Message loop */
    { "PeekMessageA",       (PVOID)PeekMessageA },
    { "GetMessageA",        (PVOID)GetMessageA },
    { "TranslateMessage",   (PVOID)TranslateMessage },
    { "DispatchMessageA",   (PVOID)DispatchMessageA },
    { "PostQuitMessage",    (PVOID)PostQuitMessage },
    { "PostMessageA",       (PVOID)PostMessageA },
    { "SendMessageA",       (PVOID)SendMessageA },
    { "DefWindowProcA",     (PVOID)DefWindowProcA },
    /* Window info */
    { "GetClientRect",      (PVOID)GetClientRect },
    { "GetWindowRect",      (PVOID)GetWindowRect },
    { "AdjustWindowRect",   (PVOID)AdjustWindowRect },
    { "AdjustWindowRectEx", (PVOID)AdjustWindowRectEx },
    { "SystemParametersInfoA",(PVOID)SystemParametersInfoA },
    { "SystemParametersInfoW",(PVOID)SystemParametersInfoA },
    { "SetTimer",            (PVOID)SetTimer },
    { "KillTimer",           (PVOID)KillTimer },
    { "GetSystemMetrics",    (PVOID)GetSystemMetrics },
    { "ChangeDisplaySettingsA",   (PVOID)ChangeDisplaySettingsA },
    { "ChangeDisplaySettingsW",   (PVOID)ChangeDisplaySettingsW },
    { "EnumDisplaySettingsA",     (PVOID)EnumDisplaySettingsA },
    { "EnumDisplaySettingsW",     (PVOID)EnumDisplaySettingsW },
    { "GetWindowLongA",     (PVOID)GetWindowLongA },
    { "SetWindowLongA",     (PVOID)SetWindowLongA },
    { "GetForegroundWindow",(PVOID)GetForegroundWindow },
    { "SetFocus",           (PVOID)SetFocus },
    { "GetDesktopWindow",   (PVOID)GetDesktopWindow },
    { "GetActiveWindow",    (PVOID)GetActiveWindow },
    /* Cursor / input */
    { "SetCursorPos",       (PVOID)SetCursorPos },
    { "GetCursorPos",       (PVOID)GetCursorPos },
    { "ShowCursor",         (PVOID)ShowCursor },
    { "ClipCursor",         (PVOID)ClipCursor },
    { "SetCapture",         (PVOID)SetCapture },
    { "ReleaseCapture",     (PVOID)ReleaseCapture },
    { "GetAsyncKeyState",   (PVOID)GetAsyncKeyState },
    { "GetKeyState",        (PVOID)GetKeyState },
    { "GetKeyboardState",   (PVOID)GetKeyboardState },
    { "MapVirtualKeyA",     (PVOID)MapVirtualKeyA },
    { "GetKeyNameTextA",    (PVOID)GetKeyNameTextA },
    { "ToAscii",            (PVOID)ToAscii },
    /* Misc */
    { "MessageBoxA",        (PVOID)MessageBoxA },
    { "MessageBoxW",        (PVOID)MessageBoxW },
    { "LoadCursorA",        (PVOID)LoadCursorA },
    { "LoadIconA",          (PVOID)LoadIconA },
    { "LoadIconW",          (PVOID)LoadIconW },
    { "GetDC",              (PVOID)GetDC },
    { "ReleaseDC",          (PVOID)ReleaseDC },
    { "InvalidateRect",     (PVOID)InvalidateRect },
    { "SetForegroundWindow",(PVOID)SetForegroundWindow },
    /* Dialog */
    { "CreateDialogParamA", (PVOID)CreateDialogParamA },
    { "CreateDialogParamW", (PVOID)CreateDialogParamW },
    { "EndDialog",          (PVOID)EndDialog },
    { "GetDlgItem",         (PVOID)GetDlgItem },
    /* Window search */
    { "FindWindowExA",      (PVOID)FindWindowExA },
    { "FindWindowExW",      (PVOID)FindWindowExW },
    /* Wide message loop */
    { "PeekMessageW",       (PVOID)PeekMessageW },
    { "GetMessageW",        (PVOID)GetMessageW },
    { "DispatchMessageW",   (PVOID)DispatchMessageW },
    { "SendMessageW",       (PVOID)SendMessageW },
    { "SendMessageTimeoutW",(PVOID)SendMessageTimeoutW },
    /* Thread messages */
    { "PostThreadMessageA", (PVOID)PostThreadMessageA },
    { "PostThreadMessageW", (PVOID)PostThreadMessageW },
    /* Window properties */
    { "GetPropA",           (PVOID)GetPropA },
    { "GetPropW",           (PVOID)GetPropW },
    { "SetPropA",           (PVOID)SetPropA },
    { "SetPropW",           (PVOID)SetPropW },
    { "RemovePropA",        (PVOID)RemovePropA },
    { "RemovePropW",        (PVOID)RemovePropW },
    /* Window thread */
    { "GetWindowThreadProcessId", (PVOID)GetWindowThreadProcessId },
    /* Clipboard */
    { "OpenClipboard",      (PVOID)OpenClipboard },
    { "CloseClipboard",     (PVOID)CloseClipboard },
    { "EmptyClipboard",     (PVOID)EmptyClipboard },
    { "SetClipboardData",   (PVOID)SetClipboardData },
    { "GetClipboardData",   (PVOID)GetClipboardData },
    /* Paint / drawing */
    { "BeginPaint",         (PVOID)BeginPaint },
    { "EndPaint",           (PVOID)EndPaint },
    { "CallWindowProcA",    (PVOID)CallWindowProcA },
    { "CallWindowProcW",    (PVOID)CallWindowProcW },
    { "DefWindowProcW",     (PVOID)DefWindowProcW },
    { "DefMDIChildProcA",   (PVOID)DefMDIChildProcA },
    { "DefMDIChildProcW",   (PVOID)DefMDIChildProcW },
    { "CreateWindowExW",    (PVOID)CreateWindowExW },
    { "RegisterClassExW",   (PVOID)RegisterClassExW },
    { "GetClassInfoExA",    (PVOID)GetClassInfoExA },
    { "GetClassInfoExW",    (PVOID)GetClassInfoExW },
    { "GetWindowLongW",     (PVOID)GetWindowLongW },
    { "SetWindowLongW",     (PVOID)SetWindowLongW },
    { "IsWindow",           (PVOID)IsWindow },
    { "IsIconic",           (PVOID)IsIconic },
    { "IsZoomed",           (PVOID)IsZoomed },
    { "IsWindowEnabled",    (PVOID)IsWindowEnabled },
    { "GetParent",          (PVOID)GetParent },
    { "EnumChildWindows",   (PVOID)EnumChildWindows },
    { "ClientToScreen",     (PVOID)ClientToScreen },
    { "ScreenToClient",     (PVOID)ScreenToClient },
    { "GetUpdateRect",      (PVOID)GetUpdateRect },
    { "FillRect",           (PVOID)FillRect },
    { "DrawFocusRect",      (PVOID)DrawFocusRect },
    { "DrawTextA",          (PVOID)DrawTextA },
    { "DrawTextExA",        (PVOID)DrawTextExA },
    { "DrawTextExW",        (PVOID)DrawTextExW },
    { "GetSysColor",        (PVOID)GetSysColor },
    /* Dialog box */
    { "DialogBoxParamA",    (PVOID)DialogBoxParamA },
    { "DialogBoxParamW",    (PVOID)DialogBoxParamW },
    /* Menu */
    { "LoadMenuA",          (PVOID)LoadMenuA },
    { "LoadMenuW",          (PVOID)LoadMenuW },
    { "GetSubMenu",         (PVOID)GetSubMenu },
    { "GetMenuItemCount",   (PVOID)GetMenuItemCount },
    { "GetMenuState",       (PVOID)GetMenuState },
    { "GetMenuItemInfoA",   (PVOID)GetMenuItemInfoA },
    { "GetMenuItemInfoW",   (PVOID)GetMenuItemInfoW },
    { "SetMenuItemInfoA",   (PVOID)SetMenuItemInfoA },
    { "SetMenuItemInfoW",   (PVOID)SetMenuItemInfoW },
    { "CheckMenuItem",      (PVOID)CheckMenuItem },
    { "TrackPopupMenu",     (PVOID)TrackPopupMenu },
    /* Cursor / misc */
    { "SetCursor",          (PVOID)SetCursor },
    { "LoadCursorW",        (PVOID)LoadCursorW },
    { "LoadImageA",         (PVOID)LoadImageA },
    { "RegisterWindowMessageA", (PVOID)RegisterWindowMessageA },
    { "RegisterWindowMessageW", (PVOID)RegisterWindowMessageW },
    { "PostMessageW",       (PVOID)PostMessageW },
    /* Additional stubs */
    { "EnableWindow",           (PVOID)EnableWindow },
    { "GetMenu",                (PVOID)GetMenu },
    { "GetMessageTime",         (PVOID)GetMessageTime },
    { "GetFocus",               (PVOID)GetFocus },
    { "IsWindowVisible",        (PVOID)IsWindowVisible },
    { "MapWindowPoints",        (PVOID)MapWindowPoints },
    { "RegisterHotKey",         (PVOID)RegisterHotKey },
    { "SetMenu",                (PVOID)SetMenu },
    { "SetParent",              (PVOID)SetParent },
    { "SetActiveWindow",        (PVOID)SetActiveWindow },
    { "SystemParametersInfoW",  (PVOID)SystemParametersInfoW },
    { "UnregisterHotKey",       (PVOID)UnregisterHotKey },
    { "ValidateRect",           (PVOID)ValidateRect },
    { NULL, NULL }
};

PVOID user32_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;
    for (int i = 0; user32_exports[i].name; i++) {
        if (u32_strcmp(func_name, user32_exports[i].name) == 0)
            return user32_exports[i].func;
    }
    return NULL;
}

PVOID user32_shim_init(void)
{
    wndclass_count = 0;
    window_count = 0;
    msg_head = msg_tail = 0;
    quit_posted = 0;
    for (int i = 0; i < 256; i++) { key_state[i] = 0; async_pressed[i] = 0; }
    mouse_buttons = 0;
    prev_was_e0 = 0;
    return (PVOID)user32_exports;
}
