#define INTEL_DRIVER_SUITE_ENTRY driver_suite_main
#include "intel_gfx_driver_host.c"

static const OKMT_WCHAR primary_name[]={'\\','\\','.','\\','D','I','S','P','L','A','Y','1',0};
static NTSTATUS source_driver_drain(void)
{
    NTSTATUS status=D3DKMT_STATUS_DEVICE_BUSY;
    for (unsigned step=0;step<16 && status==D3DKMT_STATUS_DEVICE_BUSY;step++) {
        executor_step();service_step();status=intel_gfx_driver_stop();
    }
    return status;
}
static void display_cases(void)
{
    platform_display=true;
    platform_state.ready=platform_state.gen9=platform_state.gop_scanout_retained=true;
    platform_state.gop_width=1920;platform_state.gop_height=1080;platform_state.gop_pitch_bytes=7680;
    for (unsigned held=0;held<3;held++) {
        res_reset();model_pid=7;process_closing=false;
        CHECK(intel_gfx_driver_start()==0);
        intel_gfx_driver_snapshot_t state;CHECK(intel_gfx_driver_snapshot(&state)==0);
        OKMT_ADAPTER_INFO a;DWORD source=99;
        CHECK(d3dkmt_open_adapter_name(7,primary_name,TRUE,&a,&source)==0 && source==0 && a.sources==1);
        DWORD type=0;
        CHECK(d3dkmt_query_adapter(7,a.adapter,OKMT_QUERY_ADAPTER_TYPE,&type,4,FALSE)==0 && type==OKMT_ADAPTER_DISPLAY);
        D3DKMT_DISPLAY_SNAPSHOT modes={0};
        CHECK(d3dkmt_acquire_display_modes(7,a.adapter,0,&modes)==0 && modes.count==1);
        CHECK(modes.modes[0].width==1920 && modes.modes[0].height==1080 && modes.modes[0].format==22);
        CHECK(modes.modes[0].refresh_numerator==0 && modes.modes[0].refresh_denominator==1);
        D3DKMT_DISPLAY_LEASE lease={0};D3DKMT_SCANLINE_SAMPLE sample={0};
        CHECK(d3dkmt_acquire_display(7,a.adapter,0,&lease)==0);
        CHECK(d3dkmt_sample_display(&lease,&sample)==0 && sample.scanline==417 && !sample.in_vertical_blank);
        display_source_change_t update={0};
        CHECK(!display_source_begin(&update,NULL));
        OKMT_HANDLE device=0;
        if (held!=0) CHECK(d3dkmt_release_display(&lease)==0);
        if (held==1) CHECK(display_source_begin(&update,&platform_mode));
        if (held==2) CHECK(d3dkmt_create_device(7,a.adapter,0,&device)==0);
        CHECK(intel_gfx_driver_stop()==D3DKMT_STATUS_DEVICE_BUSY);
        display_source_change_t takeover={0};CHECK(!display_source_begin(&takeover,NULL));
        OKMT_ADAPTER_INFO gone;DWORD count=99;
        CHECK(d3dkmt_enumerate_adapters(7,0,NULL,&count)==0 && count==0);
        CHECK(d3dkmt_open_adapter_name(7,primary_name,TRUE,&gone,&source)!=0);
        CHECK(modes.modes[0].width==1920);CHECK(d3dkmt_release_display_modes(&modes)==0);
        if (held==0) {
            CHECK(d3dkmt_sample_display(&lease,&sample)==D3DKMT_STATUS_DEVICE_REMOVED);
            CHECK(d3dkmt_release_display(&lease)==0);
        }
        if (held==1) CHECK(display_source_end(&update));
        if (held==2) CHECK(d3dkmt_destroy_device(7,device)==0);
        CHECK(d3dkmt_close_adapter(7,a.adapter)==0);
        CHECK(source_driver_drain()==0);
        CHECK(display_source_begin(&takeover,NULL));CHECK(display_source_end(&takeover));
        CHECK(intel_gfx_capture_clean());
    }
    /* Binding failure rolls back native resources and the physical reservation,
     * preserving the existing source; registration never forges a second GPU. */
    res_reset();D3DKMT_ADAPTER_DESCRIPTION d={.driver_version=1000,.source_count=1,.adapter_type=OKMT_ADAPTER_DISPLAY};
    OKMT_LUID other;CHECK(d3dkmt_register_adapter(&d,&other)==0);
    CHECK(d3dkmt_bind_display(other,0,primary_name,TRUE)==0);
    CHECK(intel_gfx_driver_start()==STATUS_OBJECT_NAME_COLLISION);
    CHECK(source_driver_drain()==0 && intel_gfx_capture_clean());
    display_source_change_t change={0};CHECK(display_source_begin(&change,NULL));
    CHECK(intel_gfx_driver_start()==D3DKMT_STATUS_DEVICE_BUSY);
    CHECK(display_source_end(&change));CHECK(source_driver_drain()==0);
    CHECK(d3dkmt_unregister_adapter(other)==0);
    platform_timing_fail=true;
    CHECK(intel_gfx_driver_start()==0);
    OKMT_ADAPTER_INFO a;DWORD count=0,type=0;
    CHECK(d3dkmt_enumerate_adapters(7,1,&a,&count)==0 && count==1 && a.sources==0);
    CHECK(d3dkmt_query_adapter(7,a.adapter,OKMT_QUERY_ADAPTER_TYPE,&type,4,FALSE)==0 && type==OKMT_ADAPTER_PROTOTYPE);
    CHECK(d3dkmt_close_adapter(7,a.adapter)==0);CHECK(source_driver_drain()==0);
    platform_timing_fail=false;platform_display=false;
    platform_state.ready=platform_state.gen9=platform_state.gop_scanout_retained=false;
    res_reset();
}
int main(void)
{
    if (driver_suite_main()) return 1;
    unsigned before=checks;display_cases();
    printf("Intel retained display: checks=%u failures=0\n",checks-before);return 0;
}
