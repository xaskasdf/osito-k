/* Reuse the register/failure model; exercise real blocking host consumers of
 * the production queue. No synthetic completion or graphics API is added. */
#define main bcs_register_model_tests
#include "intel_bcs_host.c"
#undef main
#include <errno.h>
#include <semaphore.h>
#include <time.h>
#include <sched.h>

static struct {
    sem_t work, result, entered;
    unsigned seen, calls, hold, resume, stop, sleeps, completed;
} wake;

static void wake_signal(void *context, uint32_t events)
{
    CHECK(context==&wake && events && !(events&~INTEL_BCS_EVENT_ALL));
    /* A callback can inspect state; a queue lock held here deadlocks. */
    intel_bcs_queue_snapshot_t snapshot;
    CHECK(intel_bcs_queue_snapshot(&queue,&snapshot)==INTEL_BCS_OK);
    __atomic_fetch_or(&wake.seen,events,__ATOMIC_RELEASE);
    __atomic_add_fetch(&wake.calls,1,__ATOMIC_RELAXED);
    if (events&(INTEL_BCS_EVENT_WORK|INTEL_BCS_EVENT_STATE|INTEL_BCS_EVENT_FAULT))
        CHECK(sem_post(&wake.work)==0);
    if (events&(INTEL_BCS_EVENT_RESULT|INTEL_BCS_EVENT_SPACE|INTEL_BCS_EVENT_STATE|INTEL_BCS_EVENT_FAULT))
        CHECK(sem_post(&wake.result)==0);
    /* Test-only callback suspension forces a concurrent teardown window.
     * Production sinks must only signal, never wait as this injection does. */
    if (__atomic_exchange_n(&wake.hold,0,__ATOMIC_ACQ_REL)) {
        CHECK(sem_post(&wake.entered)==0);
        while (!__atomic_load_n(&wake.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}

static void wake_wait(sem_t *signal)
{
    struct timespec until;
    CHECK(clock_gettime(CLOCK_REALTIME,&until)==0);
    until.tv_sec+=5;
    int r;
    do { r=sem_timedwait(signal,&until); } while (r<0 && errno==EINTR);
    CHECK(r==0);
}

static void *wake_worker(void *unused)
{
    (void)unused;
    for (;;) {
        __atomic_add_fetch(&wake.sleeps,1,__ATOMIC_RELEASE);
        wake_wait(&wake.work);
        if (__atomic_load_n(&wake.stop,__ATOMIC_ACQUIRE)) return NULL;
        /* This host MMIO model completes by observation. Physical IRQ/timer
         * delivery is separate; only its queue/wait contract is tested here. */
        for (unsigned budget=0;;budget++) {
            CHECK(budget<10000);
            intel_bcs_queue_snapshot_t s;
            CHECK(intel_bcs_queue_snapshot(&queue,&s)==INTEL_BCS_OK);
            if (s.closing || s.fault || (!s.queued && !s.active)) break;
            enum intel_bcs_status r=intel_bcs_queue_step(&queue);
            CHECK(r==INTEL_BCS_OK || r==INTEL_BCS_PENDING);
        }
    }
}

static void *wake_exit(void *unused)
{
    (void)unused;
    d3dkmt_release_process(41);
    return NULL;
}

static void wake_tests(void)
{
    memset(&wake,0,sizeof(wake));
    CHECK(sem_init(&wake.work,0,0)==0);
    CHECK(sem_init(&wake.result,0,0)==0);
    CHECK(sem_init(&wake.entered,0,0)==0);
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_watch(NULL,wake_signal,&wake)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_watch(&queue,NULL,&wake)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_unwatch(NULL)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_snapshot(&queue,NULL)==INTEL_BCS_INVALID);
    uint64_t fence;
    /* Work predates the observer and its thread: the initial signal must be
     * retained, even though nobody was sleeping when watch() returned. */
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_watch(&queue,wake_signal,&wake)==INTEL_BCS_OK);
    CHECK(wake.calls==1 && wake.seen==INTEL_BCS_EVENT_ALL);
    CHECK(intel_bcs_queue_watch(&queue,wake_signal,&wake)==INTEL_BCS_INVALID);
    pthread_t worker;
    CHECK(pthread_create(&worker,NULL,wake_worker,NULL)==0);
    for (unsigned i=0;i<128;i++) {
        if (i) CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        intel_bcs_job_result_t result;
        for (;;) {
            CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK);
            if (result.phase==INTEL_BCS_JOB_COMPLETE) break;
            CHECK(result.phase<INTEL_BCS_JOB_COMPLETE);
            wake_wait(&wake.result);
        }
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK);
        CHECK(result.error==INTEL_BCS_OK);
        wake.completed++;
    }
    __atomic_store_n(&wake.stop,1,__ATOMIC_RELEASE);
    CHECK(sem_post(&wake.work)==0);
    CHECK(pthread_join(worker,NULL)==0);
    CHECK(wake.completed==128 && queue_engine.completed_jobs==128 && !hw.resets);
    CHECK(wake.sleeps>1);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_OK);
    unsigned before=wake.calls;
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK && wake.calls==before);

    /* Full queues keep all live results. Consumption emits SPACE, allowing a
     * blocked producer to retry without polling or overwriting an old fence. */
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_watch(&queue,wake_signal,&wake)==INTEL_BCS_OK);
    uint64_t fences[INTEL_BCS_QUEUE_JOBS], sentinel=0x4321;
    intel_bcs_job_result_t result;
    for (unsigned i=0;i<INTEL_BCS_QUEUE_JOBS;i++) {
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fences[i])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_cancel(&queue,41,fences[i])==INTEL_BCS_OK);
    }
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&sentinel)==INTEL_BCS_BUSY && sentinel==0x4321);
    wake.seen=0; before=wake.calls;
    CHECK(intel_bcs_queue_result(&queue,41,fences[0],false,&result)==INTEL_BCS_OK && wake.calls==before);
    CHECK(intel_bcs_queue_result(&queue,41,fences[0],true,&result)==INTEL_BCS_OK);
    CHECK(wake.seen==INTEL_BCS_EVENT_SPACE);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK && fence>fences[31]);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_OK);

    /* An exit callback must run outside BOTH registry and queue locks. The
     * unrelated owner remains usable while that notification is suspended. */
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_watch(&queue,wake_signal,&wake)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
    wake.seen=0; wake.hold=1;
    CHECK(pthread_create(&worker,NULL,wake_exit,NULL)==0);
    wake_wait(&wake.entered);
    CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK && result.phase==INTEL_BCS_JOB_CANCELLED);
    CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[1],&queue_copy,&sentinel)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY);
    d3dkmt_release_process(42); /* locks the same registry during callback */
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY);
    CHECK(wake.seen&(INTEL_BCS_EVENT_RESULT|INTEL_BCS_EVENT_STATE));
    __atomic_store_n(&wake.resume,1,__ATOMIC_RELEASE);
    CHECK(pthread_join(worker,NULL)==0);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY); /* installed sink */
    CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
    before=wake.calls;
    d3dkmt_release_process(41);
    CHECK(wake.calls==before && !queue.notifying);

    /* Failed preparation and uncertain DMA both wake the consumer, without
     * converting the fault hint into a successful completion or RAM release. */
    for (unsigned uncertain=0;uncertain<2;uncertain++) {
        bcs_queue_host_start();
        CHECK(intel_bcs_queue_watch(&queue,wake_signal,&wake)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        if (uncertain) hw.fail_at=hw.ops+1; else prepare_failure=true;
        wake.seen=0;
        CHECK(intel_bcs_queue_step(&queue)==(uncertain?INTEL_BCS_IO:INTEL_BCS_INVALID));
        CHECK(wake.seen&INTEL_BCS_EVENT_FAULT);
        CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK);
        CHECK(result.phase==(uncertain?INTEL_BCS_JOB_RUNNING:INTEL_BCS_JOB_FAILED));
        if (uncertain) {
            CHECK(!(wake.seen&INTEL_BCS_EVENT_RESULT));
            CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
        }
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK && result.phase==INTEL_BCS_JOB_FAILED);
        CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_OK);
    }
    CHECK(sem_destroy(&wake.work)==0 && sem_destroy(&wake.result)==0 && sem_destroy(&wake.entered)==0);
}

static void wake_contention_tests(void)
{
    memset(&wake,0,sizeof(wake));
    CHECK(sem_init(&wake.work,0,0)==0 && sem_init(&wake.result,0,0)==0 && sem_init(&wake.entered,0,0)==0);
    for (unsigned stopping=0;stopping<2;stopping++) {
        bcs_queue_host_start(); memset(&access_test,0,sizeof(access_test));
        queue.ops.acquire=bcs_queue_model_acquire; queue.ops.retire=bcs_queue_model_retire;
        CHECK(intel_bcs_queue_watch(&queue,wake_signal,&wake)==INTEL_BCS_OK);
        uint64_t fence;
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        access_test.busy=1; access_test.retire_busy=3;
        unsigned before=wake.calls;
        for (unsigned retry=0;retry<16;retry++) {
            CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_BUSY);
            CHECK(wake.calls==before && !queue_engine.submitted_jobs);
        }
        access_test.busy=0;
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
        access_test.retire_busy=32;
        enum intel_bcs_status r;
        do { r=intel_bcs_queue_step(&queue); } while (r==INTEL_BCS_PENDING);
        CHECK(r==INTEL_BCS_BUSY && access_test.held[0]==fence && wake.calls==before);
        if (stopping) {
            CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_BUSY);
            CHECK(wake.calls>before && (wake.seen&INTEL_BCS_EVENT_STATE));
            before=wake.calls; /* the real close transition wakes exactly once */
        }
        for (unsigned retry=0;retry<16;retry++) {
            CHECK((stopping ? intel_bcs_queue_shutdown(&queue) : intel_bcs_queue_step(&queue))==INTEL_BCS_BUSY);
            CHECK(wake.calls==before && access_test.held[0]==fence);
        }
        access_test.retire_busy=0;
        CHECK((stopping ? intel_bcs_queue_shutdown(&queue) : intel_bcs_queue_step(&queue))==INTEL_BCS_OK);
        CHECK(wake.calls>before && !access_test.held[0]);
        intel_bcs_job_result_t result;
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unwatch(&queue)==INTEL_BCS_OK);
    }
    CHECK(sem_destroy(&wake.work)==0 && sem_destroy(&wake.result)==0 && sem_destroy(&wake.entered)==0);
}

int main(void)
{
    wake_tests();
    wake_contention_tests();
    printf("Intel BCS wakeup: %u checks, 128 blocking-consumer jobs passed\n",checks);
    return 0;
}
