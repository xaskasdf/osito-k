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

void platform_mmu_init(void)
{
    paging_init();
}

/* virt's PCI/virtio/SMP probes still live in kernel_main */
void platform_probe_devices(void)
{
}

/* QEMU loads the kernel itself (-kernel) */
int platform_kload(void)
{
    serial_puts("kload: only on SM8350 (USB console)\n");
    return -1;
}

void platform_reboot_bootloader(void)
{
    serial_puts("reboot bootloader: only on SM8350\n");
}

void platform_pmic_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("pmic: only on SM8350\n");
}

/* No temperature sensors or keys either */
void platform_start_tasks(void)
{
}

void platform_temp_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("temp: only on SM8350\n");
}

void platform_keys_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("keys: only on SM8350\n");
}

/* virt's disk is virtio-blk */
void platform_ufs_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("ufs: only on SM8350\n");
}

/* No framebuffer console: QEMU shows the UART */
void platform_screen_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("screen: only on SM8350\n");
}

/* QEMU's CPUs have no frequency to set */
void platform_cpufreq_init(void)
{
}

void platform_cpufreq_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("cpufreq: only on SM8350\n");
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
