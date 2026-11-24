#include "intel_gpu_kmt.h"
#include "intel_gpu_umd_abi.h"
#ifdef __KERNEL_X86__
#include "../win32/ntprocess.h"
#else
extern NTSTATUS nt_process_open_gpu_device_retained(ULONG,struct intel_gpu_memory *,ULONG *);
#endif

static NTSTATUS intel_gpu_kmt_open(void *context,DWORD pid,uint64_t *cookie)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !cookie) return STATUS_INVALID_PARAMETER;
    ULONG device=0;
    NTSTATUS status=nt_process_open_gpu_device_retained(pid,kmt->memory,&device);
    if (status==STATUS_SUCCESS) *cookie=device;
    return status;
}

static NTSTATUS intel_gpu_kmt_close(void *context,DWORD pid,uint64_t cookie)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !cookie || cookie>UINT32_MAX) return STATUS_INVALID_PARAMETER;
    /* The retained native-device reference prevents both exit collection and
     * manager unpublication from invalidating this cookie before close. No
     * allocator, MMIO operation or GPU wait is permitted in this callback. */
    if (intel_gpu_memory_close_device(kmt->memory,pid,(uint32_t)cookie)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_release_device(kmt->memory,pid,(uint32_t)cookie)!=INTEL_GPU_BUFFER_OK)
        return STATUS_INVALID_PARAMETER;
    kmt->wake(kmt->wake_context);
    return STATUS_SUCCESS;
}

static NTSTATUS intel_gpu_kmt_allocate(void *context,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *description,uint64_t *cookie)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !device || device>UINT32_MAX || !description || !cookie)
        return STATUS_INVALID_PARAMETER;
    uint32_t resource=0;
    enum intel_gpu_buffer_status result=intel_gpu_memory_create_retained(kmt->memory,pid,
        (uint32_t)device,description->bytes,&resource);
    if (result==INTEL_GPU_BUFFER_OK) { *cookie=resource; return STATUS_SUCCESS; }
    /* A failed allocation may own partial/quarantined backing. Retain it in
     * the native manager and notify its worker without freeing it here. */
    kmt->wake(kmt->wake_context);
    return result==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_NO_MEMORY :
        result==INTEL_GPU_BUFFER_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
}

static NTSTATUS intel_gpu_kmt_free_allocation(void *context,DWORD pid,uint64_t device,uint64_t cookie)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !device || device>UINT32_MAX || !cookie || cookie>UINT32_MAX)
        return STATUS_INVALID_PARAMETER;
    if (intel_gpu_memory_close(kmt->memory,pid,(uint32_t)device,(uint32_t)cookie)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_memory_release_resource(kmt->memory,pid,(uint32_t)device,(uint32_t)cookie)!=INTEL_GPU_BUFFER_OK)
        return STATUS_INVALID_PARAMETER;
    kmt->wake(kmt->wake_context);
    return STATUS_SUCCESS;
}

static NTSTATUS intel_gpu_kmt_translate_allocation(void *context,const void *data,DWORD bytes,
    D3DKMT_ALLOCATION_DESCRIPTION *description)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !data || !description) return STATUS_INVALID_PARAMETER;
    if (bytes!=sizeof(intel_umd_buffer_description_t)) return STATUS_NOT_SUPPORTED;
    /* Private blobs may be unaligned. Do not interpret a different driver's
     * layout or silently truncate its requested memory size. */
    intel_umd_buffer_description_t captured;
    const BYTE *source=data; BYTE *destination=(BYTE *)&captured;
    for (DWORD i=0;i<bytes;i++) destination[i]=source[i];
    if (captured.magic!=INTEL_UMD_BUFFER_MAGIC || captured.version!=INTEL_UMD_BUFFER_VERSION)
        return STATUS_NOT_SUPPORTED;
    if (captured.flags || captured.reserved || !captured.bytes || captured.bytes>INTEL_GPU_BUFFER_MAX_BYTES)
        return STATUS_INVALID_PARAMETER;
    *description=(D3DKMT_ALLOCATION_DESCRIPTION){captured.bytes};
    return STATUS_SUCCESS;
}

static NTSTATUS intel_gpu_kmt_properties(void *context,DWORD pid,uint64_t device,
    DWORD operation,const uint64_t *cookies,DWORD count,DWORD *values)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !device || device>UINT32_MAX)
        return STATUS_INVALID_PARAMETER;
    if (operation!=D3DKMT_PROPERTY_RESIDENCY && operation!=D3DKMT_PROPERTY_SET_PRIORITY)
        return STATUS_NOT_SUPPORTED;
    enum intel_gpu_buffer_status status=intel_gpu_memory_properties(kmt->memory,pid,
        (uint32_t)device,operation==D3DKMT_PROPERTY_RESIDENCY ? INTEL_GPU_PROPERTY_RESIDENCY :
        INTEL_GPU_PROPERTY_SET_PRIORITY,cookies,count,values);
    return status==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
        status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
}

static NTSTATUS intel_gpu_kmt_residency(void *context,DWORD pid,uint64_t device,
    BOOL make,const uint64_t *cookies,DWORD count)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !device || device>UINT32_MAX ||
        (make!=FALSE && make!=TRUE)) return STATUS_INVALID_PARAMETER;
    enum intel_gpu_buffer_status status=intel_gpu_memory_residency(kmt->memory,pid,
        (uint32_t)device,make,cookies,count);
    return status==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
        status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
        status==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER;
}

typedef struct {
    intel_gpu_memory_reclaim_work_t reclaim;
    intel_gpu_kmt_t *kmt;
    DWORD pid,device,mode,priority,options,count;
    const uint64_t *cookies;
    BOOL committed;
} INTEL_KMT_OFFER_WORK;
_Static_assert(sizeof(INTEL_KMT_OFFER_WORK)<=D3DKMT_OFFER_STORAGE_BYTES,"native recovery storage bound");

static NTSTATUS intel_gpu_kmt_offer_status(enum intel_gpu_buffer_status status)
{
    return status==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
        status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
        status==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_NO_MEMORY :
        status==INTEL_GPU_BUFFER_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
}
static NTSTATUS intel_gpu_kmt_offer_prepare(void *context,DWORD pid,uint64_t device,
    DWORD mode,DWORD priority,DWORD options,const uint64_t *cookies,DWORD count,void *storage,DWORD *values)
{
    intel_gpu_kmt_t *kmt=context;INTEL_KMT_OFFER_WORK *w=storage;
    if (!kmt || kmt->self!=kmt || !device || device>UINT32_MAX || !w || !cookies || !count ||
        count>D3DKMT_MAX_HANDLES || mode>D3DKMT_RECLAIM2 ||
        (mode==D3DKMT_OFFER ? priority<1 || priority>4 || (options&~3u) : priority || options || !values))
        return STATUS_INVALID_PARAMETER;
    w->kmt=kmt;w->pid=pid;w->device=(DWORD)device;w->mode=mode;w->priority=priority;
    w->options=options;w->count=count;w->cookies=cookies;w->committed=FALSE;
    if (mode==D3DKMT_OFFER) return STATUS_SUCCESS;
    NTSTATUS status=intel_gpu_kmt_offer_status(intel_gpu_memory_reclaim_prepare(kmt->memory,pid,
        (DWORD)device,cookies,count,&w->reclaim));
    if (!status) for (DWORD i=0;i<count;i++) values[i]=w->reclaim.discarded[i];
    return status;
}
static NTSTATUS intel_gpu_kmt_offer_commit(void *storage)
{
    INTEL_KMT_OFFER_WORK *w=storage;
    NTSTATUS status=intel_gpu_kmt_offer_status(w->mode==D3DKMT_OFFER ?
        intel_gpu_memory_offer(w->kmt->memory,w->pid,w->device,w->cookies,w->count,w->priority) :
        intel_gpu_memory_reclaim_commit(&w->reclaim));
    w->committed=!status;return status;
}
static void intel_gpu_kmt_offer_finish(void *storage)
{
    INTEL_KMT_OFFER_WORK *w=storage;
    if (w->reclaim.self) (void)intel_gpu_memory_reclaim_release(&w->reclaim);
    if (w->mode==D3DKMT_OFFER && w->committed && (w->options&1u)) {
        uint32_t released=0;
        (void)intel_gpu_memory_trim(w->kmt->memory,w->pid,UINT32_MAX,&released);
    }
    /* AllowDecommit is permission, not an instruction to invalidate a handle.
     * This backend retains commitment and only replaces discarded contents. */
    w->kmt->wake(w->kmt->wake_context);
}
static const D3DKMT_OFFER_PROVIDER intel_gpu_offer_ops={
    intel_gpu_kmt_offer_prepare,intel_gpu_kmt_offer_commit,intel_gpu_kmt_offer_finish};

bool intel_gpu_kmt_provider(intel_gpu_kmt_t *kmt,intel_gpu_memory_t *memory,
    void (*wake)(void *),void *wake_context)
{
    if (!kmt || kmt->self || !memory || memory->self!=memory ||
        !memory->initialized || !memory->registered || memory->closing || !wake) return false;
    kmt->self=kmt; kmt->memory=memory; kmt->wake=wake; kmt->wake_context=wake_context;
    kmt->device=(D3DKMT_DEVICE_PROVIDER){intel_gpu_kmt_open,intel_gpu_kmt_close,kmt,NULL,NULL,NULL};
    kmt->allocation=(D3DKMT_ALLOCATION_PROVIDER){intel_gpu_kmt_allocate,intel_gpu_kmt_free_allocation,
        intel_gpu_kmt_translate_allocation,intel_gpu_kmt_properties,intel_gpu_kmt_residency,&intel_gpu_offer_ops};
    return true;
}

NTSTATUS intel_gpu_kmt_acquire_device(DWORD pid,OKMT_HANDLE handle,intel_gpu_kmt_device_lease_t *lease)
{
    if (!lease || lease->memory || lease->device) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_acquire_device(pid,handle,&lease->reference);
    if (status!=STATUS_SUCCESS) return status;
    const D3DKMT_DEVICE_PROVIDER *provider=&lease->reference.provider;
    if (provider->open!=intel_gpu_kmt_open || provider->close!=intel_gpu_kmt_close) {
        (void)d3dkmt_release_device(&lease->reference);
        return STATUS_NOT_SUPPORTED;
    }
    intel_gpu_kmt_t *kmt=provider->context;
    if (!kmt || kmt->self!=kmt || !lease->reference.cookie || lease->reference.cookie>UINT32_MAX) {
        (void)d3dkmt_release_device(&lease->reference);
        return STATUS_INVALID_PARAMETER;
    }
    lease->memory=kmt->memory;
    lease->device=(uint32_t)lease->reference.cookie;
    return STATUS_SUCCESS;
}

NTSTATUS intel_gpu_kmt_release_device(intel_gpu_kmt_device_lease_t *lease)
{
    if (!lease) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_release_device(&lease->reference);
    if (status==STATUS_SUCCESS) { lease->memory=NULL; lease->device=0; }
    return status;
}

NTSTATUS intel_gpu_kmt_acquire_cpu(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE allocation,
    intel_gpu_kmt_cpu_lease_t *lease)
{
    if (!lease || lease->reference.self || lease->access.self) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_acquire_allocation(pid,device,allocation,&lease->reference);
    if (status!=STATUS_SUCCESS) return status;
    const D3DKMT_DEVICE_PROVIDER *provider=&lease->reference.provider;
    const D3DKMT_ALLOCATION_PROVIDER *memory=&lease->reference.memory;
    if (provider->open!=intel_gpu_kmt_open || provider->close!=intel_gpu_kmt_close ||
        memory->allocate!=intel_gpu_kmt_allocate || memory->close!=intel_gpu_kmt_free_allocation ||
        memory->translate!=intel_gpu_kmt_translate_allocation) status=STATUS_NOT_SUPPORTED;
    else {
        intel_gpu_kmt_t *kmt=provider->context;
        uint64_t native_device=lease->reference.device_cookie,resource=lease->reference.cookie;
        if (!kmt || kmt->self!=kmt || !native_device || native_device>UINT32_MAX || !resource || resource>UINT32_MAX)
            status=STATUS_INVALID_PARAMETER;
        else {
            enum intel_gpu_buffer_status result=intel_gpu_memory_acquire_cpu(kmt->memory,pid,
                (uint32_t)native_device,(uint32_t)resource,&lease->access);
            status=result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
                result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
                result==INTEL_GPU_BUFFER_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
            if (result!=INTEL_GPU_BUFFER_OK && result!=INTEL_GPU_BUFFER_BUSY) kmt->wake(kmt->wake_context);
        }
    }
    if (status!=STATUS_SUCCESS) (void)d3dkmt_release_allocation(&lease->reference);
    return status;
}

NTSTATUS intel_gpu_kmt_release_cpu(intel_gpu_kmt_cpu_lease_t *lease)
{
    if (!lease || lease->reference.self!=&lease->reference || lease->access.self!=&lease->access)
        return STATUS_INVALID_PARAMETER;
    intel_gpu_kmt_t *kmt=lease->reference.provider.context;
    enum intel_gpu_buffer_status result=intel_gpu_memory_release_cpu(&lease->access);
    if (result==INTEL_GPU_BUFFER_BUSY) return D3DKMT_STATUS_DEVICE_BUSY;
    if (result!=INTEL_GPU_BUFFER_OK && result!=INTEL_GPU_BUFFER_IO) return STATUS_INVALID_PARAMETER;
    /* Wake while the KMT reference still retains the driver/callback owner. */
    kmt->wake(kmt->wake_context);
    NTSTATUS status=d3dkmt_release_allocation(&lease->reference);
    return status!=STATUS_SUCCESS ? status : result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS intel_gpu_kmt_cpu_physical(intel_gpu_kmt_cpu_lease_t *lease,
    uint64_t *physical,uint64_t *rounded_bytes)
{
    if (!lease || lease->reference.self!=&lease->reference ||
        lease->access.self!=&lease->access || !lease->access.buffer)
        return STATUS_INVALID_PARAMETER;
    /* The exclusive native holder keeps this buffer and its internal access
     * stable through close. Only its persistent owner may end that access. */
    enum intel_gpu_buffer_status result=intel_gpu_buffer_cpu_physical(
        lease->access.buffer->cpu_access,physical,rounded_bytes);
    return result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
        result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
        result==INTEL_GPU_BUFFER_UNSUPPORTED ? STATUS_NOT_SUPPORTED :
        result==INTEL_GPU_BUFFER_INVALID ? STATUS_INVALID_PARAMETER : STATUS_UNSUCCESSFUL;
}

NTSTATUS intel_gpu_kmt_acquire_resident(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE allocation,
    intel_gpu_kmt_resident_lease_t *lease)
{
    if (!lease || lease->reference.self || lease->native.self) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_acquire_allocation(pid,device,allocation,&lease->reference);
    if (status!=STATUS_SUCCESS) return status;
    const D3DKMT_DEVICE_PROVIDER *provider=&lease->reference.provider;
    const D3DKMT_ALLOCATION_PROVIDER *memory=&lease->reference.memory;
    if (provider->open!=intel_gpu_kmt_open || provider->close!=intel_gpu_kmt_close ||
        memory->allocate!=intel_gpu_kmt_allocate || memory->close!=intel_gpu_kmt_free_allocation ||
        memory->translate!=intel_gpu_kmt_translate_allocation) status=STATUS_NOT_SUPPORTED;
    else {
        intel_gpu_kmt_t *kmt=provider->context;
        uint64_t native_device=lease->reference.device_cookie,resource=lease->reference.cookie;
        if (!kmt || kmt->self!=kmt || !native_device || native_device>UINT32_MAX || !resource || resource>UINT32_MAX)
            status=STATUS_INVALID_PARAMETER;
        else {
            enum intel_gpu_buffer_status result=intel_gpu_memory_acquire_resident(kmt->memory,pid,
                (DWORD)native_device,(DWORD)resource,&lease->native);
            status=result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
                result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY :
                result==INTEL_GPU_BUFFER_NOT_RESIDENT ? D3DKMT_STATUS_DEVICE_ERROR : STATUS_INVALID_PARAMETER;
            if (result==INTEL_GPU_BUFFER_NOT_RESIDENT)
                (void)d3dkmt_mark_device_error(pid,device,6); /* allocation DMA fault */
        }
    }
    if (status!=STATUS_SUCCESS) (void)d3dkmt_release_allocation(&lease->reference);
    return status;
}
NTSTATUS intel_gpu_kmt_release_resident(intel_gpu_kmt_resident_lease_t *lease)
{
    if (!lease || lease->reference.self!=&lease->reference || lease->native.self!=&lease->native)
        return STATUS_INVALID_PARAMETER;
    intel_gpu_kmt_t *kmt=lease->reference.provider.context;
    enum intel_gpu_buffer_status result=intel_gpu_memory_release_resident(&lease->native);
    if (result!=INTEL_GPU_BUFFER_OK)
        return result==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
    kmt->wake(kmt->wake_context);
    return d3dkmt_release_allocation(&lease->reference);
}
