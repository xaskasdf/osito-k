#define D3DKMT_MONITORED_SUITE_ENTRY cpuwait_regression_main
#include "d3dkmt_monitored_host.c"
#include "../win32/d3dkmt_wait.h"
#include "../kernel/sched_signal.h"
#include <setjmp.h>

static void (*wait_worker_entry)(void),(*wait_block_hook)(void);
static sched_signal_t *wait_worker_signal;
static jmp_buf wait_worker_jump;
static BOOL wait_worker_running;
static unsigned wait_spawns,wait_wakes,wait_spawn_fail,wait_wake_fail,wait_cpu;
static uint64_t wait_worker_ticks;
static OKMT_HANDLE wait_objects[2];
static uint64_t wait_targets[2];
static unsigned wait_block_round;
static void wait_worker_step(void);
uint32_t smp_current_cpu(void) {return wait_cpu;}
uint64_t paging_get_kernel_cr3(void) {return 0x123000;}
void smp_wake_bsp(void) { }
int sched_spawn_detached_in_address_space_blocked(const char *name,void (*entry)(void),uint64_t cr3,int owns)
{
    CHECK(!strcmp(name,"kmt-cpu-wait") && cr3==0x123000 && !owns && !wait_worker_entry);
    wait_spawns++;if (wait_spawn_fail) {wait_spawn_fail=0;return -1;}
    wait_worker_entry=entry;return 47;
}
int proc_wake_pid(int pid)
{
    CHECK(pid==47 && wait_worker_entry);wait_wakes++;
    if (wait_wake_fail) {wait_wake_fail=0;return -1;}
    return 0;
}
enum sched_signal_result sched_signal_wait(sched_signal_t *signal,uint64_t ticks)
{
    if (wait_worker_running) {
        wait_worker_signal=signal;wait_worker_ticks=ticks;
        __atomic_store_n(&signal->waiters,1,__ATOMIC_RELEASE);longjmp(wait_worker_jump,1);
    }
    CHECK(ticks==1 && wait_worker_entry);
    if (wait_block_hook) wait_block_hook();
    wait_worker_step();
    return __atomic_load_n(&signal->bits,__ATOMIC_ACQUIRE) ? SCHED_SIGNAL_READY : SCHED_SIGNAL_TIMEOUT;
}
static void wait_worker_step(void)
{
    CHECK(wait_worker_entry && !wait_worker_running);wait_worker_running=TRUE;
    for (unsigned i=0;i<128;i++) {
        if (wait_worker_signal) __atomic_store_n(&wait_worker_signal->waiters,0,__ATOMIC_RELEASE);
        if (!setjmp(wait_worker_jump)) {wait_worker_entry();CHECK(FALSE);}
        if (!__atomic_load_n(&wait_worker_signal->bits,__ATOMIC_ACQUIRE)) {wait_worker_running=FALSE;return;}
    }
    CHECK(FALSE);
}
static void wait_setup(BOOL compat)
{
    caller=100;g_compat32_mode=compat;cancellation_requested=FALSE;denied_first=denied_last=0;
    monitored_creates=monitored_closes=monitored_notifications=0;monitored_poison_output=monitored_fail_close=false;
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED && !event_refs);
    D3DKMT_RENDER_PROVIDER ops={.submit=sync_render_unused,.synchronize=sync_submit};
    D3DKMT_MONITORED_PROVIDER native={monitored_create,monitored_close,monitored_notify};
    CHECK(d3dkmt_register_monitored_driver(&adapter_desc,&device_ops,&memory_ops,&context_ops,&ops,&native,&ctx.luid)==0);
    OKMT_HANDLE adapter=0;D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_open_adapter(caller,ctx.luid,&adapter)==0);
    CHECK(d3dkmt_create_device(caller,adapter,0,&sync_device)==0);
    CHECK(d3dkmt_create_context(caller,sync_device,&context_desc,&sync_context,&buffers)==0);
    uint64_t cpu,gpu;
    for (unsigned i=0;i<2;i++) CHECK(d3dkmt_create_monitored_sync(caller,sync_device,i,compat,&wait_objects[i],&cpu,&gpu)==0);
    wait_targets[0]=0x1234567800000007ULL;wait_targets[1]=0xabcdef0100000009ULL;
}
static void wait_cleanup(void)
{
    d3dkmt_release_process(100);
    if (wait_worker_entry) wait_worker_step();
    for (unsigned i=0;i<4;i++) (void)d3dkmt_collect_devices();
    CHECK(!event_refs);
    sync_cleanup();
    if (wait_worker_entry) {wait_worker_step();CHECK(d3dkmt_wait_idle() && wait_worker_ticks==0);}
}
static NTSTATUS wait_public(unsigned count,OKMT_HANDLE *objects,uint64_t *values,uintptr_t event,unsigned flags)
{
    memset(user,0xa5,128);
    if (g_compat32_mode) {
        DWORD input[]={sync_device,count,(DWORD)(uintptr_t)objects,(DWORD)(uintptr_t)values,(DWORD)event,flags};
        memcpy(user,input,sizeof(input));
    } else *(OKMT_WAIT_SYNC_CPU *)user=(OKMT_WAIT_SYNC_CPU){sync_device,count,objects,(const OKMT_U64 *)values,(void *)event,flags};
    return D3DKMTWaitForSynchronizationObjectFromCpu(user);
}
static void wait_publish_arrays(void)
{
    memcpy(user+256,wait_objects,sizeof(wait_objects));memcpy(user+512,wait_targets,sizeof(wait_targets));
}
static void wait_block_signal(void)
{
    wait_block_round++;
    CHECK(wait_block_round<=2);
    CHECK(d3dkmt_signal_sync_cpu(100,sync_device,wait_block_round,wait_objects,wait_targets,FALSE)==0);
    if (wait_block_round==2) wait_block_hook=NULL;
}
static void wait_block_cancel(void)
{wait_block_hook=NULL;cancellation_requested=TRUE;}

static void wait_public_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        wait_setup(compat);OKMT_HANDLE *objects=(void *)(user+256);uint64_t *values=(void *)(user+512);
        wait_publish_arrays();values[0]=values[1]=0;
        unsigned signals=event_signals,spawns=wait_spawns;
        wait_cpu=1;
        CHECK(wait_public(2,objects,values,0x40,0)==STATUS_NOT_SUPPORTED &&
            event_signals==signals && !event_refs && wait_spawns==spawns);
        wait_cpu=0;
        CHECK(wait_public(2,objects,values,0,0)==0 && wait_spawns==spawns);
        CHECK(wait_public(2,objects,values,0x40,0)==0 && event_signals==signals+1 && !event_refs);
        CHECK(wait_public(2,objects,values,0x41,0)==STATUS_INVALID_HANDLE && !event_refs);
        wait_publish_arrays();
        if (!compat) {
            wait_spawn_fail=1;
            CHECK(wait_public(2,objects,values,0x40,0)==STATUS_INSUFFICIENT_RESOURCES && !event_refs);
            wait_wake_fail=1;
            CHECK(wait_public(2,objects,values,0x40,0)==STATUS_INSUFFICIENT_RESOURCES && !event_refs);
            CHECK(wait_spawns==2 && wait_wakes==1);
        }
        signals=event_signals;
        CHECK(wait_public(2,objects,values,0x40,0)==0);
        CHECK(wait_public(2,objects,values,0x40,1)==0 && event_refs==2);
        wait_worker_step();CHECK(wait_worker_ticks==1 && !d3dkmt_wait_idle() && event_signals==signals);
        /* Captured arrays survive the call and later input mutation. */
        objects[0]=objects[1]=0;values[0]=values[1]=0;
        CHECK(d3dkmt_signal_sync_cpu(100,sync_device,1,wait_objects,wait_targets,FALSE)==0);
        wait_worker_step();CHECK(event_refs==1 && event_signals==signals+1 && wait_worker_ticks==1);
        CHECK(d3dkmt_signal_sync_cpu(100,sync_device,2,wait_objects,wait_targets,FALSE)==0);
        /* Completion must remain set even if CPU rewinds before collection. */
        uint64_t zero[2]={0};CHECK(d3dkmt_signal_sync_cpu(100,sync_device,2,wait_objects,zero,TRUE)==0);
        wait_worker_step();CHECK(!event_refs && event_signals==signals+2 && d3dkmt_wait_idle());
        wait_publish_arrays();wait_block_round=0;wait_block_hook=wait_block_signal;
        CHECK(wait_public(2,objects,values,0,0)==0 && wait_block_round==2);
        CHECK(d3dkmt_signal_sync_cpu(100,sync_device,2,wait_objects,zero,TRUE)==0);
        wait_block_round=0;wait_block_hook=wait_block_signal;
        CHECK(wait_public(2,objects,values,0,1)==0 && wait_block_round==1);wait_block_hook=NULL;
        CHECK(d3dkmt_signal_sync_cpu(100,sync_device,2,wait_objects,zero,TRUE)==0);
        unsigned delivered=cancellation_deliveries;wait_block_hook=wait_block_cancel;
        CHECK(wait_public(2,objects,values,0,0)==STATUS_CANCELLED && cancellation_deliveries==delivered+1);
        cancellation_requested=FALSE;wait_worker_step();CHECK(d3dkmt_wait_idle());
        for (unsigned mode=0;mode<10;mode++) {
            wait_publish_arrays();unsigned count=2,flags=0;OKMT_HANDLE *h=objects;uint64_t *v=values;
            if (mode==0) count=0;
            if (mode==1) count=33;
            if (mode==2) h=(void *)(user+4092);
            if (mode==3) v=(void *)(user+4092);
            if (mode==4) objects[1]=0;
            if (mode==5) objects[1]=objects[0];
            if (mode==6) caller=101;
            if (mode==7) flags=2;
            if (mode==8) cancellation_requested=TRUE;
            if (mode==9) sync_device++;
            signals=event_signals;
            CHECK(wait_public(count,h,v,0x40,flags)!=0 && !event_refs && event_signals==signals);
            if (mode==9) sync_device--;
            caller=100;cancellation_requested=FALSE;
        }
        wait_publish_arrays();signals=event_signals;
        for (unsigned i=0;i<D3DKMT_MAX_CPU_WAITS;i++) CHECK(wait_public(2,objects,values,0x40,0)==0);
        CHECK(wait_public(2,objects,values,0x40,0)==STATUS_INSUFFICIENT_RESOURCES && event_refs==D3DKMT_MAX_CPU_WAITS);
        CHECK(d3dkmt_signal_sync_cpu(100,sync_device,2,wait_objects,wait_targets,FALSE)==0);
        wait_worker_step();CHECK(!event_refs && event_signals==signals+D3DKMT_MAX_CPU_WAITS && d3dkmt_wait_idle());
        /* Retained queued signal can satisfy a wait after public destruction. */
        wait_targets[0]++;wait_targets[1]++;wait_publish_arrays();
        D3DKMT_SYNC_COMMAND command={.count=2,.objects={wait_objects[0],wait_objects[1]},.signal=TRUE,.extended=TRUE,.vector_values=TRUE};
        memcpy(command.values,wait_targets,sizeof(wait_targets));D3DKMT_SYNC_LEASE queued={0};
        CHECK(d3dkmt_sync_capture(100,sync_context,&command,&queued)==0);
        CHECK(wait_public(2,objects,values,0x40,0)==0);
        CHECK(d3dkmt_destroy_sync(100,wait_objects[0])==0);
        unsigned publication=D3DKMT_WORK_QUEUED;
        CHECK(d3dkmt_sync_step(&queued,&publication)==0 && d3dkmt_sync_release(&queued)==0);
        signals=event_signals;wait_worker_step();CHECK(event_signals==signals+1 && !event_refs);
        wait_cleanup();
    }
    for (unsigned close=0;close<3;close++) {
        wait_setup(0);wait_publish_arrays();
        CHECK(wait_public(2,(void *)(user+256),(void *)(user+512),0x40,0)==0);
        unsigned signals=event_signals;
        if (close==0) CHECK(d3dkmt_destroy_device(100,sync_device)==0);
        if (close==1) d3dkmt_release_process(100);
        if (close==2) CHECK(d3dkmt_unregister_adapter(ctx.luid)==D3DKMT_STATUS_DEVICE_BUSY);
        wait_worker_step();CHECK(!event_refs && event_signals==signals && d3dkmt_wait_idle());
        if (close!=2) wait_cleanup();
        else {
            d3dkmt_release_process(100);for (unsigned i=0;i<4;i++) (void)d3dkmt_collect_devices();
            CHECK(!ctx.live && munmap(user,user_bytes)==0);user=NULL;user_bytes=0;
        }
    }
}
#ifndef D3DKMT_CPUWAIT_SUITE_ENTRY
#define D3DKMT_CPUWAIT_SUITE_ENTRY main
#endif
int D3DKMT_CPUWAIT_SUITE_ENTRY(void)
{
    if (cpuwait_regression_main()) return 1;
    checks=failures=0;wait_public_cases();
    printf("D3DKMT CPU wait service: checks=%u failures=%u PE32/PE64\n",checks,failures);
    return failures ? 1 : 0;
}
