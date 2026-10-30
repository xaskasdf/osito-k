/* Native-size grayscale text; integer-only at runtime. */
#include "gui.h"
#include "gui_font_data.h"

static uint32_t ui_decode(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t cp = *p++;
    if (cp < 128) { *text = (const char *)p; return cp; }
    unsigned n = cp >= 0xF0 && cp <= 0xF4 ? 3 : cp >= 0xE0 && cp < 0xF0 ? 2 :
                 cp >= 0xC2 && cp < 0xE0 ? 1 : 0;
    if (!n) { *text = (const char *)p; return '?'; }
    uint32_t minimum = n == 1 ? 0x80 : n == 2 ? 0x800 : 0x10000;
    cp &= (1u << (6 - n)) - 1;
    for (unsigned i = 0; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *text = (const char *)p; return '?'; }
        cp = (cp << 6) | (p[i] & 63);
    }
    *text = (const char *)(p + n);
    return cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ? '?' : cp;
}

static unsigned ui_face(void)
{
    unsigned scale = gui_theme_metrics()->scale_percent;
    for (unsigned i = 0; i < sizeof(gui_faces) / sizeof(gui_faces[0]); i++)
        if (gui_faces[i].scale == scale) return i;
    return 0;
}

static const gui_glyph_t *ui_glyph(uint32_t cp, unsigned face)
{
    unsigned index = '?' - 32;
    if (cp >= 32 && cp <= 126) index = cp - 32;
    else if (cp >= 160 && cp <= 255) index = 95 + cp - 160;
    else if (cp == 0x2013) index = 191;
    else if (cp == 0x2014) index = 192;
    else if (cp == 0x2026) index = 193;
    return &gui_glyphs[face * GUI_FACE_GLYPHS + index];
}

int gui_ui_text_width(const char *text)
{
    int width = 0;
    unsigned face = ui_face();
    if (!text) return 0;
    while (*text) {
        width += ui_glyph(ui_decode(&text), face)->advance;
        if (width > 1000000) return width;
    }
    return width;
}

int gui_ui_text_height(void) { return gui_faces[ui_face()].line; }

void gui_ui_text(gui_surface_t *s, int32_t x, int32_t y, const char *text,
                 uint32_t color, int32_t max_width)
{
    if (!s || !s->pixels || !text || max_width <= 0) return;
    int32_t left = x;
    int64_t right = (int64_t)x + max_width;
    unsigned face = ui_face();
    int32_t baseline = y + gui_faces[face].baseline;
    while (*text && x < right) {
        const gui_glyph_t *g = ui_glyph(ui_decode(&text), face);
        const uint8_t *run = gui_coverage + g->offset;
        unsigned total = (unsigned)g->w * g->h, pos = 0;
        while (pos < total) {
            unsigned value = *run++, a = (value >> 4) * 17;
            unsigned end = pos + (value & 15) + 1;
            if (end > total) end = total;
            for (; pos < end; pos++) {
                int32_t px = x + g->x + (int32_t)(pos % g->w);
                int32_t py = baseline + g->y + (int32_t)(pos / g->w);
                if (!a || px < left || px < 0 || py < 0 || px >= right ||
                    px >= (int32_t)s->width || py >= (int32_t)s->height) continue;
                uint32_t *dst = &s->pixels[(uint32_t)py * s->pitch + px];
                uint32_t d = *dst, inv = 255 - a;
                uint32_t r = (((color >> 16) & 255) * a + ((d >> 16) & 255) * inv + 127) / 255;
                uint32_t b = ((color & 255) * a + (d & 255) * inv + 127) / 255;
                uint32_t green = (((color >> 8) & 255) * a + ((d >> 8) & 255) * inv + 127) / 255;
                *dst = 0xFF000000 | (r << 16) | (green << 8) | b;
            }
        }
        x += g->advance;
    }
}

void gui_ui_text_fit(gui_surface_t *s, gui_rect_t r, const char *text,
                     uint32_t color, bool centered)
{
    if (!text || r.w <= 0 || r.h < gui_ui_text_height()) return;
    int width = gui_ui_text_width(text);
    int y = r.y + (r.h - gui_ui_text_height()) / 2;
    if (width <= r.w) {
        gui_ui_text(s, r.x + (centered ? (r.w - width) / 2 : 0), y, text, color, r.w);
        return;
    }
    unsigned face = ui_face();
    const char *end = text;
    int used = 0, dots = ui_glyph(0x2026, face)->advance;
    if (dots > r.w) return;
    char clipped[256];
    unsigned bytes = 0;
    while (*end) {
        const char *start = end;
        int advance = ui_glyph(ui_decode(&end), face)->advance;
        unsigned count = (unsigned)(end - start);
        if (used + advance + dots > r.w || bytes + count + 4 >= sizeof(clipped)) break;
        while (start < end) clipped[bytes++] = *start++;
        used += advance;
    }
    clipped[bytes++] = (char)0xE2; clipped[bytes++] = (char)0x80;
    clipped[bytes++] = (char)0xA6; clipped[bytes] = 0;
    gui_ui_text(s, r.x, y, clipped, color, r.w);
}
