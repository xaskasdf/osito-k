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
