#include "screen_capture.h"
#include "../include/boot_info.h"

extern boot_info_t saved_boot_info;
extern uint32_t *fb_get_vram(void);
extern bool display_gop_is_primary(void);
extern void *kmalloc(uint64_t);
extern void kfree(void *);
extern uint64_t idt_get_monotonic_ns(void);
extern void sched_yield(void);

static int gop_view(screen_view_t *out)
{
    const boot_info_t *b = &saved_boot_info;
    if (!display_gop_is_primary()) return SCREEN_UNSUPPORTED;
    if (!b->fb_base || !fb_get_vram()) return SCREEN_UNAVAILABLE;
    unsigned i = b->display_current_mode;
    if (!b->display_mode_count || b->display_mode_count > BOOT_MAX_DISPLAY_MODES ||
        i >= b->display_mode_count) return SCREEN_UNSUPPORTED;
    const boot_display_mode_t *mode = &b->display_modes[i];
    if (mode->width != b->fb_width || mode->height != b->fb_height ||
        mode->pitch != b->fb_pitch) return SCREEN_UNSUPPORTED;
    *out = (screen_view_t){fb_get_vram(), (uint64_t)b->fb_pitch * b->fb_height * 4,
        b->fb_width, b->fb_height, b->fb_pitch, mode->pixel_format};
    return SCREEN_OK;
}

static unsigned long long capture_clock(void)
{ return idt_get_monotonic_ns() / 1000000; }

/* The service checks cancellation before and after this bounded snapshot. */
static int capture_cancelled(void) { return 0; }

const screen_capture_ops_t screen_capture_gop = {
    gop_view, kmalloc, kfree, capture_clock, capture_cancelled, sched_yield
};
