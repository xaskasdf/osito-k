/* Native backing discard/recommit; synthetic DMA allocator and real lifetime
 * gates. The full resource regression runs before the new cases. */
#define main resource_suite_main
#include "intel_gpu_resource_host.c"
#undef main

static void discard_no_access(intel_gpu_buffer_t *b)
{
    intel_gpu_cpu_access_t cpu={0};intel_gpu_access_t gpu={0};
    intel_gpu_coherent_access_t coherent={0};
    CHECK(intel_gpu_buffer_pin(b,7)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_reside(b,7)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_sync(b,7)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_acquire_cpu(b,7,&cpu)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_acquire_gpu(b,7,&gpu)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_acquire_coherent(b,7,&coherent)==INTEL_GPU_BUFFER_INVALID);
    CHECK(!cpu.self && !gpu.self && !coherent.self);
}

static void discard_cycle(void)
{
    const uint64_t sizes[]={1,4095,4096,4097,21105,64*4096};
    for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);n++) {
        res_reset();intel_gpu_buffer_t b={0};
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,sizes[n])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_INVALID);
        for(unsigned round=0;round<32;round++) {
            intel_gpu_cpu_access_t cpu={0};
            CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_OK);
            memset(cpu.cpu,0x65,(size_t)b.pages*4096);
            CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_discard(&b,8)==INTEL_GPU_BUFFER_INVALID);
            intel_gpu_buffer_t moved=b;
            CHECK(intel_gpu_buffer_discard(&moved,7)==INTEL_GPU_BUFFER_INVALID);
            unsigned allocated=ram.allocated,freed=ram.freed;
            CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_OK);
            CHECK(ram.freed==freed+1 && !b.live && b.discarded && !b.cpu && !b.dma);
            CHECK(b.owner==7 && b.bytes==sizes[n] && b.pages==(sizes[n]+4095)/4096);
            CHECK(!b.closing && b.started && b.self==&b);
            CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==freed+1);
            discard_no_access(&b);
            CHECK(intel_gpu_buffer_recommit(&b,8)==INTEL_GPU_BUFFER_INVALID);
            ram.reenter=&b;
            CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_OK);
            ram.reenter=NULL;
            CHECK(b.live && !b.discarded && b.ready && ram.allocated==allocated+1);
            CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_OK);
            int zero=1;for(unsigned i=0;i<b.pages*4096;i++)if(((unsigned char *)cpu.cpu)[i])zero=0;
            CHECK(zero); /* Entire rounded backing, not only the requested size. */
            CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        }
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(ram.allocated==ram.freed);
    }
    res_reset();
}

static void discard_references(void)
{
    for(unsigned kind=0;kind<6;kind++) {
        res_reset();intel_gpu_buffer_t b={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,4097)==INTEL_GPU_BUFFER_OK);
        intel_gpu_cpu_access_t cpu={0};intel_gpu_access_t gpu={0};
        intel_gpu_coherent_access_t coherent={0};intel_gpu_coherent_reference_t ref={0};
        if(kind==0)CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_OK);
        if(kind==1)CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_OK);
        if(kind==2)CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_OK);
        if(kind==3)CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_OK);
        if(kind>=4)CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&coherent)==INTEL_GPU_BUFFER_OK);
        if(kind==5)CHECK(intel_gpu_buffer_retain_coherent(&coherent,&ref)==INTEL_GPU_BUFFER_OK);
        void *address=b.cpu;uint64_t dma=b.dma;
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(!b.closing && !b.discarded && b.cpu==address && b.dma==dma && !ram.releases);
        if(kind==0)CHECK(intel_gpu_buffer_unpin(&b,7)==INTEL_GPU_BUFFER_OK);
        if(kind==1)CHECK(intel_gpu_buffer_unreside(&b,7)==INTEL_GPU_BUFFER_OK);
        if(kind==2)CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        if(kind==3)CHECK(intel_gpu_buffer_release_gpu(&gpu)==INTEL_GPU_BUFFER_OK);
        if(kind==5)CHECK(intel_gpu_buffer_drop_coherent(&ref)==INTEL_GPU_BUFFER_OK);
        if(kind>=4)CHECK(intel_gpu_buffer_release_coherent(&coherent)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==1);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

static void discard_failures(void)
{
    for(unsigned kind=0;kind<8;kind++) {
        res_reset();intel_gpu_buffer_t b={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,4097)==INTEL_GPU_BUFFER_OK);
        ram.fail_release=1;void *address=b.cpu;uint64_t dma=b.dma;
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_IO);
        CHECK(!b.discarded && !b.closing && b.live && b.cpu==address && b.dma==dma && !ram.freed);
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==1);
        unsigned syncs=ram.syncs;
        if(kind==0)ram.fail_allocate=ram.allocated+1;
        if(kind==1)ram.fail_sync=ram.syncs+1;
        if(kind==2)ram.fail_validate=ram.validates+1;
        if(kind>=3&&kind<=6)ram.bad_dma=kind-2;
        enum intel_gpu_buffer_status status=intel_gpu_buffer_recommit(&b,7);
        CHECK(status==(kind==0?INTEL_GPU_BUFFER_NO_MEMORY:kind==1?INTEL_GPU_BUFFER_IO:
                       kind<7?INTEL_GPU_BUFFER_QUARANTINED:INTEL_GPU_BUFFER_OK));
        if(kind>=2&&kind<7) {
            CHECK(b.live && b.discarded && !b.validated && ram.syncs==syncs);
            unsigned allocations=ram.allocated;
            CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_QUARANTINED);
            CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_QUARANTINED);
            CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_QUARANTINED);
            CHECK(ram.allocated==allocations && ram.freed==1);
            for(unsigned i=0;i<8192;i++)CHECK(((unsigned char *)ram.cpu[1])[i]==0xa7);
            /* Only the synthetic allocator knows the rejected DMA is safe. */
            free(ram.cpu[1]);ram.cpu[1]=NULL;
            continue;
        }
        if(kind<2) {
            CHECK(b.discarded);discard_no_access(&b);
            unsigned allocations=ram.allocated;
            CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_OK);
            CHECK(ram.allocated==allocations+(kind==0));
        }
        CHECK(!b.discarded && b.live && b.ready);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(ram.freed==2);
    }
    res_reset();
}

typedef struct {intel_gpu_buffer_t *buffer;unsigned operation;enum intel_gpu_buffer_status status;} DISCARD_RACE;
static void *discard_worker(void *argument)
{
    DISCARD_RACE *r=argument;
    r->status=r->operation?intel_gpu_buffer_recommit(r->buffer,7):intel_gpu_buffer_discard(r->buffer,7);
    return NULL;
}
static void discard_races(void)
{
    for(unsigned operation=0;operation<2;operation++)for(unsigned round=0;round<32;round++) {
        res_reset();intel_gpu_buffer_t b={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_OK);
        if(operation)CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_OK);
        ram.hold=1;DISCARD_RACE race={&b,operation,INTEL_GPU_BUFFER_INVALID};pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,discard_worker,&race));
        while(!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE))sched_yield();
        CHECK(intel_gpu_buffer_discard(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_recommit(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_BUSY);
        intel_gpu_cpu_access_t cpu={0};intel_gpu_access_t gpu={0};
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_BUSY);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && race.status==INTEL_GPU_BUFFER_OK);ram.hold=0;
        CHECK(!b.closing);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(ram.allocated==ram.freed);
    }
    res_reset();
}

int main(void)
{
    resource_suite_main();unsigned before=checks;
    discard_cycle();discard_references();discard_failures();discard_races();
    printf("Intel GPU backing discard: %u checks passed, 64 forced callback races\n",checks-before);
    return 0;
}
