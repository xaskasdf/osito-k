/*
 * platform.c -- QEMU virt platform init + bump page allocator
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "virt.h"

/* ========================================================================
 * RAM info (used by kernel/memory.c)
 * ======================================================================== */

uint64_t platform_ram_base(void) { return RAM_BASE; }
uint64_t platform_ram_size(void) { return RAM_SIZE; }

/* ========================================================================
 * Platform init
 * ======================================================================== */

void platform_init(void *dtb)
{
    (void)dtb;
    uart_init();
}

void platform_reboot(void)
{
    /* PSCI SYSTEM_RESET via SMC */
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3");

    /* Should not reach here */
    for (;;)
        __asm__ volatile("wfe");
}

/* ========================================================================
 * GIC base addresses
 * ======================================================================== */

uint64_t platform_gicd_base(void)
{
    return GICD_BASE;
}

uint64_t platform_gicr_base(void)
{
    return GICR_BASE;
}
