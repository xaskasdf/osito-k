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

/* ── Desktop state ─────────────────────────────────────────── */

static uint32_t scr_w, scr_h;

/* Demo windows — positioned relative to screen size */
static gui_win_desc_t demo_windows[2];

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
}

/* ── Render complete desktop ───────────────────────────────── */

void gui_desktop_render(gui_surface_t *screen)
{
    /* 1. Gradient background */
    gui_gradient_v(screen, 0, 0, (int32_t)scr_w, (int32_t)scr_h,
                   GUI_BG_DARK, GUI_BG_LIGHT);

    /* 2. Top panel */
    gui_panel_render(screen, scr_w);

    /* 3. Demo windows (back to front) */
    gui_window_render(screen, &demo_windows[0]);

    /* Draw some placeholder text in the terminal window */
    int32_t tx = demo_windows[0].x + 8;
    int32_t ty = demo_windows[0].y + GUI_TITLEBAR_H + 8;
    gui_draw_text(screen, tx, ty,      "osito-k> ", GUI_MAXIMIZE, 0);
    gui_draw_text(screen, tx + gui_text_width("osito-k> "), ty,
                  "uname -a", GUI_PANEL_TEXT, 0);
    gui_draw_text(screen, tx, ty + 20, "OsitoK v1.0 aarch64/x86_64",
                  GUI_TEXT_DIM, 0);
    gui_draw_text(screen, tx, ty + 36, "osito-k> ", GUI_MAXIMIZE, 0);
    gui_draw_text(screen, tx + gui_text_width("osito-k> "), ty + 36,
                  "_", 0xFFFFFFFF, 0);

    gui_window_render(screen, &demo_windows[1]);

    /* Draw some info in the system info window */
    tx = demo_windows[1].x + 12;
    ty = demo_windows[1].y + GUI_TITLEBAR_H + 12;
    gui_draw_text(screen, tx, ty,      "Kernel:  OsitoK", GUI_PANEL_TEXT, 0);
    gui_draw_text(screen, tx, ty + 20, "Uptime:  00:00:42", GUI_PANEL_TEXT, 0);
    gui_draw_text(screen, tx, ty + 40, "Memory:  512 MB", GUI_PANEL_TEXT, 0);
    gui_draw_text(screen, tx, ty + 60, "Display: Retina", GUI_ACCENT, 0);

    /* 4. Bottom dock */
    gui_dock_render(screen, scr_w, scr_h);
}
