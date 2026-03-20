/*
 * SM8350 (ROG Phone 5) platform initialization
 */
#include "../../include/hal.h"
#include "../../include/types.h"
#include "sm8350.h"

static uint8_t *page_bump = (uint8_t *)0xA8000000UL;
static uint8_t *page_end  = (uint8_t *)0xC0000000UL;

void platform_init(void *dtb) {
    (void)dtb;
    uart_init();
}

void platform_reboot(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ULL;
    __asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3");
    for (;;) __asm__ volatile("wfe");
}

void *mem_alloc_pages(uint64_t count) {
    uint64_t size = count * 4096;
    uint8_t *result = page_bump;
    if (result + size > page_end) return (void *)0;
    page_bump += size;
    memset(result, 0, size);
    return result;
}

void mem_free_pages(void *addr, uint64_t count) {
    (void)addr; (void)count;
}

uint64_t platform_gicd_base(void) { return 0x17A00000UL; }
uint64_t platform_gicr_base(void) { return 0x17A60000UL; }
