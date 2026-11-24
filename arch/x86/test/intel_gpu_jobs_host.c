/* Persistent capture ownership; the fixture never submits work to a GPU. */
#define NATIVE_CAPTURE_SUITE_ENTRY capture_suite_main
#include "intel_gpu_capture_host.c"
#include "../drivers/intel_gpu_jobs.h"

typedef struct {
    CAPTURE_FIXTURE base;
    intel_gpu_jobs_t jobs;
    unsigned ready_count,done_count,ready_gate,done_gate,entered,resume;
    uint64_t done_ids[32],submitted;
    NTSTATUS done_status[32],submit_status;
} JOBS_FIXTURE;
static void jobs_ready(void *opaque)
{
    JOBS_FIXTURE *f=opaque;
    CHECK(!intel_gpu_jobs_empty(&f->jobs)); /* callback retention and unlocked inspection */
    __atomic_add_fetch(&f->ready_count,1,__ATOMIC_RELAXED);
    if (__atomic_exchange_n(&f->ready_gate,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&f->entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void jobs_done(void *opaque,uint64_t id,DWORD pid,NTSTATUS status)
{
    JOBS_FIXTURE *f=opaque;
    CHECK(pid==7 && id && f->done_count<32 && !intel_gpu_jobs_empty(&f->jobs));
    for (unsigned i=0;i<f->done_count;i++) CHECK(f->done_ids[i]!=id);
    f->done_ids[f->done_count]=id; f->done_status[f->done_count++]=status;
    if (__atomic_exchange_n(&f->done_gate,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&f->entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void jobs_setup(JOBS_FIXTURE *f)
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
static void jobs_cleanup(JOBS_FIXTURE *f)
{
    intel_gpu_jobs_shutdown(&f->jobs); service_step();
    CHECK(intel_gpu_jobs_empty(&f->jobs) && !intel_gpu_jobs_need_retry(&f->jobs));
    service_cleanup(&f->base.service,false);
}
static void jobs_cases(void)
{
    JOBS_FIXTURE f; jobs_setup(&f);
    uint64_t first=0xfeed,second=0xfeed;
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,63,2,2,&first)==STATUS_INVALID_PARAMETER);
    CHECK(first==0xfeed && !f.done_count); service_step(); CHECK(!f.done_count);
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&first)==0);
    intel_gpu_kmt_cpu_lease_t cpu={0};
    for (unsigned i=0;i<2;i++)
        CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[i],&cpu)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(!cpu.reference.self && !cpu.access.self);
    intel_gpu_command_context_t *ctx=&f.base.service.native.contexts.contexts[0];
    ((intel_umd_copy32_t *)((BYTE *)ctx->cpu.cpu+128))->width=8;
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&second)==0 && second>first);
    memset((BYTE *)ctx->cpu.cpu+128,0xff,64);
    intel_gpu_job_lease_t a={0},b={0};
    CHECK(intel_gpu_jobs_take(&f.jobs,&a)==0 && a.id==first && a.capture->command.width==16);
    CHECK(intel_gpu_jobs_take(&f.jobs,&b)==D3DKMT_STATUS_DEVICE_BUSY && !b.self);
    intel_gpu_job_lease_t copied=a;
    CHECK(intel_gpu_jobs_retire(&copied,0)==STATUS_INVALID_PARAMETER);
    CHECK(!intel_gpu_jobs_cancelled(&a));
    CHECK(intel_gpu_jobs_cancel(&f.jobs,8,first)==STATUS_INVALID_PARAMETER);
    CHECK(intel_gpu_jobs_cancel(&f.jobs,7,first)==D3DKMT_STATUS_DEVICE_BUSY && intel_gpu_jobs_cancelled(&a));
    CHECK(intel_gpu_jobs_retire(&a,STATUS_CANCELLED)==0 && !a.self && !f.done_count);
    service_step(); CHECK(f.done_count==1 && f.done_ids[0]==first && f.done_status[0]==STATUS_CANCELLED);
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[1],&cpu)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(intel_gpu_jobs_take(&f.jobs,&b)==0 && b.id==second && b.capture->command.width==8);
    intel_gpu_jobs_shutdown(&f.jobs); service_step();
    CHECK(!intel_gpu_jobs_empty(&f.jobs) && intel_gpu_jobs_cancelled(&b) && f.done_count==1);
    CHECK(intel_gpu_jobs_retire(&b,STATUS_CANCELLED)==0); service_step();
    CHECK(f.done_count==2 && f.done_ids[1]==second && intel_gpu_jobs_empty(&f.jobs));
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[1],&cpu)==0);
    CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    jobs_cleanup(&f);

    /* A partial reservation failure publishes no ticket. Its first hazard
     * remains collector-owned until rollback, without blocking other buffers. */
    jobs_setup(&f);
    CHECK(intel_gpu_capture_begin(7,f.base.context,128,64,2,2,&f.base.capture)==0);
    intel_gpu_memory_t *memory=&f.base.service.native.memory;
    intel_gpu_memory_resource_t *destination=NULL;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        if (&memory->resources[i].buffer==f.base.capture.resources[1].native.buffer)
            destination=&memory->resources[i];
    CHECK(destination && !destination->pending_work);
    destination->pending_work=UINT32_MAX;
    first=0xfeed;
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&first)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(first==0xfeed && !f.done_count);
    destination->pending_work=0;
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[0],&cpu)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[1],&cpu)==0);
    CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    service_step(); CHECK(!intel_gpu_jobs_need_retry(&f.jobs) && !f.done_count);
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++) CHECK(!f.jobs.jobs[i].id);
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[0],&cpu)==0);
    CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    CHECK(intel_gpu_capture_release(&f.base.capture)==0);
    jobs_cleanup(&f);

    jobs_setup(&f);
    uint64_t ids[INTEL_GPU_JOBS];
    for (unsigned i=0;i<INTEL_GPU_JOBS;i++)
        CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&ids[i])==0);
    first=0xfeed;
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&first)==D3DKMT_STATUS_DEVICE_BUSY && first==0xfeed);
    CHECK(intel_gpu_jobs_cancel(&f.jobs,7,ids[0])==0); service_step();
    CHECK(f.done_count==1 && f.done_status[0]==STATUS_CANCELLED);
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&first)==0 && first>ids[INTEL_GPU_JOBS-1]);
    CHECK(intel_gpu_jobs_cancel(&f.jobs,7,ids[0])==STATUS_INVALID_PARAMETER);
    intel_gpu_jobs_shutdown(&f.jobs); service_step();
    CHECK(f.done_count==INTEL_GPU_JOBS+1 && intel_gpu_jobs_empty(&f.jobs));
    jobs_cleanup(&f);

    for (unsigned mode=0;mode<10;mode++) {
        jobs_setup(&f); CONTEXT_FIXTURE *n=&f.base.service.native;
        CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&first)==0);
        switch (mode%5) {
        case 0: CHECK(d3dkmt_destroy_context(7,f.base.context)==0); break;
        case 1: CHECK(d3dkmt_destroy_device(7,n->device)==0); break;
        case 2: CHECK(d3dkmt_destroy_allocations(7,n->device,1,f.base.allocations)==0); break;
        case 3: CHECK(d3dkmt_unregister_adapter(n->luid)==D3DKMT_STATUS_DEVICE_BUSY); break;
        case 4: vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); break;
        }
        NTSTATUS expected=mode%5==3 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_PROCESS_IS_TERMINATING;
        if (mode<5) CHECK(intel_gpu_jobs_take(&f.jobs,&a)==expected && !a.self);
        intel_gpu_service_notify(NULL); service_step();
        CHECK(f.done_count==1 && f.done_ids[0]==first && f.done_status[0]==expected);
        jobs_cleanup(&f);
    }
    jobs_setup(&f);
    CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&first)==0);
    CHECK(intel_gpu_jobs_take(&f.jobs,&a)==0);
    intel_gpu_buffer_t *held=a.capture->resources[0].native.buffer;
    __atomic_store_n(&held->busy,1,__ATOMIC_RELEASE);
    CHECK(intel_gpu_jobs_retire(&a,STATUS_CANCELLED)==0); service_step();
    CHECK(!f.done_count && intel_gpu_jobs_need_retry(&f.jobs) && waited_ticks==1);
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[0],&cpu)==D3DKMT_STATUS_DEVICE_BUSY);
    __atomic_store_n(&held->busy,0,__ATOMIC_RELEASE);
    service_step(); CHECK(f.done_count==1 && !intel_gpu_jobs_need_retry(&f.jobs));
    CHECK(intel_gpu_kmt_acquire_cpu(7,f.base.service.native.device,f.base.allocations[0],&cpu)==0);
    CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    jobs_cleanup(&f);

    jobs_setup(&f);
    intel_gpu_capture_t *capture=&f.base.capture;
    CHECK(intel_gpu_capture_begin(7,f.base.context,128,64,2,2,capture)==0);
    const D3DKMT_ALLOCATION_LEASE *resources[2]={&capture->resources[0].reference,&capture->resources[1].reference};
    D3DKMT_CONTEXT_LEASE forged=capture->context.reference;
    unsigned publication=D3DKMT_WORK_PREPARED;
    CHECK(d3dkmt_admit_context_work(&forged,resources,2,&publication,0,1)==STATUS_INVALID_PARAMETER && !publication);
    forged.self=&forged; forged.device_cookie++;
    CHECK(d3dkmt_context_work_status(&forged,resources,2)==STATUS_INVALID_PARAMETER);
    D3DKMT_ALLOCATION_LEASE wrong=*resources[0]; const D3DKMT_ALLOCATION_LEASE *bad[2]={&wrong,resources[1]};
    CHECK(d3dkmt_admit_context_work(&capture->context.reference,bad,2,&publication,0,1)==STATUS_INVALID_PARAMETER && !publication);
    wrong.self=&wrong; wrong.cookie++;
    CHECK(d3dkmt_context_work_status(&capture->context.reference,bad,2)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_admit_context_work(&capture->context.reference,resources,2,&publication,0,1)==0 && publication==1);
    CHECK(d3dkmt_admit_context_work(&capture->context.reference,resources,2,&publication,0,1)==STATUS_PROCESS_IS_TERMINATING && publication==1);
    CHECK(d3dkmt_destroy_context(7,f.base.context)==0);
    CHECK(d3dkmt_admit_context_work(&capture->context.reference,resources,2,&publication,1,2)==STATUS_PROCESS_IS_TERMINATING && publication==1);
    CHECK(intel_gpu_capture_release(capture)==0); jobs_cleanup(&f);
}
static void *jobs_submit_thread(void *opaque)
{
    JOBS_FIXTURE *f=opaque;
    f->submit_status=intel_gpu_jobs_submit(&f->jobs,7,f->base.context,128,64,2,2,&f->submitted);
    return NULL;
}
static void *jobs_collect_thread(void *opaque)
{
    (void)opaque; service_step(); return NULL;
}
static void jobs_races(void)
{
    for (unsigned round=0;round<64;round++) {
        JOBS_FIXTURE f; jobs_setup(&f); pthread_t thread;
        if (round<32) {
            f.ready_gate=1;
            CHECK(!pthread_create(&thread,NULL,jobs_submit_thread,&f));
        } else {
            CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&f.submitted)==0);
            intel_gpu_job_lease_t lease={0};
            CHECK(intel_gpu_jobs_take(&f.jobs,&lease)==0);
            CHECK(intel_gpu_jobs_retire(&lease,STATUS_CANCELLED)==0);
            f.done_gate=1;
            CHECK(!pthread_create(&thread,NULL,jobs_collect_thread,&f));
        }
        while (!__atomic_load_n(&f.entered,__ATOMIC_ACQUIRE)) sched_yield();
        intel_gpu_jobs_shutdown(&f.jobs);
        if (round<32) service_step();
        CHECK(!intel_gpu_jobs_empty(&f.jobs));
        CHECK(intel_gpu_service_detach(f.base.service.ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        __atomic_store_n(&f.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL)); service_step();
        CHECK(f.done_count==1 && f.done_ids[0]==f.submitted && f.done_status[0]==STATUS_CANCELLED);
        CHECK(f.submit_status==0 && intel_gpu_jobs_empty(&f.jobs));
        jobs_cleanup(&f);
    }
}
static unsigned jobs_event_refs,jobs_event_signals,jobs_event_collect;
static NTSTATUS jobs_event_retain(DWORD pid,uint64_t handle,PVOID *out)
{
    CHECK(pid==7 && handle==0x40);
    jobs_event_refs++; *out=&jobs_event_refs; return STATUS_SUCCESS;
}
static void jobs_event_signal(PVOID object,DWORD pid)
{
    CHECK(object==&jobs_event_refs && jobs_event_refs && pid==7 && jobs_event_collect);
    d3dkmt_release_process(999); /* callback is outside registry lock */
    jobs_event_signals++;
}
static void jobs_event_release(PVOID object)
{
    CHECK(object==&jobs_event_refs && jobs_event_refs && jobs_event_collect);
    d3dkmt_release_process(999);
    jobs_event_refs--;
}
static const D3DKMT_SYNC_EVENT_PROVIDER jobs_events={jobs_event_retain,jobs_event_signal,jobs_event_release};
static void jobs_sync_cases(void)
{
    /* Reuse one large fixture across sequential cases; ASan keeps lexical
     * stack slots apart even when their lifetimes do not overlap. */
    JOBS_FIXTURE f;
    for (unsigned teardown=0;teardown<7;teardown++) {
        jobs_setup(&f); CONTEXT_FIXTURE *n=&f.base.service.native;
        jobs_event_refs=jobs_event_signals=jobs_event_collect=0;
        D3DKMT_SYNC_COMMAND event={.signal=TRUE,.extended=TRUE,.event_handle=0x40,.events=&jobs_events};
        uint64_t copy=0,notification=0; intel_gpu_job_lease_t lease={0},next={0};
        CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&copy)==0);
        CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,f.base.context,&event,&notification)==0 && jobs_event_refs==1);
        CHECK(intel_gpu_jobs_take(&f.jobs,&lease)==0 && lease.id==copy);
        CHECK(intel_gpu_jobs_take(&f.jobs,&next)==D3DKMT_STATUS_DEVICE_BUSY && !next.self);
        service_step(); CHECK(!jobs_event_signals && jobs_event_refs==1);
        switch(teardown) {
        case 1:CHECK(intel_gpu_jobs_cancel(&f.jobs,7,notification)==0);break;
        case 2:CHECK(d3dkmt_destroy_context(7,f.base.context)==0);break;
        case 3:CHECK(d3dkmt_destroy_device(7,n->device)==0);break;
        case 4:CHECK(d3dkmt_unregister_adapter(n->luid)==D3DKMT_STATUS_DEVICE_BUSY);break;
        case 5:vm_model_exit(7); process_closing=true; d3dkmt_release_process(7);break;
        }
        CHECK(intel_gpu_jobs_retire(&lease,0)==0);
        NTSTATUS take=intel_gpu_jobs_take(&f.jobs,&next);
        CHECK(take!=STATUS_SUCCESS && !next.self);
        CHECK(!jobs_event_signals && jobs_event_refs==1); /* no NT signal on the AP */
        if (teardown==6) CHECK(intel_gpu_jobs_cancel(&f.jobs,7,notification)==D3DKMT_STATUS_DEVICE_BUSY);
        jobs_event_collect=1; intel_gpu_service_notify(NULL); service_step();
        CHECK(jobs_event_signals==(teardown==0 || teardown==6) && !jobs_event_refs);
        service_step(); CHECK(!jobs_event_refs && jobs_event_signals==(teardown==0 || teardown==6));
        CHECK(f.done_count==2); jobs_cleanup(&f); jobs_event_collect=0;
    }
    for (unsigned type=1;type<=3;type++) {
        jobs_setup(&f); CONTEXT_FIXTURE *n=&f.base.service.native;
        D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        OKMT_HANDLE other=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_create_context(7,n->device,&desc,&other,&buffers)==0);
        D3DKMT_SYNC_DESCRIPTION info={.type=type,.initial=type==1 ? 1 : 0,.maximum=2};
        OKMT_HANDLE object=0;
        CHECK(d3dkmt_create_sync(7,n->device,&info,&object)==0 && object);
        CHECK(d3dkmt_close_adapter(7,object)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_sync(8,object)==STATUS_INVALID_PARAMETER);
        D3DKMT_SYNC_COMMAND wait={.count=1,.objects={object},.value=8,.extended=TRUE};
        D3DKMT_SYNC_COMMAND signal=wait; signal.signal=TRUE;
        uint64_t w=0,copy=0,s=0; intel_gpu_job_lease_t lease={0};
        CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,f.base.context,&wait,&w)==0);
        CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&copy)==0);
        CHECK(intel_gpu_jobs_take(&f.jobs,&lease)==D3DKMT_STATUS_DEVICE_BUSY && !lease.self);
        service_step(); CHECK(f.done_count==0);
        CHECK(intel_gpu_jobs_pending(&f.jobs,7,f.base.context)==2);
        CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,other,&signal,&s)==0);
        /* Destroy rejects new uses but must preserve the two queued references. */
        CHECK(d3dkmt_destroy_sync(7,object)==0);
        uint64_t rejected=0xfeed;
        CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,other,&signal,&rejected)==STATUS_INVALID_PARAMETER && rejected==0xfeed);
        service_step();
        CHECK(intel_gpu_jobs_take(&f.jobs,&lease)==0 && lease.id==copy);
        service_step(); CHECK(f.done_count==2);
        CHECK(intel_gpu_jobs_retire(&lease,0)==0); service_step(); CHECK(f.done_count==3);
        CHECK(d3dkmt_destroy_sync(7,object)==STATUS_INVALID_PARAMETER);
        jobs_cleanup(&f);
    }
    for (unsigned teardown=0;teardown<5;teardown++) {
        jobs_setup(&f); CONTEXT_FIXTURE *n=&f.base.service.native;
        OKMT_HANDLE object=0; D3DKMT_SYNC_DESCRIPTION info={.type=3};
        CHECK(d3dkmt_create_sync(7,n->device,&info,&object)==0);
        D3DKMT_SYNC_COMMAND signal={.count=1,.objects={object},.value=10,.extended=TRUE,.signal=TRUE};
        uint64_t copy=0,s=0; intel_gpu_job_lease_t lease={0},next={0};
        CHECK(intel_gpu_jobs_submit(&f.jobs,7,f.base.context,128,64,2,2,&copy)==0);
        CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,f.base.context,&signal,&s)==0);
        CHECK(intel_gpu_jobs_take(&f.jobs,&lease)==0 && lease.id==copy);
        CHECK(intel_gpu_jobs_take(&f.jobs,&next)==D3DKMT_STATUS_DEVICE_BUSY && !next.self);
        service_step(); CHECK(!f.done_count);
        switch (teardown) {
        case 0: CHECK(intel_gpu_jobs_cancel(&f.jobs,7,s)==0); break;
        case 1: CHECK(d3dkmt_destroy_context(7,f.base.context)==0); break;
        case 2: CHECK(d3dkmt_destroy_device(7,n->device)==0); break;
        case 3: CHECK(d3dkmt_unregister_adapter(n->luid)==D3DKMT_STATUS_DEVICE_BUSY); break;
        case 4: vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); break;
        }
        intel_gpu_service_notify(NULL); service_step();
        CHECK(f.done_count==1 && f.done_ids[0]==s && f.done_status[0]!=STATUS_SUCCESS);
        CHECK(lease.self==&lease); /* logical close cannot free claimed work */
        CHECK(intel_gpu_jobs_retire(&lease,STATUS_CANCELLED)==0); service_step();
        CHECK(f.done_count==2); jobs_cleanup(&f);
    }
    jobs_setup(&f); CONTEXT_FIXTURE *n=&f.base.service.native;
    D3DKMT_SYNC_DESCRIPTION info={.type=2,.initial=1,.maximum=2}; OKMT_HANDLE objects[2];
    CHECK(d3dkmt_create_sync(7,n->device,&info,&objects[0])==0);
    info.initial=0; CHECK(d3dkmt_create_sync(7,n->device,&info,&objects[1])==0);
    D3DKMT_SYNC_COMMAND wait={.count=2,.objects={objects[0],objects[1]},.extended=TRUE};
    D3DKMT_SYNC_LEASE held={0};
    CHECK(d3dkmt_sync_capture(7,f.base.context,&wait,&held)==0);
    unsigned publication=D3DKMT_WORK_QUEUED;
    CHECK(d3dkmt_sync_step(&held,&publication)==D3DKMT_STATUS_DEVICE_BUSY && publication==D3DKMT_WORK_QUEUED);
    CHECK(d3dkmt_sync_release(&held)==0);
    wait.count=1;
    CHECK(d3dkmt_sync_capture(7,f.base.context,&wait,&held)==0);
    CHECK(d3dkmt_sync_step(&held,&publication)==0); /* failed wait-all consumed nothing */
    CHECK(d3dkmt_sync_step(&held,&publication)!=0); /* no double consumption */
    CHECK(d3dkmt_sync_release(&held)==0);
    wait.count=2; wait.objects[1]=wait.objects[0];
    CHECK(d3dkmt_sync_capture(7,f.base.context,&wait,&held)==STATUS_INVALID_PARAMETER && !held.self);
    jobs_cleanup(&f);
}
#ifndef NATIVE_JOBS_SUITE_ENTRY
#define NATIVE_JOBS_SUITE_ENTRY main
#endif
int NATIVE_JOBS_SUITE_ENTRY(void)
{
    if (capture_suite_main()) return 1;
    unsigned before=checks;
    jobs_cases(); jobs_races(); jobs_sync_cases();
    printf("Intel persistent capture jobs: %u checks passed, 64 forced callback/shutdown races\n",checks-before);
    return 0;
}
