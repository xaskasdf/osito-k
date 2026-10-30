#include "d3dkmt_wait.h"
#include "../kernel/sched_signal.h"
extern uint32_t smp_current_cpu(void);
extern uint64_t paging_get_kernel_cr3(void);
extern int sched_spawn_detached_in_address_space_blocked(const char *,void (*)(void),uint64_t,int);
extern int proc_wake_pid(int);
extern void sched_yield(void);
extern BOOL win32_termination_requested(void);

/* Requests and event references outlive their submitting thread. The worker
 * owns a kernel address space, never an application's stack or CR3. */
typedef struct {
    D3DKMT_CPU_WAIT_LEASE lease;
    D3DKMT_SYNC_EVENT_PROVIDER events;
    PVOID event;
    DWORD pid;
    BOOL used,busy,queued,waiter,cancel,complete;
    NTSTATUS result;
    sched_signal_t done;
} KMT_CPU_WAIT;
static KMT_CPU_WAIT cpu_requests[D3DKMT_MAX_CPU_WAITS];
static sched_signal_t cpu_wait_signal;
static unsigned cpu_wait_guard,cpu_wait_state;
static int cpu_wait_pid;

static uint64_t cpu_requests_lock(void)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&cpu_wait_guard,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void cpu_requests_unlock(uint64_t flags)
{
    __atomic_store_n(&cpu_wait_guard,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
static void cpu_wait_notify(void) { sched_signal_set(&cpu_wait_signal,1); }

/* Caller owns busy. No request lock spans registry or NT-event operations. */
static BOOL cpu_wait_finish(KMT_CPU_WAIT *request,NTSTATUS status)
{
    NTSTATUS released=d3dkmt_cpu_wait_release(&request->lease);
    if (released) {
        uint64_t flags=cpu_requests_lock();
        request->cancel=TRUE;request->busy=FALSE;request->queued=TRUE;
        cpu_requests_unlock(flags);return FALSE;
    }
    if (request->event) {
        if (status==STATUS_SUCCESS) request->events.signal(request->event,request->pid);
        request->events.release(request->event);request->event=NULL;
    }
    uint64_t flags=cpu_requests_lock();
    if (request->waiter) {
        request->result=status;request->complete=TRUE;request->queued=FALSE;request->busy=FALSE;
        /* Publish the wake before the caller can recycle this fixed slot. */
        sched_signal_set(&request->done,1);
    } else *request=(KMT_CPU_WAIT){0};
    cpu_requests_unlock(flags);return TRUE;
}
static BOOL cpu_wait_collect(KMT_CPU_WAIT *request)
{
    uint64_t flags=cpu_requests_lock();
    if (!request->used || !request->queued || request->busy) {cpu_requests_unlock(flags);return FALSE;}
    request->busy=TRUE;BOOL cancel=request->cancel;
    cpu_requests_unlock(flags);
    if (cancel) (void)d3dkmt_cpu_wait_cancel(&request->lease);
    NTSTATUS status=d3dkmt_cpu_wait_status(&request->lease);
    if (status!=STATUS_PENDING) return !cpu_wait_finish(request,status);
    flags=cpu_requests_lock();request->busy=FALSE;cpu_requests_unlock(flags);
    return TRUE;
}
static void cpu_wait_worker(void)
{
    for (;;) {
        sched_signal_take(&cpu_wait_signal);BOOL pending=FALSE;
        for (DWORD i=0;i<D3DKMT_MAX_CPU_WAITS;i++) pending|=cpu_wait_collect(&cpu_requests[i]);
        /* Kernel signals wake immediately. Poll only while waits are pending,
         * to also observe coherent values written directly by GPU engines. */
        if (sched_signal_wait(&cpu_wait_signal,pending ? 1 : 0)!=SCHED_SIGNAL_READY) sched_yield();
    }
}
static NTSTATUS cpu_wait_start(void)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
#ifdef __KERNEL_X86__
    uint64_t flags;__asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!(flags&(1ULL<<9))) return STATUS_NOT_SUPPORTED;
#endif
    if (__atomic_load_n(&cpu_wait_state,__ATOMIC_ACQUIRE)==2) return STATUS_SUCCESS;
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&cpu_wait_state,&expected,1,FALSE,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return D3DKMT_STATUS_DEVICE_BUSY;
    BOOL installed=d3dkmt_cpu_wait_install_waker(cpu_wait_notify);
    if (installed && cpu_wait_pid<=0)
        cpu_wait_pid=sched_spawn_detached_in_address_space_blocked("kmt-cpu-wait",cpu_wait_worker,paging_get_kernel_cr3(),0);
    BOOL started=installed && cpu_wait_pid>0 && proc_wake_pid(cpu_wait_pid)==0;
    __atomic_store_n(&cpu_wait_state,started ? 2 : 0,__ATOMIC_RELEASE);
    return started ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
NTSTATUS d3dkmt_wait_cpu(DWORD pid,OKMT_HANDLE device,DWORD count,const OKMT_HANDLE *objects,
    const uint64_t *values,BOOL any,uint64_t event,const D3DKMT_SYNC_EVENT_PROVIDER *events)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!pid || !count || count>D3DKMT_MAX_SYNC_OBJECTS || !objects || !values ||
        (any!=FALSE && any!=TRUE) ||
        (event && (!events || !events->retain || !events->signal || !events->release))) return STATUS_INVALID_PARAMETER;
    uint64_t flags=cpu_requests_lock();KMT_CPU_WAIT *request=NULL;
    for (DWORD i=0;i<D3DKMT_MAX_CPU_WAITS;i++) if (!cpu_requests[i].used) {request=&cpu_requests[i];break;}
    if (request) {
        *request=(KMT_CPU_WAIT){.pid=pid,.used=TRUE,.busy=TRUE,.waiter=event==0,.result=STATUS_PENDING};
        if (event) request->events=*events;
    }
    cpu_requests_unlock(flags);
    if (!request) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=event ? request->events.retain(pid,event,&request->event) : STATUS_SUCCESS;
    if (!status) status=d3dkmt_cpu_wait_capture(pid,device,count,objects,values,any,&request->lease);
    if (!status) status=d3dkmt_cpu_wait_status(&request->lease);
    if (status==STATUS_PENDING) {
        status=cpu_wait_start();
        if (!status) {
            flags=cpu_requests_lock();request->queued=TRUE;request->busy=FALSE;cpu_requests_unlock(flags);
            cpu_wait_notify();
        } else (void)cpu_wait_finish(request,status);
    } else (void)cpu_wait_finish(request,status);
    if (event) return status;
    for (;;) {
        BOOL cancel=win32_termination_requested();
        flags=cpu_requests_lock();
        if (request->complete) {
            status=request->result;*request=(KMT_CPU_WAIT){0};cpu_requests_unlock(flags);return status;
        }
        if (cancel) request->cancel=TRUE;
        cpu_requests_unlock(flags);
        if (cancel) cpu_wait_notify();
        /* Termination can independently wake or race the scheduler arm. The
         * bounded backstop checks cancellation without discarding owned state. */
        enum sched_signal_result waited=sched_signal_wait(&request->done,1);
        if (waited==SCHED_SIGNAL_READY) sched_signal_take(&request->done);
        else sched_yield();
    }
}
BOOL d3dkmt_wait_idle(void)
{
    uint64_t flags=cpu_requests_lock();BOOL empty=TRUE;
    for (DWORD i=0;i<D3DKMT_MAX_CPU_WAITS;i++) empty&=!cpu_requests[i].used;
    cpu_requests_unlock(flags);
    return empty && __atomic_load_n(&cpu_wait_state,__ATOMIC_ACQUIRE)==2 &&
        __atomic_load_n(&cpu_wait_signal.waiters,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&cpu_wait_signal.bits,__ATOMIC_ACQUIRE);
}
