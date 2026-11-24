#include "intel_gpu_kmt.h"
#include "intel_gpu_jobs.h"

static NTSTATUS intel_gpu_kmt_render_submit(const D3DKMT_CONTEXT_LEASE *lease,
    const D3DKMT_RENDER_DESCRIPTION *command,DWORD *queued)
{
    if (!lease || lease->self!=lease || !command || !queued) return STATUS_INVALID_PARAMETER;
    intel_gpu_kmt_t *kmt=lease->provider.context;
    if (!kmt || kmt->self!=kmt || !kmt->jobs ||
        lease->context.open!=kmt->context.open || lease->context.close!=kmt->context.close ||
        lease->provider.open!=kmt->device.open || lease->provider.close!=kmt->device.close)
        return STATUS_INVALID_PARAMETER;
    uint64_t id=0;
    NTSTATUS status=intel_gpu_jobs_submit(kmt->jobs,lease->pid,lease->handle,command->offset,
        command->bytes,command->allocations,command->patches,&id);
    *queued=intel_gpu_jobs_pending(kmt->jobs,lease->pid,lease->handle);
    return status;
}

static NTSTATUS intel_gpu_kmt_synchronize(const D3DKMT_CONTEXT_LEASE *lease,const D3DKMT_SYNC_COMMAND *command)
{
    if (!lease || lease->self!=lease || !command) return STATUS_INVALID_PARAMETER;
    intel_gpu_kmt_t *kmt=lease->provider.context;
    if (!kmt || kmt->self!=kmt || !kmt->jobs ||
        lease->context.open!=kmt->context.open || lease->context.close!=kmt->context.close ||
        lease->provider.open!=kmt->device.open || lease->provider.close!=kmt->device.close)
        return STATUS_INVALID_PARAMETER;
    uint64_t id=0;
    return intel_gpu_jobs_submit_sync(kmt->jobs,lease->pid,lease->handle,command,&id);
}

static NTSTATUS intel_gpu_kmt_execution(void *context,DWORD pid,uint64_t device,DWORD *out)
{
    intel_gpu_kmt_t *kmt=context;
    if (!kmt || kmt->self!=kmt || !pid || !device || device>UINT32_MAX || !kmt->jobs)
        return STATUS_INVALID_PARAMETER;
    /* Registry retains the native cookie/provider. This driver shares one
     * executor across its devices; an engine fault affects all of them. */
    return intel_gpu_jobs_execution(kmt->jobs,out);
}

static void intel_gpu_kmt_device_error(void *context,DWORD pid,uint64_t device)
{
    intel_gpu_kmt_t *kmt=context;
    if (kmt && kmt->self==kmt && kmt->jobs) intel_gpu_jobs_device_error(kmt->jobs,pid,device);
}
static void intel_gpu_kmt_driver_error(void *context)
{
    intel_gpu_kmt_t *kmt=context;
    if (kmt && kmt->self==kmt) (void)d3dkmt_report_driver_error(&kmt->device);
}
bool intel_gpu_kmt_render_provider(intel_gpu_kmt_t *kmt,intel_gpu_jobs_t *jobs)
{
    if (!kmt || kmt->self!=kmt || kmt->jobs || !kmt->contexts || !jobs || jobs->self!=jobs ||
        jobs->closing || jobs->contexts!=kmt->contexts || jobs->faulted) return false;
    kmt->jobs=jobs;
    kmt->device.execution=intel_gpu_kmt_execution;
    kmt->device.error=intel_gpu_kmt_device_error;
    kmt->device.fault_state=&jobs->fault;
    jobs->faulted=intel_gpu_kmt_driver_error;
    jobs->fault_context=kmt;
    kmt->render=(D3DKMT_RENDER_PROVIDER){.submit=intel_gpu_kmt_render_submit,.synchronize=intel_gpu_kmt_synchronize};
    return true;
}
