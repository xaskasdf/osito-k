/* Real native contexts/queues/buffer ownership; MMIO and DMA are host models. */
#define INTEL_RECYCLE_SUITE_ENTRY coherent_context_regression_main
#include "intel_gpu_recycle_host.c"

static const intel_ppgtt_pat_t coherent_policy={.started=true,.ready=true};
static intel_bcs_context_t *coherent_context_start(OWNER_FIXTURE *f)
{
    owner_setup(f);
    CHECK(intel_gpu_capture_release(&f->bootstrap)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&f->owner,1,8)==0);
    CHECK(intel_gpu_bcs_owner_start(&f->owner)==0);
    uint64_t job=0;
    CHECK(intel_gpu_jobs_submit(&f->jobs.jobs,7,f->jobs.base.context,128,64,2,2,&job)==0);
    for (unsigned i=0;i<8 && !f->jobs.done_count;i++) { executor_step(); service_step(); }
    CHECK(f->jobs.done_count==1 && !f->jobs.done_status[0]);
    return &f->owner.slots[0].native;
}
static uint64_t coherent_context_pte(intel_bcs_context_t *c,uint64_t address)
{
    uint64_t *table=c->tables.cpu;
    for (unsigned shift=39;shift>12;shift-=9) {
        uint64_t entry=table[(address>>shift)&511];
        if (!(entry&1)) return 0;
        uint64_t dma=entry&~4095ULL;
        CHECK(dma>=c->tables.dma && dma-c->tables.dma<c->tables.bytes);
        table=(uint64_t *)((unsigned char *)c->tables.cpu+dma-c->tables.dma);
    }
    return table[(address>>12)&511];
}
/* Baseline suites run first: do not reserve their callers' large device
 * fixtures on the same stack through automatic inlining. */
static __attribute__((noinline)) void coherent_context_cases(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        OWNER_FIXTURE f; intel_bcs_context_t *c=coherent_context_start(&f);
        intel_gpu_buffer_t buffers[3]={0},replacements[2]={0};
        intel_gpu_coherent_access_t access[3]={0};
        const uint64_t gpu[3]={0x400000000ULL,0x4000000ULL,0x400001000ULL};
        for (unsigned i=0;i<3;i++) {
            CHECK(intel_gpu_buffer_create(&buffers[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_acquire_coherent(&buffers[i],7,&access[i])==INTEL_GPU_BUFFER_OK);
        }
        unsigned char before[8*4096];memcpy(before,c->tables.cpu,sizeof(before));
        CHECK(intel_bcs_context_bind_coherent(c,&access[0],gpu[0],NULL)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_context_bind_coherent(c,&access[0],gpu[0]+1,&coherent_policy)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_context_bind_coherent(c,&access[0],c->source_gpu,&coherent_policy)==INTEL_BCS_INVALID);
        intel_gpu_coherent_access_t copied=access[0];
        CHECK(intel_bcs_context_bind_coherent(c,&copied,gpu[0],&coherent_policy)==INTEL_BCS_INVALID);
        CHECK(!access[0].references && !buffers[0].residents && !memcmp(before,c->tables.cpu,sizeof(before)));
        for (unsigned i=0;i<3;i++) {
            if (i==2) {
                /* A third disjoint PT would exceed this fixed eight-page arena. */
                memcpy(before,c->tables.cpu,sizeof(before));
                CHECK(intel_bcs_context_bind_coherent(c,&access[i],0x18000000ULL,&coherent_policy)==INTEL_BCS_INVALID);
                CHECK(!access[i].references && !buffers[i].residents && !memcmp(before,c->tables.cpu,sizeof(before)));
            }
            CHECK(intel_bcs_context_bind_coherent(c,&access[i],gpu[i],&coherent_policy)==INTEL_BCS_OK);
            CHECK(access[i].references==1 && buffers[i].residents==1);
            CHECK(coherent_context_pte(c,gpu[i])==(access[i].dma|0x83));
            CHECK(!coherent_context_pte(c,gpu[i]+4096));
        }
        CHECK((coherent_context_pte(c,c->source_gpu)&255)==0x19);
        CHECK((coherent_context_pte(c,c->destination_gpu)&255)==0x1b);
        CHECK(intel_bcs_context_bind_coherent(c,&access[0],gpu[0],&coherent_policy)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_context_bind_coherent(c,&access[0],gpu[0]+4096,&coherent_policy)==INTEL_BCS_INVALID);
        CHECK(access[0].references==1 && buffers[0].residents==1);
        uint64_t job=0;hw.delay=1000;
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
        executor_step();
        CHECK(intel_bcs_context_unbind_coherent(c,gpu[0])==INTEL_BCS_BUSY);
        hw.delay=0;
        for (unsigned i=0;i<8 && f.jobs.done_count<2;i++) { executor_step(); service_step(); }
        CHECK(f.jobs.done_count==2 && !f.jobs.done_status[1]);
        if (mode==1) CHECK(intel_gpu_buffer_release_coherent(&access[0])==INTEL_GPU_BUFFER_BUSY);
        for (unsigned i=0;i<2;i++) CHECK(intel_gpu_buffer_create(&replacements[i],&allocator,&ram,
            mode==2 ? 8 : 7,4096)==INTEL_GPU_BUFFER_OK);
        if (mode==2) {
            CHECK(intel_bcs_queue_close_context(&f.owner.queue,7,c->queue_id)==INTEL_BCS_OK);
            CHECK(intel_bcs_context_retarget(c,8,123,&replacements[0],&replacements[1])==INTEL_BCS_OK);
            for (unsigned i=0;i<3;i++) CHECK(!coherent_context_pte(c,gpu[i]) &&
                !access[i].references && !buffers[i].residents);
            CHECK(intel_bcs_context_bind_coherent(c,&access[0],gpu[0],&coherent_policy)==INTEL_BCS_INVALID);
        } else {
            CHECK(intel_bcs_context_rebind(c,&replacements[0],&replacements[1])==INTEL_BCS_OK);
            for (unsigned i=0;i<3;i++) CHECK(coherent_context_pte(c,gpu[i])==(access[i].dma|0x83));
            if (mode!=3) for (unsigned i=0;i<3;i++) {
                CHECK(intel_bcs_context_unbind_coherent(c,gpu[i])==INTEL_BCS_OK);
                CHECK(!coherent_context_pte(c,gpu[i]) && !access[i].references && !buffers[i].residents);
            }
        }
        NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&f.owner); executor_step(); service_step(); }
        CHECK(!closed);
        for (unsigned i=0;i<3;i++) {
            CHECK(!access[i].references && !buffers[i].residents);
            CHECK(intel_gpu_buffer_release_coherent(&access[i])==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_close(&buffers[i],7)==INTEL_GPU_BUFFER_OK);
        }
        for (unsigned i=0;i<2;i++) CHECK(intel_gpu_buffer_close(&replacements[i],mode==2?8:7)==INTEL_GPU_BUFFER_OK);
        owner_cleanup(&f);
    }
    OWNER_FIXTURE f;intel_bcs_context_t *c=coherent_context_start(&f);
    intel_gpu_buffer_t buffers[INTEL_BCS_COHERENT_RANGES+1]={0};
    intel_gpu_coherent_access_t access[INTEL_BCS_COHERENT_RANGES+1]={0};
    for (unsigned i=0;i<=INTEL_BCS_COHERENT_RANGES;i++) {
        CHECK(intel_gpu_buffer_create(&buffers[i],&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&buffers[i],7,&access[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_bind_coherent(c,&access[i],0x400000000ULL+4096*i,&coherent_policy)==
            (i<INTEL_BCS_COHERENT_RANGES ? INTEL_BCS_OK : INTEL_BCS_INVALID));
        CHECK(access[i].references==(i<INTEL_BCS_COHERENT_RANGES));
    }
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        CHECK(coherent_context_pte(c,0x400000000ULL+4096*i)==(access[i].dma|0x83));
        CHECK(intel_bcs_context_unbind_coherent(c,0x400000000ULL+4096*i)==INTEL_BCS_OK);
    }
    for (unsigned i=0;i<=INTEL_BCS_COHERENT_RANGES;i++) {
        CHECK(intel_gpu_buffer_release_coherent(&access[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&buffers[i],7)==INTEL_GPU_BUFFER_OK);
    }
    owner_cleanup(&f);
}
static __attribute__((noinline)) void coherent_context_faults(void)
{
    for (unsigned mode=0;mode<5;mode++) {
        OWNER_FIXTURE f;intel_bcs_context_t *c=coherent_context_start(&f);
        intel_gpu_buffer_t buffer={0};intel_gpu_coherent_access_t access={0};
        CHECK(intel_gpu_buffer_create(&buffer,&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&buffer,7,&access)==INTEL_GPU_BUFFER_OK);
        if (mode>=2) CHECK(intel_bcs_context_bind_coherent(c,&access,0x400000000ULL,&coherent_policy)==INTEL_BCS_OK);
        if (mode==0 || mode==2) ram.fail_sync=ram.syncs+1;
        if (mode==1 || mode==3) { recycle_fail_tlb=true;f.owner.config.io.read=recycle_fault_read; }
        if (mode==4) buffer.busy=1;
        CHECK((mode<2 ? intel_bcs_context_bind_coherent(c,&access,0x400000000ULL,&coherent_policy) :
            intel_bcs_context_unbind_coherent(c,0x400000000ULL))==INTEL_BCS_QUARANTINED);
        CHECK(!c->ready && access.references==1 && buffer.residents==1);
        CHECK(f.owner.queue.fault==INTEL_BCS_QUARANTINED);
        ram.fail_sync=0;recycle_fail_tlb=false;buffer.busy=0;
        NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&f.owner);executor_step();service_step(); }
        CHECK(!closed && !access.references && !buffer.residents);
        CHECK(intel_gpu_buffer_release_coherent(&access)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&buffer,7)==INTEL_GPU_BUFFER_OK);
        owner_cleanup(&f);
    }
}
static struct {
    intel_bcs_context_t *context;
    intel_gpu_coherent_access_t *access;
    enum intel_bcs_status result;
} coherent_race;
static void *coherent_context_race_thread(void *unused)
{
    (void)unused;
    coherent_race.result=intel_bcs_context_bind_coherent(coherent_race.context,
        coherent_race.access,0x400000000ULL,&coherent_policy);
    return NULL;
}
static __attribute__((noinline)) void coherent_context_races(void)
{
    for (unsigned round=0;round<16;round++) {
        OWNER_FIXTURE f;intel_bcs_context_t *c=coherent_context_start(&f);
        intel_gpu_buffer_t buffer={0};intel_gpu_coherent_access_t access={0};
        CHECK(intel_gpu_buffer_create(&buffer,&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&buffer,7,&access)==INTEL_GPU_BUFFER_OK);
        coherent_race.context=c;coherent_race.access=&access;
        ram.hold=1;pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,coherent_context_race_thread,NULL));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(access.references==1 && buffer.residents==1);
        CHECK(intel_bcs_context_unbind_coherent(c,0x400000000ULL)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_close_context(&f.owner.queue,7,c->queue_id)==INTEL_BCS_OK);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));ram.hold=0;
        CHECK(coherent_race.result==INTEL_BCS_OK);
        CHECK(intel_bcs_context_bind_coherent(c,&access,0x400001000ULL,&coherent_policy)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_context_unbind_coherent(c,0x400000000ULL)==INTEL_BCS_OK);
        CHECK(!access.references && !buffer.residents && !coherent_context_pte(c,0x400000000ULL));
        NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&f.owner);executor_step();service_step(); }
        CHECK(!closed && !access.references && !buffer.residents);
        CHECK(intel_gpu_buffer_release_coherent(&access)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&buffer,7)==INTEL_GPU_BUFFER_OK);
        owner_cleanup(&f);
    }
}
#ifndef INTEL_COHERENT_SUITE_ENTRY
#define INTEL_COHERENT_SUITE_ENTRY main
#endif
int INTEL_COHERENT_SUITE_ENTRY(void)
{
    if (coherent_context_regression_main()) return 1;
    unsigned before=checks;
    coherent_context_cases();coherent_context_faults();coherent_context_races();
    printf("Intel context coherent mappings: %u checks passed, 28 mappings, 16 close races\n",checks-before);
    return 0;
}
