#include "intel_bcs_worker.h"
#ifdef __KERNEL_X86__
#include "../kernel/smp_work.h"
#else
#include <string.h>
extern int ws_cpu_count;
extern int smp_submit_any(void (*)(void *,void *),void *,void *);
extern void smp_wait(int);
#endif
extern uint32_t smp_current_cpu(void);
extern uint64_t paging_get_kernel_cr3(void);
extern int32_t proc_current_pid(void);
extern int sched_spawn_in_address_space_blocked(const char *, void (*)(void),uint64_t,int);
extern int proc_wake_pid(int);
extern int proc_kill_pid(int);
extern int proc_waitpid(uint32_t,int32_t *);
extern void sched_yield(void);
static intel_bcs_worker_t *workers[4];

static bool bcs_worker_caller(void)
{
    if (smp_current_cpu()!=0) return false;
#ifdef __KERNEL_X86__
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!(flags&(1ULL<<9))) return false;
#endif
    return true;
}

static void bcs_worker_wake(void *context, uint32_t events)
{
    intel_bcs_worker_t *w=context;
    sched_signal_set(&w->wake,events);
    if (w->notify) w->notify(w->notify_context,events);
}

static void bcs_worker_operation(void *context, void *unused)
{
    (void)unused;
    intel_bcs_worker_t *w=context;
    if (smp_current_cpu()) __atomic_add_fetch(&w->ap_operations,1,__ATOMIC_RELAXED);
    else __atomic_add_fetch(&w->bsp_operations,1,__ATOMIC_RELAXED);
    if (w->shutdown_op) w->op_status=intel_bcs_queue_shutdown(w->queue);
    else {
        w->op_status=intel_bcs_queue_step(w->queue);
        /* Bring-up completion observation only. Replace this active polling
         * with GPU IRQ/watchdog delivery; idle services consume no AP task. */
        for (unsigned i=0;!w->interrupt_driven && i<200000 && w->op_status==INTEL_BCS_PENDING &&
            !__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE);i++) {
            w->queue->io.relax(w->queue->engine_context);
            w->op_status=intel_bcs_queue_step(w->queue);
        }
    }
    __atomic_add_fetch(&w->operations,1,__ATOMIC_RELAXED);
    __atomic_store_n(&w->op_done,1,__ATOMIC_RELEASE);
    sched_signal_set(&w->wake,INTEL_BCS_EVENT_STATE);
}

static enum intel_bcs_status bcs_worker_run_operation(intel_bcs_worker_t *w,
    bool shutdown)
{
    w->shutdown_op=shutdown;
    __atomic_store_n(&w->op_done,0,__ATOMIC_RELEASE);
    if (ws_cpu_count<=1) {
        bcs_worker_operation(w,NULL);
        return w->op_status;
    }
    int task=smp_submit_any(bcs_worker_operation,w,NULL);
    __atomic_store_n(&w->task,task,__ATOMIC_RELEASE);
    if (task<0) return INTEL_BCS_BUSY;
    while (!__atomic_load_n(&w->op_done,__ATOMIC_ACQUIRE)) {
        sched_signal_take(&w->wake);
        if (__atomic_load_n(&w->op_done,__ATOMIC_ACQUIRE)) break;
        if (sched_signal_wait(&w->wake,100)!=SCHED_SIGNAL_READY) sched_yield();
        /* A timeout is not permission to start a second AP job or free its
         * argument. Keep waiting on the same task until completion/join. */
    }
    smp_wait(task);
    __atomic_store_n(&w->task,-1,__ATOMIC_RELEASE);
    return w->op_status;
}

void intel_bcs_worker_interrupt(intel_bcs_worker_t *w, bool fault)
{
    if (!w || w->self!=w) return;
    if (fault) __atomic_store_n(&w->irq_fault,1,__ATOMIC_RELEASE);
    __atomic_add_fetch(&w->irq_sequence,1,__ATOMIC_RELEASE);
    sched_signal_set(&w->wake,INTEL_BCS_EVENT_STATE);
}

static void bcs_worker_wait_gpu(intel_bcs_worker_t *w,unsigned seen)
{
    /* IRQs may arrive before the AP publishes op_done. A separate sequence
     * survives consumption of queue/AP wake hints while joining that item. */
    uint64_t deadline=w->queue->io.now_ns(w->queue->engine_context)+500000000ULL;
    while (__atomic_load_n(&w->irq_sequence,__ATOMIC_ACQUIRE)==seen &&
        !__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&w->irq_fault,__ATOMIC_ACQUIRE)) {
        sched_signal_take(&w->wake);
        if (__atomic_load_n(&w->irq_sequence,__ATOMIC_ACQUIRE)!=seen ||
            __atomic_load_n(&w->stop,__ATOMIC_ACQUIRE)) break;
        uint64_t now=w->queue->io.now_ns(w->queue->engine_context);
        if (now>=deadline) { w->watchdog_wakes++; break; }
        w->irq_waits++;
        enum sched_signal_result r=sched_signal_wait(&w->wake,(deadline-now+9999999)/10000000);
        if (r==SCHED_SIGNAL_TIMEOUT) { w->watchdog_wakes++; break; }
        if (r!=SCHED_SIGNAL_READY) sched_yield();
    }
}

static void bcs_worker_entry(void)
{
    intel_bcs_worker_t *w=NULL;
    int pid=proc_current_pid();
    for (unsigned i=0;i<4;i++) {
        intel_bcs_worker_t *candidate=__atomic_load_n(&workers[i],__ATOMIC_ACQUIRE);
        if (candidate && candidate->pid==pid) { w=candidate; break; }
    }
    if (!w) return;
    bool retirement_failed=false;
    for (;;) {
        sched_signal_take(&w->wake);
        bool retry=__atomic_exchange_n(&w->retry,0,__ATOMIC_ACQ_REL)!=0;
        intel_bcs_queue_snapshot_t snapshot;
        if (intel_bcs_queue_snapshot(w->queue,&snapshot)!=INTEL_BCS_OK) return;
        bool stopping=__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE) || snapshot.closing || snapshot.fault ||
            __atomic_load_n(&w->irq_fault,__ATOMIC_ACQUIRE);
        if (stopping) {
            __atomic_store_n(&w->stop,1,__ATOMIC_RELEASE);
            if (!retirement_failed || retry) {
                w->last_status=bcs_worker_run_operation(w,true);
                if (w->last_status==INTEL_BCS_OK) {
                    while (intel_bcs_queue_unwatch(w->queue)!=INTEL_BCS_OK)
                        if (sched_signal_wait(&w->wake,1)!=SCHED_SIGNAL_READY) sched_yield();
                    if (w->notify) w->notify(w->notify_context,INTEL_BCS_EVENT_STATE);
                    __atomic_store_n(&w->finished,1,__ATOMIC_RELEASE);
                    return;
                }
                if (w->last_status==INTEL_BCS_BUSY) {
                    if (sched_signal_wait(&w->wake,1)!=SCHED_SIGNAL_READY) sched_yield();
                    continue;
                }
                retirement_failed=true;
            }
        } else if (snapshot.queued || snapshot.active) {
            unsigned seen=__atomic_load_n(&w->irq_sequence,__ATOMIC_ACQUIRE);
            w->last_status=bcs_worker_run_operation(w,false);
            if (w->interrupt_driven && w->last_status==INTEL_BCS_PENDING)
                bcs_worker_wait_gpu(w,seen);
            if (w->last_status==INTEL_BCS_BUSY)
                if (sched_signal_wait(&w->wake,1)!=SCHED_SIGNAL_READY) sched_yield();
            continue;
        }
        __atomic_add_fetch(&w->sleeps,1,__ATOMIC_RELAXED);
        if (sched_signal_wait(&w->wake,0)!=SCHED_SIGNAL_READY) sched_yield();
    }
}

enum intel_bcs_status intel_bcs_worker_start(intel_bcs_worker_t *w,
    intel_bcs_queue_t *queue, intel_bcs_queue_wakeup_t notify, void *context,
    bool interrupt_driven)
{
    if (!w || w->self || !queue || !bcs_worker_caller()) return INTEL_BCS_INVALID;
    intel_bcs_queue_snapshot_t snapshot;
    if (intel_bcs_queue_snapshot(queue,&snapshot)!=INTEL_BCS_OK ||
        snapshot.closing || snapshot.fault || snapshot.worker_busy) return INTEL_BCS_INVALID;
    memset(w,0,sizeof(*w)); w->self=w; w->queue=queue; w->task=-1;
    w->notify=notify; w->notify_context=context;
    w->interrupt_driven=interrupt_driven;
    unsigned slot;
    for (slot=0;slot<4;slot++) {
        intel_bcs_worker_t *empty=NULL;
        if (__atomic_compare_exchange_n(&workers[slot],&empty,w,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) break;
    }
    if (slot==4) { w->self=NULL; return INTEL_BCS_BUSY; }
    w->pid=sched_spawn_in_address_space_blocked("intel-bcs",bcs_worker_entry,paging_get_kernel_cr3(),0);
    if (w->pid<=0) {
        __atomic_store_n(&workers[slot],NULL,__ATOMIC_RELEASE); w->self=NULL;
        return INTEL_BCS_BUSY;
    }
    __atomic_store_n(&w->started,1,__ATOMIC_RELEASE);
    if (intel_bcs_queue_watch(queue,bcs_worker_wake,w)!=INTEL_BCS_OK) {
        proc_kill_pid(w->pid);
        if (proc_waitpid((uint32_t)w->pid,NULL)!=0) return INTEL_BCS_QUARANTINED;
        __atomic_store_n(&workers[slot],NULL,__ATOMIC_RELEASE); w->started=0; w->self=NULL;
        return INTEL_BCS_INVALID;
    }
    if (proc_wake_pid(w->pid)!=0) return INTEL_BCS_QUARANTINED;
    return INTEL_BCS_OK;
}

enum intel_bcs_status intel_bcs_worker_stop(intel_bcs_worker_t *w)
{
    if (!w || w->self!=w || !bcs_worker_caller() ||
        !__atomic_load_n(&w->started,__ATOMIC_ACQUIRE)) return INTEL_BCS_INVALID;
    __atomic_store_n(&w->stop,1,__ATOMIC_RELEASE);
    __atomic_store_n(&w->retry,1,__ATOMIC_RELEASE);
    sched_signal_set(&w->wake,INTEL_BCS_EVENT_STATE);
    return INTEL_BCS_PENDING;
}

enum intel_bcs_status intel_bcs_worker_reap(intel_bcs_worker_t *w)
{
    if (!w || w->self!=w || !bcs_worker_caller()) return INTEL_BCS_INVALID;
    if (!__atomic_load_n(&w->started,__ATOMIC_ACQUIRE)) return INTEL_BCS_OK;
    if (!__atomic_load_n(&w->finished,__ATOMIC_ACQUIRE)) return INTEL_BCS_PENDING;
    if (proc_waitpid((uint32_t)w->pid,NULL)!=0) return INTEL_BCS_BUSY;
    for (unsigned i=0;i<4;i++)
        if (__atomic_load_n(&workers[i],__ATOMIC_ACQUIRE)==w)
            __atomic_store_n(&workers[i],NULL,__ATOMIC_RELEASE);
    __atomic_store_n(&w->started,0,__ATOMIC_RELEASE);
    return INTEL_BCS_OK;
}

bool intel_bcs_worker_idle(const intel_bcs_worker_t *w)
{
    return w && w->self==w && __atomic_load_n(&w->started,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&w->stop,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&w->wake.waiters,__ATOMIC_ACQUIRE)!=0 &&
        __atomic_load_n(&w->task,__ATOMIC_ACQUIRE)<0;
}
