/* Differential Windows/P1 contract; no render command or third-party binary. */
#include "../win32/d3dkmt_abi.h"
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
static unsigned checks,failures;
void *memset(void *destination,int value,__SIZE_TYPE__ bytes)
{ volatile unsigned char *p=destination;for(__SIZE_TYPE__ i=0;i<bytes;i++)p[i]=(unsigned char)value;return destination; }
static void report(const char *s)
{ OKMT_U32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((OKMT_U32)-11),s,n,&w,0); }
#define CHECK(x) do {checks++;if(!(x)){failures++;report("DEVICE-ERROR FAIL " #x "\n");}} while(0)
#define REQUIRE(x) do {CHECK(x);if(failures)ExitProcess(1);} while(0)
static void number(unsigned x)
{char s[9];for(unsigned i=0;i<8;i++)s[i]="0123456789abcdef"[(x>>(28-4*i))&15];s[8]=0;report(s);}
void device_error_entry(void)
{
    void *gdi=LoadLibraryA("gdi32.dll"),*kernel=LoadLibraryA("kernel32.dll");
    void *(CALL *CreateEventA)(void *,int,int,const char *)=GetProcAddress(kernel,"CreateEventA");
    OKMT_U32 (CALL *WaitForSingleObject)(void *,OKMT_U32)=GetProcAddress(kernel,"WaitForSingleObject");
    int (CALL *CloseHandle)(void *)=GetProcAddress(kernel,"CloseHandle");
    void (CALL *Sleep)(OKMT_U32)=GetProcAddress(kernel,"Sleep");
    unsigned pressure_retries=0;
#define RESOLVE(name,type) OKMT_S32 (CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    RESOLVE(EnumAdapters2,OKMT_ENUM_ADAPTERS2);RESOLVE(QueryAdapterInfo,OKMT_QUERY_ADAPTER_INFO);
    RESOLVE(CreateDevice,OKMT_CREATE_DEVICE);RESOLVE(DestroyDevice,OKMT_DESTROY_DEVICE);
    RESOLVE(CloseAdapter,OKMT_CLOSE_ADAPTER);RESOLVE(GetDeviceState,OKMT_DEVICE_EXECUTION);
    RESOLVE(MarkDeviceAsError,OKMT_MARK_DEVICE_ERROR);RESOLVE(CreateSynchronizationObject2,OKMT_CREATE_SYNC2);
    RESOLVE(CreateSynchronizationObject,OKMT_CREATE_SYNC);RESOLVE(DestroySynchronizationObject,OKMT_DESTROY_SYNC);
    RESOLVE(WaitForSynchronizationObjectFromCpu,OKMT_WAIT_SYNC_CPU);
    RESOLVE(SignalSynchronizationObjectFromCpu,OKMT_SIGNAL_SYNC_CPU);
#undef RESOLVE
    REQUIRE(gdi && EnumAdapters2 && QueryAdapterInfo && CreateDevice && DestroyDevice && CloseAdapter && GetDeviceState &&
        MarkDeviceAsError && CreateSynchronizationObject2 && CreateSynchronizationObject && DestroySynchronizationObject &&
        WaitForSynchronizationObjectFromCpu && SignalSynchronizationObjectFromCpu && CreateEventA && WaitForSingleObject && CloseHandle && Sleep);
    OKMT_MARK_DEVICE_ERROR invalid={0,0};CHECK(MarkDeviceAsError(&invalid)!=0);
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(EnumAdapters2(&list)==0 && list.count<=16);
    OKMT_HANDLE selected=0;
    for (unsigned i=0;i<list.count;i++) {
        OKMT_QUERY_DEVICE_IDS ids={0};OKMT_QUERY_ADAPTER_INFO q={adapters[i].adapter,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids)};
        if (QueryAdapterInfo(&q)==0 && ids.vendor==0x8086 && ids.device==0x3e9b) selected=adapters[i].adapter;
    }
#ifdef DEVICE_ERROR_WINDOWS
    if(list.count)selected=adapters[0].adapter;
#endif
#if defined(DEVICE_ERROR_PHYSICAL) || defined(DEVICE_ERROR_WINDOWS)
    REQUIRE(selected);
#else
    REQUIRE(!list.count);
#endif
    const OKMT_U32 reasons[]={0x80000000,0x80000006,0,0xffffffff,1,2,3,4,5,6,7,0x80000001,0x80000007};
    const OKMT_U32 expected[]={2,6,2,5,5,5,5,5,5,6,3,5,3};
    for (unsigned round=0;selected && round<13;round++) {
        OKMT_CREATE_DEVICE device[2]={{0},{0}};OKMT_CREATE_SYNC2 sync[2]={{0},{0}};
        for(unsigned n=0;n<2;n++) {
            device[n].object.adapter=selected;
            OKMT_S32 device_status=CreateDevice(&device[n]);
#ifdef DEVICE_ERROR_PHYSICAL
            /* The native provider has 16 device slots and deferred collection.
             * This is a loss-state contract probe, not a throughput guarantee:
             * expose pressure, allow bounded collection, and fail persistent
             * exhaustion. Never retry any other failure or a successful call. */
            for(unsigned retry=0;device_status==(OKMT_S32)0xc000009a && retry<100;retry++) {
                pressure_retries++;Sleep(10);device_status=CreateDevice(&device[n]);
            }
#endif
            if(device_status){report("DEVICE-ERROR create round=");number(round);report(" device=");number(n);report(" status=");number((unsigned)device_status);report("\n");}
            REQUIRE(device_status==0);
            sync[n].device=device[n].device;sync[n].info.type=OKMT_SYNC_MONITORED;sync[n].info.flags=0;
            REQUIRE(CreateSynchronizationObject2(&sync[n])==0 && sync[n].object && sync[n].info.data.monitored.cpu_address);
        }
        OKMT_U32 query_buffer[32]={0};OKMT_DEVICE_EXECUTION *query=(void *)query_buffer;
        query->device=device[0].device;query->type=1;REQUIRE(GetDeviceState(query)==0 && query->state==1);
        void *event=CreateEventA(0,1,0,0);REQUIRE(event);OKMT_U64 target=0x123456789abcdef0ULL;
        OKMT_WAIT_SYNC_CPU wait={device[0].device,1,&sync[0].object,&target,event,0};
        REQUIRE(WaitForSynchronizationObjectFromCpu(&wait)==0);
        CHECK(WaitForSingleObject(event,0)==258);
        OKMT_MARK_DEVICE_ERROR mark={device[0].device,reasons[round]};REQUIRE(MarkDeviceAsError(&mark)==0);
        CHECK(WaitForSingleObject(event,1000)==0);
        volatile OKMT_U32 *lost=sync[0].info.data.monitored.cpu_address,*healthy=sync[1].info.data.monitored.cpu_address;
        CHECK(lost[0]==0xffffffff && lost[1]==0xffffffff && !healthy[0] && !healthy[1]);
        REQUIRE(GetDeviceState(query)==0 && query->state==expected[round]);
        mark.reason=round&1?0x80000000:0x80000006;CHECK(MarkDeviceAsError(&mark)==0);
        CHECK(GetDeviceState(query)==0 && query->state==expected[round]);
        OKMT_SIGNAL_SYNC_CPU signal={device[0].device,1,&sync[0].object,&target,0};
        CHECK(SignalSynchronizationObjectFromCpu(&signal)==0 && lost[0]==0xffffffff && lost[1]==0xffffffff);
        OKMT_CREATE_SYNC2 extra={0};extra.device=device[0].device;extra.info.type=OKMT_SYNC_MONITORED;extra.info.flags=0;
        OKMT_S32 extra_status=CreateSynchronizationObject2(&extra);
#ifdef DEVICE_ERROR_WINDOWS
        /* Windows returned both success and GPU_EXCEPTION in the fixtures;
         * the cause of that difference is not established. Native Osito
         * refuses new GPU fence backing on loss. */
        CHECK(extra_status==0 || extra_status==(OKMT_S32)0xc01e0200);
        if(!extra_status){OKMT_DESTROY_SYNC unused={extra.object};CHECK(DestroySynchronizationObject(&unused)==0);}
#else
        CHECK(extra_status==(OKMT_S32)0xc01e0200);
#endif
        OKMT_CREATE_SYNC legacy={0};legacy.device=device[0].device;legacy.info.type=OKMT_SYNC_SEMAPHORE;legacy.info.data.semaphore.maximum=1;
        REQUIRE(CreateSynchronizationObject(&legacy)==0);OKMT_DESTROY_SYNC drop={legacy.object};CHECK(DestroySynchronizationObject(&drop)==0);
        query->device=device[1].device;CHECK(GetDeviceState(query)==0 && query->state==OKMT_EXECUTION_ACTIVE);
        CHECK(CloseHandle(event));
        for(unsigned n=0;n<2;n++) {
            drop.object=sync[n].object;REQUIRE(DestroySynchronizationObject(&drop)==0);
            OKMT_DESTROY_DEVICE close={device[n].device};REQUIRE(DestroyDevice(&close)==0);
        }
    }
    for (unsigned i=0;i<list.count;i++){OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(CloseAdapter(&close)==0);}
    report("DEVICE-ERROR pressure-retries=");number(pressure_retries);report("\n");
    report("DEVICE-ERROR DONE checks=");number(checks);report(" failures=");number(failures);report("\n");
    ExitProcess(failures?1:0);
}
