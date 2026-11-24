#include "sched_signal.h"
#ifdef __KERNEL_X86__
#include "smp_work.h"
#else
extern void smp_wake_bsp(void);
#endif

void sched_signal_set(sched_signal_t *s, uint32_t bits)
{
    if (!s || !bits) return;
    if (!__atomic_fetch_or(&s->bits,bits,__ATOMIC_ACQ_REL)) smp_wake_bsp();
}

uint32_t sched_signal_take(sched_signal_t *s)
{
    return s ? __atomic_exchange_n(&s->bits,0,__ATOMIC_ACQ_REL) : 0;
}

#ifdef __KERNEL_X86__
#include "smp.h"
extern uint64_t paging_get_kernel_cr3(void);
extern uint64_t idt_get_monotonic_ns(void);
extern int sched_spawn_in_address_space_blocked(const char *, void (*)(void), uint64_t, int);
extern int proc_wake_pid(int);
extern int proc_kill_pid(int);
extern int proc_waitpid(uint32_t, int32_t *);
extern void sched_yield(void);
extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);
static unsigned signal_test_busy;
static struct {
    sched_signal_t input, done;
    unsigned ready, ack, cpu, rounds, checks, failures, kill_mode;
} signal_test;
#define SIGNAL_CHECK(x) do { \
    __atomic_add_fetch(&signal_test.checks,1,__ATOMIC_RELAXED); \
    if (!(x)) __atomic_add_fetch(&signal_test.failures,1,__ATOMIC_RELAXED); \
} while (0)

static void sched_signal_test_consumer(void)
{
    if (signal_test.kill_mode) {
        for (;;) {
            enum sched_signal_result r=sched_signal_wait(&signal_test.input,0);
            if (r!=SCHED_SIGNAL_RETRY) break;
            sched_yield();
        }
        SIGNAL_CHECK(false); /* the owner must kill this indefinite waiter */
        sched_signal_set(&signal_test.done,1);
        return;
    }
    SIGNAL_CHECK(sched_signal_wait(&signal_test.input,100)==SCHED_SIGNAL_READY);
    SIGNAL_CHECK(sched_signal_take(&signal_test.input)==3);
    __asm__ volatile("cli" ::: "memory");
    enum sched_signal_result invalid=sched_signal_wait(&signal_test.input,1);
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; sti" : "=r"(flags) :: "memory");
    SIGNAL_CHECK(invalid==SCHED_SIGNAL_INVALID && !(flags&(1ULL<<9)));
    for (unsigned i=1;i<=16;i++) {
        __atomic_store_n(&signal_test.ready,i,__ATOMIC_RELEASE);
        enum sched_signal_result r=sched_signal_wait(&signal_test.input,100);
        SIGNAL_CHECK(r==SCHED_SIGNAL_READY);
        if (r!=SCHED_SIGNAL_READY) break;
        SIGNAL_CHECK(sched_signal_take(&signal_test.input)==4);
        SIGNAL_CHECK(!__atomic_load_n(&signal_test.input.waiters,__ATOMIC_ACQUIRE));
        signal_test.rounds++;
        __atomic_store_n(&signal_test.ack,i,__ATOMIC_RELEASE);
    }
    SIGNAL_CHECK(sched_signal_wait(&signal_test.input,1)==SCHED_SIGNAL_TIMEOUT);
    SIGNAL_CHECK(!signal_test.input.waiters && !sched_signal_take(&signal_test.input));
    sched_signal_set(&signal_test.done,1);
}

static void sched_signal_test_producer(void *unused, void *result)
{
    (void)unused; (void)result;
    signal_test.cpu=smp_current_cpu();
    SIGNAL_CHECK(signal_test.cpu>0);
    SIGNAL_CHECK(sched_signal_wait(&signal_test.input,1)==SCHED_SIGNAL_INVALID);
    uint64_t until=idt_get_monotonic_ns()+5000000000ULL;
    for (unsigned i=1;i<=16;i++) {
        while (__atomic_load_n(&signal_test.ready,__ATOMIC_ACQUIRE)!=i ||
               !__atomic_load_n(&signal_test.input.waiters,__ATOMIC_ACQUIRE)) {
            if (idt_get_monotonic_ns()>=until) {
                SIGNAL_CHECK(false); sched_signal_set(&signal_test.done,2); return;
            }
            __asm__ volatile("pause");
        }
        sched_signal_set(&signal_test.input,4);
        while (__atomic_load_n(&signal_test.ack,__ATOMIC_ACQUIRE)!=i) {
            if (idt_get_monotonic_ns()>=until) {
                SIGNAL_CHECK(false); sched_signal_set(&signal_test.done,2); return;
            }
            __asm__ volatile("pause");
        }
    }
}

int sched_signal_selftest(void)
{
    if (smp_current_cpu()!=0 || ws_cpu_count<2 ||
        __atomic_exchange_n(&signal_test_busy,1,__ATOMIC_ACQ_REL)) return 1;
    memset(&signal_test,0,sizeof(signal_test));
    int task=-1;
    int pid=sched_spawn_in_address_space_blocked("gpu-signal",sched_signal_test_consumer,
        paging_get_kernel_cr3(),0);
    SIGNAL_CHECK(pid>0);
    if (pid>0) {
        sched_signal_set(&signal_test.input,1);
        sched_signal_set(&signal_test.input,2);
        task=smp_submit_any(sched_signal_test_producer,NULL,NULL);
        SIGNAL_CHECK(task>=0);
        SIGNAL_CHECK(proc_wake_pid(pid)==0);
        uint64_t until=idt_get_monotonic_ns()+6000000000ULL;
        while (!__atomic_load_n(&signal_test.done.bits,__ATOMIC_ACQUIRE) &&
               idt_get_monotonic_ns()<until) {
            sched_yield();
            __asm__ volatile("cli" ::: "memory");
            if (!__atomic_load_n(&signal_test.done.bits,__ATOMIC_ACQUIRE))
                __asm__ volatile("sti; hlt" ::: "memory");
            else __asm__ volatile("sti" ::: "memory");
        }
        if (task>=0) smp_wait(task); /* retain static test state until AP is done */
        SIGNAL_CHECK(signal_test.rounds==16 && signal_test.cpu>0);
        /* The child signalled before returning through its exit trampoline. */
        bool reaped=false;
        for (unsigned i=0;i<1000 && !reaped;i++) {
            sched_yield(); reaped=proc_waitpid((uint32_t)pid,NULL)==0;
        }
        if (!reaped) { proc_kill_pid(pid); reaped=proc_waitpid((uint32_t)pid,NULL)==0; }
        SIGNAL_CHECK(reaped && !signal_test.input.waiters);
        if (!reaped) return 1; /* keep storage quarantined; no repeat */

        /* A killed blocked task must detach before its stack/slot is freed. */
        sched_signal_take(&signal_test.input); sched_signal_take(&signal_test.done);
        signal_test.kill_mode=1;
        pid=sched_spawn_in_address_space_blocked("gpu-signal-exit",sched_signal_test_consumer,
            paging_get_kernel_cr3(),0);
        SIGNAL_CHECK(pid>0);
        if (pid>0) {
            SIGNAL_CHECK(proc_wake_pid(pid)==0);
            until=idt_get_monotonic_ns()+1000000000ULL;
            while (!__atomic_load_n(&signal_test.input.waiters,__ATOMIC_ACQUIRE) &&
                   idt_get_monotonic_ns()<until) sched_yield();
            SIGNAL_CHECK(signal_test.input.waiters==1);
            SIGNAL_CHECK(proc_kill_pid(pid)==0 && !signal_test.input.waiters);
            reaped=proc_waitpid((uint32_t)pid,NULL)==0;
            SIGNAL_CHECK(reaped);
            if (!reaped) return 1;
            sched_signal_set(&signal_test.input,8); /* no stale waiter */
            SIGNAL_CHECK(sched_signal_take(&signal_test.input)==8);
        }
    }
    serial_puts("[GPUSIGNAL] checks="); serial_putdec(signal_test.checks);
    serial_puts(" failures="); serial_putdec(signal_test.failures);
    serial_puts(" rounds="); serial_putdec(signal_test.rounds);
    serial_puts(" ap_cpu="); serial_putdec(signal_test.cpu);
    serial_puts(" waiters="); serial_putdec(signal_test.input.waiters); serial_puts("\n");
    int failures=signal_test.failures;
    __atomic_store_n(&signal_test_busy,0,__ATOMIC_RELEASE);
    return failures;
}
#endif
