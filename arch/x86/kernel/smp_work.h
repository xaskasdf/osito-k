/*
 * OsitoK x86-64 — SMP Work-Stealing Scheduler
 *
 * Chase-Lev work-stealing deques per CPU. Any core can push tasks,
 * idle cores steal from others. Enables nested parallelism: an AP
 * running a matvec can push sub-tasks that BSP or other APs steal.
 *
 * Usage:
 *   smp_work_init();
 *   int tid = smp_submit_any(my_func, arg, result);
 *   smp_wait(tid);  // does useful work (steal/pop) while waiting
 */

#ifndef OSITOK_SMP_WORK_H
#define OSITOK_SMP_WORK_H

#include "../include/types.h"

#define SMP_MAX_APS      15   /* Max application processors (BSP excluded) */
#define SMP_TOTAL_CPUS   16   /* BSP + 15 APs */
#define SMP_IPI_VECTOR   0xFE /* IPI vector for work notification */

#define TASK_POOL_SIZE   64   /* Static task pool (bitmap-allocated) */
#define DEQUE_CAPACITY   16   /* Per-CPU deque depth (must be power of 2) */

typedef void (*smp_work_func_t)(void *arg, void *result);

/* ── Task descriptor ─────────────────────────────────────────── */

typedef struct __attribute__((aligned(64))) {
    smp_work_func_t  func;
    void            *arg;
    void            *result_buf;
    volatile int     completed;      /* 0=pending, 1=done */
    uint32_t         task_id;        /* index into task_pool[] */
    uint8_t          auto_free;      /* 1=executor frees (fire-and-forget) */
    uint8_t          _pad[3];
} smp_task_t;

/* ── Chase-Lev work-stealing deque ───────────────────────────── */

typedef struct __attribute__((aligned(128))) {
    volatile int64_t bottom;         /* modified by owner only */
    uint8_t          _pad0[56];      /* separate cache line */
    volatile int64_t top;            /* modified by thieves via CAS */
    uint8_t          _pad1[56];      /* separate cache line */
    uint32_t         buf[DEQUE_CAPACITY]; /* task_id ring buffer */
} ws_deque_t;

/* ── Per-CPU state ───────────────────────────────────────────── */

typedef struct __attribute__((aligned(64))) {
    ws_deque_t       deque;
    volatile int     sleeping;       /* 1=HLT, needs IPI to wake */
    uint32_t         cpu_index;
    uint32_t         lapic_id;
    uint64_t         tasks_completed;
    uint64_t         tasks_stolen;
} ws_cpu_t;

/* ── Globals ─────────────────────────────────────────────────── */

extern ws_cpu_t  ws_cpus[SMP_TOTAL_CPUS];
extern int       ws_cpu_count;        /* actual online CPUs (BSP + APs) */
extern int       ap_worker_count;     /* ws_cpu_count - 1 (compat) */

/* Legacy compat: tensor.c reads ap_controls[].state via raw pointer.
 * Provide the old symbol pointing at ws_cpus[0] for the AP-idle check. */

/* ── API ─────────────────────────────────────────────────────── */

/* Initialize work-stealing system (after APs are up) */
void smp_work_init(void);

/* Submit work to a specific AP's deque.
 * Returns task_id (>= 0) on success, -1 on failure. */
int smp_submit(int ap_idx, smp_work_func_t func, void *arg, void *result);

/* Submit to own deque, let idle APs steal.
 * Returns task_id (>= 0) on success, -1 on failure.
 * ISR-safe: no locks, bounded-time. */
int smp_submit_any(smp_work_func_t func, void *arg, void *result);

/* Fire-and-forget: executor auto-frees task. No smp_wait needed.
 * Returns task_id (>= 0) on success, -1 on failure. ISR-safe. */
int smp_submit_ff(smp_work_func_t func, void *arg, void *result);

/* Wait for task to complete. While waiting, executes other work
 * from own deque or steals from others (work-stealing wait). */
void smp_wait(int task_id);

/* Wait for ALL deques to drain. */
void smp_barrier(void);

/* Print work-stealing stats per CPU. */
void smp_stats(void);

/* AP-side worker loop (called from smp_ap_entry, never returns). */
void ap_worker_loop(uint32_t cpu_idx);

#endif
