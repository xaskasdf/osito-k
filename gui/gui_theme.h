/* Dark glass desktop: all coordinates passed to painters are physical pixels.
 * Convert design measurements once with gui_px(); hit tests use the same rects.
 * Included by gui.h after the surface/window declarations. */
#ifndef OSITO_GUI_THEME_H
#define OSITO_GUI_THEME_H

typedef struct { int32_t x, y, w, h; } gui_rect_t;
/* PHABLET: portrait phones and tablets. Status bar on top, a full-width
 * navigation bar below, one maximized window at a time. */
typedef enum { GUI_LAYOUT_DOCK, GUI_LAYOUT_TASKBAR, GUI_LAYOUT_PHABLET } gui_layout_t;
typedef struct {
    gui_layout_t layout;
    uint16_t scale_percent;       /* 0 = automatic; 100..300 supported */
    bool glass;
    bool reduced_motion;
} gui_theme_options_t;
typedef struct {
    uint32_t width, height;
    uint32_t width_mm, height_mm;
    uint32_t effective_width, effective_height;
    uint16_t scale_percent, physical_ppi;
    bool physical_size_valid;
} gui_metrics_t;

#define GUI_INK            0xFFEAF3FCu
#define GUI_MUTED          0xFFACBDCCu
#define GUI_SURFACE        0xFF18232Eu
#define GUI_SURFACE_RAISED 0xFF263848u
#define GUI_EDGE           0xFF516B80u
#define GUI_FOCUS          0xFF80CFFFu
#define GUI_SELECTION      0xFF285C80u

const gui_theme_options_t *gui_theme_get(void);
const gui_metrics_t *gui_theme_metrics(void);
void gui_theme_configure(const gui_theme_options_t *options);
void gui_theme_set_display(uint32_t width, uint32_t height);
void gui_theme_set_physical_size(uint32_t width_mm, uint32_t height_mm);
bool gui_theme_from_edid(const uint8_t *edid, uint32_t bytes);
int32_t gui_px(int32_t dips);
int32_t gui_panel_height(void);
int32_t gui_nav_height(void);
gui_rect_t gui_work_area(uint32_t width, uint32_t height);
bool gui_rect_contains(gui_rect_t r, int32_t x, int32_t y);

/* Proportional UTF-8 UI face. The legacy terminal cell font is independent. */
int gui_ui_text_width(const char *text);
int gui_ui_text_height(void);
void gui_ui_text(gui_surface_t *s, int32_t x, int32_t y, const char *text,
                 uint32_t color, int32_t max_width);
void gui_ui_text_fit(gui_surface_t *s, gui_rect_t r, const char *text,
                     uint32_t color, bool centered);

enum { GUI_CONTROL_HOVER = 1, GUI_CONTROL_SELECTED = 2,
       GUI_CONTROL_FOCUS = 4, GUI_CONTROL_DISABLED = 8 };
enum { GUI_ICON_TERMINAL, GUI_ICON_SETTINGS, GUI_ICON_CLOSE,
       GUI_ICON_MINIMIZE, GUI_ICON_MAXIMIZE, GUI_ICON_CHEVRON,
       GUI_ICON_PREVIOUS, GUI_ICON_NEXT, GUI_ICON_CHECK, GUI_ICON_CHEVRON_UP };
void gui_soft_shadow(gui_surface_t *s, gui_rect_t r, int blur, int offset_y, unsigned opacity);
void gui_glass_panel(gui_surface_t *s, gui_rect_t r, int radius, bool active);
void gui_outline(gui_surface_t *s, gui_rect_t r, uint32_t color);
void gui_control(gui_surface_t *s, gui_rect_t r, const char *label, unsigned state);
void gui_selector(gui_surface_t *s, gui_rect_t r, const char *label, unsigned state);
void gui_check(gui_surface_t *s, gui_rect_t r, const char *label,
               bool checked, unsigned state);
void gui_ui_icon(gui_surface_t *s, gui_rect_t r, int icon, uint32_t color);
void gui_wallpaper_render(gui_surface_t *s);

/* One layout for popup painting and input. Rows stay inside the owner's visible
 * client area; a scroll rail keeps every option reachable in a short window. */
typedef struct {
    gui_rect_t bounds, items, previous, next;
    int row_height, first, count, total;
} gui_popup_layout_t;
enum { GUI_POPUP_OUTSIDE = -1, GUI_POPUP_CHROME = -2,
       GUI_POPUP_PREVIOUS = -3, GUI_POPUP_NEXT = -4 };
gui_popup_layout_t gui_popup_geometry(gui_rect_t anchor, gui_rect_t limits,
                                      int total, int first, int hot);
gui_rect_t gui_popup_item_rect(const gui_popup_layout_t *layout, int index);
int gui_popup_hit(const gui_popup_layout_t *layout, int32_t x, int32_t y);
void gui_popup_render(gui_surface_t *s, const gui_popup_layout_t *layout,
                       const char *const *labels, int selected, int hot,
                       bool keyboard, int32_t cursor_x, int32_t cursor_y);

gui_rect_t gui_window_frame(const gui_win_desc_t *win);
gui_rect_t gui_window_button_rect(const gui_win_desc_t *win, int button);
int gui_window_button_hit(const gui_win_desc_t *win, int32_t x, int32_t y);

typedef struct {
    gui_rect_t bounds, previous, next;
    int32_t icon_size, gap, start_x;
    int first, count, capacity;
    bool paged;
} gui_dock_layout_t;
gui_dock_layout_t gui_dock_geometry(uint32_t width, uint32_t height);
gui_rect_t gui_dock_item_rect(const gui_dock_layout_t *layout, int index);
/* >=0 item; -1 outside; -2 chrome; -3 previous page; -4 next page. */
int gui_dock_hit(uint32_t width, uint32_t height, int32_t x, int32_t y);
void gui_dock_page(int delta);
int gui_dock_page_count(void);

void gui_desktop_resize(uint32_t width, uint32_t height);
void gui_desktop_render_base(gui_surface_t *screen);
void gui_desktop_render_background(gui_surface_t *screen);
void gui_desktop_render_window(gui_surface_t *screen, int index);
/* Screen-space terminal viewport with scale-aware inner padding. */
gui_rect_t gui_desktop_terminal_rect(void);
void gui_desktop_set_focus(int index);
int gui_desktop_get_focus(void);
void gui_desktop_get_cursor(int32_t *x, int32_t *y);
bool gui_desktop_click(int window, int32_t x, int32_t y);
/* True when the native Appearance pane owns the wheel; positive is upward. */
bool gui_desktop_scroll(int window, int32_t x, int32_t y, int delta);
/* Feed motion and release before other hit testing while a widget captures.
 * update returns true for the final release/cancellation as well. */
bool gui_desktop_pointer_captured(void);
bool gui_desktop_pointer_update(int32_t x, int32_t y, bool left_down);
bool gui_desktop_key(uint32_t hid_key, bool shift);

#endif
