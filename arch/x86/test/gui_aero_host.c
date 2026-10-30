/* Public GUI contracts and previews; runs the real shared software renderer. */
#include "../../../gui/gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

extern bool gui_test_fail_alloc;
static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static void check_metrics(void)
{
    gui_theme_options_t options = { GUI_LAYOUT_DOCK, 0, true, true };
    gui_theme_configure(&options);
    gui_theme_set_display(1920, 1080);
    gui_theme_set_physical_size(344, 194);
    CHECK(gui_theme_metrics()->physical_size_valid);
    CHECK(gui_theme_metrics()->scale_percent == 150);
    CHECK(gui_theme_metrics()->effective_width == 1280);
    gui_theme_set_physical_size(531, 299);
    CHECK(gui_theme_metrics()->scale_percent == 100);
    gui_theme_set_display(3840, 2160);
    gui_theme_set_physical_size(344, 194);
    CHECK(gui_theme_metrics()->scale_percent == 300);
    gui_theme_set_physical_size(0, 0);
    CHECK(!gui_theme_metrics()->physical_size_valid);
    CHECK(gui_theme_metrics()->scale_percent == 200);
    options.scale_percent = 125;
    gui_theme_configure(&options);
    gui_theme_set_display(1920, 1080);
    CHECK(gui_theme_metrics()->effective_width == 1536);
    CHECK(gui_theme_metrics()->effective_height == 864);
    CHECK(gui_px(-10) == -gui_px(10));
    options.scale_percent = 300;
    gui_theme_configure(&options);
    gui_theme_set_display(800, 600);
    CHECK(gui_theme_metrics()->effective_width >= 640);
    CHECK(gui_theme_metrics()->effective_height >= 420);
    gui_theme_set_display(1920, 1080);
    options.scale_percent = 0; gui_theme_configure(&options);
    uint8_t edid[128] = {0, 255, 255, 255, 255, 255, 255, 0};
    edid[21] = 34; edid[22] = 19;
    unsigned sum = 0;
    for (unsigned i = 0; i < 127; i++) sum += edid[i];
    edid[127] = (uint8_t)(0u - sum);
    CHECK(gui_theme_from_edid(edid, sizeof(edid)));
    CHECK(gui_theme_metrics()->physical_ppi >= 140 && gui_theme_metrics()->physical_ppi <= 146);
    edid[17] ^= 1;
    CHECK(!gui_theme_from_edid(edid, sizeof(edid)));
    CHECK(!gui_theme_from_edid(edid, 127));
    CHECK(!gui_theme_from_edid(NULL, 128));
    gui_theme_set_physical_size(10, 10000);
    CHECK(!gui_theme_metrics()->physical_size_valid);
    gui_theme_set_physical_size(0, 0);
}

static void check_text(void)
{
    static const int scales[] = {100, 125, 150, 175, 200, 250, 300};
    enum { W = 400, H = 120, P = 413, G = 32 };
    uint32_t *memory = malloc((G + P * H + G) * sizeof(uint32_t));
    CHECK(memory != NULL);
    gui_surface_t s = { memory + G, W, H, P };
    for (unsigned scale = 0; scale < sizeof(scales) / sizeof(scales[0]); scale++) {
        gui_theme_options_t options = { GUI_LAYOUT_DOCK, (uint16_t)scales[scale], true, true };
        gui_theme_set_display(3840, 2160); gui_theme_configure(&options);
        CHECK(gui_ui_text_width("WWW") > gui_ui_text_width("iii"));
        CHECK(gui_ui_text_width("Espa\xC3\xB1" "ol: \xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA") > 0);
        CHECK(gui_ui_text_width("\xF0\x80") > 0);
        for (unsigned i = 0; i < G + P * H + G; i++) memory[i] = 0xFF18232E;
        gui_ui_text(&s, 30, 15, "j\xC3\xA1\xC3\xB1 Hello, world", GUI_INK, 130);
        for (unsigned y = 0; y < H; y++)
            for (unsigned x = 0; x < P; x++)
                if (x < 30 || x >= 160) CHECK(s.pixels[y * P + x] == 0xFF18232E);
        for (unsigned i = 0; i < G; i++) {
            CHECK(memory[i] == 0xFF18232E);
            CHECK(memory[G + P * H + i] == 0xFF18232E);
        }
        gui_ui_text_fit(&s, (gui_rect_t){20, 20, 180, 80},
                         "A very long title with accented text: \xC3\xA1\xC3\xB1", GUI_INK, false);
    }
    free(memory);
}

static void check_hit_geometry(void)
{
    const unsigned sizes[][2] = {{640,480}, {800,600}, {1280,720}, {1920,1080}, {3840,2160}};
    gui_dock_task_t tasks[GUI_DOCK_MAX_TASKS];
    for (unsigned i = 0; i < GUI_DOCK_MAX_TASKS; i++)
        tasks[i] = (gui_dock_task_t){i + 10, GUI_SELECTION, (char)('A' + i % 26), i == 0, false};
    gui_dock_set_tasks(tasks, GUI_DOCK_MAX_TASKS);
    for (unsigned mode = 0; mode < 2; mode++) {
        for (unsigned sz = 0; sz < sizeof(sizes) / sizeof(sizes[0]); sz++) {
            gui_theme_options_t options = { mode ? GUI_LAYOUT_TASKBAR : GUI_LAYOUT_DOCK, 200, true, true };
            gui_theme_set_physical_size(0, 0); gui_theme_configure(&options);
            gui_desktop_init(sizes[sz][0], sizes[sz][1]);
            gui_dock_page(10000);
            int seen[GUI_DOCK_MAX_TASKS + GUI_DOCK_BUILTIN_COUNT] = {0};
            int pages = gui_dock_page_count();
            for (int page = 0; page < pages; page++) {
                gui_dock_layout_t d = gui_dock_geometry(sizes[sz][0], sizes[sz][1]);
                CHECK(d.bounds.x >= 0 && d.bounds.x + d.bounds.w <= (int)sizes[sz][0]);
                CHECK(d.bounds.y >= 0 && d.bounds.y + d.bounds.h <= (int)sizes[sz][1]);
                for (int i = d.first; i < d.first + d.count; i++) {
                    gui_rect_t r = gui_dock_item_rect(&d, i);
                    CHECK(gui_dock_hit(sizes[sz][0], sizes[sz][1], r.x + r.w / 2, r.y + r.h / 2) == i);
                    CHECK(gui_rect_contains(d.bounds, r.x, r.y));
                    CHECK(gui_rect_contains(d.bounds, r.x + r.w - 1, r.y + r.h - 1));
                    seen[i]++;
                }
                if (d.paged) {
                    CHECK(gui_dock_hit(sizes[sz][0], sizes[sz][1], d.previous.x + 2, d.previous.y + 2) == -3);
                    CHECK(gui_dock_hit(sizes[sz][0], sizes[sz][1], d.next.x + 2, d.next.y + 2) == -4);
                }
                gui_dock_page(1);
            }
            for (int i = 0; i < gui_dock_item_count(); i++) CHECK(seen[i] == 1);
            gui_win_desc_t w = {gui_px(20), gui_px(30), gui_px(500), gui_px(200), "Window", GUI_SURFACE, false};
            for (int button = 1; button <= 3; button++) {
                gui_rect_t r = gui_window_button_rect(&w, button);
                CHECK(gui_window_button_hit(&w, r.x + r.w / 2, r.y + r.h / 2) == button);
                CHECK(gui_rect_contains(gui_window_frame(&w), r.x, r.y));
            }
            w.hidden = true;
            gui_rect_t close = gui_window_button_rect(&w, 1);
            CHECK(gui_window_button_hit(&w, close.x, close.y) == 0);
        }
    }
    /* Phablet windows are always maximized: close, and minimize beside it */
    gui_theme_options_t phablet = { GUI_LAYOUT_PHABLET, 300, true, true };
    gui_theme_set_physical_size(0, 0); gui_theme_configure(&phablet);
    gui_desktop_init(1080, 2448);
    gui_win_desc_t w = {0, gui_px(30), 1080, gui_px(600), "Window", GUI_SURFACE, false};
    gui_rect_t close = gui_window_button_rect(&w, 1), minimize = gui_window_button_rect(&w, 2);
    for (int button = 1; button <= 2; button++) {
        gui_rect_t r = gui_window_button_rect(&w, button);
        CHECK(gui_window_button_hit(&w, r.x + r.w / 2, r.y + r.h / 2) == button);
    }
    CHECK(gui_window_button_rect(&w, 3).w == 0);
    CHECK(minimize.x + minimize.w < close.x && close.x - minimize.x - minimize.w <= gui_px(2));
    gui_dock_set_tasks(NULL, 0);
}

static void check_render_bounds(void)
{
    enum { W = 641, H = 481, P = 657, G = 32 };
    size_t words = G + P * H + G;
    uint32_t *memory = malloc(words * sizeof(uint32_t));
    CHECK(memory != NULL);
    gui_surface_t surface = { memory + G, W, H, P };
    gui_theme_options_t options = { GUI_LAYOUT_DOCK, 100, true, true };
    gui_theme_configure(&options);
    gui_theme_set_physical_size(0, 0);
    gui_desktop_init(W, H);
    for (int frame = 0; frame < 3; frame++) {
        for (size_t i = 0; i < words; i++) memory[i] = 0xA1B2C3D4;
        gui_test_fail_alloc = frame == 0;
        gui_desktop_render(&surface);
        gui_test_fail_alloc = false;
        for (int n = 0; n < 30; n++) {
            gui_win_desc_t win = {n * 37 % 700 - 80, n * 23 % 530 - 80,
                                  n * 71 % 500, n * 53 % 400, "Long window title", GUI_SURFACE, false};
            gui_window_render(&surface, &win, n & 1);
        }
        for (unsigned y = 0; y < H; y++)
            for (unsigned x = W; x < P; x++) CHECK(surface.pixels[y * P + x] == 0xA1B2C3D4);
        for (unsigned i = 0; i < G; i++) {
            CHECK(memory[i] == 0xA1B2C3D4);
            CHECK(memory[G + P * H + i] == 0xA1B2C3D4);
        }
    }
    free(memory);
}

static void check_settings(void)
{
    gui_theme_options_t options = { GUI_LAYOUT_DOCK, 0, true, true };
    gui_theme_configure(&options);
    gui_theme_set_physical_size(0, 0);
    gui_desktop_init(1280, 800);
    gui_desktop_show_window(1);
    CHECK(gui_desktop_key(0x2B, false));       /* Tab: scale */
    CHECK(gui_desktop_key(0x4F, false));       /* Auto -> 100 */
    CHECK(gui_theme_get()->scale_percent == 100);
    CHECK(gui_desktop_key(0x4F, false));       /* 100 -> 125 */
    CHECK(gui_theme_get()->scale_percent == 125);
    CHECK(gui_desktop_key(0x2B, false));       /* physical diagonal */
    CHECK(gui_desktop_key(0x4F, false));
    CHECK(gui_theme_metrics()->physical_size_valid);
    CHECK(gui_desktop_key(0x2B, false));
    CHECK(gui_desktop_key(0x2C, false));
    CHECK(!gui_theme_get()->glass);
    CHECK(gui_desktop_key(0x2B, false));
    CHECK(gui_desktop_key(0x2C, false));
    CHECK(!gui_theme_get()->reduced_motion);
    CHECK(gui_desktop_key(0x2B, false));       /* wrap to desktop layout */
    CHECK(gui_desktop_key(0x4F, false));
    CHECK(gui_theme_get()->layout == GUI_LAYOUT_TASKBAR);
    CHECK(gui_work_area(1280, 800).y == 0);
    CHECK(gui_desktop_key(0x28, false));      /* open a selector */
    CHECK(gui_desktop_key(0x29, false));      /* dismiss */
    CHECK(!gui_desktop_key(0x04, false));
}

static void write_preview(const char *path, unsigned width, unsigned height,
                           unsigned scale, bool taskbar, bool popup)
{
    gui_theme_options_t options = { taskbar ? GUI_LAYOUT_TASKBAR : GUI_LAYOUT_DOCK,
                                   (uint16_t)scale, true, true };
    gui_theme_configure(&options);
    gui_theme_set_physical_size(0, 0);
    gui_desktop_init(width, height);
    gui_debug_info_t info = { .fps = 60, .mem_used_mb = 768, .mem_free_mb = 7424,
        .mem_total_mb = 8192, .procs = 12, .ticks = 234500, .rtc_hour = 21, .rtc_min = 42 };
    gui_panel_set_debug(&info);
    int count;
    gui_win_desc_t *wins = gui_desktop_get_windows(&count);
    uint32_t tw = (uint32_t)wins[0].w, th = (uint32_t)wins[0].h;
    uint32_t *terminal = malloc((size_t)tw * th * 4);
    uint32_t *pixels = malloc((size_t)width * height * 4);
    CHECK(terminal && pixels);
    gui_surface_t term = {terminal, tw, th, tw};
    gui_fill_rect(&term, 0, 0, tw, th, GUI_TERM_BG);
    const char *lines[] = {"OsitoK", "", "osito> uname", "OsitoK x86_64", "", "osito> help",
        "System:  help  ps  mem  date", "Files:   ls  cd  cat  cp", "Network: ifconfig  dhcp  sshd", "", "osito> _"};
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        gui_draw_text(&term, 16, 16 + (int)i * 22, lines[i], i == 0 ? GUI_FOCUS : GUI_TERM_FG, 0);
    gui_desktop_set_terminal_surface(terminal, tw, th);
    gui_desktop_show_window(1);
    gui_dock_task_t tasks[] = {{10,0xFF396886,'F',false,false},{11,0xFF3B506A,'S',false,true}};
    gui_dock_set_tasks(tasks, 2);
    gui_surface_t screen = {pixels, width, height, width};
    gui_desktop_set_cursor(-1, -1);
    gui_desktop_render(&screen);
    if (popup) {
        gui_desktop_key(0x2B, false);
        gui_desktop_key(0x28, false);
        gui_desktop_render(&screen);
    }
    FILE *out = fopen(path, "wb");
    CHECK(out != NULL);
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        unsigned char rgb[3] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
        CHECK(fwrite(rgb, 1, 3, out) == 3);
    }
    CHECK(fclose(out) == 0);
    gui_desktop_set_terminal_surface(NULL, 0, 0);
    free(terminal); free(pixels);
}

int main(int argc, char **argv)
{
    if (argc == 7) {
        write_preview(argv[1], (unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]),
                      (unsigned)atoi(argv[4]), atoi(argv[5]) != 0, atoi(argv[6]) != 0);
        return 0;
    }
    check_metrics(); check_text(); check_hit_geometry(); check_render_bounds(); check_settings();
    printf("GUI Aero: %u checks passed\n", checks);
    return 0;
}
