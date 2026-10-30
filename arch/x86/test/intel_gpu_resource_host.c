/* Owned RAM/context failure and lifetime contracts, with synthetic DMA only. */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_bcs_context.h"
#include "../drivers/intel_gpu_memory.h"
extern void d3dkmt_release_process(uint32_t);

static unsigned checks;
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if (!(x)) { \
    fprintf(stderr,"FAIL GPU resource %d: %s\n",__LINE__,#x); abort(); \
} } while (0)
static struct {
    void *cpu[128];
    uint32_t pages[128];
    unsigned allocated, freed, validates, syncs, releases;
    unsigned fail_allocate, fail_validate, fail_sync, fail_release, bad_dma;
    unsigned bad_physical;
    intel_gpu_buffer_t *reenter;
    unsigned hold, entered, resume;
} ram;
static uint64_t ggtt[64];
static bool ggtt_fail;
static const uint64_t resource_dma=0x20000000;

static bool res_allocate(void *ctx, uint32_t pages, uint64_t *dma, void **cpu)
{
    CHECK(ctx==&ram && pages && pages<=64);
    unsigned i=ram.allocated++;
    CHECK(i<128);
    if (ram.allocated==ram.fail_allocate) return false;
    ram.cpu[i]=aligned_alloc(4096,(size_t)pages*4096);
    CHECK(ram.cpu[i]); ram.pages[i]=pages;
    memset(ram.cpu[i],0xa7,(size_t)pages*4096);
    *cpu=ram.cpu[i]; *dma=resource_dma+(uint64_t)i*0x100000;
    if (ram.bad_dma==1) *dma=0;
    if (ram.bad_dma==2) *dma+=1;
    if (ram.bad_dma==3) *dma=INTEL_GGTT_PHYS_LIMIT;
    if (ram.bad_dma==4) *dma=UINT64_MAX;
    if (ram.reenter) {
        CHECK(intel_gpu_buffer_close(ram.reenter,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_pin(ram.reenter,7)==INTEL_GPU_BUFFER_BUSY);
    }
    return true;
}

static bool res_validate(void *ctx, uint64_t dma, void *cpu, uint32_t pages)
{
    CHECK(ctx==&ram && dma>=resource_dma);
    unsigned i=(unsigned)((dma-resource_dma)/0x100000);
    CHECK(i<128 && ram.cpu[i]==cpu && ram.pages[i]==pages);
    return ++ram.validates!=ram.fail_validate;
}

static bool res_release(void *ctx, uint64_t dma, void *cpu, uint32_t pages)
{
    CHECK(ctx==&ram && dma>=resource_dma);
    unsigned i=(unsigned)((dma-resource_dma)/0x100000);
    CHECK(i<128 && ram.cpu[i]==cpu && ram.pages[i]==pages);
    for (unsigned p=0;p<64;p++) CHECK(!(ggtt[p]&1) ||
        (ggtt[p]&~4095ULL)<dma || (ggtt[p]&~4095ULL)>=dma+(uint64_t)pages*4096);
    if (++ram.releases==ram.fail_release) return false;
    if (__atomic_load_n(&ram.hold,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&ram.entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&ram.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    free(cpu); ram.cpu[i]=NULL; ram.freed++;
    return true;
}

static bool res_sync(void *ctx, uint64_t dma, void *cpu, uint32_t pages)
{
    CHECK(ctx==&ram && dma>=resource_dma);
    unsigned i=(unsigned)((dma-resource_dma)/0x100000);
    CHECK(i<128 && ram.cpu[i]==cpu && ram.pages[i]==pages);
    if (ram.reenter) CHECK(intel_gpu_buffer_close(ram.reenter,7)==INTEL_GPU_BUFFER_BUSY);
    if (__atomic_load_n(&ram.hold,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&ram.entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&ram.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return ++ram.syncs!=ram.fail_sync;
}
static bool res_cpu_physical(void *ctx,uint64_t dma,void *cpu,uint32_t pages,uint64_t *physical)
{
    CHECK(ctx==&ram && physical && dma>=resource_dma);
    unsigned i=(unsigned)((dma-resource_dma)/0x100000);
    CHECK(i<128 && ram.cpu[i]==cpu && ram.pages[i]==pages);
    /* Deliberately nonidentity DMA; CPU physical mapping uses a separate
     * synthetic domain. The VM host model never installs these PTEs. */
    *physical=0x80000000ULL+(uint64_t)i*0x100000;
    if (ram.bad_physical==1) *physical=0;
    if (ram.bad_physical==2) *physical+=1;
    if (ram.bad_physical==3) *physical=0x000ffffffffff000ULL;
    if (ram.bad_physical==4) return false;
    return true;
}
static const intel_gpu_allocator_t allocator={res_allocate,res_validate,res_release,
    res_sync,res_cpu_physical};

static void res_reset(void)
{
    for (unsigned i=0;i<128;i++) CHECK(!ram.cpu[i]);
    memset(&ram,0,sizeof(ram)); memset(ggtt,0,sizeof(ggtt)); ggtt_fail=false;
}

static void res_buffer_cases(void)
{
    const uint64_t sizes[]={1,4095,4096,4097,21105,31565,64*4096};
    for (unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);n++) {
        res_reset(); intel_gpu_buffer_t b={0}; ram.reenter=&b;
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,sizes[n])==INTEL_GPU_BUFFER_OK);
        CHECK(b.pages==(sizes[n]+4095)/4096 && b.bytes==sizes[n]);
        for (unsigned i=0;i<b.pages*4096;i++) CHECK(!((unsigned char *)b.cpu)[i]);
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,1)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_pin(&b,8)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_close(&b,8)==INTEL_GPU_BUFFER_INVALID && !b.closing);
        intel_gpu_buffer_t moved=b;
        CHECK(intel_gpu_buffer_pin(&moved,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && b.live);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_sync(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_unpin(&b,8)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_unpin(&b,7)==INTEL_GPU_BUFFER_OK && !ram.freed);
        ram.reenter=NULL; ram.fail_release=1;
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_IO && b.live);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==1);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==1);
        CHECK(intel_gpu_buffer_sync(&b,7)==INTEL_GPU_BUFFER_INVALID);
    }
    res_reset(); intel_gpu_buffer_t b={0};
    CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,0,1)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,0)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,INTEL_GPU_BUFFER_MAX_BYTES+1)==INTEL_GPU_BUFFER_INVALID);
    ram.fail_allocate=1;
    CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,1)==INTEL_GPU_BUFFER_NO_MEMORY);
    CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && !ram.releases);
    for (unsigned invalid=0;invalid<5;invalid++) {
        res_reset(); b=(intel_gpu_buffer_t){0};
        ram.bad_dma=invalid; ram.fail_validate=1;
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,4097)==INTEL_GPU_BUFFER_QUARANTINED);
        CHECK(!ram.syncs && !b.validated && b.live);
        for (unsigned i=0;i<8192;i++) CHECK(((unsigned char *)ram.cpu[0])[i]==0xa7);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_QUARANTINED && !ram.releases);
        /* Only the test allocator knows this synthetic allocation is safe. */
        free(ram.cpu[0]); ram.cpu[0]=NULL;
    }
    res_reset(); b=(intel_gpu_buffer_t){0}; ram.fail_sync=1;
    CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,4096)==INTEL_GPU_BUFFER_IO);
    CHECK(!b.ready && b.validated && b.live);
    CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_buffer_sync(&b,7)==INTEL_GPU_BUFFER_OK && b.ready);
    b.pins=UINT32_MAX;
    CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_BUSY);
    b.pins=0;
    CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
}

static void *res_buffer_thread(void *argument)
{
    CHECK(intel_gpu_buffer_sync(argument,7)==INTEL_GPU_BUFFER_OK);
    return NULL;
}

static void res_buffer_races(void)
{
    for (unsigned round=0;round<32;round++) {
        res_reset(); intel_gpu_buffer_t b={0}; pthread_t thread;
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,4097)==INTEL_GPU_BUFFER_OK);
        __atomic_store_n(&ram.hold,1,__ATOMIC_RELEASE);
        CHECK(!pthread_create(&thread,NULL,res_buffer_thread,&b));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_sync(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==1);
    }
}

static bool res_ggtt_read(void *ctx, uint32_t page, uint64_t *pte)
{ CHECK(ctx==ggtt && page<64); *pte=ggtt[page]; return true; }
static bool res_ggtt_write(void *ctx, uint32_t page, uint64_t pte)
{ CHECK(ctx==ggtt && page<64); ggtt[page]=pte; return true; }
static bool res_ggtt_sync(void *ctx)
{ CHECK(ctx==ggtt); return !ggtt_fail; }

static void res_context_cases(void)
{
    const intel_ggtt_io_t io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    const intel_bcs_copy_t copy={0x1235ff000ULL,21105,0x1403ff000ULL,31565,512,768,113,41,0,0,0,0};
    for (unsigned scenario=0;scenario<7;scenario++) {
        res_reset(); intel_gpu_buffer_t src={0},dst={0};
        intel_bcs_context_t c={0}; intel_ggtt_space_t space={0}; uint8_t used[4];
        CHECK(intel_ggtt_space_init(&space,&layout,16,32,used,sizeof(used),&io,ggtt)==INTEL_GGTT_BIND_OK);
        CHECK(intel_gpu_buffer_create(&src,&allocator,&ram,7,copy.source_bytes)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_create(&dst,&allocator,&ram,7,copy.destination_bytes)==INTEL_GPU_BUFFER_OK);
        if (scenario==1) ram.fail_allocate=3; /* table allocation */
        if (scenario==2) ram.fail_allocate=4; /* state/ring allocation */
        if (scenario==3) ram.fail_sync=4; /* table publication */
        if (scenario==4) ggtt_fail=true; /* bind and rollback uncertain */
        bool made=intel_bcs_context_create(&c,&space,&allocator,&ram,7,0x123,
            &src,copy.source,&dst,copy.destination,0x10000000);
        CHECK(made==(scenario==0 || scenario>=5));
        CHECK(src.residents==1 && dst.residents==1 && !src.pins && !dst.pins);
        if (made) {
            CHECK(c.table_pages==8 && c.state.pages==3 && c.binding.pins==1);
            uint64_t desc=0xaaaa; uint32_t tail=0xbbbb;
            intel_bcs_copy_t invalid=copy; invalid.source_bytes=src.bytes+1;
            CHECK(!intel_bcs_context_prepare(&c,&invalid,1,&desc,&tail));
            CHECK(desc==0xaaaa && tail==0xbbbb && !c.prepared);
            invalid=copy; invalid.destination+=4096;
            CHECK(!intel_bcs_context_prepare(&c,&invalid,1,&desc,&tail));
            intel_gpu_cpu_access_t cpu={0};
            CHECK(intel_gpu_buffer_acquire_cpu(&dst,7,&cpu)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_bcs_context_acquire(&c,1)==INTEL_BCS_BUSY && src.gpu_access && !dst.gpu_access);
            CHECK(intel_bcs_context_retire(&c,2)==INTEL_BCS_INVALID && src.gpu_access);
            CHECK(intel_bcs_context_retire(&c,1)==INTEL_BCS_OK && !src.gpu_access);
            CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
            for (unsigned round=0;round<70;round++) {
                CHECK(intel_bcs_context_acquire(&c,1+round)==INTEL_BCS_OK);
                CHECK(intel_gpu_buffer_acquire_cpu(&src,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
                CHECK(!intel_bcs_context_destroy(&c));
                CHECK(intel_bcs_context_prepare(&c,&copy,1+round,&desc,&tail));
                CHECK(desc==c.descriptor && tail==((round+1)*64)%4096);
                uint32_t *saved=(uint32_t *)c.state.cpu+1024;
                /* Mock hardware context save after proven completion. */
                saved[5]=tail; saved[7]=tail;
                CHECK(intel_bcs_context_retire(&c,1+round)==INTEL_BCS_OK);
                CHECK(intel_gpu_buffer_acquire_cpu(&src,7,&cpu)==INTEL_GPU_BUFFER_OK);
                CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
            }
        }
        CHECK(intel_gpu_buffer_close(&src,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_close(&dst,7)==INTEL_GPU_BUFFER_BUSY);
        intel_bcs_state_t engine={0};
        intel_bcs_queue_t q={.initialized=true,.next_context=1,.engine=&engine};
        if (scenario==5) {
            CHECK(intel_bcs_context_attach(&c,&q,NULL));
            CHECK(!intel_bcs_context_destroy(&c) && c.ready && !ram.freed);
            CHECK(intel_bcs_queue_close_context(&q,7,c.queue_id)==INTEL_BCS_OK);
            q.stopped=true;
            CHECK(!intel_bcs_context_destroy(&c) && !ram.freed);
            engine.retired=true; /* queue lifecycle tests cover the actual close */
        }
        if (scenario==4) {
            CHECK(c.binding.phase==INTEL_GGTT_QUARANTINED);
            CHECK(!intel_bcs_context_destroy(&c) && !ram.freed);
            CHECK(c.tables.live && c.state.live && src.residents && dst.residents);
            ggtt_fail=false;
        }
        if (scenario==6) {
            ram.fail_release=1;
            CHECK(!intel_bcs_context_destroy(&c));
            CHECK(c.state.live && c.tables.live && src.residents && dst.residents && !ram.freed);
        }
        CHECK(intel_bcs_context_destroy(&c));
        CHECK(intel_bcs_context_destroy(&c));
        CHECK(!src.residents && !dst.residents && !src.gpu_access && !dst.gpu_access && !c.state.live && !c.tables.live);
        CHECK(intel_gpu_buffer_close(&src,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&dst,7)==INTEL_GPU_BUFFER_OK);
        for (unsigned i=0;i<64;i++) CHECK(!ggtt[i]);
    }
    res_reset();
}

static void res_range_cases(void)
{
    static uint64_t a[32*512],b[32*512],saved[32*512];
    static intel_ppgtt_page_t pages[4096];
    const uint64_t starts[]={0x1ff000,0x3ffff000,0x7ffffff000ULL,INTEL_PPGTT_VA_LIMIT-8192};
    for (unsigned t=0;t<4;t++) {
        for (unsigned count=1;count<=2048;count*=2) {
            if (starts[t]+(uint64_t)count*4096>INTEL_PPGTT_VA_LIMIT) break;
            intel_ppgtt_range_t r={starts[t],resource_dma,(uint64_t)count*4096,(t&1)!=0};
            uint32_t planned=0,used=0,reference=0;
            CHECK(intel_ppgtt_range_pages(&r,1,&planned));
            CHECK(intel_ppgtt_build_ranges(a,sizeof(a),0x1000000,&r,1,&used));
            CHECK(planned==used);
            for (unsigned i=0;i<count;i++)
                pages[i]=(intel_ppgtt_page_t){r.gpu+(uint64_t)i*4096,r.dma+(uint64_t)i*4096,r.writable};
            CHECK(intel_ppgtt_build(b,sizeof(b),0x1000000,pages,count,&reference));
            CHECK(used==reference && !memcmp(a,b,sizeof(a)));
            memcpy(saved,a,sizeof(a));
            CHECK(!intel_ppgtt_build_ranges(a,(used-1)*4096,0x1000000,&r,1,&used));
            CHECK(used==reference && !memcmp(a,saved,sizeof(a)));
        }
    }
    intel_ppgtt_range_t ranges[3]={{0x1ff000,resource_dma,8192,false},
        {0x201000,resource_dma+8192,8192,true},{0x7ffffff000ULL,resource_dma+16384,8192,false}};
    uint32_t used=0,planned=0;
    CHECK(intel_ppgtt_range_pages(ranges,3,&planned) && planned==10);
    CHECK(intel_ppgtt_build_ranges(a,sizeof(a),0x1000000,ranges,3,&used) && used==planned);
    unsigned n=0;
    for (unsigned r=0;r<3;r++) for(unsigned p=0;p<2;p++)
        pages[n++]=(intel_ppgtt_page_t){ranges[r].gpu+p*4096,ranges[r].dma+p*4096,ranges[r].writable};
    CHECK(intel_ppgtt_build(b,sizeof(b),0x1000000,pages,n,&planned));
    CHECK(planned==used && !memcmp(a,b,sizeof(a)));
    memcpy(saved,a,sizeof(a));
    for (unsigned bad=0;bad<9;bad++) {
        intel_ppgtt_range_t r[3]; memcpy(r,ranges,sizeof(r));
        if (bad==0) r[0].gpu=0;
        if (bad==1) r[0].bytes=UINT64_MAX;
        if (bad==2) r[0].dma=UINT64_MAX;
        if (bad==3) r[1].gpu=r[0].gpu+4096;
        if (bad==4) r[2].gpu=INTEL_PPGTT_VA_LIMIT-4096;
        if (bad==5) r[0].dma=INTEL_GGTT_PHYS_LIMIT-4096;
        if (bad==6) r[0].bytes=0;
        if (bad==7) r[0].bytes=4097;
        if (bad==8) r[0].dma=0x1000000;
        CHECK(!intel_ppgtt_build_ranges(a,sizeof(a),0x1000000,r,3,&used));
        CHECK(used==planned && !memcmp(a,saved,sizeof(a)));
    }
    CHECK(!intel_ppgtt_build_ranges(a,sizeof(a),0x1000000,(void *)a,3,&used));
    CHECK(!intel_ppgtt_build_ranges(a,sizeof(a),0x1000000,ranges,3,(void *)a));
    CHECK(!intel_ppgtt_build_ranges(a,sizeof(a),0x1000000,ranges,3,(void *)ranges));
    CHECK(!intel_ppgtt_range_pages(ranges,INTEL_PPGTT_MAX_RANGES+1,&used));
    CHECK(intel_ppgtt_build_ranges(a,4096,0x1000000,NULL,0,&used) && used==1);
}

static void res_manager_cases(void)
{
    res_reset(); intel_gpu_memory_t m={0}; uint32_t a=0,b=0,c=0,r=0,s=0,t=0;
    CHECK(intel_gpu_memory_init(&m,&allocator,&ram,6,4));
    CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_BUSY);
    CHECK(intel_gpu_memory_open_device(&m,7,&a)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_open_device(&m,7,&b)==INTEL_GPU_BUFFER_OK && a!=b);
    CHECK(intel_gpu_memory_open_device(&m,8,&c)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create(&m,7,a,4097,&r)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create(&m,7,b,8192,&s)==INTEL_GPU_BUFFER_OK);
    t=0xeeee;
    CHECK(intel_gpu_memory_create(&m,7,a,1,&t)==INTEL_GPU_BUFFER_NO_MEMORY && t==0xeeee);
    CHECK(intel_gpu_memory_create(&m,8,c,8192,&t)==INTEL_GPU_BUFFER_OK && m.pages==6);
    uint32_t ignored=0;
    CHECK(intel_gpu_memory_create(&m,8,c,1,&ignored)==INTEL_GPU_BUFFER_NO_MEMORY);
    intel_gpu_memory_lease_t lease={0},other={0};
    CHECK(intel_gpu_memory_acquire(&m,8,a,r,&lease)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_acquire(&m,7,b,r,&lease)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_acquire(&m,7,a,r,&lease)==INTEL_GPU_BUFFER_OK);
    intel_gpu_memory_lease_t copied=lease;
    CHECK(intel_gpu_memory_release(&copied)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_close(&m,8,a,r)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_close(&m,7,a,r)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_acquire(&m,7,a,r,&other)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && m.pages==6 && !ram.freed);
    CHECK(intel_gpu_memory_release(&lease)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_release(&lease)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && m.pages==4 && ram.freed==1);
    uint32_t replacement;
    CHECK(intel_gpu_memory_create(&m,7,a,4097,&replacement)==INTEL_GPU_BUFFER_OK && replacement!=r);
    CHECK(intel_gpu_memory_close(&m,7,a,r)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_acquire(&m,7,a,r,&lease)==INTEL_GPU_BUFFER_INVALID);
    unsigned frees=ram.freed,syncs=ram.syncs;
    d3dkmt_release_process(7);
    CHECK(ram.freed==frees && ram.syncs==syncs && m.pages==6);
    CHECK(intel_gpu_memory_open_device(&m,7,&ignored)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_create(&m,7,a,1,&ignored)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_acquire(&m,7,b,s,&lease)==INTEL_GPU_BUFFER_INVALID);
    CHECK(intel_gpu_memory_acquire(&m,8,c,t,&lease)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && m.pages==2);
    intel_gpu_memory_shutdown(&m);
    CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_BUSY);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY);
    CHECK(intel_gpu_memory_release(&lease)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages);
    CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++) CHECK(!m.devices[i].id);
    /* Independent fixed storage and nonwrapping handles, even after reuse. */
    res_reset(); memset(&m,0,sizeof(m)); CHECK(intel_gpu_memory_init(&m,&allocator,&ram,128,128));
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++)
        CHECK(intel_gpu_memory_open_device(&m,i+1,&a)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_open_device(&m,99,&b)==INTEL_GPU_BUFFER_NO_MEMORY);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        CHECK(intel_gpu_memory_create(&m,16,a,1,&r)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_create(&m,16,a,1,&s)==INTEL_GPU_BUFFER_NO_MEMORY);
    CHECK(intel_gpu_memory_close(&m,16,a,r)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);
    m.next_id=UINT32_MAX;
    CHECK(intel_gpu_memory_create(&m,16,a,1,&s)==INTEL_GPU_BUFFER_NO_MEMORY);
    intel_gpu_memory_shutdown(&m);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);
    CHECK(!m.pages && ram.freed==64);
    /* Failed creation/cleanup retains quota until actual reclamation. */
    for (unsigned fault=0;fault<4;fault++) {
        res_reset(); memset(&m,0,sizeof(m)); CHECK(intel_gpu_memory_init(&m,&allocator,&ram,2,2));
        CHECK(intel_gpu_memory_open_device(&m,7,&a)==INTEL_GPU_BUFFER_OK);
        if (!fault) ram.fail_allocate=1;
        if (fault==1) ram.fail_sync=1;
        if (fault==2) ram.fail_release=1;
        if (fault==3) ram.fail_validate=1;
        s=0xaaaa;
        enum intel_gpu_buffer_status result=intel_gpu_memory_create(&m,7,a,8192,&s);
        CHECK(result==(fault==0 ? INTEL_GPU_BUFFER_NO_MEMORY : fault==1 ? INTEL_GPU_BUFFER_IO :
            fault==2 ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_QUARANTINED));
        CHECK(m.pages==2 && (fault==2 || s==0xaaaa));
        CHECK(intel_gpu_memory_create(&m,7,a,1,&r)==
            (fault<2 ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_NO_MEMORY));
        if (fault<2) CHECK(m.pages==1 && ram.freed==(fault==1 ? 1u : 0u));
        intel_gpu_memory_shutdown(&m);
        result=intel_gpu_memory_collect(&m);
        if (fault>=2) {
            CHECK(m.pages==2);
            CHECK(result==(fault==2 ? INTEL_GPU_BUFFER_IO : INTEL_GPU_BUFFER_QUARANTINED));
            if (fault==2) CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages);
            else { /* Test-only cleanup of a known synthetic, unregistered quarantine. */
                CHECK(!ram.releases); free(ram.cpu[0]); ram.cpu[0]=NULL;
            }
        } else CHECK(result==INTEL_GPU_BUFFER_OK && !m.pages);
    }
    res_reset();
    intel_gpu_memory_t registries[9]={0};
    for (unsigned i=0;i<9;i++) {
        CHECK(intel_gpu_memory_init(&registries[i],&allocator,&ram,2,2));
        CHECK(intel_gpu_memory_publish(&registries[i])==(i<8 ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_BUSY));
    }
    for (unsigned i=0;i<8;i++) {
        intel_gpu_memory_shutdown(&registries[i]);
        CHECK(intel_gpu_memory_unpublish(&registries[i])==INTEL_GPU_BUFFER_OK);
    }
}

typedef struct { intel_gpu_memory_t *m; uint32_t device, resource; bool create; } res_request_t;
static void *res_manager_thread(void *argument)
{
    res_request_t *request=argument;
    if (request->create)
        CHECK(intel_gpu_memory_create(request->m,7,request->device,4097,&request->resource)==INTEL_GPU_BUFFER_INVALID);
    else CHECK(intel_gpu_memory_collect(request->m)==INTEL_GPU_BUFFER_OK);
    return NULL;
}

static void res_manager_races(void)
{
    for (unsigned phase=0;phase<2;phase++) for (unsigned round=0;round<16;round++) {
        res_reset(); intel_gpu_memory_t m={0}; uint32_t device,resource=0xaaaa;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        if (phase) {
            CHECK(intel_gpu_memory_create(&m,7,device,4097,&resource)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_memory_close(&m,7,device,resource)==INTEL_GPU_BUFFER_OK);
        }
        /* Allocate blocks in sync; reclaim blocks in the release callback. */
        __atomic_store_n(&ram.hold,1,__ATOMIC_RELEASE);
        res_request_t request={&m,device,resource,!phase}; pthread_t thread;
        CHECK(!pthread_create(&thread,NULL,res_manager_thread,&request));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        unsigned frees=ram.freed;
        d3dkmt_release_process(7);
        CHECK(m.pages==2 && ram.freed==frees);
        uint32_t late=0xeeee;
        CHECK(intel_gpu_memory_create(&m,7,device,1,&late)==INTEL_GPU_BUFFER_INVALID && late==0xeeee);
        CHECK(intel_gpu_memory_open_device(&m,8,&late)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY);
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_BUSY);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        if (!phase) CHECK(request.resource==0xaaaa && m.pages==2);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages && ram.freed==1);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

static void res_cpu_cases(void)
{
    for (unsigned phase=0;phase<4;phase++) {
        res_reset(); intel_gpu_buffer_t b={0};
        intel_gpu_cpu_access_t access={0},other={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,5000)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_cpu(NULL,7,&access)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,8,&access)==INTEL_GPU_BUFFER_INVALID && !access.self);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&access)==INTEL_GPU_BUFFER_BUSY && !access.self);
        CHECK(intel_gpu_buffer_unpin(&b,7)==INTEL_GPU_BUFFER_OK);
        if (phase==2) {
            ram.fail_sync=ram.syncs+1;
            CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&access)==INTEL_GPU_BUFFER_IO && !access.self && !b.cpu_access);
            CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_INVALID);
        }
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&access)==INTEL_GPU_BUFFER_OK && access.cpu==b.cpu && access.bytes==5000);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&access)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&other)==INTEL_GPU_BUFFER_BUSY && !other.self);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_sync(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_unpin(&b,7)==INTEL_GPU_BUFFER_INVALID);
        intel_gpu_cpu_access_t copied=access;
        CHECK(intel_gpu_buffer_release_cpu(&copied)==INTEL_GPU_BUFFER_INVALID);
        memset(access.cpu,0x6d,5000);
        for (unsigned i=5000;i<8192;i++) CHECK(!((unsigned char *)access.cpu)[i]);
        __atomic_store_n(&b.busy,1,__ATOMIC_RELEASE);
        CHECK(intel_gpu_buffer_release_cpu(&access)==INTEL_GPU_BUFFER_BUSY && access.self && b.cpu_access==&access);
        __atomic_store_n(&b.busy,0,__ATOMIC_RELEASE);
        if (!phase) CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && b.live && !ram.freed);
        if (phase==3) ram.fail_sync=ram.syncs+1;
        CHECK(intel_gpu_buffer_release_cpu(&access)==(phase==3 ? INTEL_GPU_BUFFER_IO : INTEL_GPU_BUFFER_OK));
        CHECK(!access.self && !access.cpu && !b.cpu_access && b.ready==(phase!=3));
        CHECK(intel_gpu_buffer_release_cpu(&access)==INTEL_GPU_BUFFER_INVALID);
        if (phase==3) {
            CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_INVALID);
            CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&access)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_release_cpu(&access)==INTEL_GPU_BUFFER_OK);
        }
        if (phase) {
            CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_unpin(&b,7)==INTEL_GPU_BUFFER_OK);
        }
        for (unsigned i=0;i<5000;i++) CHECK(((unsigned char *)b.cpu)[i]==0x6d);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==1);
    }
    for (unsigned phase=0;phase<4;phase++) {
        res_reset(); intel_gpu_memory_t m={0}; uint32_t device=0,resource=0;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device(&m,7,&device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create(&m,7,device,5000,&resource)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_cpu_lease_t access={0},other={0}; intel_gpu_memory_lease_t pin={0};
        CHECK(intel_gpu_memory_acquire_cpu(&m,8,device,resource,&access)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_acquire_cpu(&m,7,resource,device,&access)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_acquire_cpu(&m,7,device,resource,&access)==INTEL_GPU_BUFFER_OK && access.bytes==5000);
        intel_gpu_memory_cpu_lease_t copied=access;
        CHECK(intel_gpu_memory_release_cpu(&copied)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_acquire_cpu(&m,7,device,resource,&other)==INTEL_GPU_BUFFER_BUSY && !other.self);
        CHECK(intel_gpu_memory_acquire(&m,7,device,resource,&pin)==INTEL_GPU_BUFFER_BUSY && !pin.self);
        if (phase==0) CHECK(intel_gpu_memory_close(&m,7,device,resource)==INTEL_GPU_BUFFER_OK);
        if (phase==1) CHECK(intel_gpu_memory_close_device(&m,7,device)==INTEL_GPU_BUFFER_OK);
        if (phase==2) intel_gpu_memory_release_process(7);
        if (phase==3) intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && !ram.freed && m.pages==2);
        CHECK(intel_gpu_memory_acquire_cpu(&m,7,device,resource,&other)==INTEL_GPU_BUFFER_INVALID);
        memset(access.cpu,0x7b,5000);
        if (phase&1) ram.fail_sync=ram.syncs+1;
        CHECK(intel_gpu_memory_release_cpu(&access)==(phase&1 ? INTEL_GPU_BUFFER_IO : INTEL_GPU_BUFFER_OK));
        CHECK(!access.self && !access.cpu && !ram.freed);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && ram.freed==1 && !m.pages);
        intel_gpu_memory_shutdown(&m); CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

typedef struct {
    intel_gpu_memory_t *memory;
    uint32_t device,resource;
    intel_gpu_memory_cpu_lease_t access;
    enum intel_gpu_buffer_status result;
    bool release;
} CPU_RACE;
static void *res_cpu_thread(void *argument)
{
    CPU_RACE *race=argument;
    race->result=race->release ? intel_gpu_memory_release_cpu(&race->access) :
        intel_gpu_memory_acquire_cpu(race->memory,7,race->device,race->resource,&race->access);
    return NULL;
}
static void res_cpu_races(void)
{
    for (unsigned phase=0;phase<2;phase++) for (unsigned round=0;round<32;round++) {
        res_reset(); intel_gpu_memory_t m={0}; CPU_RACE race={.memory=&m,.release=phase!=0};
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device(&m,7,&race.device)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create(&m,7,race.device,5000,&race.resource)==INTEL_GPU_BUFFER_OK);
        if (phase) CHECK(intel_gpu_memory_acquire_cpu(&m,7,race.device,race.resource,&race.access)==INTEL_GPU_BUFFER_OK);
        ram.hold=1; ram.entered=ram.resume=0;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,res_cpu_thread,&race));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        /* These operations acquire the manager lock while cache sync is
         * paused, proving no callback holds that lock. */
        intel_gpu_memory_release_process(7);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        uint32_t other=0;
        CHECK(intel_gpu_memory_open_device(&m,8,&other)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_BUSY);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL)); ram.hold=0;
        CHECK(race.result==(phase ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_INVALID));
        CHECK(!race.access.self && !race.access.cpu && !m.resources[0].cpu_access.self);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages && ram.freed==1);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

static void res_residency_cases(void)
{
    for (unsigned mode=0;mode<5;mode++) {
        res_reset(); intel_gpu_buffer_t b={0};
        intel_gpu_cpu_access_t cpu={0}; intel_gpu_access_t gpu={0},other={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,5000)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_reside(&b,8)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_OK && b.residents==2);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_BUSY && !gpu.self);
        CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        if (mode==1) ram.fail_sync=ram.syncs+1;
        enum intel_gpu_buffer_status result=intel_gpu_buffer_acquire_gpu(&b,7,&gpu);
        CHECK(result==(mode==1 ? INTEL_GPU_BUFFER_IO : INTEL_GPU_BUFFER_OK));
        if (mode==1) {
            CHECK(!gpu.self && !b.gpu_access && !b.ready);
            CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_INVALID);
            CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_OK);
        }
        intel_gpu_access_t copied=gpu;
        CHECK(intel_gpu_buffer_release_gpu(&copied)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&other)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_unreside(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_unreside(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_unreside(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        b.busy=1;
        CHECK(intel_gpu_buffer_release_gpu(&gpu)==INTEL_GPU_BUFFER_BUSY && gpu.self && b.gpu_access);
        b.busy=0;
        CHECK(intel_gpu_buffer_release_gpu(&gpu)==INTEL_GPU_BUFFER_OK && !gpu.self && !b.gpu_access);
        CHECK(intel_gpu_buffer_release_gpu(&gpu)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.allocated==ram.freed);
    }
    res_reset();
}

typedef struct {
    intel_gpu_buffer_t *buffer;
    intel_gpu_cpu_access_t cpu;
    intel_gpu_access_t gpu;
    enum intel_gpu_buffer_status status;
    bool graphics;
} RES_GPU_RACE;
static void *res_gpu_thread(void *opaque)
{
    RES_GPU_RACE *r=opaque;
    r->status=r->graphics ? intel_gpu_buffer_acquire_gpu(r->buffer,7,&r->gpu) :
        intel_gpu_buffer_acquire_cpu(r->buffer,7,&r->cpu);
    return NULL;
}
static void res_gpu_races(void)
{
    for (unsigned phase=0;phase<2;phase++) for (unsigned round=0;round<32;round++) {
        res_reset(); intel_gpu_buffer_t b={0};
        RES_GPU_RACE r={.buffer=&b,.graphics=phase!=0};
        intel_gpu_cpu_access_t cpu={0}; intel_gpu_access_t gpu={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,5000)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_OK);
        ram.hold=1;
        pthread_t worker; CHECK(!pthread_create(&worker,NULL,res_gpu_thread,&r));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(!cpu.self && !gpu.self);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(worker,NULL) && r.status==INTEL_GPU_BUFFER_OK); ram.hold=0;
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_BUSY);
        if (phase) CHECK(intel_gpu_buffer_release_gpu(&r.gpu)==INTEL_GPU_BUFFER_OK);
        else CHECK(intel_gpu_buffer_release_cpu(&r.cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_gpu(&gpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_unreside(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.allocated==ram.freed);
    }
    res_reset();
}

int main(void)
{
    res_buffer_cases(); res_buffer_races(); res_context_cases(); res_range_cases();
    res_manager_cases(); res_manager_races();
    printf("Intel GPU resources: %u checks passed\n",checks);
    unsigned before=checks;
    res_cpu_cases(); res_cpu_races();
    printf("Intel GPU CPU ownership: %u checks passed, 64 forced exit races\n",checks-before);
    before=checks; res_residency_cases();
    printf("Intel GPU residency/access: %u checks passed\n",checks-before);
    before=checks; res_gpu_races();
    printf("Intel GPU access races: %u checks passed, 64 CPU/GPU cache races\n",checks-before);
    return 0;
}
