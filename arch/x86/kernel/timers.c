/*
 * OsitoK x86-64 — Timer Subsystem
 *
 * Implements POSIX timer_create, setitimer, alarm.
 * Uses APIC ticks (100Hz) as time base.
 * Delivers SIGALRM to process on timer expiration.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern void proc_signal_pid(uint32_t pid, int sig);
extern uint32_t proc_current_pid(void);

#define SIGALRM  14

/* ── Per-Process Timers ──────────────────────────────────────── */

#define MAX_TIMERS 32

typedef struct {
    bool     active;
    uint32_t pid;
    uint64_t interval_ticks;  /* 0 = one-shot */
    uint64_t next_fire;       /* Tick count when timer fires */
    int      signal;          /* Signal to deliver (default SIGALRM) */
} ktimer_t;

static ktimer_t timers[MAX_TIMERS];

/* ── Timer Tick (called from net_poll or main loop) ──────────── */

void timers_process(void)
{
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) continue;
        if (now < timers[i].next_fire) continue;

        /* Timer expired — deliver signal */
        proc_signal_pid(timers[i].pid, timers[i].signal);

        if (timers[i].interval_ticks > 0) {
            /* Repeating: reschedule */
            timers[i].next_fire = now + timers[i].interval_ticks;
        } else {
            /* One-shot: deactivate */
            timers[i].active = false;
        }
    }
}

/* ── alarm(seconds) — schedule SIGALRM ───────────────────────── */

uint32_t timer_alarm(uint32_t seconds)
{
    uint32_t pid = proc_current_pid();

    /* Cancel existing alarm for this process */
    uint32_t remaining = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].pid == pid &&
            timers[i].signal == SIGALRM) {
            uint64_t now = idt_get_ticks();
            if (timers[i].next_fire > now)
                remaining = (uint32_t)((timers[i].next_fire - now) / 100);
            timers[i].active = false;
        }
    }

    if (seconds == 0) return remaining;  /* Cancel only */

    /* Create new alarm */
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].active = true;
            timers[i].pid = pid;
            timers[i].interval_ticks = 0;  /* One-shot */
            timers[i].next_fire = idt_get_ticks() + (uint64_t)seconds * 100;
            timers[i].signal = SIGALRM;
            return remaining;
        }
    }
    return remaining;
}

/* ── setitimer(which, new, old) — interval timer ──────────────── */

/* Timeval: seconds + microseconds */
typedef struct {
    uint64_t tv_sec;
    uint64_t tv_usec;
} timeval_t;

typedef struct {
    timeval_t it_interval;  /* Repeat interval */
    timeval_t it_value;     /* Initial expiration */
} itimerval_t;

#define ITIMER_REAL    0  /* Delivers SIGALRM */
#define ITIMER_VIRTUAL 1  /* Delivers SIGVTALRM (not implemented) */
#define ITIMER_PROF    2  /* Delivers SIGPROF (not implemented) */

int timer_setitimer(int which, const itimerval_t *newval, itimerval_t *oldval)
{
    if (which != ITIMER_REAL) return 0;  /* Only ITIMER_REAL supported */

    uint32_t pid = proc_current_pid();

    /* Find existing timer or allocate new */
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].pid == pid &&
            timers[i].signal == SIGALRM) {
            if (oldval) {
                uint64_t now = idt_get_ticks();
                uint64_t rem = (timers[i].next_fire > now) ?
                               timers[i].next_fire - now : 0;
                oldval->it_value.tv_sec = rem / 100;
                oldval->it_value.tv_usec = (rem % 100) * 10000;
                oldval->it_interval.tv_sec = timers[i].interval_ticks / 100;
                oldval->it_interval.tv_usec = (timers[i].interval_ticks % 100) * 10000;
            }
            slot = i;
            break;
        }
    }

    if (!newval) return 0;

    uint64_t value_ticks = newval->it_value.tv_sec * 100 +
                           newval->it_value.tv_usec / 10000;
    uint64_t interval_ticks = newval->it_interval.tv_sec * 100 +
                              newval->it_interval.tv_usec / 10000;

    if (value_ticks == 0) {
        /* Disarm timer */
        if (slot >= 0) timers[slot].active = false;
        return 0;
    }

    if (slot < 0) {
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (!timers[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) return -12;  /* ENOMEM */

    timers[slot].active = true;
    timers[slot].pid = pid;
    timers[slot].signal = SIGALRM;
    timers[slot].next_fire = idt_get_ticks() + value_ticks;
    timers[slot].interval_ticks = interval_ticks;

    return 0;
}

/* Cleanup timers for a process (called on exit) */
void timers_cleanup_process(uint32_t pid)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].pid == pid)
            timers[i].active = false;
    }
}
