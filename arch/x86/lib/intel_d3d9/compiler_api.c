#include "compiler_api.h"
#include "nir_frontend.h"
#include "compiler/brw_compiler.h"
#include "compiler/brw_nir.h"
#include "compiler/brw_eu.h"
#include "compiler/brw_disasm.h"
#include "dev/intel_debug.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct intel_d3d9_program) == 128, "compiler program ABI");
_Static_assert(sizeof(struct intel_d3d9_vertex_program) == 256, "vertex compiler ABI");
struct intel_d3d9_compiler {
    struct intel_device_info device;
    struct brw_compiler *native;
    void *memory;
    uint32_t pci_id, busy, abi;
};

static void compiler_message(void *context, unsigned *id, const char *format, ...)
{
    struct intel_d3d9_compile_error *error = context;
    (void)id;
    if (!error) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

uint32_t intel_d3d9_compiler_create(uint32_t abi, uint32_t pci_id,
                                   struct intel_d3d9_compiler **out)
{
    if (!out) return INTEL_D3D9_COMPILE_INVALID;
    if ((abi != INTEL_D3D9_COMPILER_ABI && abi != INTEL_D3D9_PROGRAM_LEGACY_ABI &&
         abi != INTEL_D3D9_COMPILER_RESOURCE_ABI &&
         abi != INTEL_D3D9_COMPILER_VERTEX_ABI) || pci_id != 0x3e9b)
        return INTEL_D3D9_COMPILE_UNSUPPORTED;
    struct intel_d3d9_compiler *c = calloc(1, sizeof(*c));
    if (!c) return INTEL_D3D9_COMPILE_NOMEM;
    if (!intel_get_device_info_from_pci_id(pci_id, &c->device) || c->device.ver != 9) {
        free(c); return INTEL_D3D9_COMPILE_UNSUPPORTED;
    }
    c->memory = ralloc_context(NULL);
    if (!c->memory) { free(c); return INTEL_D3D9_COMPILE_NOMEM; }
    glsl_type_singleton_init_or_ref();
    process_intel_debug_variable();
    c->native = brw_compiler_create(c->memory, &c->device);
    if (!c->native) {
        ralloc_free(c->memory); glsl_type_singleton_decref(); free(c);
        return INTEL_D3D9_COMPILE_NOMEM;
    }
    c->native->shader_debug_log = compiler_message;
    c->native->shader_perf_log = compiler_message;
    c->pci_id = pci_id; c->abi = abi;
    *out = c;
    return INTEL_D3D9_COMPILE_OK;
}

void intel_d3d9_compiler_destroy(struct intel_d3d9_compiler *c)
{
    if (!c) return;
    ralloc_free(c->memory);
    glsl_type_singleton_decref();
    free(c);
}

uint32_t intel_d3d9_compile_pixel(struct intel_d3d9_compiler *c,
    const void *bytecode, uint32_t bytes, const struct intel_d3d9_program **out,
    struct intel_d3d9_compile_error *error)
{
    struct intel_d3d9_compile_error local = {0};
    struct intel_d3d9_shader_error frontend = {0};
    struct brw_wm_prog_key key = {0};
    struct brw_wm_prog_data data = {0};
    struct brw_nir_compiler_opts options = {0};
    struct brw_compile_fs_params params = {0};
    struct intel_d3d9_uniform_bindings bindings = {0};
    struct intel_d3d9_resource_bindings resources = {0};
    uint32_t status = INTEL_D3D9_COMPILE_INVALID;
    if (!c || !out || !bytecode || !bytes || (bytes & 3) ||
        bytes > INTEL_D3D9_SHADER_MAX_WORDS * 4) goto done;
    status = INTEL_D3D9_COMPILE_UNSUPPORTED;
    if (c->abi == INTEL_D3D9_COMPILER_VERTEX_ABI) goto done;
    status = INTEL_D3D9_COMPILE_BUSY;
    if (__atomic_exchange_n(&c->busy, 1, __ATOMIC_ACQUIRE)) goto done;
    void *snapshot = malloc(bytes), *memory = ralloc_context(NULL);
    status = INTEL_D3D9_COMPILE_NOMEM;
    if (!snapshot || !memory) goto release;
    memcpy(snapshot, bytecode, bytes);
    bool resource_abi=c->abi==INTEL_D3D9_COMPILER_RESOURCE_ABI;
    struct nir_shader *nir = intel_d3d9_shader_to_nir_resources(snapshot, bytes,
        c->native->nir_options[MESA_SHADER_FRAGMENT], memory, &frontend,
        c->abi != INTEL_D3D9_PROGRAM_LEGACY_ABI ? &bindings : NULL,
        resource_abi ? &resources : NULL);
    if (!nir) {
        status = frontend.status == INTEL_D3D9_SHADER_UNSUPPORTED ? INTEL_D3D9_COMPILE_UNSUPPORTED :
                 frontend.status == INTEL_D3D9_SHADER_NOMEM ? INTEL_D3D9_COMPILE_NOMEM : INTEL_D3D9_COMPILE_INVALID;
        local.word = frontend.word_offset;
        goto release;
    }
    data.base.nr_params = bindings.count;
    if (bindings.count) {
        data.base.param = ralloc_array(memory, uint32_t, bindings.count);
        if (!data.base.param) { status = INTEL_D3D9_COMPILE_NOMEM; goto release; }
        memcpy(data.base.param, bindings.scalar, bindings.count * sizeof(uint32_t));
    }
    brw_preprocess_nir(c->native, nir, &options);
    /* The frontend creates declarations before it knows which inputs survive.
     * Recompute live I/O after DCE; the general gather pass deliberately does
     * not infer texture/sampler usage from direct-index texture instructions. */
    nir_opt_dce(nir);
    nir_remove_dead_variables(nir,nir_var_shader_in,NULL);
    nir_shader_gather_info(nir,nir_shader_get_entrypoint(nir));
    uint32_t live_samplers=0;
    nir_foreach_function_impl(impl,nir) {
        nir_foreach_block(block,impl) {
            nir_foreach_instr(instr,block) if (instr->type==nir_instr_type_tex) {
                nir_tex_instr *tex=nir_instr_as_tex(instr);
                status=INTEL_D3D9_COMPILE_UNSUPPORTED;
                if (!resource_abi || tex->sampler_index>=16 || tex->texture_index!=1+tex->sampler_index ||
                    !(resources.samplers&(1u<<tex->sampler_index))) goto release;
                live_samplers|=1u<<tex->sampler_index;
            }
        }
    }
    key.nr_color_regions = 1;
    params.base.mem_ctx = memory; params.base.nir = nir; params.base.log_data = &local;
    params.key = &key; params.prog_data = &data; params.max_polygons = 1;
    const unsigned *native = brw_compile_fs(c->native, &params);
    status = INTEL_D3D9_COMPILE_FAILED;
    if (!native) {
        if (params.base.error_str) snprintf(local.message, sizeof(local.message), "%s", params.base.error_str);
        goto release;
    }
    uint32_t mask = data.dispatch_8 | (data.dispatch_16 << 1) | (data.dispatch_32 << 2);
    if (!mask || !data.base.program_size || data.base.program_size > 16u * 1024 * 1024)
        goto release;
    /* Never publish a program whose binding/relocation requirements
     * have been dropped by the driver-facing metadata conversion. */
    status = INTEL_D3D9_COMPILE_UNSUPPORTED;
    if (data.base.nr_params > INTEL_D3D9_PROGRAM_MAX_PARAMETERS ||
        data.base.curb_read_length != (data.base.nr_params + 7) / 8 ||
        data.base.num_relocs || data.base.has_ubo_pull ||
        data.num_varying_inputs > (resource_abi ? 9u : 1u) || data.num_per_primitive_inputs ||
        (data.inputs & ~(resource_abi ? (uint64_t)0xff<<VARYING_SLOT_TEX0 : 0)) ||
        (data.barycentric_interp_modes & ~(resource_abi ? 1u<<INTEL_BARYCENTRIC_PERSPECTIVE_PIXEL : 0)) ||
        data.uses_kill || data.computed_depth_mode ||
        data.computed_stencil || data.has_side_effects) {
        snprintf(local.message, sizeof(local.message),
            "program bindings: params=%u reloc=%u ubo=%u varying=%u primitive=%u inputs=%llu bary=%u kill=%u depth=%u stencil=%u side=%u",
            data.base.nr_params, data.base.num_relocs, data.base.has_ubo_pull,
            data.num_varying_inputs, data.num_per_primitive_inputs, (unsigned long long)data.inputs,
            data.barycentric_interp_modes, data.uses_kill, data.computed_depth_mode,
            data.computed_stencil, data.has_side_effects);
        goto release;
    }
    uint32_t texcoords=0,texcoord_map=UINT32_MAX,texture_mask=0,sampler_mask=0;
    uint32_t layer=data.urb_setup[VARYING_SLOT_LAYER]>=0 ? (uint32_t)data.urb_setup[VARYING_SLOT_LAYER] : UINT32_MAX;
    unsigned slots=0,slot_mask=0;
    for (unsigned i=0;i<VARYING_SLOT_MAX;i++) if (data.urb_setup[i]>=0) {
        unsigned slot=data.urb_setup[i];
        if (slot>=data.num_varying_inputs || data.urb_setup_channel[i] || (slot_mask&(1u<<slot))) goto release;
        slot_mask|=1u<<slot; slots++;
        if (i==VARYING_SLOT_LAYER) continue;
        if (!resource_abi || i<VARYING_SLOT_TEX0 || i>VARYING_SLOT_TEX7) goto release;
        unsigned semantic=i-VARYING_SLOT_TEX0;
        if (!(resources.texcoords&(1u<<semantic))) goto release;
        texcoords|=1u<<semantic;
        texcoord_map=(texcoord_map&~(15u<<(semantic*4)))|(slot<<(semantic*4));
    }
    if (slots!=data.num_varying_inputs || (!resource_abi && data.num_varying_inputs && layer!=0)) goto release;
    texture_mask=sampler_mask=live_samplers;
    status = INTEL_D3D9_COMPILE_FAILED;
    uint32_t offsets[3] = {0, data.prog_offset_16, data.prog_offset_32};
    for (unsigned i = 0; i < 3; ++i) if (mask & (1u << i)) {
        if (offsets[i] >= data.base.program_size) goto release;
        unsigned end = brw_disassemble_find_end(&c->native->isa, native, offsets[i]);
        if (end <= offsets[i] || end > data.base.program_size ||
            !brw_validate_instructions(&c->native->isa, native, offsets[i], end, NULL)) goto release;
    }
    for (unsigned i = 0; i < data.base.nr_params; i++)
        if (!data.base.param || data.base.param[i] >= 224u * 4) goto release;
    uint32_t map_bytes = data.base.nr_params * sizeof(uint32_t);
    uint32_t code_offset = (sizeof(struct intel_d3d9_program) + bytes + map_bytes + 63) & ~63u;
    uint32_t total = code_offset + data.base.program_size;
    struct intel_d3d9_program *program = calloc(1, total);
    status = INTEL_D3D9_COMPILE_NOMEM;
    if (!program) goto release;
    program->abi = c->abi; program->header_bytes = sizeof(*program);
    program->allocation_bytes = total; program->pci_id = c->pci_id;
    program->bytecode_offset = sizeof(*program); program->bytecode_bytes = bytes;
    program->code_offset = code_offset; program->code_bytes = data.base.program_size;
    program->stage = INTEL_D3D9_PIXEL; program->simd_mask = mask;
    program->grf_used = data.base.grf_used; program->scratch_bytes = data.base.total_scratch;
    memcpy(program->kernel_offset, offsets, sizeof(offsets));
    program->dispatch_grf_start[0] = data.base.dispatch_grf_start_reg;
    program->dispatch_grf_start[1] = data.dispatch_grf_start_reg_16;
    program->dispatch_grf_start[2] = data.dispatch_grf_start_reg_32;
    program->curb_read_length = data.base.curb_read_length;
    program->constant_data_offset = data.base.const_data_offset;
    program->constant_data_bytes = data.base.const_data_size;
    program->flags = data.base.use_alt_mode ? 1u : 0u;
    program->varying_inputs = data.num_varying_inputs;
    program->layer_input = layer;
    program->barycentric_modes = data.barycentric_interp_modes;
    if (resource_abi) {
        program->texture_mask=texture_mask; program->sampler_mask=sampler_mask;
        program->texcoord_mask=texcoords; program->texcoord_map=texcoord_map;
    }
    program->parameter_count = data.base.nr_params;
    if (map_bytes) {
        program->parameter_map_offset = sizeof(*program) + bytes;
        memcpy((unsigned char *)program + program->parameter_map_offset,
               data.base.param, map_bytes);
    }
    memcpy((unsigned char *)program + program->bytecode_offset, snapshot, bytes);
    memcpy((unsigned char *)program + code_offset, native, data.base.program_size);
    *out = program;
    status = INTEL_D3D9_COMPILE_OK;
release:
    if (memory) ralloc_free(memory);
    free(snapshot);
    __atomic_store_n(&c->busy, 0, __ATOMIC_RELEASE);
done:
    local.status = status;
    if (error) *error = local;
    return status;
}

uint32_t intel_d3d9_compile_vertex(struct intel_d3d9_compiler *c,
    const void *bytecode, uint32_t bytes, const struct intel_d3d9_vertex_program **out,
    struct intel_d3d9_compile_error *error)
{
    struct intel_d3d9_compile_error local = {0};
    struct intel_d3d9_shader_error frontend = {0};
    struct brw_vs_prog_key key = {0};
    struct brw_vs_prog_data data = {0};
    struct brw_nir_compiler_opts options = {0};
    struct brw_compile_vs_params params = {0};
    struct intel_d3d9_uniform_bindings bindings = {0};
    struct intel_d3d9_vertex_bindings vertex = {0};
    uint32_t status = INTEL_D3D9_COMPILE_INVALID;
    if (!c || !out || !bytecode || !bytes || (bytes & 3) ||
        bytes > INTEL_D3D9_SHADER_MAX_WORDS * 4) goto done;
    status = INTEL_D3D9_COMPILE_UNSUPPORTED;
    if (c->abi != INTEL_D3D9_COMPILER_VERTEX_ABI) goto done;
    status = INTEL_D3D9_COMPILE_BUSY;
    if (__atomic_exchange_n(&c->busy, 1, __ATOMIC_ACQUIRE)) goto done;
    void *snapshot = malloc(bytes), *memory = ralloc_context(NULL);
    status = INTEL_D3D9_COMPILE_NOMEM;
    if (!snapshot || !memory) goto release;
    memcpy(snapshot, bytecode, bytes);
    struct nir_shader *nir = intel_d3d9_vertex_to_nir(snapshot, bytes,
        c->native->nir_options[MESA_SHADER_VERTEX], memory, &frontend, &bindings, &vertex);
    if (!nir) {
        status = frontend.status == INTEL_D3D9_SHADER_UNSUPPORTED ? INTEL_D3D9_COMPILE_UNSUPPORTED :
                 frontend.status == INTEL_D3D9_SHADER_NOMEM ? INTEL_D3D9_COMPILE_NOMEM : INTEL_D3D9_COMPILE_INVALID;
        local.word = frontend.word_offset;
        goto release;
    }
    data.base.base.nr_params = bindings.count;
    if (bindings.count) {
        data.base.base.param = ralloc_array(memory, uint32_t, bindings.count);
        if (!data.base.base.param) { status = INTEL_D3D9_COMPILE_NOMEM; goto release; }
        memcpy(data.base.base.param, bindings.scalar, bindings.count * 4);
    }
    brw_preprocess_nir(c->native, nir, &options);
    nir_opt_dce(nir);
    nir_remove_dead_variables(nir, nir_var_shader_in, NULL);
    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
    uint64_t live_outputs = nir->info.outputs_written;
    brw_compute_vue_map(&c->device, &data.base.vue_map, live_outputs, false, 1);
    params.base.mem_ctx = memory; params.base.nir = nir; params.base.log_data = &local;
    params.key = &key; params.prog_data = &data;
    const unsigned *native = brw_compile_vs(c->native, &params);
    status = INTEL_D3D9_COMPILE_FAILED;
    if (!native) {
        if (params.base.error_str) snprintf(local.message, sizeof(local.message), "%s", params.base.error_str);
        goto release;
    }
    struct brw_stage_prog_data *base = &data.base.base;
    status = INTEL_D3D9_COMPILE_UNSUPPORTED;
    if (!base->program_size || base->program_size > 16u * 1024 * 1024 ||
        base->nr_params > INTEL_D3D9_PROGRAM_MAX_PARAMETERS ||
        base->curb_read_length != (base->nr_params + 7) / 8 ||
        base->num_relocs || base->has_ubo_pull || base->total_scratch ||
        base->const_data_size || base->const_data_offset ||
        !base->grf_used || base->grf_used > 128 || base->dispatch_grf_start_reg >= 128 ||
        data.double_inputs_read || (data.inputs_read & ~((uint64_t)0xffff << VERT_ATTRIB_GENERIC0)) ||
        data.nr_attribute_slots > 16 || data.base.include_vue_handles ||
        data.base.clip_distance_mask || data.base.cull_distance_mask ||
        data.base.dispatch_mode != INTEL_DISPATCH_MODE_SIMD8 ||
        data.base.urb_read_length != (data.nr_attribute_slots + 1) / 2 ||
        !data.base.urb_entry_size || data.base.urb_entry_size > 4 ||
        data.uses_vertexid || data.uses_instanceid || data.uses_is_indexed_draw ||
        data.uses_firstvertex || data.uses_baseinstance || data.uses_drawid) {
        snprintf(local.message, sizeof(local.message), "unsupported native vertex payload or bindings");
        goto release;
    }
    uint32_t inputs = data.inputs_read >> VERT_ATTRIB_GENERIC0;
    if (inputs & ~vertex.input_mask) goto release;
    for (unsigned n = 0; n < base->nr_params; n++)
        if (!base->param || base->param[n] >= 256u * 4) goto release;
    uint32_t slots[12], outputs = 0;
    for (unsigned n = 0; n < 12; n++) {
        slots[n] = UINT32_MAX;
        if (n == 11) continue;
        unsigned varying = n == 0 ? VARYING_SLOT_POS : n < 3 ? VARYING_SLOT_COL0 + n - 1 : VARYING_SLOT_TEX0 + n - 3;
        if (!(live_outputs & ((uint64_t)1 << varying))) continue;
        int slot = data.base.vue_map.varying_to_slot[varying];
        if (slot < 0 || slot >= data.base.vue_map.num_slots || slot >= 16) goto release;
        slots[n] = slot; outputs |= 1u << n;
    }
    if (outputs != vertex.output_mask || !(outputs & 1)) goto release;
    status = INTEL_D3D9_COMPILE_FAILED;
    unsigned end = brw_disassemble_find_end(&c->native->isa, native, 0);
    if (!end || end > base->program_size ||
        !brw_validate_instructions(&c->native->isa, native, 0, end, NULL)) goto release;
    uint32_t map_bytes = base->nr_params * 4;
    uint32_t code_offset = (sizeof(struct intel_d3d9_vertex_program) + bytes + map_bytes + 63) & ~63u;
    struct intel_d3d9_vertex_program *p = calloc(1, code_offset + base->program_size);
    status = INTEL_D3D9_COMPILE_NOMEM;
    if (!p) goto release;
    p->base.abi = c->abi; p->base.header_bytes = sizeof(*p);
    p->base.allocation_bytes = code_offset + base->program_size; p->base.pci_id = c->pci_id;
    p->base.bytecode_offset = sizeof(*p); p->base.bytecode_bytes = bytes;
    p->base.code_offset = code_offset; p->base.code_bytes = base->program_size;
    p->base.stage = INTEL_D3D9_VERTEX; p->base.simd_mask = 1;
    p->base.grf_used = base->grf_used; p->base.dispatch_grf_start[0] = base->dispatch_grf_start_reg;
    p->base.curb_read_length = base->curb_read_length; p->base.flags = base->use_alt_mode ? 1u : 0u;
    p->base.varying_inputs = data.nr_attribute_slots; p->base.layer_input = UINT32_MAX;
    p->base.parameter_count = base->nr_params;
    if (map_bytes) {
        p->base.parameter_map_offset = sizeof(*p) + bytes;
        memcpy((unsigned char *)p + p->base.parameter_map_offset, base->param, map_bytes);
    }
    p->input_mask = inputs; p->output_mask = outputs;
    p->urb_entry_size = data.base.urb_entry_size; p->urb_read_length = data.base.urb_read_length;
    for (unsigned n = 0; n < 16; n++) p->input_semantics[n] = inputs & (1u << n) ? vertex.input_semantics[n] : UINT32_MAX;
    memcpy(p->output_slots, slots, sizeof(slots));
    memcpy((unsigned char *)p + sizeof(*p), snapshot, bytes);
    memcpy((unsigned char *)p + code_offset, native, base->program_size);
    *out = p; status = INTEL_D3D9_COMPILE_OK;
release:
    ralloc_free(memory); free(snapshot);
    __atomic_store_n(&c->busy, 0, __ATOMIC_RELEASE);
done:
    local.status = status;
    if (error) *error = local;
    return status;
}

void intel_d3d9_program_free(const struct intel_d3d9_program *program)
{
    free((void *)program);
}
