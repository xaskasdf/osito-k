#ifndef OSITO_MESA_COMPAT_H
#define OSITO_MESA_COMPAT_H 1

/* Force-included shim that papers over the gap between Mesa's
 * Linux/glibc expectations and OsitoK's freestanding environment.
 * Edited as we add more Mesa subdirs. */

#include <stdint.h>
#include <stddef.h>
#include <time.h>   /* OsitoK libc — provides time_t and struct tm */

/* Tell Mesa's src/c11/time.h not to redefine struct timespec — we'll
 * define it once below from libc primitives. */
#define HAVE_STRUCT_TIMESPEC 1

#ifndef _STRUCT_TIMESPEC_DEFINED
#define _STRUCT_TIMESPEC_DEFINED
struct timespec { time_t tv_sec; long tv_nsec; };
#endif

/* errno comes from libc */
extern int errno;

/* errno values that OsitoK libc errno.h doesn't define yet — Mesa references
 * a few here and there. Add as needed. */
#ifndef EINTR
#define EINTR  4
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

/* OsitoK has no concept of users — return 0 ("root") so any "are we
 * privileged?" check reads as true. */
static inline int geteuid(void) { return 0; }
static inline int getuid(void)  { return 0; }
static inline int getegid(void) { return 0; }
static inline int getgid(void)  { return 0; }
static inline int getpid(void)  { return 1; }

/* clock_nanosleep — Mesa os_time.c uses it for sleep/yield loops. We just
 * return (no kernel sleep API exposed in this layer yet). */
static inline int clock_nanosleep(int clk, int flags, const struct timespec *req, struct timespec *rem) {
    (void)clk; (void)flags; (void)req; (void)rem;
    return 0;
}

/* Endian — we are unconditionally little-endian on x86-64. */
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define UTIL_ARCH_BIG_ENDIAN    0

/* lrintf is missing from OsitoK libc math.h — declare here so Mesa can use
 * it. We also provide a simple inline fallback that calls lrint(double). */
extern long lrint(double);
static inline long lrintf(float f) { return lrint((double)f); }

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
typedef int pthread_barrier_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;
typedef int pthread_attr_t;
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

/* sysconf — names per glibc bits/confname.h, just enough for Mesa util/ */
#define _SC_PAGE_SIZE        30
#define _SC_PAGESIZE         _SC_PAGE_SIZE
#define _SC_PHYS_PAGES       85
#define _SC_AVPHYS_PAGES     86
#define _SC_NPROCESSORS_ONLN 84
static inline long sysconf(int name) {
    switch (name) {
    case _SC_NPROCESSORS_ONLN: return 4;
    case _SC_PAGE_SIZE:        return 4096;
    case _SC_PHYS_PAGES:       return (1L << 30) / 4096;  /* claim 4 GiB */
    case _SC_AVPHYS_PAGES:     return (1L << 29) / 4096;  /* claim 2 GiB free */
    default:                   return -1;
    }
}

/* C11 _Static_assert keyword — qjs_headers/assert.h doesn't define static_assert
 * (it is a C11 keyword via assert.h). Provide it here. */
#ifndef static_assert
#define static_assert(cond, msg) _Static_assert((cond), msg)
#endif

/* getenv: always NULL on OsitoK */
static inline char *getenv(const char *name) { (void)name; return (char *)0; }

/* strndup / strnlen — provided by mesa_libc_stubs.c, declared here. */
extern size_t strnlen(const char *s, size_t maxlen);
extern char  *strndup(const char *s, size_t n);
extern int    rand(void);

/* Tell Mesa code which features are on. HAVE_PTHREAD=1 forces Mesa's
 * c11/threads.h to take the pthread branch — which #include's <pthread.h>;
 * we provide a stub osito_compat/pthread.h that resolves cleanly because
 * the actual pthread surface is provided above by this very header. */
#define HAVE_PTHREAD 1
#define USE_X86_64   1

#endif
