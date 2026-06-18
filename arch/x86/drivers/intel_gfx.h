/*
 * OsitoK x86-64 — Intel Gen9 display probe
 *
 * Conservative real-hardware support for Skylake/Kaby/Coffee Lake iGPU
 * display blocks. This is not an i915 replacement: it only discovers the
 * GOP-programmed scanout state and keeps GOP fallback available.
 */

#ifndef OSITOK_INTEL_GFX_H
#define OSITOK_INTEL_GFX_H

#include "../include/types.h"

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

    int      active_pipe;
    int      active_plane;
    uint32_t pipeconf;
    uint32_t plane_ctl;
    uint32_t plane_stride;
    uint32_t plane_size;
    uint32_t plane_surf;
} intel_gfx_state_t;

bool intel_gfx_is_gen9_device(uint16_t device_id);
const char *intel_gfx_device_name(uint16_t device_id);

int intel_gfx_init(uint16_t device_id, uint8_t bus, uint8_t dev, uint8_t func,
                   uint64_t bar0_phys, uint64_t bar2_phys,
                   uint64_t gop_fb_phys, uint32_t width, uint32_t height,
                   uint32_t pitch_bytes);

int intel_gfx_is_ready(void);
const intel_gfx_state_t *intel_gfx_get_state(void);

#endif /* OSITOK_INTEL_GFX_H */
