/*
 * hal.h -- Hardware Abstraction Layer for Osito-K AArch64
 *
 * Common function declarations shared by all platforms (virt, SM8350).
 * Each platform provides its own implementation of these functions.
 */

#ifndef OSITO_ARM_HAL_H
#define OSITO_ARM_HAL_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * Platform init
 * ======================================================================== */
void platform_init(void *dtb);
void platform_reboot(void);

/* ========================================================================
 * UART (platform-specific low-level)
 * ======================================================================== */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_rx_ready(void);
char uart_getc(void);
int  uart_trygetc(void);

/* ========================================================================
 * Serial (common wrappers with \n -> \r\n conversion)
 * ======================================================================== */
void serial_init(void);
void serial_putc(char c);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_puthex(uint64_t val, int digits);
void serial_putdec(uint64_t val);
char serial_getc(void);

/* ========================================================================
 * Framebuffer
 * ======================================================================== */
void fb_init_platform(uint64_t base, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t bpp);
void fb_clear(void);
void fb_putc(char c);
void fb_puts(const char *s);
void fb_puts_color(const char *s, uint32_t color);
void fb_putdec(uint64_t val);

/* ========================================================================
 * Platform info
 * ======================================================================== */
uint64_t platform_ram_base(void);
uint64_t platform_ram_size(void);

/* ========================================================================
 * Memory manager (kernel/memory.c)
 * ======================================================================== */
void     mem_init(uint64_t base, uint64_t size);
void    *mem_alloc_pages(uint64_t count);
void    *mem_alloc_aligned(uint64_t size, uint64_t alignment);
void     mem_free_pages(void *ptr, uint64_t count);
int      mem_reserve_range(uint64_t phys, uint64_t count);
uint64_t mem_get_total(void);
uint64_t mem_get_free(void);
uint64_t mem_get_used(void);

/* ========================================================================
 * Paging / MMU (kernel/paging.c)
 * ======================================================================== */
void     paging_init(void);
int      paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
int      paging_unmap_page(uint64_t virt);
int      paging_map_mmio(uint64_t phys, uint64_t size);
uint64_t paging_get_kernel_ttbr0(void);

/* ========================================================================
 * Heap
 * ======================================================================== */
void  heap_init(void);
void *kmalloc(uint64_t size);
void  kfree(void *ptr);
void *kcalloc(uint64_t count, uint64_t size);
void *krealloc(void *ptr, uint64_t new_size);
uint64_t heap_get_used(void);
uint64_t heap_get_total(void);

/* ========================================================================
 * GIC (Generic Interrupt Controller)
 * ======================================================================== */
void     gic_init(void);
uint32_t gic_ack_irq(void);
void     gic_end_irq(uint32_t irq);
void     gic_send_sgi_self(uint32_t sgi_id);

uint64_t platform_gicd_base(void);
uint64_t platform_gicr_base(void);

/* ========================================================================
 * Timer
 * ======================================================================== */
void     timer_init(uint32_t hz);
uint64_t timer_get_ticks(void);
uint64_t timer_ms(void);
uint64_t timer_get_tick_count(void);
void     udelay(uint32_t us);
void     mdelay(uint32_t ms);
void     timer_tick_handler(void);

/* ========================================================================
 * Scheduler
 * ======================================================================== */
typedef void (*task_func_t)(void *arg);

void sched_init(void);
int  task_create(const char *name, task_func_t func, void *arg, int priority);
void task_yield(void);
void task_delay_ms(uint32_t ms);
void sched_start(void);

/* ========================================================================
 * Shell
 * ======================================================================== */
void shell_run(void);

/* ========================================================================
 * Terminal
 * ======================================================================== */
void term_init(void);
int  term_readline(const char *prompt, char *buf, int bufsize);

#endif /* OSITO_ARM_HAL_H */
