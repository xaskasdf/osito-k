/* mesa_libc_stubs.c — small implementations of libc / C11 thread surface that
 * Mesa src/util/ pulls in but OsitoK libc doesn't ship.
 *
 * Single-threaded by spec for W4.0; the C11 mutex/once stubs are no-ops, just
 * like the pthread ones in mesa_compat.h. If the W4.0 plan ever evolves to a
 * threaded Mesa we'll wire these to real kernel synchronisation primitives. */

#include <stddef.h>

/* ---- string helpers --------------------------------------------------- */

extern size_t strlen(const char *s);
extern void  *malloc(size_t);

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) out[i] = s[i];
    out[len] = 0;
    return out;
}

/* ---- strtoll / strtoull (64-bit versions) ----------------------------- */
/* Minimal conformant parsers, matching strtol but in 64-bit. Accepts
 * optional leading whitespace + sign, optional 0x prefix when base is 0 or
 * 16. Used by tgsi_text.c. */
static int _dgt(int c, int base) {
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    return (d >= 0 && d < base) ? d : -1;
}
unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { if (*p == '-') neg = 1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0 && *p == '0') { p++; base = 8; }
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    int d;
    while ((d = _dgt(*p, base)) >= 0) { v = v * base + (unsigned long long)d; p++; }
    if (end) *end = (char *)p;
    return neg ? (unsigned long long)-(long long)v : v;
}
long long strtoll(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { if (*p == '-') neg = 1; p++; }
    unsigned long long v = strtoull(p, end, base);
    return neg ? -(long long)v : (long long)v;
}

/* ---- vasprintf -------------------------------------------------------- */
/* Used by u_async_debug. We don't have real varargs formatting in a static
 * inline, so call libc vsnprintf twice (sized probe + allocate + render).
 * Declaration matches glibc. */
extern int vsnprintf(char *s, size_t n, const char *fmt, __builtin_va_list ap);
int vasprintf(char **out, const char *fmt, __builtin_va_list ap) {
    __builtin_va_list ap2;
    __builtin_va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    __builtin_va_end(ap2);
    if (n < 0) { *out = NULL; return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { *out = NULL; return -1; }
    /* Second va_copy: ap may be in indeterminate state after the probe
     * (some libc paths consume the original even though we passed ap2). */
    __builtin_va_list ap3;
    __builtin_va_copy(ap3, ap);
    vsnprintf(buf, (size_t)n + 1, fmt, ap3);
    __builtin_va_end(ap3);
    *out = buf;
    return n;
}

/* ---- rand ------------------------------------------------------------ */

static unsigned long _rng_state = 1234567u;
int rand(void) {
    /* xorshift64* — good enough for hash_table_random_entry. */
    _rng_state ^= _rng_state << 13;
    _rng_state ^= _rng_state >> 7;
    _rng_state ^= _rng_state << 17;
    return (int)(_rng_state & 0x7fffffff);
}

/* ---- C11 threads.h surface (no-ops) ---------------------------------- */

typedef int mtx_t;
typedef int once_flag;

int mtx_init   (mtx_t *m, int type)        { (void)m; (void)type; return 0; }
int mtx_lock   (mtx_t *m)                  { (void)m; return 0; }
int mtx_unlock (mtx_t *m)                  { (void)m; return 0; }
int mtx_trylock(mtx_t *m)                  { (void)m; return 0; }
int mtx_destroy(mtx_t *m)                  { (void)m; return 0; }
int mtx_timedlock(mtx_t *m, const void *t) { (void)m; (void)t; return 0; }

void call_once(once_flag *flag, void (*fn)(void)) {
    /* Set flag AFTER fn() so re-entrant calls into call_once with the
     * same flag (Mesa's glsl_type_singleton_init_or_ref does this) don't
     * skip init. Safe because environment is single-threaded. */
    if (*flag == 0) { fn(); *flag = 1; }
}

/* cnd_* family used by util/u_call_once.c via threads.h */
typedef int cnd_t;
int cnd_init     (cnd_t *c)                              { (void)c; return 0; }
int cnd_destroy  (cnd_t *c)                              { (void)c; return 0; }
int cnd_signal   (cnd_t *c)                              { (void)c; return 0; }
int cnd_broadcast(cnd_t *c)                              { (void)c; return 0; }
int cnd_wait     (cnd_t *c, mtx_t *m)                    { (void)c; (void)m; return 0; }
int cnd_timedwait(cnd_t *c, mtx_t *m, const void *ts)    { (void)c; (void)m; (void)ts; return 0; }

/* thrd_* — fail-loud shims; Mesa W4.0 must not spawn. */
typedef int thrd_t;
typedef int (*thrd_start_t)(void *);
int  thrd_create  (thrd_t *t, thrd_start_t f, void *a) { (void)t; (void)f; (void)a; return 1; /* error */ }
int  thrd_join    (thrd_t t,  int *r)                  { (void)t; (void)r; return 1; }
int  thrd_detach  (thrd_t t)                           { (void)t; return 0; }
thrd_t thrd_current(void)                              { return 0; }
int  thrd_equal   (thrd_t a, thrd_t b)                 { return a == b; }
void thrd_yield   (void)                               { }
void thrd_exit    (int code)                           { (void)code; for (;;) ; }
int  thrd_sleep   (const void *t, void *r)             { (void)t; (void)r; return 0; }

/* tss_* */
typedef int tss_t;
int   tss_create (tss_t *t, void (*d)(void *)) { (void)t; (void)d; return 0; }
void  tss_delete (tss_t t)                     { (void)t; }
void *tss_get    (tss_t t)                     { (void)t; return NULL; }
int   tss_set    (tss_t t, void *v)            { (void)t; (void)v; return 0; }

/* ---- W4.2 — math float-suffixed variants needed by NIR codegen ----- */
/* OsitoK libc/math.c provides the double versions; add float wrappers. */
extern double fmin(double, double);
extern double fmax(double, double);
extern double copysign(double, double);
extern double ldexp(double, int);

/* C99/IEEE 754-2008 NaN semantics: fmin(NaN,x)=x, fmin(x,NaN)=x, fmin(NaN,NaN)=NaN.
 * OsitoK's underlying fmin behavior may not honor this; explicit guards. */
float fminf(float a, float b) {
    if (a != a) return b;     /* a is NaN */
    if (b != b) return a;     /* b is NaN */
    return (float)fmin((double)a, (double)b);
}
float fmaxf(float a, float b) {
    if (a != a) return b;
    if (b != b) return a;
    return (float)fmax((double)a, (double)b);
}
float copysignf(float a, float b)        { return (float)copysign((double)a, (double)b); }
float ldexpf(float a, int e)             { return (float)ldexp((double)a, e); }

/* fma (fused multiply-add) — emulate a*b+c (loses fused precision but
 * NIR const-fold doesn't care about ULPs at this level). */
double fma(double a, double b, double c)  { return a * b + c; }
float  fmaf(float a, float b, float c)    { return a * b + c; }

/* isnormal: float, double, long double — return non-zero iff value
 * is finite, non-zero, and not subnormal. We don't model subnormals
 * (OsitoK x87 + SSE is IEEE 754 hardware); flush-to-zero would handle
 * that. */
int __isnormal(double x) {
    /* exponent bits 1..2046 means normal in IEEE 754 binary64 */
    union { double d; unsigned long long u; } v = { x };
    unsigned long long exp = (v.u >> 52) & 0x7ff;
    return exp != 0 && exp != 0x7ff;
}
int __isnormalf(float x) {
    union { float f; unsigned u; } v = { x };
    unsigned exp = (v.u >> 23) & 0xff;
    return exp != 0 && exp != 0xff;
}
/* isnormal is a glibc macro that dispatches by type; nir_constant_expressions.c
 * is auto-generated and calls plain isnormal(x). Provide as variadic so it
 * accepts both float and double. */
int isnormal(double x) { return __isnormal(x); }

/* ---- W4.2 — gallium util fillers --------------------------------------
 * These are referenced by libmesa_util.a (already-built) but were never
 * resolved because no test linked them in W4.0/W4.1. Smoke test in W4.2
 * (mesa-compiler-test) is the first time they show up. */

/* parse_debug_string: walks env-style "flag1,flag2,flag3" and ORs in bits.
 * Mesa's log.c calls it with debug_options[]. We pretend no flags set. */
extern int strcmp(const char *, const char *);
struct debug_named_value;
unsigned long parse_debug_string(const char *debug,
                                 const struct debug_named_value *control) {
    (void)debug; (void)control;
    return 0;
}

/* util_get_process_name: returns argv[0] basename. We have no argv;
 * return a constant string. */
const char *util_get_process_name(void) {
    return "ositok";
}

/* open_memstream: glibc creates a FILE* that grows a malloc'd buffer.
 * OsitoK has no FILE* per se; return NULL so memstream-using Mesa code
 * (mostly debug message capture) falls through to its error path. */
struct _FILE;
struct _FILE *open_memstream(char **bufp, size_t *sizep) {
    (void)bufp; (void)sizep;
    return (struct _FILE *)0;
}

/* os_read_file: Mesa scans /proc/meminfo etc. We have no fs at this
 * layer — return NULL. */
char *os_read_file(const char *filename, size_t *size) {
    (void)filename;
    if (size) *size = 0;
    return (char *)0;
}

/* sscanf: minimal stub, returns 0 (no fields parsed). Mesa only uses
 * this for /proc/meminfo MemAvailable parsing — we already returned
 * NULL above so it never runs. */
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
int sscanf(const char *str, const char *fmt, ...) {
    (void)str; (void)fmt;
    return 0;
}

/* ---- W4.2 — extra stubs surfaced by libmesa_compiler.a -------------- */

/* nir.c uses debug_get_option_cached + debug_parse_flags_option to
 * gate verbose tracing. Return 0 ("no flags set"). */
unsigned long debug_get_option_cached(const char *name, const struct debug_named_value *flags) {
    (void)name; (void)flags;
    return 0;
}
unsigned long debug_parse_flags_option(const char *name, const char *str,
                                       const struct debug_named_value *flags,
                                       unsigned long defval) {
    (void)name; (void)str; (void)flags;
    return defval;
}

/* util_tls_qsort_r: thread-local-storage-based qsort_r. We don't have
 * TLS in this build; provide a minimal non-reentrant qsort using
 * libc's qsort. We don't implement qsort either — use a brain-dead
 * insertion sort for correctness. nir.c only sorts at link time. */
extern void *memcpy(void *, const void *, size_t);
typedef int (*_cmp_t)(const void *, const void *, void *);
void util_tls_qsort_r(void *base, size_t nmemb, size_t size,
                      _cmp_t cmp, void *arg) {
    char *a = (char *)base;
    char tmp[256];  /* element size cap */
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, a + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(a + (j - 1) * size, tmp, arg) > 0) {
            memcpy(a + j * size, a + (j - 1) * size, size);
            j--;
        }
        memcpy(a + j * size, tmp, size);
    }
}

/* comma_separated_list_contains: walks "a,b,c" looking for needle.
 * Used by NIR debug-options gating; return 0 (never matches). */
int comma_separated_list_contains(const char *list, const char *needle) {
    (void)list; (void)needle;
    return 0;
}

/* _mesa_blake3_print: BLAKE3 hash hex-print to FILE*. NIR uses it for
 * shader debug dumps. We don't have FILE*; no-op. */
struct __FILE;
void _mesa_blake3_print(struct __FILE *f, const unsigned char *hash) {
    (void)f; (void)hash;
}

/* exp2 / exp2f — exp(x * ln(2)). OsitoK libc has exp() but not exp2(). */
extern double exp(double);
double exp2(double x) { return exp(x * 0.69314718055994530942); }
float  exp2f(float x) { return (float)exp2((double)x); }
extern double log2(double);
float log2f(float x) { return (float)log2((double)x); }

/* truncf / roundf / floorf / ceilf / sqrtf / fabsf / fmodf — float ops
 * NIR codegen uses. OsitoK libc has the double versions. */
extern double trunc(double);
extern double round(double);
extern double floor(double);
extern double ceil(double);
extern double sqrt(double);
extern double fabs(double);
extern double fmod(double, double);
float truncf(float x) { return (float)trunc((double)x); }
float roundf(float x) { return (float)round((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float sqrtf(float x)  { return (float)sqrt((double)x); }
float fabsf(float x)  { return (float)fabs((double)x); }
float fmodf(float a, float b) { return (float)fmod((double)a, (double)b); }

/* sinf / cosf / tanf / asinf / acosf / atanf / atan2f / expf / logf /
 * powf — common float trig/transcendental that NIR const-fold uses. */
extern double sin(double);
extern double cos(double);
extern double tan(double);
extern double asin(double);
extern double acos(double);
extern double atan(double);
extern double atan2(double, double);
extern double exp(double);
extern double log(double);
extern double pow(double, double);
extern double sinh(double);
extern double cosh(double);
extern double tanh(double);
float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float asinf(float x){ return (float)asin((double)x); }
float acosf(float x){ return (float)acos((double)x); }
float atanf(float x){ return (float)atan((double)x); }
float atan2f(float a, float b) { return (float)atan2((double)a, (double)b); }
float expf(float x) { return (float)exp((double)x); }
float logf(float x) { return (float)log((double)x); }
float powf(float a, float b) { return (float)pow((double)a, (double)b); }
float sinhf(float x){ return (float)sinh((double)x); }
float coshf(float x){ return (float)cosh((double)x); }
float tanhf(float x){ return (float)tanh((double)x); }
