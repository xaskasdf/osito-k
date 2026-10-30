/*
 * gui_task.c -- the desktop on the SM8350 panel, in its phablet layout
 *
 * `desktop` starts this task: the shared desktop (gui/) in the phablet
 * layout (status bar on top, navigation bar below, one maximized window at
 * a time) at the panel's physical size, which lands on 300% (360 x 816 dp).
 * The text console moves into the Terminal window: framebuffer.c draws into
 * an off-screen surface the desktop composes, laid out again from the log
 * (console_redraw). Frames are painted into a back buffer in cached RAM and
 * copied to the panel, only when something changed: console text, an
 * animation, a key, a finger, the status bar's minute. `desktop off` gives
 * the panel back to the console. The Power key brings the next window to
 * the front.
 *
 * Fingers (platform_touch_event, in panel pixels): a tap acts where it
 * started, on the lift, unless the finger travelled more than 8 dp; then
 * it is a swipe, and scrolls what is under it one wheel notch per 40 dp
 * (the Appearance pane). Nav bar items bring their window to the front;
 * anything else is a click in the front window.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "gui.h"
#include "task.h"

#define FRAME_MS    50                  /* at most 20 frames a second */

volatile int desktop_active;
static volatile int stop_req, next_req;

void desktop_cycle_window(void) { next_req = 1; }
void desktop_close(void)         { stop_req = 1; }

/* Power: the window behind comes to the front (restored if it was closed) */
static void cycle_window(void)
{
    int count = 0;
    gui_desktop_get_windows(&count);
    int *order = gui_desktop_get_order();
    if (count > 1)
        gui_desktop_show_window(order[0]);
}

static struct {
    int down, swipe;
    int32_t x0, y0, notches;
} finger;

static void tap(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    int hit = gui_dock_hit(w, h, x, y);
    if (hit != -1) {
        int count = 0;
        gui_desktop_get_windows(&count);
        if (hit == -3 || hit == -4)
            gui_dock_page(hit == -3 ? -1 : 1);
        else if (hit >= 0 && hit < count)
            gui_desktop_show_window(hit);
        return;
    }
    /* A tap on the scroll thumb grabs it; let go at once, swipes scroll */
    if (gui_desktop_click(gui_desktop_get_focus(), x, y) && gui_desktop_pointer_captured())
        gui_desktop_pointer_update(x, y, false);
}

static void touch_input(int32_t x, int32_t y, int down, uint32_t w, uint32_t h)
{
    if (down && !finger.down) {
        finger.down = 1;
        finger.swipe = 0;
        finger.x0 = x;
        finger.y0 = y;
        finger.notches = 0;
        return;
    }
    if (!finger.down)
        return;
    int32_t dx = x - finger.x0, dy = y - finger.y0, slop = gui_px(8);
    if (dx * dx + dy * dy > slop * slop)
        finger.swipe = 1;
    if (finger.swipe) {
        int32_t notches = dy / gui_px(40);  /* finger down: content down (upward) */
        if (notches != finger.notches)
            gui_desktop_scroll(gui_desktop_get_focus(), finger.x0, finger.y0,
                               notches - finger.notches);
        finger.notches = notches;
    }
    if (!down) {
        finger.down = 0;
        if (!finger.swipe)
            tap(finger.x0, finger.y0, w, h);
    }
}

/* The status bar: uptime as the clock (no RTC), memory, frame rate */
static void status_info(uint64_t fps)
{
    uint64_t secs = timer_get_tick_count() / 100;
    gui_debug_info_t d = {0};
    d.fps = fps;
    d.mem_total_mb = mem_get_total() >> 20;
    d.mem_free_mb = mem_get_free() >> 20;
    d.mem_used_mb = d.mem_total_mb - d.mem_free_mb;
    d.ticks = timer_get_tick_count();
    d.rtc_hour = (uint8_t)(secs / 3600 % 24);
    d.rtc_min = (uint8_t)(secs / 60 % 60);
    d.rtc_sec = (uint8_t)(secs % 60);
    gui_panel_set_debug(&d);
}

void gui_task(void *arg)
{
    (void)arg;
    uint64_t panel;
    uint32_t w, h, w_mm, h_mm;
    if (platform_panel(&panel, &w, &h, &w_mm, &h_mm)) {
        serial_puts("desktop: no panel here\n");
        return;
    }

    gui_theme_options_t options = *gui_theme_get();
    options.layout = GUI_LAYOUT_PHABLET;
    gui_theme_configure(&options);
    gui_theme_set_physical_size(w_mm, h_mm);
    gui_desktop_init(w, h);

    uint64_t back_pages = ((uint64_t)w * h * 4 + 4095) / 4096;
    uint32_t *back = mem_alloc_pages(back_pages);
    gui_rect_t t = gui_desktop_terminal_rect();
    uint64_t term_pages = ((uint64_t)t.w * t.h * 4 + 4095) / 4096;
    uint32_t *term = t.w > 0 && t.h > 0 ? mem_alloc_pages(term_pages) : (uint32_t *)0;
    if (!back || !term) {
        serial_puts("desktop: out of memory\n");
        if (back) mem_free_pages(back, back_pages);
        if (term) mem_free_pages(term, term_pages);
        return;
    }
    gui_desktop_set_terminal_surface(term, (uint32_t)t.w, (uint32_t)t.h);

    /* The console into the Terminal window */
    uint64_t flags = console_lock();
    fb_init_platform((uint64_t)(uintptr_t)term, (uint32_t)t.w, (uint32_t)t.h, (uint32_t)t.w, 32);
    fb_set_text_top(0);
    desktop_active = 1;
    console_unlock(flags);
    console_redraw();
    serial_puts("[GUI ] desktop: phablet layout at ");
    serial_putdec(gui_theme_metrics()->scale_percent);
    serial_puts("%, ");
    serial_putdec(gui_theme_metrics()->effective_width);
    serial_puts(" x ");
    serial_putdec(gui_theme_metrics()->effective_height);
    serial_puts(" dp; Power switches windows, `desktop off` leaves\n");

    gui_surface_t screen = { back, w, h, w };
    uint64_t frames = 0, fps = 0, fps_t0 = timer_ms(), shown_min = ~0ULL;
    int first = 1;
    int32_t tx, ty;
    int tdown;
    while (platform_touch_event(&tx, &ty, &tdown))
        ;                               /* from before the desktop */
    finger.down = 0;
    while (!stop_req) {
        uint64_t now = timer_ms();
        int dirty = fb_take_dirty() | first;
        if (next_req) {
            next_req = 0;
            cycle_window();
            dirty = 1;
        }
        while (platform_touch_event(&tx, &ty, &tdown)) {
            touch_input(tx, ty, tdown, w, h);
            dirty = 1;
        }
        if (gui_anim_tick(now))
            dirty = 1;
        uint64_t minute = timer_get_tick_count() / 6000;
        if (minute != shown_min) {
            shown_min = minute;
            dirty = 1;
        }
        if (dirty) {
            status_info(fps);
            uint64_t t0 = timer_ms();
            gui_desktop_render(&screen);
            memcpy((void *)(uintptr_t)panel, back, (uint64_t)w * h * 4);
            frames++;
            if (first) {
                serial_puts("[GUI ] first frame: ");
                serial_putdec(timer_ms() - t0);
                serial_puts(" ms\n");
                first = 0;
            }
        }
        if (now - fps_t0 >= 1000) {
            fps = frames * 1000 / (now - fps_t0);
            frames = 0;
            fps_t0 = now;
        }
        task_delay_ms(FRAME_MS);
    }

    /* Back to the plain console */
    desktop_active = 0;
    platform_console_to_panel();
    gui_desktop_set_terminal_surface((uint32_t *)0, 0, 0);
    mem_free_pages(term, term_pages);
    mem_free_pages(back, back_pages);
    stop_req = 0;
    serial_puts("[GUI ] desktop closed\n");
}
