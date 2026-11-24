/*
 * OsitoK GUI — Top panel (elementaryOS Wingpanel style)
 *
 * Semi-transparent dark bar across the top of the screen.
 * Left: "OsitoK" branding in accent blue.
 * Center-right: Debug stats (FPS, memory, ticks, processes).
 * Right: Clock placeholder.
 */

#include "gui.h"

/* ── Debug info (updated by kernel each frame) ─────────────── */

static gui_debug_info_t dbg;

void gui_panel_set_debug(const gui_debug_info_t *info)
{
    dbg = *info;
}

const gui_debug_info_t *gui_panel_get_debug(void)
{
    return &dbg;
}

/* ── Simple uint64 → decimal string ────────────────────────── */

static int itoa_simple(uint64_t val, char *buf)
{
    char tmp[20];
    int n = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (val) { tmp[n++] = '0' + (int)(val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

/* ── Append helpers for building stat string ───────────────── */

static int scat(char *dst, int pos, const char *src)
{
    while (*src) dst[pos++] = *src++;
    return pos;
}

static int scat_num(char *dst, int pos, uint64_t val)
{
    char tmp[20];
    itoa_simple(val, tmp);
    return scat(dst, pos, tmp);
}

/* ── Render ────────────────────────────────────────────────── */

void gui_panel_render(gui_surface_t *s, uint32_t screen_w)
{
    int height = gui_panel_height();
    if (!height) return;
    gui_glass_panel(s, (gui_rect_t){0, 0, (int)screen_w, height}, 0, false);
    int pad = gui_px(14);
    gui_rect_t brand = { pad, 0, gui_px(80), height };
    gui_ui_text_fit(s, brand, "OsitoK", GUI_INK, false);
    char clock[12];
    int ci = 0;
    if (dbg.rtc_hour < 10) clock[ci++] = '0';
    ci = scat_num(clock, ci, dbg.rtc_hour); clock[ci++] = ':';
    if (dbg.rtc_min < 10) clock[ci++] = '0';
    ci = scat_num(clock, ci, dbg.rtc_min);
    clock[ci] = 0;
    int cw = gui_ui_text_width(clock);
    gui_rect_t cr = { (int)screen_w - cw - pad, 0, cw, height };
    gui_ui_text_fit(s, cr, clock, GUI_INK, false);
    if (dbg.fps || dbg.mem_used_mb || dbg.ticks) {
        char line[96];
        int p = scat_num(line, 0, dbg.mem_used_mb);
        p = scat(line, p, " MB used    ");
        p = scat_num(line, p, dbg.fps);
        p = scat(line, p, " fps");
        line[p] = 0;
        int width = gui_ui_text_width(line);
        int left = cr.x - gui_px(24) - width;
        if (left > brand.x + brand.w + gui_px(20))
            gui_ui_text_fit(s, (gui_rect_t){left, 0, width, height}, line, GUI_MUTED, false);
    }
}
