/* Reuse the native allocator/resource suite; only NT process admission is
 * modeled here. The in-OS test separately exercises real NT objects/PMM. */
#define main resource_suite_main
#include "intel_gpu_resource_host.c"
#undef main
#include "../drivers/intel_gpu_kmt.h"
#include "../drivers/intel_gpu_umd_abi.h"

static pthread_mutex_t admission_lock=PTHREAD_MUTEX_INITIALIZER;
static bool process_closing;
static unsigned open_gate,open_entered,open_resume,wake_count;

NTSTATUS nt_process_open_gpu_device_retained(ULONG pid,struct intel_gpu_memory *m,ULONG *device)
{
    pthread_mutex_lock(&admission_lock);
    NTSTATUS status=STATUS_INVALID_CID;
    if (pid==7 || pid==8) {
        status=STATUS_PROCESS_IS_TERMINATING;
        if (!process_closing) {
            enum intel_gpu_buffer_status result=intel_gpu_memory_open_device_retained(m,pid,device);
            status=result==INTEL_GPU_BUFFER_OK ? STATUS_SUCCESS :
                result==INTEL_GPU_BUFFER_NO_MEMORY ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER;
        }
    }
    pthread_mutex_unlock(&admission_lock);
    if (__atomic_load_n(&open_gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&open_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&open_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}

static void kmt_provider_notify(void *context)
{
    CHECK(context==&wake_count);
    __atomic_add_fetch(&wake_count,1,__ATOMIC_RELAXED);
}

typedef struct { OKMT_HANDLE adapter,device; NTSTATUS status; } KMT_RACE;
static void *kmt_provider_race(void *context)
{
    KMT_RACE *race=context;
    race->status=d3dkmt_create_device(7,race->adapter,0,&race->device);
    return NULL;
}

static unsigned other_closes;
static NTSTATUS kmt_other_open(void *context,DWORD pid,uint64_t *cookie)
{
    CHECK(context==(void *)1 && pid==7);
    *cookie=0xfeed;
    return STATUS_SUCCESS;
}
static NTSTATUS kmt_other_close(void *context,DWORD pid,uint64_t cookie)
{
    CHECK(context==(void *)1 && pid==7 && cookie==0xfeed);
    other_closes++;
    return STATUS_SUCCESS;
}

static D3DKMT_ALLOCATION_PROVIDER native_allocations;
static unsigned allocation_entered,allocation_resume;
static NTSTATUS kmt_allocation_gated_open(void *ctx,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *description,uint64_t *cookie)
{
    NTSTATUS result=native_allocations.allocate(ctx,pid,device,description,cookie);
    if (result==STATUS_SUCCESS) {
        __atomic_store_n(&allocation_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&allocation_resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return result;
}
typedef struct { OKMT_HANDLE device,output; NTSTATUS status; } KMT_ALLOCATION_RACE;
static void *kmt_allocation_race(void *context)
{
    KMT_ALLOCATION_RACE *race=context;
    D3DKMT_ALLOCATION_DESCRIPTION desc={4097};
    race->status=d3dkmt_create_allocations(7,race->device,1,&desc,&race->output);
    return NULL;
}

static void kmt_cpu_cases(void)
{
    for (unsigned phase=0;phase<6;phase++) {
        res_reset(); process_closing=false; wake_count=0;
        intel_gpu_memory_t m={0}; intel_gpu_kmt_t provider={0};
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_kmt_provider(&provider,&m,kmt_provider_notify,&wake_count));
        D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
        D3DKMT_ALLOCATION_PROVIDER memory=provider.allocation;
        if (phase==5) memory.translate=NULL; /* foreign allocation policy */
        OKMT_LUID luid; OKMT_HANDLE adapter=0,device=0,group=0,allocation=0;
        CHECK(d3dkmt_register_memory_driver(&description,&provider.device,&memory,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
        CHECK(d3dkmt_create_device(7,adapter,0,&device)==0);
        D3DKMT_ALLOCATION_DESCRIPTION input={5000};
        CHECK(d3dkmt_create_resource_allocations(7,device,0,TRUE,1,&input,&group,&allocation)==0);
        intel_gpu_kmt_cpu_lease_t access={0},other={0};
        CHECK(intel_gpu_kmt_acquire_cpu(8,device,allocation,&access)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_kmt_acquire_cpu(7,device,group,&access)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_kmt_acquire_cpu(7,device,allocation,NULL)==STATUS_INVALID_PARAMETER);
        if (phase==5) {
            CHECK(intel_gpu_kmt_acquire_cpu(7,device,allocation,&access)==STATUS_NOT_SUPPORTED);
            CHECK(!access.reference.self && !access.access.self && !wake_count);
        } else {
            CHECK(intel_gpu_kmt_acquire_cpu(7,device,allocation,&access)==0);
            CHECK(access.access.bytes==5000 && access.access.cpu && access.reference.self);
            uint64_t physical=0,rounded=0;
            CHECK(intel_gpu_kmt_cpu_physical(&access,&physical,&rounded)==0 &&
                physical==0x80000000ULL && physical!=access.access.buffer->dma && rounded==8192);
            for (unsigned bad=1;bad<=4;bad++) {
                ram.bad_physical=bad; physical=rounded=0xfeed;
                CHECK(intel_gpu_kmt_cpu_physical(&access,&physical,&rounded)==STATUS_UNSUCCESSFUL &&
                    physical==0xfeed && rounded==0xfeed);
            }
            ram.bad_physical=0;
            CHECK(intel_gpu_kmt_cpu_physical(&access,NULL,&rounded)==STATUS_INVALID_PARAMETER);
            CHECK(intel_gpu_kmt_acquire_cpu(7,device,allocation,&access)==STATUS_INVALID_PARAMETER);
            CHECK(intel_gpu_kmt_acquire_cpu(7,device,allocation,&other)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(!other.reference.self && !other.access.self && !wake_count);
            intel_gpu_kmt_cpu_lease_t copied=access;
            CHECK(intel_gpu_kmt_release_cpu(&copied)==STATUS_INVALID_PARAMETER);
            intel_gpu_memory_lease_t pin={0};
            CHECK(intel_gpu_memory_acquire(&m,7,access.access.device,access.access.resource,&pin)==INTEL_GPU_BUFFER_BUSY);
            intel_gpu_buffer_t *buffer=access.access.buffer;
            __atomic_store_n(&buffer->busy,1,__ATOMIC_RELEASE);
            CHECK(intel_gpu_kmt_release_cpu(&access)==D3DKMT_STATUS_DEVICE_BUSY && access.reference.self && access.access.self);
            __atomic_store_n(&buffer->busy,0,__ATOMIC_RELEASE);
            memset(access.access.cpu,0x5d,5000);
            if (phase==0) CHECK(d3dkmt_destroy_resource(7,device,group)==0);
            if (phase==1) CHECK(d3dkmt_destroy_device(7,device)==0);
            if (phase==2) { process_closing=true; d3dkmt_release_process(7); }
            if (phase==3) CHECK(d3dkmt_unregister_adapter(luid)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && !ram.freed && m.pages==2);
            if (phase==4) ram.fail_sync=ram.syncs+1;
            CHECK(intel_gpu_kmt_release_cpu(&access)==(phase==4 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS));
            CHECK(!access.reference.self && !access.access.self && !ram.freed && wake_count);
            CHECK(intel_gpu_kmt_release_cpu(&access)==STATUS_INVALID_PARAMETER);
            if (phase==4) {
                CHECK(intel_gpu_buffer_pin(buffer,7)==INTEL_GPU_BUFFER_INVALID);
                CHECK(intel_gpu_kmt_acquire_cpu(7,device,allocation,&access)==0);
                for (unsigned i=0;i<5000;i++) CHECK(((BYTE *)access.access.cpu)[i]==0x5d);
                CHECK(intel_gpu_kmt_release_cpu(&access)==0);
                CHECK(intel_gpu_buffer_pin(buffer,7)==INTEL_GPU_BUFFER_OK);
                CHECK(intel_gpu_buffer_unpin(buffer,7)==INTEL_GPU_BUFFER_OK);
            }
        }
        d3dkmt_release_process(7);
        CHECK(d3dkmt_collect_devices()==0);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && !m.pages && ram.freed==1);
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        intel_gpu_memory_shutdown(&m); CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
}

#ifndef KMT_SUITE_ENTRY
#define KMT_SUITE_ENTRY main
#endif
int KMT_SUITE_ENTRY(void)
{
    if (resource_suite_main()) return 1;
    checks=0;
    for (unsigned phase=0;phase<4;phase++) {
        res_reset(); process_closing=false; wake_count=0;
        intel_gpu_memory_t memory={0}; intel_gpu_kmt_t kmt={0};
        CHECK(!intel_gpu_kmt_provider(&kmt,&memory,kmt_provider_notify,&wake_count));
        CHECK(intel_gpu_memory_init(&memory,&allocator,&ram,4,4));
        CHECK(!intel_gpu_kmt_provider(&kmt,&memory,kmt_provider_notify,&wake_count));
        CHECK(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK);
        CHECK(!intel_gpu_kmt_provider(&kmt,&memory,NULL,&wake_count));
        CHECK(intel_gpu_kmt_provider(&kmt,&memory,kmt_provider_notify,&wake_count));
        CHECK(!intel_gpu_kmt_provider(&kmt,&memory,kmt_provider_notify,&wake_count));
        intel_gpu_kmt_t moved=kmt; uint64_t invalid_cookie=0xfeed;
        CHECK(kmt.device.open(&moved,7,&invalid_cookie)==STATUS_INVALID_PARAMETER && invalid_cookie==0xfeed);
        CHECK(kmt.device.open(&kmt,99,&invalid_cookie)==STATUS_INVALID_CID && invalid_cookie==0xfeed);
        CHECK(kmt.device.close(&kmt,7,0)==STATUS_INVALID_PARAMETER);
        CHECK(kmt.device.close(&kmt,7,0x100000001ULL)==STATUS_INVALID_PARAMETER);
        D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
        OKMT_LUID luid; OKMT_HANDLE adapter=0,device=0;
        CHECK(d3dkmt_register_driver(&description,&kmt.device,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
        CHECK(d3dkmt_create_device(7,adapter,0,&device)==0 && device);
        intel_gpu_kmt_device_lease_t operation={0},denied={0};
        CHECK(intel_gpu_kmt_acquire_device(8,device,&denied)==STATUS_INVALID_PARAMETER && !denied.memory);
        CHECK(intel_gpu_kmt_acquire_device(7,device,&operation)==0 && operation.memory==&memory);
        CHECK(intel_gpu_kmt_acquire_device(7,device,&operation)==STATUS_INVALID_PARAMETER);
        intel_gpu_kmt_device_lease_t copied=operation;
        CHECK(intel_gpu_kmt_release_device(&copied)==STATUS_INVALID_PARAMETER);
        uint32_t native=operation.device,resource=0;
        CHECK(native && memory.devices[0].pid==7 && memory.devices[0].references==1);
        CHECK(intel_gpu_memory_release_device(&memory,7,native)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_release_device(&memory,8,native)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_create(&memory,7,native,5000,&resource)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_lease_t lease={0};
        CHECK(intel_gpu_memory_acquire(&memory,7,native,resource,&lease)==INTEL_GPU_BUFFER_OK);
        if (phase&1) { process_closing=true; d3dkmt_release_process(7); }
        else CHECK(d3dkmt_destroy_device(7,device)==0);
        CHECK(!wake_count && ram.freed==0 && memory.devices[0].references==1);
        CHECK(intel_gpu_kmt_acquire_device(7,device,&denied)==STATUS_INVALID_PARAMETER && !denied.memory);
        CHECK(d3dkmt_collect_devices()==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_kmt_release_device(&operation)==0 && !operation.memory && !operation.device);
        CHECK(intel_gpu_kmt_release_device(&operation)==STATUS_INVALID_PARAMETER);
        CHECK(wake_count==1 && ram.freed==0 && memory.pages==2 && memory.devices[0].references==0);
        if (!(phase&1)) {
            OKMT_HANDLE replacement=0;
            CHECK(d3dkmt_create_device(7,adapter,0,&replacement)==0 && replacement!=device);
            CHECK(d3dkmt_destroy_device(7,replacement)==0);
            CHECK(wake_count==2 && memory.pages==2 && !ram.freed);
        }
        CHECK(d3dkmt_destroy_device(7,device)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_memory_release_device(&memory,7,native)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        d3dkmt_release_process(7);
        intel_gpu_memory_shutdown(&memory);
        CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_memory_release(&lease)==INTEL_GPU_BUFFER_OK);
        if (phase&2) ram.fail_release=1;
        if (phase&2) CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_IO && !ram.freed);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK && ram.freed==1 && !memory.pages);
        CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK);
    }
    /* Hold the actual provider after its atomic admission, before KMT sees
     * the returned cookie. An exit collector must retain that empty device. */
    for (unsigned round=0;round<32;round++) {
        res_reset(); process_closing=false; wake_count=0;
        intel_gpu_memory_t memory={0}; intel_gpu_kmt_t kmt={0};
        CHECK(intel_gpu_memory_init(&memory,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_kmt_provider(&kmt,&memory,kmt_provider_notify,&wake_count));
        D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200}; OKMT_LUID luid;
        CHECK(d3dkmt_register_driver(&description,&kmt.device,&luid)==0);
        KMT_RACE race={.device=0xfeed};
        CHECK(d3dkmt_open_adapter(7,luid,&race.adapter)==0);
        open_gate=1; open_entered=open_resume=0;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,kmt_provider_race,&race));
        while (!__atomic_load_n(&open_entered,__ATOMIC_ACQUIRE)) sched_yield();
        pthread_mutex_lock(&admission_lock); process_closing=true; pthread_mutex_unlock(&admission_lock);
        d3dkmt_release_process(7);
        CHECK(memory.devices[0].id && memory.devices[0].closing && memory.devices[0].references==1);
        uint32_t denied=0xfeed;
        CHECK(intel_gpu_memory_open_device_retained(&memory,7,&denied)==INTEL_GPU_BUFFER_INVALID && denied==0xfeed);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_BUSY);
        intel_gpu_memory_shutdown(&memory);
        CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_BUSY);
        CHECK(d3dkmt_unregister_adapter(luid)==D3DKMT_STATUS_DEVICE_BUSY);
        __atomic_store_n(&open_resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL)); open_gate=0;
        CHECK(race.status==STATUS_PROCESS_IS_TERMINATING && race.device==0xfeed);
        CHECK(wake_count==1 && memory.devices[0].id && !memory.devices[0].references);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK && !memory.devices[0].id);
        CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK);
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        CHECK(!ram.allocated && !ram.freed);
    }
    /* External native references have a distinct quota, independent of pages. */
    res_reset(); process_closing=false; wake_count=0;
    intel_gpu_memory_t memory={0}; intel_gpu_kmt_t kmt={0};
    CHECK(intel_gpu_memory_init(&memory,&allocator,&ram,4,4));
    CHECK(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&kmt,&memory,kmt_provider_notify,&wake_count));
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200}; OKMT_LUID luid;
    OKMT_HANDLE adapter=0,handles[INTEL_GPU_MEMORY_DEVICES],extra=0xfeed;
    CHECK(d3dkmt_register_driver(&description,&kmt.device,&luid)==0);
    CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++) CHECK(d3dkmt_create_device(7,adapter,0,&handles[i])==0);
    CHECK(d3dkmt_create_device(7,adapter,0,&extra)==STATUS_INSUFFICIENT_RESOURCES && extra==0xfeed);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++) {
        CHECK(d3dkmt_destroy_device(8,handles[i])==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_device(7,handles[i])==0);
    }
    CHECK(wake_count==INTEL_GPU_MEMORY_DEVICES);
    CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK);
    CHECK(d3dkmt_create_device(7,adapter,0,&extra)==0);
    CHECK(memory.devices[0].references==1 && memory.devices[0].id>INTEL_GPU_MEMORY_DEVICES);
    CHECK(d3dkmt_destroy_device(7,handles[0])==STATUS_INVALID_PARAMETER);
    process_closing=true; d3dkmt_release_process(7);
    CHECK(wake_count==INTEL_GPU_MEMORY_DEVICES+1);
    CHECK(d3dkmt_unregister_adapter(luid)==0);
    intel_gpu_memory_shutdown(&memory);
    CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK);
    res_reset();
    /* A foreign driver's deliberately non-dereferenceable context must be
     * rejected before Intel interprets it. Rejection releases its KMT lease. */
    D3DKMT_DEVICE_PROVIDER other={kmt_other_open,kmt_other_close,(void *)1,NULL,NULL,NULL};
    CHECK(d3dkmt_register_driver(&description,&other,&luid)==0);
    CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
    CHECK(d3dkmt_create_device(7,adapter,0,&extra)==0);
    intel_gpu_kmt_device_lease_t wrong={0};
    CHECK(intel_gpu_kmt_acquire_device(7,extra,&wrong)==STATUS_NOT_SUPPORTED);
    CHECK(!wrong.reference.self && !wrong.memory && !wrong.device && !other_closes);
    CHECK(d3dkmt_destroy_device(7,extra)==0 && other_closes==1);
    CHECK(d3dkmt_close_adapter(7,adapter)==0);
    CHECK(d3dkmt_unregister_adapter(luid)==0);
    unsigned lifetime_checks=checks;
    printf("Intel GPU KMT provider: %u checks passed, 32 forced exit races\n",lifetime_checks);
    /* Both public operation retention and GPU pins survive logical close.
     * Native retained allocations cannot disappear under the KMT cookie. */
    for (unsigned phase=0;phase<4;phase++) {
        res_reset(); process_closing=false; wake_count=0;
        intel_gpu_memory_t m={0}; intel_gpu_kmt_t provider={0};
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_kmt_provider(&provider,&m,kmt_provider_notify,&wake_count));
        CHECK(d3dkmt_register_memory_driver(&description,&provider.device,&provider.allocation,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
        CHECK(d3dkmt_create_device(7,adapter,0,&extra)==0);
        D3DKMT_ALLOCATION_DESCRIPTION desc[2]={{4097},{123}};
        OKMT_HANDLE objects[2]={0,0};
        CHECK(d3dkmt_create_allocations(7,extra,2,desc,objects)==0 && objects[0] && objects[1]);
        CHECK(m.pages==3 && ram.allocated==2 && !ram.freed);
        D3DKMT_ALLOCATION_LEASE operation={0},denied={0};
        CHECK(d3dkmt_acquire_allocation(7,extra,objects[0],&operation)==0);
        CHECK(operation.provider.context==&provider && operation.cookie<=UINT32_MAX);
        uint32_t device=(uint32_t)operation.device_cookie,resource=(uint32_t)operation.cookie;
        intel_gpu_memory_lease_t pin={0};
        CHECK(intel_gpu_memory_acquire(&m,7,device,resource,&pin)==INTEL_GPU_BUFFER_OK);
        CHECK(pin.buffer->bytes==4097 && pin.buffer->pins==1);
        CHECK(intel_gpu_memory_release_resource(&m,7,device,resource)==INTEL_GPU_BUFFER_INVALID);
        if (phase==0) CHECK(d3dkmt_destroy_allocations(7,extra,2,objects)==0);
        if (phase==1) { process_closing=true; d3dkmt_release_process(7); }
        if (phase==2) CHECK(d3dkmt_destroy_device(7,extra)==0);
        if (phase==3) {
            CHECK(d3dkmt_unregister_adapter(luid)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(d3dkmt_acquire_allocation(7,extra,objects[0],&denied)==D3DKMT_STATUS_DEVICE_REMOVED);
            CHECK(d3dkmt_destroy_allocations(7,extra,2,objects)==0);
        }
        CHECK(d3dkmt_acquire_allocation(7,extra,objects[0],&denied)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && ram.freed==1 && m.pages==2);
        CHECK(d3dkmt_collect_allocations()==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(intel_gpu_memory_release_resource(&m,8,device,resource)==INTEL_GPU_BUFFER_INVALID);
        CHECK(d3dkmt_release_allocation(&operation)==0);
        CHECK(intel_gpu_memory_release_resource(&m,7,device,resource)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && ram.freed==1);
        CHECK(intel_gpu_memory_release(&pin)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && ram.freed==2 && !m.pages);
        d3dkmt_release_process(7);
        CHECK(d3dkmt_collect_devices()==0);
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    for (unsigned failure=0;failure<3;failure++) {
        res_reset(); process_closing=false; wake_count=0;
        intel_gpu_memory_t m={0}; intel_gpu_kmt_t provider={0};
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_kmt_provider(&provider,&m,kmt_provider_notify,&wake_count));
        CHECK(d3dkmt_register_memory_driver(&description,&provider.device,&provider.allocation,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
        CHECK(d3dkmt_create_device(7,adapter,0,&extra)==0);
        D3DKMT_ALLOCATION_DESCRIPTION desc[3]={{8192},{8192},{8192}};
        OKMT_HANDLE objects[3]={0xfeed,0xfeed,0xfeed};
        if (failure==1) ram.fail_allocate=2;
        if (failure==2) ram.fail_sync=2;
        NTSTATUS expected=failure==2 ? STATUS_UNSUCCESSFUL : STATUS_NO_MEMORY;
        CHECK(d3dkmt_create_allocations(7,extra,3,desc,objects)==expected);
        CHECK(objects[0]==0xfeed && objects[1]==0xfeed && objects[2]==0xfeed);
        CHECK(m.pages==4 && wake_count>=2);
        for (unsigned i=0;i<2;i++) CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(!m.pages && ram.freed==(failure==1 ? 1u : 2u));
        d3dkmt_release_process(7);
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    /* Force exit after native creation succeeds but before KMT publication.
     * The native collector must keep the cookie valid for rollback. */
    for (unsigned round=0;round<32;round++) {
        res_reset(); process_closing=false; wake_count=0;
        intel_gpu_memory_t m={0}; intel_gpu_kmt_t provider={0};
        CHECK(intel_gpu_memory_init(&m,&allocator,&ram,4,4));
        CHECK(intel_gpu_memory_publish(&m)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_kmt_provider(&provider,&m,kmt_provider_notify,&wake_count));
        native_allocations=provider.allocation;
        D3DKMT_ALLOCATION_PROVIDER gated={kmt_allocation_gated_open,native_allocations.close,
            native_allocations.translate,native_allocations.properties,native_allocations.residency,native_allocations.offer};
        CHECK(d3dkmt_register_memory_driver(&description,&provider.device,&gated,&luid)==0);
        CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
        KMT_ALLOCATION_RACE race={.output=0xfeed};
        CHECK(d3dkmt_create_device(7,adapter,0,&race.device)==0);
        allocation_entered=allocation_resume=0;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,kmt_allocation_race,&race));
        while (!__atomic_load_n(&allocation_entered,__ATOMIC_ACQUIRE)) sched_yield();
        process_closing=true; d3dkmt_release_process(7);
        CHECK(m.pages==2 && m.resources[0].references==1 && m.resources[0].closing);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_BUSY && !ram.freed);
        CHECK(d3dkmt_unregister_adapter(luid)==D3DKMT_STATUS_DEVICE_BUSY);
        __atomic_store_n(&allocation_resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL));
        CHECK(race.status==STATUS_PROCESS_IS_TERMINATING && race.output==0xfeed);
        CHECK(!m.resources[0].references && wake_count==2 && !ram.freed);
        CHECK(intel_gpu_memory_collect(&m)==INTEL_GPU_BUFFER_OK && ram.freed==1 && !m.pages);
        CHECK(d3dkmt_unregister_adapter(luid)==0);
        intel_gpu_memory_shutdown(&m);
        CHECK(intel_gpu_memory_unpublish(&m)==INTEL_GPU_BUFFER_OK);
    }
    res_reset();
    printf("Intel GPU KMT allocations: %u checks passed, 32 forced publication races\n",checks-lifetime_checks);
    unsigned allocation_checks=checks;
    process_closing=false; wake_count=0;
    intel_gpu_memory_t translated_memory={0}; intel_gpu_kmt_t translated_provider={0};
    CHECK(intel_gpu_memory_init(&translated_memory,&allocator,&ram,4,4));
    CHECK(intel_gpu_memory_publish(&translated_memory)==INTEL_GPU_BUFFER_OK);
    CHECK(intel_gpu_kmt_provider(&translated_provider,&translated_memory,kmt_provider_notify,&wake_count));
    CHECK(d3dkmt_register_memory_driver(&description,&translated_provider.device,&translated_provider.allocation,&luid)==0);
    CHECK(d3dkmt_open_adapter(7,luid,&adapter)==0);
    CHECK(d3dkmt_create_device(7,adapter,0,&extra)==0);
    intel_umd_buffer_description_t wire={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,5000,0,0};
    D3DKMT_PRIVATE_ALLOCATION captured={.bytes=sizeof(wire)};
    D3DKMT_ALLOCATION_DESCRIPTION unchanged={0xfeed};
    BYTE unaligned[sizeof(wire)+1]; memcpy(unaligned+1,&wire,sizeof(wire));
    CHECK(translated_provider.allocation.translate(&translated_provider,unaligned+1,sizeof(wire),&unchanged)==0 && unchanged.bytes==5000);
    for (unsigned mode=0;mode<8;mode++) {
        intel_umd_buffer_description_t malformed=wire;
        captured.bytes=sizeof(wire);
        if (mode==0) captured.bytes--;
        if (mode==1) malformed.magic++;
        if (mode==2) malformed.version++;
        if (mode==3) malformed.flags=1;
        if (mode==4) malformed.reserved=1;
        if (mode==5) malformed.bytes=0;
        if (mode==6) malformed.bytes=INTEL_GPU_BUFFER_MAX_BYTES+1;
        if (mode==7) malformed.bytes=UINT64_MAX;
        memcpy(captured.data,&malformed,sizeof(malformed));
        OKMT_HANDLE rejected=0xfeed;
        CHECK(d3dkmt_create_private_allocations(7,extra,1,&captured,&rejected)==
            (mode<3 ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER));
        CHECK(rejected==0xfeed && !translated_memory.pages && !ram.allocated);
    }
    captured.bytes=sizeof(wire); memcpy(captured.data,&wire,sizeof(wire));
    OKMT_HANDLE translated=0;
    CHECK(d3dkmt_create_private_allocations(7,extra,1,&captured,&translated)==0 && translated && translated_memory.pages==2);
    CHECK(d3dkmt_destroy_allocations(7,extra,1,&translated)==0);
    CHECK(intel_gpu_memory_collect(&translated_memory)==INTEL_GPU_BUFFER_OK && !translated_memory.pages && ram.freed==1);
    d3dkmt_release_process(7);
    CHECK(d3dkmt_unregister_adapter(luid)==0);
    intel_gpu_memory_shutdown(&translated_memory);
    CHECK(intel_gpu_memory_unpublish(&translated_memory)==INTEL_GPU_BUFFER_OK);
    res_reset();
    printf("Intel GPU KMT private ABI: %u checks passed\n",checks-allocation_checks);
    unsigned before_cpu=checks;
    kmt_cpu_cases();
    printf("Intel GPU KMT CPU ownership: %u checks passed\n",checks-before_cpu);
    return 0;
}
