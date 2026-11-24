#define _GNU_SOURCE
#define NATIVE_JOBS_SUITE_ENTRY jobs_suite_main
#include "intel_gpu_jobs_host.c"
#undef CHECK
#define checks bcs_checks
#define main bcs_suite_main
#include "intel_bcs_host.c"
#undef main
#undef checks
#undef CHECK
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if (!(x)) { \
    fprintf(stderr,"FAIL native submit %d: %s\n",__LINE__,#x); abort(); \
} } while (0)

/* Production capture/KMT/resource/context/queue code, with the existing BCS
 * register model and synthetic DMA allocator. No application renderer here. */
typedef struct {
    JOBS_FIXTURE jobs;
    intel_gpu_job_lease_t lease;
    intel_bcs_context_t native;
    intel_ggtt_space_t space;
    uint8_t bitmap[4];
    intel_bcs_queue_t queue;
    intel_bcs_state_t engine;
    unsigned gate,entered,resume;
    NTSTATUS status;
} SUBMIT_FIXTURE;
static bool submit_read(void *opaque,uint32_t reg,uint32_t *value)
{
    SUBMIT_FIXTURE *f=opaque;
    bool result=bcs_model_read(&hw,reg,value);
    if (hw.emitted && f->native.state.cpu) {
        uint32_t *saved=(uint32_t *)f->native.state.cpu+1024;
        saved[5]=hw.tail; saved[7]=hw.tail; /* modeled hardware context save */
    }
    return result;
}
static bool submit_write(void *opaque,uint32_t reg,uint32_t value)
{
    (void)opaque; return bcs_model_write(&hw,reg,value);
}
static uint64_t submit_now(void *opaque) { (void)opaque; return bcs_model_now(&hw); }
static void submit_relax(void *opaque) { (void)opaque; bcs_model_relax(&hw); }
static bool submit_prepare(void *opaque,const intel_bcs_copy_t *copy,uint64_t token,
    uint64_t *descriptor,uint32_t *tail)
{
    SUBMIT_FIXTURE *f=opaque;
    bool result=intel_bcs_context_prepare(&f->native,copy,token,descriptor,tail);
    if (result) { hw.next_token=token; hw.tail=*tail; hw.expected_descriptor=*descriptor; }
    return result;
}
static bool submit_token(void *opaque,uint64_t *value)
{
    (void)opaque; return bcs_model_token(&hw,value);
}
static enum intel_bcs_status submit_acquire(void *opaque,uint64_t token)
{
    SUBMIT_FIXTURE *f=opaque; return intel_bcs_context_acquire(&f->native,token);
}
static enum intel_bcs_status submit_retire(void *opaque,uint64_t token)
{
    SUBMIT_FIXTURE *f=opaque; return intel_bcs_context_retire(&f->native,token);
}
static void submit_wakeup(void *opaque,uint32_t events)
{
    SUBMIT_FIXTURE *f=opaque; (void)events;
    intel_bcs_queue_snapshot_t snapshot;
    CHECK(intel_bcs_queue_snapshot(&f->queue,&snapshot)==INTEL_BCS_OK);
    if (snapshot.staged && __atomic_exchange_n(&f->gate,0,__ATOMIC_ACQ_REL)) {
        __atomic_store_n(&f->entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void submit_setup_with_cpu(SUBMIT_FIXTURE *f,intel_gpu_kmt_cpu_lease_t *cpu)
{
    memset(f,0,sizeof(*f)); jobs_setup(&f->jobs); bcs_model_reset();
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
static void submit_setup(SUBMIT_FIXTURE *f) { submit_setup_with_cpu(f,NULL); }
static void submit_cleanup(SUBMIT_FIXTURE *f)
{
    intel_gpu_jobs_shutdown(&f->jobs.jobs);
    CHECK(intel_bcs_queue_shutdown(&f->queue)==INTEL_BCS_OK);
    if (f->lease.self) {
        NTSTATUS status=intel_gpu_jobs_poll(&f->lease);
        if (status==STATUS_INVALID_PARAMETER) CHECK(intel_gpu_jobs_retire(&f->lease,STATUS_CANCELLED)==0);
        else CHECK(status==0);
    }
    CHECK(intel_bcs_queue_unwatch(&f->queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unpublish(&f->queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_context_destroy(&f->native));
    service_step(); CHECK(f->jobs.done_count==1);
    jobs_cleanup(&f->jobs);
}
/* Keep device-sized fixtures out of the nested baseline-suite entry frames.
 * Preserve automatic storage and ASan scope checks without accumulating every
 * suite's queue on the host main stack before its baseline call returns. */
static __attribute__((noinline)) void submit_cases(void)
{
    for (unsigned mode=0;mode<6;mode++) {
        SUBMIT_FIXTURE f;
        intel_gpu_kmt_cpu_lease_t cpu={0};
        submit_setup_with_cpu(&f,mode==2 ? &cpu : NULL);
        CHECK(intel_gpu_jobs_enqueue(&f.lease,&f.native)==0);
        CHECK(intel_gpu_jobs_enqueue(&f.lease,&f.native)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_jobs_retire(&f.lease,0)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_jobs_poll(&f.lease)==D3DKMT_STATUS_DEVICE_BUSY && f.lease.self);
        if (mode==1) {
            CHECK(intel_gpu_jobs_cancel(&f.jobs.jobs,7,f.lease.id)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(intel_gpu_jobs_poll(&f.lease)==0 && !f.engine.submitted_jobs);
        } else {
            if (mode==2) {
                CHECK(intel_bcs_queue_step(&f.queue)==INTEL_BCS_BUSY && !f.engine.submitted_jobs);
                CHECK(intel_gpu_jobs_poll(&f.lease)==D3DKMT_STATUS_DEVICE_BUSY);
                CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
            }
            CHECK(intel_bcs_queue_step(&f.queue)==INTEL_BCS_PENDING);
            if (mode>=3) {
                CHECK(intel_gpu_jobs_cancel(&f.jobs.jobs,7,f.lease.id)==D3DKMT_STATUS_DEVICE_BUSY);
                if (mode==3) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
                if (mode==4) {
                    hw.stall_stop=true;
                    CHECK(intel_bcs_queue_shutdown(&f.queue)==INTEL_BCS_QUARANTINED);
                    CHECK(intel_gpu_jobs_poll(&f.lease)==D3DKMT_STATUS_DEVICE_BUSY && f.native.source_access.self);
                    hw.stall_stop=false;
                    CHECK(intel_bcs_queue_shutdown(&f.queue)==INTEL_BCS_OK);
                }
            }
            if (mode!=4) {
                enum intel_bcs_status status;
                do { status=intel_bcs_queue_step(&f.queue); } while (status==INTEL_BCS_PENDING);
                CHECK(status==INTEL_BCS_OK);
                /* A later admission/worker pass must not reap the result when
                 * the PE owner has exited: the persistent driver still owns it. */
                CHECK(intel_bcs_queue_step(&f.queue)==INTEL_BCS_OK);
            }
            CHECK(intel_gpu_jobs_poll(&f.lease)==0 && !f.lease.self);
        }
        CHECK(!f.jobs.done_count); service_step();
        CHECK(f.jobs.done_count==1 && f.jobs.done_status[0]==(mode==1 || mode==4 ? STATUS_CANCELLED : STATUS_SUCCESS));
        submit_cleanup(&f);
    }
    SUBMIT_FIXTURE f; submit_setup(&f);
    uint64_t fences[INTEL_BCS_QUEUE_JOBS];
    intel_bcs_copy_t copy;
    CHECK(intel_gpu_capture_copy(f.lease.capture,&f.native,&copy)==0);
    for (unsigned i=0;i<INTEL_BCS_QUEUE_JOBS;i++)
        CHECK(intel_bcs_queue_enqueue(&f.queue,7,f.native.queue_id,&copy,&fences[i])==INTEL_BCS_OK);
    CHECK(intel_gpu_jobs_enqueue(&f.lease,&f.native)==D3DKMT_STATUS_DEVICE_BUSY);
    intel_bcs_job_result_t result;
    CHECK(intel_bcs_queue_cancel(&f.queue,7,fences[0])==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_result(&f.queue,7,fences[0],true,&result)==INTEL_BCS_OK);
    CHECK(intel_gpu_jobs_enqueue(&f.lease,&f.native)==0);
    submit_cleanup(&f);
}
static void *submit_thread(void *opaque)
{
    SUBMIT_FIXTURE *f=opaque; f->status=intel_gpu_jobs_enqueue(&f->lease,&f->native); return NULL;
}
static __attribute__((noinline)) void submit_races(void)
{
    for (unsigned round=0;round<64;round++) {
        SUBMIT_FIXTURE f; submit_setup(&f); f.gate=1;
        unsigned writes=hw.writes;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,submit_thread,&f));
        while (!__atomic_load_n(&f.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_bcs_queue_step(&f.queue)==INTEL_BCS_OK && hw.writes==writes);
        CONTEXT_FIXTURE *n=&f.jobs.base.service.native;
        switch (round%8) {
        case 0: CHECK(d3dkmt_destroy_context(7,f.jobs.base.context)==0); break;
        case 1: CHECK(d3dkmt_destroy_device(7,n->device)==0); break;
        case 2: CHECK(d3dkmt_destroy_allocations(7,n->device,1,f.jobs.base.allocations)==0); break;
        case 3: CHECK(d3dkmt_unregister_adapter(n->luid)==D3DKMT_STATUS_DEVICE_BUSY); break;
        case 4: vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); break;
        case 5: CHECK(intel_gpu_jobs_cancel(&f.jobs.jobs,7,f.lease.id)==D3DKMT_STATUS_DEVICE_BUSY); break;
        case 6: intel_gpu_jobs_shutdown(&f.jobs.jobs); break;
        case 7: CHECK(intel_bcs_queue_close_context(&f.queue,7,f.native.queue_id)==INTEL_BCS_OK); break;
        }
        __atomic_store_n(&f.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        CHECK(f.status!=0 && intel_bcs_queue_step(&f.queue)==INTEL_BCS_OK && hw.writes==writes && !f.engine.submitted_jobs);
        CHECK(intel_gpu_jobs_poll(&f.lease)==0 && !f.lease.self);
        service_step(); CHECK(f.jobs.done_count==1 && f.jobs.done_status[0]==STATUS_CANCELLED);
        submit_cleanup(&f);
    }
}
#ifndef NATIVE_SUBMIT_SUITE_ENTRY
#define NATIVE_SUBMIT_SUITE_ENTRY main
#endif
int NATIVE_SUBMIT_SUITE_ENTRY(void)
{
    if (jobs_suite_main() || bcs_suite_main()) return 1;
    unsigned before=checks;
    submit_cases(); submit_races();
    printf("Intel captured native submission: %u checks passed, 64 forced admission/close races\n",checks-before);
    return 0;
}
