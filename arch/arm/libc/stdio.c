/*
 * stdio.c -- formatted output, the console and read-only files
 * (Osito-K AArch64 libc)
 *
 * stdout is line-buffered (it reaches the USB console and the screen on
 * each '\n', or with fflush), stderr unbuffered. Files come from OsitoFS
 * through open/read, read-only for now.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct _FILE {
    int  fd;
    int  eof, err;
    int  unbuffered;
    int  len;                   /* bytes waiting in buf (output) */
    char buf[BUFSIZ];
};

static FILE files[3] = {
    { .fd = 0 }, { .fd = 1 }, { .fd = 2, .unbuffered = 1 },
};
FILE *stdin = &files[0], *stdout = &files[1], *stderr = &files[2];

/* ── Output ─────────────────────────────────────────────────── */

int fflush(FILE *f)
{
    if (!f) {
        fflush(stdout);
        return fflush(stderr);
    }
    int off = 0;
    while (off < f->len) {
        ssize_t n = write(f->fd, f->buf + off, (size_t)(f->len - off));
        if (n <= 0) {
            f->err = 1;
            f->len = 0;
            return EOF;
        }
        off += (int)n;
    }
    f->len = 0;
    return 0;
}

static void out(FILE *f, const char *p, size_t n)
{
    if (f->unbuffered) {
        write(f->fd, p, n);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        f->buf[f->len++] = p[i];
        if (f->len == BUFSIZ || p[i] == '\n')
            fflush(f);
    }
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    out(f, &ch, 1);
    return (unsigned char)c;
}

int putc(int c, FILE *f)  { return fputc(c, f); }
int putchar(int c)        { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    out(f, s, strlen(s));
    return 0;
}

int puts(const char *s)
{
    fputs(s, stdout);
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    out(f, p, size * n);
    return n;
}

/* ── The formatter ──────────────────────────────────────────────
 * Writes through a sink: a FILE, or a buffer that keeps what fits. */

typedef struct {
    FILE  *f;
    char  *buf;
    size_t cap, len;            /* len counts everything, as printf returns */
} sink_t;

static void emit(sink_t *s, const char *p, size_t n)
{
    if (s->f) {
        out(s->f, p, n);
    } else {
        for (size_t i = 0; i < n; i++)
            if (s->len + i + 1 < s->cap)
                s->buf[s->len + i] = p[i];
    }
    s->len += n;
}

static void emit_pad(sink_t *s, char c, int n)
{
    for (; n > 0; n--)
        emit(s, &c, 1);
}

enum { F_LEFT = 1, F_ZERO = 2, F_PLUS = 4, F_SPACE = 8, F_ALT = 16 };

/* A number's digits (and sign/prefix) padded to width */
static void emit_field(sink_t *s, const char *prefix, const char *digits, int ndig,
                       int prec, int width, int flags)
{
    int zeros = prec > ndig ? prec - ndig : 0;
    int plen = (int)strlen(prefix);
    int total = plen + zeros + ndig;
    int pad = width > total ? width - total : 0;
    if ((flags & F_ZERO) && !(flags & F_LEFT) && prec < 0) {
        zeros += pad;
        pad = 0;
    }
    if (!(flags & F_LEFT))
        emit_pad(s, ' ', pad);
    emit(s, prefix, (size_t)plen);
    emit_pad(s, '0', zeros);
    emit(s, digits, (size_t)ndig);
    if (flags & F_LEFT)
        emit_pad(s, ' ', pad);
}

static int utoa(unsigned long long v, unsigned base, int upper, char *end)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        *--end = dig[v % base];
        v /= base;
        n++;
    } while (v);
    return n;
}

/* %f / %e / %g: enough for program output, not correctly rounded in every
 * last digit */
static void emit_float(sink_t *s, double v, char conv, int prec, int width, int flags)
{
    char buf[400];
    int n = 0;
    const char *sign = "";
    if (v < 0 || (v == 0 && 1 / v < 0)) {
        sign = "-";
        v = -v;
    } else if (flags & F_PLUS) {
        sign = "+";
    } else if (flags & F_SPACE) {
        sign = " ";
    }
    if (v != v || v > 1.7976931348623157e308) {
        const char *word = v != v ? "nan" : "inf";
        emit_field(s, sign, word, 3, -1, width, flags & ~F_ZERO);
        return;
    }
    if (prec < 0)
        prec = 6;
    if (prec > 30)
        prec = 30;
    int upper = conv == 'E' || conv == 'G' || conv == 'F';
    char kind = (char)(conv | 32);
    int exp10 = 0;
    if (kind != 'f' && v != 0) {
        while (v >= 10) { v /= 10; exp10++; }
        while (v < 1)   { v *= 10; exp10--; }
    }
    if (kind == 'g') {
        /* P significant digits: exponent style outside [-4, P) */
        int p = prec ? prec : 1, x = exp10;
        if (x < -4 || x >= p) {
            kind = 'e';
            prec = p - 1;
        } else {
            for (; exp10 > 0; exp10--) v *= 10;
            for (; exp10 < 0; exp10++) v /= 10;
            kind = 'f';
            prec = p - 1 - x;
        }
    }
    /* Round to prec decimals */
    double r = 0.5;
    for (int i = 0; i < prec; i++)
        r /= 10;
    v += r;
    if (kind == 'e' && v >= 10) {
        v /= 10;
        exp10++;
    }
    /* Integer part */
    double ip = 1;
    while (ip * 10 <= v)
        ip *= 10;
    for (; ip >= 1 && n < 330; ip /= 10) {
        int d = (int)(v / ip);
        if (d > 9) d = 9;
        buf[n++] = (char)('0' + d);
        v -= d * ip;
    }
    if (prec || (flags & F_ALT))
        buf[n++] = '.';
    for (int i = 0; i < prec; i++) {
        v *= 10;
        int d = (int)v;
        if (d > 9) d = 9;
        buf[n++] = (char)('0' + d);
        v -= d;
    }
    if ((conv | 32) == 'g' && !(flags & F_ALT)) {
        int dot = 0;
        for (int i = 0; i < n; i++)
            dot |= buf[i] == '.';
        while (dot && n > 1 && buf[n - 1] == '0')
            n--;
        if (n > 1 && buf[n - 1] == '.')
            n--;
    }
    if (kind == 'e') {
        buf[n++] = upper ? 'E' : 'e';
        buf[n++] = exp10 < 0 ? '-' : '+';
        int e = exp10 < 0 ? -exp10 : exp10;
        if (e >= 100) buf[n++] = (char)('0' + e / 100);
        buf[n++] = (char)('0' + e / 10 % 10);
        buf[n++] = (char)('0' + e % 10);
    }
    emit_field(s, sign, buf, n, -1, width, flags);
}

static void format(sink_t *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            const char *run = fmt;
            while (fmt[1] && fmt[1] != '%')
                fmt++;
            emit(s, run, (size_t)(fmt - run + 1));
            continue;
        }
        fmt++;
        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '0') flags |= F_ZERO;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else break;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int len = 0;                    /* 0 int, -1 short, -2 char, 1 long, 2 long long */
        for (;; fmt++) {
            if (*fmt == 'l') len = len == 1 ? 2 : 1;
            else if (*fmt == 'h') len = len == -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') len = 1;
            else break;
        }
        char digits[24], *end = digits + sizeof(digits);
        char conv = *fmt;
        switch (conv) {
        case 'd': case 'i': {
            long long v = len >= 2 ? va_arg(ap, long long) : len == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (len == -1) v = (short)v;
            if (len == -2) v = (signed char)v;
            const char *sign = v < 0 ? "-" : flags & F_PLUS ? "+" : flags & F_SPACE ? " " : "";
            unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            int n = prec == 0 && u == 0 ? 0 : utoa(u, 10, 0, end);
            emit_field(s, sign, end - n, n, prec, width, flags);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long u = len >= 2 ? va_arg(ap, unsigned long long) :
                                   len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (len == -1) u = (unsigned short)u;
            if (len == -2) u = (unsigned char)u;
            unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            const char *prefix = (flags & F_ALT) && u ? (conv == 'x' ? "0x" : conv == 'X' ? "0X" :
                                                         conv == 'o' ? "0" : "") : "";
            int n = prec == 0 && u == 0 ? 0 : utoa(u, base, conv == 'X', end);
            emit_field(s, prefix, end - n, n, prec, width, flags);
            break;
        }
        case 'p': {
            uintptr_t u = (uintptr_t)va_arg(ap, void *);
            int n = utoa(u, 16, 0, end);
            emit_field(s, "0x", end - n, n, -1, width, flags);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit_field(s, "", &c, 1, -1, width, flags & ~F_ZERO);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            int n = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
            emit_field(s, "", str, n, -1, width, flags & ~F_ZERO);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            emit_float(s, va_arg(ap, double), conv, prec, width, flags);
            break;
        case '%':
            emit(s, "%", 1);
            break;
        case 0:
            return;
        default:                        /* unknown: print it as written */
            emit(s, "%", 1);
            emit(s, &conv, 1);
            break;
        }
    }
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    sink_t s = { .f = f };
    format(&s, fmt, ap);
    return (int)s.len;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    sink_t s = { .buf = buf, .cap = size };
    format(&s, fmt, ap);
    if (size)
        buf[s.len < size ? s.len : size - 1] = 0;
    return (int)s.len;
}

int vsprintf(char *buf, const char *fmt, va_list ap)  { return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap); }
int vprintf(const char *fmt, va_list ap)              { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

/* ── Input ──────────────────────────────────────────────────── */

typedef struct {
    FILE file;
    char in[BUFSIZ];            /* read-ahead */
    int  pos, have;
} rfile_t;

static rfile_t console_in = { .file = { .fd = 0 } };

static rfile_t *reader(FILE *f)
{
    return f == stdin ? &console_in : (rfile_t *)f;
}

int fgetc(FILE *f)
{
    rfile_t *r = reader(f);
    if (r->pos == r->have) {
        if (f == stdin)
            fflush(stdout);             /* a prompt shows before waiting */
        ssize_t n = read(r->file.fd, r->in, sizeof(r->in));
        if (n <= 0) {
            f->eof = n == 0;
            f->err = n < 0;
            return EOF;
        }
        r->pos = 0;
        r->have = (int)n;
    }
    return (unsigned char)r->in[r->pos++];
}

int getc(FILE *f)   { return fgetc(f); }
int getchar(void)   { return fgetc(stdin); }

char *fgets(char *s, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (!i)
        return 0;
    s[i] = 0;
    return s;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    uint8_t *d = p;
    size_t want = size * n, got = 0;
    rfile_t *r = reader(f);
    while (got < want && r->pos < r->have)          /* what was read ahead */
        d[got++] = (uint8_t)r->in[r->pos++];
    while (got < want) {
        ssize_t k = read(f->fd, d + got, want - got);
        if (k <= 0) {
            f->eof = k == 0;
            f->err = k < 0;
            break;
        }
        got += (size_t)k;
    }
    return size ? got / size : 0;
}

FILE *fopen(const char *path, const char *mode)
{
    if (mode[0] != 'r' || strchr(mode, '+')) {
        errno = EACCES;                 /* files are read-only for now */
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    rfile_t *r = calloc(1, sizeof(*r));
    if (!r) {
        close(fd);
        return 0;
    }
    r->file.fd = fd;
    return &r->file;
}

int fclose(FILE *f)
{
    if (f == stdin || f == stdout || f == stderr)
        return fflush(f);
    int rc = close(f->fd);
    free(f);
    return rc;
}

int fseek(FILE *f, long off, int whence)
{
    rfile_t *r = reader(f);
    if (whence == SEEK_CUR)
        off -= r->have - r->pos;        /* what was read ahead isn't consumed */
    if (lseek(f->fd, off, whence) < 0)
        return -1;
    r->pos = r->have = 0;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f)
{
    rfile_t *r = reader(f);
    long at = lseek(f->fd, 0, SEEK_CUR);
    return at < 0 ? -1 : at - (r->have - r->pos);
}

void rewind(FILE *f)    { fseek(f, 0, SEEK_SET); f->err = 0; }
int feof(FILE *f)       { return f->eof; }
int ferror(FILE *f)     { return f->err; }
