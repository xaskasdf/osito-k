/* Public ABI and retained provider lifetime. No modeled draw reports success
 * as GPU work: the separate native/physical suites qualify actual submission. */
#define D3DKMT_CONTEXT_SUITE_ENTRY context_suite_main
#include "d3dkmt_context_host.c"

static struct {
    unsigned calls,mode,gate,entered,resume;
    NTSTATUS status;
    OKMT_HANDLE adapter,device,context;
} render;
static NTSTATUS render_submit(const D3DKMT_CONTEXT_LEASE *lease,
    const D3DKMT_RENDER_DESCRIPTION *command,DWORD *queued)
{
    CHECK(d3dkmt_deferred_current() && ctx.live==1 && lease->self==lease);
    CHECK(lease->provider.context==&ctx && command->offset==32 && command->bytes==64 &&
        command->allocations==2 && command->patches==2);
    CHECK(lease->buffers.commands==(uintptr_t)user+512 && lease->buffers.command_bytes==256);
    render.calls++;
    if (render.mode==20) { denied_first=(uintptr_t)user; denied_last=denied_first+368; }
    if (render.mode==21) cancellation_requested=TRUE;
    if (render.mode==22) CHECK(d3dkmt_destroy_context(caller,render.context)==0);
    if (render.mode==23) CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    if (__atomic_load_n(&render.gate,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&render.entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&render.resume,__ATOMIC_ACQUIRE)) sched_yield();
    }
    NTSTATUS status=d3dkmt_context_work_status(lease,NULL,0);
    *queued=3;
    return status ? status : render.mode==17 ? D3DKMT_STATUS_DEVICE_BUSY :
        render.mode==18 ? STATUS_NO_MEMORY : STATUS_SUCCESS;
}
static const D3DKMT_RENDER_PROVIDER render_ops={.submit=render_submit};

static void render_setup(unsigned mode,BOOL compat)
{
    memset(&render,0,sizeof(render)); render.mode=mode;
    caller=100; g_compat32_mode=0; cancellation_requested=FALSE; denied_first=denied_last=0;
    user_bytes=4096; user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    NTSTATUS status=mode==16 ? d3dkmt_register_context_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ctx.luid) :
        d3dkmt_register_render_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&render_ops,&ctx.luid);
    CHECK(status==0);
    CHECK(d3dkmt_open_adapter(caller,ctx.luid,&render.adapter)==0);
    CHECK(d3dkmt_create_device(caller,render.adapter,0,&render.device)==0);
    OKMT_CREATE_CONTEXT *create=(void *)user;
    create->device=render.device; create->node=2; create->engine=1; create->hint=9;
    create->private_data=user+256; create->private_bytes=8;
    memcpy(user+256,context_desc.private_data,8); ctx.hook=1;
    CHECK(D3DKMTCreateContext(user)==0); render.context=create->context; ctx.hook=0;
    memset(user,0,368); g_compat32_mode=compat;
    DWORD *w=(void *)user;
    w[0]=render.context; w[1]=32; w[2]=64; w[3]=2; w[4]=2;
    if (compat) {
        w[5]=w[7]=w[9]=0xdead; w[6]=w[8]=w[10]=UINT32_MAX; w[79]=0xfeed;
    } else {
        OKMT_RENDER *r=(void *)user;
        r->commands=r->allocations=r->patches=(void *)(uintptr_t)0xdead;
        r->command_bytes=r->allocation_capacity=r->patch_capacity=UINT32_MAX; r->queued=0xfeed;
    }
}
static void render_cleanup(void)
{
    caller=100; g_compat32_mode=0; cancellation_requested=FALSE; denied_first=denied_last=0;
    d3dkmt_release_process(caller);
    CHECK(d3dkmt_collect_contexts()==0 && d3dkmt_unregister_adapter(ctx.luid)==0 && !ctx.live);
    CHECK(munmap(user,user_bytes)==0); user=NULL; user_bytes=0;
}
static void render_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) for (unsigned mode=0;mode<24;mode++) {
        render_setup(mode,compat);
        DWORD *w=(void *)user;
        unsigned fi=compat ? 11 : 17,bc=compat ? 14 : 20,pr=compat ? 82 : 88;
        switch(mode) {
        case 1:w[fi]=7;break;
        case 2:w[0]=0xffffffff;break;
        case 3:caller=101;break;
        case 4:w[0]=render.device;break;
        case 5:w[1]=UINT32_MAX;break;
        case 6:w[2]=256;break;
        case 7:w[3]=5;break;
        case 8:w[4]=4;break;
        case 9:w[fi]=0x80000000;break;
        case 10:w[fi]=8;break;
        case 11:w[fi]=32;break;
        case 12:w[bc]=65;break;
        case 13:w[bc]=1;break;
        case 14:w[pr]=1;break;
        case 15:w[compat ? 12 : 18]=1;break;
        case 19:denied_first=(uintptr_t)user;denied_last=denied_first+1;break;
        }
        BYTE before[368]; memcpy(before,user,sizeof(before));
        unsigned delivered=cancellation_deliveries;
        NTSTATUS status=D3DKMTRender(user);
        NTSTATUS expected=mode<=1 ? 0 : mode==10 || mode==11 || mode==13 || mode==15 || mode==16 ? STATUS_NOT_SUPPORTED :
            mode==17 ? D3DKMT_STATUS_DEVICE_BUSY : mode==18 ? STATUS_NO_MEMORY :
            mode==21 || mode==22 ? STATUS_PROCESS_IS_TERMINATING : mode==23 ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_INVALID_PARAMETER;
        CHECK(status==expected);
        CHECK(render.calls==(mode<=1 || (mode>=17 && mode!=19) ? 1u : 0u));
        CHECK(!d3dkmt_deferred_current());
        CHECK(cancellation_deliveries==delivered+(mode==21));
        if (mode<=1 || mode==17 || mode==18 || mode==22 || mode==23) {
            CHECK(!memcmp(user,before,20));
            if (compat) {
                CHECK(w[5]==(DWORD)(uintptr_t)(user+512) && w[6]==256 && w[7]==(DWORD)(uintptr_t)(user+1024));
                CHECK(w[8]==4 && w[9]==(DWORD)(uintptr_t)(user+2048) && w[10]==3 && w[79]==3);
                CHECK(!memcmp(user+44,before+44,272) && !memcmp(user+328,before+328,8));
                CHECK(*(uint64_t *)(user+320)==0x1000123000ULL);
            } else {
                OKMT_RENDER *r=(void *)user;
                CHECK(r->commands==user+512 && r->command_bytes==256 && r->allocations==user+1024);
                CHECK(r->allocation_capacity==4 && r->patches==user+2048 && r->patch_capacity==3 && r->queued==3);
                CHECK(!memcmp(user+68,before+68,272) && !memcmp(user+352,before+352,16));
                CHECK(r->gpu_commands==0x1000123000ULL);
            }
        }
        if (mode==20 || mode==21 || mode==19 || (mode>=9 && mode<=15) || (mode>=2 && mode<=4))
            CHECK(!memcmp(user,before,compat ? 336 : 368));
        CHECK(D3DKMTRender(NULL)==STATUS_INVALID_PARAMETER);
        render_cleanup();
    }
}
static void *render_thread(void *unused)
{ (void)unused; render.status=D3DKMTRender(user); return NULL; }
static void render_races(void)
{
    for (unsigned round=0;round<32;round++) {
        render_setup(0,round&1); render.gate=1;
        pthread_t thread; CHECK(pthread_create(&thread,NULL,render_thread,NULL)==0);
        while (!__atomic_load_n(&render.entered,__ATOMIC_ACQUIRE)) sched_yield();
        switch(round%4) {
        case 0:CHECK(d3dkmt_destroy_context(caller,render.context)==0);break;
        case 1:CHECK(d3dkmt_destroy_device(caller,render.device)==0);break;
        case 2:d3dkmt_release_process(caller);break;
        case 3:CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);break;
        }
        CHECK(ctx.live==1); /* provider backing cannot close while Render holds its lease */
        __atomic_store_n(&render.resume,1,__ATOMIC_RELEASE);
        CHECK(pthread_join(thread,NULL)==0 && render.status!=0 && render.calls==1);
        render_cleanup();
    }
}
int main(void)
{
    if (context_suite_main()) return 1;
    checks=failures=0;
    render_cases(); render_races();
    printf("D3DKMT Render: checks=%u failures=%u, PE32/64 and 32 forced close/removal races\n",checks,failures);
    return failures ? 1 : 0;
}
