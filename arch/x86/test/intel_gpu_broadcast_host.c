#define INTEL_OWNER_FENCE_SUITE_ENTRY broadcast_native_regression_main
#include "intel_gpu_owner_fences_host.c"

static OKMT_HANDLE broadcast_device,broadcast_ctx[4],broadcast_fence[3];
static void broadcast_native_start(void)
{
    res_reset();model_pid=7;process_closing=false;jobs_event_refs=jobs_event_signals=0;jobs_event_collect=1;
    CHECK(intel_gfx_driver_start()==0);
    OKMT_ADAPTER_INFO adapter;DWORD count=0;
    CHECK(d3dkmt_enumerate_adapters(7,1,&adapter,&count)==0 && count==1);
    CHECK(d3dkmt_create_device(7,adapter.adapter,0,&broadcast_device)==0);
    D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
    memcpy(desc.private_data,&command_layout,sizeof(command_layout));D3DKMT_CONTEXT_BUFFERS buffers={0};
    for (unsigned i=0;i<4;i++) CHECK(d3dkmt_create_context(7,broadcast_device,&desc,&broadcast_ctx[i],&buffers)==0);
    uint64_t cpu,gpu;
    for (unsigned i=0;i<3;i++) CHECK(d3dkmt_create_monitored_sync(7,broadcast_device,0,FALSE,&broadcast_fence[i],&cpu,&gpu)==0);
}
static void broadcast_native_drain(void)
{ for (unsigned i=0;i<32;i++) {executor_step();service_step();} }
static void broadcast_native_stop(void)
{
    vm_model_exit(7);d3dkmt_release_process(7);
    NTSTATUS stopped=D3DKMT_STATUS_DEVICE_BUSY;
    for (unsigned i=0;i<64 && stopped;i++) {(void)d3dkmt_collect_devices();executor_step();service_step();stopped=intel_gfx_driver_stop();}
    CHECK(stopped==0 && !vm_count && !jobs_event_refs && intel_gfx_capture_clean() && intel_gpu_executor_quiescent());
}
static uint64_t broadcast_value(unsigned index)
{
    intel_gpu_kmt_context_lease_t context={0};intel_gpu_fence_lease_t fence={0};
    CHECK(intel_gpu_kmt_acquire_context(7,broadcast_ctx[0],&context)==0);
    CHECK(intel_gpu_fence_acquire(context.native.owner->fences,7,context.native.device,index+1,&fence)==0);
    uint64_t value=__atomic_load_n((uint64_t *)fence.cpu,__ATOMIC_ACQUIRE);
    CHECK(intel_gpu_fence_release(&fence)==0 && intel_gpu_kmt_release_context(&context)==0);return value;
}
static void broadcast_native_cases(void)
{
    for (unsigned close=0;close<7;close++) {
        broadcast_native_start();intel_gpu_jobs_t *jobs=platform.owner.jobs;uint64_t gate=0,group=0,event=0,later=0,independent=0;
        D3DKMT_SYNC_COMMAND wait={.count=1,.objects={broadcast_fence[0]},.extended=TRUE,.value=1};
        CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[1],&wait,&gate)==0);
        D3DKMT_SYNC_COMMAND signal={.count=2,.objects={broadcast_fence[1],broadcast_fence[2]},.signal=TRUE,
            .extended=TRUE,.vector_values=TRUE,.values={0x1234567800000007ULL,0xabcdef0100000009ULL},
            .broadcast_count=2,.broadcast={broadcast_ctx[0],broadcast_ctx[1]}};
        CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[0],&signal,&group)==0);
        D3DKMT_SYNC_COMMAND invalid=signal;invalid.broadcast[1]=0;uint64_t untouched=0xfeed;
        CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[0],&invalid,&untouched)==STATUS_INVALID_PARAMETER && untouched==0xfeed);
        D3DKMT_SYNC_COMMAND notify={.signal=TRUE,.extended=TRUE,.event_handle=0x40,.events=&jobs_events,
            .broadcast_count=2,.broadcast={broadcast_ctx[3],broadcast_ctx[1]}};
        CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[3],&notify,&event)==0 && jobs_event_refs==1);
        D3DKMT_SYNC_COMMAND successor=signal;successor.broadcast_count=0;successor.values[0]++;successor.values[1]++;
        CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[0],&successor,&later)==0);
        CHECK(intel_gpu_jobs_pending(jobs,7,broadcast_ctx[0])==2 && intel_gpu_jobs_pending(jobs,7,broadcast_ctx[1])==3 &&
            intel_gpu_jobs_pending(jobs,7,broadcast_ctx[3])==1);
        /* Caller storage can change after admission. */
        signal.broadcast[1]=0;signal.values[0]=signal.values[1]=0;notify.broadcast[0]=0;
        broadcast_native_drain();
        CHECK(!broadcast_value(1) && !broadcast_value(2) && !jobs_event_signals && jobs_event_refs==1);
        intel_gfx_driver_snapshot_t state;CHECK(intel_gfx_driver_snapshot(&state)==0 && !state.completed);
        if (!close) {
            D3DKMT_SYNC_COMMAND now={.signal=TRUE,.extended=TRUE,.event_handle=0x40,.events=&jobs_events};
            CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[2],&now,&independent)==0);
            broadcast_native_drain();CHECK(jobs_event_signals==1 && jobs_event_refs==1 && !broadcast_value(1));
            wait.signal=TRUE;CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[2],&wait,&independent)==0);
            broadcast_native_drain();CHECK(jobs_event_signals==2 && !jobs_event_refs);
            CHECK(broadcast_value(1)==successor.values[0] && broadcast_value(2)==successor.values[1]);
            CHECK(intel_gfx_driver_snapshot(&state)==0 && state.completed==6 && !state.failed);
            successor.broadcast_count=2;successor.broadcast[0]=broadcast_ctx[0];successor.broadcast[1]=broadcast_ctx[1];
            successor.values[0]=1;successor.values[1]=2;successor.allow_rewind=TRUE;
            CHECK(intel_gpu_jobs_submit_sync(jobs,7,broadcast_ctx[0],&successor,&group)==0);
            broadcast_native_drain();CHECK(broadcast_value(1)==1 && broadcast_value(2)==2);
        } else {
            if (close==1) CHECK(d3dkmt_destroy_context(7,broadcast_ctx[1])==0);
            if (close==2) CHECK(d3dkmt_destroy_device(7,broadcast_device)==0);
            if (close==3) {vm_model_exit(7);d3dkmt_release_process(7);}
            if (close==4) CHECK(intel_gpu_jobs_cancel(jobs,7,group)==0 && intel_gpu_jobs_cancel(jobs,7,event)==0);
            if (close==5) intel_gpu_jobs_shutdown(jobs);
            if (close==6) CHECK(d3dkmt_destroy_context(7,broadcast_ctx[3])==0);
            broadcast_native_drain();
            CHECK(!jobs_event_signals && !jobs_event_refs);
        }
        broadcast_native_stop();
    }
}
int main(void)
{
    if (broadcast_native_regression_main()) return 1;
    unsigned before=checks;broadcast_native_cases();
    printf("Intel broadcast signals: %u checks passed, joint ordering and cancellation\n",checks-before);
    return 0;
}
