#include "nir_frontend.h"
#include "compiler/nir/nir_builder.h"

struct vs_value { nir_def *value; unsigned defined; };
struct vs_values {
    struct vs_value temporary[32], input[16], constant[256], output[12];
    unsigned output_semantic[12], output_declared, inputs, temporary_count;
    struct intel_d3d9_uniform_bindings uniforms;
    struct intel_d3d9_vertex_bindings vertex;
};

static int vs_semantic(unsigned declaration)
{
    unsigned usage = declaration & 15, index = (declaration >> 16) & 15;
    if (usage == 0 && index == 0) return 0;
    if (usage == 10 && index < 2) return 1 + index;
    if (usage == 5 && index < 8) return 3 + index;
    return -1;
}

static unsigned vs_location(unsigned semantic)
{
    if (!semantic) return VARYING_SLOT_POS;
    if (semantic < 3) return VARYING_SLOT_COL0 + semantic - 1;
    return VARYING_SLOT_TEX0 + semantic - 3;
}

static nir_def *vs_source(nir_builder *b, struct vs_values *v,
    const struct intel_d3d9_shader_operand *operand, unsigned mask,
    enum intel_d3d9_shader_status *status)
{
    struct vs_value *value;
    if (operand->relative || (operand->modifier != 0 && operand->modifier != 1))
        return NULL;
    if (operand->type == D9_TEMP && operand->index < v->temporary_count)
        value = &v->temporary[operand->index];
    else if (operand->type == D9_INPUT && operand->index < 16)
        value = &v->input[operand->index];
    else if (operand->type == D9_CONST && operand->index < 256)
        value = &v->constant[operand->index];
    else return NULL;
    if (operand->type == D9_CONST && !value->defined) {
        if (v->uniforms.count > INTEL_D3D9_MAX_PUSH_DWORDS - 4) return NULL;
        value->value = nir_load_uniform(b, 4, 32, nir_imm_int(b, 0),
            .base = v->uniforms.count * 4, .range = 16);
        for (unsigned c = 0; c < 4; c++)
            v->uniforms.scalar[v->uniforms.count++] = operand->index * 4 + c;
        b->shader->num_uniforms = v->uniforms.count * 4;
        value->defined = 15;
    }
    unsigned swizzle[4];
    for (unsigned c = 0; c < 4; c++) {
        swizzle[c] = (operand->swizzle >> (2 * c)) & 3;
        if ((mask & (1u << c)) && !(value->defined & (1u << swizzle[c]))) {
            *status = INTEL_D3D9_SHADER_INVALID;
            return NULL;
        }
    }
    if (!value->value) { *status = INTEL_D3D9_SHADER_INVALID; return NULL; }
    nir_def *result = nir_swizzle(b, value->value, swizzle, 4);
    return operand->modifier ? nir_fneg(b, result) : result;
}

/* Straight-line VS2/VS3. Unsupported flow, relative addressing and vertex
 * textures fail before native compilation. This never evaluates vertices on
 * the CPU. Constants are compact push parameters, including c224..c255. */
struct nir_shader *intel_d3d9_vertex_to_nir(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error,
    struct intel_d3d9_uniform_bindings *bindings,
    struct intel_d3d9_vertex_bindings *vertex)
{
    struct intel_d3d9_shader_error local = {0};
    struct intel_d3d9_shader_info info;
    nir_shader *shader = NULL;
    void *scratch = NULL;
    bool executable = false;
    unsigned instruction_slots = 0;
    if (!options || !bindings || !vertex) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
    if (intel_d3d9_shader_decode(code, bytes, &info, NULL, 0, &local)) goto failed;
    local.status = INTEL_D3D9_SHADER_UNSUPPORTED;
    if (info.stage != INTEL_D3D9_VERTEX) goto failed;
    scratch = ralloc_context(NULL);
    if (!scratch) { local.status = INTEL_D3D9_SHADER_NOMEM; goto failed; }
    struct vs_values *v = rzalloc(scratch, struct vs_values);
    struct intel_d3d9_shader_instruction *instructions =
        ralloc_array(scratch, struct intel_d3d9_shader_instruction, info.instruction_count);
    if (!v || !instructions) { local.status = INTEL_D3D9_SHADER_NOMEM; goto failed; }
    if (intel_d3d9_shader_decode(code, bytes, &info, instructions, info.instruction_count, &local)) goto failed;
    for (unsigned n = 0; n < 16; n++) v->vertex.input_semantics[n] = UINT32_MAX;
    v->temporary_count = info.major == 2 ? 12 : 32;
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "Osito native D3D9 vertex shader");
    shader = b.shader; shader->info.internal = false;
    shader->info.next_stage = MESA_SHADER_FRAGMENT;
    b.exact = true;
    for (unsigned n = 0; n < info.instruction_count; n++) {
        const struct intel_d3d9_shader_instruction *i = &instructions[n];
        const struct intel_d3d9_shader_operand *d = &i->destination;
        unsigned index = d->index, mask = d->write_mask, read_mask = mask;
        struct vs_value *destination;
        nir_def *s[3] = {0}, *value = NULL;
        local.word_offset = i->word_offset; local.status = INTEL_D3D9_SHADER_UNSUPPORTED;
        if (i->predicated || i->control || d->relative) goto failed;
        if (i->opcode == D9_NOP) {
            if (++instruction_slots > (info.major == 2 ? 256u : 512u)) goto failed;
            continue;
        }
        if (i->opcode == D9_DCL) {
            if (executable) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            if (d->modifier || mask != 15) goto failed;
            if (d->type == D9_INPUT && index < 16) {
                unsigned semantic = i->declaration & 0x000f000fu;
                if ((semantic & 15) == 9) goto failed; /* POSITIONT bypasses VS. */
                if (v->inputs & (1u << index)) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
                for (unsigned k = 0; k < 16; k++) if (v->vertex.input_semantics[k] == semantic) {
                    local.status = INTEL_D3D9_SHADER_INVALID; goto failed;
                }
                nir_variable *input = nir_variable_create(shader, nir_var_shader_in, glsl_vec4_type(), "D3D9 vertex input");
                input->data.location = VERT_ATTRIB_GENERIC0 + index;
                v->input[index].value = nir_load_var(&b, input); v->input[index].defined = 15;
                v->inputs |= 1u << index; v->vertex.input_semantics[index] = semantic;
            } else if (info.major == 3 && d->type == D9_OUTPUT && index < 12) {
                int semantic = vs_semantic(i->declaration);
                if (semantic < 0) goto failed;
                if ((v->output_declared & (1u << index)) || (v->vertex.output_mask & (1u << semantic))) {
                    local.status = INTEL_D3D9_SHADER_INVALID; goto failed;
                }
                v->output_declared |= 1u << index; v->output_semantic[index] = semantic;
                v->vertex.output_mask |= 1u << semantic;
            } else goto failed;
            continue;
        }
        if (i->opcode == D9_DEF) {
            if (executable) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            if (index >= 256 || mask != 15) goto failed;
            if (v->constant[index].defined) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            nir_const_value constants[4] = {0};
            for (unsigned c = 0; c < 4; c++) constants[c].u32 = i->immediate[c];
            v->constant[index].value = nir_build_imm(&b, 4, 32, constants);
            v->constant[index].defined = 15;
            continue;
        }
        executable = true;
        unsigned matrix_rows = 0, matrix_columns = 0;
        switch (i->opcode) {
        case D9_MOV: case D9_ADD: case D9_SUB: case D9_MUL: case D9_MAD:
        case D9_MIN: case D9_MAX: case D9_ABS: case D9_FRC: case D9_LRP:
        case D9_SLT: case D9_SGE: break;
        case D9_DP3: read_mask = 7; break;
        case D9_DP4: read_mask = 15; break;
        case D9_M4X4: matrix_rows = 4; matrix_columns = 4; break;
        case D9_M4X3: matrix_rows = 3; matrix_columns = 4; break;
        case D9_M3X4: matrix_rows = 4; matrix_columns = 3; break;
        case D9_M3X3: matrix_rows = 3; matrix_columns = 3; break;
        case D9_M3X2: matrix_rows = 2; matrix_columns = 3; break;
        default: goto failed;
        }
        instruction_slots += matrix_rows ? matrix_rows : i->opcode == D9_LRP ? 2 : 1;
        if (instruction_slots > (info.major == 2 ? 256u : 512u)) goto failed;
        if ((d->modifier & ~1u) || (info.major == 2 && d->modifier)) goto failed;
        if (d->type == D9_TEMP && index < v->temporary_count)
            destination = &v->temporary[index];
        else {
            unsigned output = index;
            if (info.major == 2) {
                if (d->type == D9_RASTER_OUTPUT && index == 0) output = 0;
                else if (d->type == D9_ATTRIBUTE_OUTPUT && index < 2) output = 1 + index;
                else if (d->type == D9_OUTPUT && index < 8) output = 3 + index;
                else goto failed;
                v->output_declared |= 1u << output; v->output_semantic[output] = output;
                v->vertex.output_mask |= 1u << output;
            } else if (d->type != D9_OUTPUT || index >= 12) goto failed;
            if (!(v->output_declared & (1u << output))) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            destination = &v->output[output];
        }
        /* VS2/VS3 expose one input and one constant register read port per
         * instruction. Matrix instructions expand their consecutive rows. */
        for (unsigned a = 0; a < i->source_count; a++) for (unsigned c = 0; c < a; c++)
            if ((i->sources[a].type == D9_INPUT || i->sources[a].type == D9_CONST) &&
                i->sources[a].type == i->sources[c].type && i->sources[a].index != i->sources[c].index) {
                local.status = INTEL_D3D9_SHADER_INVALID; goto failed;
            }
        if (matrix_rows) {
            const struct intel_d3d9_shader_operand *row = &i->sources[1];
            if (mask != (1u << matrix_rows) - 1 || row->type != D9_CONST || row->relative ||
                row->modifier || row->swizzle != 0xe4 || row->index > 256 - matrix_rows ||
                (i->sources[0].type != D9_INPUT && i->sources[0].type != D9_TEMP) ||
                (d->type == i->sources[0].type && index == i->sources[0].index)) goto failed;
            read_mask = (1u << matrix_columns) - 1;
            s[0] = vs_source(&b, v, &i->sources[0], read_mask, &local.status);
            if (!s[0]) goto failed;
            nir_def *result[4];
            for (unsigned r = 0; r < 4; r++) {
                if (r >= matrix_rows) { result[r] = nir_imm_float(&b, 0); continue; }
                struct intel_d3d9_shader_operand operand = *row; operand.index += r;
                nir_def *right = vs_source(&b, v, &operand, read_mask, &local.status);
                if (!right) goto failed;
                result[r] = matrix_columns == 4 ? nir_fdot4(&b, s[0], right) : nir_fdot3(&b, s[0], right);
            }
            value = nir_vec(&b, result, 4);
        } else {
            for (unsigned c = 0; c < i->source_count; c++) {
                s[c] = vs_source(&b, v, &i->sources[c], read_mask, &local.status);
                if (!s[c]) goto failed;
            }
            switch (i->opcode) {
            case D9_MOV: value = s[0]; break;
            case D9_ADD: value = nir_fadd(&b, s[0], s[1]); break;
            case D9_SUB: value = nir_fsub(&b, s[0], s[1]); break;
            case D9_MUL: value = nir_fmul(&b, s[0], s[1]); break;
            case D9_MAD: value = nir_fadd(&b, nir_fmul(&b, s[0], s[1]), s[2]); break;
            case D9_MIN: value = nir_fmin(&b, s[0], s[1]); break;
            case D9_MAX: value = nir_fmax(&b, s[0], s[1]); break;
            case D9_ABS: value = nir_fabs(&b, s[0]); break;
            case D9_FRC: value = nir_ffract(&b, s[0]); break;
            case D9_LRP: value = nir_flrp(&b, s[2], s[1], s[0]); break;
            case D9_SLT: value = nir_b2f32(&b, nir_flt(&b, s[0], s[1])); break;
            case D9_SGE: value = nir_b2f32(&b, nir_fge(&b, s[0], s[1])); break;
            case D9_DP3: value = nir_fdot3(&b, s[0], s[1]); break;
            case D9_DP4: value = nir_fdot4(&b, s[0], s[1]); break;
            }
        }
        if (value->num_components == 1) value = nir_vec4(&b, value, value, value, value);
        if (d->modifier & 1) value = nir_fsat(&b, value);
        if (mask != 15) {
            nir_def *previous = destination->value ? destination->value : nir_undef(&b, 4, 32);
            nir_def *channels[4];
            for (unsigned c = 0; c < 4; c++) channels[c] = nir_channel(&b, mask & (1u << c) ? value : previous, c);
            value = nir_vec(&b, channels, 4);
        }
        destination->value = value; destination->defined |= mask;
    }
    local.status = INTEL_D3D9_SHADER_INVALID;
    if (!(v->vertex.output_mask & 1)) goto failed;
    for (unsigned n = 0; n < 12; n++) if (v->output_declared & (1u << n)) {
        if (v->output[n].defined != 15) goto failed;
        nir_variable *output = nir_variable_create(shader, nir_var_shader_out, glsl_vec4_type(), "D3D9 vertex output");
        output->data.location = vs_location(v->output_semantic[n]);
        nir_store_var(&b, output, v->output[n].value, 15);
        shader->num_outputs++;
    }
    v->vertex.input_mask = v->inputs;
    nir_shader_gather_info(shader, nir_shader_get_entrypoint(shader));
    nir_validate_shader(shader, "D3D9 native vertex frontend");
    *bindings = v->uniforms; *vertex = v->vertex;
    ralloc_steal(mem_ctx, shader); ralloc_free(scratch);
    if (error) *error = (struct intel_d3d9_shader_error){0, INTEL_D3D9_SHADER_OK};
    return shader;
failed:
    ralloc_free(shader); ralloc_free(scratch);
    if (error) *error = local;
    return NULL;
}
