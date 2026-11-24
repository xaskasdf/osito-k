/* Compare opaque shadow occlusion with the original rasterization and padding.
 * Translucent desktop chrome has its own contracts in gui_aero_host.c. */
#include "../../../gui/gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { WIDTH = 127, HEIGHT = 93, PITCH = 139, GUARD = 32,
       WORDS = GUARD + PITCH * HEIGHT + GUARD };
static uint32_t actual[WORDS], expected[WORDS], initial[WORDS];
static unsigned cases, failures;
static uint32_t random_state = 0x13579BDF;
static uint64_t blended, excluded;

static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static uint32_t blend(uint32_t dst, uint32_t src)
{
    unsigned a = src >> 24;
    if (!a) return dst;
    if (a == 255) return src;
    unsigned rb = (((src & 0xFF00FF) * a +
                    (dst & 0xFF00FF) * (255 - a)) >> 8) & 0xFF00FF;
    unsigned g = (((src & 0xFF00) * a +
                   (dst & 0xFF00) * (255 - a)) >> 8) & 0xFF00;
    return 0xFF000000 | rb | g;
}

static int skip_corner(int r, int row)
{
    if (r <= 0 || row < 0 || row >= r) return 0;
    int dy = r - 1 - row, sx = r;
    while (sx > 0 && sx * sx > r * r - dy * dy) sx--;
    return r - 1 - sx;
}

static void reference_layer(gui_surface_t *s, int x, int y, int w, int h,
                            int r, uint32_t color, const gui_win_desc_t *body)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int row = 0; row < h; row++) {
        int skip = row < r ? skip_corner(r, row) :
                   row >= h - r ? skip_corner(r, h - row - 1) : 0;
        int py = y + row;
        if (py < 0 || py >= (int)s->height) continue;
        for (int px = x + skip; px < x + w - skip; px++) {
            if (px < 0 || px >= (int)s->width) continue;
            if ((color >> 24) && body) {
                blended++;
                if (px >= body->x && px < body->x + body->w &&
                    py >= body->y && py < body->y + body->h)
                    excluded++;
            }
            uint32_t *p = &s->pixels[py * s->pitch + px];
            *p = blend(*p, color);
        }
    }
}

static void reference_shadow(gui_surface_t *s, int x, int y, int w, int h,
                             int dx, int dy, int blur, uint32_t color)
{
    gui_win_desc_t body = { .x = x, .y = y, .w = w, .h = h };
    if (blur <= 0) {
        reference_layer(s, x + dx, y + dy, w, h, 4, color, &body);
        return;
    }
    for (int i = 4; i >= 1; i--) {
        int expand = blur * i / 4;
        unsigned a = (color >> 24) * (unsigned)i / 8;
        reference_layer(s, x + dx - expand, y + dy - expand,
                        w + 2 * expand, h + 2 * expand, 4 + expand,
                        (a << 24) | (color & 0xFFFFFF), &body);
    }
}

static void reset(void)
{
    for (unsigned i = 0; i < WORDS; i++)
        initial[i] = actual[i] = expected[i] = random_word();
}

static void compare(void)
{
    cases++;
    for (unsigned i = 0; i < WORDS; i++) {
        if (actual[i] != expected[i]) {
            printf("FAIL case %u word %u: %08x != %08x\n", cases, i,
                   actual[i], expected[i]);
            failures++;
            break;
        }
    }
}

static void check_shadow(int x, int y, int w, int h, int dx, int dy,
                         int blur, uint32_t color, unsigned width,
                         unsigned height)
{
    gui_surface_t a = { actual + GUARD, width, height, PITCH };
    gui_surface_t e = { expected + GUARD, width, height, PITCH };
    reset();
    reference_shadow(&e, x, y, w, h, dx, dy, blur, color);
    gui_box_shadow(&a, x, y, w, h, dx, dy, blur, color);
    compare();
    memcpy(actual, initial, sizeof(actual));
    /* Preserve the untouched interior in the oracle independently. */
    for (unsigned py = 0; py < height; py++) {
        for (unsigned px = 0; px < width; px++) {
            if ((int)px >= x && (int)px < x + w &&
                (int)py >= y && (int)py < y + h)
                expected[GUARD + py * PITCH + px] =
                    initial[GUARD + py * PITCH + px];
        }
    }
    gui_box_shadow_outer(&a, x, y, w, h, dx, dy, blur, color);
    compare();
}


static void benchmark(void)
{
    const unsigned pitch = 1931, height = 1080;
    size_t bytes = (size_t)pitch * height * sizeof(uint32_t);
    uint32_t *a = malloc(bytes), *e = malloc(bytes);
    if (!a || !e) exit(2);
    memset(a, 0xAC, bytes);
    memset(e, 0xAC, bytes);
    gui_surface_t sa = { a, 1920, height, pitch };
    gui_surface_t se = { e, 1920, height, pitch };
    gui_win_desc_t windows[] = {
        { .x = 100, .y = 100, .w = 1152, .h = 600,
          .content_color = GUI_TERM_BG, .title = "Terminal" },
        { .x = 850, .y = 500, .w = 770, .h = 430,
          .content_color = GUI_WINDOW_BG, .title = "System Info" }
    };
    blended = excluded = 0;
    for (unsigned i = 0; i < 2; i++)
        reference_shadow(&se, windows[i].x, windows[i].y, windows[i].w,
                         windows[i].h + GUI_TITLEBAR_H, 0, 4,
                         i ? 14 : 8, i ? 0x60000000 : 0x30000000);
    printf("1080p shadow work: full=%llu outer=%llu skipped=%llu pixels/frame\n",
           (unsigned long long)blended, (unsigned long long)(blended - excluded),
           (unsigned long long)excluded);
    memset(e, 0xAC, bytes);
    double timings[2];
    for (unsigned version = 0; version < 2; version++) {
        clock_t start = clock();
        for (unsigned frame = 0; frame < 40; frame++) {
            gui_surface_t *s = version ? &sa : &se;
            gui_fill_rect(s, 0, 0, 1920, height, GUI_BG_DARK);
            for (unsigned i = 0; i < 2; i++) {
                gui_win_desc_t *w = &windows[i];
                if (version)
                    gui_box_shadow_outer(s, w->x, w->y, w->w, w->h + GUI_TITLEBAR_H,
                                          0, 4, i ? 14 : 8, i ? 0x60000000 : 0x30000000);
                else
                    gui_box_shadow(s, w->x, w->y, w->w, w->h + GUI_TITLEBAR_H,
                                    0, 4, i ? 14 : 8, i ? 0x60000000 : 0x30000000);
                gui_fill_rect(s, w->x, w->y, w->w, w->h + GUI_TITLEBAR_H, w->content_color);
            }
        }
        timings[version] = (double)(clock() - start) / CLOCKS_PER_SEC;
    }
    if (memcmp(a, e, bytes)) {
        puts("FAIL 1080p opaque shadow occlusion");
        failures++;
    }
    cases++;
    printf("1080p 40 frames CPU seconds: previous=%.6f outer=%.6f (not a gate)\n",
           timings[0], timings[1]);
    free(a);
    free(e);
}

int main(void)
{
    const int sizes[] = { 0, 1, 2, 7, 28, 81, 200 };
    const int blurs[] = { -1, 0, 1, 4, 8, 14, 31 };
    const uint32_t colors[] = { 0, 0x01A07FB3, 0x30000000, 0x60000000,
                               0x7F1278FE, 0xFF0102AA };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (unsigned j = 0; j < sizeof(blurs) / sizeof(blurs[0]); j++)
            for (unsigned k = 0; k < sizeof(colors) / sizeof(colors[0]); k++)
                check_shadow(17, 11, sizes[i], sizes[6-i], 0, 4,
                             blurs[j], colors[k], WIDTH, HEIGHT);
    for (unsigned i = 0; i < 1600; i++)
        check_shadow((int)(random_word() % 400) - 150,
                     (int)(random_word() % 300) - 120,
                     random_word() % 220, random_word() % 150,
                     (int)(random_word() % 121) - 60,
                     (int)(random_word() % 81) - 40,
                     random_word() % 32, random_word(),
                     i % 11 ? WIDTH : 0, i % 13 ? HEIGHT : 0);
    benchmark();
    printf("gui shadow: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
