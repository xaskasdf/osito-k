/* OsitoK shim — math.h */
#ifndef _MATH_H
#define _MATH_H

#define INFINITY (__builtin_inff())
#define NAN      (__builtin_nanf(""))
#define HUGE_VAL (__builtin_huge_val())
#define M_PI     3.14159265358979323846
#define M_LN2    0.69314718055994530942
#define M_LN10   2.30258509299404568402
#define M_LOG2E  1.44269504088896340736
#define M_LOG10E 0.43429448190325182765
#define M_SQRT2  1.41421356237309504880

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

int isnan(double x);
int isfinite(double x);
int isinf(double x);
int signbit(double x);
int __isnan(double x);
int __isfinite(double x);
int __isinf(double x);
int __signbit(double x);

double fabs(double x);
double sqrt(double x);
double copysign(double x, double y);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long lrint(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double modf(double x, double *iptr);
double fmin(double x, double y);
double fmax(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan2(double y, double x);
double atan(double x);
double asin(double x);
double acos(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double exp(double x);
double expm1(double x);
double pow(double x, double y);
double cbrt(double x);
double hypot(double x, double y);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double scalbn(double x, int n);

/* float versions — redirect to double */
static inline float fabsf(float x) { return (float)fabs(x); }
static inline float sqrtf(float x) { return (float)sqrt(x); }
static inline float floorf(float x) { return (float)floor(x); }
static inline float ceilf(float x) { return (float)ceil(x); }
static inline float truncf(float x) { return (float)trunc(x); }
static inline float roundf(float x) { return (float)round(x); }
static inline float sinf(float x) { return (float)sin(x); }
static inline float cosf(float x) { return (float)cos(x); }
static inline float tanf(float x) { return (float)tan(x); }
static inline float expf(float x) { return (float)exp(x); }
static inline float logf(float x) { return (float)log(x); }
static inline float log2f(float x) { return (float)log2(x); }
static inline float powf(float x, float y) { return (float)pow(x, y); }
static inline float fmodf(float x, float y) { return (float)fmod(x, y); }
static inline float atan2f(float y, float x) { return (float)atan2(y, x); }

/* long double versions — redirect to double */
static inline long double fabsl(long double x) { return (long double)fabs((double)x); }
static inline long double sqrtl(long double x) { return (long double)sqrt((double)x); }
static inline long double floorl(long double x) { return (long double)floor((double)x); }
static inline long double ceill(long double x) { return (long double)ceil((double)x); }
static inline long double truncl(long double x) { return (long double)trunc((double)x); }
static inline long double roundl(long double x) { return (long double)round((double)x); }
static inline long double logl(long double x) { return (long double)log((double)x); }
static inline long double expl(long double x) { return (long double)exp((double)x); }
static inline long double log2l(long double x) { return (long double)log2((double)x); }
long double ldexpl(long double x, int exp);

/* Macros that QuickJS uses — redirect to functions */
#define isnan(x) __isnan(x)
#define isfinite(x) __isfinite(x)
#define isinf(x) __isinf(x)
#define signbit(x) __signbit(x)

#endif
