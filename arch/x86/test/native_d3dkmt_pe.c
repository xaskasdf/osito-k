/* Adapter/UMD discovery and kernel-device lifetime oracle. No rendering,
 * driver installation or Microsoft binaries in the test image.
 * Exit 2 means the OS boundary is absent, not a successful graphics test. */
#include "../win32/d3dkmt_abi.h"

#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
API int CALL FreeLibrary(void *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *, const void *, OKMT_U32, OKMT_U32 *, void *);
API void CALL ExitProcess(OKMT_U32);
API OKMT_U32 CALL SetErrorMode(OKMT_U32);

#define REPORT(text) do { static const char msg[] = text; OKMT_U32 written; \
    WriteFile(GetStdHandle((OKMT_U32)-11), msg, sizeof(msg)-1, &written, 0); } while (0)
#define VALUE(label, value) do { OKMT_U32 number=(OKMT_U32)(value),written; \
    char digits[9]; REPORT(label); \
    for(OKMT_U32 k=0;k<8;k++)digits[k]="0123456789abcdef"[(number>>((7-k)*4))&15]; \
    digits[8]='\n'; WriteFile(GetStdHandle((OKMT_U32)-11),digits,9,&written,0); } while (0)
#define CHECK(test,label) do { checks++; if(!(test)){REPORT("KMT FAIL " label "\n");failures++;} \
    else {REPORT("KMT PASS " label "\n");} } while(0)

void native_d3dkmt_entry(void)
{
    OKMT_U32 checks=0, failures=0;
    SetErrorMode(0x8001);
    VALUE("KMT pointer-bits=", sizeof(void *) * 8);
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=0;
    OKMT_S32 (CALL *query)(const OKMT_QUERY_ADAPTER_INFO *)=0;
    OKMT_S32 (CALL *open_luid)(OKMT_OPEN_ADAPTER_FROM_LUID *)=0;
    OKMT_S32 (CALL *close_adapter)(const OKMT_CLOSE_ADAPTER *)=0;
    OKMT_S32 (CALL *create_device)(OKMT_CREATE_DEVICE *)=0;
    OKMT_S32 (CALL *destroy_device)(const OKMT_DESTROY_DEVICE *)=0;
    OKMT_S32 (CALL *create_context)(OKMT_CREATE_CONTEXT *)=0;
    OKMT_S32 (CALL *destroy_context)(const OKMT_DESTROY_CONTEXT *)=0;
    OKMT_S32 (CALL *render)(OKMT_RENDER *)=0;
    OKMT_S32 (CALL *lock_allocation)(OKMT_LOCK *)=0;
    OKMT_S32 (CALL *unlock_allocation)(const OKMT_UNLOCK *)=0;
    OKMT_S32 (CALL *create_allocation)(OKMT_CREATE_ALLOCATION *)=0;
    OKMT_S32 (CALL *create_allocation2)(OKMT_CREATE_ALLOCATION *)=0;
    OKMT_S32 (CALL *destroy_allocation)(const OKMT_DESTROY_ALLOCATION *)=0;
    OKMT_S32 (CALL *destroy_allocation2)(const OKMT_DESTROY_ALLOCATION2 *)=0;
    if(gdi) {
        enumerate=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
        query=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
        open_luid=GetProcAddress(gdi,"D3DKMTOpenAdapterFromLuid");
        close_adapter=GetProcAddress(gdi,"D3DKMTCloseAdapter");
        create_device=GetProcAddress(gdi,"D3DKMTCreateDevice");
        destroy_device=GetProcAddress(gdi,"D3DKMTDestroyDevice");
        create_context=GetProcAddress(gdi,"D3DKMTCreateContext");
        destroy_context=GetProcAddress(gdi,"D3DKMTDestroyContext");
        render=GetProcAddress(gdi,"D3DKMTRender");
        lock_allocation=GetProcAddress(gdi,"D3DKMTLock");
        unlock_allocation=GetProcAddress(gdi,"D3DKMTUnlock");
        create_allocation=GetProcAddress(gdi,"D3DKMTCreateAllocation");
        create_allocation2=GetProcAddress(gdi,"D3DKMTCreateAllocation2");
        destroy_allocation=GetProcAddress(gdi,"D3DKMTDestroyAllocation");
        destroy_allocation2=GetProcAddress(gdi,"D3DKMTDestroyAllocation2");
    }
    VALUE("KMT enum-export=",enumerate!=0);
    VALUE("KMT query-export=",query!=0);
    VALUE("KMT open-export=",open_luid!=0);
    VALUE("KMT close-export=",close_adapter!=0);
    if(!enumerate || !query || !open_luid || !close_adapter || !create_device || !destroy_device ||
        !create_allocation || !create_allocation2 || !destroy_allocation || !destroy_allocation2 ||
        !lock_allocation || !unlock_allocation || !create_context || !destroy_context || !render) {
        REPORT("KMT UNAVAILABLE adapter-interface\n");
        if(gdi)FreeLibrary(gdi);
        ExitProcess(2);
        return;
    }
#ifdef KMT_EXPECT_EMPTY
    CHECK(render(0)==(OKMT_S32)0xc000000d,"null-render");
    CHECK(create_context(0)==(OKMT_S32)0xc000000d,"null-create-context");
    CHECK(destroy_context(0)==(OKMT_S32)0xc000000d,"null-destroy-context");
    CHECK(lock_allocation(0)==(OKMT_S32)0xc000000d,"null-lock");
    CHECK(unlock_allocation(0)==(OKMT_S32)0xc000000d,"null-unlock");
    CHECK(create_allocation(0)==(OKMT_S32)0xc000000d,"null-create-allocation");
    CHECK(create_allocation2(0)==(OKMT_S32)0xc000000d,"null-create-allocation2");
    CHECK(destroy_allocation(0)==(OKMT_S32)0xc000000d,"null-destroy-allocation");
    CHECK(destroy_allocation2(0)==(OKMT_S32)0xc000000d,"null-destroy-allocation2");
    CHECK(create_device(0)==(OKMT_S32)0xc000000d,"null-create-device");
    CHECK(destroy_device(0)==(OKMT_S32)0xc000000d,"null-destroy-device");
    OKMT_CREATE_DEVICE invalid_device={0};
    invalid_device.object.adapter=1; invalid_device.device=0x4b4d5447;
    CHECK(create_device(&invalid_device)==(OKMT_S32)0xc000000d && invalid_device.device==0x4b4d5447,
          "unknown-device-adapter-preserves-output");
    OKMT_DESTROY_DEVICE invalid_destroy={1};
    CHECK(destroy_device(&invalid_destroy)==(OKMT_S32)0xc000000d,"unknown-device");
    CHECK(enumerate(0)==(OKMT_S32)0xc000000d,"null-enumeration");
    CHECK(query(0)==(OKMT_S32)0xc000000d,"null-query");
    CHECK(open_luid(0)==(OKMT_S32)0xc000000d,"null-open");
    CHECK(close_adapter(0)==(OKMT_S32)0xc000000d,"null-close");
    OKMT_OPEN_ADAPTER_FROM_LUID invalid_open={{0,0},0};
    CHECK(open_luid(&invalid_open)==(OKMT_S32)0xc000000d,"unknown-luid");
    OKMT_CLOSE_ADAPTER invalid_close={1};
    CHECK(close_adapter(&invalid_close)==(OKMT_S32)0xc000000d,"unknown-handle");
    OKMT_U32 invalid_value=0x4b4d5447;
    OKMT_QUERY_ADAPTER_INFO invalid_query={1,OKMT_QUERY_DRIVER_VERSION,&invalid_value,4};
    CHECK(query(&invalid_query)==(OKMT_S32)0xc000000d && invalid_value==0x4b4d5447,
          "invalid-query-preserves-output");
#endif

    OKMT_ALLOCATION_INFO2 allocation_info={0};
    OKMT_RENDER invalid_render;
    for (OKMT_U32 i=0;i<sizeof(invalid_render);i++) ((volatile unsigned char *)&invalid_render)[i]=0;
    invalid_render.context=0xffffffffu; invalid_render.command_length=64; invalid_render.queued=0x4b4d5447;
    CHECK(render(&invalid_render)<0,"invalid-render-context");
    CHECK(invalid_render.queued==0x4b4d5447 && !invalid_render.commands,"failed-render-preserves-output");
    OKMT_CREATE_CONTEXT invalid_context={0};
    invalid_context.device=0xffffffffu; invalid_context.context=0x4b4d5447;
    CHECK(create_context(&invalid_context)<0,"invalid-context-device");
    CHECK(invalid_context.context==0x4b4d5447,"failed-context-preserves-output");
    OKMT_DESTROY_CONTEXT invalid_context_destroy={0xffffffffu};
    CHECK(destroy_context(&invalid_context_destroy)<0,"invalid-context-destroy");
    OKMT_CREATE_ALLOCATION invalid_allocation={0};
    invalid_allocation.device=0xffffffffu; invalid_allocation.count=1;
    invalid_allocation.allocations=&allocation_info;
    OKMT_S32 allocation_status=create_allocation(&invalid_allocation);
    VALUE("KMT invalid-allocation-status=",allocation_status);
    CHECK(allocation_status<0,"invalid-allocation-device");
    allocation_status=create_allocation2(&invalid_allocation);
    VALUE("KMT invalid-allocation2-status=",allocation_status);
    CHECK(allocation_status<0,"invalid-allocation2-device");
    OKMT_HANDLE unknown_allocation=0xffffffffu;
    OKMT_LOCK invalid_lock={0};
    invalid_lock.device=invalid_lock.allocation=0xffffffffu;
    invalid_lock.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT;
    invalid_lock.data=(void *)0x4b4d5447;
    CHECK(lock_allocation(&invalid_lock)<0,"invalid-lock-device");
    CHECK(invalid_lock.data==(void *)0x4b4d5447,"failed-lock-preserves-output");
    OKMT_UNLOCK invalid_unlock={0xffffffffu,1,&unknown_allocation};
    CHECK(unlock_allocation(&invalid_unlock)<0,"invalid-unlock-device");
    OKMT_DESTROY_ALLOCATION invalid_allocation_destroy={0xffffffffu,0,&unknown_allocation,1};
    CHECK(destroy_allocation(&invalid_allocation_destroy)<0,"invalid-allocation-destroy");
    OKMT_DESTROY_ALLOCATION2 invalid_allocation_destroy2={0xffffffffu,0,&unknown_allocation,1,0};
    CHECK(destroy_allocation2(&invalid_allocation_destroy2)<0,"invalid-allocation2-destroy");

    /* Repeat with fresh handles to expose missing cleanup and stale tokens.
     * Static storage avoids a large freestanding x86 stack-probe dependency. */
    static struct {
        OKMT_ADAPTER_INFO items[64];
        OKMT_U32 guard;
    } buffer;
    for(OKMT_U32 round=0;round<2;round++) {
        VALUE("KMT round=",round);
        OKMT_ENUM_ADAPTERS2 list={0,0};
        OKMT_S32 status=enumerate(&list);
        VALUE("KMT count-status=",status);
        VALUE("KMT capacity=",list.count);
        CHECK(status==0,"count-query");
        if(status!=0)break;
        CHECK(list.count<=64,"bounded-adapter-capacity");
        if(list.count>64)break;
        list.count=64;
        list.adapters=buffer.items;
        buffer.guard=0x4b4d5447;
        status=enumerate(&list);
        VALUE("KMT enumerate-status=",status);
        VALUE("KMT count=",list.count);
        CHECK(status==0,"enumerate");
        CHECK(buffer.guard==0x4b4d5447 && list.count<=64,"array-bounds");
#ifdef KMT_EXPECT_EMPTY
        CHECK(list.count==0,"no-unregistered-adapters");
#endif
        if(status!=0 || list.count>64)break;
        for(OKMT_U32 i=0;i<list.count;i++) {
            OKMT_ADAPTER_INFO *a=&buffer.items[i];
            VALUE("KMT adapter-index=",i);
            VALUE("KMT handle=",a->adapter);
            VALUE("KMT luid-high=",a->luid.high);
            VALUE("KMT luid-low=",a->luid.low);
            VALUE("KMT sources=",a->sources);
            CHECK(a->adapter!=0,"enumerated-handle");
            struct { OKMT_U32 value,guard; } result={0,0x4b4d5447};
            OKMT_QUERY_ADAPTER_INFO info={a->adapter,OKMT_QUERY_ADAPTER_TYPE,
                                           &result.value,sizeof(result.value)};
            status=query(&info);
            VALUE("KMT type-status=",status);
            VALUE("KMT type=",result.value);
            CHECK(status==0 && result.guard==0x4b4d5447,"adapter-type");
            /* Capability bits are observations, not inferred from PCI IDs. */
            VALUE("KMT render=",(result.value & OKMT_ADAPTER_RENDER)!=0);
            VALUE("KMT display=",(result.value & OKMT_ADAPTER_DISPLAY)!=0);
            VALUE("KMT software=",(result.value & OKMT_ADAPTER_SOFTWARE)!=0);
            info.type=OKMT_QUERY_DRIVER_VERSION;
            result.value=0;
            status=query(&info);
            VALUE("KMT version-status=",status);
            VALUE("KMT driver-version=",result.value);
            CHECK(status==0 && result.guard==0x4b4d5447,"driver-version");
            if (!round && !i) {
                info.bytes=3;
                status=query(&info);
                VALUE("KMT short-query-status=",status);
                CHECK(status<0,"short-query-rejected");
                info.bytes=sizeof(result);
                status=query(&info);
                VALUE("KMT oversized-query-status=",status);
                CHECK(result.guard==0x4b4d5447,"oversized-query-bounds");
                info.bytes=sizeof(result.value);
            }

            static struct { OKMT_UMD_FILENAME data; OKMT_U32 guard; } umd;
            for(OKMT_U32 n=0;n<260;n++)umd.data.name[n]=0;
            umd.data.version=OKMT_UMD_DX9;
            umd.guard=0x4b4d5447;
            info.type=OKMT_QUERY_UMD_FILENAME;
            info.data=&umd.data;
            info.bytes=sizeof(umd.data);
            status=query(&info);
            VALUE("KMT dx9-umd-status=",status);
            CHECK(umd.guard==0x4b4d5447,"umd-buffer-bounds");
            if(status==0) {
                OKMT_U32 n=0,written;
                static char path[1561];
                OKMT_U32 end=0;
                while(n<260 && umd.data.name[n]) {
                    OKMT_U32 ch=umd.data.name[n++];
                    if(ch>=32 && ch<127)path[end++]=(char)ch;
                    else {
                        path[end++]='\\'; path[end++]='u';
                        for(OKMT_U32 k=0;k<4;k++)
                            path[end++]="0123456789abcdef"[(ch>>((3-k)*4))&15];
                    }
                }
                CHECK(n<260,"umd-name-terminated");
                path[end++]='\n';
                REPORT("KMT dx9-umd=");
                WriteFile(GetStdHandle((OKMT_U32)-11),path,end,&written,0);
            }

            struct { OKMT_QUERY_DEVICE_IDS data; OKMT_U32 guard; } ids={0};
            ids.guard=0x4b4d5447;
            info.type=OKMT_QUERY_DEVICE_IDENTIFIERS;
            info.data=&ids.data;
            info.bytes=sizeof(ids.data);
            status=query(&info);
            VALUE("KMT ids-status=",status);
            CHECK(ids.guard==0x4b4d5447,"ids-buffer-bounds");
            if(status==0) {
                VALUE("KMT vendor=",ids.data.vendor);
                VALUE("KMT device=",ids.data.device);
            }

            OKMT_OPEN_ADAPTER_FROM_LUID opened={a->luid,0};
            status=open_luid(&opened);
            VALUE("KMT reopen-status=",status);
            CHECK(status==0 && opened.adapter!=0,"open-by-luid");
            if(status==0 && opened.adapter) {
                OKMT_CREATE_DEVICE created={0};
                created.object.adapter=opened.adapter;
                OKMT_S32 device_status=create_device(&created);
                VALUE("KMT create-device-status=",device_status);
                VALUE("KMT created-device=",created.device);
                VALUE("KMT command-buffer=",created.command_buffer!=0);
                VALUE("KMT command-bytes=",created.command_bytes);
                VALUE("KMT allocation-list=",created.allocation_list!=0);
                VALUE("KMT allocation-count=",created.allocation_count);
                VALUE("KMT patch-list=",created.patch_list!=0);
                VALUE("KMT patch-count=",created.patch_count);
                CHECK(device_status==0 && created.device!=0,"create-kernel-device");
                OKMT_CLOSE_ADAPTER close={opened.adapter};
                CHECK(close_adapter(&close)==0,"close-reopened-handle");
                if (device_status==0 && created.device) {
                    OKMT_DESTROY_DEVICE destroy={created.device};
                    CHECK(destroy_device(&destroy)==0,"device-survives-adapter-close");
                    CHECK(destroy_device(&destroy)<0,"closed-device-rejected");
                }
                /* The independently enumerated reference must still work. */
                info.type=OKMT_QUERY_DRIVER_VERSION;
                info.data=&result.value;
                info.bytes=sizeof(result.value);
                CHECK(query(&info)==0,"enumerated-reference-survives-close");
            }
            OKMT_CLOSE_ADAPTER close={a->adapter};
            CHECK(close_adapter(&close)==0,"close-enumerated-handle");
            info.type=OKMT_QUERY_DRIVER_VERSION;
            info.data=&result.value;
            info.bytes=sizeof(result.value);
            status=query(&info);
            VALUE("KMT closed-query-status=",status);
            CHECK(status<0,"closed-handle-rejected");
        }
    }
    CHECK(FreeLibrary(gdi),"release-gdi");
    VALUE("KMT DONE checks=",checks);
    VALUE("KMT DONE failures=",failures);
    ExitProcess(failures?1:0);
}
