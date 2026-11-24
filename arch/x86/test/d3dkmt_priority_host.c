#define D3DKMT_CONTEXT_SUITE_ENTRY priority_context_regression
#include "d3dkmt_context_host.c"

static unsigned priority_writer_done,priority_writer_failed;
static OKMT_HANDLE priority_writer_context;
static void *priority_writer(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<4096;i++) {
        LONG priority=(LONG)(i%15)-7;
        if(d3dkmt_context_priority(100,priority_writer_context,TRUE,&priority)!=0)priority_writer_failed=1;
    }
    __atomic_store_n(&priority_writer_done,1,__ATOMIC_RELEASE);return NULL;
}
static void priority_abi_cases(BOOL compat)
{
    caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
    ctx.hook=ctx.fail_open=ctx.fail_close_pid=0;
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    CHECK(d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ctx.luid)==0);
    OKMT_HANDLE adapter=0,device=0,context=0;D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_open_adapter(100,ctx.luid,&adapter)==0 && d3dkmt_create_device(100,adapter,0,&device)==0);
    CHECK(d3dkmt_create_context(100,device,&context_desc,&context,&buffers)==0);
    OKMT_CONTEXT_PRIORITY *request=(void *)user;*request=(OKMT_CONTEXT_PRIORITY){context,0xfeed};
    CHECK(D3DKMTGetContextSchedulingPriority(request)==0 && !request->priority && request->context==context);
    D3DKMT_CONTEXT_LEASE held={0};CHECK(d3dkmt_acquire_context(100,context,&held)==0);
    for(LONG value=-7;value<=7;value++) {
        request->priority=value;BYTE before[8];memcpy(before,request,8);
        CHECK(D3DKMTSetContextSchedulingPriority(request)==0 && !memcmp(before,request,8));
        request->priority=0xfeed;
        CHECK(D3DKMTGetContextSchedulingPriority(request)==0 && request->priority==value && request->context==context);
        CHECK(d3dkmt_context_priority_snapshot(&held)==value);
    }
    const LONG bad[]={-8,8,(LONG)0x80000000,0x7fffffff,0x40000000,0x4000001f,0x40000020};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        request->priority=bad[i];BYTE before[8];memcpy(before,request,8);
        CHECK(D3DKMTSetContextSchedulingPriority(request)==(i==4 || i==5?STATUS_NOT_SUPPORTED:STATUS_INVALID_PARAMETER));
        CHECK(!memcmp(before,request,8) && d3dkmt_context_priority_snapshot(&held)==7);
    }
    request->priority=0xfeed;caller=101;
    CHECK(D3DKMTGetContextSchedulingPriority(request)==STATUS_INVALID_PARAMETER && request->priority==0xfeed);
    request->priority=-1;CHECK(D3DKMTSetContextSchedulingPriority(request)==STATUS_INVALID_PARAMETER);caller=100;
    denied_first=(uintptr_t)&request->priority;denied_last=denied_first+4;
    CHECK(D3DKMTGetContextSchedulingPriority(request)==STATUS_INVALID_PARAMETER && request->priority==-1);
    denied_first=denied_last=0;cancellation_requested=TRUE;
    CHECK(D3DKMTSetContextSchedulingPriority(request)==STATUS_PROCESS_IS_TERMINATING);
    CHECK(D3DKMTGetContextSchedulingPriority(request)==STATUS_PROCESS_IS_TERMINATING);
    cancellation_requested=FALSE;
    CHECK(!d3dkmt_deferred_current() && d3dkmt_context_priority_snapshot(&held)==7);
    CHECK(D3DKMTGetContextSchedulingPriority(NULL)==STATUS_INVALID_PARAMETER);
    CHECK(D3DKMTSetContextSchedulingPriority(NULL)==STATUS_INVALID_PARAMETER);
    memcpy(user+4090,&context,4);
    CHECK(D3DKMTGetContextSchedulingPriority(user+4090)==STATUS_INVALID_PARAMETER);
    CHECK(D3DKMTSetContextSchedulingPriority(user+4090)==STATUS_INVALID_PARAMETER);
    priority_writer_context=context;priority_writer_done=priority_writer_failed=0;pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,priority_writer,NULL));
    do {LONG value=d3dkmt_context_priority_snapshot(&held);CHECK(value>=-7 && value<=7);sched_yield();}
    while(!__atomic_load_n(&priority_writer_done,__ATOMIC_ACQUIRE));
    CHECK(!pthread_join(thread,NULL));
    CHECK(!priority_writer_failed);
    LONG last=d3dkmt_context_priority_snapshot(&held);
    CHECK(d3dkmt_destroy_context(100,context)==0 && d3dkmt_context_priority_snapshot(&held)==last);
    request->priority=0xfeed;CHECK(D3DKMTGetContextSchedulingPriority(request)==STATUS_INVALID_PARAMETER && request->priority==0xfeed);
    OKMT_HANDLE fresh=0;CHECK(d3dkmt_create_context(100,device,&context_desc,&fresh,&buffers)==0 && fresh!=context);
    request->context=fresh;CHECK(D3DKMTGetContextSchedulingPriority(request)==0 && request->priority==0);
    CHECK(d3dkmt_release_context(&held)==0 && !held.priority);
    CHECK(d3dkmt_mark_device_error(100,device,0x80000000)==0);
    request->priority=0;CHECK(D3DKMTSetContextSchedulingPriority(request)==D3DKMT_STATUS_DEVICE_ERROR);
    CHECK(d3dkmt_destroy_context(100,fresh)==0 && d3dkmt_destroy_device(100,device)==0);
    CHECK(d3dkmt_close_adapter(100,adapter)==0 && d3dkmt_unregister_adapter(ctx.luid)==0);
    CHECK(munmap(user,user_bytes)==0);user=NULL;user_bytes=0;
}
int main(void)
{
    if(priority_context_regression())return 1;
    unsigned before=checks;priority_abi_cases(FALSE);priority_abi_cases(TRUE);
    printf("D3DKMT context priority: checks=%u failures=%u; PE32/64 and concurrent live snapshots\n",checks-before,failures);
    return failures?1:0;
}
