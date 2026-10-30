#define main identity_suite_main
#include "display_identity_host.c"
#undef main
boot_info_t saved_boot_info;
static bool hardware_ready;
static uint32_t *firmware_view=(void *)0x100000;
int gpu_display_is_ready(void) { return hardware_ready; }
uint32_t *fb_get_vram(void) { return firmware_view; }
int main(void)
{
    if (identity_suite_main()) return 1;
    unsigned before=checks;
    memset(&disp,0,sizeof(disp));virtio_ready=false;virtio_vga=false;
    display_source_mode_t out={(void *)1,2,3,4};
    CHECK(!display_get_retained_gop(&out) && out.width==2);
    saved_boot_info.fb_base=0x100000;saved_boot_info.fb_width=1920;
    saved_boot_info.fb_height=1080;saved_boot_info.fb_pitch=1920;
    CHECK(display_get_retained_gop(&out) && out.base==firmware_view && out.width==1920);
    hardware_ready=true;CHECK(!display_get_retained_gop(&out));hardware_ready=false;
    virtio_ready=true;virtio_vga=true;CHECK(!display_get_retained_gop(&out));virtio_vga=false;
    CHECK(display_get_retained_gop(&out));
    disp.initialized=true;CHECK(!display_get_retained_gop(&out));
    disp.gop_fb=firmware_view;disp.gop_width=disp.width=1920;
    disp.gop_height=disp.height=1080;disp.gop_pitch=disp.pitch=1920;
    CHECK(display_get_retained_gop(&out));
    disp.width=1280;CHECK(!display_get_retained_gop(&out));disp.width=1920;
    disp.gop_fb=(void *)0x200000;CHECK(!display_get_retained_gop(&out));disp.gop_fb=firmware_view;
    disp.gpu_scanout=true;CHECK(!display_get_retained_gop(&out));disp.gpu_scanout=false;
    saved_boot_info.fb_pitch=100;CHECK(!display_get_retained_gop(&out));
    CHECK(!display_get_retained_gop(NULL));
    printf("Retained GOP view: %u checks, 0 failures\n",checks-before);return 0;
}
