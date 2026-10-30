#include "intel_gfx_display.h"
#include "intel_gfx.h"

extern bool display_gop_is_primary(void);

static NTSTATUS intel_display_scanline(void *context,DWORD source,D3DKMT_SCANLINE_SAMPLE *output)
{
    (void)context;
    if (!output || source) return STATUS_INVALID_PARAMETER;
    /* The Intel firmware plane can remain enabled after another backend
     * becomes the visible desktop. Its timing is then not the primary source.
     * Check on both sides of MMIO, and never publish a rejected sample. */
    if (!display_gop_is_primary()) return D3DKMT_STATUS_PRESENT_OCCLUDED;
    intel_gfx_timing_t sample;
    int result=intel_gfx_read_timing(&sample);
    if (result) return result==2 ? D3DKMT_STATUS_DEVICE_BUSY : D3DKMT_STATUS_PRESENT_OCCLUDED;
    if (!display_gop_is_primary()) return D3DKMT_STATUS_PRESENT_OCCLUDED;
    *output=(D3DKMT_SCANLINE_SAMPLE){sample.in_vertical_blank,sample.scanline};
    return STATUS_SUCCESS;
}

const D3DKMT_DISPLAY_PROVIDER *intel_gfx_display_provider(void)
{
    static const D3DKMT_DISPLAY_PROVIDER provider={intel_display_scanline,NULL};
    return &provider;
}
