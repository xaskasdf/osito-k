/*
 * OsitoK x86-64 — Power Management
 *
 * ACPI shutdown/reboot, CPU halt, basic power state control.
 * Uses ACPI PM1a control register for clean shutdown.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* ── I/O Port Access ─────────────────────────────────────────── */

static inline void outb_p(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw_p(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw_p(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ── ACPI Shutdown ───────────────────────────────────────────── */

/* ACPI PM1a control register — QEMU uses 0x604 by default.
 * Real hardware: parse FADT from RSDT to find PM1a_CNT_BLK.
 * SLP_TYPa for S5 (shutdown) = 0x2000 on most systems. */

#define ACPI_PM1A_CNT   0x604   /* PM1a control register (QEMU default) */
#define ACPI_SLP_TYPa   0x2000  /* S5 sleep type (shutdown) */
#define ACPI_SLP_EN      0x2000  /* Sleep enable bit */

void power_shutdown(void)
{
    serial_puts("[POWER] ACPI shutdown...\n");

    /* Try ACPI S5 shutdown (works on QEMU/KVM) */
    outw_p(ACPI_PM1A_CNT, ACPI_SLP_TYPa | ACPI_SLP_EN);

    /* If ACPI didn't work, try keyboard controller reset */
    serial_puts("[POWER] ACPI failed, trying keyboard reset...\n");
    outb_p(0x64, 0xFE);  /* Pulse reset line via keyboard controller */

    /* If nothing worked, halt */
    serial_puts("[POWER] Halting CPU\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ── Reboot ──────────────────────────────────────────────────── */

void power_reboot(void)
{
    serial_puts("[POWER] Rebooting...\n");

    /* Method 1: Keyboard controller reset (most reliable) */
    outb_p(0x64, 0xFE);

    /* Method 2: Triple fault (guaranteed reboot) */
    serial_puts("[POWER] Keyboard reset failed, triple faulting...\n");
    /* Load a null IDT and trigger an interrupt */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0; int $3" : : "m"(null_idt));

    for (;;) __asm__ volatile ("hlt");
}

/* ── CPU Halt (power-efficient idle) ─────────────────────────── */

void power_halt(void)
{
    serial_puts("[POWER] System halted\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ── CPU Frequency Info ──────────────────────────────────────── */

/* Read CPU frequency from CPUID (if available) or TSC calibration */
uint64_t power_get_cpu_freq(void)
{
    /* Try CPUID leaf 0x16 (Intel: processor frequency info) */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    uint32_t max_leaf = eax;

    if (max_leaf >= 0x16) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x16));
        if (eax > 0) return (uint64_t)eax * 1000000ULL;  /* Base freq in MHz */
    }

    /* Fallback: use TSC calibration from display.c */
    return 0;  /* Caller should use TSC-based estimate */
}

/* ── MWAIT/MONITOR for power-efficient idle ──────────────────── */

/* Check if MONITOR/MWAIT is supported */
bool power_has_mwait(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (ecx & (1 << 3)) != 0;  /* ECX bit 3 = MONITOR/MWAIT */
}
