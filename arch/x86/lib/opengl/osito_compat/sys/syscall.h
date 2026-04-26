/* OsitoK no-op shim for <sys/syscall.h>.
 *
 * Mesa util/u_queue.c uses this for `__NR_gettid`-style numbers gated on
 * `#if defined(__linux__)`. We never reach those calls in single-threaded
 * mode; just make the references compile. */
#ifndef _OSITO_SYS_SYSCALL_H
#define _OSITO_SYS_SYSCALL_H 1

#define SYS_gettid    186
#define SYS_getrandom 318
#define SYS_futex     202

#endif /* _OSITO_SYS_SYSCALL_H */
