#define D3DKMT_CPUWAIT_SUITE_ENTRY device_error_wait_regression
#include "d3dkmt_cpuwait_host.c"

static void device_error_cases(void)
{
    const DWORD reasons[]={0x80000000,0x80000006,0,0xffffffff,1,2,3,4,5,6,7,0x80000001,0x80000007};
    const DWORD states[]={2,6,2,5,5,5,5,5,5,6,3,5,3};
    for (unsigned compat=0;compat<2;compat++) for (unsigned round=0;round<13;round++) {
        wait_setup(compat);wait_publish_arrays();
        unsigned signals=event_signals;
        CHECK(wait_public(2,(void *)(user+256),(void *)(user+512),0x40,0)==0 && event_refs==1);
        D3DKMT_CONTEXT_LEASE held={0};CHECK(d3dkmt_acquire_context(100,sync_context,&held)==0);
        OKMT_MARK_DEVICE_ERROR *mark=(void *)(user+768);*mark=(OKMT_MARK_DEVICE_ERROR){sync_device,reasons[round]};
        CHECK(D3DKMTMarkDeviceAsError(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTMarkDeviceAsError(user+4090)==STATUS_INVALID_PARAMETER);
        caller=101;CHECK(D3DKMTMarkDeviceAsError(mark)==STATUS_INVALID_PARAMETER);caller=100;
        cancellation_requested=TRUE;CHECK(D3DKMTMarkDeviceAsError(mark)==STATUS_PROCESS_IS_TERMINATING);cancellation_requested=FALSE;
        CHECK(monitored_storage[1]==0 && monitored_storage[2]==1);
        CHECK(D3DKMTMarkDeviceAsError(mark)==0);
        CHECK(mark->device==sync_device && mark->reason==reasons[round]);
        CHECK(monitored_storage[1]==UINT64_MAX && monitored_storage[2]==UINT64_MAX);
        wait_worker_step();CHECK(!event_refs && event_signals==signals+1 && d3dkmt_wait_idle());
        DWORD state=0;CHECK(d3dkmt_query_device_execution(100,sync_device,&state)==0 && state==states[round]);
        mark->reason=round&1?0x80000000:0x80000006;CHECK(D3DKMTMarkDeviceAsError(mark)==0);
        CHECK(d3dkmt_query_device_execution(100,sync_device,&state)==0 && state==states[round]);
        unsigned publication=D3DKMT_WORK_PREPARED;
        CHECK(d3dkmt_admit_context_work(&held,NULL,0,&publication,D3DKMT_WORK_PREPARED,D3DKMT_WORK_QUEUED)==D3DKMT_STATUS_DEVICE_ERROR);
        CHECK(publication==D3DKMT_WORK_PREPARED);
        CHECK(d3dkmt_release_context(&held)==0);
        CHECK(d3dkmt_signal_sync_cpu(100,sync_device,2,wait_objects,wait_targets,FALSE)==0);
        CHECK(monitored_storage[1]==UINT64_MAX && monitored_storage[2]==UINT64_MAX);
        OKMT_HANDLE unused=0xfeed;uint64_t cpu=0xfeed,gpu=0xfeed;unsigned created=monitored_creates;
        CHECK(d3dkmt_create_monitored_sync(100,sync_device,0,compat,&unused,&cpu,&gpu)==D3DKMT_STATUS_DEVICE_ERROR);
        CHECK(unused==0xfeed && cpu==0xfeed && gpu==0xfeed && monitored_creates==created);
        D3DKMT_SYNC_DESCRIPTION desc={OKMT_SYNC_SEMAPHORE,0,1};
        CHECK(d3dkmt_create_sync(100,sync_device,&desc,&unused)==0);CHECK(d3dkmt_destroy_sync(100,unused)==0);
        D3DKMT_CONTEXT_BUFFERS buffers={0};unused=0xfeed;
        CHECK(d3dkmt_create_context(100,sync_device,&context_desc,&unused,&buffers)==D3DKMT_STATUS_DEVICE_ERROR && unused==0xfeed);
        CHECK(d3dkmt_destroy_device(100,sync_device)==0);
        CHECK(D3DKMTMarkDeviceAsError(mark)==STATUS_INVALID_PARAMETER);
        wait_cleanup();
    }
}
static unsigned error_create_hook,error_close_hook,error_notify_calls;
static NTSTATUS error_race_create(void *p,DWORD pid,uint64_t d,uint64_t initial,BOOL compat,D3DKMT_MONITORED_BACKING *out)
{
    NTSTATUS status=monitored_create(p,pid,d,initial,compat,out);
    if (error_create_hook) {error_create_hook=0;CHECK(d3dkmt_mark_device_error(pid,sync_device,0x80000006)==0);}
    return status;
}
static NTSTATUS error_race_close(void *p,DWORD pid,uint64_t d,uint64_t cookie)
{
    if (error_close_hook) {
        error_close_hook=0;uint64_t before=monitored_storage[cookie];
        CHECK(d3dkmt_mark_device_error(pid,sync_device,0x80000006)==0);
        CHECK(monitored_storage[cookie]==before); /* busy close storage is not touched */
    }
    return monitored_close(p,pid,d,cookie);
}
static void error_reentrant_notify(void *p,DWORD pid,uint64_t cookie)
{
    CHECK(p==&ctx && pid==100);error_notify_calls++;
    CHECK(d3dkmt_destroy_device(pid,sync_device)==0);
    unsigned retained=0;
    for (unsigned i=0;i<16;i++) retained+=ctx.devices[i].pid==pid && ctx.devices[i].cookie==cookie;
    CHECK(retained==1); /* callback may close the handle; its provider is retained */
}
static NTSTATUS error_query_race(void *p,DWORD pid,uint64_t cookie,DWORD *state)
{
    (void)p;(void)cookie;CHECK(d3dkmt_mark_device_error(pid,sync_device,0x80000006)==0);
    *state=OKMT_EXECUTION_ACTIVE;return 0;
}
static void error_provider_races(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        caller=100;g_compat32_mode=FALSE;cancellation_requested=FALSE;denied_first=denied_last=0;
        monitored_creates=monitored_closes=monitored_race=0;monitored_fail_close=monitored_poison_output=false;
        user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
        CHECK(user!=MAP_FAILED);
        D3DKMT_DEVICE_PROVIDER device=device_ops;
        if (mode==2) device.error=error_reentrant_notify;
        if (mode==3) device.execution=error_query_race;
        D3DKMT_RENDER_PROVIDER render={.submit=sync_render_unused,.synchronize=sync_submit};
        D3DKMT_MONITORED_PROVIDER native={error_race_create,error_race_close,monitored_notify};
        CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device,&memory_ops,&context_ops,&render,&native,&ctx.luid)==0);
        OKMT_HANDLE adapter=0;CHECK(d3dkmt_open_adapter(100,ctx.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(100,adapter,0,&sync_device)==0);
        if (mode<2) {
            OKMT_HANDLE object=0xfeed;uint64_t cpu=0xfeed,gpu=0xfeed;error_create_hook=mode==0;
            NTSTATUS result=d3dkmt_create_monitored_sync(100,sync_device,77,FALSE,&object,&cpu,&gpu);
            CHECK(result==(mode==0?D3DKMT_STATUS_DEVICE_ERROR:STATUS_SUCCESS));
            if (!mode) CHECK(object==0xfeed && cpu==0xfeed && gpu==0xfeed && monitored_closes==1);
            else {error_close_hook=1;CHECK(d3dkmt_destroy_sync(100,object)==0 && monitored_closes==1);}
        } else if (mode==2) {
            CHECK(d3dkmt_mark_device_error(100,sync_device,0x80000006)==0 && error_notify_calls==1);
        } else {
            DWORD state=0;CHECK(d3dkmt_query_device_execution(100,sync_device,&state)==0 && state==OKMT_EXECUTION_DMA_FAULT);
        }
        wait_cleanup();
    }
}
int main(void)
{
    if (device_error_wait_regression()) return 1;
    unsigned before=checks;device_error_cases();error_provider_races();
    printf("D3DKMT device error: checks=%u failures=%u; PE32/PE64 reason oracle and fence wake\n",checks-before,failures);
    return failures?1:0;
}
