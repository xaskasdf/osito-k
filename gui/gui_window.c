/*
 * OsitoK GUI — Window decorations (elementaryOS/macOS style)
 *
 * Renders window frame with:
 *   - Drop shadow
 *   - Title bar with traffic-light buttons
 *   - Content area fill
 *   - 1px border
 */

#include "gui.h"

void gui_window_render(gui_surface_t *s, const gui_win_desc_t *win)
{
    int32_t total_h = win->h + GUI_TITLEBAR_H;
    int32_t fx = win->x;                    /* frame x */
    int32_t fy = win->y;                    /* frame y (includes titlebar) */
    int32_t fw = win->w;                    /* frame width */
    int32_t fh = total_h;                   /* frame height */

    /* ── Drop shadow ──────────────────────────────────────── */
    gui_fill_rect_alpha(s, fx + GUI_SHADOW_OFFSET, fy + GUI_SHADOW_OFFSET,
                        fw, fh, GUI_SHADOW);

    /* ── Window border (1px) ──────────────────────────────── */
    gui_rounded_rect(s, fx - 1, fy - 1, fw + 2, fh + 2,
                     GUI_CORNER_RADIUS + 1, GUI_BORDER);

    /* ── Title bar ────────────────────────────────────────── */
    gui_rounded_rect(s, fx, fy, fw, GUI_TITLEBAR_H,
                     GUI_CORNER_RADIUS, GUI_TITLEBAR);
    /* Fill bottom of titlebar (not rounded) to meet content area */
    gui_fill_rect(s, fx, fy + GUI_CORNER_RADIUS,
                  fw, GUI_TITLEBAR_H - GUI_CORNER_RADIUS, GUI_TITLEBAR);

    /* ── Traffic-light buttons (left-aligned) ─────────────── */
    int32_t btn_y = fy + GUI_TITLEBAR_H / 2;
    int32_t btn_x = fx + 14;

    gui_fill_circle(s, btn_x, btn_y, GUI_BTN_RADIUS, GUI_CLOSE);
    btn_x += GUI_BTN_RADIUS * 2 + 6;
    gui_fill_circle(s, btn_x, btn_y, GUI_BTN_RADIUS, GUI_MINIMIZE);
    btn_x += GUI_BTN_RADIUS * 2 + 6;
    gui_fill_circle(s, btn_x, btn_y, GUI_BTN_RADIUS, GUI_MAXIMIZE);

    /* ── Title text (centered in titlebar) ────────────────── */
    if (win->title) {
        gui_draw_text_centered(s, fx, fy + (GUI_TITLEBAR_H - GUI_FONT_H) / 2,
                               fw, win->title, GUI_TITLEBAR_TEXT, 0);
    }

    /* ── Content area ─────────────────────────────────────── */
    gui_fill_rect(s, fx, fy + GUI_TITLEBAR_H, fw, win->h, win->content_color);

    /* ── Bottom rounded corners for content ───────────────── */
    /* Mask bottom corners by redrawing border arcs */
    /* (The rounded_rect for border already handles this visually) */
}
