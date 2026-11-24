#include "../lib/intel_d3d9/compiler_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct compile_fixture {
    const char *name;
    const unsigned char *input;
    unsigned bytes, status;
    const unsigned char *native;
    unsigned native_bytes;
};
#include "compiler-api-fixtures.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

static void verify_program(const struct intel_d3d9_program *p, const struct compile_fixture *f)
{
    CHECK(p && p->abi == INTEL_D3D9_PROGRAM_LEGACY_ABI && p->header_bytes == 128 && p->pci_id == 0x3e9b);
    if (!p) return;
    CHECK(p->stage == 1 && p->simd_mask && !(p->simd_mask & ~7u));
    CHECK(p->bytecode_offset == 128 && p->bytecode_bytes == f->bytes);
    CHECK(p->code_offset >= p->bytecode_offset + p->bytecode_bytes && !(p->code_offset & 63));
    CHECK(p->code_bytes == f->native_bytes && p->allocation_bytes == p->code_offset + p->code_bytes);
    CHECK(!memcmp((const unsigned char *)p + p->bytecode_offset, f->input, f->bytes));
    CHECK(!memcmp((const unsigned char *)p + p->code_offset, f->native, f->native_bytes));
    CHECK(p->grf_used && !p->scratch_bytes && !p->parameter_count && p->varying_inputs <= 1 && !p->barycentric_modes);
    CHECK(p->layer_input == (p->varying_inputs ? 0 : UINT32_MAX));
    CHECK(p->constant_data_offset <= p->code_bytes && p->constant_data_bytes <= p->code_bytes - p->constant_data_offset);
    for (unsigned i = 0; i < 3; ++i) if (p->simd_mask & (1u << i)) {
        CHECK(p->kernel_offset[i] < p->code_bytes && !(p->kernel_offset[i] & 63));
        CHECK(p->dispatch_grf_start[i] < 128);
    }
    CHECK(!p->parameter_map_offset);
    for (unsigned i = 0; i < 5; ++i) CHECK(!p->reserved[i]);
}

int main(void)
{
    struct intel_d3d9_compiler *c = (void *)1, *other = NULL;
    CHECK(intel_d3d9_compiler_create(999, 0x3e9b, &c) == INTEL_D3D9_COMPILE_UNSUPPORTED && c == (void *)1);
    CHECK(intel_d3d9_compiler_create(1, 0xffff, &c) == INTEL_D3D9_COMPILE_UNSUPPORTED && c == (void *)1);
    CHECK(intel_d3d9_compiler_create(1, 0x3e9b, NULL) == INTEL_D3D9_COMPILE_INVALID);
    unsigned positive = 0, rejected = 0;
    for (unsigned round = 0; round < 3; ++round) {
        c = NULL;
        CHECK(intel_d3d9_compiler_create(1, 0x3e9b, &c) == 0 && c);
        CHECK(intel_d3d9_compiler_create(1, 0x3e9b, &other) == 0 && other && other != c);
        if (!c || !other) return 1;
        const struct intel_d3d9_program *held[54] = {0};
        for (unsigned i = 0; i < sizeof(api_fixtures) / sizeof(api_fixtures[0]); ++i) {
            const struct compile_fixture *f = &api_fixtures[i];
            unsigned char input[65536]; memcpy(input, f->input, f->bytes);
            const struct intel_d3d9_program *p = (void *)1;
            struct intel_d3d9_compile_error error;
            unsigned status = intel_d3d9_compile_pixel(c, input, f->bytes, &p, &error);
            CHECK(status == f->status && error.status == status);
            if (status != f->status) printf("fixture %s got %u expected %u: %s\n", f->name, status, f->status, error.message);
            if (!status) {
                ++positive; memset(input, 0, f->bytes); verify_program(p, f); held[i] = p;
            } else { ++rejected; CHECK(p == (void *)1 && error.word <= f->bytes / 4); }
        }
        const struct intel_d3d9_program *p = (void *)1;
        CHECK(intel_d3d9_compile_pixel(c, NULL, 4, &p, NULL) == INTEL_D3D9_COMPILE_INVALID && p == (void *)1);
        CHECK(intel_d3d9_compile_pixel(c, api_fixtures[0].input, 2, &p, NULL) == INTEL_D3D9_COMPILE_INVALID);
        CHECK(intel_d3d9_compile_pixel(c, api_fixtures[0].input, 65540, &p, NULL) == INTEL_D3D9_COMPILE_INVALID);
        intel_d3d9_compiler_destroy(c);
        unsigned status = intel_d3d9_compile_pixel(other, api_fixtures[0].input, api_fixtures[0].bytes, &p, NULL);
        CHECK(status == 0);
        if (!status) { verify_program(p, &api_fixtures[0]); intel_d3d9_program_free(p); }
        intel_d3d9_compiler_destroy(other);
        for (unsigned i = 0; i < 54; ++i) if (held[i]) {
            verify_program(held[i], &api_fixtures[i]); intel_d3d9_program_free(held[i]);
        }
    }
    intel_d3d9_compiler_destroy(NULL); intel_d3d9_program_free(NULL);
    CHECK(positive == 102 && rejected == 60);
    printf("COMPILER API checks=%u positive=%u rejected=%u failures=%u\n", checks, positive, rejected, failures);
    return failures ? 1 : 0;
}
