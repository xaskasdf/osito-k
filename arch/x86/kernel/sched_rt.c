/*
 * OsitoK x86-64 — Real-Time Scheduler Classes
 *
 * Linux-compatible scheduling policies:
 *   SCHED_FIFO:     Fixed-priority, no time quantum (runs until yields/blocks)
 *   SCHED_RR:       Round-robin within same priority level
 *   SCHED_DEADLINE: Earliest Deadline First (EDF) with CBS
 *
 * Integrates with the existing QoS scheduler — RT tasks get
 * priority over all QoS classes.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* ── Scheduling Policies (Linux values) ──────────────────────── */

#define SCHED_NORMAL    0   /* Default time-sharing (CFS-like) */
#define SCHED_FIFO      1   /* First-in first-out (no preemption by same priority) */
#define SCHED_RR        2   /* Round-robin (with time quantum) */
#define SCHED_BATCH     3   /* Batch processing (lower priority) */
#define SCHED_IDLE      5   /* Idle (lowest priority) */
#define SCHED_DEADLINE  6   /* Earliest deadline first */

/* ── RT Priority Range ───────────────────────────────────────── */

#define RT_PRIO_MIN  1
#define RT_PRIO_MAX  99

/* ── Per-Process RT State ────────────────────────────────────── */

#define RT_MAX_TASKS 32

typedef struct {
    bool     active;
    uint32_t pid;
    int      policy;         /* SCHED_FIFO, SCHED_RR, SCHED_DEADLINE */
    int      priority;       /* 1-99 (higher = more important) */
    uint32_t rr_quantum;     /* Ticks for SCHED_RR (default: 10) */
    uint32_t rr_remaining;   /* Ticks left in current quantum */
    /* SCHED_DEADLINE parameters */
    uint64_t dl_runtime;     /* Max execution time per period (ticks) */
    uint64_t dl_deadline;    /* Relative deadline (ticks) */
    uint64_t dl_period;      /* Period length (ticks) */
    uint64_t dl_abs_deadline; /* Absolute deadline (tick count) */
    uint64_t dl_used;        /* Runtime used in current period */
} rt_task_t;

static rt_task_t rt_tasks[RT_MAX_TASKS];

/* ── Registration ────────────────────────────────────────────── */

int sched_rt_set(uint32_t pid, int policy, int priority)
{
    if (priority < RT_PRIO_MIN || priority > RT_PRIO_MAX)
        return -22;  /* EINVAL */
    if (policy != SCHED_FIFO && policy != SCHED_RR && policy != SCHED_DEADLINE)
        return -22;

    /* Find existing or allocate new */
    int slot = -1;
    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (rt_tasks[i].active && rt_tasks[i].pid == pid) { slot = i; break; }
        if (!rt_tasks[i].active && slot < 0) slot = i;
    }
    if (slot < 0) return -12;  /* ENOMEM */

    rt_task_t *rt = &rt_tasks[slot];
    rt->active = true;
    rt->pid = pid;
    rt->policy = policy;
    rt->priority = priority;
    rt->rr_quantum = 10;  /* 100ms default for SCHED_RR */
    rt->rr_remaining = rt->rr_quantum;

    serial_puts("[SCHED-RT] PID ");
    serial_putdec(pid);
    serial_puts(" → ");
    serial_puts(policy == SCHED_FIFO ? "FIFO" :
                policy == SCHED_RR ? "RR" : "DEADLINE");
    serial_puts(" prio=");
    serial_putdec((uint64_t)priority);
    serial_puts("\n");
    return 0;
}

/* Set SCHED_DEADLINE parameters */
int sched_rt_set_deadline(uint32_t pid, uint64_t runtime_ms,
                          uint64_t deadline_ms, uint64_t period_ms)
{
    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (rt_tasks[i].active && rt_tasks[i].pid == pid) {
            rt_tasks[i].dl_runtime = runtime_ms / 10;   /* Convert to ticks */
            rt_tasks[i].dl_deadline = deadline_ms / 10;
            rt_tasks[i].dl_period = period_ms / 10;
            rt_tasks[i].dl_abs_deadline = idt_get_ticks() + rt_tasks[i].dl_deadline;
            rt_tasks[i].dl_used = 0;
            return 0;
        }
    }
    return -3;  /* ESRCH */
}

/* Remove RT scheduling for a process */
int sched_rt_clear(uint32_t pid)
{
    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (rt_tasks[i].active && rt_tasks[i].pid == pid) {
            rt_tasks[i].active = false;
            return 0;
        }
    }
    return -3;
}

/* ── Scheduler Query ─────────────────────────────────────────── */

/* Find the highest-priority RT task that is ready.
 * Returns PID or 0 if no RT task is runnable. */
uint32_t sched_rt_pick(void)
{
    int best_idx = -1;
    int best_prio = 0;
    uint64_t earliest_deadline = ~0ULL;

    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (!rt_tasks[i].active) continue;

        if (rt_tasks[i].policy == SCHED_DEADLINE) {
            /* EDF: pick task with earliest absolute deadline */
            if (rt_tasks[i].dl_used < rt_tasks[i].dl_runtime &&
                rt_tasks[i].dl_abs_deadline < earliest_deadline) {
                earliest_deadline = rt_tasks[i].dl_abs_deadline;
                best_idx = i;
                best_prio = RT_PRIO_MAX + 1;  /* Deadline > any FIFO/RR */
            }
        } else {
            /* FIFO/RR: highest static priority wins */
            if (rt_tasks[i].priority > best_prio) {
                best_prio = rt_tasks[i].priority;
                best_idx = i;
            }
        }
    }

    return (best_idx >= 0) ? rt_tasks[best_idx].pid : 0;
}

/* Called each tick for RT task accounting */
void sched_rt_tick(uint32_t current_pid)
{
    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (!rt_tasks[i].active || rt_tasks[i].pid != current_pid) continue;

        if (rt_tasks[i].policy == SCHED_RR) {
            /* Decrement quantum; if expired, trigger reschedule */
            if (rt_tasks[i].rr_remaining > 0)
                rt_tasks[i].rr_remaining--;
            if (rt_tasks[i].rr_remaining == 0)
                rt_tasks[i].rr_remaining = rt_tasks[i].rr_quantum;
        }

        if (rt_tasks[i].policy == SCHED_DEADLINE) {
            rt_tasks[i].dl_used++;
            /* Check if period expired → reset */
            uint64_t now = idt_get_ticks();
            if (now >= rt_tasks[i].dl_abs_deadline) {
                rt_tasks[i].dl_abs_deadline = now + rt_tasks[i].dl_period;
                rt_tasks[i].dl_used = 0;
            }
        }
        break;
    }
}

/* Check if current RT task should be preempted */
bool sched_rt_should_preempt(uint32_t current_pid)
{
    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (!rt_tasks[i].active || rt_tasks[i].pid != current_pid) continue;
        if (rt_tasks[i].policy == SCHED_RR && rt_tasks[i].rr_remaining == 0)
            return true;
        if (rt_tasks[i].policy == SCHED_DEADLINE &&
            rt_tasks[i].dl_used >= rt_tasks[i].dl_runtime)
            return true;
        return false;  /* FIFO never preempts by quantum */
    }
    return false;
}

/* Get scheduling policy for a PID */
int sched_rt_getpolicy(uint32_t pid)
{
    for (int i = 0; i < RT_MAX_TASKS; i++) {
        if (rt_tasks[i].active && rt_tasks[i].pid == pid)
            return rt_tasks[i].policy;
    }
    return SCHED_NORMAL;
}
