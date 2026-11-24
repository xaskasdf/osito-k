/*
 * OsitoK x86-64 — SMP (Symmetric Multi-Processing)
 *
 * X-SMP: AP startup, per-CPU state, spinlocks.
 * Wakes Application Processors via INIT-SIPI-SIPI sequence.
 */

#ifndef OSITOK_SMP_H
#define OSITOK_SMP_H

#include "../include/types.h"

/* ── Configuration ──────────────────────────────────────────── */

#define SMP_MAX_CPUS     16    /* Max supported CPUs */
#define SMP_AP_STACK_SIZE  (64 * 1024)  /* 64KB per AP stack */

/* ── Per-CPU State ──────────────────────────────────────────── */

typedef struct {
    uint32_t apic_id;       /* Local APIC ID */
    uint32_t cpu_index;     /* 0 = BSP, 1+ = APs */
    bool     online;        /* CPU is running */
    bool     bsp;           /* Is Bootstrap Processor */
    uint64_t stack_top;     /* Top of kernel stack */
} cpu_info_t;

/* ── Spinlock ───────────────────────────────────────────────── */

typedef volatile uint32_t spinlock_t;

#define SPINLOCK_INIT  0

static inline void spin_lock(spinlock_t *lock)
{
    while (__sync_lock_test_and_set(lock, 1))
        __asm__ volatile ("pause" ::: "memory");
}

static inline void spin_unlock(spinlock_t *lock)
{
    __sync_lock_release(lock);
}

static inline int spin_trylock(spinlock_t *lock)
{
    return __sync_lock_test_and_set(lock, 1) == 0;
}

/* ── API ────────────────────────────────────────────────────── */

/* Initialize SMP: parse MADT, start APs.
 * Must be called after idt_init() and paging_init(). */
void smp_init(void);

/* Get number of online CPUs */
uint32_t smp_cpu_count(void);

/* Get per-CPU info */
cpu_info_t *smp_cpu_info(uint32_t index);

/* Get current CPU index (via APIC ID) */
uint32_t smp_current_cpu(void);

#endif /* OSITOK_SMP_H */
