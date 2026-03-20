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

/* Scheduler (process.c) */
extern void proc_set_qos(uint8_t qos);
extern uint64_t idt_get_ticks(void);

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

/* Desktop background now rendered by gui_desktop_render() */

/* Cursor appearance (simple 8×8 white arrow) */
static const uint8_t cursor_bitmap[8] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0xA0, 0x10
};
#define CURSOR_W 8
#define CURSOR_H 8
#define CURSOR_COLOR 0xFFFFFFFF

/* Stats */
static uint64_t comp_frames;
static uint64_t comp_direct_scanout;

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

/* ── Draw Cursor ─────────────────────────────────────────────── */

static void draw_cursor(uint32_t *dst, uint32_t pitch,
                        uint32_t scr_w, uint32_t scr_h,
                        int32_t cx, int32_t cy)
{
    for (int y = 0; y < CURSOR_H; y++) {
        int32_t py = cy + y;
        if (py < 0 || py >= (int32_t)scr_h) continue;
        uint8_t bits = cursor_bitmap[y];
        for (int x = 0; x < CURSOR_W; x++) {
            if (!(bits & (0x80 >> x))) continue;
            int32_t px = cx + x;
            if (px < 0 || px >= (int32_t)scr_w) continue;
            dst[(uint32_t)py * pitch + (uint32_t)px] = CURSOR_COLOR;
        }
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

    /* Direct scanout optimization: if exactly 1 fullscreen window,
     * its surface covers the entire screen — no compositing needed. */
    if (render_count == 1) {
        int idx = render_order[0];
        window_t *win = &windows[idx];
        if ((win->flags & WND_FULLSCREEN) &&
            win->width == (uint16_t)w && win->height == (uint16_t)h &&
            win->pixels) {
            /* Copy surface directly to back buffer (no desktop fill) */
            memcpy(back, win->pixels, (uint64_t)w * h * 4);
            /* In GPU Phase A, this would be: SET_OFFSET = shm_phys */
            display_mark_dirty();
            comp_direct_scanout++;
            return;
        }
    }

    /* Render elementaryOS-inspired desktop */
    {
        gui_surface_t screen = { back, w, h, p };
        gui_desktop_render(&screen);
    }

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

    while (compositor_running) {
        /* 1. Process input with coalescing */
        if (input_has_events()) {
            int16_t mdx, mdy, wheel;
            uint8_t buttons;
            /* Drain all events, accumulate mouse deltas */
            /* key_out buffer on stack (max 32 events per frame) */
            uint8_t key_buf[32 * 24];  /* 32 × sizeof(input_event_t) */
            int nkeys = input_drain_coalesced(&mdx, &mdy, &buttons,
                                              &wheel, key_buf, 32);

            /* Update cursor from accumulated mouse deltas */
            if (mdx || mdy)
                input_post_mouse_move(mdx, mdy);

            /* Store button and wheel state for focused window */
            comp_button_state = buttons;
            comp_wheel_accum += wheel;

            /* Push keyboard events into the ring buffer */
            /* Each key_buf entry is an input_event_t (24 bytes):
             *   offset 0: type (uint8_t)
             *   offset 1: scancode (uint8_t) */
            for (int i = 0; i < nkeys; i++) {
                uint8_t *evt = &key_buf[i * 24];
                uint8_t type = evt[0];
                uint8_t sc   = evt[1];
                uint32_t next = (key_ring_head + 1) & KEY_RING_MASK;
                if (next == key_ring_tail)
                    break;  /* ring full, drop remaining */
                key_ring[key_ring_head].scancode = (uint32_t)sc;
                key_ring[key_ring_head].pressed  = (type == 1); /* INPUT_KEY_DOWN */
                key_ring_head = next;
            }
        }

        /* 2. Render frame */
        compositor_render_frame();

        /* 3. Flip (waits for VBlank) */
        display_flip();

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

    /* Initialize GUI desktop layout */
    uint32_t sw = display_get_width();
    uint32_t sh = display_get_height();
    if (sw && sh)
        gui_desktop_init(sw, sh);

    serial_puts("[COMP] Compositor initialized (max ");
    serial_putdec(MAX_WINDOWS);
    serial_puts(" windows)\n");
}
