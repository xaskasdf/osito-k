/*
 * OsitoK GUI — Drawing primitives
 *
 * All functions clip to surface bounds. No floating point.
 */

#include "gui.h"

/* ── Helpers ───────────────────────────────────────────────── */

static inline int32_t gui_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t gui_min(int32_t a, int32_t b) { return a < b ? a : b; }

static inline uint32_t blend_pixel(uint32_t dst, uint32_t src)
{
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0xFF) return src;
    if (a == 0x00) return dst;

    uint32_t inv_a = 255 - a;
    uint32_t rb_src = src & 0x00FF00FF;
    uint32_t g_src  = src & 0x0000FF00;
    uint32_t rb_dst = dst & 0x00FF00FF;
    uint32_t g_dst  = dst & 0x0000FF00;

    uint32_t rb = ((rb_src * a + rb_dst * inv_a) >> 8) & 0x00FF00FF;
    uint32_t g  = ((g_src  * a + g_dst  * inv_a) >> 8) & 0x0000FF00;

    return 0xFF000000 | rb | g;
}

/* ── Solid rectangle ───────────────────────────────────────── */

void gui_fill_rect(gui_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t color)
{
    int32_t x0 = gui_max(x, 0);
    int32_t y0 = gui_max(y, 0);
    int32_t x1 = gui_min(x + w, (int32_t)s->width);
    int32_t y1 = gui_min(y + h, (int32_t)s->height);

    for (int32_t row = y0; row < y1; row++) {
        uint32_t *p = &s->pixels[row * s->pitch + x0];
        for (int32_t col = x0; col < x1; col++)
            *p++ = color;
    }
}

/* ── Alpha-blended rectangle ───────────────────────────────── */

void gui_fill_rect_alpha(gui_surface_t *s, int32_t x, int32_t y,
                         int32_t w, int32_t h, uint32_t color)
{
    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0xFF) { gui_fill_rect(s, x, y, w, h, color); return; }
    if (a == 0x00) return;

    int32_t x0 = gui_max(x, 0);
    int32_t y0 = gui_max(y, 0);
    int32_t x1 = gui_min(x + w, (int32_t)s->width);
    int32_t y1 = gui_min(y + h, (int32_t)s->height);

    for (int32_t row = y0; row < y1; row++) {
        uint32_t *p = &s->pixels[row * s->pitch + x0];
        for (int32_t col = x0; col < x1; col++, p++)
            *p = blend_pixel(*p, color);
    }
}

/* ── Lines ─────────────────────────────────────────────────── */

void gui_hline(gui_surface_t *s, int32_t x, int32_t y,
               int32_t w, uint32_t color)
{
    gui_fill_rect(s, x, y, w, 1, color);
}

void gui_vline(gui_surface_t *s, int32_t x, int32_t y,
               int32_t h, uint32_t color)
{
    gui_fill_rect(s, x, y, 1, h, color);
}

/* ── Vertical gradient ─────────────────────────────────────── */

void gui_gradient_v(gui_surface_t *s, int32_t x, int32_t y,
                    int32_t w, int32_t h,
                    uint32_t top_color, uint32_t bot_color)
{
    if (h <= 0) return;

    int32_t x0 = gui_max(x, 0);
    int32_t y0 = gui_max(y, 0);
    int32_t x1 = gui_min(x + w, (int32_t)s->width);
    int32_t y1 = gui_min(y + h, (int32_t)s->height);

    int32_t tr = (top_color >> 16) & 0xFF, tg = (top_color >> 8) & 0xFF, tb = top_color & 0xFF;
    int32_t br = (bot_color >> 16) & 0xFF, bg = (bot_color >> 8) & 0xFF, bb = bot_color & 0xFF;

    for (int32_t row = y0; row < y1; row++) {
        int32_t t = row - y;  /* position within gradient */
        uint32_t r = tr + (br - tr) * t / h;
        uint32_t g = tg + (bg - tg) * t / h;
        uint32_t b = tb + (bb - tb) * t / h;
        uint32_t c = 0xFF000000 | (r << 16) | (g << 8) | b;

        uint32_t *p = &s->pixels[row * s->pitch + x0];
        for (int32_t col = x0; col < x1; col++)
            *p++ = c;
    }
}

/* ── Rounded rectangle (solid) ─────────────────────────────── */

/* Corner mask: for radius r, skip[row] = how many pixels to mask */
static int32_t corner_skip(int32_t r, int32_t row)
{
    if (r <= 0 || row < 0 || row >= r) return 0;
    /* Precomputed for small radii using circle equation:
     * skip = r - sqrt(r*r - (r-1-row)^2), approximated with integer math */
    int32_t dy = r - 1 - row;
    int32_t dy2 = dy * dy;
    int32_t r2 = r * r;
    /* Integer sqrt approximation */
    int32_t sx = r;
    while (sx * sx > r2 - dy2 && sx > 0) sx--;
    return r - 1 - sx;
}

void gui_rounded_rect(gui_surface_t *s, int32_t x, int32_t y,
                      int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (r <= 0) { gui_fill_rect(s, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int32_t row = 0; row < h; row++) {
        int32_t skip = 0;
        if (row < r)
            skip = corner_skip(r, row);
        else if (row >= h - r)
            skip = corner_skip(r, h - 1 - row);

        gui_fill_rect(s, x + skip, y + row, w - 2 * skip, 1, color);
    }
}

/* ── Rounded rectangle (alpha) ─────────────────────────────── */

void gui_rounded_rect_alpha(gui_surface_t *s, int32_t x, int32_t y,
                            int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (r <= 0) { gui_fill_rect_alpha(s, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int32_t row = 0; row < h; row++) {
        int32_t skip = 0;
        if (row < r)
            skip = corner_skip(r, row);
        else if (row >= h - r)
            skip = corner_skip(r, h - 1 - row);

        gui_fill_rect_alpha(s, x + skip, y + row, w - 2 * skip, 1, color);
    }
}

/* ── Filled circle ─────────────────────────────────────────── */

void gui_fill_circle(gui_surface_t *s, int32_t cx, int32_t cy,
                     int32_t r, uint32_t color)
{
    for (int32_t dy = -r; dy <= r; dy++) {
        /* Integer horizontal span: dx where dx*dx + dy*dy <= r*r */
        int32_t dx = r;
        while (dx > 0 && dx * dx + dy * dy > r * r) dx--;
        gui_fill_rect(s, cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}
