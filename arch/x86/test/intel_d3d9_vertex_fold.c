/* Test-only constant folding verifies the frontend's arithmetic/masking.
 * Production vertex data is never run through this path. */
#include "../lib/intel_d3d9/nir_frontend.h"
#include "compiler/brw_compiler.h"
#include "compiler/nir/nir_builder.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    unsigned char code[65536]; size_t size = fread(code, 1, sizeof(code), f); fclose(f);
    glsl_type_singleton_init_or_ref();
    void *ctx = ralloc_context(NULL);
    struct intel_device_info device;
    if (!intel_get_device_info_from_pci_id(0x3e9b, &device)) return 2;
    struct brw_compiler *compiler = brw_compiler_create(ctx, &device);
    struct intel_d3d9_shader_error error;
    struct intel_d3d9_uniform_bindings bindings;
    struct intel_d3d9_vertex_bindings vertex;
    nir_shader *nir = intel_d3d9_vertex_to_nir(code, size,
        compiler->nir_options[MESA_SHADER_VERTEX], ctx, &error, &bindings, &vertex);
    int status = 1;
    if (!nir || bindings.count) goto done;
    nir_lower_alu_to_scalar(nir, NULL, NULL);
    bool progress; unsigned rounds = 0, stores = 0;
    do {
        progress = false;
        NIR_PASS(progress, nir, nir_opt_constant_folding);
        NIR_PASS(progress, nir, nir_copy_prop);
        NIR_PASS(progress, nir, nir_opt_dce);
        if (++rounds > 32) goto done;
    } while (progress);
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
        if (instr->type != nir_instr_type_intrinsic) continue;
        nir_intrinsic_instr *i = nir_instr_as_intrinsic(instr);
        if (i->intrinsic != nir_intrinsic_store_deref) continue;
        if (!nir_src_is_const(i->src[1]) || i->num_components != 4) goto done;
        nir_const_value *v = nir_src_as_const_value(i->src[1]);
        for (unsigned c = 0; c < 4; c++) if (v[c].u32 != (uint32_t)strtoul(argv[2+c], NULL, 16)) {
            printf("FOLD channel=%u actual=%08x expected=%s\n", c, v[c].u32, argv[2+c]); goto done;
        }
        stores++;
    }
    status = stores == 1 ? 0 : 1;
done:
    printf("VERTEX FOLD passed=%u\n", !status);
    ralloc_free(ctx); glsl_type_singleton_decref(); return status;
}
