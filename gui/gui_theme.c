#include "gui.h"

static gui_theme_options_t options = { GUI_LAYOUT_DOCK, 0, true, false };
static gui_metrics_t metrics = { 1280, 800, 0, 0, 1280, 800, 100, 0, false };
static const uint16_t scales[] = { 100, 125, 150, 175, 200, 250, 300 };

static void theme_recalculate(void)
{
    uint32_t target = 100;
    bool phablet = options.layout == GUI_LAYOUT_PHABLET;
    /* A phone is ~70 mm across and ~360 dp wide; a desktop needs 640 x 420 */
    uint32_t min_mm = phablet ? 40 : 80, min_w = phablet ? 360 : 640, min_h = phablet ? 600 : 420;
    metrics.physical_size_valid = false;
    metrics.physical_ppi = 0;
    if (metrics.width_mm >= min_mm && metrics.height_mm >= 50 &&
        metrics.width_mm <= 2000 && metrics.height_mm <= 2000) {
        uint32_t x = (uint32_t)((uint64_t)metrics.width * 254 /
                                (metrics.width_mm * 10));
        uint32_t y = (uint32_t)((uint64_t)metrics.height * 254 /
                                (metrics.height_mm * 10));
        /* Reject bogus EDID sizes and non-square-pixel/aspect-ratio reports. */
        if (x >= 50 && x <= 450 && y >= 50 && y <= 450 &&
            x * 100 >= y * 85 && x * 100 <= y * 115) {
            metrics.physical_ppi = (uint16_t)((x + y) / 2);
            metrics.physical_size_valid = true;
            target = (metrics.physical_ppi * 100u + 48) / 96;
        }
    }
    if (!metrics.physical_size_valid) {
        /* Conservative resolution fallback; never present this as measured PPI. */
        uint32_t short_edge = metrics.width < metrics.height ? metrics.width : metrics.height;
        target = short_edge >= 2160 ? 200 : short_edge >= 1800 ? 150 :
                 short_edge >= 1440 ? 125 : 100;
    }
    if (options.scale_percent) target = options.scale_percent;
    unsigned choice = 0;
    for (unsigned i = 1; i < sizeof(scales) / sizeof(scales[0]); i++)
        if (target >= (scales[i - 1] + scales[i]) / 2u) choice = i;
    /* Keep display settings reachable, even after an excessive manual choice. */
    while (choice && ((uint64_t)metrics.width * 100 / scales[choice] < min_w ||
                      (uint64_t)metrics.height * 100 / scales[choice] < min_h))
        choice--;
    metrics.scale_percent = scales[choice];
    metrics.effective_width = metrics.width * 100 / metrics.scale_percent;
    metrics.effective_height = metrics.height * 100 / metrics.scale_percent;
}

int32_t gui_px(int32_t dips)
{
    int64_t value = (int64_t)dips * metrics.scale_percent;
    return (int32_t)((value + (value >= 0 ? 50 : -50)) / 100);
}

const gui_theme_options_t *gui_theme_get(void) { return &options; }
const gui_metrics_t *gui_theme_metrics(void) { return &metrics; }

void gui_theme_configure(const gui_theme_options_t *value)
{
    if (!value) return;
    options = *value;
    if (options.layout != GUI_LAYOUT_TASKBAR && options.layout != GUI_LAYOUT_PHABLET)
        options.layout = GUI_LAYOUT_DOCK;
    if (options.scale_percent && options.scale_percent < 100) options.scale_percent = 100;
    if (options.scale_percent > 300) options.scale_percent = 300;
    theme_recalculate();
}

void gui_theme_set_display(uint32_t width, uint32_t height)
{
    if (!width || !height || width > 16384 || height > 16384) return;
    metrics.width = width;
    metrics.height = height;
    theme_recalculate();
}

void gui_theme_set_physical_size(uint32_t width_mm, uint32_t height_mm)
{
    metrics.width_mm = width_mm;
    metrics.height_mm = height_mm;
    theme_recalculate();
}

bool gui_theme_from_edid(const uint8_t *edid, uint32_t bytes)
{
    static const uint8_t header[8] = { 0, 255, 255, 255, 255, 255, 255, 0 };
    if (!edid || bytes < 128) return false;
    unsigned checksum = 0;
    for (unsigned i = 0; i < 8; i++) if (edid[i] != header[i]) return false;
    for (unsigned i = 0; i < 128; i++) checksum += edid[i];
    if ((checksum & 255) || !edid[21] || !edid[22]) return false;
    /* EDID base block dimensions are whole centimetres; zero is unspecified. */
    gui_theme_set_physical_size((uint32_t)edid[21] * 10, (uint32_t)edid[22] * 10);
    return metrics.physical_size_valid;
}

int32_t gui_panel_height(void)
{
    return options.layout == GUI_LAYOUT_TASKBAR ? 0 : gui_px(30);
}

/* The navigation bar's height in the phablet layout (gui_dock.c) */
int32_t gui_nav_height(void)
{
    return gui_px(64);
}

gui_rect_t gui_work_area(uint32_t width, uint32_t height)
{
    int top = gui_panel_height();
    int bottom = options.layout == GUI_LAYOUT_PHABLET ? gui_nav_height() :
                 gui_px(options.layout == GUI_LAYOUT_DOCK ? 76 : 54);
    gui_rect_t r = { 0, top, (int32_t)width, (int32_t)height - top - bottom };
    if (r.h < 1) r.h = 1;
    return r;
}

bool gui_rect_contains(gui_rect_t r, int32_t x, int32_t y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y &&
           (int64_t)x < (int64_t)r.x + r.w && (int64_t)y < (int64_t)r.y + r.h;
}
