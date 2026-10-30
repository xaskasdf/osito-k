#include "intel_gpu_executor.h"
#include "../kernel/sched_signal.h"
#ifdef __KERNEL_X86__
#include "../kernel/smp_work.h"
#else
extern int ws_cpu_count;
extern int smp_submit_any(void (*)(void *,void *),void *,void *);
extern void smp_wait(int);
#endif
extern uint32_t smp_current_cpu(void);
extern uint64_t paging_get_kernel_cr3(void);
extern int sched_spawn_detached_in_address_space_blocked(const char *,void (*)(void),uint64_t,int);
extern int proc_wake_pid(int);
extern void sched_yield(void);

typedef struct {
    intel_gpu_jobs_t *jobs;
    intel_bcs_queue_t *queue;
    intel_gpu_executor_bind_t bind;
    intel_gpu_executor_maintain_t maintain;
    void *context;
    intel_gpu_job_lease_t lease;
    sched_signal_t joined;
    uint64_t ticket;
    unsigned stop,retry,done;
    unsigned wait_ticks;
    bool busy,ready,retired,retirement_failed,native_pending,interrupt_driven;
} gpu_executor_owner_t;
static gpu_executor_owner_t gpu_executor_owners[INTEL_GPU_EXECUTOR_OWNERS];
static sched_signal_t gpu_executor_signal;
static unsigned gpu_executor_guard,gpu_executor_state,gpu_executor_retry;
static uint64_t gpu_executor_next=1;
static int gpu_executor_pid;

static uint64_t gpu_executor_lock(void)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&gpu_executor_guard,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void gpu_executor_unlock(uint64_t flags)
{
    __atomic_store_n(&gpu_executor_guard,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
void intel_gpu_executor_notify(void *unused)
{
    (void)unused; sched_signal_set(&gpu_executor_signal,1);
}
static void gpu_executor_queue_wake(void *unused,uint32_t events)
{
    (void)unused; (void)events; intel_gpu_executor_notify(NULL);
}
static NTSTATUS gpu_executor_retire(gpu_executor_owner_t *s,NTSTATUS result)
{
    if (!s->lease.self) return STATUS_SUCCESS;
    NTSTATUS status=intel_gpu_jobs_poll(&s->lease);
    if (status==STATUS_INVALID_PARAMETER && !s->native_pending)
        status=intel_gpu_jobs_retire(&s->lease,result); /* no native reservation */
    if (status==STATUS_SUCCESS) s->native_pending=false;
    return status;
}
static void gpu_executor_fault(gpu_executor_owner_t *s,enum intel_bcs_status error)
{
    /* Do not invent a DMA fault for a generic IO/binding failure. A timeout
     * or explicit engine fault has a precise public meaning; otherwise stop. */
    intel_gpu_jobs_fail(s->jobs,error==INTEL_BCS_TIMEOUT ? OKMT_EXECUTION_HUNG :
        error==INTEL_BCS_ENGINE_ERROR ? OKMT_EXECUTION_DMA_FAULT : OKMT_EXECUTION_STOPPED);
}
static void gpu_executor_operation(void *opaque,void *unused)
{
    (void)unused;
    gpu_executor_owner_t *s=opaque;
    s->wait_ticks=0;
    unsigned budget=0;
    for (;budget<4;budget++) {
        intel_bcs_queue_snapshot_t queue;
        intel_gpu_jobs_snapshot_t jobs;
        if (intel_bcs_queue_snapshot(s->queue,&queue)!=INTEL_BCS_OK ||
            intel_gpu_jobs_snapshot(s->jobs,&jobs)!=0) {
            gpu_executor_fault(s,INTEL_BCS_INVALID); s->retirement_failed=true; break;
        }
        bool stopping=__atomic_load_n(&s->stop,__ATOMIC_ACQUIRE) || queue.closing || queue.fault || jobs.closing;
        if (stopping) {
            if (queue.fault) gpu_executor_fault(s,queue.fault);
            __atomic_store_n(&s->stop,1,__ATOMIC_RELEASE);
            bool retry=__atomic_exchange_n(&s->retry,0,__ATOMIC_ACQ_REL)!=0;
            if (s->retirement_failed && !retry) break;
            if (!jobs.closing) intel_gpu_jobs_shutdown(s->jobs);
            enum intel_bcs_status status=intel_bcs_queue_shutdown(s->queue);
            if (status==INTEL_BCS_OK) {
                NTSTATUS retired=gpu_executor_retire(s,STATUS_CANCELLED);
                if (retired==STATUS_SUCCESS) {
                    if (intel_bcs_queue_unwatch(s->queue)==INTEL_BCS_OK) s->retired=true;
                    else s->wait_ticks=1;
                } else if (retired==D3DKMT_STATUS_DEVICE_BUSY) s->wait_ticks=1;
                else s->retirement_failed=true;
            } else if (status==INTEL_BCS_BUSY) s->wait_ticks=1;
            else { gpu_executor_fault(s,status); s->retirement_failed=true; }
            break;
        }
        if (!s->lease.self) {
            if (s->maintain) {
                enum intel_bcs_status maintained=s->maintain(s->context);
                if (maintained!=INTEL_BCS_OK) {
                    if (maintained!=INTEL_BCS_BUSY) {
                        gpu_executor_fault(s,maintained); __atomic_store_n(&s->stop,1,__ATOMIC_RELEASE);
                    }
                    s->wait_ticks=1;break;
                }
            }
            NTSTATUS status=intel_gpu_jobs_take(s->jobs,&s->lease);
            if (status!=0) { s->wait_ticks=jobs.queued ? 1 : 0; break; }
        }
        if (!s->native_pending) {
            if (intel_gpu_jobs_cancelled(&s->lease)) {
                if (gpu_executor_retire(s,STATUS_CANCELLED)!=0) { s->wait_ticks=1; break; }
                continue;
            }
            const intel_bcs_context_t *context=NULL;
            enum intel_bcs_status mapped=s->bind(s->context,s->lease.capture,&context);
            if (__atomic_load_n(&s->stop,__ATOMIC_ACQUIRE)) { s->wait_ticks=1; break; }
            if (mapped==INTEL_BCS_BUSY) { s->wait_ticks=1; break; }
            if (mapped!=INTEL_BCS_OK || !context || context->queue!=s->queue) {
                if (mapped==INTEL_BCS_QUARANTINED) {
                    gpu_executor_fault(s,mapped);
                    __atomic_store_n(&s->stop,1,__ATOMIC_RELEASE); s->wait_ticks=1; break;
                }
                if (gpu_executor_retire(s,STATUS_NOT_SUPPORTED)!=0) { s->wait_ticks=1; break; }
                continue;
            }
            NTSTATUS status=intel_gpu_jobs_enqueue(&s->lease,context);
            if (status!=0) {
                NTSTATUS polled=intel_gpu_jobs_poll(&s->lease);
                if (polled==0) continue;
                if (polled==STATUS_INVALID_PARAMETER) {
                    if (status==D3DKMT_STATUS_DEVICE_BUSY) { s->wait_ticks=1; break; }
                    if (gpu_executor_retire(s,status)!=0) { s->wait_ticks=1; break; }
                    continue;
                }
                /* A failed finish-stage may still own a native reservation. */
            }
            s->native_pending=true;
        }
        NTSTATUS polled=intel_gpu_jobs_poll(&s->lease);
        if (polled==0) { s->native_pending=false; continue; }
        if (polled!=D3DKMT_STATUS_DEVICE_BUSY) {
            gpu_executor_fault(s,INTEL_BCS_INVALID);
            __atomic_store_n(&s->stop,1,__ATOMIC_RELEASE); s->wait_ticks=1; break;
        }
        enum intel_bcs_status status=intel_bcs_queue_step(s->queue);
        if (status==INTEL_BCS_PENDING) { s->wait_ticks=s->interrupt_driven ? 50 : 1; break; }
        if (status==INTEL_BCS_BUSY) { s->wait_ticks=1; break; }
        if (status!=INTEL_BCS_OK) {
            gpu_executor_fault(s,status);
            __atomic_store_n(&s->stop,1,__ATOMIC_RELEASE); s->wait_ticks=1; break;
        }
        if (intel_gpu_jobs_poll(&s->lease)!=0) { s->wait_ticks=1; break; }
        s->native_pending=false;
        s->wait_ticks=1; /* bounded batch, retained queue hints usually wake earlier */
    }
    /* A consumed notification can cover more than this bounded batch.
     * Fast completion, cancellation and failed binding can all continue past
     * the normal polling path; retain a retry before parking the worker. */
    if (budget==4) s->wait_ticks=1;
    __atomic_store_n(&s->done,1,__ATOMIC_RELEASE);
    sched_signal_set(&s->joined,1);
}
static void gpu_executor_run(gpu_executor_owner_t *s)
{
    sched_signal_take(&s->joined);
    __atomic_store_n(&s->done,0,__ATOMIC_RELEASE);
    if (ws_cpu_count<=1) { gpu_executor_operation(s,NULL); return; }
    int task=smp_submit_any(gpu_executor_operation,s,NULL);
    if (task<0) { s->wait_ticks=1; return; }
    while (!__atomic_load_n(&s->done,__ATOMIC_ACQUIRE)) {
        sched_signal_take(&s->joined);
        if (__atomic_load_n(&s->done,__ATOMIC_ACQUIRE)) break;
        if (sched_signal_wait(&s->joined,100)!=SCHED_SIGNAL_READY) sched_yield();
    }
    smp_wait(task); /* timeouts never replace or abandon this operation */
}
static void gpu_executor_worker(void)
{
    for (;;) {
        sched_signal_take(&gpu_executor_signal);
        unsigned wait=0;
        for (unsigned i=0;i<INTEL_GPU_EXECUTOR_OWNERS;i++) {
            uint64_t flags=gpu_executor_lock();
            gpu_executor_owner_t *s=&gpu_executor_owners[i];
            bool run=s->ready && !s->busy && !s->retired;
            if (run) s->busy=true;
            gpu_executor_unlock(flags);
            if (!run) continue;
            gpu_executor_run(s);
            flags=gpu_executor_lock();
            s->busy=false;
            if (s->wait_ticks && (!wait || s->wait_ticks<wait)) wait=s->wait_ticks;
            gpu_executor_unlock(flags);
        }
        __atomic_store_n(&gpu_executor_retry,wait,__ATOMIC_RELEASE);
        if (sched_signal_wait(&gpu_executor_signal,wait)!=SCHED_SIGNAL_READY) sched_yield();
    }
}
NTSTATUS intel_gpu_executor_start(void)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
#ifdef __KERNEL_X86__
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!(flags&(1ULL<<9))) return STATUS_NOT_SUPPORTED;
#endif
    if (__atomic_load_n(&gpu_executor_state,__ATOMIC_ACQUIRE)==2) return STATUS_SUCCESS;
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&gpu_executor_state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return D3DKMT_STATUS_DEVICE_BUSY;
    if (gpu_executor_pid<=0) gpu_executor_pid=sched_spawn_detached_in_address_space_blocked(
        "intel-gpu-exec",gpu_executor_worker,paging_get_kernel_cr3(),0);
    bool ready=gpu_executor_pid>0 && proc_wake_pid(gpu_executor_pid)==0;
    __atomic_store_n(&gpu_executor_state,ready ? 2 : 0,__ATOMIC_RELEASE);
    return ready ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
NTSTATUS intel_gpu_executor_attach(intel_gpu_jobs_t *jobs,intel_bcs_queue_t *queue,
    intel_gpu_executor_bind_t bind,void *context,bool interrupt_driven,uint64_t *ticket)
{
    return intel_gpu_executor_attach_managed(jobs,queue,bind,NULL,context,interrupt_driven,ticket);
}
NTSTATUS intel_gpu_executor_attach_managed(intel_gpu_jobs_t *jobs,intel_bcs_queue_t *queue,
    intel_gpu_executor_bind_t bind,intel_gpu_executor_maintain_t maintain,void *context,bool interrupt_driven,uint64_t *ticket)
{
    intel_gpu_jobs_snapshot_t work; intel_bcs_queue_snapshot_t native;
    if (!jobs || jobs->ready!=intel_gpu_executor_notify || !bind || !ticket ||
        intel_gpu_jobs_snapshot(jobs,&work)!=0 || work.closing || work.claimed ||
        intel_bcs_queue_snapshot(queue,&native)!=INTEL_BCS_OK || !queue->registered ||
        native.closing || native.fault || native.queued || native.staged || native.active || native.terminal || native.worker_busy)
        return STATUS_INVALID_PARAMETER;
    NTSTATUS status=intel_gpu_executor_start();
    if (status!=0) return status;
    uint64_t flags=gpu_executor_lock();
    gpu_executor_owner_t *s=NULL;
    for (unsigned i=0;i<INTEL_GPU_EXECUTOR_OWNERS;i++) {
        if (gpu_executor_owners[i].jobs==jobs || gpu_executor_owners[i].queue==queue) {
            gpu_executor_unlock(flags); return STATUS_INVALID_PARAMETER;
        }
        if (!gpu_executor_owners[i].ticket && !s) s=&gpu_executor_owners[i];
    }
    if (!s || gpu_executor_next==UINT64_MAX) { gpu_executor_unlock(flags); return STATUS_INSUFFICIENT_RESOURCES; }
    *s=(gpu_executor_owner_t){.jobs=jobs,.queue=queue,.bind=bind,.maintain=maintain,.context=context,
        .interrupt_driven=interrupt_driven,.ticket=gpu_executor_next++,.busy=true};
    gpu_executor_unlock(flags);
    enum intel_bcs_status watched=intel_bcs_queue_watch(queue,gpu_executor_queue_wake,NULL);
    flags=gpu_executor_lock();
    if (watched==INTEL_BCS_OK) {
        intel_gpu_jobs_set_execution(jobs,OKMT_EXECUTION_ACTIVE);
        s->busy=false; s->ready=true; *ticket=s->ticket;
    }
    else *s=(gpu_executor_owner_t){0};
    gpu_executor_unlock(flags);
    intel_gpu_executor_notify(NULL);
    return watched==INTEL_BCS_OK ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
NTSTATUS intel_gpu_executor_stop(uint64_t ticket)
{
    uint64_t flags=gpu_executor_lock();
    for (unsigned i=0;ticket && i<INTEL_GPU_EXECUTOR_OWNERS;i++) {
        gpu_executor_owner_t *s=&gpu_executor_owners[i];
        if (s->ticket!=ticket || !s->ready) continue;
        __atomic_store_n(&s->stop,1,__ATOMIC_RELEASE); __atomic_store_n(&s->retry,1,__ATOMIC_RELEASE);
        intel_gpu_jobs_set_execution(s->jobs,OKMT_EXECUTION_STOPPED);
        gpu_executor_unlock(flags); intel_gpu_executor_notify(NULL); return STATUS_SUCCESS;
    }
    gpu_executor_unlock(flags); return STATUS_INVALID_PARAMETER;
}
NTSTATUS intel_gpu_executor_detach(uint64_t ticket)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    uint64_t flags=gpu_executor_lock();
    for (unsigned i=0;ticket && i<INTEL_GPU_EXECUTOR_OWNERS;i++) {
        gpu_executor_owner_t *s=&gpu_executor_owners[i];
        if (s->ticket!=ticket || !s->ready) continue;
        if (s->busy || !s->retired || s->lease.self || !intel_gpu_jobs_empty(s->jobs)) {
            gpu_executor_unlock(flags); return D3DKMT_STATUS_DEVICE_BUSY;
        }
        *s=(gpu_executor_owner_t){0};
        gpu_executor_unlock(flags); return STATUS_SUCCESS;
    }
    gpu_executor_unlock(flags); return STATUS_INVALID_PARAMETER;
}
bool intel_gpu_executor_idle(void)
{
    return __atomic_load_n(&gpu_executor_state,__ATOMIC_ACQUIRE)==2 &&
        __atomic_load_n(&gpu_executor_signal.waiters,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gpu_executor_signal.bits,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gpu_executor_retry,__ATOMIC_ACQUIRE);
}
bool intel_gpu_executor_quiescent(void)
{
    uint64_t flags=gpu_executor_lock(); bool empty=true;
    for (unsigned i=0;i<INTEL_GPU_EXECUTOR_OWNERS;i++) if (gpu_executor_owners[i].ticket) empty=false;
    gpu_executor_unlock(flags); return empty;
}

