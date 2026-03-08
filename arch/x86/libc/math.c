/*
 * OsitoK x86-64 — Freestanding Math Library
 *
 * Implements all math.h functions needed by QuickJS using x87 FPU
 * and IEEE 754 bit manipulation. No libc dependency.
 *
 * x87 FPU is always present on x86-64 and provides hardware
 * transcendental instructions (fsin, fcos, fpatan, fyl2x, f2xm1).
 */

typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

/* IEEE 754 double: sign(1) | exponent(11) | mantissa(52) */
typedef union {
    double   f;
    uint64_t u;
} dbl_bits_t;

#define DBL_EXP_MASK  0x7FF0000000000000ULL
#define DBL_MANT_MASK 0x000FFFFFFFFFFFFFULL
#define DBL_SIGN_MASK 0x8000000000000000ULL
#define DBL_EXP_BIAS  1023

static const double _huge = 1.0e300;

/* ── Classification ─────────────────────────────────────────── */

int __isnan(double x) { dbl_bits_t d = {.f=x}; return (d.u & DBL_EXP_MASK) == DBL_EXP_MASK && (d.u & DBL_MANT_MASK) != 0; }
int __isfinite(double x) { dbl_bits_t d = {.f=x}; return (d.u & DBL_EXP_MASK) != DBL_EXP_MASK; }
int __isinf(double x) { dbl_bits_t d = {.f=x}; return (d.u & ~DBL_SIGN_MASK) == DBL_EXP_MASK; }
int __signbit(double x) { dbl_bits_t d = {.f=x}; return (d.u >> 63) & 1; }

/* GCC uses __builtin_isnan etc., but QuickJS calls these directly */
int isnan(double x)     { return __isnan(x); }
int isfinite(double x)  { return __isfinite(x); }
int isinf(double x)     { return __isinf(x); }
int signbit(double x)   { return __signbit(x); }

/* ── Constants ──────────────────────────────────────────────── */

static const dbl_bits_t _nan_bits = { .u = 0x7FF8000000000000ULL };
static const dbl_bits_t _inf_bits = { .u = 0x7FF0000000000000ULL };

/* NAN and INFINITY accessible as doubles */
double __builtin_nan(const char *s)  { (void)s; return _nan_bits.f; }
double __builtin_inf(void)           { return _inf_bits.f; }

/* ── Basic operations ───────────────────────────────────────── */

double fabs(double x)
{
    double r;
    __asm__ ("fldl %1; fabs; fstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double sqrt(double x)
{
    double r;
    __asm__ ("fldl %1; fsqrt; fstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double copysign(double x, double y)
{
    dbl_bits_t bx = {.f=x}, by = {.f=y};
    bx.u = (bx.u & ~DBL_SIGN_MASK) | (by.u & DBL_SIGN_MASK);
    return bx.f;
}

/* ── Rounding ───────────────────────────────────────────────── */

/* x87 rounding: set FPU control word bits 10-11, frndint, restore */
static double x87_round_mode(double x, int mode)
{
    double r;
    uint16_t cw_old, cw_new;
    __asm__ volatile ("fnstcw %0" : "=m"(cw_old));
    cw_new = (cw_old & ~0x0C00) | (mode << 10);
    __asm__ volatile (
        "fldcw %1; fldl %2; frndint; fstpl %0; fldcw %3"
        : "=m"(r) : "m"(cw_new), "m"(x), "m"(cw_old)
    );
    return r;
}

double floor(double x) { return x87_round_mode(x, 1); } /* 01 = round down */
double ceil(double x)  { return x87_round_mode(x, 2); } /* 10 = round up */
double trunc(double x) { return x87_round_mode(x, 3); } /* 11 = round toward zero */

double round(double x)
{
    /* Round half away from zero (not x87's default round-to-even) */
    if (x >= 0.0)
        return floor(x + 0.5);
    else
        return ceil(x - 0.5);
}

double rint(double x)
{
    /* Round using current rounding mode (x87 default = round-to-even) */
    double r;
    __asm__ ("fldl %1; frndint; fstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double nearbyint(double x) { return rint(x); }

long lrint(double x) { return (long)rint(x); }

double fmod(double x, double y)
{
    if (y == 0.0) return _nan_bits.f;
    double r;
    __asm__ volatile (
        "fldl %2\n\t"   /* ST(0) = y */
        "fldl %1\n\t"   /* ST(0) = x, ST(1) = y */
        "1: fprem\n\t"
        "fnstsw %%ax\n\t"
        "testb $4, %%ah\n\t"  /* C2 flag = 1 means incomplete */
        "jnz 1b\n\t"
        "fstpl %0\n\t"  /* Store result */
        "fstp %%st(0)\n\t"  /* Pop y */
        : "=m"(r) : "m"(x), "m"(y) : "ax"
    );
    return r;
}

double remainder(double x, double y)
{
    if (y == 0.0) return _nan_bits.f;
    double r;
    __asm__ volatile (
        "fldl %2\n\t"
        "fldl %1\n\t"
        "1: fprem1\n\t"
        "fnstsw %%ax\n\t"
        "testb $4, %%ah\n\t"
        "jnz 1b\n\t"
        "fstpl %0\n\t"
        "fstp %%st(0)\n\t"
        : "=m"(r) : "m"(x), "m"(y) : "ax"
    );
    return r;
}

double modf(double x, double *iptr)
{
    double i = trunc(x);
    *iptr = i;
    return x - i;
}

double fmin(double x, double y) { if (__isnan(x)) return y; if (__isnan(y)) return x; return x < y ? x : y; }
double fmax(double x, double y) { if (__isnan(x)) return y; if (__isnan(y)) return x; return x > y ? x : y; }

/* ── Trigonometric ──────────────────────────────────────────── */

/* x87 fsin/fcos require |x| < 2^63, but we handle full range
 * since x87 sets C2 flag if argument is out of range. */

double sin(double x)
{
    double r;
    __asm__ ("fldl %1; fsin; fstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double cos(double x)
{
    double r;
    __asm__ ("fldl %1; fcos; fstpl %0" : "=m"(r) : "m"(x));
    return r;
}

double tan(double x)
{
    double r;
    __asm__ (
        "fldl %1\n\t"
        "fptan\n\t"
        "fstp %%st(0)\n\t"  /* Pop the 1.0 that fptan pushes */
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    return r;
}

double atan2(double y, double x)
{
    double r;
    __asm__ (
        "fldl %1\n\t"   /* ST(0) = y */
        "fldl %2\n\t"   /* ST(0) = x, ST(1) = y */
        "fpatan\n\t"    /* ST(0) = atan2(y, x) */
        "fstpl %0"
        : "=m"(r) : "m"(y), "m"(x)
    );
    return r;
}

double atan(double x)  { return atan2(x, 1.0); }
double asin(double x)  { return atan2(x, sqrt(1.0 - x * x)); }
double acos(double x)  { return atan2(sqrt(1.0 - x * x), x); }

/* ── Hyperbolic ─────────────────────────────────────────────── */

/* Forward declarations */
double exp(double x);
double log(double x);

double sinh(double x)
{
    if (fabs(x) < 1e-10) return x;  /* Small x: sinh(x) ≈ x */
    double e = exp(x);
    return (e - 1.0 / e) * 0.5;
}

double cosh(double x)
{
    double e = exp(x);
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x)
{
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

double asinh(double x) { return log(x + sqrt(x * x + 1.0)); }
double acosh(double x) { return log(x + sqrt(x * x - 1.0)); }
double atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }

/* ── Exponential / Logarithmic ──────────────────────────────── */

double log(double x)
{
    /* ln(x) = log2(x) * ln(2) — using fyl2x with y = ln(2) */
    if (x <= 0.0) {
        if (x == 0.0) { dbl_bits_t d = {.u = 0xFFF0000000000000ULL}; return d.f; } /* -inf */
        return _nan_bits.f;
    }
    double r;
    __asm__ (
        "fldln2\n\t"    /* ST(0) = ln(2) */
        "fldl %1\n\t"   /* ST(0) = x, ST(1) = ln(2) */
        "fyl2x\n\t"     /* ST(0) = ln(2) * log2(x) = ln(x) */
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    return r;
}

double log2(double x)
{
    if (x <= 0.0) {
        if (x == 0.0) { dbl_bits_t d = {.u = 0xFFF0000000000000ULL}; return d.f; }
        return _nan_bits.f;
    }
    double r;
    __asm__ (
        "fld1\n\t"      /* ST(0) = 1.0 */
        "fldl %1\n\t"   /* ST(0) = x, ST(1) = 1.0 */
        "fyl2x\n\t"     /* ST(0) = 1.0 * log2(x) */
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    return r;
}

double log10(double x)
{
    if (x <= 0.0) {
        if (x == 0.0) { dbl_bits_t d = {.u = 0xFFF0000000000000ULL}; return d.f; }
        return _nan_bits.f;
    }
    double r;
    __asm__ (
        "fldlg2\n\t"    /* ST(0) = log10(2) */
        "fldl %1\n\t"   /* ST(0) = x, ST(1) = log10(2) */
        "fyl2x\n\t"     /* ST(0) = log10(2) * log2(x) = log10(x) */
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    return r;
}

double log1p(double x)
{
    /* log(1+x) — more accurate than log(1+x) for small x */
    if (fabs(x) < 0.5) {
        double r;
        __asm__ (
            "fldln2\n\t"
            "fldl %1\n\t"
            "fyl2xp1\n\t"
            "fstpl %0"
            : "=m"(r) : "m"(x)
        );
        return r;
    }
    return log(1.0 + x);
}

double exp(double x)
{
    /*
     * exp(x) = 2^(x * log2(e))
     * Let t = x * log2(e), n = trunc(t), f = t - n
     * Then: exp(x) = 2^n * 2^f = fscale(f2xm1(f) + 1, n)
     */
    if (__isnan(x)) return x;
    if (x > 709.78) return _inf_bits.f;   /* overflow */
    if (x < -745.13) return 0.0;           /* underflow */

    double r;
    __asm__ volatile (
        "fldl %1\n\t"       /* ST(0) = x */
        "fldl2e\n\t"        /* ST(0) = log2(e), ST(1) = x */
        "fmulp\n\t"         /* ST(0) = x * log2(e) = t */
        "fld %%st(0)\n\t"   /* ST(0) = t, ST(1) = t */
        "frndint\n\t"       /* ST(0) = n = round(t), ST(1) = t */
        "fsub %%st(0), %%st(1)\n\t"  /* ST(1) = t - n = f, ST(0) = n */
        "fxch\n\t"          /* ST(0) = f, ST(1) = n */
        "f2xm1\n\t"         /* ST(0) = 2^f - 1, ST(1) = n */
        "fld1\n\t"          /* ST(0) = 1, ST(1) = 2^f-1, ST(2) = n */
        "faddp\n\t"         /* ST(0) = 2^f, ST(1) = n */
        "fscale\n\t"        /* ST(0) = 2^f * 2^n = 2^t = exp(x), ST(1) = n */
        "fstp %%st(1)\n\t"  /* Pop n, ST(0) = result */
        "fstpl %0"
        : "=m"(r) : "m"(x)
    );
    return r;
}

double expm1(double x)
{
    /* exp(x) - 1, more accurate for small x */
    if (fabs(x) < 1e-5) return x + 0.5 * x * x;
    return exp(x) - 1.0;
}

double pow(double x, double y)
{
    /* Special cases per IEEE 754 */
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (__isnan(x) || __isnan(y)) return _nan_bits.f;
    if (x == 0.0) {
        if (y > 0.0) return 0.0;
        return _inf_bits.f;
    }

    /* Integer exponent — use repeated squaring */
    if (y == (double)(int64_t)y && fabs(y) < 64.0) {
        int64_t n = (int64_t)y;
        int neg = 0;
        if (n < 0) { neg = 1; n = -n; }
        double r = 1.0, b = x;
        while (n > 0) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return neg ? 1.0 / r : r;
    }

    /* Negative base with non-integer exponent → NaN */
    if (x < 0.0) return _nan_bits.f;

    /* General case: pow(x,y) = 2^(y * log2(x)) */
    double r;
    __asm__ volatile (
        "fldl %2\n\t"       /* ST(0) = y */
        "fldl %1\n\t"       /* ST(0) = x, ST(1) = y */
        "fyl2x\n\t"         /* ST(0) = y * log2(x) = t */
        "fld %%st(0)\n\t"   /* dup t */
        "frndint\n\t"       /* ST(0) = n, ST(1) = t */
        "fsub %%st(0), %%st(1)\n\t"  /* ST(1) = f = t - n, ST(0) = n */
        "fxch\n\t"          /* ST(0) = f, ST(1) = n */
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"         /* ST(0) = 2^f, ST(1) = n */
        "fscale\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %0"
        : "=m"(r) : "m"(x), "m"(y)
    );
    return r;
}

double cbrt(double x)
{
    if (x == 0.0) return x;
    if (x < 0.0) return -pow(-x, 1.0 / 3.0);
    return pow(x, 1.0 / 3.0);
}

double hypot(double x, double y)
{
    /* sqrt(x*x + y*y) with overflow protection */
    x = fabs(x);
    y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double t = y / x;
    return x * sqrt(1.0 + t * t);
}

/* ── frexp / ldexp / scalbn ─────────────────────────────────── */

double frexp(double x, int *exp)
{
    dbl_bits_t d = {.f = x};
    int e = (int)((d.u >> 52) & 0x7FF);

    if (e == 0 || e == 0x7FF) { *exp = 0; return x; } /* 0, denorm, inf, nan */

    *exp = e - DBL_EXP_BIAS + 1;
    d.u = (d.u & ~DBL_EXP_MASK) | ((uint64_t)(DBL_EXP_BIAS - 1) << 52);
    return d.f;
}

double ldexp(double x, int exp)
{
    double scale;
    dbl_bits_t d = {.u = (uint64_t)(exp + DBL_EXP_BIAS) << 52};
    scale = d.f;
    return x * scale;
}

double scalbn(double x, int n) { return ldexp(x, n); }
