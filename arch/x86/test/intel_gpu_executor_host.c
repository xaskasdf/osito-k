/* Real capture, native queue and permanent executor. MMIO/DMA are modeled;
 * joined AP operations run on actual pthreads, never on the submitter stack. */
#define sched_spawn_detached_in_address_space_blocked gc_model_spawn
#define proc_wake_pid gc_model_wake
#define sched_signal_wait gc_model_wait
#define smp_wake_bsp gc_model_bsp_wake
#define NATIVE_SUBMIT_SUITE_ENTRY submit_suite_main
#include "intel_gpu_submit_host.c"
#undef sched_spawn_detached_in_address_space_blocked
#undef proc_wake_pid
#undef sched_signal_wait
#undef smp_wake_bsp
#include "../drivers/intel_gpu_executor.h"

static void (*executor_entry)(void);
static sched_signal_t *executor_signal;
static _Thread_local jmp_buf executor_wait;
static _Thread_local bool executor_running;
static unsigned executor_spawns,executor_wakes,executor_spawn_fail=1,executor_wake_fail=1;
static uint64_t executor_ticks;
static pthread_t executor_ap_thread;
static void (*executor_ap_fn)(void *,void *);
static void *executor_ap_arg,*executor_ap_arg2;
static unsigned executor_ap_live,executor_ap_done,executor_ap_calls,executor_ap_joins,executor_ap_fail;
static unsigned executor_ap_hold,executor_timeouts;
int ws_cpu_count=2;

int sched_spawn_detached_in_address_space_blocked(const char *name,void (*entry)(void),uint64_t cr3,int owns)
{
    if (strcmp(name,"intel-gpu-exec")) return gc_model_spawn(name,entry,cr3,owns);
    CHECK(cr3==0x123000 && !owns && !executor_entry);
    executor_spawns++;
    if (executor_spawn_fail) { executor_spawn_fail=0; return -1; }
    executor_entry=entry; return 43;
}
int proc_wake_pid(int pid)
{
    if (pid!=43) return gc_model_wake(pid);
    CHECK(executor_entry); executor_wakes++;
    if (executor_wake_fail) { executor_wake_fail=0; return -1; }
    return 0;
}
void smp_wake_bsp(void) { }
enum sched_signal_result sched_signal_wait(sched_signal_t *signal,uint64_t ticks)
{
    if (!executor_running) return gc_model_wait(signal,ticks);
    if (executor_ap_live) {
        CHECK(ticks==100);
        if (__atomic_exchange_n(&executor_ap_hold,0,__ATOMIC_ACQ_REL)) {
            executor_timeouts++; return SCHED_SIGNAL_TIMEOUT;
        }
        while (!__atomic_load_n(&signal->bits,__ATOMIC_ACQUIRE) &&
               !__atomic_load_n(&executor_ap_done,__ATOMIC_ACQUIRE)) sched_yield();
        return SCHED_SIGNAL_READY;
    }
    executor_signal=signal; executor_ticks=ticks;
    __atomic_store_n(&signal->waiters,1,__ATOMIC_RELEASE);
    longjmp(executor_wait,1);
}
static void *executor_ap(void *unused)
{
    (void)unused; service_cpu=1;
    while (__atomic_load_n(&executor_ap_hold,__ATOMIC_ACQUIRE)) sched_yield();
    executor_ap_fn(executor_ap_arg,executor_ap_arg2);
    __atomic_store_n(&executor_ap_done,1,__ATOMIC_RELEASE); return NULL;
}
int smp_submit_any(void (*fn)(void *,void *),void *a,void *b)
{
    CHECK(!executor_ap_live && executor_running && !service_cpu);
    if (executor_ap_fail) { executor_ap_fail=0; return -1; }
    executor_ap_fn=fn; executor_ap_arg=a; executor_ap_arg2=b;
    executor_ap_live=1; executor_ap_done=0; executor_ap_calls++;
    CHECK(!pthread_create(&executor_ap_thread,NULL,executor_ap,NULL));
    return 123;
}
void smp_wait(int task)
{
    CHECK(task==123 && executor_ap_live);
    CHECK(!pthread_join(executor_ap_thread,NULL));
    CHECK(executor_ap_done); executor_ap_live=0; executor_ap_joins++;
}
static void executor_step(void)
{
    CHECK(executor_entry && !executor_running);
    executor_running=true;
    for (unsigned round=0;round<128;round++) {
        if (executor_signal) __atomic_store_n(&executor_signal->waiters,0,__ATOMIC_RELEASE);
        if (!setjmp(executor_wait)) { executor_entry(); CHECK(false); }
        CHECK(!executor_ap_live && executor_ap_calls==executor_ap_joins);
        if (!__atomic_load_n(&executor_signal->bits,__ATOMIC_ACQUIRE)) {
            executor_running=false; return;
        }
    }
    CHECK(false); /* no persistent self-wake or abandoned AP operation */
}

typedef struct {
    SUBMIT_FIXTURE native;
    uint64_t ticket,id;
    unsigned bind_calls,busy_bind,fail_bind,gate,entered,resume;
} EXECUTOR_FIXTURE;
static enum intel_bcs_status executor_bind(void *opaque,const intel_gpu_capture_t *capture,
    const intel_bcs_context_t **out)
{
    EXECUTOR_FIXTURE *f=opaque;
    CHECK(service_cpu==(ws_cpu_count>1 ? 1u : 0u)); f->bind_calls++;
    if (__atomic_exchange_n(&f->gate,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&f->entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    if (f->busy_bind) { f->busy_bind--; return INTEL_BCS_BUSY; }
    if (f->fail_bind) return INTEL_BCS_INVALID;
    CHECK(capture->self==capture && capture->valid && f->native.native.owner==7);
    CHECK(capture->resources[0].native.buffer==f->native.native.source &&
        capture->resources[1].native.buffer==f->native.native.destination);
    *out=&f->native.native; return INTEL_BCS_OK;
}
static void executor_setup(EXECUTOR_FIXTURE *f,bool irq)
{
    memset(f,0,sizeof(*f)); submit_setup(&f->native);
    CHECK(intel_gpu_jobs_retire(&f->native.lease,STATUS_CANCELLED)==0);
    service_step(); CHECK(f->native.jobs.done_count==1);
    /* Quiescent fixture reconfiguration before any executor publication. */
    f->native.jobs.jobs.ready=intel_gpu_executor_notify;
    CHECK(intel_gpu_jobs_submit(&f->native.jobs.jobs,7,f->native.jobs.base.context,128,64,2,2,&f->id)==0);
    f->ticket=0xfeed;
    CHECK(intel_gpu_executor_attach(&f->native.jobs.jobs,&f->native.queue,executor_bind,f,irq,&f->ticket)==STATUS_INVALID_PARAMETER);
    CHECK(f->ticket==0xfeed && intel_gpu_executor_quiescent());
    CHECK(intel_bcs_queue_unwatch(&f->native.queue)==INTEL_BCS_OK);
    CHECK(intel_gpu_executor_attach(&f->native.jobs.jobs,&f->native.queue,executor_bind,f,irq,&f->ticket)==0);
    CHECK(f->ticket!=0xfeed && !intel_gpu_executor_quiescent());
    uint64_t untouched=0xfeed;
    CHECK(intel_gpu_executor_attach(&f->native.jobs.jobs,&f->native.queue,executor_bind,f,irq,&untouched)==STATUS_INVALID_PARAMETER);
    CHECK(untouched==0xfeed && intel_gpu_executor_detach(f->ticket)==D3DKMT_STATUS_DEVICE_BUSY);
}
static void executor_cleanup(EXECUTOR_FIXTURE *f)
{
    CHECK(intel_gpu_executor_stop(f->ticket)==0);
    executor_step(); service_step();
    CHECK(f->native.queue.stopped && f->native.engine.retired && !f->native.queue.wakeup);
    CHECK(intel_gpu_executor_idle() && !intel_gpu_executor_quiescent());
    service_cpu=1; CHECK(intel_gpu_executor_detach(f->ticket)==STATUS_NOT_SUPPORTED); service_cpu=0;
    CHECK(intel_gpu_executor_detach(f->ticket)==0);
    CHECK(intel_gpu_executor_detach(f->ticket)==STATUS_INVALID_PARAMETER);
    CHECK(intel_gpu_executor_stop(f->ticket)==STATUS_INVALID_PARAMETER);
    CHECK(intel_gpu_executor_quiescent());
    CHECK(intel_bcs_queue_unpublish(&f->native.queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_context_destroy(&f->native.native));
    service_step(); jobs_cleanup(&f->native.jobs);
    memset(f,0xa5,sizeof(*f));
    intel_gpu_executor_notify(f); executor_step(); /* late hint has no owner pointer */
    CHECK(intel_gpu_executor_idle() && intel_gpu_executor_quiescent());
}
static void executor_cases(void)
{
    service_cpu=1; CHECK(intel_gpu_executor_start()==STATUS_NOT_SUPPORTED); service_cpu=0;
    intel_gpu_executor_notify(NULL); /* hint before the kernel service exists */
    CHECK(intel_gpu_executor_start()==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(intel_gpu_executor_start()==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(executor_spawns==2 && executor_wakes==1);
    CHECK(intel_gpu_executor_start()==0 && executor_spawns==2 && executor_wakes==2);
    executor_step(); CHECK(intel_gpu_executor_idle() && intel_gpu_executor_quiescent());
    for (unsigned mode=0;mode<10;mode++) {
        EXECUTOR_FIXTURE f; executor_setup(&f,mode&1);
        intel_gpu_jobs_snapshot_t snapshot;
        CHECK(intel_gpu_jobs_snapshot(&f.native.jobs.jobs,&snapshot)==0 && snapshot.queued==1 && !snapshot.claimed);
        if (mode==1) f.busy_bind=3;
        if (mode==2 || mode==9) f.fail_bind=1;
        if (mode==3) CHECK(intel_gpu_jobs_cancel(&f.native.jobs.jobs,7,f.id)==0);
        if (mode==4) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
        if (mode==5) executor_ap_fail=1;
        if (mode==6) { executor_ap_hold=1; }
        if (mode==7) ws_cpu_count=1;
        if (mode>=8) {
            for (unsigned i=1;i<INTEL_GPU_JOBS;i++)
                CHECK(intel_gpu_jobs_submit(&f.native.jobs.jobs,7,f.native.jobs.base.context,128,64,2,2,&f.id)==0);
        }
        for (unsigned round=0;round<64;round++) {
            executor_step(); service_step();
            if (f.native.jobs.done_count==(mode>=8 ? INTEL_GPU_JOBS+1 : 2)) break;
            /* Only a timer or a retained hint may run the executor again.
             * Calling it unconditionally would hide a lost reactivation. */
            if(mode>=8) CHECK(executor_ticks || executor_signal->bits);
        }
        CHECK(f.native.jobs.done_count==(mode>=8 ? INTEL_GPU_JOBS+1 : 2));
        NTSTATUS expected=(mode==2 || mode==9) ? STATUS_NOT_SUPPORTED : mode==3 ? STATUS_CANCELLED :
            mode==4 ? STATUS_PROCESS_IS_TERMINATING : STATUS_SUCCESS;
        CHECK(f.native.jobs.done_status[1]==expected);
        CHECK(f.native.engine.submitted_jobs==(mode==8 ? INTEL_GPU_JOBS : (mode>=2 && mode<=4) || mode==9 ? 0 : 1));
        executor_step(); CHECK(intel_gpu_executor_idle() && !executor_ticks);
        unsigned writes=hw.writes;
        for (unsigned hint=0;hint<8;hint++) { intel_gpu_executor_notify(NULL); executor_step(); }
        CHECK(hw.writes==writes);
        executor_cleanup(&f); ws_cpu_count=2;
    }
    CHECK(executor_timeouts==1); /* same AP is joined after a wait timeout */
    EXECUTOR_FIXTURE f; executor_setup(&f,true); hw.delay=1000;
    executor_step(); service_step();
    CHECK(f.native.engine.submitted_jobs==1 && !f.native.engine.completed_jobs &&
        f.native.jobs.done_count==1 && executor_ticks==50 && !intel_gpu_executor_idle());
    vm_model_exit(7); process_closing=true; d3dkmt_release_process(7);
    hw.stall_stop=true;
    CHECK(intel_gpu_executor_stop(f.ticket)==0); executor_step(); service_step();
    CHECK(!f.native.engine.retired && f.native.native.source_access.self && f.native.jobs.done_count==1);
    CHECK(intel_gpu_executor_detach(f.ticket)==D3DKMT_STATUS_DEVICE_BUSY && !intel_gpu_executor_quiescent());
    unsigned writes=hw.writes;
    for (unsigned hint=0;hint<8;hint++) { intel_gpu_executor_notify(NULL); executor_step(); }
    CHECK(hw.writes==writes && !executor_ticks); /* failed reset parks, resources stay owned */
    hw.stall_stop=false;
    executor_cleanup(&f); /* explicit stop retries retirement */
}
static void *executor_thread(void *unused)
{
    (void)unused; executor_step(); return NULL;
}
static void executor_races(void)
{
    for (unsigned round=0;round<64;round++) {
        EXECUTOR_FIXTURE f; executor_setup(&f,round&1); f.gate=1;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,executor_thread,NULL));
        while (!__atomic_load_n(&f.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_gpu_executor_detach(f.ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        if (round&1) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
        else CHECK(intel_gpu_jobs_cancel(&f.native.jobs.jobs,7,f.id)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_executor_stop(f.ticket)==0);
        CHECK(intel_gpu_executor_detach(f.ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(f.native.jobs.done_count==1 && f.native.jobs.base.service.native.memory.pages);
        __atomic_store_n(&f.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        service_step();
        CHECK(f.native.jobs.done_count==2 && !f.native.engine.submitted_jobs);
        CHECK(f.native.jobs.done_status[1]==STATUS_CANCELLED);
        executor_cleanup(&f);
    }
}
#ifndef NATIVE_EXECUTOR_SUITE_ENTRY
#define NATIVE_EXECUTOR_SUITE_ENTRY main
#endif
int NATIVE_EXECUTOR_SUITE_ENTRY(void)
{
    if (submit_suite_main()) return 1;
    unsigned before=checks;
    executor_cases(); executor_races();
    CHECK(executor_ap_calls==executor_ap_joins && !executor_ap_live);
    printf("Intel permanent native executor: %u checks passed, 64 forced stop/exit races, %u joined AP operations\n",
        checks-before,executor_ap_joins);
    return 0;
}
