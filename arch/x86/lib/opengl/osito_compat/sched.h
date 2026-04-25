/* OsitoK shim — sched.h
 * Mesa's os_time.c uses sched_yield to spin during waits; we provide a no-op. */
#ifndef OSITO_SCHED_H
#define OSITO_SCHED_H 1
static inline int sched_yield(void) { return 0; }
#endif
