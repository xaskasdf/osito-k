/* Reuse the owned DMA allocator, GGTT model and lifetime assertions. */
#define main resource_baseline_main
#include "intel_gpu_resource_host.c"
#undef main

static struct {
    uint32_t value, status, error, reads, requests, ops, fail_at;
    uint64_t ns;
    bool stall, frozen, bad_read;
    intel_bcs_queue_t *queue;
    intel_bcs_context_t *context;
    unsigned reenter;
} tlb;
static intel_bcs_context_t rebound;
static intel_gpu_buffer_t buffers[70];
static intel_ggtt_space_t rebind_space;
static uint8_t rebind_used[4];
static intel_bcs_queue_t rebind_queue;
static intel_bcs_state_t rebind_engine;

static bool rebind_read(void *ctx,uint32_t reg,uint32_t *value)
{
    CHECK(ctx==&tlb);
    if (++tlb.ops==tlb.fail_at) return false;
    if (reg==0x22234) *value=tlb.status;
    else if (reg==0x220b8) *value=tlb.error;
    else {
        CHECK(reg==0x426c);
        if (++tlb.reads>=3 && !tlb.stall) tlb.value=0;
        *value=tlb.bad_read ? UINT32_MAX : tlb.value;
    }
    return true;
}

static bool rebind_write(void *ctx,uint32_t reg,uint32_t value)
{
    CHECK(ctx==&tlb && reg==0x426c && value==1);
    tlb.requests++; tlb.value=1; tlb.reads=0;
    if (tlb.reenter) {
        intel_bcs_queue_t *q=tlb.queue;
        intel_bcs_context_t *c=tlb.context;
        uint64_t fence=0;
        const intel_bcs_copy_t copy={0};
        CHECK(q->worker_busy && q->contexts[0].updating);
        CHECK(intel_bcs_queue_enqueue(q,c->owner,c->queue_id,&copy,&fence)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_step(q)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_context_rebind(c,&buffers[0],&buffers[1])==INTEL_BCS_BUSY);
        CHECK(!intel_bcs_context_destroy(c));
        if (tlb.reenter==2) {
            CHECK(intel_bcs_queue_shutdown(q)==INTEL_BCS_BUSY);
            CHECK(q->closing && !q->stopped && q->contexts[0].references==1);
        }
        if (tlb.reenter==3) buffers[0].busy=1; /* old pin release cannot proceed */
    }
    return ++tlb.ops!=tlb.fail_at; /* applied write can still report an error */
}
static uint64_t rebind_now(void *ctx)
{ CHECK(ctx==&tlb); if (!tlb.frozen) tlb.ns+=10000; return tlb.ns; }
static void rebind_relax(void *ctx) { CHECK(ctx==&tlb); }
static const intel_gt_io_t rebind_io={rebind_read,rebind_write,rebind_now,rebind_relax};

static void rebind_fixture(void)
{
    res_reset(); memset(buffers,0,sizeof(buffers));
    memset(&rebound,0,sizeof(rebound)); memset(&rebind_space,0,sizeof(rebind_space));
    memset(&rebind_queue,0,sizeof(rebind_queue)); memset(&tlb,0,sizeof(tlb));
    tlb.status=1;
    rebind_engine=(intel_bcs_state_t){.enabled=true,.started=true,.status=INTEL_BCS_OK};
    const intel_ggtt_io_t io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&rebind_space,&layout,16,32,rebind_used,sizeof(rebind_used),
        &io,ggtt)==INTEL_GGTT_BIND_OK);
    for (unsigned i=0;i<4;i++)
        CHECK(intel_gpu_buffer_create(&buffers[i],&allocator,&ram,7,8192)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_bcs_context_create(&rebound,&rebind_space,&allocator,&ram,7,0x123,
        &buffers[0],0x1235ff000ULL,&buffers[1],0x1403ff000ULL,0x10000000));
    rebind_queue=(intel_bcs_queue_t){.initialized=true,.next_context=1,.next_fence=1,
        .active=INTEL_BCS_QUEUE_JOBS,.io=rebind_io,.engine=&rebind_engine};
    CHECK(intel_bcs_context_attach(&rebound,&rebind_queue,&tlb));
    tlb.queue=&rebind_queue; tlb.context=&rebound;
}

static void rebind_cleanup(void)
{
    ram.fail_sync=ram.fail_release=0;
    CHECK(!intel_bcs_context_destroy(&rebound));
    CHECK(intel_bcs_queue_close_context(&rebind_queue,7,rebound.queue_id)==INTEL_BCS_OK);
    CHECK(!intel_bcs_context_destroy(&rebound));
    /* Engine reset/retirement fault injection is covered by intel_bcs_host. */
    rebind_queue.stopped=true; rebind_engine.retired=true;
    CHECK(intel_bcs_context_destroy(&rebound));
    for (unsigned i=0;i<70;i++) if (buffers[i].started)
        CHECK(intel_gpu_buffer_close(&buffers[i],7)==INTEL_GPU_BUFFER_OK);
    CHECK(ram.allocated==ram.freed);
    res_reset();
}

static void rebind_cases(void)
{
    rebind_fixture();
    uint64_t tables_dma=rebound.tables.dma, state_dma=rebound.state.dma;
    for (unsigned round=0;round<32;round++) {
        unsigned first=round ? 4+2*(round-1) : 2;
        if (round) for (unsigned j=0;j<2;j++)
            CHECK(intel_gpu_buffer_create(&buffers[first+j],&allocator,&ram,7,
                8192+(round%3)*4096)==INTEL_GPU_BUFFER_OK);
        intel_gpu_buffer_t *old_source=rebound.source,*old_destination=rebound.destination;
        CHECK(intel_bcs_context_rebind(&rebound,&buffers[first],&buffers[first+1])==INTEL_BCS_OK);
        CHECK(rebound.ready && rebound.rebindings==round+1 && rebind_engine.tlb_invalidations==round+1);
        CHECK(!old_source->residents && !old_destination->residents);
        CHECK(intel_gpu_buffer_close(old_source,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(old_destination,7)==INTEL_GPU_BUFFER_OK);
        CHECK(rebound.tables.dma==tables_dma && rebound.state.dma==state_dma);
        CHECK(rebound.source==&buffers[first] && rebound.destination==&buffers[first+1]);
        CHECK(!rebound.retained_source && !rebound.retained_destination);
        /* Independently walk all leaves across the 2 MiB boundary and verify
         * new backing, permissions, and the unmapped end of each allocation. */
        for (unsigned which=0;which<2;which++) {
            intel_gpu_buffer_t *b=&buffers[first+which];
            uint64_t base=which ? rebound.destination_gpu : rebound.source_gpu;
            for (unsigned p=0;p<=b->pages;p++) {
                uint64_t va=base+(uint64_t)p*4096,*table=rebound.tables.cpu;
                for (unsigned shift=39;shift>=21;shift-=9) {
                    uint64_t entry=table[(va>>shift)&511];
                    CHECK(entry&1);
                    table=(uint64_t *)rebound.tables.cpu+((entry&~4095ULL)-tables_dma)/8;
                }
                CHECK(table[(va>>12)&511]==(p==b->pages ? 0 : b->dma+(uint64_t)p*4096+(which?0x1b:0x19)));
            }
        }
        const intel_bcs_copy_t copy={rebound.source_gpu,8192,rebound.destination_gpu,8192,256,256,37,23,0,0,0,0};
        uint64_t descriptor; uint32_t tail;
        CHECK(intel_bcs_context_acquire(&rebound,round+1)==INTEL_BCS_OK);
        CHECK(intel_bcs_context_prepare(&rebound,&copy,round+1,&descriptor,&tail));
        uint32_t *saved=(uint32_t *)rebound.state.cpu+1024;
        saved[5]=saved[7]=tail; /* model completed context save */
        CHECK(intel_bcs_context_retire(&rebound,round+1)==INTEL_BCS_OK);
        rebind_engine.submitted=true; rebind_engine.completed=true;
        rebind_engine.submitted_jobs=rebind_engine.completed_jobs=round+1;
    }
    rebind_cleanup();

    for (unsigned failure=1;failure<=6;failure++) {
        rebind_fixture(); tlb.fail_at=failure;
        CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_QUARANTINED);
        CHECK(!rebound.ready && rebind_queue.fault==INTEL_BCS_QUARANTINED);
        for (unsigned i=0;i<4;i++) {
            CHECK(buffers[i].residents==1);
            CHECK(intel_gpu_buffer_close(&buffers[i],7)==INTEL_GPU_BUFFER_BUSY);
        }
        CHECK(!rebind_engine.tlb_invalidations && !ram.freed);
        CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_INVALID);
        rebind_cleanup();
    }
    rebind_fixture(); ram.fail_sync=ram.syncs+1;
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_QUARANTINED);
    CHECK(!tlb.ops && buffers[0].residents && buffers[2].residents);
    rebind_cleanup();

    rebind_fixture();
    uint64_t before[8*512]; memcpy(before,rebound.tables.cpu,sizeof(before));
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[2])==INTEL_BCS_INVALID);
    CHECK(intel_bcs_context_rebind(&rebound,&rebound.state,&buffers[3])==INTEL_BCS_INVALID);
    buffers[2].owner=8;
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_INVALID);
    buffers[2].owner=7; buffers[3].busy=1;
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_BUSY);
    buffers[3].busy=0;
    CHECK(!buffers[2].residents && !buffers[3].residents && !tlb.ops && !rebind_queue.fault);
    CHECK(!memcmp(before,rebound.tables.cpu,sizeof(before)));
    uint64_t capacity=rebound.tables.bytes;
    rebound.tables.bytes=4096; /* cannot accommodate even the two VA windows */
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_INVALID);
    rebound.tables.bytes=capacity;
    CHECK(rebound.ready && !buffers[2].residents && !buffers[3].residents && !tlb.ops);
    CHECK(!memcmp(before,rebound.tables.cpu,sizeof(before)));
    rebind_cleanup();

    rebind_fixture(); tlb.reenter=3;
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_QUARANTINED);
    CHECK(rebind_engine.tlb_invalidations==1 && rebound.rebindings==1);
    CHECK(rebound.source==&buffers[2] && rebound.retained_source==&buffers[0]);
    CHECK(!rebound.ready && buffers[0].residents && buffers[2].residents && !ram.freed);
    buffers[0].busy=0; rebind_cleanup();
}

static void *rebind_thread(void *arg)
{
    enum intel_bcs_status *result=arg;
    *result=intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3]);
    return NULL;
}

static void rebind_gate_cases(void)
{
    for (unsigned exit=0;exit<3;exit++) {
        rebind_fixture(); CHECK(intel_bcs_queue_publish(&rebind_queue)==INTEL_BCS_OK);
        ram.hold=1; pthread_t thread; enum intel_bcs_status result;
        CHECK(!pthread_create(&thread,NULL,rebind_thread,&result));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        uint64_t fence=0,other=0; const intel_bcs_copy_t copy={0};
        CHECK(intel_bcs_queue_enqueue(&rebind_queue,7,rebound.queue_id,&copy,&fence)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_step(&rebind_queue)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_attach(&rebind_queue,8,&ram,&other)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_enqueue(&rebind_queue,8,other,&copy,&fence)==INTEL_BCS_OK);
        CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_BUSY);
        CHECK(!intel_bcs_context_destroy(&rebound) && !ram.freed);
        if (exit==1) intel_bcs_release_process(7);
        if (exit==2) CHECK(intel_bcs_queue_shutdown(&rebind_queue)==INTEL_BCS_BUSY);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && result==INTEL_BCS_OK);
        ram.hold=0;
        CHECK(!rebind_queue.worker_busy && !rebind_queue.contexts[0].updating);
        CHECK(rebind_queue.contexts[0].references==0);
        if (exit) CHECK(intel_bcs_queue_enqueue(&rebind_queue,7,rebound.queue_id,&copy,&fence)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_queue_cancel(&rebind_queue,8,fence)==INTEL_BCS_OK);
        rebind_queue.stopped=true; /* no GPU jobs were submitted by this fixture */
        CHECK(intel_bcs_queue_unpublish(&rebind_queue)==INTEL_BCS_OK);
        rebind_queue.stopped=false;
        rebind_cleanup();
    }
    for (unsigned reenter=1;reenter<=2;reenter++) {
        rebind_fixture(); tlb.reenter=reenter;
        CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_OK);
        CHECK(tlb.requests==1 && !rebind_queue.worker_busy);
        rebind_cleanup();
    }
    rebind_fixture();
    uint64_t fence; const intel_bcs_copy_t copy={0};
    CHECK(intel_bcs_queue_enqueue(&rebind_queue,7,rebound.queue_id,&copy,&fence)==INTEL_BCS_OK);
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_BUSY && !tlb.ops);
    CHECK(intel_bcs_queue_cancel(&rebind_queue,7,fence)==INTEL_BCS_OK);
    rebind_queue.active=0;
    CHECK(intel_bcs_context_rebind(&rebound,&buffers[2],&buffers[3])==INTEL_BCS_BUSY && !tlb.ops);
    rebind_queue.active=INTEL_BCS_QUEUE_JOBS;
    rebind_cleanup();
}

static void rebind_tlb_cases(void)
{
    for (unsigned scenario=0;scenario<8;scenario++) {
        memset(&tlb,0,sizeof(tlb)); tlb.status=1;
        intel_bcs_state_t engine={.enabled=true,.status=INTEL_BCS_OK};
        if (scenario==0) engine.inflight=true;
        if (scenario==1) { engine.submitted=true; engine.completed=false; }
        if (scenario==2) { engine.submitted=true; engine.completed=true; engine.submitted_jobs=1; }
        if (scenario==3) tlb.status=0x4010;
        if (scenario==4) tlb.error=1;
        if (scenario==5) tlb.stall=true;
        if (scenario==6) { tlb.stall=true; tlb.frozen=true; }
        if (scenario==7) tlb.bad_read=true;
        enum intel_bcs_status expected=scenario<4 ? INTEL_BCS_BUSY :
            scenario==4 ? INTEL_BCS_ENGINE_ERROR : INTEL_BCS_TIMEOUT;
        CHECK(intel_bcs_invalidate_tlb(&rebind_io,&tlb,&engine)==expected);
        CHECK(!engine.tlb_invalidations);
        if (scenario<5) CHECK(!tlb.requests);
        else CHECK(tlb.requests==1 && tlb.ops<200010);
    }
}

int main(void)
{
    rebind_cases(); rebind_gate_cases(); rebind_tlb_cases();
    printf("Intel BCS rebind PASS checks=%u\n",checks);
    return 0;
}
