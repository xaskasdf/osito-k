#include "intel_gfx_driver.h"
#include "intel_gfx.h"
#include "intel_gfx_capture.h"
#include "intel_gfx_display.h"
#include "intel_gpu_service.h"
#include "intel_gpu_executor.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif
extern uint32_t smp_current_cpu(void);

static unsigned driver_control;
static struct {
    intel_gpu_memory_t memory;
    intel_gpu_contexts_t contexts;
    intel_gpu_fences_t fences;
    intel_gpu_kmt_t provider;
    intel_gpu_jobs_t jobs;
    uint64_t service;
    display_source_owner_t display;
    intel_gfx_driver_snapshot_t state;
} driver;

static void gfx_driver_done(void *opaque,uint64_t id,DWORD pid,NTSTATUS result)
{
    (void)opaque; (void)id;
    /* Collector callback on BSP; snapshot excludes BSP preemption. */
    driver.state.last_pid=pid; driver.state.last_result=result;
    driver.state.completed++;
    if (result!=STATUS_SUCCESS) driver.state.failed++;
}

static NTSTATUS gfx_driver_drain(void)
{
    driver.state.phase=INTEL_GFX_DRIVER_CLOSING;
    if (driver.state.adapter_registered) {
        NTSTATUS status=d3dkmt_unregister_adapter(driver.state.luid);
        if (!status) driver.state.adapter_registered=FALSE;
        else if (status!=D3DKMT_STATUS_DEVICE_BUSY) return status;
    }
    if (driver.jobs.self) intel_gpu_jobs_shutdown(&driver.jobs);
    if (driver.state.engine_owned) {
        NTSTATUS status=intel_gfx_capture_close();
        if (status) return status;
        driver.state.engine_owned=FALSE;
    }
    if (driver.contexts.self) intel_gpu_contexts_shutdown(&driver.contexts);
    if (driver.fences.self) intel_gpu_fences_shutdown(&driver.fences);
    if (driver.memory.initialized) intel_gpu_memory_shutdown(&driver.memory);
    if (driver.service) intel_gpu_service_notify(NULL);
    /* Withdrawal hides the adapter immediately, but live client handles may
     * still retain its provider. Wait for their close/exit before reusing it. */
    if (driver.state.adapter_registered) return D3DKMT_STATUS_DEVICE_BUSY;
    /* KMT withdrawal drained every display callback before the physical mode
     * can change. A compatible GUI update may still retain the source. */
    if (driver.display.self && !display_source_release(&driver.display))
        return D3DKMT_STATUS_DEVICE_BUSY;
    if (driver.memory.registered &&
        intel_gpu_memory_unpublish(&driver.memory)!=INTEL_GPU_BUFFER_OK) return D3DKMT_STATUS_DEVICE_BUSY;
    if (driver.service) {
        NTSTATUS status=intel_gpu_service_detach(driver.service);
        if (status) return status;
        driver.service=0;
    }
    if (!intel_gfx_capture_clean()) return STATUS_UNSUCCESSFUL;
    driver.state.phase=INTEL_GFX_DRIVER_OFF;
    return STATUS_SUCCESS;
}

static NTSTATUS gfx_driver_start_engine(bool render)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
#ifdef __KERNEL_X86__
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    if (!(flags&(1ULL<<9))) return STATUS_NOT_SUPPORTED;
#endif
    if (__atomic_exchange_n(&driver_control,1,__ATOMIC_ACQUIRE)) return D3DKMT_STATUS_DEVICE_BUSY;
    NTSTATUS status=STATUS_SUCCESS;
    DWORD node=render ? INTEL_UMD_NODE_RENDER : INTEL_UMD_NODE_COPY;
    if (driver.state.phase==INTEL_GFX_DRIVER_ON) {
        status=driver.state.node==node ? STATUS_SUCCESS : D3DKMT_STATUS_DEVICE_BUSY;
        goto done;
    }
    if (driver.state.phase!=INTEL_GFX_DRIVER_OFF) { status=D3DKMT_STATUS_DEVICE_BUSY; goto done; }
    const intel_gfx_state_t *hardware=intel_gfx_get_state();
    if (!hardware || !intel_gfx_capture_available()) { status=STATUS_NOT_SUPPORTED; goto done; }
    memset(&driver,0,sizeof(driver)); driver.state.phase=INTEL_GFX_DRIVER_STARTING;
    driver.state.node=node;
    if (!intel_gfx_capture_memory_init(&driver.memory)) { status=STATUS_UNSUCCESSFUL; goto fail; }
    if (intel_gpu_memory_publish(&driver.memory)!=INTEL_GPU_BUFFER_OK ||
        !intel_gpu_contexts_init(&driver.contexts,&driver.memory,intel_gpu_service_notify,NULL) ||
        !intel_gpu_fences_init(&driver.fences,&driver.memory,intel_gpu_service_notify,NULL) ||
        !intel_gpu_contexts_attach_fences(&driver.contexts,&driver.fences) ||
        !intel_gpu_kmt_provider(&driver.provider,&driver.memory,intel_gpu_service_notify,NULL) ||
        !intel_gpu_kmt_context_provider(&driver.provider,&driver.contexts) ||
        !intel_gpu_kmt_monitored_provider(&driver.provider) ||
        !intel_gpu_jobs_init(&driver.jobs,&driver.contexts,intel_gpu_executor_notify,gfx_driver_done,NULL) ||
        !intel_gpu_kmt_render_provider(&driver.provider,&driver.jobs)) { status=STATUS_UNSUCCESSFUL; goto fail; }
    status=intel_gpu_service_attach_fences(&driver.memory,&driver.contexts,&driver.jobs,&driver.fences,&driver.service);
    if (status) goto fail;
    driver.state.engine_owned=TRUE; /* includes partial native initialization */
    status=render ? intel_gfx_capture_open_render(&driver.jobs) : intel_gfx_capture_open(&driver.jobs);
    if (status) goto fail;
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000,.adapter_type=OKMT_ADAPTER_PROTOTYPE};
    /* One native adapter owns both the experimental execution service and,
     * when admitted, the actual retained display. No render capability or UMD
     * path is published before the full original-runtime device is ready. */
    status=intel_gfx_display_acquire(&driver.display);
    if (!status) {
        description.adapter_type=OKMT_ADAPTER_DISPLAY;
        description.source_count=1;
    } else if (status!=STATUS_NOT_SUPPORTED) goto fail;
    /* Single native Intel graphics device. This is its kernel device name,
     * not a claim to own the GDI screen or a PnP interface registration. */
    static const char device_name[]="\\Device\\OsitoIntelGraphics0";
    for (unsigned i=0;i<sizeof(device_name);i++) description.device_name[i]=(OKMT_WCHAR)device_name[i];
    description.device_ids.vendor=hardware->vendor_id; description.device_ids.device=hardware->device_id;
    intel_umd_adapter_description_t private_info={INTEL_UMD_ADAPTER_MAGIC,INTEL_UMD_ADAPTER_VERSION,
        sizeof(private_info),render ? INTEL_UMD_CAP_RCS_COPY32 : INTEL_UMD_CAP_COPY32,
        hardware->vendor_id,hardware->device_id,node,0};
    description.private_bytes=sizeof(private_info);
    memcpy(description.private_data,&private_info,sizeof(private_info));
    status=d3dkmt_register_monitored_driver(&description,&driver.provider.device,&driver.provider.allocation,
        &driver.provider.context,&driver.provider.render,&driver.provider.monitored,&driver.state.luid);
    if (status) goto fail;
    driver.state.adapter_registered=TRUE;
    if (driver.display.self) {
        status=intel_gfx_display_bind(driver.state.luid,&driver.display);
        if (status) goto fail;
    }
    driver.state.phase=INTEL_GFX_DRIVER_ON;
    goto done;
fail:
    (void)gfx_driver_drain(); /* any incomplete cleanup stays visibly CLOSING */
done:
    __atomic_store_n(&driver_control,0,__ATOMIC_RELEASE);
    return status;
}

NTSTATUS intel_gfx_driver_start(void)
{
    return gfx_driver_start_engine(false);
}
NTSTATUS intel_gfx_driver_start_render(void)
{
    return gfx_driver_start_engine(true);
}

NTSTATUS intel_gfx_driver_stop(void)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (__atomic_exchange_n(&driver_control,1,__ATOMIC_ACQUIRE)) return D3DKMT_STATUS_DEVICE_BUSY;
    NTSTATUS status=driver.state.phase==INTEL_GFX_DRIVER_OFF ? STATUS_SUCCESS : gfx_driver_drain();
    __atomic_store_n(&driver_control,0,__ATOMIC_RELEASE);
    return status;
}

NTSTATUS intel_gfx_driver_snapshot(intel_gfx_driver_snapshot_t *out)
{
    if (!out || smp_current_cpu()!=0) return STATUS_INVALID_PARAMETER;
    if (__atomic_exchange_n(&driver_control,1,__ATOMIC_ACQUIRE)) return D3DKMT_STATUS_DEVICE_BUSY;
#ifdef __KERNEL_X86__
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    *out=driver.state;
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#endif
    __atomic_store_n(&driver_control,0,__ATOMIC_RELEASE);
    return STATUS_SUCCESS;
}
