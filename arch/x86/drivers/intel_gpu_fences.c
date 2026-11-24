#include "intel_gpu_fences.h"
#ifdef __KERNEL_X86__
#include "../win32/ntprocess.h"
#else
extern NTSTATUS nt_process_resolve_vm_pid(HANDLE,ACCESS_MASK,ULONG *);
#endif

static uint64_t gpu_fence_lock(intel_gpu_fences_t *m)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&m->lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void gpu_fence_unlock(intel_gpu_fences_t *m,uint64_t flags)
{
    __atomic_store_n(&m->lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
static bool gpu_fence_bsp(void)
{
#ifdef __KERNEL_X86__
    extern uint32_t smp_current_cpu(void);
    return smp_current_cpu()==0;
#else
    return true;
#endif
}
static void gpu_fence_mapping_notify(intel_gpu_fences_t *m)
{
    void (*wake)(void *)=__atomic_load_n(&m->mapping_wake,__ATOMIC_ACQUIRE);
    if (wake) wake(NULL);
}
static NTSTATUS gpu_fence_status(enum intel_gpu_buffer_status result)
{
    return result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
        result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
        result==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_NO_MEMORY :
        result==INTEL_GPU_BUFFER_UNSUPPORTED ? STATUS_NOT_SUPPORTED :
        result==INTEL_GPU_BUFFER_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
}
/* Owner -> memory -> buffer metadata only; none of these checks calls out. */
static void gpu_fence_refresh(intel_gpu_fence_t *s)
{
    uint64_t epoch=0;
    if (s->id && !s->closing &&
        (intel_gpu_memory_device_epoch(s->owner->memory,s->pid,s->device,&epoch)!=INTEL_GPU_BUFFER_OK ||
         epoch!=s->epoch)) s->closing=true;
}
static void gpu_fence_retired(NT_VM_DRIVER_VIEW *view,BOOL safe,PVOID opaque)
{
    intel_gpu_fence_t *s=opaque;
    intel_gpu_fences_t *m=s->owner;
    uint64_t flags=gpu_fence_lock(m);
    m->callbacks++;
    if (view==&s->vm && s->id && !s->vm_done) {
        s->vm_done=true; s->closing=true; s->quarantined=!safe;
    } else s->quarantined=true;
    gpu_fence_unlock(m,flags);
    gpu_fence_mapping_notify(m);
    m->wake(m->wake_context);
    /* The record can now be reclaimed, but empty() retains the manager. */
    flags=gpu_fence_lock(m); m->callbacks--; gpu_fence_unlock(m,flags);
}
/* Caller owns busy. VM retirement is independent of outstanding GPU leases;
 * backing release waits for both. Allocator/cache/VM callbacks run unlocked. */
static NTSTATUS gpu_fence_finish(intel_gpu_fence_t *s)
{
    intel_gpu_fences_t *m=s->owner;
    uint64_t flags=gpu_fence_lock(m);
    bool done=s->vm_done,bad=s->quarantined;
    gpu_fence_unlock(m,flags);
    if (!done && !bad && gpu_fence_bsp()) {
        NTSTATUS status=nt_vm_unmap_driver_view(&s->vm);
        flags=gpu_fence_lock(m);
        if (status!=STATUS_SUCCESS && !s->vm_done) s->quarantined=true;
        done=s->vm_done; bad=s->quarantined;
        gpu_fence_unlock(m,flags);
    }
    NTSTATUS status=D3DKMT_STATUS_DEVICE_BUSY;
    if (done && !bad) {
        if (s->vm_reference.self && intel_gpu_buffer_drop_coherent(&s->vm_reference)!=INTEL_GPU_BUFFER_OK)
            bad=true;
        flags=gpu_fence_lock(m); bool users=s->references!=0; gpu_fence_unlock(m,flags);
        if (!bad && !users) {
            enum intel_gpu_buffer_status result=INTEL_GPU_BUFFER_OK;
            if (s->coherent.self) result=intel_gpu_memory_release_coherent(&s->coherent);
            status=gpu_fence_status(result);
            if (s->coherent.self && result!=INTEL_GPU_BUFFER_BUSY) bad=true;
            if (!s->coherent.self && s->resource) {
                if (intel_gpu_memory_close(m->memory,s->pid,s->device,s->resource)!=INTEL_GPU_BUFFER_OK ||
                    intel_gpu_memory_release_resource(m->memory,s->pid,s->device,s->resource)!=INTEL_GPU_BUFFER_OK)
                    bad=true;
                else s->resource=0;
            }
        }
    }
    flags=gpu_fence_lock(m);
    s->quarantined|=bad;
    if (done && !s->quarantined && !s->references && !s->vm_reference.self &&
        !s->coherent.self && !s->resource && !s->retained)
        *s=(intel_gpu_fence_t){.owner=m};
    else s->busy=false;
    bad=s->quarantined;
    gpu_fence_unlock(m,flags);
    return bad ? STATUS_UNSUCCESSFUL : status;
}
bool intel_gpu_fences_init(intel_gpu_fences_t *m,intel_gpu_memory_t *memory,
    void (*wake)(void *),void *context)
{
    if (!m || m->self || !memory || memory->self!=memory || !memory->registered || memory->closing || !wake)
        return false;
    m->self=m; m->memory=memory; m->wake=wake; m->wake_context=context; m->next_id=1;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) m->fences[i].owner=m;
    return true;
}
static NTSTATUS gpu_fence_create(intel_gpu_fences_t *m,DWORD pid,DWORD device,
    uint64_t initial,BOOL compat32,uint64_t *cookie,uint64_t *cpu_address,
    D3DKMT_MONITORED_BACKING *monitored)
{
    if (!m || m->self!=m || !pid || !device || !cookie || !cpu_address) return STATUS_INVALID_PARAMETER;
    if (!gpu_fence_bsp()) return STATUS_NOT_SUPPORTED;
    ULONG current=0;
    NTSTATUS status=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&current);
    if (status!=STATUS_SUCCESS) return status;
    if (current!=pid) return STATUS_INVALID_PARAMETER;
    uint64_t epoch=0;
    if (intel_gpu_memory_device_epoch(m->memory,pid,device,&epoch)!=INTEL_GPU_BUFFER_OK)
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_fence_lock(m);
    intel_gpu_fence_t *s=NULL;
    if (!m->closing && m->next_id && m->next_id<=INTEL_GPU_FENCE_VA_PAGES)
        for (unsigned i=0;i<INTEL_GPU_FENCES;i++) if (!m->fences[i].id) { s=&m->fences[i]; break; }
    if (!s) {
        status=m->closing ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_INSUFFICIENT_RESOURCES;
        gpu_fence_unlock(m,flags); return status;
    }
    *s=(intel_gpu_fence_t){.owner=m,.id=m->next_id++,.epoch=epoch,.pid=pid,.device=device,
        .busy=true,.retained=true,.vm_done=true};
    gpu_fence_unlock(m,flags);
    enum intel_gpu_buffer_status result=intel_gpu_memory_create_retained(m->memory,pid,device,4096,&s->resource);
    if (result==INTEL_GPU_BUFFER_OK)
        result=intel_gpu_memory_acquire_coherent(m->memory,pid,device,s->resource,&s->coherent);
    if (result==INTEL_GPU_BUFFER_OK) {
        __atomic_store_n((uint64_t *)s->coherent.access->cpu,initial,__ATOMIC_RELEASE);
        result=intel_gpu_memory_retain_coherent(&s->coherent,&s->vm_reference);
    }
    status=gpu_fence_status(result);
    if (status==STATUS_SUCCESS) {
        flags=gpu_fence_lock(m); s->vm_done=false; gpu_fence_unlock(m,flags);
        /* The WDDM x86 mapping permits cmpxchg8b-based atomic reads. Private
         * diagnostic views retain their original read-only contract. */
        status=monitored && compat32 ? nt_vm_map_driver_view(&s->vm,s->coherent.access->physical,
            4096,compat32,TRUE,gpu_fence_retired,s) :
            nt_vm_map_coherent_driver_view(&s->vm,s->coherent.access->physical,4096,compat32,gpu_fence_retired,s);
        flags=gpu_fence_lock(m);
        if (!s->vm.self) s->vm_done=true;
        if (status!=STATUS_SUCCESS && !s->vm_done) s->quarantined=true;
        gpu_fence_unlock(m,flags);
    }
    flags=gpu_fence_lock(m);
    gpu_fence_refresh(s);
    if (status==STATUS_SUCCESS && !m->closing && !s->closing && !s->vm_done) {
        *cookie=s->id; *cpu_address=s->vm.address; s->busy=false;
        if (monitored) *monitored=(D3DKMT_MONITORED_BACKING){s->id,s->vm.address,
            INTEL_GPU_FENCE_VA_BASE+(s->id-1)*4096,(volatile uint64_t *)s->coherent.access->cpu};
        gpu_fence_unlock(m,flags); return STATUS_SUCCESS;
    }
    if (status==STATUS_SUCCESS) status=STATUS_PROCESS_IS_TERMINATING;
    s->closing=true; s->retained=false;
    gpu_fence_unlock(m,flags);
    (void)gpu_fence_finish(s); m->wake(m->wake_context);
    return status;
}
NTSTATUS intel_gpu_fence_create(intel_gpu_fences_t *m,DWORD pid,DWORD device,
    uint64_t initial,BOOL compat32,uint64_t *cookie,uint64_t *cpu_address)
{ return gpu_fence_create(m,pid,device,initial,compat32,cookie,cpu_address,NULL); }

NTSTATUS intel_gpu_fence_create_monitored(intel_gpu_fences_t *m,DWORD pid,DWORD device,
    uint64_t initial,BOOL compat32,D3DKMT_MONITORED_BACKING *out)
{
    if (!out || (compat32!=FALSE && compat32!=TRUE)) return STATUS_INVALID_PARAMETER;
    uint64_t cookie=0,cpu=0;
    return gpu_fence_create(m,pid,device,initial,compat32,&cookie,&cpu,out);
}
NTSTATUS intel_gpu_fence_close(intel_gpu_fences_t *m,DWORD pid,DWORD device,uint64_t cookie)
{
    if (!m || m->self!=m || !pid || !device || !cookie) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_fence_lock(m);
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        if (s->id!=cookie || s->pid!=pid || s->device!=device || !s->retained) continue;
        s->retained=false; s->closing=true;
        gpu_fence_unlock(m,flags); gpu_fence_mapping_notify(m); m->wake(m->wake_context); return STATUS_SUCCESS;
    }
    gpu_fence_unlock(m,flags); return STATUS_INVALID_PARAMETER;
}
static NTSTATUS gpu_fence_lease_locked(intel_gpu_fence_t *s,intel_gpu_fence_lease_t *lease)
{
    if (s->references==UINT32_MAX) return D3DKMT_STATUS_DEVICE_BUSY;
    intel_gpu_coherent_access_t *a=s->coherent.access;
    NTSTATUS status=gpu_fence_status(intel_gpu_buffer_retain_coherent(a,&lease->reference));
    if (status) return status;
    s->references++;
    lease->self=lease;lease->owner=s->owner;lease->buffer=s->coherent.buffer;lease->cpu=a->cpu;
    lease->id=s->id;lease->epoch=s->epoch;lease->physical=a->physical;lease->dma=a->dma;
    lease->bytes=a->bytes;lease->pid=s->pid;lease->device=s->device;lease->resource=s->resource;
    lease->gpu=INTEL_GPU_FENCE_VA_BASE+(s->id-1)*4096;
    return STATUS_SUCCESS;
}
bool intel_gpu_fences_watch_mappings(intel_gpu_fences_t *m,void (*wake)(void *))
{
    if (!m || m->self!=m || !wake) return false;
    uint64_t flags=gpu_fence_lock(m);
    bool ready=!m->closing && (!m->mapping_wake || m->mapping_wake==wake);
    if (ready) __atomic_store_n(&m->mapping_wake,wake,__ATOMIC_RELEASE);
    gpu_fence_unlock(m,flags);return ready;
}
NTSTATUS intel_gpu_fences_capture(intel_gpu_fences_t *m,DWORD pid,DWORD device,uint64_t epoch,
    intel_gpu_fence_lease_t *leases,unsigned capacity,unsigned *count)
{
    if (!m || m->self!=m || !leases || !count || *count || !capacity || capacity>INTEL_GPU_FENCES)
        return STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<capacity;i++) if (leases[i].self || leases[i].reference.self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_fence_lock(m),current=0;
    NTSTATUS status=m->closing ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
    if (!status && (intel_gpu_memory_device_epoch(m->memory,pid,device,&current)!=INTEL_GPU_BUFFER_OK || current!=epoch))
        status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;!status && i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        if (!s->id || s->pid!=pid || s->device!=device) continue;
        gpu_fence_refresh(s);
        if (s->closing || !s->retained || s->vm_done) continue;
        if (s->epoch!=epoch || s->quarantined) { status=STATUS_UNSUCCESSFUL;break; }
        if (s->busy) { status=D3DKMT_STATUS_DEVICE_BUSY;break; }
        if (*count==capacity) { status=STATUS_INSUFFICIENT_RESOURCES;break; }
        status=gpu_fence_lease_locked(s,&leases[*count]);
        if (!status) (*count)++;
    }
    gpu_fence_unlock(m,flags);return status;
}
bool intel_gpu_fence_closed(const intel_gpu_fence_lease_t *lease)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner) return true;
    intel_gpu_fences_t *m=lease->owner;
    uint64_t flags=gpu_fence_lock(m);bool closed=true;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        if (!s->id || s->id!=lease->id || s->pid!=lease->pid || s->device!=lease->device) continue;
        gpu_fence_refresh(s);closed=m->closing || s->closing || !s->retained || s->vm_done || s->quarantined;break;
    }
    gpu_fence_unlock(m,flags);return closed;
}
NTSTATUS intel_gpu_fence_acquire(intel_gpu_fences_t *m,DWORD pid,DWORD device,uint64_t cookie,
    intel_gpu_fence_lease_t *lease)
{
    if (!m || m->self!=m || !lease || lease->self || lease->reference.self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_fence_lock(m);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        if (!s->id || s->id!=cookie || s->pid!=pid || s->device!=device) continue;
        gpu_fence_refresh(s);
        if (m->closing || s->closing || !s->retained || s->vm_done || s->quarantined) break;
        if (s->busy || s->references==UINT32_MAX) { status=D3DKMT_STATUS_DEVICE_BUSY; break; }
        status=gpu_fence_lease_locked(s,lease);
        break;
    }
    gpu_fence_unlock(m,flags); return status;
}
NTSTATUS intel_gpu_fence_release(intel_gpu_fence_lease_t *lease)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner)
        return STATUS_INVALID_PARAMETER;
    intel_gpu_fences_t *m=lease->owner;
    uint64_t flags=gpu_fence_lock(m);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        if (!s->id || s->id!=lease->id || s->pid!=lease->pid || s->device!=lease->device ||
            !s->references || lease->reference.access!=s->coherent.access) continue;
        status=gpu_fence_status(intel_gpu_buffer_drop_coherent(&lease->reference));
        if (!status) { s->references--; *lease=(intel_gpu_fence_lease_t){0}; }
        break;
    }
    gpu_fence_unlock(m,flags);
    if (!status) m->wake(m->wake_context);
    return status;
}
NTSTATUS intel_gpu_fence_retain(const intel_gpu_fence_lease_t *source,intel_gpu_fence_lease_t *lease)
{
    if (!source || source->self!=source || !source->owner || source->owner->self!=source->owner ||
        source->reference.self!=&source->reference || !lease || lease->self || lease->reference.self)
        return STATUS_INVALID_PARAMETER;
    intel_gpu_fences_t *m=source->owner;
    uint64_t flags=gpu_fence_lock(m);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        if (!s->id || s->id!=source->id || s->pid!=source->pid || s->device!=source->device ||
            s->epoch!=source->epoch || !s->references || source->reference.access!=s->coherent.access) continue;
        if (s->quarantined || s->references==UINT32_MAX) { status=D3DKMT_STATUS_DEVICE_BUSY; break; }
        status=gpu_fence_lease_locked(s,lease);
        break;
    }
    gpu_fence_unlock(m,flags);return status;
}
enum intel_bcs_status intel_gpu_fence_map(const intel_gpu_fence_lease_t *lease,
    intel_bcs_context_t *c,intel_gpu_memory_t *memory,DWORD device,const intel_ppgtt_pat_t *policy,
    intel_gpu_fence_mapping_t *mapping)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner ||
        lease->owner->memory!=memory || !device || lease->device!=device ||
        !c || !c->ready || !c->queue || !c->queue_id || c->owner!=lease->pid || c->epoch!=lease->epoch ||
        !mapping || mapping->self || mapping->lease.self || mapping->lease.reference.self)
        return INTEL_BCS_INVALID;
    /* Do not adopt a pre-existing mapping owned by somebody else. */
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++)
        if (c->coherent[i].reference.self && (c->coherent[i].gpu==lease->gpu ||
            c->coherent[i].reference.access==lease->reference.access)) return INTEL_BCS_INVALID;
    NTSTATUS retained=intel_gpu_fence_retain(lease,&mapping->lease);
    if (retained) return retained==D3DKMT_STATUS_DEVICE_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    mapping->self=mapping;mapping->context=c;mapping->queue_id=c->queue_id;
    enum intel_bcs_status status=intel_bcs_context_bind_coherent(c,mapping->lease.reference.access,
        mapping->lease.gpu,policy);
    if (status!=INTEL_BCS_OK && status!=INTEL_BCS_QUARANTINED) {
        enum intel_bcs_status closed=intel_gpu_fence_unmap(mapping);
        if (closed!=INTEL_BCS_OK) return closed;
    }
    return status;
}
enum intel_bcs_status intel_gpu_fence_unmap(intel_gpu_fence_mapping_t *mapping)
{
    if (!mapping) return INTEL_BCS_INVALID;
    if (!mapping->self) return mapping->lease.self ? INTEL_BCS_INVALID : INTEL_BCS_OK;
    if (mapping->self!=mapping || mapping->lease.self!=&mapping->lease || !mapping->context)
        return INTEL_BCS_INVALID;
    intel_bcs_context_t *c=mapping->context;
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        intel_bcs_coherent_range_t *range=&c->coherent[i];
        if (!range->reference.self || range->reference.access!=mapping->lease.reference.access) continue;
        if (!c->ready || c->queue_id!=mapping->queue_id || range->gpu!=mapping->lease.gpu || !range->mapped)
            return INTEL_BCS_QUARANTINED;
        enum intel_bcs_status status=intel_bcs_context_unbind_coherent(c,range->gpu);
        if (status!=INTEL_BCS_OK) return status;
    }
    NTSTATUS released=intel_gpu_fence_release(&mapping->lease);
    if (released) return released==D3DKMT_STATUS_DEVICE_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    *mapping=(intel_gpu_fence_mapping_t){0};return INTEL_BCS_OK;
}
NTSTATUS intel_gpu_fences_collect(intel_gpu_fences_t *m)
{
    if (!m || m->self!=m) return STATUS_INVALID_PARAMETER;
    bool bsp=gpu_fence_bsp();
    uint64_t flags=gpu_fence_lock(m);
    bool pending=m->callbacks!=0;
    intel_gpu_fence_t *s=NULL;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        unsigned index=(m->cursor+i)%INTEL_GPU_FENCES;
        intel_gpu_fence_t *c=&m->fences[index];
        gpu_fence_refresh(c);
        if (!c->id || !c->closing) continue;
        pending=true;
        if (c->busy || c->quarantined || (!c->vm_done && !bsp) ||
            (c->vm_done && !c->vm_reference.self && c->references) || (!c->resource && c->retained)) continue;
        s=c; s->busy=true; m->cursor=(index+1)%INTEL_GPU_FENCES; break;
    }
    gpu_fence_unlock(m,flags);
    NTSTATUS status=s ? gpu_fence_finish(s) : pending ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (pending) gpu_fence_mapping_notify(m);
    return status;
}
bool intel_gpu_fences_need_retry(intel_gpu_fences_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=gpu_fence_lock(m);
    bool pending=m->callbacks!=0;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) {
        intel_gpu_fence_t *s=&m->fences[i];
        gpu_fence_refresh(s);
        if (s->id && s->closing && !s->quarantined && !s->busy &&
            (!s->vm_done || s->vm_reference.self || !s->references) &&
            (s->resource || !s->retained)) pending=true;
    }
    gpu_fence_unlock(m,flags); return pending;
}
void intel_gpu_fences_shutdown(intel_gpu_fences_t *m)
{
    if (!m || m->self!=m) return;
    uint64_t flags=gpu_fence_lock(m); m->closing=true;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) if (m->fences[i].id) m->fences[i].closing=true;
    gpu_fence_unlock(m,flags); m->wake(m->wake_context);
}
bool intel_gpu_fences_empty(intel_gpu_fences_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=gpu_fence_lock(m);
    bool empty=m->closing && !m->callbacks;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) if (m->fences[i].id) empty=false;
    gpu_fence_unlock(m,flags); return empty;
}
