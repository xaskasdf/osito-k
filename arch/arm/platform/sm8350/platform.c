/*
 * SM8350 (ROG Phone 5) platform initialization
 */
#include "../../include/hal.h"
#include "../../include/types.h"
#include "sm8350.h"
#include "mmu.h"
#include "usb.h"

/* Page allocator range. DDR bank 0 ends at 0xB5000000 (device memory node;
 * the next bank starts at 0xC0000000): the old 384 MB size ran 176 MB past
 * it, into the hole. The top 2 MB is the DMA window, reserved by mmu.c. */
#define SM8350_RAM_BASE  0xA8000000UL
#define SM8350_RAM_SIZE  (RAM_END - SM8350_RAM_BASE)    /* 208 MB */

uint64_t platform_ram_base(void) { return SM8350_RAM_BASE; }
uint64_t platform_ram_size(void) { return SM8350_RAM_SIZE; }

/* Pixel rows above this belong to main.c's boot progress bars (to 449) and
 * panic band + cells (500-649); the text console gets the rest of the panel. */
#define CONSOLE_TOP  700

void platform_init(void *dtb) {
    (void)dtb;
    uart_init();
    /* ABL leaves the splash framebuffer scanning out; there is no serial
     * cable, so it doubles as the console. */
    fb_init_platform(SPLASH_FB_BASE, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                     DISPLAY_WIDTH, 32);
    fb_set_text_top(CONSOLE_TOP);
}

void platform_probe_devices(void) {
    usb_probe();
}

void platform_reboot(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3");
    for (;;) __asm__ volatile("wfe");
}

uint64_t platform_gicd_base(void) { return 0x17A00000UL; }
uint64_t platform_gicr_base(void) { return 0x17A60000UL; }
