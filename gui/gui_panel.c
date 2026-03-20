/*
 * OsitoK GUI — Top panel (elementaryOS Wingpanel style)
 *
 * Semi-transparent dark bar across the top of the screen.
 * Left: "OsitoK" branding in accent blue.
 * Right: Clock placeholder.
 */

#include "gui.h"

void gui_panel_render(gui_surface_t *s, uint32_t screen_w)
{
    /* Semi-transparent panel background */
    gui_fill_rect_alpha(s, 0, 0, (int32_t)screen_w, GUI_PANEL_HEIGHT,
                        GUI_PANEL_BG);

    /* Subtle bottom border */
    gui_hline(s, 0, GUI_PANEL_HEIGHT - 1, (int32_t)screen_w, GUI_BORDER);

    /* Left: "OsitoK" branding */
    gui_draw_text(s, 12, (GUI_PANEL_HEIGHT - GUI_FONT_H) / 2,
                  "OsitoK", GUI_ACCENT, 0);

    /* Right: clock placeholder */
    const char *clock = "12:00";
    int cw = gui_text_width(clock);
    gui_draw_text(s, (int32_t)screen_w - cw - 12,
                  (GUI_PANEL_HEIGHT - GUI_FONT_H) / 2,
                  clock, GUI_PANEL_TEXT, 0);
}
