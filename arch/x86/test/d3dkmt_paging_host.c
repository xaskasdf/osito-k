#define D3DKMT_MONITORED_SUITE_ENTRY paging_regression_main
#include "d3dkmt_monitored_host.c"

static unsigned paging_hook,paging_calls;
static NTSTATUS paging_create(void *opaque,DWORD pid,uint64_t device,uint64_t initial,BOOL compat,
    D3DKMT_MONITORED_BACKING *backing)
{
    CHECK(initial==0);paging_calls++;
    NTSTATUS status=monitored_create(opaque,pid,device,0,compat,backing);
    if (paging_hook==1) {denied_first=(uintptr_t)user+16;denied_last=denied_first+8;}
    if (paging_hook==2) cancellation_requested=TRUE;
    if (paging_hook==3) {backing->gpu_address=0;}
    if (paging_hook==4) return STATUS_UNSUCCESSFUL; /* Partial ownership stays retained. */
    return status;
}
static void paging_setup(BOOL compat)
{
    caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
    paging_hook=paging_calls=monitored_creates=monitored_closes=monitored_race=0;
    monitored_fail_close=monitored_poison_output=false;
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    D3DKMT_RENDER_PROVIDER render={.submit=sync_render_unused,.synchronize=sync_submit};
    D3DKMT_MONITORED_PROVIDER native={paging_create,monitored_close,monitored_notify};
    CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&render,&native,&ctx.luid)==0);
    OKMT_HANDLE adapter=0;D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
    CHECK(d3dkmt_create_context(caller,sync_device,&context_desc,&sync_context,&buffers)==0);
}
static void paging_header(LONG priority,DWORD physical)
{
    memset(user,0xa5,64);
    if (g_compat32_mode) {
        DWORD *p=(DWORD *)user;p[0]=sync_device;p[1]=(DWORD)priority;p[5]=physical;
    } else {
        OKMT_CREATE_PAGING_QUEUE *p=(void *)user;p->device=sync_device;p->priority=priority;p->physical_index=physical;
    }
}
static void paging_cases(BOOL compat)
{
    paging_setup(compat);
    CHECK(D3DKMTCreatePagingQueue(NULL)==STATUS_INVALID_PARAMETER);
    CHECK(D3DKMTDestroyPagingQueue(NULL)==STATUS_INVALID_PARAMETER);
    for (unsigned invalid=0;invalid<8;invalid++) {
        paging_header(invalid==0?-2:invalid==1?2:0,invalid==2?1:0);
        if (invalid==3) *(DWORD *)user=0;
        if (invalid==4) caller=101;
        if (invalid==5) cancellation_requested=TRUE;
        if (invalid==6) {denied_first=(uintptr_t)user+8;denied_last=denied_first+4;}
        if (invalid==7) {denied_first=(uintptr_t)user+16;denied_last=denied_first+4;}
        BYTE before[64];memcpy(before,user,64);
        CHECK(D3DKMTCreatePagingQueue(user)!=0 && !memcmp(user,before,64) && !paging_calls);
        caller=100;cancellation_requested=FALSE;denied_first=denied_last=0;
    }
    for (unsigned failure=1;failure<=4;failure++) {
        paging_header(0,0);paging_hook=failure;BYTE before[64];memcpy(before,user,64);
        unsigned closed=monitored_closes;
        CHECK(D3DKMTCreatePagingQueue(user)!=0 && !memcmp(user,before,64));
        CHECK(monitored_closes==closed+1);
        paging_hook=0;denied_first=denied_last=0;cancellation_requested=FALSE;
    }
    /* Wrong type, owner and device never provide access to an internal fence. */
    for (LONG priority=-1;priority<=1;priority++) {
        paging_header(priority,0);BYTE before[64];memcpy(before,user,64);
        CHECK(D3DKMTCreatePagingQueue(user)==0);
        OKMT_HANDLE queue=((DWORD *)user)[2],object=((DWORD *)user)[3];
        CHECK(queue && object && queue!=object && monitored_storage[monitored_creates]==0);
        memcpy(before+8,user+8,8);memcpy(before+16,user+16,compat?4:8);CHECK(!memcmp(before,user,64));
        CHECK(d3dkmt_close_adapter(caller,queue)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_sync(caller,queue)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_sync(caller,object)==STATUS_ACCESS_DENIED);
        CHECK(d3dkmt_destroy_device(caller,queue)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_paging_queue(101,queue)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_paging_queue(caller,object)==STATUS_INVALID_PARAMETER);
        uint64_t value=7;
        CHECK(d3dkmt_signal_sync_cpu(caller,sync_device,1,&object,&value,FALSE)==STATUS_ACCESS_DENIED);
        CHECK(d3dkmt_signal_sync_cpu(caller,sync_device,1,&object,&value,TRUE)==STATUS_ACCESS_DENIED);
        CHECK(monitored_storage[monitored_creates]==0);
        D3DKMT_SYNC_COMMAND command={.count=1,.objects={object},.value=UINT64_MAX-1,.signal=TRUE,.extended=TRUE};
        D3DKMT_SYNC_LEASE wait={0};
        CHECK(d3dkmt_sync_capture(caller,sync_context,&command,&wait)==STATUS_ACCESS_DENIED && !wait.self);
        command.signal=FALSE;CHECK(d3dkmt_sync_capture(caller,sync_context,&command,&wait)==0);
        unsigned publication=D3DKMT_WORK_QUEUED;
        CHECK(d3dkmt_sync_step(&wait,&publication)==D3DKMT_STATUS_DEVICE_BUSY);
        D3DKMT_CPU_WAIT_LEASE cpu_wait={0};value=UINT64_MAX;
        CHECK(d3dkmt_cpu_wait_capture(caller,sync_device,1,&object,&value,FALSE,&cpu_wait)==0);
        CHECK(d3dkmt_cpu_wait_status(&cpu_wait)==STATUS_PENDING);
        /* Queue destruction retains its fence and parent while a wait owns it. */
        unsigned closed=monitored_closes;*(OKMT_DESTROY_PAGING_QUEUE *)user=(OKMT_DESTROY_PAGING_QUEUE){queue};
        CHECK(D3DKMTDestroyPagingQueue(user)==0 && monitored_closes==closed);
        CHECK(monitored_storage[monitored_creates]==UINT64_MAX);
        CHECK(d3dkmt_cpu_wait_status(&cpu_wait)==STATUS_SUCCESS);
        CHECK(d3dkmt_sync_step(&wait,&publication)==0);
        CHECK(d3dkmt_cpu_wait_release(&cpu_wait)==0 && monitored_closes==closed);
        CHECK(D3DKMTDestroyPagingQueue(user)==STATUS_INVALID_PARAMETER);
        D3DKMT_SYNC_LEASE rejected={0};CHECK(d3dkmt_sync_capture(caller,sync_context,&command,&rejected)==STATUS_INVALID_PARAMETER);
        monitored_fail_close=true;CHECK(d3dkmt_sync_release(&wait)==0 && monitored_closes>closed);
        CHECK(d3dkmt_collect_syncs()==STATUS_UNSUCCESSFUL);
        monitored_fail_close=false;CHECK(d3dkmt_collect_syncs()==0);
        CHECK(d3dkmt_destroy_paging_queue(caller,queue)==STATUS_INVALID_PARAMETER);
    }
    /* Reserve all but one global handle: creating a pair must be atomic. */
    OKMT_HANDLE occupied[D3DKMT_MAX_HANDLES];unsigned count=0;
    while (count<D3DKMT_MAX_HANDLES && d3dkmt_open_adapter(caller,ctx.luid,&occupied[count])==0) count++;
    CHECK(count>0);CHECK(d3dkmt_close_adapter(caller,occupied[--count])==0);
    paging_header(0,0);unsigned calls=paging_calls;CHECK(D3DKMTCreatePagingQueue(user)==STATUS_NO_MEMORY && paging_calls==calls);
    while(count) CHECK(d3dkmt_close_adapter(caller,occupied[--count])==0);
    paging_header(0,0);CHECK(D3DKMTCreatePagingQueue(user)==0);OKMT_HANDLE queue=((DWORD *)user)[2];
    CHECK(d3dkmt_destroy_paging_queue(caller,queue)==0);
    sync_cleanup();
}
static void paging_races(BOOL compat,unsigned race)
{
    paging_setup(compat);paging_header(0,0);BYTE before[64];memcpy(before,user,64);
    monitored_race=race;
    CHECK(D3DKMTCreatePagingQueue(user)==(race==3?D3DKMT_STATUS_DEVICE_REMOVED:STATUS_PROCESS_IS_TERMINATING));
    CHECK(!memcmp(user,before,64) && monitored_creates==1 && monitored_closes==1);
    monitored_race=0;d3dkmt_release_process(caller);
    for(unsigned i=0;i<16;i++) (void)d3dkmt_collect_devices();
    CHECK(!ctx.live);if(race!=3) CHECK(d3dkmt_unregister_adapter(ctx.luid)==0);
    CHECK(munmap(user,user_bytes)==0);user=NULL;user_bytes=0;
}
#ifndef D3DKMT_PAGING_SUITE_ENTRY
#define D3DKMT_PAGING_SUITE_ENTRY main
#endif
int D3DKMT_PAGING_SUITE_ENTRY(void)
{
    CHECK(paging_regression_main()==0);
    unsigned start=checks;
    for(unsigned compat=0;compat<2;compat++) {
        paging_cases(compat);
        for(unsigned race=1;race<=3;race++)paging_races(compat,race);
    }
    printf("D3DKMT paging queues: checks=%u failures=%u PE32/PE64\n",checks-start,failures);
    return failures?1:0;
}
