/*
 * platform.c -- QEMU virt platform init + bump page allocator
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "virt.h"

/* ========================================================================
 * Bump page allocator
 *
 * Starts at 64 MB into RAM (0x44000000). Simple bump, no free.
 * ======================================================================== */

#define PAGE_SIZE       4096
#define ALLOC_BASE      (RAM_BASE + 0x04000000UL)   /* 0x44000000 */
#define ALLOC_END       (RAM_BASE + RAM_SIZE)        /* 0x60000000 */

static uint64_t alloc_next = ALLOC_BASE;

void *mem_alloc_pages(uint64_t count)
{
    uint64_t size = count * PAGE_SIZE;
    uint64_t addr = alloc_next;

    if (addr + size > ALLOC_END)
        return (void *)0;

    alloc_next = addr + size;

    /* Zero the pages */
    memset((void *)addr, 0, size);

    return (void *)addr;
}

void mem_free_pages(void *ptr, uint64_t count)
{
    /* Bump allocator: no-op */
    (void)ptr;
    (void)count;
}

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
