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
#include "../include/compositor_surface.h"
#include "../include/input_events.h"
#include "../include/surface_alpha.h"
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
extern uint8_t input_get_buttons(void);
extern void    input_post_mouse_move(int16_t dx, int16_t dy);

/* Shared memory (shm.c) */
extern void   *shm_map(uint32_t handle);
extern void    shm_unmap(uint32_t handle);
extern uint64_t shm_get_phys(uint32_t handle);
extern uint64_t shm_get_size(uint32_t handle);

/* Scheduler / process (process.c) */
extern void proc_set_qos(uint8_t qos);
extern int sched_sleep_ticks(uint64_t ticks);
extern uint64_t idt_get_ticks(void);
extern uint32_t proc_count_active(void);

/* USB HID polling (xhci.c — weak: absent if no xHCI) */
extern void xhci_poll(void) __attribute__((weak));
extern void ps2_poll(void) __attribute__((weak));
extern bool user32_activate_compositor_window(uint32_t window_id)
    __attribute__((weak));
extern void user32_deactivate_compositor_windows(void)
    __attribute__((weak));
extern bool user32_cursor_overlay_visible(uint32_t window_id)
    __attribute__((weak));


/* ── CMOS RTC helpers ──────────────────────────────────────── */

static inline uint8_t cmos_read(uint8_t reg)
{
#if defined(__EMSCRIPTEN__) || defined(COMPOSITOR_HOST_TEST)
    (void)reg;
    return 0;
#else
    __asm__ volatile ("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)0x71));
    return val;
#endif
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

#define MAX_WINDOWS     256
#define MAX_TITLE_LEN   64

/* Window flags */
#define WND_ACTIVE      (1 << 0)
#define WND_VISIBLE     (1 << 1)
#define WND_FULLSCREEN  (1 << 2)
#define WND_MINIMIZED   (1 << 3)
#define WND_FOCUSED     (1 << 4)
#define WND_DIRTY       (1 << 5)  /* surface has new content */
#define WND_TASKBAR     (1 << 6)  /* top-level application window */
#define WND_USER32      (1 << 7)  /* input is delivered through USER32 */

typedef struct {
    uint32_t id;
    int16_t  x, y;                 /* position on screen */
    uint16_t width, height;
    uint16_t surface_pitch;        /* backing surface width in pixels */
    uint16_t surface_height;       /* backing surface allocation height */
    uint32_t shm_handle;           /* shared memory containing pixels */
    uint32_t *pixels;              /* mapped pointer to surface */
    uint32_t owner_pid;
    uint32_t z_order;              /* 0 = bottom, higher = on top */
    int16_t  clip_x, clip_y;
    uint16_t clip_width, clip_height;
    bool     clip_enabled;
    bool     pixel_alpha;
    surface_alpha_t alpha;
    uint8_t  flags;
    char     title[MAX_TITLE_LEN];
} window_t;

typedef struct {
    uint32_t x, y;
    uint32_t width, height;
    bool sampled_scale;
} fullscreen_layout_t;

/* ── Compositor State ────────────────────────────────────────── */

static window_t windows[MAX_WINDOWS];
static volatile uint32_t windows_lock;
static uint32_t next_window_id = 1;
static uint32_t next_z_order = 1;
static int32_t  focused_window = -1;  /* index into windows[] */

/* Sorted window list for rendering (by z_order) */
static int render_order[MAX_WINDOWS];
static int render_count;
/* Only the render thread owns these copies and their extra SHM mappings. */
static window_t render_windows[MAX_WINDOWS];

/* Lock order: window metadata -> SHM metadata (map/size only). Never free,
 * render, wait for an AP, or enter GUI/USER32 callbacks with this lock held. */
static uint64_t compositor_windows_lock(void)
{
    uint64_t flags = 0;
#if !defined(__EMSCRIPTEN__) && !defined(COMPOSITOR_HOST_TEST)
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__sync_lock_test_and_set(&windows_lock, 1)) {
#ifndef __EMSCRIPTEN__
        __asm__ volatile ("pause" ::: "memory");
#endif
    }
    return flags;
}

static void compositor_windows_unlock(uint64_t flags)
{
    __sync_lock_release(&windows_lock);
#if !defined(__EMSCRIPTEN__) && !defined(COMPOSITOR_HOST_TEST)
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
#else
    (void)flags;
#endif
}

/* Metadata-only copy: pixel readers must use a pinned render snapshot. */
static bool compositor_window_info(int index, window_t *info)
{
    uint64_t flags = compositor_windows_lock();
    bool found = index >= 0 && index < MAX_WINDOWS &&
                 (windows[index].flags & WND_ACTIVE);
    if (found) {
        *info = windows[index];
        info->pixels = NULL;
        info->shm_handle = 0;
    }
    compositor_windows_unlock(flags);
    return found;
}

/* Mouse interaction state */
static bool     dragging;
static int32_t  drag_win_idx;       /* index into demo_windows[] */
static int32_t  drag_off_x, drag_off_y;
static bool     managed_pointer_active;
static uint8_t  prev_buttons;
static _Atomic int focused_demo_idx = 0; /* 0=Terminal (focused by default) */

/* Maximize / restore state (per demo window) */
static bool    is_maximized[2];
static int32_t saved_geom[2][4];    /* x, y, w, h before maximize */

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

/* Frame-level dirty tracking: skip render when nothing changed */
static volatile bool comp_frame_dirty = true;
static int32_t  comp_last_cx = -1, comp_last_cy = -1;
static uint64_t comp_dirty_tick;  /* force dirty once/sec for RTC clock */

/* USER32 also calls this when cursor visibility changes without pointer motion. */
void compositor_request_frame(void)
{
    __atomic_store_n(&comp_frame_dirty, true, __ATOMIC_RELEASE);
}

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

static int compositor_topmost_visible_window(void)
{
    int best = -1;
    uint32_t best_z = 0;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if ((w->flags & (WND_ACTIVE | WND_VISIBLE)) !=
                (WND_ACTIVE | WND_VISIBLE) ||
            (w->flags & WND_MINIMIZED))
            continue;
        if (best < 0 || w->z_order >= best_z) {
            best = i;
            best_z = w->z_order;
        }
    }
    return best;
}

/* Caller holds windows_lock. GUI focus is published after unlocking. */
static bool compositor_select_focus_locked(int selected)
{
    bool changed = focused_window != selected;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        uint8_t old_flags = windows[i].flags;
        windows[i].flags &= (uint8_t)~WND_FOCUSED;
        if (i == selected && (windows[i].flags & WND_ACTIVE))
            windows[i].flags |= WND_FOCUSED;
        if (old_flags != windows[i].flags)
            changed = true;
    }

    if (selected < 0 || selected >= MAX_WINDOWS ||
        !(windows[selected].flags & WND_ACTIVE))
        selected = -1;
    focused_window = selected;
    if (selected >= 0) focused_demo_idx = -1;
    return changed;
}

static void compositor_finish_focus(int selected, bool changed)
{
    if (selected >= 0) gui_desktop_set_focus(-1);
    if (changed)
        compositor_request_frame();
}

static void compositor_select_focus(int selected)
{
    uint64_t flags = compositor_windows_lock();
    bool changed = compositor_select_focus_locked(selected);
    selected = focused_window;
    compositor_windows_unlock(flags);
    compositor_finish_focus(selected, changed);
}

/* Register a new window. Returns window ID or 0 on failure. */
static uint32_t compositor_create_window_internal(uint32_t shm_handle,
                                                  int16_t x, int16_t y,
                                                  uint16_t width,
                                                  uint16_t height,
                                                  uint32_t pid,
                                                  const char *title,
                                                  bool activate)
{
    uint32_t *pixels = (uint32_t *)shm_map(shm_handle);
    if (!pixels) return 0;
    if ((uint64_t)width * height * 4 > shm_get_size(shm_handle)) {
        shm_unmap(shm_handle);
        return 0;
    }
    char copied_title[MAX_TITLE_LEN];
    int j = 0;
    if (title)
        while (title[j] && j < MAX_TITLE_LEN - 1) {
            copied_title[j] = title[j];
            j++;
        }
    copied_title[j] = '\0';
    uint64_t flags = compositor_windows_lock();
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
    if (!w) {
        compositor_windows_unlock(flags);
        shm_unmap(shm_handle);
        return 0;
    }

    compositor_request_frame();
    w->id = next_window_id++;
    w->x = x;
    w->y = y;
    w->width = width;
    w->height = height;
    w->surface_pitch = width;
    w->surface_height = height;
    w->shm_handle = shm_handle;
    w->pixels = pixels;
    w->owner_pid = pid;
    w->z_order = next_z_order++;
    w->clip_x = w->clip_y = 0;
    w->clip_width = w->clip_height = 0;
    w->clip_enabled = false;
    w->pixel_alpha = false;
    w->alpha = (surface_alpha_t){.opacity = 255};
    w->flags = WND_ACTIVE | WND_VISIBLE | WND_DIRTY;

    memcpy(w->title, copied_title, (size_t)j + 1);
    uint32_t id = w->id;
    bool changed = activate && compositor_select_focus_locked(slot);
    compositor_windows_unlock(flags);
    if (activate) compositor_finish_focus(slot, changed);

    serial_puts("[COMP] Window created: id=");
    serial_putdec(id);
    serial_puts(" '");
    serial_puts(copied_title);
    serial_puts("' ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(" @ ");
    serial_putdec((uint64_t)(uint16_t)x);
    serial_puts(",");
    serial_putdec((uint64_t)(uint16_t)y);
    serial_puts("\n");

    return id;
}

uint32_t compositor_create_window(uint32_t shm_handle,
                                  int16_t x, int16_t y,
                                  uint16_t width, uint16_t height,
                                  uint32_t pid, const char *title)
{
    return compositor_create_window_internal(shm_handle, x, y, width, height,
                                             pid, title, true);
}

/* USER32 creates hidden controls and child surfaces before deciding which
 * top-level window owns the foreground. Do not let allocation itself steal
 * compositor focus; USER32 will activate the correct root explicitly. */
uint32_t compositor_create_window_inactive(uint32_t shm_handle,
                                           int16_t x, int16_t y,
                                           uint16_t width, uint16_t height,
                                           uint32_t pid, const char *title)
{
    return compositor_create_window_internal(shm_handle, x, y, width, height,
                                             pid, title, false);
}

/* Destroy a window */
void compositor_destroy_window(uint32_t window_id)
{
    uint32_t handle = 0;
    bool found = false, was_focused = false, changed = false;
    int fallback = -1;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            found = true;
            was_focused = focused_window == i || (windows[i].flags & WND_FOCUSED);
            compositor_request_frame();
            handle = windows[i].shm_handle;
            windows[i].shm_handle = 0;
            windows[i].pixels = NULL;
            windows[i].flags = 0;
            if (was_focused) {
                fallback = compositor_topmost_visible_window();
                changed = compositor_select_focus_locked(fallback);
                if (fallback < 0) focused_demo_idx = 0;
            }
            break;
        }
    }
    compositor_windows_unlock(flags);
    if (handle) shm_unmap(handle);
    if (was_focused) {
        compositor_finish_focus(fallback, changed);
        if (fallback < 0) gui_desktop_show_window(0);
    }
    if (found) {
        serial_puts("[COMP] Window destroyed: id=");
        serial_putdec(window_id);
        serial_puts("\n");
    }
}

void compositor_set_visible(uint32_t window_id, bool visible)
{
    bool lost_focus = false, changed = false;
    int fallback = -1;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;
        uint8_t old_flags = windows[i].flags;
        if (visible)
            windows[i].flags |= WND_VISIBLE;
        else
            windows[i].flags &= (uint8_t)~WND_VISIBLE;
        if (!visible && focused_window == i) {
            lost_focus = true;
            fallback = compositor_topmost_visible_window();
            changed = compositor_select_focus_locked(fallback);
            if (fallback < 0) focused_demo_idx = 0;
        }
        if (windows[i].flags != old_flags)
            compositor_request_frame();
        break;
    }
    compositor_windows_unlock(flags);
    if (lost_focus) {
        compositor_finish_focus(fallback, changed);
        if (fallback < 0) gui_desktop_show_window(0);
    }
}

void compositor_set_title(uint32_t window_id, const char *title)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;

        char updated[MAX_TITLE_LEN];
        int j = 0;
        if (title) {
            while (title[j] && j < MAX_TITLE_LEN - 1) {
                updated[j] = title[j];
                j++;
            }
        }
        updated[j] = '\0';

        bool changed = false;
        for (int k = 0; k < MAX_TITLE_LEN; k++) {
            if (windows[i].title[k] != updated[k]) changed = true;
            windows[i].title[k] = updated[k];
            if (!updated[k]) break;
        }
        if (changed) compositor_request_frame();
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_set_taskbar(uint32_t window_id, bool taskbar)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;
        uint8_t old_flags = windows[i].flags;
        if (taskbar)
            windows[i].flags |= WND_TASKBAR;
        else
            windows[i].flags &= (uint8_t)~WND_TASKBAR;
        if (old_flags != windows[i].flags)
            compositor_request_frame();
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_focus_window(uint32_t window_id)
{
    uint64_t flags = compositor_windows_lock();
    int selected = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) &&
            windows[i].id == window_id) {
            selected = i;
            break;
        }
    }
    bool changed = selected >= 0 && compositor_select_focus_locked(selected);
    compositor_windows_unlock(flags);
    if (selected >= 0) compositor_finish_focus(selected, changed);
}

void compositor_set_position(uint32_t window_id, int16_t x, int16_t y)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;
        if (windows[i].x != x || windows[i].y != y) {
            windows[i].x = x;
            windows[i].y = y;
            compositor_request_frame();
        }
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_set_z_order(uint32_t window_id, uint32_t z_order)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;
        if (windows[i].z_order != z_order) {
            windows[i].z_order = z_order;
            compositor_request_frame();
        }
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_set_clip_rect(uint32_t window_id, bool enabled,
                              int16_t x, int16_t y,
                              uint16_t width, uint16_t height)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;
        window_t *w = &windows[i];
        if (w->clip_enabled != enabled || w->clip_x != x || w->clip_y != y ||
            w->clip_width != width || w->clip_height != height) {
            w->clip_enabled = enabled;
            w->clip_x = x;
            w->clip_y = y;
            w->clip_width = width;
            w->clip_height = height;
            compositor_request_frame();
        }
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_set_pixel_alpha(uint32_t window_id, bool enabled)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!(w->flags & WND_ACTIVE) || w->id != window_id) continue;
        if (w->pixel_alpha != enabled) {
            w->pixel_alpha = enabled;
            compositor_request_frame();
        }
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_set_surface_alpha(uint32_t window_id,
                                   const surface_alpha_t *alpha)
{
    if (!alpha) return;
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!(w->flags & WND_ACTIVE) || w->id != window_id) continue;
        if (memcmp(&w->alpha, alpha, sizeof(*alpha))) {
            w->alpha = *alpha;
            compositor_request_frame();
        }
        break;
    }
    compositor_windows_unlock(lock_flags);
}

void compositor_set_size(uint32_t window_id, uint16_t width, uint16_t height)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;

        if (width > windows[i].surface_pitch)
            width = windows[i].surface_pitch;
        if (height > windows[i].surface_height)
            height = windows[i].surface_height;
        if (windows[i].width != width || windows[i].height != height) {
            windows[i].width = width;
            windows[i].height = height;
            windows[i].flags |= WND_DIRTY;
            compositor_request_frame();
        }
        break;
    }
    compositor_windows_unlock(lock_flags);
}

bool compositor_replace_surface(uint32_t window_id, uint32_t shm_handle,
                                uint16_t width, uint16_t height)
{
    uint32_t *pixels = (uint32_t *)shm_map(shm_handle);
    if (!pixels)
        return false;
    if ((uint64_t)width * height * 4 > shm_get_size(shm_handle)) {
        shm_unmap(shm_handle);
        return false;
    }

    uint32_t old_handle = 0;
    bool replaced = false;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WND_ACTIVE) || windows[i].id != window_id)
            continue;

        old_handle = windows[i].shm_handle;
        windows[i].shm_handle = shm_handle;
        windows[i].pixels = pixels;
        windows[i].width = width;
        windows[i].height = height;
        windows[i].surface_pitch = width;
        windows[i].surface_height = height;
        windows[i].flags |= WND_DIRTY;
        compositor_request_frame();
        replaced = true;
        break;
    }
    compositor_windows_unlock(flags);
    if (old_handle) shm_unmap(old_handle);
    if (!replaced) shm_unmap(shm_handle);
    return replaced;
}

bool compositor_acquire_surface(uint32_t window_id, compositor_surface_ref_t *surface)
{
    if (!surface) return false;
    *surface = (compositor_surface_ref_t){0};
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        const window_t *w = &windows[i];
        if (!(w->flags & WND_ACTIVE) || w->id != window_id || !w->pixels)
            continue;
        uint32_t *pixels = shm_map(w->shm_handle);
        if (pixels) {
            surface->handle = w->shm_handle;
            surface->pixels = pixels;
            surface->width = surface->pitch = w->surface_pitch;
            surface->height = w->surface_height;
        }
        break;
    }
    compositor_windows_unlock(flags);
    return surface->handle != 0;
}

void compositor_release_surface(compositor_surface_ref_t *surface)
{
    if (!surface) return;
    uint32_t handle = surface->handle;
    *surface = (compositor_surface_ref_t){0};
    if (handle) shm_unmap(handle);
}

/* Destroy all windows owned by a process (called on proc_free) */
void compositor_cleanup_process(uint32_t pid)
{
    uint32_t ids[MAX_WINDOWS];
    int count = 0;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].owner_pid == pid)
            ids[count++] = windows[i].id;
    }
    compositor_windows_unlock(flags);
    for (int i = 0; i < count; i++) compositor_destroy_window(ids[i]);
}

static void compositor_raise_window_to_front(int idx)
{
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    if (!(windows[idx].flags & WND_ACTIVE)) return;

    uint8_t old_z = windows[idx].z_order;
    uint8_t max_z = old_z;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].z_order > max_z)
            max_z = windows[i].z_order;
    }
    if (old_z >= max_z) return;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (i != idx && (windows[i].flags & WND_ACTIVE) &&
            windows[i].z_order > old_z) {
            windows[i].z_order--;
        }
    }
    windows[idx].z_order = max_z;
}

/* Signal that a window's surface has new content */
void compositor_signal_dirty(uint32_t window_id)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            windows[i].flags |= WND_DIRTY;
            if (windows[i].flags & WND_FULLSCREEN)
                compositor_raise_window_to_front(i);
            break;
        }
    }
    compositor_request_frame();
    compositor_windows_unlock(lock_flags);
}

/* Reverse lookup: find window_id of the window whose backing SHM surface
 * equals `shm_handle`. Returns 0 if no active window owns that handle. */
uint32_t compositor_find_window_by_shm(uint32_t shm_handle)
{
    if (!shm_handle) return 0;
    uint32_t id = 0;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) &&
            windows[i].shm_handle == shm_handle) {
            id = windows[i].id;
            break;
        }
    }
    compositor_windows_unlock(flags);
    return id;
}

/* Set fullscreen mode */
void compositor_set_fullscreen(uint32_t window_id, bool fullscreen)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            bool was_fullscreen = (windows[i].flags & WND_FULLSCREEN) != 0;
            if (fullscreen) {
                windows[i].flags |= WND_FULLSCREEN;
                windows[i].x = 0;
                windows[i].y = 0;
                if (!was_fullscreen)
                    compositor_raise_window_to_front(i);
            } else {
                windows[i].flags &= ~WND_FULLSCREEN;
            }
            if (was_fullscreen != fullscreen)
                compositor_request_frame();
            break;
        }
    }
    compositor_windows_unlock(lock_flags);
}

static bool compositor_fullscreen_layout(const window_t *window,
                                         uint32_t output_width,
                                         uint32_t output_height,
                                         fullscreen_layout_t *layout)
{
    if (!window || !layout || !window->width || !window->height ||
        !output_width || !output_height)
        return false;

    uint32_t source_width = window->width;
    uint32_t source_height = window->height;
    uint32_t width;
    uint32_t height;

    if (source_width == output_width && source_height == output_height) {
        width = output_width;
        height = output_height;
        layout->sampled_scale = false;
    } else if ((window->flags & WND_USER32) ||
               source_width > output_width ||
               source_height > output_height) {
        if ((uint64_t)source_width * output_height >=
            (uint64_t)source_height * output_width) {
            width = output_width;
            height = (uint32_t)(((uint64_t)source_height * output_width) /
                                source_width);
        } else {
            height = output_height;
            width = (uint32_t)(((uint64_t)source_width * output_height) /
                               source_height);
        }
        if (!width) width = 1;
        if (!height) height = 1;
        layout->sampled_scale = true;
    } else {
        uint32_t scale_x = output_width / source_width;
        uint32_t scale_y = output_height / source_height;
        uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
        width = source_width * scale;
        height = source_height * scale;
        layout->sampled_scale = false;
    }

    layout->x = (output_width - width) / 2;
    layout->y = (output_height - height) / 2;
    layout->width = width;
    layout->height = height;
    return true;
}

bool compositor_get_fullscreen_content_rect(uint32_t window_id,
                                            int32_t *x, int32_t *y,
                                            uint32_t *width,
                                            uint32_t *height)
{
    uint32_t output_width = display_get_width();
    uint32_t output_height = display_get_height();
    window_t snapshot;
    bool found = false;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *window = &windows[i];
        uint8_t required = WND_ACTIVE | WND_VISIBLE | WND_FULLSCREEN;
        if (window->id != window_id ||
            (window->flags & required) != required ||
            (window->flags & WND_MINIMIZED))
            continue;

        snapshot = *window;
        found = true;
        break;
    }
    compositor_windows_unlock(flags);
    fullscreen_layout_t layout;
    if (!found || !compositor_fullscreen_layout(&snapshot, output_width,
                                                output_height, &layout))
        return false;
    if (x) *x = (int32_t)layout.x;
    if (y) *y = (int32_t)layout.y;
    if (width) *width = layout.width;
    if (height) *height = layout.height;
    return true;
}

void compositor_set_user32_managed(uint32_t window_id, bool managed)
{
    uint64_t lock_flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & WND_ACTIVE) && windows[i].id == window_id) {
            if (managed)
                windows[i].flags |= WND_USER32;
            else
                windows[i].flags &= (uint8_t)~WND_USER32;
            break;
        }
    }
    compositor_windows_unlock(lock_flags);
}

/* ── Build Render Order ──────────────────────────────────────── */

static void release_render_surfaces(void)
{
    for (int i = 0; i < render_count; i++) {
        window_t *snapshot = &render_windows[render_order[i]];
        if (snapshot->shm_handle) shm_unmap(snapshot->shm_handle);
        snapshot->shm_handle = 0;
        snapshot->pixels = NULL;
    }
    render_count = 0;
}

static void build_render_order(void)
{
    release_render_surfaces();
    uint64_t flags = compositor_windows_lock();
    render_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if ((windows[i].flags & (WND_ACTIVE | WND_VISIBLE)) ==
            (WND_ACTIVE | WND_VISIBLE) &&
            !(windows[i].flags & WND_MINIMIZED) && windows[i].pixels) {
            uint32_t *pixels = (uint32_t *)shm_map(windows[i].shm_handle);
            if (!pixels) continue;
            render_windows[i] = windows[i];
            render_windows[i].pixels = pixels;
            render_order[render_count++] = i;
        }
    }
    compositor_windows_unlock(flags);

    /* Simple insertion sort by z_order (stable, small N) */
    for (int i = 1; i < render_count; i++) {
        int key = render_order[i];
        int j = i - 1;
        while (j >= 0 && render_windows[render_order[j]].z_order > render_windows[key].z_order) {
            render_order[j + 1] = render_order[j];
            j--;
        }
        render_order[j + 1] = key;
    }
}

/* ── Blit Window to Back Buffer ──────────────────────────────── */

/* Band-parallel blit worker — same pattern as scaleblit_worker */
typedef struct {
    uint32_t       *dst;
    uint32_t        dst_pitch;
    const uint32_t *src;
    uint32_t        src_width;
    int32_t         sx, sy, dx, dy;
    int32_t         bw;
    int32_t         y_start, y_end;
} blit_band_arg_t;

static void blit_band_worker(void *arg, void *result)
{
    (void)result;
    blit_band_arg_t *a = (blit_band_arg_t *)arg;
    for (int32_t y = a->y_start; y < a->y_end; y++) {
        uint32_t *d = a->dst + (uint32_t)(a->dy + y) * a->dst_pitch + (uint32_t)a->dx;
        const uint32_t *s = a->src + (uint32_t)(a->sy + y) * a->src_width + (uint32_t)a->sx;
        memcpy(d, s, (uint64_t)a->bw * sizeof(uint32_t));
    }
}

static inline uint32_t compositor_blend_pixel(uint32_t dst, uint32_t src);

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
    if (w->clip_enabled) {
        int32_t clip_right = (int32_t)w->clip_x + w->clip_width;
        int32_t clip_bottom = (int32_t)w->clip_y + w->clip_height;
        if (dx < w->clip_x) {
            int32_t delta = w->clip_x - dx;
            sx += delta;
            bw -= delta;
            dx = w->clip_x;
        }
        if (dy < w->clip_y) {
            int32_t delta = w->clip_y - dy;
            sy += delta;
            bh -= delta;
            dy = w->clip_y;
        }
        if (dx + bw > clip_right) bw = clip_right - dx;
        if (dy + bh > clip_bottom) bh = clip_bottom - dy;
    }
    if (bw <= 0 || bh <= 0) return;

    bool layered_alpha = w->alpha.per_pixel || w->alpha.color_key_enabled ||
                         w->alpha.opacity != 255;
    if (w->pixel_alpha || layered_alpha) {
        for (int32_t y = 0; y < bh; y++) {
            uint32_t *d = dst + (uint32_t)(dy + y) * dst_pitch + (uint32_t)dx;
            const uint32_t *s = w->pixels + (uint32_t)(sy + y) * w->surface_pitch + (uint32_t)sx;
            for (int32_t x = 0; x < bw; x++)
                d[x] = layered_alpha ? surface_alpha_blend(d[x], s[x], &w->alpha)
                                     : compositor_blend_pixel(d[x], s[x]);
        }
        return;
    }

    extern int ap_worker_count;

    if (bh >= 256 && ap_worker_count > 0) {
        /* Parallel blit: split scanlines into bands across APs + BSP */
        extern int smp_submit_any(void (*)(void*, void*), void*, void*);
        extern void smp_wait(int);

        int n_ap = ap_worker_count;
        if (n_ap > 2) n_ap = 2;  /* diminishing returns for memcpy */
        int32_t band = bh / (n_ap + 1);
        blit_band_arg_t bb_args[2];
        int bb_ids[2];

        for (int i = 0; i < n_ap; i++) {
            bb_args[i] = (blit_band_arg_t){
                dst, dst_pitch, w->pixels, w->surface_pitch,
                sx, sy, dx, dy, bw, i * band, (i + 1) * band
            };
            bb_ids[i] = smp_submit_any(blit_band_worker, &bb_args[i], NULL);
            if (bb_ids[i] < 0)
                blit_band_worker(&bb_args[i], NULL);
        }
        /* BSP handles remainder */
        for (int32_t y = n_ap * band; y < bh; y++) {
            uint32_t *d = dst + (uint32_t)(dy + y) * dst_pitch + (uint32_t)dx;
            const uint32_t *s = w->pixels + (uint32_t)(sy + y) * w->surface_pitch + (uint32_t)sx;
            memcpy(d, s, (uint64_t)bw * sizeof(uint32_t));
        }
        for (int i = 0; i < n_ap; i++)
            if (bb_ids[i] >= 0) smp_wait(bb_ids[i]);
    } else {
        /* Serial blit (small windows or no APs) */
        for (int32_t y = 0; y < bh; y++) {
            uint32_t *d = dst + ((uint32_t)(dy + y)) * dst_pitch + (uint32_t)dx;
            const uint32_t *s = w->pixels + ((uint32_t)(sy + y)) * w->surface_pitch + (uint32_t)sx;
            memcpy(d, s, (uint64_t)bw * sizeof(uint32_t));
        }
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

static inline uint32_t compositor_blend_pixel(uint32_t dst, uint32_t src)
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
            *p = compositor_blend_pixel(*p, shadow);
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
            *p = compositor_blend_pixel(*p, src);
        }
    }
}

static inline void draw_cursor_overlay(uint32_t *dst, uint32_t pitch,
                                       uint32_t scr_w, uint32_t scr_h,
                                       const window_t *owner)
{
#ifndef __EMSCRIPTEN__
    if (owner && (owner->flags & WND_USER32) &&
        user32_cursor_overlay_visible &&
        !user32_cursor_overlay_visible(owner->id))
        return;
    int32_t cx, cy;
    input_get_cursor(&cx, &cy);
    draw_cursor(dst, pitch, scr_w, scr_h, cx, cy);
#else
    (void)dst;
    (void)pitch;
    (void)scr_w;
    (void)scr_h;
    (void)owner;
#endif
}

/* ── Hit-testing for demo windows ─────────────────────────────── */

/* Check if point (mx,my) is inside a demo window (including titlebar).
 * Returns window index into demo_windows[], checking top-to-bottom in render order. */
static int hit_test_demo_window(int32_t mx, int32_t my)
{
    int count = 0;
    gui_win_desc_t *dw = gui_desktop_get_windows(&count);
    int *order = gui_desktop_get_order();
    for (int i = count - 1; i >= 0; i--) {
        int idx = order[i];
        if (!dw[idx].hidden && gui_rect_contains(gui_window_frame(&dw[idx]), mx, my))
            return idx;
    }
    return -1;
}

/* Pointer ownership follows the same foreground layer as painting.
 * Keep the winning surface latched until button-up: a captured drag may leave
 * the original rectangle, but must never fall through to a demo window. */
static int hit_test_managed_window(int32_t mx, int32_t my)
{
    int native = gui_desktop_get_focus();
    if (native >= 0 && hit_test_demo_window(mx, my) == native)
        return -1;
    int best = -1;
    uint32_t best_z = 0;
    uint64_t flags = compositor_windows_lock();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if ((w->flags & (WND_ACTIVE | WND_VISIBLE)) !=
                (WND_ACTIVE | WND_VISIBLE) ||
            (w->flags & WND_MINIMIZED) || !w->pixels || !w->width || !w->height)
            continue;

        int32_t left = w->x;
        int32_t top = w->y;
        int32_t right = left + (int32_t)w->width;
        int32_t bottom = top + (int32_t)w->height;
        if (w->clip_enabled) {
            int32_t clip_right = w->clip_x + (int32_t)w->clip_width;
            int32_t clip_bottom = w->clip_y + (int32_t)w->clip_height;
            if (w->clip_x > left) left = w->clip_x;
            if (w->clip_y > top) top = w->clip_y;
            if (clip_right < right) right = clip_right;
            if (clip_bottom < bottom) bottom = clip_bottom;
        }

        if (mx >= left && mx < right && my >= top && my < bottom &&
            (best < 0 || w->z_order >= best_z)) {
            if ((uint32_t)(mx - w->x) >= w->surface_pitch ||
                (uint32_t)(my - w->y) >= w->surface_height ||
                w->alpha.input_transparent ||
                !surface_alpha_coverage(w->pixels[(my - w->y) *
                                          w->surface_pitch + mx - w->x], &w->alpha))
                continue;
            best = i;
            best_z = w->z_order;
        }
    }
    compositor_windows_unlock(flags);
    return best;
}

/* Check if point hits a traffic-light button. Returns 1=close, 2=min, 3=max, 0=none */
static int hit_test_buttons(gui_win_desc_t *w, int32_t mx, int32_t my)
{
    return gui_window_button_hit(w, mx, my);
}

static char taskbar_label(const char *title)
{
    if (title) {
        for (int i = 0; title[i]; i++) {
            char c = title[i];
            if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        }
    }
    return 'W';
}

static int taskbar_representative(uint32_t owner_pid)
{
    if (focused_window >= 0 && focused_window < MAX_WINDOWS &&
        windows[focused_window].owner_pid == owner_pid &&
        (windows[focused_window].flags & WND_TASKBAR))
        return focused_window;

    int best = -1;
    uint32_t best_z = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if ((w->flags & (WND_ACTIVE | WND_TASKBAR)) !=
            (WND_ACTIVE | WND_TASKBAR) || w->owner_pid != owner_pid)
            continue;
        if (best < 0 || w->z_order >= best_z) {
            best = i;
            best_z = w->z_order;
        }
    }
    return best;
}

static void refresh_dock_tasks(void)
{
    static const uint32_t colors[] = {
        0xFF2F80ED, 0xFF27AE60, 0xFFF2994A, 0xFF9B51E0,
        0xFFEB5757, 0xFF00A6A6, 0xFF6D5BD0, 0xFF4F7C5B,
    };
    gui_dock_task_t tasks[GUI_DOCK_MAX_TASKS];
    int count = 0;
    uint32_t previous_pid = 0;
    uint64_t flags = compositor_windows_lock();
    while (count < GUI_DOCK_MAX_TASKS) {
        uint32_t best_pid = ~0u;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            if ((w->flags & (WND_ACTIVE | WND_TASKBAR)) !=
                (WND_ACTIVE | WND_TASKBAR))
                continue;
            if (w->owner_pid > previous_pid && w->owner_pid < best_pid)
                best_pid = w->owner_pid;
        }
        if (best_pid == ~0u) break;

        int best = taskbar_representative(best_pid);
        if (best < 0) break;
        window_t *w = &windows[best];
        bool minimized = true;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if ((windows[i].flags & (WND_ACTIVE | WND_TASKBAR | WND_VISIBLE)) ==
                    (WND_ACTIVE | WND_TASKBAR | WND_VISIBLE) &&
                windows[i].owner_pid == best_pid) {
                minimized = false;
                break;
            }
        }
        tasks[count].id = best_pid;
        tasks[count].color = colors[best_pid % (sizeof(colors) / sizeof(colors[0]))];
        tasks[count].label = taskbar_label(w->title);
        tasks[count].focused = focused_window >= 0 &&
                               windows[focused_window].owner_pid == best_pid;
        tasks[count].minimized = minimized;
        previous_pid = best_pid;
        count++;
    }
    compositor_windows_unlock(flags);
    gui_dock_set_tasks(tasks, count);
}

static int managed_window_by_pid(uint32_t owner_pid)
{
    uint64_t flags = compositor_windows_lock();
    int index = taskbar_representative(owner_pid);
    compositor_windows_unlock(flags);
    return index;
}

static void focus_demo_window(int index)
{
    compositor_select_focus(-1);
    focused_demo_idx = index;
    if (user32_deactivate_compositor_windows)
        user32_deactivate_compositor_windows();
    gui_desktop_show_window(index);
    compositor_request_frame();
}

static void focus_managed_window(int index)
{
    window_t info;
    if (!compositor_window_info(index, &info)) return;
    uint32_t id = info.id;
    if (user32_activate_compositor_window &&
        user32_activate_compositor_window(id))
        return;
    compositor_focus_window(id);
}

static void cycle_task_switcher(void)
{
    int demo_count = 0;
    gui_desktop_get_windows(&demo_count);
    refresh_dock_tasks();
    int task_count = gui_dock_item_count() - GUI_DOCK_BUILTIN_COUNT;
    int total = demo_count + task_count;
    if (total <= 1) return;

    uint64_t flags = compositor_windows_lock();
    uint32_t current_pid = focused_window >= 0 && focused_window < MAX_WINDOWS
                           ? windows[focused_window].owner_pid : 0;
    compositor_windows_unlock(flags);
    int current = -1;
    if (focused_demo_idx >= 0 && focused_demo_idx < demo_count) {
        current = focused_demo_idx;
    } else if (current_pid) {
        for (int i = 0; i < task_count; i++) {
            if (gui_dock_task_id(GUI_DOCK_BUILTIN_COUNT + i) == current_pid) {
                current = demo_count + i;
                break;
            }
        }
    }

    int next = (current + 1) % total;
    if (next < demo_count) {
        focus_demo_window(next);
        serial_puts("[COMP] Alt+Tab -> desktop window ");
        serial_putdec((uint64_t)next);
        serial_puts("\n");
        return;
    }

    uint32_t pid = gui_dock_task_id(GUI_DOCK_BUILTIN_COUNT + next - demo_count);
    int index = managed_window_by_pid(pid);
    if (index >= 0) {
        focus_managed_window(index);
        serial_puts("[COMP] Alt+Tab -> app window ");
        serial_putdec(pid);
        serial_puts("\n");
    }
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
    return gui_dock_hit(display_get_width(), display_get_height(), mx, my);
}

/* HID also feeds USER32 before this frame's compositor input pass. Prevent a
 * click on native chrome from activating a covered Win32 window underneath.
 * USER32 retains its own explicit capture before consulting this predicate. */
bool compositor_pointer_blocked_by_desktop(int32_t x, int32_t y)
{
    extern volatile bool compositor_running;
    if (!compositor_running) return false;
    if (dragging || gui_desktop_pointer_captured()) return true;
    uint64_t flags = compositor_windows_lock();
    bool fullscreen = focused_window >= 0 && focused_window < MAX_WINDOWS &&
                      (windows[focused_window].flags & WND_FULLSCREEN);
    compositor_windows_unlock(flags);
    if (gui_desktop_get_focus() < 0 && fullscreen) return false;
    if (hit_test_dock(x, y) != -1 ||
        (gui_theme_get()->layout == GUI_LAYOUT_DOCK && y >= 0 && y < GUI_PANEL_HEIGHT))
        return true;
    return hit_test_managed_window(x, y) < 0 && hit_test_demo_window(x, y) >= 0;
}

static void compositor_route_wheel(int16_t wheel)
{
    if (!wheel) return;
    if (gui_desktop_pointer_captured()) return;
    int32_t x, y;
    input_get_cursor(&x, &y);
    if (focused_demo_idx == 1 && gui_desktop_get_focus() == 1 &&
        !(gui_theme_get()->layout == GUI_LAYOUT_DOCK && y < GUI_PANEL_HEIGHT) &&
        hit_test_dock(x, y) == -1 && hit_test_managed_window(x, y) < 0 &&
        hit_test_demo_window(x, y) == 1 && gui_desktop_scroll(1, x, y, wheel)) {
        compositor_request_frame();
        return;
    }
    comp_wheel_accum += wheel;
}

/* Process mouse clicks on demo windows */
static void process_mouse_input(void)
{
    gui_desktop_resize(display_get_width(), display_get_height());
    static unsigned geometry_scale, geometry_layout, geometry_width, geometry_height;
    const gui_metrics_t *metrics = gui_theme_metrics();
    if (geometry_scale != metrics->scale_percent || geometry_layout != (unsigned)gui_theme_get()->layout ||
        geometry_width != metrics->width || geometry_height != metrics->height) {
        for (unsigned i = 0; i < sizeof(is_maximized) / sizeof(is_maximized[0]); i++)
            is_maximized[i] = false;
        geometry_scale = metrics->scale_percent;
        geometry_layout = (unsigned)gui_theme_get()->layout;
        geometry_width = metrics->width; geometry_height = metrics->height;
        dragging = false;
    }
    int32_t pointer_x, pointer_y;
    input_get_cursor(&pointer_x, &pointer_y);
    gui_desktop_set_cursor(pointer_x, pointer_y);
    uint8_t pressed  = comp_button_state & ~prev_buttons;
    uint8_t released = prev_buttons & ~comp_button_state;
    if (pressed || released)
        __atomic_store_n(&comp_frame_dirty, true, __ATOMIC_RELEASE);

    if (gui_desktop_pointer_update(pointer_x, pointer_y, (comp_button_state & 1) != 0)) {
        if (pointer_x != comp_last_cx || pointer_y != comp_last_cy ||
            !gui_desktop_pointer_captured())
            compositor_request_frame();
        prev_buttons = comp_button_state;
        return;
    }

    if (pressed & 1) {
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);
        int managed_hit = hit_test_dock(cx, cy) == -1
                        ? hit_test_managed_window(cx, cy) : -1;
        if (managed_hit >= 0) {
            (void)gui_desktop_click(-1, cx, cy);
            dragging = false;
            drag_win_idx = -1;
            managed_pointer_active = true;
            focus_managed_window(managed_hit);

            static unsigned managed_trace_count;
            if (managed_trace_count++ < 32) {
                serial_puts("[COMP] managed pointer id=");
                window_t info;
                serial_putdec(compositor_window_info(managed_hit, &info) ? info.id : 0);
                serial_puts(" cx="); serial_putdec((uint64_t)cx);
                serial_puts(" cy="); serial_putdec((uint64_t)cy);
                serial_puts("\n");
            }
        }
    }

    if (managed_pointer_active) {
        if (released & 1)
            managed_pointer_active = false;
        prev_buttons = comp_button_state;
        return;
    }

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
        if (dock_hit != -1) {
            if (dock_hit == -3 || dock_hit == -4)
                gui_dock_page(dock_hit == -3 ? -1 : 1);
            if (dock_hit >= 0) {
                /* Icon click: map icon index → demo_window index.
                 * Dock icon 0 = Terminal (window 0), icon 1 = Files (window 1).
                 * Icons beyond the window count are cosmetic only. */
                int count2;
                gui_desktop_get_windows(&count2);
                if (dock_hit < count2) {
                    focus_demo_window(dock_hit);
                    serial_puts("[COMP] Dock icon="); serial_putdec((uint64_t)dock_hit);
                    serial_puts(" raised\n");
                } else {
                    uint32_t pid = gui_dock_task_id(dock_hit);
                    int index = managed_window_by_pid(pid);
                    if (index >= 0) {
                        focus_managed_window(index);
                        serial_puts("[COMP] Dock app pid=");
                        serial_putdec(pid);
                        serial_puts(" raised\n");
                    }
                }
            }
            prev_buttons = comp_button_state;
            return;  /* dock consumed the click */
        }

        int hit = hit_test_demo_window(cx, cy);

        if (hit >= 0) {
            int count;
            gui_win_desc_t *dw = gui_desktop_get_windows(&count);
            focus_demo_window(hit);

            /* Shared window chrome geometry, including scaled hit areas. */
            int btn = hit_test_buttons(&dw[hit], cx, cy);
            if (btn == 1 || btn == 2) {
                dw[hit].hidden = true;
                is_maximized[hit] = false;
                gui_anim_cancel(&dw[hit].x); gui_anim_cancel(&dw[hit].y);
                gui_anim_cancel(&dw[hit].w); gui_anim_cancel(&dw[hit].h);
                focused_demo_idx = -1;
                int *order = gui_desktop_get_order();
                for (int i = count - 1; i >= 0; i--) {
                    if (!dw[order[i]].hidden) {
                        focus_demo_window(order[i]);
                        break;
                    }
                }
            } else if (btn == 3) {
                /* Maximize / restore toggle — animated (Phase 3.3) */
                if (is_maximized[hit]) {
                    uint32_t duration = gui_theme_get()->reduced_motion ? 0 : 180;
                    gui_anim_start(&dw[hit].x, saved_geom[hit][0], duration, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].y, saved_geom[hit][1], duration, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].w, saved_geom[hit][2], duration, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].h, saved_geom[hit][3], duration, gui_ease_out_cubic, NULL, NULL);
                    is_maximized[hit] = false;
                } else {
                    saved_geom[hit][0] = dw[hit].x;
                    saved_geom[hit][1] = dw[hit].y;
                    saved_geom[hit][2] = dw[hit].w;
                    saved_geom[hit][3] = dw[hit].h;
                    gui_rect_t area = gui_work_area(display_get_width(), display_get_height());
                    uint32_t duration = gui_theme_get()->reduced_motion ? 0 : 180;
                    gui_anim_start(&dw[hit].x, area.x, duration, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].y, area.y, duration, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].w, area.w, duration, gui_ease_out_cubic, NULL, NULL);
                    gui_anim_start(&dw[hit].h, area.h - GUI_TITLEBAR_H,
                                   duration, gui_ease_out_cubic, NULL, NULL);
                    is_maximized[hit] = true;
                }
            } else if (gui_desktop_click(hit, cx, cy)) {
                dragging = false;
            } else if (cy < dw[hit].y + GUI_TITLEBAR_H) {
                /* Titlebar click → start drag */
                dragging = true;
                drag_win_idx = hit;
                drag_off_x = cx - dw[hit].x;
                drag_off_y = cy - dw[hit].y;
            }

            /* Raise clicked window to front (changes render order, not struct data) */
            if (!dw[hit].hidden) gui_desktop_raise_window(hit);
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
    gui_rect_t content = gui_desktop_terminal_rect();
    *tw = content.w > 0 ? (uint32_t)content.w : 1;
    *th = content.h > 0 ? (uint32_t)content.h : 1;
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

typedef struct {
    const uint32_t *src;
    uint32_t *dst;
    uint32_t src_pitch, dst_pitch, sw, sc, ox, oy, dw;
    uint32_t y_start, y_end;
} scaleblit_arg_t;

void scaleblit_worker(void *arg, void *result)
{
    (void)result;
    scaleblit_arg_t *a = arg;

    for (uint32_t _y = a->y_start; _y < a->y_end; _y++) {
        const uint32_t *_src = a->src + _y * a->src_pitch;
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

static void blit_scaled_integer(uint32_t *dst, uint32_t dst_pitch,
                                const window_t *win,
                                const fullscreen_layout_t *layout)
{
    extern int ap_worker_count;
    extern int smp_submit_any(void (*)(void *, void *), void *, void *);
    extern void smp_wait(int);

    int n_ap = win->height >= 64 ? ap_worker_count : 0;
    if (n_ap > 3) n_ap = 3;
    uint32_t band = win->height / (uint32_t)(n_ap + 1);
    scaleblit_arg_t bsp = {
        win->pixels, dst, win->surface_pitch, dst_pitch, win->width,
        layout->width / win->width, layout->x, layout->y, layout->width,
        0, win->height
    };
    scaleblit_arg_t args[3];
    int ids[3];

    for (int i = 0; i < n_ap; i++) {
        args[i] = bsp;
        args[i].y_start = (uint32_t)i * band;
        args[i].y_end = (uint32_t)(i + 1) * band;
        ids[i] = smp_submit_any(scaleblit_worker, &args[i], NULL);
        if (ids[i] < 0)
            scaleblit_worker(&args[i], NULL);
    }

    bsp.y_start = (uint32_t)n_ap * band;
    scaleblit_worker(&bsp, NULL);
    for (int i = 0; i < n_ap; i++)
        if (ids[i] >= 0) smp_wait(ids[i]);
}

/* ── Render One Frame ────────────────────────────────────────── */

static void compositor_render_desktop_scene(uint32_t *back, uint32_t width,
                                             uint32_t height, uint32_t pitch)
{
    gui_surface_t screen = { back, width, height, pitch };
    gui_desktop_render_background(&screen);
    int count = 0;
    gui_desktop_get_windows(&count);
    int *order = gui_desktop_get_order();
    int foreground = gui_desktop_get_focus();
    for (int i = 0; i < count; i++) {
        if (order[i] != foreground)
            gui_desktop_render_window(&screen, order[i]);
    }
    /* Preserve USER32's parent/child and popup ordering inside its layer. */
    for (int i = 0; i < render_count; i++)
        blit_window(back, pitch, width, height, &render_windows[render_order[i]]);
    if (foreground >= 0)
        gui_desktop_render_window(&screen, foreground);
    gui_desktop_render_dock(&screen);
}

static bool __hot compositor_render_frame(void)
{
    uint32_t *back = display_get_back_buffer();
    uint32_t w = display_get_width();
    uint32_t h = display_get_height();
    uint32_t p = display_get_pitch();

    if (!back) return false;

    /* Frame-level dirty skip: if nothing changed and cursor didn't move,
     * skip the entire render. Force dirty once per second for RTC clock. */
    {
        int32_t cx, cy;
        input_get_cursor(&cx, &cy);
        uint64_t now_tick = idt_get_ticks();
        bool cursor_moved = (cx != comp_last_cx || cy != comp_last_cy);
        bool clock_tick   = (now_tick - comp_dirty_tick >= 100); /* 1 sec @ 100Hz */
        bool frame_dirty = __atomic_exchange_n(&comp_frame_dirty, false,
                                                __ATOMIC_ACQ_REL);
        if (!frame_dirty && !cursor_moved && !clock_tick)
            return false;
        comp_last_cx = cx;
        comp_last_cy = cy;
        if (clock_tick)
            comp_dirty_tick = now_tick;
    }

    build_render_order();

    /* Fullscreen window: bypass desktop UI, blit scaled to screen.
     * Supports any source size via pixel-perfect integer upscaling. */
    for (int _fi = render_count - 1; _fi >= 0; _fi--) {
        if (gui_desktop_get_focus() >= 0) break;
        window_t *win = &render_windows[render_order[_fi]];
        if (!(win->flags & WND_FULLSCREEN) || !win->pixels) continue;

        uint32_t _sw = (uint32_t)win->width;
        uint32_t _sh = (uint32_t)win->height;
        uint32_t _sp = (uint32_t)win->surface_pitch;
        if (!_sw || !_sh || _sp < _sw || win->surface_height < _sh)
            continue;

        fullscreen_layout_t _layout;
        if (!compositor_fullscreen_layout(win, w, h, &_layout))
            continue;

        /* Direct scanout: source matches screen exactly */
        if (_sw == w && _sh == h) {
            if (_sp == w && p == w) {
                memcpy(back, win->pixels, (uint64_t)w * h * 4);
            } else {
                for (uint32_t _y = 0; _y < h; _y++)
                    memcpy(back + _y * p, win->pixels + _y * _sp,
                           (uint64_t)w * 4);
            }
            draw_cursor_overlay(back, p, w, h, win);
            display_mark_dirty();
            comp_direct_scanout++;
            goto rendered;
        }

        /* Pixel-perfect: largest integer scale that fits within the screen.
         * For DOOM 320×200 on 1024×768: scale=3 → 960×600, centered. */
        /* USER32 fullscreen represents a display-mode switch and should fill
         * the physical output while preserving aspect ratio, even when the
         * ratio requires a fractional scale (640x480 on 1280x800, for
         * example). Native SHM clients retain pixel-perfect integer scaling.
         * Oversized sources always use this path to avoid centering underflow. */
        if (_layout.sampled_scale) {
            uint32_t _dw = _layout.width;
            uint32_t _dh = _layout.height;
            uint32_t _ox = _layout.x;
            uint32_t _oy = _layout.y;
            uint64_t _xstep = ((uint64_t)_sw << 32) / _dw;
            uint64_t _ystep = ((uint64_t)_sh << 32) / _dh;

            memset(back, 0, (uint64_t)p * h * 4);
            uint64_t _ypos = 0;
            for (uint32_t _dy = 0; _dy < _dh; _dy++, _ypos += _ystep) {
                uint32_t _sy = (uint32_t)(_ypos >> 32);
                if (_sy >= _sh) _sy = _sh - 1;
                const uint32_t *_src = win->pixels + _sy * _sp;
                uint32_t *_dst = back + (_oy + _dy) * p + _ox;
                uint64_t _xpos = 0;
                for (uint32_t _dx = 0; _dx < _dw;
                     _dx++, _xpos += _xstep) {
                    uint32_t _sx = (uint32_t)(_xpos >> 32);
                    if (_sx >= _sw) _sx = _sw - 1;
                    _dst[_dx] = _src[_sx];
                }
            }

            draw_cursor_overlay(back, p, w, h, win);
            display_mark_dirty();
            comp_direct_scanout++;
            goto rendered;
        }

        /* Clear letterbox/pillarbox borders to black */
        memset(back, 0, (uint64_t)p * h * 4);

        /* Scale-blit: build one scaled row, then memcpy for repeated rows.
         * Row-major traversal keeps writes sequential for cache efficiency.
         * Parallel path partitions source rows into bands across APs. */
        blit_scaled_integer(back, p, win, &_layout);

        draw_cursor_overlay(back, p, w, h, win);
        display_mark_dirty();
        comp_direct_scanout++;
        goto rendered;
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

    refresh_dock_tasks();

    compositor_render_desktop_scene(back, w, h, p);

    /* Cursor visibility belongs to the surface under it, not whichever app
     * most recently called ShowCursor. Desktop chrome retains its pointer. */
    int32_t cursor_x, cursor_y;
    input_get_cursor(&cursor_x, &cursor_y);
    int cursor_owner = hit_test_dock(cursor_x, cursor_y) == -1
        ? hit_test_managed_window(cursor_x, cursor_y) : -1;
    window_t cursor_info;
    bool cursor_known = compositor_window_info(cursor_owner, &cursor_info);
    draw_cursor_overlay(back, p, w, h, cursor_known ? &cursor_info : NULL);

    display_mark_dirty();
rendered:
    /* Each blitter has joined its AP jobs before releasing these mappings. */
    release_render_surfaces();
    return true;
}

/* ── Compositor Main Loop ────────────────────────────────────── */

/* This runs as a QOS_INTERACTIVE kernel thread.
 * Called from sched_spawn("compositor", compositor_thread). */

volatile bool compositor_running;
static volatile bool compositor_claimed;

#ifdef __EMSCRIPTEN__
/* WASM helper: kernel uses rAF instead of compositor_thread; just mark running. */
void compositor_start_wasm(void)
{
    __atomic_store_n(&compositor_claimed, true, __ATOMIC_RELEASE);
    __atomic_store_n(&compositor_running, true, __ATOMIC_RELEASE);
}

/* One frame of the compositor, callable from JS via Module.ccall.
 * Replaces the body of the compositor_thread while loop on WASM. */
static bool __hot compositor_render_frame(void);
void wasm_compositor_frame(void)
{
    if (!compositor_running) return;
    bool gui_animation_active = gui_anim_any_active();
    gui_anim_tick(idt_get_ticks() * 10);
    if (gui_animation_active) compositor_request_frame();
    /* Drain mouse/wheel state into comp_button_state so process_mouse_input
     * sees button transitions. JS-side mousedown/mouseup pushed events
     * into the input ring via input_post_mouse_button. */
    if (input_has_events()) {
        int16_t mdx, mdy, wheel;
        uint8_t buttons;
        uint8_t key_buf[32 * 24];
        int nkeys = input_drain_coalesced(&mdx, &mdy, &buttons, &wheel, key_buf, 32);
        for (int i = 0; i < nkeys; i++) {
            uint8_t *event = &key_buf[i * 24];
            if (focused_demo_idx == 1 && event[0] == 1 &&
                gui_desktop_key(event[1], (event[3] & KEY_FLAG_SHIFT) != 0))
                __atomic_store_n(&comp_frame_dirty, true, __ATOMIC_RELEASE);
        }
        comp_button_state = buttons;
        compositor_route_wheel(wheel);
    }
    process_mouse_input();
    /* Resync to the real current button state so prev_buttons can
     * detect the next press. The drain's btn_pressed_accum or-back
     * leaves comp_button_state pinned at "pressed" even when the
     * physical button has already been released within the same
     * frame, breaking subsequent click detection. */
    comp_button_state = input_get_buttons();
    bool rendered = compositor_render_frame();
    /* The native compositor_thread's blit step never runs in WASM; the
     * rAF wrapper has to invoke display_flip itself or pixels stay in
     * the back buffer and the canvas shows black. */
    if (rendered)
        display_flip();
}
#endif

void compositor_thread(void)
{
    /* shell.c normally reserves the compositor before spawning this thread.
     * Keep direct callers safe as well, without allowing a second owner. */
    if (!__atomic_load_n(&compositor_claimed, __ATOMIC_ACQUIRE)) {
        bool expected = false;
        if (!__atomic_compare_exchange_n(&compositor_claimed, &expected, true,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            return;
    }

    proc_set_qos(QOS_INTERACTIVE);
    __atomic_store_n(&compositor_running, true, __ATOMIC_RELEASE);

    serial_puts("[COMP] Compositor thread started (QOS_INTERACTIVE)\n");

    bool comp_was_fullscreen = false;

    while (compositor_running) {
        /* 0. Drain PS/2 and USB reports outside hardware IRQ context. */
        if (ps2_poll) ps2_poll();
        if (xhci_poll) xhci_poll();

        /* Native fullscreen clients (e.g. DOOM) consume SYS_GET_INPUT_EVENT.
         * USER32 fullscreen windows keep normal queue draining for Alt+Tab;
         * their HID events are delivered directly by the Win32 bridge. */
        bool has_fullscreen = false;
        bool has_direct_input_fullscreen = false;
        {
            extern bool input_game_mode;
            bool managed_foreground = gui_desktop_get_focus() < 0;
            uint64_t flags = compositor_windows_lock();
            for (int _i = 0; _i < MAX_WINDOWS; _i++) {
                if (!managed_foreground) break;
                if ((windows[_i].flags &
                     (WND_ACTIVE | WND_VISIBLE | WND_FULLSCREEN)) ==
                    (WND_ACTIVE | WND_VISIBLE | WND_FULLSCREEN) &&
                    !(windows[_i].flags & WND_MINIMIZED)) {
                    has_fullscreen = true;
                    if (!(windows[_i].flags & WND_USER32))
                        has_direct_input_fullscreen = true;
                }
            }
            compositor_windows_unlock(flags);
            input_game_mode = has_direct_input_fullscreen;
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
            compositor_route_wheel(wheel);

            /* Push keyboard events into the ring buffer */
            /* Each key_buf entry is an input_event_t (24 bytes):
             *   offset 0: type (uint8_t)
             *   offset 1: scancode (uint8_t) */
            for (int i = 0; i < nkeys; i++) {
                uint8_t *evt = &key_buf[i * 24];
                uint8_t type = evt[0];
                uint8_t sc   = evt[1];

#if !defined(OK_QUIET) || !OK_QUIET
                static uint32_t input_trace_count;
                if (input_trace_count++ < 64) {
                    serial_puts("[COMP] key type=");
                    serial_putdec(type);
                    serial_puts(" sc=0x");
                    serial_puthex(sc, 2);
                    serial_puts("\n");
                }
#endif

                /* Both transports supply HID usages and post-transition flags.
                 * Set-1 aliases here collide with HID slash (0x38) and L (0x0F). */
                uint8_t modifiers = evt[3];
                bool comp_shift_held = (modifiers & KEY_FLAG_SHIFT) != 0;
                bool comp_ctrl_held = (modifiers & KEY_FLAG_CTRL) != 0;
                if (type == 1 && (modifiers & KEY_FLAG_ALT) && sc == 0x2B) {
                    cycle_task_switcher();
                    continue;  /* don't push Tab into key ring while Alt is held */
                }

                /* Push event into key_ring (for games / GUI) */
                if (!has_fullscreen && type == 1 && focused_demo_idx == 1 &&
                    gui_desktop_key(sc, comp_shift_held)) {
                    __atomic_store_n(&comp_frame_dirty, true, __ATOMIC_RELEASE);
                    continue;
                }
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
                            if (modifiers & KEY_FLAG_CAPS) {
                                if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                                else if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                            }
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
        /* input_drain_coalesced preserves a press+release pair as one pressed
         * frame so fast clicks are observable. Restore the physical state
         * immediately afterward; otherwise a native drag remains armed until
         * another HID report happens to arrive. */
        comp_button_state = input_get_buttons();

        /* Mark frame dirty on mouse movement (keyboard input dirtied via
         * compositor_signal_dirty when terminal/app writes new pixels) */
        {
            int32_t _cx, _cy;
            input_get_cursor(&_cx, &_cy);
            if (_cx != comp_last_cx || _cy != comp_last_cy)
                compositor_request_frame();
        }

        /* 2. Tick animation engine (100 APIC ticks = 1000ms) */
        bool gui_animation_active = gui_anim_any_active();
        gui_anim_tick(idt_get_ticks() * 10);  /* convert to ms (100Hz * 10 = ms) */
        if (gui_animation_active) compositor_request_frame();

        /* 3. Render frame */
        bool rendered = compositor_render_frame();

        /* 4. Flip (waits for VBlank) */
        if (rendered)
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
            /* A surviving managed window may simply have restored its display
             * mode. Keep its foreground ownership; only return to the terminal
             * when no visible managed focus remains. */
            uint64_t flags = compositor_windows_lock();
            bool lost_focus = focused_window < 0 || focused_window >= MAX_WINDOWS ||
                (windows[focused_window].flags &
                 (WND_ACTIVE | WND_VISIBLE)) !=
                    (WND_ACTIVE | WND_VISIBLE) ||
                (windows[focused_window].flags & WND_MINIMIZED);
            compositor_windows_unlock(flags);
            if (lost_focus) focus_demo_window(0);
        }
        comp_was_fullscreen = has_fullscreen;

        comp_frames++;

        /* INTERACTIVE tasks outrank normal Win32 processes. Park until the
         * next scheduler tick so frame pacing cannot monopolize the BSP. */
        sched_sleep_ticks(1);
    }

    __atomic_store_n(&compositor_claimed, false, __ATOMIC_RELEASE);
}

/* ── Control ─────────────────────────────────────────────────── */

void compositor_stop(void)
{
    __atomic_store_n(&compositor_running, false, __ATOMIC_RELEASE);
#ifdef __EMSCRIPTEN__
    /* There is no native compositor thread to release the reservation. */
    __atomic_store_n(&compositor_claimed, false, __ATOMIC_RELEASE);
#endif
}

bool compositor_is_running(void)
{
    return __atomic_load_n(&compositor_claimed, __ATOMIC_ACQUIRE);
}

bool compositor_begin_start(void)
{
    bool expected = false;
    return __atomic_compare_exchange_n(&compositor_claimed, &expected, true,
                                       false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

void compositor_abort_start(void)
{
    __atomic_store_n(&compositor_running, false, __ATOMIC_RELEASE);
    __atomic_store_n(&compositor_claimed, false, __ATOMIC_RELEASE);
}

void compositor_focus_terminal(void)
{
    if (!__atomic_load_n(&compositor_running, __ATOMIC_ACQUIRE))
        return;

    focus_demo_window(0);
    display_mark_dirty();
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
    dragging = false;
    drag_win_idx = -1;
    managed_pointer_active = false;
    prev_buttons = 0;
    focused_demo_idx = 0;   /* terminal has focus by default */
    is_maximized[0] = false;
    is_maximized[1] = false;
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
