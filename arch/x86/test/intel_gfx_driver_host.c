#define INTEL_OWNER_SUITE_ENTRY owner_suite_main
#include "intel_gpu_bcs_owner_host.c"
#include "../drivers/intel_gfx_driver.h"
#include "../drivers/intel_gfx.h"
#include "../drivers/intel_gfx_display.h"

static OWNER_FIXTURE platform;
static bool platform_available=true,platform_fail_open;
static intel_gfx_state_t platform_state={.vendor_id=0x8086,.device_id=0x3e9b};
static bool platform_display,platform_timing_fail;
static display_source_mode_t platform_mode={(void *)0x100000,1920,1080,1920};
bool display_gop_is_primary(void) { return platform_display; }
bool display_get_retained_gop(display_source_mode_t *out)
{ if (!platform_display) return false;*out=platform_mode;return true; }
int intel_gfx_read_timing(intel_gfx_timing_t *out)
{
    if (platform_timing_fail) return 1;
    *out=(intel_gfx_timing_t){.active_lines=1080,.total_lines=1112,.scanline=417};
    return 0;
}
const intel_gfx_state_t *intel_gfx_get_state(void) { return &platform_state; }
bool intel_gfx_capture_available(void) { return platform_available && (!platform.owner.self || platform.owner.closed); }
bool intel_gfx_capture_memory_init(intel_gpu_memory_t *memory)
{
    return intel_gpu_memory_init(memory,&allocator,&ram,64,32);
}
NTSTATUS intel_gfx_capture_open(intel_gpu_jobs_t *jobs)
{
    memset(&platform,0,sizeof(platform)); platform.pat=0x03112233; bcs_model_reset();
    const intel_ggtt_io_t io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&platform.space,&layout,16,32,platform.bitmap,sizeof(platform.bitmap),&io,ggtt)==INTEL_GGTT_BIND_OK);
    intel_gpu_bcs_config_t config={.space=&platform.space,.allocator=allocator,.allocator_context=&ram,
        .io={owner_read,owner_write,owner_now,owner_relax},.hardware_context=&platform,
        .token_read=owner_token,.mapping_stopped=owner_mapping,.ggtt_limit=0x10000000,
        .source_gpu=0x10000000,.destination_gpu=0x20000000,.first_fence=0x22334455,.first_hardware_id=0x2001};
    CHECK(intel_gpu_bcs_owner_init(&platform.owner,jobs,&config)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&platform.owner,8,8)==0);
    if (platform_fail_open) return STATUS_UNSUCCESSFUL;
    return intel_gpu_bcs_owner_start(&platform.owner);
}
NTSTATUS intel_gfx_capture_open_render(intel_gpu_jobs_t *jobs) { (void)jobs; return STATUS_NOT_SUPPORTED; }
NTSTATUS intel_gfx_capture_close(void) { return intel_gpu_bcs_owner_close(&platform.owner); }
bool intel_gfx_capture_clean(void)
{
    return (!platform.owner.self || platform.owner.closed) && ram.allocated==ram.freed;
}
static void driver_cases(void)
{
    intel_gfx_driver_snapshot_t state;
    platform_available=false;
    CHECK(intel_gfx_driver_start()==STATUS_NOT_SUPPORTED);
    CHECK(intel_gfx_driver_snapshot(&state)==0 && state.phase==INTEL_GFX_DRIVER_OFF);
    platform_available=true;
    for (unsigned mode=0;mode<3;mode++) {
        res_reset(); model_pid=7; process_closing=false;
        CHECK(intel_gfx_driver_start()==0);
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.phase==INTEL_GFX_DRIVER_ON && state.adapter_registered && state.engine_owned);
        OKMT_LUID luid=state.luid;
        CHECK(intel_gfx_driver_start()==0);
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.luid.low==luid.low);
        OKMT_ADAPTER_INFO adapter; DWORD count=0;
        CHECK(d3dkmt_enumerate_adapters(7,1,&adapter,&count)==0 && count==1);
        DWORD type=0;
        CHECK(d3dkmt_query_adapter(7,adapter.adapter,OKMT_QUERY_ADAPTER_TYPE,&type,sizeof(type),FALSE)==0 && type==OKMT_ADAPTER_PROTOTYPE);
        OKMT_HANDLE device,context,allocations[2];
        CHECK(d3dkmt_create_device(7,adapter.adapter,0,&device)==0);
        D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        D3DKMT_CONTEXT_BUFFERS buffers;
        CHECK(d3dkmt_create_context(7,device,&desc,&context,&buffers)==0);
        D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{1000},{1000}};
        CHECK(d3dkmt_create_allocations(7,device,2,sizes,allocations)==0);
        intel_gpu_kmt_context_lease_t lease={0};
        CHECK(intel_gpu_kmt_acquire_context(7,context,&lease)==0);
        intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COMMAND_VERSION,
            INTEL_UMD_COPY32,sizeof(command),13,7,80,96,0,0,{0}};
        OKMT_ALLOCATION_LIST list[2]={{allocations[0],0},{allocations[1],1}};
        OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
        memcpy((BYTE *)lease.native.commands+128,&command,sizeof(command));
        memcpy(lease.native.allocations,list,sizeof(list)); memcpy(lease.native.patches,patches,sizeof(patches));
        D3DKMT_RENDER_DESCRIPTION render={128,sizeof(command),2,2}; DWORD pending=0;
        CHECK(lease.reference.render.submit(&lease.reference,&render,&pending)==0 && pending==1);
        CHECK(intel_gpu_kmt_release_context(&lease)==0);
        if (mode!=1) {
            for (unsigned step=0;step<8;step++) { executor_step(); service_step(); }
            CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==1 && !state.failed && state.last_pid==7);
        }
        NTSTATUS stopped=intel_gfx_driver_stop();
        CHECK(stopped==D3DKMT_STATUS_DEVICE_BUSY);
        for (unsigned step=0;step<8;step++) { executor_step(); service_step(); stopped=intel_gfx_driver_stop(); }
        CHECK(stopped==D3DKMT_STATUS_DEVICE_BUSY); /* live KMT device still owns provider */
        CHECK(d3dkmt_enumerate_adapters(7,0,NULL,&count)==0 && count==0);
        CHECK(intel_gfx_driver_start()==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.phase==INTEL_GFX_DRIVER_CLOSING);
        CHECK(d3dkmt_create_context(7,device,&desc,&context,&buffers)==D3DKMT_STATUS_DEVICE_REMOVED);
        vm_model_exit(7); d3dkmt_release_process(7);
        for (unsigned step=0;step<16 && stopped;step++) { executor_step(); service_step(); stopped=intel_gfx_driver_stop(); }
        CHECK(stopped==0 && intel_gfx_capture_clean() && !vm_count && intel_gpu_executor_quiescent());
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.phase==INTEL_GFX_DRIVER_OFF && !state.adapter_registered && !state.engine_owned);
        CHECK(state.completed==1 && state.failed==(mode==1 ? 1u : 0u));
        CHECK(intel_gfx_driver_stop()==0);
    }
    res_reset(); platform_fail_open=true;
    CHECK(intel_gfx_driver_start()==STATUS_UNSUCCESSFUL);
    for (unsigned step=0;step<8;step++) { executor_step(); service_step(); (void)intel_gfx_driver_stop(); }
    CHECK(intel_gfx_driver_snapshot(&state)==0 && state.phase==INTEL_GFX_DRIVER_OFF);
    CHECK(intel_gfx_capture_clean()); platform_fail_open=false;
    res_reset();
}
#ifndef INTEL_DRIVER_SUITE_ENTRY
#define INTEL_DRIVER_SUITE_ENTRY main
#endif
int INTEL_DRIVER_SUITE_ENTRY(void)
{
    if (owner_suite_main()) return 1;
    unsigned before=checks;
    driver_cases();
    printf("Intel native driver lifetime: %u checks passed\n",checks-before);
    return 0;
}
