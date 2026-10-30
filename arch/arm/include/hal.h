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
/* Into the bootloader's fastboot mode, for flashing (shell `reboot
 * bootloader`). Returns only if it can't. */
void platform_reboot_bootloader(void);
void platform_pmic_cmd(int argc, char **argv);  /* shell `pmic` */
void platform_rpmh_cmd(int argc, char **argv);  /* shell `rpmh` */
/* MMU + caches (called from kernel_main after mem_init) */
void platform_mmu_init(void);
/* Board devices beyond the core (called from kernel_main after the heap) */
void platform_probe_devices(void);
/* Receive a kernel Image from the host and run it (shell `kload`).
 * Returns only on failure. */
int platform_kload(void);
/* CPU frequency: set at boot, shell `cpufreq` to show or change it */
void platform_cpufreq_init(void);
void platform_cpufreq_cmd(int argc, char **argv);
/* Board tasks, once the scheduler exists (SM8350: the thermal limit, keys) */
void platform_start_tasks(void);
void platform_temp_cmd(int argc, char **argv);   /* shell `temp` */
void platform_keys_cmd(int argc, char **argv);   /* shell `keys` */
void platform_ufs_cmd(int argc, char **argv);    /* shell `ufs` */
void platform_put_cmd(int argc, char **argv);    /* shell `put`: a file over USB */
void platform_get_cmd(int argc, char **argv);    /* shell `get`: one back to the host */
void platform_touch_cmd(int argc, char **argv);  /* shell `touch` */
void platform_net_cmd(int argc, char **argv);    /* shell `net`: USB networking */
/* The next finger event in panel pixels (down: 1 pressed or moved, 0
 * lifted): 1, or 0 when there is none */
int  platform_touch_event(int32_t *x, int32_t *y, int *down);

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
/* Page the screen's text area through the console log: `lines` further
 * back (negative: forward). At 0 lines back it is the live console again,
 * with whatever came meanwhile. Returns how many lines back it shows. */
#define CONSOLE_LIVE    (-(1 << 30))
int  console_scroll(int lines);
void console_redraw(void);  /* the log's tail, after the text area moved */
/* Exclusive use of the console devices (USB gadget, framebuffer) across
 * CPUs, IRQs off; nests on one CPU. serial_* take it themselves. */
uint64_t console_lock(void);
void     console_unlock(uint64_t flags);
/* While on, console output still reaches the screen and the log but not
 * the host: a binary transfer (`get`) owns that link */
void     console_host_hold(int on);

/* ========================================================================
 * Framebuffer
 * ======================================================================== */
void fb_init_platform(uint64_t base, uint32_t width, uint32_t height,
                      uint32_t stride, uint32_t bpp);
void fb_set_text_top(uint32_t y);
void fb_clear(void);
void fb_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
/* SM8350 screen debug mode (SCREEN_DEBUG, shell `screen`): boot progress
 * bars, a heartbeat square per CPU and the console from pixel row 700. Off,
 * the console has the whole panel. */
extern volatile int screen_debug;
void platform_screen_cmd(int argc, char **argv);    /* shell `screen` */
/* Per-CPU status square in the heartbeat row, in debug mode */
#define FB_CPU_MARK(cpu, color) \
    do { if (screen_debug && !desktop_active) fb_fill((cpu) * 70, 420, 60, 30, (color)); } while (0)
int  fb_take_dirty(void);           /* text drawn since the last call */

/* The desktop (kernel/gui_task.c, shell `desktop`): the shared GUI in its
 * phablet layout on the panel, the console inside its Terminal window */
extern volatile int desktop_active;
void desktop_cycle_window(void);    /* the next window to the front (Power) */
void desktop_close(void);           /* back to the plain console */
/* The panel: framebuffer, pixels and millimetres; -1 if none (virt) */
int  platform_panel(uint64_t *base, uint32_t *w, uint32_t *h,
                    uint32_t *w_mm, uint32_t *h_mm);
void platform_console_to_panel(void);   /* the console back on it, redrawn */
void fb_putc(char c);
void fb_puts(const char *s);
void fb_puts_color(const char *s, uint32_t color);
void fb_putdec(uint64_t val);
/* Show the log in `ring` (`total` bytes ever written) `lines` further back;
 * console_scroll calls it under the console lock. fb_redraw_log: just the
 * live console, laid out again from the log (console_redraw). */
int  fb_history(const char *ring, uint32_t size, uint32_t total, int lines);
void fb_redraw_log(const char *ring, uint32_t size, uint32_t total);

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
void smp_bench(int any, int count);     /* shell `cpubench [any [n]]` */
void smp_bench_mix(void);               /* shell `cpubench mix` */
void smp_burn(int cpu, int seconds);    /* shell `burn`; cpu -1 = all */
int  smp_cpu_id(void);          /* kernel/sched.c: this CPU's core number */
uint64_t smp_cpu_ticks(int cpu);    /* timer ticks it took: 0 = never ran */

/* ========================================================================
 * Memory manager (kernel/memory.c)
 * ======================================================================== */
void     mem_init(uint64_t base, uint64_t size);
void    *mem_alloc_pages(uint64_t count);
void    *mem_alloc_aligned(uint64_t size, uint64_t alignment);
void     mem_free_pages(void *ptr, uint64_t count);
int      mem_reserve_range(uint64_t phys, uint64_t count);
/* A second region for big allocations (the platform maps it first); high
 * allocations fall back to the first region when there is none */
void     mem_add_high(uint64_t base, uint64_t size);
void    *mem_alloc_aligned_high(uint64_t size, uint64_t alignment);
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
/* cpu: a core number, or TASK_ANY_CPU for whichever CPU is free */
#define TASK_ANY_CPU    (-1)
int  task_create_on(const char *name, task_func_t func, void *arg, int priority,
                    int cpu);
/* Relative speed of a CPU (cpufreq): unpinned tasks prefer the fastest */
void sched_set_capacity(int cpu, uint32_t capacity);
int  task_self(void);                   /* the calling task's id */
/* Move task id to cpu (or TASK_ANY_CPU); the caller moves before this
 * returns. Returns where it was pinned, -2 for a bad id or CPU. */
int  task_pin(int id, int cpu);
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

/* The device it lives on: VirtIO-blk unless a platform sets another (byte
 * offsets from the device's start) */
void osfs2_set_backend(int (*read)(uint64_t off, void *buf, uint64_t len),
                       int (*write)(uint64_t off, const void *buf, uint64_t len),
                       int (*flush)(void));
int  osfs2_format(uint64_t bytes, const char *label);  /* empty fs, 1 MB blocks */
int  osfs2_mount(uint64_t part_offset);
void osfs2_unmount(void);
bool osfs2_is_mounted(void);
uint32_t osfs2_file_count(void);
osfs2_file_t *osfs2_file_at(uint32_t index);
void osfs2_list(void);
osfs2_file_t *osfs2_find(const char *name);
int  osfs2_read(osfs2_file_t *file, uint64_t offset, void *buf, uint64_t len);
osfs2_file_t *osfs2_create(const char *name, uint64_t size);
int  osfs2_sync(void);      /* what osfs2_write wrote, out of the device's cache */
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
