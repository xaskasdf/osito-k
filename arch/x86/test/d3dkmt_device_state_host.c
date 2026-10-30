/* Public PE32/PE64 capture, removal races and retained provider lifetime. */
#define main state_adapter_regression
#include "d3dkmt_registry_host.c"
#undef main

static struct { OKMT_HANDLE device; OKMT_LUID luid; DWORD state;
    unsigned calls,closed,hook; NTSTATUS result; } execution;
static NTSTATUS state_open(void *c,DWORD pid,uint64_t *cookie)
{ CHECK(c==&execution && pid==100);*cookie=42;return 0; }
static NTSTATUS state_close(void *c,DWORD pid,uint64_t cookie)
{ CHECK(c==&execution && pid==100 && cookie==42);execution.closed++;return 0; }
static NTSTATUS state_query(void *c,DWORD pid,uint64_t cookie,DWORD *out)
{
    CHECK(c==&execution && pid==100 && cookie==42 && !execution.closed);
    execution.calls++;
    DWORD count=0;CHECK(d3dkmt_enumerate_adapters(pid,0,NULL,&count)==0 && count==1);
    if (execution.hook==1) CHECK(d3dkmt_destroy_device(pid,execution.device)==0);
    if (execution.hook==2) d3dkmt_release_process(pid);
    if (execution.hook==3) CHECK(d3dkmt_unregister_adapter(execution.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    if (execution.hook==4) {denied_first=(uintptr_t)(user+8);denied_last=denied_first+4;}
    if (execution.hook==5) cancellation_requested=TRUE;
    if (execution.hook==6) {((OKMT_DEVICE_EXECUTION *)user)->device=0;((OKMT_DEVICE_EXECUTION *)user)->type=6;}
    CHECK(!execution.closed); /* close/exit cannot free provider during callback */
    *out=execution.state;return execution.result;
}
static void state_cases(BOOL compat,unsigned hook)
{
    memset(&execution,0,sizeof(execution));execution.state=OKMT_EXECUTION_ACTIVE;
    caller=100;g_compat32_mode=compat;denied_first=denied_last=0;cancellation_requested=FALSE;
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000};
    D3DKMT_DEVICE_PROVIDER provider={state_open,state_close,&execution,hook==7?NULL:state_query,NULL,NULL};
    OKMT_HANDLE adapter;
    CHECK(d3dkmt_register_driver(&description,&provider,&execution.luid)==0);
    CHECK(d3dkmt_open_adapter(caller,execution.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&execution.device)==0);
    memset(user,0xa5,4096);
    OKMT_DEVICE_EXECUTION *q=(void *)user;
    *q=(OKMT_DEVICE_EXECUTION){execution.device,1,0xfeed};execution.hook=hook;
    if (hook) {
        NTSTATUS expected=hook==1 || hook==2 || hook==5 ? STATUS_PROCESS_IS_TERMINATING :
            hook==3 ? D3DKMT_STATUS_DEVICE_REMOVED : hook==4 ? STATUS_INVALID_PARAMETER :
            hook==7 ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
        CHECK(D3DKMTGetDeviceState(q)==expected);
        CHECK(q->state==(hook==6?OKMT_EXECUTION_ACTIVE:0xfeed));
        if (hook==1 || hook==2) CHECK(execution.closed==1);
    } else {
        CHECK(D3DKMTGetDeviceState(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTGetDeviceState(user+4090)==STATUS_INVALID_PARAMETER);
        for (DWORD state=1;state<=7;state++) {
            execution.state=state;q->state=0xfeed;
            CHECK(D3DKMTGetDeviceState(q)==0 && q->state==state);
            CHECK(q->device==execution.device && q->type==1);
        }
        for (unsigned i=12;i<4096;i++) CHECK(user[i]==0xa5);
        for (DWORD type=0;type<=7;type++) if (type!=1) {
            q->type=type;q->state=0xfeed;unsigned before=execution.calls;
            CHECK(D3DKMTGetDeviceState(q)==(type && type<=6?STATUS_NOT_SUPPORTED:STATUS_INVALID_PARAMETER));
            CHECK(q->state==0xfeed && execution.calls==before);
        }
        q->type=1;q->device=adapter;CHECK(D3DKMTGetDeviceState(q)==STATUS_INVALID_PARAMETER);
        q->device=execution.device;caller=101;CHECK(D3DKMTGetDeviceState(q)==STATUS_INVALID_PARAMETER);caller=100;
        execution.state=0;CHECK(D3DKMTGetDeviceState(q)==STATUS_UNSUCCESSFUL && q->state==0xfeed);
        execution.state=8;CHECK(D3DKMTGetDeviceState(q)==STATUS_UNSUCCESSFUL && q->state==0xfeed);
        execution.result=STATUS_NO_MEMORY;CHECK(D3DKMTGetDeviceState(q)==STATUS_NO_MEMORY && q->state==0xfeed);
        execution.result=0;execution.state=1;
        denied_first=(uintptr_t)(user+8);denied_last=denied_first+4;unsigned before=execution.calls;
        CHECK(D3DKMTGetDeviceState(q)==STATUS_INVALID_PARAMETER && execution.calls==before);
        denied_first=denied_last=0;
    }
    cancellation_requested=FALSE;denied_first=denied_last=0;
    if (hook!=1 && hook!=2) CHECK(d3dkmt_destroy_device(caller,execution.device)==0);
    q->device=execution.device;q->type=1;q->state=0xfeed;
    CHECK(D3DKMTGetDeviceState(q)==STATUS_INVALID_PARAMETER && q->state==0xfeed);
    if (hook!=2) CHECK(d3dkmt_close_adapter(caller,adapter)==0);
    CHECK(d3dkmt_unregister_adapter(execution.luid)==0);
    CHECK(execution.closed==1);
}
int main(void)
{
    if (state_adapter_regression()) return 1;
    unsigned before=checks;
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    for (unsigned compat=0;compat<2;compat++) for (unsigned hook=0;hook<8;hook++) state_cases(compat,hook);
    CHECK(munmap(user,user_bytes)==0);
    printf("Device execution ABI checks=%u failures=%u\n",checks-before,failures);
    return failures?1:0;
}
