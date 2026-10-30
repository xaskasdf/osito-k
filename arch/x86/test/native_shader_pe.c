/* CPU compiler-input probe only. Never creates a GPU device or submits work. */
#include "../lib/intel_d3d9/shader.h"
#include "native_shader_fixtures.h"

__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long,
                                           unsigned long *, void *);
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned);

static unsigned checks;
static struct intel_d3d9_shader_instruction ir[256];

void *memset(void *target, int value, size_t bytes)
{
    volatile unsigned char *p = target;
    while (bytes--) *p++ = (unsigned char)value;
    return target;
}

void *memcpy(void *target, const void *source, size_t bytes)
{
    volatile unsigned char *p = target;
    const unsigned char *q = source;
    while (bytes--) *p++ = *q++;
    return target;
}

static void report(const char *label, unsigned value)
{
    unsigned long bytes, n = 0;
    char text[9];
    while (label[n]) ++n;
    WriteFile(GetStdHandle((unsigned long)-11), label, n, &bytes, NULL);
    for (n = 0; n < 8; ++n) text[n] = "0123456789abcdef"[(value >> (28 - n * 4)) & 15];
    text[8] = '\n';
    WriteFile(GetStdHandle((unsigned long)-11), text, 9, &bytes, NULL);
}

static void check(int valid, unsigned line)
{
    ++checks;
    if (!valid) { report("SHADER FAIL line=", line); ExitProcess(1); }
}
#define CHECK(x) check(!!(x), __LINE__)

void native_shader_entry(void)
{
    static const unsigned counts[] = {2,5,3,4,5,13,6,4,5,4,7,7,4,8,9,5};
    unsigned index, fixtures = sizeof(shader_fixtures) / sizeof(shader_fixtures[0]);
    CHECK(fixtures == sizeof(counts) / sizeof(counts[0]));
    for (index = 0; index < fixtures; ++index) {
        struct intel_d3d9_shader_info info, counted;
        struct intel_d3d9_shader_error error;
        unsigned n, bytes = shader_fixtures[index].bytes;
        const void *code = shader_fixtures[index].data;
        CHECK(intel_d3d9_shader_decode(code, bytes, &counted, NULL, 0, &error) == 0);
        CHECK(counted.instruction_count == counts[index]);
        CHECK(intel_d3d9_shader_decode(code, bytes, &info, ir, 256, &error) == 0);
        CHECK(info.word_count == bytes / 4 && info.instruction_count == counted.instruction_count);
        CHECK(error.word_offset == 0 && error.status == 0);
        CHECK(intel_d3d9_shader_decode(code, bytes, &info, ir, counts[index] - 1, &error) == INTEL_D3D9_SHADER_CAPACITY);
        for (n = 0; n < bytes; ++n)
            CHECK(intel_d3d9_shader_decode(code, n, &info, ir, 256, &error) != 0);
    }
    report("SHADER fixtures=", fixtures);
    report("SHADER checks=", checks);
    report("SHADER failures=", 0);
    ExitProcess(0);
}
