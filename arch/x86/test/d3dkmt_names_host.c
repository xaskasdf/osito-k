/* Registry/marshaller lifetime tests. The display bindings below are fixtures,
 * never registered in a shipped kernel or used to claim physical scanout. */
#define main registry_suite_main
#include "d3dkmt_registry_host.c"
#undef main

static const OKMT_WCHAR device_name[]=u"\\Device\\OsitoTestGraphics0";
static const OKMT_WCHAR screen_name[]=u"\\\\.\\DISPLAY1";
static const OKMT_WCHAR secondary_name[]=u"\\\\.\\DISPLAY2";
static const OKMT_WCHAR display_alias[]=u"display";
static const OKMT_WCHAR lower_device[]=u"\\device\\ositotestgraphics0";
static const OKMT_WCHAR lower_screen[]=u"\\\\.\\display1";
BOOL gdi32_dc_display_name(HANDLE dc,WCHAR name[32])
{
    if ((uintptr_t)dc!=0xabcdefu) return FALSE;
    memcpy(name,screen_name,sizeof(screen_name)); return TRUE;
}

static void named_cases(void)
{
    D3DKMT_ADAPTER_DESCRIPTION desc={.driver_version=1000,.adapter_type=OKMT_ADAPTER_DISPLAY,.source_count=2};
    memcpy(desc.device_name,device_name,sizeof(device_name));
    OKMT_LUID luid,other;
    CHECK(d3dkmt_register_adapter(&desc,&luid)==0);
    CHECK(d3dkmt_register_adapter(&desc,&other)==STATUS_OBJECT_NAME_COLLISION);
    memcpy(desc.device_name,lower_device,sizeof(lower_device));
    CHECK(d3dkmt_register_adapter(&desc,&other)==STATUS_OBJECT_NAME_COLLISION);
    desc.device_name[0]=0;
    CHECK(d3dkmt_register_adapter(&desc,&other)==0);
    CHECK(d3dkmt_bind_display(luid,2,screen_name,TRUE)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_bind_display(luid,1,screen_name,TRUE)==0);
    CHECK(d3dkmt_bind_display(luid,1,screen_name,TRUE)==0);
    CHECK(d3dkmt_bind_display(luid,0,secondary_name,FALSE)==0);
    CHECK(d3dkmt_bind_display(other,0,screen_name,FALSE)==STATUS_OBJECT_NAME_COLLISION);
    CHECK(d3dkmt_bind_display(other,0,secondary_name,TRUE)==STATUS_OBJECT_NAME_COLLISION);
    CHECK(d3dkmt_bind_display(luid,0,secondary_name,TRUE)==STATUS_OBJECT_NAME_COLLISION);
    OKMT_ADAPTER_INFO opened; DWORD source;
    const OKMT_WCHAR *names[]={device_name,lower_device,screen_name,lower_screen,display_alias,secondary_name};
    for (unsigned i=0;i<6;i++) {
        CHECK(d3dkmt_open_adapter_name(33,names[i],i>=2,&opened,&source)==0);
        CHECK(opened.luid.low==luid.low && opened.luid.high==luid.high && opened.sources==2);
        CHECK(source==(i>=2 && i<=4 ? 1u : 0u));
        CHECK(d3dkmt_close_adapter(34,opened.adapter)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_close_adapter(33,opened.adapter)==0);
    }
    CHECK(d3dkmt_unbind_display(luid,1)==0);
    CHECK(d3dkmt_open_adapter_name(33,screen_name,TRUE,&opened,&source)==STATUS_UNSUCCESSFUL);
    CHECK(d3dkmt_open_adapter_name(33,display_alias,TRUE,&opened,&source)==STATUS_UNSUCCESSFUL);
    CHECK(d3dkmt_open_adapter_name(33,device_name,FALSE,&opened,&source)==0);
    OKMT_HANDLE retained=opened.adapter;
    CHECK(d3dkmt_bind_display(luid,1,screen_name,TRUE)==0);
    CHECK(d3dkmt_unregister_adapter(luid)==0);
    CHECK(d3dkmt_open_adapter_name(33,device_name,FALSE,&opened,&source)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_open_adapter_name(33,secondary_name,TRUE,&opened,&source)==STATUS_UNSUCCESSFUL);
    memcpy(desc.device_name,device_name,sizeof(device_name));
    CHECK(d3dkmt_register_adapter(&desc,&luid)==0);
    DWORD version=0;
    CHECK(d3dkmt_query_adapter(33,retained,OKMT_QUERY_DRIVER_VERSION,&version,4,0)==D3DKMT_STATUS_DEVICE_REMOVED);
    CHECK(d3dkmt_close_adapter(33,retained)==0);
    CHECK(d3dkmt_bind_display(luid,1,screen_name,TRUE)==0);
    CHECK(d3dkmt_unregister_adapter(other)==0);
    desc.adapter_type=OKMT_ADAPTER_PROTOTYPE|OKMT_ADAPTER_DISPLAY; desc.device_name[0]=0;
    CHECK(d3dkmt_register_adapter(&desc,&other)==0);
    CHECK(d3dkmt_bind_display(other,0,secondary_name,FALSE)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_unregister_adapter(other)==0);
    desc.adapter_type=OKMT_ADAPTER_RENDER; desc.umd32[0]='a'; desc.umd64[0]='b';
    CHECK(d3dkmt_register_adapter(&desc,&other)==0);
    CHECK(d3dkmt_bind_display(other,0,secondary_name,FALSE)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_unregister_adapter(other)==0);
    for (unsigned i=0;i<260;i++) desc.device_name[i]='x';
    CHECK(d3dkmt_register_adapter(&desc,&other)==STATUS_INVALID_PARAMETER);

    user_bytes=4096;
    user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);
    if (user!=MAP_FAILED) for (int compat=0;compat<=1;compat++) {
        g_compat32_mode=compat;
        size_t pointer_bytes=compat ? 4 : 8;
        memset(user,0x5a,4096);
        uintptr_t name=(uintptr_t)(user+512); memcpy(user,&name,pointer_bytes);
        memcpy(user+512,device_name,sizeof(device_name));
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==0);
        OKMT_HANDLE handle; memcpy(&handle,user+pointer_bytes,4);
        CHECK(d3dkmt_close_adapter(caller,handle)==0);
        CHECK(!memcmp(user,&name,pointer_bytes) && user[pointer_bytes+12]==0x5a);
        /* Caller exit reclaims name-opened handles as ordinary references. */
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==0);
        memcpy(&handle,user+pointer_bytes,4); d3dkmt_release_process(caller);
        CHECK(d3dkmt_close_adapter(caller,handle)==STATUS_INVALID_PARAMETER);
        denied_first=(uintptr_t)(user+pointer_bytes); denied_last=denied_first+1;
        for (unsigned i=0;i<D3DKMT_MAX_HANDLES+2;i++) CHECK(D3DKMTOpenAdapterFromDeviceName(user)==STATUS_INVALID_PARAMETER);
        denied_first=denied_last=0;
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==0);
        memcpy(&handle,user+pointer_bytes,4); CHECK(d3dkmt_close_adapter(caller,handle)==0);
        /* Unknown names preserve outputs; empty paths zero them, as Windows. */
        memset(user+pointer_bytes,0x5a,12); ((OKMT_WCHAR *)(user+512))[1]='x';
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==STATUS_INVALID_PARAMETER);
        CHECK(*(DWORD *)(user+pointer_bytes)==0x5a5a5a5a);
        *(OKMT_WCHAR *)(user+512)=0;
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==STATUS_OBJECT_PATH_SYNTAX_BAD);
        for (unsigned i=0;i<12;i++) CHECK(user[pointer_bytes+i]==0);
        /* String touching a page boundary and malformed, unterminated input. */
        name=(uintptr_t)(user+4096-sizeof(device_name)); memcpy(user,&name,pointer_bytes);
        memcpy((void *)name,device_name,sizeof(device_name));
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==0);
        memcpy(&handle,user+pointer_bytes,4); CHECK(d3dkmt_close_adapter(caller,handle)==0);
        *(OKMT_WCHAR *)(user+4094)='x';
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==STATUS_INVALID_PARAMETER);
        name=(uintptr_t)(user+512); memcpy(user,&name,pointer_bytes);
        for (unsigned i=0;i<260;i++) ((OKMT_WCHAR *)(user+512))[i]='x';
        CHECK(D3DKMTOpenAdapterFromDeviceName(user)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTOpenAdapterFromDeviceName(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTOpenAdapterFromHdc(NULL)==STATUS_INVALID_PARAMETER);
        CHECK(D3DKMTOpenAdapterFromGdiDisplayName(NULL)==STATUS_INVALID_PARAMETER);
        for (unsigned trial=0;trial<2;trial++) {
            memset(user,0x5a,80);
            memcpy(user,trial ? display_alias : screen_name,trial ? sizeof(display_alias) : sizeof(screen_name));
            CHECK(D3DKMTOpenAdapterFromGdiDisplayName(user)==0);
            OKMT_OPEN_ADAPTER_FROM_GDI_NAME *gdi=(void *)user;
            CHECK(gdi->luid.low==luid.low && gdi->source==1 && user[80]==0x5a);
            CHECK(d3dkmt_close_adapter(caller,gdi->adapter)==0);
        }
        memset(user,0x5a,80); *(OKMT_WCHAR *)user=0;
        CHECK(D3DKMTOpenAdapterFromGdiDisplayName(user)==STATUS_UNSUCCESSFUL);
        CHECK(*(DWORD *)(user+64)==0x5a5a5a5a);
        for (unsigned i=0;i<32;i++) ((OKMT_WCHAR *)user)[i]='x';
        CHECK(D3DKMTOpenAdapterFromGdiDisplayName(user)==STATUS_INVALID_PARAMETER);
        memset(user,0x5a,80); uintptr_t dc=0xabcdefu; memcpy(user,&dc,pointer_bytes);
        CHECK(D3DKMTOpenAdapterFromHdc(user)==0);
        memcpy(&handle,user+pointer_bytes,4);
        CHECK(*(DWORD *)(user+pointer_bytes+12)==1 && user[pointer_bytes+16]==0x5a);
        CHECK(d3dkmt_close_adapter(caller,handle)==0);
        dc=1; memcpy(user,&dc,pointer_bytes);
        CHECK(D3DKMTOpenAdapterFromHdc(user)==STATUS_INVALID_PARAMETER);
        for (unsigned i=0;i<16;i++) CHECK(user[pointer_bytes+i]==0);
        for (unsigned i=0;i<D3DKMT_MAX_HANDLES;i++) CHECK(d3dkmt_open_adapter(caller,luid,&handle)==0);
        memset(user,0x5a,80); memcpy(user,screen_name,sizeof(screen_name));
        CHECK(D3DKMTOpenAdapterFromGdiDisplayName(user)==STATUS_NO_MEMORY);
        CHECK(*(DWORD *)(user+64)==0x5a5a5a5a);
        d3dkmt_release_process(caller);
    }
    if (user!=MAP_FAILED) CHECK(munmap(user,4096)==0);
    CHECK(d3dkmt_unregister_adapter(luid)==0);
    CHECK(d3dkmt_open_adapter_name(33,display_alias,TRUE,&opened,&source)==STATUS_UNSUCCESSFUL);
}

int main(void)
{
    if (registry_suite_main()) return 1;
    named_cases();
    printf("KMTNAMES checks=%u failures=%u\n",checks,failures);
    return failures ? 1 : 0;
}
