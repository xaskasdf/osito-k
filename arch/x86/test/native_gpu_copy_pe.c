/* Real PE client of the Osito native prototype copy driver. No Microsoft
 * binary, Direct3D replacement, kernel helper export or raw GPU address. */
#include "../win32/d3dkmt_abi.h"
#include "../drivers/intel_gpu_umd_abi.h"
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
#define GPUCOPY_ALLOCATION_BUSY ((OKMT_S32)0xc01e0102)
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
API void CALL Sleep(OKMT_U32);
#ifdef GPUCOPY_MONITORED
typedef struct {
    void *base,*allocation;
    OKMT_U32 allocation_protection;
    __UINTPTR_TYPE__ region_bytes;
    OKMT_U32 state,protection,type;
} GPU_MEMORY_INFO;
static OKMT_U64 gpu_monitored_read(void *address)
{
#if __SIZEOF_POINTER__ == 4
    OKMT_U64 value=0;
    __asm__ volatile("lock; cmpxchg8b %1" : "+A"(value), "+m"(*(volatile OKMT_U64 *)address) :
        "b"(0), "c"(0) : "memory", "cc");
    return value;
#else
    return __atomic_load_n((volatile OKMT_U64 *)address,__ATOMIC_ACQUIRE);
#endif
}
#endif
API OKMT_U32 CALL GetCurrentProcessId(void);
API OKMT_U32 CALL GetModuleFileNameA(void *,char *,OKMT_U32);
typedef struct {
    OKMT_U32 bytes;
    char *reserved,*desktop,*title;
    OKMT_U32 x,y,width,height,x_chars,y_chars,fill,flags;
    unsigned short show,reserved_bytes;
    void *reserved_data,*input,*output,*error;
} GPU_STARTUP;
typedef struct { void *process,*thread; OKMT_U32 pid,tid; } GPU_PROCESS;
_Static_assert(sizeof(GPU_STARTUP)==(sizeof(void *)==8 ? 104 : 68),"STARTUPINFO ABI");
API int CALL CreateProcessA(const char *,char *,void *,void *,int,OKMT_U32,void *,const char *,GPU_STARTUP *,GPU_PROCESS *);
API OKMT_U32 CALL WaitForSingleObject(void *,OKMT_U32);
API int CALL GetExitCodeProcess(void *,OKMT_U32 *);
API int CALL CloseHandle(void *);
API void *CALL CreateEventA(void *,int,int,const char *);
API int CALL DuplicateHandle(void *,void *,void *,void **,OKMT_U32,int,OKMT_U32);
#if defined(GPUCOPY_CPU_WAIT) || defined(GPUCOPY_BLOCKING_LOCK)
API void *CALL CreateThread(void *,__UINTPTR_TYPE__,OKMT_U32 (CALL *)(void *),void *,OKMT_U32,OKMT_U32 *);
API int CALL SetEvent(void *);
API int CALL GetExitCodeThread(void *,OKMT_U32 *);
API int CALL TerminateThread(void *,OKMT_U32);
#endif
#ifdef GPUCOPY_BLOCKING_LOCK
typedef struct {
    OKMT_S32 (CALL *lock)(OKMT_LOCK *);
    OKMT_LOCK input;
    void *started;
    unsigned char first[4];
} GPU_LOCK_WAITER;
static OKMT_U32 CALL gpu_lock_wait_thread(void *argument)
{
    GPU_LOCK_WAITER *waiter=argument;
    if (!SetEvent(waiter->started)) return 99;
    OKMT_S32 status=waiter->lock(&waiter->input);
    if (!status && waiter->input.data)
        for (unsigned i=0;i<4;i++) waiter->first[i]=((volatile unsigned char *)waiter->input.data)[i];
    return (OKMT_U32)status;
}
static void gpu_lock_report(const char *label,OKMT_U32 value)
{
    OKMT_U32 bytes=0,written;while(label[bytes])bytes++;
    WriteFile(GetStdHandle((OKMT_U32)-11),label,bytes,&written,0);
    char hex[9];for(unsigned i=0;i<8;i++)hex[i]="0123456789abcdef"[(value>>(28-4*i))&15];hex[8]='\n';
    WriteFile(GetStdHandle((OKMT_U32)-11),hex,sizeof(hex),&written,0);
}
#endif
#ifdef GPUCOPY_CPU_WAIT
typedef struct {
    OKMT_S32 (CALL *wait)(OKMT_WAIT_SYNC_CPU *);
    OKMT_WAIT_SYNC_CPU input;
    OKMT_HANDLE objects[2];
    OKMT_U64 values[2];
    void *started;
} GPU_CPU_WAITER;
static OKMT_U32 CALL gpu_cpu_wait_thread(void *argument)
{
    GPU_CPU_WAITER *waiter=argument;
    if (!SetEvent(waiter->started)) return 99;
    return (OKMT_U32)waiter->wait(&waiter->input);
}
#endif
#define SAY(text) do { static const char message[]=text; OKMT_U32 written; \
    WriteFile(GetStdHandle((OKMT_U32)-11),message,sizeof(message)-1,&written,0); } while(0)
#define REQUIRE(test,label) do { if (!(test)) { SAY("GPUCOPY FAIL " label "\n"); goto fail; } \
    SAY("GPUCOPY PASS " label "\n"); } while(0)

static void gpu_copy_zero(void *data,OKMT_U32 bytes)
{
    volatile unsigned char *p=data;
    for (OKMT_U32 i=0;i<bytes;i++) p[i]=0;
}
static void gpu_copy_bytes(void *dst,const void *src,OKMT_U32 bytes)
{
    volatile unsigned char *d=dst;
    const unsigned char *s=src;
    for (OKMT_U32 i=0;i<bytes;i++) d[i]=s[i];
}
#ifdef GPUCOPY_LOCK2
static OKMT_S32 (CALL *modern_lock)(OKMT_LOCK2 *);
static OKMT_S32 (CALL *modern_unlock)(OKMT_UNLOCK2 *);
static OKMT_S32 CALL gpu_copy_lock2(OKMT_LOCK *old)
{
    OKMT_LOCK2 args={old->device,old->allocation,0,old->data};
    OKMT_S32 status=modern_lock(&args);
    if (!status) old->data=args.data;
    return status;
}
static OKMT_S32 CALL gpu_copy_unlock2(OKMT_UNLOCK *old)
{
    if (old->count!=1) return (OKMT_S32)0xc000000d;
    OKMT_UNLOCK2 args={old->device,old->allocations[0]};
    return modern_unlock(&args);
}
#endif
void native_gpu_copy_entry(void)
{
    /* Default builds exercise child NT processes. GPUCOPY_ROOT instead checks
     * consecutive shell launches with PID 1 and fresh kernel-assigned GPU
     * incarnations. Neither mode claims online hardware-slot reclamation. */
#ifndef GPUCOPY_ROOT
    if (GetCurrentProcessId()==1) {
        char path[512]; GPU_STARTUP startup; GPU_PROCESS child;
        gpu_copy_zero(&startup,sizeof(startup)); gpu_copy_zero(&child,sizeof(child));
        startup.bytes=sizeof(startup);
        OKMT_U32 bytes=GetModuleFileNameA(0,path,sizeof(path));
        REQUIRE(bytes && bytes<sizeof(path),"launcher-path");
        REQUIRE(CreateProcessA(path,0,0,0,0,0,0,0,&startup,&child) && child.pid!=1,"child-process");
        REQUIRE(WaitForSingleObject(child.process,15000)==0,"child-returned");
        OKMT_U32 code=259;
        REQUIRE(GetExitCodeProcess(child.process,&code),"child-status");
        CloseHandle(child.thread); CloseHandle(child.process);
        REQUIRE(code==0,"child-succeeded");
        ExitProcess(0); return;
    }
#else
    REQUIRE(GetCurrentProcessId()==1,"root-process");
#endif
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (CALL *query)(OKMT_QUERY_ADAPTER_INFO *)=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
    OKMT_S32 (CALL *create_device)(OKMT_CREATE_DEVICE *)=GetProcAddress(gdi,"D3DKMTCreateDevice");
    OKMT_S32 (CALL *create_context)(OKMT_CREATE_CONTEXT *)=GetProcAddress(gdi,"D3DKMTCreateContext");
    OKMT_S32 (CALL *allocate)(OKMT_CREATE_ALLOCATION *)=GetProcAddress(gdi,"D3DKMTCreateAllocation");
    OKMT_S32 (CALL *lock)(OKMT_LOCK *)=GetProcAddress(gdi,"D3DKMTLock");
    OKMT_S32 (CALL *unlock)(OKMT_UNLOCK *)=GetProcAddress(gdi,"D3DKMTUnlock");
    OKMT_S32 (CALL *render)(OKMT_RENDER *)=GetProcAddress(gdi,"D3DKMTRender");
    OKMT_S32 (CALL *destroy_context)(OKMT_DESTROY_CONTEXT *)=GetProcAddress(gdi,"D3DKMTDestroyContext");
    OKMT_S32 (CALL *destroy_device)(OKMT_DESTROY_DEVICE *)=GetProcAddress(gdi,"D3DKMTDestroyDevice");
    REQUIRE(gdi && enumerate && query && create_device && create_context && allocate && lock && unlock &&
        render && destroy_context && destroy_device,"public-exports");
#ifdef GPUCOPY_LOCK2
    modern_lock=GetProcAddress(gdi,"D3DKMTLock2");modern_unlock=GetProcAddress(gdi,"D3DKMTUnlock2");
    REQUIRE(modern_lock && modern_unlock,"Lock2-exports");
    lock=gpu_copy_lock2;unlock=gpu_copy_unlock2;
#endif
#if defined(GPUCOPY_PAGING) || defined(GPUCOPY_RESIDENCY) || defined(GPUCOPY_OFFER)
    OKMT_S32 (CALL *create_paging)(OKMT_CREATE_PAGING_QUEUE *)=GetProcAddress(gdi,"D3DKMTCreatePagingQueue");
    OKMT_S32 (CALL *destroy_paging)(OKMT_DESTROY_PAGING_QUEUE *)=GetProcAddress(gdi,"D3DKMTDestroyPagingQueue");
    REQUIRE(create_paging && destroy_paging,"paging-exports");
#endif
    OKMT_ADAPTER_INFO adapters[16];
    OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(enumerate(&list)==0 && list.count<=16,"enumerate");
    OKMT_HANDLE adapter=0;
    for (OKMT_U32 i=0;i<list.count;i++) {
        OKMT_U32 type=0;
        OKMT_QUERY_ADAPTER_INFO q={adapters[i].adapter,OKMT_QUERY_ADAPTER_TYPE,&type,sizeof(type)};
        if (query(&q)!=0 || (type!=OKMT_ADAPTER_PROTOTYPE && type!=OKMT_ADAPTER_DISPLAY)) continue;
        OKMT_QUERY_DEVICE_IDS ids;
        gpu_copy_zero(&ids,sizeof(ids)); q.type=OKMT_QUERY_DEVICE_IDENTIFIERS; q.data=&ids; q.bytes=sizeof(ids);
        if (query(&q)==0 && ids.vendor==0x8086 && ids.device==0x3e9b) adapter=adapters[i].adapter;
    }
    if (!adapter) { SAY("GPUCOPY UNAVAILABLE native-service\n"); ExitProcess(2); return; }
    OKMT_CREATE_DEVICE device;
    gpu_copy_zero(&device,sizeof(device)); device.object.adapter=adapter;
    REQUIRE(create_device(&device)==0 && device.device,"device");
    OKMT_U32 copy_node=INTEL_UMD_NODE_COPY;
#ifdef GPUCOPY_BLOCKING_LOCK
    intel_umd_adapter_description_t native_info;
    gpu_copy_zero(&native_info,sizeof(native_info));
    OKMT_QUERY_ADAPTER_INFO native_query={adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&native_info,sizeof(native_info)};
    REQUIRE(query(&native_query)==0 && native_info.magic==INTEL_UMD_ADAPTER_MAGIC &&
        ((native_info.capabilities==INTEL_UMD_CAP_COPY32 && native_info.copy_node==INTEL_UMD_NODE_COPY) ||
         (native_info.capabilities==INTEL_UMD_CAP_RCS_COPY32 && native_info.copy_node==INTEL_UMD_NODE_RENDER)),"blocking-lock-native-node");
    copy_node=native_info.copy_node;
#endif
    intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
    OKMT_CREATE_CONTEXT context;
    gpu_copy_zero(&context,sizeof(context)); context.device=device.device; context.node=copy_node;
    context.private_data=&layout; context.private_bytes=sizeof(layout);
    REQUIRE(create_context(&context)==0 && context.context && context.command_bytes>=1024 &&
        context.allocation_count>=2 && context.patch_count>=2,"context");
#ifdef GPUCOPY_SYNC
    OKMT_S32 (CALL *create_sync)(OKMT_CREATE_SYNC2 *)=GetProcAddress(gdi,"D3DKMTCreateSynchronizationObject2");
    OKMT_S32 (CALL *wait_sync)(OKMT_WAIT_SYNC2 *)=GetProcAddress(gdi,"D3DKMTWaitForSynchronizationObject2");
    OKMT_S32 (CALL *signal_sync)(OKMT_SIGNAL_SYNC2 *)=GetProcAddress(gdi,"D3DKMTSignalSynchronizationObject2");
    OKMT_S32 (CALL *destroy_sync)(OKMT_DESTROY_SYNC *)=GetProcAddress(gdi,"D3DKMTDestroySynchronizationObject");
    REQUIRE(create_sync && wait_sync && signal_sync && destroy_sync,"sync-exports");
#ifdef GPUCOPY_MONITORED_APIS
    OKMT_S32 (CALL *wait_gpu)(OKMT_SYNC_GPU *)=GetProcAddress(gdi,"D3DKMTWaitForSynchronizationObjectFromGpu");
    OKMT_S32 (CALL *signal_gpu)(OKMT_SYNC_GPU *)=GetProcAddress(gdi,"D3DKMTSignalSynchronizationObjectFromGpu");
    OKMT_S32 (CALL *signal_cpu)(OKMT_SIGNAL_SYNC_CPU *)=GetProcAddress(gdi,"D3DKMTSignalSynchronizationObjectFromCpu");
    REQUIRE(wait_gpu && signal_gpu && signal_cpu,"monitored-modern-exports");
#ifdef GPUCOPY_CPU_WAIT
    OKMT_S32 (CALL *wait_cpu)(OKMT_WAIT_SYNC_CPU *)=GetProcAddress(gdi,"D3DKMTWaitForSynchronizationObjectFromCpu");
    REQUIRE(wait_cpu,"monitored-cpu-wait-export");
#ifdef GPUCOPY_BROADCAST
    OKMT_S32 (CALL *signal_gpu2)(OKMT_SIGNAL_SYNC_GPU2 *)=GetProcAddress(gdi,"D3DKMTSignalSynchronizationObjectFromGpu2");
    REQUIRE(signal_gpu2,"broadcast-export");
#endif
#endif
#endif
    OKMT_CREATE_CONTEXT other;
    gpu_copy_zero(&other,sizeof(other)); other.device=device.device; other.node=copy_node;
    other.private_data=&layout; other.private_bytes=sizeof(layout);
    REQUIRE(create_context(&other)==0 && other.context,"signal-context");
#ifdef GPUCOPY_BROADCAST
    OKMT_CREATE_CONTEXT third;
    gpu_copy_zero(&third,sizeof(third));third.device=device.device;third.node=INTEL_UMD_NODE_COPY;
    third.private_data=&layout;third.private_bytes=sizeof(layout);
    REQUIRE(create_context(&third)==0 && third.context,"broadcast-third-context");
    OKMT_CREATE_SYNC2 gate;
    gpu_copy_zero(&gate,sizeof(gate));gate.device=device.device;gate.info.type=OKMT_SYNC_MONITORED;gate.info.flags=0;
    REQUIRE(create_sync(&gate)==0 && gate.object,"broadcast-gate-fence");
#endif
    OKMT_CREATE_SYNC2 fence;
    gpu_copy_zero(&fence,sizeof(fence)); fence.device=device.device; fence.info.type=OKMT_SYNC_FENCE;
#ifdef GPUCOPY_MONITORED
    fence.info.type=OKMT_SYNC_MONITORED; fence.info.flags=0;
    fence.info.data.monitored.initial=0x1234567800000000ULL;
#endif
    REQUIRE(create_sync(&fence)==0 && fence.object,"create-fence");
#ifdef GPUCOPY_MONITORED
    REQUIRE(fence.info.data.monitored.cpu_address && fence.info.data.monitored.gpu_address>=0x400000000ULL,"monitored-views");
    REQUIRE(gpu_monitored_read(fence.info.data.monitored.cpu_address)==0x1234567800000000ULL,"monitored-initial-atomic-read");
    __UINTPTR_TYPE__ (CALL *query_memory)(const void *,GPU_MEMORY_INFO *,__UINTPTR_TYPE__)=
        GetProcAddress(LoadLibraryA("kernel32.dll"),"VirtualQuery");
    GPU_MEMORY_INFO mapping;
    gpu_copy_zero(&mapping,sizeof(mapping));
    REQUIRE(query_memory && query_memory(fence.info.data.monitored.cpu_address,&mapping,sizeof(mapping))==sizeof(mapping),"monitored-query-view");
    REQUIRE(mapping.protection==(sizeof(void *)==4 ? 4u : 2u),"monitored-ABI-protection");
#ifdef GPUCOPY_MONITORED_APIS
    OKMT_CREATE_SYNC2 second;
    gpu_copy_zero(&second,sizeof(second));second.device=device.device;second.info.type=OKMT_SYNC_MONITORED;second.info.flags=0;
    second.info.data.monitored.initial=0x5566778800000000ULL;
    REQUIRE(create_sync(&second)==0 && second.object,"second-monitored-fence");
#endif
#endif
#endif
    intel_umd_buffer_description_t description={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,1000,0,0};
    OKMT_ALLOCATION_INFO info[2]; OKMT_CREATE_ALLOCATION allocation;
    gpu_copy_zero(info,sizeof(info)); gpu_copy_zero(&allocation,sizeof(allocation));
    for (unsigned role=0;role<2;role++) { info[role].private_data=&description; info[role].private_bytes=sizeof(description); }
    allocation.device=device.device; allocation.count=2; allocation.allocations=info;
    REQUIRE(allocate(&allocation)==0 && info[0].allocation && info[1].allocation,"resources");
    OKMT_HANDLE handles[2]={info[0].allocation,info[1].allocation};
#ifdef GPUCOPY_OFFER
    OKMT_S32 (CALL *offer)(OKMT_OFFER_ALLOCATIONS *)=GetProcAddress(gdi,"D3DKMTOfferAllocations");
    OKMT_S32 (CALL *reclaim)(OKMT_RECLAIM_ALLOCATIONS *)=GetProcAddress(gdi,"D3DKMTReclaimAllocations");
    OKMT_S32 (CALL *reclaim2)(OKMT_RECLAIM_ALLOCATIONS2 *)=GetProcAddress(gdi,"D3DKMTReclaimAllocations2");
    OKMT_S32 (CALL *residency)(OKMT_ALLOCATION_PROPERTIES *)=GetProcAddress(gdi,"D3DKMTQueryAllocationResidency");
    REQUIRE(offer && reclaim && reclaim2 && residency,"offer-exports");
    OKMT_CREATE_PAGING_QUEUE offer_queue={device.device,0,0,0,0,0};
    REQUIRE(create_paging(&offer_queue)==0,"offer-paging-queue");
#endif
#ifdef GPUCOPY_RESIDENCY
    OKMT_S32 (CALL *make_resident)(OKMT_MAKE_RESIDENT *)=GetProcAddress(gdi,"D3DKMTMakeResident");
    OKMT_S32 (CALL *evict)(OKMT_EVICT *)=GetProcAddress(gdi,"D3DKMTEvict");
    REQUIRE(make_resident && evict,"residency-exports");
    OKMT_CREATE_PAGING_QUEUE residency_queue={device.device,0,0,0,0,0};
    REQUIRE(create_paging(&residency_queue)==0,"residency-paging-queue");
#endif
#ifdef GPUCOPY_LOCK2
    for (unsigned bit=0;bit<32;bit++) {
        OKMT_LOCK2 invalid={device.device,handles[0],1u<<bit,(void *)0x1234};
        REQUIRE(modern_lock(&invalid)==(OKMT_S32)0xc000000d && invalid.data==(void *)0x1234,"Lock2-reserved-flags");
    }
#endif
    OKMT_UNLOCK release={device.device,1,&handles[0]};
    for (unsigned round=0;round<2;round++) {
#ifdef GPUCOPY_OFFER
        OKMT_U32 lost[2]={0,0};
        OKMT_ALLOCATION_PROPERTIES offered_state={device.device,0,handles,2,lost};
        OKMT_OFFER_ALLOCATIONS offered={device.device,0,handles,2,4,1};
        REQUIRE(offer(&offered)==0,"offer-before-DMA");
        for (unsigned retry=0;retry<100;retry++) {
            REQUIRE(residency(&offered_state)==0,"offer-residency");
            if (lost[0]==3 && lost[1]==3) break;
            Sleep(10); /* Previous round's completed jobs may still be retiring. */
        }
        REQUIRE(lost[0]==3 && lost[1]==3,"offer-physical-pages-released");
        if (round) {
            OKMT_RECLAIM_ALLOCATIONS2 take={offer_queue.queue,2,0,handles,lost,0xfeed};
            REQUIRE(reclaim2(&take)==0 && !take.fence,"offer-reclaim2-before-DMA");
        } else {
            OKMT_RECLAIM_ALLOCATIONS take={device.device,0,handles,lost,2};
            REQUIRE(reclaim(&take)==0,"offer-reclaim-before-DMA");
        }
        REQUIRE(lost[0]==1 && lost[1]==1,"offer-loss-reported");
#endif
#ifdef GPUCOPY_RESIDENCY
        OKMT_HANDLE duplicated[4]={handles[0],handles[1],handles[0],handles[1]};
        OKMT_MAKE_RESIDENT retain={residency_queue.queue,4,duplicated,(const OKMT_U32 *)1,round,0xfeed,0xfeed};
        REQUIRE(make_resident(&retain)==0 && retain.count==4 && !retain.fence && !retain.trim,"residency-duplicate-make");
        OKMT_EVICT drop={device.device,4,duplicated,round,0xfeed};
        REQUIRE(evict(&drop)==0 && drop.trim==(sizeof(void *)==4?0xfeed:0),"residency-duplicate-evict");
        drop.count=2;drop.allocations=handles;drop.flags=round+2;
        REQUIRE(evict(&drop)==0 && evict(&drop)==0,"residency-evict-to-zero");
        retain.count=2;retain.allocations=handles;retain.flags=round+2;retain.fence=retain.trim=0xfeed;
        REQUIRE(make_resident(&retain)==0 && retain.count==2 && !retain.fence && !retain.trim,"residency-restore-before-DMA");
#endif
        for (unsigned role=0;role<2;role++) {
            OKMT_LOCK access;
            gpu_copy_zero(&access,sizeof(access)); access.device=device.device; access.allocation=handles[role];
            access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            OKMT_S32 status=GPUCOPY_ALLOCATION_BUSY;
            for (unsigned wait=0;wait<300 && status==GPUCOPY_ALLOCATION_BUSY;wait++) {
                status=lock(&access); if (status==GPUCOPY_ALLOCATION_BUSY) Sleep(10);
            }
            REQUIRE(status==0 && access.data,"CPU-fill-lock");
            volatile unsigned char *data=access.data;
#ifdef GPUCOPY_OFFER
            unsigned cleared=1;for (unsigned i=0;i<1000;i++) if (data[i]) cleared=0;
            REQUIRE(cleared,"offer-replacement-zeroed");
#endif
            for (unsigned i=0;i<1000;i++) data[i]=role ? 0xa6 : (unsigned char)(i*13+round*7);
#ifdef GPUCOPY_EXIT_PENDING
            if (role==1) continue; /* exit must revoke this CPU view and cancel queued work */
#endif
            release.allocations=&handles[role]; REQUIRE(unlock(&release)==0,"CPU-fill-unlock");
        }
        intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COMMAND_VERSION,
            INTEL_UMD_COPY32,sizeof(command),13,7,80,96,0,0,{0}};
        OKMT_ALLOCATION_LIST resources[2]={{handles[0],0},{handles[1],1}};
        OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
        gpu_copy_bytes((unsigned char *)context.command_buffer+128,&command,sizeof(command));
        gpu_copy_bytes(context.allocation_list,resources,sizeof(resources));
        gpu_copy_bytes(context.patch_list,patches,sizeof(patches));
        OKMT_RENDER submission;
        gpu_copy_zero(&submission,sizeof(submission)); submission.context=context.context;
        submission.command_offset=128; submission.command_length=sizeof(command);
        submission.allocation_count=2; submission.patch_count=2;
#ifdef GPUCOPY_SYNC
        OKMT_U64 sync_target=round+1;
#ifdef GPUCOPY_MONITORED
        sync_target+=0x1234567800000000ULL;
#endif
        OKMT_WAIT_SYNC2 wait;
        gpu_copy_zero(&wait,sizeof(wait)); wait.context=context.context; wait.count=1;
        wait.objects[0]=fence.object; wait.values[0]=sync_target;
#ifdef GPUCOPY_MONITORED_APIS
        OKMT_HANDLE sync_handles[2]={fence.object,second.object};
        OKMT_U64 sync_values[2]={sync_target,0x5566778800000000ULL+round+1};
        OKMT_SYNC_GPU modern;
        gpu_copy_zero(&modern,sizeof(modern));modern.context=context.context;modern.count=2;
        modern.objects=sync_handles;modern.data.values=sync_values;
        REQUIRE(wait_gpu(&modern)==0,"queue-unsatisfied-wait");
#else
        REQUIRE(wait_sync(&wait)==0,"queue-unsatisfied-wait");
#endif
#endif
#ifdef GPUCOPY_PAGING
        OKMT_CREATE_PAGING_QUEUE paging={device.device,(OKMT_S32)round,0,0,0,0};
        REQUIRE(create_paging(&paging)==0 && paging.queue && paging.sync && paging.cpu_address,"paging-create");
        OKMT_WAIT_SYNC2 page_wait;
        gpu_copy_zero(&page_wait,sizeof(page_wait));page_wait.context=context.context;page_wait.count=1;
        page_wait.objects[0]=paging.sync;page_wait.values[0]=~(OKMT_U64)0;
        REQUIRE(wait_sync(&page_wait)==0,"paging-queued-wait");
#endif
        REQUIRE(render(&submission)==0,"Render-admission");
        gpu_copy_zero((unsigned char *)context.command_buffer+128,sizeof(command));
#ifdef GPUCOPY_SYNC
        void *event=CreateEventA(0,1,0,0),*observer=0;
        REQUIRE(event && DuplicateHandle((void *)-1,event,(void *)-1,&observer,0,0,2),"event-duplicate");
        OKMT_SIGNAL_SYNC2 notify;
        gpu_copy_zero(&notify,sizeof(notify)); notify.context=context.context; notify.flags=2;
        notify.values[0]=(OKMT_U64)(__UINTPTR_TYPE__)event;
        REQUIRE(signal_sync(&notify)==0,"enqueue-cpu-event");
        REQUIRE(CloseHandle(event),"close-queued-event-handle");
        void *replacement=CreateEventA(0,1,0,0);
        REQUIRE(replacement!=0,"replacement-event");
        Sleep(100);
        REQUIRE(WaitForSingleObject(observer,0)==258,"event-unsignaled-before-GPU");
#ifdef GPUCOPY_PAGING
        OKMT_DESTROY_PAGING_QUEUE close_paging={paging.queue};
        REQUIRE(destroy_paging(&close_paging)==0,"paging-destroy-while-queued");
#endif
        OKMT_LOCK before;
        gpu_copy_zero(&before,sizeof(before)); before.device=device.device; before.allocation=handles[1];
        before.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
        /* A queued write is still a dependency while its context waits on a
         * fence. A normal Lock must not expose the old destination bytes. */
        REQUIRE(lock(&before)==(OKMT_S32)0xc01e0102 && !before.data,"blocked-copy-denies-CPU-lock");
#ifdef GPUCOPY_BLOCKING_LOCK
        GPU_LOCK_WAITER locked[2];void *lock_threads[2];
        gpu_copy_zero(locked,sizeof(locked));
        for (unsigned i=0;i<2;i++) {
            locked[i].lock=lock;locked[i].input=before;
            locked[i].input.flags&=~OKMT_LOCK_DO_NOT_WAIT;
            locked[i].started=CreateEventA(0,1,0,0);
            REQUIRE(locked[i].started,"blocking-lock-event");
            lock_threads[i]=CreateThread(0,0,gpu_lock_wait_thread,&locked[i],0,0);
            REQUIRE(lock_threads[i] && WaitForSingleObject(locked[i].started,1000)==0,"blocking-lock-started");
            REQUIRE(WaitForSingleObject(lock_threads[i],50)==258 && !locked[i].input.data,"blocking-lock-waits-for-GPU");
            if (!i) {
                OKMT_U32 code=259;
                REQUIRE(TerminateThread(lock_threads[i],73),"blocking-lock-cancel");
                REQUIRE(WaitForSingleObject(lock_threads[i],2000)==0 && GetExitCodeThread(lock_threads[i],&code) && code==73 && !locked[i].input.data,"blocking-lock-cancelled-no-view");
                REQUIRE(CloseHandle(lock_threads[i]) && CloseHandle(locked[i].started),"blocking-lock-cancel-cleanup");
            }
        }
#endif
#ifdef GPUCOPY_CPU_WAIT
        void *cpu_all=CreateEventA(0,1,0,0),*cpu_any=CreateEventA(0,1,0,0),*cpu_observer=0;
        REQUIRE(cpu_all && cpu_any && DuplicateHandle((void *)-1,cpu_all,(void *)-1,&cpu_observer,0,0,2),"cpu-wait-events");
        OKMT_HANDLE cpu_objects[2]={fence.object,second.object};
        OKMT_U64 cpu_values[2]={sync_values[0],sync_values[1]};
        OKMT_WAIT_SYNC_CPU cpu_wait={device.device,2,cpu_objects,cpu_values,cpu_all,0};
        REQUIRE(wait_cpu(&cpu_wait)==0,"cpu-wait-async-all-admitted");
        cpu_wait.event=cpu_any;cpu_wait.flags=1;
        REQUIRE(wait_cpu(&cpu_wait)==0,"cpu-wait-async-any-admitted");
        cpu_objects[0]=cpu_objects[1]=0;cpu_values[0]=cpu_values[1]=0;
        REQUIRE(CloseHandle(cpu_all),"cpu-wait-close-retained-event");
        void *cpu_replacement=CreateEventA(0,1,0,0);
        REQUIRE(cpu_replacement,"cpu-wait-replacement-event");
        GPU_CPU_WAITER cpu_waiters[2];void *cpu_threads[2];
        gpu_copy_zero(cpu_waiters,sizeof(cpu_waiters));
        for (unsigned i=0;i<2;i++) {
            GPU_CPU_WAITER *w=&cpu_waiters[i];w->wait=wait_cpu;
            w->objects[0]=fence.object;w->objects[1]=second.object;
            w->values[0]=sync_values[0];w->values[1]=sync_values[1];
            w->input=(OKMT_WAIT_SYNC_CPU){device.device,2,w->objects,w->values,0,i};
            w->started=CreateEventA(0,1,0,0);
            cpu_threads[i]=CreateThread(0,0,gpu_cpu_wait_thread,w,0,0);
            REQUIRE(w->started && cpu_threads[i] && WaitForSingleObject(w->started,1000)==0,"cpu-wait-thread-started");
            REQUIRE(WaitForSingleObject(cpu_threads[i],20)==258,"cpu-wait-thread-blocked");
        }
        REQUIRE(WaitForSingleObject(cpu_observer,0)==258 && WaitForSingleObject(cpu_any,0)==258,"cpu-wait-events-pending");
#endif
        OKMT_SIGNAL_SYNC2 signal;
        gpu_copy_zero(&signal,sizeof(signal)); signal.context=other.context; signal.count=1;
        signal.objects[0]=fence.object; signal.values[0]=sync_target;
#ifdef GPUCOPY_MONITORED_APIS
#ifdef GPUCOPY_BROADCAST
        OKMT_HANDLE gate_handle=gate.object;OKMT_U64 gate_value=round+1;
        OKMT_SYNC_GPU gate_wait;
        gpu_copy_zero(&gate_wait,sizeof(gate_wait));gate_wait.context=third.context;gate_wait.count=1;
        gate_wait.objects=&gate_handle;gate_wait.data.values=&gate_value;
        REQUIRE(wait_gpu(&gate_wait)==0,"broadcast-block-secondary-context");
        OKMT_HANDLE members[2]={other.context,third.context};
        OKMT_SIGNAL_SYNC_GPU2 broadcast;
        gpu_copy_zero(&broadcast,sizeof(broadcast));broadcast.count=2;broadcast.objects=sync_handles;
        broadcast.context_count=2;broadcast.contexts=members;broadcast.data.values=sync_values;
        REQUIRE(signal_gpu2(&broadcast)==0,"broadcast-signal-admitted");
        void *group_event=CreateEventA(0,1,0,0),*group_observer=0;
        REQUIRE(group_event && DuplicateHandle((void *)-1,group_event,(void *)-1,&group_observer,0,0,2),"broadcast-event-duplicate");
        broadcast.count=0;broadcast.flags=2;broadcast.data.event=group_event;
        REQUIRE(signal_gpu2(&broadcast)==0,"broadcast-event-admitted");
        REQUIRE(CloseHandle(group_event),"broadcast-close-event-handle");
        void *group_replacement=CreateEventA(0,1,0,0);
        REQUIRE(group_replacement,"broadcast-replacement-event");
        members[0]=members[1]=0;sync_handles[0]=sync_handles[1]=0;sync_values[0]=sync_values[1]=0;
        Sleep(100);
        REQUIRE(WaitForSingleObject(observer,0)==258 && WaitForSingleObject(group_observer,0)==258 &&
            WaitForSingleObject(cpu_observer,0)==258 && WaitForSingleObject(cpu_any,0)==258,"broadcast-waits-for-every-context");
        OKMT_SIGNAL_SYNC_CPU gate_signal={device.device,1,&gate_handle,&gate_value,0};
        REQUIRE(signal_cpu(&gate_signal)==0,"broadcast-release-secondary-context");
        REQUIRE(WaitForSingleObject(group_observer,5000)==0,"broadcast-event-completed");
        REQUIRE(WaitForSingleObject(group_replacement,0)==258,"broadcast-replacement-unsignaled");
        REQUIRE(CloseHandle(group_observer) && CloseHandle(group_replacement),"broadcast-event-cleanup");
#else
        if (!round) {
            OKMT_SIGNAL_SYNC_CPU cpu_signal={device.device,1,sync_handles,sync_values,0};
            REQUIRE(signal_cpu(&cpu_signal)==0,"monitored-signal-first-CPU");
            REQUIRE(WaitForSingleObject(observer,50)==258,"monitored-wait-needs-all-values");
#ifdef GPUCOPY_CPU_WAIT
            REQUIRE(WaitForSingleObject(cpu_any,1000)==0 && WaitForSingleObject(cpu_threads[1],1000)==0,"cpu-wait-any-first-value");
            REQUIRE(WaitForSingleObject(cpu_observer,0)==258 && WaitForSingleObject(cpu_threads[0],20)==258,"cpu-wait-all-still-pending");
#endif
            cpu_signal.count=2;
            REQUIRE(signal_cpu(&cpu_signal)==0,"monitored-signal-both-CPU");
        } else {
            modern.context=other.context;
            REQUIRE(signal_gpu(&modern)==0,"monitored-signal-both-GPU");
        }
        sync_handles[0]=sync_handles[1]=0;sync_values[0]=sync_values[1]=0;
#endif
#else
        REQUIRE(signal_sync(&signal)==0,"signal-other-context");
#endif
        REQUIRE(WaitForSingleObject(observer,5000)==0,"event-after-GPU-retirement");
#ifdef GPUCOPY_BLOCKING_LOCK
        OKMT_U32 lock_code=259;
        REQUIRE(WaitForSingleObject(lock_threads[1],2000)==0 && GetExitCodeThread(lock_threads[1],&lock_code) && !lock_code && locked[1].input.data,"blocking-lock-after-GPU");
        unsigned lock_equal=1;
        for (unsigned i=0;i<1000;i++) {
            unsigned row=i/96,col=i%96;
            unsigned char expected=row<7 && col<52 ? (unsigned char)((row*80+col)*13+round*7) : 0xa6;
            unsigned char actual=((volatile unsigned char *)locked[1].input.data)[i];
            if (actual!=expected) {
                gpu_lock_report("GPUCOPY blocking mismatch offset=",i);
                gpu_lock_report("GPUCOPY blocking mismatch actual=",actual);
                gpu_lock_report("GPUCOPY blocking mismatch expected=",expected);
                gpu_lock_report("GPUCOPY blocking waiter first=",locked[1].first[0]);
                lock_equal=0;break;
            }
        }
        REQUIRE(lock_equal,"blocking-lock-sees-GPU-write");
        release.allocations=&handles[1];REQUIRE(unlock(&release)==0,"blocking-lock-unlock");
        REQUIRE(CloseHandle(lock_threads[1]) && CloseHandle(locked[1].started),"blocking-lock-thread-cleanup");
#endif
#ifdef GPUCOPY_CPU_WAIT
        REQUIRE(WaitForSingleObject(cpu_observer,1000)==0 && WaitForSingleObject(cpu_any,1000)==0,"cpu-wait-events-completed");
        REQUIRE(WaitForSingleObject(cpu_replacement,0)==258,"cpu-wait-replacement-unsignaled");
        for (unsigned i=0;i<2;i++) {
            OKMT_U32 code=259;
            REQUIRE(WaitForSingleObject(cpu_threads[i],1000)==0 && GetExitCodeThread(cpu_threads[i],&code) && !code,"cpu-wait-thread-completed");
            REQUIRE(CloseHandle(cpu_threads[i]) && CloseHandle(cpu_waiters[i].started),"cpu-wait-thread-cleanup");
        }
        REQUIRE(CloseHandle(cpu_observer) && CloseHandle(cpu_any) && CloseHandle(cpu_replacement),"cpu-wait-events-cleanup");
#endif
#ifdef GPUCOPY_MONITORED
        REQUIRE(gpu_monitored_read(fence.info.data.monitored.cpu_address)==sync_target,"monitored-value-after-retirement");
#ifdef GPUCOPY_MONITORED_APIS
        REQUIRE(gpu_monitored_read(second.info.data.monitored.cpu_address)==0x5566778800000000ULL+round+1,"monitored-second-value-after-retirement");
#endif
#endif
        REQUIRE(WaitForSingleObject(replacement,0)==258,"replacement-event-unsignaled");
        REQUIRE(CloseHandle(observer) && CloseHandle(replacement),"event-cleanup");
#endif
#ifdef GPUCOPY_EXIT_PENDING
        SAY("GPUCOPY EXIT pending-copy-with-CPU-view\n"); ExitProcess(0); return;
#endif
        unsigned equal=0;
        /* Until public fences are connected, a bounded test polls exclusive
         * CPU ownership. It never treats Render admission as completion. */
        for (unsigned attempt=0;attempt<300 && !equal;attempt++) {
            Sleep(10);
            OKMT_LOCK access;
            gpu_copy_zero(&access,sizeof(access)); access.device=device.device; access.allocation=handles[1];
            access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            OKMT_S32 status=lock(&access);
            if (status==GPUCOPY_ALLOCATION_BUSY) continue;
            if (status) {
                static char message[]="GPUCOPY result-lock status=0x00000000\n";
                static const char digits[]="0123456789abcdef";
                for (unsigned i=0;i<8;i++) message[sizeof(message)-10+i]=digits[((OKMT_U32)status>>(28-i*4))&15];
                OKMT_U32 written;
                WriteFile(GetStdHandle((OKMT_U32)-11),message,sizeof(message)-1,&written,0);
            }
            REQUIRE(status==0 && access.data,"CPU-result-lock");
            const volatile unsigned char *actual=access.data; equal=1;
            for (unsigned i=0;i<1000;i++) {
                unsigned row=i/96,col=i%96;
                unsigned char expected=row<7 && col<52 ? (unsigned char)((row*80+col)*13+round*7) : 0xa6;
                if (actual[i]!=expected) equal=0;
            }
            release.allocations=&handles[1]; REQUIRE(unlock(&release)==0,"CPU-result-unlock");
        }
        REQUIRE(equal,"GPU-bytes-and-padding");
    }
#ifdef GPUCOPY_CPU_WAIT
    GPU_CPU_WAITER cancelled;
    gpu_copy_zero(&cancelled,sizeof(cancelled));cancelled.wait=wait_cpu;
    cancelled.objects[0]=fence.object;cancelled.objects[1]=second.object;
    cancelled.values[0]=0x1234567800000003ULL;cancelled.values[1]=0x5566778800000003ULL;
    cancelled.input=(OKMT_WAIT_SYNC_CPU){device.device,2,cancelled.objects,cancelled.values,0,0};
    cancelled.started=CreateEventA(0,1,0,0);
    void *cancel_thread=CreateThread(0,0,gpu_cpu_wait_thread,&cancelled,0,0);
    REQUIRE(cancelled.started && cancel_thread && WaitForSingleObject(cancelled.started,1000)==0,"cpu-wait-cancel-started");
    REQUIRE(WaitForSingleObject(cancel_thread,50)==258,"cpu-wait-cancel-pending");
    REQUIRE(TerminateThread(cancel_thread,73),"cpu-wait-cancel-request");
    OKMT_U32 cancel_code=259;
    REQUIRE(WaitForSingleObject(cancel_thread,2000)==0 && GetExitCodeThread(cancel_thread,&cancel_code) && cancel_code==73,"cpu-wait-cancel-returned");
    REQUIRE(CloseHandle(cancel_thread) && CloseHandle(cancelled.started),"cpu-wait-cancel-cleanup");
#ifdef GPUCOPY_CPU_WAIT_EXIT
    cancelled.input.event=CreateEventA(0,1,0,0);
    REQUIRE(cancelled.input.event && wait_cpu(&cancelled.input)==0,"cpu-wait-exit-pending");
    REQUIRE(CloseHandle(cancelled.input.event),"cpu-wait-exit-close-event");
    SAY("GPUCOPY EXIT pending-cpu-wait copies=2 verified_bytes=2000 failures=0\n");ExitProcess(0);return;
#endif
#endif
#ifdef GPUCOPY_RESIDENCY
    OKMT_DESTROY_PAGING_QUEUE close_residency={residency_queue.queue};
    REQUIRE(destroy_paging(&close_residency)==0,"residency-destroy-queue");
#endif
#ifdef GPUCOPY_OFFER
    OKMT_DESTROY_PAGING_QUEUE close_offer={offer_queue.queue};
    REQUIRE(destroy_paging(&close_offer)==0,"offer-destroy-queue");
#endif
    OKMT_DESTROY_CONTEXT close_context={context.context};
    REQUIRE(destroy_context(&close_context)==0,"destroy-context");
#ifdef GPUCOPY_SYNC
    close_context.context=other.context; REQUIRE(destroy_context(&close_context)==0,"destroy-signal-context");
    OKMT_DESTROY_SYNC close_sync={fence.object}; REQUIRE(destroy_sync(&close_sync)==0,"destroy-fence");
#ifdef GPUCOPY_MONITORED_APIS
    close_sync.object=second.object;REQUIRE(destroy_sync(&close_sync)==0,"destroy-second-fence");
#ifdef GPUCOPY_BROADCAST
    close_context.context=third.context;REQUIRE(destroy_context(&close_context)==0,"broadcast-destroy-third-context");
    close_sync.object=gate.object;REQUIRE(destroy_sync(&close_sync)==0,"broadcast-destroy-gate");
#endif
#endif
#endif
    OKMT_DESTROY_DEVICE close_device={device.device};
    REQUIRE(destroy_device(&close_device)==0,"destroy-device");
    SAY("GPUCOPY DONE copies=2 verified_bytes=2000 failures=0\n"); ExitProcess(0); return;
fail:
    ExitProcess(1);
}
