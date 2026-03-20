/*
 * sched.c -- Preemptive scheduler for OsitoK AArch64
 * Priority-based round-robin. Port of ESP8266 design.
 */
#include "../include/hal.h"
#include "../include/aarch64.h"
#include "task.h"

task_tcb_t  task_pool[MAX_TASKS];
task_tcb_t *current_task;
volatile int need_schedule;

static int last_scheduled;
static uint8_t stack_pool[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

extern void task_entry_trampoline(void);
extern void sched_start_asm(uint64_t sp);

static void idle_task(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile("wfe");
}

void sched_init(void) {
    serial_puts("[SCHED] Initializing...\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_pool[i].state = TASK_STATE_FREE;
        task_pool[i].id = i;
    }
    last_scheduled = 0;
    need_schedule = 0;
    current_task = (void *)0;

    /* Create idle task (ID 0, priority 0) */
    task_create("idle", idle_task, (void *)0, 0);
    serial_puts("[SCHED] Ready\n");
}

int task_create(const char *name, task_func_t func, void *arg, int priority) {
    uint64_t flags = irq_save();

    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        irq_restore(flags);
        serial_puts("[SCHED] No free slots!\n");
        return -1;
    }

    task_tcb_t *t = &task_pool[slot];
    t->state = TASK_STATE_READY;
    t->priority = (uint8_t)priority;
    t->name = name;
    t->ticks_run = 0;
    t->wake_tick = 0;
    t->stack_base = stack_pool[slot];
    t->stack_size = TASK_STACK_SIZE;

    /* Set up initial context frame on stack top */
    uint64_t sp_top = (uint64_t)(t->stack_base + t->stack_size);
    sp_top &= ~0xFULL;  /* 16-byte align */
    sp_top -= 272;       /* Context frame size */

    /* Zero the frame */
    memset((void *)sp_top, 0, 272);

    /* Set up context: x19=func, x20=arg for trampoline */
    uint64_t *frame = (uint64_t *)sp_top;
    frame[19] = (uint64_t)func;     /* x19 */
    frame[20] = (uint64_t)arg;      /* x20 */
    frame[31] = sp_top + 272;       /* saved SP (pre-exception) */

    /* ELR = trampoline, SPSR = EL1h with IRQs unmasked */
    frame[32] = (uint64_t)task_entry_trampoline;  /* ELR_EL1 */
    frame[33] = 0x00000305ULL;                    /* SPSR_EL1: EL1h, ~IRQ */

    t->sp = sp_top;

    serial_puts("[SCHED] Task '");
    serial_puts(name);
    serial_puts("' id=");
    serial_putdec(slot);
    serial_puts(" pri=");
    serial_putdec(priority);
    serial_puts("\n");

    irq_restore(flags);
    return slot;
}

void schedule(void) {
    /* Find highest priority ready task */
    uint8_t best_pri = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_READY && task_pool[i].priority >= best_pri)
            best_pri = task_pool[i].priority;
    }

    /* Round-robin among tasks at best_pri */
    int best = -1;
    int start = (last_scheduled + 1) % MAX_TASKS;
    for (int n = 0; n < MAX_TASKS; n++) {
        int i = (start + n) % MAX_TASKS;
        if (task_pool[i].state == TASK_STATE_READY && task_pool[i].priority == best_pri) {
            if (i == 0 && best_pri == 0) {
                /* Skip idle if others ready */
                int others = 0;
                for (int j = 1; j < MAX_TASKS; j++)
                    if (task_pool[j].state == TASK_STATE_READY) { others = 1; break; }
                if (others) continue;
            }
            best = i;
            break;
        }
    }
    if (best < 0) best = 0;  /* Fall back to idle */

    if (current_task && current_task->state == TASK_STATE_RUNNING)
        current_task->state = TASK_STATE_READY;

    current_task = &task_pool[best];
    current_task->state = TASK_STATE_RUNNING;
    last_scheduled = best;
}

void task_yield(void) {
    gic_send_sgi_self(0);
}

void task_delay_ms(uint32_t ms) {
    uint64_t flags = irq_save();
    uint64_t ticks = (uint64_t)ms / 10;
    if (ticks == 0) ticks = 1;
    current_task->wake_tick = timer_get_tick_count() + ticks;
    current_task->state = TASK_STATE_BLOCKED;
    irq_restore(flags);
    task_yield();
}

void task_exit_handler(void) {
    uint64_t flags = irq_save();
    if (current_task)
        current_task->state = TASK_STATE_DEAD;
    irq_restore(flags);
    task_yield();
    for (;;) __asm__ volatile("wfe");
}

void sched_start(void) {
    serial_puts("[SCHED] Starting\n");
    schedule();  /* Pick first task */

    /* Bootstrap first task without eret (QEMU workaround).
     * Subsequent context switches use eret from IRQ handler. */
    irq_disable();
    sched_start_asm(current_task->sp);
    __builtin_unreachable();
}
