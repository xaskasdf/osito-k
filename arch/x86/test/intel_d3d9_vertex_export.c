#include "../lib/intel_d3d9/compiler_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef VERTEX_DISASSEMBLE
#include "compiler/brw_eu.h"
#include "compiler/brw_disasm.h"
#endif

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    unsigned char code[65536], saved[65536];
    size_t bytes = fread(code, 1, sizeof(code), f); int eof = fgetc(f);
    fclose(f); if (eof != EOF) return 2;
    memcpy(saved, code, bytes);
    struct intel_d3d9_compiler *c = NULL, *other = NULL;
    CHECK(!intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_VERTEX_ABI, 0x3e9b, &c));
    CHECK(!intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_ABI, 0x3e9b, &other));
    if (!c || !other) return 1;
    const struct intel_d3d9_vertex_program *p = (void *)1, *q = (void *)1;
    const struct intel_d3d9_program *pixel = (void *)1;
    struct intel_d3d9_compile_error error = {0};
    CHECK(intel_d3d9_compile_pixel(c, code, bytes, &pixel, NULL) == INTEL_D3D9_COMPILE_UNSUPPORTED && pixel == (void *)1);
    CHECK(intel_d3d9_compile_vertex(other, code, bytes, &p, NULL) == INTEL_D3D9_COMPILE_UNSUPPORTED && p == (void *)1);
    CHECK(intel_d3d9_compile_vertex(c, NULL, bytes, &p, NULL) == INTEL_D3D9_COMPILE_INVALID && p == (void *)1);
    unsigned status = intel_d3d9_compile_vertex(c, code, bytes, &p, &error);
    CHECK(status == error.status);
    unsigned again = intel_d3d9_compile_vertex(c, code, bytes, &q, NULL);
    CHECK(again == status);
    intel_d3d9_compiler_destroy(other); intel_d3d9_compiler_destroy(c);
    if (status) {
        CHECK(p == (void *)1 && q == (void *)1);
        printf("VERTEX status=%u word=%u unchanged=%u checks=%u failures=%u %s\n", status, error.word, p == (void *)1, checks, failures, error.message);
        return failures ? 1 : 3;
    }
    memset(code, 0, bytes);
    const struct intel_d3d9_program *b = &p->base;
    CHECK(!memcmp(p, q, b->allocation_bytes));
    CHECK(b->abi == 4 && b->header_bytes == 256 && b->pci_id == 0x3e9b && b->stage == 0 && b->simd_mask == 1);
    CHECK(b->bytecode_offset == 256 && b->bytecode_bytes == bytes);
    CHECK(!memcmp((const unsigned char *)p + 256, saved, bytes));
    CHECK(!(b->code_offset & 63) && b->code_offset >= 256 + bytes && b->code_bytes);
    CHECK(b->allocation_bytes == b->code_offset + b->code_bytes && !b->scratch_bytes);
    CHECK(!b->constant_data_bytes && !b->constant_data_offset && b->grf_used && b->grf_used <= 128);
    CHECK(!b->barycentric_modes && b->layer_input == UINT32_MAX && b->varying_inputs <= 16);
    CHECK(p->output_mask & 1); CHECK(!(p->output_mask & ~2047u)); CHECK(p->output_slots[0] == 1);
    CHECK(p->urb_entry_size && p->urb_entry_size <= 4 && p->urb_read_length == (b->varying_inputs + 1) / 2);
    unsigned count = 0;
    for (unsigned n = 0; n < 16; n++) {
        if (p->input_mask & (1u << n)) { count++; CHECK(p->input_semantics[n] != UINT32_MAX); }
        else CHECK(p->input_semantics[n] == UINT32_MAX);
    }
    CHECK(count == b->varying_inputs && !(p->input_mask & ~65535u));
    for (unsigned n = 0; n < 12; n++) {
        if (p->output_mask & (1u << n)) CHECK(p->output_slots[n] < 16);
        else CHECK(p->output_slots[n] == UINT32_MAX);
    }
    CHECK(b->curb_read_length == (b->parameter_count + 7) / 8 && b->parameter_count <= 512);
    if (b->parameter_count) {
        CHECK(b->parameter_map_offset == 256 + bytes);
        CHECK(b->parameter_map_offset + b->parameter_count * 4 <= b->code_offset);
        const uint32_t *map = (const void *)((const unsigned char *)p + b->parameter_map_offset);
        for (unsigned n = 0; n < b->parameter_count; n++) CHECK(map[n] < 1024);
    } else CHECK(!b->parameter_map_offset);
    for (unsigned n = 0; n < 5; n++) CHECK(!b->reserved[n]);
    for (unsigned n = 0; n < 3; n++) CHECK(!b->kernel_offset[n]);
    CHECK(b->dispatch_grf_start[0] < 128 && !b->dispatch_grf_start[1] && !b->dispatch_grf_start[2]);
#ifdef VERTEX_DISASSEMBLE
    struct intel_device_info device;
    struct brw_isa_info isa;
    CHECK(intel_get_device_info_from_pci_id(0x3e9b, &device));
    brw_init_isa_info(&isa, &device);
    brw_disassemble_with_errors(&isa, (const char *)p + b->code_offset, 0, stdout);
#endif
    if (!failures) {
        f = fopen(argv[2], "wb"); CHECK(f != NULL);
        if (f) { CHECK(fwrite(p, 1, b->allocation_bytes, f) == b->allocation_bytes); CHECK(!fclose(f)); }
    }
    printf("VERTEX status=0 inputs=%04x outputs=%03x params=%u bytes=%u checks=%u failures=%u\n",
        p->input_mask, p->output_mask, b->parameter_count, b->code_bytes, checks, failures);
    intel_d3d9_program_free(&p->base); intel_d3d9_program_free(&q->base);
    return failures ? 1 : 0;
}
