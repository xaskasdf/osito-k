/* Real native/KMT lifetime code; only NT process VM mapping is modeled here. */
#define KMT_SUITE_ENTRY kmt_suite_main
#include "intel_gpu_kmt_host.c"
#include "../drivers/intel_gpu_cpu_view.h"

static pthread_mutex_t vm_mutex=PTHREAD_MUTEX_INITIALIZER;
static NT_VM_DRIVER_VIEW *vm_views[128];
static unsigned vm_count,vm_mode,vm_unmaps;
static unsigned map_gate,map_entered,map_resume;
static unsigned retire_gate,retire_entered,retire_resume;
static uint64_t vm_next=0x30000000;
static unsigned vm_readonly_maps;
static _Thread_local ULONG model_pid=7;

NTSTATUS nt_process_resolve_vm_pid(HANDLE process,ACCESS_MASK access,ULONG *pid)
{
    CHECK(process==NT_CURRENT_PROCESS && !access && pid);
    pthread_mutex_lock(&admission_lock);
    NTSTATUS result=process_closing ? STATUS_PROCESS_IS_TERMINATING : STATUS_SUCCESS;
    if (!result) *pid=model_pid;
    pthread_mutex_unlock(&admission_lock);
    return result;
}

NTSTATUS nt_vm_map_driver_view(NT_VM_DRIVER_VIEW *view,uint64_t physical,SIZE_T bytes,
    BOOL compat32,BOOL writable,NT_VM_DRIVER_RETIRED retired,PVOID context)
{
    if (!writable) vm_readonly_maps++;
    ULONG pid;
    NTSTATUS status=nt_process_resolve_vm_pid(NT_CURRENT_PROCESS,0,&pid);
    if (status) return status;
    CHECK(view && !view->self && physical>=0x80000000ULL && bytes && !(bytes&4095));
    if (vm_mode==1) return STATUS_NO_MEMORY;
    pthread_mutex_lock(&vm_mutex);
    unsigned slot=128;
    for (unsigned i=0;i<128;i++) if (!vm_views[i]) { slot=i; break; }
    CHECK(slot<128);
    *view=(NT_VM_DRIVER_VIEW){.self=view,.retired=retired,.context=context,
        .address=vm_next+(compat32 ? 0 : 0x4000000000ULL),.physical=physical,
        .cr3=0x1000,.bytes=bytes,.mapped_bytes=bytes,.owner_pid=pid,.state=NT_VM_DRIVER_LIVE};
    vm_next+=0x10000; vm_views[slot]=view; vm_count++;
    if (vm_mode==2 || vm_mode==5) {
        vm_views[slot]=NULL; vm_count--; view->state=NT_VM_DRIVER_RETIRED_STATE;
    }
    if (vm_mode==3) view->state=NT_VM_DRIVER_QUARANTINED;
    pthread_mutex_unlock(&vm_mutex);
    if (vm_mode==2 || vm_mode==5) retired(view,TRUE,context);
    if (__atomic_load_n(&map_gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&map_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&map_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return vm_mode==2 || vm_mode==3 ? STATUS_NO_MEMORY : STATUS_SUCCESS;
}

NTSTATUS nt_vm_map_coherent_driver_view(NT_VM_DRIVER_VIEW *view,uint64_t physical,SIZE_T bytes,
    BOOL compat32,NT_VM_DRIVER_RETIRED retired,PVOID context)
{
    return nt_vm_map_driver_view(view,physical,bytes,compat32,FALSE,retired,context);
}

NTSTATUS nt_vm_unmap_driver_view(NT_VM_DRIVER_VIEW *view)
{
    pthread_mutex_lock(&vm_mutex);
    unsigned slot=128;
    for (unsigned i=0;i<128;i++) if (vm_views[i]==view) { slot=i; break; }
    if (slot==128) {
        pthread_mutex_unlock(&vm_mutex); return STATUS_INVALID_PARAMETER;
    }
    CHECK(view->self==view);
    vm_unmaps++;
    if (vm_mode==4 || view->state==NT_VM_DRIVER_QUARANTINED) {
        view->state=NT_VM_DRIVER_QUARANTINED;
        pthread_mutex_unlock(&vm_mutex); return STATUS_UNSUCCESSFUL;
    }
    vm_views[slot]=NULL; vm_count--; view->state=NT_VM_DRIVER_RETIRED_STATE;
    pthread_mutex_unlock(&vm_mutex);
    view->retired(view,TRUE,view->context);
    return STATUS_SUCCESS;
}

static void vm_model_exit(ULONG pid)
{
    for (unsigned i=0;i<128;i++) {
        pthread_mutex_lock(&vm_mutex);
        NT_VM_DRIVER_VIEW *view=vm_views[i];
        if (!view || view->owner_pid!=pid) { pthread_mutex_unlock(&vm_mutex); continue; }
        vm_views[i]=NULL; vm_count--;
        BOOL safe=view->state!=NT_VM_DRIVER_QUARANTINED;
        if (safe) view->state=NT_VM_DRIVER_RETIRED_STATE;
        pthread_mutex_unlock(&vm_mutex);
        if (__atomic_load_n(&retire_gate,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&retire_entered,1,__ATOMIC_RELEASE);
            while (!__atomic_load_n(&retire_resume,__ATOMIC_ACQUIRE)) sched_yield();
        }
        view->retired(view,safe,view->context);
    }
}

typedef struct {
    intel_gpu_memory_t memory;
    intel_gpu_kmt_t provider;
    intel_gpu_cpu_views_t views;
    OKMT_LUID luid;
    OKMT_HANDLE adapter,device,allocation;
    unsigned wakes;
    unsigned wake_gate,wake_entered,wake_resume;
} VIEW_FIXTURE;

static void vm_model_wake(void *opaque)
{
    VIEW_FIXTURE *f=opaque;
    /* Reentrant metadata inspection must not deadlock with a callback lock. */
    (void)intel_gpu_cpu_views_empty(&f->views);
    __atomic_add_fetch(&f->wakes,1,__ATOMIC_RELAXED);
    if (__atomic_load_n(&f->wake_gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&f->wake_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->wake_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}

static void view_setup(VIEW_FIXTURE *f)
{
    res_reset(); CHECK(!vm_count);
    memset(f,0,sizeof(*f)); memset(vm_views,0,sizeof(vm_views));
    vm_mode=vm_unmaps=map_gate=map_entered=map_resume=0; model_pid=7;
    retire_gate=retire_entered=retire_resume=0;
    process_closing=false; wake_count=0;
    CHECK(intel_gpu_memory_init(&f->memory,&allocator,&ram,8,8));
    CHECK(intel_gpu_memory_publish(&f->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&f->provider,&f->memory,kmt_provider_notify,&wake_count));
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    CHECK(d3dkmt_register_memory_driver(&description,&f->provider.device,&f->provider.allocation,&f->luid)==0);
    CHECK(d3dkmt_open_adapter(7,f->luid,&f->adapter)==0);
    CHECK(d3dkmt_create_device(7,f->adapter,0,&f->device)==0);
    D3DKMT_ALLOCATION_DESCRIPTION allocation={5000};
    CHECK(d3dkmt_create_allocations(7,f->device,1,&allocation,&f->allocation)==0);
    CHECK(intel_gpu_cpu_views_init(&f->views,vm_model_wake,f));
    CHECK(!intel_gpu_cpu_views_init(&f->views,vm_model_wake,f));
    CHECK(!intel_gpu_cpu_views_empty(&f->views));
}

static void view_cleanup(VIEW_FIXTURE *f,bool fixture_quarantine)
{
    vm_model_exit(7);
    intel_gpu_cpu_views_shutdown(&f->views);
    if (fixture_quarantine) {
        CHECK(!vm_count && !intel_gpu_cpu_views_empty(&f->views));
        /* Test-only recovery after destroying the entire modeled VM domain,
         * with all producers joined. No production quarantine bypass exists. */
        for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++)
            f->views.views[i].quarantined=false;
    }
    for (unsigned i=0;i<INTEL_GPU_CPU_VIEWS;i++)
        CHECK(intel_gpu_cpu_views_collect(&f->views)==0);
    CHECK(intel_gpu_cpu_views_empty(&f->views) && !f->views.callbacks);
    d3dkmt_release_process(7);
    CHECK(d3dkmt_collect_devices()==0);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        CHECK(intel_gpu_memory_collect(&f->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(!f->memory.pages && ram.allocated==ram.freed && !vm_count);
    CHECK(d3dkmt_unregister_adapter(f->luid)==0);
    intel_gpu_memory_shutdown(&f->memory);
    CHECK(intel_gpu_memory_unpublish(&f->memory)==INTEL_GPU_BUFFER_OK);
    res_reset();
}

static void view_cases(void)
{
    for (unsigned mode=0;mode<11;mode++) {
        VIEW_FIXTURE f; view_setup(&f);
        if (mode<6) vm_mode=mode;
        if (mode==6) ram.bad_physical=2;
        if (mode==7) f.memory.resources[0].buffer.allocator.cpu_physical=NULL;
        uint64_t ticket=0xfeed,address=0xfeed;
        CHECK(intel_gpu_cpu_view_acquire(&f.views,8,f.device,f.allocation,TRUE,TRUE,&ticket,&address)==STATUS_INVALID_PARAMETER);
        CHECK(ticket==0xfeed && address==0xfeed);
        NTSTATUS result=intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,
            mode&1,TRUE,&ticket,&address);
        if (mode==1 || mode==2 || mode==3 || mode==5 || mode==6 || mode==7) {
            CHECK(result==(mode==5 ? STATUS_PROCESS_IS_TERMINATING : mode==6 ? STATUS_UNSUCCESSFUL :
                mode==7 ? STATUS_NOT_SUPPORTED : STATUS_NO_MEMORY));
            CHECK(ticket==0xfeed && address==0xfeed);
            if (mode!=3) CHECK(!vm_count && !f.memory.resources[0].cpu_holder);
            else CHECK(vm_count==1 && f.memory.resources[0].cpu_holder);
        } else {
            CHECK(result==0 && ticket!=0xfeed && address!=0xfeed && vm_count==1);
            intel_gpu_cpu_view_t *record=&f.views.views[0];
            CHECK(record->vm.physical==0x80000000 && record->vm.physical!=record->cpu.access.buffer->dma &&
                record->vm.bytes==8192 && record->cpu.access.bytes==5000);
            uint64_t untouched=0xfeed,other_address=0xfeed;
            CHECK(intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,TRUE,TRUE,&untouched,&other_address)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(untouched==0xfeed && other_address==0xfeed);
            CHECK(intel_gpu_cpu_view_release(&f.views,8,ticket)==STATUS_INVALID_PARAMETER);
            CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket+1)==STATUS_INVALID_PARAMETER);
            if (mode==8) {
                CHECK(d3dkmt_destroy_allocations(7,f.device,1,&f.allocation)==0);
                CHECK(intel_gpu_memory_collect(&f.memory)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
            }
            if (mode==9) ram.fail_sync=ram.syncs+1;
            if (mode==10) {
                intel_gpu_cpu_views_shutdown(&f.views);
                CHECK(!intel_gpu_cpu_views_empty(&f.views));
                CHECK(intel_gpu_cpu_views_collect(&f.views)==0 && !vm_count);
                CHECK(intel_gpu_cpu_views_empty(&f.views));
                CHECK(intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,TRUE,TRUE,&untouched,&other_address)==D3DKMT_STATUS_DEVICE_REMOVED);
            } else if (mode==4) {
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==STATUS_UNSUCCESSFUL);
                CHECK(vm_count==1 && f.memory.resources[0].cpu_holder && vm_unmaps==1);
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==STATUS_UNSUCCESSFUL && vm_unmaps==1);
                CHECK(intel_gpu_cpu_views_collect(&f.views)==D3DKMT_STATUS_DEVICE_BUSY);
            } else if (mode==0) {
                intel_gpu_buffer_t *buffer=record->cpu.access.buffer;
                __atomic_store_n(&buffer->busy,1,__ATOMIC_RELEASE);
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==D3DKMT_STATUS_DEVICE_BUSY);
                CHECK(!vm_count && record->cpu.access.self && record->closing);
                __atomic_store_n(&buffer->busy,0,__ATOMIC_RELEASE);
                CHECK(intel_gpu_cpu_views_collect(&f.views)==0);
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==STATUS_INVALID_PARAMETER);
                uint64_t later=0,later_address=0;
                CHECK(intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,FALSE,TRUE,&later,&later_address)==0 && later>ticket);
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==STATUS_INVALID_PARAMETER && vm_count==1);
                CHECK(intel_gpu_cpu_view_release(&f.views,7,later)==0);
            } else {
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==
                    (mode==9 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS));
                CHECK(!vm_count && !record->ticket && !f.memory.resources[0].cpu_holder);
                CHECK(intel_gpu_cpu_view_release(&f.views,7,ticket)==STATUS_INVALID_PARAMETER);
                if (mode==9) CHECK(!f.memory.resources[0].buffer.ready);
            }
        }
        view_cleanup(&f,mode==3 || mode==4);
    }
}

typedef struct { VIEW_FIXTURE *f; bool release; uint64_t ticket,address; NTSTATUS result; } VIEW_RACE;
static void *view_race(void *opaque)
{
    VIEW_RACE *r=opaque;
    r->result=r->release ? intel_gpu_cpu_view_release(&r->f->views,7,r->ticket) :
        intel_gpu_cpu_view_acquire(&r->f->views,7,r->f->device,r->f->allocation,
            FALSE,TRUE,&r->ticket,&r->address);
    return NULL;
}

static void *view_exit_thread(void *opaque)
{
    (void)opaque;
    vm_model_exit(7);
    return NULL;
}

static void view_exit_races(void)
{
    for (unsigned phase=0;phase<3;phase++) for (unsigned round=0;round<32;round++) {
        VIEW_FIXTURE f; view_setup(&f);
        VIEW_RACE r={.f=&f,.ticket=0xfeed,.address=0xfeed};
        if (phase==2) {
            CHECK(intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,FALSE,TRUE,&r.ticket,&r.address)==0);
            r.release=true;
        }
        if (phase==1) map_gate=1;
        else ram.hold=1;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,view_race,&r));
        unsigned *entered=phase==1 ? &map_entered : &ram.entered;
        while (!__atomic_load_n(entered,__ATOMIC_ACQUIRE)) sched_yield();
        pthread_mutex_lock(&admission_lock); process_closing=true; pthread_mutex_unlock(&admission_lock);
        d3dkmt_release_process(7); vm_model_exit(7);
        intel_gpu_cpu_views_shutdown(&f.views);
        CHECK(!intel_gpu_cpu_views_empty(&f.views));
        CHECK(intel_gpu_cpu_views_collect(&f.views)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_memory_collect(&f.memory)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        if (phase==1) __atomic_store_n(&map_resume,1,__ATOMIC_RELEASE);
        else __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        if (phase<2) CHECK(r.result!=0 && r.ticket==0xfeed && r.address==0xfeed);
        else CHECK(r.result==0);
        CHECK(!vm_count && !f.memory.resources[0].cpu_holder);
        view_cleanup(&f,false);
    }
    for (unsigned round=0;round<32;round++) {
        VIEW_FIXTURE f; view_setup(&f);
        uint64_t ticket=0,address=0;
        CHECK(intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,FALSE,TRUE,&ticket,&address)==0);
        intel_gpu_cpu_views_shutdown(&f.views);
        f.wake_gate=1;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,view_exit_thread,NULL));
        while (!__atomic_load_n(&f.wake_entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_gpu_cpu_views_collect(&f.views)==0);
        CHECK(!intel_gpu_cpu_views_empty(&f.views));
        CHECK(intel_gpu_cpu_views_collect(&f.views)==D3DKMT_STATUS_DEVICE_BUSY);
        __atomic_store_n(&f.wake_resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        CHECK(intel_gpu_cpu_views_collect(&f.views)==0 && intel_gpu_cpu_views_empty(&f.views));
        view_cleanup(&f,false);
    }
    for (unsigned round=0;round<32;round++) {
        VIEW_FIXTURE f; view_setup(&f);
        uint64_t ticket=0,address=0;
        CHECK(intel_gpu_cpu_view_acquire(&f.views,7,f.device,f.allocation,FALSE,TRUE,&ticket,&address)==0);
        retire_gate=1;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,view_exit_thread,NULL));
        while (!__atomic_load_n(&retire_entered,__ATOMIC_ACQUIRE)) sched_yield();
        intel_gpu_cpu_views_shutdown(&f.views);
        /* VM has removed its entry but still owns the callback reference. */
        CHECK(intel_gpu_cpu_views_collect(&f.views)==STATUS_UNSUCCESSFUL);
        CHECK(!intel_gpu_cpu_views_empty(&f.views) && f.memory.resources[0].cpu_holder && !ram.freed);
        __atomic_store_n(&retire_resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        CHECK(intel_gpu_cpu_views_collect(&f.views)==0);
        CHECK(intel_gpu_cpu_views_empty(&f.views));
        view_cleanup(&f,false);
    }
}

#ifndef CPU_VIEW_SUITE_ENTRY
#define CPU_VIEW_SUITE_ENTRY main
#endif
int CPU_VIEW_SUITE_ENTRY(void)
{
    if (kmt_suite_main()) return 1;
    unsigned before=checks;
    view_cases(); view_exit_races();
    printf("Intel GPU persistent CPU views: %u checks passed, 160 forced exit races\n",checks-before);
    return 0;
}
