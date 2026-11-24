/* Production compositor + SHM, with deferred AP work and concurrent replacement. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OSITOK_X86_TYPES_H
#define __hot
#define __cold
#define SHM_HOST_TEST
#define COMPOSITOR_HOST_TEST
#include "../include/paging.h"
#undef PHYS_TO_VIRT
#undef VIRT_TO_PHYS
#define PHYS_TO_VIRT(p) ((void *)(p))
#define VIRT_TO_PHYS(p) ((uint64_t)(uintptr_t)(p))
#include "../kernel/shm.c"
#ifdef COMPOSITOR_TEST_SOURCE
#include COMPOSITOR_TEST_SOURCE
#define render_windows windows
#define release_render_surfaces() ((void)0)
#else
#include "../kernel/compositor.c"
#endif

static unsigned checks, failures, allocations, releases;
#define CHECK(value) do { __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED); \
    if (!(value)) { unsigned n = __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); \
        if (n < 20) fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)
enum { WIDTH = 16, HEIGHT = 640, PITCH = 20, GUARD = 16 };
#define SENTINEL 0xa5e21749u
#define BACKGROUND 0xff204060u
#define OLD_COLOR 0xffff0000u
#define NEW_COLOR 0xff0000ffu
static uint32_t output[GUARD + PITCH * HEIGHT + GUARD];
static uint32_t live_window, old_handle, new_handle;
static bool replace_background, replace_submit, destroy_present, stress_done;
static unsigned submissions, waits, rejected;
int ap_worker_count;
static struct { void (*fn)(void *, void *); void *arg, *result; bool pending; } jobs[3];

static void probe_metadata(void) { (void)compositor_find_window_by_shm(1); }
void serial_puts(const char *s) { (void)s; probe_metadata(); }
void serial_puthex(uint64_t v, int n) { (void)v; (void)n; }
void serial_putdec(uint64_t v) { (void)v; }
uint32_t proc_exec_pid(void) { return 7; }
const char *proc_current_name(void) { return "surface owner"; }
uint32_t proc_count_active(void) { return 1; }
void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    probe_metadata();
    void *pixels = NULL;
    if (posix_memalign(&pixels, alignment, size)) return NULL;
    __atomic_fetch_add(&allocations, 1, __ATOMIC_RELAXED);
    return pixels;
}
void mem_free_pages(void *pixels, uint64_t pages)
{
    (void)pages;
    probe_metadata();
    __atomic_fetch_add(&releases, 1, __ATOMIC_RELAXED);
    free(pixels);
}
uint64_t mem_get_free(void) { return 1u << 28; }
uint64_t mem_get_used(void) { return 1u << 20; }
uint64_t mem_get_total(void) { return 1u << 29; }
uint64_t idt_get_ticks(void) { return 1000; }
uint32_t *display_get_back_buffer(void) { return output + GUARD; }
uint32_t display_get_width(void) { return WIDTH; }
uint32_t display_get_height(void) { return HEIGHT; }
uint32_t display_get_pitch(void) { return PITCH; }
void input_get_cursor(int32_t *x, int32_t *y) { *x = -100; *y = -100; }

void gui_desktop_set_focus(int index) { (void)index; probe_metadata(); }
int gui_desktop_get_focus(void) { return -1; }
void gui_desktop_show_window(int index) { (void)index; probe_metadata(); }
gui_win_desc_t *gui_desktop_get_windows(int *count) { if (count) *count = 0; return NULL; }
int *gui_desktop_get_order(void) { static int order[1]; return order; }
gui_rect_t gui_window_frame(const gui_win_desc_t *window) { (void)window; return (gui_rect_t){0}; }
void gui_desktop_render_window(gui_surface_t *screen, int index) { (void)screen; (void)index; }
void gui_desktop_render_dock(gui_surface_t *screen) { (void)screen; }
void gui_dock_set_tasks(const gui_dock_task_t *tasks, int count) { (void)tasks; (void)count; probe_metadata(); }
int gui_dock_hit(uint32_t width, uint32_t height, int32_t x, int32_t y) { (void)width; (void)height; (void)x; (void)y; return -1; }
void gui_panel_set_debug(const gui_debug_info_t *info) { (void)info; probe_metadata(); }

static void replace_surface(void)
{
    CHECK(compositor_replace_surface(live_window, new_handle, 2, 300));
    CHECK(shm_get_size(old_handle) != 0);
    surface_alpha_t alpha = {.opacity = 0};
    compositor_set_surface_alpha(live_window, &alpha);
    compositor_set_position(live_window, 8, 1);
    compositor_set_clip_rect(live_window, true, 8, 1, 1, 1);
}
void gui_desktop_render_background(gui_surface_t *screen)
{
    for (uint32_t y = 0; y < screen->height; y++) for (uint32_t x = 0; x < screen->width; x++)
        screen->pixels[y * screen->pitch + x] = BACKGROUND;
    if (replace_background) { replace_background = false; replace_surface(); }
}
void display_mark_dirty(void)
{
    probe_metadata();
    if (destroy_present) {
        destroy_present = false;
        compositor_destroy_window(live_window);
        CHECK(shm_get_size(old_handle) != 0);
    }
}
int smp_submit_any(void (*fn)(void *, void *), void *arg, void *result)
{
    unsigned index = submissions++;
    CHECK(index < 3);
    if (replace_submit) { replace_submit = false; replace_surface(); }
    if (index >= 3 || (rejected & (1u << index))) return -1;
    jobs[index].fn = fn; jobs[index].arg = arg; jobs[index].result = result; jobs[index].pending = true;
    return (int)index;
}
void smp_wait(int index)
{
    CHECK(index >= 0 && index < 3 && jobs[index].pending);
    CHECK(shm_get_size(old_handle) != 0);
    jobs[index].fn(jobs[index].arg, jobs[index].result);
    jobs[index].pending = false;
    waits++;
}

static uint32_t surface(unsigned width, unsigned height, uint32_t color)
{
    uint32_t handle = shm_create_surface(width, height, 3);
    uint32_t *pixels = shm_map(handle);
    CHECK(handle && pixels);
    if (pixels) {
        for (unsigned i = 0; i < width * height; i++) pixels[i] = color;
        shm_unmap(handle);
    }
    return handle;
}
static void reset_output(void)
{
    for (unsigned i = 0; i < sizeof(output) / sizeof(*output); i++) output[i] = SENTINEL;
    submissions = waits = 0;
    memset(jobs, 0, sizeof(jobs));
}
static void check_image(unsigned x, unsigned y, unsigned width, unsigned height,
                        uint32_t color, uint32_t background, bool padding)
{
    bool matches = true;
    for (unsigned row = 0; row < HEIGHT; row++) for (unsigned col = 0; col < PITCH; col++) {
        uint32_t expected = col >= WIDTH ? (padding ? SENTINEL : background) :
            col >= x && col - x < width && row >= y && row - y < height ? color : background;
        if (output[GUARD + row * PITCH + col] != expected) matches = false;
    }
    CHECK(matches);
    for (unsigned i = 0; i < GUARD; i++)
        CHECK(output[i] == SENTINEL && output[GUARD + PITCH * HEIGHT + i] == SENTINEL);
}
static void start_window(unsigned width, unsigned height)
{
    old_handle = surface(width, height, OLD_COLOR);
    live_window = compositor_create_window_inactive(old_handle, 0, 0, width, height, 7, "old surface");
    CHECK(live_window);
    shm_destroy(old_handle);
}
static void replace_before_draw(void)
{
    start_window(4, 300);
    new_handle = surface(2, 300, NEW_COLOR);
    reset_output();
    build_render_order();
    replace_surface();
    compositor_render_desktop_scene(output + GUARD, WIDTH, HEIGHT, PITCH);
    check_image(0, 0, 4, 300, OLD_COLOR, BACKGROUND, true);
    release_render_surfaces();
    CHECK(!shm_get_size(old_handle));
    compositor_destroy_window(live_window);
    shm_destroy(new_handle);
    CHECK(!shm_get_active_count());
}
static void destroy_and_reuse_slot(void)
{
    start_window(4, 20);
    reset_output();
    build_render_order();
    compositor_destroy_window(live_window);
    CHECK(shm_get_size(old_handle) != 0);
    new_handle = surface(2, 300, NEW_COLOR);
    uint32_t replacement = compositor_create_window_inactive(new_handle, 8, 1, 2, 300, 8, "new window");
    CHECK(replacement && replacement != live_window);
    compositor_render_desktop_scene(output + GUARD, WIDTH, HEIGHT, PITCH);
    check_image(0, 0, 4, 20, OLD_COLOR, BACKGROUND, true);
    release_render_surfaces();
    CHECK(!shm_get_size(old_handle));
    compositor_destroy_window(replacement); shm_destroy(new_handle);
    CHECK(!shm_get_active_count());
}
static void frame_contract(unsigned mode, unsigned mask)
{
    unsigned before = failures;
    unsigned width = mode == 1 ? WIDTH : mode == 3 ? 7 : 4;
    unsigned height = mode == 1 ? HEIGHT : mode == 3 ? 13 : 300;
    start_window(width, height);
    new_handle = surface(2, 300, NEW_COLOR);
    reset_output();
    rejected = mask;
    ap_worker_count = 3;
    if (mode == 0) {
        surface_alpha_t alpha = {.opacity = 128};
        compositor_set_surface_alpha(live_window, &alpha);
        replace_background = true;
    } else if (mode == 4) replace_submit = true;
    else {
        compositor_set_fullscreen(live_window, true);
        if (mode == 3) compositor_set_user32_managed(live_window, true);
        if (mode == 2) replace_submit = true;
        else destroy_present = true;
    }
    compositor_request_frame();
    CHECK(compositor_render_frame());
    CHECK(!render_count && !replace_background && !replace_submit && !destroy_present);
    for (unsigned i = 0; i < 3; i++) CHECK(!jobs[i].pending);
    if (mode == 0) check_image(0, 0, 4, 300, 0xff902030u, BACKGROUND, true);
    if (mode == 1) check_image(0, 0, WIDTH, HEIGHT, OLD_COLOR, 0, true);
    if (mode == 2) check_image(4, 20, 8, 600, OLD_COLOR, 0, false);
    if (mode == 3) check_image(0, 305, 16, 29, OLD_COLOR, 0, false);
    if (mode == 4) check_image(0, 0, 4, 300, OLD_COLOR, BACKGROUND, true);
    CHECK(!shm_get_size(old_handle));
    compositor_destroy_window(live_window); shm_destroy(new_handle);
    CHECK(!shm_get_active_count());
    if (failures != before) fprintf(stderr, "frame mode=%u rejected=%u\n", mode, mask);
}
static void *replace_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 500; i++) {
        uint32_t width = (i & 1) ? 4 : 2;
        uint32_t handle = surface(width, 300, i & 1 ? OLD_COLOR : NEW_COLOR);
        CHECK(compositor_replace_surface(live_window, handle, width, 300));
        shm_destroy(handle);
        compositor_set_title(live_window, "concurrent");
        compositor_set_size(live_window, width, 300);
        sched_yield();
    }
    __atomic_store_n(&stress_done, true, __ATOMIC_RELEASE);
    return NULL;
}
static void concurrent_contract(void)
{
    start_window(4, 300);
    ap_worker_count = 0;
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, replace_worker, NULL));
    do {
        build_render_order();
        CHECK(render_count == 1);
        if (render_count) {
            const window_t *snapshot = &render_windows[render_order[0]];
            uint32_t expected = snapshot->width == 4 ? OLD_COLOR : NEW_COLOR;
            bool matches = true;
            for (unsigned i = 0; i < (unsigned)snapshot->width * snapshot->height; i++)
                if (snapshot->pixels[i] != expected) matches = false;
            CHECK(matches);
        }
        (void)hit_test_managed_window(1, 1);
        release_render_surfaces();
    } while (!__atomic_load_n(&stress_done, __ATOMIC_ACQUIRE));
    CHECK(!pthread_join(thread, NULL));
    compositor_destroy_window(live_window);
    CHECK(!shm_get_active_count());
}
#ifndef COMPOSITOR_TEST_SOURCE
static void acquired_surface_contract(void)
{
    compositor_surface_ref_t surface;
    CHECK(!compositor_acquire_surface(0, NULL));
    CHECK(!compositor_acquire_surface(0, &surface));
    CHECK(!surface.handle && !surface.pixels);
    start_window(4, 20);
    compositor_set_size(live_window, 2, 10);
    compositor_set_visible(live_window, false);
    CHECK(compositor_acquire_surface(live_window, &surface));
    CHECK(surface.handle == old_handle && surface.width == 4 &&
          surface.height == 20 && surface.pitch == 4);
    new_handle = surface.handle;
    compositor_destroy_window(live_window);
    CHECK(shm_get_size(old_handle) != 0);
    for (unsigned i = 0; i < 80; i++) CHECK(surface.pixels[i] == OLD_COLOR);
    compositor_surface_ref_t missing;
    CHECK(!compositor_acquire_surface(live_window, &missing));
    CHECK(!missing.handle && !missing.pixels);
    compositor_release_surface(&surface);
    CHECK(!surface.handle && !surface.pixels && !surface.width &&
          !surface.height && !surface.pitch);
    CHECK(!shm_get_size(old_handle));
    compositor_release_surface(&surface);
    compositor_release_surface(NULL);
    CHECK(!shm_get_active_count());
}
#endif

int main(int argc, char **argv)
{
    (void)argv;
    shm_init();
    replace_before_draw();
    if (argc == 1) {
        destroy_and_reuse_slot();
        for (unsigned mode = 0; mode < 5; mode++)
            for (unsigned mask = 0; mask < 8; mask++) frame_contract(mode, mask);
        concurrent_contract();
#ifndef COMPOSITOR_TEST_SOURCE
        acquired_surface_contract();
#endif
        CHECK(!compositor_create_window_inactive(0, 0, 0, 2, 2, 7, "invalid"));
        uint32_t small = surface(1, 1, OLD_COLOR);
        CHECK(!compositor_create_window_inactive(small, 0, 0, 64, 64, 7, "too large"));
        CHECK(!compositor_replace_surface(0xbad, small, 1, 1));
        shm_destroy(small);
    }
    CHECK(allocations == releases && !shm_get_active_count());
    printf("Compositor surface lifetime: %u checks, %u failures; allocated=%u freed=%u\n",
           checks, failures, allocations, releases);
    return failures ? 1 : 0;
}
