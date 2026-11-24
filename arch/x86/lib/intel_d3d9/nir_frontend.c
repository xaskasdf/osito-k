#include "nir_frontend.h"
#include "compiler/nir/nir_builder.h"

struct d9_value { nir_def *value; uint8_t defined; };
struct d9_values {
    struct d9_value temporary[32], constant[224], color, input[10], texture[8];
    uint32_t samplers,texcoords,resources;
};

static nir_def *d9_nir_source(nir_builder *b, struct d9_values *values,
                            const struct intel_d3d9_shader_operand *operand,
                            unsigned mask, enum intel_d3d9_shader_status *status,
                            struct intel_d3d9_uniform_bindings *bindings)
{
    struct d9_value *value;
    unsigned swizzle[4];
    if (operand->relative) return NULL;
    if (operand->type == D9_TEMP && operand->index < 32)
        value = &values->temporary[operand->index];
    else if (operand->type == D9_CONST && operand->index < 224)
        value = &values->constant[operand->index];
    else if (values->resources && operand->type == D9_INPUT && operand->index < 10)
        value = &values->input[operand->index];
    else if (values->resources && operand->type == D9_ADDRESS_TEXTURE && operand->index < 8)
        value = &values->texture[operand->index];
    else return NULL;
    if (operand->modifier != 0 && operand->modifier != 1 &&
        operand->modifier != 11 && operand->modifier != 12) return NULL;
    if (operand->type == D9_CONST && !value->defined && bindings) {
        if (bindings->count > INTEL_D3D9_MAX_PUSH_DWORDS - 4) return NULL;
        value->value = nir_load_uniform(b, 4, 32, nir_imm_int(b, 0),
                                       .base = bindings->count * 4, .range = 16);
        for (unsigned c = 0; c < 4; c++)
            bindings->scalar[bindings->count++] = operand->index * 4 + c;
        b->shader->num_uniforms = bindings->count * 4;
        value->defined = 15;
    }
    for (unsigned c = 0; c < 4; ++c) {
        swizzle[c] = (operand->swizzle >> (c * 2)) & 3;
        if ((mask & (1u << c)) && !(value->defined & (1u << swizzle[c]))) {
            /* The legacy entry point deliberately permits no external data. */
            if (operand->type != D9_CONST) *status = INTEL_D3D9_SHADER_INVALID;
            return NULL;
        }
    }
    if (!value->value) return NULL;
    nir_def *result = nir_swizzle(b, value->value, swizzle, 4);
    if (operand->modifier == 11 || operand->modifier == 12) result = nir_fabs(b, result);
    if (operand->modifier == 1 || operand->modifier == 12) result = nir_fneg(b, result);
    return result;
}

struct nir_shader *intel_d3d9_shader_to_nir_resources(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error,
    struct intel_d3d9_uniform_bindings *bindings,
    struct intel_d3d9_resource_bindings *resources)
{
    struct intel_d3d9_shader_error local = {0};
    struct intel_d3d9_shader_info info;
    struct intel_d3d9_shader_instruction *instructions;
    nir_shader *shader = NULL;
    void *scratch = NULL;
    nir_builder b;
    struct d9_values values = {.resources=resources!=NULL};
    struct intel_d3d9_uniform_bindings local_bindings = {0};
    bool executable_seen = false;
    if (!options) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
    if (intel_d3d9_shader_decode(code, bytes, &info, NULL, 0, &local)) goto failed;
    local.status = INTEL_D3D9_SHADER_UNSUPPORTED;
    if (info.stage != INTEL_D3D9_PIXEL) goto failed;
    scratch = ralloc_context(NULL);
    if (!scratch) { local.status = INTEL_D3D9_SHADER_NOMEM; goto failed; }
    instructions = ralloc_array(scratch, struct intel_d3d9_shader_instruction, info.instruction_count);
    if (!instructions) { local.status = INTEL_D3D9_SHADER_NOMEM; goto failed; }
    if (intel_d3d9_shader_decode(code, bytes, &info, instructions, info.instruction_count, &local)) goto failed;
    b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, options, "Osito native D3D9 pixel shader");
    shader = b.shader;
    shader->info.internal = false;
    shader->info.next_stage = MESA_SHADER_NONE;
    b.exact = true;
    for (unsigned n = 0; n < info.instruction_count; ++n) {
        const struct intel_d3d9_shader_instruction *i = &instructions[n];
        struct d9_value *destination;
        nir_def *sources[3] = {0}, *value = NULL;
        unsigned mask = i->destination.write_mask, read_mask = mask;
        local.word_offset = i->word_offset;
        local.status = INTEL_D3D9_SHADER_UNSUPPORTED;
        if (i->predicated || i->control || i->destination.relative) goto failed;
        if (i->opcode == D9_NOP) continue;
        if (i->opcode == D9_DCL && resources) {
            if (executable_seen) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            if (i->destination.modifier) goto failed; /* centroid/partial precision */
            unsigned index=i->destination.index,semantic;
            if (i->destination.type == D9_SAMPLER) {
                if (index>=16 || mask!=15 || i->declaration!=0x90000000u) goto failed;
                if (values.samplers&(1u<<index)) { local.status=INTEL_D3D9_SHADER_INVALID; goto failed; }
                values.samplers|=1u<<index;
                continue;
            }
            if (info.major==2 && i->destination.type==D9_ADDRESS_TEXTURE && index<8 &&
                i->declaration==0x80000000u) {
                destination=&values.texture[index]; semantic=index;
            } else if (info.major==3 && i->destination.type==D9_INPUT && index<10 &&
                (i->declaration&0xffff)==5 && ((i->declaration>>16)&15)<8) {
                destination=&values.input[index]; semantic=(i->declaration>>16)&15;
            } else goto failed;
            if (destination->defined || (values.texcoords&(1u<<semantic))) {
                local.status=INTEL_D3D9_SHADER_INVALID; goto failed;
            }
            nir_variable *input=nir_variable_create(shader,nir_var_shader_in,glsl_vec4_type(),"D3D9 texcoord");
            input->data.location=VARYING_SLOT_TEX0+semantic;
            input->data.interpolation=INTERP_MODE_SMOOTH;
            destination->value=nir_load_var(&b,input); destination->defined=mask;
            values.texcoords|=1u<<semantic;
            continue;
        }
        if (i->opcode == D9_DEF) {
            nir_const_value constants[4] = {0};
            if (executable_seen) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            if (i->destination.index >= 224 || mask != 15) goto failed;
            destination = &values.constant[i->destination.index];
            if (destination->defined) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
            for (unsigned c = 0; c < 4; ++c) constants[c].u32 = i->immediate[c];
            destination->value = nir_build_imm(&b, 4, 32, constants);
            destination->defined = 15;
            continue;
        }
        executable_seen = true;
        switch (i->opcode) {
        case D9_MOV: case D9_ADD: case D9_SUB: case D9_MUL: case D9_MAD:
        case D9_MIN: case D9_MAX: case D9_ABS: case D9_FRC: case D9_LRP:
            break;
        case D9_DP3: read_mask = 7; break;
        case D9_DP4: read_mask = 15; break;
        case D9_TEX:
            if (!resources) goto failed;
            read_mask=3; break;
        default: goto failed;
        }
        if (i->destination.modifier & ~1u) goto failed;
        if (i->destination.type == D9_TEMP && i->destination.index < 32)
            destination = &values.temporary[i->destination.index];
        else if (i->destination.type == D9_COLOR_OUTPUT && i->destination.index == 0)
            destination = &values.color;
        else goto failed;
        if (i->opcode==D9_TEX) {
            const struct intel_d3d9_shader_operand *coordinate=&i->sources[0],*sampler=&i->sources[1];
            if (sampler->type!=D9_SAMPLER || sampler->index>=16 || sampler->relative ||
                sampler->modifier || sampler->swizzle!=0xe4 || coordinate->modifier ||
                i->destination.type!=D9_TEMP ||
                (info.major==2 && (mask!=15 || coordinate->swizzle!=0xe4 ||
                  (coordinate->type!=D9_TEMP && coordinate->type!=D9_ADDRESS_TEXTURE)))) goto failed;
            if (!(values.samplers&(1u<<sampler->index))) {
                local.status=INTEL_D3D9_SHADER_INVALID; goto failed;
            }
        }
        for (unsigned s = 0; s < (i->opcode==D9_TEX ? 1u : i->source_count); ++s) {
            sources[s] = d9_nir_source(&b, &values, &i->sources[s], read_mask,
                                      &local.status, bindings ? &local_bindings : NULL);
            if (!sources[s]) goto failed;
        }
        switch (i->opcode) {
        case D9_TEX: {
            nir_tex_instr *tex=nir_tex_instr_create(shader,1);
            tex->op=nir_texop_tex; tex->sampler_dim=GLSL_SAMPLER_DIM_2D;
            tex->dest_type=nir_type_float32; tex->coord_components=2;
            tex->texture_index=1+i->sources[1].index; tex->sampler_index=i->sources[1].index;
            tex->src[0].src_type=nir_tex_src_coord;
            tex->src[0].src=nir_src_for_ssa(nir_channels(&b,sources[0],3));
            nir_def_init(&tex->instr,&tex->def,4,32);
            nir_builder_instr_insert(&b,&tex->instr); value=&tex->def;
            break;
        }
        case D9_MOV: value = sources[0]; break;
        case D9_ADD: value = nir_fadd(&b, sources[0], sources[1]); break;
        case D9_SUB: value = nir_fsub(&b, sources[0], sources[1]); break;
        case D9_MUL: value = nir_fmul(&b, sources[0], sources[1]); break;
        case D9_MAD: value = nir_fadd(&b, nir_fmul(&b, sources[0], sources[1]), sources[2]); break;
        case D9_MIN: value = nir_fmin(&b, sources[0], sources[1]); break;
        case D9_MAX: value = nir_fmax(&b, sources[0], sources[1]); break;
        case D9_ABS: value = nir_fabs(&b, sources[0]); break;
        case D9_FRC: value = nir_ffract(&b, sources[0]); break;
        case D9_LRP: value = nir_flrp(&b, sources[2], sources[1], sources[0]); break;
        case D9_DP3: value = nir_fdot3(&b, sources[0], sources[1]); break;
        case D9_DP4: value = nir_fdot4(&b, sources[0], sources[1]); break;
        }
        if (value->num_components == 1) value = nir_vec4(&b, value, value, value, value);
        if (i->destination.modifier & 1) value = nir_fsat(&b, value);
        if (mask != 15) {
            nir_def *previous = destination->value ? destination->value : nir_undef(&b, 4, 32);
            nir_def *channels[4];
            for (unsigned c = 0; c < 4; ++c)
                channels[c] = nir_channel(&b, mask & (1u << c) ? value : previous, c);
            value = nir_vec(&b, channels, 4);
        }
        destination->value = value;
        destination->defined |= mask;
    }
    if (values.color.defined != 15) { local.status = INTEL_D3D9_SHADER_INVALID; goto failed; }
    nir_variable *color = nir_variable_create(shader, nir_var_shader_out,
                                             glsl_vec4_type(), "D3D9 color 0");
    color->data.location = FRAG_RESULT_DATA0;
    nir_store_var(&b, color, values.color.value, 15);
    shader->num_outputs = 1;
    nir_shader_gather_info(shader, nir_shader_get_entrypoint(shader));
    nir_validate_shader(shader, "D3D9 native frontend");
    ralloc_steal(mem_ctx, shader);
    ralloc_free(scratch);
    if (bindings) *bindings = local_bindings;
    if (resources) *resources=(struct intel_d3d9_resource_bindings){values.samplers,values.texcoords};
    if (error) *error = (struct intel_d3d9_shader_error){0, INTEL_D3D9_SHADER_OK};
    return shader;
failed:
    ralloc_free(shader);
    ralloc_free(scratch);
    if (error) *error = local;
    return NULL;
}

struct nir_shader *intel_d3d9_shader_to_nir_bindings(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error,
    struct intel_d3d9_uniform_bindings *bindings)
{
    return intel_d3d9_shader_to_nir_resources(code,bytes,options,mem_ctx,error,bindings,NULL);
}

struct nir_shader *intel_d3d9_shader_to_nir(
    const void *code, size_t bytes, const struct nir_shader_compiler_options *options,
    void *mem_ctx, struct intel_d3d9_shader_error *error)
{
    return intel_d3d9_shader_to_nir_bindings(code, bytes, options, mem_ctx, error, NULL);
}
