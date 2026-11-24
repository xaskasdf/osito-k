#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../dos/dos_fpu_softfloat.h"

unsigned cpu_xsave_active;
extern const uint8_t dos_fpu_register_ops[], dos_fpu_memory_ops[];
extern uint64_t dos_fpu_host_step(void *, const uint8_t *, void *, uint32_t);

static const struct { uint64_t sig; uint16_t exp; } values[] = {
    {0, 0}, {0, 0x8000}, {0x8000000000000000ull, 0x3fff},
    {0x8000000000000000ull, 0xbfff}, {0xc000000000000000ull, 0x3fff},
    {0x8000000000000000ull, 0x4000}, {0x8000000000000001ull, 0x3fff},
    {0xffffffffffffffffull, 0x3ffe}, {0x8000000000000000ull, 0x7ffe},
    {0xffffffffffffffffull, 0x7ffe}, {0x8000000000000000ull, 1},
    {1, 0}, {0x4000000000000000ull, 0}, {0x8000000000000000ull, 0},
    {0x4000000000000000ull, 0x3fff}, {0x8000000000000000ull, 0x7fff},
    {0x8000000000000000ull, 0xffff}, {0xa000000000000123ull, 0x7fff},
    {0xc000000000000123ull, 0x7fff}, {0x8000000000000000ull, 0x400f},
    {0x8000000000000000ull, 0xc00f}, {0x8000000000000000ull, 0x3fbe},
};
static const uint32_t single[] = {
    0, 0x80000000, 0x3f800000, 0xbf800000, 0x3fc00000, 0x40000000,
    0x3f800001, 0x3f7fffff, 0x7f000000, 0x7f7fffff, 0x00800000,
    1, 0x00400000, 0x007fffff, 0x80800000, 0x7f800000, 0xff800000,
    0x7fa00123, 0x7fc00456, 0x47800000, 0xc7800000, 0x1f000000,
};
static const uint64_t dual[] = {
    0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x3ff8000000000000ull, 0x4000000000000000ull, 0x3ff0000000000001ull,
    0x3fefffffffffffffull, 0x7fe0000000000000ull, 0x7fefffffffffffffull,
    0x0010000000000000ull, 1, 0x0004000000000000ull, 0x000fffffffffffffull,
    0x8010000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
    0x7ff4000000000123ull, 0x7ff8000000000456ull, 0x40f0000000000000ull,
    0xc0f0000000000000ull, 0x3be0000000000000ull,
};

static void put(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) p[i] = value >> (8 * i);
}

static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

int main(void)
{
    unsigned checks = 0, failures = 0, forms = 0;
    const unsigned pcs[] = {0, 2, 3};
    for (unsigned opcode = 0xd8; opcode <= 0xde; opcode++)
    for (unsigned modrm = 0; modrm <= 0xff; modrm++) {
        /* Approximate trig instructions have a separate MPFR-backed test. */
        if (opcode == 0xd9 && modrm != 0xfa && modrm != 0xfc && modrm != 0xfd) continue;
        bool memory = modrm < 0xc0;
        if (memory && (modrm & 0xc7u)) continue;
        _Alignas(64) uint8_t image[1024] = {0};
        uint8_t operand[108] = {0};
        dos_fpu_numeric_result_t computed;
        if (!dos_fpu_soft_evaluate(image, operand, opcode, modrm, &computed)) continue;
        if (!computed.write) continue; /* Comparisons have their own oracle. */
        forms++;
        unsigned form_failures = 0;
        for (unsigned a = 0; a < sizeof(values) / sizeof(values[0]); a++)
        for (unsigned b = 0; b < sizeof(values) / sizeof(values[0]); b++)
        for (unsigned pc = 0; pc < 3; pc++) for (unsigned rc = 0; rc < 4; rc++)
        for (unsigned mask = 0; mask < 7; mask++) {
            memset(image, 0, sizeof(image));
            uint16_t control = 0x7f | (pcs[pc] << 8) | (rc << 10);
            if (mask) control &= ~(1u << (mask - 1));
            put(image, control, 2);
            put(image + 24, 0x1f80, 4);
            image[4] = 0xff;
            for (unsigned reg = 0; reg < 8; reg++) {
                put(image + 32 + reg * 16, values[b].sig, 8);
                put(image + 40 + reg * 16, values[b].exp, 2);
            }
            put(image + 32, values[a].sig, 8);
            put(image + 40, values[a].exp, 2);
            put(operand, opcode == 0xdc ? dual[b] : single[b], 8);
            if (!dos_fpu_soft_evaluate(image, operand, opcode, modrm, &computed)) return 2;
            const uint8_t *code = memory ? dos_fpu_memory_ops +
                ((opcode - 0xd8u) * 8u + (modrm >> 3)) * 8u :
                dos_fpu_register_ops + ((opcode - 0xd8u) * 64u + modrm - 0xc0u) * 8u;
            dos_fpu_host_step(image, code, operand, 0);
            unsigned actual = get16(image + 2) & 0x23fu;
            bool inhibited = (computed.exceptions & ~control & 7u) != 0;
            unsigned dest = (computed.destination - computed.pops) & 7u;
            bool same = actual == computed.exceptions && (inhibited ||
                !memcmp(image + 32 + 16 * dest, computed.value, 10));
            checks++;
            if (!same) {
                failures++;
                if (form_failures++ < 2)
                    printf("FAIL %02x%02x a=%u b=%u CW=%04x flags=%04x/%04x value=%04x/%04x\n",
                           opcode, modrm, a, b, control, actual, computed.exceptions,
                           get16(image + 40 + 16 * dest), get16(computed.value + 8));
            }
        }
        if (form_failures) printf("FORM %02x%02x failures=%u\n", opcode, modrm, form_failures);
    }
    printf("SOFTFLOAT HOST forms=%u checks=%u failures=%u\n", forms, checks, failures);
    return failures != 0;
}
