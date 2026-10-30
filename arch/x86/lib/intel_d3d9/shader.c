#include "shader.h"

static uint32_t shader_word(const uint8_t *code, uint32_t index)
{
    const uint8_t *p = code + (size_t)index * 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Layout only. Profile/capability legality belongs to semantic validation. */
static int shader_layout(struct intel_d3d9_shader_instruction *i, uint8_t major)
{
    i->destination_count = 1;
    switch (i->opcode) {
    case D9_MOV: case D9_RCP: case D9_RSQ: case D9_EXP: case D9_LOG:
    case D9_LIT: case D9_FRC: case D9_ABS: case D9_NRM: case D9_MOVA:
    case D9_EXPP: case D9_LOGP: case D9_DSX: case D9_DSY:
        i->source_count = 1; break;
    case D9_ADD: case D9_SUB: case D9_MUL: case D9_DP3: case D9_DP4:
    case D9_MIN: case D9_MAX: case D9_SLT: case D9_SGE: case D9_DST:
    case D9_M4X4: case D9_M4X3: case D9_M3X4: case D9_M3X3: case D9_M3X2:
    case D9_POW: case D9_CRS: case D9_TEX: case D9_BEM: case D9_SETP:
    case D9_TEXLDL:
        i->source_count = 2; break;
    case D9_MAD: case D9_LRP: case D9_SGN: case D9_CMP: case D9_DP2ADD:
        i->source_count = 3; break;
    case D9_SINCOS:
        i->source_count = major == 2 ? 3 : 1; break;
    case D9_TEXLDD:
        i->source_count = 4; break;
    case D9_TEXKILL: case D9_DCL:
        break;
    case D9_DEF: case D9_DEFI:
        i->immediate_count = 4; break;
    case D9_DEFB:
        i->immediate_count = 1; break;
    case D9_CALL: case D9_LABEL: case D9_REP: case D9_IF: case D9_BREAKP:
        i->destination_count = 0; i->source_count = 1; break;
    case D9_CALLNZ: case D9_LOOP: case D9_IFC: case D9_BREAKC:
        i->destination_count = 0; i->source_count = 2; break;
    case D9_NOP: case D9_RET: case D9_ENDLOOP: case D9_ENDREP:
    case D9_ELSE: case D9_ENDIF: case D9_BREAK:
        i->destination_count = 0; break;
    default:
        return 0;
    }
    return 1;
}

static int shader_operand(const uint8_t *code, uint32_t *pos, uint32_t end,
                          int destination, const struct intel_d3d9_shader_info *info,
                          struct intel_d3d9_shader_operand *operand)
{
    uint32_t t, a;
    if (*pos == end) return 0;
    t = shader_word(code, *pos);
    if (!(t & 0x80000000u) || (t & 0xc000u)) return 0;
    operand->type = (uint8_t)(((t >> 28) & 7) | ((t >> 8) & 24));
    if (operand->type > D9_PREDICATE) return 0;
    operand->index = (uint16_t)(t & 0x7ff);
    operand->relative = (uint8_t)((t >> 13) & 1);
    if (destination) {
        operand->write_mask = (uint8_t)((t >> 16) & 15);
        operand->modifier = (uint8_t)((t >> 20) & 15);
        if (!operand->write_mask || (t & 0x0f800000u)) return 0;
        if (operand->relative && (info->stage != INTEL_D3D9_VERTEX || info->major < 3))
            return 0;
    } else {
        operand->swizzle = (uint8_t)(t >> 16);
        operand->modifier = (uint8_t)((t >> 24) & 15);
        if (operand->modifier > 13) return 0;
        if (operand->relative && info->stage == INTEL_D3D9_PIXEL && info->major < 3)
            return 0;
    }
    ++*pos;
    if (!operand->relative) return 1;
    if (*pos == end) return 0;
    a = shader_word(code, *pos);
    operand->address_type = (uint8_t)(((a >> 28) & 7) | ((a >> 8) & 24));
    operand->address_index = (uint16_t)(a & 0x7ff);
    operand->address_swizzle = (uint8_t)(a >> 16);
    if (!(a & 0x80000000u) || (a & 0x0f00e000u) ||
        (operand->address_type != D9_ADDRESS_TEXTURE &&
         operand->address_type != D9_LOOP_REGISTER)) return 0;
    ++*pos;
    return 1;
}

static enum intel_d3d9_shader_status shader_pass(
    const uint8_t *code, uint32_t words, struct intel_d3d9_shader_info *info,
    struct intel_d3d9_shader_instruction *output, uint32_t *bad)
{
    uint32_t pos = 1, count = 0;
    while (pos < words) {
        struct intel_d3d9_shader_instruction i = {0};
        uint32_t start = pos, t = shader_word(code, pos++), op = t & 0xffff;
        uint32_t end, n;
        *bad = start;
        if (op == 0xffff) {
            if (t != 0xffff || pos != words) return INTEL_D3D9_SHADER_INVALID;
            info->instruction_count = count;
            return INTEL_D3D9_SHADER_OK;
        }
        if (op == 0xfffe) {
            n = (t >> 16) & 0x7fff;
            if ((t & 0x80000000u) || n > words - pos) return INTEL_D3D9_SHADER_INVALID;
            pos += n;
            continue;
        }
        if (t & 0xe0000000u) return INTEL_D3D9_SHADER_INVALID;
        n = (t >> 24) & 15;
        if (n > words - pos) return INTEL_D3D9_SHADER_INVALID;
        end = pos + n;
        i.word_offset = start; i.opcode = (uint16_t)op;
        i.word_count = (uint8_t)(n + 1);
        i.control = (uint8_t)(t >> 16); i.predicated = (uint8_t)((t >> 28) & 1);
        if (!shader_layout(&i, info->major)) return INTEL_D3D9_SHADER_UNSUPPORTED;
        if (op == D9_IFC || op == D9_BREAKC || op == D9_SETP) {
            if (!i.control || i.control > 6) return INTEL_D3D9_SHADER_INVALID;
        } else if (op == D9_TEX) {
            if (i.control > 2) return INTEL_D3D9_SHADER_INVALID;
        } else if (i.control) return INTEL_D3D9_SHADER_INVALID;
        if (i.predicated && (i.immediate_count || op == D9_DCL))
            return INTEL_D3D9_SHADER_INVALID;
        if (op == D9_DCL) {
            if (pos == end) return INTEL_D3D9_SHADER_INVALID;
            i.declaration = shader_word(code, pos++);
        }
        if (i.destination_count && !shader_operand(code, &pos, end, 1, info, &i.destination))
            goto invalid_operand;
        /* Microsoft's assembler places the predicate after the destination,
         * before ordinary sources (also exercised with p0 and !p0 fixtures). */
        if (i.predicated) {
            if (!shader_operand(code, &pos, end, 0, info, &i.predicate)) goto invalid_operand;
            if (i.predicate.type != D9_PREDICATE || i.predicate.index ||
                i.predicate.relative || (i.predicate.modifier != 0 && i.predicate.modifier != 13))
                return INTEL_D3D9_SHADER_INVALID;
        }
        for (n = 0; n < i.source_count; ++n)
            if (!shader_operand(code, &pos, end, 0, info, &i.sources[n])) goto invalid_operand;
        if (i.immediate_count > end - pos) goto invalid_operand;
        for (n = 0; n < i.immediate_count; ++n) i.immediate[n] = shader_word(code, pos++);
        if (pos != end) goto invalid_operand;
        if (op == D9_DCL) {
            uint32_t allowed = i.destination.type == D9_SAMPLER ? 0xf8000000u : 0x800f000fu;
            if (!(i.declaration & 0x80000000u) || (i.declaration & ~allowed) ||
                i.destination.relative) return INTEL_D3D9_SHADER_INVALID;
            if (i.destination.type == D9_SAMPLER) {
                n = (i.declaration >> 27) & 15;
                if (n < 2 || n > 4) return INTEL_D3D9_SHADER_INVALID;
            } else if ((i.declaration & 15) > 13) return INTEL_D3D9_SHADER_INVALID;
        }
        if (i.immediate_count && (i.destination.relative || i.destination.modifier ||
            i.destination.type != (op == D9_DEF ? D9_CONST : op == D9_DEFI ? D9_INT : D9_BOOL)))
            return INTEL_D3D9_SHADER_INVALID;
        if (op == D9_DEFB && i.immediate[0] > 1) return INTEL_D3D9_SHADER_INVALID;
        if (count == INTEL_D3D9_SHADER_MAX_INSTRUCTIONS) return INTEL_D3D9_SHADER_UNSUPPORTED;
        if (output) output[count] = i;
        ++count;
        continue;
invalid_operand:
        *bad = pos;
        return INTEL_D3D9_SHADER_INVALID;
    }
    *bad = pos;
    return INTEL_D3D9_SHADER_INVALID; /* missing END */
}

enum intel_d3d9_shader_status intel_d3d9_shader_decode(
    const void *code, size_t bytes, struct intel_d3d9_shader_info *info,
    struct intel_d3d9_shader_instruction *instructions, uint32_t capacity,
    struct intel_d3d9_shader_error *error)
{
    struct intel_d3d9_shader_info result = {0};
    enum intel_d3d9_shader_status status = INTEL_D3D9_SHADER_INVALID;
    uint32_t version, bad = 0;
    size_t output_bytes;
    uintptr_t input = (uintptr_t)code, output = (uintptr_t)instructions;
    if (!code || !info || bytes < 8 || (bytes & 3) || (!instructions && capacity)) goto done;
    if (bytes > INTEL_D3D9_SHADER_MAX_WORDS * 4u) {
        status = INTEL_D3D9_SHADER_UNSUPPORTED; goto done;
    }
    if (input > UINTPTR_MAX - bytes) goto done;
    version = shader_word(code, 0);
    if ((version >> 16) != 0xfffe && (version >> 16) != 0xffff) goto done;
    result.stage = (version >> 16) == 0xffff ? INTEL_D3D9_PIXEL : INTEL_D3D9_VERTEX;
    result.major = (uint8_t)(version >> 8); result.minor = (uint8_t)version;
    if ((result.major != 2 && result.major != 3) || result.minor) {
        status = INTEL_D3D9_SHADER_UNSUPPORTED; goto done;
    }
    result.word_count = (uint32_t)(bytes / 4);
    status = shader_pass(code, result.word_count, &result, NULL, &bad);
    if (status != INTEL_D3D9_SHADER_OK) goto done;
    if (instructions) {
        if (capacity < result.instruction_count) { status = INTEL_D3D9_SHADER_CAPACITY; goto done; }
        output_bytes = (size_t)result.instruction_count * sizeof(*instructions);
        if (output > UINTPTR_MAX - output_bytes ||
            (output_bytes && input < output + output_bytes && output < input + bytes)) {
            status = INTEL_D3D9_SHADER_INVALID; goto done;
        }
        status = shader_pass(code, result.word_count, &result, instructions, &bad);
        if (status != INTEL_D3D9_SHADER_OK) goto done;
    }
    *info = result;
done:
    if (error) {
        error->status = status;
        error->word_offset = status == INTEL_D3D9_SHADER_OK ? 0 : bad;
    }
    return status;
}
