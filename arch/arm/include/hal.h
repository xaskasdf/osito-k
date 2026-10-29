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
/* MMU + caches (called from kernel_main after mem_init) */
void platform_mmu_init(void);
/* Board devices beyond the core (called from kernel_main after the heap) */
void platform_probe_devices(void);
/* Receive a kernel Image from the host and run it (shell `kload`).
 * Returns only on failure. */
int platform_kload(void);

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
void klog_dump(void);       /* replay the console log (shell `dmesg`) */
/* Exclusive use of the console devices (USB gadget, framebuffer) across
 * CPUs, IRQs off; nests on one CPU. serial_* take it themselves. */
uint64_t console_lock(void);
void     console_unlock(uint64_t flags);

/* ========================================================================
 * Framebuffer
 * ======================================================================== */
void fb_init_platform(uint64_t base, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t bpp);
void fb_set_text_top(uint32_t y);
void fb_clear(void);
void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
/* Per-CPU status square in the heartbeat row (SM8350 screen) */
#define FB_CPU_MARK(cpu, color)  fb_fill((cpu) * 70, 420, 60, 30, (color))
void fb_putc(char c);
void fb_puts(const char *s);
void fb_puts_color(const char *s, uint32_t color);
void fb_putdec(uint64_t val);

/* ========================================================================
 * Platform info
 * ======================================================================== */
uint64_t platform_ram_base(void);
uint64_t platform_ram_size(void);
const char *cpu_core_name(uint64_t midr);  /* "Cortex-A55"..., NULL if unknown */

/* ========================================================================
 * SMP (kernel/smp.c)
 * ======================================================================== */
#define MAX_CPUS    8

void smp_boot_aps(int num_cpus, int psci_use_smc);
void smp_release_aps(void);     /* online APs start scheduling (sched_start) */
void smp_stop_aps(void);        /* before handing the machine to another kernel */
void smp_cpu_off(void);         /* this CPU, for good (SGI_STOP) */
int  smp_this_core(void);       /* core number from MPIDR */
int  smp_get_cpu_count(void);
void smp_print_cpus(void);
void smp_bench(int any);        /* shell `cpubench [any]` */
int  smp_cpu_id(void);          /* kernel/sched.c: this CPU's core number */

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
#define SGI_YIELD   0       /* task_yield: reschedule this CPU */
#define SGI_STOP    1       /* smp_stop_aps: turn this CPU off */

void     gic_init(void);            /* distributor + this CPU (boot CPU) */
int      gic_init_cpu(void);        /* this CPU's redistributor + interface */
uint32_t gic_ack_irq(void);
void     gic_end_irq(uint32_t irq);
void     gic_send_sgi(uint64_t mpidr, uint32_t sgi_id);
void     gic_send_sgi_self(uint32_t sgi_id);

uint64_t platform_gicd_base(void);
uint64_t platform_gicr_base(void);

/* ========================================================================
 * Timer
 * ======================================================================== */
void     timer_init(uint32_t hz);       /* boot CPU */
void     timer_init_cpu(void);          /* start the tick on a secondary CPU */
uint64_t timer_get_ticks(void);
uint64_t timer_ms(void);
uint64_t timer_get_tick_count(void);
void     udelay(uint32_t us);
void     mdelay(uint32_t ms);
void     timer_tick_handler(int count);  /* count: advance the tick count (boot CPU) */

/* ========================================================================
 * Scheduler
 * ======================================================================== */
typedef void (*task_func_t)(void *arg);

void sched_init(void);
/* Runs on the boot CPU, like before SMP */
int  task_create(const char *name, task_func_t func, void *arg, int priority);
/* cpu: a core number, or -1 (TASK_ANY_CPU) for whichever CPU is free */
int  task_create_on(const char *name, task_func_t func, void *arg, int priority,
                    int cpu);
void task_yield(void);
void task_delay_ms(uint32_t ms);
void sched_start(void);

/* ========================================================================
 * VirtIO block device (drivers/virtio_blk.c)
 * ======================================================================== */
int      virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func, uint64_t bars[6]);
int      virtio_blk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
int      virtio_blk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len);
int      virtio_blk_flush(void);
uint64_t virtio_blk_capacity(void);

/* ========================================================================
 * OsitoFS v2 (fs/ositofs2.c) — types from include/common/ositofs2_format.h
 * ======================================================================== */
#include "../../../include/common/ositofs2_format.h"

int  osfs2_mount(uint64_t part_offset);
void osfs2_list(void);
osfs2_file_t *osfs2_find(const char *name);
int  osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len);
osfs2_file_t *osfs2_create(const char *name, uint64_t size);
int  osfs2_delete(const char *name);

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
