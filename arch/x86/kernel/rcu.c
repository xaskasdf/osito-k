/*
 * OsitoK x86-64 — RCU (Read-Copy-Update)
 *
 * Lock-free read access to shared data structures.
 * Writers create new versions; readers see consistent snapshots.
 * Grace period detection via per-CPU quiescent state tracking.
 *
 * Usage:
 *   rcu_read_lock();
 *   ptr = rcu_dereference(global_ptr);
 *   // use ptr safely, no locks
 *   rcu_read_unlock();
 *
 *   // Writer:
 *   new = kmalloc(...);
 *   *new = *old;
 *   new->field = new_value;
 *   rcu_assign_pointer(global_ptr, new);
 *   synchronize_rcu();  // wait for all readers to finish
 *   kfree(old);
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern void kfree(void *ptr);

/* ── Per-CPU RCU State ───────────────────────────────────────── */

#define RCU_MAX_CPUS 8

typedef struct {
    uint64_t gp_seq;         /* Last grace period this CPU passed through */
    uint32_t nesting;        /* rcu_read_lock nesting depth */
    bool     qs_pending;     /* Quiescent state needed */
} rcu_cpu_t;

static rcu_cpu_t rcu_cpus[RCU_MAX_CPUS];
static uint64_t  rcu_gp_seq;          /* Current grace period sequence */
static uint64_t  rcu_gp_completed;    /* Last completed GP */
static bool      rcu_initialized;

/* Callback queue: functions to call after GP completes */
#define RCU_CB_MAX 64

typedef struct {
    void (*func)(void *data);
    void  *data;
    uint64_t gp_seq;  /* GP that must complete before calling */
} rcu_callback_t;

static rcu_callback_t rcu_callbacks[RCU_CB_MAX];
static int rcu_cb_count;

/* ── Init ────────────────────────────────────────────────────── */

void rcu_init(void)
{
    memset(rcu_cpus, 0, sizeof(rcu_cpus));
    rcu_gp_seq = 1;
    rcu_gp_completed = 0;
    rcu_cb_count = 0;
    rcu_initialized = true;
    serial_puts("[RCU] Initialized\n");
}

/* ── Read-Side Critical Section ──────────────────────────────── */

void rcu_read_lock(void)
{
    /* In a uniprocessor or cooperative kernel, just increment nesting.
     * Preemption disabled implicitly (we don't preempt in RCU sections). */
    rcu_cpus[0].nesting++;
}

void rcu_read_unlock(void)
{
    if (rcu_cpus[0].nesting > 0)
        rcu_cpus[0].nesting--;
    /* If nesting reaches 0, this CPU has passed through a quiescent state */
    if (rcu_cpus[0].nesting == 0)
        rcu_cpus[0].gp_seq = rcu_gp_seq;
}

/* ── Writer-Side: Synchronize ────────────────────────────────── */

/* Start a new grace period and wait for all CPUs to pass through */
void synchronize_rcu(void)
{
    if (!rcu_initialized) return;

    uint64_t target_gp = ++rcu_gp_seq;

    /* Wait for all CPUs to report quiescent state for this GP */
    uint64_t timeout = idt_get_ticks() + 500;  /* 5 second timeout */
    while (idt_get_ticks() < timeout) {
        bool all_passed = true;
        for (int i = 0; i < RCU_MAX_CPUS; i++) {
            if (rcu_cpus[i].nesting > 0 || rcu_cpus[i].gp_seq < target_gp) {
                all_passed = false;
                break;
            }
        }
        if (all_passed) break;
#ifndef __EMSCRIPTEN__
        __asm__ volatile ("pause");
#endif
    }

    rcu_gp_completed = target_gp;

    /* Process callbacks that were waiting for this GP */
    for (int i = 0; i < rcu_cb_count; ) {
        if (rcu_callbacks[i].gp_seq <= rcu_gp_completed) {
            rcu_callbacks[i].func(rcu_callbacks[i].data);
            rcu_callbacks[i] = rcu_callbacks[--rcu_cb_count];
        } else {
            i++;
        }
    }
}

/* ── Deferred Free (call_rcu) ────────────────────────────────── */

/* Schedule a callback to run after the current GP completes */
void call_rcu(void (*func)(void *), void *data)
{
    if (rcu_cb_count >= RCU_CB_MAX) {
        /* Queue full — synchronize immediately */
        synchronize_rcu();
    }
    if (rcu_cb_count < RCU_CB_MAX) {
        rcu_callbacks[rcu_cb_count].func = func;
        rcu_callbacks[rcu_cb_count].data = data;
        rcu_callbacks[rcu_cb_count].gp_seq = rcu_gp_seq;
        rcu_cb_count++;
    }
}

/* Convenience: free memory after GP */
static void rcu_kfree_cb(void *ptr) { kfree(ptr); }
void kfree_rcu(void *ptr) { call_rcu(rcu_kfree_cb, ptr); }

/* ── Scheduler Hook ──────────────────────────────────────────── */

/* Called from scheduler context switch — marks quiescent state */
void rcu_note_context_switch(uint32_t cpu)
{
    if (cpu < RCU_MAX_CPUS && rcu_cpus[cpu].nesting == 0)
        rcu_cpus[cpu].gp_seq = rcu_gp_seq;
}

uint64_t rcu_get_gp_seq(void) { return rcu_gp_seq; }
uint64_t rcu_get_gp_completed(void) { return rcu_gp_completed; }
