/* Real service/contexts/native memory; only BSP scheduling and NT VM modeled. */
#define NATIVE_CONTEXT_SUITE_ENTRY context_suite_main
#include "intel_gpu_context_host.c"
#include "../drivers/intel_gpu_service.h"
#include "../kernel/sched_signal.h"
#include <setjmp.h>

static void (*service_entry)(void);
static sched_signal_t *service_signal;
static _Thread_local jmp_buf service_wait;
static unsigned spawn_calls,wake_calls,fail_spawn=1,fail_wake=1;
static _Thread_local unsigned service_cpu;
static uint64_t waited_ticks;
uint32_t smp_current_cpu(void) { return service_cpu; }
uint64_t paging_get_kernel_cr3(void) { return 0x123000; }
int sched_spawn_detached_in_address_space_blocked(const char *name,void (*entry)(void),uint64_t cr3,int owns)
{
    CHECK(!strcmp(name,"intel-gpu-gc") && cr3==0x123000 && !owns && !service_entry);
    spawn_calls++;
    if (fail_spawn) { fail_spawn=0; return -1; }
    service_entry=entry; return 42;
}
int proc_wake_pid(int pid)
{
    CHECK(pid==42 && service_entry); wake_calls++;
    if (fail_wake) { fail_wake=0; return -1; }
    return 0;
}
void smp_wake_bsp(void) { }
enum sched_signal_result sched_signal_wait(sched_signal_t *signal,uint64_t ticks)
{
    service_signal=signal; waited_ticks=ticks;
    __atomic_store_n(&signal->waiters,1,__ATOMIC_RELEASE);
    longjmp(service_wait,1);
}
static void service_step(void)
{
    CHECK(service_entry);
    for (unsigned round=0;round<8;round++) {
        if (service_signal) __atomic_store_n(&service_signal->waiters,0,__ATOMIC_RELEASE);
        if (!setjmp(service_wait)) { service_entry(); CHECK(false); }
        if (!__atomic_load_n(&service_signal->bits,__ATOMIC_ACQUIRE)) return;
    }
    CHECK(false); /* retained notifications must converge without a wake storm */
}
typedef struct { CONTEXT_FIXTURE native; uint64_t ticket; } SERVICE_FIXTURE;
static void service_setup(SERVICE_FIXTURE *f)
{
    res_reset(); CHECK(!vm_count);
    memset(f,0,sizeof(*f)); memset(vm_views,0,sizeof(vm_views));
    vm_mode=vm_unmaps=map_gate=map_entered=map_resume=0;
    retire_gate=retire_entered=retire_resume=0; model_pid=7; process_closing=false;
    CONTEXT_FIXTURE *n=&f->native;
    CHECK(intel_gpu_memory_init(&n->memory,&allocator,&ram,64,32));
    CHECK(intel_gpu_memory_publish(&n->memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&n->provider,&n->memory,intel_gpu_service_notify,NULL));
    CHECK(intel_gpu_contexts_init(&n->contexts,&n->memory,intel_gpu_service_notify,NULL));
    CHECK(intel_gpu_kmt_context_provider(&n->provider,&n->contexts));
    f->ticket=0xfeed;
    if (!spawn_calls) {
        service_cpu=1;
        CHECK(intel_gpu_service_attach(&n->memory,&n->contexts,&f->ticket)==STATUS_NOT_SUPPORTED);
        CHECK(!spawn_calls && f->ticket==0xfeed);
        service_cpu=0;
        CHECK(intel_gpu_service_attach(&n->memory,&n->contexts,&f->ticket)==STATUS_INSUFFICIENT_RESOURCES);
        CHECK(intel_gpu_service_attach(&n->memory,&n->contexts,&f->ticket)==STATUS_INSUFFICIENT_RESOURCES);
        CHECK(spawn_calls==2 && wake_calls==1 && f->ticket==0xfeed);
    }
    CHECK(intel_gpu_service_attach(&n->memory,&n->contexts,&f->ticket)==0 && f->ticket!=0xfeed);
    uint64_t untouched=0xfeed;
    CHECK(intel_gpu_service_attach(&n->memory,&n->contexts,&untouched)==STATUS_INVALID_PARAMETER && untouched==0xfeed);
    CHECK(intel_gpu_service_detach(f->ticket)==D3DKMT_STATUS_DEVICE_BUSY);
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    CHECK(d3dkmt_register_context_driver(&description,&n->provider.device,&n->provider.allocation,
        &n->provider.context,&n->luid)==0);
    CHECK(d3dkmt_open_adapter(7,n->luid,&n->adapter)==0);
    CHECK(d3dkmt_create_device(7,n->adapter,0,&n->device)==0);
    service_step(); CHECK(waited_ticks==0 && intel_gpu_service_idle());
}
static void service_cleanup(SERVICE_FIXTURE *f,bool quarantine)
{
    CONTEXT_FIXTURE *n=&f->native;
    vm_model_exit(7); d3dkmt_release_process(7);
    intel_gpu_contexts_shutdown(&n->contexts);
    intel_gpu_memory_shutdown(&n->memory);
    if (quarantine) {
        service_step(); CHECK(!intel_gpu_contexts_empty(&n->contexts) && waited_ticks==0);
        CHECK(intel_gpu_service_detach(f->ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        /* Fixture-only after every modeled VM mapping/producer is destroyed. */
        for (unsigned i=0;i<INTEL_GPU_CONTEXTS;i++) n->contexts.contexts[i].quarantined=false;
        intel_gpu_service_notify(NULL);
    }
    service_step();
    CHECK(intel_gpu_contexts_empty(&n->contexts) && !n->memory.pages && !vm_count);
    CHECK(ram.allocated==ram.freed && intel_gpu_service_idle());
    CHECK(d3dkmt_collect_devices()==0 && d3dkmt_unregister_adapter(n->luid)==0);
    CHECK(intel_gpu_memory_unpublish(&n->memory)==INTEL_GPU_BUFFER_OK);
    service_cpu=1; CHECK(intel_gpu_service_detach(f->ticket)==STATUS_NOT_SUPPORTED); service_cpu=0;
    CHECK(intel_gpu_service_detach(f->ticket)==0);
    CHECK(intel_gpu_service_detach(f->ticket)==STATUS_INVALID_PARAMETER);
    /* Delayed wake has no pointer into the detached owner, now reusable. */
    memset(f,0xa5,sizeof(*f)); intel_gpu_service_notify(NULL); service_step();
    CHECK(intel_gpu_service_idle() && spawn_calls==2 && wake_calls==2);
    res_reset();
}
static void *service_thread(void *unused)
{
    (void)unused;
    service_step();
    return NULL;
}
static void service_cases(void)
{
    for (unsigned round=0;round<64;round++) {
        SERVICE_FIXTURE f; service_setup(&f);
        CONTEXT_FIXTURE *n=&f.native;
        D3DKMT_CONTEXT_DESCRIPTION desc={.private_bytes=sizeof(command_layout),.compat32=round&1};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        OKMT_HANDLE context=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_create_context(7,n->device,&desc,&context,&buffers)==0);
        intel_gpu_kmt_context_lease_t lease={0};
        CHECK(intel_gpu_kmt_acquire_context(7,context,&lease)==0);
        unsigned mode=round%4;
        if (mode==0) CHECK(d3dkmt_destroy_context(7,context)==0);
        if (mode==1) CHECK(d3dkmt_destroy_device(7,n->device)==0);
        if (mode==2) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
        if (mode==3) vm_model_exit(7);
        service_step(); CHECK(waited_ticks==0 && intel_gpu_service_idle() && n->memory.pages==2 && !ram.freed);
        n->memory.resources[0].busy=true;
        CHECK(intel_gpu_kmt_release_context(&lease)==0);
        service_step(); CHECK(waited_ticks==1 && !intel_gpu_service_idle());
        CHECK(!vm_count && n->contexts.contexts[0].cpu.self && n->memory.pages==2);
        CHECK(!sched_signal_take(service_signal));
        n->memory.resources[0].busy=false; /* only timer retry, no new app hint */
        service_step(); CHECK(waited_ticks==0 && intel_gpu_service_idle() && !n->memory.pages && ram.freed==1);
        if (mode==3) {
            CHECK(n->contexts.contexts[0].retained && n->contexts.contexts[0].id);
            CHECK(d3dkmt_destroy_context(7,context)==0);
            service_step(); CHECK(!n->contexts.contexts[0].id);
        }
        service_cleanup(&f,false);
    }
    for (unsigned round=0;round<32;round++) {
        SERVICE_FIXTURE f; service_setup(&f);
        CONTEXT_FIXTURE *n=&f.native;
        D3DKMT_CONTEXT_DESCRIPTION desc={.private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        OKMT_HANDLE context=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_create_context(7,n->device,&desc,&context,&buffers)==0);
        CHECK(d3dkmt_destroy_context(7,context)==0);
        ram.hold=1;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,service_thread,NULL));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(!intel_gpu_service_idle() && !ram.freed);
        intel_gpu_contexts_shutdown(&n->contexts);
        intel_gpu_memory_shutdown(&n->memory);
        CHECK(intel_gpu_service_detach(f.ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_memory_unpublish(&n->memory)==INTEL_GPU_BUFFER_BUSY);
        intel_gpu_service_notify(NULL); /* retained while the collector is in a callback */
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL)); ram.hold=0;
        CHECK(intel_gpu_contexts_empty(&n->contexts) && !n->memory.pages && intel_gpu_service_idle());
        service_cleanup(&f,false);
    }
    SERVICE_FIXTURE f; service_setup(&f);
    CONTEXT_FIXTURE *n=&f.native;
    D3DKMT_CONTEXT_DESCRIPTION desc={.private_bytes=sizeof(command_layout)};
    memcpy(desc.private_data,&command_layout,sizeof(command_layout));
    OKMT_HANDLE context=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_create_context(7,n->device,&desc,&context,&buffers)==0);
    vm_mode=4;
    CHECK(d3dkmt_destroy_context(7,context)==0);
    service_step(); CHECK(waited_ticks==0 && intel_gpu_service_idle());
    CHECK(n->memory.pages==2 && n->contexts.contexts[0].quarantined && !ram.freed);
    service_cleanup(&f,true);
}
#ifndef NATIVE_SERVICE_SUITE_ENTRY
#define NATIVE_SERVICE_SUITE_ENTRY main
#endif
int NATIVE_SERVICE_SUITE_ENTRY(void)
{
    if (context_suite_main()) return 1;
    unsigned before=checks;
    service_cases();
    printf("Intel native cleanup service: %u checks passed, 64 deferred rounds, 32 forced detach races\n",checks-before);
    return 0;
}
