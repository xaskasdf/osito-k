/* Owned Gen9 2D image storage and immutable sampling state. */
#ifndef OSITOK_INTEL_GEN9_TEXTURE_H
#define OSITOK_INTEL_GEN9_TEXTURE_H
#include <stdint.h>
#include <stdbool.h>

#define INTEL_GEN9_IMAGE_LEVELS 13u
enum intel_gen9_image_format {
    INTEL_GEN9_IMAGE_BGRA8=21, INTEL_GEN9_IMAGE_BGRX8=22
};
typedef struct { uint32_t x,y,width,height,offset; } intel_gen9_image_level_t;
typedef struct {
    uint32_t width,height,levels,format,pitch,rows,bytes;
    intel_gen9_image_level_t mip[INTEL_GEN9_IMAGE_LEVELS];
} intel_gen9_image_t;
/* Linear, single-sample, single-layer BGRA8/BGRX8, HALIGN/VALIGN 4, no aux.
 * All levels share one pitch and allocation. pitch=0 selects 64B alignment;
 * an explicit pitch must be a multiple of 4 and fit the entire mip layout.
 * bytes is rounded to pages; every page and padding byte must be initialized
 * and owned before GPU publication. This function performs no allocation;
 * the UMD uses this same canonical layout for its whole-image allocation. */
bool intel_gen9_image_layout(uint32_t width,uint32_t height,uint32_t levels,
    uint32_t format,uint32_t pitch,intel_gen9_image_t *);
/* Only kernel-owned, pinned canonical layouts/GPU mappings. No retention or
 * cache transitions occur here; the caller retains the complete allocation
 * until retirement. first/count is a view of the original image, not a
 * rebased pointer to one mip. srgb selects a hardware format conversion. */
bool intel_gen9_image_state(const intel_gen9_image_t *,uint64_t gpu,uint64_t bytes,
    uint32_t first,uint32_t count,uint32_t srgb,uint32_t state[16]);

typedef struct {
    uint32_t min_filter,mag_filter,mip_filter; /* point=1, linear=2; mip none=0 */
    uint32_t address_u,address_v,address_w; /* wrap=1, mirror=2, clamp=3, border=4, mirror-once=5 */
    int32_t lod_bias; /* signed Q8, [-4096,4095] */
    uint32_t min_lod,max_lod; /* unsigned Q8, [0,4095] */
    uint32_t border_argb;
} intel_gen9_sampler_t;
typedef struct { uint32_t state[4],border[4]; } intel_gen9_sampler_state_t;
/* Normalized coordinates, no anisotropy/comparison, immutable native state.
 * border_offset is a nonzero 64B-aligned offset into the owned dynamic-state
 * allocation. Caller publishes border[] there and keeps it resident. Values
 * outside this explicit subset are rejected, never silently approximated.
 * No input API accepts application-supplied hardware commands. */
bool intel_gen9_sampler_state(const intel_gen9_sampler_t *,uint32_t border_offset,
    uint32_t dynamic_bytes,intel_gen9_sampler_state_t *);
#endif
