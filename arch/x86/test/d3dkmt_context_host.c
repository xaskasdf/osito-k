/* Context lifetime/ABI with modeled providers; no hardware capability claim. */
#define main adapter_suite_main
#include "d3dkmt_registry_host.c"
#undef main
#include <sched.h>

static struct {
    pthread_mutex_t lock;
    struct { DWORD pid,contexts; uint64_t cookie; } devices[16];
    struct { DWORD pid; uint64_t device,cookie; } slots[D3DKMT_MAX_HANDLES];
    unsigned serial,live,opens,closes,fail_open,gate,entered,resume,hook;
    DWORD fail_close_pid;
    uintptr_t output;
    OKMT_LUID luid;
    D3DKMT_CONTEXT_DESCRIPTION captured;
} ctx={.lock=PTHREAD_MUTEX_INITIALIZER};

static NTSTATUS context_device_open(void *data,DWORD pid,uint64_t *cookie)
{
    CHECK(data==&ctx);
    pthread_mutex_lock(&ctx.lock);
    NTSTATUS status=STATUS_NO_MEMORY;
    for (unsigned i=0;i<16;i++) if (!ctx.devices[i].pid) {
        ctx.devices[i].pid=pid; *cookie=ctx.devices[i].cookie=++ctx.serial;
        status=0; break;
    }
    pthread_mutex_unlock(&ctx.lock); return status;
}
static NTSTATUS context_device_close(void *data,DWORD pid,uint64_t cookie)
{
    CHECK(data==&ctx);
    pthread_mutex_lock(&ctx.lock);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<16;i++) if (ctx.devices[i].pid==pid && ctx.devices[i].cookie==cookie) {
        CHECK(!ctx.devices[i].contexts);
        ctx.devices[i].pid=0; status=0; break;
    }
    pthread_mutex_unlock(&ctx.lock); return status;
}
static NTSTATUS context_allocate(void *data,DWORD pid,uint64_t device,
    const D3DKMT_ALLOCATION_DESCRIPTION *description,uint64_t *cookie)
{
    (void)data; (void)pid; (void)device; (void)description; (void)cookie;
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS context_free(void *data,DWORD pid,uint64_t device,uint64_t cookie)
{
    (void)data; (void)pid; (void)device; (void)cookie;
    CHECK(0); return STATUS_INVALID_PARAMETER;
}
static NTSTATUS context_open(void *data,DWORD pid,uint64_t device,
    const D3DKMT_CONTEXT_DESCRIPTION *description,uint64_t *cookie,D3DKMT_CONTEXT_BUFFERS *buffers)
{
    CHECK(data==&ctx);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    if (description->node!=2 || description->engine!=1 || description->flags) return STATUS_NOT_SUPPORTED;
    if (description->private_bytes!=8) return STATUS_INVALID_PARAMETER;
    uint64_t magic; memcpy(&magic,description->private_data,8);
    if (magic!=0x33445566778899aaULL) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&ctx.lock);
    ctx.opens++; ctx.captured=*description;
    NTSTATUS status=STATUS_NO_MEMORY;
    if (!ctx.fail_open) for (unsigned d=0;d<16;d++) {
        if (ctx.devices[d].pid!=pid || ctx.devices[d].cookie!=device) continue;
        for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) if (!ctx.slots[i].pid) {
            ctx.slots[i].pid=pid; ctx.slots[i].device=device;
            *cookie=ctx.slots[i].cookie=++ctx.serial;
            ctx.devices[d].contexts++; ctx.live++; status=0; break;
        }
        break;
    }
    pthread_mutex_unlock(&ctx.lock);
    if (ctx.hook) {
        CHECK(d3dkmt_deferred_current());
        *buffers=(D3DKMT_CONTEXT_BUFFERS){(uintptr_t)user+512,(uintptr_t)user+1024,
            (uintptr_t)user+2048,0x1000123000ULL,256,4,3};
        if (ctx.hook==2) { denied_first=ctx.output; denied_last=denied_first+1; }
        if (ctx.hook==3) cancellation_requested=TRUE;
        if (ctx.hook==4) buffers->commands=0xffff800000001000ULL;
        if (ctx.hook==5) buffers->patch_count=UINT32_MAX;
        if (ctx.hook==6) {
            memset(user+256,0,8);
            CHECK(!memcmp(description->private_data,&magic,8));
        }
        if (ctx.hook==7) buffers->commands=0;
    }
    if (__atomic_load_n(&ctx.gate,__ATOMIC_ACQUIRE)==1) {
        __atomic_store_n(&ctx.entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&ctx.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    return status;
}
static NTSTATUS context_close(void *data,DWORD pid,uint64_t device,uint64_t cookie)
{
    CHECK(data==&ctx);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    if (__atomic_load_n(&ctx.gate,__ATOMIC_ACQUIRE)==2) {
        __atomic_store_n(&ctx.entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&ctx.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    pthread_mutex_lock(&ctx.lock);
    ctx.closes++;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) if (ctx.slots[i].pid==pid &&
        ctx.slots[i].device==device && ctx.slots[i].cookie==cookie) {
        status=D3DKMT_STATUS_DEVICE_BUSY;
        if (ctx.fail_close_pid==pid) break;
        for (unsigned d=0;d<16;d++) if (ctx.devices[d].pid==pid && ctx.devices[d].cookie==device) {
            CHECK(ctx.devices[d].contexts); ctx.devices[d].contexts--;
        }
        ctx.slots[i].pid=0; ctx.live--; status=0; break;
    }
    pthread_mutex_unlock(&ctx.lock);
    CHECK(status!=STATUS_INVALID_PARAMETER); return status;
}
static const D3DKMT_DEVICE_PROVIDER device_ops={context_device_open,context_device_close,&ctx,NULL,NULL,NULL};
static const D3DKMT_ALLOCATION_PROVIDER memory_ops={context_allocate,context_free,NULL,NULL,NULL,NULL};
static const D3DKMT_CONTEXT_PROVIDER context_ops={context_open,context_close};
static const D3DKMT_ADAPTER_DESCRIPTION adapter_desc={.driver_version=3200};
static const D3DKMT_CONTEXT_DESCRIPTION context_desc={.node=2,.engine=1,.hint=9,.private_bytes=8,
    .private_data={0xaa,0x99,0x88,0x77,0x66,0x55,0x44,0x33}};

static void context_cases(void)
{
    OKMT_HANDLE adapter,device,handle=0xfeed;
    D3DKMT_CONTEXT_BUFFERS buffers={.commands=0xfeed};
    CHECK(d3dkmt_register_memory_driver(&adapter_desc,&device_ops,&memory_ops,&ctx.luid)==0);
    CHECK(d3dkmt_open_adapter(12,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    CHECK(d3dkmt_create_context(12,device,&context_desc,&handle,&buffers)==STATUS_NOT_SUPPORTED);
    CHECK(handle==0xfeed && buffers.commands==0xfeed);
    d3dkmt_release_process(12); CHECK(d3dkmt_unregister_adapter(ctx.luid)==0);
    D3DKMT_CONTEXT_PROVIDER invalid=context_ops; invalid.close=NULL;
    CHECK(d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&invalid,&ctx.luid)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ctx.luid)==0);
    for (unsigned mode=0;mode<4;mode++) {
        CHECK(d3dkmt_open_adapter(12,ctx.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
        CHECK(d3dkmt_create_context(13,device,&context_desc,&handle,&buffers)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_create_context(12,adapter,&context_desc,&handle,&buffers)==STATUS_INVALID_PARAMETER);
        D3DKMT_CONTEXT_DESCRIPTION invalid_desc=context_desc; invalid_desc.flags=128;
        CHECK(d3dkmt_create_context(12,device,&invalid_desc,&handle,&buffers)==STATUS_INVALID_PARAMETER);
        invalid_desc=context_desc; invalid_desc.private_bytes=257;
        CHECK(d3dkmt_create_context(12,device,&invalid_desc,&handle,&buffers)==STATUS_INVALID_PARAMETER);
        ctx.fail_open=1;
        CHECK(d3dkmt_create_context(12,device,&context_desc,&handle,&buffers)==STATUS_NO_MEMORY && !ctx.live);
        ctx.fail_open=0;
        CHECK(d3dkmt_create_context(12,device,&context_desc,&handle,&buffers)==0 && ctx.live==1);
        CHECK(d3dkmt_close_adapter(12,handle)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_device(12,handle)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_context(12,device)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_context(13,handle)==STATUS_INVALID_PARAMETER);
        D3DKMT_CONTEXT_LEASE first={0},last={0},denied={0};
        CHECK(d3dkmt_acquire_context(13,handle,&denied)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_acquire_context(12,handle,&first)==0 && first.device==device && first.cookie);
        CHECK(d3dkmt_acquire_context(12,handle,&first)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_acquire_context(12,handle,&last)==0 && last.cookie==first.cookie);
        D3DKMT_CONTEXT_LEASE moved=first;
        CHECK(d3dkmt_release_context(&moved)==STATUS_INVALID_PARAMETER);
        if (mode==0) CHECK(d3dkmt_destroy_context(12,handle)==0);
        else if (mode==1) CHECK(d3dkmt_destroy_device(12,device)==0);
        else d3dkmt_release_process(12);
        CHECK(d3dkmt_acquire_context(12,handle,&denied)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_collect_contexts()==D3DKMT_STATUS_DEVICE_BUSY && ctx.live==1);
        CHECK(d3dkmt_release_context(&first)==0 && !first.self && ctx.live==1);
        ctx.fail_close_pid=mode==3 ? 12 : 0;
        CHECK(d3dkmt_release_context(&last)==0 && !last.self && ctx.live==(mode==3));
        if (mode==3) {
            CHECK(d3dkmt_collect_contexts()==D3DKMT_STATUS_DEVICE_BUSY && ctx.live==1);
            ctx.fail_close_pid=0; CHECK(d3dkmt_collect_devices()==0 && !ctx.live);
        }
        CHECK(d3dkmt_release_context(&last)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_context(12,handle)==STATUS_INVALID_PARAMETER);
        d3dkmt_release_process(12);
    }
    CHECK(d3dkmt_unregister_adapter(ctx.luid)==0);
    /* Handle exhaustion reserves no driver ownership; generations prevent reuse. */
    CHECK(d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ctx.luid)==0);
    CHECK(d3dkmt_open_adapter(12,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES-2;i++)
        CHECK(d3dkmt_create_context(12,device,&context_desc,&handle,&buffers)==0);
    unsigned opened=ctx.opens;
    CHECK(d3dkmt_create_context(12,device,&context_desc,&handle,&buffers)==STATUS_NO_MEMORY && opened==ctx.opens);
    d3dkmt_release_process(12); CHECK(!ctx.live && d3dkmt_unregister_adapter(ctx.luid)==0);
    CHECK(d3dkmt_acquire_context(12,handle,&(D3DKMT_CONTEXT_LEASE){0})==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_collect_devices()==0);
}

typedef struct { OKMT_HANDLE device,handle; unsigned close; NTSTATUS status; } CONTEXT_RACE;
static void *context_thread(void *argument)
{
    CONTEXT_RACE *r=argument;
    D3DKMT_CONTEXT_BUFFERS buffers={0};
    r->status=r->close ? d3dkmt_destroy_context(12,r->handle) :
        d3dkmt_create_context(12,r->device,&context_desc,&r->handle,&buffers);
    return NULL;
}
static void context_races(void)
{
    for (unsigned phase=0;phase<2;phase++) for (unsigned round=0;round<32;round++) {
        OKMT_HANDLE adapter,device; D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ctx.luid)==0);
        CHECK(d3dkmt_open_adapter(12,ctx.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(12,adapter,0,&device)==0);
        CONTEXT_RACE race={.device=device,.handle=0xfeed,.close=phase};
        if (phase) CHECK(d3dkmt_create_context(12,device,&context_desc,&race.handle,&buffers)==0);
        ctx.gate=phase+1; ctx.entered=ctx.resume=0;
        pthread_t thread; CHECK(pthread_create(&thread,NULL,context_thread,&race)==0);
        while (!__atomic_load_n(&ctx.entered,__ATOMIC_ACQUIRE)) sched_yield();
        if (round%3==0) d3dkmt_release_process(12);
        else if (round%3==1) CHECK(d3dkmt_destroy_device(12,device)==0);
        CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        CHECK(d3dkmt_collect_devices()==(round%3==2 && !phase ? STATUS_SUCCESS : D3DKMT_STATUS_DEVICE_BUSY));
        CHECK(ctx.live==1);
        __atomic_store_n(&ctx.resume,1,__ATOMIC_RELEASE);
        CHECK(pthread_join(thread,NULL)==0); ctx.gate=0;
        CHECK(race.status==(phase ? 0 : round%3==2 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_PROCESS_IS_TERMINATING));
        if (!phase) CHECK(race.handle==0xfeed);
        d3dkmt_release_process(12); CHECK(!ctx.live);
        CHECK(d3dkmt_collect_devices()==0 && d3dkmt_unregister_adapter(ctx.luid)==0);
    }
}

static void context_abi_cases(void)
{
    user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED); if (user==MAP_FAILED) return;
    caller=12;
    CHECK(d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ctx.luid)==0);
    OKMT_HANDLE adapter,device;
    CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&device)==0);
    for (int compat=0;compat<2;compat++) for (unsigned mode=1;mode<=7;mode++) {
        g_compat32_mode=compat;
        memset(user,0x75,4096);
        memcpy(user+256,context_desc.private_data,8);
        unsigned size=compat ? 64 : sizeof(OKMT_CREATE_CONTEXT),offset=compat ? 28 : 32;
        if (compat) {
            DWORD *words=(DWORD *)user;
            words[0]=device; words[1]=2; words[2]=1; words[3]=0;
            words[4]=(DWORD)(uintptr_t)(user+256); words[5]=8; words[6]=9;
        } else {
            OKMT_CREATE_CONTEXT *c=(void *)user;
            c->device=device; c->node=2; c->engine=1; c->flags=0;
            c->private_data=user+256; c->private_bytes=8; c->hint=9;
        }
        BYTE before[96]; memcpy(before,user,size);
        ctx.hook=mode; ctx.output=(uintptr_t)user+offset;
        ctx.fail_close_pid=mode==2 ? caller : 0;
        NTSTATUS status=D3DKMTCreateContext(user);
        BOOL success=mode==1 || mode==6;
        CHECK(status==(success ? 0 : mode==3 ? STATUS_PROCESS_IS_TERMINATING : STATUS_INVALID_PARAMETER));
        CHECK(!d3dkmt_deferred_current());
        CHECK(!memcmp(user,before,offset));
        if (success) {
            OKMT_HANDLE handle; memcpy(&handle,user+offset,4);
            CHECK(handle && handle!=0x75757575 && ctx.live==1);
            CHECK(ctx.captured.compat32==compat && ctx.captured.hint==9 && ctx.captured.private_bytes==8);
            CHECK(!memcmp(ctx.captured.private_data,context_desc.private_data,8));
            if (compat) {
                DWORD *w=(DWORD *)user;
                CHECK(w[8]==(DWORD)(uintptr_t)(user+512) && w[9]==256);
                CHECK(w[10]==(DWORD)(uintptr_t)(user+1024) && w[11]==4);
                CHECK(w[12]==(DWORD)(uintptr_t)(user+2048) && w[13]==3);
                CHECK(*(uint64_t *)(user+56)==0x1000123000ULL);
            } else {
                OKMT_CREATE_CONTEXT *c=(void *)user;
                CHECK(c->command_buffer==user+512 && c->command_bytes==256);
                CHECK(c->allocation_list==user+1024 && c->allocation_count==4);
                CHECK(c->patch_list==user+2048 && c->patch_count==3 && c->gpu_commands==0x1000123000ULL);
            }
            memcpy(user+128,&handle,4);
            CHECK(D3DKMTDestroyContext(user+128)==0);
            CHECK(D3DKMTDestroyContext(user+128)==STATUS_INVALID_PARAMETER);
        } else CHECK(!memcmp(user,before,size));
        if (mode==2) {
            CHECK(ctx.live==1 && d3dkmt_collect_contexts()==D3DKMT_STATUS_DEVICE_BUSY);
            ctx.fail_close_pid=0;
            CHECK(d3dkmt_collect_contexts()==0);
        }
        CHECK(!ctx.live);
        ctx.hook=0; denied_first=denied_last=0; cancellation_requested=FALSE;
        CHECK(D3DKMTCreateContext(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTDestroyContext(NULL)==STATUS_INVALID_PARAMETER);
    }
    g_compat32_mode=0; d3dkmt_release_process(caller);
    CHECK(d3dkmt_collect_contexts()==0 && d3dkmt_unregister_adapter(ctx.luid)==0);
    CHECK(munmap(user,user_bytes)==0); user=NULL; user_bytes=0;
}
#ifndef D3DKMT_CONTEXT_SUITE_ENTRY
#define D3DKMT_CONTEXT_SUITE_ENTRY main
#endif
int D3DKMT_CONTEXT_SUITE_ENTRY(void)
{
    if (adapter_suite_main()) return 1;
    checks=failures=0;
    context_cases(); context_races(); context_abi_cases();
    printf("D3DKMT contexts checks=%u failures=%u live=%u, 64 forced lifecycle races, PE32/64\n",checks,failures,ctx.live);
    return failures ? 1 : 0;
}
