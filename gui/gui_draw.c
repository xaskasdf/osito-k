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

/* ── Phase 2.1: AA primitives ─────────────────────────────── */

/* Integer sqrt with 8 fractional bits (result = floor(sqrt(n) * 256)) */
static int32_t isqrt_fp8(int32_t n)
{
    if (n <= 0) return 0;
    /* Compute isqrt(n << 16): multiply by 2^16 under the sqrt → result * 2^8 */
    uint32_t x = (uint32_t)n;
    uint32_t rem = x << 16;  /* n * 65536 */
    uint32_t root = 0;
    uint32_t bit = 1u << 23; /* start high bit: 2^23 → result up to 2^16 */
    while (bit > 0) {
        uint32_t test = root | bit;
        if (test * test <= rem)
            root = test;
        bit >>= 1;
    }
    return (int32_t)root;
}

/* Anti-aliased rounded rectangle (solid color, 1px smooth edge) */
void gui_rounded_rect_aa(gui_surface_t *s, int32_t x, int32_t y,
                         int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (r <= 0) { gui_fill_rect(s, x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    uint32_t src_a = (color >> 24) & 0xFF;
    if (src_a == 0) return;
    uint32_t rgb   = color & 0x00FFFFFF;
    int32_t r_fp8  = r << 8;  /* r in fixed-point */

    int32_t x0 = gui_max(x - 1, 0);
    int32_t y0 = gui_max(y - 1, 0);
    int32_t x1 = gui_min(x + w + 1, (int32_t)s->width);
    int32_t y1 = gui_min(y + h + 1, (int32_t)s->height);

    for (int32_t py = y0; py < y1; py++) {
        for (int32_t px = x0; px < x1; px++) {
            /* Map pixel to the nearest corner quadrant */
            int32_t qx = (px < x + r) ? (x + r - px) : (px > x + w - r) ? (px - (x + w - r)) : 0;
            int32_t qy = (py < y + r) ? (y + r - py) : (py > y + h - r) ? (py - (y + h - r)) : 0;

            int32_t coverage;
            if (qx <= 0 && qy <= 0) {
                /* Inside rectangle body — fully covered */
                coverage = 256;
            } else {
                /* In corner zone: compute distance to arc center */
                int32_t dist = isqrt_fp8(qx * qx + qy * qy);  /* in 8.8 */
                coverage = r_fp8 + 128 - dist;
                if (coverage < 0) coverage = 0;
                if (coverage > 256) coverage = 256;
            }

            if (coverage == 0) continue;
            uint32_t a = (src_a * coverage) >> 8;
            uint32_t *p = &s->pixels[py * s->pitch + px];
            *p = blend_pixel(*p, (a << 24) | rgb);
        }
    }
}

/* Anti-aliased filled circle */
void gui_fill_circle_aa(gui_surface_t *s, int32_t cx, int32_t cy,
                        int32_t r, uint32_t color)
{
    if (r <= 0) return;
    uint32_t src_a = (color >> 24) & 0xFF;
    uint32_t rgb   = color & 0x00FFFFFF;
    int32_t r_fp8  = r << 8;

    int32_t x0 = gui_max(cx - r - 1, 0);
    int32_t y0 = gui_max(cy - r - 1, 0);
    int32_t x1 = gui_min(cx + r + 2, (int32_t)s->width);
    int32_t y1 = gui_min(cy + r + 2, (int32_t)s->height);

    for (int32_t py = y0; py < y1; py++) {
        for (int32_t px = x0; px < x1; px++) {
            int32_t dx = px - cx;
            int32_t dy = py - cy;
            int32_t dist = isqrt_fp8(dx * dx + dy * dy);
            int32_t coverage = r_fp8 + 128 - dist;
            if (coverage <= 0) continue;
            if (coverage > 256) coverage = 256;
            uint32_t a = (src_a * coverage) >> 8;
            uint32_t *p = &s->pixels[py * s->pitch + px];
            *p = blend_pixel(*p, (a << 24) | rgb);
        }
    }
}

/* ── Phase 2.2: Multi-layer soft shadow ────────────────────── */

void gui_box_shadow(gui_surface_t *s, int32_t x, int32_t y,
                    int32_t w, int32_t h, int32_t offset_x, int32_t offset_y,
                    int32_t blur, uint32_t color)
{
    if (blur <= 0) {
        gui_rounded_rect_alpha(s, x + offset_x, y + offset_y, w, h, 4, color);
        return;
    }
    /* Single-layer fast shadow: one expanded rounded rect.
     * ~15x faster than the old multi-layer loop (was 15 passes → 1). */
    int32_t expand = blur / 2;
    int32_t er = GUI_CORNER_RADIUS + expand;
    gui_rounded_rect_alpha(s, x + offset_x - expand, y + offset_y - expand,
                           w + 2 * expand, h + 2 * expand, er, color);
}

/* ── Phase 2.7: Horizontal gradient ───────────────────────── */

void gui_gradient_h(gui_surface_t *s, int32_t x, int32_t y,
                    int32_t w, int32_t h,
                    uint32_t left_color, uint32_t right_color)
{
    if (w <= 0) return;

    int32_t x0 = gui_max(x, 0);
    int32_t y0 = gui_max(y, 0);
    int32_t x1 = gui_min(x + w, (int32_t)s->width);
    int32_t y1 = gui_min(y + h, (int32_t)s->height);

    int32_t lr = (left_color  >> 16) & 0xFF, lg = (left_color  >> 8) & 0xFF, lb = left_color  & 0xFF;
    int32_t rr = (right_color >> 16) & 0xFF, rg = (right_color >> 8) & 0xFF, rb = right_color & 0xFF;

    for (int32_t col = x0; col < x1; col++) {
        int32_t t = col - x;
        uint32_t r = (uint32_t)(lr + (rr - lr) * t / w);
        uint32_t g = (uint32_t)(lg + (rg - lg) * t / w);
        uint32_t b = (uint32_t)(lb + (rb - lb) * t / w);
        uint32_t c = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int32_t row = y0; row < y1; row++)
            s->pixels[row * s->pitch + col] = c;
    }
}
