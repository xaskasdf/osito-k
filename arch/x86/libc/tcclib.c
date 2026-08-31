/*
 * tcclib.c — Extended libc for running TCC inside OsitoK.
 *
 * Provides FILE* I/O, formatted output, string/number conversion,
 * qsort, setjmp stubs, and OS stubs that TCC needs but our minimal
 * crt.c doesn't provide.
 *
 * Many functions are stubs (signals, threads, mprotect, etc.) since
 * OsitoK doesn't support those features. They return safe defaults.
 */

/* ── Types ── */

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef long time_t;
typedef int mode_t;
typedef int pid_t;

/* va_list — same as crt.c */
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg
#define va_copy  __builtin_va_copy

#define NULL ((void *)0)
#define EOF  (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ── Syscall imports from crt.c / syscall.S ── */

extern long __syscall1(long nr, long a1);
extern long __syscall2(long nr, long a1, long a2);
extern long __syscall3(long nr, long a1, long a2, long a3);

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_lseek   8
#define SYS_brk     12
#define SYS_exit    60
#define SYS_unlink  87
#define SYS_access  21

/* Wrappers — duplicated from crt.c because we're freestanding */
static ssize_t _write(int fd, const void *buf, size_t n)
{ return __syscall3(SYS_write, fd, (long)buf, (long)n); }

static ssize_t _read(int fd, void *buf, size_t n)
{ return __syscall3(SYS_read, fd, (long)buf, (long)n); }

static int _open(const char *path, int flags, int mode)
{ return (int)__syscall3(SYS_open, (long)path, flags, mode); }

static int _close(int fd)
{ return (int)__syscall1(SYS_close, fd); }

static long _lseek(int fd, long off, int whence)
{ return __syscall3(SYS_lseek, fd, off, whence); }

/* ── errno ── */

static int _errno_val;

int *__errno_location(void) { return &_errno_val; }

/* ── FILE* I/O ── */

#define _FILE_READ   1
#define _FILE_WRITE  2
#define _FILE_APPEND 4
#define _FILE_EOF    8
#define _FILE_ERR    16
#define _FILE_INUSE  32
#define _FILE_UNBUF  64

#define FILE_BUFSIZ 1024
#define FILE_MAX    24

typedef struct _FILE {
    int fd;
    int flags;
    unsigned char buf[FILE_BUFSIZ];
    int buf_pos;    /* current position in buffer */
    int buf_len;    /* valid bytes in buffer (read mode) */
    int buf_dirty;  /* buffer has unflushed writes */
} FILE;

static FILE _files[FILE_MAX] = {
    [0] = { .fd = 0, .flags = _FILE_READ | _FILE_INUSE },
    [1] = { .fd = 1, .flags = _FILE_WRITE | _FILE_INUSE | _FILE_UNBUF },
    [2] = { .fd = 2, .flags = _FILE_WRITE | _FILE_INUSE | _FILE_UNBUF },
};

/* Forward declarations */
int fflush(FILE *f);
int fputc(int c, FILE *f);
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* Global pointers */
FILE *stdin  = &_files[0];
FILE *stdout = &_files[1];
FILE *stderr = &_files[2];

static void _stdio_init(void) { /* no-op — static init is sufficient */ }

static FILE *_file_alloc(void)
{
    _stdio_init();
    for (int i = 3; i < FILE_MAX; i++) {
        if (!(_files[i].flags & _FILE_INUSE)) {
            _files[i].buf_pos = 0;
            _files[i].buf_len = 0;
            _files[i].buf_dirty = 0;
            return &_files[i];
        }
    }
    return NULL;
}

static int _parse_mode(const char *mode)
{
    int flags = 0;
    switch (mode[0]) {
    case 'r': flags = _FILE_READ; break;
    case 'w': flags = _FILE_WRITE; break;
    case 'a': flags = _FILE_WRITE | _FILE_APPEND; break;
    default: return -1;
    }
    if (mode[1] == '+' || (mode[1] && mode[2] == '+'))
        flags |= _FILE_READ | _FILE_WRITE;
    return flags;
}

/* open flags: O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=0x40, O_TRUNC=0x200, O_APPEND=0x400 */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

FILE *fopen(const char *path, const char *mode)
{
    int fflags = _parse_mode(mode);
    if (fflags < 0) return NULL;

    int oflags;
    if ((fflags & (_FILE_READ | _FILE_WRITE)) == (_FILE_READ | _FILE_WRITE))
        oflags = O_RDWR;
    else if (fflags & _FILE_WRITE)
        oflags = O_WRONLY;
    else
        oflags = O_RDONLY;

    if (fflags & _FILE_WRITE) {
        oflags |= O_CREAT;
        if (fflags & _FILE_APPEND)
            oflags |= O_APPEND;
        else if (!(fflags & _FILE_READ))
            oflags |= O_TRUNC;
    }

    int fd = _open(path, oflags, 0666);
    if (fd < 0) return NULL;

    FILE *f = _file_alloc();
    if (!f) { _close(fd); return NULL; }

    f->fd = fd;
    f->flags = fflags | _FILE_INUSE;
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    int fflags = _parse_mode(mode);
    if (fflags < 0) return NULL;
    FILE *f = _file_alloc();
    if (!f) return NULL;
    f->fd = fd;
    f->flags = fflags | _FILE_INUSE;
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    if (!f) return NULL;
    if (f->flags & _FILE_INUSE) {
        fflush(f);
        _close(f->fd);
    }
    if (!path) return NULL;
    int fflags = _parse_mode(mode);
    if (fflags < 0) return NULL;
    int oflags = (fflags & _FILE_WRITE) ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
    int fd = _open(path, oflags, 0666);
    if (fd < 0) return NULL;
    f->fd = fd;
    f->flags = fflags | _FILE_INUSE;
    f->buf_pos = 0;
    f->buf_len = 0;
    f->buf_dirty = 0;
    return f;
}

int fflush(FILE *f)
{
    if (!f) return 0;
    if (f->buf_dirty && f->buf_pos > 0) {
        _write(f->fd, f->buf, f->buf_pos);
        f->buf_pos = 0;
        f->buf_dirty = 0;
    }
    return 0;
}

int fclose(FILE *f)
{
    if (!f) return EOF;
    fflush(f);
    int r = _close(f->fd);
    f->flags = 0;
    f->fd = -1;
    return r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !size || !nmemb) return 0;
    size_t total = size * nmemb;
    unsigned char *dst = (unsigned char *)ptr;
    size_t done = 0;

    /* First, consume buffered data */
    while (done < total && f->buf_pos < f->buf_len) {
        dst[done++] = f->buf[f->buf_pos++];
    }

    /* Read remaining directly */
    while (done < total) {
        ssize_t n = _read(f->fd, dst + done, total - done);
        if (n <= 0) {
            f->flags |= (n == 0) ? _FILE_EOF : _FILE_ERR;
            break;
        }
        done += (size_t)n;
    }

    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!f || !size || !nmemb) return 0;
    size_t total = size * nmemb;
    const unsigned char *src = (const unsigned char *)ptr;

    /* Unbuffered: write directly */
    if (f->flags & _FILE_UNBUF) {
        size_t done = 0;
        while (done < total) {
            ssize_t n = _write(f->fd, src + done, total - done);
            if (n <= 0) { f->flags |= _FILE_ERR; break; }
            done += (size_t)n;
        }
        return done / size;
    }

    /* Buffered write */
    for (size_t i = 0; i < total; i++) {
        f->buf[f->buf_pos++] = src[i];
        f->buf_dirty = 1;
        if (f->buf_pos >= FILE_BUFSIZ) fflush(f);
    }
    return nmemb;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return (fwrite(&ch, 1, 1, f) == 1) ? c : EOF;
}

int fputs(const char *s, FILE *f)
{
    size_t len = 0;
    while (s[len]) len++;
    return (fwrite(s, 1, len, f) == len) ? 0 : EOF;
}

int fgetc(FILE *f)
{
    if (!f) return EOF;
    /* Refill buffer if empty */
    if (f->buf_pos >= f->buf_len) {
        ssize_t n = _read(f->fd, f->buf, FILE_BUFSIZ);
        if (n <= 0) {
            f->flags |= (n == 0) ? _FILE_EOF : _FILE_ERR;
            return EOF;
        }
        f->buf_pos = 0;
        f->buf_len = (int)n;
    }
    return f->buf[f->buf_pos++];
}

int fseek(FILE *f, long off, int whence)
{
    if (!f) return -1;
    fflush(f);
    f->buf_pos = 0;
    f->buf_len = 0;
    long r = _lseek(f->fd, off, whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *f)
{
    if (!f) return -1;
    long pos = _lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    /* Adjust for buffered read data not yet consumed */
    return pos - (f->buf_len - f->buf_pos);
}

int feof(FILE *f) { return f ? (f->flags & _FILE_EOF) != 0 : 0; }
int ferror(FILE *f) { return f ? (f->flags & _FILE_ERR) != 0 : 0; }
void clearerr(FILE *f) { if (f) f->flags &= ~(_FILE_EOF | _FILE_ERR); }
int fileno(FILE *f) { return f ? f->fd : -1; }

void rewind(FILE *f)
{
    if (!f) return;
    (void)fseek(f, 0, SEEK_SET);
    clearerr(f);
}

int putchar(int c)
{
    _stdio_init();
    return fputc(c, stdout);
}

/* ── Formatted output (vsnprintf core) ── */

/* Internal: format into a callback */
typedef struct {
    char *buf;
    size_t pos;
    size_t max;
    FILE *fp;
} _fmt_ctx;

static void _fmt_putc(_fmt_ctx *ctx, char c)
{
    if (ctx->fp) {
        fputc(c, ctx->fp);
        ctx->pos++;
    } else if (ctx->buf && ctx->pos < ctx->max - 1) {
        ctx->buf[ctx->pos++] = c;
    } else {
        ctx->pos++;  /* count only */
    }
}

static void _fmt_puts(_fmt_ctx *ctx, const char *s)
{
    while (*s) _fmt_putc(ctx, *s++);
}

static void _fmt_putdec(_fmt_ctx *ctx, long val, int is_unsigned)
{
    unsigned long uv;
    if (!is_unsigned && val < 0) {
        _fmt_putc(ctx, '-');
        uv = (unsigned long)(-(val + 1)) + 1;
    } else {
        uv = (unsigned long)val;
    }
    char tmp[20];
    int i = 0;
    if (uv == 0) tmp[i++] = '0';
    else while (uv > 0) { tmp[i++] = '0' + (int)(uv % 10); uv /= 10; }
    while (i > 0) _fmt_putc(ctx, tmp[--i]);
}

static void _fmt_puthex(_fmt_ctx *ctx, unsigned long val, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[16];
    int i = 0;
    if (val == 0) tmp[i++] = '0';
    else while (val > 0) { tmp[i++] = digits[val & 0xf]; val >>= 4; }
    while (i > 0) _fmt_putc(ctx, tmp[--i]);
}

static int _fmt_core(_fmt_ctx *ctx, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { _fmt_putc(ctx, *fmt); continue; }
        fmt++;

        /* Flags */
        int pad_zero = 0, left_align = 0, show_sign = 0, space_sign = 0, alt = 0;
        for (;;) {
            if (*fmt == '0') pad_zero = 1;
            else if (*fmt == '-') left_align = 1;
            else if (*fmt == '+') show_sign = 1;
            else if (*fmt == ' ') space_sign = 1;
            else if (*fmt == '#') alt = 1;
            else break;
            fmt++;
        }
        (void)show_sign; (void)space_sign; (void)alt;

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        (void)prec; /* used for strings below */

        /* Length */
        int is_long = 0, is_longlong = 0, is_short = 0, is_size = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_longlong = 1; fmt++; } }
        else if (*fmt == 'h') { is_short = 1; fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { is_size = 1; fmt++; }
        else if (*fmt == 'j' || *fmt == 't') { is_long = 1; fmt++; }

        /* Conversion */
        char conv_buf[32];
        const char *out_str = NULL;
        int out_len = 0;

        switch (*fmt) {
        case 'd': case 'i': {
            long v = (is_long || is_longlong || is_size) ? va_arg(ap, long) : (long)va_arg(ap, int);
            _fmt_putdec(ctx, v, 0);
            continue;
        }
        case 'u': {
            unsigned long v = (is_long || is_longlong || is_size)
                ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            _fmt_putdec(ctx, (long)v, 1);
            continue;
        }
        case 'x': case 'X': {
            unsigned long v = (is_long || is_longlong || is_size)
                ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            _fmt_puthex(ctx, v, *fmt == 'X');
            continue;
        }
        case 'o': {
            unsigned long v = (is_long || is_longlong || is_size)
                ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            char tmp[22]; int i = 0;
            if (v == 0) tmp[i++] = '0';
            else while (v > 0) { tmp[i++] = '0' + (int)(v & 7); v >>= 3; }
            while (i > 0) _fmt_putc(ctx, tmp[--i]);
            continue;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            _fmt_puts(ctx, "0x");
            _fmt_puthex(ctx, v, 0);
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            if (prec >= 0 && prec < slen) slen = prec;
            /* Padding */
            if (!left_align && width > slen)
                for (int i = 0; i < width - slen; i++) _fmt_putc(ctx, ' ');
            for (int i = 0; i < slen; i++) _fmt_putc(ctx, s[i]);
            if (left_align && width > slen)
                for (int i = 0; i < width - slen; i++) _fmt_putc(ctx, ' ');
            continue;
        }
        case 'c': {
            int c = va_arg(ap, int);
            _fmt_putc(ctx, (char)c);
            continue;
        }
        case '%':
            _fmt_putc(ctx, '%');
            continue;
        case 'f': case 'F': {
            double v = va_arg(ap, double);
            int dp = (prec >= 0) ? prec : 6;
            if (dp > 20) dp = 20;
            /* Sign */
            if (v < 0.0) { _fmt_putc(ctx, '-'); v = -v; }
            else if (show_sign) _fmt_putc(ctx, '+');
            else if (space_sign) _fmt_putc(ctx, ' ');
            /* Check special values */
            if (__builtin_isnan(v)) { _fmt_puts(ctx, "nan"); continue; }
            if (__builtin_isinf(v)) { _fmt_puts(ctx, "inf"); continue; }
            /* Integer part */
            unsigned long ipart = (unsigned long)v;
            double frac = v - (double)ipart;
            /* Round the fractional part */
            double rnd = 0.5;
            for (int i = 0; i < dp; i++) rnd /= 10.0;
            frac += rnd;
            if (frac >= 1.0) { ipart++; frac -= 1.0; }
            _fmt_putdec(ctx, (long)ipart, 1);
            if (dp > 0 || alt) {
                _fmt_putc(ctx, '.');
                for (int i = 0; i < dp; i++) {
                    frac *= 10.0;
                    int d = (int)frac;
                    if (d > 9) d = 9;
                    _fmt_putc(ctx, '0' + d);
                    frac -= d;
                }
            }
            continue;
        }
        case 'e': case 'E': {
            double v = va_arg(ap, double);
            int dp = (prec >= 0) ? prec : 6;
            if (dp > 20) dp = 20;
            /* Sign */
            if (v < 0.0) { _fmt_putc(ctx, '-'); v = -v; }
            else if (show_sign) _fmt_putc(ctx, '+');
            else if (space_sign) _fmt_putc(ctx, ' ');
            /* Special values */
            if (__builtin_isnan(v)) { _fmt_puts(ctx, "nan"); continue; }
            if (__builtin_isinf(v)) { _fmt_puts(ctx, "inf"); continue; }
            /* Normalize to 1.xxxxx * 10^exp */
            int exp10 = 0;
            if (v == 0.0) {
                exp10 = 0;
            } else {
                while (v >= 10.0) { v /= 10.0; exp10++; }
                while (v < 1.0) { v *= 10.0; exp10--; }
            }
            /* Round the mantissa */
            double rnd = 0.5;
            for (int i = 0; i < dp; i++) rnd /= 10.0;
            v += rnd;
            if (v >= 10.0) { v /= 10.0; exp10++; }
            /* Print mantissa digit */
            int lead = (int)v;
            if (lead > 9) lead = 9;
            _fmt_putc(ctx, '0' + lead);
            double frac = v - lead;
            if (dp > 0 || alt) {
                _fmt_putc(ctx, '.');
                for (int i = 0; i < dp; i++) {
                    frac *= 10.0;
                    int d = (int)frac;
                    if (d > 9) d = 9;
                    _fmt_putc(ctx, '0' + d);
                    frac -= d;
                }
            }
            /* Print exponent */
            _fmt_putc(ctx, (*fmt == 'E') ? 'E' : 'e');
            if (exp10 < 0) { _fmt_putc(ctx, '-'); exp10 = -exp10; }
            else _fmt_putc(ctx, '+');
            if (exp10 < 10) _fmt_putc(ctx, '0');
            if (exp10 >= 100) {
                _fmt_putc(ctx, '0' + exp10 / 100);
                _fmt_putc(ctx, '0' + (exp10 / 10) % 10);
                _fmt_putc(ctx, '0' + exp10 % 10);
            } else {
                _fmt_putc(ctx, '0' + exp10 / 10);
                _fmt_putc(ctx, '0' + exp10 % 10);
            }
            continue;
        }
        case 'g': case 'G': {
            /* %g: use %e if exp < -4 or exp >= precision, else %f */
            double v = va_arg(ap, double);
            int gprec = (prec >= 0) ? prec : 6;
            if (gprec == 0) gprec = 1;
            double av = v < 0 ? -v : v;
            /* Determine exponent */
            int exp10 = 0;
            if (av != 0.0 && !__builtin_isnan(av) && !__builtin_isinf(av)) {
                double tmp = av;
                while (tmp >= 10.0) { tmp /= 10.0; exp10++; }
                while (tmp < 1.0) { tmp *= 10.0; exp10--; }
            }
            /* Push the value back and format as %e or %f via recursive call */
            char gbuf[64];
            if (exp10 < -4 || exp10 >= gprec) {
                /* Use %e with precision = gprec-1 */
                char gfmt[16];
                int fi = 0;
                gfmt[fi++] = '%';
                if (show_sign) gfmt[fi++] = '+';
                gfmt[fi++] = '.';
                if (gprec - 1 >= 10) { gfmt[fi++] = '0' + (gprec-1)/10; gfmt[fi++] = '0' + (gprec-1)%10; }
                else gfmt[fi++] = '0' + (gprec-1);
                gfmt[fi++] = (*fmt == 'G') ? 'E' : 'e';
                gfmt[fi] = '\0';
                snprintf(gbuf, sizeof(gbuf), gfmt, v);
            } else {
                /* Use %f with precision = gprec - exp10 - 1 */
                int fprec = gprec - exp10 - 1;
                if (fprec < 0) fprec = 0;
                char gfmt[16];
                int fi = 0;
                gfmt[fi++] = '%';
                if (show_sign) gfmt[fi++] = '+';
                gfmt[fi++] = '.';
                if (fprec >= 10) { gfmt[fi++] = '0' + fprec/10; gfmt[fi++] = '0' + fprec%10; }
                else gfmt[fi++] = '0' + fprec;
                gfmt[fi++] = 'f';
                gfmt[fi] = '\0';
                snprintf(gbuf, sizeof(gbuf), gfmt, v);
            }
            /* Strip trailing zeros (unless # flag) */
            if (!alt) {
                int glen = 0;
                while (gbuf[glen]) glen++;
                /* Find 'e' or 'E' to not strip exponent */
                int epos = -1;
                for (int i = 0; i < glen; i++) if (gbuf[i] == 'e' || gbuf[i] == 'E') { epos = i; break; }
                int strip_end = (epos >= 0) ? epos : glen;
                int dot = -1;
                for (int i = 0; i < strip_end; i++) if (gbuf[i] == '.') { dot = i; break; }
                if (dot >= 0) {
                    while (strip_end > dot + 1 && gbuf[strip_end-1] == '0') strip_end--;
                    if (strip_end == dot + 1) strip_end = dot; /* remove dot too */
                    /* Reassemble */
                    if (epos >= 0) {
                        int elen = glen - epos;
                        for (int i = 0; i < elen; i++) gbuf[strip_end + i] = gbuf[epos + i];
                        gbuf[strip_end + (glen - epos)] = '\0';
                    } else {
                        gbuf[strip_end] = '\0';
                    }
                }
            }
            _fmt_puts(ctx, gbuf);
            continue;
        }
        case 'n':
            /* %n — store chars written so far */
            if (is_long) *va_arg(ap, long *) = (long)ctx->pos;
            else *va_arg(ap, int *) = (int)ctx->pos;
            continue;
        default:
            _fmt_putc(ctx, '%');
            if (*fmt) _fmt_putc(ctx, *fmt);
            continue;
        }
    }

    return (int)ctx->pos;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    _fmt_ctx ctx = { buf, 0, size, NULL };
    int r = _fmt_core(&ctx, fmt, ap);
    if (buf && size > 0) buf[ctx.pos < size ? ctx.pos : size - 1] = '\0';
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    _stdio_init();
    _fmt_ctx ctx = { NULL, 0, 0, f };
    return _fmt_core(&ctx, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

/* ── String functions ── */

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++) if (*s == (char)c) last = s;
    if (c == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!needle[0]) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    for (; *s; s++) {
        const char *a;
        for (a = accept; *a; a++) if (*s == *a) break;
        if (!*a) break;
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    for (; *s; s++) {
        for (const char *r = reject; *r; r++) if (*s == *r) return n;
        n++;
    }
    return n;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (*d) d++;
    for (size_t i = 0; i < n && src[i]; i++) *d++ = src[i];
    *d = '\0';
    return dst;
}

char *strdup(const char *s)
{
    extern void *malloc(size_t);
    extern void *memcpy(void *, const void *, size_t);
    extern size_t strlen(const char *);
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s || d >= s + n) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    return 0;
}

int bcmp(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n);
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

static const char *_error_strings[] = {
    "Success", "Operation not permitted", "No such file or directory",
    "No such process", "Interrupted system call", "I/O error",
    "No such device or address", "Argument list too long",
    "Exec format error", "Bad file descriptor", "No child processes",
    "Resource temporarily unavailable", "Out of memory", "Permission denied",
    "Bad address", NULL, "Device or resource busy", "File exists",
    "Invalid cross-device link", "No such device", "Not a directory",
    "Is a directory", "Invalid argument"
};

char *strerror(int errnum)
{
    if (errnum >= 0 && errnum < (int)(sizeof(_error_strings)/sizeof(_error_strings[0]))
        && _error_strings[errnum])
        return (char *)_error_strings[errnum];
    return (char *)"Unknown error";
}

/* ── Number conversion ── */

static int _isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static int _isdigit(int c) { return c >= '0' && c <= '9'; }
static int _isxdigit(int c) { return _isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int _isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int _toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int _tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int isspace(int c) { return _isspace(c); }
int isdigit(int c) { return _isdigit(c); }
int isxdigit(int c) { return _isxdigit(c); }
int isalpha(int c) { return _isalpha(c); }
int isalnum(int c) { return _isalpha(c) || _isdigit(c); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int toupper(int c) { return _toupper(c); }
int tolower(int c) { return _tolower(c); }

long strtol(const char *s, char **endp, int base)
{
    while (_isspace(*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        { base = 16; s += 2; }
    else if ((base == 0 || base == 8) && s[0] == '0')
        { base = 8; s++; }
    else if (base == 0)
        base = 10;

    unsigned long acc = 0;
    const char *start = s;
    for (; *s; s++) {
        int d;
        if (_isdigit(*s)) d = *s - '0';
        else if (_isalpha(*s)) d = _toupper(*s) - 'A' + 10;
        else break;
        if (d >= base) break;
        acc = acc * (unsigned long)base + (unsigned long)d;
    }
    if (endp) *endp = (char *)(s > start ? s : start);
    return neg ? -(long)acc : (long)acc;
}

unsigned long strtoul(const char *s, char **endp, int base)
{
    while (_isspace(*s)) s++;
    if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        { base = 16; s += 2; }
    else if ((base == 0 || base == 8) && s[0] == '0')
        { base = 8; s++; }
    else if (base == 0)
        base = 10;

    unsigned long acc = 0;
    const char *start = s;
    for (; *s; s++) {
        int d;
        if (_isdigit(*s)) d = *s - '0';
        else if (_isalpha(*s)) d = _toupper(*s) - 'A' + 10;
        else break;
        if (d >= base) break;
        acc = acc * (unsigned long)base + (unsigned long)d;
    }
    if (endp) *endp = (char *)(s > start ? s : start);
    return acc;
}

unsigned long long strtoull(const char *s, char **endp, int base)
{
    return (unsigned long long)strtoul(s, endp, base);
}

/* glibc C23 redirects — alias to standard versions */
long __isoc23_strtol(const char *s, char **endp, int base)
{ return strtol(s, endp, base); }

unsigned long __isoc23_strtoul(const char *s, char **endp, int base)
{ return strtoul(s, endp, base); }

unsigned long long __isoc23_strtoull(const char *s, char **endp, int base)
{ return strtoull(s, endp, base); }

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

/* Basic strtod — handles sign, digits, decimal point, exponent */
double strtod(const char *s, char **endp)
{
    while (_isspace(*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    double val = 0.0;
    const char *start = s;
    while (_isdigit(*s)) { val = val * 10.0 + (*s - '0'); s++; }

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (_isdigit(*s)) { val += (*s - '0') * frac; frac *= 0.1; s++; }
    }

    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') s++;
        int exp = 0;
        while (_isdigit(*s)) { exp = exp * 10 + (*s - '0'); s++; }
        double mul = 1.0;
        for (int i = 0; i < exp; i++) mul *= 10.0;
        if (eneg) val /= mul; else val *= mul;
    }

    if (endp) *endp = (char *)s;
    return neg ? -val : val;
}

float strtof(const char *s, char **endp)
{
    return (float)strtod(s, endp);
}

long double strtold(const char *s, char **endp)
{
    return (long double)strtod(s, endp);
}

/* ldexpl: x * 2^exp */
long double ldexpl(long double x, int exp)
{
    double r = (double)x;
    if (exp > 0) for (int i = 0; i < exp; i++) r *= 2.0;
    else for (int i = 0; i < -exp; i++) r /= 2.0;
    return (long double)r;
}

/* ── Memory ── */

extern void *malloc(size_t);
extern void free(void *);

void *calloc(size_t nmemb, size_t size)
{
    extern void *memset(void *, int, size_t);
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *old, size_t new_size)
{
    extern void *memcpy(void *, const void *, size_t);
    if (!old) return malloc(new_size);
    if (!new_size) { free(old); return NULL; }

    /* Our bump allocator doesn't track sizes, so we always copy.
     * Assume old block is at least new_size or we're growing.
     * We'll just allocate new and copy (conservative — may copy garbage
     * beyond old bounds, but TCC always grows buffers so old_size < new_size). */
    void *p = malloc(new_size);
    if (p) {
        /* Copy up to new_size bytes. For a bump allocator, reading beyond
         * the original allocation is safe (it's all our heap). */
        memcpy(p, old, new_size);
        /* Note: free() is a no-op in our bump allocator */
        free(old);
    }
    return p;
}

/* ── qsort ── */

static void _swap(char *a, char *b, size_t size)
{
    char tmp;
    for (size_t i = 0; i < size; i++) {
        tmp = a[i]; a[i] = b[i]; b[i] = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *))
{
    /* Simple insertion sort — good enough for TCC's symbol tables */
    char *b = (char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0 && cmp(b + j * size, b + (j-1) * size) < 0; j--) {
            _swap(b + j * size, b + (j-1) * size, size);
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid * size);
        if (r == 0) return (void *)(b + mid * size);
        else if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* ── abs ── */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* ── OS stubs — return safe defaults ── */

char **environ = NULL;

char *getenv(const char *name)
{
    if (!name || !*name || !environ)
        return NULL;

    for (char **entry = environ; *entry; entry++) {
        const char *lhs = *entry;
        const char *rhs = name;
        while (*rhs && *lhs == *rhs) {
            lhs++;
            rhs++;
        }
        if (!*rhs && *lhs == '=')
            return (char *)(lhs + 1);
    }
    return NULL;
}

char *getcwd(char *buf, size_t size)
{
    if (buf && size > 1) { buf[0] = '/'; buf[1] = '\0'; return buf; }
    return NULL;
}

char *realpath(const char *path, char *resolved)
{
    extern size_t strlen(const char *);
    extern void *memcpy(void *, const void *, size_t);
    if (!path) return NULL;
    if (resolved) {
        size_t len = strlen(path);
        memcpy(resolved, path, len + 1);
        return resolved;
    }
    return strdup(path);
}

int unlink(const char *path) { return (int)__syscall1(SYS_unlink, (long)path); }
int remove(const char *path) { return unlink(path); }
int access(const char *path, int mode) { return (int)__syscall2(SYS_access, (long)path, mode); }

int execvp(const char *file, char *const argv[])
{
    (void)file; (void)argv;
    _errno_val = 38; /* ENOSYS */
    return -1;
}

long sysconf(int name)
{
    if (name == 30) return 4096; /* _SC_PAGESIZE */
    return -1;
}

/* mprotect is in crt.o (real syscall) — do not duplicate here */

/* ── Time stubs ── */

typedef struct { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; } tm_t;
/* Forward declare as struct tm for ABI compatibility */
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };

time_t time(time_t *t)
{
    time_t val = 0; /* Always returns epoch 0 */
    if (t) *t = val;
    return val;
}

struct tm *localtime(const time_t *t)
{
    static struct tm tm0 = { 0, 0, 0, 1, 0, 70, 4, 0, 0 }; /* Jan 1 1970 */
    (void)t;
    return &tm0;
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
    if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
    return 0;
}

/* ── Signal stubs ── */

typedef void (*sighandler_t)(int);
typedef unsigned long sigset_t;

struct sigaction_t {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};

int sigaction(int sig, const void *act, void *oldact)
{
    (void)sig; (void)act; (void)oldact;
    return 0;
}

int sigaddset(sigset_t *set, int signum) { (void)set; (void)signum; return 0; }
int sigemptyset(sigset_t *set) { if (set) *set = 0; return 0; }
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{ (void)how; (void)set; (void)oldset; return 0; }

sighandler_t signal(int sig, sighandler_t handler)
{ (void)sig; (void)handler; return (sighandler_t)0; }

/* ── Semaphore stubs (TCC threading — unused in OsitoK) ── */

typedef struct { int value; } sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value)
{ (void)pshared; if (sem) sem->value = (int)value; return 0; }

int sem_post(sem_t *sem) { if (sem) sem->value++; return 0; }
int sem_wait(sem_t *sem) { if (sem) sem->value--; return 0; }

/* ── abort ── */

void abort(void)
{
    _write(2, "abort()\n", 8);
    __syscall1(SYS_exit, 134);
    __builtin_unreachable();
}

/* ── fesetround / fegetround (x87 FPU rounding) ── */

int fesetround(int round)
{
    unsigned short cw;
    __asm__ volatile ("fnstcw %0" : "=m"(cw));
    cw = (cw & ~0x0C00) | ((round & 3) << 10);
    __asm__ volatile ("fldcw %0" : : "m"(cw));
    return 0;
}

int fegetround(void)
{
    unsigned short cw;
    __asm__ volatile ("fnstcw %0" : "=m"(cw));
    return (cw >> 10) & 3;
}

/* ── localtime_r / gmtime_r / mktime / strftime / clock stubs ── */

struct _tm_compat {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

struct _tm_compat *localtime_r(const time_t *t, struct _tm_compat *result)
{
    (void)t;
    if (result) {
        result->tm_sec = 0; result->tm_min = 0; result->tm_hour = 0;
        result->tm_mday = 1; result->tm_mon = 0; result->tm_year = 70;
        result->tm_wday = 4; result->tm_yday = 0; result->tm_isdst = 0;
        result->tm_gmtoff = 0; result->tm_zone = "UTC";
    }
    return result;
}

struct _tm_compat *gmtime_r(const time_t *t, struct _tm_compat *result)
{
    return localtime_r(t, result);
}

struct _tm_compat *gmtime(const time_t *t)
{
    static struct _tm_compat _gm;
    return gmtime_r(t, &_gm);
}

time_t mktime(struct _tm_compat *tm)
{
    (void)tm;
    return 0;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct _tm_compat *tm)
{
    (void)fmt; (void)tm;
    if (s && max > 0) s[0] = '\0';
    return 0;
}

long clock(void) { return 0; }

/* ── malloc_usable_size stub ── */

size_t malloc_usable_size(const void *ptr)
{
    (void)ptr;
    return 0;
}

/* ── strtoimax / strtoumax ── */

long strtoimax(const char *s, char **endp, int base)
{
    return strtol(s, endp, base);
}

unsigned long strtoumax(const char *s, char **endp, int base)
{
    return strtoul(s, endp, base);
}

/* ── getpid ── */

int getpid(void) { return 1; }

/* ── exit ── */

void exit(int status)
{
    /* Flush all open streams */
    _stdio_init();
    for (int i = 0; i < FILE_MAX; i++) {
        if (_files[i].flags & _FILE_INUSE)
            fflush(&_files[i]);
    }
    __syscall1(SYS_exit, status);
    __builtin_unreachable();
}

/* ── 128-bit integer division (GCC __int128 support for libbf.c) ── */

typedef unsigned __int128 uint128_t;

uint128_t __udivmodti4(uint128_t num, uint128_t den, uint128_t *rem)
{
    if (den == 0) { if (rem) *rem = 0; return 0; }
    if (num < den) { if (rem) *rem = num; return 0; }
    if (den == 1) { if (rem) *rem = 0; return num; }

    /* Binary long division */
    uint128_t quot = 0;
    int shift = 0;

    /* Find highest bit position of num */
    uint128_t tmp = num;
    while (tmp > den) { tmp >>= 1; shift++; }

    for (int i = shift; i >= 0; i--) {
        if (num >= (den << i)) {
            num -= (den << i);
            quot |= ((uint128_t)1 << i);
        }
    }
    if (rem) *rem = num;
    return quot;
}

uint128_t __udivti3(uint128_t a, uint128_t b)
{
    return __udivmodti4(a, b, (uint128_t *)0);
}

/* ── Assert ── */

void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
{
    _stdio_init();
    fprintf(stderr, "assert failed: %s at %s:%u (%s)\n", expr, file, line, func ? func : "?");
    __syscall1(SYS_exit, 1);
    __builtin_unreachable();
}

/* ── setjmp/longjmp — defined in syscall.S ── */
/* _setjmp and longjmp are provided as assembly routines */
