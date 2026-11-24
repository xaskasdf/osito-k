/* Public marshalling and retained synchronization metadata, not GPU execution. */
#define D3DKMT_CONTEXT_SUITE_ENTRY context_suite_main
#include "d3dkmt_context_host.c"
static OKMT_HANDLE sync_device,sync_context;
static unsigned sync_calls;
static D3DKMT_SYNC_COMMAND sync_command;
static NTSTATUS sync_render_unused(const D3DKMT_CONTEXT_LEASE *context,const D3DKMT_RENDER_DESCRIPTION *command,DWORD *queued)
{ (void)context; (void)command; (void)queued; return STATUS_NOT_SUPPORTED; }
static NTSTATUS sync_submit(const D3DKMT_CONTEXT_LEASE *context,const D3DKMT_SYNC_COMMAND *command)
{
    CHECK(d3dkmt_deferred_current() && context->self==context && context->handle==sync_context);
    sync_calls++; sync_command=*command;
    D3DKMT_SYNC_LEASE lease={0};
    NTSTATUS status=d3dkmt_sync_capture(context->pid,context->handle,command,&lease);
    if (!status) CHECK(d3dkmt_sync_release(&lease)==0);
    return status;
}
static void sync_setup(BOOL compat)
{
    caller=100; g_compat32_mode=compat; cancellation_requested=FALSE; denied_first=denied_last=0;
    sync_calls=0; user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    D3DKMT_RENDER_PROVIDER ops={.submit=sync_render_unused,.synchronize=sync_submit};
    CHECK(d3dkmt_register_render_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ops,&ctx.luid)==0);
    OKMT_HANDLE adapter=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
    CHECK(d3dkmt_create_context(caller,sync_device,&context_desc,&sync_context,&buffers)==0);
}
static void sync_cleanup(void)
{
    caller=100; g_compat32_mode=0; cancellation_requested=FALSE; denied_first=denied_last=0;
    d3dkmt_release_process(caller);
    CHECK(d3dkmt_collect_contexts()==0 && d3dkmt_collect_devices()==0 && !ctx.live);
    CHECK(d3dkmt_unregister_adapter(ctx.luid)==0);
    CHECK(munmap(user,user_bytes)==0); user=NULL; user_bytes=0;
}
static void sync_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        sync_setup(compat);
        for (unsigned extended=0;extended<2;extended++) for (unsigned type=1;type<=3;type++) {
            if (!extended && type==3) continue;
            memset(user,0,512);
            OKMT_HANDLE handle=0;
            if (extended) {
                OKMT_CREATE_SYNC2 *c=(void *)user; c->device=sync_device; c->info.type=type;
                if (type==2) { c->info.data.semaphore.maximum=3; c->info.data.semaphore.initial=2; }
                if (type==3) c->info.data.value=0x100000007ULL;
                c->info.shared=0xfeed;
                CHECK(D3DKMTCreateSynchronizationObject2(c)==0 && c->object && !c->info.shared); handle=c->object;
            } else {
                OKMT_CREATE_SYNC *c=(void *)user; c->device=sync_device; c->info.type=type;
                if (type==2) { c->info.data.semaphore.maximum=3; c->info.data.semaphore.initial=2; }
                CHECK(D3DKMTCreateSynchronizationObject(c)==0 && c->object); handle=c->object;
            }
            for (unsigned signal=0;signal<2;signal++) {
                memset(user,0,512);
                OKMT_WAIT_SYNC *w=(void *)user; w->context=sync_context; w->count=1; w->objects[0]=handle;
                if (extended) {
                    if (signal) ((OKMT_SIGNAL_SYNC2 *)user)->values[0]=0x100000008ULL;
                    else ((OKMT_WAIT_SYNC2 *)user)->values[0]=0x100000008ULL;
                }
                BYTE before[512]; memcpy(before,user,512);
                NTSTATUS status=signal ? (extended ? D3DKMTSignalSynchronizationObject2(user) : D3DKMTSignalSynchronizationObject(user)) :
                    (extended ? D3DKMTWaitForSynchronizationObject2(user) : D3DKMTWaitForSynchronizationObject(user));
                CHECK(status==0 && !memcmp(user,before,512));
                CHECK(sync_command.count==1 && sync_command.objects[0]==handle && sync_command.signal==(BOOL)signal &&
                    sync_command.extended==(BOOL)extended && sync_command.value==(extended ? 0x100000008ULL : 0));
            }
            OKMT_DESTROY_SYNC *d=(void *)user; d->object=handle;
            CHECK(D3DKMTDestroySynchronizationObject(d)==0);
            CHECK(D3DKMTDestroySynchronizationObject(d)==STATUS_INVALID_PARAMETER);
        }
        CHECK(sync_calls==10 && !d3dkmt_deferred_current());
        for (unsigned mode=0;mode<8;mode++) {
            memset(user,0,512); OKMT_CREATE_SYNC2 *c=(void *)user;
            c->device=sync_device; c->info.type=OKMT_SYNC_SEMAPHORE;
            c->info.data.semaphore.maximum=2; c->info.data.semaphore.initial=1; c->object=0xfeed;
            switch(mode) {
            case 0:c->info.data.semaphore.maximum=0;break;
            case 1:c->info.data.semaphore.initial=3;break;
            case 2:c->device=sync_context;break;
            case 3:c->info.type=5;break;
            case 4:c->info.flags=1;break;
            case 5:denied_first=(uintptr_t)&c->object;denied_last=denied_first+4;break;
            case 6:cancellation_requested=TRUE;break;
            case 7:caller=101;break;
            }
            CHECK(D3DKMTCreateSynchronizationObject2(c)!=0 && c->object==0xfeed);
            caller=100; cancellation_requested=FALSE; denied_first=denied_last=0;
        }
        for (unsigned mode=0;mode<6;mode++) {
            memset(user,0,512); OKMT_SIGNAL_SYNC2 *s=(void *)user;
            s->context=sync_context; s->count=1; s->objects[0]=0xffffffffu;
            switch(mode) {
            case 0:s->count=0;break;
            case 1:s->count=33;break;
            case 2:s->flags=0x80000000;break;
            case 3:s->flags=1;break;
            case 4:s->broadcast_count=1;break;
            case 5:s->broadcast_count=65;break;
            }
            CHECK(D3DKMTSignalSynchronizationObject2(s)!=0 && sync_calls==10);
        }
        for (unsigned mode=0;mode<7;mode++) {
            memset(user,0,512); OKMT_SIGNAL_SYNC2 *s=(void *)user;
            s->context=sync_context; s->flags=2; s->values[0]=0x40;
            unsigned before=sync_calls;
            switch (mode) {
            case 1:s->count=1;break;
            case 2:s->values[0]=0;break;
            case 3:s->values[0]=0x44;break;
            case 4:s->flags=3;break;
            case 5:s->values[0]=0xdeadbeef00000040ULL;break;
            case 6:caller=101;break;
            }
            NTSTATUS status=D3DKMTSignalSynchronizationObject2(s);
            CHECK((status==0)==(mode==0 || (mode==5 && compat)));
            CHECK(!event_refs && !event_signals);
            if (!status) CHECK(sync_calls==before+1 && sync_command.event_handle==0x40 && sync_command.count==0);
            caller=100;
        }
        CHECK(!d3dkmt_deferred_current()); sync_cleanup();
    }
}
#ifndef D3DKMT_SYNC_SUITE_ENTRY
#define D3DKMT_SYNC_SUITE_ENTRY main
#endif
int D3DKMT_SYNC_SUITE_ENTRY(void)
{
    if (context_suite_main()) return 1;
    checks=failures=0; sync_cases();
    printf("D3DKMT synchronization ABI: checks=%u failures=%u PE32/PE64\n",checks,failures);
    return failures ? 1 : 0;
}
