/*
 * OsitoK x86-64 — Intel Gen9 display probe
 *
 * Conservative real-hardware support for Skylake/Kaby/Coffee Lake iGPU
 * display blocks. This is not an i915 replacement: it only discovers the
 * GOP-programmed scanout state and keeps GOP fallback available.
 */

#ifndef OSITOK_INTEL_GFX_H
#define OSITOK_INTEL_GFX_H

#include "intel_ggtt.h"

typedef struct {
    bool     present;
    bool     ready;
    bool     gen9;
    bool     gop_scanout_retained;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;

    uint64_t bar0_phys;
    uint64_t bar2_phys;
    uint64_t gop_fb_phys;
    uint32_t gop_width;
    uint32_t gop_height;
    uint32_t gop_pitch_bytes;

    /* Firmware memory discovery is independent of scanout detection/ready.
     * Neither this snapshot nor sampled PTEs authorize allocation or DMA. */
    bool memory_valid;
    bool gop_in_aperture;
    bool gop_sample_present;
    intel_ggtt_config_t memory_config;
    intel_ggtt_layout_t memory;
    uint64_t gop_ggtt_offset;
    uint64_t gop_first_pte, gop_last_pte;
    uint64_t gop_first_phys, gop_last_phys;
    bool fence_snapshot_valid;
    uint32_t fence_valid_mask;
    uint64_t fence[32];
    /* Read-only VT-d observation, not a prepared DMA domain or render-ready
     * state. Revalidate at actual driver takeover, before binding any page. */
    bool dma_state_valid, dma_identity_confirmed;
    uint32_t dma_gsts, dma_pmen, dma_target_faults;

    int      active_pipe;
    int      active_plane;
    int      active_transcoder; /* 0..2=A/B/C, 3=eDP; not a pipe index */
    int      active_port;       /* 0..4=DDI A..E */
    uint32_t power_well, fuse_status;
    uint32_t trans_ddi, ddi_buf, trans_htotal, trans_vtotal, pipe_src;
    uint32_t pipeconf;
    uint32_t plane_ctl;
    uint32_t plane_stride;
    uint32_t plane_size;
    uint32_t plane_surf;
    uint32_t plane_surf_live, plane_offset, plane_pos;
} intel_gfx_state_t;

bool intel_gfx_is_gen9_device(uint16_t device_id);
const char *intel_gfx_device_name(uint16_t device_id);

int intel_gfx_init(uint16_t device_id, uint8_t bus, uint8_t dev, uint8_t func,
                   uint64_t bar0_phys, uint64_t bar2_phys,
                   uint64_t gop_fb_phys, uint32_t width, uint32_t height,
                   uint32_t pitch_bytes);

int intel_gfx_is_ready(void);
const intel_gfx_state_t *intel_gfx_get_state(void);

/* Read-only observation of the retained, progressive Gen9 GOP output.
 * No display ownership, modeset capability or synthetic clock is implied.
 * Output is unchanged on failure: 0 success, 1 unavailable, 2 unstable.
 * Fields describe a single frame; frame_count wraps naturally at 32 bits. */
typedef struct {
    uint32_t frame_count, frame_timestamp, raw_scanline, scanline;
    uint32_t active_lines, total_lines;
    bool in_vertical_blank;
} intel_gfx_timing_t;
int intel_gfx_read_timing(intel_gfx_timing_t *);
void intel_gfx_timing_test(void);

/* Opt-in, one-shot physical GGTT qualification, not render acceleration. */
void intel_gfx_memory_test(const intel_gfx_state_t *);

/* A diagnostic may retain DMA references after failed engine retirement.
 * Check before tearing down devices for kexec; PCI BME is not a GT drain. */
bool intel_gfx_kexec_ready(void);
/* Opt-in post-boot native service test: 0 pass, 1 fail, 2 unavailable. */
int intel_gfx_bcs_service_test(void);

#endif /* OSITOK_INTEL_GFX_H */
