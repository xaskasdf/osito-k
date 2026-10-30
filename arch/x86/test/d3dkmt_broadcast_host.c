#define D3DKMT_MONITORED_SUITE_ENTRY broadcast_public_regression_main
#include "d3dkmt_monitored_host.c"

static OKMT_HANDLE broadcast_contexts[OKMT_MAX_BROADCAST_CONTEXTS],broadcast_objects[2];
static uint64_t broadcast_values[2];
static unsigned broadcast_count;
static void broadcast_setup(BOOL compat,unsigned count)
{
    caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
    monitored_creates=monitored_closes=0;monitored_poison_output=monitored_fail_close=false;
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED && !event_refs);
    D3DKMT_RENDER_PROVIDER ops={.submit=sync_render_unused,.synchronize=sync_submit};
    D3DKMT_MONITORED_PROVIDER native={monitored_create,monitored_close,monitored_notify};
    CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ops,&native,&ctx.luid)==0);
    OKMT_HANDLE adapter=0;D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0 && d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
    broadcast_count=count;
    for (unsigned i=0;i<count;i++) CHECK(d3dkmt_create_context(caller,sync_device,&context_desc,&broadcast_contexts[i],&buffers)==0);
    sync_context=broadcast_contexts[0];
    uint64_t cpu,gpu;
    for (unsigned i=0;i<2;i++) CHECK(d3dkmt_create_monitored_sync(caller,sync_device,0,compat,&broadcast_objects[i],&cpu,&gpu)==0);
    broadcast_values[0]=0x1234567800000007ULL;broadcast_values[1]=0xabcdef0100000009ULL;
}
static void broadcast_input(unsigned flags)
{
    memset(user,0xa5,128);
    memcpy(user+256,broadcast_objects,sizeof(broadcast_objects));
    memcpy(user+512,broadcast_values,sizeof(broadcast_values));
    memcpy(user+1024,broadcast_contexts,broadcast_count*sizeof(OKMT_HANDLE));
    uint64_t value=flags&2 ? 0x40 : (uintptr_t)(user+512);
    if (g_compat32_mode) {
        struct { DWORD count,objects,flags,context_count,contexts;OKMT_U64 data[8]; } input;
        memset(&input,0xa5,sizeof(input));memset(input.data,0,sizeof(input.data));
        input.count=flags&2 ? 0 : 2;input.objects=(DWORD)(uintptr_t)(user+256);input.flags=flags;
        input.context_count=broadcast_count;input.contexts=(DWORD)(uintptr_t)(user+1024);
        input.data[0]=0xa5a5a5a500000000ULL|value;memcpy(user,&input,sizeof(input));
    } else {
        OKMT_SIGNAL_SYNC_GPU2 *input=(void *)user;
        input->count=flags&2 ? 0 : 2;input->objects=(void *)(user+256);input->flags=flags;
        input->context_count=broadcast_count;input->contexts=(void *)(user+1024);
        memset(&input->data,0,sizeof(input->data));input->data.value=value;
    }
}
static void broadcast_cleanup(BOOL removed)
{
    caller=100;cancellation_requested=FALSE;d3dkmt_release_process(100);
    NTSTATUS status=D3DKMT_STATUS_DEVICE_BUSY;
    for (unsigned i=0;i<D3DKMT_MAX_HANDLES && (status || ctx.live);i++) status=d3dkmt_collect_devices();
    CHECK(!status && !ctx.live);
    if (!removed) CHECK(d3dkmt_unregister_adapter(ctx.luid)==0);
    CHECK(!event_refs && munmap(user,user_bytes)==0);user=NULL;user_bytes=0;
}
static void broadcast_public_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        broadcast_setup(compat,64);broadcast_input(0);
        CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==0);
        CHECK(sync_command.broadcast_count==64 && sync_command.broadcast[63]==broadcast_contexts[63] &&
            sync_command.vector_values && !memcmp(sync_command.values,broadcast_values,sizeof(broadcast_values)));
        broadcast_input(4);CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==0 && sync_command.allow_rewind);
        unsigned signals=event_signals;broadcast_input(2);
        CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==0 && !event_refs && event_signals==signals);
        broadcast_input(2);
        ((DWORD *)user)[compat ? 6 : 8]=0x41;
        CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==STATUS_INVALID_HANDLE && !event_refs);
        OKMT_HANDLE legacy=0;
        D3DKMT_SYNC_DESCRIPTION legacy_desc={.type=OKMT_SYNC_FENCE};
        CHECK(d3dkmt_create_sync(100,sync_device,&legacy_desc,&legacy)==0);
        broadcast_input(0);((DWORD *)user)[0]=1;((OKMT_HANDLE *)(user+256))[0]=legacy;
        uint64_t legacy_value=0x9876543212345678ULL;
        memcpy(user+(compat ? 24 : 32),&legacy_value,sizeof(legacy_value));
        CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==0 && !sync_command.vector_values && sync_command.value==legacy_value);
        CHECK(d3dkmt_destroy_sync(100,legacy)==0);
        OKMT_HANDLE adapter=0,foreign_device=0,foreign_context=0;D3DKMT_CONTEXT_BUFFERS buffers={0};
        CHECK(d3dkmt_open_adapter(100,ctx.luid,&adapter)==0 && d3dkmt_create_device(100,adapter,0,&foreign_device)==0);
        CHECK(d3dkmt_create_context(100,foreign_device,&context_desc,&foreign_context,&buffers)==0);
        broadcast_input(0);((OKMT_HANDLE *)(user+1024))[63]=foreign_context;
        CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_device(100,foreign_device)==0);
        for (unsigned mode=0;mode<15;mode++) {
            broadcast_input(mode==5 ? 1 : mode==6 ? 8 : mode==7 ? 6 : 0);
            DWORD *words=(void *)user;
            unsigned count_word=compat ? 3 : 5,contexts_word=compat ? 4 : 6,value_word=compat ? 6 : 8;
            if (mode==0) words[count_word]=0;
            if (mode==1) words[count_word]=65;
            if (mode==2) words[contexts_word]=(DWORD)(uintptr_t)(user+4092);
            if (mode==3) ((OKMT_HANDLE *)(user+1024))[63]=broadcast_contexts[0];
            if (mode==4) ((OKMT_HANDLE *)(user+1024))[63]=0;
            if (mode==8) words[value_word]=(DWORD)(uintptr_t)(user+4092);
            if (mode==9) words[0]=0;
            if (mode==10) words[0]=33;
            if (mode==11) caller=101;
            if (mode==12) cancellation_requested=TRUE;
            if (mode==13) ((OKMT_HANDLE *)(user+256))[1]=0;
            if (mode==14) words[value_word+2]=1;
            CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)!=0 && !event_refs);
            caller=100;cancellation_requested=FALSE;
        }
        broadcast_input(0);CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==0);
        D3DKMT_SYNC_LEASE lease={0};CHECK(d3dkmt_sync_capture(100,sync_context,&sync_command,&lease)==0 && lease.extra_count==63);
        unsigned publication=D3DKMT_WORK_PREPARED;
        CHECK(d3dkmt_sync_admit(&lease,&publication,TRUE)==0 && publication==D3DKMT_WORK_QUEUED);
        CHECK(d3dkmt_destroy_context(100,broadcast_contexts[63])==0);
        CHECK(d3dkmt_sync_step(&lease,&publication)==STATUS_PROCESS_IS_TERMINATING && publication==D3DKMT_WORK_QUEUED);
        CHECK(!monitored_storage[1] && !monitored_storage[2] && d3dkmt_sync_release(&lease)==0);
        broadcast_cleanup(FALSE);
    }
    for (unsigned close=0;close<5;close++) {
        broadcast_setup(0,2);broadcast_input(0);CHECK(D3DKMTSignalSynchronizationObjectFromGpu2(user)==0);
        D3DKMT_SYNC_LEASE lease={0};CHECK(d3dkmt_sync_capture(100,sync_context,&sync_command,&lease)==0);
        unsigned publication=D3DKMT_WORK_PREPARED;
        if (close==0) CHECK(d3dkmt_destroy_context(100,broadcast_contexts[1])==0);
        if (close==1) CHECK(d3dkmt_destroy_device(100,sync_device)==0);
        if (close==2) d3dkmt_release_process(100);
        if (close==3) CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        if (close==4) publication=D3DKMT_WORK_CANCELLED;
        CHECK(d3dkmt_sync_admit(&lease,&publication,TRUE)!=0);
        CHECK(publication==(close==4 ? D3DKMT_WORK_CANCELLED : D3DKMT_WORK_PREPARED));
        CHECK(!monitored_storage[1] && !monitored_storage[2] && d3dkmt_sync_release(&lease)==0);
        broadcast_cleanup(close==3);
    }
}
int main(void)
{
    if (broadcast_public_regression_main()) return 1;
    checks=failures=0;broadcast_public_cases();
    printf("D3DKMT broadcast ABI: checks=%u failures=%u PE32/PE64\n",checks,failures);
    return failures ? 1 : 0;
}
