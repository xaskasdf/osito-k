/* W4.8 — extras needed by Mesa/Gallium when linking libGL.a.
 *
 * tcclib.c covers most of what Mesa wants (printf family, malloc, str*,
 * mem*, abs, bsearch, qsort, abort, exit, file IO stubs, __assert_fail,
 * __errno_location).  This file fills the remaining gaps without
 * touching tcclib.c (so /test/tcc.elf and /test/qjs.elf builds stay
 * binary-identical):
 *
 *   __cxa_atexit / __dso_handle      C++ runtime
 *   strcasecmp                       glsl_parser uses for keyword cmp
 *   stpcpy                           Mesa string helpers
 *   strtok / strtok_r                Mesa env parsing
 *   getc / ungetc / asprintf         libc misc
 *   lroundf / llround / rintf        math (float-int rounding)
 *   c23_timespec_get                 c11/impl/time.c reference
 *   syscall                          generic 6-arg gateway (for
 *                                    futex / clock_gettime fallbacks)
 *
 *   operator new / operator delete   C++ allocator: forward to malloc/free
 *   __throw_length_error             stl exception stub (just abort)
 *   _Unwind_Resume                   eh-personality stub (abort)
 *
 * Linked alongside crt.o + tcclib.o + math.o into hello_gl.elf.
 */

#include <stdarg.h>

typedef unsigned long size_t;
typedef long           ssize_t;

extern void *malloc(size_t n);
extern void  free(void *p);
extern int   vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
extern void  abort(void);
extern void  exit(int);
extern long  __syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

/* ── C++ runtime atexit — store in fixed table, invoke on exit() (best
 *    effort; OsitoK exits via raw syscall and won't run them anyway). ── */

#define MAX_ATEXIT 64
static struct { void (*fn)(void *); void *arg; void *dso; } atexit_tbl[MAX_ATEXIT];
static int atexit_n;

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    if (atexit_n >= MAX_ATEXIT) return -1;
    atexit_tbl[atexit_n].fn  = fn;
    atexit_tbl[atexit_n].arg = arg;
    atexit_tbl[atexit_n].dso = dso;
    atexit_n++;
    return 0;
}

void *__dso_handle = (void *)0;

/* ── String helpers ── */

static int tolower_ascii(int c)
{ return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

int strcasecmp(const char *a, const char *b)
{
    for (;;) {
        int ca = tolower_ascii((unsigned char)*a++);
        int cb = tolower_ascii((unsigned char)*b++);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        int ca = tolower_ascii((unsigned char)*a++);
        int cb = tolower_ascii((unsigned char)*b++);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src) != '\0') { dst++; src++; }
    return dst;
}

static char *strtok_state;

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *s = str ? str : *saveptr;
    if (!s) return (char *)0;
    /* skip leading delims */
    while (*s) {
        const char *d = delim;
        int hit = 0;
        while (*d) { if (*s == *d) { hit = 1; break; } d++; }
        if (!hit) break;
        s++;
    }
    if (!*s) { *saveptr = (char *)0; return (char *)0; }
    char *tok = s;
    while (*s) {
        const char *d = delim;
        while (*d) { if (*s == *d) { *s = '\0'; *saveptr = s + 1; return tok; } d++; }
        s++;
    }
    *saveptr = (char *)0;
    return tok;
}

char *strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &strtok_state);
}

/* ── stdio extras ── */

typedef struct FILE FILE;
extern int fgetc(FILE *f);
extern FILE *stdin;

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) { (void)f; return c; }   /* not supported */

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    /* size pass — vsnprintf with size 0 returns required length */
    va_start(ap, fmt);
    int n = vsnprintf((char *)0, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { *out = (char *)0; return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { *out = (char *)0; return -1; }
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    *out = buf;
    return n;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2;
    __builtin_va_copy(ap2, ap);
    int n = vsnprintf((char *)0, 0, fmt, ap2);
    __builtin_va_end(ap2);
    if (n < 0) { *out = (char *)0; return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { *out = (char *)0; return -1; }
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    *out = buf;
    return n;
}

/* ── Math (float→int rounding) ── */

float rintf(float x)
{
    if (x >= 0.0f) return (float)(long)(x + 0.5f);
    return (float)(long)(x - 0.5f);
}
double rint(double x)
{
    if (x >= 0.0) return (double)(long)(x + 0.5);
    return (double)(long)(x - 0.5);
}

long lroundf(float x) { return (long)(x >= 0.0f ? x + 0.5f : x - 0.5f); }
long lround(double x) { return (long)(x >= 0.0  ? x + 0.5  : x - 0.5);  }
long long llroundf(float x) { return (long long)(x >= 0.0f ? x + 0.5f : x - 0.5f); }
long long llround(double x) { return (long long)(x >= 0.0  ? x + 0.5  : x - 0.5);  }

float roundf(float x) { return (float)lroundf(x); }
double round(double x) { return (double)lround(x); }

float truncf(float x) { return (float)(long)x; }
double trunc(double x) { return (double)(long)x; }

float nearbyintf(float x) { return rintf(x); }
double nearbyint(double x) { return rint(x); }

/* exp2f / log2f stubs — Mesa uses for shader math; deliver naive  */
extern double exp2(double); /* in math.c if present */

/* ── Time ── */

struct timespec_ll { long tv_sec; long tv_nsec; };
int c23_timespec_get(struct timespec_ll *ts, int base)
{
    (void)base;
    if (ts) { ts->tv_sec = 0; ts->tv_nsec = 0; }
    return 1;
}
int timespec_get(struct timespec_ll *ts, int base)
{ return c23_timespec_get(ts, base); }

/* ── Generic syscall gateway (Linux x86-64 ABI). ── */

long syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    return __syscall6(nr, a1, a2, a3, a4, a5, a6);
}

/* ── C++ allocator forwards ── */

void *_Znwm(size_t n) { return malloc(n); }                  /* operator new(size_t)        */
void *_Znam(size_t n) { return malloc(n); }                  /* operator new[](size_t)      */
void  _ZdlPv(void *p) { free(p); }                            /* operator delete(void*)      */
void  _ZdaPv(void *p) { free(p); }                            /* operator delete[](void*)    */
void  _ZdlPvm(void *p, size_t n) { (void)n; free(p); }        /* operator delete(void*,sz_t) */
void  _ZdaPvm(void *p, size_t n) { (void)n; free(p); }        /* operator delete[](void*,sz)*/

/* ── C++ exception stubs ── */

void _ZSt20__throw_length_errorPKc(const char *msg) { (void)msg; abort(); }
void _ZSt19__throw_logic_errorPKc(const char *msg)  { (void)msg; abort(); }
void _ZSt17__throw_bad_allocv(void)                 { abort(); }

void _Unwind_Resume(void *exc) { (void)exc; abort(); }

/* ── Misc personality stubs Mesa drags in via libstdc++ ── */

void *__gxx_personality_v0;

/* ── memcmp may be referenced from cso_cache; tcclib provides one ── */
