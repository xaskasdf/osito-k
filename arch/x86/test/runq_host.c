#define PROCESS_RUNQ_HOST_TEST
/* Do not interpose the kernel context-switch routine on host libc. */
#define sched_yield runq_unused_kernel_yield
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wattributes"
#include "../kernel/process.c"
#pragma GCC diagnostic pop
#undef sched_yield

extern int printf(const char *, ...);
extern void exit(int);
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned checks, irq_depth, switches;
static bool interrupts_enabled = true;
static int pending_switch = -1;
static process_t processes[12];
static uint64_t sleep_deadlines[12];
static sched_signal_t *signal_waits[12];
static unsigned signal_ipis;

void smp_wake_bsp(void) { signal_ipis++; }

uint64_t idt_get_ticks(void) { return 100; }

uint64_t runq_test_irq_save(void)
{
    uint64_t flags = interrupts_enabled ? 0x200 : 0;
    interrupts_enabled = false;
    irq_depth++;
    return flags;
}

void runq_test_irq_restore(uint64_t flags)
{
    CHECK(irq_depth > 0);
    irq_depth--;
    interrupts_enabled = (flags & 0x200) != 0;
    if (interrupts_enabled && pending_switch >= 0) {
        int idx = pending_switch;
        pending_switch = -1;
        CHECK(proctab[idx].state == PROC_READY);
        /* The newly woken task can run and block again before the original
         * waker resumes. Use the real transitions to model that timer tick. */
        proc_transition(&proctab[idx], PROC_RUNNING);
        proc_transition(&proctab[idx], PROC_BLOCKED);
        switches++;
    }
}

static void check_queues(void)
{
    bool seen[12] = {0};
    unsigned count = 0;
    for (unsigned q = 0; q < QOS_NUM_CLASSES; q++) {
        int tail = -1;
        for (int idx = runq_head[q]; idx >= 0; idx = proctab[idx].runq_next) {
            CHECK(idx < proc_capacity && !seen[idx]);
            seen[idx] = true;
            CHECK(proctab[idx].state == PROC_READY && proctab[idx].qos_class == q);
            tail = idx; count++;
        }
        CHECK(runq_tail[q] == tail);
    }
    CHECK(count == ready_count);
    for (int i = 0; i < proc_capacity; i++) {
        CHECK(seen[i] == (proctab[i].state == PROC_READY));
        if (!seen[i]) CHECK(proctab[i].runq_next == -1);
    }
}

static void setup(unsigned qos)
{
    CHECK(!irq_depth && pending_switch == -1);
    interrupts_enabled = true;
    memset(processes, 0, sizeof(processes));
    memset(sleep_deadlines, 0, sizeof(sleep_deadlines));
    memset(signal_waits,0,sizeof(signal_waits));
    sched_signals=signal_waits;
    sched_signal_count=0;
    signal_ipis=0;
    proctab = processes;
    proc_capacity = sizeof(processes) / sizeof(processes[0]);
    sched_sleep_deadline = sleep_deadlines;
    sched_sleeping_count = 0;
    sched_current_idx = 0;
    current_proc = &processes[0];
    runq_init();
    for (int i = 0; i < proc_capacity; i++) {
        processes[i].qos_class = qos;
        processes[i].runq_next = -1;
        processes[i].pid = i + 1;
        processes[i].quantum = 5;
    }
    proc_transition(&processes[0], PROC_RUNNING);
    proc_transition(&processes[1], PROC_READY);
    proc_transition(&processes[2], PROC_READY);
    proc_transition(&processes[3], PROC_BLOCKED);
    check_queues();
}

static void test_normal(void)
{
    for (unsigned q = 0; q < QOS_NUM_CLASSES; q++) {
        setup(q);
        sched_sleep_deadline[3] = 200;
        sched_sleeping_count = 1;
        sched_unblock(3);
        CHECK(processes[3].state == PROC_READY && runq_head[q] == 3);
        CHECK(processes[3].runq_next == 1 && processes[1].runq_next == 2);
        CHECK(runq_tail[q] == 2 && ready_count == 3);
        CHECK(!processes[0].quantum && !sched_sleep_deadline[3] && !sched_sleeping_count);
        check_queues();
    }
}

static void test_preempt(void)
{
    for (unsigned q = 0; q < QOS_NUM_CLASSES; q++) {
        setup(q);
        pending_switch = 3;
        unsigned before = switches;
        sched_unblock(3);
        CHECK(switches == before + 1 && pending_switch == -1);
        CHECK(processes[3].state == PROC_BLOCKED);
        check_queues();
        CHECK(runq_head[q] == 1 && runq_tail[q] == 2 && ready_count == 2);
    }
}

static void test_ineligible(void)
{
    setup(QOS_DEFAULT);
    const unsigned states[] = {PROC_FREE, PROC_READY, PROC_RUNNING, PROC_ZOMBIE, PROC_ALLOCATING};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        proc_transition(&processes[3], states[i]);
        uint32_t before = ready_count;
        sched_unblock(3);
        CHECK(processes[3].state == states[i] && ready_count == before);
        check_queues();
    }
    sched_unblock(-1); sched_unblock(proc_capacity);
    check_queues();
}

static void test_irq_state(void)
{
    setup(QOS_DEFAULT);
    interrupts_enabled = false;
    sched_unblock(3);
    CHECK(!interrupts_enabled && !irq_depth);
    sched_unblock(3); sched_unblock(-1);
    CHECK(!interrupts_enabled && !irq_depth);
    check_queues();
    interrupts_enabled = true;
}

static void test_churn(void)
{
    setup(QOS_DEFAULT);
    uint32_t random = 0x5a17;
    for (unsigned i = 0; i < 10000; i++) {
        random = random * 1664525 + 1013904223;
        int idx = 3 + ((random >> 8) % 9);
        if (processes[idx].state == PROC_READY) {
            proc_transition(&processes[idx], PROC_BLOCKED);
        } else {
            proc_transition(&processes[idx], PROC_BLOCKED);
            processes[idx].qos_class = (random >> 24) % QOS_NUM_CLASSES;
            if (random & 4) pending_switch = idx;
            sched_unblock(idx);
        }
        check_queues();
    }
    CHECK(switches > 100);
}

static void test_signal(void)
{
    setup(QOS_DEFAULT);
    sched_signal_t signal={0};
    sched_signal_set(&signal,1); sched_signal_set(&signal,4);
    CHECK(signal_ipis==1 && sched_signal_take(&signal)==5 && !signal.bits);
    sched_signal_set(&signal,0); sched_signal_set(NULL,1);
    CHECK(signal_ipis==1 && !sched_signal_take(NULL));
    /* Indefinite waits also participate in the sweep and are detached on
     * every terminal/ready transition, before a task slot can be recycled. */
    signal_waits[3]=&signal; sched_signal_count=1; signal.waiters=1;
    uint64_t flags=runq_irq_save();
    sched_sleep_timeout_sweep(100);
    CHECK(processes[3].state==PROC_BLOCKED && signal.waiters==1);
    sched_signal_set(&signal,8);
    sched_sleep_timeout_sweep(100);
    CHECK(processes[3].state==PROC_READY && runq_head[QOS_DEFAULT]==3);
    CHECK(!signal.waiters && !sched_signal_count && !signal_waits[3]);
    CHECK(!processes[0].quantum && sched_signal_take(&signal)==8);
    runq_irq_restore(flags);
    check_queues();
    const unsigned targets[]={PROC_READY,PROC_RUNNING,PROC_ZOMBIE,PROC_FREE};
    for (unsigned i=0;i<sizeof(targets)/sizeof(*targets);i++) {
        proc_transition(&processes[3],PROC_BLOCKED);
        signal_waits[3]=&signal; sched_signal_count=1; signal.waiters=1;
        sched_sleep_deadline[3]=200; sched_sleeping_count=1;
        proc_transition(&processes[3],targets[i]);
        CHECK(!signal_waits[3] && !signal.waiters && !sched_signal_count);
        CHECK(!sched_sleep_deadline[3] && !sched_sleeping_count);
        sched_signal_set(&signal,1);
        flags=runq_irq_save(); sched_sleep_timeout_sweep(201); runq_irq_restore(flags);
        CHECK(processes[3].state==targets[i]);
        CHECK(sched_signal_take(&signal)==1);
        check_queues();
    }
    proc_transition(&processes[3],PROC_BLOCKED);
    signal_waits[3]=&signal; sched_signal_count=1; signal.waiters=1;
    sched_sleep_deadline[3]=101; sched_sleeping_count=1;
    flags=runq_irq_save(); sched_sleep_timeout_sweep(101); runq_irq_restore(flags);
    CHECK(processes[3].state==PROC_READY && !signal.waiters && !sched_signal_count);
    CHECK(!sched_sleep_deadline[3] && !sched_sleeping_count && !signal.bits);
    check_queues();
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "normal")) test_normal();
    else if (!strcmp(argv[1], "preempt")) test_preempt();
    else if (!strcmp(argv[1], "ineligible")) test_ineligible();
    else if (!strcmp(argv[1], "irq-state")) test_irq_state();
    else if (!strcmp(argv[1], "churn")) test_churn();
    else if (!strcmp(argv[1], "signal")) test_signal();
    else CHECK(false);
    CHECK(!irq_depth && interrupts_enabled && pending_switch == -1);
    printf("Scheduler runq %s: %u checks\n", argv[1], checks);
    return 0;
}
