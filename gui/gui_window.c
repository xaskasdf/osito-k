/*
 * OsitoK GUI — Window decorations (retina-quality, macOS-inspired)
 *
 * Phase 2 upgrades:
 *   - Multi-layer soft shadow (gui_box_shadow)
 *   - AA rounded rect titlebar with gradient
 *   - AA traffic-light buttons
 *   - Focused vs unfocused state
 */

#include "gui.h"

void gui_window_render(gui_surface_t *s, const gui_win_desc_t *win, bool focused)
{
    int32_t fx = win->x;
    int32_t fy = win->y;
    int32_t fw = win->w;
    int32_t fh = win->h + GUI_TITLEBAR_H;

    /* ── Multi-layer soft shadow (Phase 2.2) ──────────────── */
    int32_t blur    = focused ? 14 : 8;
    uint32_t sh_clr = focused ? 0x60000000 : 0x30000000;
    gui_box_shadow(s, fx, fy, fw, fh, 0, 4, blur, sh_clr);

    /* ── Window border: 1px AA rounded rect ───────────────── */
    gui_rounded_rect_aa(s, fx - 1, fy - 1, fw + 2, fh + 2,
                        GUI_CORNER_RADIUS + 1, GUI_BORDER);

    /* ── Title bar: gradient fill (Phase 2.7) ─────────────── */
    uint32_t tb_top = focused ? 0xFF404048 : 0xFF353538;
    uint32_t tb_bot = focused ? 0xFF303038 : 0xFF2A2A2E;

    /* Rounded top corners only: draw full rounded rect for top half,
     * then fill the bottom part square */
    gui_rounded_rect_aa(s, fx, fy, fw, GUI_TITLEBAR_H,
                        GUI_CORNER_RADIUS, tb_top);
    /* Overlay a gradient from top to bottom of titlebar */
    gui_gradient_v(s, fx, fy, fw, GUI_TITLEBAR_H, tb_top, tb_bot);
    /* Clip the gradient corners out by re-applying the rounded mask */
    /* (simpler: fill corners black then the gradient sits inside) */

    /* ── Content area ─────────────────────────────────────── */
    gui_fill_rect(s, fx, fy + GUI_TITLEBAR_H, fw, win->h, win->content_color);

    /* ── Bottom rounded corners (border mask) ─────────────── */
    gui_rounded_rect_aa(s, fx - 1, fy - 1, fw + 2, fh + 2,
                        GUI_CORNER_RADIUS + 1, 0x00000000);  /* noop (just redraw border) */

    /* ── Traffic-light buttons (Phase 2 AA) ───────────────── */
    int32_t btn_y = fy + GUI_TITLEBAR_H / 2;
    int32_t btn_x = fx + 14;

    uint32_t c_close = focused ? GUI_CLOSE    : GUI_BTN_DIM;
    uint32_t c_min   = focused ? GUI_MINIMIZE : GUI_BTN_DIM;
    uint32_t c_max   = focused ? GUI_MAXIMIZE : GUI_BTN_DIM;

    gui_fill_circle_aa(s, btn_x, btn_y, GUI_BTN_RADIUS, c_close | 0xFF000000);
    btn_x += GUI_BTN_RADIUS * 2 + 6;
    gui_fill_circle_aa(s, btn_x, btn_y, GUI_BTN_RADIUS, c_min | 0xFF000000);
    btn_x += GUI_BTN_RADIUS * 2 + 6;
    gui_fill_circle_aa(s, btn_x, btn_y, GUI_BTN_RADIUS, c_max | 0xFF000000);

    /* ── Title text (centered in titlebar) ────────────────── */
    if (win->title) {
        uint32_t txt_color = focused ? GUI_TITLEBAR_TEXT : GUI_TEXT_DIM;
        gui_draw_text_centered(s, fx, fy + (GUI_TITLEBAR_H - GUI_FONT_H) / 2,
                               fw, win->title, txt_color, 0);
    }
}
