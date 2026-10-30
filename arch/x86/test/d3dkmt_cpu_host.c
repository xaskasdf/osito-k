/* Real public marshalling, native ownership and service loop. NT mappings and
 * BSP scheduling are modeled; the existing suite also forces AP/exit races. */
#define _GNU_SOURCE
#define CPU_VIEW_SUITE_ENTRY owner_suite_main
#include "intel_gpu_cpu_view_host.c"
#include "../win32/d3dkmt.h"
#include "../win32/d3dkmt_cpu.h"
#include "../kernel/sched_signal.h"
#include <sys/mman.h>
#include <setjmp.h>

static void (*worker_entry)(void);
static sched_signal_t *worker_signal;
static jmp_buf worker_wait;
static unsigned spawn_calls,wake_calls,fail_spawn=1,fail_wake=1,cpu_index;
static uint64_t waited_ticks;
static unsigned char *public_user;
static unsigned writes,deny_write,cancel_checks,cancel_at,checkpoints;
static void (*client_wait_hook)(void);
static unsigned client_waits;
static enum sched_signal_result client_wait_result;
int g_compat32_mode;

uint32_t smp_current_cpu(void) { return cpu_index; }
uint64_t paging_get_kernel_cr3(void) { return 0x123000; }
int sched_spawn_detached_in_address_space_blocked(const char *name,void (*entry)(void),uint64_t cr3,int owns)
{
    CHECK(!strcmp(name,"kmt-cpu") && cr3==0x123000 && !owns && !worker_entry);
    spawn_calls++;
    if (fail_spawn) { fail_spawn=0; return -1; }
    worker_entry=entry;
    return 42;
}
int proc_wake_pid(int pid)
{
    CHECK(pid==42 && worker_entry); wake_calls++;
    if (fail_wake) { fail_wake=0; return -1; }
    return 0;
}
void smp_wake_bsp(void) { }
enum sched_signal_result sched_signal_wait(sched_signal_t *signal,uint64_t ticks)
{
    if (client_wait_hook) {
        CHECK(signal && ticks==1 && !signal->bits && d3dkmt_deferred_current());
        client_waits++; client_wait_hook(); return client_wait_result;
    }
    /* One cooperative service turn per step; preserve the actual wake bits.
     * No claim that this models kernel task switching or TLB invalidation. */
    worker_signal=signal; waited_ticks=ticks;
    __atomic_store_n(&signal->waiters,1,__ATOMIC_RELEASE);
    longjmp(worker_wait,1);
}
static void public_step(void)
{
    CHECK(worker_entry);
    if (worker_signal) __atomic_store_n(&worker_signal->waiters,0,__ATOMIC_RELEASE);
    if (!setjmp(worker_wait)) {
        worker_entry();
        CHECK(false); /* keep the setjmp frame alive; no tail call */
    }
}
DWORD WINAPI GetCurrentProcessId(void) { return model_pid; }
/* This suite exercises memory locks only. Sanitizer global registration may
 * retain the unrelated event-provider table; any actual event call is a bug. */
NTSTATUS ntsync_reference_event_for_process(HANDLE h,DWORD pid,PVOID *object)
{ (void)h;(void)pid;(void)object;CHECK(false);return STATUS_INVALID_HANDLE; }
void ntsync_signal_event_reference(PVOID object,DWORD pid)
{ (void)object;(void)pid;CHECK(false); }
void ntsync_event_release(PVOID object)
{ (void)object;CHECK(false); }
int win32_user_range_readable(const void *p,SIZE_T bytes,BOOL compat)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)public_user;
    return a>=b && bytes<=4096 && a-b<=4096-bytes &&
        (!compat || (a<0x80000000u && bytes<=0x80000000u-a));
}
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{
    return ++writes!=deny_write && win32_user_range_readable(p,bytes,compat);
}
BOOL win32_termination_requested(void)
{
    CHECK(d3dkmt_deferred_current());
    return ++cancel_checks==cancel_at;
}
void win32_main_termination_checkpoint(void)
{
    CHECK(!d3dkmt_deferred_current()); checkpoints++;
}

static NTSTATUS public_lock(OKMT_HANDLE device,OKMT_HANDLE allocation,DWORD flags,uint64_t *address)
{
    writes=cancel_checks=0;
    memset(public_user,0xa5,64);
    if (g_compat32_mode) {
        DWORD *p=(DWORD *)public_user;
        p[0]=device; p[1]=allocation; p[2]=p[3]=p[4]=0;
        p[5]=0xfeed; p[6]=flags; p[8]=p[9]=0;
    } else {
        OKMT_LOCK h={.device=device,.allocation=allocation,.data=(void *)0xfeed,.flags=flags};
        memcpy(public_user,&h,sizeof(h));
    }
    BYTE before[64]; memcpy(before,public_user,64);
    NTSTATUS result=D3DKMTLock(public_user);
    unsigned offset=g_compat32_mode ? 20 : 24,width=g_compat32_mode ? 4 : 8;
    *address=0; memcpy(address,public_user+offset,width);
    if (result!=STATUS_SUCCESS) CHECK(!memcmp(before,public_user,64) && *address==0xfeed);
    else {
        memcpy(before+offset,public_user+offset,width);
        CHECK(!memcmp(before,public_user,64));
        CHECK(*address && *address!=0xfeed && (!g_compat32_mode || *address<0x80000000ULL));
    }
    return result;
}
static NTSTATUS public_unlock(OKMT_HANDLE device,DWORD count,const OKMT_HANDLE *handles)
{
    void *list=public_user+128;
    if (count<=64) memcpy(list,handles,count*sizeof(*handles));
    if (g_compat32_mode) {
        DWORD header[3]={device,count,(DWORD)(uintptr_t)list};
        memcpy(public_user,header,sizeof(header));
    } else {
        OKMT_UNLOCK header={device,count,list}; memcpy(public_user,&header,sizeof(header));
    }
    return D3DKMTUnlock(public_user);
}

static void public_cases(void)
{
    public_user=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(public_user!=MAP_FAILED && (uintptr_t)public_user<0x80000000u);
    const DWORD base=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT;
    for (unsigned bits=0;bits<2;bits++) {
        VIEW_FIXTURE f; view_setup(&f); g_compat32_mode=bits;
        uint64_t address=0;
        CHECK(D3DKMTLock(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTUnlock(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTLock(public_user+4096-16)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock(0,f.allocation,base,&address)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock(f.device,f.allocation,base|3,&address)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock(f.device,f.allocation,base|0x80000000,&address)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock(f.device,f.allocation,base|OKMT_LOCK_ALTERNATE_VA,&address)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock(f.device,f.allocation,OKMT_LOCK_DO_NOT_WAIT,&address)==STATUS_NOT_SUPPORTED);
        CHECK(public_lock(f.device,f.allocation,base|OKMT_LOCK_DISCARD,&address)==STATUS_NOT_SUPPORTED);
        deny_write=1;
        CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_INVALID_PARAMETER && !vm_count);
        deny_write=0; cancel_at=1;
        CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_PROCESS_IS_TERMINATING && !vm_count);
        cancel_at=0;
        if (!bits) {
            cpu_index=1;
            CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_NOT_SUPPORTED && !spawn_calls);
            cpu_index=0;
            CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_INSUFFICIENT_RESOURCES);
            CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_INSUFFICIENT_RESOURCES);
            CHECK(spawn_calls==2 && wake_calls==1 && !vm_count);
        }
        CHECK(public_lock(f.device,f.allocation,base,&address)==0 && vm_count==1);
        CHECK(spawn_calls==2 && wake_calls==2);
        CHECK(public_lock(f.device,f.allocation,base,&address)==(NTSTATUS)0xc01e0102);
        OKMT_HANDLE list[2]={f.allocation,0x7fffffff};
        CHECK(public_unlock(f.device,2,list)==STATUS_INVALID_PARAMETER && vm_count==1);
        list[1]=f.allocation;
        CHECK(public_unlock(f.device,2,list)==STATUS_INVALID_PARAMETER && vm_count==1);
        CHECK(public_unlock(f.device+1,1,list)==STATUS_INVALID_PARAMETER && vm_count==1);
        model_pid=8;
        CHECK(public_unlock(f.device,1,list)==STATUS_INVALID_PARAMETER && vm_count==1);
        model_pid=7;
        CHECK(public_unlock(f.device,65,list)==STATUS_INVALID_PARAMETER && vm_count==1);
        CHECK(public_unlock(f.device,1,list)==0 && !vm_count);
        CHECK(public_unlock(f.device,1,list)==STATUS_INVALID_PARAMETER);
        public_step(); CHECK(waited_ticks==0 && d3dkmt_cpu_idle());
        deny_write=2;
        CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_INVALID_PARAMETER);
        CHECK(!vm_count && !f.memory.resources[0].cpu_holder);
        deny_write=0; cancel_at=2;
        CHECK(public_lock(f.device,f.allocation,base,&address)==STATUS_PROCESS_IS_TERMINATING);
        CHECK(!vm_count && !f.memory.resources[0].cpu_holder); cancel_at=0;
        for (unsigned mode=0;mode<3;mode++) {
            DWORD flags=base|(mode==0 ? OKMT_LOCK_READ_ONLY : mode==1 ? OKMT_LOCK_WRITE_ONLY : OKMT_LOCK_DO_NOT_EVICT);
            CHECK(public_lock(f.device,f.allocation,flags,&address)==0);
            CHECK(public_unlock(f.device,1,list)==0);
        }
        OKMT_HANDLE second=0;
        D3DKMT_ALLOCATION_DESCRIPTION desc={4096};
        CHECK(d3dkmt_create_allocations(7,f.device,1,&desc,&second)==0);
        for (unsigned round=0;round<32;round++) {
            CHECK(public_lock(f.device,f.allocation,base,&address)==0);
            CHECK(public_lock(f.device,second,base,&address)==0 && vm_count==2);
            list[1]=second;
            f.memory.resources[0].busy=true;
            CHECK(public_unlock(f.device,2,list)==D3DKMT_STATUS_DEVICE_BUSY && !vm_count);
            CHECK(f.memory.resources[0].cpu_holder && !f.memory.resources[1].cpu_holder);
            public_step(); CHECK(waited_ticks==1 && !d3dkmt_cpu_idle());
            sched_signal_take(worker_signal); /* retry must not need another hint */
            f.memory.resources[0].busy=false;
            public_step(); CHECK(waited_ticks==0 && d3dkmt_cpu_idle());
            CHECK(!f.memory.resources[0].cpu_holder);
        }
        CHECK(public_lock(f.device,f.allocation,base,&address)==0);
        CHECK(public_lock(f.device,second,base,&address)==0);
        d3dkmt_release_process(7); vm_model_exit(7);
        CHECK(!vm_count && f.memory.resources[0].cpu_holder && !ram.freed);
        public_step(); CHECK(waited_ticks==0 && d3dkmt_cpu_idle());
        CHECK(!f.memory.resources[0].cpu_holder && !f.memory.resources[1].cpu_holder);
        view_cleanup(&f,false);
    }
    CHECK(checkpoints>100 && spawn_calls==2 && wake_calls==2);
    CHECK(!munmap(public_user,4096));
}

static NTSTATUS public_lock2(OKMT_HANDLE device,OKMT_HANDLE allocation,DWORD flags,uint64_t *address)
{
    writes=cancel_checks=0;
    memset(public_user,0xa5,64);
    if (g_compat32_mode) {
        DWORD h[4]={device,allocation,flags,0xfeed};memcpy(public_user,h,sizeof(h));
    } else {
        OKMT_LOCK2 h={device,allocation,flags,(void *)0xfeed};memcpy(public_user,&h,sizeof(h));
    }
    BYTE before[64];memcpy(before,public_user,sizeof(before));
    NTSTATUS result=D3DKMTLock2(public_user);
    unsigned offset=g_compat32_mode ? 12 : 16,width=g_compat32_mode ? 4 : 8;
    *address=0;memcpy(address,public_user+offset,width);
    if (!result) {
        CHECK(*address && *address!=0xfeed && (!g_compat32_mode || *address<0x80000000ULL));
        memcpy(before+offset,public_user+offset,width);
    } else CHECK(*address==0xfeed);
    CHECK(!memcmp(before,public_user,sizeof(before)));
    return result;
}
static NTSTATUS public_unlock2(OKMT_HANDLE device,OKMT_HANDLE allocation)
{
    OKMT_UNLOCK2 input={device,allocation};memcpy(public_user,&input,sizeof(input));
    NTSTATUS status=D3DKMTUnlock2(public_user);
    CHECK(!memcmp(public_user,&input,sizeof(input)));
    return status;
}
static void public_lock2_cases(void)
{
    public_user=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(public_user!=MAP_FAILED);
    for (unsigned bits=0;bits<2;bits++) {
        VIEW_FIXTURE f;view_setup(&f);g_compat32_mode=bits;uint64_t address;
        CHECK(D3DKMTLock2(NULL)==STATUS_INVALID_PARAMETER && D3DKMTUnlock2(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTLock2(public_user+4096-(bits ? 15 : 23))==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTUnlock2(public_user+4089)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock2(0,f.allocation,0,&address)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock2(f.device,0,0,&address)==STATUS_INVALID_PARAMETER);
        for (unsigned bit=0;bit<32;bit++)
            CHECK(public_lock2(f.device,f.allocation,1u<<bit,&address)==STATUS_INVALID_PARAMETER && !vm_count);
        model_pid=8;CHECK(public_lock2(f.device,f.allocation,0,&address)==STATUS_INVALID_PARAMETER);model_pid=7;
        for (unsigned after=1;after<=2;after++) {
            deny_write=after;
            CHECK(public_lock2(f.device,f.allocation,0,&address)==STATUS_INVALID_PARAMETER && !vm_count);
            deny_write=0;cancel_at=after;
            CHECK(public_lock2(f.device,f.allocation,0,&address)==STATUS_PROCESS_IS_TERMINATING && !vm_count);
            cancel_at=0;public_step();CHECK(!f.memory.resources[0].cpu_holder);
        }
        /* An accepted queued GPU dependency must exclude the modern CPU view
         * even before the hardware worker starts executing the job. */
        intel_gpu_kmt_resident_lease_t pending={0};
        CHECK(intel_gpu_kmt_acquire_resident(7,f.device,f.allocation,&pending)==0);
        CHECK(intel_gpu_memory_reserve_work(&pending.native)==INTEL_GPU_BUFFER_OK);
        CHECK(public_lock2(f.device,f.allocation,0,&address)==(NTSTATUS)0xc01e0102 && !vm_count);
        CHECK(intel_gpu_kmt_release_resident(&pending)==0);
        unsigned readonly=vm_readonly_maps;
        CHECK(public_lock2(f.device,f.allocation,0,&address)==0 && vm_count==1 && readonly==vm_readonly_maps);
        CHECK(public_lock(f.device,f.allocation,OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT,&address)==(NTSTATUS)0xc01e0102);
        CHECK(public_lock2(f.device,f.allocation,0,&address)==(NTSTATUS)0xc01e0102);
        model_pid=8;CHECK(public_unlock2(f.device,f.allocation)==STATUS_INVALID_PARAMETER && vm_count==1);model_pid=7;
        CHECK(public_unlock2(f.device+1,f.allocation)==STATUS_INVALID_PARAMETER && vm_count==1);
        CHECK(public_unlock(f.device,1,&f.allocation)==0 && !vm_count);
        CHECK(public_unlock2(f.device,f.allocation)==STATUS_INVALID_PARAMETER);
        CHECK(public_lock(f.device,f.allocation,OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT,&address)==0);
        CHECK(public_unlock2(f.device,f.allocation)==0 && !vm_count);
        for (unsigned round=0;round<16;round++) {
            CHECK(public_lock2(f.device,f.allocation,0,&address)==0);
            f.memory.resources[0].busy=true;
            CHECK(public_unlock2(f.device,f.allocation)==D3DKMT_STATUS_DEVICE_BUSY && !vm_count);
            public_step();CHECK(!d3dkmt_cpu_idle() && f.memory.resources[0].cpu_holder);
            f.memory.resources[0].busy=false;public_step();CHECK(d3dkmt_cpu_idle());
        }
        CHECK(public_lock2(f.device,f.allocation,0,&address)==0);
        CHECK(d3dkmt_destroy_allocations(7,f.device,1,&f.allocation)==0 && vm_count==1 && !ram.freed);
        CHECK(public_unlock2(f.device,f.allocation)==0 && !vm_count);public_step();
        D3DKMT_ALLOCATION_DESCRIPTION desc={4096};
        CHECK(d3dkmt_create_allocations(7,f.device,1,&desc,&f.allocation)==0);
        CHECK(public_lock2(f.device,f.allocation,0,&address)==0);
        d3dkmt_release_process(7);vm_model_exit(7);public_step();
        CHECK(d3dkmt_cpu_idle() && !vm_count);view_cleanup(&f,false);
    }
    CHECK(!munmap(public_user,4096));
}

static VIEW_FIXTURE *blocked_fixture;
static intel_gpu_kmt_resident_lease_t blocked_gpu;
static unsigned blocked_mode;
static void blocking_progress(void)
{
    CHECK(client_waits<=3);
    if (blocked_mode==7 || client_waits<3) return;
    VIEW_FIXTURE *f=blocked_fixture;
    if (blocked_mode==1) cancel_at=cancel_checks+1;
    else if (blocked_mode==2) CHECK(d3dkmt_destroy_device(7,f->device)==0);
    else if (blocked_mode==3) CHECK(d3dkmt_destroy_allocations(7,f->device,1,&f->allocation)==0);
    else if (blocked_mode==5) {d3dkmt_release_process(7);process_closing=true;}
    else if (blocked_mode==8) CHECK(d3dkmt_cpu_unlock(7,f->device,1,&f->allocation)==0);
    else {
        CHECK(intel_gpu_kmt_release_resident(&blocked_gpu)==0);
        if (blocked_mode==4) deny_write=writes+1;
    }
}
static void public_blocking_cases(void)
{
    public_user=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(public_user!=MAP_FAILED);
    for (unsigned bits=0;bits<2;bits++) for (unsigned mode=0;mode<9;mode++) {
        VIEW_FIXTURE f;view_setup(&f);g_compat32_mode=bits;uint64_t address;
        unsigned readonly=vm_readonly_maps;
        CHECK(public_lock(f.device,f.allocation,OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY,&address)==0);
        CHECK(vm_readonly_maps==readonly+1);
        CHECK(public_unlock(f.device,1,&f.allocation)==0 && !vm_count);
        blocked_fixture=&f;blocked_mode=mode;client_waits=0;
        client_wait_result=mode==6 ? SCHED_SIGNAL_RETRY : mode==7 ? SCHED_SIGNAL_INVALID : SCHED_SIGNAL_TIMEOUT;
        if (mode==8) {
            CHECK(public_lock(f.device,f.allocation,OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT,&address)==0);
        } else {
            CHECK(intel_gpu_kmt_acquire_resident(7,f.device,f.allocation,&blocked_gpu)==0);
            CHECK(intel_gpu_memory_reserve_work(&blocked_gpu.native)==INTEL_GPU_BUFFER_OK);
        }
        client_wait_hook=blocking_progress;
        CHECK(public_lock(f.device,f.allocation,OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT,&address)==(NTSTATUS)0xc01e0102);
        CHECK(!client_waits);
        NTSTATUS status=public_lock(f.device,f.allocation,OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY,&address);
        client_wait_hook=NULL;
        CHECK(client_waits==(mode==7 ? 1u : 3u));
        NTSTATUS expected=(mode==1 || mode==5) ? STATUS_PROCESS_IS_TERMINATING :
            (mode>=2 && mode<=4) ? STATUS_INVALID_PARAMETER : mode==7 ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
        CHECK(status==expected);
        if (!status) CHECK(public_unlock(f.device,1,&f.allocation)==0);
        CHECK(!vm_count);
        if (blocked_gpu.reference.self) CHECK(intel_gpu_kmt_release_resident(&blocked_gpu)==0);
        cancel_at=deny_write=0;process_closing=false;
        public_step();CHECK(d3dkmt_cpu_idle());view_cleanup(&f,false);
    }
    CHECK(!munmap(public_user,4096));
}

int main(void)
{
    if (owner_suite_main()) return 1;
    unsigned before=checks;
    public_cases();
    printf("Public KMT CPU service: %u checks passed, PE32/64, 64 deferred batches\n",checks-before);
    before=checks;public_lock2_cases();
    printf("Public KMT Lock2/Unlock2: %u checks passed, PE32/64, GPU hazards and rollback\n",checks-before);
    before=checks;public_blocking_cases();
    printf("Public KMT blocking Lock: %u checks passed, PE32/64, GPU/CPU waits, cancellation and publication rollback\n",checks-before);
    return 0;
}
