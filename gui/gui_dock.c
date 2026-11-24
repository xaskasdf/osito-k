/*
 * OsitoK GUI — Glass dock / bottom taskbar / phablet navigation bar
 *
 * Shared paged layout for rendering and pointer hit testing.
 */

#include "gui.h"

/* ── Dock items ────────────────────────────────────────────── */

#define DOCK_ITEM_COUNT GUI_DOCK_BUILTIN_COUNT

static gui_dock_task_t dock_tasks[GUI_DOCK_MAX_TASKS];
static int dock_task_count;

void gui_dock_set_tasks(const gui_dock_task_t *tasks, int count)
{
    if (count < 0) count = 0;
    if (count > GUI_DOCK_MAX_TASKS) count = GUI_DOCK_MAX_TASKS;
    for (int i = 0; i < count; i++)
        dock_tasks[i] = tasks[i];
    dock_task_count = count;
}

int gui_dock_item_count(void)
{
    return (int)DOCK_ITEM_COUNT + dock_task_count;
}

uint32_t gui_dock_task_id(int dock_index)
{
    int task_index = dock_index - (int)DOCK_ITEM_COUNT;
    if (task_index < 0 || task_index >= dock_task_count)
        return 0;
    return dock_tasks[task_index].id;
}

/* ── Render ────────────────────────────────────────────────── */

static int dock_page_index;

/* Phablet: a full-width navigation bar, larger targets, centered */
static gui_dock_layout_t nav_geometry(uint32_t width, uint32_t height)
{
    gui_dock_layout_t d = {0};
    int items = gui_dock_item_count();
    d.icon_size = gui_px(48); d.gap = gui_px(16);
    int slot = d.icon_size + d.gap;
    int available = (int)width - gui_px(32) - d.gap;
    d.capacity = available / slot;
    if (d.capacity < 1) d.capacity = 1;
    d.paged = items > d.capacity;
    if (d.paged) {
        d.capacity = (available - gui_px(56)) / slot;
        if (d.capacity < 1) d.capacity = 1;
    }
    int pages = (items + d.capacity - 1) / d.capacity;
    if (dock_page_index >= pages) dock_page_index = pages - 1;
    if (dock_page_index < 0) dock_page_index = 0;
    d.first = dock_page_index * d.capacity;
    d.count = items - d.first;
    if (d.count > d.capacity) d.count = d.capacity;
    int h = gui_nav_height();
    d.bounds = (gui_rect_t){ 0, (int)height - h, (int)width, h };
    d.start_x = ((int)width - (d.count * slot - d.gap)) / 2;
    if (d.paged) {
        d.previous = (gui_rect_t){ gui_px(8), d.bounds.y + (h - gui_px(40)) / 2, gui_px(28), gui_px(40) };
        d.next = d.previous;
        d.next.x = (int)width - gui_px(36);
    }
    return d;
}

gui_dock_layout_t gui_dock_geometry(uint32_t width, uint32_t height)
{
    if (gui_theme_get()->layout == GUI_LAYOUT_PHABLET)
        return nav_geometry(width, height);
    gui_dock_layout_t d = {0};
    bool bar = gui_theme_get()->layout == GUI_LAYOUT_TASKBAR;
    int items = gui_dock_item_count();
    d.icon_size = gui_px(40); d.gap = gui_px(8);
    int slot = d.icon_size + d.gap;
    int reserve = bar ? gui_px(136) : gui_px(32);
    int available = (int)width - reserve - d.gap;
    d.capacity = available / slot;
    if (d.capacity < 1) d.capacity = 1;
    d.paged = items > d.capacity;
    if (d.paged) {
        d.capacity = (available - gui_px(56)) / slot;
        if (d.capacity < 1) d.capacity = 1;
    }
    int pages = (items + d.capacity - 1) / d.capacity;
    if (dock_page_index >= pages) dock_page_index = pages - 1;
    if (dock_page_index < 0) dock_page_index = 0;
    d.first = dock_page_index * d.capacity;
    d.count = items - d.first;
    if (d.count > d.capacity) d.count = d.capacity;
    int w = d.count * slot + d.gap + (d.paged ? gui_px(56) : 0);
    d.bounds = (gui_rect_t){ bar ? 0 : ((int)width - w) / 2,
        (int)height - gui_px(bar ? 52 : 66), bar ? (int)width : w, gui_px(bar ? 52 : 56) };
    d.start_x = d.bounds.x + d.gap + (d.paged ? gui_px(28) : 0);
    if (d.paged) {
        d.previous = (gui_rect_t){ d.bounds.x + gui_px(4), d.bounds.y + gui_px(10), gui_px(24), gui_px(32) };
        d.next = d.previous;
        d.next.x = d.start_x + d.count * slot;
    }
    return d;
}

gui_rect_t gui_dock_item_rect(const gui_dock_layout_t *d, int index)
{
    if (index < d->first || index >= d->first + d->count) return (gui_rect_t){0};
    return (gui_rect_t){ d->start_x + (index - d->first) * (d->icon_size + d->gap),
        d->bounds.y + (d->bounds.h - d->icon_size) / 2 - gui_px(1), d->icon_size, d->icon_size };
}

int gui_dock_hit(uint32_t width, uint32_t height, int32_t x, int32_t y)
{
    gui_dock_layout_t d = gui_dock_geometry(width, height);
    if (!gui_rect_contains(d.bounds, x, y)) return -1;
    if (d.paged && gui_rect_contains(d.previous, x, y)) return -3;
    if (d.paged && gui_rect_contains(d.next, x, y)) return -4;
    for (int i = d.first; i < d.first + d.count; i++)
        if (gui_rect_contains(gui_dock_item_rect(&d, i), x, y)) return i;
    return -2;
}

int gui_dock_page_count(void)
{
    const gui_metrics_t *m = gui_theme_metrics();
    gui_dock_layout_t d = gui_dock_geometry(m->width, m->height);
    return (gui_dock_item_count() + d.capacity - 1) / d.capacity;
}

void gui_dock_page(int delta)
{
    int count = gui_dock_page_count();
    dock_page_index += delta;
    if (dock_page_index < 0) dock_page_index = count - 1;
    if (dock_page_index >= count) dock_page_index = 0;
}

void gui_dock_render(gui_surface_t *s, uint32_t screen_w, uint32_t screen_h,
                     int32_t cursor_x, int32_t cursor_y)
{
    gui_dock_layout_t d = gui_dock_geometry(screen_w, screen_h);
    bool bar = gui_theme_get()->layout == GUI_LAYOUT_TASKBAR;
    bool nav = gui_theme_get()->layout == GUI_LAYOUT_PHABLET;
    int glyph = gui_px(nav ? 28 : 22);
    gui_soft_shadow(s, d.bounds, gui_px(10), gui_px(3), 64);
    gui_glass_panel(s, d.bounds, bar || nav ? 0 : gui_px(9), true);
    int win_count = 0;
    gui_win_desc_t *wins = gui_desktop_get_windows(&win_count);
    for (int i = d.first; i < d.first + d.count; i++) {
        gui_rect_t r = gui_dock_item_rect(&d, i);
        bool builtin = i < (int)DOCK_ITEM_COUNT;
        gui_dock_task_t *task = builtin ? NULL : &dock_tasks[i - (int)DOCK_ITEM_COUNT];
        bool visible = builtin ? i < win_count && !wins[i].hidden : !task->minimized;
        bool focused = builtin ? visible && gui_desktop_get_focus() == i : task->focused;
        bool hover = gui_rect_contains(r, cursor_x, cursor_y);
        unsigned state = (focused ? GUI_CONTROL_SELECTED : 0) | (hover ? GUI_CONTROL_HOVER : 0);
        if (hover || focused) gui_control(s, r, "", state);
        if (builtin) {
            gui_rect_t icon = nav ? (gui_rect_t){ r.x + (r.w - glyph) / 2, r.y + (r.h - glyph) / 2, glyph, glyph }
                                  : (gui_rect_t){ r.x + gui_px(9), r.y + gui_px(9), glyph, glyph };
            gui_ui_icon(s, icon, i == 0 ? GUI_ICON_TERMINAL : GUI_ICON_SETTINGS,
                         visible ? (i ? 0xFFBCDCF2 : 0xFFC2E2D1) : GUI_MUTED);
        } else {
            char label[2] = { task->label, 0 };
            gui_rect_t tile = { r.x + gui_px(7), r.y + gui_px(6), gui_px(26), gui_px(27) };
            gui_rounded_rect(s, tile.x, tile.y, tile.w, tile.h, gui_px(4), task->color | 0xFF000000);
            gui_ui_text_fit(s, tile, label, GUI_INK, true);
        }
        if (visible || !builtin) {
            int length = gui_px(focused ? 20 : 8);
            gui_fill_rect(s, r.x + (r.w - length) / 2, r.y + r.h + gui_px(1), length, gui_px(2),
                           focused ? GUI_FOCUS : GUI_MUTED);
        }
    }
    if (d.paged) {
        gui_control(s, d.previous, "", gui_rect_contains(d.previous, cursor_x, cursor_y) ? GUI_CONTROL_HOVER : 0);
        gui_control(s, d.next, "", gui_rect_contains(d.next, cursor_x, cursor_y) ? GUI_CONTROL_HOVER : 0);
        gui_ui_icon(s, d.previous, GUI_ICON_PREVIOUS, GUI_INK);
        gui_ui_icon(s, d.next, GUI_ICON_NEXT, GUI_INK);
    }
    if (bar && screen_w > (uint32_t)gui_px(230)) {
        const gui_debug_info_t *debug = gui_panel_get_debug();
        char time[6] = { (char)('0' + debug->rtc_hour / 10), (char)('0' + debug->rtc_hour % 10), ':',
                        (char)('0' + debug->rtc_min / 10), (char)('0' + debug->rtc_min % 10), 0 };
        gui_rect_t clock = { (int)screen_w - gui_px(94), d.bounds.y, gui_px(80), d.bounds.h };
        gui_ui_text_fit(s, clock, time, GUI_INK, true);
    }
}
