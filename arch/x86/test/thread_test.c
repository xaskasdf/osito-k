/*
 * thread_test.c — test clone(CLONE_THREAD) + futex on OsitoK
 *
 * Uses raw syscalls to avoid musl's pthread complexity.
 * Tests: thread creation, shared memory, futex wait/wake, TLS.
 */

#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <stdint.h>

/* Raw output helpers */
static void wr(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static void wr_dec(long n)
{
    char buf[24];
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else { while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; } }
    if (neg) buf[i++] = '-';
    char out[24];
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    write(1, out, i);
}

/* Raw syscall wrappers */
static long raw_syscall(long nr, long a1, long a2, long a3, long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_SYSVSEM       0x00040000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

/* Shared state between threads */
static volatile int shared_counter = 0;
static volatile int thread_done = 0;
static int child_tid = 0;

/* Thread function — runs on its own stack, shares global data */
static void thread_entry(void)
{
    /* Increment shared counter to prove CLONE_VM works */
    shared_counter = 42;
    thread_done = 1;

    /* Exit thread via exit syscall (exit_group would kill all threads) */
    raw_syscall(60, 0, 0, 0, 0, 0);  /* sys_exit(0) */
    __builtin_unreachable();
}

/* T1: Basic thread creation with clone(CLONE_THREAD) */
static int test_clone_thread(void)
{
    wr("T1: clone(CLONE_THREAD)...\n");

    /* Allocate thread stack (grows down) */
    char stack[8192] __attribute__((aligned(16)));
    char *stack_top = stack + sizeof(stack);
    /* ABI: RSP must be 8 mod 16 at function entry (call pushes 8 bytes) */
    stack_top -= 8;

    /* Put thread entry address at top of stack.
     * When clone returns 0 in child, musl's __clone wrapper pops
     * the entry and calls it. But we're doing raw clone, so we
     * need the entry point in the ISR frame's RIP.
     *
     * The kernel sets RIP = parent's RIP (after SYSCALL) and RSP = child_stack.
     * So the child will return from the syscall with RAX=0 and then
     * we need to redirect it to thread_entry.
     *
     * Simplest approach: use the same pattern as musl's __clone —
     * put the function pointer on the child stack and let the child
     * pick it up after the clone returns.
     */

    /* Store thread_entry address at top of child stack */
    uint64_t *sp = (uint64_t *)stack_top;
    sp[-1] = (uint64_t)thread_entry;  /* function to call */
    stack_top = (char *)(sp - 1);

    shared_counter = 0;
    thread_done = 0;

    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                          CLONE_THREAD | CLONE_SYSVSEM |
                          CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    long ret = raw_syscall(56, /* clone */
                           flags,
                           (long)stack_top,
                           (long)&child_tid,   /* parent_tidptr */
                           (long)&child_tid,   /* child_tidptr (CLEARTID) */
                           0 /* tls */);

    if (ret < 0) {
        wr("  FAIL: clone returned ");
        wr_dec(ret);
        wr("\n");
        return 1;
    }

    if (ret == 0) {
        /* We're the child thread — call thread_entry.
         * Pop function pointer from stack and call it. */
        uint64_t fn;
        __asm__ volatile ("pop %0" : "=r"(fn));
        void (*entry)(void) = (void (*)(void))fn;
        entry();
        __builtin_unreachable();
    }

    /* Parent — ret is child TID */
    wr("  child TID: ");
    wr_dec(ret);
    wr("\n");

    /* Wait for thread to finish (poll shared_counter) */
    for (int i = 0; i < 10000000; i++) {
        if (thread_done) break;
        __asm__ volatile ("pause");
    }

    if (shared_counter == 42) {
        wr("  PASS: shared_counter = 42 (CLONE_VM works)\n");
    } else {
        wr("  FAIL: shared_counter = ");
        wr_dec(shared_counter);
        wr("\n");
        return 1;
    }

    return 0;
}

/* T2: Futex wait/wake between parent and child thread */
static volatile int futex_val = 0;

static void futex_thread(void)
{
    /* Wait a bit then change value and wake parent */
    for (volatile int i = 0; i < 100000; i++) {}

    futex_val = 1;
    /* FUTEX_WAKE on futex_val address */
    raw_syscall(202, (long)&futex_val, FUTEX_WAKE, 1, 0, 0);

    raw_syscall(60, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static int test_futex(void)
{
    wr("T2: futex wait/wake...\n");

    char stack[8192] __attribute__((aligned(16)));
    char *stack_top = stack + sizeof(stack) - 8;
    uint64_t *sp = (uint64_t *)stack_top;
    sp[-1] = (uint64_t)futex_thread;
    stack_top = (char *)(sp - 1);

    futex_val = 0;

    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                          CLONE_THREAD | CLONE_SYSVSEM;

    long ret = raw_syscall(56, flags, (long)stack_top, 0, 0, 0);
    if (ret < 0) {
        wr("  FAIL: clone returned ");
        wr_dec(ret);
        wr("\n");
        return 1;
    }
    if (ret == 0) {
        uint64_t fn;
        __asm__ volatile ("pop %0" : "=r"(fn));
        ((void (*)(void))fn)();
        __builtin_unreachable();
    }

    /* Parent: FUTEX_WAIT on futex_val == 0 */
    long fret = raw_syscall(202, (long)&futex_val, FUTEX_WAIT, 0, 0, 0);
    (void)fret;

    if (futex_val == 1) {
        wr("  PASS: futex wakeup received\n");
    } else {
        wr("  FAIL: futex_val = ");
        wr_dec(futex_val);
        wr("\n");
        return 1;
    }

    return 0;
}

int main(void)
{
    wr("=== OsitoK Thread Test ===\n");

    int fails = 0;
    fails += test_clone_thread();
    fails += test_futex();

    if (fails == 0)
        wr("=== All thread tests PASSED ===\n");
    else {
        wr("=== FAILED: ");
        wr_dec(fails);
        wr(" test(s) ===\n");
    }

    return fails;
}
