#include "intel_gpu_service.h"
#include "intel_gpu_jobs.h"
#include "../kernel/sched_signal.h"
extern uint32_t smp_current_cpu(void);
extern uint64_t paging_get_kernel_cr3(void);
extern int sched_spawn_detached_in_address_space_blocked(const char *,void (*)(void),uint64_t,int);
extern int proc_wake_pid(int);
extern void sched_yield(void);

static struct {
    intel_gpu_memory_t *memory;
    intel_gpu_contexts_t *contexts;
    intel_gpu_jobs_t *jobs;
    intel_gpu_fences_t *fences;
    uint64_t ticket;
    bool busy;
} gpu_service_owners[INTEL_GPU_SERVICE_OWNERS];
static sched_signal_t gpu_service_signal;
static uint64_t gpu_service_next=1;
static unsigned gpu_service_guard,gpu_service_state,gpu_service_retry;
static int gpu_service_pid;

static uint64_t gpu_service_lock(void)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&gpu_service_guard,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void gpu_service_unlock(uint64_t flags)
{
    __atomic_store_n(&gpu_service_guard,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
void intel_gpu_service_notify(void *unused)
{
    (void)unused;
    sched_signal_set(&gpu_service_signal,1);
}
static void gpu_service_kmt_wake(void)
{
    intel_gpu_service_notify(NULL);
}
static void gpu_service_worker(void)
{
    for (;;) {
        sched_signal_take(&gpu_service_signal);
        bool retry=false;
        /* KMT owns references above the native resource owners. Closing a
         * process/device need not release any GPU job, so native notifications
         * alone cannot drive this collection. No service lock spans callbacks. */
        unsigned kmt_budget=d3dkmt_collect_budget();
        if (kmt_budget>32) kmt_budget=32;
        for (unsigned i=0;i<kmt_budget;i++) (void)d3dkmt_collect_devices();
        for (unsigned slot=0;slot<INTEL_GPU_SERVICE_OWNERS;slot++) {
            uint64_t flags=gpu_service_lock();
            intel_gpu_memory_t *memory=gpu_service_owners[slot].memory;
            intel_gpu_contexts_t *contexts=gpu_service_owners[slot].contexts;
            intel_gpu_jobs_t *jobs=gpu_service_owners[slot].jobs;
            intel_gpu_fences_t *fences=gpu_service_owners[slot].fences;
            if (memory) gpu_service_owners[slot].busy=true;
            gpu_service_unlock(flags);
            if (!memory) continue;
            if (jobs) {
                for (unsigned i=0;i<INTEL_GPU_JOBS;i++) (void)intel_gpu_jobs_collect(jobs);
                retry|=intel_gpu_jobs_need_retry(jobs);
            }
            if (fences) {
                for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
                    (void)intel_gpu_fences_collect(fences);
                    if (!intel_gpu_fences_need_retry(fences)) break;
                }
                retry|=intel_gpu_fences_need_retry(fences);
            }
            /* Never hold the service lock across VM, allocator or cache code.
             * A full bounded round prevents a retained/failed resource from
             * starving later slots. References are retried on producer wake;
             * a transient context CPU-release contention gets a timer retry. */
            for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
                (void)intel_gpu_contexts_collect(contexts);
                if (!intel_gpu_contexts_need_retry(contexts)) break;
            }
            unsigned budget=intel_gpu_memory_collect_budget(memory);
            if (!budget) budget=1; /* empty device retirement */
            for (unsigned i=0;i<budget;i++)
                (void)intel_gpu_memory_collect(memory);
            uint32_t released=0;
            (void)intel_gpu_memory_trim_deferred(memory,&released);
            retry|=intel_gpu_contexts_need_retry(contexts);
            flags=gpu_service_lock(); gpu_service_owners[slot].busy=false; gpu_service_unlock(flags);
        }
        retry|=d3dkmt_collect_budget()!=0;
        __atomic_store_n(&gpu_service_retry,retry,__ATOMIC_RELEASE);
        /* Bits arriving during collection remain set. wait() arms/rechecks;
         * no application call is needed to finish notified retirement. */
        if (sched_signal_wait(&gpu_service_signal,retry ? 1 : 0)!=SCHED_SIGNAL_READY) sched_yield();
    }
}
static NTSTATUS gpu_service_start(void)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
#ifdef __KERNEL_X86__
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!(flags&(1ULL<<9))) return STATUS_NOT_SUPPORTED;
#endif
    if (__atomic_load_n(&gpu_service_state,__ATOMIC_ACQUIRE)==2) return STATUS_SUCCESS;
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&gpu_service_state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return D3DKMT_STATUS_DEVICE_BUSY;
    if (!d3dkmt_install_retirement_waker(gpu_service_kmt_wake)) {
        __atomic_store_n(&gpu_service_state,0,__ATOMIC_RELEASE);
        return STATUS_NOT_SUPPORTED;
    }
    if (gpu_service_pid<=0)
        gpu_service_pid=sched_spawn_detached_in_address_space_blocked("intel-gpu-gc",
            gpu_service_worker,paging_get_kernel_cr3(),0);
    bool started=gpu_service_pid>0 && proc_wake_pid(gpu_service_pid)==0;
    __atomic_store_n(&gpu_service_state,started ? 2 : 0,__ATOMIC_RELEASE);
    return started ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
static NTSTATUS gpu_service_attach_owner(intel_gpu_memory_t *memory,intel_gpu_contexts_t *contexts,
    intel_gpu_jobs_t *jobs,intel_gpu_fences_t *fences,uint64_t *ticket)
{
    if (!memory || memory->self!=memory || !memory->registered || memory->closing ||
        !contexts || contexts->self!=contexts || contexts->closing || contexts->memory!=memory ||
        contexts->wake!=intel_gpu_service_notify || contexts->wake_context || !ticket)
        return STATUS_INVALID_PARAMETER;
    if (jobs && (jobs->self!=jobs || jobs->contexts!=contexts || jobs->closing)) return STATUS_INVALID_PARAMETER;
    if (fences && (fences->self!=fences || fences->memory!=memory || fences->closing ||
        fences->wake!=intel_gpu_service_notify || fences->wake_context)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=gpu_service_start();
    if (status!=STATUS_SUCCESS) return status;
    uint64_t flags=gpu_service_lock();
    unsigned empty=INTEL_GPU_SERVICE_OWNERS;
    for (unsigned i=0;i<INTEL_GPU_SERVICE_OWNERS;i++) {
        if (gpu_service_owners[i].memory==memory || gpu_service_owners[i].contexts==contexts) {
            gpu_service_unlock(flags); return STATUS_INVALID_PARAMETER;
        }
        if (!gpu_service_owners[i].ticket) empty=i;
    }
    if (empty==INTEL_GPU_SERVICE_OWNERS || gpu_service_next==UINT64_MAX) {
        gpu_service_unlock(flags); return STATUS_INSUFFICIENT_RESOURCES;
    }
    gpu_service_owners[empty].memory=memory; gpu_service_owners[empty].contexts=contexts;
    gpu_service_owners[empty].jobs=jobs;
    gpu_service_owners[empty].fences=fences;
    *ticket=gpu_service_owners[empty].ticket=gpu_service_next++;
    gpu_service_unlock(flags);
    intel_gpu_service_notify(NULL);
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_service_attach(intel_gpu_memory_t *memory,intel_gpu_contexts_t *contexts,uint64_t *ticket)
{
    return gpu_service_attach_owner(memory,contexts,NULL,NULL,ticket);
}
NTSTATUS intel_gpu_service_attach_jobs(intel_gpu_memory_t *memory,intel_gpu_contexts_t *contexts,
    intel_gpu_jobs_t *jobs,uint64_t *ticket)
{
    if (!jobs) return STATUS_INVALID_PARAMETER;
    return gpu_service_attach_owner(memory,contexts,jobs,NULL,ticket);
}
NTSTATUS intel_gpu_service_attach_fences(intel_gpu_memory_t *memory,intel_gpu_contexts_t *contexts,
    intel_gpu_jobs_t *jobs,intel_gpu_fences_t *fences,uint64_t *ticket)
{
    if (!fences) return STATUS_INVALID_PARAMETER;
    return gpu_service_attach_owner(memory,contexts,jobs,fences,ticket);
}
NTSTATUS intel_gpu_service_detach(uint64_t ticket)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!ticket) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_service_lock();
    for (unsigned i=0;i<INTEL_GPU_SERVICE_OWNERS;i++) if (gpu_service_owners[i].ticket==ticket) {
        if (gpu_service_owners[i].busy ||
            (gpu_service_owners[i].jobs && !intel_gpu_jobs_empty(gpu_service_owners[i].jobs)) ||
            (gpu_service_owners[i].fences && !intel_gpu_fences_empty(gpu_service_owners[i].fences)) ||
            !intel_gpu_contexts_empty(gpu_service_owners[i].contexts) ||
            gpu_service_owners[i].memory->registered) {
            gpu_service_unlock(flags); return D3DKMT_STATUS_DEVICE_BUSY;
        }
        gpu_service_owners[i].memory=NULL; gpu_service_owners[i].contexts=NULL;
        gpu_service_owners[i].jobs=NULL;
        gpu_service_owners[i].fences=NULL;
        gpu_service_owners[i].ticket=0;
        gpu_service_unlock(flags); return STATUS_SUCCESS;
    }
    gpu_service_unlock(flags); return STATUS_INVALID_PARAMETER;
}
BOOL intel_gpu_service_idle(void)
{
    return __atomic_load_n(&gpu_service_state,__ATOMIC_ACQUIRE)==2 &&
        __atomic_load_n(&gpu_service_signal.waiters,__ATOMIC_ACQUIRE)!=0 &&
        !__atomic_load_n(&gpu_service_retry,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gpu_service_signal.bits,__ATOMIC_ACQUIRE);
}
