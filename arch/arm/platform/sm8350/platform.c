/*
 * SM8350 (ROG Phone 5) platform initialization
 */
#include "../../include/hal.h"
#include "../../include/types.h"
#include "sm8350.h"

#define SM8350_RAM_BASE  0xA8000000UL
#define SM8350_RAM_SIZE  0x18000000UL   /* 384 MB usable range */

uint64_t platform_ram_base(void) { return SM8350_RAM_BASE; }
uint64_t platform_ram_size(void) { return SM8350_RAM_SIZE; }

void platform_init(void *dtb) {
    (void)dtb;
    uart_init();
}

void platform_reboot(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3");
    for (;;) __asm__ volatile("wfe");
}

uint64_t platform_gicd_base(void) { return 0x17A00000UL; }
uint64_t platform_gicr_base(void) { return 0x17A60000UL; }
