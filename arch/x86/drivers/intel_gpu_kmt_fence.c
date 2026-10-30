#include "intel_gpu_kmt.h"
#include "intel_gpu_executor.h"

static NTSTATUS kmt_fence_create(void *context,DWORD pid,uint64_t device,
    uint64_t initial,BOOL compat,D3DKMT_MONITORED_BACKING *out)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !kmt->contexts || !kmt->contexts->fences ||
        !device || device>UINT32_MAX) return STATUS_INVALID_PARAMETER;
    return intel_gpu_fence_create_monitored(kmt->contexts->fences,pid,(DWORD)device,initial,compat,out);
}
static NTSTATUS kmt_fence_close(void *context,DWORD pid,uint64_t device,uint64_t cookie)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !kmt->contexts || !kmt->contexts->fences ||
        !device || device>UINT32_MAX) return STATUS_INVALID_PARAMETER;
    return intel_gpu_fence_close(kmt->contexts->fences,pid,(DWORD)device,cookie);
}
bool intel_gpu_kmt_monitored_provider(intel_gpu_kmt_t *kmt)
{
    if (!kmt || kmt->self!=kmt || !kmt->contexts || !kmt->contexts->fences ||
        kmt->contexts->fences->memory!=kmt->memory || kmt->contexts->fences->closing ||
        kmt->monitored.create || kmt->monitored.close) return false;
    kmt->monitored=(D3DKMT_MONITORED_PROVIDER){kmt_fence_create,kmt_fence_close,intel_gpu_executor_notify};
    return true;
}
