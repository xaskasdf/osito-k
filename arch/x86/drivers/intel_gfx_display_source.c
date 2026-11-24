#include "intel_gfx_display.h"
#include "intel_gfx.h"

NTSTATUS intel_gfx_display_acquire(display_source_owner_t *owner)
{
    if (!owner || owner->self) return STATUS_INVALID_PARAMETER;
    display_source_change_t change={0};
    if (!display_source_begin(&change,NULL)) return D3DKMT_STATUS_DEVICE_BUSY;
    display_source_mode_t mode;
    const intel_gfx_state_t *hardware=intel_gfx_get_state();
    intel_gfx_timing_t timing;
    NTSTATUS status=STATUS_NOT_SUPPORTED;
    if (hardware && hardware->ready && hardware->gen9 && hardware->gop_scanout_retained &&
        display_get_retained_gop(&mode) && mode.width==hardware->gop_width &&
        mode.height==hardware->gop_height && (uint64_t)mode.pitch*4==hardware->gop_pitch_bytes &&
        !intel_gfx_read_timing(&timing)) {
        status=display_source_adopt(&change,owner,&mode) ? STATUS_SUCCESS : D3DKMT_STATUS_DEVICE_BUSY;
    }
    if (change.self) (void)display_source_end(&change);
    return status;
}

NTSTATUS intel_gfx_display_bind(OKMT_LUID luid,const display_source_owner_t *owner)
{
    display_source_mode_t mode;
    if (!display_source_mode(owner,&mode)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_set_display_provider(luid,intel_gfx_display_provider());
    if (status) return status;
    static const OKMT_WCHAR primary[]={'\\','\\','.','\\','D','I','S','P','L','A','Y','1',0};
    status=d3dkmt_bind_display(luid,0,primary,TRUE);
    if (status) return status;
    /* The verified native plane is linear XRGB8888, progressive and unrotated.
     * GUI pacing is not a physical refresh measurement. 0/1 means unknown;
     * firmware alternatives are not modes we can switch to at runtime. */
    OKMT_DISPLAY_MODE current={.width=mode.width,.height=mode.height,.format=22,
        .refresh_denominator=1,.scanline_order=1,.orientation=1};
    status=d3dkmt_publish_display_modes(luid,0,&current,1);
    if (status) (void)d3dkmt_unbind_display(luid,0);
    return status;
}
