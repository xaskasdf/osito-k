#include "../drivers/intel_gfx_display.h"
#include "../drivers/intel_gfx.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures,calls;
static int native_result;
static intel_gfx_timing_t timing;
static bool primary=true,withdraw_during_read;
static unsigned primary_queries;
#define CHECK(x) do { checks++;if (!(x)) { failures++;printf("FAIL line %d\n",__LINE__); } } while (0)
bool display_gop_is_primary(void) { primary_queries++;return primary; }
int intel_gfx_read_timing(intel_gfx_timing_t *out)
{
    calls++;
    if (!native_result) *out=timing;
    if (withdraw_during_read) primary=false;
    return native_result;
}
int main(void)
{
    const D3DKMT_DISPLAY_PROVIDER *p=intel_gfx_display_provider();
    CHECK(p && p->scanline);
    for (unsigned blank=0;blank<2;blank++) {
        timing=(intel_gfx_timing_t){.scanline=blank?1082:417,.in_vertical_blank=blank};
        D3DKMT_SCANLINE_SAMPLE s={2,UINT32_MAX};
        CHECK(p->scanline(p->context,0,&s)==0);
        CHECK(s.in_vertical_blank==(BOOL)blank && s.scanline==timing.scanline);
    }
    D3DKMT_SCANLINE_SAMPLE s={2,UINT32_MAX};
    for (native_result=1;native_result<=2;native_result++) {
        CHECK(p->scanline(p->context,0,&s)==(native_result==1?D3DKMT_STATUS_PRESENT_OCCLUDED:D3DKMT_STATUS_DEVICE_BUSY));
        CHECK(s.in_vertical_blank==2 && s.scanline==UINT32_MAX);
    }
    unsigned before=calls;
    CHECK(p->scanline(p->context,1,&s)==STATUS_INVALID_PARAMETER);
    CHECK(p->scanline(p->context,0,NULL)==STATUS_INVALID_PARAMETER);
    CHECK(calls==before && s.in_vertical_blank==2 && s.scanline==UINT32_MAX);
    /* A running Intel plane may be secondary after a GUI backend takeover.
     * Reject before MMIO, or discard the capture if takeover occurs in MMIO. */
    native_result=0;primary=false;
    CHECK(p->scanline(p->context,0,&s)==D3DKMT_STATUS_PRESENT_OCCLUDED);
    CHECK(calls==before && s.in_vertical_blank==2 && s.scanline==UINT32_MAX);
    primary=true;withdraw_during_read=true;
    CHECK(p->scanline(p->context,0,&s)==D3DKMT_STATUS_PRESENT_OCCLUDED);
    CHECK(calls==before+1 && s.in_vertical_blank==2 && s.scanline==UINT32_MAX);
    before=primary_queries;
    CHECK(p->scanline(p->context,1,&s)==STATUS_INVALID_PARAMETER);
    CHECK(p->scanline(p->context,0,NULL)==STATUS_INVALID_PARAMETER);
    CHECK(primary_queries==before);
    withdraw_during_read=false;primary=true;
    CHECK(p->scanline(p->context,0,&s)==0);
    CHECK(s.in_vertical_blank==(BOOL)timing.in_vertical_blank && s.scanline==timing.scanline);
    printf("Intel display provider: checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
