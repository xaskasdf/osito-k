/* Real native ownership, isolated loss and concurrent executor admission. */
#define NATIVE_EXECUTOR_SUITE_ENTRY device_error_executor_regression
#include "intel_gpu_executor_host.c"
#include "../drivers/intel_gpu_kmt_render.c"

/* The production error callback is installed before registry publication. */
static void error_jobs_setup(JOBS_FIXTURE *f)
{
    res_reset(); CHECK(!vm_count);
    memset(f,0,sizeof(*f)); memset(vm_views,0,sizeof(vm_views));
    vm_mode=vm_unmaps=map_gate=map_entered=map_resume=0;
    retire_gate=retire_entered=retire_resume=0; model_pid=7; process_closing=false;
    CONTEXT_FIXTURE *n=&f->base.service.native;
    CHECK(intel_gpu_memory_init(&n->memory,&allocator,&ram,64,32));
    CHECK(intel_gpu_memory_publish(&n->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&n->provider,&n->memory,intel_gpu_service_notify,NULL));
    CHECK(intel_gpu_contexts_init(&n->contexts,&n->memory,intel_gpu_service_notify,NULL));
    CHECK(intel_gpu_kmt_context_provider(&n->provider,&n->contexts));
    CHECK(intel_gpu_jobs_init(&f->jobs,&n->contexts,jobs_ready,jobs_done,f));
    CHECK(intel_gpu_service_attach_jobs(&n->memory,&n->contexts,&f->jobs,&f->base.service.ticket)==0);
    CHECK(intel_gpu_kmt_render_provider(&n->provider,&f->jobs));
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    CHECK(d3dkmt_register_context_driver(&description,&n->provider.device,&n->provider.allocation,
        &n->provider.context,&n->luid)==0);
    CHECK(d3dkmt_open_adapter(7,n->luid,&n->adapter)==0);
    CHECK(d3dkmt_create_device(7,n->adapter,0,&n->device)==0);
    D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
    memcpy(desc.private_data,&command_layout,sizeof(command_layout));
    D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_create_context(7,n->device,&desc,&f->base.context,&buffers)==0);
    D3DKMT_ALLOCATION_DESCRIPTION allocations[2]={{1000},{1000}};
    CHECK(d3dkmt_create_allocations(7,n->device,2,allocations,f->base.allocations)==0);
    capture_fill(&f->base); service_step();
    CHECK(waited_ticks==0 && intel_gpu_service_idle());
}

static void error_submit_setup(SUBMIT_FIXTURE *f,intel_gpu_kmt_cpu_lease_t *cpu)
{
    memset(f,0,sizeof(*f)); error_jobs_setup(&f->jobs); bcs_model_reset();
    uint64_t id=0;
    if (cpu) CHECK(intel_gpu_kmt_acquire_cpu(7,f->jobs.base.service.native.device,
        f->jobs.base.allocations[0],cpu)==0);
    CHECK(intel_gpu_jobs_submit(&f->jobs.jobs,7,f->jobs.base.context,128,64,2,2,&id)==0);
    CHECK(intel_gpu_jobs_take(&f->jobs.jobs,&f->lease)==0 && f->lease.id==id);
    const intel_ggtt_io_t ggtt_io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&f->space,&layout,16,32,f->bitmap,sizeof(f->bitmap),&ggtt_io,ggtt)==INTEL_GGTT_BIND_OK);
    CHECK(intel_bcs_context_create(&f->native,&f->space,&allocator,&ram,7,0x1001,
        f->lease.capture->resources[0].native.buffer,0x10000000,
        f->lease.capture->resources[1].native.buffer,0x20000000,0x10000000));
    const intel_gt_io_t io={submit_read,submit_write,submit_now,submit_relax};
    const intel_bcs_queue_ops_t ops={submit_prepare,submit_token,submit_acquire,submit_retire};
    CHECK(intel_bcs_enable(&io,f,0x1003000,&f->engine)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_init(&f->queue,&f->engine,&io,f,&ops,0x102030405060ULL)==INTEL_BCS_OK);
    CHECK(intel_bcs_context_attach(&f->native,&f->queue,f));
    CHECK(intel_bcs_queue_publish(&f->queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_watch(&f->queue,submit_wakeup,f)==INTEL_BCS_OK);
}

static void error_executor_setup(EXECUTOR_FIXTURE *f,bool irq)
{
    memset(f,0,sizeof(*f)); error_submit_setup(&f->native,NULL);
    CHECK(intel_gpu_jobs_retire(&f->native.lease,STATUS_CANCELLED)==0);
    service_step(); CHECK(f->native.jobs.done_count==1);
    /* Quiescent fixture reconfiguration before any executor publication. */
    f->native.jobs.jobs.ready=intel_gpu_executor_notify;
    CHECK(intel_gpu_jobs_submit(&f->native.jobs.jobs,7,f->native.jobs.base.context,128,64,2,2,&f->id)==0);
    f->ticket=0xfeed;
    CHECK(intel_gpu_executor_attach(&f->native.jobs.jobs,&f->native.queue,executor_bind,f,irq,&f->ticket)==STATUS_INVALID_PARAMETER);
    CHECK(f->ticket==0xfeed && intel_gpu_executor_quiescent());
    CHECK(intel_bcs_queue_unwatch(&f->native.queue)==INTEL_BCS_OK);
    CHECK(intel_gpu_executor_attach(&f->native.jobs.jobs,&f->native.queue,executor_bind,f,irq,&f->ticket)==0);
    CHECK(f->ticket!=0xfeed && !intel_gpu_executor_quiescent());
    uint64_t untouched=0xfeed;
    CHECK(intel_gpu_executor_attach(&f->native.jobs.jobs,&f->native.queue,executor_bind,f,irq,&untouched)==STATUS_INVALID_PARAMETER);
    CHECK(untouched==0xfeed && intel_gpu_executor_detach(f->ticket)==D3DKMT_STATUS_DEVICE_BUSY);
}

static void isolated_error_cases(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        EXECUTOR_FIXTURE f;error_executor_setup(&f,false);
        CONTEXT_FIXTURE *n=&f.native.jobs.base.service.native;
        OKMT_HANDLE healthy=0,context=0,sem=0;
        CHECK(d3dkmt_create_device(7,n->adapter,0,&healthy)==0);
        D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_create_context(7,healthy,&desc,&context,&buffers)==0);
        D3DKMT_SYNC_DESCRIPTION sync={OKMT_SYNC_SEMAPHORE,0,1};
        CHECK(d3dkmt_create_sync(7,healthy,&sync,&sem)==0);
        pthread_t thread;
        if (mode==1) {f.busy_bind=100;executor_step();}
        if (mode==2) {hw.delay=0xffffffffu;executor_step();CHECK(f.native.engine.submitted_jobs==1);}
        if (mode==3) {
            f.gate=1;CHECK(!pthread_create(&thread,NULL,executor_thread,NULL));
            while (!__atomic_load_n(&f.entered,__ATOMIC_ACQUIRE)) sched_yield();
        }
        CHECK(d3dkmt_mark_device_error(8,n->device,0x80000000)==STATUS_INVALID_PARAMETER);
        uint64_t pages=n->memory.pages;
        CHECK(d3dkmt_mark_device_error(7,n->device,0x80000000)==0);
        CHECK(n->memory.pages==pages && !f.native.engine.retired);
        DWORD state=0;CHECK(d3dkmt_query_device_execution(7,n->device,&state)==0 && state==OKMT_EXECUTION_RESET);
        CHECK(d3dkmt_query_device_execution(7,healthy,&state)==0 && state==OKMT_EXECUTION_ACTIVE);
        uint64_t rejected=0xfeed;
        CHECK(intel_gpu_jobs_submit(&f.native.jobs.jobs,7,f.native.jobs.base.context,128,64,2,2,&rejected)==D3DKMT_STATUS_DEVICE_ERROR && rejected==0xfeed);
        if (mode==2) {
            CHECK(f.native.native.source_access.self && f.native.jobs.done_count==1);
            CHECK(d3dkmt_destroy_device(7,n->device)==0);
            CHECK(f.native.native.source_access.self && !f.native.engine.retired);
            hw.delay=hw.status_reads+1;
        }
        if (mode==3) {__atomic_store_n(&f.resume,1,__ATOMIC_RELEASE);CHECK(!pthread_join(thread,NULL));}
        f.busy_bind=0;
        D3DKMT_SYNC_COMMAND signal={.count=1,.signal=TRUE,.objects={sem}};
        uint64_t admitted=0;CHECK(intel_gpu_jobs_submit_sync(&f.native.jobs.jobs,7,context,&signal,&admitted)==0 && admitted);
        for (unsigned i=0;i<64 && f.native.jobs.done_count<3;i++){executor_step();service_step();}
        CHECK(f.native.jobs.done_count==3);
        BOOL lost_seen=FALSE,healthy_seen=FALSE;
        for (unsigned i=1;i<f.native.jobs.done_count;i++) {
            if (f.native.jobs.done_ids[i]==admitted) {healthy_seen=TRUE;CHECK(f.native.jobs.done_status[i]==STATUS_SUCCESS);}
            if (f.native.jobs.done_ids[i]==f.id) {
                lost_seen=TRUE;NTSTATUS result=f.native.jobs.done_status[i];
                CHECK(result==STATUS_CANCELLED || result==D3DKMT_STATUS_DEVICE_ERROR || result==STATUS_SUCCESS);
            }
        }
        CHECK(lost_seen && healthy_seen); /* independent contexts may retire in either order */
        CHECK(f.native.engine.submitted_jobs==(mode==2?1u:0u));
        CHECK(d3dkmt_query_device_execution(7,healthy,&state)==0 && state==OKMT_EXECUTION_ACTIVE);
        CHECK(d3dkmt_destroy_device(7,healthy)==0);
        executor_cleanup(&f);
    }
}
#ifndef NATIVE_DEVICE_ERROR_SUITE_ENTRY
#define NATIVE_DEVICE_ERROR_SUITE_ENTRY main
#endif
int NATIVE_DEVICE_ERROR_SUITE_ENTRY(void)
{
    if (device_error_executor_regression()) return 1;
    unsigned before=checks;isolated_error_cases();
    printf("Native device error: %u checks passed; queued, claimed, active DMA and concurrent binding\n",checks-before);
    return 0;
}
