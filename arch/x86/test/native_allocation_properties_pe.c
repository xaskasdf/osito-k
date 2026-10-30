/* Public ABI probe over actual native backing, or empty-adapter validation. */
#include "../win32/d3dkmt_abi.h"
#include "../drivers/intel_gpu_umd_abi.h"
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
#define CHECK(x) do {checks++;if(!(x)){failures++;report("PROPERTIES FAIL " #x "\n");}} while(0)
#define REQUIRE(x) do {CHECK(x);if(failures)ExitProcess(1);} while(0)
static void number(unsigned x)
{char s[9];for(unsigned i=0;i<8;i++)s[i]="0123456789abcdef"[(x>>(28-4*i))&15];s[8]=0;report(s);}
void allocation_properties_entry(void)
{
    void *gdi=LoadLibraryA("gdi32.dll");
#define RESOLVE(name,type) OKMT_S32 (CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    RESOLVE(EnumAdapters2,OKMT_ENUM_ADAPTERS2);
    RESOLVE(QueryAdapterInfo,OKMT_QUERY_ADAPTER_INFO);
    RESOLVE(CreateDevice,OKMT_CREATE_DEVICE);
    RESOLVE(DestroyDevice,OKMT_DESTROY_DEVICE);
    RESOLVE(CloseAdapter,OKMT_CLOSE_ADAPTER);
    RESOLVE(CreateAllocation,OKMT_CREATE_ALLOCATION);
    RESOLVE(DestroyAllocation,OKMT_DESTROY_ALLOCATION);
    RESOLVE(QueryAllocationResidency,OKMT_ALLOCATION_PROPERTIES);
    RESOLVE(SetAllocationPriority,OKMT_ALLOCATION_PROPERTIES);
    RESOLVE(Lock,OKMT_LOCK);RESOLVE(Unlock,OKMT_UNLOCK);
#undef RESOLVE
    REQUIRE(gdi && EnumAdapters2 && QueryAdapterInfo && CreateDevice && DestroyDevice &&
        CloseAdapter && CreateAllocation && DestroyAllocation && QueryAllocationResidency &&
        SetAllocationPriority && Lock && Unlock);
    OKMT_U32 values[3]={0xfeed,0xfeed,0xfeed};OKMT_HANDLE handles[3]={0,0,0};
    OKMT_ALLOCATION_PROPERTIES properties={0,0,handles,3,values};
    CHECK(QueryAllocationResidency(&properties)!=0 && values[0]==0xfeed);
    CHECK(SetAllocationPriority(&properties)!=0);
    CHECK(QueryAllocationResidency(0)!=0 && SetAllocationPriority(0)!=0);
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(EnumAdapters2(&list)==0 && list.count<=16);
    OKMT_HANDLE selected=0;
    for (unsigned i=0;i<list.count;i++) {
        OKMT_QUERY_DEVICE_IDS ids={0};
        OKMT_QUERY_ADAPTER_INFO info={adapters[i].adapter,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids)};
        if (QueryAdapterInfo(&info)==0 && ids.vendor==0x8086 && ids.device==0x3e9b) selected=adapters[i].adapter;
    }
#ifdef PROPERTIES_PHYSICAL
    REQUIRE(selected!=0);
#else
    CHECK(selected==0);
#endif
    if (selected) {
        OKMT_CREATE_DEVICE device={0};device.object.adapter=selected;
        REQUIRE(CreateDevice(&device)==0 && device.device);
        intel_umd_buffer_description_t private_data={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,5000,0,0};
        OKMT_ALLOCATION_INFO infos[3]={{0}};
        for (unsigned i=0;i<3;i++){infos[i].private_data=&private_data;infos[i].private_bytes=sizeof(private_data);}
        OKMT_CREATE_ALLOCATION create={0};create.device=device.device;create.count=3;
        create.allocations=infos;create.flags=1; /* CreateResource */
        REQUIRE(CreateAllocation(&create)==0 && create.resource);
        for (unsigned i=0;i<3;i++){handles[i]=infos[i].allocation;REQUIRE(handles[i]!=0);}
        properties=(OKMT_ALLOCATION_PROPERTIES){device.device,0,handles,3,values};
        CHECK(QueryAllocationResidency(&properties)==0 && values[0]==2 && values[1]==2 && values[2]==2);
        properties.resource=create.resource;properties.allocations=0;properties.count=0;values[0]=0xfeed;
        CHECK(QueryAllocationResidency(&properties)==0 && values[0]==2);
        OKMT_U32 priorities[]={0,0x28000000,0x50000000,0x78000000,0xa0000000,0xc8000000,0xffffffff};
        for (unsigned i=0;i<sizeof(priorities)/sizeof(priorities[0]);i++) {
            values[0]=priorities[i];CHECK(SetAllocationPriority(&properties)==0);
            CHECK(QueryAllocationResidency(&properties)==0 && values[0]==2);
        }
        properties.resource=0;properties.allocations=handles;properties.count=3;
        values[0]=0;values[1]=0xa0000000;values[2]=0xffffffff;
        CHECK(SetAllocationPriority(&properties)==0);
        OKMT_LOCK locked={0};locked.device=device.device;locked.allocation=handles[0];
        locked.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT|OKMT_LOCK_WRITE_ONLY;
        REQUIRE(Lock(&locked)==0 && locked.data);
        for (unsigned i=0;i<5000;i++)((unsigned char *)locked.data)[i]=(unsigned char)(i^0x5a);
        CHECK(QueryAllocationResidency(&properties)==0 && values[0]==2);
        OKMT_UNLOCK unlocked={device.device,1,handles};REQUIRE(Unlock(&unlocked)==0);
        locked.data=0;locked.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT|OKMT_LOCK_READ_ONLY;
        REQUIRE(Lock(&locked)==0 && locked.data);
        for (unsigned i=0;i<5000;i++)CHECK(((unsigned char *)locked.data)[i]==(unsigned char)(i^0x5a));
        REQUIRE(Unlock(&unlocked)==0);
        OKMT_DESTROY_ALLOCATION drop={device.device,0,handles+1,1};REQUIRE(DestroyAllocation(&drop)==0);
        values[0]=values[1]=values[2]=0xfeed;
        CHECK(QueryAllocationResidency(&properties)!=0 && values[0]==0xfeed && values[1]==0xfeed && values[2]==0xfeed);
        CHECK(SetAllocationPriority(&properties)!=0);
        properties.resource=create.resource;properties.allocations=0;properties.count=0;
        CHECK(QueryAllocationResidency(&properties)==0 && values[0]==2);
        drop=(OKMT_DESTROY_ALLOCATION){device.device,create.resource,0,0};REQUIRE(DestroyAllocation(&drop)==0);
        values[0]=0xfeed;CHECK(QueryAllocationResidency(&properties)!=0 && values[0]==0xfeed);
        OKMT_DESTROY_DEVICE close={device.device};REQUIRE(DestroyDevice(&close)==0);
    }
    for (unsigned i=0;i<list.count;i++){OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(CloseAdapter(&close)==0);}
    report("PROPERTIES DONE checks=");number(checks);report(" failures=");number(failures);report("\n");
    ExitProcess(failures?1:0);
}
