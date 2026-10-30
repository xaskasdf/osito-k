/* Regression: native focus must agree with pixels and pointer ownership. */
#define COMPOSITOR_HOST_TEST
#include "../kernel/compositor.c"

extern int printf(const char *, ...);
static int32_t pointer_x, pointer_y;
static unsigned checks, failures;
static uint32_t back[800 * 600], app_pixels[600 * 400], child_pixels[100 * 100];
static unsigned pinned;
int ap_worker_count;
void input_get_cursor(int32_t *x, int32_t *y) { *x = pointer_x; *y = pointer_y; }
uint32_t display_get_width(void) { return 800; }
uint32_t display_get_height(void) { return 600; }
void serial_puts(const char *s) { (void)s; }
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int digits) { (void)n; (void)digits; }
void shm_unmap(uint32_t handle) { (void)handle; pinned--; }
void *shm_map(uint32_t handle)
{
    if (handle == 10) { pinned++; return app_pixels; }
    if (handle == 11) { pinned++; return child_pixels; }
    return NULL;
}
int smp_submit_any(void (*fn)(void *, void *), void *arg, void *result)
{ (void)fn; (void)arg; (void)result; return -1; }
void smp_wait(int id) { (void)id; }

#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL %d: %s\n", __LINE__, #test); failures++; } } while (0)

static void check_scroll_capture(gui_win_desc_t *native)
{
    native[1] = (gui_win_desc_t){350, 90, 360, 190, "System", GUI_SURFACE, false};
    focus_demo_window(1);
    pointer_x=400; pointer_y=230; comp_button_state=0;
    process_mouse_input();
    CHECK(gui_desktop_scroll(1, pointer_x, pointer_y, 32767));
    pointer_x=695; pointer_y=195; comp_button_state=1;
    process_mouse_input();
    CHECK(gui_desktop_pointer_captured());
    CHECK(!dragging && !managed_pointer_active);
    comp_last_cx=pointer_x; comp_last_cy=pointer_y; comp_frame_dirty=false;
    process_mouse_input();
    CHECK(gui_desktop_pointer_captured() && !comp_frame_dirty);
    CHECK(hit_test_managed_window(150, 330) == 0);
    CHECK(compositor_pointer_blocked_by_desktop(150, 330));
    pointer_x=150; pointer_y=599;
    comp_wheel_accum=3; compositor_route_wheel(-1);
    CHECK(compositor_get_wheel_delta() == 3);
    process_mouse_input();
    CHECK(gui_desktop_pointer_captured() && focused_demo_idx == 1);
    CHECK(comp_frame_dirty);
    CHECK(focused_window == -1 && !managed_pointer_active);
    CHECK(native[1].x == 350 && native[1].y == 90);
    CHECK(windows[0].x == 100 && windows[0].y == 80);
    comp_button_state=0; process_mouse_input();
    CHECK(!gui_desktop_pointer_captured());
    CHECK(focused_demo_idx == 1 && focused_window == -1);
    CHECK(!compositor_pointer_blocked_by_desktop(150, 330));
    pointer_x=150; pointer_y=330; comp_button_state=1;
    process_mouse_input();
    CHECK(focused_window == 0 && gui_desktop_get_focus() == -1);
    comp_button_state=0; process_mouse_input();
    CHECK(!managed_pointer_active && !gui_desktop_pointer_captured());
}

int main(void)
{
    gui_theme_options_t options = {GUI_LAYOUT_DOCK, 100, false, true};
    gui_theme_configure(&options);
    gui_desktop_init(800, 600);
    gui_win_desc_t *native = gui_desktop_get_windows(NULL);
    native[0] = (gui_win_desc_t){60, 60, 440, 330, "Terminal", 0xFF172F24, false};
    native[1].hidden = true;
    for (unsigned i = 0; i < 600 * 400; i++) app_pixels[i] = 0xFF352346;
    for (unsigned i = 0; i < 100 * 100; i++) child_pixels[i] = 0xFF783D22;
    windows[0] = (window_t){ .id=10, .shm_handle=10, .x=100, .y=80, .width=600, .height=400,
        .surface_pitch=600, .surface_height=400, .pixels=app_pixels, .owner_pid=7,
        .z_order=1000, .alpha={.opacity=255}, .flags=WND_ACTIVE|WND_VISIBLE|WND_TASKBAR };
    windows[1] = (window_t){ .id=11, .shm_handle=11, .x=200, .y=180, .width=100, .height=100,
        .surface_pitch=100, .surface_height=100, .pixels=child_pixels, .owner_pid=7,
        .z_order=1001, .alpha={.opacity=255}, .flags=WND_ACTIVE|WND_VISIBLE };
    compositor_running = true;
    build_render_order();
    compositor_select_focus(0);
    compositor_render_desktop_scene(back, 800, 600, 800);
    CHECK(gui_desktop_get_focus() == -1);
    CHECK(back[200 * 800 + 220] == 0xFF783D22);
    CHECK(hit_test_managed_window(220, 200) == 1);
    CHECK(!compositor_pointer_blocked_by_desktop(220, 200));

    focus_demo_window(0);
    compositor_render_desktop_scene(back, 800, 600, 800);
    CHECK(focused_window == -1 && gui_desktop_get_focus() == 0);
    CHECK(back[200 * 800 + 220] == native[0].content_color);
    CHECK(hit_test_managed_window(220, 200) == -1);
    CHECK(compositor_pointer_blocked_by_desktop(220, 200));
    CHECK(hit_test_managed_window(600, 300) == 0);
    CHECK(!compositor_pointer_blocked_by_desktop(600, 300));

    /* A drag begins on the now-visible title, crosses an app, then releases. */
    process_mouse_input(); /* settle initial layout state */
    native[0].x = 60; native[0].y = 60;
    pointer_x=150; pointer_y=75; comp_button_state=1;
    process_mouse_input();
    CHECK(dragging && !managed_pointer_active);
    pointer_x=210; pointer_y=115;
    process_mouse_input();
    CHECK(native[0].x == 120 && native[0].y == 100);
    CHECK(windows[0].x == 100 && windows[0].y == 80);
    CHECK(compositor_pointer_blocked_by_desktop(650, 400));
    comp_button_state=0; process_mouse_input();
    CHECK(!dragging);
    compositor_render_desktop_scene(back, 800, 600, 800);
    CHECK(back[200 * 800 + 220] == native[0].content_color);

    /* Clicking the exposed app returns both pixels and input to that app. */
    pointer_x=650; pointer_y=400; comp_button_state=1;
    process_mouse_input();
    CHECK(focused_window == 0 && gui_desktop_get_focus() == -1);
    comp_button_state=0; process_mouse_input();
    compositor_render_desktop_scene(back, 800, 600, 800);
    CHECK(back[200 * 800 + 220] == 0xFF783D22);
    CHECK(hit_test_managed_window(220, 200) == 1);
    gui_dock_layout_t dock = gui_dock_geometry(800, 600);
    CHECK(compositor_pointer_blocked_by_desktop(dock.bounds.x+4, dock.bounds.y+4));

    /* Hidden foreground native windows must not occlude clients or steal input. */
    focus_demo_window(0); native[0].hidden = true;
    compositor_render_desktop_scene(back, 800, 600, 800);
    CHECK(gui_desktop_get_focus() == -1);
    CHECK(back[200 * 800 + 220] == 0xFF783D22);
    CHECK(hit_test_managed_window(220, 200) == 1);
    /* Only the focused native Appearance pane consumes its own wheel input. */
    native[1] = (gui_win_desc_t){350, 90, 360, 190, "System", GUI_SURFACE, false};
    focus_demo_window(1);
    pointer_x=400; pointer_y=230;
    comp_wheel_accum=3; comp_frame_dirty=false;
    compositor_route_wheel(-1);
    CHECK(comp_wheel_accum == 3 && comp_frame_dirty);
    CHECK(compositor_get_wheel_delta() == 3);
    CHECK(compositor_get_wheel_delta() == 0);
    pointer_x=120; pointer_y=400;
    compositor_route_wheel(-2);
    CHECK(compositor_get_wheel_delta() == -2);
    pointer_x=400; pointer_y=230;
    compositor_select_focus(0);
    compositor_route_wheel(2);
    CHECK(compositor_get_wheel_delta() == 2);
    focus_demo_window(1); native[1].hidden=true;
    compositor_route_wheel(-1);
    CHECK(compositor_get_wheel_delta() == -1);
    native[1].hidden=false;
    pointer_x=dock.bounds.x+4; pointer_y=dock.bounds.y+4;
    compositor_route_wheel(1);
    CHECK(compositor_get_wheel_delta() == 1);
    pointer_x=400; pointer_y=10;
    compositor_route_wheel(-1);
    CHECK(compositor_get_wheel_delta() == -1);
    check_scroll_capture(native);
    release_render_surfaces();
    CHECK(pinned == 0);
    printf("GUI compositor focus: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
