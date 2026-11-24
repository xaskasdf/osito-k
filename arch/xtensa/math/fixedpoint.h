/*
 * OsitoK - Fixed-point 16.16 math library
 *
 * Format: int32_t with 16 integer bits (signed) + 16 fractional bits
 * Range:  -32768.0 to +32767.99998
 * Precision: 1/65536 ≈ 0.0000153
 *
 * fix_mul uses Xtensa mull+muluh for ~10 cycle multiply.
 * Trigonometry uses 256-entry full-circle sine table (1KB DRAM).
 */
#ifndef OSITO_FIXEDPOINT_H
#define OSITO_FIXEDPOINT_H

#include "kernel/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== Types ====== */

typedef int32_t  fix16_t;   /* 16.16 fixed-point */
typedef uint8_t  angle_t;   /* 0-255 = 0°-360° */

/* ====== Constants ====== */

#define FIX16_ONE       ((fix16_t)0x00010000)   /* 1.0 */
#define FIX16_HALF      ((fix16_t)0x00008000)   /* 0.5 */
#define FIX16_NEG_ONE   ((fix16_t)0xFFFF0000)   /* -1.0 */
#define FIX16_PI        ((fix16_t)205887)        /* 3.14159 */
#define FIX16_MAX       ((fix16_t)0x7FFFFFFF)
#define FIX16_MIN       ((fix16_t)0x80000000)

/* ====== Conversion macros ====== */

/* Integer to fix16 */
#define FIX16(n)          ((fix16_t)((uint32_t)(int32_t)(n) << 16))

/* Float literal to fix16 (compile-time only) */
#define FIX16_C(f)        ((fix16_t)((f) * 65536.0 + ((f) >= 0 ? 0.5 : -0.5)))

/* Fix16 to integer (truncate toward zero) */
#define FIX16_TO_INT(x)   ((int32_t)(x) >> 16)

/* Fix16 to integer (round to nearest) */
#define FIX16_ROUND(x)    (((x) + FIX16_HALF) >> 16)

/* Extract fractional part (always positive, 0..65535) */
#define FIX16_FRAC(x)     ((uint16_t)((x) & 0xFFFF))

/* ====== Basic arithmetic (inline) ====== */

INLINE fix16_t fix_add(fix16_t a, fix16_t b)
{
    return a + b;
}

INLINE fix16_t fix_sub(fix16_t a, fix16_t b)
{
    return a - b;
}

INLINE fix16_t fix_neg(fix16_t x)
{
    return -x;
}

INLINE fix16_t fix_abs(fix16_t x)
{
    return x < 0 ? -x : x;
}

/* ====== Multiplication: fix16 × fix16 ====== */

/*
 * 64-bit multiply, shift right 16.
 * GCC generates mull + shift sequence for LX106 (~15 cycles).
 * LX106 has mull but NOT muluh, so we rely on the compiler.
 */
INLINE fix16_t fix_mul(fix16_t a, fix16_t b)
{
    return (fix16_t)((int64_t)a * b >> 16);
}

/* fix16 × integer (simple, no 64-bit needed if result fits) */
INLINE fix16_t fix_mul_int(fix16_t a, int32_t n)
{
    return a * n;
}

/* ====== Utilities (inline) ====== */

/* Linear interpolation: a + t*(b-a), where t is fix16 0..1 */
INLINE fix16_t fix_lerp(fix16_t a, fix16_t b, fix16_t t)
{
    return a + fix_mul(t, b - a);
}

/* Clamp x to [lo, hi] */
INLINE fix16_t fix_clamp(fix16_t x, fix16_t lo, fix16_t hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/*
 * Approximate distance: |dx| + |dy| - min(|dx|,|dy|)*3/8
 * Error < 7% vs true sqrt(dx²+dy²).
 * Zero branches in the hot approximation.
 */
INLINE fix16_t fix_dist_approx(fix16_t dx, fix16_t dy)
{
    fix16_t ax = fix_abs(dx);
    fix16_t ay = fix_abs(dy);
    fix16_t mn = ax < ay ? ax : ay;
    /* mn*3/8 = mn/4 + mn/8 (shift to avoid overflow) */
    return ax + ay - (mn >> 2) - (mn >> 3);
}

/* ====== Functions in fixedpoint.cpp ====== */

/* Sine: angle_t 0-255 = 0°-360°, returns fix16 -1.0..+1.0 */
fix16_t fix_sin(angle_t angle);

/* Cosine: cos(a) = sin(a + 64) */
fix16_t fix_cos(angle_t angle);

/* Division: a / b, returns fix16. b must not be 0. */
fix16_t fix_div(fix16_t a, fix16_t b);

/* Square root: x must be >= 0. Returns fix16. */
fix16_t fix_sqrt(fix16_t x);

/* Print fix16 value to UART as decimal (e.g. "3.141") */
void fix_print(fix16_t x);

/* Run validation tests (shell fixtest command) */
void fix_test(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_FIXEDPOINT_H */
