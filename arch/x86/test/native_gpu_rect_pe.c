/* Real PE32/PE64 KMT rectangle copies, verified after a CPU completion event. */
#define native_gpu_copy_entry unused_copy_entry
#include "native_gpu_copy_pe.c"
#undef native_gpu_copy_entry
static const unsigned rects[][8]={
    {3,5,7,11,5,7,64,128},{0,0,0,0,16,1,64,128},
    {15,31,14,32,1,1,64,128},{4,40,5,50,6,3,64,128},
    {0,4096,0,4097,1,4,4,4},{0,6144,0,6147,1,1,4,4},
    {0,16383,0,17,1,1,4,4},{0,17,0,16383,1,1,4,4}
};
void native_gpu_rect_entry(void)
{
    if (GetCurrentProcessId()==1) {
        char path[512];GPU_STARTUP startup;GPU_PROCESS child;
        gpu_copy_zero(&startup,sizeof(startup));gpu_copy_zero(&child,sizeof(child));startup.bytes=sizeof(startup);
        OKMT_U32 bytes=GetModuleFileNameA(0,path,sizeof(path));
        REQUIRE(bytes && bytes<sizeof(path),"rect-launcher-path");
        REQUIRE(CreateProcessA(path,0,0,0,0,0,0,0,&startup,&child) && child.pid!=1,"rect-child");
        REQUIRE(WaitForSingleObject(child.process,60000)==0,"rect-child-returned");
        OKMT_U32 code=259;REQUIRE(GetExitCodeProcess(child.process,&code),"rect-child-status");
        CloseHandle(child.thread);CloseHandle(child.process);
        if (code==2) {ExitProcess(2);return;}
        REQUIRE(code==0,"rect-child-succeeded");ExitProcess(0);return;
    }
    void *gdi=LoadLibraryA("gdi32.dll");
#define FN(name,type) OKMT_S32 (CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    FN(EnumAdapters2,OKMT_ENUM_ADAPTERS2);FN(QueryAdapterInfo,OKMT_QUERY_ADAPTER_INFO);
    FN(CreateDevice,OKMT_CREATE_DEVICE);FN(CreateContext,OKMT_CREATE_CONTEXT);
    FN(CreateAllocation,OKMT_CREATE_ALLOCATION);FN(Lock,OKMT_LOCK);FN(Unlock,OKMT_UNLOCK);
    FN(Render,OKMT_RENDER);FN(SignalSynchronizationObject2,OKMT_SIGNAL_SYNC2);
    FN(DestroyContext,OKMT_DESTROY_CONTEXT);FN(DestroyDevice,OKMT_DESTROY_DEVICE);
#undef FN
    REQUIRE(gdi && EnumAdapters2 && QueryAdapterInfo && CreateDevice && CreateContext && CreateAllocation &&
        Lock && Unlock && Render && SignalSynchronizationObject2 && DestroyContext && DestroyDevice,"rect-exports");
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(EnumAdapters2(&list)==0 && list.count<=16,"rect-enumerate");
    OKMT_HANDLE adapter=0;intel_umd_adapter_description_t native;
    for (unsigned i=0;i<list.count;i++) {
        gpu_copy_zero(&native,sizeof(native));
        OKMT_QUERY_ADAPTER_INFO q={adapters[i].adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&native,sizeof(native)};
        if (!QueryAdapterInfo(&q) && native.magic==INTEL_UMD_ADAPTER_MAGIC && native.vendor==0x8086 &&
            native.device==0x3e9b) {adapter=adapters[i].adapter;break;}
    }
    if (!adapter) {SAY("RECTCOPY UNAVAILABLE native-prototype\n");ExitProcess(2);return;}
    REQUIRE(native.copy_node<=1,"rect-engine");
    OKMT_CREATE_DEVICE device;gpu_copy_zero(&device,sizeof(device));device.object.adapter=adapter;
    REQUIRE(CreateDevice(&device)==0 && device.device,"rect-device");
    intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
    OKMT_CREATE_CONTEXT context;gpu_copy_zero(&context,sizeof(context));
    context.device=device.device;context.node=native.copy_node;context.private_data=&layout;context.private_bytes=sizeof(layout);
    REQUIRE(CreateContext(&context)==0 && context.command_bytes>=192 && context.allocation_count>=2 &&
        context.patch_count>=2,"rect-context");
    intel_umd_buffer_description_t descriptions[2]={
        {INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,65536,0,0},
        {INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,49152,0,0}};
    OKMT_ALLOCATION_INFO info[2];gpu_copy_zero(info,sizeof(info));
    for (unsigned i=0;i<2;i++) {info[i].private_data=&descriptions[i];info[i].private_bytes=sizeof(descriptions[i]);}
    OKMT_CREATE_ALLOCATION allocation;gpu_copy_zero(&allocation,sizeof(allocation));
    allocation.device=device.device;allocation.count=2;allocation.allocations=info;
    REQUIRE(CreateAllocation(&allocation)==0 && info[0].allocation && info[1].allocation,"rect-resources");
    for (unsigned round=0;round<sizeof(rects)/sizeof(*rects);round++) {
        const unsigned *r=rects[round];
        unsigned swap=round==7;
        OKMT_HANDLE handles[2]={info[swap].allocation,info[swap^1].allocation};
        unsigned lengths[2]={(unsigned)descriptions[swap].bytes,(unsigned)descriptions[swap^1].bytes};
        for (unsigned role=0;role<2;role++) {
            OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;access.allocation=handles[role];
            access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY;
            REQUIRE(Lock(&access)==0 && access.data,"rect-fill-lock");
            volatile unsigned char *data=access.data;
            for (unsigned i=0;i<lengths[role];i++) data[i]=role?0xa6:(unsigned char)(i*13+round*7);
            OKMT_UNLOCK release={device.device,1,&handles[role]};REQUIRE(Unlock(&release)==0,"rect-fill-unlock");
        }
        intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COPY_RECT_VERSION,INTEL_UMD_COPY32,
            sizeof(command),r[4],r[5],r[6],r[7],0,0,{r[0],r[1],r[2],r[3]}};
        OKMT_ALLOCATION_LIST resources[2]={{handles[0],0},{handles[1],1}};
        OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
        gpu_copy_bytes((unsigned char *)context.command_buffer+128,&command,sizeof(command));
        gpu_copy_bytes(context.allocation_list,resources,sizeof(resources));gpu_copy_bytes(context.patch_list,patches,sizeof(patches));
        OKMT_RENDER submission;gpu_copy_zero(&submission,sizeof(submission));submission.context=context.context;
        submission.command_offset=128;submission.command_length=64;submission.allocation_count=2;submission.patch_count=2;
        REQUIRE(Render(&submission)==0,"rect-admitted");
        gpu_copy_zero((unsigned char *)context.command_buffer+128,sizeof(command));
        gpu_copy_zero(context.allocation_list,sizeof(resources));gpu_copy_zero(context.patch_list,sizeof(patches));
        void *event=CreateEventA(0,1,0,0);REQUIRE(event,"rect-event");
        OKMT_SIGNAL_SYNC2 signal;gpu_copy_zero(&signal,sizeof(signal));signal.context=context.context;signal.flags=2;
        signal.values[0]=(OKMT_U64)(__UINTPTR_TYPE__)event;
        REQUIRE(SignalSynchronizationObject2(&signal)==0,"rect-completion-enqueued");
        REQUIRE(WaitForSingleObject(event,5000)==0,"rect-completed");REQUIRE(CloseHandle(event),"rect-event-closed");
        for (unsigned role=0;role<2;role++) {
            OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;access.allocation=handles[role];
            access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            REQUIRE(Lock(&access)==0 && access.data,"rect-result-lock");
            const volatile unsigned char *data=access.data;unsigned equal=1;
            for (unsigned i=0;i<lengths[role];i++) {
                unsigned char expected=(unsigned char)(i*13+round*7);
                if (role) {
                    unsigned y=i/r[7],x=i%r[7];expected=0xa6;
                    if (y>=r[3] && y<r[3]+r[5] && x>=r[2]*4 && x<(r[2]+r[4])*4)
                        expected=(unsigned char)(((r[1]+y-r[3])*r[6]+r[0]*4+x-r[2]*4)*13+round*7);
                }
                if (data[i]!=expected) {equal=0;break;}
            }
            OKMT_UNLOCK release={device.device,1,&handles[role]};REQUIRE(Unlock(&release)==0,"rect-result-unlock");
            REQUIRE(equal,"rect-exact-bytes-and-padding");
        }
    }
    OKMT_DESTROY_CONTEXT close_context={context.context};REQUIRE(DestroyContext(&close_context)==0,"rect-close-context");
    OKMT_DESTROY_DEVICE close_device={device.device};REQUIRE(DestroyDevice(&close_device)==0,"rect-close-device");
    SAY("RECTCOPY DONE copies=8 verified_bytes=917504 failures=0\n");ExitProcess(0);return;
fail:ExitProcess(1);
}
