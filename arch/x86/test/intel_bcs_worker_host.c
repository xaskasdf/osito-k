/* Run the production persistent worker and queue with a register model.
 * pthreads stand in for the native BSP task, AP pool and scheduler blocking;
 * physical scheduling/MMIO are separately qualified in the P1 diagnostic. */
#define main bcs_register_model_tests
#include "intel_bcs_host.c"
#undef main
#include "../drivers/intel_bcs_worker.h"
#include <errno.h>
#include <time.h>
#include <sched.h>

static pthread_mutex_t scheduler_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scheduler_event=PTHREAD_COND_INITIALIZER;
static _Thread_local uint32_t host_cpu;
static _Thread_local int host_pid=1;
static struct {
    pthread_t thread, ap;
    void (*entry)(void), (*operation)(void *,void *);
    void *argument, *result;
    unsigned awake, killed, done, live, ap_live, ap_hold, ap_entered;
    unsigned spawn_fail, submit_fail, submits, joins;
    int pid;
} scheduler;
int ws_cpu_count=2;
static intel_bcs_worker_t service;
static sched_signal_t client;
static unsigned client_calls;

uint32_t smp_current_cpu(void) { return host_cpu; }
uint64_t paging_get_kernel_cr3(void) { return 0x1000; }
int32_t proc_current_pid(void) { return host_pid; }

static void *host_worker_thread(void *unused)
{
    (void)unused;
    host_pid=scheduler.pid;
    CHECK(pthread_mutex_lock(&scheduler_lock)==0);
    while (!scheduler.awake && !scheduler.killed)
        CHECK(pthread_cond_wait(&scheduler_event,&scheduler_lock)==0);
    bool killed=scheduler.killed;
    CHECK(pthread_mutex_unlock(&scheduler_lock)==0);
    if (!killed) scheduler.entry();
    __atomic_store_n(&scheduler.done,1,__ATOMIC_RELEASE);
    return NULL;
}

int sched_spawn_in_address_space_blocked(const char *name, void (*entry)(void),
    uint64_t cr3, int owns)
{
    CHECK(!strcmp(name,"intel-bcs") && cr3==0x1000 && !owns && !scheduler.live);
    if (scheduler.spawn_fail) return -1;
    scheduler.awake=scheduler.killed=scheduler.done=0;
    scheduler.entry=entry; scheduler.live=1; scheduler.pid++;
    CHECK(pthread_create(&scheduler.thread,NULL,host_worker_thread,NULL)==0);
    return scheduler.pid;
}
int proc_wake_pid(int pid)
{
    CHECK(pid==scheduler.pid && scheduler.live);
    CHECK(pthread_mutex_lock(&scheduler_lock)==0);
    scheduler.awake=1;
    CHECK(pthread_cond_broadcast(&scheduler_event)==0);
    CHECK(pthread_mutex_unlock(&scheduler_lock)==0);
    return 0;
}
int proc_kill_pid(int pid)
{
    CHECK(pid==scheduler.pid && scheduler.live);
    CHECK(pthread_mutex_lock(&scheduler_lock)==0);
    CHECK(!scheduler.awake); /* rollback of an unpublished blocked task only */
    scheduler.killed=1;
    CHECK(pthread_cond_broadcast(&scheduler_event)==0);
    CHECK(pthread_mutex_unlock(&scheduler_lock)==0);
    CHECK(pthread_join(scheduler.thread,NULL)==0);
    return 0;
}
int proc_waitpid(uint32_t pid, int32_t *status)
{
    (void)status;
    CHECK(pid==(uint32_t)scheduler.pid && scheduler.live);
    if (!__atomic_load_n(&scheduler.done,__ATOMIC_ACQUIRE)) return -1;
    if (!scheduler.killed) CHECK(pthread_join(scheduler.thread,NULL)==0);
    scheduler.live=0;
    return 0;
}
static void *host_ap_thread(void *unused)
{
    (void)unused;
    host_cpu=1;
    __atomic_store_n(&scheduler.ap_entered,1,__ATOMIC_RELEASE);
    while (__atomic_load_n(&scheduler.ap_hold,__ATOMIC_ACQUIRE)) sched_yield();
    scheduler.operation(scheduler.argument,scheduler.result);
    return NULL;
}
int smp_submit_any(void (*fn)(void *,void *), void *arg, void *result)
{
    CHECK(!host_cpu && !scheduler.ap_live);
    if (scheduler.submit_fail) { scheduler.submit_fail--; return -1; }
    scheduler.ap_live=1; scheduler.ap_entered=0;
    scheduler.operation=fn; scheduler.argument=arg; scheduler.result=result;
    __atomic_add_fetch(&scheduler.submits,1,__ATOMIC_RELEASE);
    CHECK(pthread_create(&scheduler.ap,NULL,host_ap_thread,NULL)==0);
    return 7;
}
void smp_wait(int task)
{
    CHECK(task==7 && scheduler.ap_live);
    CHECK(pthread_join(scheduler.ap,NULL)==0);
    scheduler.ap_live=0;
    __atomic_add_fetch(&scheduler.joins,1,__ATOMIC_RELEASE);
}
void smp_wake_bsp(void)
{
    CHECK(pthread_mutex_lock(&scheduler_lock)==0);
    CHECK(pthread_cond_broadcast(&scheduler_event)==0);
    CHECK(pthread_mutex_unlock(&scheduler_lock)==0);
}
enum sched_signal_result sched_signal_wait(sched_signal_t *signal, uint64_t ticks)
{
    CHECK(!host_cpu && signal);
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME,&until)==0);
    uint64_t unit=service.interrupt_driven ? 10000000 : 1000000;
    uint64_t ns=(uint64_t)until.tv_nsec+(ticks ? ticks : 5000)*unit;
    until.tv_sec+=ns/1000000000; until.tv_nsec=ns%1000000000;
    CHECK(pthread_mutex_lock(&scheduler_lock)==0);
    __atomic_add_fetch(&signal->waiters,1,__ATOMIC_RELEASE);
    int result=0;
    while (!__atomic_load_n(&signal->bits,__ATOMIC_ACQUIRE) && result!=ETIMEDOUT) {
        result=pthread_cond_timedwait(&scheduler_event,&scheduler_lock,&until);
        CHECK(result==0 || result==ETIMEDOUT);
    }
    bool ready=__atomic_load_n(&signal->bits,__ATOMIC_ACQUIRE)!=0;
    __atomic_sub_fetch(&signal->waiters,1,__ATOMIC_RELEASE);
    CHECK(pthread_mutex_unlock(&scheduler_lock)==0);
    CHECK(ticks || ready); /* test harness watchdog for an indefinite wait */
    return ready ? SCHED_SIGNAL_READY : SCHED_SIGNAL_TIMEOUT;
}
static void worker_notify(void *ctx, uint32_t events)
{
    CHECK(ctx==&client);
    __atomic_add_fetch(&client_calls,1,__ATOMIC_RELAXED);
    sched_signal_set(ctx,events);
}
static void worker_pause(void)
{
    struct timespec pause={0,1000000};
    CHECK(nanosleep(&pause,NULL)==0);
}
static void worker_wait_idle(void)
{
    unsigned budget=5000;
    while (!intel_bcs_worker_idle(&service) && --budget) worker_pause();
    CHECK(budget && scheduler.submits==scheduler.joins);
}
static void worker_finish(void)
{
    CHECK(intel_bcs_worker_stop(&service)==INTEL_BCS_PENDING);
    unsigned budget=5000;
    while (intel_bcs_worker_reap(&service)!=INTEL_BCS_OK && --budget) worker_pause();
    CHECK(budget && !service.started && !scheduler.live && !scheduler.ap_live);
    CHECK(queue.stopped && queue_engine.retired && !queue.wakeup && !queue.notifying);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
}
static void worker_tests(void)
{
    scheduler.pid=100;
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
    scheduler.spawn_fail=1;
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,false)==INTEL_BCS_BUSY);
    CHECK(!service.self && !service.started && !queue.wakeup);
    scheduler.spawn_fail=0;
    CHECK(intel_bcs_queue_watch(&queue,worker_notify,&client)==INTEL_BCS_OK);
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,false)==INTEL_BCS_INVALID);
    CHECK(!service.self && !service.started && !scheduler.live);
    CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_OK);
    /* Rejected AP caller cannot publish native task state. */
    host_cpu=1;
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,false)==INTEL_BCS_INVALID);
    host_cpu=0;
    uint64_t fence;
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
    scheduler.submit_fail=3;
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,false)==INTEL_BCS_OK);
    int pid=service.pid;
    for (unsigned i=0;i<64;i++) {
        if (i) CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        intel_bcs_job_result_t result;
        for (;;) {
            sched_signal_take(&client);
            CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK);
            if (result.phase==INTEL_BCS_JOB_COMPLETE) break;
            CHECK(result.phase<INTEL_BCS_JOB_COMPLETE);
            CHECK(sched_signal_wait(&client,1000)==SCHED_SIGNAL_READY);
        }
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK && !result.error);
        worker_wait_idle();
        CHECK(service.pid==pid && service.ap_operations==i+1 && !service.bsp_operations);
        unsigned operations=service.operations;
        worker_pause();
        CHECK(service.operations==operations); /* truly parked, no idle AP jobs */
    }
    /* Wait timeout cannot release/reuse AP argument or submit a second job. */
    scheduler.ap_entered=0; scheduler.ap_hold=1;
    CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[1],&queue_copy,&fence)==INTEL_BCS_OK);
    unsigned budget=5000;
    while (!__atomic_load_n(&scheduler.ap_entered,__ATOMIC_ACQUIRE) && --budget) worker_pause();
    CHECK(budget);
    unsigned submitted=scheduler.submits;
    for (unsigned i=0;i<250;i++) worker_pause();
    CHECK(scheduler.submits==submitted && scheduler.joins+1==submitted && !service.finished);
    CHECK(intel_bcs_worker_stop(&service)==INTEL_BCS_PENDING);
    CHECK(intel_bcs_worker_reap(&service)==INTEL_BCS_PENDING && queue.wakeup);
    __atomic_store_n(&scheduler.ap_hold,0,__ATOMIC_RELEASE);
    worker_finish();
    unsigned calls=client_calls;
    d3dkmt_release_process(41);
    CHECK(client_calls==calls); /* detached before object lifetime ends */

    /* A failed retirement quarantines all pointers; an explicit retry may
     * retire and join later. No false fence success or release in between. */
    memset(&service,0,sizeof(service)); bcs_queue_host_start();
    CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
    hw.stall_reset=true; hw.stall_stop=true;
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,false)==INTEL_BCS_OK);
    CHECK(intel_bcs_worker_stop(&service)==INTEL_BCS_PENDING);
    budget=5000;
    while ((!__atomic_load_n(&service.wake.waiters,__ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&scheduler.joins,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&service.task,__ATOMIC_ACQUIRE)>=0) && --budget) worker_pause();
    /* Allow the first shutdown to run, then verify its stable quarantined state. */
    for (unsigned i=0;i<20;i++) worker_pause();
    CHECK(budget && service.last_status==INTEL_BCS_QUARANTINED);
    CHECK(service.started && !service.finished && queue.wakeup && !queue.stopped);
    CHECK(intel_bcs_worker_reap(&service)==INTEL_BCS_PENDING);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY);
    hw.stall_reset=false; hw.stall_stop=false;
    worker_finish();

    /* Single CPU fallback and an automatic fault use the same lifetime path. */
    memset(&service,0,sizeof(service)); bcs_queue_host_start();
    CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
    ws_cpu_count=1; prepare_failure=true;
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,false)==INTEL_BCS_OK);
    budget=5000;
    while (intel_bcs_worker_reap(&service)!=INTEL_BCS_OK && --budget) worker_pause();
    CHECK(budget && service.bsp_operations==2 && !service.ap_operations);
    intel_bcs_job_result_t result;
    CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK && result.phase==INTEL_BCS_JOB_FAILED);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
}
static uint64_t worker_irq_now(void *unused)
{
    (void)unused;
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    return (uint64_t)now.tv_sec*1000000000+now.tv_nsec;
}
static void worker_irq_tests(void)
{
    ws_cpu_count=2;
    memset(&service,0,sizeof(service)); bcs_queue_host_start();
    queue.io.now_ns=worker_irq_now;
    hw.delay=1; /* delivered completion is visible on the following status read */
    CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_worker_start(&service,&queue,worker_notify,&client,true)==INTEL_BCS_OK);
    worker_wait_idle();
    CHECK(pthread_barrier_init(&race_enter,NULL,2)==0);
    CHECK(pthread_barrier_init(&race_release,NULL,2)==0);
    for (unsigned i=0;i<17;i++) {
        uint64_t fence;
        if (!i) race_stage=2;
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        if (!i) {
            /* Interrupt before the AP returns its submission result. */
            (void)pthread_barrier_wait(&race_enter);
            intel_bcs_worker_interrupt(&service,false);
            (void)pthread_barrier_wait(&race_release);
        } else {
            unsigned budget=5000;
            while ((!__atomic_load_n(&service.wake.waiters,__ATOMIC_ACQUIRE) ||
                __atomic_load_n(&service.task,__ATOMIC_ACQUIRE)>=0 ||
                __atomic_load_n(&service.operations,__ATOMIC_ACQUIRE)!=i*2+1) && --budget) worker_pause();
            CHECK(budget);
            unsigned operations=service.operations;
            for (unsigned n=0;n<3;n++) worker_pause();
            CHECK(service.operations==operations); /* no active completion polling */
            intel_bcs_worker_interrupt(&service,false);
        }
        intel_bcs_job_result_t result;
        for (;;) {
            sched_signal_take(&client);
            CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK);
            if (result.phase==INTEL_BCS_JOB_COMPLETE) break;
            CHECK(result.phase<INTEL_BCS_JOB_COMPLETE);
            CHECK(sched_signal_wait(&client,100)==SCHED_SIGNAL_READY);
        }
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK && !result.error);
        worker_wait_idle(); race_stage=0;
        CHECK(service.operations==(i+1)*2 && !service.watchdog_wakes);
    }
    CHECK(pthread_barrier_destroy(&race_enter)==0 && pthread_barrier_destroy(&race_release)==0);
    /* A lost interrupt still allows a real completion to be observed once
     * by the watchdog. With a missing token it reports failure, never success. */
    for (unsigned missing=0;missing<2;missing++) {
        hw.missing_token=missing!=0;
        uint64_t fence;
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        intel_bcs_job_result_t result;
        for (;;) {
            sched_signal_take(&client);
            CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK);
            if (result.phase>=INTEL_BCS_JOB_COMPLETE) break;
            enum sched_signal_result waited=sched_signal_wait(&client,150);
            if (waited!=SCHED_SIGNAL_READY) {
                intel_bcs_queue_snapshot_t snapshot;
                CHECK(intel_bcs_queue_snapshot(&queue,&snapshot)==INTEL_BCS_OK);
                fprintf(stderr,"worker watchdog missing=%u wait=%u queued=%u active=%u terminal=%u fault=%u closing=%u busy=%u operations=%llu task=%d done=%u wake=%u client=%u\n",
                    missing,waited,snapshot.queued,snapshot.active,snapshot.terminal,snapshot.fault,
                    snapshot.closing,snapshot.worker_busy,(unsigned long long)__atomic_load_n(&service.operations,__ATOMIC_ACQUIRE),
                    __atomic_load_n(&service.task,__ATOMIC_ACQUIRE),__atomic_load_n(&service.op_done,__ATOMIC_ACQUIRE),
                    __atomic_load_n(&service.wake.bits,__ATOMIC_ACQUIRE),__atomic_load_n(&client.bits,__ATOMIC_ACQUIRE));
            }
            CHECK(waited==SCHED_SIGNAL_READY);
        }
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK);
        if (!missing) {
            CHECK(result.phase==INTEL_BCS_JOB_COMPLETE && !result.error);
            worker_wait_idle(); CHECK(service.watchdog_wakes==1);
        } else {
            CHECK(result.phase==INTEL_BCS_JOB_FAILED && result.error==INTEL_BCS_TIMEOUT);
            unsigned budget=5000;
            while (intel_bcs_worker_reap(&service)!=INTEL_BCS_OK && --budget) worker_pause();
            CHECK(budget && service.watchdog_wakes==2);
            CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
        }
    }
}
int main(void)
{
    worker_tests();
    worker_irq_tests();
    printf("Intel BCS persistent worker: %u checks, 64 parked/resumed batches, 17 IRQ jobs and 2 watchdog cases passed\n",checks);
    return 0;
}
