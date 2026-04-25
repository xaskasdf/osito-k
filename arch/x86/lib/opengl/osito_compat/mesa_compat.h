#ifndef OSITO_MESA_COMPAT_H
#define OSITO_MESA_COMPAT_H 1

/* Force-included shim that papers over the gap between Mesa's
 * Linux/glibc expectations and OsitoK's freestanding environment.
 * Edited as we add more Mesa subdirs.
 *
 * In W4.2 we extended scope to compile C++ files (compiler/glsl/).
 * Those compile with the hosted gcc:12 libstdc++ (no -nostdinc),
 * which means glibc <stdint.h>, <time.h>, etc. are visible. We
 * gate the OsitoK qjs-libc-only bits with __cplusplus checks so
 * the same header is force-included into both C and C++ TUs. */

#ifndef __cplusplus
#  include <stdint.h>
#  include <stddef.h>
#  include <time.h>   /* OsitoK libc — provides time_t and struct tm */
/* W4.5 — inttypes-style PRI* macros required by mesa/main/{draw,context}.c.
 * Standard C99 <inttypes.h> exists on hosted libc but we're -nostdinc, so
 * paste the canonical glibc-style definitions for x86-64.
 */
#  ifndef PRIxPTR
#    define PRIxPTR  "lx"
#    define PRIdPTR  "ld"
#    define PRIuPTR  "lu"
#  endif
#  ifndef PRId64
#    define PRId64   "ld"
#    define PRIu64   "lu"
#    define PRIx64   "lx"
#    define PRIX64   "lX"
#    define PRIo64   "lo"
#  endif
#  ifndef PRId32
#    define PRId32   "d"
#    define PRIu32   "u"
#    define PRIx32   "x"
#    define PRIX32   "X"
#  endif
#else
   /* C++ side: pull <ctime> via libstdc++ (NOT mesa's src/c11/time.h
    * which is a polyfill that itself recursively #includes <time.h>).
    * <ctime> brings in glibc time.h which defines struct timespec
    * + time_t. We also pull <pthread.h> early so mesa's c11/threads.h
    * sees PTHREAD_ONCE_INIT, pthread_t, etc. (HAVE_PTHREAD branch). */
#  include <cstdint>
#  include <cstddef>
#  include <ctime>
#  include <sched.h>     /* cpu_set_t (used by glibc pthread.h) */
#  include <pthread.h>
#endif

#ifndef __cplusplus
/* Tell Mesa's src/c11/time.h not to redefine struct timespec — we'll
 * define it once below from libc primitives. */
#define HAVE_STRUCT_TIMESPEC 1

#ifndef _STRUCT_TIMESPEC_DEFINED
#define _STRUCT_TIMESPEC_DEFINED
struct timespec { time_t tv_sec; long tv_nsec; };
#endif
#else
/* glibc time.h already gave us struct timespec */
#define HAVE_STRUCT_TIMESPEC 1
#endif

/* errno comes from libc */
#ifndef __cplusplus
extern int errno;
#else
#  include <errno.h>
#endif

/* errno values that OsitoK libc errno.h doesn't define yet — Mesa references
 * a few here and there. Add as needed. */
#ifndef EINTR
#define EINTR  4
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif

#ifndef __cplusplus
/* W4.5 — POSIX-only or libc helpers used by mesa/main/.  These code paths
 * are usually gated on env vars or features we don't enable, but the
 * compiler still needs decls. Bodies (if ever called) live in
 * osito_compat/mesa_libc_stubs.c.
 *
 * NB: mkdir/stat/unlink are declared via osito_compat/sys/stat.h (which
 * is pulled by util/disk_cache.h transitively).  Don't redeclare here.
 */
extern char *strtok_r(char *str, const char *delim, char **saveptr);
#endif

/* OsitoK has no concept of users — return 0 ("root") so any "are we
 * privileged?" check reads as true. In C++ mode hosted glibc <unistd.h>
 * (pulled by libstdc++) supplies these prototypes; only enable shim
 * in C TUs to avoid signature collisions. */
#ifndef __cplusplus
static inline int geteuid(void) { return 0; }
static inline int getuid(void)  { return 0; }
static inline int getegid(void) { return 0; }
static inline int getgid(void)  { return 0; }
static inline int getpid(void)  { return 1; }
#endif

/* clock_nanosleep — Mesa os_time.c uses it for sleep/yield loops. We just
 * return (no kernel sleep API exposed in this layer yet). */
#ifndef __cplusplus
static inline int clock_nanosleep(int clk, int flags, const struct timespec *req, struct timespec *rem) {
    (void)clk; (void)flags; (void)req; (void)rem;
    return 0;
}
#endif

/* Endian — we are unconditionally little-endian on x86-64. */
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define UTIL_ARCH_BIG_ENDIAN    0

/* lrintf is missing from OsitoK libc math.h — declare here so Mesa can use
 * it. We also provide a simple inline fallback that calls lrint(double). */
#ifndef __cplusplus
extern long lrint(double);
static inline long lrintf(float f) { return lrint((double)f); }
/* llrint / llrintf — not in libc math.h. u_pack_color.h uses them for
 * packing depth into 32-bit integer representation. */
static inline long long llrint(double x)  { return (long long)lrint(x); }
static inline long long llrintf(float x)  { return (long long)lrint((double)x); }
/* rintf — libc math.h has rint (double); provide the float form. */
extern double rint(double);
static inline float rintf(float x) { return (float)rint((double)x); }
/* strtoll / strtoull — OsitoK libc has strtol/strtoul; extend to 64-bit. */
extern long long strtoll(const char *s, char **end, int base);
extern unsigned long long strtoull(const char *s, char **end, int base);
#else
/* C++ side: hosted glibc <math.h>, <stdlib.h> have these. Pull headers. */
#  include <math.h>
#  include <stdlib.h>
#  include <string.h>
#endif

/* posix_memalign: wrap libc malloc with manual over-allocation + align.
 * Mesa's ralloc/blob/SIMD codepaths pass alignments of 16/32/64 bytes;
 * returning 8-byte-aligned mem to those would cause #GP on movaps. */
#ifndef __cplusplus
extern void *malloc(size_t);
extern void  free(void *);
static inline int posix_memalign(void **out, size_t a, size_t s) {
    if (a < sizeof(void *) || (a & (a - 1))) return 22; /* EINVAL */
    void *raw = malloc(s + a - 1 + sizeof(void *));
    if (!raw) return 12;  /* ENOMEM */
    void *aligned = (void *)(((uintptr_t)raw + sizeof(void *) + a - 1) & ~(a - 1));
    ((void **)aligned)[-1] = raw;
    *out = aligned;
    return 0;
}
#endif
/* Companion: callers that mix posix_memalign + free won't recover the raw
 * pointer; for now we live with that — Mesa's util/blob.c uses ralloc which
 * is its own pool so doesn't hit free() on an aligned alloc. Document the
 * constraint here so W4.1+ can add a wrapper if needed. */
#define HAVE_POSIX_MEMALIGN 1

/* atexit stub: Mesa's os_misc.c registers options_tbl_fini; on OsitoK we
 * don't run global destructors anyway. No-op preserves link compatibility. */
#ifndef __cplusplus
static inline int atexit(void (*f)(void)) { (void)f; return 0; }
#endif

/* pthread no-ops — Mesa is well-tested in single-threaded mode.
 * In C mode we provide our own trivial typedefs. In C++ mode the
 * hosted glibc + libstdc++ already define pthread_t etc, so we
 * just provide the no-op INLINE wrappers (they don't conflict
 * because they're at namespace scope and inline). The pthread_*
 * functions on glibc are real prototypes — but our static inlines
 * are weak overrides if linked with -Wl,--allow-multiple-definition,
 * else they're not actually emitted (static inline = TU-local). */
#ifndef __cplusplus
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
/* W4.3 — zink uses util/rwlock.h which wraps pthread_rwlock_*. Stub
 * the type + ops so single-threaded compile works. */
typedef int pthread_rwlock_t;
#define PTHREAD_RWLOCK_INITIALIZER 0
static inline int pthread_rwlock_init(pthread_rwlock_t *l, const void *a) { (void)l; (void)a; return 0; }
static inline int pthread_rwlock_destroy(pthread_rwlock_t *l) { (void)l; return 0; }
static inline int pthread_rwlock_rdlock(pthread_rwlock_t *l) { (void)l; return 0; }
static inline int pthread_rwlock_wrlock(pthread_rwlock_t *l) { (void)l; return 0; }
static inline int pthread_rwlock_unlock(pthread_rwlock_t *l) { (void)l; return 0; }
#endif

/* clock_gettime CLOCK_MONOTONIC: reuse our gettimeofday syscall (96) */
#ifndef __cplusplus
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  0
typedef int clockid_t;
extern long syscall(long, ...);
static inline int clock_gettime(int clk, struct timespec *ts) {
    (void)clk;
    struct { long sec; long usec; } tv;
    syscall(96, &tv, 0);
    ts->tv_sec  = tv.sec;
    ts->tv_nsec = tv.usec * 1000;
    return 0;
}
#endif

/* sysconf — names per glibc bits/confname.h, just enough for Mesa util/.
 * In C++ mode glibc's <unistd.h> (pulled by libstdc++ headers) defines
 * these; only enable our shim in pure C TUs. */
#ifndef __cplusplus
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
#endif

/* C11 _Static_assert keyword — qjs_headers/assert.h doesn't define
 * static_assert (it is a C11 keyword via assert.h). Provide it here.
 * In C++ static_assert is a builtin keyword, no shim needed. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert(cond, msg) _Static_assert((cond), msg)
#endif

/* getenv: always NULL on OsitoK */
#ifndef __cplusplus
static inline char *getenv(const char *name) { (void)name; return (char *)0; }
#endif

/* strndup / strnlen — provided by mesa_libc_stubs.c, declared here. */
#ifndef __cplusplus
extern size_t strnlen(const char *s, size_t maxlen);
extern char  *strndup(const char *s, size_t n);
extern int    rand(void);
#endif

/* W4.2 — math constants (libc math.h is freestanding-light) */
#ifndef M_PI
#define M_PI       3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923
#endif
#ifndef M_PI_4
#define M_PI_4     0.78539816339744830962
#endif
#ifndef M_E
#define M_E        2.7182818284590452354
#endif
/* M_LOG2E is provided by qjs_headers/math.h (C) or glibc <math.h> (C++).
 * Don't define it from this force-included header — would always warn on
 * redefinition since this header is processed before any libc math.h. */
#ifndef M_LN2
#define M_LN2      0.69314718055994530942
#endif
#ifndef M_SQRT2
#define M_SQRT2    1.41421356237309504880
#endif
#ifndef HUGE_VAL
#define HUGE_VAL   (__builtin_huge_val())
#endif
#ifndef HUGE_VALF
#define HUGE_VALF  (__builtin_huge_valf())
#endif
#ifndef NAN
#define NAN        (__builtin_nanf(""))
#endif
#ifndef INFINITY
#define INFINITY   (__builtin_inff())
#endif

/* W4.2 — extra glibc-isms used by compiler/{glsl,nir,spirv}/ */
#ifndef __cplusplus
extern char *strdup(const char *s);
extern char *strcasestr(const char *haystack, const char *needle);
extern int   strcasecmp(const char *a, const char *b);
extern int   strncasecmp(const char *a, const char *b, size_t n);
#endif

/* basename / dirname — Mesa shader cache and disk layout helpers.
 * We don't have a filesystem layer for shader caches anyway; provide
 * the simplest correct implementations. NOTE: glibc dirname() mutates
 * its input; we follow that convention.
 *
 * In C++ mode the hosted glibc <libgen.h> may already declare these;
 * we don't force-define our versions there. Mesa C++ files don't seem
 * to call basename/dirname directly. */
#ifndef __cplusplus
static inline char *basename_compat(char *path) {
    if (!path || !*path) return (char *)".";
    char *p, *last = path;
    for (p = path; *p; ++p) if (*p == '/') last = p + 1;
    return last;
}
static inline char *dirname_compat(char *path) {
    if (!path || !*path) return (char *)".";
    int len = 0;
    while (path[len]) len++;
    while (len > 0 && path[len - 1] == '/') len--;
    while (len > 0 && path[len - 1] != '/') len--;
    if (len == 0) return (char *)".";
    while (len > 1 && path[len - 1] == '/') len--;
    path[len] = '\0';
    return path;
}
#define basename(p) basename_compat(p)
#define dirname(p)  dirname_compat(p)

/* W4.4 — dlfcn.h stubs.  Mesa zink_screen.c has a setup_renderdoc()
 * helper gated by ZINK_RENDERDOC=... env var; without that env var the
 * function returns early before calling dlopen.  Provide RTLD_* macros
 * and dlopen/dlsym stubs so the file builds.  Calls return NULL because
 * we never reach this code path on OsitoK (getenv() always NULL). */
#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x4
#define RTLD_GLOBAL 0x8
#define RTLD_NOLOAD 0x10
static inline void *dlopen(const char *file, int flag) { (void)file; (void)flag; return (void *)0; }
static inline void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return (void *)0; }
static inline int   dlclose(void *handle) { (void)handle; return 0; }
static inline char *dlerror(void) { return (char *)0; }

/* W4.4 — sscanf / vasprintf are referenced by zink_screen.c (gated
 * behind ZINK_RENDERDOC and the debug marker callback respectively).
 * Stubs return -1 to mimic "no-match" / "format error". */
typedef __builtin_va_list __mesa_va_list;
/* W4.4-fix: removed static-inline sscanf/vasprintf stubs that lived here.
 * The real implementations now live exclusively in mesa_libc_stubs.c
 * (file-scope extern), so C++ TUs using hosted glibc <stdio.h> see only
 * the libstdc++ declarations and link against our impls — no
 * redefinition collision. C TUs that include this header pick up only
 * the typedef, which is what they actually need. */
#ifndef __cplusplus
extern int sscanf(const char *str, const char *fmt, ...);
extern int vasprintf(char **out, const char *fmt, __mesa_va_list ap);
#endif

/* mmap stubs — Mesa shader cache uses mmap on Linux for file-backed
 * pages. On OsitoK we have no shader cache (no fs persistence beyond
 * the read-only ROM), so make every mmap fail and the caller will
 * fall through to the in-memory path. */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON    MAP_ANONYMOUS
#define MAP_FAILED  ((void *)-1)
typedef long off_t;
static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return MAP_FAILED;
}
static inline int munmap(void *addr, size_t len) { (void)addr; (void)len; return 0; }
static inline int mprotect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }
#endif

/* Tell Mesa code which features are on. HAVE_PTHREAD=1 forces Mesa's
 * c11/threads.h to take the pthread branch — which #include's <pthread.h>;
 * we provide a stub osito_compat/pthread.h that resolves cleanly because
 * the actual pthread surface is provided above by this very header. */
#define HAVE_PTHREAD 1
#define USE_X86_64   1
/* W4.2 — In C++ mode hosted glibc supplies real secure_getenv.
 * Tell Mesa not to redefine it as static inline. */
#ifdef __cplusplus
#  define HAVE_SECURE_GETENV 1
#endif

#endif
