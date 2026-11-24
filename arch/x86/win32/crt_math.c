/* Real double-precision CRT operations with explicit PE32 argument packing.
 * compat32.c consumes x87 arguments before entering the INT2E gateway. */
#include "crt_math.h"
#include "msvcrt_shim.h"
#include "compat32.h"
#include "math/math.h"

typedef union { double value; uint64_t bits; } CRT_MATH_VALUE;

static double crt_math_argument(uint64_t low, uint64_t high)
{
    CRT_MATH_VALUE value = { .bits = (uint32_t)low | ((uint64_t)(uint32_t)high << 32) };
    return value.value;
}

static uint64_t crt_math_result(double value)
{
    CRT_MATH_VALUE result = { .value = value };
    return result.bits;
}

double WINAPI crt_frexp(double value, int *exponent)
{
    if (!exponent) { *crt_errno() = 22; return NAN; }
    *exponent = 0;
    if (value == 0.0) return 0.0;
    if (!isfinite(value)) {
        *exponent = -1;
        return value - value;
    }
    return osito_libm_frexp(value, exponent);
}

double WINAPI crt_ldexp(double value, int exponent)
{
    double result = osito_libm_ldexp(value, exponent);
    if (isfinite(value) && value != 0.0 &&
        (!isfinite(result) || result == 0.0))
        *crt_errno() = 34;
    return result;
}

double WINAPI crt_modf(double value, double *integral)
{
    if (!integral) { *crt_errno() = 22; return NAN; }
    double result = osito_libm_modf(value, integral);
    /* Legacy x64 msvcrt returns positive zero for a nonzero finite integer;
     * the x86 export preserves the input sign. Both preserve -0 and -Inf. */
    if (!g_compat32_mode && result == 0.0 && value != 0.0 && isfinite(value))
        return 0.0;
    return result;
}

uint64_t WINAPI crt_frexp_compat32(uint64_t low, uint64_t high, uint64_t output)
{
    return crt_math_result(crt_frexp(crt_math_argument(low, high),
                                    (int *)(uintptr_t)(uint32_t)output));
}

uint64_t WINAPI crt_ldexp_compat32(uint64_t low, uint64_t high, uint64_t exponent)
{
    return crt_math_result(crt_ldexp(crt_math_argument(low, high), (int32_t)exponent));
}

uint64_t WINAPI crt_modf_compat32(uint64_t low, uint64_t high, uint64_t output)
{
    return crt_math_result(crt_modf(crt_math_argument(low, high),
                                   (double *)(uintptr_t)(uint32_t)output));
}

#define CRT_UNARY_INTRINSIC(name) \
double WINAPI crt_CI##name(double value) \
{ \
    double result = osito_libm_##name(value); \
    if (!isnan(value) && isnan(result)) *crt_errno() = 33; \
    else if (isfinite(value) && !isfinite(result)) *crt_errno() = 34; \
    return result; \
} \
uint64_t WINAPI crt_CI##name##_compat32(uint64_t low, uint64_t high) \
{ \
    return crt_math_result(crt_CI##name(crt_math_argument(low, high))); \
}
CRT_UNARY_INTRINSIC(asin)
CRT_UNARY_INTRINSIC(atan)
CRT_UNARY_INTRINSIC(cos)
CRT_UNARY_INTRINSIC(cosh)
CRT_UNARY_INTRINSIC(log)
CRT_UNARY_INTRINSIC(sin)
CRT_UNARY_INTRINSIC(sinh)
CRT_UNARY_INTRINSIC(tan)
CRT_UNARY_INTRINSIC(tanh)
#undef CRT_UNARY_INTRINSIC

double WINAPI crt_CIatan2(double ordinate, double abscissa)
{
    return osito_libm_atan2(ordinate, abscissa);
}

uint64_t WINAPI crt_CIatan2_compat32(uint64_t ylow, uint64_t yhigh,
                                    uint64_t xlow, uint64_t xhigh)
{
    return crt_math_result(crt_CIatan2(crt_math_argument(ylow, yhigh),
                                      crt_math_argument(xlow, xhigh)));
}

/* Ordinary C exports receive stack doubles in PE32, unlike the _CI entry
 * points. Keep separate provider addresses so the two direct ABI bindings
 * cannot alias. Legacy msvcrt reports EDOM for NaN results, including NaN
 * inputs; its measured underflow behavior preserves errno. */
#define CRT_SCALAR_UNARY(name) \
double WINAPI crt_##name(double value) \
{ \
    double result = osito_libm_##name(value); \
    if (isnan(result)) *crt_errno() = 33; \
    else if (isfinite(value) && !isfinite(result)) *crt_errno() = 34; \
    return result; \
} \
uint64_t WINAPI crt_##name##_compat32(uint64_t low, uint64_t high) \
{ \
    return crt_math_result(crt_##name(crt_math_argument(low, high))); \
}
CRT_SCALAR_UNARY(cos)
CRT_SCALAR_UNARY(sin)
CRT_SCALAR_UNARY(acos)
CRT_SCALAR_UNARY(log10)
CRT_SCALAR_UNARY(log)
CRT_SCALAR_UNARY(exp)
CRT_SCALAR_UNARY(asin)
CRT_SCALAR_UNARY(atan)
#undef CRT_SCALAR_UNARY

double WINAPI crt_pow(double base, double exponent)
{
    double result = osito_libm_pow(base, exponent);
    if (isnan(result)) *crt_errno() = 33;
    else if (isfinite(base) && isfinite(exponent) && !isfinite(result)) *crt_errno() = 34;
    return result;
}

uint64_t WINAPI crt_pow_compat32(uint64_t low, uint64_t high,
                                uint64_t exponent_low, uint64_t exponent_high)
{
    return crt_math_result(crt_pow(crt_math_argument(low, high),
                                   crt_math_argument(exponent_low, exponent_high)));
}

double WINAPI crt_atan2(double ordinate, double abscissa)
{
    double result = osito_libm_atan2(ordinate, abscissa);
    if (isnan(result)) *crt_errno() = 33;
    return result;
}

uint64_t WINAPI crt_atan2_compat32(uint64_t ylow, uint64_t yhigh,
                                  uint64_t xlow, uint64_t xhigh)
{
    return crt_math_result(crt_atan2(crt_math_argument(ylow, yhigh),
                                    crt_math_argument(xlow, xhigh)));
}

double WINAPI crt_fmod(double value, double divisor)
{
    double result = crt_CIfmod(value, divisor);
    if (isnan(result)) *crt_errno() = 33;
    /* The legacy x86 scalar export loses the sign of an input zero; its
     * nonzero negative integral remainders and the x64 export preserve it. */
    if (g_compat32_mode && value == 0.0 && !isnan(result)) return 0.0;
    return result;
}

uint64_t WINAPI crt_fmod_compat32(uint64_t low, uint64_t high,
                                 uint64_t divisor_low, uint64_t divisor_high)
{
    return crt_math_result(crt_fmod(crt_math_argument(low, high),
                                   crt_math_argument(divisor_low, divisor_high)));
}

/* Compute in the existing double libm and round to the actual float return
 * type before overflow classification. This also keeps subnormal inputs out
 * of approximate or integer-only replacements. */
#define CRT_FLOAT_UNARY(name, implementation) \
float WINAPI crt_##name(float value) \
{ \
    float result = (float)implementation((double)value); \
    if (isnan(result)) *crt_errno() = 33; \
    else if (isfinite(value) && !isfinite(result)) *crt_errno() = 34; \
    return result; \
}
CRT_FLOAT_UNARY(sqrtf, osito_libm_sqrt)
CRT_FLOAT_UNARY(sinf, osito_libm_sin)
CRT_FLOAT_UNARY(cosf, osito_libm_cos)
CRT_FLOAT_UNARY(asinf, osito_libm_asin)
CRT_FLOAT_UNARY(acosf, osito_libm_acos)
CRT_FLOAT_UNARY(logf, osito_libm_log)
CRT_FLOAT_UNARY(log10f, osito_libm_log10)
CRT_FLOAT_UNARY(expf, osito_libm_exp)
CRT_FLOAT_UNARY(floorf, osito_libm_floor)
CRT_FLOAT_UNARY(ceilf, crt_ceil)
#undef CRT_FLOAT_UNARY

#define CRT_FLOAT_BINARY(name, implementation) \
float WINAPI crt_##name(float left, float right) \
{ \
    float result = (float)implementation((double)left, (double)right); \
    if (isnan(result)) *crt_errno() = 33; \
    else if (isfinite(left) && isfinite(right) && !isfinite(result)) *crt_errno() = 34; \
    return result; \
}
CRT_FLOAT_BINARY(powf, osito_libm_pow)
CRT_FLOAT_BINARY(atan2f, osito_libm_atan2)
CRT_FLOAT_BINARY(fmodf, crt_CIfmod)
#undef CRT_FLOAT_BINARY

float WINAPI crt_modff(float value, float *integral)
{
    if (!integral) { *crt_errno() = 22; return NAN; }
    double whole;
    float fraction = (float)crt_modf((double)value, &whole);
    *integral = (float)whole;
    return fraction;
}

/* Keep separate provider addresses: the ordinary exports return through
 * ST(0) in PE32, while these internal CRT functions return through XMM0.
 * Unlike the other precise entries, SQRT is an SSE operation and does not
 * report domain errors through errno, as measured on the host CRT. */
#define CRT_SSE2_UNARY(name, implementation) \
double WINAPI crt_sse2_##name(double value) { return implementation(value); } \
uint64_t WINAPI crt_sse2_##name##_bits(uint64_t low, uint64_t high) \
{ return crt_math_result(crt_sse2_##name(crt_math_argument(low, high))); }
CRT_SSE2_UNARY(sqrt, osito_libm_sqrt)
CRT_SSE2_UNARY(log, crt_log)
CRT_SSE2_UNARY(sin, crt_sin)
CRT_SSE2_UNARY(log10, crt_log10)
CRT_SSE2_UNARY(cos, crt_cos)
CRT_SSE2_UNARY(exp, crt_exp)
#undef CRT_SSE2_UNARY
double WINAPI crt_sse2_pow(double value, double exponent)
{ return crt_pow(value, exponent); }
uint64_t WINAPI crt_sse2_pow_bits(uint64_t low, uint64_t high,
                                 uint64_t exponent_low, uint64_t exponent_high)
{ return crt_pow_compat32(low, high, exponent_low, exponent_high); }
