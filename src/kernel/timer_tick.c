/*
 * OsitoK - Exception/interrupt dispatcher and FRC1 timer setup
 *
 * All level-1 exceptions and interrupts are routed through _xt_user_exc
 * (in context_switch.S), which calls os_exception_handler().
 *
 * This dispatcher checks EXCCAUSE to determine what happened:
 *   - EXCCAUSE=4: Level-1 interrupt -> check INTERRUPT register
 *     - INUM 9 (FRC1): timer tick -> schedule()
 *     - INUM 7 (SOFT): task_yield() -> schedule()
 *     - INUM 5 (UART): UART RX -> read FIFO into ring buffer
 *   - Other EXCCAUSE: ignored for now (return, task resumes)
 *
 * This code MUST be in IRAM (runs in exception context).
 */

#include "kernel/task.h"
#include "drivers/uart.h"

/* Xtensa EXCCAUSE values */
#define EXCCAUSE_LEVEL1_INTERRUPT  4

/*
 * os_exception_handler - main C dispatcher
 *
 * Called from context_switch.S after saving context, on the ISR stack.
 * After this returns, context_switch.S reloads current_task->sp
 * (which may have changed if schedule() ran).
 */
void os_exception_handler(void)
{
    uint32_t exccause;
    __asm__ volatile("rsr %0, exccause" : "=a"(exccause));

    if (exccause != EXCCAUSE_LEVEL1_INTERRUPT) {
        /* Non-interrupt exception (illegal instruction, load error, etc.)
         * For now, just return and let the task resume.
         * A real kernel would kill the task or dump diagnostics. */
        return;
    }

    /* Read which interrupts are pending */
    uint32_t intr;
    __asm__ volatile("rsr %0, interrupt" : "=a"(intr));

    int need_schedule = 0;

    /* Handle FRC1 timer interrupt */
    if (intr & (1 << INUM_TIMER_FRC1)) {
        FRC1_INT_CLR = 1;
        tick_count++;
        /* Count this timer tick for the interrupted task */
        current_task->ticks_run++;

        /* Wake tasks whose sleep timer has expired */
        task_tcb_t *pool = sched_get_task_pool();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (pool[i].state == TASK_STATE_BLOCKED &&
                pool[i].wake_tick != 0 &&
                (int32_t)(tick_count - pool[i].wake_tick) >= 0)
            {
                pool[i].wake_tick = 0;
                pool[i].state = TASK_STATE_READY;
            }
        }

        need_schedule = 1;
    }

    /* Handle software interrupt (task_yield) */
    if (intr & (1 << INUM_SOFT)) {
        need_schedule = 1;
    }

    /* Handle UART interrupt */
    if (intr & (1 << INUM_UART)) {
        uart_isr_handler();
    }

    /* Reschedule if needed */
    if (need_schedule) {
        schedule();
    }

    /* Clear edge-triggered and software interrupts */
    uint32_t clr = intr & ((1 << INUM_TIMER_FRC1) | (1 << INUM_SOFT));
    if (clr) {
        __asm__ volatile("wsr %0, intclear" :: "a"(clr));
    }
}

/*
 * timer_init - configure FRC1 for periodic ticks
 *
 * FRC1 parameters:
 *   - Prescaler: /16
 *   - Load value: 80MHz / 16 / 100Hz = 50000
 *   - Auto-reload mode
 *   - Edge interrupt
 *
 * The interrupt routing:
 *   FRC1 fires -> DPORT edge interrupt -> Xtensa INUM 9 -> level-1 exception
 *   -> VECBASE+0x50 (UserExcVec) -> _xt_user_exc -> os_exception_handler()
 */
void timer_init(void)
{
    /* Disable timer first */
    FRC1_CTRL = 0;

    /* Set load value for 100 Hz tick (10ms period) */
    FRC1_LOAD = FRC1_LOAD_VAL;

    /* Clear any pending interrupt */
    FRC1_INT_CLR = 1;

    /* Enable edge interrupt for FRC1 in DPORT */
    DPORT_EDGE_INT_ENABLE |= DPORT_EDGE_INT_TIMER1;

    /* Configure and enable FRC1:
     *   - Prescaler /16
     *   - Auto-reload
     *   - Edge interrupt type
     *   - Enable
     */
    FRC1_CTRL = FRC1_CTRL_DIV16
              | FRC1_CTRL_AUTOLOAD
              | FRC1_CTRL_INT_EDGE
              | FRC1_CTRL_EN;

    /* Enable Xtensa interrupt numbers for FRC1, UART, and software yield */
    uint32_t mask = (1 << INUM_TIMER_FRC1) | (1 << INUM_UART) | (1 << INUM_SOFT);
    __asm__ volatile(
        "rsr.intenable a2\n"
        "or  a2, a2, %0\n"
        "wsr.intenable a2\n"
        :: "r"(mask) : "a2"
    );

    uart_puts("timer: FRC1 configured at ");
    uart_put_dec(TICK_HZ);
    uart_puts(" Hz (load=");
    uart_put_dec(FRC1_LOAD_VAL);
    uart_puts(")\n");
}
