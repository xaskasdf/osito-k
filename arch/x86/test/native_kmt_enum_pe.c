/* Independent adapter-enumeration oracle: no GPU commands or runtime DLLs. */
#define native_d3dkmt_entry unused_discovery_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry

void native_kmt_enum_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS *)=gdi ? GetProcAddress(gdi,"D3DKMTEnumAdapters") : 0;
    OKMT_S32 (CALL *enumerate2)(OKMT_ENUM_ADAPTERS2 *)=gdi ? GetProcAddress(gdi,"D3DKMTEnumAdapters2") : 0;
    OKMT_S32 (CALL *close_adapter)(const OKMT_CLOSE_ADAPTER *)=gdi ? GetProcAddress(gdi,"D3DKMTCloseAdapter") : 0;
    if (!enumerate || !enumerate2 || !close_adapter) { REPORT("ENUM UNAVAILABLE\n"); ExitProcess(2); }
    OKMT_ADAPTER_INFO baseline[OKMT_MAX_ENUM_ADAPTERS];
    OKMT_ENUM_ADAPTERS2 current={OKMT_MAX_ENUM_ADAPTERS,baseline};
    CHECK(enumerate2(&current)==0 && current.count<=OKMT_MAX_ENUM_ADAPTERS,"reference-enumeration");
    if (failures) ExitProcess(1);
    const OKMT_U32 inputs[]={0,1,16,17,0xffffffffu};
    for (unsigned trial=0;trial<sizeof(inputs)/sizeof(inputs[0]);trial++) {
        OKMT_ENUM_ADAPTERS data;
        for (unsigned i=0;i<sizeof(data);i++) ((volatile unsigned char *)&data)[i]=0x5a;
        data.count=inputs[trial];
        OKMT_S32 status=enumerate(&data);
        VALUE("ENUM input=",inputs[trial]); VALUE("ENUM status=",status); VALUE("ENUM count=",data.count);
        CHECK(status==0,"legacy-enumeration");
        if (status>=0) {
            CHECK(data.count<=OKMT_MAX_ENUM_ADAPTERS,"bounded-count");
            CHECK(data.count==current.count,"same-adapter-count");
            for (unsigned i=0;i<data.count && i<OKMT_MAX_ENUM_ADAPTERS;i++) {
                VALUE("ENUM luid=",data.adapters[i].luid.low);
                CHECK(data.adapters[i].luid.low==baseline[i].luid.low &&
                      data.adapters[i].luid.high==baseline[i].luid.high,"same-adapter-luid");
                CHECK(data.adapters[i].sources==baseline[i].sources &&
                      data.adapters[i].precise_present_regions==baseline[i].precise_present_regions,"same-adapter-properties");
                CHECK(data.adapters[i].adapter!=baseline[i].adapter,"separate-owned-handle");
                OKMT_CLOSE_ADAPTER close={data.adapters[i].adapter};
                CHECK(close_adapter(&close)==0,"close-enumerated-adapter");
            }
        }
    }
    for (unsigned i=0;i<current.count;i++) {
        OKMT_CLOSE_ADAPTER close={baseline[i].adapter};
        CHECK(close_adapter(&close)==0,"close-reference-adapter");
    }
    VALUE("ENUM checks=",checks); VALUE("ENUM failures=",failures);
    FreeLibrary(gdi); ExitProcess(failures ? 1 : 0);
}
