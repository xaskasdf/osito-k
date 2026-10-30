/* Public registry identities backed by the production native fence manager. */
#define INTEL_OWNER_FENCE_SUITE_ENTRY monitored_regression_main
#include "intel_gpu_owner_fences_host.c"

static void monitored_native_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        res_reset(); model_pid=7; process_closing=false;
        CHECK(intel_gfx_driver_start()==0);
        OKMT_ADAPTER_INFO adapter; DWORD count=0;
        CHECK(d3dkmt_enumerate_adapters(7,1,&adapter,&count)==0 && count==1);
        OKMT_HANDLE device=0,context=0,other=0,object=0;
        CHECK(d3dkmt_create_device(7,adapter.adapter,0,&device)==0);
        D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_create_context(7,device,&desc,&context,&buffers)==0);
        CHECK(d3dkmt_create_context(7,device,&desc,&other,&buffers)==0);
        uint64_t cpu=0xfeed,gpu=0xfeed;
        CHECK(d3dkmt_create_monitored_sync(8,device,0,compat,&object,&cpu,&gpu)==STATUS_INVALID_PARAMETER);
        CHECK(!object && cpu==0xfeed && gpu==0xfeed);
        unsigned readonly=vm_readonly_maps;
        CHECK(d3dkmt_create_monitored_sync(7,device,0x1234567887654321ULL,compat,&object,&cpu,&gpu)==0);
        CHECK(object && cpu!=0xfeed && gpu==INTEL_GPU_FENCE_VA_BASE);
        CHECK(compat ? cpu<=UINT32_MAX && vm_readonly_maps==readonly : cpu>UINT32_MAX && vm_readonly_maps==readonly+1);
        intel_gpu_kmt_context_lease_t parent={0};
        CHECK(intel_gpu_kmt_acquire_context(7,context,&parent)==0);
        intel_gpu_fences_t *fences=parent.native.owner->fences;
        intel_gpu_fence_lease_t backing={0};
        CHECK(intel_gpu_fence_acquire(fences,7,parent.native.device,1,&backing)==0);
        CHECK(backing.gpu==gpu && __atomic_load_n((uint64_t *)backing.cpu,__ATOMIC_ACQUIRE)==0x1234567887654321ULL);
        CHECK(intel_gpu_kmt_release_context(&parent)==0);
        D3DKMT_SYNC_COMMAND wait={.count=1,.objects={object},.value=0xabcdef0100000009ULL,.extended=TRUE};
        D3DKMT_SYNC_COMMAND signal=wait; signal.signal=TRUE;
        uint64_t wait_id=0,signal_id=0;
        CHECK(intel_gpu_jobs_submit_sync(platform.owner.jobs,7,context,&wait,&wait_id)==0);
        executor_step(); service_step();
        intel_gfx_driver_snapshot_t state;
        CHECK(intel_gfx_driver_snapshot(&state)==0 && !state.completed);
        CHECK(intel_gpu_jobs_submit_sync(platform.owner.jobs,7,other,&signal,&signal_id)==0 && signal_id>wait_id);
        for (unsigned i=0;i<16;i++) { executor_step(); service_step(); }
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==2 && !state.failed);
        CHECK(__atomic_load_n((uint64_t *)backing.cpu,__ATOMIC_ACQUIRE)==signal.value);
        OKMT_HANDLE second=0;uint64_t second_cpu=0,second_gpu=0;
        CHECK(d3dkmt_create_monitored_sync(7,device,5,compat,&second,&second_cpu,&second_gpu)==0);
        D3DKMT_SYNC_COMMAND vector={.count=2,.objects={object,second},.extended=TRUE,.vector_values=TRUE,
            .values={0xabcdef010000000aULL,0x1234000000000008ULL}};
        CHECK(intel_gpu_jobs_submit_sync(platform.owner.jobs,7,context,&vector,&wait_id)==0);
        executor_step();service_step();
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==2);
        CHECK(d3dkmt_signal_sync_cpu(7,device,1,vector.objects,vector.values,FALSE)==0);
        CHECK(__atomic_load_n((uint64_t *)backing.cpu,__ATOMIC_ACQUIRE)==vector.values[0]);
        executor_step();service_step();
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==2);
        CHECK(d3dkmt_signal_sync_cpu(7,device,2,vector.objects,vector.values,FALSE)==0);
        for (unsigned i=0;i<16;i++) { executor_step();service_step(); }
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==3 && !state.failed);
        vector.signal=TRUE;vector.values[0]++;vector.values[1]++;
        CHECK(intel_gpu_jobs_submit_sync(platform.owner.jobs,7,other,&vector,&signal_id)==0);
        for (unsigned i=0;i<16;i++) { executor_step();service_step(); }
        CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==4 && !state.failed);
        CHECK(__atomic_load_n((uint64_t *)backing.cpu,__ATOMIC_ACQUIRE)==vector.values[0]);
        CHECK(d3dkmt_destroy_sync(7,second)==0);
        D3DKMT_SYNC_LEASE retained={0},denied={0};
        CHECK(d3dkmt_sync_capture(7,context,&wait,&retained)==0);
        CHECK(d3dkmt_destroy_sync(7,object)==0);
        CHECK(d3dkmt_sync_capture(7,context,&wait,&denied)==STATUS_INVALID_PARAMETER);
        unsigned publication=D3DKMT_WORK_QUEUED;
        CHECK(d3dkmt_sync_step(&retained,&publication)==0 && publication==D3DKMT_WORK_SUBMITTED);
        CHECK(d3dkmt_sync_release(&retained)==0);
        CHECK(intel_gpu_fence_closed(&backing));
        CHECK(intel_gpu_fence_release(&backing)==0);
        CHECK(d3dkmt_destroy_sync(7,object)==STATUS_INVALID_PARAMETER);
        vm_model_exit(7); d3dkmt_release_process(7);
        NTSTATUS stopped=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<64 && stopped;i++) {
            (void)d3dkmt_collect_devices(); executor_step(); service_step(); stopped=intel_gfx_driver_stop();
        }
        CHECK(stopped==0 && !vm_count && intel_gfx_capture_clean() && intel_gpu_executor_quiescent());
    }
}
int main(void)
{
    if (monitored_regression_main()) return 1;
    unsigned before=checks; monitored_native_cases();
    printf("Intel public monitored fences: %u checks passed, native storage and queued synchronization PE32/PE64\n",checks-before);
    return 0;
}
