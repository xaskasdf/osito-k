#include "intel_gpu_contexts.h"
#include "intel_gpu_umd_abi.h"
#ifdef __KERNEL_X86__
#include "../win32/ntprocess.h"
#else
#include <string.h>
extern NTSTATUS nt_process_resolve_vm_pid(HANDLE,ACCESS_MASK,ULONG *);
#endif

static uint64_t gpu_context_lock(intel_gpu_contexts_t *m)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&m->lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void gpu_context_unlock(intel_gpu_contexts_t *m,uint64_t flags)
{
    __atomic_store_n(&m->lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
static bool gpu_context_bsp(void)
{
#ifdef __KERNEL_X86__
    extern uint32_t smp_current_cpu(void);
    return smp_current_cpu()==0;
#else
    return true;
#endif
}
static void gpu_context_closed_notify(intel_gpu_contexts_t *m)
{
    m->wake(m->wake_context);
    void (*wake)(void *)=__atomic_load_n(&m->mapping_wake,__ATOMIC_ACQUIRE);
    if (wake) wake(NULL);
}
bool intel_gpu_contexts_watch_mappings(intel_gpu_contexts_t *m,void (*wake)(void *))
{
    if (!m || m->self!=m || !wake) return false;
    uint64_t flags=gpu_context_lock(m);
    bool ready=!m->closing && (!m->mapping_wake || m->mapping_wake==wake);
    if (ready) __atomic_store_n(&m->mapping_wake,wake,__ATOMIC_RELEASE);
    gpu_context_unlock(m,flags);return ready;
}
static void gpu_context_retired(NT_VM_DRIVER_VIEW *view,BOOL safe,PVOID opaque)
{
    intel_gpu_command_context_t *s=opaque;
    intel_gpu_contexts_t *m=s->owner;
    uint64_t flags=gpu_context_lock(m);
    m->callbacks++;
    if (view==&s->vm && s->id && !s->vm_done) {
        s->vm_done=true; s->closing=true; s->quarantined=!safe;
    } else s->quarantined=true;
    gpu_context_unlock(m,flags);
    gpu_context_closed_notify(m);
    flags=gpu_context_lock(m); m->callbacks--; gpu_context_unlock(m,flags);
}

/* Caller owns busy, no command-capture lease can appear on a closing record.
 * A VM callback may mark it closed but cannot recycle its stable storage. */
static NTSTATUS gpu_context_finish(intel_gpu_command_context_t *s)
{
    intel_gpu_contexts_t *m=s->owner;
    uint64_t flags=gpu_context_lock(m);
    bool done=s->vm_done,bad=s->quarantined;
    gpu_context_unlock(m,flags);
    if (!done && !bad && gpu_context_bsp()) {
        NTSTATUS status=nt_vm_unmap_driver_view(&s->vm);
        flags=gpu_context_lock(m);
        if (status!=STATUS_SUCCESS && !s->vm_done) s->quarantined=true;
        done=s->vm_done; bad=s->quarantined;
        gpu_context_unlock(m,flags);
    }
    NTSTATUS status=D3DKMT_STATUS_DEVICE_BUSY;
    if (done && !bad) {
        enum intel_gpu_buffer_status result=INTEL_GPU_BUFFER_OK;
        if (s->cpu.self) result=intel_gpu_memory_release_cpu(&s->cpu);
        status=result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
            result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
        if (s->cpu.self && result!=INTEL_GPU_BUFFER_BUSY) bad=true;
        if (!s->cpu.self && s->resource) {
            if (intel_gpu_memory_close(m->memory,s->pid,s->device,s->resource)!=INTEL_GPU_BUFFER_OK ||
                intel_gpu_memory_release_resource(m->memory,s->pid,s->device,s->resource)!=INTEL_GPU_BUFFER_OK)
                bad=true;
            else s->resource=0;
        }
    }
    flags=gpu_context_lock(m);
    s->quarantined|=bad;
    if (done && !s->quarantined && !s->cpu.self && !s->resource && !s->retained)
        *s=(intel_gpu_command_context_t){.owner=m};
    else s->busy=false;
    bad=s->quarantined;
    gpu_context_unlock(m,flags);
    return bad ? STATUS_UNSUCCESSFUL : status;
}

bool intel_gpu_contexts_attach_fences(intel_gpu_contexts_t *m,intel_gpu_fences_t *fences)
{
    if (!m || m->self!=m || !fences || fences->self!=fences || fences->memory!=m->memory || fences->closing)
        return false;
    uint64_t flags=gpu_context_lock(m);
    bool ready=!m->closing && !m->fences;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) if (m->contexts[i].references || m->contexts[i].busy) ready=false;
    if (ready) m->fences=fences;
    gpu_context_unlock(m,flags);return ready;
}
bool intel_gpu_contexts_init(intel_gpu_contexts_t *m,intel_gpu_memory_t *memory,
    void (*wake)(void *),void *context)
{
    if (!m || m->self || !memory || memory->self!=memory || !memory->registered || memory->closing || !wake)
        return false;
    m->self=m; m->memory=memory; m->wake=wake; m->wake_context=context; m->next_id=1;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) m->contexts[i].owner=m;
    return true;
}

NTSTATUS intel_gpu_context_create(intel_gpu_contexts_t *m,DWORD pid,DWORD device,
    const D3DKMT_CONTEXT_DESCRIPTION *description,uint64_t *cookie,D3DKMT_CONTEXT_BUFFERS *buffers)
{
    if (!m || m->self!=m || !pid || !device || !description || !cookie || !buffers)
        return STATUS_INVALID_PARAMETER;
    if (!gpu_context_bsp()) return STATUS_NOT_SUPPORTED;
    if (description->node>INTEL_UMD_NODE_COPY || description->engine || description->flags ||
        description->private_bytes!=sizeof(intel_umd_context_description_t)) return STATUS_NOT_SUPPORTED;
    intel_umd_context_description_t layout;
    memcpy(&layout,description->private_data,sizeof(layout));
    if (layout.magic!=INTEL_UMD_CONTEXT_MAGIC || layout.version!=INTEL_UMD_CONTEXT_VERSION)
        return STATUS_NOT_SUPPORTED;
    if (layout.reserved || layout.command_bytes<256 || (layout.command_bytes&3) ||
        layout.command_bytes>INTEL_UMD_CONTEXT_MAX_COMMAND_BYTES ||
        !layout.allocation_count || layout.allocation_count>INTEL_UMD_CONTEXT_MAX_ALLOCATIONS ||
        !layout.patch_count || layout.patch_count>INTEL_UMD_CONTEXT_MAX_PATCHES)
        return STATUS_INVALID_PARAMETER;
    ULONG current=0;
    NTSTATUS status=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&current);
    if (status!=STATUS_SUCCESS) return status;
    if (current!=pid) return STATUS_INVALID_PARAMETER;
    uint64_t epoch=0;
    if (intel_gpu_memory_device_epoch(m->memory,pid,device,&epoch)!=INTEL_GPU_BUFFER_OK)
        return STATUS_INVALID_PARAMETER;
    DWORD allocations=(layout.command_bytes+63)&~63u;
    DWORD patches=(allocations+layout.allocation_count*8+63)&~63u;
    DWORD total=patches+layout.patch_count*24;
    uint64_t flags=gpu_context_lock(m);
    intel_gpu_command_context_t *s=NULL;
    if (!m->closing && m->next_id!=UINT64_MAX)
        for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) if (!m->contexts[i].id) { s=&m->contexts[i]; break; }
    if (!s) {
        status=m->closing ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_INSUFFICIENT_RESOURCES;
        gpu_context_unlock(m,flags); return status;
    }
    *s=(intel_gpu_command_context_t){.owner=m,.id=m->next_id++,.pid=pid,.device=device,.epoch=epoch,
        .node=description->node,.command_bytes=layout.command_bytes,.allocation_count=layout.allocation_count,
        .patch_count=layout.patch_count,.allocation_offset=allocations,.patch_offset=patches,
        .busy=true,.retained=true,.vm_done=true};
    gpu_context_unlock(m,flags);
    enum intel_gpu_buffer_status result=intel_gpu_memory_create_retained(m->memory,pid,device,total,&s->resource);
    if (result==INTEL_GPU_BUFFER_OK)
        result=intel_gpu_memory_acquire_cpu(m->memory,pid,device,s->resource,&s->cpu);
    uint64_t physical=0,bytes=0;
    if (result==INTEL_GPU_BUFFER_OK)
        result=intel_gpu_buffer_cpu_physical(s->cpu.buffer->cpu_access,&physical,&bytes);
    status=result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
        result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
        result==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_NO_MEMORY :
        result==INTEL_GPU_BUFFER_UNSUPPORTED ? STATUS_NOT_SUPPORTED :
        result==INTEL_GPU_BUFFER_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
    if (status==STATUS_SUCCESS) {
        flags=gpu_context_lock(m); s->vm_done=false; gpu_context_unlock(m,flags);
        status=nt_vm_map_driver_view(&s->vm,physical,bytes,description->compat32,TRUE,gpu_context_retired,s);
        flags=gpu_context_lock(m);
        if (!s->vm.self) s->vm_done=true;
        if (status!=STATUS_SUCCESS && !s->vm_done) s->quarantined=true;
        gpu_context_unlock(m,flags);
    }
    flags=gpu_context_lock(m);
    if (status==STATUS_SUCCESS && !m->closing && !s->closing && !s->vm_done) {
        *cookie=s->id;
        *buffers=(D3DKMT_CONTEXT_BUFFERS){s->vm.address,s->vm.address+allocations,s->vm.address+patches,
            0,layout.command_bytes,layout.allocation_count,layout.patch_count};
        s->busy=false;
        gpu_context_unlock(m,flags); return STATUS_SUCCESS;
    }
    if (status==STATUS_SUCCESS) status=STATUS_PROCESS_IS_TERMINATING;
    s->closing=true; s->retained=false;
    gpu_context_unlock(m,flags);
    (void)gpu_context_finish(s); m->wake(m->wake_context);
    return status;
}

NTSTATUS intel_gpu_context_close(intel_gpu_contexts_t *m,DWORD pid,DWORD device,uint64_t cookie)
{
    if (!m || m->self!=m || !pid || !device || !cookie) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_context_lock(m);
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
        intel_gpu_command_context_t *s=&m->contexts[i];
        if (s->id!=cookie || s->pid!=pid || s->device!=device || !s->retained) continue;
        s->retained=false; s->closing=true;
        gpu_context_unlock(m,flags); gpu_context_closed_notify(m); return STATUS_SUCCESS;
    }
    gpu_context_unlock(m,flags); return STATUS_INVALID_PARAMETER;
}

NTSTATUS intel_gpu_context_acquire(intel_gpu_contexts_t *m,DWORD pid,DWORD device,uint64_t cookie,
    intel_gpu_context_lease_t *lease)
{
    if (!m || m->self!=m || !lease || lease->self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=gpu_context_lock(m);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
        intel_gpu_command_context_t *s=&m->contexts[i];
        if (!s->id || s->id!=cookie || s->pid!=pid || s->device!=device) continue;
        if (m->closing || s->closing || !s->retained || s->vm_done || s->quarantined) break;
        if (s->busy || s->references==UINT32_MAX) { status=D3DKMT_STATUS_DEVICE_BUSY; break; }
        s->references++;
        *lease=(intel_gpu_context_lease_t){lease,m,cookie,pid,device,s->node,s->cpu.cpu,
            (BYTE *)s->cpu.cpu+s->allocation_offset,(BYTE *)s->cpu.cpu+s->patch_offset,
            s->command_bytes,s->allocation_count,s->patch_count,s->epoch};
        status=STATUS_SUCCESS; break;
    }
    gpu_context_unlock(m,flags); return status;
}

NTSTATUS intel_gpu_context_release(intel_gpu_context_lease_t *lease)
{
    if (!lease || lease->self!=lease || !lease->owner || lease->owner->self!=lease->owner)
        return STATUS_INVALID_PARAMETER;
    intel_gpu_contexts_t *m=lease->owner;
    uint64_t flags=gpu_context_lock(m);
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
        intel_gpu_command_context_t *s=&m->contexts[i];
        if (!s->id || s->id!=lease->id || s->pid!=lease->pid || s->device!=lease->device ||
            !s->references || s->cpu.cpu!=lease->commands) continue;
        s->references--; *lease=(intel_gpu_context_lease_t){0};
        gpu_context_unlock(m,flags); m->wake(m->wake_context); return STATUS_SUCCESS;
    }
    gpu_context_unlock(m,flags); return STATUS_INVALID_PARAMETER;
}

NTSTATUS intel_gpu_contexts_collect(intel_gpu_contexts_t *m)
{
    if (!m || m->self!=m) return STATUS_INVALID_PARAMETER;
    bool bsp=gpu_context_bsp();
    uint64_t flags=gpu_context_lock(m);
    bool pending=m->callbacks!=0;
    intel_gpu_command_context_t *s=NULL;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
        unsigned index=(m->cursor+i)%INTEL_GPU_CONTEXTS;
        intel_gpu_command_context_t *c=&m->contexts[index];
        if (!c->id || !c->closing) continue;
        pending=true;
        if (c->busy || c->references || c->quarantined || (!c->vm_done && !bsp) ||
            (!c->resource && c->retained)) continue;
        s=c; s->busy=true; m->cursor=(index+1)%INTEL_GPU_CONTEXTS; break;
    }
    gpu_context_unlock(m,flags);
    return s ? gpu_context_finish(s) : pending ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
bool intel_gpu_contexts_need_retry(intel_gpu_contexts_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=gpu_context_lock(m);
    bool pending=m->callbacks!=0;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
        intel_gpu_command_context_t *s=&m->contexts[i];
        if (s->id && s->closing && !s->quarantined && !s->busy && !s->references &&
            (s->resource || !s->retained)) pending=true;
    }
    gpu_context_unlock(m,flags); return pending;
}
void intel_gpu_contexts_shutdown(intel_gpu_contexts_t *m)
{
    if (!m || m->self!=m) return;
    uint64_t flags=gpu_context_lock(m); m->closing=true;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) if (m->contexts[i].id) m->contexts[i].closing=true;
    gpu_context_unlock(m,flags); gpu_context_closed_notify(m);
}
bool intel_gpu_context_closed(intel_gpu_contexts_t *m,DWORD pid,DWORD device,uint64_t id)
{
    if (!m || m->self!=m || !pid || !device || !id) return false;
    uint64_t flags=gpu_context_lock(m);
    bool closed=id<m->next_id;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) {
        intel_gpu_command_context_t *s=&m->contexts[i];
        if (s->id==id) {
            closed=s->pid==pid && s->device==device &&
                (m->closing || s->closing || s->vm_done || !s->retained);
            break;
        }
    }
    gpu_context_unlock(m,flags); return closed;
}

bool intel_gpu_contexts_empty(intel_gpu_contexts_t *m)
{
    if (!m || m->self!=m) return false;
    uint64_t flags=gpu_context_lock(m);
    bool empty=m->closing && !m->callbacks;
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) if (m->contexts[i].id) empty=false;
    gpu_context_unlock(m,flags); return empty;
}
