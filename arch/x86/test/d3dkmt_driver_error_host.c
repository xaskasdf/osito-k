#define D3DKMT_CPUWAIT_SUITE_ENTRY driver_error_wait_regression
#include "d3dkmt_cpuwait_host.c"

static DWORD driver_fault,other_fault;
static D3DKMT_DEVICE_PROVIDER fault_provider;
static unsigned driver_open_race,driver_create_race,driver_close_race;
static void publish_fault(DWORD state)
{
    __atomic_store_n(&driver_fault,state,__ATOMIC_RELEASE);
    CHECK(d3dkmt_report_driver_error(&fault_provider)==0);
}
static NTSTATUS driver_open(void *data,DWORD pid,uint64_t *cookie)
{
    NTSTATUS status=context_device_open(data,pid,cookie);
    if(driver_open_race){driver_open_race=0;publish_fault(OKMT_EXECUTION_HUNG);}
    return status;
}
static NTSTATUS fault_fence_create(void *opaque,DWORD pid,uint64_t device,uint64_t initial,
    BOOL compat,D3DKMT_MONITORED_BACKING *out)
{
    CHECK(opaque==&ctx && (pid==100 || pid==101) && device && compat==g_compat32_mode);
    unsigned slot=++monitored_creates;CHECK(slot<64);monitored_storage[slot]=initial;
    *out=(D3DKMT_MONITORED_BACKING){slot,(uintptr_t)user+2048,0x400000000ULL+4096*slot,&monitored_storage[slot]};
    if(driver_create_race){driver_create_race=0;publish_fault(OKMT_EXECUTION_DMA_FAULT);}
    return 0;
}
static NTSTATUS fault_fence_close(void *opaque,DWORD pid,uint64_t device,uint64_t cookie)
{
    CHECK(opaque==&ctx && (pid==100 || pid==101) && device && cookie && cookie<=monitored_creates);
    if(driver_close_race){
        driver_close_race=0;uint64_t before=monitored_storage[cookie];publish_fault(OKMT_EXECUTION_HUNG);
        CHECK(monitored_storage[cookie]==before);
    }
    monitored_closes++;return 0;
}
static void driver_error_cases(BOOL compat,unsigned mode)
{
    caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
    monitored_creates=monitored_closes=0;driver_fault=other_fault=0;
    driver_open_race=driver_create_race=driver_close_race=0;
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    fault_provider=device_ops;fault_provider.open=driver_open;fault_provider.fault_state=&driver_fault;
    D3DKMT_DEVICE_PROVIDER healthy=fault_provider;healthy.fault_state=&other_fault;
    D3DKMT_RENDER_PROVIDER render={.submit=sync_render_unused,.synchronize=sync_submit};
    D3DKMT_MONITORED_PROVIDER monitored={fault_fence_create,fault_fence_close,monitored_notify};
    CHECK(d3dkmt_report_driver_error(&fault_provider)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_report_driver_error(NULL)==STATUS_INVALID_PARAMETER);
    OKMT_LUID other;
    CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&fault_provider,&memory_ops,&context_ops,&render,&monitored,&ctx.luid)==0);
    CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&healthy,&memory_ops,&context_ops,&render,&monitored,&other)==0);
    OKMT_HANDLE adapters[3],devices[3],fences[3];uint64_t cpu,gpu;
    for(unsigned i=0;i<3;i++) {
        DWORD pid=i==1?101:100;
        CHECK(d3dkmt_open_adapter(pid,i==2?other:ctx.luid,&adapters[i])==0);
        CHECK(d3dkmt_create_device(pid,adapters[i],0,&devices[i])==0);
        CHECK(d3dkmt_create_monitored_sync(pid,devices[i],17,compat,&fences[i],&cpu,&gpu)==0);
    }
    sync_device=devices[0];sync_context=0;
    D3DKMT_CONTEXT_BUFFERS buffers={0};D3DKMT_CONTEXT_LEASE held={0};
    CHECK(d3dkmt_create_context(100,sync_device,&context_desc,&sync_context,&buffers)==0);
    CHECK(d3dkmt_acquire_context(100,sync_context,&held)==0);
    wait_objects[0]=fences[0];wait_targets[0]=99;wait_publish_arrays();
    unsigned signals=event_signals;
    CHECK(wait_public(1,(void *)(user+256),(void *)(user+512),0x40,0)==0 && event_refs==1);
    DWORD expected=mode==1?OKMT_EXECUTION_DMA_FAULT:mode==2?OKMT_EXECUTION_STOPPED:OKMT_EXECUTION_HUNG;
    if(mode==3)CHECK(d3dkmt_mark_device_error(100,devices[0],0x80000000)==0);
    if(mode==4) {
        driver_open_race=1;OKMT_HANDLE value=0xfeed;
        CHECK(d3dkmt_create_device(100,adapters[0],0,&value)==D3DKMT_STATUS_DEVICE_ERROR && value==0xfeed);
    } else if(mode==5) {
        driver_create_race=1;OKMT_HANDLE value=0xfeed;cpu=gpu=0xfeed;
        CHECK(d3dkmt_create_monitored_sync(100,devices[0],7,compat,&value,&cpu,&gpu)==D3DKMT_STATUS_DEVICE_ERROR);
        CHECK(value==0xfeed && cpu==0xfeed && gpu==0xfeed);expected=OKMT_EXECUTION_DMA_FAULT;
    } else if(mode==6) {
        driver_close_race=1;CHECK(d3dkmt_destroy_sync(101,fences[1])==0);fences[1]=0;
    } else publish_fault(expected);
    wait_worker_step();CHECK(!event_refs && event_signals==signals+1 && d3dkmt_wait_idle());
    CHECK(monitored_storage[1]==UINT64_MAX && monitored_storage[2]==(mode==6?17:UINT64_MAX) && monitored_storage[3]==17);
    DWORD state=0;CHECK(d3dkmt_query_device_execution(100,devices[0],&state)==0 && state==(mode==3?OKMT_EXECUTION_RESET:expected));
    CHECK(d3dkmt_query_device_execution(101,devices[1],&state)==0 && state==expected);
    unsigned publication=D3DKMT_WORK_PREPARED;
    CHECK(d3dkmt_admit_context_work(&held,NULL,0,&publication,D3DKMT_WORK_PREPARED,D3DKMT_WORK_QUEUED)==D3DKMT_STATUS_DEVICE_ERROR);
    CHECK(publication==D3DKMT_WORK_PREPARED && d3dkmt_release_context(&held)==0);
    OKMT_HANDLE extra=0xfeed;
    CHECK(d3dkmt_create_device(100,adapters[0],0,&extra)==D3DKMT_STATUS_DEVICE_ERROR && extra==0xfeed);
    CHECK(d3dkmt_create_device(100,adapters[2],0,&extra)==0 && d3dkmt_destroy_device(100,extra)==0);
    CHECK(d3dkmt_report_driver_error(&fault_provider)==0);
    for(unsigned i=0;i<3;i++) {
        DWORD pid=i==1?101:100;
        if(fences[i])CHECK(d3dkmt_destroy_sync(pid,fences[i])==0);
        CHECK(d3dkmt_destroy_device(pid,devices[i])==0 && d3dkmt_close_adapter(pid,adapters[i])==0);
    }
    CHECK(d3dkmt_unregister_adapter(other)==0);
    wait_cleanup();
    /* Fault predates registration: the shared atomic source closes admission
     * even when no adapter existed to receive the original notification. */
    CHECK(d3dkmt_report_driver_error(&fault_provider)==D3DKMT_STATUS_DEVICE_REMOVED);
    CHECK(d3dkmt_register_driver(&adapter_desc,&fault_provider,&ctx.luid)==0);
    CHECK(d3dkmt_open_adapter(100,ctx.luid,&extra)==0);
    OKMT_HANDLE value=0xfeed;CHECK(d3dkmt_create_device(100,extra,0,&value)==D3DKMT_STATUS_DEVICE_ERROR && value==0xfeed);
    CHECK(d3dkmt_close_adapter(100,extra)==0 && d3dkmt_unregister_adapter(ctx.luid)==0);
}
int main(void)
{
    if(driver_error_wait_regression())return 1;
    unsigned before=checks;
    for(unsigned compat=0;compat<2;compat++)for(unsigned mode=0;mode<7;mode++)driver_error_cases(compat,mode);
    printf("D3DKMT driver error: checks=%u failures=%u; cross-process fences, adapter isolation and publication races\n",checks-before,failures);
    return failures?1:0;
}
