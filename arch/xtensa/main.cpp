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
#include "mem/heap.h"
#include "fs/ositofs.h"
#include "kernel/task.h"
#include "drivers/input.h"
#include "drivers/video.h"
#include "shell/shell.h"

extern "C" {

/* ====== Kernel entry point ====== */

void kernel_main(void)
{
    uart_init();

    uart_puts("\n");
    uart_puts("=============================\n");
    uart_puts("  OsitoK v" OSITO_VERSION_STRING "\n");
    uart_puts("  Bare-metal kernel for ESP8266\n");
    uart_puts("=============================\n");

    /* Initialize memory pool and heap */
    pool_init();
    heap_init();

    /* Mount filesystem (non-fatal if not formatted yet) */
    fs_init();

    /* Initialize scheduler (creates idle task) */
    sched_init();

    /* Initialize input subsystem (ADC + button GPIO) */
    input_init();

    /* Initialize video framebuffer */
    video_init();

    /* Create user tasks (higher priority = runs first) */
    task_create("input", input_task, nullptr, 2);
    task_create("shell", shell_task, nullptr, 3);

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
