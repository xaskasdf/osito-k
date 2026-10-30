#include "intel_bcs_queue.h"
#ifndef __KERNEL_X86__
#include <string.h>
#endif

static unsigned queue_registry_lock;
static intel_bcs_queue_t *queue_registry[8];

static uint64_t bcs_queue_lock(unsigned *lock)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(lock,1,__ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}

static void bcs_queue_unlock(unsigned *lock, uint64_t flags)
{
    __atomic_store_n(lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags & (1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

typedef struct {
    intel_bcs_queue_wakeup_t call;
    void *context;
    uint32_t events;
} bcs_queue_notice_t;

/* Called under the queue lock. Pin the sink before unlocking; even an exit
 * notification collected under the registry lock must invoke it outside both. */
static bcs_queue_notice_t bcs_queue_notify_take(intel_bcs_queue_t *q)
{
    bcs_queue_notice_t n={0};
    if (q->wakeup && q->events) {
        n=(bcs_queue_notice_t){q->wakeup,q->wakeup_context,q->events};
        q->notifying++;
    }
    q->events=0;
    return n;
}

static void bcs_queue_notify_call(intel_bcs_queue_t *q, bcs_queue_notice_t n)
{
    if (!n.call) return;
    n.call(n.context,n.events);
    uint64_t flags=bcs_queue_lock(&q->lock);
    q->notifying--;
    bcs_queue_unlock(&q->lock,flags);
}

static enum intel_bcs_status bcs_queue_return(intel_bcs_queue_t *q,
    uint64_t flags, enum intel_bcs_status status)
{
    bcs_queue_notice_t n=bcs_queue_notify_take(q);
    bcs_queue_unlock(&q->lock,flags);
    bcs_queue_notify_call(q,n);
    return status;
}

#define QUEUE_RETURN(q, flags, value) return bcs_queue_return(q,flags,value)

static intel_bcs_queue_context_t *bcs_queue_context(intel_bcs_queue_t *q,
    uint64_t owner, uint64_t id)
{
    if (!q || !q->initialized || !owner || !id) return NULL;
    for (unsigned i=0; i<INTEL_BCS_QUEUE_CONTEXTS; i++)
        if (q->contexts[i].backing && q->contexts[i].owner==owner &&
            q->contexts[i].id==id) return &q->contexts[i];
    return NULL;
}

static intel_bcs_queue_job_t *bcs_queue_job(intel_bcs_queue_t *q,
    uint64_t owner, uint64_t fence)
{
    if (!q || !q->initialized || !owner || !fence) return NULL;
    for (unsigned i=0; i<INTEL_BCS_QUEUE_JOBS; i++)
        if (q->jobs[i].result.phase!=INTEL_BCS_JOB_EMPTY &&
            q->jobs[i].owner==owner && q->jobs[i].fence==fence) return &q->jobs[i];
    return NULL;
}

static void bcs_queue_finish(intel_bcs_queue_t *q, intel_bcs_queue_job_t *j,
    enum intel_bcs_job_phase phase, enum intel_bcs_status error)
{
    intel_bcs_queue_context_t *c=bcs_queue_context(q,j->owner,j->context);
    if (j->gate.state) {
        unsigned expected=j->gate.pending;
        (void)__atomic_compare_exchange_n(j->gate.state,&expected,j->gate.rejected,false,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
    /* Only called for queued/running work, whose context cannot be released. */
    c->references--;
    j->result.phase=phase; j->result.error=error;
    q->events|=INTEL_BCS_EVENT_RESULT;
}

static void bcs_queue_reap(intel_bcs_queue_t *q)
{
    /* An exited owner cannot consume results. Reclaim only terminal metadata;
     * active/preparing work and every DMA bundle keep their references. */
    for (unsigned i=0;i<INTEL_BCS_QUEUE_JOBS;i++) {
        intel_bcs_queue_job_t *j=&q->jobs[i];
        if (j->retained || j->result.phase<INTEL_BCS_JOB_COMPLETE) continue;
        intel_bcs_queue_context_t *c=bcs_queue_context(q,j->owner,j->context);
        if (c && c->process_exited) {
            memset(j,0,sizeof(*j));
            q->events|=INTEL_BCS_EVENT_SPACE;
        }
    }
}

static enum intel_bcs_status bcs_queue_init(intel_bcs_queue_t *q,
    intel_bcs_state_t *engine, const intel_gt_io_t *io, void *engine_context,
    const intel_bcs_queue_ops_t *ops,intel_rcs_queue_prepare_t draw,uint64_t first_fence)
{
    if (!q || q->initialized || !engine || (engine->engine!=INTEL_CS_COPY && engine->engine!=INTEL_CS_RENDER) || !engine->enabled || engine->retired ||
        engine->inflight || engine->submitted || engine->status!=INTEL_BCS_OK ||
        !io || !io->read || !io->write || !io->now_ns || !io->relax ||
        !ops || (!ops->prepare && !draw) || !ops->token_read || !first_fence ||
        (draw && (engine->engine!=INTEL_CS_RENDER || !ops->acquire || !ops->retire)) ||
        first_fence==UINT64_MAX || (!!ops->acquire != !!ops->retire)) return INTEL_BCS_INVALID;
    memset(q,0,sizeof(*q));
    q->engine=engine; q->io=*io; q->ops=*ops; q->engine_context=engine_context;
    q->prepare_draw=draw;
    q->next_context=1; q->next_fence=first_fence; q->active=INTEL_BCS_QUEUE_JOBS;
    q->initialized=true;
    return INTEL_BCS_OK;
}

enum intel_bcs_status intel_bcs_queue_init(intel_bcs_queue_t *q,intel_bcs_state_t *engine,
    const intel_gt_io_t *io,void *context,const intel_bcs_queue_ops_t *ops,uint64_t fence)
{ return bcs_queue_init(q,engine,io,context,ops,NULL,fence); }
enum intel_bcs_status intel_bcs_queue_init_draw(intel_bcs_queue_t *q,intel_bcs_state_t *engine,
    const intel_gt_io_t *io,void *context,const intel_bcs_queue_ops_t *ops,
    intel_rcs_queue_prepare_t draw,uint64_t fence)
{
    if (!draw) return INTEL_BCS_INVALID;
    return bcs_queue_init(q,engine,io,context,ops,draw,fence);
}

enum intel_bcs_status intel_bcs_queue_attach(intel_bcs_queue_t *q, uint64_t owner,
    void *backing, uint64_t *context)
{ return intel_bcs_queue_attach_epoch(q,owner,0,backing,context); }

enum intel_bcs_status intel_bcs_queue_attach_epoch(intel_bcs_queue_t *q, uint64_t owner,
    uint64_t epoch, void *backing, uint64_t *context)
{
    if (!q || !q->initialized || !owner || !backing ||
        !context) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    if (q->closing || q->fault) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (q->next_context==UINT64_MAX) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    unsigned free_slot=INTEL_BCS_QUEUE_CONTEXTS;
    for (unsigned i=0; i<INTEL_BCS_QUEUE_CONTEXTS; i++) {
        if (q->contexts[i].backing==backing ||
            (q->contexts[i].owner==owner && q->contexts[i].epoch==epoch && q->contexts[i].process_exited))
            QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
        if (!q->contexts[i].backing && free_slot==INTEL_BCS_QUEUE_CONTEXTS) free_slot=i;
    }
    if (free_slot==INTEL_BCS_QUEUE_CONTEXTS) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    intel_bcs_queue_context_t *c=&q->contexts[free_slot];
    *c=(intel_bcs_queue_context_t){.backing=backing,.owner=owner,.epoch=epoch,.id=q->next_context++};
    *context=c->id;
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

static enum intel_bcs_status bcs_queue_enqueue(intel_bcs_queue_t *q, uint64_t owner,
    uint64_t context,const intel_bcs_copy_t *copy,const intel_rcs_queue_draw_t *draw,
    const intel_bcs_queue_gate_t *gate,uint64_t *fence)
{
    if (!q || !q->initialized || (!copy && !draw) || !fence ||
        (draw && !q->prepare_draw) || (copy && q->prepare_draw && !q->ops.prepare)) return INTEL_BCS_INVALID;
    intel_rcs_queue_draw_t snapshot;
    if (draw) snapshot=*draw;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_context_t *c=bcs_queue_context(q,owner,context);
    if (!c || c->closing || q->closing || q->fault) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (c->updating) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    if (q->next_fence==UINT64_MAX) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    if (gate && __atomic_load_n(gate->state,__ATOMIC_ACQUIRE)!=gate->pending)
        QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    bcs_queue_reap(q);
    for (unsigned i=0; i<INTEL_BCS_QUEUE_JOBS; i++) {
        intel_bcs_queue_job_t *j=&q->jobs[i];
        if (j->result.phase!=INTEL_BCS_JOB_EMPTY) continue;
        *j=(intel_bcs_queue_job_t){.owner=owner,.context=context,.fence=q->next_fence++,
            .copy=copy ? *copy : (intel_bcs_copy_t){0},
            .is_draw=draw!=NULL,
            .result={gate ? INTEL_BCS_JOB_STAGED : INTEL_BCS_JOB_QUEUED,INTEL_BCS_PENDING},
            .retained=gate!=NULL};
        if (draw) j->draw=snapshot;
        if (gate) j->gate=*gate;
        c->references++;
        *fence=j->fence;
        q->events|=gate ? INTEL_BCS_EVENT_STATE : INTEL_BCS_EVENT_WORK;
        QUEUE_RETURN(q,flags,INTEL_BCS_OK);
    }
    QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
}

enum intel_bcs_status intel_bcs_queue_enqueue(intel_bcs_queue_t *q,uint64_t owner,
    uint64_t context,const intel_bcs_copy_t *copy,uint64_t *fence)
{
    return bcs_queue_enqueue(q,owner,context,copy,NULL,NULL,fence);
}
enum intel_bcs_status intel_bcs_queue_stage(intel_bcs_queue_t *q,uint64_t owner,
    uint64_t context,const intel_bcs_copy_t *copy,const intel_bcs_queue_gate_t *gate,uint64_t *fence)
{
    if (!gate || !gate->state || ((uintptr_t)gate->state&(_Alignof(unsigned)-1)) ||
        gate->pending==gate->accepted || gate->pending==gate->rejected || gate->accepted==gate->rejected)
        return INTEL_BCS_INVALID;
    return bcs_queue_enqueue(q,owner,context,copy,NULL,gate,fence);
}
enum intel_bcs_status intel_bcs_queue_enqueue_draw(intel_bcs_queue_t *q,uint64_t owner,
    uint64_t context,const intel_rcs_queue_draw_t *draw,uint64_t *fence)
{ return bcs_queue_enqueue(q,owner,context,NULL,draw,NULL,fence); }
enum intel_bcs_status intel_bcs_queue_stage_draw(intel_bcs_queue_t *q,uint64_t owner,
    uint64_t context,const intel_rcs_queue_draw_t *draw,const intel_bcs_queue_gate_t *gate,uint64_t *fence)
{
    if (!gate || !gate->state || ((uintptr_t)gate->state&(_Alignof(unsigned)-1)) ||
        gate->pending==gate->accepted || gate->pending==gate->rejected || gate->accepted==gate->rejected)
        return INTEL_BCS_INVALID;
    return bcs_queue_enqueue(q,owner,context,NULL,draw,gate,fence);
}
enum intel_bcs_status intel_bcs_queue_finish_stage(intel_bcs_queue_t *q,uint64_t owner,
    uint64_t fence,unsigned *state)
{
    if (!q || !q->initialized || !state) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_job_t *j=bcs_queue_job(q,owner,fence);
    if (!j || j->gate.state!=state || !j->retained) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    unsigned value=__atomic_load_n(state,__ATOMIC_ACQUIRE);
    if (value==j->gate.pending) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    if (value!=j->gate.accepted && value!=j->gate.rejected) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (j->result.phase==INTEL_BCS_JOB_STAGED) {
        if (value==j->gate.accepted && !q->closing && !q->fault) {
            j->result.phase=INTEL_BCS_JOB_QUEUED; q->events|=INTEL_BCS_EVENT_WORK;
        } else bcs_queue_finish(q,j,INTEL_BCS_JOB_CANCELLED,INTEL_BCS_OK);
    }
    j->gate=(intel_bcs_queue_gate_t){0};
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

static enum intel_bcs_status bcs_queue_update_mappings(intel_bcs_queue_t *q,
    uint64_t owner,uint64_t context,intel_bcs_queue_update_t update,void *argument,bool removing)
{
    if (!q || !q->initialized || !update) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_context_t *c=bcs_queue_context(q,owner,context);
    if (!c || (c->closing && !removing) || q->closing || q->fault || q->stopped)
        QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (q->worker_busy || q->active<INTEL_BCS_QUEUE_JOBS || c->references)
        QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    q->worker_busy=true; c->updating=true; c->references++;
    bcs_queue_unlock(&q->lock,flags);
    enum intel_bcs_status r=update(c->backing,argument,&q->io,q->engine);
    flags=bcs_queue_lock(&q->lock);
    c->references--; c->updating=false; q->worker_busy=false;
    if (r!=INTEL_BCS_OK && r!=INTEL_BCS_INVALID && r!=INTEL_BCS_BUSY) {
        q->fault=r; q->events|=INTEL_BCS_EVENT_FAULT;
    }
    /* Wake a worker that encountered BUSY, including shutdown racing update. */
    q->events|=INTEL_BCS_EVENT_WORK|INTEL_BCS_EVENT_STATE;
    QUEUE_RETURN(q,flags,r);
}
enum intel_bcs_status intel_bcs_queue_update_context(intel_bcs_queue_t *q,
    uint64_t owner,uint64_t context,intel_bcs_queue_update_t update,void *argument)
{
    return bcs_queue_update_mappings(q,owner,context,update,argument,false);
}
enum intel_bcs_status intel_bcs_queue_remove_context_mappings(intel_bcs_queue_t *q,
    uint64_t owner,uint64_t context,intel_bcs_queue_update_t update,void *argument)
{
    return bcs_queue_update_mappings(q,owner,context,update,argument,true);
}

enum intel_bcs_status intel_bcs_queue_retarget_context(intel_bcs_queue_t *q,
    uint64_t owner,uint64_t context,uint64_t replacement_owner,uint64_t epoch,
    intel_bcs_queue_update_t update,void *argument,uint64_t *replacement_context)
{
    if (!q || !q->initialized || !replacement_owner || !update || !replacement_context)
        return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_context_t *c=bcs_queue_context(q,owner,context);
    if (!c || !c->closing || q->closing || q->fault || q->stopped)
        QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (q->worker_busy || q->active<INTEL_BCS_QUEUE_JOBS || c->references || q->next_context==UINT64_MAX)
        QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    for (unsigned i=0;i<INTEL_BCS_QUEUE_CONTEXTS;i++)
        if (q->contexts[i].backing && q->contexts[i].owner==replacement_owner &&
            q->contexts[i].epoch==epoch && q->contexts[i].process_exited)
            QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    bcs_queue_reap(q);
    for (unsigned i=0;i<INTEL_BCS_QUEUE_JOBS;i++)
        if (q->jobs[i].result.phase!=INTEL_BCS_JOB_EMPTY &&
            q->jobs[i].owner==owner && q->jobs[i].context==context)
            QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    uint64_t next=q->next_context++;
    q->worker_busy=true; c->updating=true; c->references++;
    c->replacement_owner=replacement_owner; c->replacement_exited=false;
    bcs_queue_unlock(&q->lock,flags);
    enum intel_bcs_status result=update(c->backing,argument,&q->io,q->engine);
    flags=bcs_queue_lock(&q->lock);
    c->references--; c->updating=false; q->worker_busy=false;
    if (result==INTEL_BCS_OK) {
        c->owner=replacement_owner; c->epoch=epoch; c->id=next;
        c->process_exited=c->replacement_exited;
        c->closing=c->replacement_exited || q->closing;
        *replacement_context=next;
    } else if (result!=INTEL_BCS_INVALID && result!=INTEL_BCS_BUSY) {
        q->fault=result; q->events|=INTEL_BCS_EVENT_FAULT;
    }
    c->replacement_owner=0; c->replacement_exited=false;
    q->events|=INTEL_BCS_EVENT_WORK|INTEL_BCS_EVENT_STATE;
    QUEUE_RETURN(q,flags,result);
}

enum intel_bcs_status intel_bcs_queue_result(intel_bcs_queue_t *q, uint64_t owner,
    uint64_t fence, bool consume, intel_bcs_job_result_t *result)
{
    if (!q || !q->initialized || !result) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_job_t *j=bcs_queue_job(q,owner,fence);
    if (!j || (consume && (j->result.phase<INTEL_BCS_JOB_COMPLETE || j->gate.state))) {
        enum intel_bcs_status r=j ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        bcs_queue_unlock(&q->lock,flags);
        return r;
    }
    *result=j->result;
    if (!consume) { bcs_queue_unlock(&q->lock,flags); return INTEL_BCS_OK; }
    memset(j,0,sizeof(*j));
    q->events|=INTEL_BCS_EVENT_SPACE;
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

enum intel_bcs_status intel_bcs_queue_cancel(intel_bcs_queue_t *q,
    uint64_t owner, uint64_t fence)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_job_t *j=bcs_queue_job(q,owner,fence);
    if (!j) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (j->result.phase==INTEL_BCS_JOB_RUNNING || j->result.phase==INTEL_BCS_JOB_PREPARING)
        QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    if (j->result.phase==INTEL_BCS_JOB_QUEUED || j->result.phase==INTEL_BCS_JOB_STAGED)
        bcs_queue_finish(q,j,INTEL_BCS_JOB_CANCELLED,INTEL_BCS_OK);
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

static void bcs_queue_close(intel_bcs_queue_t *q, intel_bcs_queue_context_t *c)
{
    if (!c->closing) q->events|=INTEL_BCS_EVENT_STATE;
    c->closing=true;
    for (unsigned i=0; i<INTEL_BCS_QUEUE_JOBS; i++) {
        intel_bcs_queue_job_t *j=&q->jobs[i];
        if (j->context!=c->id || j->owner!=c->owner) continue;
        if (j->result.phase==INTEL_BCS_JOB_QUEUED || j->result.phase==INTEL_BCS_JOB_STAGED)
            bcs_queue_finish(q,j,INTEL_BCS_JOB_CANCELLED,INTEL_BCS_OK);
    }
}

enum intel_bcs_status intel_bcs_queue_close_context(intel_bcs_queue_t *q,
    uint64_t owner, uint64_t context)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_context_t *c=bcs_queue_context(q,owner,context);
    if (!c) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    bcs_queue_close(q,c);
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

enum intel_bcs_status intel_bcs_queue_release_context(intel_bcs_queue_t *q,
    uint64_t owner, uint64_t context)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    intel_bcs_queue_context_t *c=bcs_queue_context(q,owner,context);
    if (!c) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (!c->closing || c->references || !q->stopped || !q->engine->retired)
        QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    c->backing=NULL;
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

/* The worker owns the active slot and its context reference. Hardware safety
 * has already been established, but the result is not yet visible as terminal.
 * The caller releases the metadata lock before entering this helper. */
static enum intel_bcs_status bcs_queue_retire_job(intel_bcs_queue_t *q,
    intel_bcs_queue_job_t *j,intel_bcs_queue_context_t *c,bool stopping)
{
    enum intel_bcs_status r=j->access_started ? q->ops.retire(c->backing,j->fence) : INTEL_BCS_OK;
    uint64_t flags=bcs_queue_lock(&q->lock);
    q->worker_busy=false;
    if (r!=INTEL_BCS_OK) {
        if (r!=INTEL_BCS_BUSY) { q->fault=INTEL_BCS_QUARANTINED; q->events|=INTEL_BCS_EVENT_FAULT; }
        /* BUSY made no progress. Preserve external notices, but do not wake
         * the worker for its own retry: it must wait for a timer/resource
         * notification so the CPU owner can run and release the resource. */
        QUEUE_RETURN(q,flags,r==INTEL_BCS_BUSY ? r : INTEL_BCS_QUARANTINED);
    }
    j->access_started=false; j->retiring=false;
    q->active=INTEL_BCS_QUEUE_JOBS;
    if (j->retry && !q->closing && !c->closing) {
        j->result=(intel_bcs_job_result_t){INTEL_BCS_JOB_QUEUED,INTEL_BCS_PENDING};
        j->retry=false; /* returning the same work is not new work */
        QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    }
    if (j->retry) j->retired_result=(intel_bcs_job_result_t){INTEL_BCS_JOB_CANCELLED,INTEL_BCS_OK};
    bcs_queue_finish(q,j,j->retired_result.phase,j->retired_result.error);
    if (j->result.phase==INTEL_BCS_JOB_FAILED) {
        q->fault=j->result.error; q->events|=INTEL_BCS_EVENT_FAULT;
    }
    if (stopping) { q->stopped=true; q->events|=INTEL_BCS_EVENT_STATE; }
    r=!stopping && j->result.phase==INTEL_BCS_JOB_FAILED ? j->result.error : INTEL_BCS_OK;
    QUEUE_RETURN(q,flags,r);
}

enum intel_bcs_status intel_bcs_queue_step(intel_bcs_queue_t *q)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    if (q->worker_busy) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    if (q->closing) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    if (q->fault) QUEUE_RETURN(q,flags,q->fault);
    bcs_queue_reap(q);
    if (q->active<INTEL_BCS_QUEUE_JOBS) {
        intel_bcs_queue_job_t *j=&q->jobs[q->active];
        intel_bcs_queue_context_t *c=bcs_queue_context(q,j->owner,j->context);
        q->worker_busy=true;
        bcs_queue_unlock(&q->lock,flags);
        if (j->retiring) return bcs_queue_retire_job(q,j,c,false);
        enum intel_bcs_status r=intel_cs_check(&q->io,c->backing,q->ops.token_read,q->engine);
        flags=bcs_queue_lock(&q->lock);
        if (r==INTEL_BCS_PENDING) { q->worker_busy=false; QUEUE_RETURN(q,flags,r); }
        if (r!=INTEL_BCS_OK) {
            q->worker_busy=false; q->fault=r; q->events|=INTEL_BCS_EVENT_FAULT;
            QUEUE_RETURN(q,flags,r);
        }
        j->retiring=true;
        j->retired_result=(intel_bcs_job_result_t){INTEL_BCS_JOB_COMPLETE,INTEL_BCS_OK};
        bcs_queue_unlock(&q->lock,flags);
        return bcs_queue_retire_job(q,j,c,false);
    }
    unsigned next=INTEL_BCS_QUEUE_JOBS;
    for (unsigned i=0; i<INTEL_BCS_QUEUE_JOBS; i++)
        if (q->jobs[i].result.phase==INTEL_BCS_JOB_QUEUED &&
            (next==INTEL_BCS_QUEUE_JOBS || q->jobs[i].fence<q->jobs[next].fence)) next=i;
    if (next==INTEL_BCS_QUEUE_JOBS) QUEUE_RETURN(q,flags,INTEL_BCS_OK);
    intel_bcs_queue_job_t *j=&q->jobs[next];
    intel_bcs_queue_context_t *c=bcs_queue_context(q,j->owner,j->context);
    uint64_t descriptor=0;
    uint32_t tail=0;
    q->worker_busy=true; q->active=next; j->result.phase=INTEL_BCS_JOB_PREPARING;
    j->access_started=q->ops.acquire!=NULL;
    bcs_queue_unlock(&q->lock,flags);
    enum intel_bcs_status acquired=q->ops.acquire ? q->ops.acquire(c->backing,j->fence) : INTEL_BCS_OK;
    flags=bcs_queue_lock(&q->lock);
    bool can_prepare=!c->closing && !q->closing;
    bcs_queue_unlock(&q->lock,flags);
    bool prepared=can_prepare && acquired==INTEL_BCS_OK && (j->is_draw ?
        q->prepare_draw(c->backing,&j->draw,j->fence,&descriptor,&tail) :
        q->ops.prepare && q->ops.prepare(c->backing,&j->copy,j->fence,&descriptor,&tail));
    flags=bcs_queue_lock(&q->lock);
    if (c->closing || q->closing) {
        j->retiring=true;
        j->retired_result=(intel_bcs_job_result_t){INTEL_BCS_JOB_CANCELLED,INTEL_BCS_OK};
    } else if (!prepared) {
        j->retiring=true; j->retry=acquired==INTEL_BCS_BUSY;
        j->retired_result=(intel_bcs_job_result_t){INTEL_BCS_JOB_FAILED,
            acquired==INTEL_BCS_OK ? INTEL_BCS_INVALID : acquired};
    }
    if (j->retiring) {
        bcs_queue_unlock(&q->lock,flags);
        return bcs_queue_retire_job(q,j,c,false);
    }
    /* Publish references before any MMIO write that could start execution. */
    c->published=true; j->result.phase=INTEL_BCS_JOB_RUNNING;
    bcs_queue_unlock(&q->lock,flags);
    enum intel_bcs_status r=intel_cs_submit(&q->io,c->backing,q->ops.token_read,
        descriptor,j->fence,tail,q->engine);
    flags=bcs_queue_lock(&q->lock);
    q->worker_busy=false;
    if (r!=INTEL_BCS_OK) {
        q->fault=r; q->events|=INTEL_BCS_EVENT_FAULT;
        QUEUE_RETURN(q,flags,r);
    }
    QUEUE_RETURN(q,flags,INTEL_BCS_PENDING);
}

enum intel_bcs_status intel_bcs_queue_shutdown(intel_bcs_queue_t *q)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    if (q->stopped) QUEUE_RETURN(q,flags,INTEL_BCS_OK);
    if (!q->closing) q->events|=INTEL_BCS_EVENT_STATE;
    q->closing=true;
    for (unsigned i=0; i<INTEL_BCS_QUEUE_CONTEXTS; i++)
        if (q->contexts[i].backing)
            bcs_queue_close(q,&q->contexts[i]);
    if (q->worker_busy) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    q->worker_busy=true;
    bcs_queue_unlock(&q->lock,flags);
    enum intel_bcs_status r=intel_cs_close(&q->io,q->engine_context,q->engine);
    flags=bcs_queue_lock(&q->lock);
    if (r!=INTEL_BCS_OK || !q->engine->retired) {
        q->worker_busy=false; q->events|=INTEL_BCS_EVENT_FAULT;
        QUEUE_RETURN(q,flags,INTEL_BCS_QUARANTINED);
    }
    if (q->active<INTEL_BCS_QUEUE_JOBS) {
        intel_bcs_queue_job_t *j=&q->jobs[q->active];
        intel_bcs_queue_context_t *c=bcs_queue_context(q,j->owner,j->context);
        if (!j->retiring) j->retired_result=(intel_bcs_job_result_t){
            q->fault ? INTEL_BCS_JOB_FAILED : INTEL_BCS_JOB_ABORTED,q->fault};
        j->retiring=true;
        bcs_queue_unlock(&q->lock,flags);
        return bcs_queue_retire_job(q,j,c,true);
    }
    q->stopped=true; q->worker_busy=false;
    q->events|=INTEL_BCS_EVENT_STATE;
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

enum intel_bcs_status intel_bcs_queue_watch(intel_bcs_queue_t *q,
    intel_bcs_queue_wakeup_t wakeup, void *context)
{
    if (!q || !q->initialized || !wakeup) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    if (q->wakeup || q->closing) QUEUE_RETURN(q,flags,INTEL_BCS_INVALID);
    q->wakeup=wakeup; q->wakeup_context=context;
    q->events|=INTEL_BCS_EVENT_ALL;
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

enum intel_bcs_status intel_bcs_queue_unwatch(intel_bcs_queue_t *q)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    if (q->notifying) QUEUE_RETURN(q,flags,INTEL_BCS_BUSY);
    q->wakeup=NULL; q->wakeup_context=NULL; q->events=0;
    QUEUE_RETURN(q,flags,INTEL_BCS_OK);
}

enum intel_bcs_status intel_bcs_queue_snapshot(intel_bcs_queue_t *q,
    intel_bcs_queue_snapshot_t *snapshot)
{
    if (!q || !q->initialized || !snapshot) return INTEL_BCS_INVALID;
    uint64_t flags=bcs_queue_lock(&q->lock);
    *snapshot=(intel_bcs_queue_snapshot_t){.fault=q->fault,.closing=q->closing,
        .stopped=q->stopped,.worker_busy=q->worker_busy};
    for (unsigned i=0;i<INTEL_BCS_QUEUE_JOBS;i++) {
        enum intel_bcs_job_phase p=q->jobs[i].result.phase;
        if (p==INTEL_BCS_JOB_STAGED) snapshot->staged++;
        else if (p==INTEL_BCS_JOB_QUEUED) snapshot->queued++;
        else if (p==INTEL_BCS_JOB_PREPARING || p==INTEL_BCS_JOB_RUNNING) snapshot->active++;
        else if (p>=INTEL_BCS_JOB_COMPLETE) snapshot->terminal++;
    }
    /* A read never delivers a notification from a partially completed writer. */
    bcs_queue_unlock(&q->lock,flags);
    return INTEL_BCS_OK;
}

enum intel_bcs_status intel_bcs_queue_publish(intel_bcs_queue_t *q)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t registry_flags=bcs_queue_lock(&queue_registry_lock);
    uint64_t flags=bcs_queue_lock(&q->lock);
    enum intel_bcs_status r=INTEL_BCS_INVALID;
    if (!q->closing && !q->registered) {
        r=INTEL_BCS_BUSY;
        for (unsigned i=0;i<8;i++) if (!queue_registry[i]) {
            queue_registry[i]=q; q->registered=true; r=INTEL_BCS_OK; break;
        }
    }
    bcs_queue_unlock(&q->lock,flags);
    bcs_queue_unlock(&queue_registry_lock,registry_flags);
    return r;
}

enum intel_bcs_status intel_bcs_queue_unpublish(intel_bcs_queue_t *q)
{
    if (!q || !q->initialized) return INTEL_BCS_INVALID;
    uint64_t registry_flags=bcs_queue_lock(&queue_registry_lock);
    uint64_t flags=bcs_queue_lock(&q->lock);
    enum intel_bcs_status r=INTEL_BCS_INVALID;
    if (q->registered) {
        r=INTEL_BCS_BUSY;
        bool retained=false;
        for (unsigned i=0;i<INTEL_BCS_QUEUE_JOBS;i++) retained|=q->jobs[i].retained;
        if (q->stopped && !q->worker_busy && !q->wakeup && !q->notifying && !retained) {
            for (unsigned i=0;i<8;i++) if (queue_registry[i]==q) queue_registry[i]=NULL;
            q->registered=false; r=INTEL_BCS_OK;
        }
    }
    bcs_queue_unlock(&q->lock,flags);
    bcs_queue_unlock(&queue_registry_lock,registry_flags);
    return r;
}

void intel_bcs_release_process(uint32_t process_id)
{
    if (!process_id) return;
    intel_bcs_queue_t *notified[8]={0};
    bcs_queue_notice_t notices[8]={0};
    uint64_t registry_flags=bcs_queue_lock(&queue_registry_lock);
    for (unsigned i=0;i<8;i++) {
        intel_bcs_queue_t *q=queue_registry[i];
        if (!q) continue;
        uint64_t flags=bcs_queue_lock(&q->lock);
        for (unsigned c=0;c<INTEL_BCS_QUEUE_CONTEXTS;c++) {
            if (q->contexts[c].backing && q->contexts[c].owner==process_id) {
                q->contexts[c].process_exited=true;
                bcs_queue_close(q,&q->contexts[c]);
            }
            if (q->contexts[c].replacement_owner==process_id) {
                q->contexts[c].replacement_exited=true;
                q->events|=INTEL_BCS_EVENT_STATE;
            }
        }
        notices[i]=bcs_queue_notify_take(q);
        notified[i]=q;
        bcs_queue_unlock(&q->lock,flags);
    }
    bcs_queue_unlock(&queue_registry_lock,registry_flags);
    for (unsigned i=0;i<8;i++) bcs_queue_notify_call(notified[i],notices[i]);
}
