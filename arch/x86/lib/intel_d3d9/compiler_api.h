#ifndef OSITO_INTEL_D3D9_COMPILER_API_H
#define OSITO_INTEL_D3D9_COMPILER_API_H
#include <stdint.h>

#define INTEL_D3D9_COMPILER_ABI 2u
#define INTEL_D3D9_PROGRAM_LEGACY_ABI 1u
#define INTEL_D3D9_COMPILER_RESOURCE_ABI 3u
#define INTEL_D3D9_COMPILER_VERTEX_ABI 4u
#define INTEL_D3D9_PROGRAM_MAX_PARAMETERS 512u
enum intel_d3d9_compile_status {
    INTEL_D3D9_COMPILE_OK, INTEL_D3D9_COMPILE_INVALID,
    INTEL_D3D9_COMPILE_UNSUPPORTED, INTEL_D3D9_COMPILE_NOMEM,
    INTEL_D3D9_COMPILE_FAILED, INTEL_D3D9_COMPILE_BUSY
};
struct intel_d3d9_compiler;
struct intel_d3d9_compile_error { uint32_t status, word; char message[192]; };

/* Owned CPU program, not a GPU address or a submission packet. All offsets
 * except kernel/constant offsets are relative to this allocation; those two
 * are relative to code_offset. No Mesa pointers or C bitfields cross this ABI.
 * V1 describes pixel arithmetic using local DEF constants. V2 adds a compact
 * scalar parameter map (DWORD indices into D3D9's 224 float4 registers).
 * That map lies after bytecode and before machine code, relative to this
 * allocation. V3 opts into TEXCOORD inputs and 2D texture/sampler bindings;
 * its metadata must be consumed by a supporting GPU submission path. The
 * UMD selects V3 for resource shaders. Pull buffers and relocations remain
 * unsupported. V4 is an opt-in vertex compiler contract, with the extension
 * below; existing pixel submission paths do not accept it.
 * The caller treats every byte as immutable until program_free. */
struct intel_d3d9_program {
    uint32_t abi, header_bytes, allocation_bytes, pci_id;
    uint32_t bytecode_offset, bytecode_bytes, code_offset, code_bytes;
    uint32_t stage, simd_mask, grf_used, scratch_bytes;
    uint32_t kernel_offset[3], dispatch_grf_start[3];
    uint32_t curb_read_length, constant_data_offset, constant_data_bytes, flags;
    /* Gen9 may require a dummy layer input even for a constant shader.
     * layer_input is its payload slot, or UINT32_MAX when absent. */
    uint32_t varying_inputs, barycentric_modes, parameter_count, layer_input;
    uint32_t parameter_map_offset;
    union {
        uint32_t reserved[5];
        /* V3 only: D3D sN uses sampler N / BTI 1+N. Masks name live resources
         * after optimization. Each TEXCOORDn payload slot is a four-bit nibble
         * in texcoord_map; 15 means absent. Layer, when present, stays separate.
         * V1/V2 retain five zero reserved words and their existing semantics. */
        struct { uint32_t texture_mask,sampler_mask,texcoord_mask,texcoord_map,resource_reserved; };
    };
};

/* V4 vertex program: 256-byte header. SIMD8, one native entry point at zero,
 * no scratch, samplers, relocations or generated vertex/instance IDs. The
 * base's varying_inputs is the live input count; layer_input is UINT32_MAX.
 * Its parameter map indexes c0..c255. All base.reserved words remain zero.
 * input_mask names v0..v15. The VF must compact active inputs in increasing
 * register order. input_semantics encode usage | (index << 16); inactive
 * entries are UINT32_MAX. output_mask bits and output_slots indices name
 * POSITION0, COLOR0, COLOR1, TEXCOORD0..7; entry 11 is reserved/UINT32_MAX.
 * Slots are 16-byte VUE slots (including the hardware header/position slots).
 * urb_entry_size uses 64-byte units; urb_read_length uses 32-byte units.
 * Position is D3D clip space; viewport/clip configuration belongs to the
 * eventual submission path. This header alone never authorizes execution. */
struct intel_d3d9_vertex_program {
    struct intel_d3d9_program base;
    uint32_t input_mask, output_mask, urb_entry_size, urb_read_length;
    uint32_t input_semantics[16], output_slots[12];
};

#ifdef __cplusplus
extern "C" {
#endif
/* Results are published only on success. A context permits one compilation
 * at a time; distinct contexts may run concurrently. Destroy requires no
 * active call. Programs own their bytecode/native code independently of the
 * context, and must be released through the same library that created them. */
uint32_t intel_d3d9_compiler_create(uint32_t abi, uint32_t pci_id,
                                   struct intel_d3d9_compiler **out);
void intel_d3d9_compiler_destroy(struct intel_d3d9_compiler *compiler);
uint32_t intel_d3d9_compile_pixel(struct intel_d3d9_compiler *compiler,
    const void *bytecode, uint32_t bytes, const struct intel_d3d9_program **out,
    struct intel_d3d9_compile_error *error);
uint32_t intel_d3d9_compile_vertex(struct intel_d3d9_compiler *compiler,
    const void *bytecode, uint32_t bytes, const struct intel_d3d9_vertex_program **out,
    struct intel_d3d9_compile_error *error);
/* Release a vertex program through &program->base. */
void intel_d3d9_program_free(const struct intel_d3d9_program *program);
#ifdef __cplusplus
}
#endif
#endif
