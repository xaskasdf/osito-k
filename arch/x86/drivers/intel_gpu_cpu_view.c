#include "intel_gpu_cpu_view.h"
#ifdef __KERNEL_X86__
#include "../win32/ntprocess.h"
#else
#include <string.h>
extern NTSTATUS nt_process_resolve_vm_pid(HANDLE,ACCESS_MASK,ULONG *);
#endif

static uint64_t cpu_view_lock(intel_gpu_cpu_views_t *m)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&m->lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void cpu_view_unlock(intel_gpu_cpu_views_t *m,uint64_t flags)
{
    __atomic_store_n(&m->lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
static bool cpu_view_bsp(void)
{
#ifdef __KERNEL_X86__
    extern uint32_t smp_current_cpu(void);
    return smp_current_cpu()==0;
#else
    return true;
#endif
}

static void cpu_view_retired(NT_VM_DRIVER_VIEW *view,BOOL safe,PVOID opaque)
{
    intel_gpu_cpu_view_t *s=opaque;
    intel_gpu_cpu_views_t *m=s->owner;
    uint64_t flags=cpu_view_lock(m);
    m->callbacks++;
    if (view==&s->vm && s->ticket && !s->vm_done) {
        s->vm_done=true; s->closing=true; s->quarantined=!safe;
    } else {
        /* A duplicate/foreign callback must not authorize native release. */
        s->quarantined=true;
    }
    cpu_view_unlock(m,flags);
    m->wake(m->wake_context);
    flags=cpu_view_lock(m);
    m->callbacks--;
    cpu_view_unlock(m,flags);
}

/* Caller owns busy until this function returns. Callback metadata can change
 * concurrently, but neither a second collector nor shutdown can recycle s. */
static NTSTATUS cpu_view_finish(intel_gpu_cpu_view_t *s)
{
    intel_gpu_cpu_views_t *m=s->owner;
    uint64_t flags=cpu_view_lock(m);
    bool done=s->vm_done,quarantined=s->quarantined;
    cpu_view_unlock(m,flags);
    if (!done && !quarantined && cpu_view_bsp()) {
        NTSTATUS unmap=nt_vm_unmap_driver_view(&s->vm);
        flags=cpu_view_lock(m);
        if (unmap!=STATUS_SUCCESS && !s->vm_done)
            s->quarantined=true;
        done=s->vm_done; quarantined=s->quarantined;
        cpu_view_unlock(m,flags);
    }
    NTSTATUS status=D3DKMT_STATUS_DEVICE_BUSY;
    bool native_bad=false;
    if (done && !quarantined) {
        status=s->cpu.access.self ? intel_gpu_kmt_release_cpu(&s->cpu) : STATUS_SUCCESS;
        if (!s->cpu.access.self && !s->cpu.reference.self) {
            flags=cpu_view_lock(m);
            *s=(intel_gpu_cpu_view_t){.owner=m};
            cpu_view_unlock(m,flags);
            return status;
        }
        if (status!=D3DKMT_STATUS_DEVICE_BUSY) native_bad=true;
    }
    flags=cpu_view_lock(m);
    s->quarantined|=native_bad; s->busy=false;
    quarantined=s->quarantined;
    cpu_view_unlock(m,flags);
    return quarantined ? STATUS_UNSUCCESSFUL : status;
}

bool intel_gpu_cpu_views_init(intel_gpu_cpu_views_t *m,void (*wake)(void *),void *context)
{
    if (!m || m->self || !wake) return false;
    m->self=m; m->wake=wake; m->wake_context=context; m->next_ticket=1;
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) m->views[i].owner=m;
    return true;
}

NTSTATUS intel_gpu_cpu_view_acquire(intel_gpu_cpu_views_t *m,DWORD pid,
    OKMT_HANDLE device,OKMT_HANDLE allocation,BOOL compat32,BOOL writable,
    uint64_t *ticket,uint64_t *address)
{
    if (!m || m->self!=m || !pid || !device || !allocation || !ticket || !address)
        return STATUS_INVALID_PARAMETER;
    if (!cpu_view_bsp()) return STATUS_NOT_SUPPORTED;
    ULONG current=0;
    NTSTATUS status=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&current);
    if (status!=STATUS_SUCCESS) return status;
    if (current!=pid) return STATUS_INVALID_PARAMETER;
    uint64_t flags=cpu_view_lock(m);
    intel_gpu_cpu_view_t *s=NULL;
    if (!m->closing && m->next_ticket!=UINT64_MAX) {
        for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) {
            intel_gpu_cpu_view_t *candidate=&m->views[i];
            if (candidate->ticket && candidate->pid==pid &&
                candidate->device==device && candidate->allocation==allocation) {
                cpu_view_unlock(m,flags); return D3DKMT_STATUS_DEVICE_BUSY;
            }
            if (!candidate->ticket && !s) s=candidate;
        }
    }
    if (!s) {
        status=m->closing ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_INSUFFICIENT_RESOURCES;
        cpu_view_unlock(m,flags); return status;
    }
    *s=(intel_gpu_cpu_view_t){.owner=m,.ticket=m->next_ticket++,.pid=pid,
        .device=device,.allocation=allocation,.busy=true,.vm_done=true};
    cpu_view_unlock(m,flags);
    status=intel_gpu_kmt_acquire_cpu(pid,device,allocation,&s->cpu);
    uint64_t physical=0,bytes=0;
    if (status==STATUS_SUCCESS) status=intel_gpu_kmt_cpu_physical(&s->cpu,&physical,&bytes);
    if (status==STATUS_SUCCESS) {
        flags=cpu_view_lock(m); s->vm_done=false; cpu_view_unlock(m,flags);
        status=nt_vm_map_driver_view(&s->vm,physical,bytes,compat32,writable,
                                     cpu_view_retired,s);
        flags=cpu_view_lock(m);
        if (!s->vm.self) s->vm_done=true; /* VM never accepted our reference. */
        if (status!=STATUS_SUCCESS && !s->vm_done) s->quarantined=true;
        cpu_view_unlock(m,flags);
    }
    flags=cpu_view_lock(m);
    if (status==STATUS_SUCCESS && !m->closing && !s->closing && !s->vm_done) {
        *ticket=s->ticket; *address=s->vm.address;
        s->busy=false;
        cpu_view_unlock(m,flags);
        return STATUS_SUCCESS;
    }
    if (status==STATUS_SUCCESS) status=STATUS_PROCESS_IS_TERMINATING;
    s->closing=true;
    cpu_view_unlock(m,flags);
    (void)cpu_view_finish(s);
    m->wake(m->wake_context);
    return status;
}

NTSTATUS intel_gpu_cpu_view_release(intel_gpu_cpu_views_t *m,DWORD pid,uint64_t ticket)
{
    if (!m || m->self!=m || !pid || !ticket) return STATUS_INVALID_PARAMETER;
    if (!cpu_view_bsp()) return STATUS_NOT_SUPPORTED;
    uint64_t flags=cpu_view_lock(m);
    intel_gpu_cpu_view_t *s=NULL;
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++)
        if (m->views[i].ticket==ticket && m->views[i].pid==pid) { s=&m->views[i]; break; }
    if (!s || s->busy) {
        NTSTATUS result=s ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
        cpu_view_unlock(m,flags); return result;
    }
    s->busy=true; s->closing=true;
    cpu_view_unlock(m,flags);
    NTSTATUS status=cpu_view_finish(s);
    m->wake(m->wake_context);
    return status;
}

NTSTATUS intel_gpu_cpu_views_collect(intel_gpu_cpu_views_t *m)
{
    if (!m || m->self!=m) return STATUS_INVALID_PARAMETER;
    bool bsp=cpu_view_bsp(),pending=false;
    uint64_t flags=cpu_view_lock(m);
    /* A notifier may still be using the owner after another collector has
     * consumed the last slot. Report retry, not a drained cleanup queue. */
    pending=m->callbacks!=0;
    intel_gpu_cpu_view_t *s=NULL;
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) {
        unsigned n=(m->cursor+i)%INTEL_GPU_CPU_VIEWS;
        intel_gpu_cpu_view_t *candidate=&m->views[n];
        if (!candidate->ticket || !candidate->closing) continue;
        pending=true;
        if (candidate->busy || candidate->quarantined || (!candidate->vm_done && !bsp)) continue;
        s=candidate; s->busy=true; m->cursor=(n+1)%INTEL_GPU_CPU_VIEWS; break;
    }
    cpu_view_unlock(m,flags);
    return s ? cpu_view_finish(s) : pending ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

NTSTATUS intel_gpu_cpu_views_unlock(intel_gpu_cpu_views_t *m,DWORD pid,
    OKMT_HANDLE device,DWORD count,const OKMT_HANDLE *allocations)
{
    if (!m || m->self!=m || !pid || !device || !count ||
        count>INTEL_GPU_CPU_VIEWS || !allocations) return STATUS_INVALID_PARAMETER;
    if (!cpu_view_bsp()) return STATUS_NOT_SUPPORTED;
    intel_gpu_cpu_view_t *selected[INTEL_GPU_CPU_VIEWS];
    uint64_t flags=cpu_view_lock(m);
    for (DWORD i=0;i<count;i++) {
        selected[i]=NULL;
        for (unsigned j=0;j<INTEL_GPU_CPU_VIEWS;j++) {
            intel_gpu_cpu_view_t *s=&m->views[j];
            if (s->ticket && s->pid==pid && s->device==device &&
                s->allocation==allocations[i]) { selected[i]=s; break; }
        }
        if (!selected[i]) { cpu_view_unlock(m,flags); return STATUS_INVALID_PARAMETER; }
        for (DWORD j=0;j<i;j++) if (selected[i]==selected[j]) {
            cpu_view_unlock(m,flags); return STATUS_INVALID_PARAMETER;
        }
        if (selected[i]->busy || selected[i]->closing) {
            cpu_view_unlock(m,flags); return D3DKMT_STATUS_DEVICE_BUSY;
        }
    }
    for (DWORD i=0;i<count;i++) selected[i]->busy=selected[i]->closing=true;
    cpu_view_unlock(m,flags);
    NTSTATUS result=STATUS_SUCCESS;
    for (DWORD i=0;i<count;i++) {
        NTSTATUS status=cpu_view_finish(selected[i]);
        if (status!=STATUS_SUCCESS && result==STATUS_SUCCESS) result=status;
    }
    m->wake(m->wake_context);
    return result;
}

bool intel_gpu_cpu_views_need_retry(intel_gpu_cpu_views_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=cpu_view_lock(m);
    bool pending=m->callbacks!=0;
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) {
        intel_gpu_cpu_view_t *s=&m->views[i];
        if (s->ticket && s->closing && !s->quarantined) pending=true;
    }
    cpu_view_unlock(m,flags);
    return pending;
}

void intel_gpu_cpu_views_shutdown(intel_gpu_cpu_views_t *m)
{
    if (!m || m->self!=m) return;
    uint64_t flags=cpu_view_lock(m);
    m->closing=true;
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++)
        if (m->views[i].ticket) m->views[i].closing=true;
    cpu_view_unlock(m,flags);
    m->wake(m->wake_context);
}

bool intel_gpu_cpu_views_empty(intel_gpu_cpu_views_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=cpu_view_lock(m);
    bool empty=m->closing && !m->callbacks;
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++) if (m->views[i].ticket) empty=false;
    cpu_view_unlock(m,flags);
    return empty;
}
