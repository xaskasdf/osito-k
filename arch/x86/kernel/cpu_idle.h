#ifndef OSITOK_CPU_IDLE_H
#define OSITOK_CPU_IDLE_H

#include "../include/types.h"

/* CPU indices are the MADT/SMP indices, not work-stealing deque slots. */
void cpu_idle_init_cpu(uint32_t cpu);
void cpu_idle_poll(uint32_t cpu);
/* Publish a wake event before sending the AP's IPI. */
void cpu_idle_notify(uint32_t cpu);
/* Caller has disabled IRQs and checked its work queues. Returns with IF=1. */
void cpu_idle_wait(uint32_t cpu, const volatile uint32_t *word, uint32_t expected);
void cpu_idle_dump(void);
void cpu_idle_set(bool mwait);
void cpu_idle_selftest(void);

/* SysV, IF=0 for a real wait. 0=fault, 1=MWAIT returned, 2=word changed. */
int x86_mwait_safe(const volatile uint32_t *word, uint32_t expected, uint32_t hint);

#endif
