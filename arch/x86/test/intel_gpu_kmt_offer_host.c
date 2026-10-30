/* Native Intel backing behind retained KMT resource groups. No GPU emulation. */
#define KMT_SUITE_ENTRY offer_native_regression_main
#include "intel_gpu_kmt_host.c"

typedef struct {
    intel_gpu_memory_t memory;
    intel_gpu_kmt_t provider;
    OKMT_LUID luid;
    OKMT_HANDLE adapter,device,group,handles[3];
    uint32_t native_device;
    uint64_t cookies[3];
} OFFER_FIXTURE;
static void offer_native_setup(OFFER_FIXTURE *f)
{
    memset(f,0,sizeof(*f));res_reset();process_closing=false;wake_count=0;
    CHECK(intel_gpu_memory_init(&f->memory,&allocator,&ram,8,8));
    CHECK(intel_gpu_memory_publish(&f->memory)==0);
    CHECK(intel_gpu_kmt_provider(&f->provider,&f->memory,kmt_provider_notify,&wake_count));
    D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=1000};
    CHECK(d3dkmt_register_memory_driver(&desc,&f->provider.device,&f->provider.allocation,&f->luid)==0);
    CHECK(d3dkmt_open_adapter(7,f->luid,&f->adapter)==0 && d3dkmt_create_device(7,f->adapter,0,&f->device)==0);
    D3DKMT_ALLOCATION_DESCRIPTION input[3]={{4097},{4096},{4096}};
    CHECK(d3dkmt_create_resource_allocations(7,f->device,0,TRUE,3,input,&f->group,f->handles)==0);
    for (unsigned i=0;i<3;i++) {
        intel_gpu_kmt_cpu_lease_t cpu={0};CHECK(intel_gpu_kmt_acquire_cpu(7,f->device,f->handles[i],&cpu)==0);
        memset(cpu.access.cpu,0x31+i,(size_t)cpu.access.bytes);
        f->native_device=(uint32_t)cpu.reference.device_cookie;f->cookies[i]=cpu.reference.cookie;
        CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    }
}
static void offer_native_teardown(OFFER_FIXTURE *f)
{
    ram.fail_allocate=ram.fail_sync=ram.fail_release=ram.hold=0;
    d3dkmt_release_process(7);
    for (unsigned i=0;i<16;i++) {(void)d3dkmt_collect_devices();(void)intel_gpu_memory_collect(&f->memory);}
    CHECK(d3dkmt_unregister_adapter(f->luid)==0);
    intel_gpu_memory_shutdown(&f->memory);
    CHECK(intel_gpu_memory_collect(&f->memory)==0 && !f->memory.pages);
    CHECK(intel_gpu_memory_unpublish(&f->memory)==0);
    for (unsigned i=0;i<128;i++) CHECK(!ram.cpu[i]);
    res_reset();
}
static D3DKMT_OFFER_WORK *offer_native_work(OFFER_FIXTURE *f,BOOL group)
{
    D3DKMT_OFFER_WORK *w=calloc(1,sizeof(*w));CHECK(w);
    for (unsigned i=0;i<3;i++) w->inputs[i]=group?f->group:f->handles[i];
    return w;
}
static void offer_native_mark(OFFER_FIXTURE *f,BOOL group,DWORD flags,DWORD priority)
{
    D3DKMT_OFFER_WORK *w=offer_native_work(f,group);
    CHECK(d3dkmt_offer_prepare(7,f->device,group,3,D3DKMT_OFFER,priority,flags,w)==0);
    CHECK(d3dkmt_offer_commit(w)==0 && d3dkmt_offer_commit(w)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_offer_release(w)==0);free(w);
}
static void offer_native_content(OFFER_FIXTURE *f,unsigned index,BYTE expected)
{
    intel_gpu_kmt_cpu_lease_t cpu={0};CHECK(intel_gpu_kmt_acquire_cpu(7,f->device,f->handles[index],&cpu)==0);
    BOOL good=TRUE;
    for (uint64_t i=0;i<cpu.access.bytes;i++) if (((BYTE *)cpu.access.cpu)[i]!=expected) good=FALSE;
    CHECK(good);CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
}
static void offer_native_cases(void)
{
    for (unsigned groups=0;groups<2;groups++) for (unsigned flags=0;flags<4;flags++) {
        OFFER_FIXTURE f;offer_native_setup(&f);offer_native_mark(&f,groups,flags,4);
        CHECK(f.memory.pages==((flags&1)?0u:4u));
        if (!(flags&1)) {
            uint32_t priorities[3]={0xa0000000,0x50000000,0x78000000},freed=0;
            CHECK(intel_gpu_memory_properties(&f.memory,7,f.native_device,INTEL_GPU_PROPERTY_SET_PRIORITY,f.cookies,3,priorities)==0);
            CHECK(intel_gpu_memory_trim(&f.memory,7,1,&freed)==0 && freed==1);
        }
        D3DKMT_OFFER_WORK *w=offer_native_work(&f,groups);
        CHECK(d3dkmt_offer_prepare(7,f.device,groups,3,D3DKMT_RECLAIM,0,0,w)==0);
        for (unsigned i=0;i<3;i++) CHECK(w->results[i]==(groups || (flags&1) || i==1));
        D3DKMT_ALLOCATION_DESCRIPTION d={4096};OKMT_HANDLE group=0,allocation=0;
        if (groups) CHECK(d3dkmt_create_resource_allocations(7,f.device,f.group,FALSE,1,&d,&group,&allocation)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(d3dkmt_offer_release(w)==0);free(w); /* Aborted publication must keep loss. */
        w=offer_native_work(&f,groups);
        CHECK(d3dkmt_offer_prepare(7,f.device,groups,3,D3DKMT_RECLAIM,0,0,w)==0);
        for (unsigned i=0;i<3;i++) CHECK(w->results[i]==(groups || (flags&1) || i==1));
        CHECK(d3dkmt_offer_commit(w)==0 && d3dkmt_offer_release(w)==0);free(w);
        for (unsigned i=0;i<3;i++) offer_native_content(&f,i,(flags&1)||i==1?0:0x31+i);
        offer_native_teardown(&f);
    }
    for (unsigned failure=0;failure<2;failure++) {
        OFFER_FIXTURE f;offer_native_setup(&f);offer_native_mark(&f,TRUE,1,2);
        D3DKMT_OFFER_WORK *w=offer_native_work(&f,TRUE);
        if (!failure) ram.fail_allocate=ram.allocated+2; else ram.fail_sync=ram.syncs+2;
        CHECK(d3dkmt_offer_prepare(7,f.device,TRUE,3,D3DKMT_RECLAIM,0,0,w)==(failure?STATUS_UNSUCCESSFUL:STATUS_NO_MEMORY));
        CHECK(!w->self);free(w);
        ram.fail_allocate=ram.fail_sync=0;
        w=offer_native_work(&f,TRUE);
        CHECK(d3dkmt_offer_prepare(7,f.device,TRUE,3,D3DKMT_RECLAIM,0,0,w)==0);
        CHECK(w->results[0] && w->results[1] && w->results[2]);
        CHECK(d3dkmt_offer_commit(w)==0 && d3dkmt_offer_release(w)==0);free(w);
        for (unsigned i=0;i<3;i++) offer_native_content(&f,i,0);
        offer_native_teardown(&f);
    }
}
typedef struct {OFFER_FIXTURE *f;D3DKMT_OFFER_WORK *w;NTSTATUS status;unsigned *start;} OFFER_RACE;
static void *offer_native_prepare_worker(void *argument)
{
    OFFER_RACE *r=argument;
    r->status=d3dkmt_offer_prepare(7,r->f->device,TRUE,3,D3DKMT_RECLAIM,0,0,r->w);return NULL;
}
static void *offer_native_commit_worker(void *argument)
{
    OFFER_RACE *r=argument;while (!__atomic_load_n(r->start,__ATOMIC_ACQUIRE)) sched_yield();
    r->status=d3dkmt_offer_commit(r->w);return NULL;
}
static void offer_native_races(void)
{
    for (unsigned action=0;action<4;action++) for (unsigned round=0;round<8;round++) {
        OFFER_FIXTURE f;offer_native_setup(&f);offer_native_mark(&f,TRUE,1,2);
        OFFER_RACE r={.f=&f,.w=offer_native_work(&f,TRUE)};pthread_t thread;
        ram.hold=1;CHECK(!pthread_create(&thread,NULL,offer_native_prepare_worker,&r));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        if (action==0) CHECK(d3dkmt_destroy_resource(7,f.device,f.group)==0);
        if (action==1) CHECK(d3dkmt_destroy_device(7,f.device)==0);
        if (action==2) d3dkmt_release_process(7);
        if (action==3) {intel_gpu_memory_release_process(7);d3dkmt_release_process(7);}
        CHECK(ram.freed==3);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);CHECK(!pthread_join(thread,NULL));ram.hold=0;
        if (r.status==0) {
            CHECK(d3dkmt_offer_commit(r.w)==STATUS_PROCESS_IS_TERMINATING);
            CHECK(d3dkmt_offer_release(r.w)==0);
        } else CHECK(action>=2 && r.status==STATUS_INVALID_PARAMETER && !r.w->self);
        free(r.w);offer_native_teardown(&f);
    }
    for (unsigned round=0;round<32;round++) {
        OFFER_FIXTURE f;offer_native_setup(&f);offer_native_mark(&f,TRUE,1,2);
        D3DKMT_OFFER_WORK *w=offer_native_work(&f,TRUE);
        CHECK(d3dkmt_offer_prepare(7,f.device,TRUE,3,D3DKMT_RECLAIM,0,0,w)==0);
        unsigned start=0;OFFER_RACE races[2]={{.w=w,.start=&start},{.w=w,.start=&start}};
        pthread_t threads[2];for (unsigned i=0;i<2;i++) CHECK(!pthread_create(threads+i,NULL,offer_native_commit_worker,races+i));
        __atomic_store_n(&start,1,__ATOMIC_RELEASE);
        for (unsigned i=0;i<2;i++) CHECK(!pthread_join(threads[i],NULL));
        CHECK((races[0].status==0 && races[1].status==STATUS_INVALID_PARAMETER) ||
              (races[1].status==0 && races[0].status==STATUS_INVALID_PARAMETER));
        CHECK(d3dkmt_offer_release(w)==0);free(w);offer_native_teardown(&f);
    }
}
int main(void)
{
    unsigned before=checks;offer_native_cases();offer_native_races();
    printf("Intel KMT offer/reclaim: %u checks passed, 32 forced close races, 32 concurrent commit groups\n",checks-before);
    return 0;
}
