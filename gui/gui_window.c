/* Dark glass window frames. Painting and pointer input share these rectangles. */
#include "gui.h"

gui_rect_t gui_window_frame(const gui_win_desc_t *win)
{
    gui_rect_t r = { win->x, win->y, win->w, win->h + GUI_TITLEBAR_H };
    return r;
}

gui_rect_t gui_window_button_rect(const gui_win_desc_t *win, int button)
{
    /* Phablet windows are always maximized: close and minimize only */
    bool phablet = gui_theme_get()->layout == GUI_LAYOUT_PHABLET;
    int right = win->x + win->w - gui_px(6);
    int width = gui_px(button == 1 ? 42 : 30);
    int offset = button == 1 ? 0 : button == 3 || phablet ? gui_px(43) : gui_px(74);
    gui_rect_t r = { right - offset - width, win->y + gui_px(4), width, gui_px(26) };
    if (button < 1 || button > 3 || (phablet && button == 3) || r.x < win->x + gui_px(22))
        r.w = 0;
    return r;
}

int gui_window_button_hit(const gui_win_desc_t *win, int32_t x, int32_t y)
{
    if (win->hidden) return 0;
    for (int i = 1; i <= 3; i++)
        if (gui_rect_contains(gui_window_button_rect(win, i), x, y)) return i;
    return 0;
}

void gui_window_render(gui_surface_t *s, const gui_win_desc_t *win, bool focused)
{
    if (!win || win->hidden || win->w <= 0 || win->h < 0) return;
    gui_rect_t frame = gui_window_frame(win);
    gui_soft_shadow(s, frame, gui_px(focused ? 18 : 12), gui_px(4), focused ? 88 : 48);
    /* Snapshot the backdrop before painting our opaque client. */
    gui_rect_t title = { frame.x, frame.y, frame.w, GUI_TITLEBAR_H };
    gui_glass_panel(s, title, gui_px(5), focused);
    gui_fill_rect(s, frame.x, frame.y + GUI_TITLEBAR_H, frame.w, win->h, win->content_color);
    gui_vline(s, frame.x - 1, frame.y + gui_px(5), frame.h - gui_px(5), focused ? GUI_EDGE : 0xFF354857);
    gui_vline(s, frame.x + frame.w, frame.y + gui_px(5), frame.h - gui_px(5), focused ? GUI_EDGE : 0xFF354857);
    gui_hline(s, frame.x, frame.y + frame.h, frame.w, 0xFF080F16);

    int32_t pointer_x, pointer_y;
    gui_desktop_get_cursor(&pointer_x, &pointer_y);
    for (int button = 1; button <= 3; button++) {
        gui_rect_t r = gui_window_button_rect(win, button);
        if (r.w <= 0) continue;
        bool hover = focused && gui_rect_contains(r, pointer_x, pointer_y);
        if (button == 1 && (focused || hover)) {
            gui_rounded_rect(s, r.x, r.y, r.w, r.h, gui_px(3), hover ? 0xFFE57B66 : 0xFF97524C);
            gui_gradient_v(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2,
                            hover ? 0xFFDD7867 : 0xFFAF6258, hover ? 0xFFAB332F : 0xFF743B38);
            gui_hline(s, r.x + 2, r.y + 1, r.w - 4, hover ? 0xFFFFC4AD : 0xFFD59D8E);
        } else if (hover) {
            gui_control(s, r, "", GUI_CONTROL_HOVER);
        } else {
            gui_fill_rect_alpha(s, r.x, r.y, r.w, r.h / 2,
                                 focused ? 0x26D4EDFF : 0x12C6DAE8);
            gui_fill_rect_alpha(s, r.x, r.y + r.h / 2, r.w, r.h - r.h / 2, 0x18040E18);
            gui_hline(s, r.x + 1, r.y, r.w - 2, focused ? 0xFF7899AF : 0xFF4B6273);
            gui_vline(s, r.x, r.y + 1, r.h - 2, focused ? 0xFF577B94 : 0xFF3A5061);
            gui_hline(s, r.x + 1, r.y + r.h - 1, r.w - 2, 0xFF10212D);
        }
        int size = gui_px(14);
        gui_rect_t icon = { r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size, size };
        gui_ui_icon(s, icon, button == 1 ? GUI_ICON_CLOSE : button == 2 ? GUI_ICON_MINIMIZE : GUI_ICON_MAXIMIZE,
                     focused ? GUI_INK : GUI_MUTED);
    }
    gui_rect_t icon = { frame.x + gui_px(10), frame.y + gui_px(10), gui_px(16), gui_px(16) };
    if (frame.w > gui_px(160))
        gui_ui_icon(s, icon, win->title && win->title[0] == 'T' ? GUI_ICON_TERMINAL : GUI_ICON_SETTINGS,
                     focused ? GUI_FOCUS : GUI_MUTED);
    gui_rect_t label = { frame.x + gui_px(34), frame.y, frame.w - gui_px(152), GUI_TITLEBAR_H };
    gui_rect_t shadow = label; shadow.y += gui_px(1);
    gui_ui_text_fit(s, shadow, win->title, 0xFF040C14, false);
    gui_ui_text_fit(s, label, win->title, focused ? GUI_INK : GUI_MUTED, false);
}
