/*
 * OsitoK GUI — Bottom dock (elementaryOS Plank style)
 *
 * Centered rounded rectangle with application icons.
 * Each icon is a colored square with a label character.
 */

#include "gui.h"

/* ── Dock items ────────────────────────────────────────────── */

typedef struct {
    uint32_t color;
    char     label;
} dock_item_t;

static const dock_item_t dock_items[] = {
    { GUI_MAXIMIZE,  'T' },   /* Terminal (green) — window 0 */
    { GUI_ACCENT,    'I' },   /* System Info (blue) — window 1 */
};

#define DOCK_ITEM_COUNT  (sizeof(dock_items) / sizeof(dock_items[0]))

/* ── Render ────────────────────────────────────────────────── */

void gui_dock_render(gui_surface_t *s, uint32_t screen_w, uint32_t screen_h,
                     int32_t cursor_x, int32_t cursor_y)
{
    int32_t item_slot = GUI_DOCK_ICON_SIZE + GUI_DOCK_PADDING;
    int32_t dock_w = (int32_t)DOCK_ITEM_COUNT * item_slot + GUI_DOCK_PADDING;
    int32_t dock_h = GUI_DOCK_HEIGHT;
    int32_t dock_x = ((int32_t)screen_w - dock_w) / 2;
    int32_t dock_y = (int32_t)screen_h - dock_h - 8;  /* 8px from bottom */

    /* Dock container (rounded, semi-transparent) */
    gui_rounded_rect_alpha(s, dock_x, dock_y, dock_w, dock_h,
                           8, GUI_DOCK_BG);

    /* Subtle top border on dock */
    gui_hline(s, dock_x + 8, dock_y, dock_w - 16, 0x40FFFFFF);

    /* Get window state to show open/hidden indicator */
    int win_count = 0;
    gui_win_desc_t *wins = gui_desktop_get_windows(&win_count);

    /* Render each icon */
    for (int i = 0; i < (int)DOCK_ITEM_COUNT; i++) {
        int32_t ix = dock_x + GUI_DOCK_PADDING + i * item_slot;
        int32_t iy = dock_y + (dock_h - GUI_DOCK_ICON_SIZE) / 2;

        /* Dim icon if window is hidden (closed/minimized) */
        bool visible = (i < win_count && !wins[i].hidden);
        bool hovered = (cursor_x >= ix && cursor_x < ix + GUI_DOCK_ICON_SIZE &&
                        cursor_y >= iy && cursor_y < iy + GUI_DOCK_ICON_SIZE);
        uint32_t icon_color = hovered ? GUI_DOCK_HOVER
                            : (visible ? dock_items[i].color : 0xFF3A3A40);

        /* Icon square with rounded corners */
        gui_rounded_rect(s, ix, iy, GUI_DOCK_ICON_SIZE, GUI_DOCK_ICON_SIZE,
                         6, icon_color);

        /* Center the label character in the icon */
        char label[2] = { dock_items[i].label, 0 };
        uint32_t text_color = visible ? 0xFFFFFFFF : 0xFF888888;
        gui_draw_text_centered_aa(s, ix, iy + (GUI_DOCK_ICON_SIZE - GUI_FONT_H) / 2,
                                  GUI_DOCK_ICON_SIZE, label, text_color);

        /* Active indicator dot below icon (macOS-style) */
        if (visible) {
            int32_t dot_x = ix + GUI_DOCK_ICON_SIZE / 2;
            int32_t dot_y = iy + GUI_DOCK_ICON_SIZE + 4;
            gui_fill_circle_aa(s, dot_x, dot_y, 2, 0xFFFFFFFF);
        }
    }
}
