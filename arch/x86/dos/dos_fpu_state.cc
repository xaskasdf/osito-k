#include "dos_fpu_state.h"

static unsigned word(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void word(uint8_t *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void copy(uint8_t *to, const uint8_t *from, unsigned count)
{
    for (unsigned i = 0; i < count; i++) to[i] = from[i];
}

static void top(uint8_t *image, unsigned next)
{
    unsigned status = word(image + 2), old = (status >> 11) & 7u;
    if (old != next) {
        uint8_t before[128];
        copy(before, image + 32, sizeof(before));
        for (unsigned i = 0; i < 8; i++)
            copy(image + 32 + i * 16, before + ((i + next - old) & 7u) * 16, 16);
    }
    word(image + 2, (status & ~0x3800u) | (next << 11));
}

static void summary(uint8_t *image)
{
    unsigned status = word(image + 2) & ~0x8080u;
    if (status & ~word(image) & 0x3fu) status |= 0x8080u;
    word(image + 2, status);
}

static void reset(uint8_t *image)
{
    top(image, 0);
    word(image, 0x037f);
    word(image + 2, 0);
    image[4] = 0;
    for (unsigned i = 6; i < 24; i++) image[i] = 0;
}

static unsigned tags(const uint8_t *image)
{
    unsigned result = 0, first = (word(image + 2) >> 11) & 7u;
    for (unsigned physical = 0; physical < 8; physical++) {
        unsigned tag = 3;
        if (image[4] & (1u << physical)) {
            const uint8_t *p = image + 32 + ((physical - first) & 7u) * 16;
            unsigned nonzero = 0, exponent = word(p + 8) & 0x7fffu;
            for (unsigned b = 0; b < 8; b++) nonzero |= p[b];
            tag = !exponent && !nonzero ? 1 : !exponent || exponent == 0x7fff || !(p[7] & 0x80) ? 2 : 0;
        }
        result |= tag << (physical * 2);
    }
    return result;
}

bool dos_fpu_state_step(uint8_t *image, uint8_t *operand, uint8_t opcode,
                        uint8_t modrm, uint32_t flags, uint64_t *result)
{
    unsigned group = (modrm >> 3) & 7u, index = modrm & 7u;
    unsigned first = (word(image + 2) >> 11) & 7u;
    if (modrm < 0xc0) {
        bool environment = (opcode == 0xd9 || opcode == 0xdd) && (group == 4 || group == 6);
        if (environment && group == 4) {
            unsigned loaded = word(operand + 4);
            top(image, (loaded >> 11) & 7u);
            word(image, (word(operand) & 0x1f3fu) | 0x40u);
            word(image + 2, loaded);
            unsigned full = word(operand + 8);
            image[4] = 0;
            for (unsigned i = 0; i < 8; i++)
                if (((full >> (i * 2)) & 3u) != 3) image[4] |= 1u << i;
            if (opcode == 0xdd)
                for (unsigned i = 0; i < 8; i++) copy(image + 32 + i * 16, operand + 28 + i * 10, 10);
            summary(image);
        } else if (environment) {
            for (unsigned i = 0; i < 28; i++) operand[i] = 0;
            word(operand, word(image)); word(operand + 4, word(image + 2));
            word(operand + 8, tags(image));
            if (opcode == 0xdd) {
                for (unsigned i = 0; i < 8; i++) copy(operand + 28 + i * 10, image + 32 + i * 16, 10);
                reset(image);
            } else {
                word(image, word(image) | 0x3fu);
                summary(image);
            }
        } else if (opcode == 0xd9 && group == 5) {
            word(image, (word(operand) & 0x1f3fu) | 0x40u);
            summary(image);
        } else if (opcode == 0xd9 && group == 7) word(operand, word(image));
        else if (opcode == 0xdd && group == 7) word(operand, word(image + 2));
        else return false;
        return true;
    }
    if (opcode == 0xdb && modrm >= 0xe0 && modrm <= 0xe4) {
        if (modrm == 0xe2) word(image + 2, word(image + 2) & 0x7f00u);
        if (modrm == 0xe3) reset(image);
        return true;
    }
    if (opcode == 0xdf && modrm == 0xe0) { *result = word(image + 2); return true; }
    if (opcode == 0xd9 && modrm == 0xd0) return true;
    if (opcode == 0xdd && modrm < 0xc8) {
        image[4] &= ~(1u << ((first + index) & 7u));
        return true;
    }
    uint8_t *st0 = image + 32, *sti = st0 + index * 16;
    if (opcode == 0xd9 && modrm < 0xc8) {
        uint8_t value[10]; copy(value, sti, 10);
        first = (first + 7) & 7u;
        top(image, first);
        copy(st0, value, 10); image[4] |= 1u << first;
    } else if (opcode == 0xd9 && modrm < 0xd0) {
        uint8_t value[10]; copy(value, st0, 10); copy(st0, sti, 10); copy(sti, value, 10);
    } else if (opcode == 0xdd && modrm >= 0xd0 && modrm < 0xe0) {
        copy(sti, st0, 10); image[4] |= 1u << ((first + index) & 7u);
        if (modrm >= 0xd8) { image[4] &= ~(1u << first); top(image, (first + 1) & 7u); }
    } else if ((opcode == 0xda || opcode == 0xdb) && modrm < 0xe0) {
        static const unsigned masks[] = {1, 0x40, 0x41, 4};
        bool take = (flags & masks[group]) != 0;
        if (opcode == 0xdb) take = !take;
        if (take) copy(st0, sti, 10);
    } else if (opcode == 0xd9 && (modrm == 0xe0 || modrm == 0xe1)) {
        if (modrm == 0xe0) st0[9] ^= 0x80;
        else st0[9] &= 0x7f;
    } else if (opcode == 0xd9 && (modrm == 0xf6 || modrm == 0xf7)) {
        top(image, (first + (modrm == 0xf6 ? 7 : 1)) & 7u);
    } else if (opcode == 0xd9 && modrm >= 0xe8 && modrm <= 0xee) {
        /* Floor significands of the architectural constants; RC only changes
         * the low bit. Constant loads never raise precision or set C1. */
        static const uint64_t floor[] = {UINT64_C(0x8000000000000000), UINT64_C(0xd49a784bcd1b8afe),
            UINT64_C(0xb8aa3b295c17f0bb), UINT64_C(0xc90fdaa22168c234), UINT64_C(0x9a209a84fbcff798),
            UINT64_C(0xb17217f7d1cf79ab), 0};
        static const unsigned exponents[] = {0x3fff, 0x4000, 0x3fff, 0x4000, 0x3ffd, 0x3ffe, 0};
        static const bool nearest_up[] = {false, false, true, true, true, true, false};
        unsigned constant = modrm - 0xe8, rc = word(image) >> 10 & 3u;
        bool rounded = constant && constant < 6 && (rc == 2 || (rc == 0 && nearest_up[constant]));
        uint64_t value = floor[constant] + rounded;
        first = (first + 7) & 7u; top(image, first);
        for (unsigned b = 0; b < 8; b++) st0[b] = value >> (8 * b);
        word(st0 + 8, exponents[constant]); image[4] |= 1u << first;
    } else return false;
    word(image + 2, word(image + 2) & ~0x200u);
    return true;
}
