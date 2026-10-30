#define main scan_registry_suite_main
#define win32_user_range_writable scan_base_writable
#include "d3dkmt_registry_host.c"
#undef main
#undef win32_user_range_writable
#include <stdlib.h>

static const OKMT_WCHAR scan_name[]=u"\\\\.\\DISPLAY1";
static struct { OKMT_LUID luid;unsigned calls,action; } scan_context;
static unsigned copy_action;
static NTSTATUS provider_result;
static BOOL provider_blank;
static DWORD provider_line;
BOOL gdi32_dc_display_name(HANDLE dc,WCHAR name[32]) { (void)dc;(void)name;return FALSE; }
static NTSTATUS sample_scanline(void *context,DWORD source,D3DKMT_SCANLINE_SAMPLE *out)
{
    CHECK(context==&scan_context && source==1);
    scan_context.calls++;
    unsigned action=scan_context.action;scan_context.action=0;
    if (action==1) CHECK(d3dkmt_unregister_adapter(scan_context.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    if (action==2) CHECK(d3dkmt_unbind_display(scan_context.luid,1)==0);
    if (action==3) CHECK(d3dkmt_set_display_provider(scan_context.luid,NULL)==D3DKMT_STATUS_DEVICE_BUSY);
    if (action==4) { cancellation_requested=TRUE;win32_main_termination_checkpoint(); }
    *out=(D3DKMT_SCANLINE_SAMPLE){provider_blank,provider_line};return provider_result;
}
static const D3DKMT_DISPLAY_PROVIDER scan_provider={sample_scanline,&scan_context};
int win32_user_range_writable(void *p,SIZE_T bytes,BOOL compat)
{
    unsigned action=copy_action;copy_action=0;
    if (action==1) CHECK(d3dkmt_unbind_display(scan_context.luid,1)==0);
    if (action==2) CHECK(d3dkmt_unregister_adapter(scan_context.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    return scan_base_writable(p,bytes,compat);
}
static OKMT_HANDLE scan_setup(void)
{
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000,.source_count=2,.adapter_type=OKMT_ADAPTER_DISPLAY};
    CHECK(d3dkmt_register_adapter(&description,&scan_context.luid)==0);
    CHECK(d3dkmt_bind_display(scan_context.luid,1,scan_name,TRUE)==0);
    CHECK(d3dkmt_set_display_provider(scan_context.luid,&scan_provider)==0);
    OKMT_HANDLE h=0;CHECK(d3dkmt_open_adapter(caller,scan_context.luid,&h)==0);
    provider_result=0;provider_blank=FALSE;provider_line=417;
    scan_context.calls=scan_context.action=copy_action=0;
    return h;
}
static void scan_close(OKMT_HANDLE h)
{
    CHECK(d3dkmt_close_adapter(caller,h)==0);
    CHECK(d3dkmt_unregister_adapter(scan_context.luid)==0);
}
static void header(OKMT_HANDLE h,DWORD source)
{
    memset(user,0x5a,user_bytes);memcpy(user,&h,4);memcpy(user+4,&source,4);
}
static BOOL unchanged(void)
{
    for (unsigned i=8;i<user_bytes;i++) if (user[i]!=0x5a) return FALSE;
    return TRUE;
}
static void publication_cases(void)
{
    OKMT_HANDLE h=scan_setup();D3DKMT_DISPLAY_LEASE lease={0};D3DKMT_SCANLINE_SAMPLE sample;
    CHECK(d3dkmt_acquire_display(caller+1,h,1,&lease)==STATUS_INVALID_PARAMETER && !lease.self);
    CHECK(d3dkmt_acquire_display(caller,h,2,&lease)==STATUS_INVALID_PARAMETER && !lease.self);
    CHECK(d3dkmt_acquire_display(caller,h,1,&lease)==0);
    CHECK(d3dkmt_acquire_display(caller,h,1,&lease)==STATUS_INVALID_PARAMETER);
    D3DKMT_DISPLAY_LEASE copied=lease;
    CHECK(d3dkmt_release_display(&copied)==STATUS_INVALID_PARAMETER);
    CHECK(d3dkmt_set_display_provider(scan_context.luid,NULL)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_set_display_provider(scan_context.luid,&scan_provider)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_sample_display(&lease,&sample)==0 && sample.scanline==417 && !sample.in_vertical_blank);
    CHECK(d3dkmt_unbind_display(scan_context.luid,1)==0);
    CHECK(d3dkmt_bind_display(scan_context.luid,1,scan_name,TRUE)==0);
    CHECK(d3dkmt_sample_display(&lease,&sample)==D3DKMT_STATUS_PRESENT_OCCLUDED);
    CHECK(d3dkmt_unregister_adapter(scan_context.luid)==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(d3dkmt_display_status(&lease)==D3DKMT_STATUS_DEVICE_REMOVED);
    /* An admitted callback keeps the provider's adapter slot occupied. */
    D3DKMT_ADAPTER_DESCRIPTION description={.driver_version=1000};OKMT_LUID others[16];
    for (unsigned i=0;i<15;i++) CHECK(d3dkmt_register_adapter(&description,&others[i])==0);
    CHECK(d3dkmt_register_adapter(&description,&others[15])==STATUS_NO_MEMORY);
    CHECK(d3dkmt_release_display(&lease)==0 && !lease.self);
    CHECK(d3dkmt_release_display(&lease)==STATUS_INVALID_PARAMETER);
    scan_close(h);
    CHECK(d3dkmt_register_adapter(&description,&others[15])==0);
    for (unsigned i=0;i<16;i++) CHECK(d3dkmt_unregister_adapter(others[i])==0);
    for (unsigned kind=0;kind<2;kind++) {
        description=(D3DKMT_ADAPTER_DESCRIPTION){.driver_version=1000,.source_count=2,.umd32={'x'},.umd64={'x'},
            .adapter_type=kind ? OKMT_ADAPTER_DISPLAY|OKMT_ADAPTER_PROTOTYPE : OKMT_ADAPTER_RENDER};
        CHECK(d3dkmt_register_adapter(&description,&scan_context.luid)==0);
        CHECK(d3dkmt_open_adapter(caller,scan_context.luid,&h)==0);
        CHECK(d3dkmt_set_display_provider(scan_context.luid,&scan_provider)==STATUS_NOT_SUPPORTED);
        CHECK(d3dkmt_acquire_display(caller,h,1,&lease)==STATUS_NOT_SUPPORTED);
        scan_close(h);
    }
}
static void copyout_cases(void)
{
    for (unsigned compat=0;compat<2;compat++) {
        g_compat32_mode=compat;
        for (unsigned action=0;action<11;action++) {
            OKMT_HANDLE h=scan_setup();header(h,1);
            NTSTATUS expected=STATUS_SUCCESS;
            switch (action) {
            case 0:break;
            case 1:provider_blank=TRUE;provider_line=1082;break;
            case 2:header(h,0);break; /* inactive source, no provider call */
            case 3:scan_context.action=1;expected=D3DKMT_STATUS_DEVICE_REMOVED;break;
            case 4:scan_context.action=2;expected=D3DKMT_STATUS_PRESENT_OCCLUDED;break;
            case 5:scan_context.action=3;break;
            case 6:provider_blank=2;expected=STATUS_UNSUCCESSFUL;break;
            case 7:provider_result=STATUS_NO_MEMORY;expected=STATUS_NO_MEMORY;break;
            case 8:copy_action=1;expected=D3DKMT_STATUS_PRESENT_OCCLUDED;break;
            case 9:copy_action=2;expected=D3DKMT_STATUS_DEVICE_REMOVED;break;
            case 10:scan_context.action=4;expected=STATUS_PROCESS_IS_TERMINATING;break;
            }
            unsigned deliveries=cancellation_deliveries;
            CHECK(D3DKMTGetScanLine(user)==expected);
            if (expected) CHECK(unchanged());
            else {
                OKMT_GET_SCANLINE *out=(void *)user;
                CHECK(out->adapter==h && out->source==(action==2?0u:1u));
                CHECK(out->in_vertical_blank==(action==2?TRUE:provider_blank));
                CHECK(out->scanline==(action==2?0u:provider_line));
                CHECK(user[9]==0x5a && user[10]==0x5a && user[11]==0x5a && user[16]==0x5a);
            }
            CHECK(scan_context.calls==(action==2?0u:1u));
            if (action==10) { CHECK(cancellation_deliveries==deliveries+1);cancellation_requested=FALSE; }
            scan_close(h);
        }
        OKMT_HANDLE h=scan_setup();
        for (unsigned byte=8;byte<16;byte++) {
            header(h,1);denied_first=(uintptr_t)user+byte;denied_last=denied_first+1;
            NTSTATUS status=D3DKMTGetScanLine(user);
            if (byte>=9 && byte<=11) CHECK(status==0); /* padding isn't an output */
            else CHECK(status==STATUS_INVALID_PARAMETER && unchanged());
        }
        denied_first=denied_last=0;
        header(h,1);CHECK(D3DKMTGetScanLine(NULL)==STATUS_INVALID_PARAMETER && unchanged());
        header(h,1);caller++;CHECK(D3DKMTGetScanLine(user)==STATUS_INVALID_PARAMETER && unchanged());caller--;
        header(h,0xffffffffu);CHECK(D3DKMTGetScanLine(user)==STATUS_INVALID_PARAMETER && unchanged());
        scan_close(h);header(h,1);CHECK(D3DKMTGetScanLine(user)==STATUS_INVALID_PARAMETER && unchanged());
    }
}
static void *lease_worker(void *arg)
{
    OKMT_HANDLE h=(OKMT_HANDLE)(uintptr_t)arg;
    for (unsigned i=0;i<4000;i++) {
        D3DKMT_DISPLAY_LEASE lease={0};D3DKMT_SCANLINE_SAMPLE sample;
        CHECK(d3dkmt_acquire_display(100,h,0,&lease)==0);
        CHECK(d3dkmt_sample_display(&lease,&sample)==0 && sample.in_vertical_blank && !sample.scanline);
        CHECK(d3dkmt_release_display(&lease)==0);
    }
    return NULL;
}
int main(void)
{
    CHECK(scan_registry_suite_main()==0);
    user_bytes=4096;user=mmap(NULL,user_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    CHECK(user!=MAP_FAILED);if(user==MAP_FAILED) return 1;
    caller=100;publication_cases();copyout_cases();
    OKMT_HANDLE h=scan_setup();pthread_t workers[4];
    for (unsigned i=0;i<4;i++) CHECK(!pthread_create(workers+i,NULL,lease_worker,(void *)(uintptr_t)h));
    for (unsigned i=0;i<4;i++) CHECK(!pthread_join(workers[i],NULL));
    scan_close(h);CHECK(munmap(user,user_bytes)==0);
    printf("D3DKMT scanline: checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
