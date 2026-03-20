/*
 * main.c -- Osito-K AArch64 kernel entry point
 *
 * Initializes platform, prints banner and CPU info, then halts.
 */

#include "../include/hal.h"
#include "../include/aarch64.h"

/* ========================================================================
 * Banner
 * ======================================================================== */

static void print_banner(void)
{
    serial_puts("\n");
    serial_puts("  ___  ____ ___ _____ ___        _  __\n");
    serial_puts(" / _ \\/ ___|_ _|_   _/ _ \\      | |/ /\n");
    serial_puts("| | | \\___ \\| |  | || | | |_____| ' / \n");
    serial_puts("| |_| |___) | |  | || |_| |_____| . \\ \n");
    serial_puts(" \\___/|____/___| |_| \\___/      |_|\\_\\\n");
    serial_puts("\n");
    serial_puts("  Osito-K AArch64 kernel\n");
    serial_puts("  naranjositos.tech\n");
    serial_puts("\n");
}

/* ========================================================================
 * CPU info
 * ======================================================================== */

static void print_cpu_info(void)
{
    uint64_t midr = read_midr_el1();
    uint64_t mpidr = read_mpidr_el1();
    uint64_t el = read_currentel();

    uint32_t implementer = (midr >> 24) & 0xFF;
    uint32_t variant = (midr >> 20) & 0xF;
    uint32_t part = (midr >> 4) & 0xFFF;
    uint32_t revision = midr & 0xF;

    serial_puts("[CPU ] MIDR_EL1 = ");
    serial_puthex(midr, 8);

    serial_puts("  (impl=");
    serial_puthex(implementer, 2);

    serial_puts(" part=");
    serial_puthex(part, 3);

    serial_puts(" r");
    serial_putdec(variant);
    serial_puts("p");
    serial_putdec(revision);
    serial_puts(")\n");

    /* Identify core */
    serial_puts("[CPU ] Core: ");
    if (implementer == 0x41) {  /* ARM Ltd */
        switch (part) {
        case 0xD05: serial_puts("Cortex-A55"); break;
        case 0xD0B: serial_puts("Cortex-A76"); break;
        case 0xD41: serial_puts("Cortex-A78"); break;
        case 0xD44: serial_puts("Cortex-X1");  break;
        case 0xD08: serial_puts("Cortex-A72"); break;
        case 0xD07: serial_puts("Cortex-A57"); break;
        case 0xD03: serial_puts("Cortex-A53"); break;
        default:
            serial_puts("ARM unknown (");
            serial_puthex(part, 3);
            serial_puts(")");
            break;
        }
    } else {
        serial_puts("Unknown implementer ");
        serial_puthex(implementer, 2);
    }
    serial_puts("\n");

    serial_puts("[CPU ] MPIDR_EL1 = ");
    serial_puthex(mpidr, 16);
    serial_puts("\n");

    serial_puts("[CPU ] CurrentEL = EL");
    serial_putdec(el);
    serial_puts("\n");
}

/* ========================================================================
 * Weak default exception handlers
 * ======================================================================== */

__attribute__((weak))
void exception_handler(uint64_t esr, uint64_t elr, uint64_t far)
{
    serial_puts("\n[PANIC] Synchronous exception!\n");
    serial_puts("  ESR_EL1 = ");
    serial_puthex(esr, 8);
    serial_puts("\n");
    serial_puts("  ELR_EL1 = ");
    serial_puthex(elr, 16);
    serial_puts("\n");
    serial_puts("  FAR_EL1 = ");
    serial_puthex(far, 16);
    serial_puts("\n");

    for (;;)
        __asm__ volatile("wfe");
}

void irq_handler(void)
{
    uint32_t irqnr = gic_ack_irq();

    if (irqnr == 30) {
        timer_tick_handler();
    } else if (irqnr == 0) {
        /* SGI 0 — yield (handled later by scheduler) */
    } else if (irqnr != 1023) {
        serial_puts("[IRQ ] Unhandled INTID: ");
        serial_putdec(irqnr);
        serial_puts("\n");
    }

    if (irqnr != 1023)
        gic_end_irq(irqnr);
}

__attribute__((weak))
void serror_handler(uint64_t esr)
{
    serial_puts("\n[PANIC] SError!\n");
    serial_puts("  ESR_EL1 = ");
    serial_puthex(esr, 8);
    serial_puts("\n");

    for (;;)
        __asm__ volatile("wfe");
}

/* ========================================================================
 * Kernel entry
 * ======================================================================== */

void kernel_main(void *dtb)
{
    platform_init(dtb);

    serial_puts("[KERN] Serial OK\n");

    print_banner();
    print_cpu_info();

    /* Step 2: GIC */
    gic_init();

    /* Step 3: Timer (100 Hz) */
    timer_init(100);

    /* Step 4: Heap */
    heap_init();

    /* Enable IRQs */
    irq_enable();
    serial_puts("[KERN] IRQs enabled\n");

    /* Tick counter test */
    serial_puts("[KERN] Tick test (3s)...\n");
    for (int i = 0; i < 3; i++) {
        mdelay(1000);
        serial_puts("  ticks = ");
        serial_putdec(timer_get_tick_count());
        serial_puts("\n");
    }

    serial_puts("[KERN] Boot complete. Halting.\n");

    for (;;)
        __asm__ volatile("wfe");
}
