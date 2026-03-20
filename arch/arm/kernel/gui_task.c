/*
 * gui_task.c -- Desktop GUI task for OsitoK AArch64
 *
 * Renders the elementaryOS-inspired desktop to the SM8350 splash framebuffer.
 * Launched from shell via "desktop" command.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "gui.h"
#include "task.h"

/* SM8350 framebuffer constants (from sm8350.h) */
#ifndef SPLASH_FB_BASE
#define SPLASH_FB_BASE  0x0E5000000ULL
#endif
#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH   1080
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT  2448
#endif

void gui_task(void *arg)
{
    (void)arg;

    gui_surface_t screen;
    screen.pixels = (uint32_t *)SPLASH_FB_BASE;
    screen.width  = DISPLAY_WIDTH;
    screen.height = DISPLAY_HEIGHT;
    screen.pitch  = DISPLAY_WIDTH;

    gui_desktop_init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    gui_desktop_render(&screen);

    serial_puts("[GUI ] Desktop rendered to framebuffer\n");

    /* Stay alive — future: refresh on input events */
    for (;;)
        task_delay_ms(1000);
}
