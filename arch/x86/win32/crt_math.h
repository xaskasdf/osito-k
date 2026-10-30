#ifndef OSITO_WIN32_CRT_MATH_H
#define OSITO_WIN32_CRT_MATH_H
#include "nttypes.h"

double WINAPI crt_frexp(double value, int *exponent);
double WINAPI crt_ldexp(double value, int exponent);
double WINAPI crt_modf(double value, double *integral);
uint64_t WINAPI crt_frexp_compat32(uint64_t low, uint64_t high, uint64_t output);
uint64_t WINAPI crt_ldexp_compat32(uint64_t low, uint64_t high, uint64_t exponent);
uint64_t WINAPI crt_modf_compat32(uint64_t low, uint64_t high, uint64_t output);

#define CRT_DECLARE_INTRINSIC(name) \
    double WINAPI crt_CI##name(double value); \
    uint64_t WINAPI crt_CI##name##_compat32(uint64_t low, uint64_t high)
CRT_DECLARE_INTRINSIC(asin);
CRT_DECLARE_INTRINSIC(atan);
CRT_DECLARE_INTRINSIC(cos);
CRT_DECLARE_INTRINSIC(cosh);
CRT_DECLARE_INTRINSIC(log);
CRT_DECLARE_INTRINSIC(sin);
CRT_DECLARE_INTRINSIC(sinh);
CRT_DECLARE_INTRINSIC(tan);
CRT_DECLARE_INTRINSIC(tanh);
#undef CRT_DECLARE_INTRINSIC
double WINAPI crt_CIatan2(double ordinate, double abscissa);
uint64_t WINAPI crt_CIatan2_compat32(uint64_t ylow, uint64_t yhigh,
                                    uint64_t xlow, uint64_t xhigh);
#define CRT_DECLARE_SCALAR(name) \
    double WINAPI crt_##name(double value); \
    uint64_t WINAPI crt_##name##_compat32(uint64_t low, uint64_t high)
CRT_DECLARE_SCALAR(cos);
CRT_DECLARE_SCALAR(sin);
CRT_DECLARE_SCALAR(acos);
CRT_DECLARE_SCALAR(log10);
CRT_DECLARE_SCALAR(log);
CRT_DECLARE_SCALAR(exp);
CRT_DECLARE_SCALAR(asin);
CRT_DECLARE_SCALAR(atan);
#undef CRT_DECLARE_SCALAR
double WINAPI crt_pow(double base, double exponent);
uint64_t WINAPI crt_pow_compat32(uint64_t low, uint64_t high,
                                uint64_t exponent_low, uint64_t exponent_high);
double WINAPI crt_atan2(double ordinate, double abscissa);
uint64_t WINAPI crt_atan2_compat32(uint64_t ylow, uint64_t yhigh,
                                  uint64_t xlow, uint64_t xhigh);
double WINAPI crt_fmod(double value, double divisor);
uint64_t WINAPI crt_fmod_compat32(uint64_t low, uint64_t high,
                                 uint64_t divisor_low, uint64_t divisor_high);

/* Legacy msvcrt exposes these float entry points in PE64 only. */
#define CRT_DECLARE_FLOAT(name) float WINAPI crt_##name(float value)
CRT_DECLARE_FLOAT(sqrtf);
CRT_DECLARE_FLOAT(sinf);
CRT_DECLARE_FLOAT(cosf);
CRT_DECLARE_FLOAT(asinf);
CRT_DECLARE_FLOAT(acosf);
CRT_DECLARE_FLOAT(logf);
CRT_DECLARE_FLOAT(log10f);
CRT_DECLARE_FLOAT(expf);
CRT_DECLARE_FLOAT(floorf);
CRT_DECLARE_FLOAT(ceilf);
#undef CRT_DECLARE_FLOAT
float WINAPI crt_powf(float base, float exponent);
float WINAPI crt_atan2f(float ordinate, float abscissa);
float WINAPI crt_fmodf(float value, float divisor);
float WINAPI crt_modff(float value, float *integral);

/* Provider identities for PE32 register-argument intrinsics. Their direct
 * adapters spill XMM0/XMM1 before crossing the integer INT2E gateway. */
#define CRT_DECLARE_SSE2(name) \
    double WINAPI crt_sse2_##name(double value); \
    uint64_t WINAPI crt_sse2_##name##_bits(uint64_t low, uint64_t high)
CRT_DECLARE_SSE2(sqrt);
CRT_DECLARE_SSE2(log);
CRT_DECLARE_SSE2(sin);
CRT_DECLARE_SSE2(log10);
CRT_DECLARE_SSE2(cos);
CRT_DECLARE_SSE2(exp);
#undef CRT_DECLARE_SSE2
double WINAPI crt_sse2_pow(double value, double exponent);
uint64_t WINAPI crt_sse2_pow_bits(uint64_t low, uint64_t high,
                                 uint64_t exponent_low, uint64_t exponent_high);
#endif
