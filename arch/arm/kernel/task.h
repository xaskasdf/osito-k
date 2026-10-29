#ifndef OSITO_ARM_TASK_H
#define OSITO_ARM_TASK_H

#include "../include/types.h"
#include "../include/hal.h"

#define MAX_TASKS       32      /* includes one idle task per CPU */
#define TASK_STACK_SIZE 8192
#define TASK_ANY_CPU    (-1)

typedef enum {
    TASK_STATE_FREE    = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,
    TASK_STATE_DEAD    = 4,
} task_state_t;

/* Task Control Block — sp MUST be at offset 0 (used by asm) */
typedef struct {
    uint64_t        sp;         /* Offset 0: saved stack pointer */
    task_state_t    state;
    uint8_t         id;
    uint8_t         priority;
    int8_t          cpu;        /* CPU it may run on, or TASK_ANY_CPU */
    int8_t          on_cpu;     /* CPU running it, or that ran it last */
    uint8_t         idle;       /* a CPU's idle task: only runs when nothing else can */
    uint32_t        ticks_run;
    uint64_t        wake_tick;
    uint8_t        *stack_base;
    uint32_t        stack_size;
    const char     *name;
} task_tcb_t;

/* Per-CPU scheduler state; TPIDR_EL1 points at this CPU's entry.
 * current and irq_stack_top are at fixed offsets for context_switch.S. */
struct cpu_sched {
    task_tcb_t *current;        /* 0 */
    uint64_t    irq_stack_top;  /* 8 */
    task_tcb_t *idle;
    int         id;             /* core number, index into cpu_sched[] */
    int         boot;           /* the CPU that booted the kernel */
    int         need_schedule;
    int         last;           /* round-robin cursor into task_pool */
    uint64_t    ticks;          /* this CPU's timer ticks */
};

extern task_tcb_t       task_pool[MAX_TASKS];
extern struct cpu_sched cpu_sched[MAX_CPUS];

/* volatile: a task can move to another CPU between two reads */
static inline struct cpu_sched *this_cpu(void)
{
    struct cpu_sched *c;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

#define current_task (this_cpu()->current)

void schedule(void);
void sched_tick(void);
void sched_cpu_init(int cpu, int boot);
void sched_ap_start(void);
void task_exit_handler(void);

#endif
