/* Production capture/executor/owner path with private fence creation.
 * Public monitored-fence calls are not supplied by this fixture. */
#define INTEL_RECYCLE_SUITE_ENTRY owner_fence_regression_main
#include "intel_gpu_recycle_host.c"

static void owner_fence_collect(intel_gpu_fences_t *fences)
{
    for (unsigned i=0;i<INTEL_GPU_FENCES+1;i++) (void)intel_gpu_fences_collect(fences);
    service_step();
}
static void owner_fence_start(OWNER_FIXTURE *f,intel_gpu_fences_t *fences)
{
    owner_setup(f);
    intel_gpu_contexts_t *contexts=f->jobs.jobs.contexts;
    CHECK(intel_gpu_fences_init(fences,contexts->memory,intel_gpu_service_notify,NULL));
    CHECK(!intel_gpu_contexts_attach_fences(contexts,fences)); /* bootstrap retains context */
    CHECK(intel_gpu_capture_release(&f->bootstrap)==0);
    CHECK(intel_gpu_contexts_attach_fences(contexts,fences));
    CHECK(!intel_gpu_contexts_attach_fences(contexts,fences));
    CHECK(intel_gpu_bcs_owner_reserve(&f->owner,1,64)==0);
    CHECK(intel_gpu_bcs_owner_start(&f->owner)==0);
    CHECK(f->owner.coherent_pat.ready && (f->pat4&255)==7);
    CHECK(fences->mapping_wake==intel_gpu_executor_notify);
}
static uint64_t owner_fence_pte(intel_bcs_context_t *c,uint64_t address)
{
    uint64_t *table=c->tables.cpu;
    for (unsigned shift=39;shift>12;shift-=9) {
        uint64_t entry=table[(address>>shift)&511];
        if (!(entry&1)) return 0;
        uint64_t dma=entry&~4095ULL;
        CHECK(dma>=c->tables.dma && dma-c->tables.dma<c->tables.bytes);
        table=(uint64_t *)((BYTE *)c->tables.cpu+dma-c->tables.dma);
    }
    return table[(address>>12)&511];
}
static void owner_fence_finish(OWNER_FIXTURE *f,intel_gpu_fences_t *fences)
{
    intel_gpu_jobs_shutdown(&f->jobs.jobs);
    NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
    for (unsigned i=0;i<24 && closed;i++) {
        closed=intel_gpu_bcs_owner_close(&f->owner);executor_step();owner_fence_collect(fences);
    }
    CHECK(!closed && !f->owner.coherent_pat.changed && f->pat4==0x03221155);
    intel_gpu_fences_shutdown(fences);owner_fence_collect(fences);
    CHECK(intel_gpu_fences_empty(fences));owner_cleanup(f);
}
static void owner_fence_cases(void)
{
    for (unsigned mode=0;mode<8;mode++) {
        OWNER_FIXTURE f;intel_gpu_fences_t fences={0};owner_fence_start(&f,&fences);
        DWORD device=f.jobs.jobs.contexts->contexts[0].device;
        uint64_t id=0,cpu=0,job=0;
        CHECK(intel_gpu_fence_create(&fences,7,device,42,mode&1,&id,&cpu)==0);
        intel_gpu_fence_t *record=&fences.fences[0];
        uint64_t dma=record->coherent.access->dma,gpu=INTEL_GPU_FENCE_VA_BASE;
        hw.delay=1000;
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
        CHECK(f.jobs.jobs.jobs[0].capture.fence_count==1 && record->references==1);
        if (mode==1) { CHECK(intel_gpu_fence_close(&fences,7,device,id)==0);owner_fence_collect(&fences); }
        if (mode==5) { recycle_fail_tlb=true;f.owner.config.io.read=recycle_fault_read; }
        executor_step();
        intel_gpu_bcs_slot_t *slot=&f.owner.slots[0];
        CHECK(slot->fences[0].self && slot->fences[0].lease.id==id);
        CHECK(owner_fence_pte(&slot->native,gpu)==(dma|0x83));
        CHECK(record->references==2 && record->coherent.buffer->residents==1);
        if (mode==5) {
            CHECK(f.owner.queue.fault==INTEL_BCS_QUARANTINED && !slot->native.ready);
            recycle_fail_tlb=false;
        }
        if (mode==2 || mode==3 || mode==4) {
            CHECK(intel_gpu_fence_close(&fences,7,device,id)==0);owner_fence_collect(&fences);
            if (mode==3) CHECK(d3dkmt_destroy_context(7,f.jobs.base.context)==0);
            if (mode==4) { vm_model_exit(7);d3dkmt_release_process(7); }
            CHECK(record->references==2 && slot->fences[0].self);
        }
        hw.delay=0;
        if (mode!=5) {
            for (unsigned i=0;i<16 && !f.jobs.done_count;i++) { executor_step();owner_fence_collect(&fences); }
            CHECK(f.jobs.done_count==1 && !f.jobs.done_status[0]);
            if (mode==0 || mode==6 || mode==7) {
                CHECK(slot->fences[0].self && record->references==1);
                uint64_t updates=slot->native.rebindings;
                CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
                for (unsigned i=0;i<16 && f.jobs.done_count<2;i++) { executor_step();owner_fence_collect(&fences); }
                CHECK(f.jobs.done_count==2 && !f.jobs.done_status[1]);
                CHECK(slot->native.rebindings==updates); /* no redundant TLB update for reuse */
                CHECK(intel_gpu_fence_close(&fences,7,device,id)==0);
                if (mode==6) { recycle_fail_tlb=true;f.owner.config.io.read=recycle_fault_read; }
                if (mode==7) record->coherent.buffer->busy=1;
                executor_step();
                if (mode==6 || mode==7) {
                    CHECK(f.owner.queue.fault==INTEL_BCS_QUARANTINED && slot->fences[0].self);
                    recycle_fail_tlb=false;record->coherent.buffer->busy=0;
                }
            }
            if (mode<6) {
                for (unsigned i=0;i<8;i++) { executor_step();owner_fence_collect(&fences); }
                if (slot->fences[0].self || record->id || owner_fence_pte(&slot->native,gpu))
                    fprintf(stderr,"mapping close mode=%u map=%u id=%llu refs=%u pte=%llx queuefault=%u jobsclosing=%u\n",
                        mode,slot->fences[0].self!=NULL,(unsigned long long)record->id,record->references,
                        (unsigned long long)owner_fence_pte(&slot->native,gpu),f.owner.queue.fault,f.jobs.jobs.closing);
                CHECK(!slot->fences[0].self && !record->id && !owner_fence_pte(&slot->native,gpu));
                CHECK(!f.jobs.jobs.closing && f.owner.engine.started && !f.owner.engine.retired);
            }
        } else CHECK(intel_gpu_fence_close(&fences,7,device,id)==0);
        owner_fence_finish(&f,&fences);
    }
}
static void owner_fence_capacity(void)
{
    OWNER_FIXTURE f;intel_gpu_fences_t fences={0};owner_fence_start(&f,&fences);
    /* Isolate the mapping limit from the fixture's smaller process quota. */
    fences.memory->process_page_limit=64;
    DWORD device=f.jobs.jobs.contexts->contexts[0].device;
    uint64_t ids[29]={0},cpu=0,job=0xfeed;
    for (unsigned i=0;i<29;i++) CHECK(intel_gpu_fence_create(&fences,7,device,i,TRUE,&ids[i],&cpu)==0);
    CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(job==0xfeed);
    service_step();CHECK(!f.jobs.done_count && !f.owner.slots[0].pid);
    for (unsigned i=0;i<29;i++) CHECK(!fences.fences[i].references);
    CHECK(intel_gpu_fence_close(&fences,7,device,ids[28])==0);owner_fence_collect(&fences);
    CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
    for (unsigned i=0;i<16 && !f.jobs.done_count;i++) { executor_step();owner_fence_collect(&fences); }
    CHECK(f.jobs.done_count==1 && !f.jobs.done_status[0]);
    for (unsigned i=0;i<28;i++) {
        CHECK(f.owner.slots[0].fences[i].self);
        CHECK(intel_gpu_fence_close(&fences,7,device,ids[i])==0);
    }
    for (unsigned i=0;i<8;i++) { executor_step();owner_fence_collect(&fences); }
    for (unsigned i=0;i<28;i++) CHECK(!f.owner.slots[0].fences[i].self && !fences.fences[i].id);
    owner_fence_finish(&f,&fences);
}
#ifndef INTEL_OWNER_FENCE_SUITE_ENTRY
#define INTEL_OWNER_FENCE_SUITE_ENTRY main
#endif
int INTEL_OWNER_FENCE_SUITE_ENTRY(void)
{
    if (owner_fence_regression_main()) return 1;
    unsigned before=checks;owner_fence_cases();owner_fence_capacity();
    printf("Intel executor fence ownership: %u checks passed, 8 close/failure cases, 28 mapped fences\n",checks-before);
    return 0;
}
