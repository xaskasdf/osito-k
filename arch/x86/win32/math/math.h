/* Osito-K freestanding double-only API for the vendored musl subset.
 * Namespace every definition so none overrides host or kernel math symbols. */
#ifndef OSITO_CRT_LIBM_API_H
#define OSITO_CRT_LIBM_API_H
#include <stdint.h>
#include <float.h>
#define hidden __attribute__((visibility("hidden")))
typedef double double_t;
typedef float float_t;
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isnan(x) __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define sin osito_libm_sin
#define cos osito_libm_cos
#define tan osito_libm_tan
#define acos osito_libm_acos
#define pow osito_libm_pow
#define log10 osito_libm_log10
#define asin osito_libm_asin
#define atan osito_libm_atan
#define atan2 osito_libm_atan2
#define sinh osito_libm_sinh
#define cosh osito_libm_cosh
#define tanh osito_libm_tanh
#define log osito_libm_log
#define exp osito_libm_exp
#define expm1 osito_libm_expm1
#define frexp osito_libm_frexp
#define modf osito_libm_modf
#define ldexp osito_libm_ldexp
#define scalbn osito_libm_scalbn
#define floor osito_libm_floor
#define fabs osito_libm_fabs
#define sqrt osito_libm_sqrt
#define __sin osito_libm___sin
#define __cos osito_libm___cos
#define __tan osito_libm___tan
#define __rem_pio2 osito_libm___rem_pio2
#define __rem_pio2_large osito_libm___rem_pio2_large
#define __expo2 osito_libm___expo2
#define __math_divzero osito_libm___math_divzero
#define __math_invalid osito_libm___math_invalid
#define __math_oflow osito_libm___math_oflow
#define __math_uflow osito_libm___math_uflow
#define __math_xflow osito_libm___math_xflow
#define __exp_data osito_libm___exp_data
#define __log_data osito_libm___log_data
#define __pow_log_data osito_libm___pow_log_data
#define __rsqrt_tab osito_libm___rsqrt_tab
double sin(double);
double cos(double);
double tan(double);
double acos(double);
double pow(double,double);
double log10(double);
double asin(double);
double atan(double);
double sinh(double);
double cosh(double);
double tanh(double);
double log(double);
double exp(double);
double expm1(double);
double floor(double);
double fabs(double);
double sqrt(double);
double atan2(double,double);
double frexp(double,int *);
double modf(double,double *);
double ldexp(double,int);
double scalbn(double,int);
#endif
