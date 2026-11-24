/* Real KMT, owner, queue and collector; synthetic MMIO/DMA only. */
#define INTEL_OWNER_FENCE_SUITE_ENTRY mapping_regression_main
#include "intel_gpu_owner_fences_host.c"

static void mapping_offer(OWNER_FIXTURE *f,unsigned mode,unsigned options,unsigned lost)
{
    D3DKMT_OFFER_WORK *w=calloc(1,sizeof(*w));CHECK(w);
    for(unsigned i=0;i<2;i++)w->inputs[i]=f->jobs.base.allocations[i];
    CHECK(d3dkmt_offer_prepare(7,f->jobs.base.service.native.device,FALSE,2,mode,
        mode==D3DKMT_OFFER?4:0,options,w)==0);
    if(mode!=D3DKMT_OFFER)CHECK(w->results[0]==lost && w->results[1]==lost);
    CHECK(d3dkmt_offer_commit(w)==0 && d3dkmt_offer_release(w)==0);free(w);
}
static void mapping_copy(OWNER_FIXTURE *f,unsigned completed)
{
    uint64_t job;
    CHECK(intel_gpu_jobs_submit(&f->jobs.jobs,7,f->jobs.base.context,128,64,2,2,&job)==0);
    for(unsigned i=0;i<24 && f->jobs.done_count<completed;i++){executor_step();service_step();}
    CHECK(f->jobs.done_count==completed && !f->jobs.done_status[completed-1]);
}
/* Keep the large synthetic owner separate from the nested baseline suites. */
static __attribute__((noinline)) void mapping_cases(void)
{
    for(unsigned mode=0;mode<8;mode++) {
        OWNER_FIXTURE f;intel_gpu_fences_t fences={0};owner_fence_start(&f,&fences);
        intel_gpu_memory_t *memory=f.jobs.jobs.contexts->memory;
        uint32_t device=f.jobs.jobs.contexts->contexts[0].device;
        uint64_t fence,cpu,job,cookies[2];intel_gpu_buffer_t *buffers[2];
        CHECK(intel_gpu_fence_create(&fences,7,device,0,TRUE,&fence,&cpu)==0);
        for(unsigned i=0;i<2;i++) {
            intel_gpu_kmt_cpu_lease_t view={0};
            CHECK(intel_gpu_kmt_acquire_cpu(7,f.jobs.base.service.native.device,f.jobs.base.allocations[i],&view)==0);
            cookies[i]=view.reference.cookie;buffers[i]=view.access.buffer;
            memset(view.access.cpu,0x4a,(size_t)view.access.bytes);
            CHECK(intel_gpu_kmt_release_cpu(&view)==0);
        }
        mapping_copy(&f,1);intel_gpu_bcs_slot_t *slot=&f.owner.slots[0];
        uint64_t generation=slot->native.queue_id;uint32_t charged=memory->pages;
        CHECK(buffers[0]->residents && buffers[1]->residents && slot->fences[0].self);
        if(mode==1 || mode==2) {
            hw.delay=1000;
            CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
            if(mode==1) {executor_step();CHECK(f.owner.engine.inflight);}
        }
        intel_gpu_capture_t retained={0};
        if(mode==6)CHECK(intel_gpu_capture_begin(7,f.jobs.base.context,128,64,2,2,&retained)==0);
        mapping_offer(&f,D3DKMT_OFFER,mode==7?0:1,0);
        CHECK(memory->pages==charged);
        if(mode==3)mapping_offer(&f,D3DKMT_RECLAIM,0,0); /* cancels deferred pressure */
        if(mode==4){recycle_fail_tlb=true;f.owner.config.io.read=recycle_fault_read;}
        if(mode==5)ram.fail_sync=ram.syncs+1;
        if(mode==1){service_step();CHECK(memory->pages==charged && f.owner.engine.inflight);}
        hw.delay=0;
        for(unsigned i=0;i<24;i++){executor_step();service_step();}
        if(mode==4 || mode==5) {
            CHECK(f.owner.queue.fault==INTEL_BCS_QUARANTINED && !slot->native.ready);
            CHECK(memory->pages==charged && buffers[0]->live && buffers[1]->live);
            CHECK(buffers[0]->residents && buffers[1]->residents);
            recycle_fail_tlb=false;ram.fail_sync=0;
        } else if(mode==3 || mode==7) {
            CHECK(!slot->native.bindings_closed && slot->native.queue_id==generation && memory->pages==charged);
            if(mode==7)mapping_offer(&f,D3DKMT_RECLAIM,0,0);
            mapping_copy(&f,2);
        } else {
            if(mode==1 || mode==2)CHECK(f.jobs.done_count==2 && !f.jobs.done_status[1]);
            CHECK(slot->native.bindings_closed && !slot->fences[0].self && fences.fences[0].references==(mode==6));
            if(mode==6) {
                CHECK(memory->pages==charged && buffers[0]->live);
                CHECK(intel_gpu_capture_release(&retained)==0);service_step();
            }
            uint32_t residency[2]={0};
            CHECK(intel_gpu_memory_properties(memory,7,device,INTEL_GPU_PROPERTY_RESIDENCY,cookies,2,residency)==0);
            CHECK(residency[0]==3 && residency[1]==3 && memory->pages<charged);
            for(unsigned cycle=0;cycle<3;cycle++) {
                mapping_offer(&f,D3DKMT_RECLAIM,0,1);
                for(unsigned i=0;i<2;i++) {
                    intel_gpu_kmt_cpu_lease_t view={0};
                    CHECK(intel_gpu_kmt_acquire_cpu(7,f.jobs.base.service.native.device,f.jobs.base.allocations[i],&view)==0);
                    bool zero=true;for(uint64_t b=0;b<view.access.bytes;b++)if(((BYTE*)view.access.cpu)[b])zero=false;
                    CHECK(zero);CHECK(intel_gpu_kmt_release_cpu(&view)==0);
                }
                mapping_copy(&f,f.jobs.done_count+1);
                CHECK(slot->native.queue_id!=generation && slot->fences[0].self && !slot->native.bindings_closed);
                generation=slot->native.queue_id;
                mapping_offer(&f,D3DKMT_OFFER,1,0);
                for(unsigned i=0;i<8;i++){executor_step();service_step();}
                CHECK(slot->native.bindings_closed && !buffers[0]->live && !buffers[1]->live);
            }
        }
        CHECK(intel_gpu_fence_close(&fences,7,device,fence)==0);
        owner_fence_finish(&f,&fences);
    }
}
int main(void)
{
    if(mapping_regression_main())return 1;
    unsigned before=checks;mapping_cases();
    printf("Intel offered mapping retirement: %u checks passed, 8 pressure/queue/failure cases\n",checks-before);
    return 0;
}
