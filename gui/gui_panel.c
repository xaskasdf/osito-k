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
    int32_t text_y = (GUI_PANEL_HEIGHT - GUI_FONT_H) / 2;

    /* Semi-transparent panel background */
    gui_fill_rect_alpha(s, 0, 0, (int32_t)screen_w, GUI_PANEL_HEIGHT,
                        GUI_PANEL_BG);

    /* Subtle bottom border */
    gui_hline(s, 0, GUI_PANEL_HEIGHT - 1, (int32_t)screen_w, GUI_BORDER);

    /* Left: "OsitoK" branding */
    gui_draw_text(s, 12, text_y, "OsitoK", GUI_ACCENT, 0);

    /* Right: RTC clock (HH:MM:SS from CMOS RTC) */
    char clock[12];
    {
        int ci = 0;
        if (dbg.rtc_hour < 10) clock[ci++] = '0';
        ci = scat_num(clock, ci, dbg.rtc_hour); clock[ci++] = ':';
        if (dbg.rtc_min < 10) clock[ci++] = '0';
        ci = scat_num(clock, ci, dbg.rtc_min); clock[ci++] = ':';
        if (dbg.rtc_sec < 10) clock[ci++] = '0';
        ci = scat_num(clock, ci, dbg.rtc_sec);
        clock[ci] = '\0';
    }
    int cw = gui_text_width(clock);
    gui_draw_text(s, (int32_t)screen_w - cw - 12, text_y,
                  clock, GUI_PANEL_TEXT, 0);

    /* Center-right: debug stats (to the left of clock) */
    if (dbg.fps || dbg.mem_used_mb || dbg.ticks) {
        char line[96];
        int p = 0;
        p = scat(line, p, "FPS:");
        p = scat_num(line, p, dbg.fps);
        p = scat(line, p, "  MEM:");
        p = scat_num(line, p, dbg.mem_used_mb);
        p = scat(line, p, "/");
        p = scat_num(line, p, dbg.mem_used_mb + dbg.mem_free_mb);
        p = scat(line, p, "MB  P:");
        p = scat_num(line, p, dbg.procs);
        line[p] = '\0';

        int tw = gui_text_width(line);
        int32_t x = (int32_t)screen_w - cw - 12 - tw - 20;
        gui_draw_text(s, x, text_y, line, GUI_TEXT_DIM, 0);
    }
}
