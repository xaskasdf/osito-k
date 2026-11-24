/* Real fence owners, contexts and page tables; only RAM/VM/MMIO are modeled. */
#define INTEL_COHERENT_SUITE_ENTRY fence_mapping_regression_main
#include "intel_gpu_context_coherent_host.c"

static void mapping_collect(intel_gpu_fences_t *fences)
{
    for (unsigned i=0;i<INTEL_GPU_FENCES+1;i++) {
        (void)intel_gpu_fences_collect(fences);
        (void)intel_gpu_memory_collect(fences->memory);
    }
}
static void mapping_cases(void)
{
    for (unsigned mode=0;mode<10;mode++) {
        OWNER_FIXTURE f;intel_bcs_context_t *c=coherent_context_start(&f);
        CONTEXT_FIXTURE *n=&f.jobs.base.service.native;
        DWORD device=n->contexts.contexts[0].device;
        intel_gpu_fences_t fences={0};intel_gpu_fence_lease_t lease={0},bad={0};
        intel_gpu_fence_mapping_t mapping={0},duplicate={0};
        uint64_t cookie=0,address=0;
        CHECK(intel_gpu_fences_init(&fences,&n->memory,intel_gpu_service_notify,NULL));
        CHECK(intel_gpu_fence_create(&fences,7,device,42,mode&1,&cookie,&address)==0);
        CHECK(intel_gpu_fence_acquire(&fences,7,device,cookie,&lease)==0);
        uint64_t gpu=lease.gpu,dma=lease.dma;
        CHECK(gpu==INTEL_GPU_FENCE_VA_BASE && gpu!=lease.physical && gpu!=lease.dma);
        intel_gpu_fence_lease_t copied=lease;
        CHECK(intel_gpu_fence_retain(&copied,&bad)==STATUS_INVALID_PARAMETER && !bad.self);
        CHECK(intel_gpu_fence_map(&copied,c,&n->memory,device,&coherent_policy,&mapping)==INTEL_BCS_INVALID);
        CHECK(intel_gpu_fence_map(&lease,c,&n->memory,device+1,&coherent_policy,&mapping)==INTEL_BCS_INVALID);
        CHECK(intel_gpu_fence_map(&lease,c,NULL,device,&coherent_policy,&mapping)==INTEL_BCS_INVALID);
        c->epoch++;
        CHECK(intel_gpu_fence_map(&lease,c,&n->memory,device,&coherent_policy,&mapping)==INTEL_BCS_INVALID);
        c->epoch--;
        CHECK(intel_gpu_fence_map(&lease,c,&n->memory,device,NULL,&mapping)==INTEL_BCS_INVALID && !mapping.self);
        CHECK(fences.fences[0].references==1);
        if (mode==1) {
            CHECK(intel_gpu_fence_close(&fences,7,device,cookie)==0);
            mapping_collect(&fences);
            CHECK(fences.fences[0].vm_done && fences.fences[0].references==1);
            CHECK(intel_gpu_fence_acquire(&fences,7,device,cookie,&bad)==STATUS_INVALID_PARAMETER);
        }
        if (mode==5) { recycle_fail_tlb=true;f.owner.config.io.read=recycle_fault_read; }
        CHECK(intel_gpu_fence_map(&lease,c,&n->memory,device,&coherent_policy,&mapping)==
            (mode==5?INTEL_BCS_QUARANTINED:INTEL_BCS_OK));
        CHECK(mapping.self && mapping.lease.gpu==gpu && fences.fences[0].references==2);
        CHECK(coherent_context_pte(c,gpu)==(dma|0x83));
        CHECK(intel_gpu_fence_map(&lease,c,&n->memory,device,&coherent_policy,&duplicate)==INTEL_BCS_INVALID && !duplicate.self);
        intel_gpu_fence_mapping_t copy=mapping;
        CHECK(intel_gpu_fence_unmap(&copy)==INTEL_BCS_INVALID);
        if (mode==2) {
            uint64_t job=0;hw.delay=1000;
            CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
            executor_step();
            CHECK(intel_gpu_fence_unmap(&mapping)==INTEL_BCS_BUSY && mapping.self);
            hw.delay=0;
            for (unsigned i=0;i<8 && f.jobs.done_count<2;i++) { executor_step();service_step(); }
            CHECK(f.jobs.done_count==2 && !f.jobs.done_status[1]);
        }
        if (mode==6) { recycle_fail_tlb=true;f.owner.config.io.read=recycle_fault_read; }
        if (mode==8) ram.fail_sync=ram.syncs+1;
        if (mode==9) mapping.lease.buffer->busy=1;
        if (mode==5 || mode==6 || mode==8 || mode==9) {
            CHECK(intel_gpu_fence_unmap(&mapping)==INTEL_BCS_QUARANTINED && mapping.self);
            CHECK(fences.fences[0].references==2 && mapping.lease.buffer->residents==1);
            recycle_fail_tlb=false;ram.fail_sync=0;mapping.lease.buffer->busy=0;
        }
        if (mode!=1) CHECK(intel_gpu_fence_close(&fences,7,device,cookie)==0);
        CHECK(intel_gpu_fence_release(&lease)==0);
        mapping_collect(&fences);
        CHECK(fences.fences[0].references==1 && fences.fences[0].resource && mapping.lease.self);
        if (mode==7) {
            /* Logical device teardown cannot release this mapping or its manager. */
            CHECK(intel_gpu_memory_close_device(&n->memory,7,device)==INTEL_GPU_BUFFER_OK);
            mapping_collect(&fences);CHECK(mapping.lease.self && fences.fences[0].resource);
        }
        intel_gpu_buffer_t replacements[2]={0};
        if (mode==3) {
            for (unsigned i=0;i<2;i++) CHECK(intel_gpu_buffer_create(&replacements[i],&allocator,&ram,8,4096)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_bcs_queue_close_context(&f.owner.queue,7,c->queue_id)==INTEL_BCS_OK);
            CHECK(intel_bcs_context_retarget(c,8,123,&replacements[0],&replacements[1])==INTEL_BCS_OK);
            CHECK(!coherent_context_pte(c,gpu));
        }
        bool stop_first=mode==4 || mode==5 || mode==6 || mode==8 || mode==9;
        if (!stop_first) CHECK(intel_gpu_fence_unmap(&mapping)==INTEL_BCS_OK && !mapping.self);
        NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&f.owner);executor_step();service_step(); }
        CHECK(!closed);
        CHECK(intel_gpu_fence_unmap(&mapping)==INTEL_BCS_OK && !mapping.self);
        CHECK(intel_gpu_fence_unmap(&mapping)==INTEL_BCS_OK);
        mapping_collect(&fences);CHECK(!fences.fences[0].id);
        if (mode==0) {
            /* Address exhaustion is explicit; success never aliases an old VA. */
            fences.next_id=INTEL_GPU_FENCE_VA_PAGES;
            CHECK(intel_gpu_fence_create(&fences,7,device,0,FALSE,&cookie,&address)==0);
            CHECK(intel_gpu_fence_acquire(&fences,7,device,cookie,&lease)==0);
            CHECK(lease.gpu==INTEL_GPU_FENCE_VA_BASE+(INTEL_GPU_FENCE_VA_PAGES-1)*4096 && lease.gpu!=gpu);
            uint64_t untouched=0xfeed,cpu=0xbeef;
            CHECK(intel_gpu_fence_create(&fences,7,device,0,FALSE,&untouched,&cpu)==STATUS_INSUFFICIENT_RESOURCES);
            CHECK(untouched==0xfeed && cpu==0xbeef);
            CHECK(intel_gpu_fence_close(&fences,7,device,cookie)==0 && intel_gpu_fence_release(&lease)==0);
        }
        intel_gpu_fences_shutdown(&fences);mapping_collect(&fences);
        CHECK(intel_gpu_fences_empty(&fences));
        if (mode==3) for (unsigned i=0;i<2;i++) CHECK(intel_gpu_buffer_close(&replacements[i],8)==INTEL_GPU_BUFFER_OK);
        owner_cleanup(&f);
    }
}
static struct {
    const intel_gpu_fence_lease_t *lease;
    intel_bcs_context_t *context;
    intel_gpu_memory_t *memory;
    DWORD device;
    intel_gpu_fence_mapping_t mapping;
    enum intel_bcs_status result;
} mapping_race;
static void *mapping_race_thread(void *unused)
{
    (void)unused;
    mapping_race.result=intel_gpu_fence_map(mapping_race.lease,mapping_race.context,
        mapping_race.memory,mapping_race.device,&coherent_policy,&mapping_race.mapping);
    return NULL;
}
static void mapping_races(void)
{
    for (unsigned round=0;round<16;round++) {
        OWNER_FIXTURE f;intel_bcs_context_t *c=coherent_context_start(&f);
        CONTEXT_FIXTURE *n=&f.jobs.base.service.native;
        DWORD device=n->contexts.contexts[0].device;
        intel_gpu_fences_t fences={0};intel_gpu_fence_lease_t lease={0};
        uint64_t cookie=0,address=0;
        CHECK(intel_gpu_fences_init(&fences,&n->memory,intel_gpu_service_notify,NULL));
        CHECK(intel_gpu_fence_create(&fences,7,device,42,round&1,&cookie,&address)==0);
        CHECK(intel_gpu_fence_acquire(&fences,7,device,cookie,&lease)==0);
        memset(&mapping_race,0,sizeof(mapping_race));
        mapping_race.lease=&lease;mapping_race.context=c;mapping_race.memory=&n->memory;mapping_race.device=device;
        ram.hold=1;pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,mapping_race_thread,NULL));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(fences.fences[0].references==2 && lease.buffer->residents==1);
        CHECK(intel_gpu_fence_close(&fences,7,device,cookie)==0);
        if (!(round&1)) CHECK(intel_bcs_queue_close_context(&f.owner.queue,7,c->queue_id)==INTEL_BCS_OK);
        mapping_collect(&fences);
        CHECK(fences.fences[0].vm_done && fences.fences[0].resource);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));ram.hold=0;
        CHECK(mapping_race.result==INTEL_BCS_OK && mapping_race.mapping.self);
        CHECK(intel_gpu_fence_release(&lease)==0);
        mapping_collect(&fences);CHECK(fences.fences[0].references==1);
        CHECK(intel_gpu_fence_unmap(&mapping_race.mapping)==INTEL_BCS_OK && !mapping_race.mapping.self);
        NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&f.owner);executor_step();service_step(); }
        CHECK(!closed && intel_gpu_fence_unmap(&mapping_race.mapping)==INTEL_BCS_OK);
        intel_gpu_fences_shutdown(&fences);mapping_collect(&fences);
        CHECK(intel_gpu_fences_empty(&fences));owner_cleanup(&f);
    }
}
static void mapping_multi_context(void)
{
    OWNER_FIXTURE f;owner_setup(&f);
    CHECK(intel_gpu_capture_release(&f.bootstrap)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&f.owner,2,8)==0);
    CHECK(intel_gpu_bcs_owner_start(&f.owner)==0);
    CONTEXT_FIXTURE *n=&f.jobs.base.service.native;
    D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
    memcpy(desc.private_data,&command_layout,sizeof(command_layout));
    D3DKMT_CONTEXT_BUFFERS buffers={0};OKMT_HANDLE second=0;
    CHECK(d3dkmt_create_context(7,n->device,&desc,&second,&buffers)==0);
    intel_gpu_command_context_t *first=&n->contexts.contexts[0],*other=&n->contexts.contexts[1];
    CHECK(first->cpu.bytes==other->cpu.bytes);
    memcpy(other->cpu.cpu,first->cpu.cpu,first->cpu.bytes);
    uint64_t job=0;
    CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
    CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,second,128,64,2,2,&job)==0);
    for (unsigned i=0;i<16 && f.jobs.done_count<2;i++) { executor_step();service_step(); }
    CHECK(f.jobs.done_count==2 && !f.jobs.done_status[0] && !f.jobs.done_status[1]);
    intel_gpu_fences_t fences={0};intel_gpu_fence_lease_t lease={0};
    intel_gpu_fence_mapping_t maps[2]={0};uint64_t cookie=0,address=0;
    CHECK(intel_gpu_fences_init(&fences,&n->memory,intel_gpu_service_notify,NULL));
    CHECK(intel_gpu_fence_create(&fences,7,first->device,42,FALSE,&cookie,&address)==0);
    CHECK(intel_gpu_fence_acquire(&fences,7,first->device,cookie,&lease)==0);
    uint64_t gpu=lease.gpu,dma=lease.dma;
    for (unsigned i=0;i<2;i++) {
        intel_bcs_context_t *c=&f.owner.slots[i].native;
        CHECK(intel_gpu_fence_map(&lease,c,&n->memory,first->device,&coherent_policy,&maps[i])==INTEL_BCS_OK);
        CHECK(coherent_context_pte(c,gpu)==(dma|0x83));
    }
    CHECK(lease.buffer->residents==2 && fences.fences[0].references==3);
    CHECK(intel_gpu_fence_close(&fences,7,first->device,cookie)==0 && intel_gpu_fence_release(&lease)==0);
    CHECK(intel_gpu_fence_unmap(&maps[0])==INTEL_BCS_OK);
    mapping_collect(&fences);
    CHECK(!coherent_context_pte(&f.owner.slots[0].native,gpu));
    CHECK(coherent_context_pte(&f.owner.slots[1].native,gpu)==(dma|0x83));
    CHECK(fences.fences[0].references==1 && fences.fences[0].resource && maps[1].lease.buffer->residents==1);
    CHECK(intel_gpu_fence_unmap(&maps[1])==INTEL_BCS_OK);
    intel_gpu_fences_shutdown(&fences);mapping_collect(&fences);
    CHECK(intel_gpu_fences_empty(&fences));owner_cleanup(&f);
}
int main(void)
{
    if (fence_mapping_regression_main()) return 1;
    unsigned before=checks;
    mapping_cases();mapping_races();mapping_multi_context();
    printf("Intel managed fence mappings: %u checks passed, 10 lifetime/failure cases, 16 close races, shared VA in two contexts\n",checks-before);
    return 0;
}
