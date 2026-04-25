#ifndef OSITO_MESA_COMPAT_H
#define OSITO_MESA_COMPAT_H 1

/* Force-included shim that papers over the gap between Mesa's
 * Linux/glibc expectations and OsitoK's freestanding environment.
 * Edited as we add more Mesa subdirs. */

#include <stdint.h>
#include <stddef.h>

/* errno comes from libc */
extern int errno;

/* posix_memalign: wrap our libc malloc */
extern void *malloc(size_t);
extern void  free(void *);
static inline int posix_memalign(void **out, size_t a, size_t s) {
    (void)a;
    void *p = malloc(s);
    if (!p) return 12;  /* ENOMEM */
    *out = p;
    return 0;
}

/* pthread no-ops — Mesa is well-tested in single-threaded mode */
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_t;
typedef int pthread_once_t;
typedef int pthread_key_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER  0
#define PTHREAD_ONCE_INIT         0
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *a) { (void)m; (void)a; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const void *a) { (void)c; (void)a; return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c; (void)m; return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_once(pthread_once_t *o, void (*f)(void)) { if (*o == 0) { *o = 1; f(); } return 0; }
static inline int pthread_create(pthread_t *t, const void *a, void *(*f)(void *), void *arg) {
    (void)t; (void)a; (void)f; (void)arg; return 11; /* EAGAIN — refuse to spawn */
}
static inline int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }

/* clock_gettime CLOCK_MONOTONIC: reuse our gettimeofday syscall (96) */
struct timespec { long tv_sec; long tv_nsec; };
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  0
extern long syscall(long, ...);
static inline int clock_gettime(int clk, struct timespec *ts) {
    (void)clk;
    struct { long sec; long usec; } tv;
    syscall(96, &tv, 0);
    ts->tv_sec  = tv.sec;
    ts->tv_nsec = tv.usec * 1000;
    return 0;
}

/* sysconf */
#define _SC_NPROCESSORS_ONLN 84
static inline long sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) return 4;
    return -1;
}

/* getenv: always NULL on OsitoK */
static inline char *getenv(const char *name) { (void)name; return (char *)0; }

/* Tell Mesa code which features are off */
#define HAVE_PTHREAD 0
#define USE_X86_64   1

#endif
