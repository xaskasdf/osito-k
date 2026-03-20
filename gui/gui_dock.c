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
    { GUI_MAXIMIZE,  'T' },   /* Terminal (green) */
    { GUI_ACCENT,    'F' },   /* Files (blue) */
    { 0xFF808080,    'S' },   /* Settings (gray) */
};

#define DOCK_ITEM_COUNT  (sizeof(dock_items) / sizeof(dock_items[0]))

/* ── Render ────────────────────────────────────────────────── */

void gui_dock_render(gui_surface_t *s, uint32_t screen_w, uint32_t screen_h)
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

    /* Render each icon */
    for (int i = 0; i < (int)DOCK_ITEM_COUNT; i++) {
        int32_t ix = dock_x + GUI_DOCK_PADDING + i * item_slot;
        int32_t iy = dock_y + (dock_h - GUI_DOCK_ICON_SIZE) / 2;

        /* Icon square with rounded corners */
        gui_rounded_rect(s, ix, iy, GUI_DOCK_ICON_SIZE, GUI_DOCK_ICON_SIZE,
                         6, dock_items[i].color);

        /* Center the label character in the icon */
        char label[2] = { dock_items[i].label, 0 };
        gui_draw_text_centered(s, ix, iy + (GUI_DOCK_ICON_SIZE - GUI_FONT_H) / 2,
                               GUI_DOCK_ICON_SIZE, label,
                               0xFFFFFFFF, 0);
    }
}
