/* Real native Render provider, capture, queues and collector; VM/DMA modeled. */
#define NATIVE_JOBS_SUITE_ENTRY jobs_suite_main
#include "intel_gpu_jobs_host.c"

static void native_render_cases(void)
{
    for (unsigned mode=0;mode<5;mode++) {
        JOBS_FIXTURE f; jobs_setup(&f);
        CONTEXT_FIXTURE *n=&f.base.service.native;
        CHECK(intel_gpu_kmt_render_provider(&n->provider,&f.jobs));
        CHECK(!intel_gpu_kmt_render_provider(&n->provider,&f.jobs));
        D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=3200};
        OKMT_LUID luid; OKMT_HANDLE adapter,device,context;
        CHECK(d3dkmt_register_render_driver(&desc,&n->provider.device,&n->provider.allocation,
            &n->provider.context,&n->provider.render,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0 && d3dkmt_create_device(7,adapter,0,&device)==0);
        D3DKMT_CONTEXT_DESCRIPTION cd={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
        memcpy(cd.private_data,&command_layout,sizeof(command_layout));
        D3DKMT_CONTEXT_BUFFERS buffers;
        CHECK(d3dkmt_create_context(7,device,&cd,&context,&buffers)==0);
        D3DKMT_CONTEXT_LEASE lease={0}; CHECK(d3dkmt_acquire_context(7,f.base.context,&lease)==0);
        /* The same provider captures each context's own handles; foreign
         * device resources are rejected rather than borrowed implicitly. */
        D3DKMT_CONTEXT_LEASE second={0}; CHECK(d3dkmt_acquire_context(7,context,&second)==0);
        CHECK(second.render.submit==n->provider.render.submit);
        D3DKMT_RENDER_DESCRIPTION command={128,64,2,2}; DWORD queued=0;
        CHECK(second.render.submit(&second,&command,&queued)!=0 && !queued);
        CHECK(d3dkmt_release_context(&second)==0);
        service_step();
        unsigned total=mode==1 ? INTEL_GPU_JOBS : 1;
        if (mode==2) command.bytes=63;
        if (mode==3) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
        if (mode==4) CHECK(d3dkmt_unregister_adapter(n->luid)==D3DKMT_STATUS_DEVICE_BUSY);
        for (unsigned i=0;i<total;i++) {
            NTSTATUS status=n->provider.render.submit(&lease,&command,&queued);
            CHECK((status==0)==(mode<2));
            CHECK(queued==(mode<2 ? i+1 : 0));
        }
        if (mode==1) CHECK(n->provider.render.submit(&lease,&command,&queued)==D3DKMT_STATUS_DEVICE_BUSY && queued==INTEL_GPU_JOBS);
        if (mode<2) {
            memset((BYTE *)n->contexts.contexts[0].cpu.cpu+128,0,64);
            for (unsigned i=0;i<total;i++) {
                intel_gpu_job_lease_t job={0}; CHECK(intel_gpu_jobs_take(&f.jobs,&job)==0);
                CHECK(job.capture->valid && job.capture->command.width==16 && job.capture->command.height==8);
                CHECK(intel_gpu_jobs_retire(&job,STATUS_CANCELLED)==0); /* test owner never submits DMA */
                CHECK(intel_gpu_jobs_pending(&f.jobs,7,f.base.context)==total-i-1);
            }
        }
        CHECK(d3dkmt_release_context(&lease)==0);
        d3dkmt_release_process(7); service_step();
        CHECK(d3dkmt_collect_devices()==0 && d3dkmt_unregister_adapter(luid)==0);
        jobs_cleanup(&f);
    }
}
int main(void)
{
    if (jobs_suite_main()) return 1;
    unsigned before=checks; native_render_cases();
    printf("Intel native Render: %u checks passed; captured staging, backpressure, close and removal\n",checks-before);
    return 0;
}
