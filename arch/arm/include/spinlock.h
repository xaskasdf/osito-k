/*
 * spinlock.h -- spinlocks for Osito-K AArch64
 *
 * Load-acquire/store-exclusive with wfe while waiting (an unlock clears the
 * waiter's exclusive monitor, which wakes it). Plain v8.0 instructions: the
 * kernel builds with -march=armv8.2-a, whose __atomic builtins emit LSE
 * atomics that QEMU's cortex-a72 doesn't have.
 *
 * Until secondary CPUs run tasks (smp_running) locking is skipped: there is
 * nothing to exclude, and exclusives need cacheable memory, which early boot
 * (MMU off) doesn't have.
 */

#ifndef OSITO_ARM_SPINLOCK_H
#define OSITO_ARM_SPINLOCK_H

#include <stdint.h>
#include "aarch64.h"

extern volatile int smp_running;        /* kernel/smp.c */

typedef struct { volatile uint32_t locked; } spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    if (!smp_running)
        return;
    uint32_t tmp;
    __asm__ volatile(
        "   sevl\n"
        "1: wfe\n"
        "2: ldaxr   %w0, [%1]\n"
        "   cbnz    %w0, 1b\n"
        "   stxr    %w0, %w2, [%1]\n"
        "   cbnz    %w0, 2b\n"
        : "=&r"(tmp) : "r"(&l->locked), "r"(1u) : "memory");
}

/* One attempt, no waiting: 1 if it took the lock */
static inline int spin_trylock(spinlock_t *l)
{
    if (!smp_running)
        return 1;
    uint32_t held, fail = 1;
    __asm__ volatile(
        "   ldaxr   %w0, [%2]\n"
        "   cbnz    %w0, 1f\n"
        "   stxr    %w1, %w3, [%2]\n"
        "   b       2f\n"
        "1: clrex\n"
        "2:\n"
        : "=&r"(held), "+&r"(fail) : "r"(&l->locked), "r"(1u) : "memory");
    return !held && !fail;
}

/* Unconditional, so a lock taken just before smp_running changes is still
 * released */
static inline void spin_unlock(spinlock_t *l)
{
    __asm__ volatile("stlr wzr, [%0]" : : "r"(&l->locked) : "memory");
}

/* Holding a lock with IRQs on would let the tick switch to a task that then
 * spins on it, on the same CPU */
static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags = irq_save();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

#endif
