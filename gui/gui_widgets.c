/* Software painters. Integer arithmetic and bounded reusable glass scratch.
 * Like the desktop background cache, these painters run on the GUI thread. */
#include "gui.h"

static uint32_t ui_mix(uint32_t a, uint32_t b, unsigned t)
{
    if (t > 255) t = 255;
    unsigned inv = 255 - t;
    unsigned r = (((a >> 16) & 255) * inv + ((b >> 16) & 255) * t + 127) / 255;
    unsigned g = (((a >> 8) & 255) * inv + ((b >> 8) & 255) * t + 127) / 255;
    unsigned blue = ((a & 255) * inv + (b & 255) * t + 127) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | blue;
}

void gui_soft_shadow(gui_surface_t *s, gui_rect_t r, int blur, int offset_y, unsigned opacity)
{
    if (r.w <= 0 || r.h <= 0 || blur <= 0) return;
    if (blur > 96) blur = 96;
    if (opacity > 255) opacity = 255;
    int y0 = r.y + offset_y - blur, y1 = r.y + r.h + offset_y + blur;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)s->height) y1 = (int)s->height;
    unsigned radius2 = (unsigned)(blur * blur);
    for (int y = y0; y < y1; y++) {
        bool alongside = y >= r.y && y < r.y + r.h;
        /* Iterate only the two outside strips alongside the opaque frame. */
        for (int strip = 0; strip < (alongside ? 2 : 1); strip++) {
            int x0 = strip ? r.x + r.w : r.x - blur;
            int x1 = alongside && !strip ? r.x : r.x + r.w + blur;
            if (x0 < 0) x0 = 0;
            if (x1 > (int)s->width) x1 = (int)s->width;
            int dy = y < r.y + offset_y ? r.y + offset_y - y :
                     y >= r.y + r.h + offset_y ? y - (r.y + r.h + offset_y - 1) : 0;
            for (int x = x0; x < x1; x++) {
                int dx = x < r.x ? r.x - x : x >= r.x + r.w ? x - (r.x + r.w - 1) : 0;
                unsigned distance2 = (unsigned)(dx * dx + dy * dy);
                if (distance2 >= radius2) continue;
                unsigned falloff = (radius2 - distance2) * 255 / radius2;
                unsigned alpha = opacity * falloff * falloff / 65025;
                uint32_t *pixel = &s->pixels[(uint32_t)y * s->pitch + x];
                *pixel = ui_mix(*pixel, 0xFF000000, alpha);
            }
        }
    }
}

static int ui_round_inset(int radius, int row, int height)
{
    int y = row < radius ? radius - 1 - row :
            row >= height - radius ? row - (height - radius) : 0;
    int x = radius;
    while (x > 0 && x * x + y * y > radius * radius) x--;
    return radius - x;
}

/* Snapshot before painting: glass must sample the layers behind it, never its
 * own freshly painted pixels. A screen-aligned grid prevents shimmer on drag.
 * Two separable box passes approximate a soft Gaussian at a fraction of the
 * native pixel count. 256 KiB is reused by every glass rectangle; no allocation
 * or framebuffer-sized blur is required. Huge rectangles increase sample step. */
#define UI_GLASS_SAMPLES 32768
static uint32_t glass_scratch[2][UI_GLASS_SAMPLES];
typedef struct { int x, y, w, h, step; } ui_glass_grid_t;

static void ui_glass_blur_axis(uint32_t *dst, const uint32_t *src,
                                int width, int height, bool vertical)
{
    int lines = vertical ? width : height;
    int length = vertical ? height : width;
    int stride = vertical ? width : 1;
    for (int line = 0; line < lines; line++) {
        int base = vertical ? line : line * width;
        unsigned red = 0, green = 0, blue = 0;
        for (int tap = -2; tap <= 2; tap++) {
            int pos = tap < 0 ? 0 : tap >= length ? length - 1 : tap;
            uint32_t pixel = src[base + pos * stride];
            red += (pixel >> 16) & 255;
            green += (pixel >> 8) & 255;
            blue += pixel & 255;
        }
        for (int pos = 0; pos < length; pos++) {
            dst[base + pos * stride] = 0xFF000000 |
                (((red + 2) / 5) << 16) | (((green + 2) / 5) << 8) | ((blue + 2) / 5);
            int previous = pos - 2 < 0 ? 0 : pos - 2;
            int next = pos + 3 >= length ? length - 1 : pos + 3;
            uint32_t leaving = src[base + previous * stride];
            uint32_t entering = src[base + next * stride];
            red = red - ((leaving >> 16) & 255) + ((entering >> 16) & 255);
            green = green - ((leaving >> 8) & 255) + ((entering >> 8) & 255);
            blue = blue - (leaving & 255) + (entering & 255);
        }
    }
}

static ui_glass_grid_t ui_glass_backdrop(const gui_surface_t *s,
                                          int x0, int y0, int x1, int y1)
{
    ui_glass_grid_t grid = { .step = gui_px(3) };
    if (grid.step < 1) grid.step = 1;
    for (;;) {
        grid.x = (x0 / grid.step - 5) * grid.step;
        grid.y = (y0 / grid.step - 5) * grid.step;
        grid.w = (x1 - 1) / grid.step - x0 / grid.step + 12;
        grid.h = (y1 - 1) / grid.step - y0 / grid.step + 12;
        if ((uint64_t)grid.w * grid.h <= UI_GLASS_SAMPLES) break;
        grid.step *= 2;
    }
    int offset = (grid.step + 2) / 4;
    for (int y = 0; y < grid.h; y++) {
        for (int x = 0; x < grid.w; x++) {
            unsigned red = 0, green = 0, blue = 0;
            for (int sample = 0; sample < 4; sample++) {
                int sx = grid.x + x * grid.step + (sample & 1 ? offset : -offset);
                int sy = grid.y + y * grid.step + (sample & 2 ? offset : -offset);
                if (sx < 0) sx = 0;
                if (sy < 0) sy = 0;
                if (sx >= (int)s->width) sx = (int)s->width - 1;
                if (sy >= (int)s->height) sy = (int)s->height - 1;
                uint32_t pixel = s->pixels[(uint32_t)sy * s->pitch + sx];
                red += (pixel >> 16) & 255;
                green += (pixel >> 8) & 255;
                blue += pixel & 255;
            }
            glass_scratch[0][y * grid.w + x] = 0xFF000000 |
                (((red + 2) / 4) << 16) | (((green + 2) / 4) << 8) | ((blue + 2) / 4);
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        ui_glass_blur_axis(glass_scratch[1], glass_scratch[0], grid.w, grid.h, false);
        ui_glass_blur_axis(glass_scratch[0], glass_scratch[1], grid.w, grid.h, true);
    }
    /* Dark glass must remain readable even over a white client surface.
     * Preserve hue while limiting the luminance of the sampled backdrop. */
    for (int i = 0; i < grid.w * grid.h; i++) {
        uint32_t color = glass_scratch[0][i];
        unsigned luminance = (((color >> 16) & 255) * 54 +
                              ((color >> 8) & 255) * 183 + (color & 255) * 19) >> 8;
        if (luminance > 128)
            glass_scratch[0][i] = ui_mix(color, 0xFF000000, (luminance - 128) * 255 / luminance);
    }
    return grid;
}

static void ui_glass_row(const ui_glass_grid_t *grid, int y)
{
    int v = y - grid->y, gy = v / grid->step;
    unsigned ty = (unsigned)(v % grid->step) * 255 / grid->step;
    const uint32_t *pixel = glass_scratch[0] + gy * grid->w;
    /* The second blur buffer is free now. Interpolate each sample column once
     * per output row, rather than doing both axes for every physical pixel. */
    for (int x = 0; x < grid->w; x++)
        glass_scratch[1][x] = ui_mix(pixel[x], pixel[x + grid->w], ty);
}

void gui_glass_panel(gui_surface_t *s, gui_rect_t r, int radius, bool active)
{
    if (!s || !s->pixels || !s->width || !s->height || s->pitch < s->width ||
        s->width > 16384 || s->height > 16384 || r.w <= 0 || r.h <= 0) return;
    int64_t right_edge = (int64_t)r.x + r.w, bottom_edge = (int64_t)r.y + r.h;
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = right_edge > (int)s->width ? (int)s->width : (int)right_edge;
    int y1 = bottom_edge > (int)s->height ? (int)s->height : (int)bottom_edge;
    if (x0 >= x1 || y0 >= y1) return;
    if (radius < 0) radius = 0;
    if (radius > 96) radius = 96;
    if (radius > r.w / 2) radius = r.w / 2;
    if (radius > r.h / 2) radius = r.h / 2;
    bool glass = gui_theme_get()->glass;
    ui_glass_grid_t grid = {0};
    if (glass) grid = ui_glass_backdrop(s, x0, y0, x1, y1);
    unsigned weight_step = glass ? 255u * 65536 / grid.step : 0;
    int reflection_period = gui_px(640), reflection_width = gui_px(120);
    for (int y = y0; y < y1; y++) {
        int row = y - r.y, inset = ui_round_inset(radius, row, r.h);
        int64_t left = (int64_t)r.x + inset, right = right_edge - inset;
        int start = left > x0 ? (int)left : x0;
        int end = right < x1 ? (int)right : x1;
        unsigned t = r.h > 1 ? (unsigned)((uint64_t)row * 255 / (r.h - 1)) : 0;
        uint32_t tint = ui_mix(active ? 0xFF294A61 : 0xFF2A3B49,
                               active ? 0xFF0C2536 : 0xFF142532, t);
        unsigned gloss = t < 116 ? (116 - t) * (active ? 52 : 30) / 116 : 0;
        int sample = 0, fraction = 0, phase = 0;
        if (glass) {
            ui_glass_row(&grid, y);
            sample = (start - grid.x) / grid.step;
            fraction = (start - grid.x) % grid.step;
            phase = (int)(((int64_t)start - r.x + (int64_t)row * 2) % reflection_period);
        }
        for (int x = start; x < end; x++) {
            uint32_t *p = &s->pixels[(uint32_t)y * s->pitch + x];
            uint32_t color = tint;
            if (glass) {
                color = ui_mix(glass_scratch[1][sample], glass_scratch[1][sample + 1],
                               (unsigned)fraction * weight_step >> 16);
                color = ui_mix(color, tint, active ? 112 : 152);
                if (++fraction == grid.step) { fraction = 0; sample++; }
            }
            color = ui_mix(color, 0xFFE0F5FF, gloss);
            if (glass) {
                int distance = phase - reflection_period / 3;
                if (distance < 0) distance = -distance;
                if (distance < reflection_width)
                    color = ui_mix(color, 0xFFD2EDFF,
                        (unsigned)(reflection_width - distance) * (active ? 22 : 12) / reflection_width);
                if (++phase == reflection_period) phase = 0;
            }
            /* Bright outer lip and dark inner edge give the glass thickness. */
            if (row == 0 || x == left || x == right - 1)
                color = ui_mix(color, active ? 0xFFD5F0FF : 0xFF9DB7C7, row == 0 ? 140 : 85);
            if (row == 1 && x > left && x < right - 1)
                color = ui_mix(color, 0xFFD5F0FF, active ? 35 : 18);
            if (row == r.h - 1) color = ui_mix(color, 0xFF030C15, 144);
            *p = color;
        }
    }
}

void gui_outline(gui_surface_t *s, gui_rect_t r, uint32_t color)
{
    if (r.w <= 0 || r.h <= 0) return;
    gui_hline(s, r.x, r.y, r.w, color);
    gui_hline(s, r.x, r.y + r.h - 1, r.w, color);
    gui_vline(s, r.x, r.y, r.h, color);
    gui_vline(s, r.x + r.w - 1, r.y, r.h, color);
}

void gui_control(gui_surface_t *s, gui_rect_t r, const char *label, unsigned state)
{
    uint32_t top = state & GUI_CONTROL_SELECTED ? 0xFF366787 : 0xFF344858;
    uint32_t bottom = state & GUI_CONTROL_SELECTED ? 0xFF244962 : 0xFF243442;
    if (state & GUI_CONTROL_HOVER) { top = ui_mix(top, GUI_FOCUS, 40); bottom = ui_mix(bottom, GUI_FOCUS, 25); }
    if (state & GUI_CONTROL_DISABLED) { top = 0xFF25313B; bottom = 0xFF202A33; }
    gui_rounded_rect(s, r.x, r.y, r.w, r.h, gui_px(4), GUI_EDGE);
    if (r.w > 2 && r.h > 2) {
        /* Insets keep the gradient inside the rounded boundary. */
        gui_gradient_v(s, r.x + 2, r.y + 1, r.w - 4, r.h - 2, top, bottom);
        gui_hline(s, r.x + gui_px(4), r.y + 1, r.w - gui_px(8),
                   state & GUI_CONTROL_SELECTED ? 0xFF6B9EBB : 0xFF60788B);
    }
    if (state & GUI_CONTROL_FOCUS) {
        gui_rect_t focus = { r.x - 2, r.y - 2, r.w + 4, r.h + 4 };
        gui_outline(s, focus, GUI_FOCUS);
    }
    r.x += gui_px(10); r.w -= gui_px(20);
    gui_ui_text_fit(s, r, label, state & GUI_CONTROL_DISABLED ? GUI_MUTED : GUI_INK, false);
}

void gui_check(gui_surface_t *s, gui_rect_t r, const char *label,
               bool checked, unsigned state)
{
    int size = gui_px(18);
    gui_rect_t box = { r.x, r.y + (r.h - size) / 2, size, size };
    gui_control(s, box, "", state | (checked ? GUI_CONTROL_SELECTED : 0));
    if (checked) {
        gui_rect_t icon = { box.x + gui_px(2), box.y + gui_px(2), box.w - gui_px(4), box.h - gui_px(4) };
        gui_ui_icon(s, icon, GUI_ICON_CHECK, GUI_INK);
    }
    r.x += size + gui_px(10); r.w -= size + gui_px(10);
    gui_ui_text_fit(s, r, label, GUI_INK, false);
}

void gui_selector(gui_surface_t *s, gui_rect_t r, const char *label, unsigned state)
{
    if (r.w <= 0 || r.h <= 0) return;
    gui_control(s, r, "", state);
    int button = gui_px(30), pad = gui_px(10);
    if (r.w > button + pad * 2 && r.h >= gui_px(20)) {
        int separator = r.x + r.w - button;
        gui_fill_rect_alpha(s, separator + 1, r.y + 2, button - 3, r.h - 4,
                             state & GUI_CONTROL_SELECTED ? 0x225BB7E8 : 0x164F7087);
        gui_vline(s, separator, r.y + gui_px(5), r.h - gui_px(10), 0xFF1C2D3B);
        gui_vline(s, separator + 1, r.y + gui_px(5), r.h - gui_px(10), 0xFF506B80);
        int size = gui_px(14);
        gui_rect_t arrow = {separator + (button - size) / 2,
                            r.y + (r.h - size) / 2, size, size};
        gui_ui_icon(s, arrow, state & GUI_CONTROL_SELECTED ? GUI_ICON_CHEVRON_UP : GUI_ICON_CHEVRON,
                     state & GUI_CONTROL_DISABLED ? GUI_MUTED : GUI_INK);
        r.w -= button;
    }
    r.x += pad; r.w -= pad * 2;
    gui_ui_text_fit(s, r, label, state & GUI_CONTROL_DISABLED ? GUI_MUTED : GUI_INK, false);
}

gui_popup_layout_t gui_popup_geometry(gui_rect_t anchor, gui_rect_t limits,
                                      int total, int first, int hot)
{
    gui_popup_layout_t p = {0};
    int pad = gui_px(3), margin = gui_px(4), row = gui_px(26);
    if (total <= 0 || anchor.w <= 0 || anchor.h <= 0 || limits.x < 0 || limits.y < 0 ||
        limits.w > 16384 || limits.h > 16384 || limits.x > 16384 || limits.y > 16384 ||
        limits.w < gui_px(64) || limits.h < row + 2 * (pad + margin)) return p;
    int left = limits.x + margin, top = limits.y + margin;
    int right = limits.x + limits.w - margin, bottom = limits.y + limits.h - margin;
    p.row_height = row; p.total = total;
    int64_t below = (int64_t)anchor.y + anchor.h + pad;
    int64_t above = (int64_t)anchor.y - pad;
    if (below < top) below = top;
    if (below > bottom) below = bottom;
    if (above < top) above = top;
    if (above > bottom) above = bottom;
    int below_count = (int)(bottom - below - pad * 2) / row;
    int above_count = (int)(above - top - pad * 2) / row;
    if (below_count < 0) below_count = 0;
    if (above_count < 0) above_count = 0;
    bool upward = below_count < total && above_count > below_count;
    p.count = upward ? above_count : below_count;
    /* Only cover the anchor if neither side can hold even one row. */
    bool overlap = p.count == 0;
    if (overlap) p.count = (bottom - top - pad * 2) / row;
    if (p.count > total) p.count = total;
    if (first < 0) first = 0;
    if (first > total - p.count) first = total - p.count;
    if (hot >= 0 && hot < total) {
        if (hot < first) first = hot;
        if (hot >= first + p.count) first = hot - p.count + 1;
    }
    p.first = first;
    p.bounds.w = anchor.w < right - left ? anchor.w : right - left;
    if (p.bounds.w < gui_px(96))
        p.bounds.w = gui_px(96) < right - left ? gui_px(96) : right - left;
    p.bounds.h = p.count * row + pad * 2;
    p.bounds.x = anchor.x;
    if (p.bounds.x > right - p.bounds.w) p.bounds.x = right - p.bounds.w;
    if (p.bounds.x < left) p.bounds.x = left;
    int64_t y = overlap ? bottom - p.bounds.h : upward ? above - p.bounds.h : below;
    if (y < top) y = top;
    if (y > bottom - p.bounds.h) y = bottom - p.bounds.h;
    p.bounds.y = (int)y;
    p.items = (gui_rect_t){p.bounds.x + pad, p.bounds.y + pad,
                          p.bounds.w - pad * 2, p.count * row};
    if (p.count < total) {
        int rail = gui_px(18);
        p.items.w -= rail + pad;
        int height = gui_px(18);
        if (height * 2 > p.items.h) height = p.items.h / 2;
        p.previous = (gui_rect_t){p.items.x + p.items.w + pad, p.items.y, rail, height};
        p.next = p.previous; p.next.y = p.items.y + p.items.h - height;
    }
    return p;
}

gui_rect_t gui_popup_item_rect(const gui_popup_layout_t *p, int index)
{
    if (!p || index < p->first || index - p->first >= p->count) return (gui_rect_t){0};
    return (gui_rect_t){p->items.x, p->items.y + (index - p->first) * p->row_height,
                        p->items.w, p->row_height};
}

int gui_popup_hit(const gui_popup_layout_t *p, int32_t x, int32_t y)
{
    if (!p || !gui_rect_contains(p->bounds, x, y)) return GUI_POPUP_OUTSIDE;
    if (gui_rect_contains(p->items, x, y))
        return p->first + (y - p->items.y) / p->row_height;
    if (p->first > 0 && gui_rect_contains(p->previous, x, y)) return GUI_POPUP_PREVIOUS;
    if (p->first + p->count < p->total && gui_rect_contains(p->next, x, y)) return GUI_POPUP_NEXT;
    return GUI_POPUP_CHROME;
}

void gui_popup_render(gui_surface_t *s, const gui_popup_layout_t *p,
                       const char *const *labels, int selected, int hot,
                       bool keyboard, int32_t cursor_x, int32_t cursor_y)
{
    if (!p || !p->count || !labels) return;
    gui_rect_t r = p->bounds;
    gui_soft_shadow(s, r, gui_px(12), gui_px(4), 96);
    gui_rounded_rect(s, r.x, r.y, r.w, r.h, gui_px(4), GUI_EDGE);
    gui_gradient_v(s, r.x + 2, r.y + 2, r.w - 4, r.h - 4, 0xFF293D4E, 0xFF1B2B39);
    gui_hline(s, r.x + gui_px(4), r.y + 1, r.w - gui_px(8), 0xFF7994A8);
    gui_hline(s, r.x + gui_px(3), r.y + r.h - 1, r.w - gui_px(6), 0xFF0C1823);
    int gutter = gui_px(27);
    gui_vline(s, p->items.x + gutter, p->items.y + gui_px(3), p->items.h - gui_px(6), 0xFF3C5264);
    int hover = gui_popup_hit(p, cursor_x, cursor_y);
    int active = keyboard ? hot : hover;
    for (int i = p->first; i < p->first + p->count; i++) {
        gui_rect_t item = gui_popup_item_rect(p, i);
        if (i == active) {
            gui_rect_t highlight = item;
            highlight.y += gui_px(1); highlight.h -= gui_px(2);
            gui_control(s, highlight, "", GUI_CONTROL_SELECTED);
        }
        if (i == selected) {
            int size = gui_px(14);
            gui_rect_t check = {item.x + (gutter - size) / 2, item.y + (item.h - size) / 2, size, size};
            gui_ui_icon(s, check, GUI_ICON_CHECK, GUI_FOCUS);
        }
        item.x += gutter + gui_px(8); item.w -= gutter + gui_px(16);
        gui_ui_text_fit(s, item, labels[i], GUI_INK, false);
    }
    if (p->count < p->total) {
        gui_vline(s, p->previous.x - gui_px(2), p->items.y, p->items.h, 0xFF405769);
        const gui_rect_t arrows[2] = {p->previous, p->next};
        for (int i = 0; i < 2; i++) {
            bool enabled = i ? p->first + p->count < p->total : p->first > 0;
            bool hovered = enabled && !keyboard && gui_rect_contains(arrows[i], cursor_x, cursor_y);
            if (hovered) gui_control(s, arrows[i], "", GUI_CONTROL_HOVER);
            gui_ui_icon(s, arrows[i], i ? GUI_ICON_CHEVRON : GUI_ICON_CHEVRON_UP,
                         enabled ? GUI_INK : 0xFF657A8A);
        }
    }
}

/* Coordinates are eighths of a design pixel on a 16 x 16 icon grid. */
static bool ui_icon_sample(int u, int v, int icon)
{
    if (icon == GUI_ICON_CHEVRON_UP) { icon = GUI_ICON_CHEVRON; v = 128 - v; }
    int dx = u - 64, dy = v - 64, d2 = dx * dx + dy * dy;
    switch (icon) {
    case GUI_ICON_TERMINAL:
        return ((v >= 16 && v < 24) || (v >= 104 && v < 112)) ? u >= 8 && u < 120 :
               ((u >= 8 && u < 16) || (u >= 112 && u < 120)) && v >= 16 && v < 112 ? true :
               u >= 32 && u < 64 && ((v - u >= 12 && v - u < 20) || (v + u >= 108 && v + u < 116)) ? true :
               v >= 80 && v < 88 && u >= 72 && u < 96;
    case GUI_ICON_SETTINGS:
        return (d2 >= 1100 && d2 <= 2400) ||
               (u >= 56 && u < 76 && (v < 27 || v >= 103)) ||
               (v >= 56 && v < 76 && (u < 27 || u >= 103)) ||
               (((u - v > -7 && u - v < 7) || (u + v > 121 && u + v < 135)) && d2 >= 1950 && d2 <= 4250);
    case GUI_ICON_CLOSE:
        return u >= 24 && u < 104 && v >= 24 && v < 104 &&
               ((u - v > -7 && u - v < 7) || (u + v > 120 && u + v < 136));
    case GUI_ICON_MINIMIZE:
        return u >= 24 && u < 104 && v >= 88 && v < 100;
    case GUI_ICON_MAXIMIZE:
        return ((v >= 24 && v < 36) || (v >= 96 && v < 104)) ? u >= 24 && u < 104 :
               ((u >= 24 && u < 32) || (u >= 96 && u < 104)) && v >= 24 && v < 104;
    case GUI_ICON_CHEVRON:
        return u >= 24 && u < 104 && v >= 40 && v < 86 &&
               ((u < 64 && v - u >= 12 && v - u < 22) ||
                (u >= 64 && v + u >= 140 && v + u < 150));
    case GUI_ICON_PREVIOUS:
        return v >= 24 && v < 104 && ((v < 64 && u + v >= 92 && u + v < 102) ||
                                     (v >= 64 && u - v >= -36 && u - v < -26));
    case GUI_ICON_NEXT:
        return v >= 24 && v < 104 && ((v < 64 && u - v >= 28 && u - v < 38) ||
                                     (v >= 64 && u + v >= 156 && u + v < 166));
    case GUI_ICON_CHECK:
        return (u >= 12 && u < 51 && v - u > 36 && v - u < 54) ||
               (u >= 44 && u < 116 && v + u > 130 && v + u < 148);
    default: return false;
    }
}

void gui_ui_icon(gui_surface_t *s, gui_rect_t r, int icon, uint32_t color)
{
    if (r.w <= 0 || r.h <= 0) return;
    for (int y = 0; y < r.h; y++) {
        int py = r.y + y;
        if (py < 0 || py >= (int)s->height) continue;
        for (int x = 0; x < r.w; x++) {
            int px = r.x + x;
            if (px < 0 || px >= (int)s->width) continue;
            unsigned coverage = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    coverage += ui_icon_sample(((x * 8 + sx * 2 + 1) * 16) / r.w,
                                               ((y * 8 + sy * 2 + 1) * 16) / r.h, icon);
            if (coverage) {
                uint32_t *p = &s->pixels[(uint32_t)py * s->pitch + px];
                *p = ui_mix(*p, color, coverage * 255 / 16);
            }
        }
    }
}

void gui_wallpaper_render(gui_surface_t *s)
{
    if (!s->width || !s->height) return;
    for (uint32_t y = 0; y < s->height; y++) {
        int v = (int)((uint64_t)y * 1024 / s->height);
        uint32_t base = ui_mix(GUI_BG_DARK, GUI_BG_LIGHT, (unsigned)v / 4);
        for (uint32_t x = 0; x < s->width; x++) {
            int u = (int)((uint64_t)x * 1024 / s->width);
            int dx = u - 720, dy = v - 480;
            unsigned dist = (unsigned)(dx * dx / 2 + dy * dy);
            unsigned glow = dist < 330000 ? (330000 - dist) / 3500 : 0;
            uint32_t color = ui_mix(base, 0xFF356D8D, glow);
            /* Broad glass folds; computed once per mode and cached by desktop. */
            int beam = u + v * 3 / 5;
            if (beam > 870 && beam < 1110)
                color = ui_mix(color, 0xFF6A9CA9, (unsigned)(1110 - beam) / 10);
            if (beam > 1150 && beam < 1170)
                color = ui_mix(color, 0xFF88ACBD, (unsigned)(1170 - beam) / 2);
            s->pixels[y * s->pitch + x] = color;
        }
    }
}
