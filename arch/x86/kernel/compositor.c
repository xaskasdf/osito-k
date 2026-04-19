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

static inline uint8_t cmos_read(uint8_t reg)
{
    __asm__ volatile ("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)0x71));
    return val;
}

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

/* Maximize / restore state (per demo window) */
static bool    is_maximized[2];
static int32_t saved_geom[2][4];    /* x, y, w, h before maximize */

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

/* Process mouse clicks on demo windows */
static void process_mouse_input(void)
{
    uint8_t pressed  = comp_button_state & ~prev_buttons;
    uint8_t released = prev_buttons & ~comp_button_state;

    if (pressed & 1) {  /* left button newly pressed */
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);

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
            if (btn == 1) {
                /* Close: hide by moving offscreen (demo windows can't be destroyed) */
                dw[hit].x = -9999;
                dw[hit].y = -9999;
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
        if (drag_win_idx >= 0 && drag_win_idx < count) {
            dw[drag_win_idx].x = cx - drag_off_x;
            dw[drag_win_idx].y = cy - drag_off_y;
        }
    }

    /* Release */
    if (released & 1) {
        dragging = false;
        drag_win_idx = -1;
    }

    prev_buttons = comp_button_state;
}

/* ── Terminal Surface ────────────────────────────────────────── */

/* Return the pixel dimensions of the Terminal window content area.
 * Called from shell.c to size the shm surface before fb_redirect(). */
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

/* ── Scale-blit SMP worker ──────────────────────────────────── */

void scaleblit_worker(void *arg, void *result)
{
    (void)result;
    struct {
        const uint32_t *src; uint32_t *dst;
        uint32_t dst_pitch, sw, sc, ox, oy, dw;
        uint32_t y_start, y_end;
    } *a = arg;

    for (uint32_t _y = a->y_start; _y < a->y_end; _y++) {
        const uint32_t *_src = a->src + _y * a->sw;
        uint32_t *_row0 = a->dst + (a->oy + _y * a->sc) * a->dst_pitch + a->ox;
        for (uint32_t _x = 0; _x < a->sw; _x++) {
            uint32_t _px = _src[_x];
            uint32_t _base = _x * a->sc;
            for (uint32_t _rx = 0; _rx < a->sc; _rx++)
                _row0[_base + _rx] = _px;
        }
        for (uint32_t _ry = 1; _ry < a->sc; _ry++)
            memcpy(a->dst + (a->oy + _y * a->sc + _ry) * a->dst_pitch + a->ox,
                   _row0, (uint64_t)a->dw * 4);
    }
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
         * Row-major traversal keeps writes sequential for cache efficiency.
         * Parallel path partitions source rows into bands across APs. */
        {
            extern int ap_worker_count;
            extern int smp_submit_any(void (*)(void*, void*), void*, void*);
            extern void smp_wait(int);
            extern void scaleblit_worker(void *, void *);

            int n_ap = ap_worker_count;
            /* Only parallelize if enough rows to amortize IPI overhead */
            if (n_ap > 0 && _sh >= 64) {
                if (n_ap > 3) n_ap = 3;
                int total = n_ap + 1;
                uint32_t band = _sh / total;

                typedef struct {
                    const uint32_t *src; uint32_t *dst;
                    uint32_t dst_pitch, sw, sc, ox, oy, dw;
                    uint32_t y_start, y_end;
                } scaleblit_arg_t;

                scaleblit_arg_t sb_args[3];
                int sb_ap[3];

                for (int i = 0; i < n_ap; i++) {
                    sb_args[i].src = win->pixels;
                    sb_args[i].dst = back;
                    sb_args[i].dst_pitch = p;
                    sb_args[i].sw = _sw;
                    sb_args[i].sc = _sc;
                    sb_args[i].ox = _ox;
                    sb_args[i].oy = _oy;
                    sb_args[i].dw = _dw;
                    sb_args[i].y_start = i * band;
                    sb_args[i].y_end = (i + 1) * band;
                    sb_ap[i] = smp_submit_any(scaleblit_worker, &sb_args[i], NULL);
                }

                /* BSP handles the last band */
                for (uint32_t _y = n_ap * band; _y < _sh; _y++) {
                    const uint32_t *_src2 = win->pixels + _y * _sw;
                    uint32_t *_row0 = back + (_oy + _y * _sc) * p + _ox;
                    for (uint32_t _x = 0; _x < _sw; _x++) {
                        uint32_t _px = _src2[_x];
                        uint32_t _base = _x * _sc;
                        for (uint32_t _rx = 0; _rx < _sc; _rx++)
                            _row0[_base + _rx] = _px;
                    }
                    for (uint32_t _ry = 1; _ry < _sc; _ry++)
                        memcpy(back + (_oy + _y * _sc + _ry) * p + _ox, _row0,
                               (uint64_t)_dw * 4);
                }

                for (int i = 0; i < n_ap; i++)
                    if (sb_ap[i] >= 0) smp_wait(sb_ap[i]);
            } else {
                /* Serial path */
                for (uint32_t _y = 0; _y < _sh; _y++) {
                    const uint32_t *_src2 = win->pixels + _y * _sw;
                    uint32_t *_row0 = back + (_oy + _y * _sc) * p + _ox;
                    for (uint32_t _x = 0; _x < _sw; _x++) {
                        uint32_t _px = _src2[_x];
                        uint32_t _base = _x * _sc;
                        for (uint32_t _rx = 0; _rx < _sc; _rx++)
                            _row0[_base + _rx] = _px;
                    }
                    for (uint32_t _ry = 1; _ry < _sc; _ry++)
                        memcpy(back + (_oy + _y * _sc + _ry) * p + _ox, _row0,
                               (uint64_t)_dw * 4);
                }
            }
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
        /* Read CMOS RTC (BCD mode, 24h), adjust to GMT-3 */
        info.rtc_sec  = bcd2bin(cmos_read(0x00));
        info.rtc_min  = bcd2bin(cmos_read(0x02));
        { uint8_t raw_h = bcd2bin(cmos_read(0x04));
          info.rtc_hour = (raw_h + 24 - 3) % 24; } /* UTC → GMT-3 */
        gui_panel_set_debug(&info);
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

                /* Alt+Tab: cycle through windows (USB Tab = HID 0x2B, PS/2 Tab = 0x0F) */
                if (type == 1 && alt_held && (sc == 0x2B || sc == 0x0F)) {
                    int count;
                    gui_desktop_get_windows(&count);
                    if (count > 1) {
                        focused_demo_idx = (focused_demo_idx + 1) % count;
                        if (focused_demo_idx < 0) focused_demo_idx = 0;
                        gui_desktop_raise_window(focused_demo_idx);
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

                /* Focus-based terminal routing: convert HID key-down events to
                 * ASCII/VT100 and push to kb_buf ONLY when the terminal window
                 * has focus (focused_demo_idx == 0). Other windows don't get
                 * keyboard input — they'd need their own routing. */
                if (!has_fullscreen && type == 1 && focused_demo_idx == 0) {
                    extern void kb_push(char c);
                    extern void kb_push_esc(const char *seq);
                    extern const char hid_normal[];
                    extern const char hid_shifted[];
                    switch (sc) {
                    case 0x4F: kb_push_esc("C");  break; /* Right */
                    case 0x50: kb_push_esc("D");  break; /* Left */
                    case 0x51: kb_push_esc("B");  break; /* Down */
                    case 0x52: kb_push_esc("A");  break; /* Up */
                    case 0x4A: kb_push_esc("H");  break; /* Home */
                    case 0x4D: kb_push_esc("F");  break; /* End */
                    case 0x49: kb_push_esc("2~"); break; /* Insert */
                    case 0x4C: kb_push_esc("3~"); break; /* Delete */
                    case 0x4B: kb_push_esc("5~"); break; /* Page Up */
                    case 0x4E: kb_push_esc("6~"); break; /* Page Down */
                    default:
                        if (sc < 0x54) {
                            char c = comp_shift_held ? hid_shifted[sc] : hid_normal[sc];
                            if (comp_ctrl_held && c >= 'a' && c <= 'z') c = c - 'a' + 1;
                            if (comp_ctrl_held && c >= 'A' && c <= 'Z') c = c - 'A' + 1;
                            if (c) kb_push(c);
                        }
                        break;
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
