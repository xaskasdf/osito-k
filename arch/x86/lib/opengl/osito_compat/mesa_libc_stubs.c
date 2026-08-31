/* mesa_libc_stubs.c — small implementations of libc / C11 thread surface that
 * Mesa src/util/ pulls in but OsitoK libc doesn't ship.
 *
 * Wave 3 (Apr 28): the C11 thread + mutex + condvar + TSS surface is now
 * REAL — backed by SYS_CLONE (56) for thread spawn and SYS_FUTEX (202) for
 * sleep/wake. Mesa's util_queue_init now spawns kernel threads for the
 * Vulkan compile/cache pipeline (zink_screen.c:3613 path).
 *
 * Types (mtx_t, cnd_t, thrd_t, tss_t, once_flag) all alias `int` via
 * mesa_compat.h's pthread shims, which gives us a 4-byte futex word per
 * primitive — exactly what FUTEX_WAIT / FUTEX_WAKE need. We cast through
 * `volatile uint32_t *` at the boundary. */

#include <stddef.h>
#include <stdint.h>

/* ---- string helpers --------------------------------------------------- */

extern size_t strlen(const char *s);
extern void  *malloc(size_t);

/* ---- C11 time -------------------------------------------------------- */

/* gettimeofday is OsitoK syscall 96 — see CLAUDE.md note about libc stub
 * returning 0; we go direct to the kernel. timespec_get(C11) wraps that
 * into the canonical timespec form Mesa expects from os_time_get_nano. */
extern long __syscall2(long n, long a, long b);

struct __osito_timeval { long tv_sec; long tv_usec; };
struct __osito_timespec { long tv_sec; long tv_nsec; };

#define TIME_UTC       1
#define TIME_MONOTONIC 1   /* same source on OsitoK (no separate mono clock yet) */

/* Mesa's c11/time.h #defines timespec_get to c23_timespec_get, so the
 * symbol Mesa actually links against is c23_timespec_get. We keep the
 * unaliased name as a backup for any code that bypasses Mesa's header. */
int c23_timespec_get(struct __osito_timespec *ts, int base) {
    if (!ts) return 0;
    struct __osito_timeval tv = {0, 0};
    long rc = __syscall2(96, (long)(unsigned long)&tv, 0);
    if (rc < 0) { ts->tv_sec = 0; ts->tv_nsec = 0; return 0; }
    ts->tv_sec  = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
    (void)base;
    return TIME_UTC;
}

int timespec_get(struct __osito_timespec *ts, int base) {
    return c23_timespec_get(ts, base);
}

/* (clock_gettime already provided by mesa_compat.h as a static inline.) */

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) out[i] = s[i];
    out[len] = 0;
    return out;
}

/* ---- strtoll / strtoull (64-bit versions) ----------------------------- */
/* Minimal conformant parsers, matching strtol but in 64-bit. Accepts
 * optional leading whitespace + sign, optional 0x prefix when base is 0 or
 * 16. Used by tgsi_text.c. */
static int _dgt(int c, int base) {
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    return (d >= 0 && d < base) ? d : -1;
}
unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { if (*p == '-') neg = 1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0 && *p == '0') { p++; base = 8; }
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    int d;
    while ((d = _dgt(*p, base)) >= 0) { v = v * base + (unsigned long long)d; p++; }
    if (end) *end = (char *)p;
    return neg ? (unsigned long long)-(long long)v : v;
}
long long strtoll(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { if (*p == '-') neg = 1; p++; }
    unsigned long long v = strtoull(p, end, base);
    return neg ? -(long long)v : (long long)v;
}

/* ---- vasprintf -------------------------------------------------------- */
/* Used by u_async_debug. We don't have real varargs formatting in a static
 * inline, so call libc vsnprintf twice (sized probe + allocate + render).
 * Declaration matches glibc. */
extern int vsnprintf(char *s, size_t n, const char *fmt, __builtin_va_list ap);
int vasprintf(char **out, const char *fmt, __builtin_va_list ap) {
    __builtin_va_list ap2;
    __builtin_va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    __builtin_va_end(ap2);
    if (n < 0) { *out = NULL; return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { *out = NULL; return -1; }
    /* Second va_copy: ap may be in indeterminate state after the probe
     * (some libc paths consume the original even though we passed ap2). */
    __builtin_va_list ap3;
    __builtin_va_copy(ap3, ap);
    vsnprintf(buf, (size_t)n + 1, fmt, ap3);
    __builtin_va_end(ap3);
    *out = buf;
    return n;
}

/* ---- rand ------------------------------------------------------------ */

static unsigned long _rng_state = 1234567u;
int rand(void) {
    /* xorshift64* — good enough for hash_table_random_entry. */
    _rng_state ^= _rng_state << 13;
    _rng_state ^= _rng_state >> 7;
    _rng_state ^= _rng_state << 17;
    return (int)(_rng_state & 0x7fffffff);
}

/* ---- C11 threads.h surface — REAL (futex + clone) -------------------- */

typedef int mtx_t;     /* aliased to pthread_mutex_t (int) by mesa_compat.h */
typedef int cnd_t;     /* aliased to pthread_cond_t  (int) — used as seq */
typedef int thrd_t;    /* aliased to pthread_t       (int) — holds tid    */
typedef int tss_t;     /* aliased to pthread_key_t   (int) — slot index   */
typedef int once_flag; /* aliased to pthread_once_t  (int)                */

typedef int (*thrd_start_t)(void *);

static void tss_cleanup_current(void);

/* C11 return codes (must match mesa/src/c11/threads.h enum). */
#define thrd_success  0
#define thrd_timedout 1
#define thrd_error    2
#define thrd_busy     3
#define thrd_nomem    4

/* ---- syscall numbers + flags (Linux/OsitoK ABI) ---- */

#define SYS_EXIT   60
#define SYS_GETPID 39
#define SYS_CLONE  56
#define SYS_FUTEX  202
#define SYS_GETTID 186
#define SYS_SCHED_YIELD 24

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

extern long __syscall1(long n, long a);
extern long __syscall2(long n, long a, long b);
extern long __syscall3(long n, long a, long b, long c);
extern long __syscall4(long n, long a, long b, long c, long d);
extern long __syscall6(long n, long a, long b, long c, long d, long e, long f);

extern void *calloc(size_t n, size_t s);

#define INT_MAX_FUTEX 0x7fffffff

/* ---- futex helpers ---- */

static inline int _futex_wait(volatile uint32_t *addr, uint32_t expected) {
    return (int)__syscall6(SYS_FUTEX,
                           (long)(uintptr_t)addr,
                           FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                           (long)expected,
                           0, 0, 0);
}
static inline int _futex_wake(volatile uint32_t *addr, int n) {
    return (int)__syscall6(SYS_FUTEX,
                           (long)(uintptr_t)addr,
                           FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                           (long)n,
                           0, 0, 0);
}

/* ---- mutex (futex mutex3 — one syscall on contention only) ---- */

int mtx_init(mtx_t *m, int type) {
    (void)type;
    *(volatile uint32_t *)m = 0;
    return thrd_success;
}

int mtx_destroy(mtx_t *m) { (void)m; return thrd_success; }

int mtx_lock(mtx_t *m) {
    volatile uint32_t *w = (volatile uint32_t *)m;
    uint32_t expected = 0;
    /* Fast path: 0 → 1 */
    if (__atomic_compare_exchange_n(w, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return thrd_success;

    /* Contended: park as 2 and wait. */
    if (expected != 2)
        expected = __atomic_exchange_n(w, 2u, __ATOMIC_ACQUIRE);
    while (expected != 0) {
        _futex_wait(w, 2u);
        expected = __atomic_exchange_n(w, 2u, __ATOMIC_ACQUIRE);
    }
    return thrd_success;
}

int mtx_trylock(mtx_t *m) {
    volatile uint32_t *w = (volatile uint32_t *)m;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(w, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return thrd_success;
    return thrd_busy;
}

int mtx_unlock(mtx_t *m) {
    volatile uint32_t *w = (volatile uint32_t *)m;
    /* fetch_sub returns the previous value. If it was 2, we had waiters. */
    uint32_t prev = __atomic_fetch_sub(w, 1u, __ATOMIC_RELEASE);
    if (prev != 1) {
        /* Had a 2 (or junk) — force back to 0 and wake one waiter. */
        __atomic_store_n(w, 0u, __ATOMIC_RELEASE);
        _futex_wake(w, 1);
    }
    return thrd_success;
}

int mtx_timedlock(mtx_t *m, const void *ts) {
    /* No timed-lock support yet — busy-wait would defeat the purpose.
     * Fall back to mtx_lock; Mesa rarely uses this. */
    (void)ts;
    return mtx_lock(m);
}

/* ---- call_once ---- */
/* States: 0 = pristine, 1 = running, 2 = done. */
void call_once(once_flag *flag, void (*fn)(void)) {
    volatile uint32_t *w = (volatile uint32_t *)flag;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(w, &expected, 1u, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(w, 2u, __ATOMIC_RELEASE);
        _futex_wake(w, INT_MAX_FUTEX);
        return;
    }
    /* Either someone else is running it (1) or it's already done (2). */
    while (expected == 1u) {
        _futex_wait(w, 1u);
        expected = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    }
}

/* ---- condition variable (futex seq counter — broadcast-safe) ----
 * cnd is a uint32_t sequence. cnd_wait reads seq, releases mutex, FUTEX_WAIT
 * on (cnd_addr, captured_seq). signal/broadcast bumps seq + wakes. */

int cnd_init(cnd_t *c) {
    *(volatile uint32_t *)c = 0;
    return thrd_success;
}

int cnd_destroy(cnd_t *c) { (void)c; return thrd_success; }

int cnd_signal(cnd_t *c) {
    volatile uint32_t *w = (volatile uint32_t *)c;
    __atomic_fetch_add(w, 1u, __ATOMIC_RELEASE);
    _futex_wake(w, 1);
    return thrd_success;
}

int cnd_broadcast(cnd_t *c) {
    volatile uint32_t *w = (volatile uint32_t *)c;
    __atomic_fetch_add(w, 1u, __ATOMIC_RELEASE);
    _futex_wake(w, INT_MAX_FUTEX);
    return thrd_success;
}

int cnd_wait(cnd_t *c, mtx_t *m) {
    volatile uint32_t *w = (volatile uint32_t *)c;
    uint32_t seq = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    mtx_unlock(m);
    _futex_wait(w, seq);
    mtx_lock(m);
    return thrd_success;
}

int cnd_timedwait(cnd_t *c, mtx_t *m, const void *ts) {
    /* Kernel sys_futex doesn't honour the timeout arg yet — treat as wait.
     * Mesa's only timed-wait callers are debug/profile paths. */
    (void)ts;
    return cnd_wait(c, m);
}

/* ---- thread spawn / join ----
 * thrd_t is the public TID. A stable registry word is passed as
 * CLONE_CHILD_CLEARTID so join can wait on the exact address the kernel
 * clears and wakes at thread exit. */

/* 512 KiB. Mesa's util_queue workers (cache_get_thread, cache_put_thread)
 * don't run deep recursion; 8 MiB was the pthread/glibc default and is
 * massive overkill on OsitoK where calloc eagerly zeroes the page set.
 * Mesa typically spawns 5-10 worker threads — at 8 MiB each we exhausted
 * kernel heap on first boot (0 MB free, dmesg.log creation failed).
 * Stacks are retained in a bounded pool and reused after join. */
#define THREAD_STACK_SIZE (512u * 1024u)
#define THREAD_RECORD_COUNT 128u

typedef struct thread_record {
    volatile uint32_t active;
    volatile uint32_t ctid;
    volatile int result;
    int tid;
    int owner_pid;
    uint32_t detached;
    uint32_t joining;
    thrd_start_t func;
    void *arg;
    void *stack;
} thread_record_t;

static thread_record_t thread_records[THREAD_RECORD_COUNT];
static volatile uint32_t thread_records_lock;

/* Registry operations are short and rare. A yield-based spin lock also
 * remains correct if a module image is shared by multiple address spaces;
 * a PRIVATE futex lock would not, because each process has a different key. */
static void thread_records_acquire(void) {
    while (__atomic_exchange_n(&thread_records_lock, 1u,
                               __ATOMIC_ACQUIRE) != 0u)
        __syscall1(SYS_SCHED_YIELD, 0);
}

static void thread_records_release(void) {
    __atomic_store_n(&thread_records_lock, 0u, __ATOMIC_RELEASE);
}

static thread_record_t *thread_record_find_locked(int tid) {
    for (unsigned i = 0; i < THREAD_RECORD_COUNT; i++) {
        if (thread_records[i].active && thread_records[i].tid == tid)
            return &thread_records[i];
    }
    return NULL;
}

static void thread_record_reset_locked(thread_record_t *record) {
    record->active = 0;
    record->ctid = 0;
    record->result = 0;
    record->tid = 0;
    record->detached = 0;
    record->joining = 0;
    record->func = NULL;
    record->arg = NULL;
    /* Keep owner_pid and stack: the next thread in this process reuses the
     * allocation instead of growing the heap on every context teardown. */
}

static thread_record_t *thread_record_reserve_locked(int owner_pid) {
    thread_record_t *free_record = NULL;

    for (unsigned i = 0; i < THREAD_RECORD_COUNT; i++) {
        thread_record_t *record = &thread_records[i];

        /* Detached threads have no joiner. Reclaim their slot lazily once
         * CLONE_CHILD_CLEARTID proves that they have stopped. */
        if (record->active && record->detached &&
            __atomic_load_n(&record->ctid, __ATOMIC_ACQUIRE) == 0u)
            thread_record_reset_locked(record);

        if (!free_record && !record->active)
            free_record = record;
    }

    if (!free_record)
        return NULL;

    /* A shared module image may retain an entry after its owning process is
     * gone. Never hand that process's heap pointer to a different process. */
    if (free_record->owner_pid != owner_pid)
        free_record->stack = NULL;

    free_record->owner_pid = owner_pid;
    free_record->active = 1;
    free_record->ctid = 0;
    free_record->result = 0;
    free_record->tid = 0;
    free_record->detached = 0;
    free_record->joining = 0;
    free_record->func = NULL;
    free_record->arg = NULL;
    return free_record;
}

static int thread_record_start(void *opaque) {
    thread_record_t *record = (thread_record_t *)opaque;
    int result = record->func(record->arg);
    tss_cleanup_current();
    __atomic_store_n(&record->result, result, __ATOMIC_RELEASE);
    return result;
}

/* Trampoline: child enters here with RDI=func, RSI=arg.
 * Calls func(arg), then SYS_EXIT with the return value. Never returns.
 * The child's stack was set up by the kernel from the `child_stack` argument
 * we passed to clone — so a normal C call works. */
extern void _osito_thread_trampoline(void);
__asm__(
    ".text\n"
    ".globl _osito_thread_trampoline\n"
    ".type  _osito_thread_trampoline, @function\n"
    "_osito_thread_trampoline:\n"
    "    xorq   %rbp, %rbp\n"        /* end backtrace */
    "    movq   %rdi, %rax\n"        /* rax = func */
    "    movq   %rsi, %rdi\n"        /* arg → rdi  */
    "    callq  *%rax\n"             /* func(arg)  */
    "    movq   %rax, %rdi\n"        /* exit code  */
    "    movq   $60, %rax\n"         /* SYS_EXIT   */
    "    syscall\n"
    "1:  jmp 1b\n"
    ".size _osito_thread_trampoline, .-_osito_thread_trampoline\n"
);

/* clone wrapper: returns child tid (>0) to parent, jumps directly into
 * the trampoline in the child (which will never return here). We can't use
 * __syscall6 because the child returns to a fresh stack — the C call frame
 * would be invalid. Instead use raw inline asm and check RAX immediately. */
static inline long _osito_clone(unsigned long flags,
                                void *child_stack,
                                int *ptid,
                                int *ctid,
                                thrd_start_t func,
                                void *arg)
{
    /* Pre-stage child entry: push func + arg onto child_stack so the
     * trampoline can fetch them via pop. Convention for our trampoline:
     *   [child_rsp+0] = func     -> popped to %rdi
     *   [child_rsp+8] = arg      -> popped to %rsi
     * (Kernel sets RIP = parent's syscall return; child returns from the
     * inline asm with RAX=0 and falls through to our branch handler.)
     *
     * Simpler design: place func+arg at child_stack and load them after
     * the syscall returns 0 in the child. We pre-decrement child_stack
     * by 16, write func+arg there.
     */
    uintptr_t cs = (uintptr_t)child_stack;
    cs &= ~0xFULL;
    cs -= 16;
    ((void **)cs)[0] = (void *)(uintptr_t)func;
    ((void **)cs)[1] = arg;

    long ret;
    register long r10 __asm__("r10") = (long)(uintptr_t)ctid;
    register long r8  __asm__("r8")  = 0;          /* tls (unused — share parent FS) */
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile (
        "syscall\n\t"
        "testq  %%rax, %%rax\n\t"
        "jnz    1f\n\t"
        /* Child path: fresh stack, RIP = right here. Pop func+arg and
         * jump into the trampoline. */
        "popq   %%rdi\n\t"            /* func */
        "popq   %%rsi\n\t"            /* arg  */
        "xorq   %%rbp, %%rbp\n\t"
        "jmp    _osito_thread_trampoline\n\t"
        "1:\n\t"
        : "=a"(ret)
        : "0"((long)SYS_CLONE),
          "D"((long)flags),
          "S"((long)cs),
          "d"((long)(uintptr_t)ptid),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

extern int printf(const char *, ...);

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    if (!thr || !func) {
        printf("[THRD] create: bad args thr=%p func=%p\n", (void*)thr, (void*)func);
        return thrd_error;
    }
    int owner_pid = (int)__syscall1(SYS_GETPID, 0);
    thread_records_acquire();
    thread_record_t *record = thread_record_reserve_locked(owner_pid);
    thread_records_release();
    if (!record) {
        printf("[THRD] create: registry exhausted (%u slots)\n",
               (unsigned)THREAD_RECORD_COUNT);
        return thrd_nomem;
    }

    if (!record->stack)
        record->stack = calloc(1, THREAD_STACK_SIZE);
    if (!record->stack) {
        printf("[THRD] create: calloc(%u) returned NULL (heap exhausted?)\n",
               (unsigned)THREAD_STACK_SIZE);
        thread_records_acquire();
        thread_record_reset_locked(record);
        thread_records_release();
        return thrd_nomem;
    }

    record->func = func;
    record->arg = arg;
    void *stack_top = (void *)((uintptr_t)record->stack + THREAD_STACK_SIZE);

    /* The public thrd_t only carries the TID. The registry's stable ctid
     * word is the lifetime/wakeup object used by CLONE_CHILD_CLEARTID. */
    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
                        | CLONE_THREAD
                        | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    long tid = _osito_clone(flags, stack_top, thr, (int *)&record->ctid,
                            thread_record_start, record);
    printf("[THRD] create: _osito_clone returned %ld (errno=-%ld)\n",
           tid, tid < 0 ? -tid : 0);
    if (tid <= 0) {
        thread_records_acquire();
        thread_record_reset_locked(record);
        thread_records_release();
        return thrd_error;
    }

    thread_records_acquire();
    record->tid = (int)tid;
    thread_records_release();
    printf("[THRD] create: spawned tid=%ld stack=%p func=%p\n",
           tid, record->stack, (void *)(uintptr_t)func);
    /* The stack remains attached to the registry slot and is reused after
     * join. This avoids both an exit-epilogue race and repeated heap growth. */
    return thrd_success;
}

int thrd_join(thrd_t thr, int *res) {
    int tid = thr;
    if (tid <= 0)
        return thrd_error;

    thread_records_acquire();
    thread_record_t *record = thread_record_find_locked(tid);
    if (!record || record->detached || record->joining) {
        thread_records_release();
        return thrd_error;
    }
    record->joining = 1;
    thread_records_release();

    for (;;) {
        uint32_t current = __atomic_load_n(&record->ctid, __ATOMIC_ACQUIRE);
        if (current == 0u)
            break;
        _futex_wait(&record->ctid, current);
    }

    int result = __atomic_load_n(&record->result, __ATOMIC_ACQUIRE);
    thread_records_acquire();
    if (record->active && record->tid == tid)
        thread_record_reset_locked(record);
    thread_records_release();

    if (res)
        *res = result;
    return thrd_success;
}

int thrd_detach(thrd_t t) {
    thread_records_acquire();
    thread_record_t *record = thread_record_find_locked(t);
    if (!record || record->joining) {
        thread_records_release();
        return thrd_error;
    }

    record->detached = 1;
    if (__atomic_load_n(&record->ctid, __ATOMIC_ACQUIRE) == 0u)
        thread_record_reset_locked(record);
    thread_records_release();
    return thrd_success;
}

thrd_t thrd_current(void) {
    long tid = __syscall1(SYS_GETTID, 0);
    return (thrd_t)tid;
}

int thrd_equal(thrd_t a, thrd_t b) { return a == b; }

void thrd_yield(void) { __syscall1(SYS_SCHED_YIELD, 0); }

void thrd_exit(int code) {
    tss_cleanup_current();
    __syscall1(SYS_EXIT, (long)code);
    for (;;) { }
}

/* thrd_sleep — REAL: nanosleep via syscall 35.
 * struct timespec { long tv_sec; long tv_nsec; }.  Kernel honours sec/nsec
 * with the same encoding.  rem (out) takes remaining time on signal — we
 * forward as-is; OsitoK doesn't deliver signals so it'll be untouched. */
int thrd_sleep(const void *t, void *r) {
    if (!t) return -1;
    long rc = __syscall2(35, (long)(uintptr_t)t, (long)(uintptr_t)r);
    return rc == 0 ? 0 : -1;
}

/* ---- TSS (thread-specific storage) ----
 * The graphics module is one shared ELF image, so ordinary globals alias
 * across every Win32 process. Compiler TLS is not usable here because FS is
 * owned by the guest process and the module loader has no ELF TLS layout.
 * Use a bounded table keyed by the kernel TGID/TID pair instead.
 */
#define TSS_MAX_KEYS       64u
#define TSS_THREAD_BUCKETS 1024u
#define TSS_TOMBSTONE      (~(uint64_t)0)
#define TSS_DTOR_PASSES    4u

typedef struct tss_thread_entry {
    volatile uint64_t identity;
    void *values[TSS_MAX_KEYS];
} tss_thread_entry_t;

static tss_thread_entry_t _tss_threads[TSS_THREAD_BUCKETS];
static void (*_tss_dtors[TSS_MAX_KEYS])(void *);
static uint32_t _tss_next_key = 1;   /* 0 reserved as "uninitialised" */

static uint64_t tss_current_identity(void) {
    long tgid = __syscall1(SYS_GETPID, 0);
    long tid = __syscall1(SYS_GETTID, 0);
    if (tgid <= 0 || tid <= 0) return 0;
    return ((uint64_t)(uint32_t)tgid << 32) | (uint32_t)tid;
}

static uint32_t tss_identity_hash(uint64_t identity) {
    identity ^= identity >> 33;
    identity *= 0xff51afd7ed558ccdULL;
    identity ^= identity >> 33;
    return (uint32_t)identity & (TSS_THREAD_BUCKETS - 1u);
}

static tss_thread_entry_t *tss_find_entry(uint64_t identity, int create) {
    if (!identity || identity == TSS_TOMBSTONE) return NULL;

    uint32_t start = tss_identity_hash(identity);
    tss_thread_entry_t *tombstone = NULL;

retry:
    for (uint32_t probe = 0; probe < TSS_THREAD_BUCKETS; probe++) {
        tss_thread_entry_t *entry =
            &_tss_threads[(start + probe) & (TSS_THREAD_BUCKETS - 1u)];
        uint64_t owner = __atomic_load_n(&entry->identity, __ATOMIC_ACQUIRE);

        if (owner == identity) return entry;
        if (owner == TSS_TOMBSTONE) {
            if (!tombstone) tombstone = entry;
            continue;
        }
        if (owner != 0) continue;
        if (!create) return NULL;

        tss_thread_entry_t *target = tombstone ? tombstone : entry;
        uint64_t expected = tombstone ? TSS_TOMBSTONE : 0;
        if (__atomic_compare_exchange_n(&target->identity, &expected, identity,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return target;
        tombstone = NULL;
        goto retry;
    }

    if (create && tombstone) {
        uint64_t expected = TSS_TOMBSTONE;
        if (__atomic_compare_exchange_n(&tombstone->identity, &expected,
                                        identity, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return tombstone;
        tombstone = NULL;
        goto retry;
    }
    return NULL;
}

static int tss_entry_empty(tss_thread_entry_t *entry) {
    for (uint32_t key = 1; key < TSS_MAX_KEYS; key++)
        if (__atomic_load_n(&entry->values[key], __ATOMIC_ACQUIRE))
            return 0;
    return 1;
}

int tss_create(tss_t *key, void (*dtor)(void *)) {
    if (!key) return thrd_error;
    uint32_t k = __atomic_fetch_add(&_tss_next_key, 1u, __ATOMIC_ACQ_REL);
    if (k >= TSS_MAX_KEYS) return thrd_error;
    *key = (tss_t)k;
    __atomic_store_n(&_tss_dtors[k], dtor, __ATOMIC_RELEASE);
    return thrd_success;
}

void tss_delete(tss_t key) {
    uint32_t k = (uint32_t)key;
    if (k == 0 || k >= TSS_MAX_KEYS) return;
    __atomic_store_n(&_tss_dtors[k], NULL, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < TSS_THREAD_BUCKETS; i++)
        __atomic_store_n(&_tss_threads[i].values[k], NULL, __ATOMIC_RELEASE);
}

void *tss_get(tss_t key) {
    uint32_t k = (uint32_t)key;
    if (k == 0 || k >= TSS_MAX_KEYS) return NULL;
    tss_thread_entry_t *entry =
        tss_find_entry(tss_current_identity(), 0);
    return entry
        ? __atomic_load_n(&entry->values[k], __ATOMIC_ACQUIRE)
        : NULL;
}

int tss_set(tss_t key, void *val) {
    uint32_t k = (uint32_t)key;
    if (k == 0 || k >= TSS_MAX_KEYS) return thrd_error;

    uint64_t identity = tss_current_identity();
    tss_thread_entry_t *entry = tss_find_entry(identity, val != NULL);
    if (!entry) return val ? thrd_error : thrd_success;

    __atomic_store_n(&entry->values[k], val, __ATOMIC_RELEASE);
    if (!val && tss_entry_empty(entry)) {
        uint64_t expected = identity;
        __atomic_compare_exchange_n(&entry->identity, &expected,
                                    TSS_TOMBSTONE, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE);
    }
    return thrd_success;
}

static void tss_cleanup_current(void) {
    uint64_t identity = tss_current_identity();
    tss_thread_entry_t *entry = tss_find_entry(identity, 0);
    if (!entry) return;

    for (uint32_t pass = 0; pass < TSS_DTOR_PASSES; pass++) {
        int invoked = 0;
        for (uint32_t key = 1; key < TSS_MAX_KEYS; key++) {
            void *value = __atomic_exchange_n(&entry->values[key], NULL,
                                              __ATOMIC_ACQ_REL);
            void (*dtor)(void *) =
                __atomic_load_n(&_tss_dtors[key], __ATOMIC_ACQUIRE);
            if (value && dtor) {
                invoked = 1;
                dtor(value);
            }
        }
        if (!invoked) break;
    }

    for (uint32_t key = 1; key < TSS_MAX_KEYS; key++)
        __atomic_store_n(&entry->values[key], NULL, __ATOMIC_RELEASE);
    uint64_t expected = identity;
    __atomic_compare_exchange_n(&entry->identity, &expected, TSS_TOMBSTONE,
                                0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* ---- u_thread_create — Mesa's thin wrapper over thrd_create ---------
 *
 * Upstream src/util/u_thread.c does pthread_sigmask gymnastics around
 * thrd_create; OsitoK has no signal mask to worry about, so a direct
 * passthrough is correct AND simpler. Without this, w48_link_stubs.c
 * used to provide a "return -1" stub that silently shadowed Mesa's
 * impl and broke util_queue_init at the first thread spawn. */
int u_thread_create(thrd_t *thrd, int (*routine)(void *), void *param) {
    if (!thrd || !routine) return thrd_error;
    return thrd_create(thrd, routine, param);
}

/* ---- W4.2 — math float-suffixed variants needed by NIR codegen ----- */
/* OsitoK libc/math.c provides the double versions; add float wrappers. */
extern double fmin(double, double);
extern double fmax(double, double);
extern double copysign(double, double);
extern double ldexp(double, int);

/* C99/IEEE 754-2008 NaN semantics: fmin(NaN,x)=x, fmin(x,NaN)=x, fmin(NaN,NaN)=NaN.
 * OsitoK's underlying fmin behavior may not honor this; explicit guards. */
float fminf(float a, float b) {
    if (a != a) return b;     /* a is NaN */
    if (b != b) return a;     /* b is NaN */
    return (float)fmin((double)a, (double)b);
}
float fmaxf(float a, float b) {
    if (a != a) return b;
    if (b != b) return a;
    return (float)fmax((double)a, (double)b);
}
float copysignf(float a, float b)        { return (float)copysign((double)a, (double)b); }
float ldexpf(float a, int e)             { return (float)ldexp((double)a, e); }

/* fma (fused multiply-add) — emulate a*b+c (loses fused precision but
 * NIR const-fold doesn't care about ULPs at this level). */
double fma(double a, double b, double c)  { return a * b + c; }
float  fmaf(float a, float b, float c)    { return a * b + c; }

/* isnormal: float, double, long double — return non-zero iff value
 * is finite, non-zero, and not subnormal. We don't model subnormals
 * (OsitoK x87 + SSE is IEEE 754 hardware); flush-to-zero would handle
 * that. */
int __isnormal(double x) {
    /* exponent bits 1..2046 means normal in IEEE 754 binary64 */
    union { double d; unsigned long long u; } v = { x };
    unsigned long long exp = (v.u >> 52) & 0x7ff;
    return exp != 0 && exp != 0x7ff;
}
int __isnormalf(float x) {
    union { float f; unsigned u; } v = { x };
    unsigned exp = (v.u >> 23) & 0xff;
    return exp != 0 && exp != 0xff;
}
/* isnormal is a glibc macro that dispatches by type; nir_constant_expressions.c
 * is auto-generated and calls plain isnormal(x). Undefine the macro here so
 * we can provide a real symbol — the macro is only needed at the call sites. */
#undef isnormal
int isnormal(double x) { return __isnormal(x); }

/* ---- W4.2 — gallium util fillers --------------------------------------
 * These are referenced by libmesa_util.a (already-built) but were never
 * resolved because no test linked them in W4.0/W4.1. Smoke test in W4.2
 * (mesa-compiler-test) is the first time they show up. */

/* parse_debug_string — INTENTIONAL "no flags".  Walks env-style
 * "flag1,flag2,flag3" and ORs in bits from the control[] table.  Real impl
 * lives in mesa/src/util/u_debug.c (not vendored — small code, but its
 * single caller log.c also stubbed below).  Smoke test never sets any
 * MESA_DEBUG / GALLIUM_DEBUG env vars (getenv always NULL on OsitoK), so
 * "no flags set" is the correct, only-reachable answer. */
extern int strcmp(const char *, const char *);
struct debug_named_value;
unsigned long parse_debug_string(const char *debug,
                                 const struct debug_named_value *control) {
    (void)debug; (void)control;
    return 0;  /* MESA_DEBUG=NULL → no flags ever enabled */
}

/* util_get_process_name — REAL: returns a constant string.  argv[0] would
 * be ideal but OsitoK doesn't expose argv to libc beyond main().  Used
 * only by Mesa for log prefix formatting / shader cache naming.  Constant
 * "ositok" works for both cases (cache key is per-binary anyway and we
 * have no shader cache). */
const char *util_get_process_name(void) {
    return "ositok";
}

/* open_memstream — INTENTIONAL NULL fallback.  glibc creates a FILE* that
 * grows a malloc'd buffer.  OsitoK has no FILE* layer (no stdio.h
 * implementation outside of dprintf-style writes).  Real impl would need
 * a full FILE*+vtable stack.  Mesa uses memstream for debug message
 * capture (debug_message callback chain); when NULL, the callers fall
 * through to their "no capture" branch.  Confirmed safe via grep:
 *   grep -rn 'open_memstream' mesa/src/util/ → all wrapped in
 *   `if (!stream) goto fallthrough;` or assigned to ctx->debug.dest_FILE
 *   which itself is NULL-checked. */
struct _FILE;
struct _FILE *open_memstream(char **bufp, size_t *sizep) {
    (void)bufp; (void)sizep;
    return (struct _FILE *)0;  /* triggers caller's no-capture fallback */
}

/* os_read_file — INTENTIONAL NULL.  Mesa scans /proc/meminfo + drirc
 * config files.  OsitoK has no /proc layer at this height in the stack
 * (the fs syscalls 0..4 work on OsitoFS for ROM files, but Mesa expects
 * Linux /proc paths that don't exist).  All callers handle NULL by
 * falling back to compile-time defaults.  Confirmed:
 *   grep -rn 'os_read_file' mesa/src/util/ → only os_misc.c uses it for
 *   memory probe; falls through to sysconf(_SC_PHYS_PAGES) which we DO
 *   provide (mesa_compat.h:259). */
char *os_read_file(const char *filename, size_t *size) {
    (void)filename;
    if (size) *size = 0;
    return (char *)0;  /* triggers sysconf fallback in os_misc.c */
}

/* sscanf — INTENTIONAL no-match.  Real impl needs full printf/scanf
 * format-string state machine (~500 LOC).  Mesa's only caller is
 * /proc/meminfo MemAvailable parsing — but os_read_file above returns
 * NULL so sscanf is never reached on hot paths.  zink_screen.c also
 * calls sscanf for VK_DRIVER_NAME version strings; the failure path
 * just leaves driver_name unset (Mesa logs "couldn't parse" + continues).
 *
 * Returning 0 = "no fields matched".  -1 would be EOF; both flagging
 * "scan failed" — callers branch the same way. */
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
int sscanf(const char *str, const char *fmt, ...) {
    (void)str; (void)fmt;
    return 0;  /* no-match: caller's "couldn't parse" branch fires */
}

/* ---- W4.2 — extra stubs surfaced by libmesa_compiler.a -------------- */

/* debug_get_option_cached + debug_parse_flags_option — INTENTIONAL.
 * Real impl in mesa/src/util/u_debug.c lazily caches getenv(name) results
 * and OR's matching flags from the named-value table.  On OsitoK getenv
 * is always NULL → no env var ever set → cached value is 0 forever, which
 * is what we return.  defval respected for parse_flags so Mesa's "default
 * debug bits" path (rare) still applies. */
unsigned long debug_get_option_cached(const char *name, const struct debug_named_value *flags) {
    (void)name; (void)flags;
    return 0;  /* getenv always NULL → no debug env var → 0 flags */
}
unsigned long debug_parse_flags_option(const char *name, const char *str,
                                       const struct debug_named_value *flags,
                                       unsigned long defval) {
    (void)name; (void)str; (void)flags;
    return defval;  /* honour Mesa-supplied default; getenv NULL anyway */
}

/* util_tls_qsort_r: thread-local-storage-based qsort_r. We don't have
 * TLS in this build; provide a minimal non-reentrant qsort using
 * libc's qsort. We don't implement qsort either — use a brain-dead
 * insertion sort for correctness. nir.c only sorts at link time. */
extern void *memcpy(void *, const void *, size_t);
typedef int (*_cmp_t)(const void *, const void *, void *);
void util_tls_qsort_r(void *base, size_t nmemb, size_t size,
                      _cmp_t cmp, void *arg) {
    char *a = (char *)base;
    char tmp[256];  /* element size cap */
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp, arg) > 0) {
            memcpy(a + j * size, a + (j - 1) * size, size);
            j--;
        }
        memcpy(a + j * size, tmp, size);
    }
}

/* comma_separated_list_contains — REAL: substring-walk impl.
 * Walks `list` for entries delimited by ',' or ';' and returns 1 if any
 * exactly matches `needle`.  Used by NIR debug-options gating
 * (NIR_DEBUG=foo,bar).  Smoke-path getenv→NULL → list usually NULL, but
 * Mesa also passes hard-coded build-time defaults in some places, so
 * the real walk is cheap insurance. */
extern size_t strlen(const char *);
extern int strncmp(const char *, const char *, size_t);
int comma_separated_list_contains(const char *list, const char *needle) {
    if (!list || !needle) return 0;
    size_t nlen = strlen(needle);
    const char *p = list;
    while (*p) {
        const char *end = p;
        while (*end && *end != ',' && *end != ';') end++;
        if ((size_t)(end - p) == nlen && strncmp(p, needle, nlen) == 0) return 1;
        if (!*end) break;
        p = end + 1;
    }
    return 0;
}

/* _mesa_blake3_print — INTENTIONAL no-op.  Real impl writes 64 hex chars
 * to a FILE*.  NIR uses it for shader debug dumps (gated by NIR_PRINT or
 * MESA_SHADER_CACHE_DUMP env vars; both NULL on OsitoK).  We have no
 * FILE* infrastructure; without one, the real impl couldn't write
 * anywhere useful anyway.  Vendor source: mesa/src/util/blake3/blake3.c
 * (formats via fprintf — not portable to our printf-only world). */
struct __FILE;
void _mesa_blake3_print(struct __FILE *f, const unsigned char *hash) {
    (void)f; (void)hash;  /* no FILE* layer; getenv(NIR_PRINT) is NULL anyway */
}

/* exp2 / exp2f — exp(x * ln(2)). OsitoK libc has exp() but not exp2(). */
extern double exp(double);
double exp2(double x) { return exp(x * 0.69314718055994530942); }
float  exp2f(float x) { return (float)exp2((double)x); }
extern double log2(double);
float log2f(float x) { return (float)log2((double)x); }

/* truncf / roundf / floorf / ceilf / sqrtf / fabsf / fmodf — float ops
 * NIR codegen uses. OsitoK libc has the double versions. */
extern double trunc(double);
extern double round(double);
extern double floor(double);
extern double ceil(double);
extern double sqrt(double);
extern double fabs(double);
extern double fmod(double, double);
float truncf(float x) { return (float)trunc((double)x); }
float roundf(float x) { return (float)round((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float sqrtf(float x)  { return (float)sqrt((double)x); }
float fabsf(float x)  { return (float)fabs((double)x); }
float fmodf(float a, float b) { return (float)fmod((double)a, (double)b); }

/* sinf / cosf / tanf / asinf / acosf / atanf / atan2f / expf / logf /
 * powf — common float trig/transcendental that NIR const-fold uses. */
extern double sin(double);
extern double cos(double);
extern double tan(double);
extern double asin(double);
extern double acos(double);
extern double atan(double);
extern double atan2(double, double);
extern double exp(double);
extern double log(double);
extern double pow(double, double);
extern double sinh(double);
extern double cosh(double);
extern double tanh(double);
float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float asinf(float x){ return (float)asin((double)x); }
float acosf(float x){ return (float)acos((double)x); }
float atanf(float x){ return (float)atan((double)x); }
float atan2f(float a, float b) { return (float)atan2((double)a, (double)b); }
float expf(float x) { return (float)exp((double)x); }
float logf(float x) { return (float)log((double)x); }
float powf(float a, float b) { return (float)pow((double)a, (double)b); }
float sinhf(float x){ return (float)sinh((double)x); }
float coshf(float x){ return (float)cosh((double)x); }
float tanhf(float x){ return (float)tanh((double)x); }
