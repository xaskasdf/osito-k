#include "intel_gpu_kmt.h"

static NTSTATUS intel_gpu_kmt_context_open(void *context,DWORD pid,uint64_t device,
    const D3DKMT_CONTEXT_DESCRIPTION *description,uint64_t *cookie,D3DKMT_CONTEXT_BUFFERS *buffers)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !kmt->contexts || !device || device>UINT32_MAX)
        return STATUS_INVALID_PARAMETER;
    return intel_gpu_context_create(kmt->contexts,pid,(DWORD)device,description,cookie,buffers);
}
static NTSTATUS intel_gpu_kmt_context_close(void *context,DWORD pid,uint64_t device,uint64_t cookie)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !kmt->contexts || !device || device>UINT32_MAX)
        return STATUS_INVALID_PARAMETER;
    return intel_gpu_context_close(kmt->contexts,pid,(DWORD)device,cookie);
}
bool intel_gpu_kmt_context_provider(intel_gpu_kmt_t *kmt,intel_gpu_contexts_t *contexts)
{
    if (!kmt || kmt->self!=kmt || kmt->contexts || !contexts || contexts->self!=contexts ||
        contexts->closing || contexts->memory!=kmt->memory ||
        contexts->wake!=kmt->wake || contexts->wake_context!=kmt->wake_context) return false;
    kmt->contexts=contexts;
    kmt->context=(D3DKMT_CONTEXT_PROVIDER){intel_gpu_kmt_context_open,intel_gpu_kmt_context_close};
    return true;
}

NTSTATUS intel_gpu_kmt_acquire_context(DWORD pid,OKMT_HANDLE handle,intel_gpu_kmt_context_lease_t *lease)
{
    if (!lease || lease->reference.self || lease->native.self) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_acquire_context(pid,handle,&lease->reference);
    if (status!=STATUS_SUCCESS) return status;
    if (lease->reference.context.open!=intel_gpu_kmt_context_open ||
        lease->reference.context.close!=intel_gpu_kmt_context_close) status=STATUS_NOT_SUPPORTED;
    else {
        intel_gpu_kmt_t *kmt=lease->reference.provider.context;
        uint64_t device=lease->reference.device_cookie;
        if (!kmt || kmt->self!=kmt || !kmt->contexts || !device || device>UINT32_MAX)
            status=STATUS_INVALID_PARAMETER;
        else status=intel_gpu_context_acquire(kmt->contexts,pid,(DWORD)device,lease->reference.cookie,&lease->native);
    }
    if (status!=STATUS_SUCCESS) (void)d3dkmt_release_context(&lease->reference);
    return status;
}
NTSTATUS intel_gpu_kmt_release_context(intel_gpu_kmt_context_lease_t *lease)
{
    if (!lease || lease->reference.self!=&lease->reference || lease->native.self!=&lease->native)
        return STATUS_INVALID_PARAMETER;
    NTSTATUS status=intel_gpu_context_release(&lease->native);
    if (status==STATUS_SUCCESS) status=d3dkmt_release_context(&lease->reference);
    return status;
}
