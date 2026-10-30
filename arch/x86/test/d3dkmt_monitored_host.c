#define D3DKMT_SYNC_SUITE_ENTRY monitored_abi_regression_main
#include "d3dkmt_sync_host.c"
#include <stdbool.h>

static uint64_t monitored_storage[64];
static unsigned monitored_creates,monitored_closes,monitored_notifications;
static unsigned monitored_race;
static bool monitored_poison_output,monitored_fail_close;
static NTSTATUS monitored_create(void *opaque,DWORD pid,uint64_t device,uint64_t initial,
    BOOL compat,D3DKMT_MONITORED_BACKING *out)
{
    CHECK(opaque && pid==caller && device && compat==g_compat32_mode);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0 && count==1);
    monitored_creates++; CHECK(monitored_creates<64); monitored_storage[monitored_creates]=initial;
    *out=(D3DKMT_MONITORED_BACKING){monitored_creates,(uintptr_t)user+2048,
        0x400000000ULL+4096*monitored_creates,&monitored_storage[monitored_creates]};
    if (monitored_poison_output) {
        denied_first=(uintptr_t)user+__builtin_offsetof(OKMT_CREATE_SYNC2,info.data.monitored.gpu_address);
        denied_last=denied_first+8;
    }
    if (monitored_race==1) CHECK(d3dkmt_destroy_device(pid,sync_device)==0);
    if (monitored_race==2) d3dkmt_release_process(pid);
    if (monitored_race==3) CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    return STATUS_SUCCESS;
}
static NTSTATUS monitored_close(void *opaque,DWORD pid,uint64_t device,uint64_t cookie)
{
    CHECK(opaque && pid==100 && device && cookie && cookie<=monitored_creates);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0);
    monitored_closes++;
    return monitored_fail_close ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
static void monitored_notify(void *opaque)
{
    CHECK(opaque==&ctx);
    DWORD count=0; CHECK(d3dkmt_enumerate_adapters(100,0,NULL,&count)==0);
    monitored_notifications++;
}
static void monitored_abi_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        caller=100; g_compat32_mode=compat; cancellation_requested=FALSE; denied_first=denied_last=0;
        user_bytes=4096; user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
        CHECK(user!=MAP_FAILED);
        monitored_creates=monitored_closes=0; monitored_poison_output=monitored_fail_close=false;
        D3DKMT_RENDER_PROVIDER ops={.submit=sync_render_unused,.synchronize=sync_submit};
        D3DKMT_MONITORED_PROVIDER native={monitored_create,monitored_close,monitored_notify};
        CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ops,&native,&ctx.luid)==0);
        OKMT_HANDLE adapter=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
        CHECK(d3dkmt_create_context(caller,sync_device,&context_desc,&sync_context,&buffers)==0);
        for (unsigned mode=0;mode<9;mode++) {
            memset(user,0xa5,512); OKMT_CREATE_SYNC2 *c=(void *)user;
            c->device=sync_device; c->info.type=OKMT_SYNC_MONITORED; c->info.flags=0;
            c->info.data.monitored.initial=0x9988776655443322ULL; c->info.data.monitored.affinity=0;
            c->object=0xfeed;
            unsigned created=monitored_creates,closed=monitored_closes;
            if (mode==1) c->info.flags=2;
            if (mode==2) c->info.flags=3;
            if (mode==3) c->info.data.monitored.affinity=2;
            if (mode==4) {denied_first=(uintptr_t)&c->info.data.monitored.cpu_address;denied_last=denied_first+(compat?4:8);}
            if (mode==5) {denied_first=(uintptr_t)&c->info.data.monitored.gpu_address;denied_last=denied_first+8;}
            if (mode==6) monitored_poison_output=true;
            if (mode==7) caller=101;
            if (mode==8) cancellation_requested=TRUE;
            BYTE before[sizeof(*c)]; memcpy(before,c,sizeof(*c));
            NTSTATUS status=D3DKMTCreateSynchronizationObject2(c);
            if (!mode) {
                CHECK(status==0 && c->object && c->object!=0xfeed && !c->info.shared);
                CHECK(c->info.data.monitored.initial==0x9988776655443322ULL && monitored_storage[monitored_creates]==c->info.data.monitored.initial);
                CHECK(c->info.data.monitored.gpu_address==0x400001000ULL);
                uint64_t cpu=0; memcpy(&cpu,&c->info.data.monitored.cpu_address,compat?4:8);
                CHECK(cpu==(uintptr_t)user+2048);
                if (compat) CHECK(!memcmp((BYTE *)&c->info.data.monitored.cpu_address+4,
                    before+__builtin_offsetof(OKMT_CREATE_SYNC2,info.data.monitored.cpu_address)+4,4));
                OKMT_DESTROY_SYNC destroy={c->object};
                monitored_fail_close=true; CHECK(d3dkmt_destroy_sync(caller,destroy.object)==0);
                CHECK(monitored_closes==closed+1);
                CHECK(d3dkmt_collect_syncs()==STATUS_UNSUCCESSFUL && monitored_closes==closed+2);
                monitored_fail_close=false; CHECK(d3dkmt_collect_syncs()==0 && monitored_closes==closed+3);
            } else {
                CHECK(status!=0 && !memcmp(c,before,sizeof(*c)));
                CHECK(monitored_creates==created+(mode==6) && monitored_closes==closed+(mode==6));
            }
            caller=100; cancellation_requested=FALSE; monitored_poison_output=false; denied_first=denied_last=0;
        }
        sync_cleanup();
    }
    /* Force teardown between native allocation and public handle publication. */
    for (unsigned compat=0;compat<2;compat++) for (unsigned race=1;race<=3;race++) {
        caller=100; g_compat32_mode=compat; cancellation_requested=FALSE; denied_first=denied_last=0;
        user_bytes=4096; user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
        CHECK(user!=MAP_FAILED);
        monitored_creates=monitored_closes=0; monitored_poison_output=monitored_fail_close=false;
        D3DKMT_RENDER_PROVIDER ops={.submit=sync_render_unused,.synchronize=sync_submit};
        D3DKMT_MONITORED_PROVIDER native={monitored_create,monitored_close,monitored_notify};
        CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ops,&native,&ctx.luid)==0);
        OKMT_HANDLE adapter=0;
        CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
        memset(user,0xa5,512); OKMT_CREATE_SYNC2 *c=(void *)user;
        c->device=sync_device; c->info.type=OKMT_SYNC_MONITORED; c->info.flags=0;
        c->info.data.monitored.initial=0x100000007ULL; c->info.data.monitored.affinity=1;
        BYTE before[sizeof(*c)]; memcpy(before,c,sizeof(*c));
        monitored_race=race;
        NTSTATUS status=D3DKMTCreateSynchronizationObject2(c);
        monitored_race=0;
        CHECK(status==(race==3 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_PROCESS_IS_TERMINATING));
        CHECK(!memcmp(c,before,sizeof(*c)) && monitored_creates==1 && monitored_closes==1);
        d3dkmt_release_process(caller);
        CHECK(d3dkmt_collect_syncs()==0 && d3dkmt_collect_devices()==0 && !ctx.live);
        if (race!=3) CHECK(d3dkmt_unregister_adapter(ctx.luid)==0);
        CHECK(munmap(user,user_bytes)==0); user=NULL; user_bytes=0;
    }
}
static NTSTATUS monitored_cpu_request(unsigned count,const OKMT_HANDLE *handles,const uint64_t *values,unsigned flags)
{
    memset(user,0xa5,128);
    if (g_compat32_mode) {
        DWORD small[]={sync_device,count,(DWORD)(uintptr_t)handles,(DWORD)(uintptr_t)values,flags};
        memcpy(user,small,sizeof(small));
    } else *(OKMT_SIGNAL_SYNC_CPU *)user=(OKMT_SIGNAL_SYNC_CPU){sync_device,count,handles,(const OKMT_U64 *)values,flags};
    return D3DKMTSignalSynchronizationObjectFromCpu(user);
}
static NTSTATUS monitored_gpu_request(BOOL signal,unsigned count,const OKMT_HANDLE *handles,uint64_t value)
{
    memset(user,0xa5,128);
    OKMT_SYNC_GPU *input=(void *)user;input->context=sync_context;input->count=count;
    if (g_compat32_mode) { DWORD pointer=(DWORD)(uintptr_t)handles;memcpy(&input->objects,&pointer,4); }
    else input->objects=handles;
    input->data.value=value;
    return signal ? D3DKMTSignalSynchronizationObjectFromGpu(user) : D3DKMTWaitForSynchronizationObjectFromGpu(user);
}
static void monitored_array_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
        user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
        CHECK(user!=MAP_FAILED);monitored_creates=monitored_closes=monitored_notifications=0;
        D3DKMT_RENDER_PROVIDER ops={.submit=sync_render_unused,.synchronize=sync_submit};
        D3DKMT_MONITORED_PROVIDER native={monitored_create,monitored_close,monitored_notify};
        CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ops,&native,&ctx.luid)==0);
        OKMT_HANDLE adapter=0;D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
        CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
        CHECK(d3dkmt_create_context(caller,sync_device,&context_desc,&sync_context,&buffers)==0);
        OKMT_HANDLE objects[2]={0};uint64_t cpu,gpu;
        for (unsigned i=0;i<2;i++) CHECK(d3dkmt_create_monitored_sync(caller,sync_device,0x100000000ULL+i,compat,&objects[i],&cpu,&gpu)==0);
        OKMT_HANDLE *handles=(void *)(user+256);uint64_t *values=(void *)(user+512);
        memcpy(handles,objects,sizeof(objects));values[0]=0x200000007ULL;values[1]=0x300000009ULL;
        uint64_t pointer=(uintptr_t)values;if (compat) pointer|=0xa5a5a5a500000000ULL;
        CHECK(monitored_gpu_request(FALSE,2,handles,pointer)==0 && sync_command.vector_values);
        CHECK(sync_command.values[0]==values[0] && sync_command.values[1]==values[1]);
        D3DKMT_SYNC_LEASE pending={0};CHECK(d3dkmt_sync_capture(caller,sync_context,&sync_command,&pending)==0);
        unsigned publication=D3DKMT_WORK_QUEUED;
        CHECK(d3dkmt_sync_step(&pending,&publication)==D3DKMT_STATUS_DEVICE_BUSY && publication==D3DKMT_WORK_QUEUED);
        values[1]=0x300000008ULL;
        CHECK(monitored_cpu_request(2,handles,values,0)==0 && monitored_notifications==1);
        CHECK(d3dkmt_sync_step(&pending,&publication)==D3DKMT_STATUS_DEVICE_BUSY);
        values[1]++;
        CHECK(monitored_cpu_request(2,handles,values,0)==0 && monitored_notifications==2);
        CHECK(d3dkmt_sync_step(&pending,&publication)==0 && d3dkmt_sync_release(&pending)==0);
        /* A bad second input must not update the first object or wake anyone. */
        for (unsigned mode=0;mode<9;mode++) {
            uint64_t before[2]={monitored_storage[1],monitored_storage[2]};
            unsigned notified=monitored_notifications,count=2,flags=0;
            memcpy(handles,objects,sizeof(objects));values[0]=before[0]+1;values[1]=before[1]+1;
            const OKMT_HANDLE *h=handles;const uint64_t *v=values;
            if (mode==0) handles[1]=0;
            if (mode==1) handles[1]=handles[0];
            if (mode==2) caller=101;
            if (mode==3) values[1]=before[1]-1;
            if (mode==4) flags=8;
            if (mode==5) count=0;
            if (mode==6) count=33;
            if (mode==7) h=(const void *)(user+4092);
            if (mode==8) v=(const void *)(user+4092);
            CHECK(monitored_cpu_request(count,h,v,flags)!=0);
            CHECK(monitored_notifications==notified && monitored_storage[1]==before[0] && monitored_storage[2]==before[1]);
            caller=100;
        }
        memcpy(handles,objects,sizeof(objects));values[0]=7;values[1]=9;
        CHECK(monitored_cpu_request(2,handles,values,4)==0 && monitored_storage[1]==7 && monitored_storage[2]==9);
        values[0]=0x1234567887654321ULL;values[1]=0x4433221100000007ULL;
        CHECK(monitored_gpu_request(TRUE,2,handles,pointer)==0);
        CHECK(d3dkmt_sync_capture(caller,sync_context,&sync_command,&pending)==0);
        /* User arrays can be changed immediately after admission. */
        handles[0]=0;values[0]=0;values[1]=0;
        unsigned closed=monitored_closes;CHECK(d3dkmt_destroy_sync(caller,objects[0])==0 && monitored_closes==closed);
        publication=D3DKMT_WORK_QUEUED;
        CHECK(d3dkmt_sync_step(&pending,&publication)==0);
        CHECK(monitored_storage[1]==0x1234567887654321ULL && monitored_storage[2]==0x4433221100000007ULL);
        CHECK(d3dkmt_sync_release(&pending)==0 && monitored_closes==closed+1);
        memcpy(handles,objects,sizeof(objects));CHECK(monitored_gpu_request(TRUE,2,handles,pointer)!=0);
        handles[0]=objects[1];handles[1]=objects[1];CHECK(monitored_gpu_request(FALSE,2,handles,pointer)!=0);
        CHECK(monitored_gpu_request(FALSE,1,handles,(uintptr_t)user+4092)!=0);
        CHECK(monitored_gpu_request(FALSE,33,handles,pointer)!=0);
        CHECK(monitored_gpu_request(FALSE,0,handles,pointer)!=0);
        CHECK(d3dkmt_destroy_sync(caller,objects[1])==0);
        /* Legacy fence waits carry a scalar, not an address to dereference. */
        D3DKMT_SYNC_DESCRIPTION legacy={.type=OKMT_SYNC_FENCE,.initial=0x100000009ULL};
        for (unsigned i=0;i<2;i++) CHECK(d3dkmt_create_sync(caller,sync_device,&legacy,&handles[i])==0);
        CHECK(monitored_gpu_request(FALSE,2,handles,0x100000009ULL)==0 && !sync_command.vector_values);
        CHECK(d3dkmt_sync_capture(caller,sync_context,&sync_command,&pending)==0);
        publication=D3DKMT_WORK_QUEUED;
        CHECK(d3dkmt_sync_step(&pending,&publication)==0 && d3dkmt_sync_release(&pending)==0);
        CHECK(monitored_gpu_request(TRUE,2,handles,pointer)!=0);
        CHECK(monitored_cpu_request(2,handles,values,0)!=0);
        sync_cleanup();
    }
}
#ifndef D3DKMT_MONITORED_SUITE_ENTRY
#define D3DKMT_MONITORED_SUITE_ENTRY main
#endif
int D3DKMT_MONITORED_SUITE_ENTRY(void)
{
    if (monitored_abi_regression_main()) return 1;
    checks=failures=0; monitored_abi_cases(); monitored_array_cases();
    printf("D3DKMT monitored ABI: checks=%u failures=%u PE32/PE64\n",checks,failures);
    return failures ? 1 : 0;
}
