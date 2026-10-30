/* Public scanline ABI. Hardware is enumerated, never synthesized by a probe. */
#include <stddef.h>
#define native_d3dkmt_entry unused_scanline_base_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry
void native_scanline_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *scan)(OKMT_GET_SCANLINE *)=GetProcAddress(gdi,"D3DKMTGetScanLine");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (CALL *close_adapter)(const OKMT_CLOSE_ADAPTER *)=GetProcAddress(gdi,"D3DKMTCloseAdapter");
    OKMT_S32 (CALL *query)(const OKMT_QUERY_ADAPTER_INFO *)=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
    CHECK(scan && enumerate && close_adapter && query,"scanline-exports");
    if (!scan || !enumerate || !close_adapter || !query) ExitProcess(2);
    CHECK(sizeof(OKMT_GET_SCANLINE)==16,"scanline-layout");
    CHECK(scan(NULL)==(OKMT_S32)0xc000000d,"scanline-null");
    OKMT_GET_SCANLINE request={0,0,0x5a,{0x5a,0x5a,0x5a},0x5a5a5a5a};
    CHECK(scan(&request)==(OKMT_S32)0xc000000d,"scanline-invalid-adapter");
    CHECK(request.in_vertical_blank==0x5a && request.scanline==0x5a5a5a5a,"scanline-invalid-preserved");
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    CHECK(enumerate(&list)==0 && list.count<=16,"scanline-enumerate");
#ifdef KMT_SCANLINE_EXPECT_EMPTY
    CHECK(!list.count,"scanline-no-synthetic-display");
#endif
    for (OKMT_U32 i=0;i<list.count && i<16;i++) {
        OKMT_U32 type=0;OKMT_QUERY_ADAPTER_INFO info={adapters[i].adapter,OKMT_QUERY_ADAPTER_TYPE,&type,4};
        CHECK(query(&info)==0,"scanline-type");
        request=(OKMT_GET_SCANLINE){adapters[i].adapter,0,0x5a,{0x5a,0x5a,0x5a},0x5a5a5a5a};
        OKMT_S32 status=scan(&request);
        if (!(type&OKMT_ADAPTER_DISPLAY) || (type&OKMT_ADAPTER_PROTOTYPE)) {
            CHECK(status==(OKMT_S32)0xc00000bb,"scanline-render-only");
            CHECK(request.in_vertical_blank==0x5a && request.scanline==0x5a5a5a5a,"scanline-unsupported-preserved");
        } else {
            CHECK(!status && request.in_vertical_blank<=1,"scanline-real-display");
        }
        CHECK(request.adapter==adapters[i].adapter && !request.source,"scanline-inputs-preserved");
        CHECK(request.padding[0]==0x5a && request.padding[1]==0x5a && request.padding[2]==0x5a,"scanline-padding");
        OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(close_adapter(&close)==0,"scanline-close");
        request=(OKMT_GET_SCANLINE){close.adapter,0,0x5a,{0x5a,0x5a,0x5a},0x5a5a5a5a};
        CHECK(scan(&request)==(OKMT_S32)0xc000000d && request.in_vertical_blank==0x5a && request.scanline==0x5a5a5a5a,"scanline-stale");
    }
    VALUE("SCANLINE checks=",checks);VALUE("SCANLINE failures=",failures);
    FreeLibrary(gdi);ExitProcess(failures?1:0);
}
