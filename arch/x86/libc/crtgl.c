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

/* ── More math: frexpf for nir_lower_flrp ── */

float frexpf(float x, int *exp_out)
{
    if (x == 0.0f) { *exp_out = 0; return 0.0f; }
    /* extract exponent from float bit pattern: bias 127, mantissa 23 bits */
    union { float f; unsigned u; } u; u.f = x;
    int exp = (int)((u.u >> 23) & 0xff) - 126;  /* normalised mantissa is in [0.5,1) → bias-126 */
    u.u = (u.u & 0x807fffffu) | (126u << 23);
    *exp_out = exp;
    return u.f;
}

double frexp(double x, int *exp_out)
{
    if (x == 0.0) { *exp_out = 0; return 0.0; }
    union { double f; unsigned long long u; } u; u.f = x;
    int exp = (int)((u.u >> 52) & 0x7ff) - 1022;
    u.u = (u.u & 0x800fffffffffffffull) | ((unsigned long long)1022 << 52);
    *exp_out = exp;
    return u.f;
}

/* ── llabs ── */
long long llabs(long long x) { return x < 0 ? -x : x; }

/* ── libgcc builtins not provided by minimal libgcc ── */
int __popcountdi2(unsigned long long x)
{
    int c = 0;
    while (x) { c += (int)(x & 1); x >>= 1; }
    return c;
}
int __popcountsi2(unsigned int x)
{
    int c = 0;
    while (x) { c += (int)(x & 1); x >>= 1; }
    return c;
}

/* W4.10++ POSIX file helpers used by mesa/src/util/os_file.c.
 * Previously stubbed; now implemented as real syscall wrappers.
 * Linux x86-64 syscalls: fcntl=72, open=2, getpid=39, getrandom=318. */

#define F_DUPFD_CLOEXEC 1030
#define F_GETFL         3

int os_dupfd_cloexec(int fd)
{
    long r = __syscall6(72, fd, F_DUPFD_CLOEXEC, 0, 0, 0, 0);
    return (int)r;
}

int os_same_file_description(int fd1, int fd2)
{
    if (fd1 == fd2) return 1;
    long f1 = __syscall6(72, fd1, F_GETFL, 0, 0, 0, 0);
    long f2 = __syscall6(72, fd2, F_GETFL, 0, 0, 0, 0);
    if (f1 < 0 || f2 < 0) return -1;
    return (f1 == f2) ? 1 : 0;
}

int os_file_create_unique(const char *prefix, int filemode)
{
    if (!prefix) return -1;
    /* O_CREAT=0x40, O_EXCL=0x80, O_RDWR=0x2, O_CLOEXEC=0x80000 */
    const int flags = 0x40 | 0x80 | 0x2 | 0x80000;
    size_t plen = 0;
    while (prefix[plen]) plen++;
    if (plen > 200) return -1;
    long pid = __syscall6(39, 0, 0, 0, 0, 0, 0);
    char path[256];
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned long rnd = 0;
        __syscall6(318, (long)&rnd, sizeof(rnd), 0, 0, 0, 0);
        size_t i = 0;
        for (size_t j = 0; j < plen && i < sizeof(path) - 32; j++) path[i++] = prefix[j];
        path[i++] = '.';
        char tmp[24]; int t = 0;
        unsigned long v = (unsigned long)pid;
        if (v == 0) tmp[t++] = '0';
        else { while (v) { tmp[t++] = '0' + (v % 10); v /= 10; } }
        while (t--) path[i++] = tmp[t];
        path[i++] = '.';
        for (int s = 60; s >= 0; s -= 4) {
            int nib = (int)((rnd >> s) & 0xf);
            path[i++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
        }
        path[i] = 0;
        long fd = __syscall6(2, (long)path, flags, filemode, 0, 0, 0);
        if (fd >= 0) return (int)fd;
    }
    return -1;
}
