/* Real fence/memory/service code; only scheduling, RAM and NT VM are modeled. */
#define NATIVE_SERVICE_SUITE_ENTRY service_suite_main
#include "intel_gpu_service_host.c"

typedef struct {
    intel_gpu_memory_t memory;
    intel_gpu_contexts_t contexts;
    intel_gpu_fences_t fences;
    DWORD device;
    unsigned wakes,wake_gate,wake_entered,wake_resume;
    uint64_t ticket;
} FENCE_FIXTURE;
static void fence_wake(void *opaque)
{
    FENCE_FIXTURE *f=opaque;
    (void)intel_gpu_fences_empty(&f->fences);
    __atomic_add_fetch(&f->wakes,1,__ATOMIC_RELAXED);
    if (__atomic_load_n(&f->wake_gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&f->wake_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->wake_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void fence_setup(FENCE_FIXTURE *f,bool service,unsigned pages)
{
    res_reset(); CHECK(!vm_count);
    memset(f,0,sizeof(*f)); memset(vm_views,0,sizeof(vm_views));
    vm_mode=vm_unmaps=map_gate=map_entered=map_resume=0;
    retire_gate=retire_entered=retire_resume=0; model_pid=7; process_closing=false;
    CHECK(intel_gpu_memory_init(&f->memory,&allocator,&ram,pages,pages));
    CHECK(intel_gpu_memory_publish(&f->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_memory_open_device_epoch(&f->memory,7,29,&f->device,true)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_fences_init(&f->fences,&f->memory,service?intel_gpu_service_notify:fence_wake,service?NULL:f));
    CHECK(!intel_gpu_fences_init(&f->fences,&f->memory,fence_wake,f));
    if (service) {
        CHECK(intel_gpu_contexts_init(&f->contexts,&f->memory,intel_gpu_service_notify,NULL));
        CHECK(intel_gpu_service_attach_fences(&f->memory,&f->contexts,NULL,&f->fences,&f->ticket)==0);
        service_step(); CHECK(intel_gpu_service_idle());
    }
}
static void fence_drain(FENCE_FIXTURE *f)
{
    if (f->ticket) service_step();
    else for (unsigned i=0;i<INTEL_GPU_FENCES+1;i++) {
        (void)intel_gpu_fences_collect(&f->fences);
        (void)intel_gpu_memory_collect(&f->memory);
    }
}
static void fence_cleanup(FENCE_FIXTURE *f,bool quarantine)
{
    vm_model_exit(7);
    intel_gpu_fences_shutdown(&f->fences);
    intel_gpu_memory_shutdown(&f->memory);
    if (f->contexts.self) intel_gpu_contexts_shutdown(&f->contexts);
    CHECK(intel_gpu_memory_release_device(&f->memory,7,f->device)==INTEL_GPU_BUFFER_OK);
    if (quarantine) {
        fence_drain(f); CHECK(!vm_count && f->memory.pages && !ram.freed);
        /* Only this fixture has destroyed every VM domain and joined all users.
         * Production has no quarantine bypass or fabricated retirement proof. */
        for (unsigned i=0;i<INTEL_GPU_FENCES;i++) f->fences.fences[i].quarantined=false;
    }
    fence_drain(f);
    CHECK(intel_gpu_fences_empty(&f->fences) && !f->memory.pages && !vm_count);
    CHECK(intel_gpu_memory_unpublish(&f->memory)==INTEL_GPU_BUFFER_OK);
    if (f->ticket) {
        CHECK(intel_gpu_service_detach(f->ticket)==0);
        memset(f,0xa5,sizeof(*f)); intel_gpu_service_notify(NULL); service_step();
        CHECK(intel_gpu_service_idle());
    }
    res_reset();
}
static void fence_cases(void)
{
    for (unsigned mode=0;mode<8;mode++) {
        FENCE_FIXTURE f; fence_setup(&f,false,2);
        uint64_t id=0,address=0,second=0,address2=0;
        unsigned before=vm_readonly_maps;
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,0x11223344ffffffffULL,mode&1,&id,&address)==0);
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,9,!(mode&1),&second,&address2)==0);
        CHECK(id!=second && address!=address2 && vm_readonly_maps==before+2);
        CHECK((mode&1)?address<0x100000000ULL:address>=0x100000000ULL);
        uint64_t extra=0xfeed,extra_address=0xfeed;
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,0,TRUE,&extra,&extra_address)==STATUS_NO_MEMORY);
        CHECK(extra==0xfeed && extra_address==0xfeed && f.memory.pages==2);
        intel_gpu_fence_lease_t lease={0},last={0},bad={0};
        CHECK(intel_gpu_fence_acquire(&f.fences,8,f.device,id,&bad)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_fence_acquire(&f.fences,7,f.device+1,id,&bad)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_fence_acquire(&f.fences,7,f.device,id,&lease)==0);
        CHECK(lease.epoch==29 && lease.bytes==4096 && lease.physical!=lease.dma);
        CHECK(lease.physical==f.fences.fences[0].vm.physical);
        CHECK(*(uint64_t *)lease.cpu==0x11223344ffffffffULL);
        for (unsigned i=8;i<4096;i++) CHECK(!((BYTE *)lease.cpu)[i]);
        CHECK(intel_gpu_fence_acquire(&f.fences,7,f.device,id,&last)==0);
        intel_gpu_fence_lease_t copy=lease;
        CHECK(intel_gpu_fence_release(&copy)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_fence_close(&f.fences,8,f.device,id)==STATUS_INVALID_PARAMETER);
        if (mode==1) CHECK(intel_gpu_memory_close_device(&f.memory,7,f.device)==INTEL_GPU_BUFFER_OK);
        if (mode==2) intel_gpu_memory_release_process(7);
        if (mode==3) vm_model_exit(7);
        if (mode==4) intel_gpu_fences_shutdown(&f.fences);
        if (mode==5) intel_gpu_memory_shutdown(&f.memory);
        if (mode==6) vm_mode=4;
        CHECK(intel_gpu_fence_close(&f.fences,7,f.device,id)==0);
        CHECK(intel_gpu_fence_close(&f.fences,7,f.device,id)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_fence_close(&f.fences,7,f.device,second)==0);
        CHECK(intel_gpu_fence_acquire(&f.fences,7,f.device,id,&bad)==STATUS_INVALID_PARAMETER && !bad.self);
        fence_drain(&f);
        CHECK(f.memory.pages==(mode==6?2u:1u) && lease.self && last.self);
        CHECK(mode==6 ? f.fences.fences[0].quarantined : !vm_count);
        CHECK(!intel_gpu_fences_need_retry(&f.fences));
        CHECK(intel_gpu_fence_release(&lease)==0 && !lease.self);
        fence_drain(&f); CHECK(f.memory.pages && last.self);
        if (mode==7) ram.fail_sync=ram.syncs+1;
        CHECK(intel_gpu_fence_release(&last)==0 && !last.self);
        fence_drain(&f);
        fence_cleanup(&f,mode==6);
    }
    for (unsigned failure=0;failure<7;failure++) {
        FENCE_FIXTURE f; fence_setup(&f,false,2);
        if (failure==0) ram.fail_allocate=1;
        if (failure==1) ram.bad_physical=4;
        if (failure==2) ram.fail_sync=2;
        if (failure>=3) vm_mode=failure==6?5:failure-2;
        uint64_t id=0xfeed,address=0xfeed;
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,0,FALSE,&id,&address)!=0);
        CHECK(id==0xfeed && address==0xfeed);
        fence_cleanup(&f,failure==5);
    }
    FENCE_FIXTURE f; fence_setup(&f,false,INTEL_GPU_FENCES);
    uint64_t ids[INTEL_GPU_FENCES],address=0;
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++)
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,i,TRUE,&ids[i],&address)==0);
    uint64_t untouched=0xfeed;
    CHECK(intel_gpu_fence_create(&f.fences,7,f.device,0,TRUE,&untouched,&address)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(untouched==0xfeed);
    intel_gpu_fences_shutdown(&f.fences); fence_drain(&f);
    CHECK(!f.memory.pages && !intel_gpu_fences_empty(&f.fences));
    CHECK(!intel_gpu_fences_need_retry(&f.fences));
    for (unsigned i=0;i<INTEL_GPU_FENCES;i++) CHECK(intel_gpu_fence_close(&f.fences,7,f.device,ids[i])==0);
    fence_cleanup(&f,false);
}
typedef struct { FENCE_FIXTURE *f; uint64_t id,address; NTSTATUS status; } FENCE_RACE;
static void *fence_create_thread(void *opaque)
{
    FENCE_RACE *r=opaque;
    r->status=intel_gpu_fence_create(&r->f->fences,7,r->f->device,7,TRUE,&r->id,&r->address);
    return NULL;
}
static void *fence_exit_thread(void *opaque) { (void)opaque; vm_model_exit(7); return NULL; }
static void fence_races(void)
{
    for (unsigned round=0;round<32;round++) {
        FENCE_FIXTURE f; fence_setup(&f,false,2);
        FENCE_RACE r={.f=&f,.id=0xfeed,.address=0xfeed}; pthread_t thread;
        map_gate=1;
        CHECK(!pthread_create(&thread,NULL,fence_create_thread,&r));
        while (!__atomic_load_n(&map_entered,__ATOMIC_ACQUIRE)) sched_yield();
        if (round&1) vm_model_exit(7);
        else CHECK(intel_gpu_memory_close_device(&f.memory,7,f.device)==INTEL_GPU_BUFFER_OK);
        fence_drain(&f); CHECK(f.memory.pages==1 && !ram.freed);
        __atomic_store_n(&map_resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && r.status!=0 && r.id==0xfeed && r.address==0xfeed);
        fence_cleanup(&f,false);
    }
    for (unsigned round=0;round<16;round++) {
        FENCE_FIXTURE f; fence_setup(&f,false,2);
        uint64_t id=0,address=0; pthread_t thread;
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,8,FALSE,&id,&address)==0);
        f.wake_gate=1;
        CHECK(!pthread_create(&thread,NULL,fence_exit_thread,NULL));
        while (!__atomic_load_n(&f.wake_entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(f.fences.callbacks==1);
        __atomic_store_n(&f.wake_gate,0,__ATOMIC_RELEASE);
        CHECK(intel_gpu_fence_close(&f.fences,7,f.device,id)==0);
        intel_gpu_fences_shutdown(&f.fences); fence_drain(&f);
        CHECK(!f.memory.pages && !intel_gpu_fences_empty(&f.fences));
        __atomic_store_n(&f.wake_resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL) && intel_gpu_fences_empty(&f.fences));
        fence_cleanup(&f,false);
    }
}
static void fence_service_cases(void)
{
    for (unsigned round=0;round<8;round++) {
        FENCE_FIXTURE f; fence_setup(&f,true,2);
        uint64_t id=0,address=0; intel_gpu_fence_lease_t lease={0};
        CHECK(intel_gpu_fence_create(&f.fences,7,f.device,42,round&1,&id,&address)==0);
        CHECK(intel_gpu_fence_acquire(&f.fences,7,f.device,id,&lease)==0);
        CHECK(intel_gpu_fence_close(&f.fences,7,f.device,id)==0);
        service_step(); CHECK(!vm_count && f.memory.pages==1 && !ram.freed && intel_gpu_service_idle());
        CHECK(intel_gpu_service_detach(f.ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        f.memory.resources[0].busy=true;
        CHECK(intel_gpu_fence_release(&lease)==0);
        service_step(); CHECK(waited_ticks==1 && !intel_gpu_service_idle() && f.memory.pages==1);
        f.memory.resources[0].busy=false;
        service_step(); CHECK(waited_ticks==0 && intel_gpu_service_idle() && !f.memory.pages);
        fence_cleanup(&f,false);
    }
}
int main(void)
{
    if (service_suite_main()) return 1;
    unsigned before=checks;
    fence_cases(); fence_races(); fence_service_cases();
    printf("Intel coherent fence owners: %u checks passed, 32 creation races, 16 retirement callbacks, 8 service rounds\n",checks-before);
    return 0;
}
