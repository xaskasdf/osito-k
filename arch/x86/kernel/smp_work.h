/*
 * OsitoK x86-64 — SMP Work Distribution
 *
 * Activates AP cores as general-purpose workers. The BSP publishes
 * tasks to per-AP control blocks and sends IPIs to wake sleeping APs.
 * Each AP executes the task and signals completion.
 *
 * Usage:
 *   smp_work_init();                          // after APs are up
 *   int ap = smp_submit_any(my_func, arg, result);
 *   smp_wait(ap);                             // block until done
 */

#ifndef OSITOK_SMP_WORK_H
#define OSITOK_SMP_WORK_H

#include "../include/types.h"

#define SMP_MAX_APS     15   /* Max application processors (BSP excluded) */
#define SMP_IPI_VECTOR  0xFE /* IPI vector for work notification */

typedef void (*smp_work_func_t)(void *arg, void *result);

/* Per-AP control block — cache-line aligned to avoid false sharing */
typedef struct __attribute__((aligned(64))) {
    volatile int     state;          /* 0=offline, 1=idle, 2=busy */
    volatile int     work_pending;   /* 1 = new work in slot */
    smp_work_func_t  func;
    void            *arg;
    void            *result_buf;
    volatile int     done;           /* set to 1 by AP when complete */
    uint32_t         lapic_id;
    uint64_t         tasks_completed;
} ap_control_t;

enum { AP_OFFLINE = 0, AP_IDLE = 1, AP_BUSY = 2 };

extern ap_control_t ap_controls[SMP_MAX_APS];
extern int          ap_worker_count;

/* Initialize work system (called from main.c after smp_init) */
void smp_work_init(void);

/* Submit work to a specific AP. Returns 0 on success, -1 if busy. */
int smp_submit(int ap_idx, smp_work_func_t func, void *arg, void *result);

/* Submit to any idle AP. Returns ap_idx on success, -1 if all busy. */
int smp_submit_any(smp_work_func_t func, void *arg, void *result);

/* Wait for a specific AP to finish its current work. */
void smp_wait(int ap_idx);

/* Wait for ALL busy APs to finish. */
void smp_barrier(void);

/* AP-side worker loop (called from smp_ap_entry, never returns). */
void ap_worker_loop(uint32_t cpu_idx);

#endif
