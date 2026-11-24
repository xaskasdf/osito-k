/* Opt-in, bounded read-only qualification of the actual display counters. */
#include "intel_gfx.h"
#include "intel_gfx_display.h"
extern uint64_t idt_get_monotonic_ns(void);
extern void serial_puts(const char *);
extern void serial_putdec(uint64_t);

void intel_gfx_timing_test(void)
{
    uint64_t start=idt_get_monotonic_ns(),elapsed=0;
    unsigned samples=0,active=0,blank=0,retries=0,errors=0,frames=0;
    uint32_t first_frame=0,previous_frame=0,previous_timestamp=0;
    uint32_t minimum=UINT32_MAX,maximum=0,edge_min=UINT32_MAX,edge_max=0;
    intel_gfx_timing_t sample={0};
    /* Time bound plus work bound also handles a broken monotonic clock.
     * No MMIO writes, spinlocks, interrupt masking or GPU driver takeover. */
    while (samples+retries+errors<200000 && elapsed<250000000ULL) {
        int status=intel_gfx_read_timing(&sample);
        if (status==2) retries++;
        else if (status) { errors++; break; }
        else {
            if (!samples) first_frame=previous_frame=sample.frame_count;
            else if (sample.frame_count!=previous_frame) {
                uint32_t advance=sample.frame_count-previous_frame;
                if (advance>2 || sample.frame_timestamp==previous_timestamp) errors++;
                frames+=advance;
                if (sample.raw_scanline<edge_min) edge_min=sample.raw_scanline;
                if (sample.raw_scanline>edge_max) edge_max=sample.raw_scanline;
            }
            previous_frame=sample.frame_count;previous_timestamp=sample.frame_timestamp;
            if (sample.in_vertical_blank) blank++; else active++;
            if (sample.raw_scanline<minimum) minimum=sample.raw_scanline;
            if (sample.raw_scanline>maximum) maximum=sample.raw_scanline;
            samples++;
        }
        elapsed=idt_get_monotonic_ns()-start;
    }
    bool passed=!errors && samples>100 && active && blank && frames>=4;
    serial_puts("[IGFX] native scanout timing passed=");serial_putdec(passed);
    serial_puts(" samples=");serial_putdec(samples);
    serial_puts(" active=");serial_putdec(active);
    serial_puts(" blank=");serial_putdec(blank);
    serial_puts(" retries=");serial_putdec(retries);
    serial_puts(" errors=");serial_putdec(errors);
    serial_puts(" frame_start=");serial_putdec(first_frame);
    serial_puts(" frames=");serial_putdec(frames);
    serial_puts(" raw_min=");serial_putdec(minimum);
    serial_puts(" raw_max=");serial_putdec(maximum);
    serial_puts(" edge_min=");serial_putdec(edge_min);
    serial_puts(" edge_max=");serial_putdec(edge_max);
    serial_puts(" active_lines=");serial_putdec(sample.active_lines);
    serial_puts(" total_lines=");serial_putdec(sample.total_lines);
    serial_puts(" elapsed_ns=");serial_putdec(elapsed);
    serial_puts(" mean_query_ns=");serial_putdec(samples ? elapsed/samples : 0);
    serial_puts("\n");
    /* Exercise the native KMT callback without publishing a display adapter.
     * Ownership/binding belongs to the display service, not this diagnostic. */
    const D3DKMT_DISPLAY_PROVIDER *provider=intel_gfx_display_provider();
    unsigned queries=0,failures=0;
    for (unsigned i=0;i<64;i++) {
        D3DKMT_SCANLINE_SAMPLE value={2,UINT32_MAX};
        NTSTATUS status=provider->scanline(provider->context,0,&value);
        if (status || (value.in_vertical_blank!=FALSE && value.in_vertical_blank!=TRUE) ||
            value.scanline>=sample.total_lines ||
            value.in_vertical_blank!=(value.scanline>=sample.active_lines)) failures++;
        queries++;
    }
    D3DKMT_SCANLINE_SAMPLE value={2,UINT32_MAX};
    if (provider->scanline(provider->context,1,&value)!=STATUS_INVALID_PARAMETER ||
        value.in_vertical_blank!=2 || value.scanline!=UINT32_MAX) failures++;
    if (provider->scanline(provider->context,0,NULL)!=STATUS_INVALID_PARAMETER) failures++;
    serial_puts("[IGFX] KMT scanline backend passed=");serial_putdec(!failures);
    serial_puts(" queries=");serial_putdec(queries);
    serial_puts(" failures=");serial_putdec(failures);serial_puts("\n");
}
