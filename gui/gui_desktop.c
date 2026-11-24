/*
 * OsitoK GUI — Desktop orchestrator
 *
 * Paints the complete elementaryOS-inspired desktop:
 *   1. Gradient background
 *   2. Top panel (Wingpanel)
 *   3. Demo windows
 *   4. Bottom dock (Plank)
 * In the phablet layout the windows are maximized and only the front one
 * is painted; the dock becomes a navigation bar.
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

/* Cursor position for dock hover (set by compositor each frame) */
static int32_t desk_cursor_x, desk_cursor_y;
static bool settings_keyboard_focus;

void gui_desktop_set_cursor(int32_t x, int32_t y)
{
    if (x != desk_cursor_x || y != desk_cursor_y) settings_keyboard_focus = false;
    desk_cursor_x = x;
    desk_cursor_y = y;
}

/* Demo windows — positioned relative to screen size */
static gui_win_desc_t demo_windows[2];
#define NUM_DEMO_WINDOWS 2
static int demo_order[NUM_DEMO_WINDOWS] = {0, 1}; /* render order: last = on top */
static int desktop_focus;

/* Initial geometry for each window — used to restore after close */
static int32_t init_geom[NUM_DEMO_WINDOWS][4];  /* x, y, w, h */

static unsigned last_scale = 100;
static gui_layout_t last_layout = GUI_LAYOUT_DOCK;
static uint32_t detected_width_mm, detected_height_mm;
static int settings_open = -1, settings_focus, settings_hot, settings_first;
static int settings_scroll;
static struct {
    bool active;
    int start_y, start_scroll;
    unsigned scale;
    gui_rect_t body;
} settings_drag;
static int settings_values[3];
static const uint16_t setting_scales[] = { 0, 100, 125, 150, 175, 200, 250, 300 };
static const char *const setting_labels[3][8] = {
    { "Dock and top panel", "Bottom taskbar", "Phablet (portrait)" },
    { "Automatic", "100%", "125%", "150%", "175%", "200%", "250%", "300%" },
    { "Auto (monitor)", "13.3 inch", "14 inch", "15.6 inch", "24 inch", "27 inch" }
};
static const int setting_counts[3] = { 3, 8, 6 };
/* The "Desktop layout" row, in setting_labels[0] order */
static const gui_layout_t setting_layouts[3] = {
    GUI_LAYOUT_DOCK, GUI_LAYOUT_TASKBAR, GUI_LAYOUT_PHABLET
};

static int layout_setting(void)
{
    for (int i = 0; i < 3; i++)
        if (setting_layouts[i] == gui_theme_get()->layout) return i;
    return 0;
}

void gui_desktop_get_cursor(int32_t *x, int32_t *y)
{
    if (x) *x = desk_cursor_x;
    if (y) *y = desk_cursor_y;
}

static void desktop_cancel_window_anim(gui_win_desc_t *w)
{
    gui_anim_cancel(&w->x); gui_anim_cancel(&w->y);
    gui_anim_cancel(&w->w); gui_anim_cancel(&w->h);
}

static void desktop_fit(gui_win_desc_t *w)
{
    gui_rect_t area = gui_work_area(scr_w, scr_h);
    if (gui_theme_get()->layout == GUI_LAYOUT_PHABLET) {
        /* Every window fills the work area, title bar included */
        w->x = area.x; w->y = area.y; w->w = area.w;
        w->h = area.h > GUI_TITLEBAR_H + 1 ? area.h - GUI_TITLEBAR_H : 1;
        return;
    }
    int margin = gui_px(12);
    int max_w = area.w - margin * 2;
    int max_h = area.h - GUI_TITLEBAR_H - margin * 2;
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;
    if (w->w > max_w) w->w = max_w;
    if (w->h > max_h) w->h = max_h;
    if (w->w < 1) w->w = 1;
    if (w->h < 1) w->h = 1;
    if (w->x + w->w > area.x + area.w - margin) w->x = area.x + area.w - margin - w->w;
    if (w->y + w->h + GUI_TITLEBAR_H > area.y + area.h - margin)
        w->y = area.y + area.h - margin - w->h - GUI_TITLEBAR_H;
    if (w->x < area.x + margin) w->x = area.x + margin;
    if (w->y < area.y + margin) w->y = area.y + margin;
}

void gui_desktop_resize(uint32_t width, uint32_t height)
{
    if (!width || !height) return;
    gui_theme_set_display(width, height);
    unsigned scale = gui_theme_metrics()->scale_percent;
    gui_layout_t layout = gui_theme_get()->layout;
    if (scr_w == width && scr_h == height && last_scale == scale && last_layout == layout) return;
    scr_w = width; scr_h = height;
    for (int i = 0; i < NUM_DEMO_WINDOWS; i++) {
        gui_win_desc_t *w = &demo_windows[i];
        desktop_cancel_window_anim(w);
        if (last_scale && scale != last_scale) {
            w->x = (int32_t)((int64_t)w->x * scale / last_scale);
            w->y = (int32_t)((int64_t)w->y * scale / last_scale);
            w->w = (int32_t)((int64_t)w->w * scale / last_scale);
            w->h = (int32_t)((int64_t)w->h * scale / last_scale);
        }
        desktop_fit(w);
        init_geom[i][0] = w->x; init_geom[i][1] = w->y;
        init_geom[i][2] = w->w; init_geom[i][3] = w->h;
    }
    last_scale = scale; last_layout = layout;
    settings_open = -1;
    settings_scroll = 0;
    settings_drag.active = false;
}

typedef struct {
    gui_rect_t body, viewport, controls[5], labels[3], footer;
    gui_rect_t up, down, track, thumb;
    int content_height, scroll_max;
    bool compact, stacked;
} desktop_settings_layout_t;

static desktop_settings_layout_t desktop_settings_layout(void)
{
    gui_win_desc_t *w = &demo_windows[1];
    desktop_settings_layout_t l = {0};
    l.body = (gui_rect_t){w->x, w->y + GUI_TITLEBAR_H, w->w, w->h};
    int rail = 0;
    /* Reserving a scrollbar can cross the stacking threshold. A second pass
     * settles that dependency without altering any input rectangles later. */
    for (int pass = 0; pass < 2; pass++) {
        int width = l.body.w - rail;
        l.stacked = width < gui_px(440);
        l.compact = l.stacked || l.body.h < gui_px(350);
        int header = gui_px(l.compact ? 44 : 80);
        int row = gui_px(l.stacked ? 72 : l.compact ? 40 : 50);
        l.viewport = (gui_rect_t){l.body.x, l.body.y + header, width, l.body.h - header - gui_px(6)};
        if (l.viewport.w < 0) l.viewport.w = 0;
        if (l.viewport.h < 0) l.viewport.h = 0;
        /* Sum the individually rounded metrics used by the painter. Scaling
         * the aggregate instead can clip the final line at fractional DPI. */
        l.content_height = 3 * row + gui_px(30) + gui_px(40) +
                           (l.stacked ? 3 : 2) * gui_px(23) + gui_px(22) + gui_px(6);
        l.scroll_max = l.content_height > l.viewport.h ? l.content_height - l.viewport.h : 0;
        rail = l.scroll_max ? gui_px(24) : 0;
    }
    if (settings_scroll > l.scroll_max) settings_scroll = l.scroll_max;
    if (settings_scroll < 0) settings_scroll = 0;
    int pad = gui_px(l.stacked ? 16 : 20);
    int row = gui_px(l.stacked ? 72 : l.compact ? 40 : 50);
    int top = l.viewport.y - settings_scroll;
    int column = l.viewport.w * 2 / 5;
    for (int i = 0; i < 3; i++) {
        if (l.stacked) {
            l.labels[i] = (gui_rect_t){l.viewport.x + pad, top + i * row, l.viewport.w - pad * 2, gui_px(22)};
            l.controls[i] = l.labels[i];
            l.controls[i].y += gui_px(24); l.controls[i].h = gui_px(32);
        } else {
            l.controls[i] = (gui_rect_t){l.viewport.x + column, top + i * row,
                                        l.viewport.w - column - pad, gui_px(32)};
            l.labels[i] = (gui_rect_t){l.viewport.x + pad, l.controls[i].y,
                                      column - pad - gui_px(16), l.controls[i].h};
        }
    }
    for (int i = 3; i < 5; i++)
        l.controls[i] = (gui_rect_t){l.viewport.x + pad, top + 3 * row + (i - 3) * gui_px(30),
                                    l.viewport.w - pad * 2, gui_px(28)};
    l.footer = (gui_rect_t){l.viewport.x + pad, l.controls[4].y + gui_px(40),
                            l.viewport.w - pad * 2, gui_px(22)};
    if (l.scroll_max && l.viewport.h > 0) {
        int button = gui_px(18);
        if (button * 2 > l.viewport.h) button = l.viewport.h / 2;
        l.up = (gui_rect_t){l.body.x + l.body.w - gui_px(22), l.viewport.y, gui_px(18), button};
        l.down = l.up; l.down.y = l.viewport.y + l.viewport.h - button;
        l.track = (gui_rect_t){l.up.x + gui_px(2), l.up.y + button, l.up.w - gui_px(4), l.viewport.h - button * 2};
        if (l.track.h > 0) {
            int height = (int)((int64_t)l.track.h * l.viewport.h / l.content_height);
            if (height < gui_px(24)) height = gui_px(24);
            if (height > l.track.h) height = l.track.h;
            l.thumb = l.track; l.thumb.h = height;
            l.thumb.y += (int)((int64_t)(l.track.h - height) * settings_scroll / l.scroll_max);
        }
    }
    return l;
}

static void desktop_settings_geometry(gui_rect_t controls[5], gui_rect_t *body)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    *body = l.body;
    for (int i = 0; i < 5; i++) controls[i] = l.controls[i];
}

static void desktop_settings_scroll_by(int64_t delta)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    int64_t next = (int64_t)settings_scroll + delta;
    settings_scroll = next < 0 ? 0 : next > l.scroll_max ? l.scroll_max : (int)next;
}

bool gui_desktop_pointer_captured(void)
{
    return settings_drag.active && desktop_focus == 1 && !demo_windows[1].hidden;
}

/* Once the thumb owns the pointer, horizontal motion and leaving the window
 * do not transfer the gesture. The compositor calls this through button-up. */
bool gui_desktop_pointer_update(int32_t x, int32_t y, bool left_down)
{
    (void)x;
    if (!settings_drag.active) return false;
    desktop_settings_layout_t l = desktop_settings_layout();
    int travel = l.track.h - l.thumb.h;
    if (!gui_desktop_pointer_captured() || travel <= 0 ||
        settings_drag.scale != gui_theme_metrics()->scale_percent ||
        l.body.x != settings_drag.body.x || l.body.y != settings_drag.body.y ||
        l.body.w != settings_drag.body.w || l.body.h != settings_drag.body.h) {
        settings_drag.active = false;
        return true;
    }
    /* Relative to the press, so grabbing a rounded thumb never jumps a pixel. */
    int64_t delta = ((int64_t)y - settings_drag.start_y) * l.scroll_max;
    delta += delta < 0 ? -travel / 2 : travel / 2;
    int64_t next = settings_drag.start_scroll + delta / travel;
    settings_scroll = next < 0 ? 0 : next > l.scroll_max ? l.scroll_max : (int)next;
    settings_keyboard_focus = false;
    if (!left_down) settings_drag.active = false;
    return true;
}

static bool desktop_settings_reveal(int index)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    if (index < 0 || index >= 5 || l.viewport.h < l.controls[index].h || l.controls[index].w <= 0) return false;
    gui_rect_t r = l.controls[index];
    if (index < 3 && l.stacked && r.y + r.h - l.labels[index].y <= l.viewport.h) {
        r.h += r.y - l.labels[index].y; r.y = l.labels[index].y;
    }
    if (r.h + gui_px(6) <= l.viewport.h) { r.y -= gui_px(3); r.h += gui_px(6); }
    if (r.y < l.viewport.y) desktop_settings_scroll_by(r.y - l.viewport.y);
    else if (r.y + r.h > l.viewport.y + l.viewport.h)
        desktop_settings_scroll_by(r.y + r.h - l.viewport.y - l.viewport.h);
    return true;
}

static bool desktop_settings_control_visible(const desktop_settings_layout_t *l, int i)
{
    gui_rect_t r = l->controls[i];
    return r.w > 0 && r.h > 0 && r.y >= l->viewport.y && r.y + r.h <= l->viewport.y + l->viewport.h;
}

static gui_rect_t desktop_local_rect(gui_rect_t r, int x, int y)
{
    r.x -= x; r.y -= y; return r;
}

static bool desktop_clip_surface(gui_surface_t *screen, gui_rect_t r,
                                  gui_surface_t *clip, int *origin_x, int *origin_y)
{
    if (r.w <= 0 || r.h <= 0) return false;
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int64_t x1 = (int64_t)r.x + r.w, y1 = (int64_t)r.y + r.h;
    if (x1 > screen->width) x1 = screen->width;
    if (y1 > screen->height) y1 = screen->height;
    if (x0 >= x1 || y0 >= y1) return false;
    *origin_x = x0; *origin_y = y0;
    *clip = (gui_surface_t){screen->pixels + (uint32_t)y0 * screen->pitch + x0,
                            (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), screen->pitch};
    return true;
}

static void desktop_setting_step(int row, int value)
{
    gui_theme_options_t options = *gui_theme_get();
    if (row >= 0 && row < 3) {
        int count = setting_counts[row];
        value = (value % count + count) % count;
        settings_values[row] = value;
    }
    if (row == 0) options.layout = setting_layouts[settings_values[0]];
    if (row == 1) options.scale_percent = setting_scales[settings_values[1]];
    if (row == 2) {
        static const uint16_t diagonals[] = { 0, 133, 140, 156, 240, 270 };
        if (!value) {
            gui_theme_set_physical_size(detected_width_mm, detected_height_mm);
        } else {
            const gui_metrics_t *m = gui_theme_metrics();
            uint64_t square = (uint64_t)m->width * m->width + (uint64_t)m->height * m->height;
            uint64_t diagonal = m->width + m->height;
            for (int i = 0; i < 8; i++) diagonal = (diagonal + square / diagonal) / 2;
            uint32_t mm = diagonals[value] * 254u / 100;
            gui_theme_set_physical_size((uint32_t)(mm * (uint64_t)m->width / diagonal),
                                        (uint32_t)(mm * (uint64_t)m->height / diagonal));
        }
    }
    if (row == 3) options.glass = !options.glass;
    if (row == 4) options.reduced_motion = !options.reduced_motion;
    gui_theme_configure(&options);
    settings_open = -1;
    gui_desktop_resize(scr_w, scr_h);
    (void)desktop_settings_reveal(row);
}

static gui_popup_layout_t desktop_settings_menu(const gui_rect_t controls[5], gui_rect_t body)
{
    if (settings_open < 0 || settings_open >= 3) return (gui_popup_layout_t){0};
    body = desktop_settings_layout().viewport;
    int right = body.x + body.w, bottom = body.y + body.h;
    if (body.x < 0) body.x = 0;
    if (body.y < 0) body.y = 0;
    if (right > (int)scr_w) right = (int)scr_w;
    if (bottom > (int)scr_h) bottom = (int)scr_h;
    body.w = right - body.x; body.h = bottom - body.y;
    return gui_popup_geometry(controls[settings_open], body, setting_counts[settings_open],
                              settings_first, settings_hot);
}

static void desktop_settings_footer(gui_surface_t *clip, gui_rect_t footer, bool stacked)
{
    gui_hline(clip, footer.x, footer.y - gui_px(8), footer.w, 0xFF344757);
    const gui_metrics_t *m = gui_theme_metrics();
    char line[128];
    int n = _scat_num(line, 0, m->width); n = _scat(line, n, " x ");
    n = _scat_num(line, n, m->height); n = _scat(line, n, " px");
    if (stacked) {
        line[n] = 0;
        gui_ui_text_fit(clip, footer, line, GUI_MUTED, false);
        footer.y += gui_px(23); n = 0;
    } else n = _scat(line, n, "   /   ");
    n = _scat_num(line, n, m->effective_width); n = _scat(line, n, " x ");
    n = _scat_num(line, n, m->effective_height); n = _scat(line, n, " effective"); line[n] = 0;
    gui_ui_text_fit(clip, footer, line, GUI_MUTED, false);
    footer.y += gui_px(23);
    n = _scat_num(line, 0, m->scale_percent); n = _scat(line, n, "%  ");
    if (gui_theme_get()->scale_percent && gui_theme_get()->scale_percent != m->scale_percent) {
        n = _scat(line, n, stacked ? "Display size limit" : "Maximum scale that fits this display");
    } else if (m->physical_size_valid) {
        n = _scat_num(line, n, m->physical_ppi); n = _scat(line, n, " ppi");
    } else if (gui_theme_get()->scale_percent) n = _scat(line, n, stacked ? "Manual; size unknown" : "Manual scale; display size unknown");
    else n = _scat(line, n, stacked ? "Auto (estimated)" : "Auto scale estimated from resolution");
    line[n] = 0;
    gui_ui_text_fit(clip, footer, line, GUI_MUTED, false);
    footer.y += gui_px(23);
    const gui_debug_info_t *di = gui_panel_get_debug();
    n = _scat_num(line, 0, di->mem_used_mb); n = _scat(line, n, " / ");
    n = _scat_num(line, n, di->mem_total_mb); n = _scat(line, n, " MB   ");
    n = _scat_num(line, n, di->procs); n = _scat(line, n, " processes"); line[n] = 0;
    gui_ui_text_fit(clip, footer, line, GUI_MUTED, false);
}

static void desktop_settings_render(gui_surface_t *screen)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    gui_rect_t body = l.body;
    gui_surface_t clip;
    int ox, oy;
    if (!desktop_clip_surface(screen, body, &clip, &ox, &oy)) return;
    gui_rect_t heading = { body.x + gui_px(20), body.y + gui_px(12), body.w - gui_px(40), gui_px(26) };
    gui_ui_text_fit(&clip, desktop_local_rect(heading, ox, oy), "Appearance", GUI_INK, false);
    if (!l.compact) {
        heading.y += gui_px(26);
        gui_ui_text_fit(&clip, desktop_local_rect(heading, ox, oy), "Choose a layout and a comfortable size.", GUI_MUTED, false);
    }
    settings_values[0] = layout_setting();
    for (unsigned i = 0; i < sizeof(setting_scales) / sizeof(setting_scales[0]); i++)
        if (setting_scales[i] == gui_theme_get()->scale_percent) settings_values[1] = (int)i;
    if (l.scroll_max) {
        gui_rect_t track = desktop_local_rect(l.track, ox, oy);
        gui_fill_rect(&clip, track.x, track.y, track.w, track.h, 0xFF101E2A);
        gui_control(&clip, desktop_local_rect(l.thumb, ox, oy), "",
                     settings_drag.active ? GUI_CONTROL_SELECTED :
                     gui_rect_contains(l.thumb, desk_cursor_x, desk_cursor_y) ? GUI_CONTROL_HOVER : 0);
        gui_rect_t arrows[2] = {l.up, l.down};
        for (int i = 0; i < 2; i++) {
            bool enabled = i ? settings_scroll < l.scroll_max : settings_scroll > 0;
            gui_rect_t local = desktop_local_rect(arrows[i], ox, oy);
            if (enabled && gui_rect_contains(arrows[i], desk_cursor_x, desk_cursor_y))
                gui_control(&clip, local, "", GUI_CONTROL_HOVER);
            gui_ui_icon(&clip, local, i ? GUI_ICON_CHEVRON : GUI_ICON_CHEVRON_UP,
                         enabled ? GUI_INK : 0xFF657A8A);
        }
    }
    if (!desktop_clip_surface(screen, l.viewport, &clip, &ox, &oy)) return;
    const char *labels[] = { "Desktop layout", "Interface scale", "Display size" };
    bool over_view = gui_rect_contains(l.viewport, desk_cursor_x, desk_cursor_y);
    for (int i = 0; i < 5; i++) {
        unsigned state = over_view && gui_rect_contains(l.controls[i], desk_cursor_x, desk_cursor_y) ? GUI_CONTROL_HOVER : 0;
        if (settings_keyboard_focus && settings_focus == i) state |= GUI_CONTROL_FOCUS;
        gui_rect_t control = desktop_local_rect(l.controls[i], ox, oy);
        if (i < 3) {
            gui_ui_text_fit(&clip, desktop_local_rect(l.labels[i], ox, oy), labels[i], GUI_MUTED, false);
            gui_selector(&clip, control, setting_labels[i][settings_values[i]],
                          state | (settings_open == i ? GUI_CONTROL_SELECTED : 0));
        } else gui_check(&clip, control, i == 3 ? "Glass transparency" : "Reduce motion",
                         i == 3 ? gui_theme_get()->glass : gui_theme_get()->reduced_motion, state);
    }
    desktop_settings_footer(&clip, desktop_local_rect(l.footer, ox, oy), l.stacked);
    if (settings_open >= 0 && settings_open < 3) {
        gui_popup_layout_t menu = desktop_settings_menu(l.controls, body);
        settings_first = menu.first;
        menu.bounds = desktop_local_rect(menu.bounds, ox, oy);
        menu.items = desktop_local_rect(menu.items, ox, oy);
        menu.previous = desktop_local_rect(menu.previous, ox, oy);
        menu.next = desktop_local_rect(menu.next, ox, oy);
        gui_popup_render(&clip, &menu, setting_labels[settings_open], settings_values[settings_open],
                           settings_hot, settings_keyboard_focus, desk_cursor_x - ox, desk_cursor_y - oy);
    }
}

static void desktop_settings_open(int index)
{
    settings_open = index; settings_hot = settings_values[index]; settings_first = 0;
    desktop_settings_layout_t l = desktop_settings_layout();
    gui_popup_layout_t menu = desktop_settings_menu(l.controls, l.body);
    if (!menu.count) settings_open = -1;
}

/* A positive wheel delta moves toward the top, matching the input driver. */
bool gui_desktop_scroll(int window, int32_t x, int32_t y, int delta)
{
    if (window != 1 || demo_windows[1].hidden || !delta ||
        x < 0 || y < 0 || x >= (int)scr_w || y >= (int)scr_h) return false;
    desktop_settings_layout_t l = desktop_settings_layout();
    if (!gui_rect_contains(l.body, x, y) || y < l.viewport.y ||
        y >= l.viewport.y + l.viewport.h) return false;
    if (gui_desktop_pointer_captured()) return true;
    settings_keyboard_focus = false;
    if (settings_open >= 0) {
        gui_popup_layout_t menu = desktop_settings_menu(l.controls, l.body);
        if (!menu.count) { settings_open = -1; return true; }
        int64_t first = (int64_t)menu.first - delta;
        int last = menu.total - menu.count;
        settings_first = first < 0 ? 0 : first > last ? last : (int)first;
        if (settings_hot < settings_first) settings_hot = settings_first;
        if (settings_hot >= settings_first + menu.count)
            settings_hot = settings_first + menu.count - 1;
    } else desktop_settings_scroll_by(-(int64_t)delta * gui_px(40));
    return true;
}

bool gui_desktop_click(int window, int32_t x, int32_t y)
{
    settings_drag.active = false;
    if (window != 1 || demo_windows[1].hidden) { settings_open = -1; return false; }
    if (x < 0 || y < 0 || x >= (int)scr_w || y >= (int)scr_h) return false;
    settings_keyboard_focus = false;
    gui_rect_t controls[5], body;
    desktop_settings_geometry(controls, &body);
    if (settings_open >= 0) {
        gui_popup_layout_t menu = desktop_settings_menu(controls, body);
        settings_first = menu.first;
        int hit = gui_popup_hit(&menu, x, y);
        if (hit >= 0) desktop_setting_step(settings_open, hit);
        else if (hit == GUI_POPUP_PREVIOUS) settings_hot = --settings_first;
        else if (hit == GUI_POPUP_NEXT) settings_hot = ++settings_first + menu.count - 1;
        else if (hit == GUI_POPUP_OUTSIDE) settings_open = -1;
        return true;
    }
    desktop_settings_layout_t l = desktop_settings_layout();
    if (l.scroll_max && gui_rect_contains(l.body, x, y)) {
        if (gui_rect_contains(l.up, x, y)) { desktop_settings_scroll_by(-gui_px(40)); return true; }
        if (gui_rect_contains(l.down, x, y)) { desktop_settings_scroll_by(gui_px(40)); return true; }
        if (gui_rect_contains(l.thumb, x, y)) {
            if (l.track.h > l.thumb.h) {
                settings_drag.active = true;
                settings_drag.start_y = y;
                settings_drag.start_scroll = settings_scroll;
                settings_drag.scale = gui_theme_metrics()->scale_percent;
                settings_drag.body = l.body;
            }
            return true;
        }
        if (gui_rect_contains(l.track, x, y)) {
            int page = l.viewport.h - gui_px(16);
            if (page < 1) page = 1;
            if (y < l.thumb.y) desktop_settings_scroll_by(-page);
            else if (y >= l.thumb.y + l.thumb.h) desktop_settings_scroll_by(page);
            return true;
        }
    }
    if (!gui_rect_contains(l.viewport, x, y)) return false;
    for (int i = 0; i < 5; i++) {
        if (!gui_rect_contains(controls[i], x, y)) continue;
        settings_focus = i;
        if (!desktop_settings_reveal(i)) return true;
        if (i < 3) desktop_settings_open(i);
        else desktop_setting_step(i, 0);
        return true;
    }
    return false;
}

bool gui_desktop_key(uint32_t key, bool shift)
{
    if (demo_windows[1].hidden) return false;
    if (key != 0x2B && key != 0x28 && key != 0x2C && key != 0x29 &&
        key != 0x4A && key != 0x4B && key != 0x4D && key != 0x4E && (key < 0x4F || key > 0x52))
        return false;
    settings_keyboard_focus = true;
    if (settings_drag.active) {
        settings_drag.active = false;
        if (key == 0x29)
            desktop_settings_scroll_by((int64_t)settings_drag.start_scroll - settings_scroll);
    }
    if (key == 0x29) { settings_open = -1; return true; }
    if (key == 0x2B) {
        settings_open = -1;
        settings_focus = (settings_focus + (shift ? 4 : 1)) % 5;
        (void)desktop_settings_reveal(settings_focus);
        return true;
    }
    desktop_settings_layout_t l = desktop_settings_layout();
    if (settings_open >= 0) {
        gui_popup_layout_t menu = desktop_settings_menu(l.controls, l.body);
        if (!menu.count) { settings_open = -1; return true; }
        int count = setting_counts[settings_open];
        if (key == 0x4A) settings_hot = 0;
        else if (key == 0x4D) settings_hot = count - 1;
        else if (key == 0x4B || key == 0x4E) {
            settings_hot += key == 0x4B ? -menu.count : menu.count;
            if (settings_hot < 0) settings_hot = 0;
            if (settings_hot >= count) settings_hot = count - 1;
        } else if (key >= 0x4F && key <= 0x52) {
            int direction = key == 0x50 || key == 0x52 ? -1 : 1;
            settings_hot = (settings_hot + direction + count) % count;
        } else desktop_setting_step(settings_open, settings_hot);
        return true;
    }
    if (key == 0x4A || key == 0x4D) {
        settings_focus = key == 0x4A ? 0 : 4;
        (void)desktop_settings_reveal(settings_focus);
        return true;
    }
    if (key == 0x4B || key == 0x4E) {
        int page = l.viewport.h - gui_px(16);
        if (page < 1) page = 1;
        desktop_settings_scroll_by(key == 0x4B ? -page : page);
        settings_keyboard_focus = false;
        return true;
    }
    /* Wheel/page scrolling may hide the old focus. The first activation shows
     * it again, so an offscreen option never changes without being visible. */
    bool was_visible = desktop_settings_control_visible(&l, settings_focus);
    if (!desktop_settings_reveal(settings_focus) || !was_visible) return true;
    if (key >= 0x4F && key <= 0x52) {
        int direction = key == 0x50 || key == 0x52 ? -1 : 1;
        if (settings_focus < 3) desktop_setting_step(settings_focus, settings_values[settings_focus] + direction);
        return true;
    }
    if (settings_focus >= 3) desktop_setting_step(settings_focus, 0);
    else desktop_settings_open(settings_focus);
    return true;
}

void gui_desktop_init(uint32_t screen_w, uint32_t screen_h)
{
    gui_theme_set_display(screen_w, screen_h);
#ifdef __KERNEL_X86__
    /* Use the existing display driver's EDID path only after it is ready.
     * GOP/Intel paths without physical dimensions retain the labelled fallback. */
    extern int gpu_display_is_ready(void) __attribute__((weak));
    extern int gpu_display_read_edid(uint8_t *) __attribute__((weak));
    if (gpu_display_is_ready && gpu_display_read_edid && gpu_display_is_ready()) {
        uint8_t edid[128];
        if (gpu_display_read_edid(edid) == 0) (void)gui_theme_from_edid(edid, sizeof(edid));
    }
#endif
    scr_w = screen_w; scr_h = screen_h;
    gui_rect_t area = gui_work_area(screen_w, screen_h);
    int margin = gui_px(20);
    int mw = (int)screen_w * 2 / 3;
    if (mw > gui_px(1040)) mw = gui_px(1040);
    int mh = area.h * 3 / 5;
    demo_windows[0] = (gui_win_desc_t){ (int)screen_w / 12, area.y + margin,
        mw, mh, "Terminal", GUI_TERM_BG, false };
    demo_windows[1] = (gui_win_desc_t){ (int)screen_w - gui_px(560) - margin,
        area.y + (area.h - gui_px(426)) / 2, gui_px(560), gui_px(390),
        "System", GUI_SURFACE, true };
    for (int i = 0; i < NUM_DEMO_WINDOWS; i++) {
        desktop_fit(&demo_windows[i]);
        init_geom[i][0] = demo_windows[i].x; init_geom[i][1] = demo_windows[i].y;
        init_geom[i][2] = demo_windows[i].w; init_geom[i][3] = demo_windows[i].h;
    }
    demo_order[0] = 1; demo_order[1] = 0;
    desktop_focus = 0;
    last_scale = gui_theme_metrics()->scale_percent;
    last_layout = gui_theme_get()->layout;
    detected_width_mm = gui_theme_metrics()->width_mm;
    detected_height_mm = gui_theme_metrics()->height_mm;
    settings_open = -1;
    settings_focus = 0; settings_hot = 0; settings_first = 0; settings_scroll = 0;
    settings_drag.active = false;
    settings_keyboard_focus = false;
    settings_values[0] = layout_setting();
    settings_values[1] = 0; settings_values[2] = 0;
    for (unsigned i = 0; i < sizeof(setting_scales) / sizeof(setting_scales[0]); i++)
        if (setting_scales[i] == gui_theme_get()->scale_percent) settings_values[1] = (int)i;
}

/* ── Per-window content rendering ─────────────────────────── */

/* Shared by the painter and the compositor's terminal surface allocation. */
gui_rect_t gui_desktop_terminal_rect(void)
{
    const gui_win_desc_t *w = &demo_windows[0];
    gui_rect_t r = {w->x, w->y + GUI_TITLEBAR_H, w->w, w->h};
    if (r.w <= 0 || r.h <= 0) return (gui_rect_t){r.x, r.y, 0, 0};
    int px = gui_px(8), py = px;
    if (px * 2 >= r.w) px = (r.w - 1) / 2;
    if (py * 2 >= r.h) py = (r.h - 1) / 2;
    r.x += px; r.y += py; r.w -= px * 2; r.h -= py * 2;
    return r;
}

static void render_window_content(gui_surface_t *screen, int idx)
{
    gui_win_desc_t *w = &demo_windows[idx];
    if (w->hidden) return;
    bool focused = desktop_focus == idx;
    gui_window_render(screen, w, focused);
    if (idx == 1) {
        desktop_settings_render(screen);
        return;
    }
    if (!term_surface_px) return;
    gui_rect_t content = gui_desktop_terminal_rect();
    int32_t cx0 = content.x, cy0 = content.y;
    int32_t bw = (int32_t)term_surface_w, bh = (int32_t)term_surface_h;
    int32_t sx = 0, sy = 0, dx = cx0, dy = cy0;
    if (dx < 0) { sx = -dx; bw += dx; dx = 0; }
    if (dy < 0) { sy = -dy; bh += dy; dy = 0; }
    if (dx + bw > (int32_t)screen->width) bw = (int32_t)screen->width - dx;
    if (dy + bh > (int32_t)screen->height) bh = (int32_t)screen->height - dy;
    if (dx + bw > cx0 + content.w) bw = cx0 + content.w - dx;
    if (dy + bh > cy0 + content.h) bh = cy0 + content.h - dy;
    if (bw > 0 && bh > 0) {
        for (int32_t row = 0; row < bh; row++)
            memcpy(screen->pixels + (uint32_t)(dy + row) * screen->pitch + dx,
                   term_surface_px + (uint32_t)(sy + row) * term_surface_w + sx, (uint32_t)bw * 4);
    }
}

/* ── Render complete desktop ───────────────────────────────── */

/* Cached gradient background — rendered once, memcpy'd each frame.
 * Much cheaper than recomputing per-pixel interpolation 60x/sec. */
static uint32_t *bg_cache;
static uint32_t  bg_cache_w, bg_cache_h;

static void bg_cache_ensure(gui_surface_t *screen)
{
    uint32_t sw = screen->width, sh = screen->height;
    if (bg_cache && bg_cache_w == sw && bg_cache_h == sh) return;
    if (!sw || !sh || sw > 16384 || sh > 16384) return;
    extern void *kmalloc(uint64_t size);
    extern void kfree(void *ptr);
    uint32_t *fresh = (uint32_t *)kmalloc((uint64_t)sw * sh * 4);
    if (!fresh) return;
    gui_surface_t tmp = { fresh, sw, sh, sw };
    gui_wallpaper_render(&tmp);
    if (bg_cache) kfree(bg_cache);
    bg_cache = fresh;
    bg_cache_w = sw; bg_cache_h = sh;
}

void gui_desktop_render_background(gui_surface_t *screen)
{
    gui_desktop_resize(screen->width, screen->height);
    bg_cache_ensure(screen);
    if (bg_cache && bg_cache_w == screen->width && bg_cache_h == screen->height) {
        for (uint32_t row = 0; row < screen->height; row++)
            memcpy(screen->pixels + row * screen->pitch, bg_cache + row * screen->width,
                   (uint64_t)screen->width * 4);
    } else {
        gui_wallpaper_render(screen);
    }
}

void gui_desktop_render_window(gui_surface_t *screen, int index)
{
    if (index >= 0 && index < NUM_DEMO_WINDOWS)
        render_window_content(screen, index);
}

void gui_desktop_render_base(gui_surface_t *screen)
{
    gui_desktop_render_background(screen);
    if (gui_theme_get()->layout == GUI_LAYOUT_PHABLET) {
        /* Maximized windows cover each other: only the front one shows */
        for (int i = NUM_DEMO_WINDOWS - 1; i >= 0; i--)
            if (!demo_windows[demo_order[i]].hidden) {
                gui_desktop_render_window(screen, demo_order[i]);
                break;
            }
        return;
    }
    for (int i = 0; i < NUM_DEMO_WINDOWS; i++)
        gui_desktop_render_window(screen, demo_order[i]);
}

void gui_desktop_render(gui_surface_t *screen)
{
    gui_desktop_render_base(screen);
    gui_desktop_render_dock(screen);
}

void gui_desktop_render_dock(gui_surface_t *screen)
{
    gui_panel_render(screen, screen->width);
    gui_dock_render(screen, screen->width, screen->height, desk_cursor_x, desk_cursor_y);
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
    gui_desktop_set_focus(idx);
}

void gui_desktop_set_focus(int index)
{
    desktop_focus = index >= 0 && index < NUM_DEMO_WINDOWS ? index : -1;
    if (desktop_focus != 1) {
        settings_open = -1;
        settings_drag.active = false;
    }
}

int gui_desktop_get_focus(void)
{
    return desktop_focus >= 0 && !demo_windows[desktop_focus].hidden ? desktop_focus : -1;
}

/* Dock action: restore window to initial position if closed, then raise.
 * Restores position if window was hidden (close/minimize). */
void gui_desktop_show_window(int idx)
{
    if (idx < 0 || idx >= NUM_DEMO_WINDOWS) return;
    gui_win_desc_t *w = &demo_windows[idx];
    if (w->hidden) {
        desktop_cancel_window_anim(w);
        w->x = init_geom[idx][0]; w->y = init_geom[idx][1];
        w->w = init_geom[idx][2]; w->h = init_geom[idx][3];
        w->hidden = false;
        desktop_fit(w);
        if (!gui_theme_get()->reduced_motion) {
            int target = w->y;
            w->y += gui_px(12);
            gui_anim_start(&w->y, target, 160, gui_ease_out_cubic, NULL, NULL);
        }
    }
    gui_desktop_raise_window(idx);
}

int *gui_desktop_get_order(void)
{
    return demo_order;
}
