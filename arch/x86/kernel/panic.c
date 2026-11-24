/*
 * OsitoK x86-64 — Kernel Panic + Watchdog
 *
 * panic(): fatal error handler with register dump and stack trace.
 * Watchdog: NMI-based lockup detection (soft lockup = no schedule for 10s).
 * Crash info dumped to serial for post-mortem analysis.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putc(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern void power_halt(void) __attribute__((weak));
extern void boot_diag_flush(const char *reason);

/* ── Panic ───────────────────────────────────────────────────── */

static volatile bool panicking;

void __attribute__((noreturn)) kernel_panic(const char *msg)
{
    /* Prevent recursive panic */
    if (panicking) {
        serial_puts("[PANIC] Recursive panic! Halting.\n");
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
    panicking = true;

    __asm__ volatile ("cli");  /* Disable interrupts */

    serial_puts("\n\n");
    serial_puts("============================================================\n");
    serial_puts("  KERNEL PANIC: ");
    serial_puts(msg);
    serial_puts("\n");
    serial_puts("============================================================\n");

    /* Dump registers */
    uint64_t rsp, rbp, rip, rflags, cr3;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));

    serial_puts("  RSP=0x"); serial_puthex(rsp, 16);
    serial_puts("  RBP=0x"); serial_puthex(rbp, 16);
    serial_puts("\n");
    serial_puts("  CR3=0x"); serial_puthex(cr3, 16);
    serial_puts("  RFLAGS=0x"); serial_puthex(rflags, 16);
    serial_puts("\n");

    /* Stack trace via frame pointer chain */
    serial_puts("\n  Stack trace:\n");
    uint64_t *frame = (uint64_t *)rbp;
    for (int depth = 0; depth < 16; depth++) {
        if ((uint64_t)frame < 0x1000 || (uint64_t)frame > 0x7FFFFFFFFFFF)
            break;
        uint64_t ret_addr = frame[1];
        serial_puts("    #");
        serial_putdec((uint64_t)depth);
        serial_puts(": 0x");
        serial_puthex(ret_addr, 16);
        serial_puts("\n");
        frame = (uint64_t *)frame[0];
    }

    /* Dump tick count for timing analysis */
    serial_puts("\n  Ticks: ");
    serial_putdec(idt_get_ticks());
    serial_puts(" (");
    serial_putdec(idt_get_ticks() / 100);
    serial_puts(" seconds uptime)\n");

    serial_puts("\n  System halted. Reboot to continue.\n");
    serial_puts("============================================================\n");
    boot_diag_flush("panic");

    /* Halt all CPUs */
    if (power_halt)
        power_halt();
    else {
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
    __builtin_unreachable();
}

/* ── Soft Lockup Watchdog ────────────────────────────────────── */

/* Tracks the last time each CPU ran the scheduler.
 * If a CPU hasn't scheduled in >10 seconds, it's locked up. */

#define WATCHDOG_TIMEOUT_TICKS  1000  /* 10 seconds at 100Hz */
#define WATCHDOG_MAX_CPUS       8

static uint64_t watchdog_last_tick[WATCHDOG_MAX_CPUS];
static bool watchdog_enabled;

void watchdog_init(void)
{
    for (int i = 0; i < WATCHDOG_MAX_CPUS; i++)
        watchdog_last_tick[i] = idt_get_ticks();
    watchdog_enabled = true;
    serial_puts("[WATCHDOG] Soft lockup detection enabled (10s timeout)\n");
}

/* Called from scheduler tick to reset the watchdog for this CPU */
void watchdog_pet(uint32_t cpu_id)
{
    if (cpu_id < WATCHDOG_MAX_CPUS)
        watchdog_last_tick[cpu_id] = idt_get_ticks();
}

/* Called periodically (e.g., from NMI or timer) to check for lockups */
void watchdog_check(void)
{
    if (!watchdog_enabled) return;
    uint64_t now = idt_get_ticks();

    for (int i = 0; i < WATCHDOG_MAX_CPUS; i++) {
        if (watchdog_last_tick[i] == 0) continue;  /* CPU not active */
        uint64_t elapsed = now - watchdog_last_tick[i];
        if (elapsed > WATCHDOG_TIMEOUT_TICKS) {
            serial_puts("[WATCHDOG] SOFT LOCKUP on CPU ");
            serial_putdec((uint64_t)i);
            serial_puts(" (");
            serial_putdec(elapsed / 100);
            serial_puts("s without schedule)\n");
            /* Reset to avoid spamming */
            watchdog_last_tick[i] = now;
        }
    }
}

/* ── WARN/BUG macros (for debug builds) ──────────────────────── */

void kernel_warn(const char *file, int line, const char *msg)
{
    serial_puts("[WARN] ");
    serial_puts(file);
    serial_puts(":");
    serial_putdec((uint64_t)line);
    serial_puts(": ");
    serial_puts(msg);
    serial_puts("\n");
}

void kernel_bug(const char *file, int line, const char *msg)
{
    serial_puts("[BUG] ");
    serial_puts(file);
    serial_puts(":");
    serial_putdec((uint64_t)line);
    serial_puts(": ");
    serial_puts(msg);
    serial_puts("\n");
    kernel_panic("BUG() triggered");
}
