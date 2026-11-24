/* Real desktop integration plus public popup contracts, without kernel input. */
#include "../../../gui/gui_desktop.c"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static void popup_options(int scale)
{
    gui_theme_options_t options = {GUI_LAYOUT_DOCK, (uint16_t)scale, true, true};
    gui_theme_set_display(3840, 2160);
    gui_theme_set_physical_size(0, 0);
    gui_theme_configure(&options);
}

static void check_popup_geometry(void)
{
    const int scales[] = {100, 125, 150, 175, 200, 250, 300};
    const int totals[] = {2, 8, 32};
    for (unsigned scale = 0; scale < sizeof(scales) / sizeof(scales[0]); scale++) {
        popup_options(scales[scale]);
        for (unsigned total = 0; total < sizeof(totals) / sizeof(totals[0]); total++) {
            for (int small = 0; small < 2; small++) {
                gui_rect_t limits = {gui_px(20), gui_px(40), gui_px(350), gui_px(small ? 95 : 330)};
                for (int side = 0; side < 4; side++) {
                    gui_rect_t anchor = {side & 1 ? limits.x + limits.w - 50 : -10,
                                         side & 2 ? limits.y + limits.h - 20 : 10,
                                         gui_px(220), gui_px(32)};
                    for (int hot = 0; hot < totals[total]; hot++) {
                        gui_popup_layout_t p = gui_popup_geometry(anchor, limits, totals[total], 1000, hot);
                        CHECK(p.count > 0 && p.count <= p.total);
                        CHECK(p.bounds.x >= limits.x && p.bounds.y >= limits.y);
                        CHECK(p.bounds.x + p.bounds.w <= limits.x + limits.w);
                        CHECK(p.bounds.y + p.bounds.h <= limits.y + limits.h);
                        CHECK(p.items.w > 0 && p.items.h > 0);
                        CHECK(p.first <= hot && hot < p.first + p.count);
                        int room = p.row_height + gui_px(10);
                        if ((int64_t)anchor.y + anchor.h + room <= limits.y + limits.h ||
                            (int64_t)anchor.y - room >= limits.y)
                            CHECK(p.bounds.y + p.bounds.h <= anchor.y ||
                                  p.bounds.y >= (int64_t)anchor.y + anchor.h);
                        for (int i = 0; i < p.total; i++) {
                            gui_rect_t row = gui_popup_item_rect(&p, i);
                            if (i < p.first || i >= p.first + p.count) CHECK(row.h == 0);
                            else {
                                CHECK(gui_popup_hit(&p, row.x, row.y) == i);
                                CHECK(gui_popup_hit(&p, row.x + row.w - 1, row.y + row.h - 1) == i);
                            }
                        }
                        CHECK(gui_popup_hit(&p, p.bounds.x - 1, p.bounds.y) == GUI_POPUP_OUTSIDE);
                        CHECK(gui_popup_hit(&p, p.bounds.x, p.bounds.y) == GUI_POPUP_CHROME);
                        CHECK(gui_popup_hit(&p, p.items.x, p.items.y - 1) == GUI_POPUP_CHROME);
                        CHECK(gui_popup_hit(&p, p.items.x - 1, p.items.y) == GUI_POPUP_CHROME);
                        CHECK(gui_popup_hit(&p, p.items.x, p.items.y + p.items.h) == GUI_POPUP_CHROME);
                        if (p.count < p.total) {
                            CHECK(gui_popup_hit(&p, p.previous.x + 2, p.previous.y + 2) ==
                                  (p.first ? GUI_POPUP_PREVIOUS : GUI_POPUP_CHROME));
                            CHECK(gui_popup_hit(&p, p.next.x + 2, p.next.y + 2) ==
                                  (p.first + p.count < p.total ? GUI_POPUP_NEXT : GUI_POPUP_CHROME));
                        }
                    }
                }
            }
        }
        gui_rect_t limits = {0, 0, gui_px(350), gui_px(330)};
        gui_popup_layout_t narrow = gui_popup_geometry((gui_rect_t){0, 0, 1, gui_px(32)}, limits, 8, 0, 0);
        CHECK(narrow.count > 0 && narrow.items.w > 0);
        CHECK(gui_popup_geometry((gui_rect_t){0, 0, 100, 32}, limits, 0, 0, 0).count == 0);
        CHECK(gui_popup_geometry((gui_rect_t){0, 0, 0, 32}, limits, 8, 0, 0).count == 0);
        CHECK(gui_popup_geometry((gui_rect_t){INT_MAX, INT_MIN, INT_MAX, 32}, limits, 8, 0, 0).count > 0);
        CHECK(gui_popup_geometry((gui_rect_t){0, 0, 100, 32}, (gui_rect_t){0, 0, 10, 10}, 8, 0, 0).count == 0);
    }
}

static void check_popup_rendering(void)
{
    enum {W = 641, H = 481, P = 657, G = 32, N = G + P * H + G};
    uint32_t *memory = malloc(N * sizeof(uint32_t));
    uint32_t *reference = malloc(N * sizeof(uint32_t));
    CHECK(memory && reference);
    gui_surface_t s = {memory + G, W, H, P};
    const int scales[] = {100, 125, 150, 175, 200, 250, 300};
    for (unsigned scale = 0; scale < sizeof(scales) / sizeof(scales[0]); scale++) {
        popup_options(scales[scale]);
        gui_rect_t limits = {3, 3, W - 6, H - 6};
        gui_rect_t anchor = {W - gui_px(290), H - gui_px(110), gui_px(280), gui_px(32)};
        gui_popup_layout_t p = gui_popup_geometry(anchor, limits, 8, 0, 2);
        CHECK(p.count > 0);
        gui_rect_t pointer = gui_popup_item_rect(&p, p.first);
        for (int run = 0; run < 2; run++) {
            for (int i = 0; i < N; i++) memory[i] = 0xAA192F45;
            gui_popup_render(&s, &p, setting_labels[1], 1, 2, true,
                              run ? pointer.x + 3 : -100, run ? pointer.y + 3 : -100);
            if (!run) memcpy(reference, memory, N * sizeof(uint32_t));
            else CHECK(!memcmp(reference, memory, N * sizeof(uint32_t)));
            for (int y = 0; y < H; y++)
                for (int x = W; x < P; x++) CHECK(s.pixels[y * P + x] == 0xAA192F45);
            for (int i = 0; i < G; i++) {
                CHECK(memory[i] == 0xAA192F45);
                CHECK(memory[G + P * H + i] == 0xAA192F45);
            }
        }
    }
    free(reference); free(memory);
}

static gui_popup_layout_t current_popup(void)
{
    gui_rect_t controls[5], body;
    desktop_settings_geometry(controls, &body);
    return desktop_settings_menu(controls, body);
}

static void check_desktop_input(void)
{
    popup_options(100);
    gui_desktop_init(1280, 800);
    gui_desktop_show_window(1);
    CHECK(gui_desktop_key(0x2B, false)); /* Focus scale. */
    CHECK(gui_desktop_key(0x28, false)); /* Open. */
    CHECK(settings_keyboard_focus);
    gui_desktop_set_cursor(desk_cursor_x, desk_cursor_y);
    CHECK(settings_keyboard_focus);
    gui_desktop_set_cursor(desk_cursor_x + 1, desk_cursor_y);
    CHECK(!settings_keyboard_focus);
    gui_popup_layout_t p = current_popup();
    CHECK(settings_open == 1 && gui_theme_get()->scale_percent == 100);
    const int border[][2] = {{p.bounds.x, p.bounds.y}, {p.items.x + 15, p.items.y - 1},
        {p.items.x - 1, p.items.y + 15}, {p.items.x + 15, p.items.y + p.items.h}};
    for (unsigned i = 0; i < sizeof(border) / sizeof(border[0]); i++) {
        CHECK(gui_desktop_click(1, border[i][0], border[i][1]));
        CHECK(settings_open == 1 && gui_theme_get()->scale_percent == 100);
    }
    gui_rect_t target = gui_popup_item_rect(&p, 2);
    CHECK(gui_desktop_click(1, target.x + 30, target.y + target.h / 2));
    CHECK(settings_open == -1 && gui_theme_get()->scale_percent == 125);

    CHECK(gui_desktop_key(0x28, false));
    CHECK(gui_desktop_key(0x51, false));
    CHECK(gui_theme_get()->scale_percent == 125); /* Navigation only. */
    CHECK(gui_desktop_key(0x29, false));
    CHECK(settings_open == -1 && gui_theme_get()->scale_percent == 125);

    popup_options(100); gui_desktop_init(1280, 800); gui_desktop_show_window(1);
    demo_windows[1].h = 150; /* Only a few full-size rows can fit. */
    CHECK(gui_desktop_key(0x2B, false)); CHECK(gui_desktop_key(0x28, false));
    p = current_popup();
    CHECK(p.count > 0 && p.count < 8);
    for (int steps = 0; p.first + p.count < p.total; steps++) {
        CHECK(steps < 8);
        CHECK(gui_desktop_click(1, p.next.x + 3, p.next.y + 3));
        p = current_popup();
        CHECK(gui_theme_get()->scale_percent == 100 && settings_open == 1);
    }
    CHECK(gui_popup_item_rect(&p, 7).h > 0);
    for (int steps = 0; p.first > 0; steps++) {
        CHECK(steps < 8);
        CHECK(gui_desktop_click(1, p.previous.x + 3, p.previous.y + 3));
        p = current_popup();
    }
    CHECK(p.first == 0);
    for (int i = 0; i < 16; i++) {
        CHECK(gui_desktop_key(0x51, false));
        p = current_popup();
        target = gui_popup_item_rect(&p, settings_hot);
        CHECK(target.h > 0 && gui_popup_hit(&p, target.x + 2, target.y + 2) == settings_hot);
        CHECK(gui_theme_get()->scale_percent == 100);
    }
    CHECK(gui_desktop_click(1, demo_windows[1].x + 1, demo_windows[1].y + GUI_TITLEBAR_H + 1));
    CHECK(settings_open == -1 && gui_theme_get()->scale_percent == 100);
}

static void check_glass_repaint(void)
{
    enum {W = 800, H = 600};
    uint32_t *pixels = malloc(W * H * sizeof(uint32_t));
    uint32_t *reference = malloc(W * H * sizeof(uint32_t));
    CHECK(pixels && reference);
    popup_options(100); gui_desktop_init(W, H);
    gui_desktop_set_cursor(-50, -50);
    demo_windows[0] = (gui_win_desc_t){60, 100, 320, 190, "Terminal", GUI_TERM_BG, false};
    demo_windows[1].hidden = true;
    gui_desktop_set_focus(0);
    gui_surface_t s = {pixels, W, H, W};
    gui_desktop_render_background(&s);
    uint32_t background = pixels[180 * W + 150];
    gui_desktop_render(&s);
    CHECK(pixels[180 * W + 150] == GUI_TERM_BG);
    demo_windows[0].x = 400; demo_windows[0].y = 240;
    gui_desktop_render(&s);
    CHECK(pixels[180 * W + 150] == background);
    memcpy(reference, pixels, W * H * sizeof(uint32_t));
    for (int frame = 0; frame < 4; frame++) {
        gui_desktop_render(&s);
        CHECK(!memcmp(reference, pixels, W * H * sizeof(uint32_t)));
    }
    free(reference); free(pixels);
}

int main(void)
{
    check_popup_geometry();
    check_popup_rendering();
    check_desktop_input();
    check_glass_repaint();
    printf("GUI popup: %u checks passed\n", checks);
    return 0;
}
