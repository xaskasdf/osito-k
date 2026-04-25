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
    if (*flag == 0) { *flag = 1; fn(); }
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
