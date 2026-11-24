/* Registry transactions use synthetic provider cookies, no graphics emulation. */
#define main adapter_suite_main
#include "d3dkmt_registry_host.c"
#undef main
#include <sched.h>

static struct {
    pthread_mutex_t lock;
    struct { DWORD pid,allocations; uint64_t cookie; } devices[16];
    struct { DWORD pid; uint64_t device,cookie; } allocations[D3DKMT_MAX_HANDLES];
    unsigned live,opens,closes,serial,fail_at,gate,entered,resume;
    uint64_t fail_close;
    OKMT_LUID luid;
} mem={.lock=PTHREAD_MUTEX_INITIALIZER};
static unsigned api_hook;
static uintptr_t api_last_output;

static NTSTATUS allocation_device_open(void *context,DWORD pid,uint64_t *cookie)
{
    CHECK(context==&mem);
    pthread_mutex_lock(&mem.lock);
    NTSTATUS status=STATUS_NO_MEMORY;
    for (unsigned i=0;i<16;i++) if (!mem.devices[i].pid) {
        mem.devices[i].pid=pid; *cookie=mem.devices[i].cookie=++mem.serial;
        status=0; break;
    }
    pthread_mutex_unlock(&mem.lock); return status;
}
static NTSTATUS allocation_device_close(void *context,DWORD pid,uint64_t cookie)
{
    CHECK(context==&mem);
    pthread_mutex_lock(&mem.lock);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<16;i++) if (mem.devices[i].pid==pid && mem.devices[i].cookie==cookie) {
        CHECK(!mem.devices[i].allocations);
        mem.devices[i].pid=0; status=0; break;
    }
    pthread_mutex_unlock(&mem.lock); return status;
}
static NTSTATUS allocation_open(void *context,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *desc,uint64_t *cookie)
{
    CHECK(context==&mem && desc->bytes>0);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    pthread_mutex_lock(&mem.lock);
    NTSTATUS status=STATUS_NO_MEMORY;
    mem.opens++;
    for (unsigned d=0;d<16;d++) if (mem.devices[d].pid==pid && mem.devices[d].cookie==device) {
        if (mem.opens==mem.fail_at) break;
        for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) if (!mem.allocations[i].pid) {
            mem.allocations[i].pid=pid; mem.allocations[i].device=device;
            *cookie=mem.allocations[i].cookie=++mem.serial;
            mem.devices[d].allocations++; mem.live++; status=0; break;
        }
        break;
    }
    pthread_mutex_unlock(&mem.lock);
    if (api_hook) {
        CHECK(d3dkmt_deferred_current() && d3dkmt_deferred_for_kernel_pid((int)caller));
        if (api_hook==2) { denied_first=api_last_output; denied_last=denied_first+1; }
        if (api_hook==3) cancellation_requested=TRUE;
    }
    if (__atomic_load_n(&mem.gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&mem.entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&mem.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}
static NTSTATUS allocation_close(void *context,DWORD pid,uint64_t device,uint64_t cookie)
{
    CHECK(context==&mem);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    pthread_mutex_lock(&mem.lock);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    mem.closes++;
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) if (mem.allocations[i].pid==pid &&
        mem.allocations[i].device==device && mem.allocations[i].cookie==cookie) {
        status=STATUS_NO_MEMORY;
        if (mem.fail_close==cookie || mem.fail_close==UINT64_MAX) break;
        for (unsigned d=0;d<16;d++) if (mem.devices[d].pid==pid && mem.devices[d].cookie==device) {
            CHECK(mem.devices[d].allocations); mem.devices[d].allocations--;
        }
        mem.allocations[i].pid=0; mem.live--; status=0; break;
    }
    pthread_mutex_unlock(&mem.lock);
    CHECK(status!=STATUS_INVALID_PARAMETER); return status;
}
typedef struct { OKMT_HANDLE device,output[3]; NTSTATUS result; } ALLOCATION_RACE;
static void *allocation_race(void *context)
{
    ALLOCATION_RACE *race=context;
    D3DKMT_ALLOCATION_DESCRIPTION desc[3]={{4096},{8192},{123}};
    race->result=d3dkmt_create_allocations(12,race->device,3,desc,race->output);
    return NULL;
}

static NTSTATUS allocation_decode(void *context,const void *data,DWORD bytes,D3DKMT_ALLOCATION_DESCRIPTION *desc)
{
    CHECK(context==&mem);
    uint64_t size=0;
    if (bytes!=sizeof(size)) return STATUS_NOT_SUPPORTED;
    memcpy(&size,data,sizeof(size));
    if (!size || size>1024*1024) return STATUS_INVALID_PARAMETER;
    desc->bytes=size; return 0;
}

static void allocation_abi_cases(void)
{
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    D3DKMT_DEVICE_PROVIDER device_provider={allocation_device_open,allocation_device_close,&mem,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_PROVIDER provider={allocation_open,allocation_close,allocation_decode,NULL,NULL,NULL};
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    OKMT_HANDLE adapter=0,device=0,other=0;
    CHECK(d3dkmt_open_adapter(caller,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&device)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&other)==0);
    user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    if (user==MAP_FAILED) return;
    for (unsigned compat=0;compat<2;compat++) for (unsigned modern=0;modern<2;modern++) {
        g_compat32_mode=(int)compat;
        NTSTATUS (WINAPI *create)(PVOID)=modern ? D3DKMTCreateAllocation2 : D3DKMTCreateAllocation;
        NTSTATUS (WINAPI *destroy)(PCVOID)=modern ? D3DKMTDestroyAllocation2 : D3DKMTDestroyAllocation;
        SIZE_T stride=modern ? (compat ? 56 : 96) : (compat ? 24 : 40);
        for (unsigned mode=0;mode<15;mode++) {
            memset(user,0xa5,4096); memset(user,0,compat ? 44 : 72);
            memset(user+512,0,stride*3);
            const uint64_t sizes[3]={4096,8192,123};
            for (unsigned i=0;i<3;i++) {
                memcpy(user+2000+i*8,&sizes[i],8);
                BYTE *entry=user+512+i*stride;
                if (compat) {
                    DWORD *p=(DWORD *)entry;
                    p[0]=0xfeed; p[2]=(DWORD)(uintptr_t)(user+2000+i*8); p[3]=8;
                } else {
                    OKMT_ALLOCATION_INFO *p=(void *)entry;
                    p->allocation=0xfeed; p->private_data=user+2000+i*8; p->private_bytes=8;
                }
                if (modern) memset(entry+(compat ? 24 : 40),0xa5,8);
            }
            if (compat) {
                DWORD *p=(DWORD *)user; p[0]=device; p[2]=0xfeed; p[7]=3; p[8]=(DWORD)(uintptr_t)(user+512);
            } else {
                OKMT_CREATE_ALLOCATION *p=(void *)user;
                p->device=device; p->global_share=0xfeed; p->count=3; p->allocations=user+512;
            }
            BYTE *last=user+512+2*stride;
            DWORD *private_size=compat ? (DWORD *)(last+12) : (DWORD *)(last+24);
            DWORD *info_flags=compat ? (DWORD *)(last+20) : (DWORD *)(last+32);
            DWORD *count=compat ? (DWORD *)(user+28) : (DWORD *)(user+44);
            DWORD *options=compat ? (DWORD *)(user+36) : (DWORD *)(user+56);
            NTSTATUS expected=STATUS_INVALID_PARAMETER;
            api_hook=1; unsigned opens=mem.opens;
            if (mode==0) expected=0;
            if (mode==1) {
                if (compat) *(DWORD *)(last+8)=(DWORD)(uintptr_t)(user+4093);
                else ((OKMT_ALLOCATION_INFO *)last)->private_data=user+4093;
            }
            if (mode==2) *private_size=0;
            if (mode==3) *count=65;
            if (mode==4) { denied_first=(uintptr_t)(last+stride-1); denied_last=denied_first+1; }
            if (mode==5) { mem.fail_at=mem.opens+2; expected=STATUS_NO_MEMORY; }
            if (mode==6) { cancellation_requested=TRUE; expected=STATUS_PROCESS_IS_TERMINATING; }
            if (mode==7) { api_hook=3; expected=STATUS_PROCESS_IS_TERMINATING; }
            if (mode==8) { api_hook=2; api_last_output=(uintptr_t)(last+stride-1); }
            if (mode==9) memset(user+2016,0,8);
            if (mode==10) { *options=2; expected=STATUS_NOT_SUPPORTED; }
            if (mode==11) { *info_flags=1; expected=STATUS_NOT_SUPPORTED; }
            if (mode==12) {
                if (modern) *(DWORD *)(last+(compat ? 36 : 56))=1;
                else { *(DWORD *)(last+(compat ? 4 : 8))=1; expected=STATUS_NOT_SUPPORTED; }
            }
            if (mode==13) {
                if (compat) ((DWORD *)user)[8]=(DWORD)(uintptr_t)user;
                else ((OKMT_CREATE_ALLOCATION *)user)->allocations=user;
            }
            if (mode==14) *options=0x80000000u;
            BYTE saved[4096]; memcpy(saved,user,4096);
            unsigned deliveries=cancellation_deliveries;
            CHECK(create(user)==expected);
            CHECK(!d3dkmt_deferred_current() && !d3dkmt_deferred_for_kernel_pid((int)caller));
            if (mode==6 || mode==7) CHECK(cancellation_deliveries==deliveries+1);
            cancellation_requested=FALSE; denied_first=denied_last=0; mem.fail_at=0; api_hook=0;
            if (expected==0) {
                OKMT_HANDLE handles[3];
                for (unsigned i=0;i<3;i++) {
                    memcpy(&handles[i],user+512+i*stride,4);
                    CHECK(handles[i] && handles[i]!=0xfeed);
                    memcpy(saved+512+i*stride,&handles[i],4);
                    if (modern) memset(saved+512+i*stride+(compat ? 24 : 40),0,8);
                }
                memset(saved+8,0,4);
                CHECK(!memcmp(saved,user,4096) && mem.live==3);
                memcpy(user+1800,handles,sizeof(handles));
                memset(user+128,0,24);
                if (compat) {
                    DWORD *p=(DWORD *)(user+128); p[0]=device; p[2]=(DWORD)(uintptr_t)(user+1800); p[3]=3;
                } else {
                    OKMT_DESTROY_ALLOCATION2 *p=(void *)(user+128);
                    p->device=device; p->allocations=(void *)(user+1800); p->count=3;
                }
                *(DWORD *)(user+1804)=handles[0];
                CHECK(destroy(user+128)==STATUS_INVALID_PARAMETER && mem.live==3);
                *(DWORD *)(user+1804)=handles[1];
                *(DWORD *)(user+128)=other;
                CHECK(destroy(user+128)==STATUS_INVALID_PARAMETER && mem.live==3);
                *(DWORD *)(user+128)=device;
                if (modern) {
                    *(DWORD *)(user+128+(compat ? 16 : 20))=2;
                    CHECK(destroy(user+128)==STATUS_NOT_SUPPORTED && mem.live==3);
                    *(DWORD *)(user+128+(compat ? 16 : 20))=0;
                } else if (!compat) memset(user+148,0xa5,4); /* legacy padding */
                memcpy(saved,user,4096);
                CHECK(destroy(user+128)==0 && mem.live==0 && !memcmp(saved,user,4096));
                CHECK(destroy(user+128)==STATUS_INVALID_PARAMETER);
            } else {
                CHECK(!mem.live && !memcmp(saved,user,4096));
                if (mode!=5 && mode!=7 && mode!=8) CHECK(mem.opens==opens);
            }
        }
        CHECK(create(NULL)==STATUS_INVALID_PARAMETER && destroy(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(create(user+4090)==STATUS_INVALID_PARAMETER && destroy(user+4090)==STATUS_INVALID_PARAMETER);
    }
    CHECK(munmap(user,4096)==0); user=NULL; user_bytes=0;
    d3dkmt_release_process(caller);
    CHECK(d3dkmt_unregister_adapter(mem.luid)==0);
    CHECK(!d3dkmt_deferred_for_kernel_pid(0));
}

typedef struct {
    OKMT_HANDLE device,resource,result_resource,output[3];
    BOOL create;
    NTSTATUS result;
} RESOURCE_RACE;
static void *allocation_resource_race(void *context)
{
    RESOURCE_RACE *race=context;
    D3DKMT_ALLOCATION_DESCRIPTION input[3]={{4096},{8192},{123}};
    race->result=d3dkmt_create_resource_allocations(12,race->device,race->resource,
        race->create,3,input,&race->result_resource,race->output);
    return NULL;
}

static void allocation_resource_cases(void)
{
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    D3DKMT_DEVICE_PROVIDER device_provider={allocation_device_open,allocation_device_close,&mem,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_PROVIDER provider={allocation_open,allocation_close,allocation_decode,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_DESCRIPTION input[64];
    for (unsigned i=0;i<64;i++) input[i].bytes=4096+i;
    OKMT_HANDLE adapter=0,device=0,other=0,group=0,second=0,result=0xfeed,output[64];
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&other)==0);
    unsigned opens=mem.opens;
    CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,0,NULL,&group,NULL)==0 && group);
    CHECK(mem.opens==opens);
    CHECK(d3dkmt_close_adapter(12,group)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_device(12,group)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_resource(13,device,group)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_resource(12,other,group)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_resource(12,device,device)==STATUS_INVALID_PARAMETER);
    D3DKMT_ALLOCATION_LEASE denied={0};
    CHECK(d3dkmt_acquire_allocation(12,device,group,&denied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_resource_allocations(12,device,group,TRUE,1,input,&result,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_resource_allocations(12,device,group,FALSE,0,NULL,&result,NULL)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_resource_allocations(12,other,group,FALSE,1,input,&result,output)==STATUS_INVALID_PARAMETER);
    CHECK(result==0xfeed && mem.opens==opens);
    CHECK(d3dkmt_create_resource_allocations(12,device,group,FALSE,3,input,&result,output)==0 && result==group);
    OKMT_HANDLE members[3]; memcpy(members,output,sizeof(members));
    CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,1,input,&second,output)==0);
    OKMT_HANDLE mixed[2]={members[0],output[0]};
    CHECK(d3dkmt_destroy_allocations(12,device,2,mixed)==STATUS_INVALID_PARAMETER && mem.live==4);
    CHECK(d3dkmt_create_allocations(12,device,1,input,output)==0);
    mixed[1]=output[0];
    CHECK(d3dkmt_destroy_allocations(12,device,2,mixed)==STATUS_INVALID_PARAMETER && mem.live==5);
    mixed[0]=output[0]; mixed[1]=members[0];
    CHECK(d3dkmt_destroy_allocations(12,device,2,mixed)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_allocations(12,device,1,output)==0);
    CHECK(d3dkmt_destroy_resource(12,device,second)==0 && mem.live==3);
    CHECK(d3dkmt_destroy_allocations(12,device,1,members)==0 && mem.live==2);
    for (unsigned i=0;i<3;i++) output[i]=0xfeed;
    result=0xfeed; mem.fail_at=mem.opens+2;
    CHECK(d3dkmt_create_resource_allocations(12,device,group,FALSE,3,input,&result,output)==STATUS_NO_MEMORY);
    CHECK(result==0xfeed && output[0]==0xfeed && output[1]==0xfeed && output[2]==0xfeed && mem.live==2);
    mem.fail_at=0;
    D3DKMT_ALLOCATION_LEASE pinned={0};
    CHECK(d3dkmt_acquire_allocation(12,device,members[1],&pinned)==0);
    CHECK(d3dkmt_destroy_resource(12,device,group)==0 && mem.live==1);
    CHECK(d3dkmt_destroy_resource(12,device,group)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_resource_allocations(12,device,group,FALSE,1,input,&result,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_allocation(12,device,members[1],&denied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_device(12,device)==0 && mem.live==1);
    CHECK(d3dkmt_unregister_adapter(mem.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_release_allocation(&pinned)==0 && !mem.live);
    d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);

    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,64,input,&group,output)==0);
    CHECK(d3dkmt_create_resource_allocations(12,device,group,FALSE,64,input,&result,output)==0 && mem.live==128);
    /* Group destruction is not limited to a single 64-entry creation batch. */
    CHECK(d3dkmt_destroy_resource(12,device,group)==0 && !mem.live);
    for (unsigned round=0;round<128;round++) {
        CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,0,NULL,&second,NULL)==0 && second!=group);
        CHECK(d3dkmt_destroy_resource(12,device,group)==STATUS_INVALID_PARAMETER);
        group=second;
        CHECK(d3dkmt_destroy_resource(12,device,group)==0);
    }
    /* Rollback of a new group survives failed driver cleanup without making
     * the unpublished group accessible or leaking its namespace permanently. */
    mem.fail_at=mem.opens+3; mem.fail_close=UINT64_MAX; result=0xfeed;
    output[0]=output[1]=output[2]=0xfeed;
    CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,3,input,&result,output)==STATUS_NO_MEMORY);
    CHECK(result==0xfeed && output[0]==0xfeed && output[1]==0xfeed && output[2]==0xfeed && mem.live==2);
    CHECK(d3dkmt_collect_allocations()==STATUS_NO_MEMORY);
    mem.fail_at=0; mem.fail_close=0;
    CHECK(d3dkmt_collect_allocations()==0 && d3dkmt_collect_allocations()==0 && !mem.live);
    /* Empty resources must retain their device until process/device teardown,
     * and the last free slot cannot fit a new group plus one allocation. */
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES-3;i++)
        CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,0,NULL,&group,NULL)==0);
    opens=mem.opens;
    CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,1,input,&result,output)==STATUS_NO_MEMORY);
    CHECK(mem.opens==opens && result==0xfeed);
    d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);

    for (unsigned round=0;round<64;round++) {
        CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
        CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
        BOOL append=(round&1)!=0; unsigned action=(round/2)%4;
        group=0;
        if (append) CHECK(d3dkmt_create_resource_allocations(12,device,0,TRUE,1,input,&group,output)==0);
        RESOURCE_RACE race={.device=device,.resource=group,.create=!append,
            .result_resource=0xfeed,.output={0xfeed,0xfeed,0xfeed}};
        mem.gate=1; mem.entered=mem.resume=0;
        pthread_t thread; CHECK(pthread_create(&thread,NULL,allocation_resource_race,&race)==0);
        while (!__atomic_load_n(&mem.entered,__ATOMIC_ACQUIRE)) sched_yield();
        result=0xfeed;
        if (append) CHECK(d3dkmt_create_resource_allocations(12,device,group,FALSE,1,input,&result,output)==D3DKMT_STATUS_DEVICE_BUSY && result==0xfeed);
        NTSTATUS expected;
        if (action==0) { CHECK(d3dkmt_destroy_device(12,device)==0); expected=STATUS_PROCESS_IS_TERMINATING; }
        else if (action==1) { d3dkmt_release_process(12); expected=STATUS_PROCESS_IS_TERMINATING; }
        else if (action==2) { CHECK(d3dkmt_unregister_adapter(mem.luid)==D3DKMT_STATUS_DEVICE_BUSY); expected=D3DKMT_STATUS_DEVICE_REMOVED; }
        else if (append) { CHECK(d3dkmt_destroy_resource(12,device,group)==0); expected=STATUS_INVALID_PARAMETER; }
        else { mem.fail_at=mem.opens+1; expected=STATUS_NO_MEMORY; }
        __atomic_store_n(&mem.resume,1,__ATOMIC_RELEASE);
        CHECK(pthread_join(thread,NULL)==0); mem.gate=0; mem.fail_at=0;
        CHECK(race.result==expected && race.result_resource==0xfeed);
        CHECK(race.output[0]==0xfeed && race.output[1]==0xfeed && race.output[2]==0xfeed);
        d3dkmt_release_process(12); CHECK(!mem.live && d3dkmt_unregister_adapter(mem.luid)==0);
    }
    CHECK(d3dkmt_collect_devices()==0 && d3dkmt_collect_allocations()==0);
}

static void allocation_resource_abi_cases(void)
{
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    D3DKMT_DEVICE_PROVIDER device_provider={allocation_device_open,allocation_device_close,&mem,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_PROVIDER provider={allocation_open,allocation_close,allocation_decode,NULL,NULL,NULL};
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    OKMT_HANDLE adapter=0,device=0;
    CHECK(d3dkmt_open_adapter(caller,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&device)==0);
    user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    if (user==MAP_FAILED) return;
    for (unsigned compat=0;compat<2;compat++) for (unsigned modern=0;modern<2;modern++) {
        g_compat32_mode=(int)compat;
        NTSTATUS (WINAPI *create)(PVOID)=modern ? D3DKMTCreateAllocation2 : D3DKMTCreateAllocation;
        NTSTATUS (WINAPI *destroy)(PCVOID)=modern ? D3DKMTDestroyAllocation2 : D3DKMTDestroyAllocation;
        SIZE_T stride=modern ? (compat ? 56 : 96) : (compat ? 24 : 40);
        memset(user,0,4096);
        if (compat) { DWORD *h=(void *)user; h[0]=device; h[9]=1; }
        else { OKMT_CREATE_ALLOCATION *h=(void *)user; h->device=device; h->flags=1; }
        unsigned opens=mem.opens;
        denied_first=(uintptr_t)(user+4); denied_last=denied_first+1;
        CHECK(create(user)==STATUS_INVALID_PARAMETER && mem.opens==opens);
        denied_first=denied_last=0;
        CHECK(create(user)==0 && mem.opens==opens);
        OKMT_HANDLE group=*(DWORD *)(user+4),first=0;
        CHECK(group && !*(DWORD *)(user+8));
        /* Append one member to the empty resource. Later rollback must never
         * destroy this pre-existing member or its resource handle. */
        uint64_t size=4096; memcpy(user+2000,&size,8);
        if (compat) {
            DWORD *h=(void *)user; h[7]=1; h[8]=(DWORD)(uintptr_t)(user+512); h[9]=0;
            DWORD *i=(void *)(user+512); i[2]=(DWORD)(uintptr_t)(user+2000); i[3]=8;
        } else {
            OKMT_CREATE_ALLOCATION *h=(void *)user; h->count=1; h->allocations=user+512; h->flags=0;
            OKMT_ALLOCATION_INFO *i=(void *)(user+512); i->private_data=user+2000; i->private_bytes=8;
        }
        CHECK(create(user)==0 && mem.live==1 && *(DWORD *)(user+4)==group);
        first=*(DWORD *)(user+512);
        for (unsigned new_group=0;new_group<2;new_group++) for (unsigned mode=0;mode<7;mode++) {
            memset(user,0xa5,4096); memset(user,0,compat ? 44 : 72);
            memset(user+512,0,stride*2);
            for (unsigned i=0;i<2;i++) {
                size=4096+i; memcpy(user+2000+i*8,&size,8);
                if (compat) {
                    DWORD *entry=(void *)(user+512+i*stride);
                    entry[0]=0xfeed; entry[2]=(DWORD)(uintptr_t)(user+2000+i*8); entry[3]=8;
                } else {
                    OKMT_ALLOCATION_INFO *entry=(void *)(user+512+i*stride);
                    entry->allocation=0xfeed; entry->private_data=user+2000+i*8; entry->private_bytes=8;
                }
                if (modern) memset(user+512+i*stride+(compat ? 24 : 40),0xa5,8);
            }
            if (compat) {
                DWORD *h=(void *)user; h[0]=device; h[1]=new_group ? 0 : group; h[2]=0xfeed;
                h[7]=2; h[8]=(DWORD)(uintptr_t)(user+512); h[9]=new_group;
            } else {
                OKMT_CREATE_ALLOCATION *h=(void *)user; h->device=device; h->resource=new_group ? 0 : group;
                h->global_share=0xfeed; h->count=2; h->allocations=user+512; h->flags=new_group;
            }
            NTSTATUS expected=STATUS_INVALID_PARAMETER;
            if (mode==0) expected=0;
            if (mode==1) memset(user+2008,0,8);
            if (mode==2) { mem.fail_at=mem.opens+2; expected=STATUS_NO_MEMORY; }
            if (mode==3) { api_hook=2; api_last_output=(uintptr_t)(user+512+2*stride-1); }
            if (mode==4) { api_hook=3; expected=STATUS_PROCESS_IS_TERMINATING; }
            if (mode==5) { denied_first=(uintptr_t)(user+4); denied_last=denied_first+1; }
            if (mode==6) { api_hook=2; api_last_output=(uintptr_t)(user+4); mem.fail_close=UINT64_MAX; }
            BYTE saved[4096]; memcpy(saved,user,4096); opens=mem.opens;
            CHECK(create(user)==expected);
            CHECK(!d3dkmt_deferred_current());
            api_hook=0; cancellation_requested=FALSE; denied_first=denied_last=0; mem.fail_at=0;
            if (expected==0) {
                OKMT_HANDLE handles[2],published=*(DWORD *)(user+4);
                CHECK(published && (new_group ? published!=group : published==group));
                memcpy(saved+4,&published,4); memset(saved+8,0,4);
                for (unsigned i=0;i<2;i++) {
                    handles[i]=*(DWORD *)(user+512+i*stride);
                    CHECK(handles[i] && handles[i]!=0xfeed);
                    memcpy(saved+512+i*stride,&handles[i],4);
                    if (modern) memset(saved+512+i*stride+(compat ? 24 : 40),0,8);
                }
                CHECK(!memcmp(saved,user,4096) && mem.live==3);
                memset(user+128,0,24); memcpy(user+1800,handles,sizeof(handles));
                if (compat) {
                    DWORD *h=(void *)(user+128); h[0]=device; h[1]=new_group ? published : 0;
                    h[2]=new_group ? 0 : (DWORD)(uintptr_t)(user+1800); h[3]=new_group ? UINT32_MAX : 2;
                } else {
                    OKMT_DESTROY_ALLOCATION2 *h=(void *)(user+128); h->device=device; h->resource=new_group ? published : 0;
                    h->allocations=new_group ? NULL : (void *)(user+1800); h->count=new_group ? UINT32_MAX : 2;
                }
                CHECK(destroy(user+128)==0 && mem.live==1);
                CHECK(destroy(user+128)==STATUS_INVALID_PARAMETER);
            } else {
                CHECK(!memcmp(saved,user,4096));
                if (mode==1 || mode==5) CHECK(opens==mem.opens);
                if (mode==6) {
                    CHECK(mem.live==3 && d3dkmt_collect_allocations()==STATUS_NO_MEMORY);
                    mem.fail_close=0;
                    CHECK(d3dkmt_collect_allocations()==0 && d3dkmt_collect_allocations()==0);
                }
                CHECK(mem.live==1);
            }
            D3DKMT_ALLOCATION_LEASE lease={0};
            CHECK(d3dkmt_acquire_allocation(caller,device,first,&lease)==0);
            CHECK(d3dkmt_release_allocation(&lease)==0);
        }
        memset(user+128,0,24);
        if (compat) {
            DWORD *h=(void *)(user+128); h[0]=device; h[1]=group; h[2]=1; h[3]=UINT32_MAX;
        } else {
            OKMT_DESTROY_ALLOCATION2 *h=(void *)(user+128);
            h->device=device; h->resource=group; h->allocations=(void *)1; h->count=UINT32_MAX;
        }
        CHECK(destroy(user+128)==STATUS_INVALID_PARAMETER && mem.live==1);
        if (compat) ((DWORD *)(user+128))[2]=0;
        else ((OKMT_DESTROY_ALLOCATION2 *)(user+128))->allocations=NULL;
        CHECK(destroy(user+128)==0 && !mem.live);
    }
    CHECK(munmap(user,4096)==0); user=NULL; user_bytes=0;
    d3dkmt_release_process(caller); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);
}

int main(void)
{
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=3200};
    D3DKMT_DEVICE_PROVIDER device_provider={allocation_device_open,allocation_device_close,&mem,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_PROVIDER provider={allocation_open,allocation_close,NULL,NULL,NULL,NULL};
    D3DKMT_ALLOCATION_DESCRIPTION input[3]={{4096},{8192},{123}};
    OKMT_HANDLE adapter=0,device=0,other=0,output[3]={0xfeed,0xfeed,0xfeed};
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,NULL,&mem.luid)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_register_driver(&description,&device_provider,&mem.luid)==0);
    CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    CHECK(d3dkmt_create_allocations(12,device,3,input,output)==STATUS_NOT_SUPPORTED && output[0]==0xfeed);
    d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&other)==0);
    unsigned opened=mem.opens;
    CHECK(d3dkmt_create_allocations(13,device,3,input,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_allocations(12,adapter,3,input,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_allocations(12,device,0,input,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_allocations(12,device,65,input,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_allocations(12,device,1,NULL,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_create_allocations(12,device,1,input,NULL)==STATUS_INVALID_PARAMETER);
    input[1].bytes=0;
    CHECK(d3dkmt_create_allocations(12,device,3,input,output)==STATUS_INVALID_PARAMETER && mem.opens==opened);
    input[1].bytes=8192;
    for (unsigned fail=1;fail<=3;fail++) {
        mem.fail_at=mem.opens+fail;
        CHECK(d3dkmt_create_allocations(12,device,3,input,output)==STATUS_NO_MEMORY);
        CHECK(!mem.live && output[0]==0xfeed && output[1]==0xfeed && output[2]==0xfeed);
    }
    mem.fail_at=0;
    CHECK(d3dkmt_create_allocations(12,device,3,input,output)==0 && mem.live==3);
    D3DKMT_ALLOCATION_LEASE first={0},second={0},denied={0};
    CHECK(d3dkmt_acquire_allocation(13,device,output[0],&denied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_allocation(12,other,output[0],&denied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_allocation(12,device,device,&denied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_allocation(12,device,output[0],NULL)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_device(12,output[0])==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_close_adapter(12,output[0])==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_allocation(12,device,output[0],&first)==0 && first.cookie && first.device_cookie);
    CHECK(d3dkmt_acquire_allocation(12,device,output[0],&first)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_acquire_allocation(12,device,output[0],&second)==0 && first.cookie==second.cookie);
    D3DKMT_ALLOCATION_LEASE moved=first;
    CHECK(d3dkmt_release_allocation(&moved)==STATUS_INVALID_PARAMETER);
    OKMT_HANDLE bad[2]={output[0],output[0]};
    CHECK(d3dkmt_destroy_allocations(12,device,2,bad)==STATUS_INVALID_PARAMETER);
    bad[1]=other;
    CHECK(d3dkmt_destroy_allocations(12,device,2,bad)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_allocations(12,other,3,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_allocations(13,device,3,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_allocations(12,device,0,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_allocations(12,device,3,output)==0 && mem.live==1);
    CHECK(d3dkmt_acquire_allocation(12,device,output[0],&denied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_allocations(12,device,3,output)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_collect_allocations()==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_release_allocation(&first)==0 && !first.self && mem.live==1);
    CHECK(d3dkmt_release_allocation(&first)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_destroy_device(12,device)==0);
    CHECK(d3dkmt_unregister_adapter(mem.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_release_allocation(&second)==0 && !mem.live);
    CHECK(d3dkmt_destroy_device(12,other)==0);
    CHECK(d3dkmt_close_adapter(12,adapter)==0);
    CHECK(d3dkmt_unregister_adapter(mem.luid)==0);

    /* Fail cleanup as well as a middle allocation: ownership must survive
     * without publishing any of the completed allocations. */
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    mem.fail_at=mem.opens+3; mem.fail_close=UINT64_MAX;
    output[0]=output[1]=output[2]=0xfeed;
    CHECK(d3dkmt_create_allocations(12,device,3,input,output)==STATUS_NO_MEMORY && mem.live==2);
    CHECK(output[0]==0xfeed && output[1]==0xfeed && output[2]==0xfeed);
    CHECK(d3dkmt_collect_allocations()==STATUS_NO_MEMORY);
    uint64_t stuck=mem.allocations[0].cookie;
    mem.fail_close=stuck;
    (void)d3dkmt_collect_allocations(); (void)d3dkmt_collect_allocations();
    CHECK(mem.live==1); mem.fail_close=0; mem.fail_at=0;
    CHECK(d3dkmt_collect_allocations()==0 && !mem.live);
    d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);

    /* Closing the device, exiting, or removing the adapter while the provider
     * is outside the registry lock cannot publish even the first child. */
    for (unsigned round=0;round<48;round++) {
        CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
        CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
        ALLOCATION_RACE race={.device=device,.output={0xfeed,0xfeed,0xfeed}};
        mem.gate=1; mem.entered=mem.resume=0;
        pthread_t thread; CHECK(pthread_create(&thread,NULL,allocation_race,&race)==0);
        while (!__atomic_load_n(&mem.entered,__ATOMIC_ACQUIRE)) sched_yield();
        if (round%3==0) CHECK(d3dkmt_destroy_device(12,device)==0);
        else if (round%3==1) d3dkmt_release_process(12);
        CHECK(d3dkmt_unregister_adapter(mem.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(mem.live==1);
        __atomic_store_n(&mem.resume,1,__ATOMIC_RELEASE);
        CHECK(pthread_join(thread,NULL)==0); mem.gate=0;
        CHECK(race.result==(round%3==2 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_PROCESS_IS_TERMINATING));
        CHECK(race.output[0]==0xfeed && race.output[1]==0xfeed && race.output[2]==0xfeed && !mem.live);
        d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);
    }
    /* Capacity is reserved before the first callback; caller buffers remain
     * unchanged and allocating an empty batch cannot consume namespace. */
    CHECK(d3dkmt_register_memory_driver(&description,&device_provider,&provider,&mem.luid)==0);
    CHECK(d3dkmt_open_adapter(12,mem.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES-4;i++) CHECK(d3dkmt_open_adapter(13,mem.luid,&other)==0);
    opened=mem.opens;
    CHECK(d3dkmt_create_allocations(12,device,3,input,output)==STATUS_NO_MEMORY && mem.opens==opened);
    CHECK(output[0]==0xfeed && output[1]==0xfeed && output[2]==0xfeed);
    d3dkmt_release_process(13);
    for (unsigned round=0;round<256;round++) {
        CHECK(d3dkmt_create_allocations(12,device,3,input,output)==0);
        OKMT_HANDLE stale=output[0];
        CHECK(d3dkmt_destroy_allocations(12,device,3,output)==0 && !mem.live);
        CHECK(d3dkmt_acquire_allocation(12,device,stale,&denied)==STATUS_INVALID_PARAMETER);
    }
    d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(mem.luid)==0);
    CHECK(d3dkmt_release_allocation(NULL)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_collect_devices()==0 && d3dkmt_collect_allocations()==0);
    unsigned previous=checks;
    allocation_resource_cases();
    printf("D3DKMT resource groups checks=%u failures=%u, 64 forced publication races\n",checks-previous,failures);
    previous=checks;
    allocation_abi_cases();
    printf("D3DKMT allocation ABI checks=%u failures=%u\n",checks-previous,failures);
    previous=checks;
    allocation_resource_abi_cases();
    printf("D3DKMT resource ABI checks=%u failures=%u\n",checks-previous,failures);
    printf("D3DKMT allocation checks=%u failures=%u live=%u, 48 forced publication races\n",checks,failures,mem.live);
    return failures ? 1 : 0;
}
