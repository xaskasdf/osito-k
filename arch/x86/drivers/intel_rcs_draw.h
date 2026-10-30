/* Native Gen9 textured rectangle pipeline. Kernel-owned input only. */
#ifndef OSITOK_INTEL_RCS_DRAW_H
#define OSITOK_INTEL_RCS_DRAW_H
#include "intel_rcs.h"
#include "../lib/intel_d3d9/compiler_api.h"
#include "intel_gpu_umd_abi.h"
#define INTEL_RCS_DRAW_BYTES 8192u
typedef struct {
    uint64_t batch_gpu,source_gpu,destination_gpu,source_bytes,destination_bytes;
    uint32_t source_width,source_height,source_pitch;
    uint32_t destination_width,destination_height,destination_pitch;
    uint32_t source_x,source_y,destination_x,destination_y,width,height;
} intel_rcs_draw_t;
/* Builds commands, immutable state, vertices and a native SIMD16 sampling
 * shader in a separate owned 8 KiB PPGTT allocation. BGRA8 linear, nearest,
 * no scaling/blending/depth. Entire distinct source/destination ranges must
 * be pinned/mapped. Exclusive RCS, UC MOCS slots 0/1 and coherent CPU writes
 * are caller prerequisites. No raw application GPU addresses or commands. */
bool intel_rcs_draw_build(uint32_t *batch,size_t bytes,const intel_rcs_draw_t *);
/* Native program and render target are independently owned/pinned PPGTT
 * allocations. The caller uploads code_bytes from program+code_offset at
 * program_gpu and keeps it immutable until GPU completion. This interface
 * validates metadata, not arbitrary EU instructions; only compiler-validated,
 * kernel-owned programs may reach this internal builder. No user pointers. */
#define INTEL_RCS_PIXEL_MAX_CODE 65536u
typedef struct {
    uint32_t count;
    uint32_t values[INTEL_D3D9_PROGRAM_MAX_PARAMETERS];
} intel_rcs_constants_t;
typedef struct {
    uint64_t batch_gpu,program_gpu,destination_gpu,destination_bytes;
    uint32_t destination_width,destination_height,destination_pitch;
    uint32_t x,y,width,height,dispatch_mask;
    intel_rcs_constants_t constants; /* Owned per-draw snapshot, binary32 bits. */
} intel_rcs_pixel_draw_t;
bool intel_rcs_pixel_validate(const struct intel_d3d9_program *,size_t bytes);
bool intel_rcs_pixel_build(uint32_t *batch,size_t bytes,const intel_rcs_pixel_draw_t *,
    const struct intel_d3d9_program *,size_t program_bytes);
/* Screen-space XYZW binary32 vertices, one stream, no vertex shader/depth.
 * The GPU fetches the supplied vertices without CPU transforms.
 * pixel.x/y/width/height is the scissor. D3D9 integer pixel centers and the
 * top-left coverage rule apply. All four allocations are distinct, pinned and
 * owned; vertices must remain immutable until confirmed GPU completion. */
enum intel_rcs_topology { INTEL_RCS_TRIANGLE_LIST=4,INTEL_RCS_TRIANGLE_STRIP=5,INTEL_RCS_TRIANGLE_FAN=6 };
typedef struct {
    intel_rcs_pixel_draw_t pixel;
    uint64_t vertices_gpu,vertices_bytes;
    uint32_t vertex_offset,vertex_stride,vertex_count,topology;
    intel_umd_blend_t blend; /* Zero preserves the legacy opaque draw. */
} intel_rcs_primitive_draw_t;
bool intel_rcs_primitive_build(uint32_t *batch,size_t bytes,const intel_rcs_primitive_draw_t *,
    const struct intel_d3d9_program *,size_t program_bytes);
/* Repeat identical geometry with distinct scissors in one hardware batch.
 * All scissors are validated before the output is touched. Zero count uses
 * the primitive's original scissor and preserves the single-draw contract. */
#define INTEL_RCS_MAX_SCISSORS 32u
typedef struct { uint32_t x,y,width,height; } intel_rcs_scissor_t;
bool intel_rcs_primitives_build(uint32_t *,size_t,const intel_rcs_primitive_draw_t *,
    const struct intel_d3d9_program *,size_t,const intel_rcs_scissor_t *,unsigned);
#endif
