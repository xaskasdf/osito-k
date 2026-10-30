#include "../lib/intel_d3d9/nir_frontend.h"
#include "compiler/brw_compiler.h"
#include "compiler/brw_nir.h"
#include "compiler/brw_eu.h"
#include "compiler/brw_disasm.h"
#include "dev/intel_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void compiler_log(void *context, unsigned *id, const char *format, ...)
{
    va_list args;
    (void)context; (void)id;
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
}

int main(int argc, char **argv)
{
    unsigned char code[65536];
    struct intel_device_info device;
    struct intel_d3d9_shader_error error;
    struct brw_wm_prog_key key = {0};
    struct brw_wm_prog_data data = {0};
    struct brw_nir_compiler_opts options = {0};
    struct brw_compile_fs_params params = {0};
    FILE *input, *output;
    size_t bytes;
    if (argc != 3 && argc != 7) return 2;
    input = fopen(argv[1], "rb");
    if (!input) return 2;
    bytes = fread(code, 1, sizeof(code), input);
    if (ferror(input) || !feof(input)) return 2;
    fclose(input);
    if (!intel_get_device_info_from_pci_id(0x3e9b, &device) || device.ver != 9) return 3;
    void *ctx = ralloc_context(NULL);
    glsl_type_singleton_init_or_ref();
    process_intel_debug_variable();
    struct brw_compiler *compiler = brw_compiler_create(ctx, &device);
    compiler->shader_debug_log = compiler_log;
    compiler->shader_perf_log = compiler_log;
    struct nir_shader *nir = intel_d3d9_shader_to_nir(code, bytes,
        compiler->nir_options[MESA_SHADER_FRAGMENT], ctx, &error);
    if (!nir) {
        fprintf(stderr, "frontend status=%u word=%u\n", error.status, error.word_offset);
        ralloc_free(ctx); glsl_type_singleton_decref(); return 4;
    }
    if (argc == 7) {
        bool progress;
        unsigned iterations = 0, stores = 0;
        nir_lower_alu_to_scalar(nir, NULL, NULL);
        do {
            progress = false;
            NIR_PASS(progress, nir, nir_opt_constant_folding);
            NIR_PASS(progress, nir, nir_copy_prop);
            NIR_PASS(progress, nir, nir_opt_dce);
            if (++iterations > 32) return 8;
        } while (progress);
        nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
        nir_foreach_instr(instruction, block) {
            if (instruction->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instruction);
            if (intrinsic->intrinsic != nir_intrinsic_store_deref) continue;
            if (!nir_src_is_const(intrinsic->src[1]) || intrinsic->num_components != 4) {
                nir_print_shader(nir, stderr); return 8;
            }
            nir_const_value *value = nir_src_as_const_value(intrinsic->src[1]);
            for (unsigned c = 0; c < 4; ++c)
                if (value[c].u32 != (uint32_t)strtoul(argv[3+c], NULL, 16)) {
                    fprintf(stderr, "channel %u got %08x expected %s\n", c, value[c].u32, argv[3+c]); return 8;
                }
            ++stores;
        }
        if (stores != 1) return 8;
        printf("D3D9 constant result matches all four expected channels\n");
    }
    nir_print_shader(nir, stdout);
    brw_preprocess_nir(compiler, nir, &options);
    key.nr_color_regions = 1;
    params.base.mem_ctx = ctx; params.base.nir = nir;
    params.key = &key; params.prog_data = &data; params.max_polygons = 1;
    const unsigned *native = brw_compile_fs(compiler, &params);
    if (!native) {
        fprintf(stderr, "Intel compiler failed: %s\n", params.base.error_str);
        ralloc_free(ctx); glsl_type_singleton_decref(); return 5;
    }
    int valid = 1;
    const unsigned starts[] = {0, data.prog_offset_16, data.prog_offset_32};
    const unsigned enabled[] = {data.dispatch_8, data.dispatch_16, data.dispatch_32};
    for (unsigned variant = 0; variant < 3; ++variant) if (enabled[variant]) {
        unsigned end = brw_disassemble_find_end(&compiler->isa, native, starts[variant]);
        valid &= end > starts[variant] && end <= data.base.program_size &&
            brw_validate_instructions(&compiler->isa, native, starts[variant], end, NULL);
        brw_disassemble_with_errors(&compiler->isa, native, starts[variant], stdout);
    }
    printf("Native Intel compiler pci=3e9b gen=%u bytes=%u simd8=%u simd16=%u simd32=%u grf=%u scratch=%u validated=%u\n",
           device.ver, data.base.program_size, data.dispatch_8, data.dispatch_16, data.dispatch_32,
           data.base.grf_used, data.base.total_scratch, valid);
    if (!data.base.program_size || !(data.dispatch_8 || data.dispatch_16 || data.dispatch_32) ||
        data.base.total_scratch || !valid) return 6;
    output = fopen(argv[2], "wb");
    if (!output || fwrite(native, 1, data.base.program_size, output) != data.base.program_size || fclose(output)) return 7;
    ralloc_free(ctx);
    glsl_type_singleton_decref();
    return 0;
}
