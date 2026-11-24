/* Retained kernel-service wake bits. Not a user handle or NT event. */
#ifndef OSITOK_SCHED_SIGNAL_H
#define OSITOK_SCHED_SIGNAL_H
#if defined(__KERNEL_X86__) || defined(PROCESS_RUNQ_HOST_TEST)
#include "../include/types.h"
#else
#include <stdint.h>
#endif
typedef struct {
    uint32_t bits;
    uint32_t waiters; /* scheduler-owned; atomic read allowed for diagnostics */
} sched_signal_t;
enum sched_signal_result {
    SCHED_SIGNAL_READY, SCHED_SIGNAL_TIMEOUT, SCHED_SIGNAL_RETRY,
    SCHED_SIGNAL_INVALID
};
/* Stable zero-initialized kernel storage. A sender may run on BSP/AP/IRQ;
 * it only publishes bits and requests a BSP scheduler interrupt. Bits combine
 * until taken, including sends before a consumer exists. Keep storage alive
 * through all senders and waits (or until waiting tasks are fully reaped).
 * Multiple waiters recheck their own predicates; one take consumes the bits. */
void sched_signal_set(sched_signal_t *, uint32_t);
uint32_t sched_signal_take(sched_signal_t *);
/* BSP task with IRQs enabled, no subsystem locks or active preemption hold.
 * Does not consume bits. Zero timeout means no timer; nonzero is 10ms ticks.
 * RETRY is a spurious/external wake or inability to switch away; caller must
 * recheck its predicate, not assume completion. AP/IRQ calls are rejected. */
enum sched_signal_result sched_signal_wait(sched_signal_t *, uint64_t ticks);
int sched_signal_selftest(void);
#endif
