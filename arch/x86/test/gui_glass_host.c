/* Backdrop material contracts: real public renderer, guarded pitched buffers. */
#include "../../../gui/gui.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static void glass_options(int scale, bool enabled)
{
    gui_theme_options_t options = { GUI_LAYOUT_DOCK, (uint16_t)scale, enabled, true };
    gui_theme_set_display(3840, 2160);
    gui_theme_configure(&options);
}

static unsigned color_distance(uint32_t a, uint32_t b)
{
    unsigned sum = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        sum += (unsigned)abs((int)((a >> shift) & 255) - (int)((b >> shift) & 255));
    return sum;
}

static void glass_check_bounds(void)
{
    enum { W = 83, H = 57, P = 97, G = 32, N = G + P * H + G };
    uint32_t *memory = malloc(N * sizeof(uint32_t));
    CHECK(memory != NULL);
    gui_surface_t s = {memory + G, W, H, P};
    const gui_rect_t cases[] = {
        {13, 9, 50, 37}, {-7, 9, 50, 37}, {50, 9, 50, 37},
        {13, -9, 50, 37}, {13, 40, 50, 37}, {-10, -10, 150, 100},
        {0, 0, 1, 1}, {82, 56, 1, 1}, {0, 0, W, H}, {0, 0, 0, 4},
        {0, 0, -1, 4}, {0, 0, 10, -1}, {-80, -80, 2, 2}, {100, 100, 4, 4},
        {INT_MIN, INT_MIN, INT_MAX, INT_MAX}, {INT_MAX, INT_MAX, 50, 50},
        {-INT_MAX + 20, -INT_MAX + 20, INT_MAX, INT_MAX},
        {1, 1, INT_MAX, INT_MAX}
    };
    const int scales[] = {100, 125, 150, 175, 200, 250, 300};
    for (unsigned scale = 0; scale < sizeof(scales) / sizeof(scales[0]); scale++) {
        for (unsigned enabled = 0; enabled < 2; enabled++) {
            glass_options(scales[scale], enabled != 0);
            for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
                for (int i = 0; i < N; i++) memory[i] = 0x19374B65;
                gui_rect_t r = cases[c];
                gui_glass_panel(&s, r, c & 1 ? INT_MAX : -1, (c & 2) != 0);
                for (int i = 0; i < G; i++) {
                    CHECK(memory[i] == 0x19374B65);
                    CHECK(memory[G + P * H + i] == 0x19374B65);
                }
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < P; x++) {
                        if (x >= W || x < r.x || y < r.y ||
                            x >= (int64_t)r.x + r.w || y >= (int64_t)r.y + r.h)
                            CHECK(s.pixels[y * P + x] == 0x19374B65);
                    }
                }
            }
        }
    }
    gui_surface_t invalid = {NULL, W, H, P};
    gui_glass_panel(NULL, cases[0], 4, true);
    gui_glass_panel(&invalid, cases[0], 4, true);
    invalid.pixels = memory; invalid.pitch = 1;
    gui_glass_panel(&invalid, cases[0], 4, true);
    invalid.pitch = P; invalid.width = 0;
    gui_glass_panel(&invalid, cases[0], 4, true);
    free(memory);
}

static void glass_check_material(void)
{
    enum { W = 160, H = 120, N = W * H };
    uint32_t *a = malloc(N * sizeof(uint32_t)), *b = malloc(N * sizeof(uint32_t));
    CHECK(a && b);
    gui_surface_t sa = {a, W, H, W}, sb = {b, W, H, W};
    gui_rect_t r = {40, 24, 80, 72};
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        glass_options(100, enabled != 0);
        gui_fill_rect(&sa, 0, 0, W, H, 0xFFB02E36);
        gui_fill_rect(&sb, 0, 0, W, H, 0xFF264FAA);
        gui_glass_panel(&sa, r, 0, true);
        gui_glass_panel(&sb, r, 0, true);
        if (enabled) {
            CHECK(color_distance(a[70 * W + 80], b[70 * W + 80]) > 60);
        } else {
            for (int y = r.y; y < r.y + r.h; y++)
                for (int x = r.x; x < r.x + r.w; x++)
                    CHECK(a[y * W + x] == b[y * W + x]);
        }
    }

    /* Read the halo outside the rectangle, but never paint it. */
    gui_fill_rect(&sa, 0, 0, W, H, 0xFF151A20);
    gui_fill_rect(&sb, 0, 0, W, H, 0xFF151A20);
    gui_fill_rect(&sb, r.x - 10, r.y, 10, r.h, 0xFFFF2030);
    gui_glass_panel(&sa, r, 0, true);
    gui_glass_panel(&sb, r, 0, true);
    CHECK(color_distance(a[70 * W + 42], b[70 * W + 42]) > 10);
    CHECK(b[70 * W + 39] == 0xFFFF2030);
    CHECK(color_distance(a[70 * W + 80], b[70 * W + 80]) == 0);

    /* A changed scene must not reuse a stale previous backdrop. */
    uint32_t first = a[70 * W + 42];
    gui_fill_rect(&sa, 0, 0, W, H, 0xFF151A20);
    gui_glass_panel(&sa, r, 0, true);
    CHECK(a[70 * W + 42] == first);

    /* Thin detail disappears in the blur; an alpha overlay alone fails this. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) a[y * W + x] = x & 1 ? 0xFFFFFFFF : 0xFF000000;
    gui_glass_panel(&sa, r, 0, true);
    unsigned difference = 0;
    for (int x = 60; x < 100; x++)
        difference += color_distance(a[70 * W + x], a[70 * W + x + 1]);
    CHECK(difference < 40 * 9);

    /* Keep a bright backdrop dark enough for light title text. */
    gui_fill_rect(&sa, 0, 0, W, H, 0xFFFFFFFF);
    gui_glass_panel(&sa, r, 0, true);
    uint32_t p = a[40 * W + 80];
    unsigned luminance = (((p >> 16) & 255) * 54 + ((p >> 8) & 255) * 183 + (p & 255) * 19) >> 8;
    CHECK(luminance < 145);
    free(a); free(b);

    /* Exercise adaptive sampling when the region exceeds the fixed scratch. */
    uint32_t *large = malloc((2048 * 512 + 2) * sizeof(uint32_t));
    CHECK(large != NULL);
    large[0] = large[2048 * 512 + 1] = 0x12345678;
    gui_surface_t s = {large + 1, 2048, 512, 2048};
    gui_fill_rect(&s, 0, 0, 2048, 512, 0xFF365B72);
    gui_glass_panel(&s, (gui_rect_t){0, 0, 2048, 512}, 12, true);
    CHECK(large[0] == 0x12345678 && large[2048 * 512 + 1] == 0x12345678);
    CHECK(s.pixels[256 * 2048 + 1024] != 0xFF365B72);
    free(large);
}

static void glass_material_preview(const char *path)
{
    enum { W = 1280, H = 800 };
    uint32_t *pixels = malloc(W * H * sizeof(uint32_t));
    CHECK(pixels != NULL);
    glass_options(100, true);
    gui_surface_t s = {pixels, W, H, W};
    gui_wallpaper_render(&s);
    gui_gradient_h(&s, 90, 220, 1100, 400, 0xFF275D80, 0xFF946837);
    for (int x = 90; x < 1190; x += 24)
        gui_vline(&s, x, 220, 400, 0xFF93BCD1);
    for (int y = 242; y < 620; y += 36)
        gui_ui_text(&s, 112, y, "Ventanas, texto y color detras del cristal", 0xFFE5EDF4, 1060);
    gui_ui_text(&s, 90, 144, "OSITOK  /  CRISTAL OSCURO", GUI_INK, 1100);
    gui_ui_text(&s, 90, 174, "Mismo fondo, material activado a la izquierda y desactivado a la derecha", GUI_MUTED, 1100);
    gui_rect_t left = {140, 275, 450, 230}, right = {690, 275, 450, 230};
    gui_soft_shadow(&s, left, 18, 4, 88);
    gui_glass_panel(&s, left, 6, true);
    glass_options(100, false);
    gui_soft_shadow(&s, right, 18, 4, 88);
    gui_glass_panel(&s, right, 6, true);
    gui_ui_text(&s, 166, 300, "Cristal", GUI_INK, 398);
    gui_ui_text(&s, 166, 460, "Fondo difuso, texto nitido", GUI_INK, 398);
    gui_ui_text(&s, 716, 300, "Opaco", GUI_INK, 398);
    gui_ui_text(&s, 716, 460, "Sin transparencia ni desenfoque", GUI_INK, 398);
    glass_options(100, true);
    gui_win_desc_t win = {220, 576, 840, 80, "Terminal - marco sobre el mismo fondo", GUI_SURFACE, false};
    gui_desktop_set_cursor(-1, -1);
    gui_window_render(&s, &win, true);
    gui_ui_text(&s, 240, 638, "osito> _", GUI_INK, 780);
    FILE *out = fopen(path, "wb");
    CHECK(out != NULL);
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
        CHECK(fwrite(rgb, 1, 3, out) == 3);
    }
    CHECK(fclose(out) == 0);
    free(pixels);
}

static void glass_benchmark(void)
{
    const unsigned modes[][3] = {{1920, 1080, 100}, {3840, 2160, 200}};
    for (unsigned mode = 0; mode < 2; mode++) {
        unsigned w = modes[mode][0], h = modes[mode][1];
        uint32_t *pixels = malloc((size_t)w * h * sizeof(uint32_t));
        CHECK(pixels != NULL);
        gui_surface_t s = {pixels, w, h, w};
        for (unsigned enabled = 0; enabled < 2; enabled++) {
            glass_options((int)modes[mode][2], enabled != 0);
            const int frames = 30;
            clock_t start = clock();
            for (int frame = 0; frame < frames; frame++) {
                gui_fill_rect(&s, 0, 0, (int)w, (int)h, 0xFF304354);
                gui_glass_panel(&s, (gui_rect_t){0, 0, (int)w, gui_px(34)}, 0, true);
                gui_glass_panel(&s, (gui_rect_t){gui_px(140), gui_px(160), gui_px(900), gui_px(36)}, gui_px(5), true);
                gui_glass_panel(&s, (gui_rect_t){gui_px(200), gui_px(240), gui_px(800), gui_px(36)}, gui_px(5), false);
                gui_glass_panel(&s, (gui_rect_t){(int)w / 2 - gui_px(160), (int)h - gui_px(70), gui_px(320), gui_px(56)}, gui_px(8), true);
            }
            double ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC / frames;
            printf("Glass render %ux%u scale=%u%% glass=%u: %.3f ms/frame (host CPU, fill + four regions)\n",
                   w, h, modes[mode][2], enabled, ms);
        }
        free(pixels);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcmp(argv[1], "--benchmark")) glass_benchmark();
        else glass_material_preview(argv[1]);
        return 0;
    }
    glass_check_bounds();
    glass_check_material();
    printf("GUI glass: %u checks passed\n", checks);
    return 0;
}
