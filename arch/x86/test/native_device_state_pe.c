/* Public device execution query on native hardware, or empty QEMU adapter set. */
#include "../win32/d3dkmt_abi.h"
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *,const char *);
API void *CALL GetStdHandle(OKMT_U32);
API int CALL WriteFile(void *,const void *,OKMT_U32,OKMT_U32 *,void *);
API void CALL ExitProcess(OKMT_U32);
static unsigned checks,failures;
static void report(const char *s)
{ OKMT_U32 n=0,w;while(s[n])n++;WriteFile(GetStdHandle((OKMT_U32)-11),s,n,&w,0); }
#define CHECK(x) do {checks++;if(!(x)){failures++;report("DEVICE-STATE FAIL " #x "\n");}} while(0)
#define REQUIRE(x) do {CHECK(x);if(failures)ExitProcess(1);} while(0)
static void number(unsigned x)
{char s[9];for(unsigned i=0;i<8;i++)s[i]="0123456789abcdef"[(x>>(28-4*i))&15];s[8]=0;report(s);}
void device_state_entry(void)
{
    void *gdi=LoadLibraryA("gdi32.dll");
#define RESOLVE(name,type) OKMT_S32 (CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    RESOLVE(EnumAdapters2,OKMT_ENUM_ADAPTERS2);RESOLVE(QueryAdapterInfo,OKMT_QUERY_ADAPTER_INFO);
    RESOLVE(CreateDevice,OKMT_CREATE_DEVICE);RESOLVE(DestroyDevice,OKMT_DESTROY_DEVICE);
    RESOLVE(CloseAdapter,OKMT_CLOSE_ADAPTER);RESOLVE(GetDeviceState,OKMT_DEVICE_EXECUTION);
#undef RESOLVE
    REQUIRE(gdi && EnumAdapters2 && QueryAdapterInfo && CreateDevice && DestroyDevice && CloseAdapter && GetDeviceState);
    struct { OKMT_DEVICE_EXECUTION execution; OKMT_U32 guard[29]; } query;
    for (unsigned i=0;i<29;i++)query.guard[i]=0xfeedabcd;
    query.execution=(OKMT_DEVICE_EXECUTION){0,1,0xfeed};
    CHECK(GetDeviceState(0)!=0 && GetDeviceState(&query.execution)!=0 && query.execution.state==0xfeed);
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(EnumAdapters2(&list)==0 && list.count<=16);
    OKMT_HANDLE selected=0;
    for (unsigned i=0;i<list.count;i++) {
        OKMT_QUERY_DEVICE_IDS ids={0};OKMT_QUERY_ADAPTER_INFO q={adapters[i].adapter,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids)};
        if (QueryAdapterInfo(&q)==0 && ids.vendor==0x8086 && ids.device==0x3e9b) selected=adapters[i].adapter;
    }
#ifdef DEVICE_STATE_PHYSICAL
    REQUIRE(selected);
#else
    REQUIRE(!list.count);
#endif
    if (selected) for (unsigned round=0;round<4;round++) {
        OKMT_CREATE_DEVICE device={0};device.object.adapter=selected;
        REQUIRE(CreateDevice(&device)==0 && device.device);
        query.execution=(OKMT_DEVICE_EXECUTION){device.device,1,0xfeed};
        for (unsigned n=0;n<32;n++) CHECK(GetDeviceState(&query.execution)==0 && query.execution.state==OKMT_EXECUTION_ACTIVE);
        for (unsigned n=0;n<29;n++) CHECK(query.guard[n]==0xfeedabcd);
        for (unsigned type=0;type<=7;type++) if (type!=1) {
            query.execution.type=type;query.execution.state=0xfeed;
            CHECK(GetDeviceState(&query.execution)!=0 && query.execution.state==0xfeed);
        }
        OKMT_DESTROY_DEVICE close={device.device};REQUIRE(DestroyDevice(&close)==0);
        query.execution.type=1;query.execution.state=0xfeed;
        CHECK(GetDeviceState(&query.execution)!=0 && query.execution.state==0xfeed);
    }
    for (unsigned i=0;i<list.count;i++){OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(CloseAdapter(&close)==0);}
    report("DEVICE-STATE DONE checks=");number(checks);report(" failures=");number(failures);report("\n");
    ExitProcess(failures?1:0);
}
