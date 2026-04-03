/*
 * OsitoK GUI — Desktop orchestrator
 *
 * Paints the complete elementaryOS-inspired desktop:
 *   1. Gradient background
 *   2. Top panel (Wingpanel)
 *   3. Demo windows
 *   4. Bottom dock (Plank)
 */

#include "gui.h"

/* ── Helpers ───────────────────────────────────────────────── */

static int _itoa(uint64_t val, char *buf)
{
    char tmp[20]; int n = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (val) { tmp[n++] = '0' + (int)(val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

static int _scat(char *dst, int pos, const char *src)
{
    while (*src) dst[pos++] = *src++;
    return pos;
}

static int _scat_num(char *dst, int pos, uint64_t val)
{
    char tmp[20]; _itoa(val, tmp);
    return _scat(dst, pos, tmp);
}

/* ── Desktop state ─────────────────────────────────────────── */

static uint32_t scr_w, scr_h;

/* Terminal shm surface — set by compositor before rendering starts.
 * Blitted into window 0 content area at the correct z-order. */
static uint32_t *term_surface_px;
static uint32_t  term_surface_w;
static uint32_t  term_surface_h;

void gui_desktop_set_terminal_surface(uint32_t *px, uint32_t w, uint32_t h)
{
    term_surface_px = px;
    term_surface_w  = w;
    term_surface_h  = h;
}

/* Demo windows — positioned relative to screen size */
static gui_win_desc_t demo_windows[2];
#define NUM_DEMO_WINDOWS 2
static int demo_order[NUM_DEMO_WINDOWS] = {0, 1}; /* render order: last = on top */

/* Initial geometry for each window — used to restore after close */
static int32_t init_geom[NUM_DEMO_WINDOWS][4];  /* x, y, w, h */

void gui_desktop_init(uint32_t screen_w, uint32_t screen_h)
{
    scr_w = screen_w;
    scr_h = screen_h;

    /* Main window: "Terminal" — 60% width, 45% height, centered */
    int32_t mw = (int32_t)(screen_w * 3 / 5);
    int32_t mh = (int32_t)(screen_h * 9 / 20);
    int32_t mx = ((int32_t)screen_w - mw) / 2;
    int32_t my = GUI_PANEL_HEIGHT + 20;

    demo_windows[0].x = mx;
    demo_windows[0].y = my;
    demo_windows[0].w = mw;
    demo_windows[0].h = mh;
    demo_windows[0].title = "Terminal";
    demo_windows[0].content_color = 0xFF1A1A2E;  /* Dark terminal blue */
    init_geom[0][0] = mx; init_geom[0][1] = my;
    init_geom[0][2] = mw; init_geom[0][3] = mh;

    /* Secondary window: "System Info" — smaller, offset */
    int32_t sw = (int32_t)(screen_w * 2 / 5);
    int32_t sh = (int32_t)(screen_h / 4);
    int32_t sx = mx + mw - sw + 40;
    int32_t sy = my + mh - sh + 60;

    /* Clamp to screen */
    if (sx + sw > (int32_t)screen_w - 10) sx = (int32_t)screen_w - sw - 10;
    if (sy + sh + GUI_TITLEBAR_H > (int32_t)screen_h - GUI_DOCK_HEIGHT - 20)
        sy = (int32_t)screen_h - sh - GUI_TITLEBAR_H - GUI_DOCK_HEIGHT - 20;

    demo_windows[1].x = sx;
    demo_windows[1].y = sy;
    demo_windows[1].w = sw;
    demo_windows[1].h = sh;
    demo_windows[1].title = "System Info";
    demo_windows[1].content_color = GUI_WINDOW_BG;
    init_geom[1][0] = sx; init_geom[1][1] = sy;
    init_geom[1][2] = sw; init_geom[1][3] = sh;

}

/* ── Per-window content rendering ─────────────────────────── */

static void render_window_content(gui_surface_t *screen, int idx)
{
    gui_win_desc_t *w = &demo_windows[idx];
    if (w->hidden) return;
    /* Focused = this window is last (topmost) in the render order */
    bool focused = (demo_order[NUM_DEMO_WINDOWS - 1] == idx);
    gui_window_render(screen, w, focused);

    if (idx == 0) {
        /* Terminal window: fill background first, then blit shm surface if set. */
        int32_t cx0 = w->x + GUI_BORDER_W;
        int32_t cy0 = w->y + GUI_TITLEBAR_H + GUI_BORDER_W;
        gui_fill_rect(screen, cx0, cy0, w->w, w->h, w->content_color);

        if (term_surface_px) {
            int32_t bw = (int32_t)term_surface_w;
            int32_t bh = (int32_t)term_surface_h;
            int32_t sx = 0, sy = 0;
            int32_t dx = cx0, dy = cy0;

            if (dx < 0) { sx = -dx; bw += dx; dx = 0; }
            if (dy < 0) { sy = -dy; bh += dy; dy = 0; }
            if (dx + bw > (int32_t)screen->width)  bw = (int32_t)screen->width  - dx;
            if (dy + bh > (int32_t)screen->height) bh = (int32_t)screen->height - dy;
            if (dx + bw > cx0 + w->w) bw = cx0 + w->w - dx;
            if (dy + bh > cy0 + w->h) bh = cy0 + w->h - dy;

            if (bw > 0 && bh > 0) {
                for (int32_t row = 0; row < bh; row++) {
                    uint32_t *d = screen->pixels
                                  + (uint32_t)(dy + row) * screen->pitch
                                  + (uint32_t)dx;
                    const uint32_t *s = term_surface_px
                                        + (uint32_t)(sy + row) * term_surface_w
                                        + (uint32_t)sx;
                    memcpy(d, s, (uint32_t)bw * 4);
                }
            }
        }
    } else if (idx == 1) {
        /* System Info window content */
        int32_t tx = w->x + 12;
        int32_t ty = w->y + GUI_TITLEBAR_H + 12;
        gui_draw_text(screen, tx, ty, "Kernel:  OsitoK", GUI_PANEL_TEXT, 0);

        const gui_debug_info_t *di = gui_panel_get_debug();
        char line[64];
        int p = 0;
        uint64_t secs = di->ticks / 100;
        uint64_t h = secs / 3600, m = (secs / 60) % 60, s2 = secs % 60;
        p = _scat(line, p, "Uptime:  ");
        p = _scat_num(line, p, h); line[p++] = ':';
        if (m  < 10) { line[p++] = '0'; } p = _scat_num(line, p, m);  line[p++] = ':';
        if (s2 < 10) { line[p++] = '0'; } p = _scat_num(line, p, s2);
        line[p] = '\0';
        gui_draw_text(screen, tx, ty + 20, line, GUI_PANEL_TEXT, 0);

        p = 0;
        p = _scat(line, p, "Memory:  ");
        p = _scat_num(line, p, di->mem_total_mb);
        p = _scat(line, p, " MB");
        line[p] = '\0';
        gui_draw_text(screen, tx, ty + 40, line, GUI_PANEL_TEXT, 0);

        p = 0;
        p = _scat(line, p, "Used:    ");
        p = _scat_num(line, p, di->mem_used_mb);
        p = _scat(line, p, " MB");
        line[p] = '\0';
        gui_draw_text(screen, tx, ty + 60, line, GUI_PANEL_TEXT, 0);

        p = 0;
        p = _scat(line, p, "Procs:   ");
        p = _scat_num(line, p, di->procs);
        line[p] = '\0';
        gui_draw_text(screen, tx, ty + 80, line, GUI_ACCENT, 0);
    }
}

/* ── Render complete desktop ───────────────────────────────── */

void gui_desktop_render(gui_surface_t *screen)
{
    /* Use live screen dimensions (survives modeset) instead of cached scr_w/scr_h */
    uint32_t sw = screen->width;
    uint32_t sh = screen->height;

    /* 1. Gradient background */
    gui_gradient_v(screen, 0, 0, (int32_t)sw, (int32_t)sh,
                   GUI_BG_DARK, GUI_BG_LIGHT);

    /* 2. Top panel */
    gui_panel_render(screen, sw);

    /* 3. Demo windows (back to front using render order) */
    for (int i = 0; i < NUM_DEMO_WINDOWS; i++)
        render_window_content(screen, demo_order[i]);

    /* 4. Bottom dock */
    gui_dock_render(screen, sw, sh);
}

gui_win_desc_t *gui_desktop_get_windows(int *count)
{
    if (count) *count = NUM_DEMO_WINDOWS;
    return demo_windows;
}

void gui_desktop_raise_window(int idx)
{
    /* Move window idx to end of render order (= on top) */
    if (idx < 0 || idx >= NUM_DEMO_WINDOWS) return;
    /* Find it in order and shift others down */
    int pos = -1;
    for (int i = 0; i < NUM_DEMO_WINDOWS; i++) {
        if (demo_order[i] == idx) { pos = i; break; }
    }
    if (pos < 0) return;
    for (int i = pos; i < NUM_DEMO_WINDOWS - 1; i++)
        demo_order[i] = demo_order[i + 1];
    demo_order[NUM_DEMO_WINDOWS - 1] = idx;
}

/* Dock action: restore window to initial position if closed, then raise.
 * Restores position if window was hidden (close/minimize). */
void gui_desktop_show_window(int idx)
{
    if (idx < 0 || idx >= NUM_DEMO_WINDOWS) return;
    gui_win_desc_t *w = &demo_windows[idx];
    if (w->hidden) {
        w->x = init_geom[idx][0];
        w->y = init_geom[idx][1];
        w->w = init_geom[idx][2];
        w->h = init_geom[idx][3];
        w->hidden = false;
    }
    gui_desktop_raise_window(idx);
}

int *gui_desktop_get_order(void)
{
    return demo_order;
}
