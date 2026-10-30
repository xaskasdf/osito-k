#include "../lib/intel_d3d9/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static struct intel_d3d9_shader_instruction ir[INTEL_D3D9_SHADER_MAX_INSTRUCTIONS];
static struct intel_d3d9_shader_instruction before[INTEL_D3D9_SHADER_MAX_INSTRUCTIONS];

static void check(int passed, int line)
{
    ++checks;
    if (!passed) { fprintf(stderr, "shader check failed at line %d\n", line); exit(1); }
}
#define CHECK(x) check(!!(x), __LINE__)

static void decode_cases(void)
{
    struct intel_d3d9_shader_info info, old;
    struct intel_d3d9_shader_error error;
    uint32_t shader[] = {0xffff0300, 0x02000001, 0x800f0800, 0xa0e40000, 0xffff};
    uint32_t comments[] = {0xffff0200, 0x0002fffe, 0xffff, 0xdeadbeef,
                          0x02000001, 0x800f0800, 0xa0e40000, 0xffff};
    uint32_t temporary[sizeof(comments) / 4];
    uint8_t unaligned[sizeof(comments) + 1];
    uint32_t *large;
    unsigned n;
    memset(&info, 0x5a, sizeof(info)); old = info;
    memset(ir, 0x3c, sizeof(ir)); memcpy(before, ir, sizeof(ir));
    CHECK(intel_d3d9_shader_decode(shader, sizeof(shader), &info, NULL, 0, &error) == 0);
    CHECK(info.instruction_count == 1 && info.stage == INTEL_D3D9_PIXEL && info.major == 3);
    CHECK(error.status == 0 && error.word_offset == 0);
    CHECK(!memcmp(before, ir, sizeof(ir)));
    info = old;
    CHECK(intel_d3d9_shader_decode(shader, sizeof(shader), &info, ir, 0, &error) == INTEL_D3D9_SHADER_CAPACITY);
    CHECK(!memcmp(&info, &old, sizeof(info)) && !memcmp(before, ir, sizeof(ir)));
    CHECK(intel_d3d9_shader_decode(shader, sizeof(shader), &info, ir, 1, &error) == 0);
    CHECK(ir[0].opcode == D9_MOV && ir[0].destination.type == D9_COLOR_OUTPUT);
    CHECK(ir[0].sources[0].type == D9_CONST && ir[0].sources[0].swizzle == 0xe4);
    CHECK(ir[0].word_offset == 1 && ir[0].word_count == 3);
    CHECK(!memcmp(&before[1], &ir[1], sizeof(ir) - sizeof(ir[0])));
    memcpy(unaligned + 1, comments, sizeof(comments));
    CHECK(intel_d3d9_shader_decode(unaligned + 1, sizeof(comments), &info, ir, 1, NULL) == 0);
    CHECK(info.instruction_count == 1 && ir[0].word_offset == 4);
    CHECK(intel_d3d9_shader_decode(shader, sizeof(shader), &info, (void *)shader, 1, NULL) == INTEL_D3D9_SHADER_INVALID);
    CHECK(intel_d3d9_shader_decode(NULL, sizeof(shader), &info, NULL, 0, NULL) == INTEL_D3D9_SHADER_INVALID);
    CHECK(intel_d3d9_shader_decode(shader, sizeof(shader), NULL, NULL, 0, NULL) == INTEL_D3D9_SHADER_INVALID);
    CHECK(intel_d3d9_shader_decode(shader, sizeof(shader), &info, NULL, 1, NULL) == INTEL_D3D9_SHADER_INVALID);
    /* Exact-byte-boundary truncations, not merely missing END. */
    for (n = 0; n < sizeof(shader); ++n) {
        info = old; memcpy(ir, before, sizeof(ir));
        CHECK(intel_d3d9_shader_decode(shader, n, &info, ir, 1, &error) != 0);
        CHECK(!memcmp(&info, &old, sizeof(info)) && !memcmp(before, ir, sizeof(ir)));
    }
    /* Independently chosen illegal fields: length, opcode, parameter marker,
     * empty destination mask, reserved source modifier and trailing garbage. */
    for (n = 0; n < 8; ++n) {
        memcpy(temporary, shader, sizeof(shader));
        if (n == 0) temporary[1] = 0x03000001;
        if (n == 1) temporary[1] = 0x01000001;
        if (n == 2) temporary[1] = 0x02000123;
        if (n == 3) temporary[2] &= ~0x80000000u;
        if (n == 4) temporary[2] &= ~0x000f0000u;
        if (n == 5) temporary[3] |= 0x0f000000u;
        if (n == 6) temporary[4] |= 0x00010000u;
        if (n == 7) temporary[0] = 0xffff0104;
        info = old; memcpy(ir, before, sizeof(ir));
        CHECK(intel_d3d9_shader_decode(temporary, sizeof(shader), &info, ir, 1, &error) != 0);
        CHECK(!memcmp(&info, &old, sizeof(info)) && !memcmp(before, ir, sizeof(ir)));
        CHECK(error.word_offset < sizeof(shader) / 4);
    }
    memcpy(temporary, comments, sizeof(comments)); temporary[1] = 0x7ffffffe;
    CHECK(intel_d3d9_shader_decode(temporary, sizeof(temporary), &info, NULL, 0, NULL) == INTEL_D3D9_SHADER_INVALID);
    temporary[1] = 0x8002fffe;
    CHECK(intel_d3d9_shader_decode(temporary, sizeof(temporary), &info, NULL, 0, NULL) == INTEL_D3D9_SHADER_INVALID);
    memcpy(temporary, shader, sizeof(shader)); temporary[5] = 0;
    CHECK(intel_d3d9_shader_decode(temporary, sizeof(shader) + 4, &info, NULL, 0, NULL) == INTEL_D3D9_SHADER_INVALID);
    large = calloc(INTEL_D3D9_SHADER_MAX_WORDS + 1, 4); CHECK(large != NULL);
    large[0] = 0xffff0300;
    large[INTEL_D3D9_SHADER_MAX_INSTRUCTIONS + 1] = 0xffff;
    CHECK(intel_d3d9_shader_decode(large, (INTEL_D3D9_SHADER_MAX_INSTRUCTIONS + 2) * 4,
          &info, ir, INTEL_D3D9_SHADER_MAX_INSTRUCTIONS, NULL) == 0);
    large[INTEL_D3D9_SHADER_MAX_INSTRUCTIONS + 1] = 0;
    large[INTEL_D3D9_SHADER_MAX_INSTRUCTIONS + 2] = 0xffff;
    CHECK(intel_d3d9_shader_decode(large, (INTEL_D3D9_SHADER_MAX_INSTRUCTIONS + 3) * 4,
          &info, NULL, 0, NULL) == INTEL_D3D9_SHADER_UNSUPPORTED);
    CHECK(intel_d3d9_shader_decode(large, (INTEL_D3D9_SHADER_MAX_WORDS + 1) * 4,
          &info, NULL, 0, NULL) == INTEL_D3D9_SHADER_UNSUPPORTED);
    free(large);
}

static void fixture_case(const char *directory, const char *name, unsigned count)
{
    char path[2048];
    uint8_t bytes[4096], mutation[4096];
    size_t size, n, k;
    FILE *file;
    struct intel_d3d9_shader_info info, old;
    struct intel_d3d9_shader_error error;
    snprintf(path, sizeof(path), "%s/%s.bin", directory, name);
    file = fopen(path, "rb"); CHECK(file != NULL);
    size = fread(bytes, 1, sizeof(bytes), file); CHECK(!ferror(file) && feof(file)); fclose(file);
    CHECK(size >= 8 && size < sizeof(bytes));
    CHECK(intel_d3d9_shader_decode(bytes, size, &info, ir, INTEL_D3D9_SHADER_MAX_INSTRUCTIONS, &error) == 0);
    CHECK(info.instruction_count == count);
    CHECK(info.stage == (name[0] == 'p' ? INTEL_D3D9_PIXEL : INTEL_D3D9_VERTEX));
    CHECK(info.major == (unsigned)(name[2] - '0'));
    if (!strcmp(name, "ps30-predicate")) {
        CHECK(ir[1].opcode == D9_SETP && ir[1].control == 1);
        CHECK(ir[2].predicated && ir[2].predicate.type == D9_PREDICATE);
        CHECK(ir[2].predicate.modifier == 0 && ir[3].predicate.modifier == 13);
        CHECK(ir[2].sources[0].type == D9_CONST && ir[2].sources[0].index == 0);
        CHECK(ir[3].sources[0].index == 1 && ir[3].destination.type == D9_TEMP);
    }
    if (!strcmp(name, "ps20-constants")) {
        CHECK(ir[0].opcode == D9_DEF && ir[0].immediate_count == 4);
        CHECK(ir[0].immediate[0] == 0x3f800000 && ir[0].immediate[1] == 0xc0000000);
        CHECK(ir[0].immediate[2] == 0x3f000000 && ir[0].immediate[3] == 0);
        CHECK(ir[1].destination.modifier == 1 && ir[1].sources[0].swizzle == 0x1b);
    }
    if (!strcmp(name, "vs30-relative")) {
        CHECK(ir[3].sources[0].relative && ir[3].sources[0].address_type == D9_ADDRESS_TEXTURE);
        CHECK(ir[3].sources[0].address_swizzle == 0 && ir[3].word_count == 4);
    }
    if (!strcmp(name, "vs30-flow")) {
        CHECK(ir[2].opcode == D9_DEFI && ir[2].immediate[0] == 2);
        CHECK(ir[3].opcode == D9_DEFB && ir[3].immediate[0] == 1);
        CHECK(ir[7].opcode == D9_ADD && ir[7].sources[1].relative);
        CHECK(ir[7].sources[1].address_type == D9_LOOP_REGISTER);
    }
    if (!strcmp(name, "vs20-sincos")) CHECK(ir[3].opcode == D9_SINCOS && ir[3].source_count == 3);
    if (!strcmp(name, "vs30-sincos")) CHECK(ir[2].opcode == D9_SINCOS && ir[2].source_count == 1);
    if (!strcmp(name, "ps30-gradient")) CHECK(ir[4].opcode == D9_TEXLDD && ir[4].source_count == 4);
    if (!strcmp(name, "ps20-texture")) CHECK(ir[1].declaration == 0x90000000 && ir[1].destination.type == D9_SAMPLER);
    /* Test every truncated length with a separately sized allocation, so ASan
     * catches accesses past the caller's declared bytes, not just the array. */
    memset(&old, 0xa7, sizeof(old));
    memset(before, 0x6b, sizeof(before));
    for (n = 0; n < size; ++n) {
        uint8_t *shortened = malloc(n ? n : 1); CHECK(shortened != NULL);
        memcpy(shortened, bytes, n); info = old; memcpy(ir, before, sizeof(ir));
        CHECK(intel_d3d9_shader_decode(shortened, n, &info, ir, INTEL_D3D9_SHADER_MAX_INSTRUCTIONS, &error) != 0);
        CHECK(!memcmp(&info, &old, sizeof(info)) && !memcmp(before, ir, sizeof(ir)));
        free(shortened);
    }
    /* Mutate every individual bit. Valid mutations may remain valid; invalid
     * ones must never publish a partially decoded program. */
    for (n = 0; n < size; ++n) for (k = 0; k < 8; ++k) {
        enum intel_d3d9_shader_status status;
        memcpy(mutation, bytes, size); mutation[n] ^= (uint8_t)(1u << k);
        info = old; memcpy(ir, before, sizeof(ir));
        status = intel_d3d9_shader_decode(mutation, size, &info, ir, INTEL_D3D9_SHADER_MAX_INSTRUCTIONS, &error);
        CHECK(status == error.status);
        if (status) CHECK(!memcmp(&info, &old, sizeof(info)) && !memcmp(before, ir, sizeof(ir)));
        else CHECK(info.instruction_count <= INTEL_D3D9_SHADER_MAX_INSTRUCTIONS && info.word_count == size / 4);
    }
    printf("fixture %s instructions=%u passed\n", name, count);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    decode_cases();
    fixture_case(argv[1], "vs20-position", 2);
    fixture_case(argv[1], "ps20-texture", 5);
    fixture_case(argv[1], "ps20-constants", 3);
    fixture_case(argv[1], "vs30-relative", 4);
    fixture_case(argv[1], "ps30-predicate", 5);
    fixture_case(argv[1], "vs30-flow", 13);
    fixture_case(argv[1], "ps30-gradient", 6);
    fixture_case(argv[1], "ps30-lod", 4);
    fixture_case(argv[1], "vs20-sincos", 5);
    fixture_case(argv[1], "vs30-sincos", 4);
    fixture_case(argv[1], "ps30-call", 7);
    fixture_case(argv[1], "ps30-rep", 7);
    fixture_case(argv[1], "vs30-sgn", 4);
    fixture_case(argv[1], "vs30-relative-output", 8);
    fixture_case(argv[1], "vs30-hlsl", 9);
    fixture_case(argv[1], "ps20-hlsl", 5);
    printf("native shader decoder checks=%u failures=0\n", checks);
    return 0;
}
