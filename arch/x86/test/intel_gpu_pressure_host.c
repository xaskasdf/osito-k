/* Real memory manager; synthetic physical allocator, no rendered pixels. */
#define main resource_suite_main
#include "intel_gpu_resource_host.c"
#undef main

static intel_gpu_memory_t *pressure_manager;
static uint32_t pressure_device;
static unsigned pressure_hook,pressure_callbacks;

static bool pressure_release(void *ctx,uint64_t dma,void *cpu,uint32_t pages)
{
    pressure_callbacks++;
    if (pressure_hook==1) return false;
    if (pressure_hook==2) {
        uint32_t output=0xeeee;
        CHECK(intel_gpu_memory_create(pressure_manager,7,pressure_device,4096,&output)==INTEL_GPU_BUFFER_NO_MEMORY);
        CHECK(output==0xeeee);
    }
    return res_release(ctx,dma,cpu,pages);
}
static const intel_gpu_allocator_t pressure_allocator={res_allocate,res_validate,
    pressure_release,res_sync,res_cpu_physical};

static void pressure_setup(intel_gpu_memory_t *m,uint32_t total,uint32_t process)
{
    res_reset(); pressure_hook=pressure_callbacks=0;
    CHECK(intel_gpu_memory_init(m,&pressure_allocator,&ram,total,process));
    CHECK(intel_gpu_memory_publish(m)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_open_device(m,7,&pressure_device)==INTEL_GPU_BUFFER_OK);
    pressure_manager=m;
}

static void pressure_cleanup(intel_gpu_memory_t *m)
{
    pressure_hook=0;
    intel_gpu_memory_shutdown(m);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES && m->pages;i++)
        CHECK(intel_gpu_memory_collect(m)==INTEL_GPU_BUFFER_OK);
    CHECK(!m->pages && ram.allocated==ram.freed);
    CHECK(intel_gpu_memory_unpublish(m)==INTEL_GPU_BUFFER_OK);
}

static void pressure_chains(void)
{
    intel_gpu_memory_t m={0}; pressure_setup(&m,64,32);
    /* Same rounded page demand as the P1 texture sequence, with no worker
     * scheduled between destroys and subsequent reservations. */
    const unsigned chains[]={5,5,17,3,3,13,5};
    for (unsigned chain=0;chain<sizeof(chains)/sizeof(*chains);chain++) {
        uint32_t ids[17]={0};
        for (unsigned i=0;i<chains[chain];i++)
            CHECK(intel_gpu_memory_create(&m,7,pressure_device,4096,&ids[i])==INTEL_GPU_BUFFER_OK);
        for (unsigned i=0;i<chains[chain];i++)
            CHECK(intel_gpu_memory_close(&m,7,pressure_device,ids[i])==INTEL_GPU_BUFFER_OK);
        CHECK(m.pages<=32);
    }
    CHECK(ram.freed>0);
    pressure_cleanup(&m);
}

static void pressure_retained(void)
{
    for (unsigned mode=0;mode<6;mode++) {
        intel_gpu_memory_t m={0}; pressure_setup(&m,1,1);
        uint32_t id=0,next=0xeeee;
        CHECK((mode==2 ? intel_gpu_memory_create_retained(&m,7,pressure_device,1,&id) :
            intel_gpu_memory_create(&m,7,pressure_device,1,&id))==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_lease_t pin={0};
        intel_gpu_memory_cpu_lease_t cpu={0};
        intel_gpu_memory_coherent_lease_t coherent={0};
        intel_gpu_memory_resident_lease_t resident={0};
        if (mode==0) CHECK(intel_gpu_memory_acquire(&m,7,pressure_device,id,&pin)==INTEL_GPU_BUFFER_OK);
        if (mode==1) CHECK(intel_gpu_memory_acquire_cpu(&m,7,pressure_device,id,&cpu)==INTEL_GPU_BUFFER_OK);
        if (mode==3) pressure_hook=1;
        if (mode==4) CHECK(intel_gpu_memory_acquire_coherent(&m,7,pressure_device,id,&coherent)==INTEL_GPU_BUFFER_OK);
        if (mode==5) {
            CHECK(intel_gpu_memory_acquire_resident(&m,7,pressure_device,id,&resident)==INTEL_GPU_BUFFER_OK);
            CHECK(intel_gpu_memory_reserve_work(&resident)==INTEL_GPU_BUFFER_OK);
        }
        CHECK(intel_gpu_memory_close(&m,7,pressure_device,id)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create(&m,7,pressure_device,1,&next)==INTEL_GPU_BUFFER_NO_MEMORY);
        CHECK(next==0xeeee && m.pages==1 && !ram.freed);
        CHECK(pressure_callbacks==(mode==3 ? 1u : 0u));
        if (mode==0) CHECK(intel_gpu_memory_release(&pin)==INTEL_GPU_BUFFER_OK);
        if (mode==1) CHECK(intel_gpu_memory_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        if (mode==2) CHECK(intel_gpu_memory_release_resource(&m,7,pressure_device,id)==INTEL_GPU_BUFFER_OK);
        if (mode==3) pressure_hook=0;
        if (mode==4) CHECK(intel_gpu_memory_release_coherent(&coherent)==INTEL_GPU_BUFFER_OK);
        if (mode==5) CHECK(intel_gpu_memory_release_resident(&resident)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_create(&m,7,pressure_device,1,&next)==INTEL_GPU_BUFFER_OK);
        CHECK(next!=id && m.pages==1 && ram.freed==1);
        pressure_cleanup(&m);
    }
}

static void pressure_limits(void)
{
    for (unsigned mode=0;mode<3;mode++) {
        intel_gpu_memory_t m={0}; pressure_setup(&m,mode==1 ? 128 : 2,mode==1 ? 128 : 2);
        uint32_t other=0,ids[64]={0},output=0xeeee;
        CHECK(intel_gpu_memory_open_device(&m,8,&other)==INTEL_GPU_BUFFER_OK);
        unsigned count=mode==1 ? 64 : 2;
        for (unsigned i=0;i<count;i++)
            CHECK(intel_gpu_memory_create(&m,8,other,4096,&ids[i])==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_close(&m,8,other,ids[count-1])==INTEL_GPU_BUFFER_OK);
        if (mode==2) {
            CHECK(intel_gpu_memory_create(&m,7,pressure_device,3*4096,&output)==INTEL_GPU_BUFFER_NO_MEMORY);
            CHECK(output==0xeeee && !pressure_callbacks && !ram.freed);
        } else {
            CHECK(intel_gpu_memory_create(&m,7,pressure_device,4096,&output)==INTEL_GPU_BUFFER_OK);
            CHECK(m.pages==count && ram.freed==1);
        }
        pressure_cleanup(&m);
    }
    intel_gpu_memory_t m={0}; pressure_setup(&m,1,1);
    uint32_t id=0,output=0xeeee;
    CHECK(intel_gpu_memory_create(&m,7,pressure_device,1,&id)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_close(&m,7,pressure_device,id)==INTEL_GPU_BUFFER_OK);
    pressure_hook=2;
    CHECK(intel_gpu_memory_create(&m,7,pressure_device,1,&output)==INTEL_GPU_BUFFER_OK);
    CHECK(pressure_callbacks==1 && ram.freed==1);
    pressure_cleanup(&m);
}

typedef struct {
    intel_gpu_memory_t *m;
    uint32_t device,output;
    enum intel_gpu_buffer_status result;
} pressure_request_t;
static void *pressure_thread(void *opaque)
{
    pressure_request_t *r=opaque;
    r->result=intel_gpu_memory_create(r->m,7,r->device,1,&r->output);
    return NULL;
}
static void pressure_races(void)
{
    for (unsigned mode=0;mode<3;mode++) for (unsigned round=0;round<16;round++) {
        intel_gpu_memory_t m={0}; pressure_setup(&m,1,1);
        uint32_t id=0,late=0xeeee;
        CHECK(intel_gpu_memory_create(&m,7,pressure_device,1,&id)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_close(&m,7,pressure_device,id)==INTEL_GPU_BUFFER_OK);
        pressure_request_t request={&m,pressure_device,0xeeee,INTEL_GPU_BUFFER_OK};
        ram.hold=1;
        pthread_t worker; CHECK(!pthread_create(&worker,NULL,pressure_thread,&request));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_gpu_memory_create(&m,7,pressure_device,1,&late)==INTEL_GPU_BUFFER_NO_MEMORY && late==0xeeee);
        if (mode==0) CHECK(intel_gpu_memory_close_device(&m,7,pressure_device)==INTEL_GPU_BUFFER_OK);
        if (mode==1) intel_gpu_memory_release_process(7);
        if (mode==2) intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_BUSY && m.pages==1);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(worker,NULL)); ram.hold=0;
        CHECK(request.result==INTEL_GPU_BUFFER_INVALID && request.output==0xeeee);
        CHECK(!m.pages && ram.freed==1 && pressure_callbacks==1);
        pressure_cleanup(&m);
    }
}

int main(void)
{
    resource_suite_main();
    unsigned before=checks;
    pressure_chains(); pressure_retained(); pressure_limits(); pressure_races();
    printf("Intel GPU allocation pressure: %u checks passed, 48 close/exit races\n",checks-before);
    return 0;
}
