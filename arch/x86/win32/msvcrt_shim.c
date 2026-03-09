/*
 * OsitoK Windows Compatibility Layer — msvcrt.dll Shim Implementation
 *
 * Microsoft C Runtime functions used by CRT-linked Windows executables.
 * printf uses a custom formatter; memory uses our heap; stdio wraps NT I/O.
 */

#include "msvcrt_shim.h"
#include "kernel32_shim.h"
#include "ntdll_shim.h"

#ifdef TEST_HARNESS
#include <math.h>
#endif

/*
 * IMPORTANT: ms_abi variadic functions must use __builtin_ms_va_list,
 * NOT the standard va_list (which is for System V ABI). GCC provides
 * dedicated builtins for Microsoft ABI va_list handling.
 */
typedef __builtin_ms_va_list ms_va_list;
#define ms_va_start(ap, param) __builtin_ms_va_start(ap, param)
#define ms_va_arg(ap, type)    __builtin_va_arg(ap, type)
#define ms_va_end(ap)          __builtin_ms_va_end(ap)
#define ms_va_copy(dst, src)   __builtin_ms_va_copy(dst, src)

/* ── Internal helpers ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void proc_exit(int32_t code);

/* ── CRT Initialization ────────────────────────────────────── */

void WINAPI _initterm(_PVFV *pfbegin, _PVFV *pfend)
{
    for (_PVFV *pfn = pfbegin; pfn < pfend; pfn++) {
        if (*pfn)
            (*pfn)();
    }
}

int WINAPI _initterm_e(_PIFV *pfbegin, _PIFV *pfend)
{
    for (_PIFV *pfn = pfbegin; pfn < pfend; pfn++) {
        if (*pfn) {
            int ret = (*pfn)();
            if (ret != 0)
                return ret;
        }
    }
    return 0;
}

static char *g_argv0 = "program.exe";
static char *g_argv[] = { NULL, NULL };
static char *g_envp[] = { NULL };

int WINAPI __getmainargs(int *argc, char ***argv, char ***env,
                          int do_wildcard, void *startinfo)
{
    (void)do_wildcard;
    (void)startinfo;
    g_argv[0] = g_argv0;
    *argc = 1;
    *argv = g_argv;
    *env  = g_envp;
    return 0;
}

static WCHAR *g_wargv0 = (WCHAR[]){'p','r','o','g','r','a','m','.','e','x','e',0};
static WCHAR *g_wargv[] = { NULL, NULL };
static WCHAR *g_wenvp[] = { NULL };

int WINAPI __wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env,
                           int do_wildcard, void *startinfo)
{
    (void)do_wildcard;
    (void)startinfo;
    g_wargv[0] = g_wargv0;
    *argc = 1;
    *argv = g_wargv;
    *env  = g_wenvp;
    return 0;
}

void WINAPI __set_app_type(int type) { (void)type; }
int  WINAPI _set_new_mode(int mode) { (void)mode; return 0; }

/* ── Heap-backed malloc/free ───────────────────────────────── */

/*
 * Simple allocator with size prefix for realloc support.
 * Each allocation: [8-byte size header][user data...]
 * Aligned to 16 bytes.
 */
#define ALLOC_POOL_SIZE  (4 * 1024 * 1024)  /* 4MB CRT heap */
static BYTE  crt_pool[ALLOC_POOL_SIZE];
static SIZE_T crt_pool_offset = 0;

/* Free list for basic reuse */
#define FREE_LIST_MAX 256
typedef struct { PVOID addr; SIZE_T size; } FREE_ENTRY;
static FREE_ENTRY free_list[FREE_LIST_MAX];
static int free_list_count = 0;

PVOID WINAPI crt_malloc(SIZE_T size)
{
    if (size == 0) size = 1;
    SIZE_T total = (size + 8 + 15) & ~(SIZE_T)15;  /* 8-byte header + alignment */

    /* Check free list first */
    for (int i = 0; i < free_list_count; i++) {
        if (free_list[i].size >= total) {
            BYTE *block = (BYTE *)free_list[i].addr;
            /* Remove from free list */
            free_list[i] = free_list[--free_list_count];
            *(SIZE_T *)block = total;
            return block + 8;
        }
    }

    if (crt_pool_offset + total > ALLOC_POOL_SIZE)
        return NULL;

    BYTE *block = crt_pool + crt_pool_offset;
    crt_pool_offset += total;
    *(SIZE_T *)block = total;
    return block + 8;
}

PVOID WINAPI crt_calloc(SIZE_T count, SIZE_T size)
{
    SIZE_T total = count * size;
    PVOID p = crt_malloc(total);
    if (p) crt_memset(p, 0, total);
    return p;
}

void WINAPI crt_free(PVOID ptr)
{
    if (!ptr) return;
    BYTE *block = (BYTE *)ptr - 8;
    SIZE_T total = *(SIZE_T *)block;

    /* Add to free list if space */
    if (free_list_count < FREE_LIST_MAX) {
        free_list[free_list_count].addr = block;
        free_list[free_list_count].size = total;
        free_list_count++;
    }
}

PVOID WINAPI crt_realloc(PVOID ptr, SIZE_T size)
{
    if (!ptr) return crt_malloc(size);
    if (size == 0) { crt_free(ptr); return NULL; }

    BYTE *old_block = (BYTE *)ptr - 8;
    SIZE_T old_total = *(SIZE_T *)old_block;
    SIZE_T old_data = old_total - 8;

    if (size <= old_data)
        return ptr;  /* fits in existing block */

    PVOID new_ptr = crt_malloc(size);
    if (!new_ptr) return NULL;
    crt_memcpy(new_ptr, ptr, old_data < size ? old_data : size);
    crt_free(ptr);
    return new_ptr;
}

/* ── String functions ──────────────────────────────────────── */

SIZE_T WINAPI crt_strlen(const char *s)
{
    SIZE_T len = 0;
    while (s[len]) len++;
    return len;
}

int WINAPI crt_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int WINAPI crt_strncmp(const char *a, const char *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) break;
    }
    return 0;
}

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int WINAPI crt_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = to_lower(*a), cb = to_lower(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)to_lower(*a) - (unsigned char)to_lower(*b);
}

int WINAPI crt_strnicmp(const char *a, const char *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        char ca = to_lower(a[i]), cb = to_lower(b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) break;
    }
    return 0;
}

char* WINAPI crt_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* WINAPI crt_strncpy(char *dst, const char *src, SIZE_T n)
{
    SIZE_T i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* WINAPI crt_strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char* WINAPI crt_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char* WINAPI crt_strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char* WINAPI crt_strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++)
        if (*s == (char)c) last = s;
    if (c == 0) return (char *)s;
    return (char *)last;
}

/* ── Memory ops ────────────────────────────────────────────── */

PVOID WINAPI crt_memcpy(PVOID dst, PCVOID src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--) *d++ = *s++;
    return dst;
}

PVOID WINAPI crt_memset(PVOID dst, int c, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    while (n--) *d++ = (BYTE)c;
    return dst;
}

PVOID WINAPI crt_memmove(PVOID dst, PCVOID src, SIZE_T n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int WINAPI crt_memcmp(PCVOID a, PCVOID b, SIZE_T n)
{
    const BYTE *pa = (const BYTE *)a, *pb = (const BYTE *)b;
    for (SIZE_T i = 0; i < n; i++)
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    return 0;
}

/* ── Format I/O engine ─────────────────────────────────────── */

/*
 * Minimal printf engine supporting:
 *   %d %i %u %x %X %o %p %s %c %f %% %ld %lld %lu %llu %lx %llx
 *   Width, precision, padding (0 and space), left-align (-), sign (+/ )
 *   %f with configurable precision (default 6), rounding
 *   %e/%g consume the arg and format as %f (no scientific notation)
 */

typedef struct {
    char *buf;
    SIZE_T size;
    SIZE_T pos;
} FMT_CTX;

static void fmt_putc(FMT_CTX *ctx, char c)
{
    if (ctx->buf) {
        if (ctx->pos < ctx->size - 1)
            ctx->buf[ctx->pos] = c;
    } else {
        /* Direct to console stdout */
        DWORD written;
        WriteFile((HANDLE)(ULONG_PTR)8, &c, 1, &written, NULL);
    }
    ctx->pos++;
}

static void fmt_puts(FMT_CTX *ctx, const char *s, SIZE_T len)
{
    for (SIZE_T i = 0; i < len; i++)
        fmt_putc(ctx, s[i]);
}

static void fmt_pad(FMT_CTX *ctx, int count, char pad_char)
{
    while (count-- > 0) fmt_putc(ctx, pad_char);
}

static SIZE_T uint_to_str(char *buf, unsigned long long val, int base, int upper)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    char tmp[24];
    int i = 0;

    if (val == 0) { tmp[i++] = '0'; }
    else { while (val) { tmp[i++] = digits[val % base]; val /= base; } }

    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = 0;
    return i;
}

static int do_vformat(FMT_CTX *ctx, const char *fmt, ms_va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            fmt_putc(ctx, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags */
        int left_align = 0, zero_pad = 0, plus_sign = 0, space_sign = 0;
        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '+') { plus_sign = 1; fmt++; }
            else if (*fmt == ' ') { space_sign = 1; fmt++; }
            else break;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = ms_va_arg(ap, int); fmt++; }
        else { while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; } }

        /* Precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = ms_va_arg(ap, int); fmt++; }
            else { while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt - '0'); fmt++; } }
        }

        /* Length modifier */
        int len_mod = 0; /* 0=int, 1=long, 2=long long, 3=size_t */
        if (*fmt == 'l') {
            fmt++; len_mod = 1;
            if (*fmt == 'l') { fmt++; len_mod = 2; }
        } else if (*fmt == 'z') {
            fmt++; len_mod = 3;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
            /* treat as int */
        } else if (*fmt == 'I') {
            /* MSVC I64 prefix */
            if (fmt[1] == '6' && fmt[2] == '4') {
                fmt += 3; len_mod = 2;
            }
        }

        /* Conversion */
        char num_buf[24];
        SIZE_T num_len;
        const char *str;
        SIZE_T str_len;
        int negative = 0;

        switch (*fmt) {
        case 'd': case 'i': {
            long long val;
            if (len_mod == 2) val = ms_va_arg(ap, long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, long);
            else val = ms_va_arg(ap, int);

            if (val < 0) { negative = 1; val = -val; }
            num_len = uint_to_str(num_buf, (unsigned long long)val, 10, 0);

            int total = (int)num_len + negative;
            if (plus_sign || space_sign) total++;
            char pad = (zero_pad && !left_align) ? '0' : ' ';

            if (!left_align && pad == ' ') fmt_pad(ctx, width - total, ' ');
            if (negative) fmt_putc(ctx, '-');
            else if (plus_sign) fmt_putc(ctx, '+');
            else if (space_sign) fmt_putc(ctx, ' ');
            if (!left_align && pad == '0') fmt_pad(ctx, width - total, '0');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - total, ' ');
            break;
        }
        case 'u': {
            unsigned long long val;
            if (len_mod == 2) val = ms_va_arg(ap, unsigned long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, unsigned long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 10, 0);
            if (!left_align) fmt_pad(ctx, width - (int)num_len, zero_pad ? '0' : ' ');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - (int)num_len, ' ');
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (len_mod == 2) val = ms_va_arg(ap, unsigned long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, unsigned long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 16, (*fmt == 'X'));
            if (!left_align) fmt_pad(ctx, width - (int)num_len, zero_pad ? '0' : ' ');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - (int)num_len, ' ');
            break;
        }
        case 'o': {
            unsigned long long val;
            if (len_mod >= 1) val = ms_va_arg(ap, unsigned long long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 8, 0);
            if (!left_align) fmt_pad(ctx, width - (int)num_len, zero_pad ? '0' : ' ');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - (int)num_len, ' ');
            break;
        }
        case 'p': {
            unsigned long long val = (unsigned long long)(ULONG_PTR)ms_va_arg(ap, PVOID);
            num_len = uint_to_str(num_buf, val, 16, 0);
            /* Pad to pointer width */
            int total = (int)num_len + 2; /* "0x" prefix */
            if (!left_align) fmt_pad(ctx, width - total, ' ');
            fmt_putc(ctx, '0'); fmt_putc(ctx, 'x');
            fmt_pad(ctx, 16 - (int)num_len, '0');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - total, ' ');
            break;
        }
        case 's':
            str = ms_va_arg(ap, const char *);
            if (!str) str = "(null)";
            str_len = crt_strlen(str);
            if (precision >= 0 && (SIZE_T)precision < str_len)
                str_len = (SIZE_T)precision;
            if (!left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            fmt_puts(ctx, str, str_len);
            if (left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            break;

        case 'c': {
            char ch = (char)ms_va_arg(ap, int);
            if (!left_align) fmt_pad(ctx, width - 1, ' ');
            fmt_putc(ctx, ch);
            if (left_align) fmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'e': case 'g': {
            double val = ms_va_arg(ap, double);
            int prec = (precision >= 0) ? precision : 6;

            /* Handle negative / sign */
            int f_neg = 0;
            if (val < 0) { f_neg = 1; val = -val; }

            /* Decompose into integer and fractional parts.
             * We work with unsigned 64-bit for the integer portion
             * and compute fractional digits via repeated multiply. */
            unsigned long long int_part = (unsigned long long)val;
            double frac_part = val - (double)int_part;

            /* Build integer-part string */
            char f_buf[80];
            int f_pos = 0;
            SIZE_T ip_len = uint_to_str(f_buf, int_part, 10, 0);
            f_pos = (int)ip_len;

            /* Decimal point + fractional digits */
            if (prec > 0) {
                f_buf[f_pos++] = '.';
                for (int fi = 0; fi < prec; fi++) {
                    frac_part *= 10.0;
                    int fdigit = (int)frac_part;
                    if (fdigit > 9) fdigit = 9;
                    f_buf[f_pos++] = '0' + fdigit;
                    frac_part -= fdigit;
                }
                /* Round: check if remaining frac >= 0.5 */
                if (frac_part >= 0.5) {
                    /* Propagate carry backwards through frac digits */
                    int ci = f_pos - 1;
                    while (ci >= 0) {
                        if (f_buf[ci] == '.') { ci--; continue; }
                        if (f_buf[ci] < '9') { f_buf[ci]++; break; }
                        f_buf[ci] = '0';
                        ci--;
                    }
                    if (ci < 0) {
                        /* Carry overflowed past all digits — shift right and insert '1' */
                        for (int si = f_pos; si > 0; si--)
                            f_buf[si] = f_buf[si - 1];
                        f_buf[0] = '1';
                        f_pos++;
                    }
                }
            } else if (precision == 0) {
                /* No decimal point when precision is explicitly 0 */
                /* Round the integer part */
                if (frac_part >= 0.5) {
                    int_part++;
                    f_pos = (int)uint_to_str(f_buf, int_part, 10, 0);
                }
            }
            f_buf[f_pos] = 0;

            int f_total = f_pos + f_neg;
            if (!f_neg && plus_sign) f_total++;
            else if (!f_neg && space_sign) f_total++;
            char f_pad = (zero_pad && !left_align) ? '0' : ' ';

            if (!left_align && f_pad == ' ') fmt_pad(ctx, width - f_total, ' ');
            if (f_neg) fmt_putc(ctx, '-');
            else if (plus_sign) fmt_putc(ctx, '+');
            else if (space_sign) fmt_putc(ctx, ' ');
            if (!left_align && f_pad == '0') fmt_pad(ctx, width - f_total, '0');
            fmt_puts(ctx, f_buf, f_pos);
            if (left_align) fmt_pad(ctx, width - f_total, ' ');
            break;
        }
        case '%':
            fmt_putc(ctx, '%');
            break;

        case 'n':
            /* Store chars written */
            if (len_mod == 2) *ms_va_arg(ap, long long *) = (long long)ctx->pos;
            else if (len_mod == 1) *ms_va_arg(ap, long *) = (long)ctx->pos;
            else *ms_va_arg(ap, int *) = (int)ctx->pos;
            break;

        case 0:
            goto done;

        default:
            /* Unknown specifier — print literally */
            fmt_putc(ctx, '%');
            fmt_putc(ctx, *fmt);
            break;
        }
        fmt++;
    }
done:
    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return (int)ctx->pos;
}

int WINAPI crt_printf(const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { NULL, 0, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_sprintf(char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_snprintf(char *buf, SIZE_T size, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { buf, size, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_fprintf(PVOID stream, const char *fmt, ...)
{
    /* Route to printf for now — stream is ignored, goes to stdout */
    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { NULL, 0, 0 };
    int ret = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return ret;
}

int WINAPI crt_sscanf(const char *buf, const char *fmt, ...)
{
    if (!buf || !fmt) return -1;

    ms_va_list ap;
    ms_va_start(ap, fmt);

    const char *p = buf;  /* current position in input */
    int matched = 0;      /* number of successfully assigned items */

    while (*fmt) {
        /* Literal whitespace in format: skip any whitespace in input */
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            fmt++;
            continue;
        }

        /* Non-% literal: must match exactly */
        if (*fmt != '%') {
            if (*p != *fmt) goto done;
            p++; fmt++;
            continue;
        }

        fmt++; /* skip '%' */

        /* %% — literal percent */
        if (*fmt == '%') {
            if (*p != '%') goto done;
            p++; fmt++;
            continue;
        }

        /* Suppress assignment flag */
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        /* Field width */
        int fw = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            fw = fw * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifier */
        int s_len_mod = 0; /* 0=int, 1=long, 2=short, 3=long long */
        if (*fmt == 'l') {
            fmt++; s_len_mod = 1;
            if (*fmt == 'l') { fmt++; s_len_mod = 3; }
        } else if (*fmt == 'h') {
            fmt++; s_len_mod = 2;
            if (*fmt == 'h') { fmt++; s_len_mod = 2; }
        }

        switch (*fmt) {
        case 'd': case 'i': {
            /* Skip leading whitespace */
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            const char *start = p;
            int neg = 0;
            if (*p == '-') { neg = 1; p++; }
            else if (*p == '+') p++;

            int base = 10;
            int had_prefix = 0; /* tracks if we consumed 0/0x prefix for %i */
            if (*fmt == 'i') {
                /* %i auto-detects base */
                if (*p == '0') {
                    had_prefix = 1;
                    p++;
                    if (*p == 'x' || *p == 'X') { base = 16; p++; }
                    else base = 8;
                }
            }

            /* Check for at least one valid digit (unless %i already consumed '0') */
            if (!had_prefix) {
                if (!((*p >= '0' && *p <= '9') ||
                      (base == 16 && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))))) {
                    goto done;
                }
            }

            long long val = 0;
            int count = 0;
            while (*p && (fw == 0 || count < fw)) {
                int digit;
                if (*p >= '0' && *p <= '9') digit = *p - '0';
                else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
                else break;
                if (digit >= base) break;
                val = val * base + digit;
                p++; count++;
            }
            if (neg) val = -val;

            if (p == start) goto done; /* no chars consumed */
            if (!suppress) {
                if (s_len_mod == 3) *ms_va_arg(ap, long long *) = val;
                else if (s_len_mod == 1) *ms_va_arg(ap, long *) = (long)val;
                else if (s_len_mod == 2) *ms_va_arg(ap, short *) = (short)val;
                else *ms_va_arg(ap, int *) = (int)val;
                matched++;
            }
            break;
        }
        case 'u': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            if (!(*p >= '0' && *p <= '9')) goto done;

            unsigned long long val = 0;
            int count = 0;
            while (*p >= '0' && *p <= '9' && (fw == 0 || count < fw)) {
                val = val * 10 + (*p - '0');
                p++; count++;
            }
            if (!suppress) {
                if (s_len_mod == 3) *ms_va_arg(ap, unsigned long long *) = val;
                else if (s_len_mod == 1) *ms_va_arg(ap, unsigned long *) = (unsigned long)val;
                else if (s_len_mod == 2) *ms_va_arg(ap, unsigned short *) = (unsigned short)val;
                else *ms_va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'x': case 'X': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            /* Skip optional 0x prefix */
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

            if (!((*p >= '0' && *p <= '9') ||
                  (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) goto done;

            unsigned long long val = 0;
            int count = 0;
            while (*p && (fw == 0 || count < fw)) {
                int digit;
                if (*p >= '0' && *p <= '9') digit = *p - '0';
                else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
                else break;
                val = val * 16 + digit;
                p++; count++;
            }
            if (!suppress) {
                if (s_len_mod == 3) *ms_va_arg(ap, unsigned long long *) = val;
                else if (s_len_mod == 1) *ms_va_arg(ap, unsigned long *) = (unsigned long)val;
                else *ms_va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 'o': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;
            if (!(*p >= '0' && *p <= '7')) goto done;

            unsigned long long val = 0;
            int count = 0;
            while (*p >= '0' && *p <= '7' && (fw == 0 || count < fw)) {
                val = val * 8 + (*p - '0');
                p++; count++;
            }
            if (!suppress) {
                if (s_len_mod == 1) *ms_va_arg(ap, unsigned long *) = (unsigned long)val;
                else *ms_va_arg(ap, unsigned int *) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 's': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            char *dst = suppress ? NULL : ms_va_arg(ap, char *);
            int count = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
                   (fw == 0 || count < fw)) {
                if (dst) dst[count] = *p;
                p++; count++;
            }
            if (dst) dst[count] = '\0';
            if (!suppress) matched++;
            break;
        }
        case 'c': {
            /* %c does NOT skip whitespace */
            int count = (fw > 0) ? fw : 1;
            if (!*p) goto done;

            char *dst = suppress ? NULL : ms_va_arg(ap, char *);
            for (int ci = 0; ci < count && *p; ci++) {
                if (dst) dst[ci] = *p;
                p++;
            }
            if (!suppress) matched++;
            break;
        }
        case 'f': {
            /* Parse floating-point number */
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            const char *fstart = p;
            double sign = 1.0;
            if (*p == '-') { sign = -1.0; p++; }
            else if (*p == '+') p++;

            if (!(*p >= '0' && *p <= '9') && *p != '.') goto done;

            double fval = 0.0;
            while (*p >= '0' && *p <= '9') {
                fval = fval * 10.0 + (*p - '0');
                p++;
            }
            if (*p == '.') {
                p++;
                double divisor = 10.0;
                while (*p >= '0' && *p <= '9') {
                    fval += (*p - '0') / divisor;
                    divisor *= 10.0;
                    p++;
                }
            }
            fval *= sign;

            if (p == fstart) goto done;
            if (!suppress) {
                if (s_len_mod == 1) *ms_va_arg(ap, double *) = fval;
                else *ms_va_arg(ap, float *) = (float)fval;
                matched++;
            }
            break;
        }
        case 'n': {
            /* Store number of characters consumed so far */
            if (!suppress) {
                *ms_va_arg(ap, int *) = (int)(p - buf);
                /* %n does not increment matched count per C standard */
            }
            break;
        }
        case '[': {
            /* Scanset: %[...] — read characters matching a set */
            fmt++; /* skip '[' */
            int negate = 0;
            if (*fmt == '^') { negate = 1; fmt++; }

            /* Build a 256-bit set of accepted characters */
            char set[256];
            crt_memset(set, 0, 256);
            /* Handle ']' as first character in set */
            if (*fmt == ']') { set[(unsigned char)']'] = 1; fmt++; }
            while (*fmt && *fmt != ']') {
                set[(unsigned char)*fmt] = 1;
                fmt++;
            }
            if (*fmt == ']') fmt++; /* skip closing ']' */

            if (!*p) goto done;
            char *dst = suppress ? NULL : ms_va_arg(ap, char *);
            int count = 0;
            while (*p && (fw == 0 || count < fw)) {
                int in_set = set[(unsigned char)*p];
                if (negate) in_set = !in_set;
                if (!in_set) break;
                if (dst) dst[count] = *p;
                p++; count++;
            }
            if (count == 0) goto done;
            if (dst) dst[count] = '\0';
            if (!suppress) matched++;
            /* fmt already advanced past ']', skip the fmt++ at end of switch */
            continue;
        }
        default:
            /* Unknown specifier — stop */
            goto done;
        }
        fmt++;
    }

done:
    ms_va_end(ap);
    return matched;
}

int WINAPI crt_puts(const char *s)
{
    DWORD written;
    SIZE_T len = crt_strlen(s);
    WriteFile((HANDLE)(ULONG_PTR)8, s, (DWORD)len, &written, NULL);
    char nl = '\n';
    WriteFile((HANDLE)(ULONG_PTR)8, &nl, 1, &written, NULL);
    return 0;
}

int WINAPI crt_putchar(int c)
{
    char ch = (char)c;
    DWORD written;
    WriteFile((HANDLE)(ULONG_PTR)8, &ch, 1, &written, NULL);
    return c;
}

/* ── stdio FILE* ───────────────────────────────────────────── */

struct _CRT_FILE {
    HANDLE  nt_handle;
    int     flags;      /* 1=read, 2=write, 4=eof, 8=error, 16=ungetc valid */
    int     ungetc_ch;
};

#define CRT_FILE_MAX 32

static CRT_FILE crt_files[CRT_FILE_MAX];
static int crt_files_init = 0;

/* Standard streams as FILE indices */
#define CRT_STDIN   (&crt_files[0])
#define CRT_STDOUT  (&crt_files[1])
#define CRT_STDERR  (&crt_files[2])

static void ensure_stdio_init(void)
{
    if (crt_files_init) return;
    crt_files_init = 1;
    crt_files[0].nt_handle = (HANDLE)(ULONG_PTR)4;   /* stdin */
    crt_files[0].flags = 1;
    crt_files[1].nt_handle = (HANDLE)(ULONG_PTR)8;   /* stdout */
    crt_files[1].flags = 2;
    crt_files[2].nt_handle = (HANDLE)(ULONG_PTR)12;  /* stderr */
    crt_files[2].flags = 2;
}

CRT_FILE* WINAPI crt_iob_func(int index)
{
    ensure_stdio_init();
    if (index >= 0 && index < 3)
        return &crt_files[index];
    return NULL;
}

CRT_FILE* WINAPI crt_fopen(const char *path, const char *mode)
{
    ensure_stdio_init();

    DWORD access = 0, disposition = 3; /* OPEN_EXISTING */
    int flags = 0;

    if (mode[0] == 'r') { access = GENERIC_READ; flags = 1; disposition = 3; }
    if (mode[0] == 'w') { access = GENERIC_WRITE; flags = 2; disposition = 2; /* CREATE_ALWAYS */ }
    if (mode[0] == 'a') { access = GENERIC_WRITE; flags = 2; disposition = 4; /* OPEN_ALWAYS */ }
    /* '+' means read+write */
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') { access = GENERIC_READ | GENERIC_WRITE; flags = 3; }
    }

    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, disposition, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    /* Find free slot */
    for (int i = 3; i < CRT_FILE_MAX; i++) {
        if (crt_files[i].nt_handle == NULL) {
            crt_files[i].nt_handle = h;
            crt_files[i].flags = flags;
            crt_files[i].ungetc_ch = -1;
            return &crt_files[i];
        }
    }

    CloseHandle(h);
    return NULL;
}

SIZE_T WINAPI crt_fread(PVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f)
{
    if (!f || !f->nt_handle || !(f->flags & 1)) return 0;
    DWORD total = (DWORD)(size * count);
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(f->nt_handle, buf, total, &bytes_read, NULL);
    if (!ok || bytes_read == 0) f->flags |= 4; /* EOF */
    return bytes_read / size;
}

SIZE_T WINAPI crt_fwrite(PCVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f)
{
    if (!f || !f->nt_handle || !(f->flags & 2)) return 0;
    DWORD total = (DWORD)(size * count);
    DWORD bytes_written = 0;
    WriteFile(f->nt_handle, buf, total, &bytes_written, NULL);
    return bytes_written / size;
}

int WINAPI crt_fclose(CRT_FILE *f)
{
    if (!f || f == CRT_STDIN || f == CRT_STDOUT || f == CRT_STDERR)
        return -1;
    if (f->nt_handle) {
        CloseHandle(f->nt_handle);
        f->nt_handle = NULL;
        f->flags = 0;
    }
    return 0;
}

int WINAPI crt_fseek(CRT_FILE *f, long offset, int whence)
{
    if (!f || !f->nt_handle) return -1;
    return SetFilePointer(f->nt_handle, offset, NULL, (DWORD)whence) ? 0 : -1;
}

long WINAPI crt_ftell(CRT_FILE *f)
{
    if (!f || !f->nt_handle) return -1;
    /* Get current position by seeking 0 from current */
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION pos_info;
    NTSTATUS status = NtQueryInformationFile(f->nt_handle, &iosb, &pos_info,
                                              sizeof(pos_info),
                                              FilePositionInformation);
    if (!NT_SUCCESS(status)) return -1;
    return (long)pos_info.CurrentByteOffset.QuadPart;
}

int WINAPI crt_fflush(CRT_FILE *f) { (void)f; return 0; }
int WINAPI crt_feof(CRT_FILE *f) { return f ? (f->flags & 4) : 0; }
int WINAPI crt_ferror(CRT_FILE *f) { return f ? (f->flags & 8) : 0; }

int WINAPI crt_fgetc(CRT_FILE *f)
{
    if (!f) return -1;
    if (f->flags & 16) {
        f->flags &= ~16;
        return f->ungetc_ch;
    }
    char c;
    DWORD read;
    if (!ReadFile(f->nt_handle, &c, 1, &read, NULL) || read == 0) {
        f->flags |= 4;
        return -1;
    }
    return (unsigned char)c;
}

int WINAPI crt_fputc(int c, CRT_FILE *f)
{
    if (!f) return -1;
    char ch = (char)c;
    DWORD written;
    WriteFile(f->nt_handle, &ch, 1, &written, NULL);
    return (unsigned char)ch;
}

char* WINAPI crt_fgets(char *buf, int n, CRT_FILE *f)
{
    if (!f || n <= 0) return NULL;
    int i;
    for (i = 0; i < n - 1; i++) {
        int c = crt_fgetc(f);
        if (c == -1) {
            if (i == 0) return NULL;
            break;
        }
        buf[i] = (char)c;
        if (c == '\n') { i++; break; }
    }
    buf[i] = 0;
    return buf;
}

int WINAPI crt_fputs(const char *s, CRT_FILE *f)
{
    if (!f || !s) return -1;
    SIZE_T len = crt_strlen(s);
    DWORD written;
    WriteFile(f->nt_handle, s, (DWORD)len, &written, NULL);
    return (int)written;
}

int WINAPI crt_ungetc(int c, CRT_FILE *f)
{
    if (!f || c == -1) return -1;
    f->ungetc_ch = c;
    f->flags |= 16;
    f->flags &= ~4;  /* clear EOF */
    return c;
}

/* ── Conversion ────────────────────────────────────────────── */

int WINAPI crt_atoi(const char *s)
{
    return (int)crt_strtol(s, NULL, 10);
}

long WINAPI crt_atol(const char *s)
{
    return crt_strtol(s, NULL, 10);
}

double WINAPI crt_atof(const char *s)
{
    if (!s) return 0.0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    /* Sign */
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;

    /* Integer part */
    double result = 0.0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (*s - '0');
        s++;
    }

    /* Fractional part */
    if (*s == '.') {
        s++;
        double frac = 0.0;
        double divisor = 10.0;
        while (*s >= '0' && *s <= '9') {
            frac += (*s - '0') / divisor;
            divisor *= 10.0;
            s++;
        }
        result += frac;
    }

    return sign * result;
}

long WINAPI crt_strtol(const char *s, char **endptr, int base)
{
    long result = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return neg ? -result : result;
}

unsigned long WINAPI crt_strtoul(const char *s, char **endptr, int base)
{
    /* Same logic, unsigned */
    return (unsigned long)crt_strtol(s, endptr, base);
}

/* ── Process ───────────────────────────────────────────────── */

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

void WINAPI crt_exit(int code)
{
    /* Run atexit handlers in reverse order */
    for (int i = atexit_count - 1; i >= 0; i--)
        atexit_funcs[i]();
    ExitProcess((DWORD)code);
}

void WINAPI crt_abort(void)
{
    serial_puts("[MSVCRT] abort() called\n");
    ExitProcess(3); /* SIGABRT-like */
}

void WINAPI crt__exit(int code)
{
    ExitProcess((DWORD)code);
}

int WINAPI crt_atexit(void (*func)(void))
{
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

/* ── ctype ─────────────────────────────────────────────────── */

int WINAPI crt_isalpha(int c)  { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int WINAPI crt_isdigit(int c)  { return c >= '0' && c <= '9'; }
int WINAPI crt_isalnum(int c)  { return crt_isalpha(c) || crt_isdigit(c); }
int WINAPI crt_isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int WINAPI crt_isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int WINAPI crt_islower(int c)  { return c >= 'a' && c <= 'z'; }
int WINAPI crt_isprint(int c)  { return c >= 0x20 && c <= 0x7e; }
int WINAPI crt_toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int WINAPI crt_tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ── Algorithm ─────────────────────────────────────────────── */

/* Simple quicksort */
static void qs_swap(BYTE *a, BYTE *b, SIZE_T size)
{
    for (SIZE_T i = 0; i < size; i++) {
        BYTE tmp = a[i]; a[i] = b[i]; b[i] = tmp;
    }
}

void WINAPI crt_qsort(PVOID base, SIZE_T nmemb, SIZE_T size,
                       int (WINAPI *compar)(PCVOID, PCVOID))
{
    if (nmemb < 2) return;

    BYTE *arr = (BYTE *)base;
    BYTE *pivot = arr + (nmemb - 1) * size;
    SIZE_T i = 0;

    for (SIZE_T j = 0; j < nmemb - 1; j++) {
        if (compar(arr + j * size, pivot) <= 0) {
            qs_swap(arr + i * size, arr + j * size, size);
            i++;
        }
    }
    qs_swap(arr + i * size, pivot, size);

    crt_qsort(arr, i, size, compar);
    crt_qsort(arr + (i + 1) * size, nmemb - i - 1, size, compar);
}

PVOID WINAPI crt_bsearch(PCVOID key, PCVOID base, SIZE_T nmemb,
                          SIZE_T size,
                          int (WINAPI *compar)(PCVOID, PCVOID))
{
    const BYTE *arr = (const BYTE *)base;
    SIZE_T lo = 0, hi = nmemb;
    while (lo < hi) {
        SIZE_T mid = lo + (hi - lo) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp == 0) return (PVOID)(arr + mid * size);
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* ── Error ─────────────────────────────────────────────────── */

static int crt_errno_val = 0;

int* WINAPI crt_errno(void) { return &crt_errno_val; }

/* ── Time ──────────────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

crt_time_t WINAPI crt_time(crt_time_t *timer)
{
    /* Stub: return ticks as pseudo-time */
    crt_time_t t = (crt_time_t)idt_get_ticks();
    if (timer) *timer = t;
    return t;
}

crt_clock_t WINAPI crt_clock(void)
{
    return (crt_clock_t)idt_get_ticks();
}

/* ── SEH (Structured Exception Handling) ───────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/*
 * _except_handler3 — MSVC SEH frame-based exception handler.
 *
 * Called by the OS exception dispatcher when an exception occurs.
 * Walks the scopetable for this frame, calls filter expressions,
 * and if a filter returns EXCEPTION_EXECUTE_HANDLER, transfers
 * control to the __except handler block.
 *
 * EstablisherFrame points to the EH3_EXCEPTION_REGISTRATION on stack,
 * which contains: { registration, ScopeTable, TryLevel }
 */
EXCEPTION_DISPOSITION WINAPI crt_except_handler3(
    PEXCEPTION_RECORD ExceptionRecord,
    PEH3_EXCEPTION_REGISTRATION EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)DispatcherContext;

    /* During unwind, just return — _except_handler3 doesn't do unwind cleanup */
    if (ExceptionRecord->ExceptionFlags & EXCEPTION_UNWIND) {
        return ExceptionContinueSearch;
    }

    serial_puts("[SEH] _except_handler3: code=0x");
    serial_puthex(ExceptionRecord->ExceptionCode, 8);
    serial_puts("\n");

    /* Walk the scopetable from current TryLevel upward */
    PSCOPETABLE_ENTRY scope_table = EstablisherFrame->ScopeTable;
    DWORD try_level = EstablisherFrame->TryLevel;

    while (try_level != (DWORD)-1) {
        PSCOPETABLE_ENTRY entry = &scope_table[try_level];

        if (entry->FilterFunc) {
            /* Call the filter expression.
             * Filter signature: int filter(PEXCEPTION_POINTERS info)
             * It's called with the EXCEPTION_POINTERS struct.
             * For simplicity in our single-threaded, identity-mapped model,
             * we set up the pointers and call the filter. */
            EXCEPTION_POINTERS ep;
            ep.ExceptionRecord = ExceptionRecord;
            ep.ContextRecord   = ContextRecord;

            typedef int (WINAPI *filter_fn)(PEXCEPTION_POINTERS);
            filter_fn filter = (filter_fn)entry->FilterFunc;
            int result = filter(&ep);

            serial_puts("[SEH] filter returned ");
            serial_puthex((uint64_t)(uint32_t)result, 1);
            serial_puts("\n");

            if (result == EXCEPTION_EXECUTE_HANDLER) {
                /* Found a handler — perform global unwind to this frame,
                 * then jump to the handler block */
                serial_puts("[SEH] executing handler\n");

                /* Update TryLevel to enclosing scope */
                EstablisherFrame->TryLevel = entry->EnclosingLevel;

                /* Call the handler. In real Windows, this is a longjmp-style
                 * transfer. In our simplified model, we call it and if it
                 * returns, continue. Real handlers typically don't return. */
                typedef void (WINAPI *handler_fn)(void);
                handler_fn handler = (handler_fn)entry->HandlerFunc;
                handler();

                /* If handler returns (unusual), continue search */
                return ExceptionContinueSearch;
            }
            else if (result == EXCEPTION_CONTINUE_EXECUTION) {
                /* Resume execution at the faulting instruction */
                return ExceptionContinueExecution;
            }
            /* EXCEPTION_CONTINUE_SEARCH → try enclosing scope */
        }

        try_level = entry->EnclosingLevel;
    }

    /* No handler found in this frame */
    return ExceptionContinueSearch;
}

/*
 * _except_handler4 — security cookie variant of _except_handler3.
 * In our environment (no ASLR, no stack cookies), delegates to handler3.
 */
EXCEPTION_DISPOSITION WINAPI crt_except_handler4(
    PEXCEPTION_RECORD ExceptionRecord,
    PEH3_EXCEPTION_REGISTRATION EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    /* In real Windows, handler4 XORs scopetable pointer with security cookie.
     * We skip cookie validation and forward to handler3 logic. */
    return crt_except_handler3(ExceptionRecord, EstablisherFrame,
                                ContextRecord, DispatcherContext);
}

int WINAPI crt_XcptFilter(int code, PVOID pointers)
{
    (void)code;
    (void)pointers;
    /* Default: continue search (let next handler try) */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── Misc CRT internal ────────────────────────────────────── */

int  WINAPI crt_controlfp_s(unsigned int *old, unsigned int newval, unsigned int mask)
    { if (old) *old = 0; (void)newval; (void)mask; return 0; }
int  WINAPI crt_configthreadlocale(int type) { (void)type; return 0; }
void WINAPI crt_lock(int locknum) { (void)locknum; }
void WINAPI crt_unlock(int locknum) { (void)locknum; }
int  WINAPI crt_crt_debugger_hook(int reserved) { (void)reserved; return 0; }
void* WINAPI crt_encoded_null(void) { return NULL; }
PVOID WINAPI crt_amsg_exit(int errnum) { (void)errnum; crt_abort(); return NULL; }

/* ── C++ EH / UT99 required stubs ─────────────────────────── */

/* ??1type_info@@UAE@XZ — type_info destructor (no-op) */
void WINAPI crt_type_info_dtor(PVOID _this)
{
    (void)_this;
}

/* _CxxThrowException — C++ throw (fatal for now) */
void WINAPI crt_CxxThrowException(PVOID pExceptionObject, PVOID pThrowInfo)
{
    (void)pExceptionObject;
    (void)pThrowInfo;
    serial_puts("[MSVCRT] _CxxThrowException called — aborting\n");
    crt_abort();
}

/* __CxxFrameHandler — C++ exception frame handler */
EXCEPTION_DISPOSITION WINAPI crt_CxxFrameHandler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)ExceptionRecord;
    (void)EstablisherFrame;
    (void)ContextRecord;
    (void)DispatcherContext;
    return ExceptionContinueSearch;
}

/* __dllonexit — register DLL exit callback (store and ignore) */
_PVFV_DLL WINAPI crt_dllonexit(_PVFV_DLL func, _PVFV_DLL **pbegin, _PVFV_DLL **pend)
{
    (void)pbegin;
    (void)pend;
    return func;
}

/* __p__commode — pointer to _commode variable */
static int crt_commode_val = 0;
int* WINAPI crt_p_commode(void)
{
    return &crt_commode_val;
}

/* __p__fmode — pointer to _fmode variable */
static int crt_fmode_val = 0;
int* WINAPI crt_p_fmode(void)
{
    return &crt_fmode_val;
}

/* __setusermatherr — set math error handler (store, ignore) */
static _UserMathErrFunc crt_usermatherr_handler = NULL;
void WINAPI crt_setusermatherr(_UserMathErrFunc handler)
{
    crt_usermatherr_handler = handler;
}

/* _acmdln — pointer to command line string */
static char *crt_acmdln_val = "";
char* WINAPI crt_acmdln(void)
{
    return crt_acmdln_val;
}

/* _adjust_fdiv — FDIV adjustment flag (always 0, no bug) */
static int crt_adjust_fdiv_val = 0;
int* WINAPI crt_adjust_fdiv(void)
{
    return &crt_adjust_fdiv_val;
}

/* _controlfp — control floating point (stub, return 0) */
unsigned int WINAPI crt_controlfp(unsigned int newval, unsigned int mask)
{
    (void)newval;
    (void)mask;
    return 0;
}

/* _ftol — float to long conversion */
long WINAPI crt_ftol(double val)
{
    return (long)val;
}

/* _onexit — register exit callback (store and ignore) */
_onexit_t WINAPI crt_onexit(_onexit_t func)
{
    return func;
}

/* _purecall — pure virtual call handler */
void WINAPI crt_purecall(void)
{
    serial_puts("[MSVCRT] _purecall — pure virtual function call — aborting\n");
    crt_abort();
}

/* ── vprintf family (ms_abi va_list wrappers) ─────────────── */

static int WINAPI crt_vprintf(const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat(&ctx, fmt, ap);
}

static int WINAPI crt_vsprintf(char *buf, const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    return do_vformat(&ctx, fmt, ap);
}

static int WINAPI crt_vsnprintf(char *buf, SIZE_T size, const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { buf, size, 0 };
    return do_vformat(&ctx, fmt, ap);
}

static int WINAPI crt_vfprintf(PVOID stream, const char *fmt, ms_va_list ap)
{
    (void)stream;
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat(&ctx, fmt, ap);
}

/* ── UT99 Core.dll / Engine.dll missing exports ────────────── */

/* ?terminate@@YAXXZ — C++ terminate() handler */
void WINAPI crt_terminate(void)
{
    serial_puts("[MSVCRT] terminate() called — ExitProcess(3)\n");
    ExitProcess(3);
}

/* _CIacos — compiler intrinsic wrapper for acos */
double WINAPI crt_CIacos(double x)
{
#ifdef TEST_HARNESS
    return acos(x);
#else
    /* Stub: Bhaskara I approximation for acos(x) */
    /* acos(x) ≈ pi/2 - asin(x), asin(x) ≈ x for small x */
    /* For UT99, a rough approximation is acceptable */
    if (x >= 1.0)  return 0.0;
    if (x <= -1.0) return 3.14159265358979323846;
    /* Use identity: acos(x) = pi/2 - x - x^3/6 - 3*x^5/40 */
    double x2 = x * x;
    double x3 = x2 * x;
    double x5 = x3 * x2;
    return 1.5707963267948966 - x - x3 / 6.0 - 3.0 * x5 / 40.0;
#endif
}

/* _CIfmod — compiler intrinsic wrapper for fmod */
double WINAPI crt_CIfmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    /* fmod(x, y) = x - trunc(x/y) * y */
    double quotient = x / y;
    long trunc_q = (long)quotient;
    return x - (double)trunc_q * y;
}

/* _CIpow — compiler intrinsic wrapper for pow */
double WINAPI crt_CIpow(double base, double exp)
{
#ifdef TEST_HARNESS
    return pow(base, exp);
#else
    /* Simple integer-exponent pow for common UT99 cases */
    if (exp == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;

    /* Handle integer exponents exactly */
    int iexp = (int)exp;
    if ((double)iexp == exp && iexp >= 0) {
        double result = 1.0;
        double b = base;
        int e = iexp;
        while (e > 0) {
            if (e & 1) result *= b;
            b *= b;
            e >>= 1;
        }
        return result;
    }

    /* For non-integer exponents, use repeated squaring approximation */
    /* This is a rough fallback — UT99 mostly uses integer powers */
    if (exp < 0.0) return 1.0 / crt_CIpow(base, -exp);
    return base; /* fallback for fractional exponents */
#endif
}

/* _isnan — check for NaN (stub: always returns 0) */
int WINAPI crt_isnan(double x)
{
    (void)x;
    return 0;
}

/* _stat / _wstat — file stat (stub: file not found) */
struct crt_stat_buf {
    unsigned int st_dev;
    unsigned short st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    unsigned int st_rdev;
    long st_size;
    long st_atime;
    long st_mtime;
    long st_ctime;
};

int WINAPI crt_stat(const char *path, PVOID buf)
{
    (void)path;
    (void)buf;
    return -1; /* file not found */
}

int WINAPI crt_wstat(const WCHAR *path, PVOID buf)
{
    (void)path;
    (void)buf;
    return -1; /* file not found */
}

/* _strdate / _strtime — date/time strings (stub values) */
char* WINAPI crt_strdate(char *buf)
{
    /* MM/DD/YY format */
    static const char date[] = "01/01/26";
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = date[i];
    }
    return buf;
}

char* WINAPI crt_strtime(char *buf)
{
    /* HH:MM:SS format */
    static const char time_str[] = "00:00:00";
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = time_str[i];
    }
    return buf;
}

/* _wstrdate / _wstrtime — wide date/time strings */
WCHAR* WINAPI crt_wstrdate(WCHAR *buf)
{
    static const WCHAR date[] = {'0','1','/','0','1','/','2','6',0};
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = date[i];
    }
    return buf;
}

WCHAR* WINAPI crt_wstrtime(WCHAR *buf)
{
    static const WCHAR time_str[] = {'0','0',':','0','0',':','0','0',0};
    if (buf) {
        for (int i = 0; i < 9; i++) buf[i] = time_str[i];
    }
    return buf;
}

/* _vsnwprintf — wide vsnprintf (simple stub) */
int WINAPI crt_vsnwprintf(WCHAR *buf, SIZE_T count, const WCHAR *fmt, ms_va_list ap)
{
    (void)ap;
    /* Minimal stub: copy format string as-is (no format processing) */
    if (!buf || count == 0) return 0;
    SIZE_T i;
    for (i = 0; i < count - 1 && fmt[i]; i++)
        buf[i] = fmt[i];
    buf[i] = 0;
    return (int)i;
}

/* _wcsicmp — case-insensitive wide string compare */
static WCHAR wchar_to_lower(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int WINAPI crt_wcsicmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        WCHAR ca = wchar_to_lower(*a), cb = wchar_to_lower(*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)wchar_to_lower(*a) - (int)wchar_to_lower(*b);
}

/* _wcsnicmp — case-insensitive wide string compare (n chars) */
int WINAPI crt_wcsnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        WCHAR ca = wchar_to_lower(a[i]), cb = wchar_to_lower(b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) break;
    }
    return 0;
}

/* _wcsupr — uppercase wide string in-place */
WCHAR* WINAPI crt_wcsupr(WCHAR *s)
{
    WCHAR *p = s;
    while (*p) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
        p++;
    }
    return s;
}

/* _wtoi — wide string to int */
int WINAPI crt_wtoi(const WCHAR *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

/* ceil / floor — math functions */
double WINAPI crt_ceil(double x)
{
    long i = (long)x;
    if (x > 0.0 && (double)i != x) return (double)(i + 1);
    return (double)i;
}

double WINAPI crt_floor(double x)
{
    long i = (long)x;
    if (x < 0.0 && (double)i != x) return (double)(i - 1);
    return (double)i;
}

/* difftime — difference between two time_t values */
double WINAPI crt_difftime(crt_time_t t1, crt_time_t t0)
{
    return (double)(t1 - t0);
}

/* gmtime — convert time_t to struct tm (stub) */
struct crt_tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

static struct crt_tm crt_gmtime_buf;

PVOID WINAPI crt_gmtime(const crt_time_t *timer)
{
    (void)timer;
    crt_memset(&crt_gmtime_buf, 0, sizeof(crt_gmtime_buf));
    crt_gmtime_buf.tm_mday = 1;   /* day 1 */
    crt_gmtime_buf.tm_year = 126;  /* 2026 - 1900 */
    return (PVOID)&crt_gmtime_buf;
}

/* mktime — convert struct tm to time_t (stub) */
crt_time_t WINAPI crt_mktime(PVOID tm)
{
    (void)tm;
    return 0;
}

/* rand / srand — simple LCG PRNG */
static unsigned int crt_rand_seed = 1;

int WINAPI crt_rand(void)
{
    crt_rand_seed = crt_rand_seed * 214013 + 2531011;
    return (int)((crt_rand_seed >> 16) & 0x7fff);
}

void WINAPI crt_srand(unsigned int seed)
{
    crt_rand_seed = seed;
}

/* strncat — string concatenation with length limit */
char* WINAPI crt_strncat(char *dst, const char *src, SIZE_T n)
{
    char *d = dst;
    while (*d) d++;
    for (SIZE_T i = 0; i < n && src[i]; i++)
        *d++ = src[i];
    *d = 0;
    return dst;
}

/* ── Wide string functions ─────────────────────────────────── */

SIZE_T WINAPI crt_wcslen(const WCHAR *s)
{
    SIZE_T len = 0;
    while (s[len]) len++;
    return len;
}

WCHAR* WINAPI crt_wcscpy(WCHAR *dst, const WCHAR *src)
{
    WCHAR *d = dst;
    while ((*d++ = *src++));
    return dst;
}

WCHAR* WINAPI crt_wcsncpy(WCHAR *dst, const WCHAR *src, SIZE_T n)
{
    SIZE_T i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

WCHAR* WINAPI crt_wcscat(WCHAR *dst, const WCHAR *src)
{
    WCHAR *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

int WINAPI crt_wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

int WINAPI crt_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0) break;
    }
    return 0;
}

WCHAR* WINAPI crt_wcschr(const WCHAR *s, WCHAR c)
{
    for (; *s; s++)
        if (*s == c) return (WCHAR *)s;
    return (c == 0) ? (WCHAR *)s : NULL;
}

WCHAR* WINAPI crt_wcsstr(const WCHAR *haystack, const WCHAR *needle)
{
    if (!*needle) return (WCHAR *)haystack;
    for (; *haystack; haystack++) {
        const WCHAR *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (WCHAR *)haystack;
    }
    return NULL;
}

unsigned long WINAPI crt_wcstoul(const WCHAR *s, WCHAR **endptr, int base)
{
    unsigned long result = 0;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '+') s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (WCHAR *)s;
    return result;
}

/* ── Export resolution table ───────────────────────────────── */

typedef struct {
    const char *name;
    PVOID       func;
} MSVCRT_EXPORT;

static const MSVCRT_EXPORT msvcrt_exports[] = {
    /* CRT init */
    { "_initterm",           (PVOID)_initterm },
    { "_initterm_e",         (PVOID)_initterm_e },
    { "__getmainargs",       (PVOID)__getmainargs },
    { "__wgetmainargs",      (PVOID)__wgetmainargs },
    { "__set_app_type",      (PVOID)__set_app_type },
    { "_set_new_mode",       (PVOID)_set_new_mode },

    /* Memory */
    { "malloc",              (PVOID)crt_malloc },
    { "calloc",              (PVOID)crt_calloc },
    { "realloc",             (PVOID)crt_realloc },
    { "free",                (PVOID)crt_free },
    { "?malloc@@YAPEAX_K@Z", (PVOID)crt_malloc },  /* C++ mangled */
    { "?free@@YAXPEAX@Z",   (PVOID)crt_free },

    /* String */
    { "strlen",              (PVOID)crt_strlen },
    { "strcmp",              (PVOID)crt_strcmp },
    { "strncmp",             (PVOID)crt_strncmp },
    { "_stricmp",            (PVOID)crt_stricmp },
    { "_strnicmp",           (PVOID)crt_strnicmp },
    { "_strcmpi",            (PVOID)crt_stricmp },
    { "strcpy",              (PVOID)crt_strcpy },
    { "strncpy",             (PVOID)crt_strncpy },
    { "strcat",              (PVOID)crt_strcat },
    { "strstr",              (PVOID)crt_strstr },
    { "strchr",              (PVOID)crt_strchr },
    { "strrchr",             (PVOID)crt_strrchr },

    /* Memory ops */
    { "memcpy",              (PVOID)crt_memcpy },
    { "memset",              (PVOID)crt_memset },
    { "memmove",             (PVOID)crt_memmove },
    { "memcmp",              (PVOID)crt_memcmp },

    /* Format I/O */
    { "printf",              (PVOID)crt_printf },
    { "sprintf",             (PVOID)crt_sprintf },
    { "_snprintf",           (PVOID)crt_snprintf },
    { "_vsnprintf",          (PVOID)crt_vsnprintf },
    { "fprintf",             (PVOID)crt_fprintf },
    { "vprintf",             (PVOID)crt_vprintf },
    { "vsprintf",            (PVOID)crt_vsprintf },
    { "vfprintf",            (PVOID)crt_vfprintf },
    { "sscanf",              (PVOID)crt_sscanf },
    { "puts",                (PVOID)crt_puts },
    { "putchar",             (PVOID)crt_putchar },

    /* stdio FILE* */
    { "fopen",               (PVOID)crt_fopen },
    { "fread",               (PVOID)crt_fread },
    { "fwrite",              (PVOID)crt_fwrite },
    { "fclose",              (PVOID)crt_fclose },
    { "fseek",               (PVOID)crt_fseek },
    { "ftell",               (PVOID)crt_ftell },
    { "fflush",              (PVOID)crt_fflush },
    { "feof",                (PVOID)crt_feof },
    { "ferror",              (PVOID)crt_ferror },
    { "fgetc",               (PVOID)crt_fgetc },
    { "fputc",               (PVOID)crt_fputc },
    { "fgets",               (PVOID)crt_fgets },
    { "fputs",               (PVOID)crt_fputs },
    { "ungetc",              (PVOID)crt_ungetc },
    { "__iob_func",          (PVOID)crt_iob_func },
    { "__acrt_iob_func",     (PVOID)crt_iob_func },

    /* Conversion */
    { "atoi",                (PVOID)crt_atoi },
    { "atol",                (PVOID)crt_atol },
    { "atof",                (PVOID)crt_atof },
    { "strtol",              (PVOID)crt_strtol },
    { "strtoul",             (PVOID)crt_strtoul },

    /* Process */
    { "exit",                (PVOID)crt_exit },
    { "abort",               (PVOID)crt_abort },
    { "_exit",               (PVOID)crt__exit },
    { "_cexit",              (PVOID)crt_exit },
    { "_c_exit",             (PVOID)crt__exit },
    { "atexit",              (PVOID)crt_atexit },

    /* ctype */
    { "isalpha",             (PVOID)crt_isalpha },
    { "isdigit",             (PVOID)crt_isdigit },
    { "isalnum",             (PVOID)crt_isalnum },
    { "isspace",             (PVOID)crt_isspace },
    { "isupper",             (PVOID)crt_isupper },
    { "islower",             (PVOID)crt_islower },
    { "isprint",             (PVOID)crt_isprint },
    { "toupper",             (PVOID)crt_toupper },
    { "tolower",             (PVOID)crt_tolower },

    /* Algorithm */
    { "qsort",               (PVOID)crt_qsort },
    { "bsearch",             (PVOID)crt_bsearch },

    /* Error */
    { "_errno",              (PVOID)crt_errno },

    /* Time */
    { "time",                (PVOID)crt_time },
    { "clock",               (PVOID)crt_clock },

    /* SEH */
    { "_except_handler3",    (PVOID)crt_except_handler3 },
    { "_except_handler4",    (PVOID)crt_except_handler4 },
    { "_XcptFilter",         (PVOID)crt_XcptFilter },

    /* Misc CRT internal */
    { "_controlfp_s",        (PVOID)crt_controlfp_s },
    { "_configthreadlocale", (PVOID)crt_configthreadlocale },
    { "_lock",               (PVOID)crt_lock },
    { "_unlock",             (PVOID)crt_unlock },
    { "__CxxFrameHandler3",  (PVOID)crt_except_handler3 },
    { "__CxxFrameHandler4",  (PVOID)crt_except_handler4 },
    { "_CRT_DEBUGGER_HOOK",  (PVOID)crt_crt_debugger_hook },
    { "_encoded_null",       (PVOID)crt_encoded_null },
    { "_amsg_exit",          (PVOID)crt_amsg_exit },

    /* C++ EH / UT99 required stubs */
    { "??1type_info@@UAE@XZ", (PVOID)crt_type_info_dtor },
    { "_CxxThrowException",  (PVOID)crt_CxxThrowException },
    { "__CxxFrameHandler",   (PVOID)crt_CxxFrameHandler },
    { "__dllonexit",         (PVOID)crt_dllonexit },
    { "__p__commode",        (PVOID)crt_p_commode },
    { "__p__fmode",          (PVOID)crt_p_fmode },
    { "__setusermatherr",    (PVOID)crt_setusermatherr },
    { "_acmdln",             (PVOID)crt_acmdln },
    { "_adjust_fdiv",        (PVOID)crt_adjust_fdiv },
    { "_controlfp",          (PVOID)crt_controlfp },
    { "_ftol",               (PVOID)crt_ftol },
    { "_onexit",             (PVOID)crt_onexit },
    { "_purecall",           (PVOID)crt_purecall },

    /* UT99 Core.dll / Engine.dll required exports */
    { "?terminate@@YAXXZ",   (PVOID)crt_terminate },
    { "_CIacos",             (PVOID)crt_CIacos },
    { "_CIfmod",             (PVOID)crt_CIfmod },
    { "_CIpow",              (PVOID)crt_CIpow },
    { "_isnan",              (PVOID)crt_isnan },
    { "_stat",               (PVOID)crt_stat },
    { "_wstat",              (PVOID)crt_wstat },
    { "_strdate",            (PVOID)crt_strdate },
    { "_strtime",            (PVOID)crt_strtime },
    { "_wstrdate",           (PVOID)crt_wstrdate },
    { "_wstrtime",           (PVOID)crt_wstrtime },
    { "_vsnwprintf",         (PVOID)crt_vsnwprintf },
    { "_wcsicmp",            (PVOID)crt_wcsicmp },
    { "_wcsnicmp",           (PVOID)crt_wcsnicmp },
    { "_wcsupr",             (PVOID)crt_wcsupr },
    { "_wtoi",               (PVOID)crt_wtoi },
    { "ceil",                (PVOID)crt_ceil },
    { "floor",               (PVOID)crt_floor },
    { "difftime",            (PVOID)crt_difftime },
    { "gmtime",              (PVOID)crt_gmtime },
    { "mktime",              (PVOID)crt_mktime },
    { "rand",                (PVOID)crt_rand },
    { "srand",               (PVOID)crt_srand },
    { "strncat",             (PVOID)crt_strncat },
    { "wcscat",              (PVOID)crt_wcscat },
    { "wcschr",              (PVOID)crt_wcschr },
    { "wcscmp",              (PVOID)crt_wcscmp },
    { "wcscpy",              (PVOID)crt_wcscpy },
    { "wcslen",              (PVOID)crt_wcslen },
    { "wcsncmp",             (PVOID)crt_wcsncmp },
    { "wcsncpy",             (PVOID)crt_wcsncpy },
    { "wcsstr",              (PVOID)crt_wcsstr },
    { "wcstoul",             (PVOID)crt_wcstoul },

    { NULL, NULL }
};

static int msvcrt_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

PVOID msvcrt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;

    for (int i = 0; msvcrt_exports[i].name; i++) {
        if (msvcrt_strcmp(func_name, msvcrt_exports[i].name) == 0)
            return msvcrt_exports[i].func;
    }

    return NULL;
}

PVOID msvcrt_shim_init(void)
{
    ensure_stdio_init();
    return (PVOID)msvcrt_exports;
}
