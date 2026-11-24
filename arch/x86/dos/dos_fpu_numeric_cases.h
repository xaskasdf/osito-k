#ifndef OSITOK_DOS_FPU_NUMERIC_CASES_H
#define OSITOK_DOS_FPU_NUMERIC_CASES_H

/* Test operands are encoded directly, without compiler floating-point math. */
enum {
    FPN_ZERO, FPN_ONE, FPN_TWO, FPN_NEG_ONE, FPN_INF, FPN_SNAN, FPN_QNAN,
    FPN_UNNORMAL, FPN_DENORMAL, FPN_MINIMUM, FPN_LARGE, FPN_HALF_ULP,
    FPN_ONE_HALF, FPN_ONE_EXTRA, FPN_SCALE_LARGE
};
static const struct { uint64_t significand; uint16_t exponent; } fpn_values[] = {
    {0, 0}, {0x8000000000000000ull, 0x3fff}, {0x8000000000000000ull, 0x4000},
    {0x8000000000000000ull, 0xbfff}, {0x8000000000000000ull, 0x7fff},
    {0xa000000000000123ull, 0x7fff}, {0xc000000000000123ull, 0x7fff},
    {0x4000000000000000ull, 0x3fff}, {0x4000000000000000ull, 0},
    {0x8000000000000000ull, 1}, {0x8000000000000000ull, 0x7ffe},
    {0x8000000000000000ull, 0x3fbe}, {0xc000000000000000ull, 0x3fff},
    {0x8000000200000000ull, 0x3fff}, {0x8000000000000000ull, 0x400f}
};
enum { FPN_NO_MEMORY, FPN_SNAN32, FPN_SNAN64, FPN_DENORMAL32, FPN_DENORMAL64,
       FPN_SNAN80, FPN_DENORMAL80 };
enum { FPN_DEST0, FPN_POP, FPN_PUSH, FPN_PAIR, FPN_COMPARE_POP,
       FPN_COMPARE_POP2, FPN_FLAGS_POP, FPN_STORE_POP, FPN_STORE_REG_POP };
static const struct fpn_case {
    const char *name;
    uint8_t opcode, modrm, a, b, memory, exception, effect;
} fpn_cases[] = {
    {"fdivp-invalid", 0xde,0xf9, FPN_ZERO,FPN_ZERO,0,1,FPN_POP},
    {"faddp-snan", 0xde,0xc1, FPN_SNAN,FPN_ONE,0,1,FPN_POP},
    {"faddp-unsupported", 0xde,0xc1, FPN_UNNORMAL,FPN_ONE,0,1,FPN_POP},
    {"fsqrt-negative", 0xd9,0xfa, FPN_NEG_ONE,FPN_ONE,0,1,FPN_DEST0},
    {"fptan-infinity", 0xd9,0xf2, FPN_INF,FPN_ONE,0,1,FPN_PAIR},
    {"fsincos-infinity", 0xd9,0xfb, FPN_INF,FPN_ONE,0,1,FPN_PAIR},
    {"fxtract-snan", 0xd9,0xf4, FPN_SNAN,FPN_ONE,0,1,FPN_PAIR},
    {"fyl2x-negative", 0xd9,0xf1, FPN_NEG_ONE,FPN_ONE,0,1,FPN_POP},
    {"fcomip-qnan", 0xdf,0xf1, FPN_QNAN,FPN_ONE,0,1,FPN_FLAGS_POP},
    {"fucomip-snan", 0xdf,0xe9, FPN_SNAN,FPN_ONE,0,1,FPN_FLAGS_POP},
    {"fcomp-qnan", 0xd8,0xd9, FPN_QNAN,FPN_ONE,0,1,FPN_COMPARE_POP},
    {"fcompp-qnan", 0xde,0xd9, FPN_QNAN,FPN_ONE,0,1,FPN_COMPARE_POP2},
    {"fld-snan32", 0xd9,0x00, FPN_ONE,FPN_ONE,FPN_SNAN32,1,FPN_PUSH},
    {"fld-snan64", 0xdd,0x00, FPN_ONE,FPN_ONE,FPN_SNAN64,1,FPN_PUSH},
    {"fdiv-snan32", 0xd8,0x30, FPN_ONE,FPN_ONE,FPN_SNAN32,1,FPN_DEST0},
    {"faddp-denormal", 0xde,0xc1, FPN_DENORMAL,FPN_ONE,0,2,FPN_POP},
    {"fadd-denormal32", 0xd8,0x00, FPN_ONE,FPN_ONE,FPN_DENORMAL32,2,FPN_DEST0},
    {"fld-denormal32", 0xd9,0x00, FPN_ONE,FPN_ONE,FPN_DENORMAL32,2,FPN_PUSH},
    {"fld-denormal64", 0xdd,0x00, FPN_ONE,FPN_ONE,FPN_DENORMAL64,2,FPN_PUSH},
    {"fcomp-denormal", 0xd8,0xd9, FPN_DENORMAL,FPN_ONE,0,2,FPN_COMPARE_POP},
    {"fucomip-denormal", 0xdf,0xe9, FPN_DENORMAL,FPN_ONE,0,2,FPN_FLAGS_POP},
    {"fdivp-zero", 0xde,0xf9, FPN_ZERO,FPN_ONE,0,4,FPN_POP},
    {"fxtract-zero", 0xd9,0xf4, FPN_ZERO,FPN_ONE,0,4,FPN_PAIR},
    {"fyl2x-zero", 0xd9,0xf1, FPN_ZERO,FPN_ONE,0,4,FPN_POP},
    {"fmulp-overflow", 0xde,0xc9, FPN_LARGE,FPN_LARGE,0,8,FPN_POP},
    {"fmulp-underflow", 0xde,0xc9, FPN_MINIMUM,FPN_MINIMUM,0,16,FPN_POP},
    {"faddp-overflow", 0xde,0xc1, FPN_LARGE,FPN_LARGE,0,8,FPN_POP},
    {"fdivp-overflow", 0xde,0xf9, FPN_MINIMUM,FPN_LARGE,0,8,FPN_POP},
    {"fscale-overflow", 0xd9,0xfd, FPN_LARGE,FPN_ONE,0,8,FPN_DEST0},
    {"fscale-underflow", 0xd9,0xfd, FPN_MINIMUM,FPN_NEG_ONE,0,16,FPN_DEST0},
    {"fscale-massive-overflow", 0xd9,0xfd, FPN_ONE,FPN_SCALE_LARGE,0,8,FPN_DEST0},
    {"fistp-precision", 0xdb,0x18, FPN_ONE_HALF,FPN_ONE,0,32,FPN_STORE_POP},
    {"faddp-precision", 0xde,0xc1, FPN_ONE,FPN_HALF_ULP,0,32,FPN_POP},
    {"fsqrt-precision", 0xd9,0xfa, FPN_TWO,FPN_ONE,0,32,FPN_DEST0},
    {"fptan-precision", 0xd9,0xf2, FPN_ONE,FPN_ONE,0,32,FPN_PAIR},
    {"fstp-overflow32", 0xd9,0x18, FPN_LARGE,FPN_ONE,0,8,FPN_STORE_POP},
    {"fstp-underflow32", 0xd9,0x18, FPN_MINIMUM,FPN_ONE,0,16,FPN_STORE_POP},
    {"fstp-snan32", 0xd9,0x18, FPN_SNAN,FPN_ONE,0,1,FPN_STORE_POP},
    {"fstp-precision32", 0xd9,0x18, FPN_ONE_EXTRA,FPN_ONE,0,32,FPN_STORE_POP},
    {"fld-snan80", 0xdb,0x28, FPN_ONE,FPN_ONE,FPN_SNAN80,0,FPN_PUSH},
    {"fld-denormal80", 0xdb,0x28, FPN_ONE,FPN_ONE,FPN_DENORMAL80,0,FPN_PUSH},
    {"fld-register-snan", 0xd9,0xc1, FPN_ONE,FPN_SNAN,0,0,FPN_PUSH},
    {"fstp-register-snan", 0xdd,0xd9, FPN_SNAN,FPN_ONE,0,0,FPN_STORE_REG_POP}
};

#endif
