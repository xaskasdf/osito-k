/* Native buffers/KMT/context owners are real; only process VM is modeled. */
#define CPU_VIEW_SUITE_ENTRY cpu_view_suite_main
#include "intel_gpu_cpu_view_host.c"

typedef struct {
    intel_gpu_memory_t memory;
    intel_gpu_kmt_t provider;
    intel_gpu_contexts_t contexts;
    OKMT_LUID luid;
    OKMT_HANDLE adapter,device;
    unsigned wakes,wake_gate,wake_entered,wake_resume;
} CONTEXT_FIXTURE;
static const intel_umd_context_description_t command_layout={INTEL_UMD_CONTEXT_MAGIC,
    INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
static void context_wake(void *opaque)
{
    CONTEXT_FIXTURE *f=opaque;
    (void)intel_gpu_contexts_empty(&f->contexts);
    __atomic_add_fetch(&f->wakes,1,__ATOMIC_RELAXED);
    if (__atomic_load_n(&f->wake_gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&f->wake_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->wake_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
}
static void context_setup(CONTEXT_FIXTURE *f)
{
    res_reset(); CHECK(!vm_count);
    memset(f,0,sizeof(*f)); memset(vm_views,0,sizeof(vm_views));
    vm_mode=vm_unmaps=map_gate=map_entered=map_resume=0;
    retire_gate=retire_entered=retire_resume=0; model_pid=7; process_closing=false;
    CHECK(intel_gpu_memory_init(&f->memory,&allocator,&ram,64,32));
    CHECK(intel_gpu_memory_publish(&f->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&f->provider,&f->memory,context_wake,f));
    CHECK(intel_gpu_contexts_init(&f->contexts,&f->memory,context_wake,f));
    CHECK(intel_gpu_kmt_context_provider(&f->provider,&f->contexts));
    CHECK(!intel_gpu_kmt_context_provider(&f->provider,&f->contexts));
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    CHECK(d3dkmt_register_context_driver(&description,&f->provider.device,&f->provider.allocation,
        &f->provider.context,&f->luid)==0);
    CHECK(d3dkmt_open_adapter(7,f->luid,&f->adapter)==0);
    CHECK(d3dkmt_create_device(7,f->adapter,0,&f->device)==0);
}
static void context_cleanup(CONTEXT_FIXTURE *f,bool quarantine)
{
    vm_model_exit(7); d3dkmt_release_process(7);
    intel_gpu_contexts_shutdown(&f->contexts);
    if (quarantine) {
        CHECK(!vm_count && !intel_gpu_contexts_empty(&f->contexts));
        /* Fixture-only teardown after all VM references/producers are gone.
         * No production operation bypasses an uncertain mapping retirement. */
        for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) f->contexts.contexts[i].quarantined=false;
    }
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) CHECK(intel_gpu_contexts_collect(&f->contexts)==0);
    CHECK(intel_gpu_contexts_empty(&f->contexts) && !f->contexts.callbacks);
    CHECK(d3dkmt_collect_devices()==0 && d3dkmt_unregister_adapter(f->luid)==0);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        CHECK(intel_gpu_memory_collect(&f->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(!f->memory.pages && ram.allocated==ram.freed && !vm_count);
    intel_gpu_memory_shutdown(&f->memory);
    CHECK(intel_gpu_memory_unpublish(&f->memory)==INTEL_GPU_BUFFER_OK);
    res_reset();
}
static void native_context_cases(void)
{
    for (unsigned invalid=0;invalid<15;invalid++) {
        CONTEXT_FIXTURE f; context_setup(&f);
        intel_umd_context_description_t layout=command_layout;
        D3DKMT_CONTEXT_DESCRIPTION description={.private_bytes=sizeof(layout)};
        switch (invalid) {
        case 0: layout.magic=0; break;
        case 1: layout.version++; break;
        case 2: description.node=2; break;
        case 3: description.engine=1; break;
        case 4: description.flags=1; break;
        case 5: description.private_bytes--; break;
        case 6: layout.command_bytes=0; break;
        case 7: layout.command_bytes=252; break;
        case 8: layout.command_bytes=257; break;
        case 9: layout.command_bytes=65540; break;
        case 10: layout.allocation_count=0; break;
        case 11: layout.allocation_count=65; break;
        case 12: layout.patch_count=0; break;
        case 13: layout.patch_count=65; break;
        case 14: layout.reserved=1; break;
        }
        memcpy(description.private_data,&layout,sizeof(layout));
        OKMT_HANDLE context=0xfeed;
        D3DKMT_CONTEXT_BUFFERS buffers={.commands=0xfeed};
        CHECK(d3dkmt_create_context(7,f.device,&description,&context,&buffers)!=STATUS_SUCCESS);
        CHECK(context==0xfeed && buffers.commands==0xfeed && !ram.allocated && !vm_count);
        context_cleanup(&f,false);
    }
    for (unsigned largest=0;largest<2;largest++) {
        CONTEXT_FIXTURE f; context_setup(&f);
        intel_umd_context_description_t layout=command_layout;
        layout.command_bytes=largest ? 65536 : 256;
        layout.allocation_count=largest ? 64 : 1;
        layout.patch_count=largest ? 64 : 1;
        D3DKMT_CONTEXT_DESCRIPTION description={.private_bytes=sizeof(layout),.node=largest};
        memcpy(description.private_data,&layout,sizeof(layout));
        OKMT_HANDLE context=0;
        D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_create_context(7,f.device,&description,&context,&buffers)==0);
        CHECK(f.memory.pages==(largest ? 17u : 1u));
        CHECK(buffers.allocations==buffers.commands+layout.command_bytes &&
            buffers.patches==buffers.allocations+(largest ? 512 : 64));
        CHECK(d3dkmt_destroy_context(7,context)==0);
        context_cleanup(&f,false);
    }
    for (unsigned mode=0;mode<15;mode++) {
        CONTEXT_FIXTURE f; context_setup(&f);
        D3DKMT_CONTEXT_DESCRIPTION description={.private_bytes=sizeof(command_layout),.compat32=mode&1};
        memcpy(description.private_data,&command_layout,sizeof(command_layout));
        if (mode>=8 && mode<=11) vm_mode=mode-7;
        if (mode==11) vm_mode=5;
        if (mode==12) ram.bad_physical=2;
        if (mode==13) f.memory.allocator.cpu_physical=NULL;
        OKMT_HANDLE context=0xfeed;
        D3DKMT_CONTEXT_BUFFERS buffers={.commands=0xfeed};
        CHECK(d3dkmt_create_context(8,f.device,&description,&context,&buffers)==STATUS_INVALID_PARAMETER);
        NTSTATUS status=d3dkmt_create_context(7,f.device,&description,&context,&buffers);
        bool failed=mode>=8 && mode<=13;
        CHECK(status==(failed ? mode==11 ? STATUS_PROCESS_IS_TERMINATING : mode==12 ? STATUS_UNSUCCESSFUL :
            mode==13 ? STATUS_NOT_SUPPORTED : STATUS_NO_MEMORY : STATUS_SUCCESS));
        if (failed) {
            CHECK(context==0xfeed && buffers.commands==0xfeed);
            CHECK(vm_count==(mode==10));
        } else {
            CHECK(context!=0xfeed && buffers.commands!=0xfeed && vm_count==1 && f.memory.pages==2);
            CHECK(buffers.command_bytes==4096 && buffers.allocation_count==64 && buffers.patch_count==64);
            CHECK(buffers.allocations==buffers.commands+4096 && buffers.patches==buffers.commands+4608 && !buffers.gpu_commands);
            CHECK((buffers.commands<0x80000000ULL)==(description.compat32!=0));
            intel_gpu_kmt_context_lease_t lease={0},denied={0};
            CHECK(intel_gpu_kmt_acquire_context(8,context,&denied)==STATUS_INVALID_PARAMETER);
            CHECK(intel_gpu_kmt_acquire_context(7,context,&lease)==0);
            CHECK(lease.native.command_bytes==4096 && lease.native.allocation_count==64);
            CHECK(lease.native.allocations==(BYTE *)lease.native.commands+4096 &&
                lease.native.patches==(BYTE *)lease.native.commands+4608);
            intel_gpu_kmt_context_lease_t copied=lease;
            CHECK(intel_gpu_kmt_release_context(&copied)==STATUS_INVALID_PARAMETER);
            for (unsigned i=0;i<8192;i++) CHECK(!((BYTE *)lease.native.commands)[i]);
            memset(lease.native.commands,0xa5,4096);
            intel_gpu_command_context_t *record=&f.contexts.contexts[0];
            CHECK(record->vm.physical==0x80000000 && record->vm.physical!=record->cpu.buffer->dma);
            if (mode==1) CHECK(d3dkmt_destroy_device(7,f.device)==0);
            else if (mode==2) { process_closing=true; d3dkmt_release_process(7); }
            else if (mode==3 || mode==4) vm_model_exit(7);
            else if (mode==5) CHECK(d3dkmt_unregister_adapter(f.luid)==D3DKMT_STATUS_DEVICE_BUSY);
            else CHECK(d3dkmt_destroy_context(7,context)==0);
            CHECK(intel_gpu_memory_collect(&f.memory)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
            if (mode==3 || mode==4) {
                CHECK(intel_gpu_contexts_collect(&f.contexts)==D3DKMT_STATUS_DEVICE_BUSY);
                CHECK(intel_gpu_kmt_acquire_context(7,context,&denied)==STATUS_INVALID_PARAMETER);
            }
            CHECK(((BYTE *)lease.native.commands)[4095]==0xa5);
            CHECK(intel_gpu_kmt_release_context(&lease)==0 && !lease.reference.self && !lease.native.self);
            if (mode==3 || mode==4) {
                CHECK(intel_gpu_contexts_collect(&f.contexts)==0 && !record->resource && record->retained);
                CHECK(!intel_gpu_contexts_need_retry(&f.contexts));
                CHECK(d3dkmt_destroy_context(7,context)==0);
            }
            if (mode==5) CHECK(d3dkmt_destroy_context(7,context)==0);
            if (mode==6) ram.fail_sync=ram.syncs+1;
            if (mode==7) vm_mode=4;
            if (mode==14) {
                f.memory.resources[0].busy=true;
                CHECK(intel_gpu_contexts_collect(&f.contexts)==D3DKMT_STATUS_DEVICE_BUSY);
                CHECK(record->cpu.self && record->resource && intel_gpu_contexts_need_retry(&f.contexts));
                f.memory.resources[0].busy=false;
            }
            CHECK(intel_gpu_contexts_collect(&f.contexts)==((mode==6 || mode==7) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS));
            CHECK(!ram.freed); /* memory collector is a separate ownership layer */
            if (mode!=7) CHECK(!record->id);
            else CHECK(record->quarantined && record->resource && !intel_gpu_contexts_need_retry(&f.contexts));
            CHECK(intel_gpu_kmt_acquire_context(7,context,&denied)!=STATUS_SUCCESS);
        }
        context_cleanup(&f,mode==7 || mode==10);
    }
    CONTEXT_FIXTURE f; context_setup(&f);
    D3DKMT_CONTEXT_DESCRIPTION description={.private_bytes=sizeof(command_layout)};
    memcpy(description.private_data,&command_layout,sizeof(command_layout));
    OKMT_HANDLE handles[INTEL_GPU_CONTEXTS],untouched=0xfeed;
    D3DKMT_CONTEXT_BUFFERS buffers={0};
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++)
        CHECK(d3dkmt_create_context(7,f.device,&description,&handles[i],&buffers)==0);
    unsigned allocated=ram.allocated;
    CHECK(d3dkmt_create_context(7,f.device,&description,&untouched,&buffers)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(untouched==0xfeed && ram.allocated==allocated);
    intel_gpu_contexts_shutdown(&f.contexts);
    CHECK(!intel_gpu_contexts_empty(&f.contexts));
    for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) CHECK(d3dkmt_destroy_context(7,handles[i])==0);
    context_cleanup(&f,false);
}
typedef struct { CONTEXT_FIXTURE *fixture; D3DKMT_CONTEXT_DESCRIPTION description; OKMT_HANDLE handle; NTSTATUS status; unsigned exit; } NATIVE_CONTEXT_RACE;
static void *native_context_thread(void *argument)
{
    NATIVE_CONTEXT_RACE *r=argument;
    if (r->exit) vm_model_exit(7);
    else {
        D3DKMT_CONTEXT_BUFFERS buffers={0};
        r->status=d3dkmt_create_context(7,r->fixture->device,&r->description,&r->handle,&buffers);
    }
    return NULL;
}
static void native_context_races(void)
{
    for (unsigned phase=0;phase<2;phase++) for (unsigned round=0;round<32;round++) {
        CONTEXT_FIXTURE f; context_setup(&f);
        NATIVE_CONTEXT_RACE race={.fixture=&f,.description={.private_bytes=sizeof(command_layout)},.handle=0xfeed,.exit=phase};
        memcpy(race.description.private_data,&command_layout,sizeof(command_layout));
        if (!phase) map_gate=1;
        else {
            D3DKMT_CONTEXT_BUFFERS buffers={0};
            CHECK(d3dkmt_create_context(7,f.device,&race.description,&race.handle,&buffers)==0);
            CHECK(d3dkmt_destroy_context(7,race.handle)==0);
            intel_gpu_contexts_shutdown(&f.contexts);
            f.wake_gate=1;
        }
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,native_context_thread,&race));
        if (!phase) {
            while (!__atomic_load_n(&map_entered,__ATOMIC_ACQUIRE)) sched_yield();
            if (round%3==0) { vm_model_exit(7); d3dkmt_release_process(7); }
            else if (round%3==1) CHECK(d3dkmt_destroy_device(7,f.device)==0);
            else CHECK(d3dkmt_unregister_adapter(f.luid)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(!intel_gpu_contexts_empty(&f.contexts) && !ram.freed);
            __atomic_store_n(&map_resume,1,__ATOMIC_RELEASE);
        } else {
            while (!__atomic_load_n(&f.wake_entered,__ATOMIC_ACQUIRE)) sched_yield();
            CHECK(intel_gpu_contexts_collect(&f.contexts)==0);
            CHECK(!intel_gpu_contexts_empty(&f.contexts) && intel_gpu_contexts_need_retry(&f.contexts));
            __atomic_store_n(&f.wake_resume,1,__ATOMIC_RELEASE);
        }
        CHECK(!pthread_join(thread,NULL)); map_gate=0; f.wake_gate=0;
        if (!phase) {
            CHECK(race.status==(round%3==2 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_PROCESS_IS_TERMINATING));
            CHECK(race.handle==0xfeed);
        } else CHECK(intel_gpu_contexts_empty(&f.contexts));
        context_cleanup(&f,false);
    }
}
#ifndef NATIVE_CONTEXT_SUITE_ENTRY
#define NATIVE_CONTEXT_SUITE_ENTRY main
#endif
int NATIVE_CONTEXT_SUITE_ENTRY(void)
{
    if (cpu_view_suite_main()) return 1;
    unsigned before=checks;
    native_context_cases(); native_context_races();
    printf("Intel native command contexts: %u checks passed, 64 forced lifecycle races\n",checks-before);
    return 0;
}
