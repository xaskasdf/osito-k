/*
 * sched.c -- Preemptive scheduler for OsitoK AArch64
 * Priority-based round-robin. Port of ESP8266 design.
 *
 * SMP: one task pool shared by every CPU under sched_lock. Each CPU
 * reschedules on its own tick or yield SGI and picks the best READY task it
 * may run (a task is pinned to one CPU or runs on any); with nothing to do
 * it runs its own idle task. A task that is RUNNING belongs to the CPU
 * running it, so no two CPUs pick the same one.
 */
#include "../include/hal.h"
#include "../include/aarch64.h"
#include "../include/spinlock.h"
#include "task.h"

#define IRQ_STACK_SIZE  8192

task_tcb_t       task_pool[MAX_TASKS];
struct cpu_sched cpu_sched[MAX_CPUS];

static spinlock_t sched_lock;
static int boot_cpu;
static uint8_t stack_pool[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));
/* irq_vector_entry switches here: once schedule() makes the interrupted
 * task READY, another CPU may resume it on its own stack */
static uint8_t irq_stacks[MAX_CPUS][IRQ_STACK_SIZE]
    __attribute__((aligned(16)));

extern void task_entry_trampoline(void);
extern void sched_start_asm(uint64_t sp);

static void idle_task(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile("wfe");
}

/* Point TPIDR_EL1 at this CPU's state. Before IRQs are enabled on it. */
void sched_cpu_init(int cpu, int boot) {
    struct cpu_sched *c = &cpu_sched[cpu];
    c->id = cpu;
    c->boot = boot;
    c->irq_stack_top = (uint64_t)(uintptr_t)&irq_stacks[cpu][IRQ_STACK_SIZE];
    if (boot)
        boot_cpu = cpu;
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(c) : "memory");
}

int smp_cpu_id(void) {
    return this_cpu()->id;
}

/* Take a free slot and build the task's first context. Caller holds
 * sched_lock. */
static task_tcb_t *task_alloc(const char *name, task_func_t func, void *arg,
                              int priority, int cpu, int idle) {
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return (void *)0;

    task_tcb_t *t = &task_pool[slot];
    t->id = (uint8_t)slot;
    t->priority = (uint8_t)priority;
    t->cpu = (int8_t)cpu;
    t->on_cpu = (int8_t)(cpu >= 0 ? cpu : boot_cpu);
    t->idle = (uint8_t)idle;
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
    t->state = TASK_STATE_READY;
    return t;
}

void sched_init(void) {
    serial_puts("[SCHED] Initializing...\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_pool[i].state = TASK_STATE_FREE;
        task_pool[i].id = i;
    }
    struct cpu_sched *c = this_cpu();
    c->last = 0;
    c->need_schedule = 0;
    c->current = (void *)0;
    c->idle = task_alloc("idle", idle_task, (void *)0, 0, c->id, 1);
    serial_puts("[SCHED] Ready\n");
}

int task_create_on(const char *name, task_func_t func, void *arg, int priority,
                   int cpu) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    task_tcb_t *t = task_alloc(name, func, arg, priority, cpu, 0);
    spin_unlock_irqrestore(&sched_lock, flags);
    if (!t) {
        serial_puts("[SCHED] No free slots!\n");
        return -1;
    }

    serial_puts("[SCHED] Task '");
    serial_puts(name);
    serial_puts("' id=");
    serial_putdec(t->id);
    serial_puts(" pri=");
    serial_putdec(priority);
    if (cpu >= 0) {
        serial_puts(" cpu=");
        serial_putdec(cpu);
    }
    serial_puts("\n");
    return t->id;
}

int task_create(const char *name, task_func_t func, void *arg, int priority) {
    return task_create_on(name, func, arg, priority, boot_cpu);
}

/* From the IRQ handler (IRQs off), on this CPU's IRQ stack */
void schedule(void) {
    struct cpu_sched *c = this_cpu();
    spin_lock(&sched_lock);

    task_tcb_t *prev = c->current;
    if (prev) {
        if (prev->state == TASK_STATE_RUNNING)
            prev->state = TASK_STATE_READY;
        else if (prev->state == TASK_STATE_DEAD)
            prev->state = TASK_STATE_FREE;      /* off its stack for good */
    }

    /* Highest priority wins; the scan starts after the last pick, so equal
     * priorities take turns */
    int best = -1, best_pri = -1;
    for (int n = 1; n <= MAX_TASKS; n++) {
        int i = (c->last + n) % MAX_TASKS;
        task_tcb_t *t = &task_pool[i];
        if (t->state != TASK_STATE_READY || t->idle)
            continue;
        if (t->cpu != TASK_ANY_CPU && t->cpu != c->id)
            continue;
        if (t->priority > best_pri) {
            best = i;
            best_pri = t->priority;
        }
    }

    task_tcb_t *next = best >= 0 ? &task_pool[best] : c->idle;
    if (best >= 0)
        c->last = best;
    next->state = TASK_STATE_RUNNING;
    next->on_cpu = (int8_t)c->id;
    c->current = next;
    spin_unlock(&sched_lock);
}

/* Timer tick on this CPU (IRQ handler): account it, wake sleepers whose
 * time has come (any CPU may wake any task), ask for a reschedule */
void sched_tick(void) {
    struct cpu_sched *c = this_cpu();
    uint64_t now = timer_get_tick_count();
    spin_lock(&sched_lock);
    if (c->current)
        c->current->ticks_run++;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_STATE_BLOCKED &&
            task_pool[i].wake_tick != 0 &&
            (int64_t)(now - task_pool[i].wake_tick) >= 0) {
            task_pool[i].wake_tick = 0;
            task_pool[i].state = TASK_STATE_READY;
        }
    }
    spin_unlock(&sched_lock);
    c->need_schedule = 1;
}

void task_yield(void) {
    gic_send_sgi_self(SGI_YIELD);
}

void task_delay_ms(uint32_t ms) {
    uint64_t ticks = (uint64_t)ms / 10;
    if (ticks == 0) ticks = 1;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    current_task->wake_tick = timer_get_tick_count() + ticks;
    current_task->state = TASK_STATE_BLOCKED;
    spin_unlock_irqrestore(&sched_lock, flags);
    task_yield();
}

void task_exit_handler(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (current_task)
        current_task->state = TASK_STATE_DEAD;
    spin_unlock_irqrestore(&sched_lock, flags);
    task_yield();
    for (;;) __asm__ volatile("wfe");
}

void sched_start(void) {
    serial_puts("[SCHED] Starting\n");
    irq_disable();
    schedule();  /* Pick first task */
    smp_release_aps();

    /* Bootstrap first task without eret (QEMU workaround).
     * Subsequent context switches use eret from IRQ handler. */
    sched_start_asm(current_task->sp);
    __builtin_unreachable();
}

/* A secondary CPU joins (GIC and timer already set up, IRQs masked): it
 * starts in a fresh idle task, sched_start_asm can only enter a task that
 * never ran. Returns only if the pool is full. */
void sched_ap_start(void) {
    struct cpu_sched *c = this_cpu();
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    task_tcb_t *idle = task_alloc("idle", idle_task, (void *)0, 0, c->id, 1);
    if (idle) {
        idle->state = TASK_STATE_RUNNING;
        idle->on_cpu = (int8_t)c->id;
        c->idle = idle;
        c->current = idle;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    if (!idle)
        return;
    sched_start_asm(idle->sp);
}
