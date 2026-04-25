/* OsitoK shim — sched.h
 * Mesa's os_time.c uses sched_yield to spin during waits; we provide a no-op.
 * In C++ mode (W4.2+) we delegate to hosted glibc <sched.h> via include_next
 * so cpu_set_t (referenced by glibc <pthread.h>) is available.
 */
#ifndef OSITO_SCHED_H
#define OSITO_SCHED_H 1
#ifdef __cplusplus
#  include_next <sched.h>
#else
static inline int sched_yield(void) { return 0; }
#endif
#endif
