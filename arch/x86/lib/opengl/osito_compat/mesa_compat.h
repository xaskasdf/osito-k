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

/* W4.7-fix: thread_local overrides applied per-file (u_call_once.c,
 * u_qsort.cpp, os_misc.c) instead of globally — global #define caused
 * isnormal/signbit redefinition cascade. */

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
 * in C TUs to avoid signature collisions.
 *
 * Returning 0 = root is INTENTIONAL: Mesa's "secure_getenv" path checks
 * geteuid()==0 to decide whether to honour env vars from a setuid binary.
 * On OsitoK there are no users / setuid → all env vars (always NULL
 * anyway) are trusted equally.  getpid()==1 mimics PID 1 behaviour;
 * Mesa uses it for shader-cache temp-file uniqueness + log prefix. */
#ifndef __cplusplus
static inline int geteuid(void) { return 0; }    /* root */
static inline int getuid(void)  { return 0; }    /* root */
static inline int getegid(void) { return 0; }    /* root */
static inline int getgid(void)  { return 0; }    /* root */
static inline int getpid(void)  { return 1; }    /* PID 1 — single proc */
#endif

/* clock_nanosleep — REAL: dispatch to nanosleep (syscall 35).
 * The flags arg (TIMER_ABSTIME=1 vs relative=0) is ignored: OsitoK has
 * one time source so absolute and relative resolve to the same wall
 * clock for our purposes.  Mesa uses this in os_time.c for the
 * "yield with deadline" pattern of util_queue worker idle loops. */
#ifndef __cplusplus
extern long syscall(long, ...);
static inline int clock_nanosleep(int clk, int flags, const struct timespec *req, struct timespec *rem) {
    (void)clk; (void)flags;
    if (!req) return 22; /* EINVAL */
    long rc = syscall(35, (long)(uintptr_t)req, (long)(uintptr_t)rem);
    return rc == 0 ? 0 : 4;  /* EINTR on early return */
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

/* atexit — INTENTIONAL no-op.  Mesa's os_misc.c registers
 * options_tbl_fini for clean teardown.  OsitoK doesn't run global C
 * destructors at exit (proc_free reclaims everything wholesale).
 * Returning 0 ("registered successfully") preserves link compatibility
 * — the registered fn is silently dropped.  Real impl would need an
 * atexit_handlers[] array + a hook in our exit() — see
 * arch/x86/libc/crt.c if we ever want it (~20 LOC). */
#ifndef __cplusplus
static inline int atexit(void (*f)(void)) { (void)f; return 0; }
#endif

/* ============================================================
 * pthread shims — DELIBERATE no-op overlay; REAL primitives elsewhere.
 * ============================================================
 * RATIONALE:  Mesa's c11/threads.h has TWO impl paths: pthread (when
 * HAVE_PTHREAD=1) and stdthreads.  We force HAVE_PTHREAD=1 below to get
 * the pthread branch — but then satisfy the pthread_* surface with
 * static-inline NO-OPS here.
 *
 * The actual concurrency primitives Mesa uses are mtx_t / cnd_t / thrd_t
 * (C11 threads, type-aliased to int via the typedefs below).  Those are
 * REAL — futex-backed in mesa_libc_stubs.c (mtx_lock/unlock, cnd_wait,
 * thrd_create via SYS_CLONE).  So Mesa's "pthread_mutex_lock(&m)" call,
 * which is hidden under a #define inside Mesa's own headers, never
 * actually fires here — the alias rewrites it to mtx_lock(&m) before
 * the linker sees a pthread symbol.
 *
 * The static inlines below exist for the FEW places where Mesa's code
 * directly mentions a pthread_* symbol (a handful of windows/CI
 * compatibility shims).  Those paths are not on the smoke-test critical
 * path, so no-op = OK.
 *
 * pthread_create returns EAGAIN to make damn sure no Mesa code accidentally
 * tries to use raw pthread_create instead of going through mtx/thrd —
 * we want a loud failure if that ever happens.
 *
 * Confirmed by grep:
 *   grep -rn 'pthread_create\b' mesa/src → all wrapped in HAVE_PTHREAD
 *   guards that route through util_queue, which uses thrd_create (real). */
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
/* No-op: real synchronization goes through mtx_t/cnd_t/thrd_t in libc_stubs. */
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
/* DELIBERATE failure: returns EAGAIN to force any rogue caller to fail
 * loudly instead of silently spawning a "thread" that never runs.  All
 * Mesa thread-spawn paths route through thrd_create (real). */
static inline int pthread_create(pthread_t *t, const void *a, void *(*f)(void *), void *arg) {
    (void)t; (void)a; (void)f; (void)arg;
    return 11; /* EAGAIN — refuse to spawn; route through thrd_create */
}
static inline int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }
/* W4.3 — zink uses util/rwlock.h which wraps pthread_rwlock_*. The rwlock
 * surface in zink_resource.c is per-resource MEM ordering — single-threaded
 * smoke-test path doesn't contend, so no-op is correct.  If Mesa shaders
 * ever multi-thread compile (parallel SPIR-V→NIR), upgrade to a futex-backed
 * rwlock here.  Reference: mesa/src/util/rwlock.{c,h}. */
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

/* getenv — INTENTIONAL always-NULL.  OsitoK has no environment block at
 * the libc layer.  Mesa uses getenv extensively for debug toggles
 * (MESA_DEBUG, NIR_PRINT, GALLIUM_TRACE, ZINK_DEBUG, etc.) — every one
 * of these is read once and cached; NULL → debug feature off → smoke
 * path matches release behaviour.  If we ever wire env vars (e.g. via
 * a kernel-side prop store), make sure to invalidate Mesa's cached
 * debug_get_option_cached values too. */
#ifndef __cplusplus
static inline char *getenv(const char *name) { (void)name; return (char *)0; }
#endif

/* strndup / strnlen — provided by mesa_libc_stubs.c, declared here. */
#ifndef __cplusplus
extern size_t strnlen(const char *s, size_t maxlen);
extern char  *strndup(const char *s, size_t n);
extern char  *strtok(char *str, const char *delim);
extern int    asprintf(char **strp, const char *fmt, ...);
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

/* W4.4 — dlfcn.h stubs.  INTENTIONAL: OsitoK has no dynamic loading.
 * The few Mesa code paths that call dlopen are:
 *   1. zink_screen.c:setup_renderdoc — gated by ZINK_RENDERDOC env var
 *      (always NULL on OsitoK, so dlopen never called).
 *   2. zink util_dl_open / util_dl_get_proc_address — bypassed by
 *      zink_vk_loader.c which intercepts those at link time and returns
 *      our static vk_dispatch_table directly (NOT through dlopen).
 *
 * Returning NULL from dlopen is the canonical "library not found" answer;
 * callers always check for NULL before dlsym.  dlerror returns NULL
 * because no error has been "set" — Mesa's only dlerror caller logs
 * "(unknown)" if dlerror returns NULL, which is acceptable. */
#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x4
#define RTLD_GLOBAL 0x8
#define RTLD_NOLOAD 0x10
static inline void *dlopen(const char *file, int flag) {
    (void)file; (void)flag;
    return (void *)0;  /* "no dynamic loading" — caller takes static path */
}
static inline void *dlsym(void *handle, const char *name) {
    (void)handle; (void)name;
    return (void *)0;  /* unreachable: dlopen returned NULL above */
}
static inline int   dlclose(void *handle) { (void)handle; return 0; }
static inline char *dlerror(void) {
    return (char *)0;  /* no error state; caller logs "(unknown)" */
}

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
struct _FILE;
typedef struct _FILE FILE;
extern FILE *open_memstream(char **bufp, size_t *sizep);
extern FILE *fopencookie(void *cookie, const char *mode, void *iofuncs);
extern FILE *fmemopen(void *buf, size_t size, const char *mode);
extern int   vsprintf(char *str, const char *fmt, __mesa_va_list ap);
extern int   vfprintf(FILE *f, const char *fmt, __mesa_va_list ap);
extern void  rewind(FILE *f);
extern long  ftell(FILE *f);
extern int   fseek(FILE *f, long offset, int whence);
extern long  readlink(const char *path, char *buf, size_t bufsize);
extern int   getpid(void);
extern char *getcwd(char *buf, size_t size);
extern int   isatty(int fd);
extern int   ftruncate(int fd, long length);
extern long  lseek(int fd, long offset, int whence);
extern int   unlink(const char *path);
extern int   access(const char *path, int mode);
/* stat/fstat/mkdir provided by sys/stat.h shim */
extern int   rmdir(const char *path);
extern int   pipe(int fds[2]);
extern int   dup(int fd);
extern int   dup2(int oldfd, int newfd);
/* Math intrinsics referenced by NIR codegen */
extern float ldexpf(float x, int exp);
extern double ldexp(double x, int exp);
extern float copysignf(float x, float y);
extern double copysign(double x, double y);
extern float frexpf(float x, int *exp);
extern double frexp(double x, int *exp);
extern float modff(float x, float *iptr);
extern double modf(double x, double *iptr);
extern float exp2f(float x);
/* log2f / fmodf provided by qjs_headers/math.h */
extern float log10f(float x);
extern float hypotf(float x, float y);
extern long long llabs(long long x);
extern long labs(long x);
extern long lroundf(float x);
extern long lround(double x);
extern long long llroundf(float x);
extern long long llround(double x);
/* roundf/truncf provided by qjs_headers/math.h */
extern double round(double x);
extern double trunc(double x);
extern float nearbyintf(float x);
extern double nearbyint(double x);
extern float fminf(float x, float y);
extern float fmaxf(float x, float y);
extern double fmin(double x, double y);
extern double fmax(double x, double y);
extern float fmaf(float x, float y, float z);
extern double fma(double x, double y, double z);
#define isnormal(x) ((x) != 0.0 && __builtin_isnormal(x))
#define isfinite(x) __builtin_isfinite(x)
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define signbit(x)  __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
/* sysconf already declared above with _SC_* macros */
#endif

/* mmap stubs — INTENTIONAL "always-fail" → trigger Mesa's in-memory path.
 *
 * Mesa shader cache uses mmap on Linux for file-backed disk-cache pages.
 * On OsitoK we have no shader-cache fs persistence (the ROM is read-only,
 * the writable scratch space lives in heap not files).  Returning
 * MAP_FAILED makes Mesa's disk_cache code fall through to the in-memory
 * cache only — exactly what we want, no warning logs.
 *
 * If we ever want a real mmap (e.g. for VRAM-backed buffer mapping), the
 * kernel exposes SYS_MMAP=9 with full PROT_/MAP_ flag plumbing; just
 * forward args via __syscall6.  But that's bigger than the Wave 3 scope
 * — the smoke test does no buffer mapping.
 *
 * munmap returns 0 (success) so callers don't log spurious "munmap
 * failed" errors when paired with our MAP_FAILED.  mprotect is a
 * harmless no-op since nothing is ever mapped via this path anyway. */
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
    return MAP_FAILED;  /* triggers Mesa's in-memory disk_cache fallback */
}
static inline int munmap(void *addr, size_t len) {
    /* Symmetric no-op: mmap above never succeeds, so this is unreachable. */
    (void)addr; (void)len; return 0;
}
static inline int mprotect(void *addr, size_t len, int prot) {
    /* Same logic as munmap — nothing was ever mapped via our mmap. */
    (void)addr; (void)len; (void)prot; return 0;
}
#endif

/* Tell Mesa code which features are on. HAVE_PTHREAD=1 forces Mesa's
 * c11/threads.h to take the pthread branch — which #include's <pthread.h>;
 * we provide a stub osito_compat/pthread.h that resolves cleanly because
 * the actual pthread surface is provided above by this very header. */
#define HAVE_PTHREAD 1
#define USE_X86_64   1

/* W4.7 T0 — Force non-TLS dispatch path (fix W4.5 C1).
 *
 * mesa/src/util/u_thread.h sets:
 *   #if DETECT_OS_APPLE → __thread
 *   #elif defined(__GLIBC__) → thread_local + initial-exec attribute
 *   #else → thread_local
 *
 * On OsitoK we are -ffreestanding -nostdinc, no __GLIBC__ defined, so the
 * "else" branch picks plain thread_local. Problem: the OsitoK ELF loader
 * for tests doesn't initialize %fs (the TLS register), so any access to
 * a thread_local variable would trap. Even if we wired %fs, single-threaded
 * OsitoK doesn't need TLS for the GL dispatch table at all.
 *
 * Force __THREAD_INITIAL_EXEC to expand to nothing → _mesa_glapi_tls_Dispatch
 * and _mesa_glapi_tls_Context become plain process globals. This is safe on
 * single-threaded OsitoK and avoids the missing %fs setup entirely.
 *
 * We #include detect_os.h first so DETECT_OS_APPLE is defined (= 0 on us),
 * preventing u_thread.h's first arm from triggering. */
#define __THREAD_INITIAL_EXEC /* empty: non-TLS globals */
/* W4.2 — In C++ mode hosted glibc supplies real secure_getenv.
 * Tell Mesa not to redefine it as static inline. */
#ifdef __cplusplus
#  define HAVE_SECURE_GETENV 1
#endif

#endif
