/* Public PE ABI admission tests; no synthetic display is registered. */
#include <stddef.h>
#define native_d3dkmt_entry unused_display_modes_base_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry

void native_display_modes_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *get_modes)(OKMT_GET_DISPLAY_MODE_LIST *)=GetProcAddress(gdi,"D3DKMTGetDisplayModeList");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (CALL *close_adapter)(const OKMT_CLOSE_ADAPTER *)=GetProcAddress(gdi,"D3DKMTCloseAdapter");
    CHECK(get_modes && enumerate && close_adapter,"mode-exports");
    if (!get_modes || !enumerate || !close_adapter) ExitProcess(2);
    CHECK(sizeof(OKMT_DISPLAY_MODE)==44,"mode-entry-layout");
    CHECK(sizeof(OKMT_GET_DISPLAY_MODE_LIST)==(sizeof(void *)==4?16u:24u),"mode-list-layout");
    CHECK(get_modes(NULL)==(OKMT_S32)0xc000000d,"mode-null-header");
    OKMT_GET_DISPLAY_MODE_LIST invalid={0,0,(void *)1,0x5a5a5a5a};
    CHECK(get_modes(&invalid)==(OKMT_S32)0xc000000d,"mode-invalid-adapter");
    CHECK(invalid.modes==(void *)1 && invalid.count==0x5a5a5a5a,"mode-invalid-output-preserved");
    OKMT_ADAPTER_INFO adapters[16];
    OKMT_ENUM_ADAPTERS2 list={16,adapters};
    CHECK(enumerate(&list)==0 && list.count<=16,"mode-enumerate");
#ifdef KMT_MODES_EXPECT_EMPTY
    CHECK(!list.count,"mode-no-synthetic-display");
#endif
    for (OKMT_U32 i=0;i<list.count && i<16;i++) {
        OKMT_U32 type=0;
        OKMT_QUERY_ADAPTER_INFO info={adapters[i].adapter,OKMT_QUERY_ADAPTER_TYPE,&type,4};
        OKMT_S32 (CALL *query)(const OKMT_QUERY_ADAPTER_INFO *)=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
        CHECK(query && query(&info)==0,"mode-adapter-type");
        if (query && query(&info)==0 && (!(type&OKMT_ADAPTER_DISPLAY) || (type&OKMT_ADAPTER_PROTOTYPE))) {
            for (unsigned source=0;source<2;source++) {
                invalid=(OKMT_GET_DISPLAY_MODE_LIST){adapters[i].adapter,source?0xffffffffu:0,NULL,0x5a5a5a5a};
                CHECK(get_modes(&invalid)==(OKMT_S32)0xc00000bb,"mode-render-only-unsupported");
                CHECK(invalid.count==0x5a5a5a5a,"mode-render-only-output-preserved");
            }
        }
        OKMT_CLOSE_ADAPTER close={adapters[i].adapter};CHECK(close_adapter(&close)==0,"mode-close");
        invalid=(OKMT_GET_DISPLAY_MODE_LIST){close.adapter,0,NULL,0x5a5a5a5a};
        CHECK(get_modes(&invalid)==(OKMT_S32)0xc000000d && invalid.count==0x5a5a5a5a,"mode-stale-adapter");
    }
    VALUE("MODES checks=",checks);VALUE("MODES failures=",failures);
    FreeLibrary(gdi);ExitProcess(failures?1:0);
}
