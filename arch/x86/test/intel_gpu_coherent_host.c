/* Real buffer state machine; allocator/physical addresses are a host model. */
#define main resource_suite_main
#include "intel_gpu_resource_host.c"
#undef main

static void coherent_cases(void)
{
    for (unsigned bad=1;bad<=5;bad++) {
        res_reset(); intel_gpu_buffer_t b={0}; intel_gpu_coherent_access_t a={0};
        intel_gpu_allocator_t callbacks=allocator;
        if (bad==5) callbacks.cpu_physical=NULL;
        CHECK(intel_gpu_buffer_create(&b,&callbacks,&ram,7,5000)==INTEL_GPU_BUFFER_OK);
        ram.bad_physical=bad;
        CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&a)==
            (bad==5 ? INTEL_GPU_BUFFER_UNSUPPORTED : INTEL_GPU_BUFFER_IO));
        CHECK(!a.self && !b.coherent_access && !ram.freed);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
    }
    for (unsigned phase=0;phase<4;phase++) {
        res_reset(); intel_gpu_buffer_t b={0}; intel_gpu_coherent_access_t a={0};
        intel_gpu_cpu_access_t cpu={0}; intel_gpu_access_t gpu={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,1)==INTEL_GPU_BUFFER_OK);
        CHECK((phase==0 ? intel_gpu_buffer_acquire_cpu(&b,7,&cpu) :
            phase==1 ? intel_gpu_buffer_acquire_gpu(&b,7,&gpu) :
            phase==2 ? intel_gpu_buffer_pin(&b,7) : intel_gpu_buffer_reside(&b,7))==INTEL_GPU_BUFFER_OK);
        unsigned before=ram.syncs;
        CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&a)==INTEL_GPU_BUFFER_BUSY);
        CHECK(!a.self && !b.coherent_access && ram.syncs==before);
        CHECK((phase==0 ? intel_gpu_buffer_release_cpu(&cpu) :
            phase==1 ? intel_gpu_buffer_release_gpu(&gpu) :
            phase==2 ? intel_gpu_buffer_unpin(&b,7) : intel_gpu_buffer_unreside(&b,7))==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&a)==INTEL_GPU_BUFFER_OK);
        CHECK(a.cpu==b.cpu && a.dma==b.dma && a.physical==0x80000000 && a.physical!=a.dma && a.bytes==4096);
        for (unsigned i=0;i<4096;i++) CHECK(((unsigned char *)a.cpu)[i]==0);
        CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&a)==INTEL_GPU_BUFFER_INVALID);
        intel_gpu_coherent_access_t second={0},copied=a;
        intel_gpu_coherent_reference_t first={0},last={0},late={0};
        CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&second)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_release_coherent(&copied)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_retain_coherent(&copied,&first)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_retain_coherent(&a,&first)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_retain_coherent(&a,&last)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_retain_coherent(&a,&first)==INTEL_GPU_BUFFER_INVALID);
        intel_gpu_coherent_reference_t copied_ref=first;
        CHECK(intel_gpu_buffer_drop_coherent(&copied_ref)==INTEL_GPU_BUFFER_INVALID && a.references==2);
        before=ram.syncs;
        CHECK(intel_gpu_buffer_sync(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_pin(&b,7)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(ram.syncs==before && !cpu.self && !gpu.self);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_coherent(&a)==INTEL_GPU_BUFFER_BUSY && a.closing && !b.closing);
        CHECK(intel_gpu_buffer_retain_coherent(&a,&late)==INTEL_GPU_BUFFER_INVALID && !late.self);
        CHECK(intel_gpu_buffer_reside(&b,7)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(intel_gpu_buffer_drop_coherent(&first)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_drop_coherent(&first)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_buffer_release_coherent(&a)==INTEL_GPU_BUFFER_BUSY && a.references==1);
        CHECK(intel_gpu_buffer_drop_coherent(&last)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_coherent(&a)==INTEL_GPU_BUFFER_BUSY && !a.references);
        CHECK(ram.syncs==before);
        CHECK(intel_gpu_buffer_unreside(&b,7)==INTEL_GPU_BUFFER_OK);
        if (phase&1) ram.fail_sync=ram.syncs+1;
        CHECK(intel_gpu_buffer_release_coherent(&a)==((phase&1)?INTEL_GPU_BUFFER_IO:INTEL_GPU_BUFFER_OK));
        CHECK(!a.self && !b.coherent_access && b.ready==!(phase&1));
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==ram.allocated);
    }
    res_reset(); intel_gpu_buffer_t b={0}; intel_gpu_coherent_access_t a={0};
    CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,5000)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_buffer_acquire_coherent(&b,8,&a)==INTEL_GPU_BUFFER_INVALID);
    ram.fail_sync=ram.syncs+1;
    CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&a)==INTEL_GPU_BUFFER_IO);
    CHECK(!a.self && !b.coherent_access && !b.ready);
    ram.fail_sync=0;
    CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&a)==INTEL_GPU_BUFFER_OK);
    intel_gpu_coherent_reference_t ref={0};
    a.references=UINT32_MAX;
    CHECK(intel_gpu_buffer_retain_coherent(&a,&ref)==INTEL_GPU_BUFFER_BUSY && !ref.self);
    a.references=0;
    CHECK(intel_gpu_buffer_release_coherent(&a)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK);
    res_reset();
}

typedef struct {
    intel_gpu_buffer_t *buffer;
    intel_gpu_coherent_access_t access;
    enum intel_gpu_buffer_status status;
} coherent_race_t;
static void *coherent_thread(void *opaque)
{
    coherent_race_t *r=opaque;
    r->status=intel_gpu_buffer_acquire_coherent(r->buffer,7,&r->access);
    return NULL;
}
static void coherent_races(void)
{
    for (unsigned round=0;round<32;round++) {
        res_reset(); intel_gpu_buffer_t b={0}; coherent_race_t r={.buffer=&b};
        intel_gpu_cpu_access_t cpu={0};intel_gpu_access_t gpu={0};intel_gpu_coherent_access_t other={0};
        CHECK(intel_gpu_buffer_create(&b,&allocator,&ram,7,5000)==INTEL_GPU_BUFFER_OK);
        ram.hold=1;pthread_t thread;CHECK(!pthread_create(&thread,NULL,coherent_thread,&r));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(intel_gpu_buffer_acquire_cpu(&b,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_gpu(&b,7,&gpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_acquire_coherent(&b,7,&other)==INTEL_GPU_BUFFER_BUSY);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && r.status==INTEL_GPU_BUFFER_OK);ram.hold=0;
        intel_gpu_coherent_reference_t view={0},work={0};
        CHECK(intel_gpu_buffer_retain_coherent(&r.access,&view)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_retain_coherent(&r.access,&work)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(intel_gpu_buffer_release_coherent(&r.access)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_buffer_drop_coherent(&work)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_coherent(&r.access)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(intel_gpu_buffer_drop_coherent(&view)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_coherent(&r.access)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&b,7)==INTEL_GPU_BUFFER_OK && ram.freed==ram.allocated);
    }
    res_reset();
}

static void coherent_manager_cases(void)
{
    for (unsigned phase=0;phase<4;phase++) {
        res_reset();intel_gpu_memory_t m={0};uint32_t a=0,b=0,c=0,resource=0,other=0,denied=0x76543210;
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,2));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device_epoch(&m,7,11,&a,true)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device_epoch(&m,7,11,&b,true)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device_epoch(&m,8,21,&c,true)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,a,5000,&resource)==INTEL_GPU_BUFFER_OK && m.pages==2);
        CHECK(intel_gpu_memory_create(&m,7,b,1,&denied)==INTEL_GPU_BUFFER_NO_MEMORY && denied==0x76543210);
        CHECK(intel_gpu_memory_create(&m,8,c,5000,&other)==INTEL_GPU_BUFFER_OK && m.pages==4);
        CHECK(intel_gpu_memory_close_device(&m,8,c)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_device(&m,8,c)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && m.pages==2);
        intel_gpu_memory_coherent_lease_t lease={0},bad={0};
        CHECK(intel_gpu_memory_acquire_coherent(&m,8,a,resource,&bad)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_acquire_coherent(&m,7,b,resource,&bad)==INTEL_GPU_BUFFER_INVALID && !bad.self);
        CHECK(intel_gpu_memory_acquire_coherent(&m,7,a,resource,&lease)==INTEL_GPU_BUFFER_OK);
        CHECK(lease.access->physical!=lease.access->dma && lease.access->bytes==8192 && lease.pid==7 && lease.device==a);
        intel_gpu_memory_coherent_lease_t copy=lease;
        intel_gpu_coherent_reference_t view={0},work={0},late={0};
        CHECK(intel_gpu_memory_retain_coherent(&copy,&late)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_release_coherent(&copy)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_retain_coherent(&lease,&view)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_retain_coherent(&lease,&work)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_resident_lease_t resident={0};
        CHECK(intel_gpu_memory_acquire_resident(&m,7,a,resource,&resident)==INTEL_GPU_BUFFER_OK);
        if (phase==0) CHECK(intel_gpu_memory_close(&m,7,a,resource)==INTEL_GPU_BUFFER_OK);
        if (phase==1) CHECK(intel_gpu_memory_close_device(&m,7,a)==INTEL_GPU_BUFFER_OK);
        if (phase==2) intel_gpu_memory_release_process(7);
        if (phase==3) intel_gpu_memory_shutdown(&m);
        /* Logical manager close must deny retention before buffer close runs. */
        CHECK(!lease.buffer->closing);
        CHECK(intel_gpu_memory_retain_coherent(&lease,&late)==INTEL_GPU_BUFFER_INVALID && !late.self);
        CHECK(intel_gpu_memory_acquire_coherent(&m,7,a,resource,&bad)==INTEL_GPU_BUFFER_INVALID);
        if (phase==2) {
            uint32_t next=0;
            CHECK(intel_gpu_memory_open_device_epoch(&m,7,11,&next,false)==INTEL_GPU_BUFFER_INVALID && !next);
            CHECK(intel_gpu_memory_open_device_epoch(&m,7,12,&next,false)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_memory_create(&m,7,next,1,&denied)==INTEL_GPU_BUFFER_NO_MEMORY && denied==0x76543210);
        }
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_release_resource(&m,7,a,resource)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_device(&m,7,a)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_device(&m,7,b)==INTEL_GPU_BUFFER_OK);
        CHECK(!intel_gpu_memory_collect_budget(&m));
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && m.pages==2);
        CHECK(intel_gpu_memory_release_coherent(&lease)==INTEL_GPU_BUFFER_BUSY && lease.self);
        CHECK(intel_gpu_buffer_drop_coherent(&work)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_coherent(&lease)==INTEL_GPU_BUFFER_BUSY && m.pages==2);
        CHECK(intel_gpu_buffer_drop_coherent(&view)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_coherent(&lease)==INTEL_GPU_BUFFER_BUSY); /* mapped GPU residency */
        CHECK(intel_gpu_memory_release_resident(&resident)==INTEL_GPU_BUFFER_OK);
        if (phase&1) ram.fail_sync=ram.syncs+1;
        CHECK(intel_gpu_memory_release_coherent(&lease)==((phase&1)?INTEL_GPU_BUFFER_IO:INTEL_GPU_BUFFER_OK));
        CHECK(!lease.self && m.pages==2 && intel_gpu_memory_collect_budget(&m)==1);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages && ram.freed==ram.allocated);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

typedef struct {
    intel_gpu_memory_t *memory;
    uint32_t device,resource;
    intel_gpu_memory_coherent_lease_t lease;
    enum intel_gpu_buffer_status status;
} coherent_manager_race_t;
static void *coherent_manager_race_thread(void *opaque)
{
    coherent_manager_race_t *r=opaque;
    r->status=intel_gpu_memory_acquire_coherent(r->memory,7,r->device,r->resource,&r->lease);
    return NULL;
}
static void coherent_manager_races(void)
{
    for (unsigned phase=0;phase<2;phase++) for (unsigned round=0;round<32;round++) {
        res_reset();intel_gpu_memory_t m={0};coherent_manager_race_t r={.memory=&m};
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,2));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device_epoch(&m,7,11,&r.device,true)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create_retained(&m,7,r.device,5000,&r.resource)==INTEL_GPU_BUFFER_OK);
        ram.hold=1;if (phase) ram.fail_sync=ram.syncs+2;
        pthread_t thread;CHECK(!pthread_create(&thread,NULL,coherent_manager_race_thread,&r));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        intel_gpu_memory_release_process(7);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && m.pages==2 && !ram.freed);
        CHECK(!intel_gpu_memory_collect_budget(&m));
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && r.status==INTEL_GPU_BUFFER_INVALID && !r.lease.self);ram.hold=0;
        CHECK(!m.resources[0].coherent_access.self && !m.resources[0].coherent_holder && !m.resources[0].busy);
        CHECK(intel_gpu_memory_release_resource(&m,7,r.device,r.resource)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_device(&m,7,r.device)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages && ram.freed==ram.allocated);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

int main(void)
{
    CHECK(!resource_suite_main());unsigned start=checks;
    coherent_cases();coherent_races();
    printf("Intel coherent ownership: %u checks passed, 32 forced acquisition races\n",checks-start);
    start=checks;coherent_manager_cases();coherent_manager_races();
    printf("Intel coherent device ownership: %u checks passed, 64 forced exit races\n",checks-start);
    return 0;
}
