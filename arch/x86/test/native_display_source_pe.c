#include <stddef.h>
#define native_d3dkmt_entry unused_source_base_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry
void *memset(void *destination,int value,size_t size)
{
    volatile unsigned char *p=destination;
    for (size_t i=0;i<size;i++) p[i]=(unsigned char)value;
    return destination;
}
void native_display_source_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *gdi=LoadLibraryA("gdi32.dll"),*user=LoadLibraryA("user32.dll"),*kernel=LoadLibraryA("kernel32.dll");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (CALL *query)(const OKMT_QUERY_ADAPTER_INFO *)=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
    OKMT_S32 (CALL *scan)(OKMT_GET_SCANLINE *)=GetProcAddress(gdi,"D3DKMTGetScanLine");
    OKMT_S32 (CALL *modes)(OKMT_GET_DISPLAY_MODE_LIST *)=GetProcAddress(gdi,"D3DKMTGetDisplayModeList");
    OKMT_S32 (CALL *open_gdi)(OKMT_OPEN_ADAPTER_FROM_GDI_NAME *)=GetProcAddress(gdi,"D3DKMTOpenAdapterFromGdiDisplayName");
    OKMT_S32 (CALL *open_hdc)(OKMT_OPEN_ADAPTER_FROM_HDC *)=GetProcAddress(gdi,"D3DKMTOpenAdapterFromHdc");
    OKMT_S32 (CALL *close_adapter)(const OKMT_CLOSE_ADAPTER *)=GetProcAddress(gdi,"D3DKMTCloseAdapter");
    void *(CALL *get_dc)(void *)=GetProcAddress(user,"GetDC");
    int (CALL *release_dc)(void *,void *)=GetProcAddress(user,"ReleaseDC");
    void (CALL *sleep)(OKMT_U32)=GetProcAddress(kernel,"Sleep");
    CHECK(enumerate && query && scan && modes && open_gdi && open_hdc && close_adapter && get_dc && release_dc && sleep,"source-exports");
    if (failures) ExitProcess(2);
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    CHECK(enumerate(&list)==0 && list.count<=16,"source-enumerate");
#ifdef KMT_SOURCE_EXPECT_EMPTY
    CHECK(!list.count,"source-no-synthetic-adapter");
#else
    CHECK(list.count==1 && adapters[0].sources==1,"source-one-native-display");
    if (list.count!=1) ExitProcess(2);
    OKMT_U32 type=0;OKMT_QUERY_ADAPTER_INFO info={adapters[0].adapter,OKMT_QUERY_ADAPTER_TYPE,&type,4};
    CHECK(query(&info)==0 && type==OKMT_ADAPTER_DISPLAY,"source-display-only-readiness");
    OKMT_UMD_FILENAME umd={.version=OKMT_UMD_DX9};
    info.type=OKMT_QUERY_UMD_FILENAME;info.data=&umd;info.bytes=sizeof(umd);
    CHECK(query(&info)==(OKMT_S32)0xc00000bb && !umd.name[0],"source-render-driver-unpublished");
    OKMT_OPEN_ADAPTER_FROM_GDI_NAME name={.name={'\\','\\','.','\\','D','I','S','P','L','A','Y','1',0}};
    CHECK(open_gdi(&name)==0 && name.adapter && !name.source,"source-gdi-binding");
    CHECK(name.luid.low==adapters[0].luid.low && name.luid.high==adapters[0].luid.high,"source-gdi-same-gpu");
    void *dc=get_dc(NULL);CHECK(dc!=NULL,"source-screen-dc");
    OKMT_OPEN_ADAPTER_FROM_HDC hdc={.dc=dc};
    CHECK(open_hdc(&hdc)==0 && hdc.adapter && !hdc.source,"source-hdc-binding");
    CHECK(hdc.luid.low==name.luid.low && hdc.luid.high==name.luid.high,"source-hdc-same-gpu");
    OKMT_GET_DISPLAY_MODE_LIST ml={adapters[0].adapter,0,NULL,0};
    CHECK(modes(&ml)==0 && ml.count==1,"source-current-mode-count");
    struct { OKMT_DISPLAY_MODE mode;OKMT_U32 guard; } record={.guard=0x5a5a5a5a};
    ml.modes=&record.mode;ml.count=1;
    CHECK(modes(&ml)==0 && ml.count==1 && record.guard==0x5a5a5a5a,"source-mode-copy");
    CHECK(record.mode.width && record.mode.height && record.mode.format==22,"source-physical-mode");
    CHECK(!record.mode.integer_refresh && !record.mode.refresh_numerator && record.mode.refresh_denominator==1,"source-no-invented-refresh");
    CHECK(record.mode.scanline_order==1 && record.mode.orientation==1,"source-progressive-unrotated");
    VALUE("SOURCE width=",record.mode.width);VALUE("SOURCE height=",record.mode.height);
    OKMT_U32 prior=0xffffffff,changes=0,blank=0;
    for (unsigned i=0;i<128;i++) {
        OKMT_GET_SCANLINE sample={adapters[0].adapter,0,0x5a,{0x5a,0x5a,0x5a},0x5a5a5a5a};
        CHECK(scan(&sample)==0,"source-native-sample");
        CHECK(sample.adapter==adapters[0].adapter && !sample.source && sample.padding[0]==0x5a && sample.padding[1]==0x5a && sample.padding[2]==0x5a,"source-sample-inputs-padding");
        CHECK(sample.in_vertical_blank<=1 && sample.scanline<8192,"source-hardware-line-bounds");
        CHECK(sample.in_vertical_blank ? sample.scanline>=record.mode.height : sample.scanline<record.mode.height,"source-native-blanking");
        changes+=i && prior!=sample.scanline;prior=sample.scanline;blank+=sample.in_vertical_blank==1;
        sleep(1);
    }
    CHECK(changes>4,"source-hardware-progress");
    VALUE("SOURCE changed=",changes);VALUE("SOURCE blank=",blank);
    OKMT_GET_SCANLINE invalid={adapters[0].adapter,1,0x5a,{0x5a,0x5a,0x5a},0x5a5a5a5a};
    CHECK(scan(&invalid)==(OKMT_S32)0xc000000d && invalid.in_vertical_blank==0x5a && invalid.scanline==0x5a5a5a5a,"source-invalid-id-preserved");
    OKMT_CLOSE_ADAPTER a={name.adapter};CHECK(close_adapter(&a)==0,"source-close-gdi");
    a.adapter=hdc.adapter;CHECK(close_adapter(&a)==0,"source-close-hdc");
    CHECK(release_dc(NULL,dc)!=0,"source-release-dc");
#endif
    for (OKMT_U32 i=0;i<list.count && i<16;i++) {
        OKMT_CLOSE_ADAPTER a={adapters[i].adapter};CHECK(close_adapter(&a)==0,"source-close-enumerated");
    }
    VALUE("SOURCE checks=",checks);VALUE("SOURCE failures=",failures);
    FreeLibrary(kernel);FreeLibrary(user);FreeLibrary(gdi);ExitProcess(failures?1:0);
}
