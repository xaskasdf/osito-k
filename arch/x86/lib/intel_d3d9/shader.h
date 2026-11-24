#ifndef OSITO_INTEL_D3D9_SHADER_H
#define OSITO_INTEL_D3D9_SHADER_H

#include <stddef.h>
#include <stdint.h>

/* Bounded compiler input, independent of SDK headers and GPU command format. */
#define INTEL_D3D9_SHADER_MAX_WORDS 16384u
#define INTEL_D3D9_SHADER_MAX_INSTRUCTIONS 4096u

enum intel_d3d9_shader_status {
    INTEL_D3D9_SHADER_OK,
    INTEL_D3D9_SHADER_INVALID,
    INTEL_D3D9_SHADER_UNSUPPORTED,
    INTEL_D3D9_SHADER_CAPACITY,
    INTEL_D3D9_SHADER_NOMEM
};

enum intel_d3d9_shader_stage { INTEL_D3D9_VERTEX, INTEL_D3D9_PIXEL };

/* Values are the public D3D9 token ABI, not Intel EU opcodes. */
enum intel_d3d9_shader_opcode {
    D9_NOP = 0, D9_MOV, D9_ADD, D9_SUB, D9_MAD, D9_MUL, D9_RCP,
    D9_RSQ, D9_DP3, D9_DP4, D9_MIN, D9_MAX, D9_SLT, D9_SGE,
    D9_EXP, D9_LOG, D9_LIT, D9_DST, D9_LRP, D9_FRC,
    D9_M4X4, D9_M4X3, D9_M3X4, D9_M3X3, D9_M3X2,
    D9_CALL, D9_CALLNZ, D9_LOOP, D9_RET, D9_ENDLOOP, D9_LABEL,
    D9_DCL, D9_POW, D9_CRS, D9_SGN, D9_ABS, D9_NRM, D9_SINCOS,
    D9_REP, D9_ENDREP, D9_IF, D9_IFC, D9_ELSE, D9_ENDIF,
    D9_BREAK, D9_BREAKC, D9_MOVA, D9_DEFB, D9_DEFI,
    D9_TEXKILL = 65, D9_TEX, D9_EXPP = 78, D9_LOGP,
    D9_DEF = 81, D9_CMP = 88, D9_BEM, D9_DP2ADD, D9_DSX,
    D9_DSY, D9_TEXLDD, D9_SETP, D9_TEXLDL, D9_BREAKP
};

enum intel_d3d9_shader_register {
    D9_TEMP, D9_INPUT, D9_CONST, D9_ADDRESS_TEXTURE, D9_RASTER_OUTPUT,
    D9_ATTRIBUTE_OUTPUT, D9_OUTPUT, D9_INT, D9_COLOR_OUTPUT,
    D9_DEPTH_OUTPUT, D9_SAMPLER, D9_CONST2, D9_CONST3, D9_CONST4,
    D9_BOOL, D9_LOOP_REGISTER, D9_HALF_TEMP, D9_MISC, D9_LABEL_REGISTER,
    D9_PREDICATE
};

struct intel_d3d9_shader_operand {
    uint16_t index, address_index;
    uint8_t type, modifier, swizzle, write_mask;
    uint8_t relative, address_type, address_swizzle, reserved;
};

struct intel_d3d9_shader_instruction {
    uint32_t word_offset;
    uint16_t opcode;
    uint8_t control, word_count;
    uint8_t destination_count, source_count, immediate_count, predicated;
    struct intel_d3d9_shader_operand destination, sources[4], predicate;
    /* DCL semantic/texture type token, otherwise zero. */
    uint32_t declaration;
    /* Bit-exact DEF/DEFI/DEFB constants; never interpreted as host floats. */
    uint32_t immediate[4];
};

struct intel_d3d9_shader_info {
    uint32_t instruction_count, word_count;
    uint8_t stage, major, minor, reserved;
};

struct intel_d3d9_shader_error {
    uint32_t word_offset;
    enum intel_d3d9_shader_status status;
};

/* Decode SM2.0/SM3.0 token syntax. This is NOT a shader semantic verifier or
 * native code generator: register limits, stage/opcode legality, control flow,
 * declarations and resource use still require compiler validation. In
 * particular, success must never authorize submitting the input to the GPU.
 *
 * Input is little endian, byte-alignment is sufficient. The caller owns an
 * immutable readable snapshot for both passes. All outputs must be disjoint
 * from each other and the input (the input/instruction overlap is checked).
 * With instructions == NULL and capacity == 0, only validate/count. Otherwise
 * capacity is an instruction count. Info/instructions are unchanged on failure;
 * error (optional) identifies the offending DWORD or the missing next DWORD.
 * No allocation, runtime/OS dependency or hidden mutable state.
 */
enum intel_d3d9_shader_status intel_d3d9_shader_decode(
    const void *code, size_t bytes, struct intel_d3d9_shader_info *info,
    struct intel_d3d9_shader_instruction *instructions, uint32_t capacity,
    struct intel_d3d9_shader_error *error);

#endif
