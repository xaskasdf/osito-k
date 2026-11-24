#ifndef OSITO_INTEL_D3D9_NIR_FRONTEND_H
#define OSITO_INTEL_D3D9_NIR_FRONTEND_H
#include "shader.h"

struct nir_shader;
struct nir_shader_compiler_options;

/* Gen9 push payload: compact scalar slots map back to D3D9 c0..c223.
 * Overflow is explicitly unsupported until uniform-buffer pulling exists. */
#define INTEL_D3D9_MAX_PUSH_DWORDS 512u
struct intel_d3d9_uniform_bindings {
    uint32_t count;
    uint32_t scalar[INTEL_D3D9_MAX_PUSH_DWORDS];
};
/* Opt-in resource frontend. D3D sN maps to sampler N and surface BTI 1+N;
 * BTI 0 remains the color target. Only explicit 2D samplers and TEXCOORD0..7
 * are supported. This describes compilation, not executable driver support. */
struct intel_d3d9_resource_bindings { uint32_t samplers, texcoords; };
/* Vertex input slots are v0..v15, compacted in increasing register order by
 * the backend. Semantics encode D3DDECLUSAGE | (UsageIndex << 16).
 * Output bits: POSITION0, COLOR0, COLOR1, TEXCOORD0..7. */
struct intel_d3d9_vertex_bindings {
    uint32_t input_mask, input_semantics[16], output_mask;
};
struct nir_shader *intel_d3d9_vertex_to_nir(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error,
    struct intel_d3d9_uniform_bindings *bindings,
    struct intel_d3d9_vertex_bindings *vertex);
struct nir_shader *intel_d3d9_shader_to_nir_resources(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error,
    struct intel_d3d9_uniform_bindings *bindings,
    struct intel_d3d9_resource_bindings *resources);

/* Initial native compiler frontend: straight-line PS2/PS3 float arithmetic,
 * locally defined constants, component masks/swizzles and color output 0.
 * Unsupported instructions/resources/profiles fail explicitly. No interpreter,
 * runtime API, GPU submission or advertised D3D capability is implemented here.
 * The caller owns an immutable bytecode snapshot during this call. Returned
 * NIR is owned by mem_ctx; NULL leaves only error (optional) updated. */
struct nir_shader *intel_d3d9_shader_to_nir(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error);
struct nir_shader *intel_d3d9_shader_to_nir_bindings(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error,
    struct intel_d3d9_uniform_bindings *bindings);
#endif
