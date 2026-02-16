/*
 * OsitoK v0.1 - Kernel main
 *
 * Initializes all subsystems and starts preemptive scheduling:
 *   1. UART (serial I/O)
 *   2. Memory pool (block allocator)
 *   3. Scheduler (idle task)
 *   4. Create user tasks (heartbeat, shell)
 *   5. Timer (FRC1 at 100Hz)
 *   6. Start scheduler (never returns)
 */

#include "osito.h"
#include "drivers/uart.h"
#include "mem/pool_alloc.h"
#include "kernel/task.h"
#include "shell/shell.h"

extern "C" {

/* ====== Heartbeat task ====== */

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t count = 0;

    for (;;) {
        uart_lock();
        uart_puts("[heartbeat ");
        uart_put_dec(count);
        uart_puts("]\n");
        uart_unlock();
        count++;

        /* Wait ~2 seconds (200 ticks at 100Hz) */
        task_delay_ticks(200);
    }
}

/* ====== Kernel entry point ====== */

void kernel_main(void)
{
    uart_init();

    uart_puts("\n");
    uart_puts("=============================\n");
    uart_puts("  OsitoK v" OSITO_VERSION_STRING "\n");
    uart_puts("  Bare-metal kernel for ESP8266\n");
    uart_puts("=============================\n");

    /* Initialize memory pool */
    pool_init();

    /* Initialize scheduler (creates idle task) */
    sched_init();

    /* Create user tasks */
    task_create("heartbeat", heartbeat_task, nullptr, 1);
    task_create("shell", shell_task, nullptr, 1);

    /* Configure FRC1 timer for 100Hz preemptive ticks */
    timer_init();

    uart_puts("\nStarting kernel...\n\n");

    /* Start the scheduler — loads idle task context and does rfe.
     * Interrupts will be unmasked by rfe (clearing EXCM),
     * and FRC1 will immediately start preempting into schedule(). */
    sched_start();

    /* Never reached */
}

} /* extern "C" */
