#ifndef OSITOK_INTEL_RCS_VERTEX_H
#define OSITOK_INTEL_RCS_VERTEX_H
#include "intel_rcs_sampled.h"

#define INTEL_RCS_VERTEX_CONSTANTS 0x3000u
/* Native vertex stage, one typed stream. Input arrays are indexed by D3D vN;
 * inactive entries must be zero. Viewport depth is [0,1], with D3D clip space.
 * No CPU vertex transformation, depth buffer or vertex texture is involved.
 * Code/input allocations must be independently validated, pinned and immutable
 * until confirmed retirement. The internal builder checks metadata/ranges. */
typedef struct {
    uint64_t program_gpu;
    uint32_t attribute_offset[16],attribute_type[16];
    uint32_t viewport_x,viewport_y,viewport_width,viewport_height;
    intel_rcs_constants_t constants;
} intel_rcs_vertex_binding_t;

bool intel_rcs_vertex_build(uint32_t *,size_t,const intel_rcs_sampled_draw_t *,
    const struct intel_d3d9_program *,size_t,const intel_rcs_vertex_binding_t *,
    const struct intel_d3d9_vertex_program *,size_t,const intel_rcs_scissor_t *,unsigned);
/* Native indexed draw with either a VS or POSITIONT. A NULL VS binding and
 * program select POSITIONT. Index data and both programs are caller-owned,
 * pinned and immutable until retirement; this is not a public submission ABI. */
bool intel_rcs_indexed_build(uint32_t *,size_t,const intel_rcs_sampled_draw_t *,
    const struct intel_d3d9_program *,size_t,const intel_rcs_vertex_binding_t *,
    const struct intel_d3d9_vertex_program *,size_t,const intel_rcs_index_binding_t *,
    const void *index_data,size_t,const intel_rcs_scissor_t *,unsigned);

/* Prepared state shared by the validated builder and the common emitter. */
typedef struct intel_rcs_vertex_state {
    struct intel_d3d9_vertex_program program;
    uint64_t program_gpu,instruction_gpu;
    uint32_t instruction_bytes,pixel_offset,kernel_offset,vertex_bytes,sbe_read_length;
    uint32_t attribute_offset[16],attribute_format[16],attribute_control[16];
    uint32_t viewport[16];
    intel_rcs_constants_t constants;
} intel_rcs_vertex_state_t;
bool intel_rcs_sampling_build(uint32_t *,size_t,const intel_rcs_sampled_draw_t *,
    const struct intel_d3d9_program *,size_t,const intel_rcs_scissor_t *,unsigned,
    const intel_rcs_vertex_state_t *,const intel_rcs_index_binding_t *);
#endif
