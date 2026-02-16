/*
 * OsitoK - Round-robin preemptive scheduler
 *
 * Features:
 *   - Up to MAX_TASKS tasks (including idle)
 *   - Round-robin scheduling with idle task skip
 *   - Static stack allocation per task
 *   - Task creation with initial context frame setup
 */

#include "kernel/task.h"
#include "drivers/uart.h"

extern "C" {

/* ====== Global state ====== */

/* Task pool — all tasks are statically allocated */
static task_tcb_t task_pool[MAX_TASKS];

/* Static stack memory for all tasks */
static uint8_t stack_pool[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

/* Current running task */
task_tcb_t *current_task = nullptr;

/* Global tick counter (incremented by timer ISR) */
volatile uint32_t tick_count = 0;

/* Index of the last scheduled task (for round-robin) */
static int last_scheduled = -1;

/* Idle task ID (always 0) */
#define IDLE_TASK_ID 0

/* ====== Internal: idle task ====== */

static void idle_task_func(void *)
{
    for (;;) {
        REG32(0x60000914) = 0x73;  /* Feed WDT */
        __asm__ volatile("nop");
    }
}

/* Assembly trampoline (in context_switch.S) */
extern void _task_entry_trampoline(void);

/*
 * _task_exit_handler - called if a task function returns
 *
 * Called from the asm trampoline. Marks the task as dead
 * and spins forever (scheduler will skip it).
 */
void _task_exit_handler(void)
{
    uint32_t ps = irq_save();
    current_task->state = TASK_STATE_DEAD;
    irq_restore(ps);

    for (;;) {
        __asm__ volatile("waiti 0");
    }
}

/* ====== Scheduler API ====== */

void sched_init(void)
{
    /* Clear all task slots */
    ets_memset(task_pool, 0, sizeof(task_pool));

    /* Create idle task (always task 0) */
    task_tcb_t *idle = &task_pool[IDLE_TASK_ID];
    idle->id = IDLE_TASK_ID;
    idle->state = TASK_STATE_READY;
    idle->priority = 0;
    idle->name = "idle";
    idle->stack_base = (uint32_t)&stack_pool[IDLE_TASK_ID][0];
    idle->stack_size = TASK_STACK_SIZE;
    idle->ticks_run = 0;

    /* Set up initial context frame for idle task */
    uint32_t sp = idle->stack_base + idle->stack_size;
    sp &= ~0xF; /* 16-byte align */
    sp -= CTX_SIZE;

    uint32_t *frame = (uint32_t *)sp;
    ets_memset(frame, 0, CTX_SIZE);

    /* EPC1 = entry point (where the task will start executing) */
    frame[CTX_EPC1 / 4] = (uint32_t)idle_task_func;
    /* a1 = stack pointer (after frame pop) */
    frame[CTX_A1 / 4] = sp + CTX_SIZE;
    /* PS: UM=1, EXCM=1, INTLEVEL=0
     * EXCM must be 1 so that rfe can clear it (rfe clears PS.EXCM) */
    frame[CTX_PS / 4] = 0x00000030;

    idle->sp = sp;

    current_task = idle;
    last_scheduled = IDLE_TASK_ID;

    uart_puts("sched: initialized, idle task created\n");
}

int task_create(const char *name, task_func_t func, void *arg, uint8_t priority)
{
    uint32_t ps = irq_save();

    /* Find a free slot (skip 0, that's idle) */
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        irq_restore(ps);
        uart_puts("sched: no free task slots!\n");
        return -1;
    }

    task_tcb_t *t = &task_pool[slot];
    t->id = (uint8_t)slot;
    t->state = TASK_STATE_READY;
    t->priority = priority;
    t->name = name;
    t->stack_base = (uint32_t)&stack_pool[slot][0];
    t->stack_size = TASK_STACK_SIZE;
    t->ticks_run = 0;

    /* Set up initial context frame */
    uint32_t sp = t->stack_base + t->stack_size;
    sp &= ~0xF;
    sp -= CTX_SIZE;

    uint32_t *frame = (uint32_t *)sp;
    ets_memset(frame, 0, CTX_SIZE);

    /* EPC1 = assembly trampoline entry */
    frame[CTX_EPC1 / 4] = (uint32_t)_task_entry_trampoline;
    /* a2 = func pointer, a3 = arg (passed to trampoline) */
    frame[CTX_A2 / 4] = (uint32_t)func;
    frame[CTX_A3 / 4] = (uint32_t)arg;
    /* a1 = stack pointer after frame pop */
    frame[CTX_A1 / 4] = sp + CTX_SIZE;
    /* PS: UM=1, EXCM=1, INTLEVEL=0 */
    frame[CTX_PS / 4] = 0x00000030;

    t->sp = sp;

    irq_restore(ps);

    uart_puts("sched: created task '");
    uart_puts(name);
    uart_puts("' (id=");
    uart_put_dec(slot);
    uart_puts(")\n");
    return slot;
}

/*
 * schedule - pick the next task to run
 *
 * Called from the timer ISR (os_tick_handler) with interrupts disabled.
 * Implements round-robin: scan forward from last_scheduled, skip idle
 * if there's another ready task.
 */
void schedule(void)
{
    /* Update current task state */
    if (current_task->state == TASK_STATE_RUNNING) {
        current_task->state = TASK_STATE_READY;
    }

    /* Round-robin: start scanning from the task after current */
    int next = last_scheduled;
    int found = -1;

    for (int i = 0; i < MAX_TASKS; i++) {
        next = (next + 1) % MAX_TASKS;
        if (task_pool[next].state == TASK_STATE_READY && next != IDLE_TASK_ID) {
            found = next;
            break;
        }
    }

    /* If no ready task found (other than idle), run idle */
    if (found < 0) {
        found = IDLE_TASK_ID;
    }

    last_scheduled = found;
    current_task = &task_pool[found];
    current_task->state = TASK_STATE_RUNNING;
}

void sched_start(void)
{
    uart_puts("sched: starting scheduler\n");

    /* Set the idle task as running */
    current_task = &task_pool[IDLE_TASK_ID];
    current_task->state = TASK_STATE_RUNNING;

    /* Jump to the restore routine — this loads the first task's context
     * and does `rfe` to start executing it. Never returns. */
    _xt_restore_context_and_rfe();

    /* Should never reach here */
    for (;;) {}
}

void task_yield(void)
{
    /*
     * Trigger a software interrupt (INUM 7). This goes through the
     * normal _xt_user_exc path: save context, os_exception_handler
     * calls schedule(), restore (possibly different) task's context.
     * When this task is eventually re-scheduled, it resumes here.
     */
    uint32_t bit = (1 << INUM_SOFT);
    __asm__ volatile("wsr %0, intset" :: "r"(bit));
}

uint32_t get_tick_count(void)
{
    return tick_count;
}

void task_delay_ticks(uint32_t ticks)
{
    uint32_t ps = irq_save();
    current_task->wake_tick = tick_count + ticks;
    current_task->state = TASK_STATE_BLOCKED;
    irq_restore(ps);
    task_yield();
}

task_tcb_t *sched_current_task(void)
{
    return current_task;
}

task_tcb_t *sched_get_task_pool(void)
{
    return task_pool;
}

} /* extern "C" */
