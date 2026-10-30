/* Exercise the actual Appearance layout, input and clipped painter. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../gui/gui_desktop.c"

static unsigned checks;
#define CHECK(test) do { checks++; if (!(test)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static void setup(int scale, int width, int height)
{
    gui_theme_options_t options = {GUI_LAYOUT_DOCK, (uint16_t)scale, true, true};
    gui_theme_set_physical_size(0, 0);
    gui_theme_configure(&options);
    gui_desktop_init(3840, 2160);
    CHECK(gui_theme_metrics()->scale_percent == (unsigned)scale);
    gui_desktop_show_window(1);
    demo_windows[1].x = gui_px(40); demo_windows[1].y = gui_px(50);
    demo_windows[1].w = gui_px(width); demo_windows[1].h = gui_px(height);
    gui_desktop_set_cursor(-10, -10);
}

static void check_focus_visible(void)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    CHECK(desktop_settings_control_visible(&l, settings_focus));
    gui_rect_t r = l.controls[settings_focus];
    CHECK(r.x >= l.viewport.x && r.x + r.w <= l.viewport.x + l.viewport.w);
    if (l.stacked && settings_focus < 3) {
        CHECK(l.labels[settings_focus].y >= l.viewport.y);
        CHECK(l.labels[settings_focus].y + l.labels[settings_focus].h < r.y);
    }
}

static void check_layout_and_navigation(void)
{
    const int scales[] = {100, 125, 150, 175, 200, 250, 300};
    const int widths[] = {280, 420, 560}, heights[] = {150, 254, 390};
    for (unsigned s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
        for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
            for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
                setup(scales[s], widths[w], heights[h]);
                desktop_settings_layout_t l = desktop_settings_layout();
                CHECK(l.stacked == (widths[w] < 440));
                CHECK(settings_scroll == 0);
                for (int i = 0; i < 5; i++) {
                    CHECK(l.controls[i].w > 0 && l.controls[i].h > 0);
                    if (i < 3 && !l.stacked)
                        CHECK(l.labels[i].x + l.labels[i].w < l.controls[i].x);
                    if (i) CHECK(l.controls[i-1].y + l.controls[i-1].h <= l.controls[i].y);
                }
                for (int backwards = 0; backwards < 2; backwards++) {
                    for (int step = 0; step < 10; step++) {
                        int previous = settings_focus;
                        CHECK(gui_desktop_key(0x2B, backwards));
                        CHECK(settings_focus == (previous + (backwards ? 4 : 1)) % 5);
                        CHECK(settings_keyboard_focus && settings_open == -1);
                        check_focus_visible();
                    }
                }
                CHECK(gui_desktop_key(0x4D, false));
                CHECK(settings_focus == 4); check_focus_visible();
                CHECK(gui_desktop_key(0x4A, false));
                CHECK(settings_focus == 0 && settings_scroll == 0); check_focus_visible();
                int x = l.viewport.x + gui_px(5), y = l.viewport.y + gui_px(5);
                CHECK(gui_desktop_scroll(1, x, y, INT_MIN));
                l = desktop_settings_layout();
                CHECK(settings_scroll == l.scroll_max);
                CHECK(l.footer.y + (l.stacked ? 3 : 2) * gui_px(23) + l.footer.h <= l.viewport.y + l.viewport.h);
                CHECK(gui_desktop_scroll(1, x, y, INT_MAX));
                CHECK(settings_scroll == 0 && !settings_keyboard_focus);
                CHECK(!gui_desktop_scroll(0, x, y, -1));
                CHECK(!gui_desktop_scroll(1, x, l.body.y + 1, -1));
                CHECK(!gui_desktop_scroll(1, -1, y, -1));
                CHECK(!gui_desktop_scroll(1, x, y, 0));
                for (int step = 0; step < 20; step++) CHECK(gui_desktop_key(0x4E, false));
                CHECK(settings_scroll == l.scroll_max);
                for (int step = 0; step < 20; step++) CHECK(gui_desktop_key(0x4B, false));
                CHECK(settings_scroll == 0);
                demo_windows[1].hidden = true;
                CHECK(!gui_desktop_scroll(1, x, y, -1));
            }
        }
    }
}

static void check_activation_and_scrollbar(void)
{
    setup(125, 320, 160);
    CHECK(gui_desktop_key(0x4D, false));
    desktop_settings_layout_t l = desktop_settings_layout();
    CHECK(gui_desktop_scroll(1, l.viewport.x + 4, l.viewport.y + 4, INT_MAX));
    bool reduced = gui_theme_get()->reduced_motion;
    CHECK(gui_desktop_key(0x2C, false)); /* Reveal without activating. */
    CHECK(gui_theme_get()->reduced_motion == reduced);
    check_focus_visible();
    CHECK(gui_desktop_key(0x2C, false));
    CHECK(gui_theme_get()->reduced_motion != reduced);
    CHECK(gui_desktop_key(0x4A, false));
    l = desktop_settings_layout();
    CHECK(gui_desktop_click(1, l.down.x + 2, l.down.y + 2));
    CHECK(settings_scroll > 0);
    CHECK(gui_desktop_click(1, l.up.x + 2, l.up.y + 2));
    CHECK(settings_scroll == 0);
    l = desktop_settings_layout();
    CHECK(gui_desktop_click(1, l.track.x + 2, l.track.y + l.track.h - 1));
    CHECK(settings_scroll > 0);
    l = desktop_settings_layout();
    int before = settings_scroll;
    CHECK(gui_desktop_click(1, l.thumb.x + 2, l.thumb.y + l.thumb.h / 2));
    CHECK(settings_scroll == before); /* A thumb click alone does not jump. */
    CHECK(gui_desktop_pointer_captured());
    CHECK(gui_desktop_pointer_update(l.thumb.x + 2, l.thumb.y + l.thumb.h / 2, false));
    CHECK(!gui_desktop_pointer_captured() && settings_scroll == before);
    CHECK(gui_desktop_scroll(1, l.viewport.x + 4, l.viewport.y + 4, INT_MIN));
    l = desktop_settings_layout();
    CHECK(l.controls[0].y < l.viewport.y);
    CHECK(!gui_desktop_click(1, l.controls[0].x + 3, l.controls[0].y + 3));
    CHECK(settings_open == -1);
    gui_desktop_resize(scr_w, scr_h);
    CHECK(settings_scroll == l.scroll_max); /* An unchanged frame keeps position. */
    demo_windows[1].h = gui_px(600);
    l = desktop_settings_layout();
    CHECK(!l.scroll_max && !settings_scroll);
    CHECK(gui_desktop_key(0x4D, false));
    check_focus_visible();
    demo_windows[1].h = gui_px(60); /* No complete control fits below the header. */
    reduced = gui_theme_get()->reduced_motion;
    CHECK(gui_desktop_key(0x28, false));
    CHECK(gui_theme_get()->reduced_motion == reduced && settings_open == -1);
    CHECK(gui_desktop_key(0x4A, false)); CHECK(gui_desktop_key(0x28, false));
    CHECK(settings_open == -1);
    gui_desktop_init(3840, 2160);
    CHECK(!settings_scroll && !settings_first);
}

static gui_popup_layout_t popup(void)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    return desktop_settings_menu(l.controls, l.body);
}

static int press_thumb(void)
{
    desktop_settings_layout_t l = desktop_settings_layout();
    int y = l.thumb.y + l.thumb.h / 2;
    int before = settings_scroll;
    CHECK(gui_desktop_click(1, l.thumb.x + 2, y));
    CHECK(gui_desktop_pointer_captured() && settings_scroll == before);
    CHECK(gui_desktop_pointer_update(l.thumb.x + 2, y, true));
    CHECK(settings_scroll == before);
    return y;
}

static void check_thumb_capture(void)
{
    const int scales[] = {100, 125, 150, 175, 200, 250, 300};
    for (unsigned i = 0; i < sizeof(scales)/sizeof(scales[0]); i++) {
        setup(scales[i], 320, 180);
        desktop_settings_layout_t l = desktop_settings_layout();
        desktop_settings_scroll_by(l.scroll_max / 3);
        int start = settings_scroll, y = press_thumb();
        CHECK(gui_desktop_pointer_update(INT_MIN, y, true));
        CHECK(settings_scroll == start && gui_desktop_pointer_captured());
        CHECK(gui_desktop_scroll(1, l.viewport.x + 2, l.viewport.y + 2, -1));
        CHECK(settings_scroll == start); /* No competing wheel while captured. */
        int previous = settings_scroll;
        for (int step = 1; step <= 6; step++) {
            CHECK(gui_desktop_pointer_update(-100, y + gui_px(step * 8), true));
            CHECK(settings_scroll >= previous && settings_scroll <= l.scroll_max);
            previous = settings_scroll;
        }
        CHECK(gui_desktop_pointer_update(INT_MAX, INT_MAX, true));
        CHECK(settings_scroll == l.scroll_max && gui_desktop_pointer_captured());
        CHECK(gui_desktop_pointer_update(INT_MAX, INT_MIN, true));
        CHECK(!settings_scroll && gui_desktop_pointer_captured());
        CHECK(gui_desktop_pointer_update(-100, INT_MAX, false));
        CHECK(settings_scroll == l.scroll_max && !gui_desktop_pointer_captured());
        CHECK(!gui_desktop_pointer_update(0, 0, true));
        CHECK(settings_scroll == l.scroll_max);

        desktop_settings_scroll_by(-l.scroll_max / 2);
        start = settings_scroll; y = press_thumb();
        CHECK(gui_desktop_pointer_update(-100, y - gui_px(12), true));
        CHECK(settings_scroll < start);
        CHECK(gui_desktop_key(0x29, false));
        CHECK(!gui_desktop_pointer_captured() && settings_scroll == start);
        CHECK(!gui_desktop_pointer_update(-100, y + 200, false));

        y = press_thumb();
        gui_desktop_set_focus(0);
        CHECK(!gui_desktop_pointer_captured());
        CHECK(!gui_desktop_pointer_update(-100, y + 100, true));
        gui_desktop_set_focus(1);
        y = press_thumb();
        demo_windows[1].hidden = true;
        CHECK(!gui_desktop_pointer_captured());
        CHECK(gui_desktop_pointer_update(-100, y + 100, true));
        demo_windows[1].hidden = false;
        CHECK(!gui_desktop_pointer_captured());

        y = press_thumb();
        demo_windows[1].x++;
        CHECK(gui_desktop_pointer_update(-100, y + 100, true));
        CHECK(!gui_desktop_pointer_captured() && settings_scroll == start);
        y = press_thumb();
        demo_windows[1].h += gui_px(20);
        CHECK(gui_desktop_pointer_update(-100, y + 100, true));
        CHECK(!gui_desktop_pointer_captured());
        y = press_thumb();
        gui_desktop_resize(scr_w + 10, scr_h + 10);
        CHECK(!gui_desktop_pointer_captured() && !settings_scroll);
        CHECK(!gui_desktop_pointer_update(-100, y + 100, true));

        y = press_thumb();
        CHECK(gui_desktop_key(0x2B, false));
        CHECK(!gui_desktop_pointer_captured());
        check_focus_visible();
        CHECK(!gui_desktop_pointer_update(-100, y + 100, true));
        (void)press_thumb();
        gui_desktop_init(3840, 2160);
        CHECK(!gui_desktop_pointer_captured());
    }
}

static void check_popup_wheel(void)
{
    setup(150, 320, 180);
    CHECK(gui_desktop_key(0x2B, false)); CHECK(gui_desktop_key(0x28, false));
    gui_popup_layout_t p = popup();
    CHECK(settings_open == 1 && p.count > 0 && p.count < p.total);
    int before = settings_scroll;
    CHECK(gui_desktop_scroll(1, p.items.x + 4, p.items.y + 4, INT_MIN));
    p = popup();
    CHECK(p.first + p.count == p.total && settings_scroll == before);
    CHECK(gui_theme_get()->scale_percent == 150);
    CHECK(gui_desktop_key(0x4A, false)); p = popup();
    CHECK(settings_hot == 0 && p.first == 0);
    CHECK(gui_desktop_key(0x4E, false)); p = popup();
    CHECK(gui_popup_item_rect(&p, settings_hot).h > 0);
    CHECK(gui_desktop_key(0x4D, false)); p = popup();
    CHECK(settings_hot == 7 && p.first + p.count == p.total);
    CHECK(gui_desktop_scroll(1, p.items.x + 4, p.items.y + 4, INT_MAX));
    p = popup(); CHECK(p.first == 0 && settings_scroll == before);
    CHECK(gui_desktop_key(0x29, false));
    CHECK(settings_open == -1 && gui_theme_get()->scale_percent == 150);
    CHECK(gui_desktop_key(0x28, false)); CHECK(gui_desktop_key(0x4D, false));
    CHECK(gui_desktop_key(0x28, false));
    CHECK(gui_theme_get()->scale_percent == 300);
    check_focus_visible();
}

static void check_clipped_paint(void)
{
    enum { W=641, H=481, P=657, G=32, N=G+P*H+G };
    const uint32_t sentinel = 0xAA182E44;
    uint32_t *memory = malloc(N * sizeof(uint32_t));
    uint32_t *before = malloc(N * sizeof(uint32_t));
    CHECK(memory && before);
    gui_surface_t surface = {memory+G, W, H, P};
    const int positions[][2] = {{40, 50}, {-30, 50}, {530, 50}, {20, -90}, {20, 430}, {-600, -500}};
    for (int scale = 100; scale <= 300; scale += 25) {
        if (scale == 225 || scale == 275) continue;
        setup(scale, 300, 180);
        for (unsigned pos = 0; pos < sizeof(positions)/sizeof(positions[0]); pos++) {
            demo_windows[1].x = positions[pos][0]; demo_windows[1].y = positions[pos][1];
            for (int scroll = 0; scroll < 2; scroll++) {
                settings_scroll = scroll ? INT_MAX : 0;
                desktop_settings_layout_t l = desktop_settings_layout();
                for (int i = 0; i < N; i++) memory[i] = sentinel;
                desktop_settings_render(&surface);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < P; x++) {
                        if (x >= W || !gui_rect_contains(l.body, x, y))
                            CHECK(surface.pixels[y*P+x] == sentinel);
                        if (scroll && x < W && y < l.viewport.y)
                            CHECK(surface.pixels[y*P+x] == before[G+y*P+x]);
                    }
                }
                for (int i = 0; i < G; i++) {
                    CHECK(memory[i] == sentinel); CHECK(memory[G+P*H+i] == sentinel);
                }
                if (!scroll) memcpy(before, memory, N * sizeof(uint32_t));
            }
        }
    }
    free(before); free(memory);
}

static void preview(const char *path)
{
    enum { W=960, H=720 };
    gui_theme_options_t options = {GUI_LAYOUT_DOCK, 125, true, true};
    gui_theme_configure(&options); gui_desktop_init(W, H);
    gui_desktop_show_window(1);
    demo_windows[1] = (gui_win_desc_t){470, 125, 440, 430, "System", GUI_SURFACE, false};
    CHECK(gui_desktop_key(0x4D, false));
    uint32_t *pixels = calloc(W * H, sizeof(uint32_t)); CHECK(pixels);
    gui_surface_t s = {pixels, W, H, W};
    gui_desktop_render(&s);
    FILE *file = fopen(path, "wb"); CHECK(file);
    fprintf(file, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
        CHECK(fwrite(rgb, 1, 3, file) == 3);
    }
    CHECK(!fclose(file)); free(pixels);
}

int main(int argc, char **argv)
{
    check_layout_and_navigation();
    check_activation_and_scrollbar();
    check_thumb_capture();
    check_popup_wheel();
    check_clipped_paint();
    if (argc > 1) preview(argv[1]);
    printf("GUI responsive: %u checks passed\n", checks);
    return 0;
}
