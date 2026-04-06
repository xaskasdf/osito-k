/*
 * OsitoK x86-64 — Window Compositor (X-RETINA)
 *
 * Composites application windows onto the display back buffer.
 * Equivalent to Apple's WindowServer or Wayland's compositor.
 *
 * Architecture:
 *   App draws to shm surface → Compositor reads surface →
 *   Blits all windows to back buffer → display_flip()
 *
 * Runs as a QOS_INTERACTIVE kernel thread at 60fps.
 * Uses event coalescing to batch input between frames.
 *
 * Direct scanout optimization: if a single fullscreen window
 * is visible, bypass compositing and flip its surface directly.
 */

#include "../include/types.h"
#include "gui.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

/* Display subsystem (display.c) */
extern uint32_t *display_get_back_buffer(void);
extern void      display_mark_dirty(void);
extern void      display_flip(void);
extern uint32_t  display_get_width(void);
extern uint32_t  display_get_height(void);
extern uint32_t  display_get_pitch(void);
extern void      display_wait_vblank(void);

/* Input events (input_events.c) */
extern bool    input_has_events(void);
extern int     input_drain_coalesced(int16_t *mdx, int16_t *mdy,
                                     uint8_t *buttons, int16_t *wheel,
                                     void *key_out, int key_max);
extern void    input_get_cursor(int32_t *x, int32_t *y);
extern void    input_post_mouse_move(int16_t dx, int16_t dy);

/* Shared memory (shm.c) */
extern void   *shm_map(uint32_t handle);
extern void    shm_unmap(uint32_t handle);
extern uint64_t shm_get_phys(uint32_t handle);

/* Scheduler / process (process.c) */
extern void proc_set_qos(uint8_t qos);
extern uint64_t idt_get_ticks(void);
extern uint32_t proc_count_active(void);

/* USB HID polling (xhci.c — weak: absent if no xHCI) */
extern void xhci_poll(void) __attribute__((weak));


/* ── CMOS RTC helpers ──────────────────────────────────────── */

#ifdef __EMSCRIPTEN__
static inline uint8_t cmos_read(uint8_t reg) { (void)reg; return 0; }
#else
static inline uint8_t cmos_read(uint8_t reg)
{
    __asm__ volatile ("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)0x71));
    return val;
}
#endif

static inline uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

/* Memory (memory.c) */
extern uint64_t mem_get_free(void);
extern uint64_t mem_get_used(void);
extern uint64_t mem_get_total(void);

#define QOS_INTERACTIVE 3

/* Blit functions (display.c) — forward declarations using surface_t */
/* We use raw pointer blits here to avoid circular deps */

/* ── Window Structure ────────────────────────────────────────── */

#define MAX_WINDOWS     32
#define MAX_TITLE_LEN   64

/* Window flags */
#define WND_ACTIVE      (1 << 0)
#define WND_VISIBLE     (1 << 1)
#define WND_FULLSCREEN  (1 << 2)
#define WND_MINIMIZED   (1 << 3)
#define WND_FOCUSED     (1 << 4)
#define WND_DIRTY       (1 << 5)  /* surface has new content */

typedef struct {
    uint32_t id;
    int16_t  x, y;                 /* position on screen */
    uint16_t width, height;
    uint32_t shm_handle;           /* shared memory containing pixels */
    uint32_t *pixels;              /* mapped pointer to surface */
    uint32_t owner_pid;
    uint8_t  z_order;              /* 0 = bottom, higher = on top */
    uint8_t  flags;
    char     title[MAX_TITLE_LEN];
} window_t;

/* ── Compositor State ────────────────────────────────────────── */

static window_t windows[MAX_WINDOWS];
static uint32_t next_window_id = 1;
static int32_t  focused_window = -1;  /* index into windows[] */

/* Sorted window list for rendering (by z_order) */
static int render_order[MAX_WINDOWS];
static int render_count;

/* Mouse interaction state */
static bool     dragging;
static int32_t  drag_win_idx;       /* index into demo_windows[] */
static int32_t  drag_off_x, drag_off_y;
static uint8_t  prev_buttons;
static int      focused_demo_idx = 0;   /* 0=Terminal (focused by default) */

/* Window resize state */
static bool     resizing;
static int32_t  resize_win_idx;
#define RESIZE_EDGE  6          /* px from border to trigger resize */
#define RESIZE_MIN_W 100
#define RESIZE_MIN_H 60
/* Bitmask: which edges are being resized */
#define EDGE_LEFT   1
#define EDGE_RIGHT  2
#define EDGE_TOP    4
#define EDGE_BOTTOM 8
static uint8_t  resize_edges;    /* combination of EDGE_* */

/* Maximize / restore state (per demo window) */
static bool    is_maximized[2];
static int32_t saved_geom[2][4];    /* x, y, w, h before maximize */

/* ── Right-click context menu ────────────────────────────────── */

enum {
    CTX_CLOSE, CTX_MINIMIZE, CTX_MAXIMIZE, CTX_RESTORE,
    CTX_SNAP_LEFT, CTX_SNAP_RIGHT,
    CTX_SHOW_WIN0, CTX_SHOW_WIN1, CTX_SHOW_ALL
};

#define CTX_MAX_ITEMS  6
#define CTX_ITEM_H     (GUI_FONT_H + 8)
#define CTX_PAD_X      16

static struct {
    bool     visible;
    int32_t  x, y, w, h;
    int      count;
    int      hover;       /* -1 = none */
    int      target_win;  /* -1 = desktop */
    struct { const char *label; uint8_t action; } items[CTX_MAX_ITEMS];
} ctx_menu;

/* Alt+Tab overlay: show window title briefly after switching */
static uint64_t switcher_show_until;  /* tick when overlay disappears */

/* Modifier key state (tracked from HID virtual scancodes) */
static bool    alt_held;
static bool    comp_shift_held;
static bool    comp_ctrl_held;

/* Terminal surface: shm pixel buffer written by the shell thread,
 * blitted by the compositor over demo_windows[0] content area.
 * Set once from shell.c after compositor_init() and before the thread starts. */
static uint32_t  terminal_shm;
static uint32_t *terminal_pixels;
static uint32_t  terminal_w;
static uint32_t  terminal_h;

/* Desktop background now rendered by gui_desktop_render() */

/* Cursor appearance: 16×16 ARGB pre-rendered arrow with AA outline + shadow.
 * Built at init time by compositor_init_cursor(). */
#define CURSOR_W 16
#define CURSOR_H 16
static uint32_t cursor_rgba[CURSOR_W * CURSOR_H];

/* Stats */
static uint64_t comp_frames;
static uint64_t comp_direct_scanout;

/* FPS tracking (updated once per second via APIC ticks @ 100Hz) */
static uint64_t fps_last_tick;
static uint64_t fps_frame_count;
static uint64_t fps_display;  /* last computed FPS value */

/* ── Keyboard Ring Buffer (for user32_shim consumption) ──────── */

#define KEY_RING_SIZE 64
#define KEY_RING_MASK (KEY_RING_SIZE - 1)

typedef struct {
    uint32_t scancode;
    bool     pressed;
} key_ring_entry_t;

static key_ring_entry_t key_ring[KEY_RING_SIZE];
static volatile uint32_t key_ring_head;
static volatile uint32_t key_ring_tail;

/* Input state forwarded from last drain */
static uint8_t  comp_button_state;
static int16_t  comp_wheel_accum;

/* ── Window Management ───────────────────────────────────────── */

/* Register a new window. Returns window ID or 0 on failure. */
uint32_t compositor_create_window(uint32_t shm_handle,
                                  int16_t x, int16_t y,
                                  uint16_t width, uint16_t height,
                                  uint32_t pid, const char *title)
{
    /* Find free slot */
    window_t *w = NULL;
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE)) {
            w = &windows[i];
            slot = i;
            break;
        }
    }
    if (!w) return 0;

    w->id = next_window_id++;
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->shm_handle = shm_handle;
    w->pixels = (uint32_t *)shm_map(shm_handle);
    w->owner_pid = pid;
    w->z_order = (uint8_t)slot;
    w->flags = WND_ACTIVE | WND_VISIBLE | WND_DIRTY;

    /* Copy title */
    int j = 0;
    if (title) {
        while (title[j] && j < MAX_TITLE_LEN - 1) {
            w->title[j] = title[j];
            j++;
        }
    }
    w->title[j] = '\0';

    /* Focus new window */
    focused_window = slot;
    w->flags |= WND_FOCUSED;

    serial_puts("[COMP] Window created: id=");
    serial_putdec(w->id);
    serial_puts(" '");
    serial_puts(w->title);
    serial_puts("' ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(" @ ");
    serial_putdec((uint64_t)(uint16_t)x);
    serial_puts(",");
    serial_putdec((uint64_t)(uint16_t)y);
    serial_puts("\n");

    return w->id;
}

/* Destroy a window */
void compositor_destroy_window(uint32_t window_id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            if (windows[i].shm_handle)
                shm_unmap(windows[i].shm_handle);
            windows[i].flags = 0;
            if (focused_window == i) focused_window = -1;
            serial_puts("[COMP] Window destroyed: id=");
            serial_putdec(window_id);
            serial_puts("\n");
            return;
        }
    }
}

/* Destroy all windows owned by a process (called on proc_free) */
void compositor_cleanup_process(uint32_t pid)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].owner_pid == pid)
            compositor_destroy_window(windows[i].id);
    }
}

/* Signal that a window's surface has new content */
void compositor_signal_dirty(uint32_t window_id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            windows[i].flags |= WND_DIRTY;
            return;
        }
    }
}

/* Set fullscreen mode */
void compositor_set_fullscreen(uint32_t window_id, bool fullscreen)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            if (fullscreen) {
                windows[i].flags |= WND_FULLSCREEN;
                windows[i].x = 0;
                windows[i].y = 0;
            } else {
                windows[i].flags &= ~WND_FULLSCREEN;
            }
            return;
        }
    }
}

/* ── Build Render Order ──────────────────────────────────────── */

static void build_render_order(void)
{
    render_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & (WND_ACTIVE | WND_VISIBLE)) ==
            (WND_ACTIVE | WND_VISIBLE) &&
            !(windows[i].flags & WND_MINIMIZED)) {
            render_order[render_count++] = i;
        }
    }

    /* Simple insertion sort by z_order (stable, small N) */
    for (int i = 1; i < render_count; i++) {
        int key = render_order[i];
        int j = i - 1;
        while (j >= 0 && windows[render_order[j]].z_order > windows[key].z_order) {
            render_order[j + 1] = render_order[j];
            j--;
        }
        render_order[j + 1] = key;
    }
}

/* ── Blit Window to Back Buffer ──────────────────────────────── */

static void blit_window(uint32_t *dst, uint32_t dst_pitch,
                        uint32_t dst_w, uint32_t dst_h,
                        const window_t *w)
{
    if (!w->pixels) return;

    /* Clip to screen */
    int32_t sx = 0, sy = 0;
    int32_t dx = w->x, dy = w->y;
    int32_t bw = w->width, bh = w->height;

    if (dx < 0) { sx = -dx; bw += dx; dx = 0; }
    if (dy < 0) { sy = -dy; bh += dy; dy = 0; }
    if (dx + bw > (int32_t)dst_w) bw = (int32_t)dst_w - dx;
    if (dy + bh > (int32_t)dst_h) bh = (int32_t)dst_h - dy;
    if (bw <= 0 || bh <= 0) return;

    /* Fast opaque blit (scanline memcpy) */
    for (int32_t y = 0; y < bh; y++) {
        uint32_t *d = dst + ((uint32_t)(dy + y)) * dst_pitch + (uint32_t)dx;
        const uint32_t *s = w->pixels + ((uint32_t)(sy + y)) * w->width + (uint32_t)sx;
        memcpy(d, s, (uint64_t)bw * sizeof(uint32_t));
    }
}

/* ── Build AA cursor (called once at init) ───────────────────── */

/* Pre-render a 16x16 macOS-style arrow cursor with AA outline and shadow.
 * Arrow shape: tip at (0,0), body going to (0,14) and (10,10) with
 * a diagonal return stroke. Built via SDF (signed distance field approach). */
static void compositor_init_cursor(void)
{
    /* Arrow defined as fill mask: pixel (x,y) is inside arrow if:
     *   y >= x  (left edge: diagonal from tip)
     *   y <= 14 (bottom)
     *   x <= 10 (right limit of lower triangle at row 10)
     * Plus return stroke: x + y <= 14 for x >= 5 (approximate) */
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            /* Determine if pixel is inside the arrow shape */
            bool inside = false;

            /* Upper-left triangle: column 0 only (left edge = vertical spine) */
            if (x == 0 && y <= 13) inside = true;
            /* Main body: x in [0, y] for y in [0, 10] */
            if (y <= 10 && x <= y) inside = true;
            /* Lower triangle: converges at (10, 10) */
            if (y > 10 && y <= 13 && x <= (13 - y) + 0) {
                /* Narrow tail between (0,10) and (3,13) */
                if (x <= 13 - y) inside = true;
            }
            /* Diagonal stroke: from (3,10) to (10,3) */
            if (!inside && y >= 3 && y <= 10 && x >= 3 && x <= 10) {
                int32_t d = x + y - 13;
                if (d >= -1 && d <= 1) inside = true;
            }

            if (!inside) {
                cursor_rgba[y * CURSOR_W + x] = 0x00000000;
                continue;
            }

            /* Check if this is an edge pixel (has a transparent neighbor) */
            bool edge = false;
            if (x == 0 || y == 0 || x == CURSOR_W - 1 || y == CURSOR_H - 1)
                edge = true;
            else {
                /* Simplified edge detection: check 4 neighbors */
                bool n_inside[4];
                int nx[4] = {x-1, x+1, x, x};
                int ny[4] = {y, y, y-1, y+1};
                for (int k = 0; k < 4; k++) {
                    int bx = nx[k], by = ny[k];
                    bool bi = false;
                    if (bx == 0 && by <= 13) bi = true;
                    if (by <= 10 && bx <= by) bi = true;
                    if (by > 10 && by <= 13 && bx <= 13 - by) bi = true;
                    if (!bi && by >= 3 && by <= 10 && bx >= 3 && bx <= 10) {
                        int32_t d = bx + by - 13;
                        if (d >= -1 && d <= 1) bi = true;
                    }
                    n_inside[k] = bi;
                }
                for (int k = 0; k < 4; k++)
                    if (!n_inside[k]) { edge = true; break; }
            }

            if (edge) {
                /* Outline: black semi-transparent */
                cursor_rgba[y * CURSOR_W + x] = 0xCC000000;
            } else {
                /* Interior: white */
                cursor_rgba[y * CURSOR_W + x] = 0xFFFFFFFF;
            }
        }
    }
}

/* ── Draw Cursor ─────────────────────────────────────────────── */

static inline uint32_t cursor_blend(uint32_t dst, uint32_t src)
{
    uint32_t a = src >> 24;
    if (a == 0) return dst;
    if (a == 0xFF) return src;
    uint32_t inv = 255 - a;
    uint32_t rb = (((src & 0x00FF00FF) * a + (dst & 0x00FF00FF) * inv) >> 8) & 0x00FF00FF;
    uint32_t g  = (((src & 0x0000FF00) * a + (dst & 0x0000FF00) * inv) >> 8) & 0x0000FF00;
    return 0xFF000000 | rb | g;
}

static void draw_cursor(uint32_t *dst, uint32_t pitch,
                        uint32_t scr_w, uint32_t scr_h,
                        int32_t cx, int32_t cy)
{
    /* Shadow pass: draw cursor pixels shifted (1,1) with reduced alpha */
    for (int y = 0; y < CURSOR_H; y++) {
        int32_t sy = cy + y + 1;
        if (sy < 0 || sy >= (int32_t)scr_h) continue;
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t sx = cx + x + 1;
            if (sx < 0 || sx >= (int32_t)scr_w) continue;
            uint32_t src = cursor_rgba[y * CURSOR_W + x];
            uint32_t a = (src >> 24);
            if (a == 0) continue;
            uint32_t shadow = ((a >> 2) << 24); /* 25% alpha shadow */
            uint32_t *p = &dst[(uint32_t)sy * pitch + (uint32_t)sx];
            *p = cursor_blend(*p, shadow);
        }
    }

    /* Main cursor pass */
    for (int y = 0; y < CURSOR_H; y++) {
        int32_t py = cy + y;
        if (py < 0 || py >= (int32_t)scr_h) continue;
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t px = cx + x;
            if (px < 0 || px >= (int32_t)scr_w) continue;
            uint32_t src = cursor_rgba[y * CURSOR_W + x];
            if ((src >> 24) == 0) continue;
            uint32_t *p = &dst[(uint32_t)py * pitch + (uint32_t)px];
            *p = cursor_blend(*p, src);
        }
    }
}

/* ── Hit-testing for demo windows ─────────────────────────────── */

/* Check if point (mx,my) is inside a demo window (including titlebar).
 * Returns window index into demo_windows[], checking top-to-bottom in render order. */
static int hit_test_demo_window(int32_t mx, int32_t my)
{
    int count = 0;
    gui_win_desc_t *dw = gui_desktop_get_windows(&count);
    int *order = gui_desktop_get_order();

    /* Check front-to-back (last in order = on top → check first) */
    for (int i = count - 1; i >= 0; i--) {
        int idx = order[i];
        if (dw[idx].hidden) continue;
        int32_t wx = dw[idx].x;
        int32_t wy = dw[idx].y;
        int32_t ww = dw[idx].w + GUI_BORDER_W * 2;
        int32_t wh = dw[idx].h + GUI_TITLEBAR_H + GUI_BORDER_W * 2;
        if (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh)
            return idx;
    }
    return -1;
}

/* Check if point hits a traffic-light button. Returns 1=close, 2=min, 3=max, 0=none */
static int hit_test_buttons(gui_win_desc_t *w, int32_t mx, int32_t my)
{
    /* Button centers relative to window frame */
    int32_t by = w->y + GUI_TITLEBAR_H / 2;
    int32_t bx[3];
    bx[0] = w->x + 14;           /* close */
    bx[1] = bx[0] + GUI_BTN_RADIUS * 2 + 6; /* minimize */
    bx[2] = bx[1] + GUI_BTN_RADIUS * 2 + 6; /* maximize */

    for (int i = 0; i < 3; i++) {
        int32_t dx = mx - bx[i];
        int32_t dy = my - by;
        if (dx * dx + dy * dy <= (GUI_BTN_RADIUS + 2) * (GUI_BTN_RADIUS + 2))
            return i + 1;
    }
    return 0;
}

/* Hit-test the dock.
 * Returns:
 *   >= 0  : dock icon index (0, 1, 2, ...)
 *   -2    : inside dock bounds but not on an icon (padding area) → consume click
 *   -1    : completely outside dock → fall through to window hit test
 *
 * Geometry matches gui_dock_render() exactly. */
static int hit_test_dock(int32_t mx, int32_t my)
{
    uint32_t scr_w = display_get_width();
    uint32_t scr_h = display_get_height();
    int32_t item_slot = GUI_DOCK_ICON_SIZE + GUI_DOCK_PADDING;
    int32_t dock_w = 2 * item_slot + GUI_DOCK_PADDING;  /* 2 icons to match gui_dock.c */
    int32_t dock_h = GUI_DOCK_HEIGHT;
    int32_t dock_x = ((int32_t)scr_w - dock_w) / 2;
    int32_t dock_y = (int32_t)scr_h - dock_h - 8;

    static bool dock_geom_logged = false;
    if (!dock_geom_logged) {
        serial_puts("[DOCK] scr="); serial_putdec(scr_w);
        serial_puts("x"); serial_putdec(scr_h);
        serial_puts(" dock_x="); serial_putdec((uint64_t)dock_x);
        serial_puts(" dock_y="); serial_putdec((uint64_t)dock_y);
        serial_puts(" dock_w="); serial_putdec((uint64_t)dock_w);
        serial_puts(" dock_h="); serial_putdec((uint64_t)dock_h);
        serial_puts("\n");
        dock_geom_logged = true;
    }

    /* Outside dock area entirely */
    if (mx < dock_x || mx >= dock_x + dock_w) return -1;
    if (my < dock_y || my >= dock_y + dock_h) return -1;

    /* Inside dock bounds — check individual icon squares */
    for (int i = 0; i < 2; i++) {
        int32_t ix = dock_x + GUI_DOCK_PADDING + i * item_slot;
        int32_t iy = dock_y + (dock_h - GUI_DOCK_ICON_SIZE) / 2;
        if (mx >= ix && mx < ix + GUI_DOCK_ICON_SIZE &&
            my >= iy && my < iy + GUI_DOCK_ICON_SIZE)
            return i;
    }
    /* Inside dock background / padding — consume the click (dock is on top) */
    return -2;
}

/* ── Context menu helpers ─────────────────────────────────── */

static void ctx_menu_open_window(int32_t mx, int32_t my, int win_idx)
{
    ctx_menu.count = 0;
    ctx_menu.target_win = win_idx;

    ctx_menu.items[ctx_menu.count].label = "Cerrar";
    ctx_menu.items[ctx_menu.count++].action = CTX_CLOSE;
    ctx_menu.items[ctx_menu.count].label = "Minimizar";
    ctx_menu.items[ctx_menu.count++].action = CTX_MINIMIZE;
    if (is_maximized[win_idx]) {
        ctx_menu.items[ctx_menu.count].label = "Restaurar";
        ctx_menu.items[ctx_menu.count++].action = CTX_RESTORE;
    } else {
        ctx_menu.items[ctx_menu.count].label = "Maximizar";
        ctx_menu.items[ctx_menu.count++].action = CTX_MAXIMIZE;
    }
    ctx_menu.items[ctx_menu.count].label = "Snap izquierda";
    ctx_menu.items[ctx_menu.count++].action = CTX_SNAP_LEFT;
    ctx_menu.items[ctx_menu.count].label = "Snap derecha";
    ctx_menu.items[ctx_menu.count++].action = CTX_SNAP_RIGHT;

    /* Calculate dimensions */
    int max_tw = 0;
    for (int i = 0; i < ctx_menu.count; i++) {
        int tw = gui_text_width(ctx_menu.items[i].label);
        if (tw > max_tw) max_tw = tw;
    }
    ctx_menu.w = max_tw + CTX_PAD_X * 2;
    ctx_menu.h = ctx_menu.count * CTX_ITEM_H + 8;

    /* Clamp to screen */
    int32_t sw = (int32_t)display_get_width();
    int32_t sh = (int32_t)display_get_height();
    ctx_menu.x = (mx + ctx_menu.w > sw) ? sw - ctx_menu.w : mx;
    ctx_menu.y = (my + ctx_menu.h > sh) ? sh - ctx_menu.h : my;
    ctx_menu.hover = -1;
    ctx_menu.visible = true;
}

static void ctx_menu_open_desktop(int32_t mx, int32_t my)
{
    ctx_menu.count = 0;
    ctx_menu.target_win = -1;

    ctx_menu.items[ctx_menu.count].label = "Mostrar Terminal";
    ctx_menu.items[ctx_menu.count++].action = CTX_SHOW_WIN0;
    ctx_menu.items[ctx_menu.count].label = "Mostrar System Info";
    ctx_menu.items[ctx_menu.count++].action = CTX_SHOW_WIN1;
    ctx_menu.items[ctx_menu.count].label = "Mostrar todo";
    ctx_menu.items[ctx_menu.count++].action = CTX_SHOW_ALL;

    int max_tw = 0;
    for (int i = 0; i < ctx_menu.count; i++) {
        int tw = gui_text_width(ctx_menu.items[i].label);
        if (tw > max_tw) max_tw = tw;
    }
    ctx_menu.w = max_tw + CTX_PAD_X * 2;
    ctx_menu.h = ctx_menu.count * CTX_ITEM_H + 8;

    int32_t sw = (int32_t)display_get_width();
    int32_t sh = (int32_t)display_get_height();
    ctx_menu.x = (mx + ctx_menu.w > sw) ? sw - ctx_menu.w : mx;
    ctx_menu.y = (my + ctx_menu.h > sh) ? sh - ctx_menu.h : my;
    ctx_menu.hover = -1;
    ctx_menu.visible = true;
}

static void ctx_menu_execute(int action_idx);  /* forward declaration */

/* Callback: mark window hidden after shrink animation completes */
static void on_shrink_complete(void *ctx)
{
    gui_win_desc_t *w = (gui_win_desc_t *)ctx;
    w->hidden = true;
}

/* Helper: hide window with shrink animation (used by close, minimize, context menu) */
static void hide_window_animated(gui_win_desc_t *dw, int hit, int count)
{
    gui_anim_cancel(&dw[hit].x);
    gui_anim_cancel(&dw[hit].y);
    gui_anim_cancel(&dw[hit].w);
    gui_anim_cancel(&dw[hit].h);
    if (focused_demo_idx == hit) {
        focused_demo_idx = -1;
        for (int fi = 0; fi < count; fi++)
            if (!dw[fi].hidden) { focused_demo_idx = fi; break; }
    }
    int32_t mid_x = dw[hit].x + dw[hit].w / 2;
    int32_t mid_y = dw[hit].y + dw[hit].h / 2;
    gui_anim_start(&dw[hit].x, mid_x, 200, gui_ease_in_out_quad, NULL, NULL);
    gui_anim_start(&dw[hit].y, mid_y, 200, gui_ease_in_out_quad, NULL, NULL);
    gui_anim_start(&dw[hit].w, 0, 200, gui_ease_in_out_quad, NULL, NULL);
    gui_anim_start(&dw[hit].h, 0, 200, gui_ease_in_out_quad,
                   on_shrink_complete, &dw[hit]);
}

/* Helper: snap window to half-screen (used by shortcuts + context menu) */
static void snap_window(gui_win_desc_t *dw, int idx, bool left)
{
    int32_t sw2 = (int32_t)display_get_width();
    int32_t usable_h = (int32_t)display_get_height() - GUI_PANEL_HEIGHT - GUI_DOCK_HEIGHT;
    gui_anim_start(&dw[idx].x, left ? 0 : sw2 / 2, 250, gui_ease_out_cubic, NULL, NULL);
    gui_anim_start(&dw[idx].y, GUI_PANEL_HEIGHT, 250, gui_ease_out_cubic, NULL, NULL);
    gui_anim_start(&dw[idx].w, sw2 / 2, 250, gui_ease_out_cubic, NULL, NULL);
    gui_anim_start(&dw[idx].h, usable_h, 250, gui_ease_out_cubic, NULL, NULL);
    is_maximized[idx] = false;
}

/* Execute context menu action */
static void ctx_menu_execute(int action_idx)
{
    if (action_idx < 0 || action_idx >= ctx_menu.count) return;
    uint8_t action = ctx_menu.items[action_idx].action;
    int win = ctx_menu.target_win;
    int count;
    gui_win_desc_t *dw = gui_desktop_get_windows(&count);

    switch (action) {
    case CTX_CLOSE:
    case CTX_MINIMIZE:
        if (win >= 0 && win < count)
            hide_window_animated(dw, win, count);
        break;
    case CTX_MAXIMIZE:
        if (win >= 0 && win < count && !is_maximized[win]) {
            saved_geom[win][0] = dw[win].x; saved_geom[win][1] = dw[win].y;
            saved_geom[win][2] = dw[win].w; saved_geom[win][3] = dw[win].h;
            int32_t sw2 = (int32_t)display_get_width();
            int32_t uh = (int32_t)display_get_height() - GUI_PANEL_HEIGHT - GUI_DOCK_HEIGHT;
            gui_anim_start(&dw[win].x, 0, 300, gui_ease_out_cubic, NULL, NULL);
            gui_anim_start(&dw[win].y, GUI_PANEL_HEIGHT, 300, gui_ease_out_cubic, NULL, NULL);
            gui_anim_start(&dw[win].w, sw2, 300, gui_ease_out_cubic, NULL, NULL);
            gui_anim_start(&dw[win].h, uh, 300, gui_ease_out_cubic, NULL, NULL);
            is_maximized[win] = true;
        }
        break;
    case CTX_RESTORE:
        if (win >= 0 && win < count && is_maximized[win]) {
            gui_anim_start(&dw[win].x, saved_geom[win][0], 280, gui_ease_out_cubic, NULL, NULL);
            gui_anim_start(&dw[win].y, saved_geom[win][1], 280, gui_ease_out_cubic, NULL, NULL);
            gui_anim_start(&dw[win].w, saved_geom[win][2], 280, gui_ease_out_cubic, NULL, NULL);
            gui_anim_start(&dw[win].h, saved_geom[win][3], 280, gui_ease_out_cubic, NULL, NULL);
            is_maximized[win] = false;
        }
        break;
    case CTX_SNAP_LEFT:
        if (win >= 0 && win < count) snap_window(dw, win, true);
        break;
    case CTX_SNAP_RIGHT:
        if (win >= 0 && win < count) snap_window(dw, win, false);
        break;
    case CTX_SHOW_WIN0:
        gui_desktop_show_window(0);
        is_maximized[0] = false;
        focused_demo_idx = 0;
        break;
    case CTX_SHOW_WIN1:
        gui_desktop_show_window(1);
        is_maximized[1] = false;
        focused_demo_idx = 1;
        break;
    case CTX_SHOW_ALL:
        for (int i = 0; i < count; i++) {
            if (dw[i].hidden) {
                gui_desktop_show_window(i);
                is_maximized[i] = false;
            }
        }
        if (focused_demo_idx < 0) focused_demo_idx = 0;
        break;
    }
    ctx_menu.visible = false;
}

/* Process mouse clicks on demo windows */
static void process_mouse_input(void)
{
    uint8_t pressed  = comp_button_state & ~prev_buttons;
    uint8_t released = prev_buttons & ~comp_button_state;

    /* Context menu: right-click opens, any click outside closes */
    if (pressed & 2) {  /* right-click: open context menu */
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);
        ctx_menu.visible = false;  /* close previous */
        int hit = hit_test_demo_window(cx, cy);
        if (hit >= 0)
            ctx_menu_open_window(cx, cy, hit);
        else
            ctx_menu_open_desktop(cx, cy);
        prev_buttons = comp_button_state;
        return;
    }

    if (pressed & 1) {  /* left button newly pressed */
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);

        /* If context menu is open, handle it first */
        if (ctx_menu.visible) {
            if (cx >= ctx_menu.x && cx < ctx_menu.x + ctx_menu.w &&
                cy >= ctx_menu.y && cy < ctx_menu.y + ctx_menu.h) {
                int idx = (cy - ctx_menu.y - 4) / CTX_ITEM_H;
                if (idx >= 0 && idx < ctx_menu.count)
                    ctx_menu_execute(idx);
            }
            ctx_menu.visible = false;
            prev_buttons = comp_button_state;
            return;  /* consume click */
        }

        serial_puts("[COMP] LMB cx="); serial_putdec((uint64_t)cx);
        serial_puts(" cy="); serial_putdec((uint64_t)cy);
        serial_puts("\n");

        /* Check dock first — it renders on top of everything and should
         * receive clicks even when a window overlaps the dock area.
         * dock_hit >= 0 → icon index; -2 → dock padding (consume, no action);
         * -1 → outside dock entirely, fall through to window hit test. */
        int dock_hit = hit_test_dock(cx, cy);
        if (dock_hit >= -2 && dock_hit != -1) {
            if (dock_hit >= 0) {
                /* Icon click: map icon index → demo_window index.
                 * Dock icon 0 = Terminal (window 0), icon 1 = Files (window 1).
                 * Icons beyond the window count are cosmetic only. */
                int count2;
                gui_desktop_get_windows(&count2);
                if (dock_hit < count2) {
                    gui_desktop_show_window(dock_hit);
                    is_maximized[dock_hit] = false;  /* reset maximize state on reopen */
                    focused_demo_idx = dock_hit;
                    serial_puts("[COMP] Dock icon="); serial_putdec((uint64_t)dock_hit);
                    serial_puts(" raised\n");
                }
            }
            prev_buttons = comp_button_state;
            return;  /* dock consumed the click */
        }

        int hit = hit_test_demo_window(cx, cy);
        focused_demo_idx = hit;  /* -1 if missed all windows */

        if (hit >= 0) {
            int count;
            gui_win_desc_t *dw = gui_desktop_get_windows(&count);

            /* Check traffic-light buttons first */
            int btn = hit_test_buttons(&dw[hit], cx, cy);
            if (btn == 1 || btn == 2) {
                /* Close/Minimize: animate shrink then hide via callback */
                hide_window_animated(dw, hit, count);
            } else if (btn == 3) {
                /* Maximize / restore toggle — animated (Phase 3.3) */
                if (is_maximized[hit]) {
                    gui_anim_start(&dw[hit].x, saved_geom[hit][0], 280, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].y, saved_geom[hit][1], 280, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].w, saved_geom[hit][2], 280, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].h, saved_geom[hit][3], 280, gui_ease_out_cubic, NULL, NULL);
                    is_maximized[hit] = false;
                } else {
                    saved_geom[hit][0] = dw[hit].x;
                    saved_geom[hit][1] = dw[hit].y;
                    saved_geom[hit][2] = dw[hit].w;
                    saved_geom[hit][3] = dw[hit].h;
                    gui_anim_start(&dw[hit].x, 0, 300, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].y, GUI_PANEL_HEIGHT, 300, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].w, (int32_t)display_get_width(), 300, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].h, (int32_t)display_get_height() - GUI_PANEL_HEIGHT
                                              - GUI_DOCK_HEIGHT, 300, gui_ease_out_cubic, NULL, NULL);
                    is_maximized[hit] = true;
                }
            } else if (cy < dw[hit].y + GUI_TITLEBAR_H) {
                /* Titlebar click → start drag */
                dragging = true;
                drag_win_idx = hit;
                drag_off_x = cx - dw[hit].x;
                drag_off_y = cy - dw[hit].y;
            } else {
                /* Check if click is near a window edge → start resize */
                int32_t wx = dw[hit].x;
                int32_t wy = dw[hit].y + GUI_TITLEBAR_H;
                int32_t ww = dw[hit].w;
                int32_t wh = dw[hit].h;
                uint8_t edges = 0;
                if (cx - wx < RESIZE_EDGE)          edges |= EDGE_LEFT;
                if (wx + ww - cx < RESIZE_EDGE)     edges |= EDGE_RIGHT;
                if (cy - wy < RESIZE_EDGE)          edges |= EDGE_TOP;
                if (wy + wh - cy < RESIZE_EDGE)     edges |= EDGE_BOTTOM;
                if (edges) {
                    resizing = true;
                    resize_win_idx = hit;
                    resize_edges = edges;
                    is_maximized[hit] = false;
                }
            }

            /* Raise clicked window to front (changes render order, not struct data) */
            gui_desktop_raise_window(hit);
        }
    }

    /* Continue drag while button held */
    if (dragging && (comp_button_state & 1)) {
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);
        int count;
        gui_win_desc_t *dw = gui_desktop_get_windows(&count);
        if (drag_win_idx >= 0 && drag_win_idx < count &&
            !dw[drag_win_idx].hidden) {
            int32_t nx = cx - drag_off_x;
            int32_t ny = cy - drag_off_y;
            int32_t ww = dw[drag_win_idx].w;
            int32_t sw = (int32_t)display_get_width();
            int32_t sh = (int32_t)display_get_height();
            /* Clamp: keep titlebar partially on screen */
            if (nx < -(ww - 50)) nx = -(ww - 50);
            if (nx > sw - 50)    nx = sw - 50;
            if (ny < 0)          ny = 0;
            if (ny > sh - GUI_TITLEBAR_H) ny = sh - GUI_TITLEBAR_H;
            dw[drag_win_idx].x = nx;
            dw[drag_win_idx].y = ny;
        }
    }

    /* Continue resize while button held */
    if (resizing && (comp_button_state & 1)) {
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);
        int count;
        gui_win_desc_t *dw = gui_desktop_get_windows(&count);
        if (resize_win_idx >= 0 && resize_win_idx < count &&
            !dw[resize_win_idx].hidden) {
            gui_win_desc_t *rw = &dw[resize_win_idx];
            if (resize_edges & EDGE_RIGHT) {
                int32_t nw = cx - rw->x;
                if (nw >= RESIZE_MIN_W) rw->w = nw;
            }
            if (resize_edges & EDGE_BOTTOM) {
                int32_t nh = cy - rw->y - GUI_TITLEBAR_H;
                if (nh >= RESIZE_MIN_H) rw->h = nh;
            }
            if (resize_edges & EDGE_LEFT) {
                int32_t right = rw->x + rw->w;
                int32_t nw = right - cx;
                if (nw >= RESIZE_MIN_W) { rw->x = cx; rw->w = nw; }
            }
            if (resize_edges & EDGE_TOP) {
                int32_t bottom = rw->y + GUI_TITLEBAR_H + rw->h;
                int32_t nh = bottom - cy - GUI_TITLEBAR_H;
                if (nh >= RESIZE_MIN_H) { rw->y = cy; rw->h = nh; }
            }
        }
    }

    /* Release */
    if (released & 1) {
        dragging = false;
        drag_win_idx = -1;
        resizing = false;
        resize_win_idx = -1;
    }

    prev_buttons = comp_button_state;
}

/* ── Terminal Surface ────────────────────────────────────────── */

/* Return the pixel dimensions of the Terminal window content area.
 * Called from shell.c to size the shm surface before fb_redirect(). */
/* Get dimensions of a window by its SHM handle */
uint32_t compositor_get_window_dims(uint32_t shm_handle,
                                    uint16_t *out_w, uint16_t *out_h)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].shm_handle == shm_handle) {
            if (out_w) *out_w = windows[i].width;
            if (out_h) *out_h = windows[i].height;
            return 1;
        }
    }
    return 0;
}

void compositor_get_terminal_dims(uint32_t *tw, uint32_t *th)
{
    int count;
    gui_win_desc_t *dw = gui_desktop_get_windows(&count);
    if (count > 0) {
        *tw = (uint32_t)dw[0].w;
        *th = (uint32_t)dw[0].h;
    } else {
        *tw = 640;
        *th = 400;
    }
}

/* Called from shell.c desktop command after compositor_init() and before
 * spawning the compositor thread. Stores the shm surface that the shell
 * renders into so the compositor can blit it each frame. */
void compositor_set_terminal_surface(uint32_t shm, uint32_t tw, uint32_t th)
{
    terminal_shm    = shm;
    terminal_pixels = (uint32_t *)shm_map(shm);
    terminal_w      = tw;
    terminal_h      = th;

    /* Let gui_desktop blit the terminal at the correct z-order position */
    gui_desktop_set_terminal_surface(terminal_pixels, tw, th);

    serial_puts("[COMP] Terminal surface registered: ");
    serial_putdec(tw);
    serial_puts("x");
    serial_putdec(th);
    serial_puts("\n");
}

/* ── Render One Frame ────────────────────────────────────────── */

static void compositor_render_frame(void)
{
    uint32_t *back = display_get_back_buffer();
    uint32_t w = display_get_width();
    uint32_t h = display_get_height();
    uint32_t p = display_get_pitch();

    if (!back) return;

    build_render_order();

    /* Fullscreen window: bypass desktop UI, blit scaled to screen.
     * Supports any source size via pixel-perfect integer upscaling. */
    for (int _fi = 0; _fi < render_count; _fi++) {
        window_t *win = &windows[render_order[_fi]];
        if (!(win->flags & WND_FULLSCREEN) || !win->pixels) continue;

        /* Direct scanout: source matches screen exactly */
        if (win->width == (uint16_t)w && win->height == (uint16_t)h) {
            memcpy(back, win->pixels, (uint64_t)w * h * 4);
            display_mark_dirty();
            comp_direct_scanout++;
            return;
        }

        /* Pixel-perfect: largest integer scale that fits within the screen.
         * For DOOM 320×200 on 1024×768: scale=3 → 960×600, centered. */
        uint32_t _sw = (uint32_t)win->width;
        uint32_t _sh = (uint32_t)win->height;
        uint32_t _scx = w / _sw;
        uint32_t _scy = h / _sh;
        uint32_t _sc  = (_scx < _scy) ? _scx : _scy;
        if (_sc == 0) _sc = 1;

        uint32_t _dw = _sw * _sc;
        uint32_t _dh = _sh * _sc;
        uint32_t _ox = (w - _dw) / 2;
        uint32_t _oy = (h - _dh) / 2;

        /* Clear letterbox/pillarbox borders to black */
        memset(back, 0, (uint64_t)p * h * 4);

        /* Scale-blit: build one scaled row, then memcpy for repeated rows.
         * Row-major traversal keeps writes sequential for cache efficiency. */
        for (uint32_t _y = 0; _y < _sh; _y++) {
            const uint32_t *_src = win->pixels + _y * _sw;
            uint32_t *_row0 = back + (_oy + _y * _sc) * p + _ox;
            /* Expand source row horizontally */
            for (uint32_t _x = 0; _x < _sw; _x++) {
                uint32_t _px = _src[_x];
                uint32_t _base = _x * _sc;
                for (uint32_t _rx = 0; _rx < _sc; _rx++)
                    _row0[_base + _rx] = _px;
            }
            /* Duplicate expanded row for remaining scale-1 output rows */
            for (uint32_t _ry = 1; _ry < _sc; _ry++)
                memcpy(back + (_oy + _y * _sc + _ry) * p + _ox, _row0,
                       (uint64_t)_dw * 4);
        }

        display_mark_dirty();
        comp_direct_scanout++;
        return;
    }

    /* Update FPS counter (APIC timer @ 100Hz → 100 ticks = 1 second) */
    fps_frame_count++;
    {
        uint64_t now = idt_get_ticks();
        if (now - fps_last_tick >= 100) {
            fps_display = fps_frame_count;
            fps_frame_count = 0;
            fps_last_tick = now;
        }
    }

    /* Push debug info into panel for rendering */
    {
        gui_debug_info_t info;
        info.fps = fps_display;
        info.mem_used_mb  = mem_get_used()  / (1024 * 1024);
        info.mem_free_mb  = mem_get_free()  / (1024 * 1024);
        info.mem_total_mb = mem_get_total() / (1024 * 1024);
        info.ticks = idt_get_ticks();
        info.procs = proc_count_active();
        /* Read RTC time */
#ifdef __EMSCRIPTEN__
        /* Use JS Date object — already in local timezone */
        info.rtc_hour = (uint8_t)EM_ASM_INT({ return new Date().getHours(); });
        info.rtc_min  = (uint8_t)EM_ASM_INT({ return new Date().getMinutes(); });
        info.rtc_sec  = (uint8_t)EM_ASM_INT({ return new Date().getSeconds(); });
#else
        /* Prefer NTP-synced time if available, else CMOS RTC */
        extern uint32_t ntp_get_utc(void) __attribute__((weak));
        extern bool     ntp_is_synced(void) __attribute__((weak));
        if (ntp_is_synced && ntp_is_synced()) {
            uint32_t utc = ntp_get_utc();
            info.rtc_sec  = (uint8_t)(utc % 60);
            info.rtc_min  = (uint8_t)((utc / 60) % 60);
            info.rtc_hour = (uint8_t)(((utc / 3600) + 24 - 3) % 24);  /* GMT-3 */
        } else {
            /* CMOS RTC (BCD mode, 24h), adjust to GMT-3 */
            info.rtc_sec  = bcd2bin(cmos_read(0x00));
            info.rtc_min  = bcd2bin(cmos_read(0x02));
            uint8_t raw_h = bcd2bin(cmos_read(0x04));
            info.rtc_hour = (raw_h + 24 - 3) % 24;
        }
#endif
        gui_panel_set_debug(&info);
    }

    /* Pass cursor to desktop layer for dock hover effects */
    {
        int32_t mx, my;
        input_get_cursor(&mx, &my);
        gui_desktop_set_cursor(mx, my);
    }

    /* Render elementaryOS-inspired desktop (bg, panel, window chrome, dock) */
    {
        gui_surface_t screen = { back, w, h, p };
        gui_desktop_render(&screen);
    }

    /* Terminal surface is now blitted by gui_desktop_render() at the correct
     * z-order position (inside render_window_content for window 0). */

    /* Blit windows bottom-to-top */
    for (int i = 0; i < render_count; i++) {
        blit_window(back, p, w, h, &windows[render_order[i]]);
    }

    /* Alt+Tab window switcher overlay */
    if (switcher_show_until > idt_get_ticks() && focused_demo_idx >= 0) {
        int sw_count;
        gui_win_desc_t *sw_dw = gui_desktop_get_windows(&sw_count);
        if (focused_demo_idx < sw_count && sw_dw[focused_demo_idx].title) {
            const char *title = sw_dw[focused_demo_idx].title;
            int tw = gui_text_width(title);
            int32_t bw = tw + 32;
            int32_t bh = GUI_FONT_H + 20;
            int32_t bx = ((int32_t)w - bw) / 2;
            int32_t by = ((int32_t)h - bh) / 2;
            gui_surface_t scr = { back, w, h, p };
            gui_rounded_rect_alpha(&scr, bx, by, bw, bh, 8, 0xD0202028);
            gui_draw_text_centered(&scr, bx, by + 10, bw, title,
                                   0xFFFFFFFF, 0);
        }
    }

    /* Context menu overlay */
    if (ctx_menu.visible) {
        gui_surface_t scr = { back, w, h, p };
        /* Shadow */
        gui_box_shadow(&scr, ctx_menu.x, ctx_menu.y, ctx_menu.w, ctx_menu.h,
                       2, 3, 8, 0x40000000);
        /* Background */
        gui_rounded_rect_alpha(&scr, ctx_menu.x, ctx_menu.y,
                               ctx_menu.w, ctx_menu.h, 6, 0xE8202028);
        /* Border */
        gui_rounded_rect_alpha(&scr, ctx_menu.x, ctx_menu.y,
                               ctx_menu.w, ctx_menu.h, 6, 0x30FFFFFF);
        /* Update hover based on cursor position */
        int32_t cmx, cmy;
        input_get_cursor(&cmx, &cmy);
        ctx_menu.hover = -1;
        if (cmx >= ctx_menu.x && cmx < ctx_menu.x + ctx_menu.w &&
            cmy >= ctx_menu.y && cmy < ctx_menu.y + ctx_menu.h) {
            ctx_menu.hover = (cmy - ctx_menu.y - 4) / CTX_ITEM_H;
            if (ctx_menu.hover >= ctx_menu.count) ctx_menu.hover = -1;
        }
        /* Items */
        for (int i = 0; i < ctx_menu.count; i++) {
            int32_t iy = ctx_menu.y + 4 + i * CTX_ITEM_H;
            if (i == ctx_menu.hover) {
                gui_fill_rect_alpha(&scr, ctx_menu.x + 4, iy,
                                    ctx_menu.w - 8, CTX_ITEM_H, 0x80007AFF);
            }
            gui_draw_text_aa(&scr, ctx_menu.x + CTX_PAD_X,
                             iy + (CTX_ITEM_H - GUI_FONT_H) / 2,
                             ctx_menu.items[i].label,
                             (i == ctx_menu.hover) ? 0xFFFFFFFF : 0xFFE0E0E0);
        }
    }

    /* Draw cursor on top */
    int32_t cx, cy;
    input_get_cursor(&cx, &cy);
    draw_cursor(back, p, w, h, cx, cy);

    display_mark_dirty();
}

/* ── Compositor Main Loop ────────────────────────────────────── */

/* This runs as a QOS_INTERACTIVE kernel thread.
 * Called from sched_spawn("compositor", compositor_thread). */

static volatile bool compositor_running;

void compositor_thread(void)
{
    proc_set_qos(QOS_INTERACTIVE);
    compositor_running = true;

    serial_puts("[COMP] Compositor thread started (QOS_INTERACTIVE)\n");

    bool comp_was_fullscreen = false;

    while (compositor_running) {
        /* 0. Poll USB HID (xHCI) for new mouse/keyboard reports */
        if (xhci_poll) xhci_poll();

        /* Game mode: if a fullscreen window (e.g. DOOM) is active, stop
         * draining keyboard events so the process can read them via
         * SYS_GET_INPUT_EVENT. Only mouse/wheel events are coalesced. */
        bool has_fullscreen = false;
        {
            extern bool input_game_mode;
            for (int _i = 0; _i < MAX_WINDOWS; _i++) {
                if ((windows[_i].flags & (WND_ACTIVE | WND_FULLSCREEN)) ==
                    (WND_ACTIVE | WND_FULLSCREEN)) {
                    has_fullscreen = true;
                    break;
                }
            }
            input_game_mode = has_fullscreen;
        }

        /* 1. Process input with coalescing */
        if (input_has_events()) {
            int16_t mdx, mdy, wheel;
            uint8_t buttons;
            /* Drain all events, accumulate mouse deltas */
            /* key_out buffer on stack (max 32 events per frame) */
            uint8_t key_buf[32 * 24];  /* 32 × sizeof(input_event_t) */
            int nkeys = input_drain_coalesced(&mdx, &mdy, &buttons,
                                              &wheel, key_buf, 32);

            /* NOTE: Do NOT re-post mouse deltas here. input_set_mouse_abs()
             * already updated mouse_x/mouse_y when xhci_poll processed the
             * tablet report. Re-posting would double-count movement. */

            /* Store button and wheel state for focused window */
            comp_button_state = buttons;
            comp_wheel_accum += wheel;

            /* Push keyboard events into the ring buffer */
            /* Each key_buf entry is an input_event_t (24 bytes):
             *   offset 0: type (uint8_t)
             *   offset 1: scancode (uint8_t) */
            if (nkeys > 0) {
                serial_puts("[COMP] keys="); serial_putdec((uint64_t)nkeys); serial_puts("\n");
            }
            for (int i = 0; i < nkeys; i++) {
                uint8_t *evt = &key_buf[i * 24];
                uint8_t type = evt[0];
                uint8_t sc   = evt[1];

                /* Track modifier key state from HID virtual scancodes (0xE0+index). */
                if (sc == 0xE2 || sc == 0xE6 || sc == 0x38) alt_held        = (type == 1);
                if (sc == 0xE1 || sc == 0xE5)               comp_shift_held = (type == 1);
                if (sc == 0xE0 || sc == 0xE4)               comp_ctrl_held  = (type == 1);

                /* Super (GUI key): HID 0xE3 left, 0xE7 right */
                static bool super_held;
                if (sc == 0xE3 || sc == 0xE7) super_held = (type == 1);

                /* Super+H: hide focused window */
                if (type == 1 && super_held && (sc == 0x0B /* HID H */)) {
                    if (focused_demo_idx >= 0) {
                        int count2;
                        gui_win_desc_t *dw2 = gui_desktop_get_windows(&count2);
                        if (focused_demo_idx < count2 && !dw2[focused_demo_idx].hidden) {
                            gui_anim_cancel(&dw2[focused_demo_idx].x);
                            gui_anim_cancel(&dw2[focused_demo_idx].y);
                            gui_anim_cancel(&dw2[focused_demo_idx].w);
                            gui_anim_cancel(&dw2[focused_demo_idx].h);
                            int32_t mx2 = dw2[focused_demo_idx].x + dw2[focused_demo_idx].w / 2;
                            int32_t my2 = dw2[focused_demo_idx].y + dw2[focused_demo_idx].h / 2;
                            gui_anim_start(&dw2[focused_demo_idx].x, mx2, 200, gui_ease_in_out_quad, NULL, NULL);
                            gui_anim_start(&dw2[focused_demo_idx].y, my2, 200, gui_ease_in_out_quad, NULL, NULL);
                            gui_anim_start(&dw2[focused_demo_idx].w, 0, 200, gui_ease_in_out_quad, NULL, NULL);
                            gui_anim_start(&dw2[focused_demo_idx].h, 0, 200, gui_ease_in_out_quad,
                                           on_shrink_complete, &dw2[focused_demo_idx]);
                            focused_demo_idx = -1;
                            for (int fi = 0; fi < count2; fi++)
                                if (!dw2[fi].hidden) { focused_demo_idx = fi; break; }
                        }
                    }
                    continue;
                }

                /* Super+Left/Right: snap window to left/right half */
                if (type == 1 && super_held && focused_demo_idx >= 0) {
                    int cnt;
                    gui_win_desc_t *dws = gui_desktop_get_windows(&cnt);
                    if (focused_demo_idx < cnt && !dws[focused_demo_idx].hidden) {
                        int32_t sw2 = (int32_t)display_get_width();
                        int32_t sh2 = (int32_t)display_get_height();
                        int32_t usable_h = sh2 - GUI_PANEL_HEIGHT - GUI_DOCK_HEIGHT;
                        if (sc == 0x50 /* HID Left */) {
                            gui_anim_start(&dws[focused_demo_idx].x, 0, 250, gui_ease_out_cubic, NULL, NULL);
                            gui_anim_start(&dws[focused_demo_idx].y, GUI_PANEL_HEIGHT, 250, gui_ease_out_cubic, NULL, NULL);
                            gui_anim_start(&dws[focused_demo_idx].w, sw2 / 2, 250, gui_ease_out_cubic, NULL, NULL);
                            gui_anim_start(&dws[focused_demo_idx].h, usable_h, 250, gui_ease_out_cubic, NULL, NULL);
                            is_maximized[focused_demo_idx] = false;
                            continue;
                        } else if (sc == 0x4F /* HID Right */) {
                            gui_anim_start(&dws[focused_demo_idx].x, sw2 / 2, 250, gui_ease_out_cubic, NULL, NULL);
                            gui_anim_start(&dws[focused_demo_idx].y, GUI_PANEL_HEIGHT, 250, gui_ease_out_cubic, NULL, NULL);
                            gui_anim_start(&dws[focused_demo_idx].w, sw2 / 2, 250, gui_ease_out_cubic, NULL, NULL);
                            gui_anim_start(&dws[focused_demo_idx].h, usable_h, 250, gui_ease_out_cubic, NULL, NULL);
                            is_maximized[focused_demo_idx] = false;
                            continue;
                        } else if (sc == 0x52 /* HID Up */) {
                            /* Super+Up = maximize */
                            if (!is_maximized[focused_demo_idx]) {
                                saved_geom[focused_demo_idx][0] = dws[focused_demo_idx].x;
                                saved_geom[focused_demo_idx][1] = dws[focused_demo_idx].y;
                                saved_geom[focused_demo_idx][2] = dws[focused_demo_idx].w;
                                saved_geom[focused_demo_idx][3] = dws[focused_demo_idx].h;
                                gui_anim_start(&dws[focused_demo_idx].x, 0, 300, gui_ease_out_cubic, NULL, NULL);
                                gui_anim_start(&dws[focused_demo_idx].y, GUI_PANEL_HEIGHT, 300, gui_ease_out_cubic, NULL, NULL);
                                gui_anim_start(&dws[focused_demo_idx].w, sw2, 300, gui_ease_out_cubic, NULL, NULL);
                                gui_anim_start(&dws[focused_demo_idx].h, usable_h, 300, gui_ease_out_cubic, NULL, NULL);
                                is_maximized[focused_demo_idx] = true;
                            }
                            continue;
                        }
                    }
                }

                /* Alt+Tab: cycle through windows (USB Tab = HID 0x2B, PS/2 Tab = 0x0F) */
                if (type == 1 && alt_held && (sc == 0x2B || sc == 0x0F)) {
                    int count;
                    gui_win_desc_t *dw_tab = gui_desktop_get_windows(&count);
                    if (count > 1) {
                        /* Cycle to next visible (non-hidden) window */
                        int start = focused_demo_idx;
                        for (int attempt = 0; attempt < count; attempt++) {
                            focused_demo_idx = (focused_demo_idx + 1) % count;
                            if (!dw_tab[focused_demo_idx].hidden) break;
                        }
                        if (dw_tab[focused_demo_idx].hidden)
                            focused_demo_idx = start;  /* all hidden, stay put */
                        gui_desktop_raise_window(focused_demo_idx);
                        switcher_show_until = idt_get_ticks() + 100; /* 1s at 100Hz */
                        serial_puts("[COMP] Alt+Tab -> window ");
                        serial_putdec((uint64_t)focused_demo_idx);
                        serial_puts("\n");
                    }
                    continue;  /* don't push Tab into key ring while Alt is held */
                }

                /* Push event into key_ring (for games / GUI) */
                uint32_t next = (key_ring_head + 1) & KEY_RING_MASK;
                if (next != key_ring_tail) {
                    key_ring[key_ring_head].scancode = (uint32_t)sc;
                    key_ring[key_ring_head].pressed  = (type == 1);
                    key_ring_head = next;
                }

                /* Terminal keyboard routing:
                 * PS/2 → kb_process_scancode → kb_push handles most cases.
                 * BUT: when kbd_captured is false AND the terminal is focused,
                 * xHCI HID events also need routing to kb_push because xHCI
                 * skips direct kb_push when compositor is running.
                 * Only route if the event came from HID range (sc < 0xE0 = real keys,
                 * 0xE0+ = modifiers already handled above). */
                if (!has_fullscreen && type == 1 && focused_demo_idx == 0) {
                    int term_count2;
                    gui_win_desc_t *dw_kb2 = gui_desktop_get_windows(&term_count2);
                    bool term_vis = (term_count2 > 0 && !dw_kb2[0].hidden);
                    /* Only route HID scancodes (0x04-0x53), not PS/2 (which are
                     * already handled by kb_process_scancode in keyboard.c) */
                    if (term_vis && sc >= 0x04 && sc <= 0x53) {
                        extern void kb_push(char c);
                        extern void kb_push_esc(const char *seq);
                        extern const char hid_normal[];
                        extern const char hid_shifted[];
                        switch (sc) {
                        case 0x28: kb_push('\n');  break; /* Enter */
                        case 0x2A: kb_push('\b');  break; /* Backspace */
                        case 0x2B: kb_push('\t');  break; /* Tab */
                        case 0x2C: kb_push(' ');   break; /* Space */
                        case 0x4F: kb_push_esc("C"); break; /* Right */
                        case 0x50: kb_push_esc("D"); break; /* Left */
                        case 0x51: kb_push_esc("B"); break; /* Down */
                        case 0x52: kb_push_esc("A"); break; /* Up */
                        default:
                            if (sc < 0x54) {
                                char c = comp_shift_held ? hid_shifted[sc] : hid_normal[sc];
                                if (comp_ctrl_held && c >= 'a' && c <= 'z') c = c - 'a' + 1;
                                if (c) kb_push(c);
                            }
                            break;
                        }
                    }
                }
            }
        }

        /* 1b. Process mouse clicks on demo windows */
        process_mouse_input();

        /* 2. Tick animation engine (100 APIC ticks = 1000ms) */
        gui_anim_tick(idt_get_ticks() * 10);  /* convert to ms (100Hz * 10 = ms) */

        /* 3. Render frame */
        compositor_render_frame();

        /* 4. Flip (waits for VBlank) */
        display_flip();

        /* 5. Fullscreen→desktop transition: force a regular-memcpy refresh.
         * memcpy_nt (non-temporal stores) in display_flip may not trigger
         * QEMU/HVF dirty-page tracking on the frame immediately after a
         * fullscreen window is destroyed. A regular memcpy guarantees the
         * write is visible to the host display backend. */
        if (comp_was_fullscreen && !has_fullscreen) {
            serial_puts("[COMP] FS->desktop: force refresh\n");
            extern void display_force_refresh(void);
            display_force_refresh();
            /* Game exited — restore focus to terminal */
            focused_demo_idx = 0;
            gui_desktop_raise_window(0);
        }
        comp_was_fullscreen = has_fullscreen;

        comp_frames++;
    }
}

/* ── Control ─────────────────────────────────────────────────── */

void compositor_stop(void)
{
    compositor_running = false;
}

/* ── WASM: single-frame render (called from JS requestAnimationFrame) ── */

#ifdef __EMSCRIPTEN__
extern void display_flip_nowait(void);

void compositor_start_wasm(void)
{
    compositor_running = true;
    serial_puts("[COMP] WASM compositor started (rAF)\n");
}

EMSCRIPTEN_KEEPALIVE
void wasm_compositor_frame(void)
{
    if (!compositor_running) return;

    /* ── Input processing (mirrors the x86 main loop) ── */
    if (input_has_events()) {
        int16_t mdx, mdy, wheel;
        uint8_t buttons;
        uint8_t key_buf[32 * 24];
        int nkeys = input_drain_coalesced(&mdx, &mdy, &buttons,
                                          &wheel, key_buf, 32);
        comp_button_state = buttons;
        comp_wheel_accum += wheel;

        /* Key events: route to focused window's shell */
        for (int i = 0; i < nkeys; i++) {
            uint8_t *evt = &key_buf[i * 24];
            uint8_t type = evt[0];
            uint8_t sc   = evt[1];
            if (sc == 0xE2 || sc == 0xE6 || sc == 0x38) alt_held        = (type == 1);
            if (sc == 0xE1 || sc == 0xE5)               comp_shift_held = (type == 1);
            if (sc == 0xE0 || sc == 0xE4)               comp_ctrl_held  = (type == 1);
        }
    }
    process_mouse_input();

    /* ── Render ── */
    gui_anim_tick(idt_get_ticks() * 10);
    compositor_render_frame();
    display_mark_dirty();
    display_flip_nowait();
}
#endif

bool compositor_is_running(void)
{
    return compositor_running;
}

/* ── Input Query (for user32_shim) ────────────────────────────── */

/* Pop a key event from the ring buffer.
 * Returns true if an event was available, false if empty. */
bool compositor_get_key_event(uint32_t *scancode, bool *pressed)
{
    if (key_ring_tail == key_ring_head)
        return false;
    *scancode = key_ring[key_ring_tail].scancode;
    *pressed  = key_ring[key_ring_tail].pressed;
    key_ring_tail = (key_ring_tail + 1) & KEY_RING_MASK;
    return true;
}

uint8_t compositor_get_button_state(void) { return comp_button_state; }
int16_t compositor_get_wheel_delta(void)
{
    int16_t w = comp_wheel_accum;
    comp_wheel_accum = 0;
    return w;
}

/* ── Stats ───────────────────────────────────────────────────── */

uint64_t compositor_get_frames(void) { return comp_frames; }
uint64_t compositor_get_direct_scanout(void) { return comp_direct_scanout; }

/* ── Initialize ──────────────────────────────────────────────── */

void compositor_init(void)
{
    memset(windows, 0, sizeof(windows));
    next_window_id = 1;
    focused_window = -1;
    render_count = 0;
    comp_frames = 0;
    comp_direct_scanout = 0;
    compositor_running = false;
    key_ring_head = 0;
    key_ring_tail = 0;
    comp_button_state = 0;
    comp_wheel_accum = 0;
    focused_demo_idx = 0;   /* terminal has focus by default */
    is_maximized[0] = false;
    is_maximized[1] = false;
    alt_held        = false;
    comp_shift_held = false;
    comp_ctrl_held  = false;
    terminal_shm    = 0;
    terminal_pixels = NULL;
    terminal_w      = 0;
    terminal_h      = 0;

    /* Build AA cursor sprite */
    compositor_init_cursor();

    /* Initialize GUI desktop layout */
    uint32_t sw = display_get_width();
    uint32_t sh = display_get_height();
    if (sw && sh)
        gui_desktop_init(sw, sh);

    serial_puts("[COMP] Compositor initialized (max ");
    serial_putdec(MAX_WINDOWS);
    serial_puts(" windows)\n");
}
