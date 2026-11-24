/* Exercise real executor transitions with modeled hardware failures. */
#define NATIVE_EXECUTOR_SUITE_ENTRY execution_executor_regression
#include "intel_gpu_executor_host.c"
#include "../drivers/intel_gpu_kmt_render.c"

static void execution_cases(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        EXECUTOR_FIXTURE f;executor_setup(&f,false);
        CONTEXT_FIXTURE *native=&f.native.jobs.base.service.native;
        CHECK(intel_gpu_kmt_render_provider(&native->provider,&f.native.jobs.jobs));
        D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000};OKMT_LUID luid;OKMT_HANDLE adapter,device;
        CHECK(d3dkmt_register_driver(&description,&native->provider.device,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
        CHECK(d3dkmt_create_device(7,adapter,0,&device)==0);
        DWORD state=0;CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_ACTIVE);
        if (mode) hw.delay=0xffffffffu;
        executor_step();service_step();
        if (mode==1 || mode==2) {
            CHECK(f.native.engine.submitted_jobs==1 && f.native.jobs.done_count==1);
            hw.stall_stop=mode==2;
            hw.ns+=10000000000ULL; /* actual watchdog expiry in native queue */
            executor_step();service_step();
            CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_HUNG);
            if (mode==2) CHECK(!f.native.engine.retired && f.native.native.source_access.self && f.native.jobs.done_count==1);
            CHECK(intel_gpu_executor_stop(f.ticket)==0);
            CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_HUNG);
            hw.stall_stop=false;
        } else if (mode==3) {
            /* Inject ESR in the MMIO model; real native polling diagnoses it. */
            CHECK(f.native.engine.submitted_jobs==1 && f.native.jobs.done_count==1);
            hw.regs[0xb8/4]=1;
            intel_gpu_executor_notify(NULL);executor_step();service_step();
            CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_DMA_FAULT);
        } else {
            CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_ACTIVE);
            CHECK(intel_gpu_executor_stop(f.ticket)==0);
            CHECK(d3dkmt_query_device_execution(7,device,&state)==0 && state==OKMT_EXECUTION_STOPPED);
        }
        CHECK(d3dkmt_destroy_device(7,device)==0);
        state=0xfeed;CHECK(d3dkmt_query_device_execution(7,device,&state)==STATUS_INVALID_PARAMETER && state==0xfeed);
        CHECK(d3dkmt_close_adapter(7,adapter)==0 && d3dkmt_unregister_adapter(luid)==0);
        executor_cleanup(&f);
    }
}
int main(void)
{
    if (execution_executor_regression()) return 1;
    unsigned before=checks;execution_cases();
    printf("Native execution state: %u checks passed; watchdog, failed reset retention, stop and engine fault\n",checks-before);
    return 0;
}
