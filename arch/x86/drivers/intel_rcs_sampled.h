#ifndef OSITOK_INTEL_RCS_SAMPLED_H
#define OSITOK_INTEL_RCS_SAMPLED_H
#include "intel_rcs_draw.h"
#include "intel_gen9_texture.h"
#include "intel_gen9_vertex.h"
#define INTEL_RCS_SAMPLED_BYTES 16384u
#define INTEL_RCS_SAMPLED_BT 0x2000u
#define INTEL_RCS_SAMPLED_SURFACES 0x2080u
#define INTEL_RCS_SAMPLED_SAMPLERS 0x2480u
#define INTEL_RCS_SAMPLED_BORDERS 0x2580u
typedef struct {
    uint64_t gpu,bytes;
    intel_gen9_image_t image;
    uint32_t first,count,srgb;
    intel_gen9_sampler_t sampler;
} intel_rcs_texture_binding_t;
typedef struct {
    intel_rcs_primitive_draw_t primitive;
    uint32_t bound_mask,texcoord_offset[8];
    uint32_t typed_vertices,position_offset,texcoord_type[8];
    intel_rcs_texture_binding_t textures[16];
} intel_rcs_sampled_draw_t;
/* Separate immutable index range, never expanded or rebased on the CPU.
 * primitive.vertex_count bounds addressable vertex entries; count below is
 * the number of indices submitted to the hardware. base_vertex is signed.
 * min_index/num_vertices describe the original D3D index range. */
typedef struct {
    uint64_t gpu,bytes;
    uint32_t offset,count,stride;
    int32_t base_vertex;
    uint32_t min_index,num_vertices;
} intel_rcs_index_binding_t;
/* CPU view must name the pinned index allocation and stay immutable through
 * GPU retirement. Validation reads indices but does not execute geometry. */
bool intel_rcs_index_validate(const intel_rcs_index_binding_t *,const void *,size_t,
    uint32_t vertex_count,uint32_t topology);
/* ABI3, screen-space POSITIONT, no vertex shader/depth. typed_vertices=0
 * retains the original implicit float4 layout at position offset zero;
 * typed_vertices=1 fetches TEXCOORDs with their D3DDECLTYPE and explicit
 * POSITIONT offset. Formats expand to float4 in hardware.
 * Input metadata and native instructions must be validated and kernel-owned;
 * this builder validates program metadata, bindings and ranges, not EU code.
 * All GPU mappings must be pinned, immutable and retained until retirement.
 * Sampled textures may alias each other but cannot alias any of the four
 * geometry/program/destination/batch allocations. Capture/UMD admission is
 * deliberately separate: this API does not publish commands to an engine. */
bool intel_rcs_sampled_build(uint32_t *,size_t,const intel_rcs_sampled_draw_t *,
    const struct intel_d3d9_program *,size_t,const intel_rcs_scissor_t *,unsigned);

/* Internal emitter input, produced only after full validation. */
typedef struct {
    const struct intel_rcs_vertex_state *vertex;
    const intel_rcs_index_binding_t *indices;
    uint32_t mask,count,vertex_bytes,position_offset,attribute_offset[9];
    uint32_t attribute_format[9],attribute_control[9];
    uint32_t surface[16][16];
    intel_gen9_sampler_state_t sampler[16];
} intel_rcs_sampling_state_t;
bool intel_rcs_bound_emit(uint32_t *,const intel_rcs_primitive_draw_t *,
    const struct intel_d3d9_program *,const intel_rcs_scissor_t *,unsigned,
    const intel_rcs_sampling_state_t *);
#endif
