#include "intel_gpu_jobs.h"

static uint64_t gpu_jobs_lock(intel_gpu_jobs_t *m)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&m->lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void gpu_jobs_unlock(intel_gpu_jobs_t *m,uint64_t flags)
{
    __atomic_store_n(&m->lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
/* Enter locked, return unlocked. Callback retention closes the gap between
 * removing the last job and completing the callback tail. */
static void gpu_jobs_notify(intel_gpu_jobs_t *m,uint64_t flags,bool ready,bool done,
    uint64_t id,DWORD pid,NTSTATUS status)
{
    m->callbacks++;
    gpu_jobs_unlock(m,flags);
    if (ready) m->ready(m->callback_context);
    if (done) m->done(m->callback_context,id,pid,status);
    m->contexts->wake(m->contexts->wake_context);
    flags=gpu_jobs_lock(m); m->callbacks--; gpu_jobs_unlock(m,flags);
}
static NTSTATUS gpu_jobs_validate(intel_gpu_job_t *job,unsigned expected,unsigned next)
{
    if (job->sync) {
        if (expected==next) return d3dkmt_sync_admit(&job->synchronization,NULL,FALSE);
        if (expected!=D3DKMT_WORK_PREPARED || next!=D3DKMT_WORK_QUEUED) return STATUS_INVALID_PARAMETER;
        return d3dkmt_sync_admit(&job->synchronization,&job->publication,TRUE);
    }
    const D3DKMT_ALLOCATION_LEASE *resources[INTEL_UMD_MAX_RESOURCES];
    unsigned count=job->capture.resource_count;
    if (!job->capture.valid || count<2 || count>INTEL_UMD_MAX_RESOURCES) return STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<count;i++) resources[i]=&job->capture.resources[i].reference;
    if (expected==next) return d3dkmt_context_work_status(&job->capture.context.reference,resources,count);
    return d3dkmt_admit_context_work(&job->capture.context.reference,resources,count,&job->publication,expected,next);
}
bool intel_gpu_jobs_init(intel_gpu_jobs_t *m,intel_gpu_contexts_t *contexts,
    void (*ready)(void *),void (*done)(void *,uint64_t,DWORD,NTSTATUS),void *opaque)
{
    if (!m || m->self || !contexts || contexts->self!=contexts || contexts->closing || !ready || !done) return false;
    m->self=m; m->contexts=contexts; m->ready=ready; m->done=done; m->callback_context=opaque; m->next_id=1;
    m->execution=OKMT_EXECUTION_STOPPED;
    return true;
}
void intel_gpu_jobs_set_execution(intel_gpu_jobs_t *m,DWORD state)
{
    if (!m || m->self!=m || (state!=OKMT_EXECUTION_ACTIVE && state!=OKMT_EXECUTION_STOPPED &&
        state!=OKMT_EXECUTION_HUNG && state!=OKMT_EXECUTION_DMA_FAULT)) return;
    uint64_t flags=gpu_jobs_lock(m);
    DWORD previous=__atomic_load_n(&m->execution,__ATOMIC_RELAXED);
    if (!__atomic_load_n(&m->fault,__ATOMIC_RELAXED) &&
        (previous==OKMT_EXECUTION_ACTIVE || previous==OKMT_EXECUTION_STOPPED) &&
        !(state==OKMT_EXECUTION_ACTIVE && m->closing))
        __atomic_store_n(&m->execution,state,__ATOMIC_RELEASE);
    gpu_jobs_unlock(m,flags);
}
void intel_gpu_jobs_fail(intel_gpu_jobs_t *m,DWORD state)
{
    if (!m || m->self!=m || (state!=OKMT_EXECUTION_HUNG && state!=OKMT_EXECUTION_DMA_FAULT &&
        state!=OKMT_EXECUTION_STOPPED)) return;
    uint64_t flags=gpu_jobs_lock(m);
    if (__atomic_load_n(&m->fault,__ATOMIC_RELAXED)) { gpu_jobs_unlock(m,flags);return; }
    DWORD previous=__atomic_load_n(&m->execution,__ATOMIC_RELAXED);
    if (previous==OKMT_EXECUTION_HUNG || previous==OKMT_EXECUTION_DMA_FAULT) state=previous;
    __atomic_store_n(&m->execution,state,__ATOMIC_RELEASE);
    __atomic_store_n(&m->fault,state,__ATOMIC_RELEASE);
    m->callbacks++;
    gpu_jobs_unlock(m,flags);
    if (m->faulted) m->faulted(m->fault_context);
    intel_gpu_jobs_shutdown(m);
    flags=gpu_jobs_lock(m);m->callbacks--;gpu_jobs_unlock(m,flags);
}
NTSTATUS intel_gpu_jobs_execution(intel_gpu_jobs_t *m,DWORD *out)
{
    if (!m || m->self!=m || !out) return STATUS_INVALID_PARAMETER;
    *out=__atomic_load_n(&m->execution,__ATOMIC_ACQUIRE);
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_jobs_submit(intel_gpu_jobs_t *m,DWORD pid,OKMT_HANDLE context,DWORD offset,DWORD bytes,
    DWORD allocations,DWORD patches,uint64_t *ticket)
{
    if (!m || m->self!=m || !ticket) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_jobs_lock(m);
    intel_gpu_job_t *job=NULL;
    if (!m->closing && m->next_id!=UINT64_MAX)
        for (unsigned i=0;i<INTEL_GPU_JOBS;i++) if (!m->jobs[i].id) { job=&m->jobs[i]; break; }
    if (!job) {
        NTSTATUS status=m->closing ? D3DKMT_STATUS_DEVICE_REMOVED : D3DKMT_STATUS_DEVICE_BUSY;
        gpu_jobs_unlock(m,flags);
        return status;
    }
    *job=(intel_gpu_job_t){.id=m->next_id++,.pid=pid,.context=context,.phase=INTEL_GPU_JOB_PREPARING,.busy=true};
    gpu_jobs_unlock(m,flags);
    NTSTATUS status=intel_gpu_capture_begin(pid,context,offset,bytes,allocations,patches,&job->capture);
    if (status==STATUS_SUCCESS && job->capture.context.native.owner!=m->contexts) status=STATUS_INVALID_PARAMETER;
    /* Residency alone protects storage, not ordering against a later CPU lock.
     * Reserve every referenced allocation before publication, including work
     * behind an unsignaled queue wait. Partial reservations stay capture-owned
     * on failure and are released by the persistent retirement collector. */
    for (unsigned i=0;status==STATUS_SUCCESS && i<job->capture.resource_count;i++) {
        enum intel_gpu_buffer_status reserved=intel_gpu_memory_reserve_work(&job->capture.resources[i].native);
        if (reserved!=INTEL_GPU_BUFFER_OK) status=reserved==INTEL_GPU_BUFFER_BUSY ?
            D3DKMT_STATUS_DEVICE_BUSY : reserved==INTEL_GPU_BUFFER_NOT_RESIDENT ?
            D3DKMT_STATUS_DEVICE_ERROR : STATUS_INVALID_PARAMETER;
        if (reserved==INTEL_GPU_BUFFER_NOT_RESIDENT)
            (void)d3dkmt_mark_device_error(pid,job->capture.context.reference.device,6);
    }
    if (status==STATUS_SUCCESS) status=gpu_jobs_validate(job,D3DKMT_WORK_PREPARED,D3DKMT_WORK_QUEUED);
    flags=gpu_jobs_lock(m);
    if (status==STATUS_SUCCESS && (m->closing || job->cancel)) status=STATUS_CANCELLED;
    job->busy=false; job->result=status;
    if (status==STATUS_SUCCESS) {
        job->phase=INTEL_GPU_JOB_QUEUED; job->report=true; *ticket=job->id;
    } else job->phase=INTEL_GPU_JOB_RETIRING;
    gpu_jobs_notify(m,flags,status==STATUS_SUCCESS,false,0,0,0);
    return status;
}
NTSTATUS intel_gpu_jobs_submit_sync(intel_gpu_jobs_t *m,DWORD pid,OKMT_HANDLE context,
    const D3DKMT_SYNC_COMMAND *command,uint64_t *ticket)
{
    if (!m || m->self!=m || !command || !ticket || command->broadcast_count>OKMT_MAX_BROADCAST_CONTEXTS ||
        (command->broadcast_count && command->broadcast[0]!=context)) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_jobs_lock(m);
    intel_gpu_job_t *job=NULL;
    if (!m->closing && m->next_id!=UINT64_MAX)
        for (unsigned i=0;i<INTEL_GPU_JOBS;i++) if (!m->jobs[i].id) { job=&m->jobs[i]; break; }
    if (!job) {
        NTSTATUS status=m->closing ? D3DKMT_STATUS_DEVICE_REMOVED : D3DKMT_STATUS_DEVICE_BUSY;
        gpu_jobs_unlock(m,flags); return status;
    }
    *job=(intel_gpu_job_t){.id=m->next_id++,.pid=pid,.context=context,.phase=INTEL_GPU_JOB_PREPARING,.busy=true,.sync=true};
    job->context_count=command->broadcast_count;
    for (DWORD i=0;i<job->context_count;i++) {
        DWORD slot=i;OKMT_HANDLE handle=command->broadcast[i];
        while (slot && job->contexts[slot-1]>handle) {job->contexts[slot]=job->contexts[slot-1];slot--;}
        job->contexts[slot]=handle;
    }
    gpu_jobs_unlock(m,flags);
    job->capture.self=&job->capture;
    NTSTATUS status=intel_gpu_kmt_acquire_context(pid,context,&job->capture.context);
    if (status==STATUS_SUCCESS && job->capture.context.native.owner!=m->contexts) status=STATUS_INVALID_PARAMETER;
    if (status==STATUS_SUCCESS) status=d3dkmt_sync_capture(pid,context,command,&job->synchronization);
    if (status==STATUS_SUCCESS) status=gpu_jobs_validate(job,D3DKMT_WORK_PREPARED,D3DKMT_WORK_QUEUED);
    flags=gpu_jobs_lock(m);
    if (status==STATUS_SUCCESS && (m->closing || job->cancel)) status=STATUS_CANCELLED;
    job->busy=false; job->result=status;
    if (status==STATUS_SUCCESS) {
        job->phase=INTEL_GPU_JOB_QUEUED; job->report=true; *ticket=job->id;
    } else job->phase=INTEL_GPU_JOB_RETIRING;
    gpu_jobs_notify(m,flags,status==STATUS_SUCCESS,false,0,0,0);
    return status;
}
static bool gpu_jobs_contains(const intel_gpu_job_t *job,OKMT_HANDLE context)
{
    if (!job->context_count) return job->context==context;
    for (DWORD i=0;i<job->context_count;i++) if (job->contexts[i]==context) return true;
    return false;
}
static bool gpu_jobs_overlap(const intel_gpu_job_t *a,const intel_gpu_job_t *b)
{
    if (!a->context_count) return gpu_jobs_contains(b,a->context);
    if (!b->context_count) return gpu_jobs_contains(a,b->context);
    DWORD i=0,j=0;
    while (i<a->context_count && j<b->context_count) {
        if (a->contexts[i]==b->contexts[j]) return true;
        if (a->contexts[i]<b->contexts[j]) i++;else j++;
    }
    return false;
}
static bool gpu_jobs_head(const intel_gpu_jobs_t *m,const intel_gpu_job_t *candidate)
{
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        const intel_gpu_job_t *prior=&m->jobs[i];
        if (prior->id && prior->id<candidate->id && prior->pid==candidate->pid && gpu_jobs_overlap(prior,candidate) &&
            (prior->phase==INTEL_GPU_JOB_PREPARING || prior->phase==INTEL_GPU_JOB_QUEUED || prior->phase==INTEL_GPU_JOB_CLAIMED))
            return false;
    }
    return true;
}
static LONG gpu_jobs_priority(const intel_gpu_job_t *job)
{
    const D3DKMT_CONTEXT_LEASE *context=job->sync ? &job->synchronization.context : &job->capture.context.reference;
    LONG priority=d3dkmt_context_priority_snapshot(context);
    /* A broadcast joins several streams; use their highest current priority
     * without relaxing the FIFO dependency on any participating context. */
    if (job->sync) for (DWORD i=0;i<job->synchronization.extra_count;i++) {
        LONG value=d3dkmt_context_priority_snapshot(&job->synchronization.extra[i]);
        if (value>priority) priority=value;
    }
    /* Once bypassed 15 times, promote every aged head to the same FIFO tier.
     * Merely capping a numeric bonus can starve a low-priority stream while
     * several high-priority producers continuously replenish their queues. */
    return job->age>=15 ? 32 : priority+(LONG)job->age;
}
static void gpu_jobs_age_heads(intel_gpu_jobs_t *m,const intel_gpu_job_t *selected)
{
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *job=&m->jobs[i];
        if (job!=selected && job->phase==INTEL_GPU_JOB_QUEUED && !job->busy &&
            job->age<15 && gpu_jobs_head(m,job)) job->age++;
    }
}
NTSTATUS intel_gpu_jobs_take(intel_gpu_jobs_t *m,intel_gpu_job_lease_t *lease)
{
    if (!m || m->self!=m || !lease || lease->self) return STATUS_INVALID_PARAMETER;
    unsigned skipped=0;
    for (unsigned attempt=0;attempt<INTEL_GPU_JOBS;attempt++) {
        uint64_t flags=gpu_jobs_lock(m);
        intel_gpu_job_t *job=NULL;LONG best=-100;
        if (!m->closing) for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
            intel_gpu_job_t *candidate=&m->jobs[i];
            if (!(skipped&(1u<<i)) && candidate->phase==INTEL_GPU_JOB_QUEUED && !candidate->busy &&
                gpu_jobs_head(m,candidate)) {
                LONG score=gpu_jobs_priority(candidate);
                if (!job || score>best || (score==best && candidate->id<job->id)) {job=candidate;best=score;}
            }
        }
        if (!job) { gpu_jobs_unlock(m,flags); return D3DKMT_STATUS_DEVICE_BUSY; }
        job->busy=true;
        gpu_jobs_unlock(m,flags);
        if (job->sync) {
            NTSTATUS status=d3dkmt_sync_step(&job->synchronization,&job->publication);
            flags=gpu_jobs_lock(m); job->busy=false;
            if (status==D3DKMT_STATUS_DEVICE_BUSY && !m->closing && !job->cancel) {
                skipped|=1u<<(unsigned)(job-m->jobs); gpu_jobs_unlock(m,flags); continue;
            }
            /* SUBMITTED wins a concurrent cancellation; never undo a semaphore
             * decrement or report cancellation for an already committed signal. */
            if (status!=STATUS_SUCCESS && (m->closing || job->cancel)) status=STATUS_CANCELLED;
            if (status==STATUS_SUCCESS) gpu_jobs_age_heads(m,job);
            job->phase=INTEL_GPU_JOB_RETIRING; job->result=status;
            gpu_jobs_notify(m,flags,true,false,0,0,0);
            if (status!=STATUS_SUCCESS) return status;
            skipped=0; continue;
        }
        NTSTATUS status=gpu_jobs_validate(job,D3DKMT_WORK_QUEUED,D3DKMT_WORK_CLAIMED);
        flags=gpu_jobs_lock(m);
        if (status==STATUS_SUCCESS && (m->closing || job->cancel)) status=STATUS_CANCELLED;
        job->busy=false;
        if (status==STATUS_SUCCESS) {
            gpu_jobs_age_heads(m,job);
            job->phase=INTEL_GPU_JOB_CLAIMED;
            *lease=(intel_gpu_job_lease_t){lease,m,&job->capture,job->id,job->pid};
            gpu_jobs_unlock(m,flags);
        } else {
            job->phase=INTEL_GPU_JOB_RETIRING; job->result=status;
            gpu_jobs_notify(m,flags,false,false,0,0,0);
        }
        return status;
    }
    return D3DKMT_STATUS_DEVICE_BUSY;
}
DWORD intel_gpu_jobs_pending(intel_gpu_jobs_t *m,DWORD pid,OKMT_HANDLE context)
{
    if (!m || m->self!=m || !pid || !context) return 0;
    uint64_t flags=gpu_jobs_lock(m); DWORD count=0;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        const intel_gpu_job_t *job=&m->jobs[i];
        /* Preparation/retirement mutate capture outside this lock. Only
         * admitted queued/claimed work has a stable retained context here. */
        if (job->pid==pid && job->report &&
            (job->phase==INTEL_GPU_JOB_QUEUED || job->phase==INTEL_GPU_JOB_CLAIMED) &&
            gpu_jobs_contains(job,context)) count++;
    }
    gpu_jobs_unlock(m,flags); return count;
}
NTSTATUS intel_gpu_jobs_retire(intel_gpu_job_lease_t *lease,NTSTATUS result)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner) return STATUS_INVALID_PARAMETER;
    intel_gpu_jobs_t *m=lease->owner;
    uint64_t flags=gpu_jobs_lock(m);
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *job=&m->jobs[i];
        if (job->id!=lease->id || job->pid!=lease->pid || &job->capture!=lease->capture ||
            job->phase!=INTEL_GPU_JOB_CLAIMED || job->busy) continue;
        if (job->queue) { gpu_jobs_unlock(m,flags); return D3DKMT_STATUS_DEVICE_BUSY; }
        job->result=result; job->phase=INTEL_GPU_JOB_RETIRING; *lease=(intel_gpu_job_lease_t){0};
        gpu_jobs_notify(m,flags,false,false,0,0,0); return STATUS_SUCCESS;
    }
    gpu_jobs_unlock(m,flags); return STATUS_INVALID_PARAMETER;
}
NTSTATUS intel_gpu_jobs_cancel(intel_gpu_jobs_t *m,DWORD pid,uint64_t id)
{
    if (!m || m->self!=m || !pid || !id) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_jobs_lock(m);
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *job=&m->jobs[i];
        if (job->id!=id || job->pid!=pid || !job->report) continue;
        job->cancel=true;
        unsigned expected=D3DKMT_WORK_QUEUED;
        if (!__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            expected=D3DKMT_WORK_CLAIMED;
            (void)__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        }
        NTSTATUS status=D3DKMT_STATUS_DEVICE_BUSY;
        if (job->phase==INTEL_GPU_JOB_QUEUED && !job->busy) {
            job->phase=INTEL_GPU_JOB_RETIRING; job->result=STATUS_CANCELLED; status=STATUS_SUCCESS;
        }
        gpu_jobs_notify(m,flags,true,false,0,0,0); return status;
    }
    gpu_jobs_unlock(m,flags); return STATUS_INVALID_PARAMETER;
}
void intel_gpu_jobs_device_error(intel_gpu_jobs_t *m,DWORD pid,uint64_t device)
{
    if (!m || m->self!=m || !pid || !device) return;
    uint64_t flags=gpu_jobs_lock(m);
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *job=&m->jobs[i];
        if (job->pid!=pid || (job->phase!=INTEL_GPU_JOB_QUEUED && job->phase!=INTEL_GPU_JOB_CLAIMED)) continue;
        const D3DKMT_CONTEXT_LEASE *context=job->sync ? &job->synchronization.context : &job->capture.context.reference;
        if (context->device_cookie!=device) continue;
        job->cancel=true;
        unsigned expected=D3DKMT_WORK_QUEUED;
        if (!__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            expected=D3DKMT_WORK_CLAIMED;
            (void)__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        }
        if (job->phase==INTEL_GPU_JOB_QUEUED && !job->busy) {
            job->phase=INTEL_GPU_JOB_RETIRING;job->result=STATUS_CANCELLED;
        }
    }
    gpu_jobs_notify(m,flags,true,false,0,0,0);
}
BOOL intel_gpu_jobs_cancelled(const intel_gpu_job_lease_t *lease)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner) return TRUE;
    intel_gpu_jobs_t *m=lease->owner;
    uint64_t flags=gpu_jobs_lock(m);
    BOOL cancelled=TRUE;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *job=&m->jobs[i];
        if (job->id==lease->id && job->pid==lease->pid && &job->capture==lease->capture &&
            job->phase==INTEL_GPU_JOB_CLAIMED) cancelled=m->closing || job->cancel;
    }
    gpu_jobs_unlock(m,flags); return cancelled;
}
NTSTATUS intel_gpu_jobs_collect(intel_gpu_jobs_t *m)
{
    if (!m || m->self!=m) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_jobs_lock(m);
    intel_gpu_job_t *job=NULL;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        unsigned slot=(m->cursor+i)%INTEL_GPU_JOBS;
        intel_gpu_job_t *candidate=&m->jobs[slot];
        if (!candidate->busy && !candidate->quarantined && (candidate->phase==INTEL_GPU_JOB_RETIRING ||
            candidate->phase==INTEL_GPU_JOB_QUEUED)) {
            job=candidate; job->busy=true; m->cursor=(slot+1)%INTEL_GPU_JOBS; break;
        }
    }
    if (!job) { gpu_jobs_unlock(m,flags); return STATUS_SUCCESS; }
    bool queued=job->phase==INTEL_GPU_JOB_QUEUED;
    gpu_jobs_unlock(m,flags);
    if (queued) {
        NTSTATUS status=gpu_jobs_validate(job,D3DKMT_WORK_QUEUED,D3DKMT_WORK_QUEUED);
        flags=gpu_jobs_lock(m);
        if (status==STATUS_SUCCESS && !m->closing && !job->cancel) {
            job->busy=false; gpu_jobs_unlock(m,flags); return STATUS_SUCCESS;
        }
        job->phase=INTEL_GPU_JOB_RETIRING; job->result=status==STATUS_SUCCESS ? STATUS_CANCELLED : status;
        gpu_jobs_unlock(m,flags);
    }
    NTSTATUS status=job->sync ? d3dkmt_sync_release(&job->synchronization) : STATUS_SUCCESS;
    if (status==STATUS_SUCCESS) status=intel_gpu_capture_release(&job->capture);
    flags=gpu_jobs_lock(m);
    if (status!=STATUS_SUCCESS) {
        job->busy=false; job->quarantined=status!=D3DKMT_STATUS_DEVICE_BUSY;
        gpu_jobs_unlock(m,flags); return status;
    }
    bool report=job->report; uint64_t id=job->id; DWORD pid=job->pid; NTSTATUS result=job->result;
    *job=(intel_gpu_job_t){0};
    gpu_jobs_notify(m,flags,false,report,id,pid,result);
    return STATUS_SUCCESS;
}
bool intel_gpu_jobs_need_retry(intel_gpu_jobs_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=gpu_jobs_lock(m); bool retry=m->callbacks!=0;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++)
        if (m->jobs[i].phase==INTEL_GPU_JOB_RETIRING && !m->jobs[i].busy && !m->jobs[i].quarantined) retry=true;
    gpu_jobs_unlock(m,flags); return retry;
}
void intel_gpu_jobs_shutdown(intel_gpu_jobs_t *m)
{
    if (!m || m->self!=m) return;
    uint64_t flags=gpu_jobs_lock(m); m->closing=true;
    DWORD state=__atomic_load_n(&m->execution,__ATOMIC_RELAXED);
    if (state==OKMT_EXECUTION_ACTIVE)
        __atomic_store_n(&m->execution,OKMT_EXECUTION_STOPPED,__ATOMIC_RELEASE);
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *job=&m->jobs[i];
        if (!job->id) continue;
        job->cancel=true;
        unsigned expected=job->phase==INTEL_GPU_JOB_PREPARING ? D3DKMT_WORK_PREPARED : D3DKMT_WORK_QUEUED;
        if (!__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            expected=D3DKMT_WORK_CLAIMED;
            (void)__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        }
        if (!job->busy && job->phase==INTEL_GPU_JOB_QUEUED) {
            job->phase=INTEL_GPU_JOB_RETIRING; job->result=STATUS_CANCELLED;
        }
    }
    gpu_jobs_notify(m,flags,true,false,0,0,0);
}
bool intel_gpu_jobs_empty(intel_gpu_jobs_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=gpu_jobs_lock(m); bool empty=m->closing && !m->callbacks;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) if (m->jobs[i].id) empty=false;
    gpu_jobs_unlock(m,flags); return empty;
}

NTSTATUS intel_gpu_jobs_snapshot(intel_gpu_jobs_t *m,intel_gpu_jobs_snapshot_t *out)
{
    if (!m || m->self!=m || !out) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_jobs_lock(m);
    *out=(intel_gpu_jobs_snapshot_t){.closing=m->closing};
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        out->queued+=m->jobs[i].phase==INTEL_GPU_JOB_QUEUED;
        out->claimed+=m->jobs[i].phase==INTEL_GPU_JOB_CLAIMED;
        out->preparing+=m->jobs[i].phase==INTEL_GPU_JOB_PREPARING;
    }
    gpu_jobs_unlock(m,flags); return STATUS_SUCCESS;
}

NTSTATUS intel_gpu_jobs_enqueue(intel_gpu_job_lease_t *lease,const intel_bcs_context_t *context)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner ||
        !context || !context->queue || !context->queue_id) return STATUS_INVALID_PARAMETER;
    intel_gpu_jobs_t *m=lease->owner;
    uint64_t flags=gpu_jobs_lock(m);
    intel_gpu_job_t *job=NULL;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *candidate=&m->jobs[i];
        if (candidate->id==lease->id && candidate->pid==lease->pid && &candidate->capture==lease->capture &&
            candidate->phase==INTEL_GPU_JOB_CLAIMED) { job=candidate; break; }
    }
    if (!job) { gpu_jobs_unlock(m,flags); return STATUS_INVALID_PARAMETER; }
    if (job->busy || job->queue) { gpu_jobs_unlock(m,flags); return D3DKMT_STATUS_DEVICE_BUSY; }
    if (job->cancel || m->closing) { gpu_jobs_unlock(m,flags); return STATUS_CANCELLED; }
    job->busy=true;
    gpu_jobs_unlock(m,flags);
    intel_bcs_copy_t copy;
    intel_rcs_queue_draw_t draw;
    bool primitive=job->capture.command.opcode==INTEL_UMD_DRAW32;
    NTSTATUS status=primitive ? intel_gpu_capture_draw(&job->capture,context,&draw) :
        intel_gpu_capture_copy(&job->capture,context,&copy);
    if (status==STATUS_SUCCESS) {
        intel_bcs_queue_gate_t gate={&job->publication,D3DKMT_WORK_CLAIMED,
            D3DKMT_WORK_SUBMITTED,D3DKMT_WORK_CANCELLED};
        enum intel_bcs_status staged=primitive ? intel_bcs_queue_stage_draw(context->queue,job->pid,context->queue_id,&draw,&gate,&job->fence) :
            intel_bcs_queue_stage(context->queue,job->pid,context->queue_id,&copy,&gate,&job->fence);
        if (staged!=INTEL_BCS_OK) status=staged==INTEL_BCS_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
        else {
            job->queue=context->queue; job->staged=true;
            status=gpu_jobs_validate(job,D3DKMT_WORK_CLAIMED,D3DKMT_WORK_SUBMITTED);
            if (status!=STATUS_SUCCESS) {
                unsigned expected=D3DKMT_WORK_CLAIMED;
                (void)__atomic_compare_exchange_n(&job->publication,&expected,D3DKMT_WORK_CANCELLED,false,
                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
            }
            enum intel_bcs_status finished=intel_bcs_queue_finish_stage(job->queue,job->pid,job->fence,&job->publication);
            if (finished==INTEL_BCS_OK) job->staged=false;
            else status=STATUS_UNSUCCESSFUL; /* retain reservation and word for poll */
        }
    }
    flags=gpu_jobs_lock(m); job->busy=false;
    gpu_jobs_unlock(m,flags);
    return status;
}
NTSTATUS intel_gpu_jobs_poll(intel_gpu_job_lease_t *lease)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner) return STATUS_INVALID_PARAMETER;
    intel_gpu_jobs_t *m=lease->owner;
    uint64_t flags=gpu_jobs_lock(m);
    intel_gpu_job_t *job=NULL;
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) {
        intel_gpu_job_t *candidate=&m->jobs[i];
        if (candidate->id==lease->id && candidate->pid==lease->pid && &candidate->capture==lease->capture &&
            candidate->phase==INTEL_GPU_JOB_CLAIMED) { job=candidate; break; }
    }
    if (!job) { gpu_jobs_unlock(m,flags); return STATUS_INVALID_PARAMETER; }
    if (job->busy) { gpu_jobs_unlock(m,flags); return D3DKMT_STATUS_DEVICE_BUSY; }
    if (!job->queue) { gpu_jobs_unlock(m,flags); return STATUS_INVALID_PARAMETER; }
    job->busy=true;
    bool cancel=m->closing || job->cancel;
    gpu_jobs_unlock(m,flags);
    enum intel_bcs_status status=INTEL_BCS_OK;
    if (job->staged) {
        status=intel_bcs_queue_finish_stage(job->queue,job->pid,job->fence,&job->publication);
        if (status==INTEL_BCS_OK) job->staged=false;
    }
    if (status==INTEL_BCS_OK && cancel) (void)intel_bcs_queue_cancel(job->queue,job->pid,job->fence);
    intel_bcs_job_result_t result;
    if (status==INTEL_BCS_OK) status=intel_bcs_queue_result(job->queue,job->pid,job->fence,true,&result);
    flags=gpu_jobs_lock(m); job->busy=false;
    if (status!=INTEL_BCS_OK) {
        gpu_jobs_unlock(m,flags);
        return status==INTEL_BCS_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    }
    job->queue=NULL; job->fence=0; job->phase=INTEL_GPU_JOB_RETIRING;
    job->result=result.phase==INTEL_BCS_JOB_COMPLETE ? STATUS_SUCCESS :
        result.phase==INTEL_BCS_JOB_FAILED ? STATUS_UNSUCCESSFUL : STATUS_CANCELLED;
    *lease=(intel_gpu_job_lease_t){0};
    gpu_jobs_notify(m,flags,false,false,0,0,0);
    return STATUS_SUCCESS;
}
