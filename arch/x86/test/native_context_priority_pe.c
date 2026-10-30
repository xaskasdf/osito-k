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
void *memset(void *destination,int value,__SIZE_TYPE__ bytes)
{volatile unsigned char *p=destination;for(__SIZE_TYPE__ i=0;i<bytes;i++)p[i]=(unsigned char)value;return destination;}
static void report(const char *text)
{OKMT_U32 n=0,w;while(text[n])n++;WriteFile(GetStdHandle((OKMT_U32)-11),text,n,&w,0);}
static void number(unsigned x)
{char s[9];for(unsigned i=0;i<8;i++)s[i]="0123456789abcdef"[(x>>(28-4*i))&15];s[8]=0;report(s);}
#define CHECK(x) do{checks++;if(!(x)){failures++;report("PRIORITY FAIL " #x "\n");}}while(0)
#define REQUIRE(x) do{CHECK(x);if(failures)ExitProcess(1);}while(0)
void context_priority_entry(void)
{
    void *gdi=LoadLibraryA("gdi32.dll");
#define RESOLVE(name,type) OKMT_S32(CALL *name)(type *)=GetProcAddress(gdi,"D3DKMT" #name)
    RESOLVE(EnumAdapters2,OKMT_ENUM_ADAPTERS2);RESOLVE(QueryAdapterInfo,OKMT_QUERY_ADAPTER_INFO);
    RESOLVE(CreateDevice,OKMT_CREATE_DEVICE);RESOLVE(DestroyDevice,OKMT_DESTROY_DEVICE);RESOLVE(CloseAdapter,OKMT_CLOSE_ADAPTER);
    RESOLVE(CreateContext,OKMT_CREATE_CONTEXT);RESOLVE(DestroyContext,OKMT_DESTROY_CONTEXT);
    RESOLVE(GetContextSchedulingPriority,OKMT_CONTEXT_PRIORITY);RESOLVE(SetContextSchedulingPriority,OKMT_CONTEXT_PRIORITY);
#undef RESOLVE
    REQUIRE(gdi && EnumAdapters2 && QueryAdapterInfo && CreateDevice && DestroyDevice && CloseAdapter && CreateContext && DestroyContext && GetContextSchedulingPriority && SetContextSchedulingPriority);
    OKMT_CONTEXT_PRIORITY invalid={0,0x12345678};
    CHECK(GetContextSchedulingPriority(&invalid)!=0 && invalid.priority==0x12345678);
    invalid.priority=1;CHECK(SetContextSchedulingPriority(&invalid)!=0 && invalid.priority==1);
    CHECK(GetContextSchedulingPriority(0)!=0 && SetContextSchedulingPriority(0)!=0);
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(EnumAdapters2(&list)==0 && list.count<=16);OKMT_HANDLE selected=0;
    for(unsigned i=0;i<list.count;i++) {
        OKMT_QUERY_DEVICE_IDS ids={0};OKMT_QUERY_ADAPTER_INFO query={adapters[i].adapter,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids)};
        if(QueryAdapterInfo(&query)==0 && ids.vendor==0x8086 && ids.device==0x3e9b)selected=adapters[i].adapter;
    }
#ifdef PRIORITY_PHYSICAL
    REQUIRE(selected);
#else
    REQUIRE(!list.count);
#endif
    if(selected) {
        OKMT_CREATE_DEVICE device={0};device.object.adapter=selected;REQUIRE(CreateDevice(&device)==0);
        OKMT_CREATE_CONTEXT contexts[2]={{0},{0}};
        intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
        for(unsigned i=0;i<2;i++) {
            contexts[i].device=device.device;contexts[i].node=INTEL_UMD_NODE_COPY;contexts[i].private_data=&layout;contexts[i].private_bytes=sizeof(layout);
            REQUIRE(CreateContext(&contexts[i])==0 && contexts[i].context);
            OKMT_CONTEXT_PRIORITY query={contexts[i].context,0x12345678};
            REQUIRE(GetContextSchedulingPriority(&query)==0 && query.priority==0);
            for(int value=-7;value<=7;value++) {
                query.priority=value;REQUIRE(SetContextSchedulingPriority(&query)==0 && query.priority==value && query.context==contexts[i].context);
                query.priority=0x12345678;REQUIRE(GetContextSchedulingPriority(&query)==0 && query.priority==value);
            }
            const int bad[]={-8,8,(int)0x80000000u,0x7fffffff,0x40000000,0x4000001f,0x40000020};
            for(unsigned n=0;n<sizeof(bad)/sizeof(bad[0]);n++) {
                query.priority=bad[n];CHECK(SetContextSchedulingPriority(&query)==(OKMT_S32)(n==4 || n==5?0xc00000bb:0xc000000d));
                CHECK(query.priority==bad[n]);query.priority=0x12345678;
                CHECK(GetContextSchedulingPriority(&query)==0 && query.priority==7);
            }
        }
        OKMT_CONTEXT_PRIORITY first={contexts[0].context,-7},second={contexts[1].context,0x12345678};
        CHECK(SetContextSchedulingPriority(&first)==0 && GetContextSchedulingPriority(&second)==0 && second.priority==7);
        for(unsigned i=0;i<2;i++) {
            OKMT_DESTROY_CONTEXT close={contexts[i].context};CHECK(DestroyContext(&close)==0);
            OKMT_CONTEXT_PRIORITY stale={close.context,0x12345678};CHECK(GetContextSchedulingPriority(&stale)!=0 && stale.priority==0x12345678);
        }
        OKMT_DESTROY_DEVICE close={device.device};CHECK(DestroyDevice(&close)==0);
    }
    for(unsigned i=0;i<list.count;i++){OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(CloseAdapter(&close)==0);}
    report("PRIORITY DONE checks=");number(checks);report(" failures=");number(failures);report("\n");ExitProcess(failures?1:0);
}
