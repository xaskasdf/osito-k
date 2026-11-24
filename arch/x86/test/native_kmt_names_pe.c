/* Independent adapter-name/HDC oracle. Does not submit GPU work. */
#define native_d3dkmt_entry unused_names_entry
#include "native_d3dkmt_pe.c"
#undef native_d3dkmt_entry

void native_kmt_names_entry(void)
{
    OKMT_U32 checks=0,failures=0;
    void *gdi=LoadLibraryA("gdi32.dll");
    void *user=LoadLibraryA("user32.dll");
    OKMT_S32 (CALL *open_hdc)(OKMT_OPEN_ADAPTER_FROM_HDC *)=GetProcAddress(gdi,"D3DKMTOpenAdapterFromHdc");
    OKMT_S32 (CALL *open_gdi)(OKMT_OPEN_ADAPTER_FROM_GDI_NAME *)=GetProcAddress(gdi,"D3DKMTOpenAdapterFromGdiDisplayName");
    OKMT_S32 (CALL *open_device)(OKMT_OPEN_ADAPTER_FROM_DEVICE_NAME *)=GetProcAddress(gdi,"D3DKMTOpenAdapterFromDeviceName");
    OKMT_S32 (CALL *close_adapter)(const OKMT_CLOSE_ADAPTER *)=GetProcAddress(gdi,"D3DKMTCloseAdapter");
    void *(CALL *create_dc)(const char *,const char *,const char *,const void *)=GetProcAddress(gdi,"CreateDCA");
    void *(CALL *memory_dc)(void *)=GetProcAddress(gdi,"CreateCompatibleDC");
    int (CALL *delete_dc)(void *)=GetProcAddress(gdi,"DeleteDC");
    void *(CALL *get_dc)(void *)=GetProcAddress(user,"GetDC");
    int (CALL *release_dc)(void *,void *)=GetProcAddress(user,"ReleaseDC");
    int (CALL *enum_display)(const OKMT_WCHAR *,OKMT_U32,void *,OKMT_U32)=GetProcAddress(user,"EnumDisplayDevicesW");
    if (!open_hdc || !open_gdi || !open_device || !close_adapter || !create_dc ||
        !memory_dc || !delete_dc || !get_dc || !release_dc || !enum_display) {
        REPORT("NAMES UNAVAILABLE\n"); ExitProcess(2);
    }
    struct { OKMT_U32 cb; OKMT_WCHAR name[32],description[128]; OKMT_U32 flags;
             OKMT_WCHAR id[128],key[128]; } display;
    for (unsigned i=0;i<sizeof(display);i++) ((volatile unsigned char *)&display)[i]=0;
    display.cb=sizeof(display);
    CHECK(enum_display(0,0,&display,0)!=0,"display-device");
    OKMT_OPEN_ADAPTER_FROM_GDI_NAME named;
    for (unsigned i=0;i<sizeof(named);i++) ((volatile unsigned char *)&named)[i]=0x5a;
    for (unsigned i=0;i<32;i++) named.name[i]=display.name[i];
    OKMT_S32 named_status=open_gdi(&named);
    VALUE("NAMES gdi-status=",named_status); VALUE("NAMES gdi-source=",named.source);
#ifdef KMT_EXPECT_NO_DISPLAY
    CHECK(named_status==(OKMT_S32)0xc0000001 && named.adapter==0x5a5a5a5a,"no-unowned-display");
#else
    CHECK(named_status==0,"display-mapping");
#endif
    OKMT_LUID baseline=named.luid;
    OKMT_U32 source=named.source;
    if (!named_status) { OKMT_CLOSE_ADAPTER close={named.adapter}; CHECK(close_adapter(&close)==0,"close-named"); }
    for (unsigned i=0;i<32;i++) if (named.name[i]>='A' && named.name[i]<='Z') named.name[i]+='a'-'A';
    OKMT_S32 lower_status=open_gdi(&named);
    VALUE("NAMES lowercase-status=",lower_status);
    CHECK(lower_status==named_status,"name-case-insensitive");
    if (!lower_status) { OKMT_CLOSE_ADAPTER close={named.adapter}; CHECK(close_adapter(&close)==0,"close-lowercase"); }
    void *dcs[5];
    dcs[0]=create_dc("DISPLAY",0,0,0); dcs[1]=get_dc(0);
    dcs[2]=memory_dc(dcs[0]); dcs[3]=0; dcs[4]=(void *)1;
    for (unsigned i=0;i<5;i++) {
        OKMT_OPEN_ADAPTER_FROM_HDC data;
        for (unsigned j=0;j<sizeof(data);j++) ((volatile unsigned char *)&data)[j]=0x5a;
        data.dc=dcs[i];
        OKMT_S32 status=open_hdc(&data);
        VALUE("NAMES hdc-case=",i); VALUE("NAMES hdc-status=",status);
        VALUE("NAMES hdc-output=",data.adapter); VALUE("NAMES hdc-source=",data.source);
        CHECK(data.dc==dcs[i],"hdc-input-preserved");
        if (i>=3 || named_status) {
            CHECK(status==(OKMT_S32)0xc000000d,"hdc-unavailable");
            CHECK(!data.adapter && !data.luid.low && !data.luid.high && !data.source,"hdc-error-output");
        } else CHECK(status==0,"hdc-associated-device");
        if (!status) {
            CHECK(data.luid.low==baseline.low && data.luid.high==baseline.high &&
                  data.source==source,"same-display-identity");
            OKMT_CLOSE_ADAPTER close={data.adapter}; CHECK(close_adapter(&close)==0,"close-hdc");
        }
    }
    if (dcs[0]) delete_dc(dcs[0]);
    if (dcs[1]) release_dc(0,dcs[1]);
    if (dcs[2]) delete_dc(dcs[2]);
    const char *bad_gdi[]={"","\\\\.\\DISPLAY999","DISPLAY","\\\\.\\DISPLAY1x"};
    for (unsigned i=0;i<4;i++) {
        for (unsigned j=0;j<sizeof(named);j++) ((volatile unsigned char *)&named)[j]=0x5a;
        unsigned n=0;
        do { named.name[n]=(unsigned char)bad_gdi[i][n]; } while(bad_gdi[i][n++]);
        OKMT_S32 status=open_gdi(&named);
        VALUE("NAMES bad-gdi=",i); VALUE("NAMES bad-gdi-status=",status);
        VALUE("NAMES bad-gdi-output=",named.adapter); VALUE("NAMES bad-gdi-source=",named.source);
        CHECK(status==(i==2 ? named_status : (OKMT_S32)0xc0000001),"gdi-unknown-or-alias");
        if (status) CHECK(named.adapter==0x5a5a5a5a && named.source==0x5a5a5a5a,"gdi-error-preserved");
        if (!status) { OKMT_CLOSE_ADAPTER close={named.adapter}; close_adapter(&close); }
    }
    const char *names[]={"\\Device\\Video0","\\\\.\\DISPLAY1","\\Device\\OsitoIntelGraphics0","", "\\Device\\OsitoMissing"};
    for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        OKMT_WCHAR wide[80]; unsigned n=0;
        do { wide[n]=(unsigned char)names[i][n]; } while(names[i][n++]);
        OKMT_OPEN_ADAPTER_FROM_DEVICE_NAME data;
        for (unsigned j=0;j<sizeof(data);j++) ((volatile unsigned char *)&data)[j]=0x5a;
        data.name=wide;
        OKMT_S32 status=open_device(&data);
        VALUE("NAMES device-case=",i); VALUE("NAMES device-status=",status);
        VALUE("NAMES device-output=",data.adapter);
#ifdef KMT_EXPECT_NATIVE_DEVICE
        if (i==2) {
            CHECK(status==0,"native-device-open");
            OKMT_S32 (CALL *query)(const OKMT_QUERY_ADAPTER_INFO *)=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
            OKMT_QUERY_DEVICE_IDS ids;
            for (unsigned j=0;j<sizeof(ids);j++) ((volatile unsigned char *)&ids)[j]=0;
            OKMT_QUERY_ADAPTER_INFO info={data.adapter,OKMT_QUERY_DEVICE_IDENTIFIERS,&ids,sizeof(ids)};
            CHECK(query && query(&info)==0 && ids.vendor==0x8086,"native-device-identity");
            OKMT_U32 type=0;
            info.type=OKMT_QUERY_ADAPTER_TYPE; info.data=&type; info.bytes=4;
#ifdef KMT_EXPECT_NATIVE_DISPLAY
            CHECK(query && query(&info)==0 && type==OKMT_ADAPTER_DISPLAY,"native-retained-display");
#else
            CHECK(query && query(&info)==0 && type==OKMT_ADAPTER_PROTOTYPE,"native-capabilities-unpublished");
#endif
        } else
#endif
        {
            CHECK(status==(OKMT_S32)(i==3 ? 0xc000003b : 0xc000000d),"device-name-error");
            CHECK(data.adapter==(i==3 ? 0 : 0x5a5a5a5a),"device-error-output");
        }
        CHECK(data.name==wide,"device-input-preserved");
        if (!status) { OKMT_CLOSE_ADAPTER close={data.adapter}; CHECK(close_adapter(&close)==0,"close-device"); }
    }
    VALUE("NAMES checks=",checks); VALUE("NAMES failures=",failures);
    FreeLibrary(user); FreeLibrary(gdi); ExitProcess(failures ? 1 : 0);
}
