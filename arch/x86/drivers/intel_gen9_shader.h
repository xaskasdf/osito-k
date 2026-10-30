/* Gen9 native program admission. No shader interpretation or CPU drawing. */
#ifndef OSITOK_INTEL_GEN9_SHADER_H
#define OSITOK_INTEL_GEN9_SHADER_H
#include "intel_rcs_draw.h"
/* The caller owns exclusive access to the complete allocation for this call
 * and until GPU retirement. Metadata must match the captured header exactly.
 * Version 1 admits the compiler's current constant PS arithmetic output:
 * immediate float MOVs and one terminating BTI0 render-target message per
 * SIMD8/16 entry, with fully initialized payload registers. No branching,
 * indirect descriptors, architectural register writes or other messages.
 * This is a structural ISA contract, never a fixture/hash allowlist. Broader
 * compiler output requires corresponding bindings and verifier support. */
bool intel_gen9_shader_validate(const struct intel_d3d9_program *captured,
    const void *allocation,size_t bytes);
/* Resource ABI3 admission for a future explicitly bound draw path. This does
 * not enable ABI3 in the legacy capture/builder. Requires owned texture states
 * at BTI 1+sN, sampler N, mapped TEXCOORD/layer attributes, perspective-pixel
 * barycentrics and the exact Gen9 payload layout described by the header.
 * Both SIMD32 halves must terminate their RT writes before thread EOT.
 * No program execution or pixel rendering occurs in these validators. */
bool intel_gen9_resource_program_validate(const struct intel_d3d9_program *,size_t);
bool intel_gen9_resource_shader_validate(const struct intel_d3d9_program *,const void *,size_t);
/* ABI4 SIMD8 VS: direct float MOV/ADD/MUL/MAD/LRP, uniform/input reads and bounded
 * URB writes using only the hardware-provided vertex handles. No other send,
 * address register, branch, scratch or writable memory is admitted. */
bool intel_gen9_vertex_program_validate(const struct intel_d3d9_vertex_program *,size_t);
bool intel_gen9_vertex_shader_validate(const struct intel_d3d9_vertex_program *,const void *,size_t);
#endif
