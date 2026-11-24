#include "d3dkmt_cpu.h"
#include "../drivers/intel_gpu_cpu_view.h"
#include "../kernel/sched_signal.h"
extern uint32_t smp_current_cpu(void);
extern uint64_t paging_get_kernel_cr3(void);
extern int sched_spawn_detached_in_address_space_blocked(const char *,void (*)(void),uint64_t,int);
extern int proc_wake_pid(int);
extern void sched_yield(void);

/* No application stack, CR3 or parent lifetime is retained by this service. */
static intel_gpu_cpu_views_t cpu_views;
static sched_signal_t cpu_wake;
static unsigned cpu_state; /* 0 retryable start, 1 starting, 2 running */
static int cpu_worker_pid;

static void d3dkmt_cpu_wake(void *unused)
{
    (void)unused;
    sched_signal_set(&cpu_wake,1);
}

static void d3dkmt_cpu_worker(void)
{
    for (;;) {
        sched_signal_take(&cpu_wake);
        /* collect() retires at most one item; success need not mean drained.
         * Bound each turn, including contention and quarantined mappings. */
        for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) {
            (void)intel_gpu_cpu_views_collect(&cpu_views);
            if (!intel_gpu_cpu_views_need_retry(&cpu_views)) break;
        }
        uint64_t ticks=intel_gpu_cpu_views_need_retry(&cpu_views) ? 1 : 0;
        if (sched_signal_wait(&cpu_wake,ticks)!=SCHED_SIGNAL_READY) sched_yield();
    }
}

static NTSTATUS d3dkmt_cpu_start(void)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
#ifdef __KERNEL_X86__
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!(flags&(1ULL<<9))) return STATUS_NOT_SUPPORTED;
#endif
    if (__atomic_load_n(&cpu_state,__ATOMIC_ACQUIRE)==2) return STATUS_SUCCESS;
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&cpu_state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return D3DKMT_STATUS_DEVICE_BUSY;
    if (!cpu_views.self) (void)intel_gpu_cpu_views_init(&cpu_views,d3dkmt_cpu_wake,NULL);
    if (cpu_worker_pid<=0)
        cpu_worker_pid=sched_spawn_detached_in_address_space_blocked("kmt-cpu",
            d3dkmt_cpu_worker,paging_get_kernel_cr3(),0);
    /* If waking fails, preserve the same blocked task and retry it. No view
     * has been admitted yet, and a second collector must not be spawned. */
    bool started=cpu_worker_pid>0 && proc_wake_pid(cpu_worker_pid)==0;
    __atomic_store_n(&cpu_state,started ? 2 : 0,__ATOMIC_RELEASE);
    return started ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS d3dkmt_cpu_acquire(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE allocation,
    BOOL compat,BOOL writable,uint64_t *ticket,uint64_t *address)
{
    NTSTATUS status=d3dkmt_cpu_start();
    return status!=STATUS_SUCCESS ? status : intel_gpu_cpu_view_acquire(&cpu_views,
        pid,device,allocation,compat,writable,ticket,address);
}

NTSTATUS d3dkmt_cpu_release(DWORD pid,uint64_t ticket)
{
    if (__atomic_load_n(&cpu_state,__ATOMIC_ACQUIRE)!=2) return STATUS_INVALID_PARAMETER;
    return intel_gpu_cpu_view_release(&cpu_views,pid,ticket);
}

NTSTATUS d3dkmt_cpu_unlock(DWORD pid,OKMT_HANDLE device,DWORD count,const OKMT_HANDLE *allocations)
{
    if (__atomic_load_n(&cpu_state,__ATOMIC_ACQUIRE)!=2) return STATUS_INVALID_PARAMETER;
    return intel_gpu_cpu_views_unlock(&cpu_views,pid,device,count,allocations);
}

BOOL d3dkmt_cpu_idle(void)
{
    return __atomic_load_n(&cpu_state,__ATOMIC_ACQUIRE)==2 &&
        __atomic_load_n(&cpu_wake.waiters,__ATOMIC_ACQUIRE)!=0 &&
        !intel_gpu_cpu_views_need_retry(&cpu_views);
}
