/* Real executor fault detection, registry propagation and DMA retention. */
#define NATIVE_DEVICE_ERROR_SUITE_ENTRY driver_error_regression
#include "intel_gpu_device_error_host.c"

static uint64_t driver_fence_values[2];
static unsigned driver_fence_created,driver_fence_closed;
static NTSTATUS driver_fence_create(void *opaque,DWORD pid,uint64_t device,uint64_t initial,
    BOOL compat,D3DKMT_MONITORED_BACKING *out)
{
    CHECK(opaque && (pid==7 || pid==8) && device && !compat && driver_fence_created<2);
    unsigned slot=driver_fence_created++;driver_fence_values[slot]=initial;
    *out=(D3DKMT_MONITORED_BACKING){slot+1,0x20000000+4096*slot,0x100000000ULL+4096*slot,&driver_fence_values[slot]};
    return 0;
}
static NTSTATUS driver_fence_close(void *opaque,DWORD pid,uint64_t device,uint64_t cookie)
{CHECK(opaque && (pid==7 || pid==8) && device && cookie && cookie<=2);driver_fence_closed++;return 0;}

static void native_driver_fault_cases(void)
{
    for(unsigned mode=0;mode<5;mode++) {
        EXECUTOR_FIXTURE f;error_executor_setup(&f,false);
        CONTEXT_FIXTURE *n=&f.native.jobs.base.service.native;
        D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};OKMT_LUID luid;
        D3DKMT_MONITORED_PROVIDER monitored={driver_fence_create,driver_fence_close,NULL};
        CHECK(d3dkmt_register_monitored_driver(&description,&n->provider.device,&n->provider.allocation,
            &n->provider.context,&n->provider.render,&monitored,&luid)==0);
        OKMT_HANDLE adapter[2],device[2],fence[2];uint64_t cpu,gpu,target=99;
        D3DKMT_CPU_WAIT_LEASE wait[2]={{0},{0}};
        driver_fence_created=driver_fence_closed=0;
        for(unsigned i=0;i<2;i++) {
            CHECK(d3dkmt_open_adapter(7+i,luid,&adapter[i])==0);
            CHECK(d3dkmt_create_device(7+i,adapter[i],0,&device[i])==0);
            CHECK(d3dkmt_create_monitored_sync(7+i,device[i],0,FALSE,&fence[i],&cpu,&gpu)==0);
            CHECK(d3dkmt_cpu_wait_capture(7+i,device[i],1,&fence[i],&target,FALSE,&wait[i])==0);
            CHECK(d3dkmt_cpu_wait_status(&wait[i])==STATUS_PENDING);
        }
        hw.delay=0xffffffffu;executor_step();service_step();
        CHECK(f.native.engine.submitted_jobs==1 && f.native.jobs.done_count==1);
        uint64_t pages=n->memory.pages;
        DWORD expected=mode==2 ? OKMT_EXECUTION_DMA_FAULT : mode>=3 ? OKMT_EXECUTION_STOPPED : OKMT_EXECUTION_HUNG;
        if(mode==2) hw.regs[0xb8/4]=1;
        else if(mode==3) intel_gpu_jobs_fail(&f.native.jobs.jobs,OKMT_EXECUTION_STOPPED);
        else if(mode==4) CHECK(intel_gpu_executor_stop(f.ticket)==0);
        else {hw.stall_stop=mode==1;hw.ns+=10000000000ULL;}
        intel_gpu_executor_notify(NULL);executor_step();
        CHECK(f.native.jobs.jobs.fault==(mode==4?0:expected));
        for(unsigned i=0;i<2;i++) {
            DWORD state=0;CHECK(d3dkmt_query_device_execution(7+i,device[i],&state)==0 && state==expected);
            CHECK(driver_fence_values[i]==(mode==4?0:UINT64_MAX));
            CHECK(d3dkmt_cpu_wait_status(&wait[i])==(mode==4?STATUS_PENDING:STATUS_SUCCESS));
            CHECK(d3dkmt_cpu_wait_release(&wait[i])==0);
            OKMT_HANDLE untouched=0xfeed;
            if(mode==4) {
                CHECK(d3dkmt_create_device(7+i,adapter[i],0,&untouched)==0);
                CHECK(d3dkmt_destroy_device(7+i,untouched)==0);
            } else CHECK(d3dkmt_create_device(7+i,adapter[i],0,&untouched)==D3DKMT_STATUS_DEVICE_ERROR && untouched==0xfeed);
        }
        DWORD state=0;CHECK(d3dkmt_query_device_execution(7,n->device,&state)==0 && state==expected);
        CHECK(intel_gpu_executor_stop(f.ticket)==0);
        if(mode!=4) {
            intel_gpu_jobs_fail(&f.native.jobs.jobs,OKMT_EXECUTION_DMA_FAULT);
            intel_gpu_jobs_set_execution(&f.native.jobs.jobs,OKMT_EXECUTION_ACTIVE);
            CHECK(f.native.jobs.jobs.fault==expected && f.native.jobs.jobs.execution==expected);
        }
        if(mode==1) {
            service_step();CHECK(!f.native.engine.retired && f.native.native.source_access.self);
            CHECK(n->memory.pages==pages && f.native.jobs.done_count==1);
        }
        for(unsigned i=0;i<2;i++) {
            CHECK(d3dkmt_destroy_sync(7+i,fence[i])==0);
            CHECK(d3dkmt_destroy_device(7+i,device[i])==0);
            CHECK(d3dkmt_close_adapter(7+i,adapter[i])==0);
        }
        CHECK(driver_fence_closed==2 && d3dkmt_unregister_adapter(luid)==0);
        hw.stall_stop=false;executor_cleanup(&f);
    }
}
int main(void)
{
    if(driver_error_regression())return 1;
    unsigned before=checks;native_driver_fault_cases();
    printf("Native driver error: %u checks passed; watchdog/ESR, cross-process fences, first error and failed reset retention\n",checks-before);
    return 0;
}
