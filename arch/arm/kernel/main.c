/*
 * main.c -- Osito-K AArch64 kernel entry point
 *
 * Initializes platform, prints banner and CPU info, then halts.
 */

#include "../include/hal.h"
#include "../include/aarch64.h"
#include "task.h"

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
        /* Timer PPI */
        timer_tick_handler();
        if (current_task)
            current_task->ticks_run++;

        /* Wake sleeping tasks */
        uint64_t now = timer_get_tick_count();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_pool[i].state == TASK_STATE_BLOCKED &&
                task_pool[i].wake_tick != 0 &&
                (int64_t)(now - task_pool[i].wake_tick) >= 0) {
                task_pool[i].wake_tick = 0;
                task_pool[i].state = TASK_STATE_READY;
            }
        }
        need_schedule = 1;
    } else if (irqnr == 0) {
        /* SGI 0 — yield */
        need_schedule = 1;
    } else if (irqnr != 1023) {
        serial_puts("[IRQ ] Unhandled INTID: ");
        serial_putdec(irqnr);
        serial_puts("\n");
    }

    if (need_schedule && current_task) {
        need_schedule = 0;
        schedule();
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
 * Scheduler test tasks
 * ======================================================================== */

static void test_task_a(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        serial_puts("[A] tick=");
        serial_putdec(timer_get_tick_count());
        serial_puts("\n");
        task_delay_ms(500);
    }
    serial_puts("[A] done\n");
}

static void test_task_b(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        serial_puts("[B] tick=");
        serial_putdec(timer_get_tick_count());
        serial_puts("\n");
        task_delay_ms(700);
    }
    serial_puts("[B] done\n");
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

    /* Step 5: Scheduler */
    sched_init();
    task_create("taskA", test_task_a, (void *)0, 1);
    task_create("taskB", test_task_b, (void *)0, 1);

    serial_puts("[KERN] Boot complete.\n");
    irq_enable();
    sched_start();  /* Never returns */
}
