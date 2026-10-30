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

/* virt's disk is virtio-blk: `ufs mkfs disk` puts an empty OsitoFS v3 on
 * it, `ufs mount` mounts it */
void platform_ufs_cmd(int argc, char **argv)
{
    extern int osfs3_mount(uint64_t part_offset);
    extern int osfs3_format(uint64_t bytes, const char *label, uint32_t inodes);
    if (argc == 3 && strcmp(argv[1], "mkfs") == 0 && strcmp(argv[2], "disk") == 0) {
        if (!virtio_blk_capacity() || osfs3_format(virtio_blk_capacity(), "virt", 0) ||
            osfs3_mount(0))
            serial_puts("ufs: no virtio-blk disk, or formatting it failed\n");
    } else if (argc == 2 && strcmp(argv[1], "mount") == 0) {
        if (osfs3_mount(0))
            serial_puts("ufs: no OsitoFS v3 on the virtio-blk disk\n");
    } else {
        serial_puts("usage (virt): ufs [mkfs disk | mount]\n");
    }
}

void platform_put_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("put: only on SM8350 (over the USB console)\n");
}

void platform_get_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("get: only on SM8350 (over the USB console)\n");
}

void platform_touch_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("touch: only on SM8350\n");
}

void platform_rpmh_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("rpmh: only on SM8350\n");
}

void platform_net_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    serial_puts("net: virtio-net, set up at boot (see the [NET] lines)\n");
}

int platform_touch_event(int32_t *x, int32_t *y, int *down)
{
    (void)x;
    (void)y;
    (void)down;
    return 0;
}

/* No panel for the desktop: QEMU shows the UART */
int platform_panel(uint64_t *base, uint32_t *w, uint32_t *h,
                   uint32_t *w_mm, uint32_t *h_mm)
{
    (void)base; (void)w; (void)h; (void)w_mm; (void)h_mm;
    return -1;
}

void platform_console_to_panel(void)
{
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
