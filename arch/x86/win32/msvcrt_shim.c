/*
 * OsitoK Windows Compatibility Layer — msvcrt.dll Shim Implementation
 *
 * Microsoft C Runtime functions used by CRT-linked Windows executables.
 * printf uses a custom formatter; memory uses our heap; stdio wraps NT I/O.
 */

#include "msvcrt_shim.h"
#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "compat32.h"
#include "unwind64.h"
#include "win32_abi.h"
#include "wintime.h"
#include "../fs/vfs.h"
#include "../include/paging.h"

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
extern void serial_putchar(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void proc_exit(int32_t code);
extern void compat32_callback(uint32_t func_addr);
extern void sched_yield(void);
extern DWORD win32_current_process_id(void);
extern const char *win32_current_command_line(void);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);

/* ── CRT Initialization ────────────────────────────────────── */

/*
 * _initterm walks an array of function pointers and calls each non-NULL one.
 * CRITICAL: PE32 (i386) arrays contain 4-byte function pointers, but our
 * 64-bit code would read them as 8-byte pointers, combining pairs and
 * calling garbage addresses. We detect this by checking if the pointers
 * are in low memory (<4GB) and iterate with 4-byte steps.
 *
 * Additionally, these function pointers are 32-bit compat-mode code that
 * cannot be called directly from 64-bit. We use compat32_callback() which
 * switches to compat mode, calls the function, and returns.
 */

void WINAPI _initterm(_PVFV *pfbegin, _PVFV *pfend)
{
#ifndef TEST_HARNESS
    if (!g_compat32_mode) {
        typedef void (WINAPI *init_fn64_t)(void);
        uint64_t *begin64 = (uint64_t *)(ULONG_PTR)pfbegin;
        uint64_t *end64   = (uint64_t *)(ULONG_PTR)pfend;

        serial_puts("[MSVCRT] _initterm64: ");
        serial_putdec((uint64_t)(end64 - begin64));
        serial_puts(" entries at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)begin64, 16);
        serial_puts("\n");

        for (uint64_t *p = begin64; p < end64; p++) {
            if (*p)
                ((init_fn64_t)(ULONG_PTR)*p)();
        }
        return;
    }

    /* PE32 mode: treat as array of uint32_t function pointers */
    uint32_t *begin32 = (uint32_t *)(ULONG_PTR)pfbegin;
    uint32_t *end32   = (uint32_t *)(ULONG_PTR)pfend;

    serial_puts("[MSVCRT] _initterm: ");
    serial_putdec((uint64_t)(end32 - begin32));
    serial_puts(" entries at 0x");
    serial_puthex((uint64_t)(ULONG_PTR)begin32, 8);
    serial_puts("\n");

    int cb_count = 0;
    int idx = 0;
    /* Audit mode: trace each callback when the array is small or when the
     * skip ratio is suspicious. Prints "INIT[N] @0x... → ok" per callback. */
    int audit_mode = (end32 - begin32 < 200);
    for (uint32_t *p = begin32; p < end32; p++, idx++) {
        if (*p) {
            if (audit_mode) {
                serial_puts("[INIT] [");
                serial_putdec((uint64_t)idx);
                serial_puts("] @0x");
                serial_puthex(*p, 8);
                serial_puts("\n");
            }
            compat32_callback(*p);
            cb_count++;
            if (audit_mode) {
                serial_puts("[INIT] [");
                serial_putdec((uint64_t)idx);
                serial_puts("] returned\n");
            }
        }
    }
    serial_puts("[MSVCRT] _initterm done: ");
    serial_putdec(cb_count);
    serial_puts(" callbacks executed\n");

#else
    for (_PVFV *pfn = pfbegin; pfn < pfend; pfn++) {
        if (*pfn)
            (*pfn)();
    }
#endif
}

int WINAPI _initterm_e(_PIFV *pfbegin, _PIFV *pfend)
{
#ifndef TEST_HARNESS
    if (!g_compat32_mode) {
        typedef int (WINAPI *init_fn64_t)(void);
        uint64_t *begin64 = (uint64_t *)(ULONG_PTR)pfbegin;
        uint64_t *end64   = (uint64_t *)(ULONG_PTR)pfend;

        serial_puts("[MSVCRT] _initterm_e64: ");
        serial_putdec((uint64_t)(end64 - begin64));
        serial_puts(" entries at 0x");
        serial_puthex((uint64_t)(ULONG_PTR)begin64, 16);
        serial_puts("\n");

        for (uint64_t *p = begin64; p < end64; p++) {
            if (*p) {
                int ret = ((init_fn64_t)(ULONG_PTR)*p)();
                if (ret != 0)
                    return ret;
            }
        }
        return 0;
    }

    /* PE32 mode: same 32-bit pointer handling */
    uint32_t *begin32 = (uint32_t *)(ULONG_PTR)pfbegin;
    uint32_t *end32   = (uint32_t *)(ULONG_PTR)pfend;

    serial_puts("[MSVCRT] _initterm_e: ");
    serial_putdec((uint64_t)(end32 - begin32));
    serial_puts(" entries at 0x");
    serial_puthex((uint64_t)(ULONG_PTR)begin32, 8);
    serial_puts("\n");

    for (uint32_t *p = begin32; p < end32; p++) {
        if (*p) {
            /* For _initterm_e, we can't easily get the return value
             * from a 32-bit callback. Treat as void for now. */
            compat32_callback(*p);
        }
    }
    return 0;
#else
    for (_PIFV *pfn = pfbegin; pfn < pfend; pfn++) {
        if (*pfn) {
            int ret = (*pfn)();
            if (ret != 0)
                return ret;
        }
    }
    return 0;
#endif
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

int WINAPI __crtGetShowWindowMode(void) { return 10; /* SW_SHOWDEFAULT */ }
void WINAPI __set_app_type(int type) { (void)type; serial_puts("[CRT] __set_app_type\n"); }
static int crt_exchange_new_mode(int mode);
static PVOID crt_exchange_new_handler(PVOID handler);

int WINAPI _set_new_mode(int mode)
{
    return crt_exchange_new_mode(mode ? 1 : 0);
}

PVOID WINAPI crt_set_new_handler(PVOID handler)
{
    return crt_exchange_new_handler(handler);
}

/* ── Heap-backed malloc/free ───────────────────────────────── */

/* CRT allocations use the Win32 process heap. PE32 callers therefore receive
 * mapped user addresses instead of truncated kernel-heap pointers. */

PVOID WINAPI crt_malloc(SIZE_T size)
{
    PVOID result = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    if (size == 0x60 || size == 0x68) {
        static uint32_t raw_monitor_alloc_logs;
        if (raw_monitor_alloc_logs++ < 8) {
            serial_puts("[CRT-MALLOC-MON] size=");
            serial_putdec(size);
            serial_puts(" -> 0x");
            serial_puthex((ULONG_PTR)result, 16);
            serial_puts("\n");
        }
    }
    return result;
}

PVOID WINAPI crt_calloc(SIZE_T count, SIZE_T size)
{
    if (size && count > (SIZE_T)-1 / size)
        return NULL;
    SIZE_T total = count * size;
    PVOID result = HeapAlloc(GetProcessHeap(),
                             0x00000008 /* HEAP_ZERO_MEMORY */,
                             total ? total : 1);
    static uint32_t calloc120_logs;
    if (total == 0x78 && calloc120_logs++ < 8) {
        serial_puts("[CRT-CALLOC120] count=");
        serial_putdec(count);
        serial_puts(" size=");
        serial_putdec(size);
        serial_puts(" -> 0x");
        serial_puthex((ULONG_PTR)result, 16);
        serial_puts("\n");
    }
    return result;
}

void WINAPI crt_free(PVOID ptr)
{
    if (ptr)
        HeapFree(GetProcessHeap(), 0, ptr);
}

PVOID WINAPI crt_realloc(PVOID ptr, SIZE_T size)
{
    if (!ptr) return crt_malloc(size);
    if (size == 0) { crt_free(ptr); return NULL; }
    return HeapReAlloc(GetProcessHeap(), 0, ptr, size);
}

SIZE_T WINAPI crt_msize(PVOID ptr)
{
    return HeapSize(GetProcessHeap(), 0, ptr);
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

static BOOL crt_char_is_delimiter(char c, const char *delimiters)
{
    for (const char *d = delimiters; *d; d++)
        if (*d == c) return TRUE;
    return FALSE;
}

static char **crt_strtok_context(void);

char* WINAPI crt_strtok_s(char *str, const char *delimiters, char **context)
{
    if (!delimiters || !context || (!str && !*context)) {
        *crt_errno() = 22; /* EINVAL */
        return NULL;
    }

    char *cursor = str ? str : *context;
    while (*cursor && crt_char_is_delimiter(*cursor, delimiters)) cursor++;
    if (!*cursor) {
        *context = cursor;
        return NULL;
    }

    char *token = cursor;
    while (*cursor && !crt_char_is_delimiter(*cursor, delimiters)) cursor++;
    if (*cursor) *cursor++ = 0;
    *context = cursor;
    return token;
}

char* WINAPI crt_strtok(char *str, const char *delimiters)
{
    char **context = crt_strtok_context();
    if (!context) {
        *crt_errno() = 12; /* ENOMEM */
        return NULL;
    }
    return crt_strtok_s(str, delimiters, context);
}

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

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

PVOID WINAPI crt_memmove(PVOID dst, PCVOID src, SIZE_T n)
{
    if (n == 0 || dst == src) return dst;

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

PVOID WINAPI crt_memchr(PCVOID ptr, int value, SIZE_T n)
{
    const BYTE *p = (const BYTE *)ptr;
    BYTE needle = (BYTE)value;
    for (SIZE_T i = 0; i < n; i++) {
        if (p[i] == needle) return (PVOID)(ULONG_PTR)&p[i];
    }
    return NULL;
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

static void fmt_integer(FMT_CTX *ctx, const char *digits, SIZE_T digit_count,
                        int width, int precision, int left_align,
                        int zero_pad, char sign)
{
    if (precision == 0 && digit_count == 1 && digits[0] == '0')
        digit_count = 0;

    int precision_zeroes = 0;
    if (precision > (int)digit_count)
        precision_zeroes = precision - (int)digit_count;

    int content = (sign ? 1 : 0) + precision_zeroes + (int)digit_count;
    int width_pad = width > content ? width - content : 0;

    /* An explicit integer precision disables the zero flag. */
    if (!left_align && (!zero_pad || precision >= 0))
        fmt_pad(ctx, width_pad, ' ');
    if (sign)
        fmt_putc(ctx, sign);
    if (!left_align && zero_pad && precision < 0)
        fmt_pad(ctx, width_pad, '0');
    fmt_pad(ctx, precision_zeroes, '0');
    fmt_puts(ctx, digits, digit_count);
    if (left_align)
        fmt_pad(ctx, width_pad, ' ');
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

            unsigned long long magnitude;
            if (val < 0) {
                negative = 1;
                magnitude = (unsigned long long)(-(val + 1)) + 1;
            } else {
                magnitude = (unsigned long long)val;
            }
            num_len = uint_to_str(num_buf, magnitude, 10, 0);
            char sign = negative ? '-' : (plus_sign ? '+' :
                                           (space_sign ? ' ' : 0));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, sign);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (len_mod == 2) val = ms_va_arg(ap, unsigned long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, unsigned long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 10, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (len_mod == 2) val = ms_va_arg(ap, unsigned long long);
            else if (len_mod == 1 || len_mod == 3) val = ms_va_arg(ap, unsigned long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 16, (*fmt == 'X'));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (len_mod >= 1) val = ms_va_arg(ap, unsigned long long);
            else val = ms_va_arg(ap, unsigned int);

            num_len = uint_to_str(num_buf, val, 8, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
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

/*
 * do_vformat32 — Format walker for 32-bit va_list (4-byte arg slots).
 *
 * When a 32-bit PE32 program calls vsprintf/vprintf/etc via INT 0x2E thunk,
 * the va_list parameter is a pointer into the 32-bit stack where args are
 * packed in 4-byte slots. ms_va_arg reads 8-byte slots which is WRONG.
 * This version manually walks the 32-bit stack with uint32_t* increments.
 */
static int do_vformat32(FMT_CTX *ctx, const char *fmt, uint32_t *vp)
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
        if (*fmt == '*') { width = (int)(*vp++); fmt++; }
        else { while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; } }

        /* Precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = (int)(*vp++); fmt++; }
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
            if (len_mod == 2) {
                /* 64-bit: two consecutive 32-bit values (lo, hi) */
                uint32_t lo = *vp++, hi = *vp++;
                val = (long long)((uint64_t)hi << 32 | lo);
            } else {
                val = (long long)(int32_t)(*vp++);
            }

            unsigned long long magnitude;
            if (val < 0) {
                negative = 1;
                magnitude = (unsigned long long)(-(val + 1)) + 1;
            } else {
                magnitude = (unsigned long long)val;
            }
            num_len = uint_to_str(num_buf, magnitude, 10, 0);
            char sign = negative ? '-' : (plus_sign ? '+' :
                                           (space_sign ? ' ' : 0));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, sign);
            break;
        }
        case 'u': {
            unsigned long long val;
            if (len_mod == 2) {
                uint32_t lo = *vp++, hi = *vp++;
                val = ((uint64_t)hi << 32) | lo;
            } else {
                val = (unsigned long long)(*vp++);
            }

            num_len = uint_to_str(num_buf, val, 10, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long val;
            if (len_mod == 2) {
                uint32_t lo = *vp++, hi = *vp++;
                val = ((uint64_t)hi << 32) | lo;
            } else {
                val = (unsigned long long)(*vp++);
            }

            num_len = uint_to_str(num_buf, val, 16, (*fmt == 'X'));
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'o': {
            unsigned long long val;
            if (len_mod >= 1) {
                uint32_t lo = *vp++, hi = *vp++;
                val = ((uint64_t)hi << 32) | lo;
            } else {
                val = (unsigned long long)(*vp++);
            }

            num_len = uint_to_str(num_buf, val, 8, 0);
            fmt_integer(ctx, num_buf, num_len, width, precision, left_align,
                        zero_pad, 0);
            break;
        }
        case 'p': {
            /* 32-bit pointer */
            uint32_t val32 = *vp++;
            num_len = uint_to_str(num_buf, (unsigned long long)val32, 16, 0);
            int total = (int)num_len + 2; /* "0x" prefix */
            if (!left_align) fmt_pad(ctx, width - total, ' ');
            fmt_putc(ctx, '0'); fmt_putc(ctx, 'x');
            fmt_pad(ctx, 8 - (int)num_len, '0');
            fmt_puts(ctx, num_buf, num_len);
            if (left_align) fmt_pad(ctx, width - total, ' ');
            break;
        }
        case 's':
            str = (const char *)(uintptr_t)(*vp++);
            if (!str) str = "(null)";
            str_len = crt_strlen(str);
            if (precision >= 0 && (SIZE_T)precision < str_len)
                str_len = (SIZE_T)precision;
            if (!left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            fmt_puts(ctx, str, str_len);
            if (left_align) fmt_pad(ctx, width - (int)str_len, ' ');
            break;

        case 'c': {
            char ch = (char)(*vp++);
            if (!left_align) fmt_pad(ctx, width - 1, ' ');
            fmt_putc(ctx, ch);
            if (left_align) fmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'e': case 'g': {
            /* Double on 32-bit stack: 8 bytes = two consecutive uint32_t */
            uint32_t lo = *vp++, hi = *vp++;
            uint64_t bits = ((uint64_t)hi << 32) | lo;
            double val;
            __builtin_memcpy(&val, &bits, 8);
            int prec = (precision >= 0) ? precision : 6;

            int f_neg = 0;
            if (val < 0) { f_neg = 1; val = -val; }

            unsigned long long int_part = (unsigned long long)val;
            double frac_part = val - (double)int_part;

            char f_buf[80];
            int f_pos = 0;
            SIZE_T ip_len = uint_to_str(f_buf, int_part, 10, 0);
            f_pos = (int)ip_len;

            if (prec > 0) {
                f_buf[f_pos++] = '.';
                for (int fi = 0; fi < prec; fi++) {
                    frac_part *= 10.0;
                    int fdigit = (int)frac_part;
                    if (fdigit > 9) fdigit = 9;
                    f_buf[f_pos++] = '0' + fdigit;
                    frac_part -= fdigit;
                }
                if (frac_part >= 0.5) {
                    int ci = f_pos - 1;
                    while (ci >= 0) {
                        if (f_buf[ci] == '.') { ci--; continue; }
                        if (f_buf[ci] < '9') { f_buf[ci]++; break; }
                        f_buf[ci] = '0';
                        ci--;
                    }
                    if (ci < 0) {
                        for (int si = f_pos; si > 0; si--)
                            f_buf[si] = f_buf[si - 1];
                        f_buf[0] = '1';
                        f_pos++;
                    }
                }
            } else if (precision == 0) {
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
            if (len_mod == 2) {
                uint32_t addr32 = *vp++;
                long long *p = (long long *)(uintptr_t)addr32;
                *p = (long long)ctx->pos;
            } else {
                uint32_t addr32 = *vp++;
                int *p = (int *)(uintptr_t)addr32;
                *p = (int)ctx->pos;
            }
            break;

        case 0:
            goto done32;

        default:
            fmt_putc(ctx, '%');
            fmt_putc(ctx, *fmt);
            break;
        }
        fmt++;
    }
done32:
    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return (int)ctx->pos;
}

static int WINAPI crt_printf_compat32(const char *fmt, uint32_t *args)
{
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, args);
}

static int WINAPI crt_sprintf_compat32(char *buf, const char *fmt,
                                       uint32_t *args)
{
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    return do_vformat32(&ctx, fmt, args);
}

static int WINAPI crt_snprintf_compat32(char *buf, SIZE_T size,
                                        const char *fmt, uint32_t *args)
{
    FMT_CTX ctx = { buf, size, 0 };
    return do_vformat32(&ctx, fmt, args);
}

static int WINAPI crt_fprintf_compat32(PVOID stream, const char *fmt,
                                       uint32_t *args)
{
    (void)stream;
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, args);
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

typedef struct {
    ms_va_list *native;
    uint32_t *compat32;
} CRT_SCAN_ARGS;

static PVOID crt_scan_output_arg(CRT_SCAN_ARGS *args)
{
    if (args->compat32) {
        PVOID output = (PVOID)(ULONG_PTR)*args->compat32;
        args->compat32++;
        return output;
    }
    return ms_va_arg(*args->native, PVOID);
}

static int crt_vsscanf_core(const char *buf, const char *fmt,
                            CRT_SCAN_ARGS *args)
{
    if (!buf || !fmt) return -1;

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
        } else if (*fmt == 'I') {
            /* Microsoft CRT integer widths: %I32d / %I64u. */
            if (fmt[1] == '6' && fmt[2] == '4') {
                fmt += 3;
                s_len_mod = 3;
            } else if (fmt[1] == '3' && fmt[2] == '2') {
                fmt += 3;
                s_len_mod = 0;
            }
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
                if (s_len_mod == 3) *(long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(long *)crt_scan_output_arg(args) = (long)val;
                else if (s_len_mod == 2) *(short *)crt_scan_output_arg(args) = (short)val;
                else *(int *)crt_scan_output_arg(args) = (int)val;
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
                if (s_len_mod == 3) *(unsigned long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(unsigned long *)crt_scan_output_arg(args) = (unsigned long)val;
                else if (s_len_mod == 2) *(unsigned short *)crt_scan_output_arg(args) = (unsigned short)val;
                else *(unsigned int *)crt_scan_output_arg(args) = (unsigned int)val;
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
                if (s_len_mod == 3) *(unsigned long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(unsigned long *)crt_scan_output_arg(args) = (unsigned long)val;
                else *(unsigned int *)crt_scan_output_arg(args) = (unsigned int)val;
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
                if (s_len_mod == 3) *(unsigned long long *)crt_scan_output_arg(args) = val;
                else if (s_len_mod == 1) *(unsigned long *)crt_scan_output_arg(args) = (unsigned long)val;
                else if (s_len_mod == 2) *(unsigned short *)crt_scan_output_arg(args) = (unsigned short)val;
                else *(unsigned int *)crt_scan_output_arg(args) = (unsigned int)val;
                matched++;
            }
            break;
        }
        case 's': {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) goto done;

            char *dst = suppress ? NULL : (char *)crt_scan_output_arg(args);
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

            char *dst = suppress ? NULL : (char *)crt_scan_output_arg(args);
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
                if (s_len_mod == 1) *(double *)crt_scan_output_arg(args) = fval;
                else *(float *)crt_scan_output_arg(args) = (float)fval;
                matched++;
            }
            break;
        }
        case 'n': {
            /* Store number of characters consumed so far */
            if (!suppress) {
                *(int *)crt_scan_output_arg(args) = (int)(p - buf);
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
            char *dst = suppress ? NULL : (char *)crt_scan_output_arg(args);
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
    return matched;
}

int WINAPI crt_sscanf(const char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    CRT_SCAN_ARGS args = { &ap, NULL };
    int matched = crt_vsscanf_core(buf, fmt, &args);
    ms_va_end(ap);
    return matched;
}

static int WINAPI crt_sscanf_compat32(const char *buf, const char *fmt,
                                      uint32_t *args32)
{
    CRT_SCAN_ARGS args = { NULL, args32 };
    return crt_vsscanf_core(buf, fmt, &args);
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
    int     open_flags;
    int     owns_handle;
};

#define CRT_FILE_MAX 32
#define CRT_FILE_PROXY_PROCESS_SLOTS 512

/* PE32 programs can inspect FILE directly. Keep the ABI-visible object in
 * process-owned low memory and translate it to the native backing table at
 * the shim boundary. This is the MSVCR100/UCRT x86 _iobuf layout. */
typedef struct {
    uint32_t ptr;
    int32_t  cnt;
    uint32_t base;
    int32_t  flag;
    int32_t  file;
    int32_t  charbuf;
    int32_t  bufsiz;
    uint32_t tmpfname;
} CRT_FILE32;

_Static_assert(sizeof(CRT_FILE32) == 32, "PE32 _iobuf layout changed");

typedef struct {
    DWORD owner_pid;
    CRT_FILE32 *files;
} CRT_FILE_PROXY_SLOT;

#define CRT_EBADF       9
#define CRT_ENOMEM     12
#define CRT_EACCES     13
#define CRT_EINVAL     22
#define CRT_EMFILE     24
#define CRT_ENOENT      2
#define CRT_ENAMETOOLONG 38
#define CRT_EILSEQ      42
#define CRT_EOVERFLOW 132
#define CRT_ERANGE     34
#define CRT_STRUNCATE  80

#define CRT_O_WRONLY   0x0001
#define CRT_O_RDWR     0x0002
#define CRT_O_APPEND   0x0008
#define CRT_O_CREAT    0x0100
#define CRT_O_TRUNC    0x0200
#define CRT_O_EXCL     0x0400
#define CRT_O_TEXT     0x4000
#define CRT_O_BINARY   0x8000

#define CRT_FILE_BEGIN   0
#define CRT_FILE_CURRENT 1
#define CRT_FILE_END     2

#define CRT_DUPLICATE_SAME_ACCESS 0x00000002

extern BOOL WINAPI FlushFileBuffers(HANDLE hFile);

static CRT_FILE crt_files[CRT_FILE_MAX];
static int crt_files_init = 0;
static CRT_FILE_PROXY_SLOT crt_file_proxies[CRT_FILE_PROXY_PROCESS_SLOTS];
static volatile uint32_t crt_file_proxy_lock;
static volatile uint32_t crt_open_trace_count;
static volatile uint32_t crt_close_trace_count;
static volatile uint32_t crt_read_trace_count;
static volatile uint32_t crt_lseek_trace_count;
static volatile uint32_t crt_lseek32_trace_count;
static volatile uint32_t crt_stat_trace_count;

static int crt_io_trace_take(volatile uint32_t *counter, uint32_t limit)
{
    if (limit > 8) limit = 8;
    return __sync_fetch_and_add(counter, 1) < limit;
}

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
    crt_files[0].open_flags = CRT_O_TEXT;
    crt_files[1].nt_handle = (HANDLE)(ULONG_PTR)8;   /* stdout */
    crt_files[1].flags = 2;
    crt_files[1].open_flags = CRT_O_TEXT | CRT_O_WRONLY;
    crt_files[2].nt_handle = (HANDLE)(ULONG_PTR)12;  /* stderr */
    crt_files[2].flags = 2;
    crt_files[2].open_flags = CRT_O_TEXT | CRT_O_WRONLY;
}

static void crt_file_proxy_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&crt_file_proxy_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static void crt_file_proxy_lock_release(void)
{
    __sync_lock_release(&crt_file_proxy_lock);
}

static int crt_file_proxy_flags(const CRT_FILE *file)
{
    int flags = 0;
    if (file->flags & 1) flags |= 0x0001; /* _IOREAD */
    if (file->flags & 2) flags |= 0x0002; /* _IOWRT */
    if (file->flags & 4) flags |= 0x0010; /* _IOEOF */
    if (file->flags & 8) flags |= 0x0020; /* _IOERR */
    return flags;
}

static void crt_file_proxy_init(CRT_FILE32 *files)
{
    ensure_stdio_init();
    for (int fd = 0; fd < CRT_FILE_MAX; fd++) {
        files[fd].flag = crt_file_proxy_flags(&crt_files[fd]);
        files[fd].file = fd;
    }
}

static CRT_FILE32 *crt_file_proxy_array(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    CRT_FILE32 *files = NULL;
    BOOL have_free_slot = FALSE;
    crt_file_proxy_lock_acquire();
    for (uint32_t i = 0; i < CRT_FILE_PROXY_PROCESS_SLOTS; i++) {
        if (crt_file_proxies[i].owner_pid == owner_pid) {
            files = crt_file_proxies[i].files;
            break;
        }
        if (!crt_file_proxies[i].owner_pid)
            have_free_slot = TRUE;
    }
    crt_file_proxy_lock_release();

    if (files || !create || !have_free_slot)
        return files;

    CRT_FILE32 *candidate = (CRT_FILE32 *)VirtualAlloc(
        NULL, sizeof(CRT_FILE32) * CRT_FILE_MAX,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ULONG_PTR candidate_end = (ULONG_PTR)candidate +
                              sizeof(CRT_FILE32) * CRT_FILE_MAX - 1;
    if (!candidate || (ULONG_PTR)candidate > (ULONG_PTR)UINT32_MAX ||
        candidate_end > (ULONG_PTR)UINT32_MAX) {
        if (candidate)
            VirtualFree(candidate, 0, MEM_RELEASE);
        return NULL;
    }
    crt_file_proxy_init(candidate);

    CRT_FILE32 *unused = candidate;
    crt_file_proxy_lock_acquire();
    CRT_FILE_PROXY_SLOT *free_slot = NULL;
    for (uint32_t i = 0; i < CRT_FILE_PROXY_PROCESS_SLOTS; i++) {
        CRT_FILE_PROXY_SLOT *slot = &crt_file_proxies[i];
        if (slot->owner_pid == owner_pid) {
            files = slot->files;
            break;
        }
        if (!slot->owner_pid && !free_slot)
            free_slot = slot;
    }
    if (!files && free_slot) {
        free_slot->owner_pid = owner_pid;
        free_slot->files = candidate;
        files = candidate;
        unused = NULL;
    }
    crt_file_proxy_lock_release();

    if (unused)
        VirtualFree(unused, 0, MEM_RELEASE);
    if (files == candidate) {
        serial_puts("[CRT] PE32 stdio proxy pid=");
        serial_putdec(owner_pid);
        serial_puts(" va=0x");
        serial_puthex((ULONG_PTR)files, 8);
        serial_puts("\n");
    }
    return files;
}

static void crt_file_proxy_sync(int fd)
{
    if (fd < 0 || fd >= CRT_FILE_MAX) return;
    CRT_FILE32 *files = crt_file_proxy_array(FALSE);
    if (!files) return;
    files[fd].flag = crt_file_proxy_flags(&crt_files[fd]);
    files[fd].file = fd;
}

static void crt_file_proxy_release_process(DWORD process_id)
{
    crt_file_proxy_lock_acquire();
    for (uint32_t i = 0; i < CRT_FILE_PROXY_PROCESS_SLOTS; i++) {
        if (crt_file_proxies[i].owner_pid == process_id) {
            crt_file_proxies[i].owner_pid = 0;
            crt_file_proxies[i].files = NULL;
        }
    }
    crt_file_proxy_lock_release();
}

static int crt_file_index(CRT_FILE *file)
{
    uintptr_t address = (uintptr_t)file;
    uintptr_t first = (uintptr_t)&crt_files[0];
    uintptr_t end = (uintptr_t)&crt_files[CRT_FILE_MAX];

    ensure_stdio_init();
    if (address >= first && address < end &&
        ((address - first) % sizeof(CRT_FILE)) == 0) {
        int fd = (int)((address - first) / sizeof(CRT_FILE));
        return crt_files[fd].nt_handle ? fd : -1;
    }

    CRT_FILE32 *files = crt_file_proxy_array(FALSE);
    first = (uintptr_t)files;
    end = first + sizeof(CRT_FILE32) * CRT_FILE_MAX;
    if (!files || address < first || address >= end ||
        ((address - first) % sizeof(CRT_FILE32)) != 0)
        return -1;

    int fd = (int)((address - first) / sizeof(CRT_FILE32));
    return crt_files[fd].nt_handle ? fd : -1;
}

static CRT_FILE *crt_file_resolve(CRT_FILE *file, int *fd_out)
{
    int fd = crt_file_index(file);
    if (fd_out) *fd_out = fd;
    return fd >= 0 ? &crt_files[fd] : NULL;
}

static CRT_FILE *crt_file_from_fd(int fd)
{
    ensure_stdio_init();
    if (fd < 0 || fd >= CRT_FILE_MAX || !crt_files[fd].nt_handle)
        return NULL;
    return &crt_files[fd];
}

static CRT_FILE *crt_file_for_caller(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file || !g_compat32_mode)
        return file;

    CRT_FILE32 *files = crt_file_proxy_array(TRUE);
    if (!files) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    crt_file_proxy_sync(fd);
    return (CRT_FILE *)(ULONG_PTR)&files[fd];
}

static int crt_file_attach(HANDLE handle, int open_flags)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }

    ensure_stdio_init();
    for (int fd = 3; fd < CRT_FILE_MAX; fd++) {
        if (crt_files[fd].nt_handle) continue;
        crt_files[fd].nt_handle = handle;
        crt_files[fd].flags = (open_flags & CRT_O_RDWR) ? 3 :
                              (open_flags & CRT_O_WRONLY) ? 2 : 1;
        crt_files[fd].ungetc_ch = -1;
        crt_files[fd].open_flags = open_flags;
        crt_files[fd].owns_handle = 1;
        crt_file_proxy_sync(fd);
        return fd;
    }

    *crt_errno() = CRT_EMFILE;
    return -1;
}

CRT_FILE* WINAPI crt_iob_func(void)
{
    ensure_stdio_init();
    if (!g_compat32_mode)
        return &crt_files[0];

    CRT_FILE32 *files = crt_file_proxy_array(TRUE);
    return (CRT_FILE *)(ULONG_PTR)files;
}

CRT_FILE* WINAPI crt_acrt_iob_func(unsigned int index)
{
    if (index >= 3)
        return NULL;
    return crt_file_for_caller((int)index);
}

static int crt_parse_stdio_mode(const char *mode, int *open_flags)
{
    if (!mode || !mode[0] || !open_flags) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    int flags;
    if (mode[0] == 'r') flags = 0;
    else if (mode[0] == 'w')
        flags = CRT_O_WRONLY | CRT_O_CREAT | CRT_O_TRUNC;
    else if (mode[0] == 'a')
        flags = CRT_O_WRONLY | CRT_O_CREAT | CRT_O_APPEND;
    else {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    flags |= CRT_O_TEXT;
    for (const char *option = mode + 1; *option; option++) {
        switch (*option) {
        case '+':
            flags &= ~CRT_O_WRONLY;
            flags |= CRT_O_RDWR;
            break;
        case 'b':
            flags &= ~CRT_O_TEXT;
            flags |= CRT_O_BINARY;
            break;
        case 't':
            flags &= ~CRT_O_BINARY;
            flags |= CRT_O_TEXT;
            break;
        case 'x':
            flags |= CRT_O_EXCL;
            break;
        case 'c': /* commit-on-flush policy; writes are already synchronous */
        case 'n': /* inherited CRT commit policy */
            break;
        case ',':
            /* Encoding suffixes affect transcoding, which this unbuffered
             * narrow stream does not perform. Accept the documented syntax. */
            *open_flags = flags;
            return 0;
        default:
            *crt_errno() = CRT_EINVAL;
            return -1;
        }
    }

    *open_flags = flags;
    return 0;
}

static int crt_stdio_access_flags(int open_flags)
{
    if (open_flags & CRT_O_RDWR) return 3;
    if (open_flags & CRT_O_WRONLY) return 2;
    return 1;
}

CRT_FILE* WINAPI crt_fopen(const char *path, const char *mode)
{
    if (!path) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    int open_flags;
    if (crt_parse_stdio_mode(mode, &open_flags) != 0)
        return NULL;

    int fd = crt_open(path, open_flags, 0666);
    return fd >= 0 ? crt_file_for_caller(fd) : NULL;
}

CRT_FILE* WINAPI crt_fdopen(int fd, const char *mode)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    int stream_open_flags;
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return NULL;
    }
    if (crt_parse_stdio_mode(mode, &stream_open_flags) != 0)
        return NULL;

    int stream_access = crt_stdio_access_flags(stream_open_flags);
    int descriptor_access = crt_stdio_access_flags(file->open_flags);
    if ((stream_access & descriptor_access) != stream_access) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    if ((stream_open_flags & CRT_O_APPEND) &&
        crt_lseeki64(fd, 0, CRT_FILE_END) < 0)
        return NULL;

    file->flags = stream_access;
    file->ungetc_ch = -1;
    file->open_flags =
        (file->open_flags & ~(CRT_O_TEXT | CRT_O_BINARY | CRT_O_APPEND)) |
        (stream_open_flags & (CRT_O_TEXT | CRT_O_BINARY | CRT_O_APPEND));
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    return crt_file_for_caller(fd);
}

CRT_FILE* WINAPI crt_wfopen(const uint16_t *wpath, const uint16_t *wmode)
{
    /* Convert wide strings to narrow */
    char path[260], mode[16];
    int i;
    for (i = 0; i < 259 && wpath[i]; i++) path[i] = (char)wpath[i];
    path[i] = 0;
    for (i = 0; i < 15 && wmode[i]; i++) mode[i] = (char)wmode[i];
    mode[i] = 0;
    return crt_fopen(path, mode);
}

SIZE_T WINAPI crt_fread(PVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!size || !count) return 0;
    if (!buf || !file || !(file->flags & 1)) {
        if (file) {
            file->flags |= 8;
            crt_file_proxy_sync(fd);
        }
        return 0;
    }
    DWORD total = (DWORD)(size * count);
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(file->nt_handle, buf, total, &bytes_read, NULL);
    if (!ok) file->flags |= 8;
    else if (bytes_read == 0) file->flags |= 4;
    crt_file_proxy_sync(fd);
    return bytes_read / size;
}

SIZE_T WINAPI crt_fwrite(PCVOID buf, SIZE_T size, SIZE_T count, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!size || !count) return 0;
    if (!buf || !file || !(file->flags & 2)) {
        if (file) {
            file->flags |= 8;
            crt_file_proxy_sync(fd);
        }
        return 0;
    }
    DWORD total = (DWORD)(size * count);
    DWORD bytes_written = 0;
    if (!WriteFile(file->nt_handle, buf, total, &bytes_written, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
    }

    /* Echo text writes to serial (captures engine log output) */
    if (total > 0 && total < 4096) {
        const char *s = (const char *)buf;
        serial_puts("[LOG] ");
        for (DWORD i = 0; i < total && i < 200; i++) {
            char c = s[i];
            if (c >= 32 && c < 127) {
                char b[2] = { c, 0 };
                serial_puts(b);
            } else if (c == '\n') {
                serial_puts("\n[LOG] ");
            }
        }
        serial_puts("\n");
    }

    return bytes_written / size;
}

int WINAPI crt_fclose(CRT_FILE *f)
{
    int fd = crt_file_index(f);
    return fd < 0 ? -1 : crt_close(fd);
}

int WINAPI crt_fseek(CRT_FILE *f, long offset, int whence)
{
    int fd = crt_file_index(f);
    if (fd < 0 || crt_lseeki64(fd, (LONGLONG)offset, whence) < 0)
        return -1;
    return 0;
}

long WINAPI crt_ftell(CRT_FILE *f)
{
    CRT_FILE *file = crt_file_resolve(f, NULL);
    if (!file) return -1;
    /* Get current position by seeking 0 from current */
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION pos_info;
    NTSTATUS status = NtQueryInformationFile(file->nt_handle, &iosb, &pos_info,
                                              sizeof(pos_info),
                                              FilePositionInformation);
    if (!NT_SUCCESS(status)) return -1;
    return (long)pos_info.CurrentByteOffset.QuadPart;
}

int WINAPI crt_fflush(CRT_FILE *f)
{
    if (!f) return 0;
    CRT_FILE *file = crt_file_resolve(f, NULL);
    if (!file) return -1;
    if (!(file->flags & 2)) return 0;
    return FlushFileBuffers(file->nt_handle) ? 0 : -1;
}
int WINAPI crt_feof(CRT_FILE *f)
{
    CRT_FILE *file = crt_file_resolve(f, NULL);
    return file ? (file->flags & 4) : 0;
}

int WINAPI crt_ferror(CRT_FILE *f)
{
    CRT_FILE *file = crt_file_resolve(f, NULL);
    return file ? (file->flags & 8) : 0;
}

int WINAPI crt_fgetc(CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file) return -1;
    if (file->flags & 16) {
        file->flags &= ~16;
        crt_file_proxy_sync(fd);
        return file->ungetc_ch;
    }
    char c;
    DWORD read;
    if (!ReadFile(file->nt_handle, &c, 1, &read, NULL) || read == 0) {
        file->flags |= 4;
        crt_file_proxy_sync(fd);
        return -1;
    }
    return (unsigned char)c;
}

int WINAPI crt_fputc(int c, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file) return -1;
    char ch = (char)c;
    DWORD written = 0;
    if (!WriteFile(file->nt_handle, &ch, 1, &written, NULL) || written != 1) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        return -1;
    }
    return (unsigned char)ch;
}

char* WINAPI crt_fgets(char *buf, int n, CRT_FILE *f)
{
    if (!buf || !crt_file_resolve(f, NULL) || n <= 0) return NULL;
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
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file || !s) return -1;
    SIZE_T len = crt_strlen(s);
    DWORD written = 0;
    if (!WriteFile(file->nt_handle, s, (DWORD)len, &written, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        return -1;
    }
    return (int)written;
}

int WINAPI crt_ungetc(int c, CRT_FILE *f)
{
    int fd;
    CRT_FILE *file = crt_file_resolve(f, &fd);
    if (!file || c == -1) return -1;
    file->ungetc_ch = c;
    file->flags |= 16;
    file->flags &= ~4;  /* clear EOF */
    crt_file_proxy_sync(fd);
    return c;
}

int WINAPI crt_fileno(CRT_FILE *f)
{
    int fd = crt_file_index(f);
    if (fd < 0) *crt_errno() = CRT_EBADF;
    return fd;
}

static void WINAPI crt_clearerr(CRT_FILE *file)
{
    int fd;
    CRT_FILE *native = crt_file_resolve(file, &fd);
    if (native) {
        native->flags &= ~(4 | 8);
        crt_file_proxy_sync(fd);
    }
}

static void WINAPI crt_rewind(CRT_FILE *file)
{
    if (file && crt_fseek(file, 0, CRT_FILE_BEGIN) == 0)
        crt_clearerr(file);
}

static int WINAPI crt_setvbuf(CRT_FILE *file, char *buffer, int mode,
                              SIZE_T size)
{
    (void)buffer;
    (void)size;
    if (!crt_file_resolve(file, NULL) ||
        (mode != 0 && mode != 4 && mode != 64)) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    /* Osito's CRT streams are unbuffered, so all valid policies are already
     * synchronized with the backing NT handle. */
    *crt_errno() = 0;
    return 0;
}

/* ── Conversion ────────────────────────────────────────────── */

static int crt_errno_from_last_error(void)
{
    switch (GetLastError()) {
    case 2:  /* ERROR_FILE_NOT_FOUND */
    case 3:  /* ERROR_PATH_NOT_FOUND */
        return 2;
    case 5:  /* ERROR_ACCESS_DENIED */
    case 32: /* ERROR_SHARING_VIOLATION */
    case 33: /* ERROR_LOCK_VIOLATION */
        return 13;
    case 6:  /* ERROR_INVALID_HANDLE */
        return CRT_EBADF;
    case 80:  /* ERROR_FILE_EXISTS */
    case 183: /* ERROR_ALREADY_EXISTS */
        return 17;
    case 112: /* ERROR_DISK_FULL */
        return 28;
    default:
        return CRT_EINVAL;
    }
}

static DWORD crt_open_access(int flags)
{
    if (flags & CRT_O_RDWR) return GENERIC_READ | GENERIC_WRITE;
    if (flags & CRT_O_WRONLY) return GENERIC_WRITE;
    return GENERIC_READ;
}

static DWORD crt_open_disposition(int flags)
{
    if ((flags & (CRT_O_CREAT | CRT_O_EXCL)) ==
        (CRT_O_CREAT | CRT_O_EXCL))
        return 1; /* CREATE_NEW */
    if ((flags & (CRT_O_CREAT | CRT_O_TRUNC)) ==
        (CRT_O_CREAT | CRT_O_TRUNC))
        return 2; /* CREATE_ALWAYS */
    if (flags & CRT_O_CREAT) return 4; /* OPEN_ALWAYS */
    if (flags & CRT_O_TRUNC) return 5; /* TRUNCATE_EXISTING */
    return 3; /* OPEN_EXISTING */
}

static int crt_open_handle(HANDLE handle, int flags)
{
    if (handle == INVALID_HANDLE_VALUE) {
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }

    int fd = crt_file_attach(handle, flags);
    if (fd < 0) {
        CloseHandle(handle);
        return -1;
    }
    if ((flags & CRT_O_APPEND) && crt_lseeki64(fd, 0, CRT_FILE_END) < 0) {
        crt_close(fd);
        return -1;
    }
    *crt_errno() = 0;
    return fd;
}

int WINAPI crt_open(const char *path, int flags, int mode)
{
    (void)mode;
    if (!path || !path[0]) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    HANDLE handle = CreateFileA(path, crt_open_access(flags),
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, crt_open_disposition(flags), 0, NULL);
    int fd = crt_open_handle(handle, flags);
    if (crt_io_trace_take(&crt_open_trace_count, 64)) {
        serial_puts("[CRT-OPEN] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" flags=0x");
        serial_puthex((uint32_t)flags, 8);
        serial_puts(" mode=0x");
        serial_puthex((uint32_t)mode, 8);
        serial_puts(" handle=0x");
        serial_puthex((ULONG_PTR)handle, 8);
        serial_puts(" errno=");
        serial_putdec((uint64_t)(uint32_t)*crt_errno());
        serial_puts(" path='");
        serial_puts(path);
        serial_puts("'\n");
    }
    return fd;
}

int WINAPI crt_wopen(const WCHAR *path, int flags, int mode)
{
    (void)mode;
    if (!path || !path[0]) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    HANDLE handle = CreateFileW(path, crt_open_access(flags),
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                NULL, crt_open_disposition(flags), 0, NULL);
    return crt_open_handle(handle, flags);
}

static int crt_unlink_result(BOOL deleted)
{
    if (deleted) return 0;

    DWORD error = GetLastError();
    *crt_doserrno() = error;
    *crt_errno() = (error == 2 || error == 3) ? CRT_ENOENT : CRT_EACCES;
    return -1;
}

int WINAPI crt_unlink(const char *path)
{
    return crt_unlink_result(DeleteFileA(path));
}

int WINAPI crt_wunlink(const WCHAR *path)
{
    return crt_unlink_result(DeleteFileW(path));
}

static int crt_rename_result(BOOL renamed)
{
    if (renamed) return 0;

    DWORD error = GetLastError();
    int crt_error;
    switch (error) {
    case 2:  /* ERROR_FILE_NOT_FOUND */
    case 3:  /* ERROR_PATH_NOT_FOUND */
        crt_error = CRT_ENOENT;
        break;
    case 87:  /* ERROR_INVALID_PARAMETER */
    case 123: /* ERROR_INVALID_NAME */
        crt_error = CRT_EINVAL;
        break;
    case 206: /* ERROR_FILENAME_EXCED_RANGE */
        crt_error = CRT_ENAMETOOLONG;
        break;
    default:
        crt_error = CRT_EACCES;
        break;
    }
    *crt_doserrno() = error;
    *crt_errno() = crt_error;
    return -1;
}

int WINAPI crt_rename(const char *old_path, const char *new_path)
{
    return crt_rename_result(MoveFileA(old_path, new_path));
}

int WINAPI crt_wrename(const WCHAR *old_path, const WCHAR *new_path)
{
    return crt_rename_result(MoveFileW(old_path, new_path));
}

int WINAPI crt_getdrive(void)
{
    char current_directory[260];
    DWORD length = GetCurrentDirectoryA(sizeof(current_directory),
                                        current_directory);
    if (!length || length >= sizeof(current_directory)) {
        *crt_errno() = CRT_ENOMEM;
        return 0;
    }
    if (current_directory[1] != ':') return 0;

    unsigned char drive = (unsigned char)current_directory[0];
    if (drive >= 'a' && drive <= 'z') drive -= (unsigned char)('a' - 'A');
    return drive >= 'A' && drive <= 'Z' ? drive - 'A' + 1 : 0;
}

LONG_PTR WINAPI crt_get_osfhandle(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    return (LONG_PTR)(ULONG_PTR)file->nt_handle;
}

int WINAPI crt_open_osfhandle(LONG_PTR handle, int flags)
{
    int fd = crt_file_attach((HANDLE)(ULONG_PTR)handle, flags);
    if (fd >= 0) *crt_errno() = 0;
    return fd;
}

int WINAPI crt_close(int fd)
{
    int trace = crt_io_trace_take(&crt_close_trace_count, 64);
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        if (trace) {
            serial_puts("[CRT-CLOSE] invalid fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts("\n");
        }
        return -1;
    }

    /* Standard handles are kernel pseudo-handles shared by every Win32
     * process. Keep their backing objects alive. */
    if (fd < 3) {
        *crt_errno() = 0;
        return 0;
    }
    if (file->owns_handle && !CloseHandle(file->nt_handle)) {
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }

    file->nt_handle = NULL;
    file->flags = 0;
    file->ungetc_ch = -1;
    file->open_flags = 0;
    file->owns_handle = 0;
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    if (trace) {
        serial_puts("[CRT-CLOSE] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" ok\n");
    }
    return 0;
}

int WINAPI crt_read(int fd, PVOID buffer, unsigned int count)
{
    int trace = count != 1 && crt_io_trace_take(&crt_read_trace_count, 96);
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file || !(file->flags & 1)) {
        *crt_errno() = CRT_EBADF;
        if (trace) {
            serial_puts("[CRT-READ] invalid fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" count=");
            serial_putdec(count);
            serial_puts("\n");
        }
        return -1;
    }
    if (!buffer && count) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    DWORD transferred = 0;
    if (!ReadFile(file->nt_handle, buffer, count, &transferred, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        *crt_errno() = crt_errno_from_last_error();
        if (trace) {
            serial_puts("[CRT-READ] fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" count=");
            serial_putdec(count);
            serial_puts(" failed errno=");
            serial_putdec((uint64_t)(uint32_t)*crt_errno());
            serial_puts("\n");
        }
        return -1;
    }
    if (!transferred && count) file->flags |= 4;
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    if (trace) {
        serial_puts("[CRT-READ] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" count=");
        serial_putdec(count);
        serial_puts(" -> ");
        serial_putdec(transferred);
        serial_puts("\n");
    }
    return (int)transferred;
}

int WINAPI crt_write(int fd, PCVOID buffer, unsigned int count)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file || !(file->flags & 2)) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (!buffer && count) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    DWORD transferred = 0;
    if (!WriteFile(file->nt_handle, buffer, count, &transferred, NULL)) {
        file->flags |= 8;
        crt_file_proxy_sync(fd);
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }
    *crt_errno() = 0;
    return (int)transferred;
}

LONGLONG WINAPI crt_lseeki64(int fd, LONGLONG offset, int origin)
{
    int trace = crt_io_trace_take(&crt_lseek_trace_count, 96);
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        if (trace) {
            serial_puts("[CRT-LSEEK] invalid fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts("\n");
        }
        return -1;
    }
    if (origin < CRT_FILE_BEGIN || origin > CRT_FILE_END) {
        *crt_errno() = CRT_EINVAL;
        if (trace) {
            serial_puts("[CRT-LSEEK] fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" invalid origin=");
            serial_putdec((uint64_t)(uint32_t)origin);
            serial_puts(" offset=0x");
            serial_puthex((uint64_t)offset, 16);
            serial_puts("\n");
        }
        return -1;
    }

    LONG high = (LONG)((ULONGLONG)offset >> 32);
    SetLastError(0);
    DWORD low = SetFilePointer(file->nt_handle, (LONG)offset, &high,
                               (DWORD)origin);
    if (low == 0xFFFFFFFFU && GetLastError() != 0) {
        *crt_errno() = crt_errno_from_last_error();
        if (trace) {
            serial_puts("[CRT-LSEEK] fd=");
            serial_putdec((uint64_t)(uint32_t)fd);
            serial_puts(" failed errno=");
            serial_putdec((uint64_t)(uint32_t)*crt_errno());
            serial_puts("\n");
        }
        return -1;
    }

    file->flags &= ~4;
    crt_file_proxy_sync(fd);
    *crt_errno() = 0;
    LONGLONG result = (LONGLONG)(((ULONGLONG)(ULONG)high << 32) | low);
    if (trace) {
        serial_puts("[CRT-LSEEK] fd=");
        serial_putdec((uint64_t)(uint32_t)fd);
        serial_puts(" origin=");
        serial_putdec((uint64_t)(uint32_t)origin);
        serial_puts(" offset=0x");
        serial_puthex((uint64_t)offset, 16);
        serial_puts(" -> 0x");
        serial_puthex((uint64_t)result, 16);
        serial_puts("\n");
    }
    return result;
}

static LONGLONG WINAPI crt_lseeki64_compat32(uint32_t fd,
                                              uint32_t offset_low,
                                              uint32_t offset_high,
                                              uint32_t origin)
{
    ULONGLONG bits = ((ULONGLONG)offset_high << 32) | offset_low;
    if (crt_io_trace_take(&crt_lseek32_trace_count, 96)) {
        serial_puts("[CRT-LSEEK32] fd=");
        serial_putdec(fd);
        serial_puts(" raw=0x");
        serial_puthex(offset_high, 8);
        serial_puthex(offset_low, 8);
        serial_puts(" origin=");
        serial_putdec(origin);
        serial_puts("\n");
    }
    return crt_lseeki64((int)fd, (LONGLONG)bits, (int)origin);
}

LONG WINAPI crt_lseek(int fd, LONG offset, int origin)
{
    LONGLONG result = crt_lseeki64(fd, offset, origin);
    if (result < (LONGLONG)(-2147483647 - 1) || result > 2147483647) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    return (LONG)result;
}

int WINAPI crt_dup(int fd)
{
    CRT_FILE *source = crt_file_from_fd(fd);
    if (!source) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }

    HANDLE duplicate = source->nt_handle;
    BOOL owns_handle = FALSE;
    if (source->owns_handle) {
        if (!DuplicateHandle(GetCurrentProcess(), source->nt_handle,
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             CRT_DUPLICATE_SAME_ACCESS)) {
            *crt_errno() = crt_errno_from_last_error();
            return -1;
        }
        owns_handle = TRUE;
    }

    int new_fd = crt_file_attach(duplicate, source->open_flags);
    if (new_fd < 0) {
        if (owns_handle) CloseHandle(duplicate);
        return -1;
    }
    crt_files[new_fd].owns_handle = owns_handle;
    *crt_errno() = 0;
    return new_fd;
}

int WINAPI crt_dup2(int source_fd, int target_fd)
{
    CRT_FILE *source = crt_file_from_fd(source_fd);
    if (!source || target_fd < 0 || target_fd >= CRT_FILE_MAX) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (source_fd == target_fd) return 0;

    HANDLE duplicate = source->nt_handle;
    BOOL owns_handle = FALSE;
    if (source->owns_handle) {
        if (!DuplicateHandle(GetCurrentProcess(), source->nt_handle,
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             CRT_DUPLICATE_SAME_ACCESS)) {
            *crt_errno() = crt_errno_from_last_error();
            return -1;
        }
        owns_handle = TRUE;
    }

    CRT_FILE *target = &crt_files[target_fd];
    if (target->nt_handle && target->owns_handle)
        CloseHandle(target->nt_handle);
    *target = *source;
    target->nt_handle = duplicate;
    target->owns_handle = owns_handle;
    target->flags &= ~(4 | 8 | 16);
    target->ungetc_ch = -1;
    crt_file_proxy_sync(target_fd);
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_commit(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (!FlushFileBuffers(file->nt_handle)) {
        *crt_errno() = crt_errno_from_last_error();
        return -1;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_isatty(int fd)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return 0;
    }
    *crt_errno() = 0;
    return file->nt_handle == (HANDLE)(ULONG_PTR)4 ||
           file->nt_handle == (HANDLE)(ULONG_PTR)8 ||
           file->nt_handle == (HANDLE)(ULONG_PTR)12;
}

int WINAPI crt_setmode(int fd, int mode)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    const int mode_mask = CRT_O_TEXT | CRT_O_BINARY | 0x10000 | 0x20000 |
                          0x40000;
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (mode != CRT_O_TEXT && mode != CRT_O_BINARY && mode != 0x10000 &&
        mode != 0x20000 && mode != 0x40000) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    int previous = file->open_flags & mode_mask;
    if (!previous) previous = CRT_O_TEXT;
    file->open_flags = (file->open_flags & ~mode_mask) | mode;
    *crt_errno() = 0;
    return previous;
}

int WINAPI crt_chsize_s(int fd, ULONGLONG size)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    IO_STATUS_BLOCK iosb;
    FILE_POSITION_INFORMATION original;
    LARGE_INTEGER end;
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return CRT_EBADF;
    }

    NTSTATUS status = NtQueryInformationFile(file->nt_handle, &iosb, &original,
                                              sizeof(original),
                                              FilePositionInformation);
    if (!NT_SUCCESS(status)) {
        *crt_errno() = CRT_EBADF;
        return CRT_EBADF;
    }

    end.QuadPart = (LONGLONG)size;
    status = NtSetInformationFile(file->nt_handle, &iosb, &end, sizeof(end),
                                  FileEndOfFileInformation);
    (void)NtSetInformationFile(file->nt_handle, &iosb, &original,
                               sizeof(original), FilePositionInformation);
    if (!NT_SUCCESS(status)) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    *crt_errno() = 0;
    return 0;
}

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

typedef struct {
    double value;
    const char *end;
} CRT_STRTOD_RESULT;

static int crt_ascii_space(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static unsigned char crt_ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int crt_ascii_word(const char *s, const char *word)
{
    while (*word) {
        if (crt_ascii_lower((unsigned char)*s) !=
            (unsigned char)*word)
            return 0;
        s++;
        word++;
    }
    return 1;
}

static double crt_double_from_bits(uint64_t bits)
{
    double value;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

static double crt_scale_decimal(uint64_t mantissa, int decimal_exponent)
{
    if (decimal_exponent > 400)
        return crt_double_from_bits(0x7FF0000000000000ULL);
    if (decimal_exponent < -400)
        return 0.0;

    long double value = (long double)mantissa;
    long double factor = 10.0L;
    unsigned int power = decimal_exponent < 0
        ? (unsigned int)(-decimal_exponent)
        : (unsigned int)decimal_exponent;

    while (power) {
        if (power & 1U) {
            if (decimal_exponent < 0)
                value /= factor;
            else
                value *= factor;
        }
        power >>= 1;
        if (power) factor *= factor;
    }
    return (double)value;
}

static CRT_STRTOD_RESULT crt_parse_strtod(const char *input)
{
    CRT_STRTOD_RESULT result = { 0.0, input };
    if (!input) return result;

    const char *p = input;
    while (crt_ascii_space((unsigned char)*p)) p++;

    int negative = 0;
    if (*p == '-' || *p == '+') {
        negative = *p == '-';
        p++;
    }

    if (crt_ascii_word(p, "inf")) {
        p += 3;
        if (crt_ascii_word(p, "inity")) p += 5;
        uint64_t bits = 0x7FF0000000000000ULL;
        if (negative) bits |= 0x8000000000000000ULL;
        result.value = crt_double_from_bits(bits);
        result.end = p;
        return result;
    }

    if (crt_ascii_word(p, "nan")) {
        p += 3;
        if (*p == '(') {
            const char *payload = p + 1;
            const char *q = payload;
            while ((*q >= '0' && *q <= '9') ||
                   (*q >= 'A' && *q <= 'Z') ||
                   (*q >= 'a' && *q <= 'z') || *q == '_') {
                q++;
            }
            if (*q == ')') p = q + 1;
        }
        uint64_t bits = 0x7FF8000000000000ULL;
        if (negative) bits |= 0x8000000000000000ULL;
        result.value = crt_double_from_bits(bits);
        result.end = p;
        return result;
    }

    uint64_t mantissa = 0;
    uint32_t kept_digits = 0;
    int discarded_digits = 0;
    int fractional_digits = 0;
    int first_discarded = -1;
    int discarded_nonzero = 0;
    int saw_digit = 0;
    int saw_nonzero = 0;

    while (*p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p - '0');
        saw_digit = 1;
        if (digit || saw_nonzero) {
            saw_nonzero = 1;
            if (kept_digits < 19) {
                mantissa = mantissa * 10U + digit;
                kept_digits++;
            } else {
                if (discarded_digits < 1000000) discarded_digits++;
                if (first_discarded < 0)
                    first_discarded = (int)digit;
                else if (digit)
                    discarded_nonzero = 1;
            }
        }
        p++;
    }

    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            unsigned int digit = (unsigned int)(*p - '0');
            saw_digit = 1;
            if (fractional_digits < 1000000) fractional_digits++;
            if (digit || saw_nonzero) {
                saw_nonzero = 1;
                if (kept_digits < 19) {
                    mantissa = mantissa * 10U + digit;
                    kept_digits++;
                } else {
                    if (discarded_digits < 1000000) discarded_digits++;
                    if (first_discarded < 0)
                        first_discarded = (int)digit;
                    else if (digit)
                        discarded_nonzero = 1;
                }
            }
            p++;
        }
    }

    if (!saw_digit) return result;

    int explicit_exponent = 0;
    const char *exponent_mark = p;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int exponent_negative = 0;
        if (*q == '-' || *q == '+') {
            exponent_negative = *q == '-';
            q++;
        }
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') {
                if (explicit_exponent < 1000000)
                    explicit_exponent = explicit_exponent * 10 + (*q - '0');
                if (explicit_exponent > 1000000)
                    explicit_exponent = 1000000;
                q++;
            }
            if (exponent_negative) explicit_exponent = -explicit_exponent;
            p = q;
        } else {
            p = exponent_mark;
        }
    }

    if (first_discarded > 5 ||
        (first_discarded == 5 &&
         (discarded_nonzero || (mantissa & 1U)))) {
        mantissa++;
        if (mantissa == 10000000000000000000ULL) {
            mantissa = 1000000000000000000ULL;
            if (discarded_digits < 1000000) discarded_digits++;
        }
    }

    int64_t decimal_exponent = (int64_t)explicit_exponent +
                               discarded_digits - fractional_digits;
    int scale = decimal_exponent > 1000000 ? 1000000 :
                decimal_exponent < -1000000 ? -1000000 :
                (int)decimal_exponent;
    double value = mantissa ? crt_scale_decimal(mantissa, scale) : 0.0;
    if (negative) value = -value;

    if (mantissa) {
        uint64_t bits;
        __builtin_memcpy(&bits, &value, sizeof(bits));
        uint64_t exponent = bits & 0x7FF0000000000000ULL;
        if (exponent == 0 || exponent == 0x7FF0000000000000ULL)
            *crt_errno() = CRT_ERANGE;
    }

    result.value = value;
    result.end = p;
    return result;
}

double WINAPI crt_strtod(const char *s, char **endptr)
{
    CRT_STRTOD_RESULT result = crt_parse_strtod(s);
    if (endptr) *endptr = (char *)result.end;
    return result.value;
}

uint64_t WINAPI crt_strtod_compat32(uint64_t s_arg, uint64_t endptr_arg)
{
    const char *s = (const char *)(ULONG_PTR)(uint32_t)s_arg;
    CRT_STRTOD_RESULT result = crt_parse_strtod(s);
    if ((uint32_t)endptr_arg) {
        uint32_t *endptr = (uint32_t *)(ULONG_PTR)(uint32_t)endptr_arg;
        *endptr = (uint32_t)(ULONG_PTR)result.end;
    }

    uint64_t bits;
    __builtin_memcpy(&bits, &result.value, sizeof(bits));
    return bits;
}

int WINAPI crt_abs(int value)
{
    return value < 0 ? -value : value;
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

SIZE_T WINAPI crt_mbstowcs(WCHAR *destination, const char *source,
                           SIZE_T count)
{
    if (!source) {
        *crt_errno() = CRT_EINVAL;
        return (SIZE_T)-1;
    }

    if (!destination) {
        SIZE_T required = 0;
        while (source[required]) required++;
        return required;
    }

    SIZE_T converted = 0;
    while (converted < count) {
        unsigned char value = (unsigned char)source[converted];
        if (!value) {
            destination[converted] = 0;
            break;
        }
        destination[converted++] = (WCHAR)value;
    }
    return converted;
}

SIZE_T WINAPI crt_wcstombs(char *destination, const WCHAR *source,
                           SIZE_T count)
{
    if (!source) {
        *crt_errno() = CRT_EINVAL;
        return (SIZE_T)-1;
    }

    if (!destination) {
        SIZE_T required = 0;
        while (source[required]) {
            if (source[required] > 0xFF) {
                *crt_errno() = CRT_EILSEQ;
                return (SIZE_T)-1;
            }
            required++;
        }
        return required;
    }

    SIZE_T converted = 0;
    while (converted < count) {
        WCHAR value = source[converted];
        if (!value) {
            destination[converted] = 0;
            break;
        }
        if (value > 0xFF) {
            *crt_errno() = CRT_EILSEQ;
            return (SIZE_T)-1;
        }
        destination[converted++] = (char)value;
    }
    return converted;
}

/* ── Process ───────────────────────────────────────────────── */

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

void WINAPI crt_exit(int code)
{

    /* In compat32 mode, skip atexit handlers — PE32 cleanup code
     * tends to crash (NULL vtable calls, uninitialized subsystems).
     * Just terminate cleanly. */
    if (!g_compat32_mode) {
        for (int i = atexit_count - 1; i >= 0; i--)
            atexit_funcs[i]();
    }
    ExitProcess((DWORD)code);
}

void WINAPI crt_abort(void)
{
    serial_puts("[MSVCRT] abort() called\n");
    (void)crt_raise(22); /* SIGABRT */
    ExitProcess(3);      /* A returning/ignored handler cannot cancel abort. */
}

void WINAPI crt__exit(int code)
{
    ExitProcess((DWORD)code);
}

int WINAPI crt_getpid(void)
{
    return (int)GetCurrentProcessId();
}

ULONG_PTR WINAPI crt_beginthreadex(PVOID security, unsigned stack_size,
                                    PVOID start_address, PVOID argument,
                                    unsigned init_flags, unsigned *thread_id)
{
    HANDLE thread = CreateThread(
        security, (SIZE_T)stack_size,
        (LPTHREAD_START_ROUTINE)(ULONG_PTR)start_address, argument,
        (DWORD)init_flags, (DWORD *)thread_id);
    return (ULONG_PTR)thread;
}

void WINAPI __attribute__((noreturn)) crt_endthreadex(unsigned exit_code)
{
    ExitThread((DWORD)exit_code);
    __builtin_unreachable();
}

int WINAPI crt_atexit(void (*func)(void))
{
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

/* Universal CRT startup uses a caller-owned descriptor containing three
 * pointers: first registered callback, next free slot, and allocation end.
 * Keep the descriptor layout dependent on the importing PE instead of the
 * kernel's native pointer width. */
typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t end;
} UCRT_ONEXIT_TABLE32;

typedef struct {
    ULONG_PTR first;
    ULONG_PTR last;
    ULONG_PTR end;
} UCRT_ONEXIT_TABLE64;

#define UCRT_ONEXIT_INITIAL_CAPACITY 32U
#define UCRT_ONEXIT_MAX_CAPACITY     (1U << 20)
#define UCRT_INVALID_HANDLER_SLOTS   512U
#define UCRT_PROCESS_MODE_SLOTS      512U

typedef struct {
    DWORD owner_pid;
    DWORD owner_tid;
    PVOID handler;
    BOOL used;
} UCRT_INVALID_HANDLER_SLOT;

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

typedef struct {
    int commode;
    int fmode;
    int new_mode;
    PVOID new_handler;
    struct crt_tm time_buffer;
    char ctime_buffer[26];
    LONG timezone;
    int daylight;
    int adjust_fdiv;
    int mb_cur_max;
    ULONG_PTR acmdln_value;
    ULONG_PTR initenv_value;
    ULONG_PTR initenv_entries[1];
    ULONG_PTR signal_handlers[7];
    char command_line[4096];
    PVOID invalid_parameter_handler;
} UCRT_PROCESS_MODE_VALUES;

typedef struct {
    DWORD owner_pid;
    UCRT_PROCESS_MODE_VALUES *values;
} UCRT_PROCESS_MODE_SLOT;

typedef struct {
    int errno_value;
    ULONG doserrno_value;
    WCHAR wcserror_buffer[64];
    char *strtok_context;
} UCRT_THREAD_VALUES;

typedef struct _UCRT_THREAD_STATE {
    struct _UCRT_THREAD_STATE *next;
    DWORD owner_pid;
    DWORD owner_tid;
    UCRT_THREAD_VALUES *values;
} UCRT_THREAD_STATE;

static UCRT_INVALID_HANDLER_SLOT ucrt_invalid_handlers[
    UCRT_INVALID_HANDLER_SLOTS];
static UCRT_PROCESS_MODE_SLOT ucrt_process_modes[UCRT_PROCESS_MODE_SLOTS];
static UCRT_THREAD_STATE *ucrt_thread_states;
static volatile uint32_t ucrt_state_lock;

static void crt_env_release_process(DWORD process_id);
static void crt_locale_release_process(DWORD process_id);

static void ucrt_state_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&ucrt_state_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static void ucrt_state_lock_release(void)
{
    __sync_lock_release(&ucrt_state_lock);
}

static UCRT_THREAD_VALUES *ucrt_thread_state(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    DWORD owner_tid = GetCurrentThreadId();
    if (!owner_pid) owner_pid = 1;

    UCRT_THREAD_VALUES *values = NULL;
    ucrt_state_lock_acquire();
    for (UCRT_THREAD_STATE *state = ucrt_thread_states; state;
         state = state->next) {
        if (state->owner_pid == owner_pid && state->owner_tid == owner_tid) {
            values = state->values;
            break;
        }
    }

    if (!values && create) {
        UCRT_THREAD_STATE *state =
            (UCRT_THREAD_STATE *)kmalloc(sizeof(*state));
        values = (UCRT_THREAD_VALUES *)VirtualAlloc(
            NULL, sizeof(*values), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!state || !values ||
            (g_compat32_mode &&
             (ULONG_PTR)values > (ULONG_PTR)UINT32_MAX)) {
            if (values) VirtualFree(values, 0, MEM_RELEASE);
            if (state) kfree(state);
            values = NULL;
        } else {
            values->errno_value = 0;
            values->doserrno_value = 0;
            values->strtok_context = NULL;
            state->owner_pid = owner_pid;
            state->owner_tid = owner_tid;
            state->values = values;
            state->next = ucrt_thread_states;
            ucrt_thread_states = state;

            serial_puts("[CRT-PTD] pid=");
            serial_putdec(owner_pid);
            serial_puts(" tid=");
            serial_putdec(owner_tid);
            serial_puts(" va=0x");
            serial_puthex((ULONG_PTR)values,
                          g_compat32_mode ? 8 : 16);
            serial_puts("\n");
        }
    }
    ucrt_state_lock_release();
    return values;
}

static char **crt_strtok_context(void)
{
    UCRT_THREAD_VALUES *values = ucrt_thread_state(TRUE);
    return values ? &values->strtok_context : NULL;
}

static UCRT_PROCESS_MODE_VALUES *ucrt_process_mode_state(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    UCRT_PROCESS_MODE_SLOT *free_slot = NULL;
    UCRT_PROCESS_MODE_VALUES *values = NULL;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        UCRT_PROCESS_MODE_SLOT *slot = &ucrt_process_modes[i];
        if (slot->owner_pid == owner_pid) {
            values = slot->values;
            break;
        }
        if (!slot->owner_pid && !free_slot)
            free_slot = slot;
    }

    if (!values && create && free_slot) {
        values = (UCRT_PROCESS_MODE_VALUES *)VirtualAlloc(
            NULL, sizeof(*values), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (values && (!g_compat32_mode ||
                       (ULONG_PTR)values <= (ULONG_PTR)UINT32_MAX)) {
            values->commode = 0;
            values->fmode = 0x4000; /* _O_TEXT */
            values->new_mode = 0;
            values->new_handler = NULL;
            values->timezone = 0;
            values->daylight = 0;
            values->adjust_fdiv = 0;
            values->mb_cur_max = 1;
            values->acmdln_value = (ULONG_PTR)values->command_line;
            values->initenv_value = (ULONG_PTR)values->initenv_entries;
            values->initenv_entries[0] = 0;
            const char *command_line = win32_current_command_line();
            SIZE_T command_length = 0;
            if (command_line) {
                while (command_line[command_length] &&
                       command_length + 1 < sizeof(values->command_line)) {
                    values->command_line[command_length] =
                        command_line[command_length];
                    command_length++;
                }
            }
            values->command_line[command_length] = 0;
            free_slot->owner_pid = owner_pid;
            free_slot->values = values;
            serial_puts("[CRT] process mode state pid=");
            serial_putdec(owner_pid);
            serial_puts(" va=0x");
            serial_puthex((ULONG_PTR)values, g_compat32_mode ? 8 : 16);
            serial_puts("\n");
        } else {
            if (values) VirtualFree(values, 0, MEM_RELEASE);
            values = NULL;
        }
    }
    ucrt_state_lock_release();
    return values;
}

static int crt_exchange_new_mode(int mode)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return 0;

    ucrt_state_lock_acquire();
    int previous = values->new_mode;
    values->new_mode = mode;
    ucrt_state_lock_release();
    return previous;
}

static PVOID crt_exchange_new_handler(PVOID handler)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return NULL;

    ucrt_state_lock_acquire();
    PVOID previous = values->new_handler;
    values->new_handler = handler;
    ucrt_state_lock_release();
    return previous;
}

void msvcrt_release_process(DWORD process_id)
{
    if (!process_id) return;

    UCRT_THREAD_STATE *released_thread_states = NULL;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        if (ucrt_process_modes[i].owner_pid == process_id) {
            ucrt_process_modes[i].owner_pid = 0;
            ucrt_process_modes[i].values = NULL;
        }
    }
    for (uint32_t i = 0; i < UCRT_INVALID_HANDLER_SLOTS; i++) {
        if (ucrt_invalid_handlers[i].used &&
            ucrt_invalid_handlers[i].owner_pid == process_id) {
            ucrt_invalid_handlers[i].used = FALSE;
            ucrt_invalid_handlers[i].owner_pid = 0;
            ucrt_invalid_handlers[i].owner_tid = 0;
            ucrt_invalid_handlers[i].handler = NULL;
        }
    }
    UCRT_THREAD_STATE **link = &ucrt_thread_states;
    while (*link) {
        UCRT_THREAD_STATE *state = *link;
        if (state->owner_pid == process_id) {
            *link = state->next;
            state->next = released_thread_states;
            released_thread_states = state;
        } else {
            link = &state->next;
        }
    }
    ucrt_state_lock_release();

    while (released_thread_states) {
        UCRT_THREAD_STATE *next = released_thread_states->next;
        kfree(released_thread_states);
        released_thread_states = next;
    }

    crt_locale_release_process(process_id);
    crt_env_release_process(process_id);
    crt_file_proxy_release_process(process_id);
}

static void ucrt_onexit_read(PVOID opaque, BOOL is_32bit,
                             ULONG_PTR *first, ULONG_PTR *last,
                             ULONG_PTR *end)
{
    if (is_32bit) {
        UCRT_ONEXIT_TABLE32 *table = (UCRT_ONEXIT_TABLE32 *)opaque;
        *first = table->first;
        *last = table->last;
        *end = table->end;
    } else {
        UCRT_ONEXIT_TABLE64 *table = (UCRT_ONEXIT_TABLE64 *)opaque;
        *first = table->first;
        *last = table->last;
        *end = table->end;
    }
}

static void ucrt_onexit_write(PVOID opaque, BOOL is_32bit,
                              ULONG_PTR first, ULONG_PTR last,
                              ULONG_PTR end)
{
    if (is_32bit) {
        UCRT_ONEXIT_TABLE32 *table = (UCRT_ONEXIT_TABLE32 *)opaque;
        table->first = (uint32_t)first;
        table->last = (uint32_t)last;
        table->end = (uint32_t)end;
    } else {
        UCRT_ONEXIT_TABLE64 *table = (UCRT_ONEXIT_TABLE64 *)opaque;
        table->first = first;
        table->last = last;
        table->end = end;
    }
}

static BOOL ucrt_onexit_valid(ULONG_PTR first, ULONG_PTR last,
                              ULONG_PTR end, SIZE_T slot_size)
{
    if (!first && !last && !end) return TRUE;
    if (!first || !last || !end || last < first || end < last)
        return FALSE;
    if ((last - first) % slot_size || (end - first) % slot_size)
        return FALSE;
    return (end - first) / slot_size <= UCRT_ONEXIT_MAX_CAPACITY;
}

int WINAPI crt_initialize_onexit_table(PVOID opaque)
{
    if (!opaque) return -1;

    ucrt_state_lock_acquire();
    ucrt_onexit_write(opaque, g_compat32_mode ? TRUE : FALSE, 0, 0, 0);
    ucrt_state_lock_release();
    return 0;
}

int WINAPI crt_register_onexit_function(PVOID opaque, PVOID function)
{
    if (!opaque || !function) return -1;

    BOOL is_32bit = g_compat32_mode ? TRUE : FALSE;
    SIZE_T slot_size = is_32bit ? sizeof(uint32_t) : sizeof(ULONG_PTR);
    ULONG_PTR first, last, end;

    ucrt_state_lock_acquire();
    ucrt_onexit_read(opaque, is_32bit, &first, &last, &end);
    if (!ucrt_onexit_valid(first, last, end, slot_size)) {
        ucrt_state_lock_release();
        return -1;
    }

    SIZE_T count = first ? (last - first) / slot_size : 0;
    SIZE_T capacity = first ? (end - first) / slot_size : 0;
    if (count == capacity) {
        SIZE_T new_capacity = capacity ? capacity * 2 :
                              UCRT_ONEXIT_INITIAL_CAPACITY;
        if (new_capacity > UCRT_ONEXIT_MAX_CAPACITY ||
            new_capacity < capacity) {
            ucrt_state_lock_release();
            return -1;
        }

        SIZE_T bytes = new_capacity * slot_size;
        PVOID storage = first
            ? HeapReAlloc(GetProcessHeap(), 0, (PVOID)first, bytes)
            : HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!storage || (is_32bit && (ULONG_PTR)storage > 0xFFFFFFFFULL)) {
            if (storage && !first)
                HeapFree(GetProcessHeap(), 0, storage);
            ucrt_state_lock_release();
            return -1;
        }
        first = (ULONG_PTR)storage;
        last = first + count * slot_size;
        end = first + new_capacity * slot_size;
    }

    if (is_32bit)
        *(uint32_t *)last = (uint32_t)(ULONG_PTR)function;
    else
        *(ULONG_PTR *)last = (ULONG_PTR)function;
    last += slot_size;
    ucrt_onexit_write(opaque, is_32bit, first, last, end);
    ucrt_state_lock_release();
    return 0;
}

int WINAPI crt_execute_onexit_table(PVOID opaque)
{
    if (!opaque) return -1;

    BOOL is_32bit = g_compat32_mode ? TRUE : FALSE;
    SIZE_T slot_size = is_32bit ? sizeof(uint32_t) : sizeof(ULONG_PTR);
    ULONG_PTR first, last, end;

    ucrt_state_lock_acquire();
    ucrt_onexit_read(opaque, is_32bit, &first, &last, &end);
    if (!ucrt_onexit_valid(first, last, end, slot_size)) {
        ucrt_state_lock_release();
        return -1;
    }
    ucrt_onexit_write(opaque, is_32bit, 0, 0, 0);
    ucrt_state_lock_release();

    while (last > first) {
        last -= slot_size;
        ULONG_PTR callback = is_32bit
            ? *(uint32_t *)last : *(ULONG_PTR *)last;
        if (!callback) continue;
        if (is_32bit)
            compat32_callback((uint32_t)callback);
        else
            ((void (WINAPI *)(void))callback)();
    }

    if (first)
        HeapFree(GetProcessHeap(), 0, (PVOID)first);
    return 0;
}

int WINAPI crt_configure_narrow_argv(int mode)
{
    /* _crt_argv_mode has exactly three public values. OsitoK already
     * supplies normalized argv storage; wildcard expansion is not needed by
     * the Win32 loader itself. */
    return mode >= 0 && mode <= 2 ? 0 : -1;
}

int WINAPI crt_initialize_narrow_environment(void)
{
    /* __getmainargs exposes the process environment initialized by winexec. */
    return 0;
}

char *WINAPI crt_get_narrow_winmain_command_line(void)
{
    char *command = (char *)(ULONG_PTR)GetCommandLineA();
    if (!command)
        return NULL;

    char *cursor = command;
    if (*cursor == '"') {
        cursor++;
        while (*cursor && *cursor != '"')
            cursor++;
        if (*cursor == '"')
            cursor++;
    } else {
        while (*cursor && *cursor != ' ' && *cursor != '\t')
            cursor++;
    }
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    return cursor;
}

PVOID WINAPI crt_set_thread_local_invalid_parameter_handler(PVOID handler)
{
    DWORD owner_pid = GetCurrentProcessId();
    DWORD owner_tid = GetCurrentThreadId();
    uint32_t start = (owner_pid * 2654435761U ^ owner_tid) &
                     (UCRT_INVALID_HANDLER_SLOTS - 1);
    UCRT_INVALID_HANDLER_SLOT *free_slot = NULL;
    PVOID previous = NULL;

    ucrt_state_lock_acquire();
    for (uint32_t probe = 0; probe < UCRT_INVALID_HANDLER_SLOTS; probe++) {
        UCRT_INVALID_HANDLER_SLOT *slot =
            &ucrt_invalid_handlers[(start + probe) &
                                   (UCRT_INVALID_HANDLER_SLOTS - 1)];
        if (slot->used && slot->owner_pid == owner_pid &&
            slot->owner_tid == owner_tid) {
            previous = slot->handler;
            if (handler) {
                slot->handler = handler;
            } else {
                slot->used = FALSE;
                slot->owner_pid = 0;
                slot->owner_tid = 0;
                slot->handler = NULL;
            }
            ucrt_state_lock_release();
            return previous;
        }
        if (!slot->used && !free_slot) free_slot = slot;
    }

    if (handler && free_slot) {
        free_slot->owner_pid = owner_pid;
        free_slot->owner_tid = owner_tid;
        free_slot->handler = handler;
        free_slot->used = TRUE;
    }
    ucrt_state_lock_release();
    return previous;
}

PVOID WINAPI crt_get_thread_local_invalid_parameter_handler(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    PVOID handler = NULL;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_INVALID_HANDLER_SLOTS; i++) {
        UCRT_INVALID_HANDLER_SLOT *slot = &ucrt_invalid_handlers[i];
        if (slot->used && slot->owner_pid == pid && slot->owner_tid == tid) {
            handler = slot->handler;
            break;
        }
    }
    ucrt_state_lock_release();
    return handler;
}

PVOID WINAPI crt_set_invalid_parameter_handler(PVOID handler)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return NULL;
    return __atomic_exchange_n(&values->invalid_parameter_handler, handler,
                               __ATOMIC_ACQ_REL);
}

PVOID WINAPI crt_get_invalid_parameter_handler(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(FALSE);
    return values ? __atomic_load_n(&values->invalid_parameter_handler,
                                     __ATOMIC_ACQUIRE) : NULL;
}

static void crt_report_invalid_parameter(void)
{
    PVOID handler = crt_get_thread_local_invalid_parameter_handler();
    if (!handler) handler = crt_get_invalid_parameter_handler();
    if (!handler) {
        serial_puts("[MSVCRT] invalid parameter without a handler\n");
        ExitProcess(0xC0000417U); /* STATUS_INVALID_CRUNTIME_PARAMETER */
        return;
    }
    if (g_compat32_mode) {
        uint32_t args[5] = { 0, 0, 0, 0, 0 };
        (void)compat32_callback_args((uint32_t)(ULONG_PTR)handler, 5, args);
    } else {
        typedef void (WINAPI *INVALID_HANDLER)(const WCHAR *, const WCHAR *,
                                                const WCHAR *, unsigned,
                                                ULONG_PTR);
        ((INVALID_HANDLER)handler)(NULL, NULL, NULL, 0, 0);
    }
}

/* Locale. The CRT starts in C and OsitoK currently has no configurable
 * user-locale backend, so an empty locale name resolves to C as well. */
#define CRT_LC_ALL       0
#define CRT_LC_COLLATE   1
#define CRT_LC_CTYPE     2
#define CRT_LC_MONETARY  3
#define CRT_LC_NUMERIC   4
#define CRT_LC_TIME      5
#define CRT_CHAR_MAX     127

typedef struct {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    WCHAR *_W_decimal_point;
    WCHAR *_W_thousands_sep;
    WCHAR *_W_int_curr_symbol;
    WCHAR *_W_currency_symbol;
    WCHAR *_W_mon_decimal_point;
    WCHAR *_W_mon_thousands_sep;
    WCHAR *_W_positive_sign;
    WCHAR *_W_negative_sign;
} CRT_LCONV;

/* PE32 sees 32-bit pointers inside struct lconv even though the shim itself
 * is compiled as x86-64. Keep a separate layout for compat-mode callers. */
typedef struct {
    uint32_t decimal_point;
    uint32_t thousands_sep;
    uint32_t grouping;
    uint32_t int_curr_symbol;
    uint32_t currency_symbol;
    uint32_t mon_decimal_point;
    uint32_t mon_thousands_sep;
    uint32_t mon_grouping;
    uint32_t positive_sign;
    uint32_t negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    uint32_t _W_decimal_point;
    uint32_t _W_thousands_sep;
    uint32_t _W_int_curr_symbol;
    uint32_t _W_currency_symbol;
    uint32_t _W_mon_decimal_point;
    uint32_t _W_mon_thousands_sep;
    uint32_t _W_positive_sign;
    uint32_t _W_negative_sign;
} CRT_LCONV32;

static char crt_locale_c[] = "C";
static char crt_locale_dot[] = ".";
static char crt_locale_empty[] = "";
static WCHAR crt_wlocale_c[] = { 'C', 0 };
static WCHAR crt_wlocale_dot[] = { '.', 0 };
static WCHAR crt_wlocale_empty[] = { 0 };

static CRT_LCONV crt_c_lconv = {
    .decimal_point = crt_locale_dot,
    .thousands_sep = crt_locale_empty,
    .grouping = crt_locale_empty,
    .int_curr_symbol = crt_locale_empty,
    .currency_symbol = crt_locale_empty,
    .mon_decimal_point = crt_locale_empty,
    .mon_thousands_sep = crt_locale_empty,
    .mon_grouping = crt_locale_empty,
    .positive_sign = crt_locale_empty,
    .negative_sign = crt_locale_empty,
    .int_frac_digits = CRT_CHAR_MAX,
    .frac_digits = CRT_CHAR_MAX,
    .p_cs_precedes = CRT_CHAR_MAX,
    .p_sep_by_space = CRT_CHAR_MAX,
    .n_cs_precedes = CRT_CHAR_MAX,
    .n_sep_by_space = CRT_CHAR_MAX,
    .p_sign_posn = CRT_CHAR_MAX,
    .n_sign_posn = CRT_CHAR_MAX,
    ._W_decimal_point = crt_wlocale_dot,
    ._W_thousands_sep = crt_wlocale_empty,
    ._W_int_curr_symbol = crt_wlocale_empty,
    ._W_currency_symbol = crt_wlocale_empty,
    ._W_mon_decimal_point = crt_wlocale_empty,
    ._W_mon_thousands_sep = crt_wlocale_empty,
    ._W_positive_sign = crt_wlocale_empty,
    ._W_negative_sign = crt_wlocale_empty,
};

typedef struct {
    CRT_LCONV32 lconv;
    char locale_c[2];
    char locale_dot[2];
    char locale_empty[1];
    WCHAR wlocale_c[2];
    WCHAR wlocale_dot[2];
    WCHAR wlocale_empty[1];
} CRT_LOCALE32_BLOCK;

typedef struct {
    DWORD owner_pid;
    CRT_LOCALE32_BLOCK *block;
} CRT_LOCALE32_SLOT;

static CRT_LOCALE32_SLOT crt_locale32_slots[UCRT_PROCESS_MODE_SLOTS];

static void crt_locale32_block_init(CRT_LOCALE32_BLOCK *block)
{
    memset(block, 0, sizeof(*block));
    block->locale_c[0] = 'C';
    block->locale_dot[0] = '.';
    block->wlocale_c[0] = 'C';
    block->wlocale_dot[0] = '.';

    uint32_t dot = (uint32_t)(ULONG_PTR)block->locale_dot;
    uint32_t empty = (uint32_t)(ULONG_PTR)block->locale_empty;
    uint32_t wdot = (uint32_t)(ULONG_PTR)block->wlocale_dot;
    uint32_t wempty = (uint32_t)(ULONG_PTR)block->wlocale_empty;

    block->lconv.decimal_point = dot;
    block->lconv.thousands_sep = empty;
    block->lconv.grouping = empty;
    block->lconv.int_curr_symbol = empty;
    block->lconv.currency_symbol = empty;
    block->lconv.mon_decimal_point = empty;
    block->lconv.mon_thousands_sep = empty;
    block->lconv.mon_grouping = empty;
    block->lconv.positive_sign = empty;
    block->lconv.negative_sign = empty;
    block->lconv.int_frac_digits = CRT_CHAR_MAX;
    block->lconv.frac_digits = CRT_CHAR_MAX;
    block->lconv.p_cs_precedes = CRT_CHAR_MAX;
    block->lconv.p_sep_by_space = CRT_CHAR_MAX;
    block->lconv.n_cs_precedes = CRT_CHAR_MAX;
    block->lconv.n_sep_by_space = CRT_CHAR_MAX;
    block->lconv.p_sign_posn = CRT_CHAR_MAX;
    block->lconv.n_sign_posn = CRT_CHAR_MAX;
    block->lconv._W_decimal_point = wdot;
    block->lconv._W_thousands_sep = wempty;
    block->lconv._W_int_curr_symbol = wempty;
    block->lconv._W_currency_symbol = wempty;
    block->lconv._W_mon_decimal_point = wempty;
    block->lconv._W_mon_thousands_sep = wempty;
    block->lconv._W_positive_sign = wempty;
    block->lconv._W_negative_sign = wempty;
}

static CRT_LOCALE32_BLOCK *crt_locale32_state(BOOL create)
{
    DWORD owner_pid = win32_current_process_id();
    if (!owner_pid) owner_pid = 1;

    CRT_LOCALE32_BLOCK *block = NULL;
    BOOL have_free_slot = FALSE;
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        CRT_LOCALE32_SLOT *slot = &crt_locale32_slots[i];
        if (slot->owner_pid == owner_pid) {
            block = slot->block;
            break;
        }
        if (!slot->owner_pid)
            have_free_slot = TRUE;
    }
    ucrt_state_lock_release();

    if (block || !create || !have_free_slot)
        return block;

    CRT_LOCALE32_BLOCK *candidate = (CRT_LOCALE32_BLOCK *)VirtualAlloc(
        NULL, sizeof(*candidate), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!candidate ||
        (ULONG_PTR)candidate > (ULONG_PTR)UINT32_MAX - sizeof(*candidate) + 1) {
        if (candidate)
            VirtualFree(candidate, 0, MEM_RELEASE);
        return NULL;
    }
    crt_locale32_block_init(candidate);

    CRT_LOCALE32_BLOCK *unused = candidate;
    ucrt_state_lock_acquire();
    CRT_LOCALE32_SLOT *free_slot = NULL;
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        CRT_LOCALE32_SLOT *slot = &crt_locale32_slots[i];
        if (slot->owner_pid == owner_pid) {
            block = slot->block;
            break;
        }
        if (!slot->owner_pid && !free_slot)
            free_slot = slot;
    }
    if (!block && free_slot) {
        free_slot->owner_pid = owner_pid;
        free_slot->block = candidate;
        block = candidate;
        unused = NULL;
    }
    ucrt_state_lock_release();

    if (unused)
        VirtualFree(unused, 0, MEM_RELEASE);
    if (block == candidate) {
        serial_puts("[CRT] PE32 locale state pid=");
        serial_putdec(owner_pid);
        serial_puts(" va=0x");
        serial_puthex((ULONG_PTR)block, 8);
        serial_puts("\n");
    }
    return block;
}

static void crt_locale_release_process(DWORD process_id)
{
    ucrt_state_lock_acquire();
    for (uint32_t i = 0; i < UCRT_PROCESS_MODE_SLOTS; i++) {
        CRT_LOCALE32_SLOT *slot = &crt_locale32_slots[i];
        if (slot->owner_pid == process_id) {
            slot->owner_pid = 0;
            slot->block = NULL;
        }
    }
    ucrt_state_lock_release();
}

static BOOL crt_locale_category_valid(int category)
{
    return category >= CRT_LC_ALL && category <= CRT_LC_TIME;
}

static BOOL crt_locale_name_is_c(const char *locale)
{
    if (!locale[0]) return TRUE;
    if (locale[0] == 'C' && !locale[1]) return TRUE;
    return locale[0] == 'P' && locale[1] == 'O' && locale[2] == 'S' &&
           locale[3] == 'I' && locale[4] == 'X' && !locale[5];
}

static BOOL crt_wlocale_name_is_c(const WCHAR *locale)
{
    if (!locale[0]) return TRUE;
    if (locale[0] == 'C' && !locale[1]) return TRUE;
    return locale[0] == 'P' && locale[1] == 'O' && locale[2] == 'S' &&
           locale[3] == 'I' && locale[4] == 'X' && !locale[5];
}

char* WINAPI crt_setlocale(int category, const char *locale)
{
    if (!crt_locale_category_valid(category)) {
        *crt_errno() = 22; /* EINVAL */
        return NULL;
    }
    if (!locale || crt_locale_name_is_c(locale)) {
        if (g_compat32_mode) {
            CRT_LOCALE32_BLOCK *block = crt_locale32_state(TRUE);
            return block ? block->locale_c : NULL;
        }
        return crt_locale_c;
    }
    return NULL;
}

WCHAR* WINAPI crt_wsetlocale(int category, const WCHAR *locale)
{
    if (!crt_locale_category_valid(category)) {
        *crt_errno() = 22; /* EINVAL */
        return NULL;
    }
    if (!locale || crt_wlocale_name_is_c(locale)) {
        if (g_compat32_mode) {
            CRT_LOCALE32_BLOCK *block = crt_locale32_state(TRUE);
            return block ? block->wlocale_c : NULL;
        }
        return crt_wlocale_c;
    }
    return NULL;
}

PVOID WINAPI crt_localeconv(void)
{
    if (g_compat32_mode) {
        CRT_LOCALE32_BLOCK *block = crt_locale32_state(TRUE);
        return block ? &block->lconv : NULL;
    }
    return &crt_c_lconv;
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

static BOOL crt_is_wide_upper(int c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 0x00C0 && c <= 0x00D6) ||
           (c >= 0x00D8 && c <= 0x00DE) || c == 0x0178;
}

static BOOL crt_is_wide_lower(int c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 0x00E0 && c <= 0x00F6) ||
           (c >= 0x00F8 && c <= 0x00FF);
}

int WINAPI crt_towupper(int c)
{
    if ((c >= 'a' && c <= 'z') ||
        (c >= 0x00E0 && c <= 0x00F6) ||
        (c >= 0x00F8 && c <= 0x00FE))
        return c - 0x20;
    if (c == 0x00FF) return 0x0178;
    return c;
}

int WINAPI crt_towlower(int c)
{
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 0x00C0 && c <= 0x00D6) ||
        (c >= 0x00D8 && c <= 0x00DE))
        return c + 0x20;
    if (c == 0x0178) return 0x00FF;
    return c;
}

static int crt_wide_ctype_mask(int c)
{
    const int upper = 0x0001;
    const int lower = 0x0002;
    const int digit = 0x0004;
    const int space = 0x0008;
    const int punct = 0x0010;
    const int control = 0x0020;
    const int blank = 0x0040;
    const int hex = 0x0080;
    const int alpha = 0x0100;
    int result = 0;

    if (crt_is_wide_upper(c)) result |= upper | alpha;
    if (crt_is_wide_lower(c)) result |= lower | alpha;
    if (c >= '0' && c <= '9') result |= digit;
    if (c == ' ' || c == '\t') result |= blank;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v')
        result |= space;
    if ((c >= 0 && c < 0x20) || c == 0x7F) result |= control;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
        (c >= 'a' && c <= 'f'))
        result |= hex;
    if (c >= 0x20 && c <= 0x7E &&
        !(result & (upper | lower | digit | space)))
        result |= punct;
    return result;
}

int WINAPI crt_iswctype(int c, int mask)
{
    return crt_wide_ctype_mask(c) & mask;
}

int WINAPI crt_iswalpha(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0100) != 0;
}

int WINAPI crt_iswalnum(int c)
{
    return (crt_wide_ctype_mask(c) & (0x0100 | 0x0004)) != 0;
}

int WINAPI crt_iswdigit(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0004) != 0;
}

int WINAPI crt_iswspace(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0008) != 0;
}

int WINAPI crt_iswupper(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0001) != 0;
}

int WINAPI crt_iswlower(int c)
{
    return (crt_wide_ctype_mask(c) & 0x0002) != 0;
}

int WINAPI crt_iswprint(int c)
{
    return c >= 0x20 && c != 0x7F && c <= 0xFFFF;
}

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

static int crt_errno_fallback;
static ULONG crt_doserrno_fallback;
static WCHAR crt_wcserror_fallback[64];

static char *crt_error_messages[] = {
    "No error",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted function call",
    "Input/output error",
    "No such device or address",
    "Arg list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource temporarily unavailable",
    "Not enough space",
    "Permission denied",
    "Bad address",
    "Unknown error",
    "Resource device",
    "File exists",
    "Improper link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate I/O control operation",
    "Unknown error",
    "File too large",
    "No space left on device",
    "Invalid seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Domain error",
    "Result too large",
    "Unknown error",
    "Resource deadlock avoided",
    "Unknown error",
    "Filename too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    "Illegal byte sequence",
};

static int crt_sys_nerr_val =
    (int)(sizeof(crt_error_messages) / sizeof(crt_error_messages[0]));

int* WINAPI crt_errno(void)
{
    UCRT_THREAD_VALUES *values = ucrt_thread_state(TRUE);
    return values ? &values->errno_value : &crt_errno_fallback;
}

ULONG* WINAPI crt_doserrno(void)
{
    UCRT_THREAD_VALUES *values = ucrt_thread_state(TRUE);
    return values ? &values->doserrno_value : &crt_doserrno_fallback;
}

char** WINAPI crt_sys_errlist(void) { return crt_error_messages; }

int* WINAPI crt_sys_nerr(void) { return &crt_sys_nerr_val; }

char* WINAPI crt_strerror(int error)
{
    if (error >= 0 && error < crt_sys_nerr_val)
        return crt_error_messages[error];
    return "Unknown error";
}

WCHAR* WINAPI crt_wcserror(int error)
{
    UCRT_THREAD_VALUES *values = ucrt_thread_state(TRUE);
    WCHAR *buffer = values ? values->wcserror_buffer
                           : crt_wcserror_fallback;
    const char *message = crt_strerror(error);
    SIZE_T index = 0;

    while (message[index] && index + 1 < 64) {
        buffer[index] = (WCHAR)(unsigned char)message[index];
        index++;
    }
    buffer[index] = 0;
    return buffer;
}

int WINAPI crt_fpe_flt_rounds(void)
{
    unsigned int mxcsr;
    __asm__ volatile ("stmxcsr %0" : "=m"(mxcsr));

    switch ((mxcsr >> 13) & 3U) {
    case 0: return 1; /* nearest */
    case 1: return 3; /* toward negative infinity */
    case 2: return 2; /* toward positive infinity */
    default: return 0; /* toward zero */
    }
}

/* ── Time ──────────────────────────────────────────────────── */

extern uint64_t idt_get_ticks(void);

void WINAPI crt_tzset(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return;
    values->timezone = 0;
    values->daylight = 0;
}

LONG* WINAPI crt_timezone(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->timezone : NULL;
}

int* WINAPI crt_daylight(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->daylight : NULL;
}

typedef struct {
    int32_t  time;
    uint16_t millitm;
    int16_t  timezone;
    int16_t  dstflag;
} CRT_TIMEB32;

typedef struct {
    int64_t  time;
    uint16_t millitm;
    int16_t  timezone;
    int16_t  dstflag;
} CRT_TIMEB64;

_Static_assert(sizeof(CRT_TIMEB32) == 12, "PE32 _timeb layout changed");
_Static_assert(sizeof(CRT_TIMEB64) == 16, "PE64 _timeb layout changed");

static void crt_ftime_snapshot(int64_t *seconds, uint16_t *milliseconds)
{
    uint64_t filetime = wintime_now_filetime();
    *seconds = filetime >= WINTIME_UNIX_EPOCH_FILETIME
        ? (int64_t)((filetime - WINTIME_UNIX_EPOCH_FILETIME) /
                    WINTIME_TICKS_PER_SECOND)
        : -1;
    *milliseconds = (uint16_t)(
        (filetime % WINTIME_TICKS_PER_SECOND) / 10000ULL);
}

static void WINAPI crt_ftime32(CRT_TIMEB32 *result)
{
    if (!result) {
        *crt_errno() = CRT_EINVAL;
        return;
    }

    int64_t seconds;
    uint16_t milliseconds;
    crt_ftime_snapshot(&seconds, &milliseconds);
    result->time = (int32_t)seconds;
    result->millitm = milliseconds;
    result->timezone = 0;
    result->dstflag = 0;
}

static void WINAPI crt_ftime64(CRT_TIMEB64 *result)
{
    if (!result) {
        *crt_errno() = CRT_EINVAL;
        return;
    }

    crt_ftime_snapshot(&result->time, &result->millitm);
    result->timezone = 0;
    result->dstflag = 0;
}

void WINAPI crt_ftime(PVOID result)
{
    if (g_compat32_mode)
        crt_ftime32((CRT_TIMEB32 *)result);
    else
        crt_ftime64((CRT_TIMEB64 *)result);
}

crt_time_t WINAPI crt_time(crt_time_t *timer)
{
    int64_t unix_seconds = wintime_now_unix_seconds();
    crt_time_t t = unix_seconds >= 0 ? (crt_time_t)unix_seconds
                                     : (crt_time_t)-1;
    if (timer) *timer = t;
    return t;
}

crt_clock_t WINAPI crt_clock(void)
{
    /* MSVCRT CLOCKS_PER_SEC is 1000; the APIC clock advances at 100 Hz. */
    return (crt_clock_t)(idt_get_ticks() * 10ULL);
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
    (void)ContextRecord;

    /*
     * Read ExceptionCode + ExceptionFlags.
     * In compat32 mode, the ExceptionRecord pointer is a 32-bit address
     * pointing to our static seh32_exception_record (EXCEPTION_RECORD32).
     * ExceptionCode (+0) and ExceptionFlags (+4) are at the same offsets
     * in both 32-bit and 64-bit layouts, so direct access is safe.
     */
    DWORD code  = ExceptionRecord->ExceptionCode;
    DWORD flags = ExceptionRecord->ExceptionFlags;

    /* PE32 unwind runs termination funclets after validating its frame. */
    if ((flags & EXCEPTION_UNWIND) && !g_compat32_mode) {
        return ExceptionContinueSearch;
    }

    serial_puts("[SEH] _except_handler3: code=0x");
    serial_puthex(code, 8);
    serial_puts("\n");


    /*
     * Walk the scopetable from current TryLevel upward.
     *
     * In compat32 mode, EstablisherFrame points to a 32-bit struct:
     *   +0:  uint32_t Next
     *   +4:  uint32_t Handler
     *   +8:  uint32_t ScopeTable   ← 32-bit pointer
     *   +12: uint32_t TryLevel
     *
     * The 64-bit EH3_EXCEPTION_REGISTRATION has 8-byte pointers, so
     * ScopeTable is at +16 and TryLevel at +24.  We must read manually.
     *
     * Scopetable entries are also 32-bit (12 bytes each):
     *   +0: uint32_t EnclosingLevel
     *   +4: uint32_t FilterFunc    ← 32-bit code pointer
     *   +8: uint32_t HandlerFunc   ← 32-bit code pointer
     */
    if (g_compat32_mode) {
        uint32_t frame_address = (uint32_t)(ULONG_PTR)EstablisherFrame;
        if (frame_address > UINT32_MAX - 16U ||
            !compat32_range_readable(frame_address,
                                     4U * sizeof(uint32_t))) {
            serial_puts("[SEH] invalid EH3 registration frame\n");
            return ExceptionContinueSearch;
        }

        uint32_t *frame32 = (uint32_t *)(ULONG_PTR)frame_address;
        uint32_t scope32  = frame32[2];   /* offset +8 */
        uint32_t level    = frame32[3];   /* offset +12 */
        uint32_t frame_ebp = frame_address + 16U;

        if (flags & EXCEPTION_UNWIND) {
            if (!compat32_eh3_local_unwind(frame_address, scope32, -1))
                serial_puts("[SEH] malformed EH3 unwind metadata\n");
            return ExceptionContinueSearch;
        }

        extern PVOID seh32_ep_addr_for_filter(void);
        uint32_t ep_addr =
            (uint32_t)(ULONG_PTR)seh32_ep_addr_for_filter();
        if (!ep_addr || frame_address < sizeof(uint32_t) ||
            !compat32_range_readable(frame_address - sizeof(uint32_t),
                                     sizeof(uint32_t))) {
            serial_puts("[SEH] invalid EH3 exception-pointers slot\n");
            return ExceptionContinueSearch;
        }

        /* MSVC EH3 funclets read _exception_info from [EBP-0x14]. */
        *(uint32_t *)(ULONG_PTR)(frame_address - sizeof(uint32_t)) = ep_addr;

        serial_puts("[SEH] scope32=0x");
        serial_puthex(scope32, 8);
        serial_puts(" level=");
        serial_putdec(level);
        serial_puts("\n");

        int scope_steps = 0;
        while (level != (uint32_t)-1 && scope32 != 0 &&
               scope_steps++ < 64) {
            /* 32-bit SCOPETABLE_ENTRY: 12 bytes each */
            uint64_t entry_address = (uint64_t)scope32 +
                                     (uint64_t)level * 12ULL;
            if (level > 4096U || entry_address > UINT32_MAX ||
                !compat32_range_readable((uint32_t)entry_address,
                                         3U * sizeof(uint32_t))) {
                serial_puts("[SEH] invalid EH3 scope-table entry\n");
                return ExceptionContinueSearch;
            }

            uint32_t *se = (uint32_t *)(ULONG_PTR)(uint32_t)entry_address;
            uint32_t enclosing = se[0];
            uint32_t filter32  = se[1];
            uint32_t handler32 = se[2];

            if (filter32) {
                if (!compat32_range_executable(filter32, 1)) {
                    serial_puts("[SEH] non-executable EH3 filter\n");
                    return ExceptionContinueSearch;
                }
                serial_puts("[SEH] filter32 @0x");
                serial_puthex(filter32, 8);
                serial_puts("\n");

                /*
                 * Call 32-bit filter: int __cdecl filter(EXCEPTION_POINTERS32 *)
                 * We already have seh32_exception_pointers set up by the caller.
                 */
                uint32_t fargs[1] = { ep_addr };
                uint32_t result = compat32_callback_args_with_ebp(
                    filter32, 1, fargs, frame_ebp);

                serial_puts("[SEH] filter returned ");
                serial_putdec(result);
                serial_puts("\n");

                if ((int32_t)result == 1 /* EXCEPTION_EXECUTE_HANDLER */) {
                    if (!compat32_range_executable(handler32, 1)) {
                        serial_puts("[SEH] non-executable EH3 handler\n");
                        return ExceptionContinueSearch;
                    }
                    serial_puts("[SEH] EXECUTE_HANDLER @0x");
                    serial_puthex(handler32, 8);
                    serial_puts("\n");

                    if (!compat32_eh3_local_unwind(
                            frame_address, scope32, (int32_t)level)) {
                        serial_puts("[SEH] malformed EH3 local unwind\n");
                        return ExceptionContinueSearch;
                    }
                    frame32[3] = enclosing;

                    if (!compat32_eh3_schedule_handler(frame_address,
                                                       handler32)) {
                        serial_puts("[SEH] invalid EH3 transfer state\n");
                        return ExceptionContinueSearch;
                    }
                    return ExceptionContinueExecution;
                }
                else if ((int32_t)result == -1 /* EXCEPTION_CONTINUE_EXECUTION */) {
                    return ExceptionContinueExecution;
                }
                /* EXCEPTION_CONTINUE_SEARCH → try enclosing scope */
            }

            if (enclosing != (uint32_t)-1 && enclosing >= level) {
                serial_puts("[SEH] cyclic EH3 scope-table chain\n");
                return ExceptionContinueSearch;
            }
            level = enclosing;
        }

        if (level != (uint32_t)-1 && scope32 != 0 && scope_steps >= 64)
            serial_puts("[SEH] EH3 scope-table depth limit reached\n");

        return ExceptionContinueSearch;
    }

    /* ── 64-bit path (PE64 or test harness) ── */
    PSCOPETABLE_ENTRY scope_table = EstablisherFrame->ScopeTable;
    DWORD try_level = EstablisherFrame->TryLevel;

    while (try_level != (DWORD)-1) {
        PSCOPETABLE_ENTRY entry = &scope_table[try_level];

        if (entry->FilterFunc) {
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
                serial_puts("[SEH] executing handler\n");
                EstablisherFrame->TryLevel = entry->EnclosingLevel;

                typedef void (WINAPI *handler_fn)(void);
                handler_fn handler = (handler_fn)entry->HandlerFunc;
                handler();

                return ExceptionContinueSearch;
            }
            else if (result == EXCEPTION_CONTINUE_EXECUTION) {
                return ExceptionContinueExecution;
            }
        }

        try_level = entry->EnclosingLevel;
    }

    return ExceptionContinueSearch;
}

/* 32-bit EH4 scope-table support used by _except_handler4_common. */
typedef struct __attribute__((packed)) {
    int32_t previous_try_level;
    uint32_t filter;
    uint32_t handler;
} CRT_EH4_SCOPE_ENTRY32;

typedef struct __attribute__((packed)) {
    int32_t gs_cookie_offset;
    int32_t gs_cookie_xor_offset;
    int32_t eh_cookie_offset;
    int32_t eh_cookie_xor_offset;
} CRT_EH4_SCOPE_HEADER32;

static int crt_eh4_cookie_slot32(uint32_t frame_ebp, int32_t offset,
                                 uint32_t *value)
{
    int64_t address = (int64_t)(uint64_t)frame_ebp + (int64_t)offset;
    if (!value || offset < -0x100000 || offset > 0x100000 ||
        address < 0 || address > UINT32_MAX ||
        !compat32_range_readable((uint32_t)address, sizeof(uint32_t)))
        return 0;

    *value = *(const uint32_t *)(ULONG_PTR)(uint32_t)address;
    return 1;
}

static int crt_eh4_cookie_xor_address32(uint32_t frame_ebp, int32_t offset,
                                        uint32_t *address_out)
{
    int64_t address = (int64_t)(uint64_t)frame_ebp + (int64_t)offset;
    if (!address_out || offset < -0x100000 || offset > 0x100000 ||
        address < 0 || address > UINT32_MAX)
        return 0;

    *address_out = (uint32_t)address;
    return 1;
}

static int crt_eh4_validate_cookie32(uint32_t cookie_address,
                                     uint32_t check_cookie,
                                     uint32_t scope_table,
                                     uint32_t frame_ebp)
{
    if (!compat32_range_executable(check_cookie, 1) ||
        !compat32_range_readable(scope_table,
                                 sizeof(CRT_EH4_SCOPE_HEADER32))) {
        serial_puts("[SEH4] invalid cookie metadata\n");
        return 0;
    }

    const CRT_EH4_SCOPE_HEADER32 *header =
        (const CRT_EH4_SCOPE_HEADER32 *)(ULONG_PTR)scope_table;
    uint32_t expected = *(const uint32_t *)(ULONG_PTR)cookie_address;
    uint32_t cookie_part;
    uint32_t xor_address;

    if (header->gs_cookie_offset != -2) {
        if (!crt_eh4_cookie_slot32(frame_ebp, header->gs_cookie_offset,
                                   &cookie_part) ||
            !crt_eh4_cookie_xor_address32(
                frame_ebp, header->gs_cookie_xor_offset, &xor_address) ||
            (cookie_part ^ xor_address) != expected) {
            serial_puts("[SEH4] GS cookie mismatch\n");
            return 0;
        }
    }

    if (header->eh_cookie_offset == -2 ||
        !crt_eh4_cookie_slot32(frame_ebp, header->eh_cookie_offset,
                               &cookie_part) ||
        !crt_eh4_cookie_xor_address32(
            frame_ebp, header->eh_cookie_xor_offset, &xor_address) ||
        (cookie_part ^ xor_address) != expected) {
        serial_puts("[SEH4] EH cookie mismatch\n");
        return 0;
    }

    return 1;
}

static int crt_eh4_read_entry32(uint32_t scope_table, int32_t level,
                                CRT_EH4_SCOPE_ENTRY32 *entry)
{
    if (!entry || level < 0 || level > 4095)
        return 0;

    uint64_t address = (uint64_t)scope_table + 16ULL +
                       (uint64_t)(uint32_t)level * 12ULL;
    if (address > 0xFFFFFFFFULL ||
        !compat32_range_readable((uint32_t)address, sizeof(*entry)))
        return 0;

    *entry = *(const CRT_EH4_SCOPE_ENTRY32 *)(ULONG_PTR)(uint32_t)address;
    return 1;
}

static int crt_eh4_local_unwind32(uint32_t scope_table,
                                  uint32_t frame_address,
                                  int32_t stop_level)
{
    uint32_t *frame = (uint32_t *)(ULONG_PTR)frame_address;
    int32_t level = (int32_t)frame[3];
    uint32_t frame_ebp = frame_address + 16U;

    for (int guard = 0; level != stop_level && level != -2; guard++) {
        CRT_EH4_SCOPE_ENTRY32 entry;
        if (guard >= 1024 ||
            !crt_eh4_read_entry32(scope_table, level, &entry) ||
            entry.previous_try_level == level)
            return 0;

        frame[3] = (uint32_t)entry.previous_try_level;
        if (!entry.filter && entry.handler) {
            if (!compat32_range_executable(entry.handler, 1))
                return 0;
            (void)compat32_callback_args_with_ebp(entry.handler, 0, NULL,
                                                   frame_ebp);
        }
        level = entry.previous_try_level;
    }
    return level == stop_level;
}

EXCEPTION_DISPOSITION WINAPI crt_except_handler4_common(
    ULONG *cookie,
    PVOID check_cookie,
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)ContextRecord;
    (void)DispatcherContext;

    if (!g_compat32_mode)
        return ExceptionContinueSearch;

    uint32_t cookie_address = (uint32_t)(ULONG_PTR)cookie;
    uint32_t check_cookie_address = (uint32_t)(ULONG_PTR)check_cookie;
    uint32_t frame_address = (uint32_t)(ULONG_PTR)EstablisherFrame;
    uint32_t record_address = (uint32_t)(ULONG_PTR)ExceptionRecord;
    if (!compat32_range_readable(cookie_address, sizeof(uint32_t)) ||
        frame_address > UINT32_MAX - 16U ||
        !compat32_range_readable(frame_address, 6U * sizeof(uint32_t)) ||
        !compat32_range_readable(record_address, 2U * sizeof(uint32_t))) {
        serial_puts("[SEH4] invalid handler arguments\n");
        return ExceptionContinueSearch;
    }

    uint32_t *frame = (uint32_t *)(ULONG_PTR)frame_address;
    uint32_t scope_table = frame[2] ^
                           *(const uint32_t *)(ULONG_PTR)cookie_address;
    if (!compat32_range_readable(scope_table, 16)) {
        serial_puts("[SEH4] invalid decoded scope table 0x");
        serial_puthex(scope_table, 8);
        serial_puts("\n");
        return ExceptionContinueSearch;
    }

    uint32_t frame_ebp = frame_address + 16U;
    if (!crt_eh4_validate_cookie32(cookie_address, check_cookie_address,
                                   scope_table, frame_ebp))
        return ExceptionContinueSearch;

    DWORD flags = *(const uint32_t *)(ULONG_PTR)(record_address + 4U);
    if (flags & EXCEPTION_UNWIND) {
        if (!crt_eh4_local_unwind32(scope_table, frame_address, -2))
            serial_puts("[SEH4] malformed local unwind metadata\n");
        return ExceptionContinueSearch;
    }

    extern PVOID seh32_ep_addr_for_filter(void);
    uint32_t ep_address = (uint32_t)(ULONG_PTR)seh32_ep_addr_for_filter();
    if (frame_address >= sizeof(uint32_t) &&
        compat32_range_readable(frame_address - sizeof(uint32_t),
                                sizeof(uint32_t)))
        *(uint32_t *)(ULONG_PTR)(frame_address - sizeof(uint32_t)) = ep_address;

    int32_t level = (int32_t)frame[3];
    for (int guard = 0; level != -2; guard++) {
        CRT_EH4_SCOPE_ENTRY32 entry;
        if (guard >= 1024 ||
            !crt_eh4_read_entry32(scope_table, level, &entry) ||
            entry.previous_try_level == level) {
            serial_puts("[SEH4] malformed scope chain\n");
            return ExceptionContinueSearch;
        }

        if (entry.filter) {
            if (!compat32_range_executable(entry.filter, 1)) {
                serial_puts("[SEH4] invalid filter address\n");
                return ExceptionContinueSearch;
            }

            uint32_t args[1] = { ep_address };
            int32_t result = (int32_t)compat32_callback_args_with_ebp(
                entry.filter, 1, args, frame_ebp);
            if (result == EXCEPTION_CONTINUE_EXECUTION)
                return ExceptionContinueExecution;

            if (result == EXCEPTION_EXECUTE_HANDLER) {
                if (!entry.handler ||
                    !compat32_range_executable(entry.handler, 1) ||
                    !crt_eh4_local_unwind32(scope_table, frame_address,
                                             level)) {
                    serial_puts("[SEH4] invalid execute-handler metadata\n");
                    return ExceptionContinueSearch;
                }

                frame[3] = (uint32_t)entry.previous_try_level;
                if (!compat32_eh4_schedule_handler(frame_address,
                                                    entry.handler)) {
                    serial_puts("[SEH4] invalid transfer state\n");
                    return ExceptionContinueSearch;
                }
                return ExceptionContinueExecution;
            }
        }

        level = entry.previous_try_level;
    }
    return ExceptionContinueSearch;
}

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

int WINAPI crt_CppXcptFilter(int code, PVOID pointers)
{
    if ((uint32_t)code != 0xE06D7363U)
        return EXCEPTION_CONTINUE_SEARCH;
    return crt_XcptFilter(code, pointers);
}

/* compat32_dispatch intercepts these targets before the generic ABI bridge.
 * Keeping distinct functions makes import resolution explicit while avoiding
 * an incorrect 64-bit C approximation of the i386 register capture. */
__attribute__((noinline))
int WINAPI crt_compat32_setjmp_marker(PVOID environment)
{
    (void)environment;
    return 0;
}

__attribute__((noinline))
int WINAPI crt_compat32_setjmp3_marker(PVOID environment, int unwind_count)
{
    (void)environment;
    (void)unwind_count;
    return 0;
}

__attribute__((noinline))
void WINAPI crt_compat32_longjmp_marker(PVOID environment, int value)
{
    (void)environment;
    (void)value;
}

/* ── Misc CRT internal ────────────────────────────────────── */

int  WINAPI crt_configthreadlocale(int type) { (void)type; return 0; }
void WINAPI crt_lock(int locknum) { (void)locknum; }
void WINAPI crt_unlock(int locknum) { (void)locknum; }
int  WINAPI crt_crt_debugger_hook(int reserved) { (void)reserved; return 0; }
void* WINAPI crt_encoded_null(void) { return NULL; }
PVOID WINAPI crt_amsg_exit(int errnum) { (void)errnum; crt_abort(); return NULL; }

/* ── C++ EH / UT99 required stubs ─────────────────────────── */

typedef struct _CRT_TYPE_INFO_NODE32 {
    uint32_t mem_ptr;
    uint32_t next;
} CRT_TYPE_INFO_NODE32;

typedef struct _CRT_TYPE_INFO_NODE64 {
    PVOID mem_ptr;
    struct _CRT_TYPE_INFO_NODE64 *next;
} CRT_TYPE_INFO_NODE64;

#define CRT_TYPE_INFO_CHAIN_LIMIT 65536U

static volatile uint32_t crt_type_info_lock;

static void crt_type_info_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&crt_type_info_lock, 1U))
        sched_yield();
}

static void crt_type_info_lock_release(void)
{
    __sync_lock_release(&crt_type_info_lock);
}

static BOOL crt_type_info_next32(uint32_t address, uint32_t *next)
{
    if (!address) {
        *next = 0;
        return TRUE;
    }
    if (!compat32_range_readable(address, sizeof(CRT_TYPE_INFO_NODE32)))
        return FALSE;
    *next = ((const CRT_TYPE_INFO_NODE32 *)(ULONG_PTR)address)->next;
    return TRUE;
}

static BOOL crt_type_info_chain_valid32(uint32_t first)
{
    uint32_t slow = first;
    uint32_t fast = first;

    for (uint32_t steps = 0; fast; steps++) {
        uint32_t fast_next;
        uint32_t fast_next_next;
        uint32_t slow_next;

        if (steps >= CRT_TYPE_INFO_CHAIN_LIMIT ||
            !crt_type_info_next32(fast, &fast_next))
            return FALSE;
        if (!fast_next)
            return TRUE;
        if (!crt_type_info_next32(fast_next, &fast_next_next) ||
            !crt_type_info_next32(slow, &slow_next))
            return FALSE;
        slow = slow_next;
        fast = fast_next_next;
        if (fast && fast == slow)
            return FALSE;
    }
    return TRUE;
}

static PVOID crt_type_info_detach_name(PVOID object)
{
    if (!object)
        return NULL;

    if (g_compat32_mode) {
        ULONG_PTR address = (ULONG_PTR)object;
        if (address > UINT32_MAX ||
            !compat32_range_readable((uint32_t)address,
                                     2U * sizeof(uint32_t))) {
            serial_puts("[MSVCRT-RTTI] invalid PE32 type_info object\n");
            return NULL;
        }
        uint32_t *fields = (uint32_t *)object;
        return (PVOID)(ULONG_PTR)__atomic_exchange_n(
            &fields[1], 0U, __ATOMIC_ACQ_REL);
    }

    PVOID *fields = (PVOID *)object;
    return __atomic_exchange_n(&fields[1], NULL, __ATOMIC_ACQ_REL);
}

/* MSVC stores the lazily demangled name immediately after the vtable slot. */
void WINAPI crt_type_info_dtor(PVOID _this)
{
    crt_type_info_lock_acquire();
    PVOID name = crt_type_info_detach_name(_this);
    if (name)
        crt_free(name);
    crt_type_info_lock_release();
}

void WINAPI crt_type_info_dtor_internal(PVOID _this)
{
    crt_type_info_dtor(_this);
}

/*
 * MSVCR80+ keeps one __type_info_node root in each image that asks for a
 * demangled RTTI name. Each heap node owns both its mem_ptr and itself.
 */
void WINAPI crt_clean_type_info_names_internal(PVOID root_node)
{
    if (!root_node)
        return;

    crt_type_info_lock_acquire();
    if (g_compat32_mode) {
        ULONG_PTR root_address = (ULONG_PTR)root_node;
        if (root_address > UINT32_MAX ||
            !compat32_range_readable((uint32_t)root_address,
                                     sizeof(CRT_TYPE_INFO_NODE32))) {
            serial_puts("[MSVCRT-RTTI] invalid PE32 type_info root\n");
            crt_type_info_lock_release();
            return;
        }

        CRT_TYPE_INFO_NODE32 *root = (CRT_TYPE_INFO_NODE32 *)root_node;
        uint32_t current = __atomic_exchange_n(&root->next, 0U,
                                                __ATOMIC_ACQ_REL);
        if (!crt_type_info_chain_valid32(current)) {
            serial_puts("[MSVCRT-RTTI] malformed PE32 type_info chain\n");
            crt_type_info_lock_release();
            return;
        }

        while (current) {
            CRT_TYPE_INFO_NODE32 *node =
                (CRT_TYPE_INFO_NODE32 *)(ULONG_PTR)current;
            uint32_t next = node->next;
            uint32_t mem_ptr = node->mem_ptr;
            if (mem_ptr)
                crt_free((PVOID)(ULONG_PTR)mem_ptr);
            crt_free(node);
            current = next;
        }
    } else {
        CRT_TYPE_INFO_NODE64 *root = (CRT_TYPE_INFO_NODE64 *)root_node;
        CRT_TYPE_INFO_NODE64 *node = __atomic_exchange_n(
            &root->next, NULL, __ATOMIC_ACQ_REL);
        uint32_t count = 0;
        while (node && count++ < CRT_TYPE_INFO_CHAIN_LIMIT) {
            CRT_TYPE_INFO_NODE64 *next = node->next;
            if (node->mem_ptr)
                crt_free(node->mem_ptr);
            crt_free(node);
            node = next;
        }
        if (node)
            serial_puts("[MSVCRT-RTTI] malformed PE64 type_info chain\n");
    }
    crt_type_info_lock_release();
}

typedef struct _CRT_TYPE_INFO_ENTRY {
    struct _CRT_TYPE_INFO_ENTRY *next;
} CRT_TYPE_INFO_ENTRY;

void WINAPI crt_std_type_info_destroy_list(PVOID list_head)
{
    CRT_TYPE_INFO_ENTRY *entry =
        (CRT_TYPE_INFO_ENTRY *)InterlockedFlushSList(list_head);
    while (entry) {
        CRT_TYPE_INFO_ENTRY *next = entry->next;
        crt_free(entry);
        entry = next;
    }
}

/*
 * _CxxThrowException — C++ throw.
 *
 * Builds an EXCEPTION_RECORD with MSVC C++ exception code (0xE06D7363)
 * and dispatches through the current thread's 32-bit SEH chain.
 *
 * If no handler catches it, terminates the process.
 */
extern int compat32_seh_dispatch(PEXCEPTION_RECORD ExceptionRecord);

/*
 * Track the current in-flight C++ exception for re-throw support.
 * When catch(...) calls throw; → _CxxThrowException(NULL, NULL),
 * we re-use the saved exception record instead of creating a fresh one.
 */
static EXCEPTION_RECORD cxx_current_exception;
static int cxx_exception_active = 0;

void WINAPI crt_CxxThrowException(PVOID exception_object, PVOID throw_info)
{
    EXCEPTION_RECORD record;

    if (!exception_object && !throw_info) {
        if (!cxx_exception_active) {
            serial_puts("[MSVCRT] rethrow without an active exception\n");
            crt_abort();
            return;
        }
        record = cxx_current_exception;
    } else {
        BYTE *bytes = (BYTE *)&record;
        for (SIZE_T i = 0; i < sizeof(record); i++)
            bytes[i] = 0;

        extern uint32_t compat32_get_last_caller_eip(void);
        record.ExceptionCode = 0xE06D7363;
        record.ExceptionFlags = 1; /* EXCEPTION_NONCONTINUABLE */
        record.ExceptionAddress =
            (PVOID)(ULONG_PTR)compat32_get_last_caller_eip();
        record.NumberParameters = 3;
        record.ExceptionInformation[0] = 0x19930520;
        record.ExceptionInformation[1] = (ULONG_PTR)exception_object;
        record.ExceptionInformation[2] = (ULONG_PTR)throw_info;

        cxx_current_exception = record;
        cxx_exception_active = 1;
    }

    if (compat32_seh_dispatch(&record))
        return;

    cxx_exception_active = 0;
    serial_puts("[MSVCRT] unhandled C++ exception\n");
    crt_abort();
}


/*
 * __CxxFrameHandler — MSVC 6 C++ exception frame handler.
 *
 * Called from handler stubs in PE32 DLLs:
 *   mov eax, offset FuncInfo
 *   jmp __CxxFrameHandler
 *
 * In compat32 mode, the EstablisherFrame is a 32-bit EH3 registration:
 *   +0:  Next
 *   +4:  Handler (stub address)
 *   +8:  FuncInfo* (32-bit pointer to MSVC FuncInfo structure)
 *   +12: TryLevel (current exception state, -1 = no try block active)
 *
 * FuncInfo layout (MSVC 6, 32-bit):
 *   +0:  magic     (0x19930520 = VC5/6, 0x19930522 = VC7)
 *   +4:  maxState
 *   +8:  pUnwindMap
 *   +12: nTryBlocks
 *   +16: pTryBlockMap
 *
 * TryBlockMapEntry (20 bytes):
 *   +0:  tryLow
 *   +4:  tryHigh
 *   +8:  catchHigh
 *   +12: nCatches
 *   +16: pHandlerArray
 *
 * HandlerType (16 bytes):
 *   +0:  adjectives
 *   +4:  pType (type_info*, 0 = catch(...))
 *   +8:  dispCatchObj
 *   +12: addressOfHandler
 */
uint32_t crt_find_cxx_func_info(uint32_t handler_addr)
{
    if (!compat32_range_executable(handler_addr, 1))
        return 0;

    const uint8_t *stub = (const uint8_t *)(ULONG_PTR)handler_addr;
    for (uint32_t i = 0; i + 10 <= 64; i++) {
        if (!compat32_range_executable(handler_addr + i, 10))
            break;
        if (stub[i] != 0xB8 || stub[i + 5] != 0xE9)
            continue;

        uint32_t candidate = *(const uint32_t *)(stub + i + 1);
        if (!compat32_range_readable(candidate, 5U * sizeof(uint32_t)))
            continue;

        uint32_t magic = *(const uint32_t *)(ULONG_PTR)candidate;
        if (magic == 0x19930520 || magic == 0x19930522)
            return candidate;
    }
    return 0;
}

EXCEPTION_DISPOSITION WINAPI crt_CxxFrameHandler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    (void)ContextRecord;
    (void)DispatcherContext;

    DWORD code  = ExceptionRecord->ExceptionCode;
    DWORD flags = ExceptionRecord->ExceptionFlags;

    if (flags & EXCEPTION_UNWIND)
        return ExceptionContinueSearch;

    if (!g_compat32_mode || code != 0xE06D7363)
        return ExceptionContinueSearch;

    uint32_t *frame32 = (uint32_t *)(ULONG_PTR)EstablisherFrame;

    /*
     * MSVC 6 C++ EH frame layout (different from _except_handler3!):
     *   frame+0: Next
     *   frame+4: Handler (stub address with MOV EAX, FuncInfo; JMP __CxxFrameHandler)
     *   frame+8: TryLevel (at EBP-4 of establishing function)
     *
     * FuncInfo is NOT in the frame — it's encoded in the handler stub's
     * MOV EAX, imm32 instruction (opcode 0xB8).
     */
    uint32_t handler_addr = frame32[1];
    int32_t  cur_state    = (int32_t)frame32[2];  /* TryLevel */

    /* /GS wrappers perform cookie checks before MOV EAX, FuncInfo. */
    uint32_t func_info_addr = crt_find_cxx_func_info(handler_addr);

    serial_puts("[CxxEH] handler=0x");
    serial_puthex(handler_addr, 8);
    serial_puts(" FuncInfo=0x");
    serial_puthex(func_info_addr, 8);
    serial_puts(" state=");
    serial_putdec((uint32_t)cur_state);
    serial_puts("\n");

    if (!func_info_addr || cur_state == -1)
        return ExceptionContinueSearch;

    /* Validate FuncInfo magic */
    uint32_t *fi = (uint32_t *)(ULONG_PTR)func_info_addr;
    uint32_t magic = fi[0];
    if (magic != 0x19930520 && magic != 0x19930522) {
        serial_puts("[CxxEH] bad magic 0x");
        serial_puthex(magic, 8);
        serial_puts("\n");
        return ExceptionContinueSearch;
    }

    uint32_t nTryBlocks   = fi[3];
    uint32_t pTryBlockMap = fi[4];

    serial_puts("[CxxEH] nTryBlocks=");
    serial_putdec(nTryBlocks);
    serial_puts("\n");

    /* Walk try blocks looking for one that covers current state */
    for (uint32_t i = 0; i < nTryBlocks && i < 64; i++) {
        uint32_t *tb = (uint32_t *)(ULONG_PTR)(pTryBlockMap + i * 20);
        int32_t tryLow    = (int32_t)tb[0];
        int32_t tryHigh   = (int32_t)tb[1];
        int32_t catchHigh = (int32_t)tb[2];
        uint32_t nCatches  = tb[3];
        uint32_t pHandlers = tb[4];

        if (cur_state < tryLow || cur_state > tryHigh)
            continue;

        serial_puts("[CxxEH] try[");
        serial_putdec(i);
        serial_puts("] low=");
        serial_putdec((uint32_t)tryLow);
        serial_puts(" high=");
        serial_putdec((uint32_t)tryHigh);
        serial_puts(" nCatches=");
        serial_putdec(nCatches);
        serial_puts("\n");

        /* Walk catch handlers */
        for (uint32_t j = 0; j < nCatches && j < 16; j++) {
            uint32_t *ht = (uint32_t *)(ULONG_PTR)(pHandlers + j * 16);
            uint32_t adjectives  = ht[0];
            uint32_t pType       = ht[1];  /* type_info*, 0 = catch(...) */
            int32_t  dispCatchObj = (int32_t)ht[2];
            uint32_t handlerAddr = ht[3];

            serial_puts("[CxxEH] catch adj=0x");
            serial_puthex(adjectives, 8);
            serial_puts(" type=0x");
            serial_puthex(pType, 8);
            serial_puts(" handler=0x");
            serial_puthex(handlerAddr, 8);
            serial_puts("\n");

            /*
             * Match: catch(...) has pType==0.
             * For typed catches, we'd need to compare type_info names
             * between the thrown type and the catch type. For now, also
             * match if adjectives has 0x40 (HT_IsComplusEh / catch-all).
             */
            if (pType == 0 || (adjectives & 0x40)) {
                serial_puts("[CxxEH] MATCH catch(...) — executing handler @0x");
                serial_puthex(handlerAddr, 8);
                serial_puts("\n");

                /* Update TryLevel to catchHigh+1 (entered catch block).
                 * C++ EH frame: Next(+0), Handler(+4), TryLevel(+8) = frame32[2].
                 * NOT frame32[3] — that's the _except_handler3 layout. */
                frame32[2] = (uint32_t)(catchHigh + 1);

                /*
                 * Copy exception object if needed.
                 * dispCatchObj is the offset from EBP where the catch
                 * object should be stored. For catch(...), it's typically 0.
                 */
                (void)dispCatchObj;

                /*
                 * Call the 32-bit catch handler.
                 * In MSVC, this does a longjmp-style transfer to the
                 * catch block. It may not return.
                 */
                compat32_callback(handlerAddr);

                /* If handler returned, exception was handled */
                serial_puts("[CxxEH] catch handler returned\n");
                return ExceptionContinueExecution;
            }
        }
    }

    return ExceptionContinueSearch;
}

/* __dllonexit — register DLL exit callback in caller's table */
_PVFV_DLL WINAPI crt_dllonexit(_PVFV_DLL func, _PVFV_DLL **pbegin, _PVFV_DLL **pend)
{
    /* Real impl grows *pbegin..*pend table. We just use global atexit for simplicity. */
    if (func) crt_atexit((void (*)(void))func);
    return func;
}

/* __p__commode — pointer to _commode variable */
int* WINAPI crt_p_commode(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->commode : NULL;
}

/* __p__fmode — pointer to _fmode variable */
int* WINAPI crt_p_fmode(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->fmode : NULL;
}

int WINAPI crt_set_fmode(int mode)
{
    switch (mode) {
        case 0x4000:  /* _O_TEXT */
        case 0x8000:  /* _O_BINARY */
        case 0x10000: /* _O_WTEXT */
        case 0x20000: /* _O_U16TEXT */
        case 0x40000: /* _O_U8TEXT */
            break;
        default:
            return 22; /* EINVAL */
    }

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return 12; /* ENOMEM */
    values->fmode = mode;
    return 0;
}

int WINAPI crt_get_fmode(int *mode)
{
    if (!mode)
        return 22; /* EINVAL */

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return 12; /* ENOMEM */
    *mode = values->fmode;
    return 0;
}

/* __C_specific_handler — x86-64 SEH handler (stub, no-op) */
EXCEPTION_DISPOSITION WINAPI crt_C_specific_handler(
    PEXCEPTION_RECORD ExceptionRecord,
    PVOID EstablisherFrame,
    PCONTEXT ContextRecord,
    PVOID DispatcherContext)
{
    return win32_unwind64_c_specific_handler(
        ExceptionRecord, EstablisherFrame, ContextRecord,
        (PDISPATCHER_CONTEXT)DispatcherContext);
}

/* The Microsoft CRT supports this fixed signal set. Handler state belongs to
 * the process; delivery by raise() is synchronous on the calling thread. */
#define CRT_SIG_DFL ((crt_sighandler_t)0)
#define CRT_SIG_IGN ((crt_sighandler_t)1)
#define CRT_SIG_ERR ((crt_sighandler_t)(LONG_PTR)-1)

static int crt_signal_index(int sig)
{
    switch (sig) {
    case 2:  return 0; /* SIGINT */
    case 4:  return 1; /* SIGILL */
    case 8:  return 2; /* SIGFPE */
    case 11: return 3; /* SIGSEGV */
    case 15: return 4; /* SIGTERM */
    case 21: return 5; /* SIGBREAK */
    case 22: return 6; /* SIGABRT */
    default: return -1;
    }
}

crt_sighandler_t WINAPI crt_signal(int sig, crt_sighandler_t handler)
{
    int index = crt_signal_index(sig);
    if (index < 0 || handler == CRT_SIG_ERR) {
        *crt_errno() = CRT_EINVAL;
        return CRT_SIG_ERR;
    }

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) {
        *crt_errno() = CRT_ENOMEM;
        return CRT_SIG_ERR;
    }

    ucrt_state_lock_acquire();
    crt_sighandler_t previous =
        (crt_sighandler_t)(ULONG_PTR)values->signal_handlers[index];
    values->signal_handlers[index] = (ULONG_PTR)handler;
    ucrt_state_lock_release();
    *crt_errno() = 0;
    return previous;
}

int WINAPI crt_raise(int sig)
{
    int index = crt_signal_index(sig);
    if (index < 0) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) {
        *crt_errno() = CRT_ENOMEM;
        return -1;
    }

    ucrt_state_lock_acquire();
    crt_sighandler_t handler =
        (crt_sighandler_t)(ULONG_PTR)values->signal_handlers[index];
    if (handler != CRT_SIG_DFL && handler != CRT_SIG_IGN)
        values->signal_handlers[index] = (ULONG_PTR)CRT_SIG_DFL;
    ucrt_state_lock_release();

    if (handler == CRT_SIG_IGN) {
        *crt_errno() = 0;
        return 0;
    }
    if (handler == CRT_SIG_DFL) {
        serial_puts("[MSVCRT] unhandled signal ");
        serial_putdec((uint64_t)(uint32_t)sig);
        serial_puts("\n");
        ExitProcess(3);
        return 0;
    }

    if (g_compat32_mode) {
        uint32_t args[1] = { (uint32_t)sig };
        (void)compat32_callback_args((uint32_t)(ULONG_PTR)handler, 1, args);
    } else {
        handler(sig);
    }
    *crt_errno() = 0;
    return 0;
}

/* __setusermatherr — set math error handler (store, ignore) */
static _UserMathErrFunc crt_usermatherr_handler = NULL;
void WINAPI crt_setusermatherr(_UserMathErrFunc handler)
{
    crt_usermatherr_handler = handler;
}

/* _acmdln accessor used by native shim code. The exported symbol itself is
 * process-local writable storage returned by msvcrt_resolve(). */
char* WINAPI crt_acmdln(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? values->command_line : NULL;
}

/* _adjust_fdiv — FDIV adjustment flag (always 0, no bug) */
int* WINAPI crt_adjust_fdiv(void)
{
    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    return values ? &values->adjust_fdiv : NULL;
}

/* CRT flags are not hardware bit positions. The current thread's saved FPU
 * context, not a CRT-global shadow, owns the x87 and SSE control state. */
#define CRT_MCW_EM 0x0008001FU
#define CRT_EM_DENORMAL 0x00080000U
#define CRT_MCW_RC 0x00000300U
#define CRT_MCW_PC 0x00030000U
#define CRT_PC_24 0x00020000U
#define CRT_PC_53 0x00010000U
#define CRT_MCW_IC 0x00040000U
#define CRT_MCW_DN 0x03000000U
#define CRT_EM_AMBIGUOUS 0x80000000U
#define CRT_MCW_ALL (CRT_MCW_EM | CRT_MCW_RC | CRT_MCW_PC | CRT_MCW_IC | CRT_MCW_DN)

static unsigned crt_fp_exceptions_from_hw(unsigned bits)
{
    return ((bits & 1) ? 0x10U : 0) |
           ((bits & 2) ? CRT_EM_DENORMAL : 0) |
           ((bits & 4) ? 8U : 0) | ((bits & 8) ? 4U : 0) |
           ((bits & 16) ? 2U : 0) | ((bits & 32) ? 1U : 0);
}

static unsigned crt_fp_exceptions_to_hw(unsigned bits)
{
    return ((bits & 0x10) ? 1U : 0) |
           ((bits & CRT_EM_DENORMAL) ? 2U : 0) |
           ((bits & 8) ? 4U : 0) | ((bits & 4) ? 8U : 0) |
           ((bits & 2) ? 16U : 0) | ((bits & 1) ? 32U : 0);
}

static unsigned crt_fp_x87_control(unsigned short word)
{
    unsigned value = crt_fp_exceptions_from_hw(word & 0x3F) |
                     ((word >> 2) & CRT_MCW_RC);
    if ((word & 0x300) == 0) value |= CRT_PC_24;
    else if ((word & 0x300) == 0x200) value |= CRT_PC_53;
    if (word & 0x1000) value |= CRT_MCW_IC;
    return value;
}

static unsigned crt_fp_sse_control(unsigned csr)
{
    unsigned value = crt_fp_exceptions_from_hw((csr >> 7) & 0x3F) |
                     ((csr >> 5) & CRT_MCW_RC);
    switch (csr & 0x8040) {
    case 0x8040: value |= 0x01000000U; break; /* flush operands and results */
    case 0x0040: value |= 0x02000000U; break; /* flush operands */
    case 0x8000: value |= 0x03000000U; break; /* flush results */
    }
    return value;
}

unsigned int WINAPI crt_control87(unsigned int newval, unsigned int mask)
{
    unsigned short cw;
    unsigned csr;
    __asm__ volatile ("fnstcw %0; stmxcsr %1" : "=m"(cw), "=m"(csr));
    unsigned x87 = crt_fp_x87_control(cw);
    unsigned sse = crt_fp_sse_control(csr);
    mask &= CRT_MCW_ALL;

    if (g_compat32_mode) {
        unsigned x87_mask = mask & ~CRT_MCW_DN;
        unsigned value = (x87 & ~x87_mask) | (newval & x87_mask);
        unsigned short next = (cw & ~0x1F3FU) |
            crt_fp_exceptions_to_hw(value) | ((value & CRT_MCW_RC) << 2);
        switch (value & CRT_MCW_PC) {
        case CRT_PC_24: break;
        case CRT_PC_53: next |= 0x200; break;
        default: next |= 0x300; break;
        }
        if (value & CRT_MCW_IC) next |= 0x1000;
        if (next != cw) __asm__ volatile ("fldcw %0" : : "m"(next) : "memory");
        x87 = crt_fp_x87_control(next);
    }

    unsigned sse_mask = mask & ~(CRT_MCW_PC | CRT_MCW_IC);
    unsigned value = (sse & ~sse_mask) | (newval & sse_mask);
    unsigned next = (csr & ~0xFFC0U) |
        (crt_fp_exceptions_to_hw(value) << 7) | ((value & CRT_MCW_RC) << 5);
    switch (value & CRT_MCW_DN) {
    case 0x01000000U: next |= 0x8040; break;
    case 0x02000000U: next |= 0x0040; break;
    case 0x03000000U: next |= 0x8000; break;
    }
    if ((next ^ csr) & 0x0040) {
        /* DAZ is optional even with SSE2; never load a reserved MXCSR bit. */
        __attribute__((aligned(16))) unsigned char state[512];
        __asm__ volatile ("fxsave64 %0" : "=m"(state));
        unsigned supported = *(const unsigned *)(state + 28);
        if (!supported) supported = 0xFFBF;
        next &= supported;
    }
    if (next != csr) {
        next &= ~0x3FU;
        __asm__ volatile ("ldmxcsr %0" : : "m"(next) : "memory");
    }
    sse = crt_fp_sse_control(next);
    if (!g_compat32_mode) return sse;
    unsigned result = x87 | sse;
    if ((x87 ^ sse) & (CRT_MCW_EM | CRT_MCW_RC)) result |= CRT_EM_AMBIGUOUS;
    return result;
}

unsigned int WINAPI crt_controlfp(unsigned int newval, unsigned int mask)
{
    return crt_control87(newval, mask & ~CRT_EM_DENORMAL);
}

int WINAPI crt_controlfp_s(unsigned int *current, unsigned int newval,
                            unsigned int mask)
{
    if (newval & mask & ~CRT_MCW_ALL) {
        if (current) *current = crt_controlfp(0, 0);
        crt_report_invalid_parameter();
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    unsigned value = crt_controlfp(newval, mask);
    if (current) *current = value;
    return 0;
}

/* _ftol — float to long conversion */
long WINAPI crt_ftol(double val)
{
    return (long)val;
}

/* _onexit — register exit callback */
_onexit_t WINAPI crt_onexit(_onexit_t func)
{
    if (func) crt_atexit((void (*)(void))func);
    return func;
}

/* _purecall — pure virtual call handler */
void WINAPI crt_purecall(void)
{
    serial_puts("[MSVCRT] _purecall — pure virtual function call — aborting\n");
    crt_abort();
}

/* ── vprintf family (ms_abi va_list wrappers) ─────────────── */

/*
 * v*printf family — the va_list parameter is a 32-bit pointer to the
 * 32-bit caller's stack (4-byte arg slots). Cast to uint32_t* and use
 * do_vformat32 which walks 4-byte slots correctly.
 */
static int WINAPI crt_vprintf(const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

static int WINAPI crt_vsprintf(char *buf, const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { buf, (SIZE_T)-1, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

static int WINAPI crt_vsnprintf(char *buf, SIZE_T size, const char *fmt, ms_va_list ap)
{
    FMT_CTX ctx = { buf, size, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

static BOOL crt_secure_truncate_count(SIZE_T count)
{
    /* PE32 passes _TRUNCATE as a zero-extended 0xffffffff. */
    return count == (SIZE_T)-1 || count == (SIZE_T)0xFFFFFFFFU;
}

static SIZE_T crt_snprintf_s_capacity(SIZE_T size, SIZE_T count)
{
    if (crt_secure_truncate_count(count) || count >= size)
        return size;
    return count + 1;
}

static int crt_snprintf_s_result(char *buf, SIZE_T size, SIZE_T count,
                                 int required)
{
    SIZE_T max_chars = crt_secure_truncate_count(count)
        ? size - 1
        : (count < size ? count : size - 1);

    if (required >= 0 && (SIZE_T)required <= max_chars)
        return required;

    /* Explicit count and _TRUNCATE both leave a terminated prefix. When the
     * caller claimed the whole destination was available, MSVCRT treats an
     * overrun as a range error and clears the destination. */
    if (crt_secure_truncate_count(count) || count < size)
        return -1;

    buf[0] = 0;
    *crt_errno() = CRT_ERANGE;
    return -1;
}

static int WINAPI crt_snprintf_s_compat32(char *buf, SIZE_T size,
                                           SIZE_T count, const char *fmt,
                                           uint32_t *args)
{
    if (!buf || !size || !fmt) {
        if (buf && size) buf[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    FMT_CTX ctx = { buf, crt_snprintf_s_capacity(size, count), 0 };
    int required = do_vformat32(&ctx, fmt, args);
    return crt_snprintf_s_result(buf, size, count, required);
}

int WINAPI crt_snprintf_s(char *buf, SIZE_T size, SIZE_T count,
                          const char *fmt, ...)
{
    if (!buf || !size || !fmt) {
        if (buf && size) buf[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    ms_va_list ap;
    ms_va_start(ap, fmt);
    FMT_CTX ctx = { buf, crt_snprintf_s_capacity(size, count), 0 };
    int required = do_vformat(&ctx, fmt, ap);
    ms_va_end(ap);
    return crt_snprintf_s_result(buf, size, count, required);
}

static int WINAPI crt_vfprintf(PVOID stream, const char *fmt, ms_va_list ap)
{
    (void)stream;
    FMT_CTX ctx = { NULL, 0, 0 };
    return do_vformat32(&ctx, fmt, (uint32_t *)(void *)ap);
}

#define CRT_PRINTF_STANDARD_SNPRINTF (1ULL << 1)

int WINAPI crt_stdio_common_vsprintf(uint64_t options, char *buffer,
                                     SIZE_T buffer_count,
                                     const char *format, PVOID locale,
                                     PVOID arg_list)
{
    (void)locale;
    if (!format || (!buffer && buffer_count != 0) || !arg_list) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = 22; /* EINVAL */
        return -1;
    }

    FMT_CTX ctx = { buffer, buffer_count, 0 };
    int required = do_vformat(&ctx, format, (ms_va_list)arg_list);
    int truncated = buffer_count == 0 || (SIZE_T)required >= buffer_count;

    static int trace_count;
    if (trace_count < 8) {
        trace_count++;
        serial_puts("[UCRT-VS] options=0x");
        serial_puthex(options, 8);
        serial_puts(" count=");
        serial_putdec(buffer_count);
        serial_puts(" required=");
        serial_putdec((uint64_t)required);
        serial_puts(" fmt=\"");
        for (int i = 0; i < 64 && format[i]; i++)
            serial_putchar((unsigned char)format[i] < 0x80 ? format[i] : '?');
        serial_puts("\"\n");
    }

    if (truncated && !(options & CRT_PRINTF_STANDARD_SNPRINTF))
        return -1;
    return required;
}

/* Universal CRT wide formatting for native PE32+ callers. A Microsoft x64
 * va_list walks 8-byte argument homes; it must not share the PE32 formatter
 * below, whose va_list is a packed array of 4-byte stack slots. */
typedef struct {
    WCHAR *buf;
    SIZE_T size;
    SIZE_T pos;
} WFMT_CTX;

static void wfmt_putc(WFMT_CTX *ctx, WCHAR c)
{
    if (ctx->buf && ctx->size > 0 && ctx->pos < ctx->size - 1)
        ctx->buf[ctx->pos] = c;
    ctx->pos++;
}

static void wfmt_put_ascii(WFMT_CTX *ctx, const char *s, SIZE_T len)
{
    for (SIZE_T i = 0; i < len; i++)
        wfmt_putc(ctx, (WCHAR)(unsigned char)s[i]);
}

static void wfmt_put_wide(WFMT_CTX *ctx, const WCHAR *s, SIZE_T len)
{
    for (SIZE_T i = 0; i < len; i++)
        wfmt_putc(ctx, s[i]);
}

static void wfmt_pad(WFMT_CTX *ctx, int count, WCHAR c)
{
    while (count-- > 0)
        wfmt_putc(ctx, c);
}

static SIZE_T wfmt_wcsnlen(const WCHAR *s, int precision)
{
    SIZE_T n = 0;
    if (!s) return 0;
    while (s[n] && (precision < 0 || n < (SIZE_T)precision)) n++;
    return n;
}

enum {
    WLEN_DEFAULT,
    WLEN_HH,
    WLEN_H,
    WLEN_L,
    WLEN_LL,
    WLEN_J,
    WLEN_Z,
    WLEN_T,
    WLEN_I32,
    WLEN_I64,
    WLEN_CAPITAL_L
};

static void wfmt_integer(WFMT_CTX *ctx, unsigned long long value, int negative,
                         int base, int upper, int width, int precision,
                         int left, int zero, int plus, int space, int alternate)
{
    char digits[24];
    SIZE_T digits_len = uint_to_str(digits, value, base, upper);
    if (precision == 0 && value == 0) digits_len = 0;

    char prefix[2];
    int prefix_len = 0;
    if (negative) prefix[prefix_len++] = '-';
    else if (plus) prefix[prefix_len++] = '+';
    else if (space) prefix[prefix_len++] = ' ';

    char radix_prefix[2];
    int radix_len = 0;
    if (alternate && base == 16 && value != 0) {
        radix_prefix[radix_len++] = '0';
        radix_prefix[radix_len++] = upper ? 'X' : 'x';
    } else if (alternate && base == 8 &&
               (digits_len == 0 || digits[0] != '0')) {
        radix_prefix[radix_len++] = '0';
    }

    int precision_zeroes = precision > (int)digits_len
                         ? precision - (int)digits_len : 0;
    int content = prefix_len + radix_len + precision_zeroes + (int)digits_len;
    int width_pad = width > content ? width - content : 0;

    if (!left && (!zero || precision >= 0)) wfmt_pad(ctx, width_pad, ' ');
    wfmt_put_ascii(ctx, prefix, (SIZE_T)prefix_len);
    wfmt_put_ascii(ctx, radix_prefix, (SIZE_T)radix_len);
    if (!left && zero && precision < 0) wfmt_pad(ctx, width_pad, '0');
    wfmt_pad(ctx, precision_zeroes, '0');
    wfmt_put_ascii(ctx, digits, digits_len);
    if (left) wfmt_pad(ctx, width_pad, ' ');
}

static void wfmt_float(WFMT_CTX *ctx, double value, int width, int precision,
                       int left, int zero, int plus, int space)
{
    char out[96];
    int pos = 0;
    int negative = value < 0.0;
    if (negative) value = -value;
    if (precision < 0) precision = 6;
    if (precision > 48) precision = 48;

    unsigned long long integer = (unsigned long long)value;
    double fraction = value - (double)integer;
    pos = (int)uint_to_str(out, integer, 10, 0);
    if (precision > 0) {
        out[pos++] = '.';
        for (int i = 0; i < precision; i++) {
            fraction *= 10.0;
            int digit = (int)fraction;
            if (digit < 0) digit = 0;
            if (digit > 9) digit = 9;
            out[pos++] = (char)('0' + digit);
            fraction -= digit;
        }
        if (fraction >= 0.5) {
            int i = pos - 1;
            while (i >= 0) {
                if (out[i] == '.') { i--; continue; }
                if (out[i] != '9') { out[i]++; break; }
                out[i--] = '0';
            }
            if (i < 0 && pos < (int)sizeof(out) - 1) {
                for (int j = pos; j > 0; j--) out[j] = out[j - 1];
                out[0] = '1';
                pos++;
            }
        }
    } else if (fraction >= 0.5) {
        integer++;
        pos = (int)uint_to_str(out, integer, 10, 0);
    }

    char sign = negative ? '-' : plus ? '+' : space ? ' ' : 0;
    int content = pos + (sign != 0);
    int pad = width > content ? width - content : 0;
    if (!left && !zero) wfmt_pad(ctx, pad, ' ');
    if (sign) wfmt_putc(ctx, (WCHAR)sign);
    if (!left && zero) wfmt_pad(ctx, pad, '0');
    wfmt_put_ascii(ctx, out, (SIZE_T)pos);
    if (left) wfmt_pad(ctx, pad, ' ');
}

static int do_vformat_wide64(WFMT_CTX *ctx, const WCHAR *fmt, ms_va_list ap)
{
    static const WCHAR null_wide[] = {'(','n','u','l','l',')',0};
    static const char null_narrow[] = "(null)";

    while (*fmt) {
        if (*fmt != '%') {
            wfmt_putc(ctx, *fmt++);
            continue;
        }
        fmt++;

        int left = 0, zero = 0, plus = 0, space = 0, alternate = 0;
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else if (*fmt == '#') { alternate = 1; fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = ms_va_arg(ap, int);
            fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = ms_va_arg(ap, int);
                fmt++;
                if (precision < 0) precision = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        int length = WLEN_DEFAULT;
        if (*fmt == 'h') {
            fmt++;
            length = WLEN_H;
            if (*fmt == 'h') { fmt++; length = WLEN_HH; }
        } else if (*fmt == 'l') {
            fmt++;
            length = WLEN_L;
            if (*fmt == 'l') { fmt++; length = WLEN_LL; }
        } else if (*fmt == 'j') { fmt++; length = WLEN_J; }
        else if (*fmt == 'z') { fmt++; length = WLEN_Z; }
        else if (*fmt == 't') { fmt++; length = WLEN_T; }
        else if (*fmt == 'I') {
            if (fmt[1] == '6' && fmt[2] == '4') { fmt += 3; length = WLEN_I64; }
            else if (fmt[1] == '3' && fmt[2] == '2') { fmt += 3; length = WLEN_I32; }
            else { fmt++; length = WLEN_Z; }
        }

        WCHAR conversion = *fmt;
        if (!conversion) break;
        fmt++;

        switch (conversion) {
        case 'd': case 'i': {
            long long signed_value;
            if (length == WLEN_LL || length == WLEN_I64 ||
                length == WLEN_J || length == WLEN_Z || length == WLEN_T)
                signed_value = ms_va_arg(ap, long long);
            else
                signed_value = (long long)ms_va_arg(ap, int);
            int negative = signed_value < 0;
            unsigned long long magnitude = negative
                ? 0ULL - (unsigned long long)signed_value
                : (unsigned long long)signed_value;
            wfmt_integer(ctx, magnitude, negative, 10, 0, width, precision,
                         left, zero, plus, space, 0);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long value;
            if (length == WLEN_LL || length == WLEN_I64 ||
                length == WLEN_J || length == WLEN_Z || length == WLEN_T)
                value = ms_va_arg(ap, unsigned long long);
            else
                value = (unsigned long long)ms_va_arg(ap, unsigned int);
            int base = conversion == 'o' ? 8 :
                       (conversion == 'x' || conversion == 'X') ? 16 : 10;
            wfmt_integer(ctx, value, 0, base, conversion == 'X', width,
                         precision, left, zero, 0, 0, alternate);
            break;
        }
        case 'p': {
            unsigned long long value =
                (unsigned long long)(ULONG_PTR)ms_va_arg(ap, PVOID);
            wfmt_integer(ctx, value, 0, 16, 0, width, precision,
                         left, zero, 0, 0, 1);
            break;
        }
        case 's': {
            if (length == WLEN_H || length == WLEN_HH) {
                const char *s = ms_va_arg(ap, const char *);
                if (!s) s = null_narrow;
                SIZE_T len = 0;
                while (s[len] && (precision < 0 || len < (SIZE_T)precision)) len++;
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_ascii(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            } else {
                const WCHAR *s = ms_va_arg(ap, const WCHAR *);
                if (!s) s = null_wide;
                SIZE_T len = wfmt_wcsnlen(s, precision);
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_wide(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            }
            break;
        }
        case 'S': {
            const char *s = ms_va_arg(ap, const char *);
            if (!s) s = null_narrow;
            SIZE_T len = 0;
            while (s[len] && (precision < 0 || len < (SIZE_T)precision)) len++;
            if (!left) wfmt_pad(ctx, width - (int)len, ' ');
            wfmt_put_ascii(ctx, s, len);
            if (left) wfmt_pad(ctx, width - (int)len, ' ');
            break;
        }
        case 'c': {
            WCHAR c = (WCHAR)ms_va_arg(ap, int);
            if (!left) wfmt_pad(ctx, width - 1, ' ');
            wfmt_putc(ctx, c);
            if (left) wfmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'C': {
            WCHAR c = (WCHAR)(unsigned char)ms_va_arg(ap, int);
            if (!left) wfmt_pad(ctx, width - 1, ' ');
            wfmt_putc(ctx, c);
            if (left) wfmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            wfmt_float(ctx, ms_va_arg(ap, double), width, precision,
                       left, zero, plus, space);
            break;
        case 'n': {
            PVOID out = ms_va_arg(ap, PVOID);
            if (!out) break;
            if (length == WLEN_HH) *(signed char *)out = (signed char)ctx->pos;
            else if (length == WLEN_H) *(short *)out = (short)ctx->pos;
            else if (length == WLEN_LL || length == WLEN_I64 ||
                     length == WLEN_J || length == WLEN_Z || length == WLEN_T)
                *(long long *)out = (long long)ctx->pos;
            else *(int *)out = (int)ctx->pos;
            break;
        }
        case '%':
            wfmt_putc(ctx, '%');
            break;
        default:
            wfmt_putc(ctx, '%');
            wfmt_putc(ctx, conversion);
            break;
        }
    }

    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return (int)ctx->pos;
}

static uint32_t wfmt_arg32_u32(uint32_t **args)
{
    uint32_t value = **args;
    (*args)++;
    return value;
}

static uint64_t wfmt_arg32_u64(uint32_t **args)
{
    uint64_t value = (uint64_t)(*args)[0] |
                     ((uint64_t)(*args)[1] << 32);
    *args += 2;
    return value;
}

static double wfmt_arg32_double(uint32_t **args)
{
    uint64_t bits = wfmt_arg32_u64(args);
    double value;
    crt_memcpy(&value, &bits, sizeof(value));
    return value;
}

/* PE32 cdecl va_list values are packed in 4-byte stack slots. Keep this
 * parser separate from do_vformat_wide64: using ms_va_arg here would advance
 * by Microsoft x64 argument homes and merge adjacent i386 arguments. */
static SIZE_T do_vformat_wide32(WFMT_CTX *ctx, const WCHAR *fmt,
                                uint32_t *args)
{
    static const WCHAR null_wide[] = {'(','n','u','l','l',')',0};
    static const char null_narrow[] = "(null)";

    while (*fmt) {
        if (*fmt != '%') {
            wfmt_putc(ctx, *fmt++);
            continue;
        }
        fmt++;

        int left = 0, zero = 0, plus = 0, space = 0, alternate = 0;
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else if (*fmt == '#') { alternate = 1; fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = (int32_t)wfmt_arg32_u32(&args);
            fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = (int32_t)wfmt_arg32_u32(&args);
                fmt++;
                if (precision < 0) precision = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        int length = WLEN_DEFAULT;
        if (*fmt == 'h') {
            fmt++;
            length = WLEN_H;
            if (*fmt == 'h') { fmt++; length = WLEN_HH; }
        } else if (*fmt == 'l') {
            fmt++;
            length = WLEN_L;
            if (*fmt == 'l') { fmt++; length = WLEN_LL; }
        } else if (*fmt == 'j') { fmt++; length = WLEN_J; }
        else if (*fmt == 'z') { fmt++; length = WLEN_Z; }
        else if (*fmt == 't') { fmt++; length = WLEN_T; }
        else if (*fmt == 'L') { fmt++; length = WLEN_CAPITAL_L; }
        else if (*fmt == 'I') {
            if (fmt[1] == '6' && fmt[2] == '4') { fmt += 3; length = WLEN_I64; }
            else if (fmt[1] == '3' && fmt[2] == '2') { fmt += 3; length = WLEN_I32; }
            else { fmt++; length = WLEN_Z; }
        }

        WCHAR conversion = *fmt;
        if (!conversion) break;
        fmt++;

        switch (conversion) {
        case 'd': case 'i': {
            long long signed_value;
            if (length == WLEN_LL || length == WLEN_I64 ||
                length == WLEN_J) {
                signed_value = (long long)(int64_t)wfmt_arg32_u64(&args);
            } else {
                int32_t value = (int32_t)wfmt_arg32_u32(&args);
                if (length == WLEN_H) value = (short)value;
                else if (length == WLEN_HH) value = (signed char)value;
                signed_value = (long long)value;
            }
            int negative = signed_value < 0;
            unsigned long long magnitude = negative
                ? 0ULL - (unsigned long long)signed_value
                : (unsigned long long)signed_value;
            wfmt_integer(ctx, magnitude, negative, 10, 0, width, precision,
                         left, zero, plus, space, 0);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long value;
            if (length == WLEN_LL || length == WLEN_I64 ||
                length == WLEN_J) {
                value = (unsigned long long)wfmt_arg32_u64(&args);
            } else {
                uint32_t value32 = wfmt_arg32_u32(&args);
                if (length == WLEN_H) value32 = (unsigned short)value32;
                else if (length == WLEN_HH)
                    value32 = (unsigned char)value32;
                value = (unsigned long long)value32;
            }
            int base = conversion == 'o' ? 8 :
                       (conversion == 'x' || conversion == 'X') ? 16 : 10;
            wfmt_integer(ctx, value, 0, base, conversion == 'X', width,
                         precision, left, zero, 0, 0, alternate);
            break;
        }
        case 'p': {
            unsigned long long value =
                (unsigned long long)wfmt_arg32_u32(&args);
            wfmt_integer(ctx, value, 0, 16, 0, width, precision,
                         left, zero, 0, 0, 1);
            break;
        }
        case 's': {
            uint32_t raw_pointer = wfmt_arg32_u32(&args);
            if (length == WLEN_H || length == WLEN_HH) {
                const char *s = (const char *)(uintptr_t)raw_pointer;
                if (!s) s = null_narrow;
                SIZE_T len = 0;
                while (s[len] &&
                       (precision < 0 || len < (SIZE_T)precision)) len++;
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_ascii(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            } else {
                const WCHAR *s = (const WCHAR *)(uintptr_t)raw_pointer;
                if (!s) s = null_wide;
                SIZE_T len = wfmt_wcsnlen(s, precision);
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_wide(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            }
            break;
        }
        case 'S': {
            uint32_t raw_pointer = wfmt_arg32_u32(&args);
            if (length == WLEN_L || length == WLEN_LL) {
                const WCHAR *s = (const WCHAR *)(uintptr_t)raw_pointer;
                if (!s) s = null_wide;
                SIZE_T len = wfmt_wcsnlen(s, precision);
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_wide(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            } else {
                const char *s = (const char *)(uintptr_t)raw_pointer;
                if (!s) s = null_narrow;
                SIZE_T len = 0;
                while (s[len] &&
                       (precision < 0 || len < (SIZE_T)precision)) len++;
                if (!left) wfmt_pad(ctx, width - (int)len, ' ');
                wfmt_put_ascii(ctx, s, len);
                if (left) wfmt_pad(ctx, width - (int)len, ' ');
            }
            break;
        }
        case 'c': {
            uint32_t value = wfmt_arg32_u32(&args);
            WCHAR c = length == WLEN_H || length == WLEN_HH
                    ? (WCHAR)(unsigned char)value : (WCHAR)value;
            if (!left) wfmt_pad(ctx, width - 1, ' ');
            wfmt_putc(ctx, c);
            if (left) wfmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'C': {
            uint32_t value = wfmt_arg32_u32(&args);
            WCHAR c = length == WLEN_L || length == WLEN_LL
                    ? (WCHAR)value : (WCHAR)(unsigned char)value;
            if (!left) wfmt_pad(ctx, width - 1, ' ');
            wfmt_putc(ctx, c);
            if (left) wfmt_pad(ctx, width - 1, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            wfmt_float(ctx, wfmt_arg32_double(&args), width, precision,
                       left, zero, plus, space);
            break;
        case 'n': {
            PVOID out = (PVOID)(uintptr_t)wfmt_arg32_u32(&args);
            if (!out) break;
            if (length == WLEN_HH) *(signed char *)out = (signed char)ctx->pos;
            else if (length == WLEN_H) *(short *)out = (short)ctx->pos;
            else if (length == WLEN_LL || length == WLEN_I64 ||
                     length == WLEN_J)
                *(long long *)out = (long long)ctx->pos;
            else
                *(int *)out = (int)ctx->pos;
            break;
        }
        case '%':
            wfmt_putc(ctx, '%');
            break;
        default:
            wfmt_putc(ctx, '%');
            wfmt_putc(ctx, conversion);
            break;
        }
    }

    if (ctx->buf && ctx->size > 0) {
        SIZE_T end = ctx->pos < ctx->size - 1 ? ctx->pos : ctx->size - 1;
        ctx->buf[end] = 0;
    }
    return ctx->pos;
}

int WINAPI crt_vswprintf_c_l(WCHAR *buffer, SIZE_T buffer_count,
                              const WCHAR *format, PVOID locale,
                              PVOID arg_list)
{
    (void)locale; /* The locale shim currently exposes the invariant C locale. */

    if (!format || !arg_list || (!buffer && buffer_count != 0)) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (buffer && buffer_count == 0)
        return -1;

    WFMT_CTX ctx = { buffer, buffer_count, 0 };
    SIZE_T required;
    if (g_compat32_mode) {
        required = do_vformat_wide32(
            &ctx, format, (uint32_t *)(uintptr_t)arg_list);
    } else {
        int native_required = do_vformat_wide64(
            &ctx, format, (ms_va_list)arg_list);
        if (native_required < 0)
            return -1;
        required = (SIZE_T)native_required;
    }

    if (required > 0x7fffffffU) {
        *crt_errno() = CRT_EOVERFLOW;
        return -1;
    }
    if (buffer_count != 0 && required >= buffer_count)
        return -1;
    return (int)required;
}

static int crt_wformat_bounded_result(SIZE_T required, SIZE_T buffer_count)
{
    if (required > 0x7fffffffU) {
        *crt_errno() = CRT_EOVERFLOW;
        return -1;
    }
    return buffer_count != 0 && required >= buffer_count
        ? -1 : (int)required;
}

static int WINAPI crt_snwprintf_compat32(WCHAR *buffer, SIZE_T buffer_count,
                                          const WCHAR *format,
                                          uint32_t *args)
{
    if (!format || (!buffer && buffer_count != 0) ||
        (buffer && buffer_count == 0)) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    WFMT_CTX ctx = { buffer, buffer_count, 0 };
    SIZE_T required = do_vformat_wide32(&ctx, format, args);
    return crt_wformat_bounded_result(required, buffer_count);
}

int WINAPI crt_snwprintf(WCHAR *buffer, SIZE_T buffer_count,
                         const WCHAR *format, ...)
{
    if (!format || (!buffer && buffer_count != 0) ||
        (buffer && buffer_count == 0)) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    ms_va_list ap;
    ms_va_start(ap, format);
    WFMT_CTX ctx = { buffer, buffer_count, 0 };
    int required = do_vformat_wide64(&ctx, format, ap);
    ms_va_end(ap);
    if (required < 0)
        return -1;
    return crt_wformat_bounded_result((SIZE_T)required, buffer_count);
}

static int crt_fwprintf_write(PVOID stream, WCHAR *wide, SIZE_T count)
{
    if (!stream || !crt_file_resolve((CRT_FILE *)stream, NULL)) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (!count)
        return 0;

    char *narrow = (char *)crt_malloc(count + 1);
    if (!narrow) {
        *crt_errno() = CRT_ENOMEM;
        return -1;
    }
    SIZE_T converted = crt_wcstombs(narrow, wide, count + 1);
    if (converted == (SIZE_T)-1) {
        crt_free(narrow);
        return -1;
    }
    SIZE_T written = crt_fwrite(narrow, 1, converted, (CRT_FILE *)stream);
    crt_free(narrow);
    return written == converted ? (int)count : -1;
}

static WCHAR *crt_fwprintf_allocate(SIZE_T required)
{
    if (required > 0x7fffffffU ||
        required >= (SIZE_T)-1 / sizeof(WCHAR)) {
        *crt_errno() = CRT_EOVERFLOW;
        return NULL;
    }
    WCHAR *buffer = (WCHAR *)crt_malloc((required + 1) * sizeof(WCHAR));
    if (!buffer)
        *crt_errno() = CRT_ENOMEM;
    return buffer;
}

static int WINAPI crt_fwprintf_compat32(PVOID stream, const WCHAR *format,
                                         uint32_t *args)
{
    if (!stream || !format) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    WFMT_CTX measure = { NULL, 0, 0 };
    SIZE_T required = do_vformat_wide32(&measure, format, args);
    WCHAR *buffer = crt_fwprintf_allocate(required);
    if (!buffer)
        return -1;

    WFMT_CTX output = { buffer, required + 1, 0 };
    do_vformat_wide32(&output, format, args);
    int result = crt_fwprintf_write(stream, buffer, required);
    crt_free(buffer);
    return result;
}

int WINAPI crt_fwprintf(PVOID stream, const WCHAR *format, ...)
{
    if (!stream || !format) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    ms_va_list ap;
    ms_va_start(ap, format);
    ms_va_list measure_args;
    ms_va_copy(measure_args, ap);
    WFMT_CTX measure = { NULL, 0, 0 };
    int native_required = do_vformat_wide64(&measure, format, measure_args);
    ms_va_end(measure_args);
    if (native_required < 0) {
        ms_va_end(ap);
        return -1;
    }

    SIZE_T required = (SIZE_T)native_required;
    WCHAR *buffer = crt_fwprintf_allocate(required);
    if (!buffer) {
        ms_va_end(ap);
        return -1;
    }

    ms_va_list output_args;
    ms_va_copy(output_args, ap);
    WFMT_CTX output = { buffer, required + 1, 0 };
    do_vformat_wide64(&output, format, output_args);
    ms_va_end(output_args);
    ms_va_end(ap);

    int result = crt_fwprintf_write(stream, buffer, required);
    crt_free(buffer);
    return result;
}

int WINAPI crt_stdio_common_vswprintf(uint64_t options, WCHAR *buffer,
                                      SIZE_T buffer_count,
                                      const WCHAR *format, PVOID locale,
                                      PVOID arg_list)
{
    (void)locale; /* The locale shim currently exposes the invariant C locale. */
    if (!format || (!buffer && buffer_count != 0) || !arg_list) {
        if (buffer && buffer_count) buffer[0] = 0;
        *crt_errno() = 22; /* EINVAL */
        return -1;
    }

    WFMT_CTX ctx = { buffer, buffer_count, 0 };
    int required = do_vformat_wide64(&ctx, format, (ms_va_list)arg_list);
    int truncated = buffer_count == 0 || (SIZE_T)required >= buffer_count;

    static int trace_count;
    if (trace_count < 8) {
        trace_count++;
        serial_puts("[UCRT-VSW] options=0x");
        serial_puthex(options, 8);
        serial_puts(" count=");
        serial_putdec(buffer_count);
        serial_puts(" required=");
        serial_putdec((uint64_t)required);
        serial_puts(" fmt=\"");
        for (int i = 0; i < 48 && format[i]; i++)
            serial_putchar((char)(format[i] < 0x80 ? format[i] : '?'));
        serial_puts("\"\n");
    }

    if (truncated && !(options & CRT_PRINTF_STANDARD_SNPRINTF))
        return -1;
    return required;
}

/* ── MSVC C++ runtime and compiler-intrinsic exports ───────── */

/* ?terminate@@YAXXZ — C++ terminate() handler */
void WINAPI crt_terminate(void)
{
    serial_puts("[MSVCRT] terminate() called — ExitProcess(3)\n");
    ExitProcess(3);
}

static double crt_math_qnan(void)
{
    union { uint64_t bits; double value; } result;
    result.bits = 0x7FF8000000000000ULL;
    return result.value;
}

static double crt_math_positive_infinity(void)
{
    union { uint64_t bits; double value; } result;
    result.bits = 0x7FF0000000000000ULL;
    return result.value;
}

static int crt_math_is_nan(double value)
{
    union { double value; uint64_t bits; } number = { value };
    return (number.bits & 0x7FF0000000000000ULL) ==
               0x7FF0000000000000ULL &&
           (number.bits & 0x000FFFFFFFFFFFFFULL) != 0;
}

static int crt_math_is_infinite(double value)
{
    union { double value; uint64_t bits; } number = { value };
    return (number.bits & 0x7FFFFFFFFFFFFFFFULL) ==
           0x7FF0000000000000ULL;
}

/* _CIacos — compiler intrinsic wrapper for acos */
double WINAPI crt_CIacos(double x)
{
#ifdef TEST_HARNESS
    return acos(x);
#else
    if (crt_math_is_nan(x)) return x;
    if (x > 1.0 || x < -1.0) return crt_math_qnan();
    if (x == 1.0) return 0.0;
    if (x == -1.0) return 3.14159265358979323846;

    double radicand = 1.0 - x * x;
    if (radicand < 0.0) radicand = 0.0;
    double ordinate = crt_sqrt(radicand);
    double result;
    __asm__ volatile (
        "fldl %2\n\t"
        "fldl %1\n\t"
        "fpatan\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(x), "m"(ordinate));
    return result;
#endif
}

/* _CIfmod — compiler intrinsic wrapper for fmod */
double WINAPI crt_CIfmod(double x, double y)
{
    if (crt_math_is_nan(x) || crt_math_is_nan(y)) return x + y;
    if (y == 0.0 || crt_math_is_infinite(x)) return crt_math_qnan();
    if (crt_math_is_infinite(y)) return x;

    double result;
    __asm__ volatile (
        "fldl %2\n\t"
        "fldl %1\n\t"
        "1:\n\t"
        "fprem\n\t"
        "fnstsw %%ax\n\t"
        "testw $0x0400, %%ax\n\t"
        "jnz 1b\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(x), "m"(y)
        : "ax", "cc");
    return result;
}

/* ── Fast 32-bit x87 math (no INT 0x2E overhead) ──────────────
 *
 * _CIpow, _CIfmod, _CIacos, _CIexp, and _CIlog10 use the MSVC _CI calling convention:
 * arguments on the x87 FPU stack, result in ST(0).
 * These run as native 32-bit code in compat mode — no mode switch. */

uint32_t g_fast_CIpow_addr = 0;
uint32_t g_fast_CIfmod_addr = 0;
uint32_t g_fast_CIacos_addr = 0;
uint32_t g_fast_CIexp_addr = 0;
uint32_t g_fast_CIlog10_addr = 0;
uint32_t g_fast_CIsqrt_addr = 0;
uint32_t g_fast_fabs_addr = 0;
uint32_t g_fast_sqrt_addr = 0;

void compat32_init_fast_math(uint8_t *page, uint32_t user_base)
{
    if (!page || !user_base) {
        serial_puts("[FAST-MATH] runtime page missing\n");
        return;
    }

    /* Zero and make executable (page from mem_alloc_pages is identity-mapped) */
    for (int i = 0; i < 4096; i++) page[i] = 0xCC;  /* INT3 fill */

    int p = 0;

    /* _CIpow: ST(1)=base, ST(0)=exp → result in ST(0)
     * Algorithm: pow(x,y) = 2^(y * log2(x))
     * Using x87: fyl2x → f2xm1 → fscale */
    g_fast_CIpow_addr = user_base + (uint32_t)p;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fyl2x — ST(0) = ST(1) * log2(ST(0)), pop */
                               page[p++] = 0xD9; page[p++] = 0xF1;
    /* fld st(0) — dup */     page[p++] = 0xD9; page[p++] = 0xC0;
    /* frndint — ST(0) = round(val) */ page[p++] = 0xD9; page[p++] = 0xFC;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fsub st(0), st(1) — frac = val - int_part */
                               page[p++] = 0xD8; page[p++] = 0xE1;
    /* f2xm1 — ST(0) = 2^frac - 1 */
                               page[p++] = 0xD9; page[p++] = 0xF0;
    /* fld1 */                page[p++] = 0xD9; page[p++] = 0xE8;
    /* faddp st(1), st(0) — ST(0) = 2^frac */
                               page[p++] = 0xDE; page[p++] = 0xC1;
    /* fscale — ST(0) *= 2^ST(1) = 2^int_part */
                               page[p++] = 0xD9; page[p++] = 0xFD;
    /* fstp st(1) — pop int_part, result in ST(0) */
                               page[p++] = 0xDD; page[p++] = 0xD9;
    /* ret */                 page[p++] = 0xC3;

    /* Align next function */
    p = (p + 15) & ~15;

    /* _CIfmod: ST(1)=x, ST(0)=y → result in ST(0) = x mod y */
    g_fast_CIfmod_addr = user_base + (uint32_t)p;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* FPREM can produce a partial reduction; repeat while C2 is set.
     * Preserve EAX because the intrinsic ABI need not spill it. */
    /* push eax */           page[p++] = 0x50;
    /* fprem */               page[p++] = 0xD9; page[p++] = 0xF8;
    /* fnstsw ax */          page[p++] = 0xDF; page[p++] = 0xE0;
    /* test ah,4 */          page[p++] = 0xF6; page[p++] = 0xC4;
                              page[p++] = 0x04;
    /* jnz fprem */          page[p++] = 0x75; page[p++] = 0xF7;
    /* pop eax */            page[p++] = 0x58;
    /* fstp st(1) */          page[p++] = 0xDD; page[p++] = 0xD9;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* _CIacos: ST(0)=x → result in ST(0) = acos(x)
     * acos(x) = atan2(sqrt(1-x^2), x) */
    g_fast_CIacos_addr = user_base + (uint32_t)p;
    /* fld st(0) — dup x */   page[p++] = 0xD9; page[p++] = 0xC0;
    /* fmul st(0), st(0) */   page[p++] = 0xD8; page[p++] = 0xC8;
    /* fld1 */                page[p++] = 0xD9; page[p++] = 0xE8;
    /* fsubrp st(1): 1-x*x */ page[p++] = 0xDE; page[p++] = 0xE1;
    /* fsqrt */               page[p++] = 0xD9; page[p++] = 0xFA;
    /* fxch st(1) */          page[p++] = 0xD9; page[p++] = 0xC9;
    /* fpatan — atan2(ST(1), ST(0)) */
                               page[p++] = 0xD9; page[p++] = 0xF3;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* fabs(double): argument at [esp+4], cdecl result in ST(0). */
    g_fast_fabs_addr = user_base + (uint32_t)p;
    /* fld qword [esp+4] */   page[p++] = 0xDD; page[p++] = 0x44;
                               page[p++] = 0x24; page[p++] = 0x04;
    /* fabs */                page[p++] = 0xD9; page[p++] = 0xE1;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* sqrt(double): argument at [esp+4], cdecl result in ST(0). */
    g_fast_sqrt_addr = user_base + (uint32_t)p;
    /* fld qword [esp+4] */   page[p++] = 0xDD; page[p++] = 0x44;
                               page[p++] = 0x24; page[p++] = 0x04;
    /* fsqrt */               page[p++] = 0xD9; page[p++] = 0xFA;
    /* ret */                 page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* _CIexp: ST(0)=x -> result in ST(0) = e^x. */
    g_fast_CIexp_addr = user_base + (uint32_t)p;
    /* fldl2e */              page[p++] = 0xD9; page[p++] = 0xEA;
    /* fmulp st(1), st(0) */ page[p++] = 0xDE; page[p++] = 0xC9;
    /* fld st(0) */          page[p++] = 0xD9; page[p++] = 0xC0;
    /* frndint */            page[p++] = 0xD9; page[p++] = 0xFC;
    /* fxch st(1) */         page[p++] = 0xD9; page[p++] = 0xC9;
    /* fsub st(0), st(1) */  page[p++] = 0xD8; page[p++] = 0xE1;
    /* f2xm1 */              page[p++] = 0xD9; page[p++] = 0xF0;
    /* fld1 */               page[p++] = 0xD9; page[p++] = 0xE8;
    /* faddp st(1), st(0) */ page[p++] = 0xDE; page[p++] = 0xC1;
    /* fscale */             page[p++] = 0xD9; page[p++] = 0xFD;
    /* fstp st(1) */         page[p++] = 0xDD; page[p++] = 0xD9;
    /* ret */                page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* _CIlog10: ST(0)=x -> result in ST(0) = log10(x). */
    g_fast_CIlog10_addr = user_base + (uint32_t)p;
    /* fldlg2 */             page[p++] = 0xD9; page[p++] = 0xEC;
    /* fxch st(1) */         page[p++] = 0xD9; page[p++] = 0xC9;
    /* fyl2x */              page[p++] = 0xD9; page[p++] = 0xF1;
    /* ret */                page[p++] = 0xC3;

    p = (p + 15) & ~15;

    /* _CIsqrt: ST(0)=x -> result in ST(0). */
    g_fast_CIsqrt_addr = user_base + (uint32_t)p;
    /* fsqrt */              page[p++] = 0xD9; page[p++] = 0xFA;
    /* ret */                page[p++] = 0xC3;

    serial_puts("[FAST-MATH] pow=0x");
    serial_puthex(g_fast_CIpow_addr, 8);
    serial_puts(" fmod=0x");
    serial_puthex(g_fast_CIfmod_addr, 8);
    serial_puts(" acos=0x");
    serial_puthex(g_fast_CIacos_addr, 8);
    serial_puts(" fabs=0x");
    serial_puthex(g_fast_fabs_addr, 8);
    serial_puts(" sqrt=0x");
    serial_puthex(g_fast_sqrt_addr, 8);
    serial_puts(" exp=0x");
    serial_puthex(g_fast_CIexp_addr, 8);
    serial_puts(" log10=0x");
    serial_puthex(g_fast_CIlog10_addr, 8);
    serial_puts(" CIsqrt=0x");
    serial_puthex(g_fast_CIsqrt_addr, 8);
    serial_puts("\n");
}

/* _CIpow — compiler intrinsic wrapper for pow (fallback via INT 0x2E) */
double WINAPI crt_CIpow(double base, double exp)
{
#ifdef TEST_HARNESS
    return pow(base, exp);
#else
    if (exp == 0.0) return 1.0;
    if (base == 1.0) return 1.0;
    if (crt_math_is_nan(base) || crt_math_is_nan(exp)) return base + exp;
    if (base == 0.0)
        return exp < 0.0 ? crt_math_positive_infinity() : 0.0;

    /* Integer exponents preserve the sign of negative bases and avoid the
     * logarithm domain restriction. */
    if (exp >= -2147483648.0 && exp <= 2147483647.0) {
        int iexp = (int)exp;
        if ((double)iexp != exp) goto fractional_exponent;
        unsigned int magnitude = iexp < 0
            ? (unsigned int)(-(int64_t)iexp) : (unsigned int)iexp;
        double result = 1.0;
        double b = base;
        while (magnitude) {
            if (magnitude & 1U) result *= b;
            b *= b;
            magnitude >>= 1;
        }
        return iexp < 0 ? 1.0 / result : result;
    }

fractional_exponent:
    if (base < 0.0) return crt_math_qnan();

    double result;
    __asm__ volatile (
        "fldl %2\n\t"
        "fldl %1\n\t"
        "fyl2x\n\t"
        "fld %%st(0)\n\t"
        "frndint\n\t"
        "fxch %%st(1)\n\t"
        "fsub %%st(1), %%st(0)\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fscale\n\t"
        "fstp %%st(1)\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(base), "m"(exp));
    return result;
#endif
}

/* Resolver identity and non-PE32 fallback for the x87 _CIexp intrinsic. */
double WINAPI crt_CIexp(double x)
{
#ifdef TEST_HARNESS
    return exp(x);
#else
    return crt_CIpow(2.71828182845904523536, x);
#endif
}

/* Resolver identity and non-PE32 fallback for the x87 _CIlog10 intrinsic. */
double WINAPI crt_CIlog10(double x)
{
#ifdef TEST_HARNESS
    return log10(x);
#else
    double result;
    __asm__ volatile(
        "fldlg2\n\t"
        "fldl %1\n\t"
        "fyl2x\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(x));
    return result;
#endif
}

/* Resolver identity and non-PE32 fallback for the x87 _CIsqrt intrinsic. */
double WINAPI crt_CIsqrt(double x)
{
    return crt_sqrt(x);
}

static int crt_double_bits_finite(uint64_t bits)
{
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

int WINAPI crt_finite(double x)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    return crt_double_bits_finite(bits);
}

/* The PE32 gateway marshals stack DWORDs as integer arguments. Reassemble the
 * IEEE-754 payload here instead of relying on the native XMM argument ABI. */
static uint64_t WINAPI crt_finite_compat32(uint64_t low, uint64_t high)
{
    uint64_t bits = (uint32_t)low | ((uint64_t)(uint32_t)high << 32);
    return (uint64_t)crt_double_bits_finite(bits);
}

/* _isnan — check for NaN (IEEE 754: exponent all 1s, mantissa non-zero) */
static int crt_double_bits_nan(uint64_t bits)
{
    return ((bits >> 52) & 0x7FF) == 0x7FF &&
           (bits & 0x000FFFFFFFFFFFFFULL) != 0;
}

int WINAPI crt_isnan(double x)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &x, 8);
    return crt_double_bits_nan(bits);
}

static uint64_t WINAPI crt_isnan_compat32(uint64_t low, uint64_t high)
{
    uint64_t bits = (uint32_t)low | ((uint64_t)(uint32_t)high << 32);
    return (uint64_t)crt_double_bits_nan(bits);
}

static short crt_fpclass_from_parts(uint64_t exponent, uint64_t fraction,
                                    uint64_t exponent_mask)
{
    if (exponent == exponent_mask)
        return fraction ? 2 : 1;   /* FP_NAN : FP_INFINITE */
    if (exponent == 0)
        return fraction ? -2 : 0;  /* FP_SUBNORMAL : FP_ZERO */
    return -1;                     /* FP_NORMAL */
}

short WINAPI crt_dclass(double x)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    return crt_fpclass_from_parts((bits >> 52) & 0x7FF,
                                  bits & 0x000FFFFFFFFFFFFFULL, 0x7FF);
}

short WINAPI crt_fdclass(float x)
{
    uint32_t bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    return crt_fpclass_from_parts((bits >> 23) & 0xFF,
                                  bits & 0x007FFFFF, 0xFF);
}

/* PE32 MSVC stat layouts. Keep these independent from the 64-bit kernel ABI. */
typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    int32_t st_size;
    int32_t st_atime;
    int32_t st_mtime;
    int32_t st_ctime;
} CRT_STAT32;

typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    uint32_t reserved1;
    int64_t st_size;
    int32_t st_atime;
    int32_t st_mtime;
    int32_t st_ctime;
    uint32_t reserved2;
} CRT_STAT32I64;

typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    int32_t st_size;
    int64_t st_atime;
    int64_t st_mtime;
    int64_t st_ctime;
} CRT_STAT64I32;

typedef struct __attribute__((packed)) {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint16_t reserved0;
    uint32_t st_rdev;
    uint32_t reserved1;
    int64_t st_size;
    int64_t st_atime;
    int64_t st_mtime;
    int64_t st_ctime;
} CRT_STAT64;

_Static_assert(sizeof(CRT_STAT32) == 36, "PE32 _stat32 layout changed");
_Static_assert(sizeof(CRT_STAT32I64) == 48,
               "PE32 _stat32i64 layout changed");
_Static_assert(sizeof(CRT_STAT64I32) == 48,
               "PE32 _stat64i32 layout changed");
_Static_assert(sizeof(CRT_STAT64) == 56, "PE32 _stat64 layout changed");

typedef struct {
    uint16_t mode;
    uint64_t size;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
} CRT_STAT_META;

#define CRT_S_IFDIR  0x4000
#define CRT_S_IFREG  0x8000
#define CRT_S_IFCHR  0x2000
#define CRT_S_IFIFO  0x1000
#define CRT_S_IEXEC  0x0040
#define CRT_S_IWRITE 0x0080
#define CRT_S_IREAD  0x0100

extern void *osfs2_find_exact_ci(const char *name);
extern void *osfs2_find(const char *name);
extern uint64_t osfs2_file_size(void *file);
extern uint32_t osfs2_file_ctime(void *file);
extern uint32_t osfs2_file_mtime(void *file);

int WINAPI crt_chmod(const char *path, int mode)
{
    if (!path || !path[0] || !(mode & (CRT_S_IREAD | CRT_S_IWRITE))) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    char normalized[260];
    if (!win32_normalize_path(path, normalized)) {
        *crt_errno() = CRT_ENAMETOOLONG;
        return -1;
    }

    vfs_node_t node;
    if (!vfs_find(normalized, VFS_MODE_WIN32, &node)) {
        *crt_errno() = CRT_ENOENT;
        return -1;
    }

    uint16_t current_mode;
    if (vfs_get_mode(&node, &current_mode) != VFS_STATUS_OK) {
        *crt_errno() = CRT_EACCES;
        return -1;
    }

    /* The Microsoft CRT models the Win32 read-only attribute: files remain
     * readable, while _S_IWRITE toggles write access for every class. */
    uint16_t updated_mode = (uint16_t)((current_mode & 07111U) | 0444U);
    if (mode & CRT_S_IWRITE)
        updated_mode |= 0222U;
    if (vfs_set_mode(&node, updated_mode) != VFS_STATUS_OK) {
        *crt_errno() = CRT_EACCES;
        return -1;
    }

    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_wchmod(const WCHAR *path, int mode)
{
    if (!path) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    char narrow[260];
    if (!WideCharToMultiByte(0 /* CP_ACP */, 0, path, -1, narrow,
                             sizeof(narrow), NULL, NULL)) {
        *crt_errno() = CRT_ENAMETOOLONG;
        return -1;
    }
    return crt_chmod(narrow, mode);
}

static int crt_path_has_executable_suffix(const char *path)
{
    const char *suffix = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == '.') suffix = p;
    }
    if (*suffix != '.') return 0;

    char ext[5] = {0};
    int i = 0;
    while (suffix[i] && i < 4) {
        char c = suffix[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        ext[i++] = c;
    }
    if (suffix[i]) return 0;
    return (ext[1] == 'e' && ext[2] == 'x' && ext[3] == 'e') ||
           (ext[1] == 'c' && ext[2] == 'o' && ext[3] == 'm') ||
           (ext[1] == 'b' && ext[2] == 'a' && ext[3] == 't') ||
           (ext[1] == 'c' && ext[2] == 'm' && ext[3] == 'd');
}

static int crt_stat_query(const char *path, CRT_STAT_META *meta)
{
    if (!path || !path[0] || !meta) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    char normalized[260];
    if (!win32_normalize_path(path, normalized)) {
        *crt_errno() = CRT_ENAMETOOLONG;
        return -1;
    }

    void *file = osfs2_find_exact_ci(normalized);
    if (file) {
        meta->mode = CRT_S_IFREG | CRT_S_IREAD | CRT_S_IWRITE;
        vfs_node_t node;
        uint16_t stored_mode;
        if (vfs_find(normalized, VFS_MODE_WIN32, &node) &&
            vfs_get_mode(&node, &stored_mode) == VFS_STATUS_OK) {
            meta->mode = CRT_S_IFREG;
            if (stored_mode & 0444U) meta->mode |= CRT_S_IREAD;
            if (stored_mode & 0222U) meta->mode |= CRT_S_IWRITE;
            if (stored_mode & 0111U) meta->mode |= CRT_S_IEXEC;
        } else if (crt_path_has_executable_suffix(normalized)) {
            meta->mode |= CRT_S_IEXEC;
        }
        meta->size = osfs2_file_size(file);
        meta->ctime = (int64_t)osfs2_file_ctime(file);
        meta->mtime = (int64_t)osfs2_file_mtime(file);
        meta->atime = meta->mtime;
        *crt_errno() = 0;
        return 0;
    }

    if (win32_directory_exists_normalized(normalized)) {
        meta->mode = CRT_S_IFDIR | CRT_S_IREAD | CRT_S_IWRITE | CRT_S_IEXEC;
        meta->size = 0;
        meta->atime = 0;
        meta->mtime = 0;
        meta->ctime = 0;
        *crt_errno() = 0;
        return 0;
    }

    *crt_errno() = CRT_ENOENT;
    return -1;
}

#define CRT_STAT_FILL_COMMON(stat, meta) do { \
    (stat)->st_dev = 2; /* C: */ \
    (stat)->st_ino = 0; \
    (stat)->st_mode = (meta)->mode; \
    (stat)->st_nlink = 1; \
    (stat)->st_uid = 0; \
    (stat)->st_gid = 0; \
    (stat)->st_rdev = 2; \
} while (0)

static int crt_stat32_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;
    if (meta.size > 0x7FFFFFFFULL) {
        *crt_errno() = CRT_EOVERFLOW;
        return -1;
    }

    CRT_STAT32 *stat = (CRT_STAT32 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int32_t)meta.size;
    stat->st_atime = (int32_t)meta.atime;
    stat->st_mtime = (int32_t)meta.mtime;
    stat->st_ctime = (int32_t)meta.ctime;
    return 0;
}

static int crt_stat32i64_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;

    CRT_STAT32I64 *stat = (CRT_STAT32I64 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int64_t)meta.size;
    stat->st_atime = (int32_t)meta.atime;
    stat->st_mtime = (int32_t)meta.mtime;
    stat->st_ctime = (int32_t)meta.ctime;
    return 0;
}

static int crt_stat64i32_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;
    if (meta.size > 0x7FFFFFFFULL) {
        *crt_errno() = CRT_EOVERFLOW;
        return -1;
    }

    CRT_STAT64I32 *stat = (CRT_STAT64I32 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int32_t)meta.size;
    stat->st_atime = meta.atime;
    stat->st_mtime = meta.mtime;
    stat->st_ctime = meta.ctime;
    if (crt_io_trace_take(&crt_stat_trace_count, 64)) {
        serial_puts("[CRT-STAT64I32] size=");
        serial_putdec(meta.size);
        serial_puts(" mode=0x");
        serial_puthex(meta.mode, 4);
        serial_puts(" path='");
        serial_puts(path);
        serial_puts("'\n");
    }
    return 0;
}

static int crt_stat64_impl(const char *path, PVOID buf)
{
    CRT_STAT_META meta;
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    if (crt_stat_query(path, &meta) < 0) return -1;

    CRT_STAT64 *stat = (CRT_STAT64 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    stat->st_size = (int64_t)meta.size;
    stat->st_atime = meta.atime;
    stat->st_mtime = meta.mtime;
    stat->st_ctime = meta.ctime;
    return 0;
}

int WINAPI crt_stat(const char *path, PVOID buf)
{
    return crt_stat64i32_impl(path, buf);
}

int WINAPI crt_stat32(const char *path, PVOID buf)
{
    return crt_stat32_impl(path, buf);
}

int WINAPI crt_stat32i64(const char *path, PVOID buf)
{
    return crt_stat32i64_impl(path, buf);
}

int WINAPI crt_stat64i32(const char *path, PVOID buf)
{
    return crt_stat64i32_impl(path, buf);
}

int WINAPI crt_stat64(const char *path, PVOID buf)
{
    return crt_stat64_impl(path, buf);
}

static int64_t crt_filetime_to_unix_seconds(LONGLONG filetime)
{
    if (filetime <= 0 ||
        (uint64_t)filetime < WINTIME_UNIX_EPOCH_FILETIME)
        return 0;
    return (int64_t)(((uint64_t)filetime - WINTIME_UNIX_EPOCH_FILETIME) /
                     WINTIME_TICKS_PER_SECOND);
}

int WINAPI crt_fstat64(int fd, PVOID buf)
{
    CRT_FILE *file = crt_file_from_fd(fd);
    if (!file) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }
    if (!buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }

    DWORD file_type = GetFileType(file->nt_handle);
    if (!file_type && GetLastError() != 0) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }

    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION standard;
    FILE_BASIC_INFORMATION basic;
    NTSTATUS status = NtQueryInformationFile(
        file->nt_handle, &iosb, &standard, sizeof(standard),
        FileStandardInformation);
    if (NT_SUCCESS(status)) {
        status = NtQueryInformationFile(
            file->nt_handle, &iosb, &basic, sizeof(basic),
            FileBasicInformation);
    }
    if (!NT_SUCCESS(status)) {
        *crt_errno() = CRT_EBADF;
        return -1;
    }

    CRT_STAT_META meta;
    crt_memset(&meta, 0, sizeof(meta));
    if (file_type == 1) { /* FILE_TYPE_DISK */
        meta.mode = standard.Directory ? CRT_S_IFDIR : CRT_S_IFREG;
        meta.mode |= CRT_S_IREAD;
        if (!(basic.FileAttributes & FILE_ATTRIBUTE_READONLY))
            meta.mode |= CRT_S_IWRITE;
        if (standard.Directory) meta.mode |= CRT_S_IEXEC;
        meta.size = standard.Directory ? 0 :
                    (uint64_t)standard.EndOfFile.QuadPart;
        meta.atime = crt_filetime_to_unix_seconds(
            basic.LastAccessTime.QuadPart);
        meta.mtime = crt_filetime_to_unix_seconds(
            basic.LastWriteTime.QuadPart);
        meta.ctime = crt_filetime_to_unix_seconds(
            basic.CreationTime.QuadPart);
    } else {
        meta.mode = file_type == 3 ? CRT_S_IFIFO : CRT_S_IFCHR;
        if (file->flags & 1) meta.mode |= CRT_S_IREAD;
        if (file->flags & 2) meta.mode |= CRT_S_IWRITE;
    }

    CRT_STAT64 *stat = (CRT_STAT64 *)buf;
    crt_memset(stat, 0, sizeof(*stat));
    CRT_STAT_FILL_COMMON(stat, &meta);
    if (file_type != 1) {
        stat->st_dev = 0;
        stat->st_rdev = 0;
    }
    stat->st_nlink = standard.NumberOfLinks > 32767U
                   ? 32767 : (int16_t)standard.NumberOfLinks;
    stat->st_size = (int64_t)meta.size;
    stat->st_atime = meta.atime;
    stat->st_mtime = meta.mtime;
    stat->st_ctime = meta.ctime;
    *crt_errno() = 0;
    return 0;
}

typedef int (*CRT_STAT_IMPL)(const char *, PVOID);

static int crt_wstat_impl(const WCHAR *path, PVOID buf, CRT_STAT_IMPL impl)
{
    if (!path || !buf) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    char narrow[260];
    if (!WideCharToMultiByte(0 /* CP_ACP */, 0, path, -1, narrow,
                             sizeof(narrow), NULL, NULL)) {
        *crt_errno() = CRT_ENAMETOOLONG;
        return -1;
    }
    return impl(narrow, buf);
}

int WINAPI crt_wstat(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat64i32_impl);
}

int WINAPI crt_wstat32(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat32_impl);
}

int WINAPI crt_wstat32i64(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat32i64_impl);
}

int WINAPI crt_wstat64i32(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat64i32_impl);
}

int WINAPI crt_wstat64(const WCHAR *path, PVOID buf)
{
    return crt_wstat_impl(path, buf, crt_stat64_impl);
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

/* _vsnwprintf — wide vsnprintf with format processing
 *
 * CRITICAL: This is called from 32-bit compat mode via INT 0x2E thunk.
 * The va_list (ap) points to the 32-bit caller's stack where each vararg
 * occupies 4 bytes. ms_va_arg() reads 8-byte slots (64-bit mode) which
 * is WRONG — it combines two 4-byte args into one garbage value.
 * We must manually walk 4-byte slots using a uint32_t pointer.
 */
int WINAPI crt_vsnwprintf(WCHAR *buf, SIZE_T count, const WCHAR *fmt, ms_va_list ap)
{
    if (!buf || count == 0) return 0;
    if (!fmt) { buf[0] = 0; return 0; }

    /* Walk the 32-bit va_list manually — 4 bytes per arg */
    uint32_t *vp = (uint32_t *)(void *)ap;


    SIZE_T pos = 0;
    SIZE_T max = count - 1;

    while (*fmt && pos < max) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags */
        int left_align = 0, zero_pad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad = 1;
            fmt++;
        }
        (void)left_align;

        /* Parse width */
        int width = 0;
        if (*fmt == '*') {
            width = (int)(*vp++);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }

        /* Parse precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = (int)(*vp++);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; } /* ll */

        /* Conversion */
        switch (*fmt) {
        case 's': {
            /* In MSVC _vsnwprintf: %s = WCHAR* (wide string).
             * %ls is also wide string. We treat both the same. */
            uint32_t raw_ptr = *vp++;
            const WCHAR *ws = (const WCHAR *)(uintptr_t)raw_ptr;
            if (!ws) ws = (const WCHAR[]){'(','n','u','l','l',')',0};
            int n = 0;
            while (ws[n]) n++;
            if (precision >= 0 && n > precision) n = precision;
            for (int i = 0; i < n && pos < max; i++)
                buf[pos++] = ws[i];
            fmt++;
            break;
        }
        case 'S': {
            /* %S = narrow string in wide printf (MSVC: %S or %hs = char*) */
            const char *ns = (const char *)(uintptr_t)(*vp++);
            if (!ns) ns = "(null)";
            int n = 0;
            while (ns[n]) n++;
            if (precision >= 0 && n > precision) n = precision;
            for (int i = 0; i < n && pos < max; i++)
                buf[pos++] = (WCHAR)(unsigned char)ns[i];
            fmt++;
            break;
        }
        case 'c': {
            WCHAR c = (WCHAR)(*vp++);
            if (pos < max) buf[pos++] = c;
            fmt++;
            break;
        }
        case 'd': case 'i': {
            int32_t val32 = (int32_t)(*vp++);
            long long val = (long long)val32;
            int neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            WCHAR tmp[24];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + (int)(val % 10); val /= 10; }
            if (neg) tmp[len++] = '-';
            int pad = width - len;
            if (pad > 0 && zero_pad) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = '0';
            else if (pad > 0) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = ' ';
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case 'u': {
            uint32_t val = *vp++;
            WCHAR tmp[24];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = '0' + (int)(val % 10); val /= 10; }
            int pad = width - len;
            if (pad > 0 && zero_pad) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = '0';
            else if (pad > 0) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = ' ';
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case 'x': case 'X': {
            int upper = (*fmt == 'X');
            uint32_t val = *vp++;
            const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            WCHAR tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = hex[val & 0xF]; val >>= 4; }
            int pad = width - len;
            if (pad > 0 && zero_pad) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = '0';
            else if (pad > 0) for (int i = 0; i < pad && pos < max; i++) buf[pos++] = ' ';
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case 'p': {
            uint32_t val = *vp++;
            const char *hex = "0123456789abcdef";
            WCHAR tmp[20];
            int len = 0;
            if (val == 0) tmp[len++] = '0';
            else while (val > 0) { tmp[len++] = hex[val & 0xF]; val >>= 4; }
            for (int i = len - 1; i >= 0 && pos < max; i--) buf[pos++] = tmp[i];
            fmt++;
            break;
        }
        case '%':
            if (pos < max) buf[pos++] = '%';
            fmt++;
            break;
        case 0:
            break;
        default:
            /* Unknown format — output literal */
            if (pos < max) buf[pos++] = '%';
            if (pos < max) buf[pos++] = *fmt;
            fmt++;
            break;
        }
    }

    buf[pos] = 0;
    return (int)pos;
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
static double crt_round_integral(double x, uint16_t rounding)
{
    double result;
    uint16_t saved_control, control;
    __asm__ volatile ("fnstcw %0" : "=m"(saved_control));
    control = (saved_control & ~0x0C00U) | rounding;
    /* FRNDINT keeps the full floating-point range, signed zero and NaNs.
     * Restore the caller's precision, rounding and exception-mask control. */
    __asm__ volatile (
        "fldcw %1; fldl %2; frndint; fstpl %0; fldcw %3"
        : "=m"(result)
        : "m"(control), "m"(x), "m"(saved_control)
        : "st", "memory");
    return result;
}

double WINAPI crt_ceil(double x)
{
    return crt_round_integral(x, 0x0800U);
}

double WINAPI crt_floor(double x)
{
    return crt_round_integral(x, 0x0400U);
}

static uint64_t crt_round_compat32(uint64_t low, uint64_t high,
                                  uint16_t rounding)
{
    union { double value; uint64_t bits; } input, result;
    input.bits = (uint32_t)low | ((uint64_t)(uint32_t)high << 32);
    result.value = crt_round_integral(input.value, rounding);
    return result.bits;
}

uint64_t WINAPI crt_ceil_compat32(uint64_t low, uint64_t high)
{
    return crt_round_compat32(low, high, 0x0800U);
}

uint64_t WINAPI crt_floor_compat32(uint64_t low, uint64_t high)
{
    return crt_round_compat32(low, high, 0x0400U);
}

double WINAPI crt_fabs(double x)
{
    union { double value; uint64_t bits; } v = { x };
    v.bits &= ~(1ULL << 63);
    return v.value;
}

double WINAPI crt_sqrt(double x)
{
    union { double value; uint64_t bits; } v = { x };
    if (x == 0.0 || (v.bits & 0x7FF0000000000000ULL) ==
                    0x7FF0000000000000ULL)
        return x;
    if (x < 0.0) {
        v.bits = 0x7FF8000000000000ULL;
        return v.value;
    }

    union { double value; uint64_t bits; } guess;
    guess.bits = (v.bits >> 1) + 0x1FF8000000000000ULL;
    for (int i = 0; i < 8; i++)
        guess.value = 0.5 * (guess.value + x / guess.value);
    return guess.value;
}

/* difftime — difference between two time_t values */
double WINAPI crt_difftime(crt_time_t t1, crt_time_t t0)
{
    return (double)(t1 - t0);
}

/* gmtime — convert time_t to struct tm (stub) */
static int crt_tm_from_unix(int64_t unix_seconds, struct crt_tm *result)
{
    WINTIME_CALENDAR calendar;
    if (!result || wintime_unix_to_calendar(unix_seconds, &calendar) < 0)
        return CRT_EINVAL;

    result->tm_sec = calendar.second;
    result->tm_min = calendar.minute;
    result->tm_hour = calendar.hour;
    result->tm_mday = calendar.day;
    result->tm_mon = calendar.month - 1;
    result->tm_year = calendar.year - 1900;
    result->tm_wday = calendar.day_of_week;
    result->tm_yday = calendar.day_of_year;
    result->tm_isdst = 0;
    return 0;
}

PVOID WINAPI crt_gmtime(const crt_time_t *timer)
{
    if (!timer) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    UCRT_PROCESS_MODE_VALUES *state = ucrt_process_mode_state(TRUE);
    if (!state ||
        crt_tm_from_unix((int64_t)*timer, &state->time_buffer) != 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    *crt_errno() = 0;
    return (PVOID)&state->time_buffer;
}

int64_t WINAPI crt_time64(int64_t *timer)
{
    int64_t result = wintime_now_unix_seconds();
    if (timer) *timer = result;
    return result;
}

char* WINAPI crt_ctime64(const int64_t *timer)
{
    /* MSVCR100 bounds __time64_t at the final second of year 3000. */
    if (!timer || *timer < 0 || *timer > 32535215999LL) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    UCRT_PROCESS_MODE_VALUES *state = ucrt_process_mode_state(TRUE);
    if (!state) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    if (crt_tm_from_unix(*timer, &state->time_buffer) != 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    SIZE_T length = crt_strftime(state->ctime_buffer,
                                 sizeof(state->ctime_buffer),
                                 "%a %b %e %H:%M:%S %Y\n",
                                 &state->time_buffer);
    if (length != sizeof(state->ctime_buffer) - 1) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    return state->ctime_buffer;
}

int WINAPI crt_gmtime64_s(PVOID result, const int64_t *timer)
{
    if (!result || !timer) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    int error = crt_tm_from_unix(*timer, (struct crt_tm *)result);
    *crt_errno() = error;
    return error;
}

PVOID WINAPI crt_localtime64(const int64_t *timer)
{
    if (!timer) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    UCRT_PROCESS_MODE_VALUES *state = ucrt_process_mode_state(TRUE);
    if (!state || crt_tm_from_unix(*timer, &state->time_buffer) != 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    *crt_errno() = 0;
    return (PVOID)&state->time_buffer;
}

int WINAPI crt_localtime64_s(PVOID result, const int64_t *timer)
{
    /* Local time is UTC while the system time-zone bias is zero. */
    return crt_gmtime64_s(result, timer);
}

/* mktime — convert struct tm to time_t (stub) */
typedef struct {
    char *data;
    SIZE_T capacity;
    SIZE_T length;
    BOOL overflow;
} CRT_STRFTIME_OUTPUT;

static void crt_strftime_putc(CRT_STRFTIME_OUTPUT *output, char c)
{
    if (output->overflow) return;
    if (output->length + 1 >= output->capacity) {
        output->overflow = TRUE;
        return;
    }
    output->data[output->length++] = c;
    output->data[output->length] = 0;
}

static void crt_strftime_puts(CRT_STRFTIME_OUTPUT *output, const char *text)
{
    while (*text) crt_strftime_putc(output, *text++);
}

static void crt_strftime_number(CRT_STRFTIME_OUTPUT *output,
                                unsigned int value, int width, char padding,
                                BOOL alternate)
{
    char digits[16];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < (int)sizeof(digits));

    if (!alternate)
        while (count < width) {
            crt_strftime_putc(output, padding);
            width--;
        }
    while (count > 0) crt_strftime_putc(output, digits[--count]);
}

static BOOL crt_tm_is_leap_year(int year)
{
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

static int crt_tm_jan1_wday(const struct crt_tm *tm)
{
    int wday = tm->tm_wday - (tm->tm_yday % 7);
    if (wday < 0) wday += 7;
    return wday;
}

static int crt_tm_iso_weeks(int year, int jan1_wday)
{
    return jan1_wday == 4 ||
           (jan1_wday == 3 && crt_tm_is_leap_year(year)) ? 53 : 52;
}

static void crt_tm_iso_week(const struct crt_tm *tm, int *iso_year,
                            int *iso_week)
{
    int year = tm->tm_year + 1900;
    int iso_wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
    int week = (tm->tm_yday + 10 - iso_wday) / 7;
    int jan1_wday = crt_tm_jan1_wday(tm);

    if (week < 1) {
        int previous_year = year - 1;
        int previous_days = crt_tm_is_leap_year(previous_year) ? 366 : 365;
        int previous_jan1 = (jan1_wday - previous_days % 7 + 7) % 7;
        year = previous_year;
        week = crt_tm_iso_weeks(year, previous_jan1);
    } else if (week > crt_tm_iso_weeks(year, jan1_wday)) {
        year++;
        week = 1;
    }

    *iso_year = year;
    *iso_week = week;
}

static void crt_strftime_format(CRT_STRFTIME_OUTPUT *output,
                                const char *format,
                                const struct crt_tm *tm)
{
    static const char *const weekdays_short[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const weekdays_long[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
        "Friday", "Saturday"
    };
    static const char *const months_short[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    static const char *const months_long[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    while (*format && !output->overflow) {
        if (*format != '%') {
            crt_strftime_putc(output, *format++);
            continue;
        }
        format++;
        BOOL alternate = FALSE;
        if (*format == '#') {
            alternate = TRUE;
            format++;
        }
        if (*format == 'E' || *format == 'O') format++;
        char specifier = *format ? *format++ : 0;
        int year = tm->tm_year + 1900;
        int hour12 = tm->tm_hour % 12;
        if (!hour12) hour12 = 12;

        switch (specifier) {
        case '%': crt_strftime_putc(output, '%'); break;
        case 'a':
            crt_strftime_puts(output,
                tm->tm_wday >= 0 && tm->tm_wday < 7
                    ? weekdays_short[tm->tm_wday] : "???");
            break;
        case 'A':
            crt_strftime_puts(output,
                tm->tm_wday >= 0 && tm->tm_wday < 7
                    ? weekdays_long[tm->tm_wday] : "???");
            break;
        case 'b': case 'h':
            crt_strftime_puts(output,
                tm->tm_mon >= 0 && tm->tm_mon < 12
                    ? months_short[tm->tm_mon] : "???");
            break;
        case 'B':
            crt_strftime_puts(output,
                tm->tm_mon >= 0 && tm->tm_mon < 12
                    ? months_long[tm->tm_mon] : "???");
            break;
        case 'c':
            crt_strftime_format(output, "%a %b %e %H:%M:%S %Y", tm);
            break;
        case 'C':
            crt_strftime_number(output, (unsigned int)(year / 100), 2, '0',
                                alternate);
            break;
        case 'd':
            crt_strftime_number(output, (unsigned int)tm->tm_mday, 2, '0',
                                alternate);
            break;
        case 'D': crt_strftime_format(output, "%m/%d/%y", tm); break;
        case 'e':
            crt_strftime_number(output, (unsigned int)tm->tm_mday, 2, ' ',
                                alternate);
            break;
        case 'F': crt_strftime_format(output, "%Y-%m-%d", tm); break;
        case 'g': case 'G': case 'V': {
            int iso_year, iso_week;
            crt_tm_iso_week(tm, &iso_year, &iso_week);
            if (specifier == 'V')
                crt_strftime_number(output, (unsigned int)iso_week, 2, '0',
                                    alternate);
            else if (specifier == 'g')
                crt_strftime_number(output,
                    (unsigned int)((iso_year % 100 + 100) % 100), 2, '0',
                    alternate);
            else
                crt_strftime_number(output, (unsigned int)iso_year, 4, '0',
                                    alternate);
            break;
        }
        case 'H':
            crt_strftime_number(output, (unsigned int)tm->tm_hour, 2, '0',
                                alternate);
            break;
        case 'I':
            crt_strftime_number(output, (unsigned int)hour12, 2, '0',
                                alternate);
            break;
        case 'j':
            crt_strftime_number(output, (unsigned int)(tm->tm_yday + 1),
                                3, '0', alternate);
            break;
        case 'm':
            crt_strftime_number(output, (unsigned int)(tm->tm_mon + 1),
                                2, '0', alternate);
            break;
        case 'M':
            crt_strftime_number(output, (unsigned int)tm->tm_min, 2, '0',
                                alternate);
            break;
        case 'n': crt_strftime_putc(output, '\n'); break;
        case 'p': crt_strftime_puts(output, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r': crt_strftime_format(output, "%I:%M:%S %p", tm); break;
        case 'R': crt_strftime_format(output, "%H:%M", tm); break;
        case 'S':
            crt_strftime_number(output, (unsigned int)tm->tm_sec, 2, '0',
                                alternate);
            break;
        case 't': crt_strftime_putc(output, '\t'); break;
        case 'T': crt_strftime_format(output, "%H:%M:%S", tm); break;
        case 'u':
            crt_strftime_number(output,
                (unsigned int)(tm->tm_wday == 0 ? 7 : tm->tm_wday),
                1, '0', alternate);
            break;
        case 'U':
            crt_strftime_number(output,
                (unsigned int)((tm->tm_yday + 7 - tm->tm_wday) / 7),
                2, '0', alternate);
            break;
        case 'w':
            crt_strftime_number(output, (unsigned int)tm->tm_wday, 1, '0',
                                alternate);
            break;
        case 'W': {
            int monday_wday = (tm->tm_wday + 6) % 7;
            crt_strftime_number(output,
                (unsigned int)((tm->tm_yday + 7 - monday_wday) / 7),
                2, '0', alternate);
            break;
        }
        case 'x': crt_strftime_format(output, "%m/%d/%y", tm); break;
        case 'X': crt_strftime_format(output, "%H:%M:%S", tm); break;
        case 'y':
            crt_strftime_number(output,
                (unsigned int)((year % 100 + 100) % 100), 2, '0', alternate);
            break;
        case 'Y':
            crt_strftime_number(output, (unsigned int)year, 4, '0', alternate);
            break;
        case 'z': crt_strftime_puts(output, "+0000"); break;
        case 'Z': crt_strftime_puts(output, "UTC"); break;
        case 0:
            crt_strftime_putc(output, '%');
            break;
        default:
            crt_strftime_putc(output, '%');
            crt_strftime_putc(output, specifier);
            break;
        }
    }
}

SIZE_T WINAPI crt_strftime(char *buffer, SIZE_T max_size,
                           const char *format, PCVOID tm_ptr)
{
    if (!buffer || !max_size || !format || !tm_ptr) {
        if (buffer && max_size) buffer[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return 0;
    }

    CRT_STRFTIME_OUTPUT output = { buffer, max_size, 0, FALSE };
    buffer[0] = 0;
    crt_strftime_format(&output, format, (const struct crt_tm *)tm_ptr);
    if (output.overflow) {
        buffer[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return 0;
    }
    *crt_errno() = 0;
    return output.length;
}

void WINAPI crt_sleep(unsigned long milliseconds)
{
    Sleep((DWORD)milliseconds);
}

crt_time_t WINAPI crt_mktime(PVOID tm_ptr)
{
    struct crt_tm *tm = (struct crt_tm *)tm_ptr;
    if (!tm) {
        *crt_errno() = 22;
        return (crt_time_t)-1;
    }
    WINTIME_CALENDAR calendar;
    calendar.year = (uint16_t)(tm->tm_year + 1900);
    calendar.month = (uint16_t)(tm->tm_mon + 1);
    calendar.day_of_week = 0;
    calendar.day = (uint16_t)tm->tm_mday;
    calendar.hour = (uint16_t)tm->tm_hour;
    calendar.minute = (uint16_t)tm->tm_min;
    calendar.second = (uint16_t)tm->tm_sec;
    calendar.millisecond = 0;
    calendar.day_of_year = 0;
    int64_t unix_seconds;
    if (wintime_calendar_to_unix(&calendar, &unix_seconds) < 0) {
        *crt_errno() = 22;
        return (crt_time_t)-1;
    }

    WINTIME_CALENDAR normalized;
    if (wintime_unix_to_calendar(unix_seconds, &normalized) == 0) {
        tm->tm_wday = normalized.day_of_week;
        tm->tm_yday = normalized.day_of_year;
        tm->tm_isdst = 0;
    }
    *crt_errno() = 0;
    return (crt_time_t)unix_seconds;
}

int64_t WINAPI crt_mktime64(PVOID tm_ptr)
{
    return (int64_t)crt_mktime(tm_ptr);
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

char* WINAPI crt_strdup(const char *s)
{
    static volatile uint32_t strdup_trace_count;
    if (!s) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    SIZE_T chars = crt_strlen(s) + 1;
    char *copy = (char *)crt_malloc(chars);
    if (!copy) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    for (SIZE_T i = 0; i < chars; i++) copy[i] = s[i];
    if (__sync_fetch_and_add(&strdup_trace_count, 1) < 4) {
        serial_puts("[CRT-STRDUP] -> 0x");
        serial_puthex((ULONG_PTR)copy, 16);
        serial_puts(" value='");
        serial_puts(s);
        serial_puts("'\n");
    }
    return copy;
}

SIZE_T WINAPI crt_strcspn(const char *s, const char *reject)
{
    SIZE_T length = 0;
    while (s[length]) {
        for (const char *r = reject; *r; r++)
            if (s[length] == *r) return length;
        length++;
    }
    return length;
}

char* WINAPI crt_strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return NULL;
}

SIZE_T WINAPI crt_wcslen(const WCHAR *s)
{
    SIZE_T len = 0;
    while (s[len]) len++;
    return len;
}

SIZE_T WINAPI crt_wcsnlen(const WCHAR *s, SIZE_T max_chars)
{
    if (!s) {
        *crt_errno() = CRT_EINVAL;
        return 0;
    }
    SIZE_T len = 0;
    while (len < max_chars && s[len]) len++;
    return len;
}

WCHAR* WINAPI crt_wcsdup(const WCHAR *s)
{
    if (!s) return NULL;
    SIZE_T chars = crt_wcslen(s) + 1;
    WCHAR *copy = (WCHAR *)crt_malloc(chars * sizeof(WCHAR));
    if (!copy) return NULL;
    for (SIZE_T i = 0; i < chars; i++)
        copy[i] = s[i];
    return copy;
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

int WINAPI crt_wcscpy_s(WCHAR *dst, SIZE_T dst_chars, const WCHAR *src)
{
    if (!dst) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!src) {
        if (dst_chars) dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!dst_chars) {
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }

    SIZE_T src_chars = crt_wcslen(src);
    if (src_chars >= dst_chars) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }
    for (SIZE_T i = 0; i <= src_chars; i++) dst[i] = src[i];
    return 0;
}

int WINAPI crt_strncpy_s(char *dst, SIZE_T dst_chars,
                         const char *src, SIZE_T count)
{
    if (!dst || !dst_chars) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (count == 0) {
        dst[0] = 0;
        return 0;
    }
    if (!src) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    BOOL truncate = crt_secure_truncate_count(count);
    SIZE_T src_chars = crt_strlen(src);
    SIZE_T copy_chars = truncate || src_chars < count ? src_chars : count;

    if (truncate && src_chars >= dst_chars) {
        copy_chars = dst_chars - 1;
        for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
        dst[copy_chars] = 0;
        return CRT_STRUNCATE;
    }
    if (copy_chars >= dst_chars) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }

    ULONG_PTR dst_begin = (ULONG_PTR)dst;
    ULONG_PTR src_begin = (ULONG_PTR)src;
    SIZE_T span = copy_chars + 1;
    if (dst_begin < src_begin + span && src_begin < dst_begin + span) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
    dst[copy_chars] = 0;
    return 0;
}

int WINAPI crt_wcsncpy_s(WCHAR *dst, SIZE_T dst_chars,
                         const WCHAR *src, SIZE_T count)
{
    if (!dst || !dst_chars) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!src) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    BOOL truncate = count == (SIZE_T)-1 || count == 0xFFFFFFFFU;
    SIZE_T src_chars = crt_wcslen(src);
    SIZE_T copy_chars = src_chars < count ? src_chars : count;
    if (truncate && src_chars >= dst_chars) {
        copy_chars = dst_chars - 1;
        for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
        dst[copy_chars] = 0;
        return CRT_STRUNCATE;
    }
    if (copy_chars >= dst_chars) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }
    for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
    dst[copy_chars] = 0;
    return 0;
}

WCHAR* WINAPI crt_wcscat(WCHAR *dst, const WCHAR *src)
{
    WCHAR *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

int WINAPI crt_wcscat_s(WCHAR *dst, SIZE_T dst_chars, const WCHAR *src)
{
    if (!dst) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!dst_chars) {
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }

    SIZE_T dst_len = crt_wcsnlen(dst, dst_chars);
    if (dst_len == dst_chars) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!src) {
        dst[0] = 0;
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }

    SIZE_T src_len = crt_wcslen(src);
    if (src_len >= dst_chars - dst_len) {
        dst[0] = 0;
        *crt_errno() = CRT_ERANGE;
        return CRT_ERANGE;
    }
    for (SIZE_T i = 0; i <= src_len; i++) dst[dst_len + i] = src[i];
    return 0;
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

int WINAPI crt_wcscoll(const WCHAR *a, const WCHAR *b)
{
    /* OsitoK currently exposes the invariant C locale. */
    return crt_wcscmp(a, b);
}

SIZE_T WINAPI crt_wcsxfrm(WCHAR *dst, const WCHAR *src, SIZE_T dst_chars)
{
    if (!src) {
        *crt_errno() = CRT_EINVAL;
        return (SIZE_T)-1;
    }
    SIZE_T src_chars = crt_wcslen(src);
    if (dst && dst_chars) {
        SIZE_T copy_chars = src_chars < dst_chars ? src_chars : dst_chars;
        for (SIZE_T i = 0; i < copy_chars; i++) dst[i] = src[i];
        if (src_chars < dst_chars) dst[src_chars] = 0;
    }
    return src_chars;
}

WCHAR* WINAPI crt_wcschr(const WCHAR *s, WCHAR c)
{
    for (; *s; s++)
        if (*s == c) return (WCHAR *)s;
    return (c == 0) ? (WCHAR *)s : NULL;
}

WCHAR* WINAPI crt_wcsrchr(const WCHAR *s, WCHAR c)
{
    const WCHAR *last = NULL;
    do {
        if (*s == c)
            last = s;
    } while (*s++);
    return (WCHAR *)last;
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

static BOOL crt_wchar_is_delimiter(WCHAR c, const WCHAR *delimiters)
{
    for (const WCHAR *d = delimiters; *d; d++)
        if (*d == c) return TRUE;
    return FALSE;
}

WCHAR* WINAPI crt_wcstok_s(WCHAR *str, const WCHAR *delimiters,
                           WCHAR **context)
{
    if (!delimiters || !context || (!str && !*context)) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    WCHAR *cursor = str ? str : *context;
    while (*cursor && crt_wchar_is_delimiter(*cursor, delimiters)) cursor++;
    if (!*cursor) {
        *context = cursor;
        return NULL;
    }

    WCHAR *token = cursor;
    while (*cursor && !crt_wchar_is_delimiter(*cursor, delimiters)) cursor++;
    if (*cursor) *cursor++ = 0;
    *context = cursor;
    return token;
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

/* _access — check file accessibility (0=exist, 2=write, 4=read, 6=r+w) */
int WINAPI crt_access(const char *path, int mode)
{
    (void)mode;
    if (!path) return -1;
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    void *f = osfs2_find(base);
    if (!f && base != path) f = osfs2_find(path);
    return f ? 0 : -1;
}

int WINAPI crt_waccess(const WCHAR *path, int mode)
{
    if (!path) return -1;
    char narrow[260];
    int i = 0;
    for (; path[i] && i < 259; i++)
        narrow[i] = (char)(path[i] & 0xFF);
    narrow[i] = 0;
    return crt_access(narrow, mode);
}

/* _fltused — compiler marker for floating point usage */
static int crt_fltused_val = 0x9875;
int* WINAPI crt_fltused(void) { return &crt_fltused_val; }

/* CRT version globals (Windows NT 5.0 = Windows 2000) */
static unsigned int crt_osver_val = 2195;
static unsigned int crt_winver_val = 0x0500;
static unsigned int crt_winmajor_val = 5;
static unsigned int crt_winminor_val = 0;
unsigned int* WINAPI crt_p_osver(void) { return &crt_osver_val; }
unsigned int* WINAPI crt_p_winver(void) { return &crt_winver_val; }
unsigned int* WINAPI crt_p_winmajor(void) { return &crt_winmajor_val; }
unsigned int* WINAPI crt_p_winminor(void) { return &crt_winminor_val; }

/* ── Stubs for bundled MSVCRT.dll ────────────────────────────── */

static int crt_getch_stub(void)  { return -1; /* EOF */ }
static int crt_kbhit_stub(void)  { return 0;  /* no key pressed */ }

typedef struct _CRT_ENV_VALUE_CACHE {
    struct _CRT_ENV_VALUE_CACHE *next;
    DWORD pid;
    DWORD tid;
    char *narrow;
    SIZE_T narrow_capacity;
    WCHAR *wide;
    SIZE_T wide_capacity;
} CRT_ENV_VALUE_CACHE;

typedef struct _CRT_WENV_CACHE {
    struct _CRT_WENV_CACHE *next;
    DWORD pid;
    BOOL is_32bit;
    char *narrow_block;
    SIZE_T narrow_block_capacity;
    PVOID narrow_entries;
    SIZE_T narrow_entry_capacity;
    PVOID narrow_view_cell;
    WCHAR *block;
    SIZE_T block_capacity;
    PVOID entries;
    SIZE_T entry_capacity;
    PVOID view_cell;
} CRT_WENV_CACHE;

static CRT_ENV_VALUE_CACHE *crt_env_values;
static CRT_WENV_CACHE *crt_wenv_cache;
static volatile uint32_t crt_env_lock;

static void crt_env_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&crt_env_lock, 1)) {
        for (int spin = 0; spin < 100; spin++)
            __asm__ volatile ("pause" ::: "memory");
        sched_yield();
    }
}

static void crt_env_lock_release(void)
{
    __sync_lock_release(&crt_env_lock);
}

static CRT_WENV_CACHE *crt_env_array_cache_locked(DWORD pid, BOOL is_32bit)
{
    for (CRT_WENV_CACHE *cache = crt_wenv_cache; cache;
         cache = cache->next) {
        if (cache->pid == pid)
            return cache->is_32bit == is_32bit ? cache : NULL;
    }

    CRT_WENV_CACHE *cache = (CRT_WENV_CACHE *)kmalloc(sizeof(*cache));
    if (!cache) return NULL;
    memset(cache, 0, sizeof(*cache));
    cache->pid = pid;
    cache->is_32bit = is_32bit;
    cache->next = crt_wenv_cache;
    crt_wenv_cache = cache;
    return cache;
}

static CRT_ENV_VALUE_CACHE *crt_env_value_cache(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    crt_env_lock_acquire();
    for (CRT_ENV_VALUE_CACHE *cache = crt_env_values; cache;
         cache = cache->next) {
        if (cache->pid == pid && cache->tid == tid) {
            crt_env_lock_release();
            return cache;
        }
    }

    CRT_ENV_VALUE_CACHE *cache = (CRT_ENV_VALUE_CACHE *)kmalloc(sizeof(*cache));
    if (cache) {
        cache->pid = pid;
        cache->tid = tid;
        cache->narrow = NULL;
        cache->narrow_capacity = 0;
        cache->wide = NULL;
        cache->wide_capacity = 0;
        cache->next = crt_env_values;
        crt_env_values = cache;
    }
    crt_env_lock_release();
    return cache;
}

static PVOID crt_env_user_pointer(PVOID buffer)
{
    if (!buffer || !g_compat32_mode) return buffer;
    ULONG_PTR address = (ULONG_PTR)buffer;
    if (address >= KERNEL_VBASE)
        return (PVOID)(ULONG_PTR)VIRT_TO_PHYS(buffer);
    return buffer;
}

static BOOL crt_env_user_buffer(PVOID buffer, SIZE_T bytes)
{
    if (!buffer) return FALSE;
    if (!g_compat32_mode) return TRUE;

    ULONG_PTR address = (ULONG_PTR)crt_env_user_pointer(buffer);
    return address <= UINT32_MAX && bytes - 1 <= UINT32_MAX - address;
}

char* WINAPI crt_getenv(const char *name)
{
    if (!name || !*name) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    DWORD required = GetEnvironmentVariableA(name, NULL, 0);
    if (!required) return NULL;

    CRT_ENV_VALUE_CACHE *cache = crt_env_value_cache();
    if (!cache) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    if (required > cache->narrow_capacity) {
        char *buffer = (char *)crt_realloc(cache->narrow, required);
        if (!crt_env_user_buffer(buffer, required)) {
            if (buffer) crt_free(buffer);
            cache->narrow = NULL;
            cache->narrow_capacity = 0;
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->narrow = buffer;
        cache->narrow_capacity = required;
    }
    GetEnvironmentVariableA(name, cache->narrow,
                            (DWORD)cache->narrow_capacity);
    *crt_errno() = 0;
    return cache->narrow;
}

WCHAR* WINAPI crt_wgetenv(const WCHAR *name)
{
    if (!name || !*name) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    DWORD required = GetEnvironmentVariableW(name, NULL, 0);
    if (!required) return NULL;

    CRT_ENV_VALUE_CACHE *cache = crt_env_value_cache();
    if (!cache) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    SIZE_T bytes = (SIZE_T)required * sizeof(WCHAR);
    if (required > cache->wide_capacity) {
        WCHAR *buffer = (WCHAR *)crt_realloc(cache->wide, bytes);
        if (!crt_env_user_buffer(buffer, bytes)) {
            if (buffer) crt_free(buffer);
            cache->wide = NULL;
            cache->wide_capacity = 0;
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->wide = buffer;
        cache->wide_capacity = required;
    }
    GetEnvironmentVariableW(name, cache->wide,
                            (DWORD)cache->wide_capacity);
    *crt_errno() = 0;
    return cache->wide;
}

int WINAPI crt_putenv_s(const char *name, const char *value)
{
    if (!name || !*name || !value) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!SetEnvironmentVariableA(name, *value ? value : NULL)) {
        int error = crt_errno_from_last_error();
        *crt_errno() = error;
        return error;
    }
    if (!crt_p_environ() || !crt_p_wenviron()) {
        *crt_errno() = CRT_ENOMEM;
        return CRT_ENOMEM;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_wputenv_s(const WCHAR *name, const WCHAR *value)
{
    if (!name || !*name || !value) {
        *crt_errno() = CRT_EINVAL;
        return CRT_EINVAL;
    }
    if (!SetEnvironmentVariableW(name, *value ? value : NULL)) {
        int error = crt_errno_from_last_error();
        *crt_errno() = error;
        return error;
    }
    if (!crt_p_environ() || !crt_p_wenviron()) {
        *crt_errno() = CRT_ENOMEM;
        return CRT_ENOMEM;
    }
    *crt_errno() = 0;
    return 0;
}

int WINAPI crt_putenv(const char *assignment)
{
    if (!assignment) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    SIZE_T separator = assignment[0] == '=' ? 1 : 0;
    while (assignment[separator] && assignment[separator] != '=') separator++;
    if (!separator || !assignment[separator] || separator >= 64) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    char name[64];
    for (SIZE_T i = 0; i < separator; i++) name[i] = assignment[i];
    name[separator] = 0;
    return crt_putenv_s(name, assignment + separator + 1) ? -1 : 0;
}

int WINAPI crt_wputenv(const WCHAR *assignment)
{
    if (!assignment) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    SIZE_T separator = assignment[0] == '=' ? 1 : 0;
    while (assignment[separator] && assignment[separator] != '=') separator++;
    if (!separator || !assignment[separator] || separator >= 64) {
        *crt_errno() = CRT_EINVAL;
        return -1;
    }
    WCHAR name[64];
    for (SIZE_T i = 0; i < separator; i++) name[i] = assignment[i];
    name[separator] = 0;
    return crt_wputenv_s(name, assignment + separator + 1) ? -1 : 0;
}

char*** WINAPI crt_p_environ(void)
{
    DWORD pid = GetCurrentProcessId();
    BOOL is_32bit = g_compat32_mode;
    crt_env_lock_acquire();
    CRT_WENV_CACHE *cache = crt_env_array_cache_locked(pid, is_32bit);
    if (!cache) {
        crt_env_lock_release();
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }

    SIZE_T chars = kernel32_build_environment_block_w(pid, NULL, 0);
    if (chars < 2) chars = 2;
    WCHAR *source = (WCHAR *)kmalloc(chars * sizeof(WCHAR));
    if (!source) {
        crt_env_lock_release();
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    SIZE_T actual = kernel32_build_environment_block_w(pid, source, chars);
    if (actual > chars) {
        kfree(source);
        crt_env_lock_release();
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }

    if (chars > cache->narrow_block_capacity) {
        char *block = (char *)crt_realloc(cache->narrow_block, chars);
        if (!crt_env_user_buffer(block, chars)) {
            if (block) crt_free(block);
            cache->narrow_block = NULL;
            cache->narrow_block_capacity = 0;
            kfree(source);
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->narrow_block = block;
        cache->narrow_block_capacity = chars;
    }
    for (SIZE_T i = 0; i < chars; i++)
        cache->narrow_block[i] = (char)(source[i] & 0xFF);
    kfree(source);

    SIZE_T entries_capacity = chars + 1;
    if (entries_capacity > cache->narrow_entry_capacity) {
        SIZE_T entry_size = is_32bit ? sizeof(uint32_t) : sizeof(char *);
        SIZE_T entries_bytes = entries_capacity * entry_size;
        PVOID entries = crt_realloc(cache->narrow_entries, entries_bytes);
        if (!crt_env_user_buffer(entries, entries_bytes)) {
            if (entries) crt_free(entries);
            cache->narrow_entries = NULL;
            cache->narrow_entry_capacity = 0;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->narrow_entries = entries;
        cache->narrow_entry_capacity = entries_capacity;
    }
    if (!cache->narrow_view_cell) {
        SIZE_T cell_size = is_32bit ? sizeof(uint32_t) : sizeof(char **);
        cache->narrow_view_cell = crt_malloc(cell_size);
        if (!crt_env_user_buffer(cache->narrow_view_cell, cell_size)) {
            if (cache->narrow_view_cell) crt_free(cache->narrow_view_cell);
            cache->narrow_view_cell = NULL;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    SIZE_T offset = 0;
    SIZE_T count = 0;
    while (offset < chars && cache->narrow_block[offset]) {
        if (is_32bit)
            ((uint32_t *)cache->narrow_entries)[count++] =
                (uint32_t)(ULONG_PTR)(cache->narrow_block + offset);
        else
            ((char **)cache->narrow_entries)[count++] =
                cache->narrow_block + offset;
        while (offset < chars && cache->narrow_block[offset]) offset++;
        offset++;
    }
    if (is_32bit) {
        ((uint32_t *)cache->narrow_entries)[count] = 0;
        *(uint32_t *)cache->narrow_view_cell =
            (uint32_t)(ULONG_PTR)cache->narrow_entries;
    } else {
        ((char **)cache->narrow_entries)[count] = NULL;
        *(char ***)cache->narrow_view_cell = (char **)cache->narrow_entries;
    }
    char ***result = (char ***)crt_env_user_pointer(cache->narrow_view_cell);
    crt_env_lock_release();
    *crt_errno() = 0;
    return result;
}

WCHAR*** WINAPI crt_p_wenviron(void)
{
    DWORD pid = GetCurrentProcessId();
    BOOL is_32bit = g_compat32_mode;

    crt_env_lock_acquire();
    CRT_WENV_CACHE *cache = crt_env_array_cache_locked(pid, is_32bit);
    if (!cache) {
        crt_env_lock_release();
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }

    SIZE_T chars = kernel32_build_environment_block_w(pid, NULL, 0);
    if (chars < 2) chars = 2;
    SIZE_T entries_capacity = chars + 1;
    if (chars > cache->block_capacity) {
        WCHAR *block = (WCHAR *)crt_realloc(cache->block,
                                             chars * sizeof(WCHAR));
        if (!crt_env_user_buffer(block, chars * sizeof(WCHAR))) {
            if (block) crt_free(block);
            cache->block = NULL;
            cache->block_capacity = 0;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->block = block;
        cache->block_capacity = chars;
    }
    if (entries_capacity > cache->entry_capacity) {
        SIZE_T entry_size = is_32bit ? sizeof(uint32_t) : sizeof(WCHAR *);
        SIZE_T entries_bytes = entries_capacity * entry_size;
        PVOID entries = crt_realloc(cache->entries, entries_bytes);
        if (!crt_env_user_buffer(entries, entries_bytes)) {
            if (entries) crt_free(entries);
            cache->entries = NULL;
            cache->entry_capacity = 0;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
        cache->entries = entries;
        cache->entry_capacity = entries_capacity;
    }
    if (!cache->view_cell) {
        SIZE_T cell_size = is_32bit ? sizeof(uint32_t) : sizeof(WCHAR **);
        cache->view_cell = crt_malloc(cell_size);
        if (!crt_env_user_buffer(cache->view_cell, cell_size)) {
            if (cache->view_cell) crt_free(cache->view_cell);
            cache->view_cell = NULL;
            crt_env_lock_release();
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    kernel32_build_environment_block_w(pid, cache->block,
                                        cache->block_capacity);
    SIZE_T offset = 0;
    SIZE_T count = 0;
    while (offset < chars && cache->block[offset]) {
        if (is_32bit)
            ((uint32_t *)cache->entries)[count++] =
                (uint32_t)(ULONG_PTR)(cache->block + offset);
        else
            ((WCHAR **)cache->entries)[count++] = cache->block + offset;
        while (offset < chars && cache->block[offset]) offset++;
        offset++;
    }
    if (is_32bit) {
        ((uint32_t *)cache->entries)[count] = 0;
        *(uint32_t *)cache->view_cell = (uint32_t)(ULONG_PTR)cache->entries;
    } else {
        ((WCHAR **)cache->entries)[count] = NULL;
        *(WCHAR ***)cache->view_cell = (WCHAR **)cache->entries;
    }
    WCHAR ***result = (WCHAR ***)crt_env_user_pointer(cache->view_cell);
    crt_env_lock_release();
    *crt_errno() = 0;
    return result;
}

static void crt_env_release_process(DWORD process_id)
{
    CRT_ENV_VALUE_CACHE *released_values = NULL;
    CRT_WENV_CACHE *released_wenv = NULL;

    crt_env_lock_acquire();
    CRT_ENV_VALUE_CACHE **value_link = &crt_env_values;
    while (*value_link) {
        CRT_ENV_VALUE_CACHE *cache = *value_link;
        if (cache->pid != process_id) {
            value_link = &cache->next;
            continue;
        }
        *value_link = cache->next;
        cache->next = released_values;
        released_values = cache;
    }

    CRT_WENV_CACHE **wenv_link = &crt_wenv_cache;
    while (*wenv_link) {
        CRT_WENV_CACHE *cache = *wenv_link;
        if (cache->pid != process_id) {
            wenv_link = &cache->next;
            continue;
        }
        *wenv_link = cache->next;
        cache->next = released_wenv;
        released_wenv = cache;
    }
    crt_env_lock_release();

    /* User buffers belong to the process VM and are released with it. Only
     * the kernel-side cache metadata needs explicit reclamation here. */
    while (released_values) {
        CRT_ENV_VALUE_CACHE *next = released_values->next;
        kfree(released_values);
        released_values = next;
    }
    while (released_wenv) {
        CRT_WENV_CACHE *next = released_wenv->next;
        kfree(released_wenv);
        released_wenv = next;
    }
}

char* WINAPI crt_getcwd(char *buffer, int max_length)
{
    if (max_length <= 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    BOOL allocated = buffer == NULL;
    if (allocated) buffer = (char *)crt_malloc((SIZE_T)max_length);
    if (!buffer) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    DWORD length = GetCurrentDirectoryA((DWORD)max_length, buffer);
    if (!length || length >= (DWORD)max_length) {
        if (allocated) crt_free(buffer);
        *crt_errno() = CRT_ERANGE;
        return NULL;
    }
    *crt_errno() = 0;
    return buffer;
}

WCHAR* WINAPI crt_wgetcwd(WCHAR *buffer, int max_length)
{
    if (max_length <= 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }
    BOOL allocated = buffer == NULL;
    if (allocated)
        buffer = (WCHAR *)crt_malloc((SIZE_T)max_length * sizeof(WCHAR));
    if (!buffer) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    DWORD length = GetCurrentDirectoryW((DWORD)max_length, buffer);
    if (!length || length >= (DWORD)max_length) {
        if (allocated) crt_free(buffer);
        *crt_errno() = CRT_ERANGE;
        return NULL;
    }
    *crt_errno() = 0;
    return buffer;
}

static BOOL crt_drive_current_directory(int drive, char directory[260],
                                        SIZE_T *length_out)
{
    char current[260];
    DWORD current_length = GetCurrentDirectoryA(sizeof(current), current);
    if (!current_length || current_length >= sizeof(current) ||
        current[1] != ':') {
        *crt_errno() = CRT_EINVAL;
        return FALSE;
    }

    int current_drive = crt_toupper((unsigned char)current[0]) - 'A' + 1;
    int selected_drive = drive ? drive : current_drive;
    if (selected_drive < 1 || selected_drive > 26 ||
        !(GetLogicalDrives() & (1U << (selected_drive - 1)))) {
        *crt_errno() = CRT_EINVAL;
        return FALSE;
    }

    SIZE_T length;
    if (selected_drive == current_drive) {
        memcpy(directory, current, (SIZE_T)current_length + 1);
        length = current_length;
    } else {
        char drive_variable[4] = {
            '=', (char)('A' + selected_drive - 1), ':', 0
        };
        DWORD saved_length =
            GetEnvironmentVariableA(drive_variable, directory, 260);
        if (saved_length >= 260) {
            *crt_errno() = CRT_ENAMETOOLONG;
            return FALSE;
        }
        if (saved_length) {
            length = saved_length;
        } else {
            directory[0] = drive_variable[1];
            directory[1] = ':';
            directory[2] = '\\';
            directory[3] = 0;
            length = 3;
        }
    }

    *length_out = length;
    return TRUE;
}

char* WINAPI crt_getdcwd(int drive, char *buffer, int max_length)
{
    if (max_length <= 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    char directory[260];
    SIZE_T length;
    if (!crt_drive_current_directory(drive, directory, &length))
        return NULL;
    if (length + 1 > (SIZE_T)max_length) {
        *crt_errno() = CRT_ERANGE;
        return NULL;
    }

    BOOL allocated = buffer == NULL;
    if (allocated)
        buffer = (char *)crt_malloc((SIZE_T)max_length);
    if (!buffer) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    memcpy(buffer, directory, length + 1);
    *crt_errno() = 0;
    return buffer;
}

WCHAR* WINAPI crt_wgetdcwd(int drive, WCHAR *buffer, int max_length)
{
    if (max_length <= 0) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    char directory[260];
    SIZE_T length;
    if (!crt_drive_current_directory(drive, directory, &length))
        return NULL;
    if (length + 1 > (SIZE_T)max_length) {
        *crt_errno() = CRT_ERANGE;
        return NULL;
    }

    BOOL allocated = buffer == NULL;
    if (allocated) {
        buffer = (WCHAR *)crt_malloc(
            (SIZE_T)max_length * sizeof(WCHAR));
    }
    if (!buffer) {
        *crt_errno() = CRT_ENOMEM;
        return NULL;
    }
    for (SIZE_T index = 0; index <= length; index++)
        buffer[index] = (WCHAR)(unsigned char)directory[index];
    *crt_errno() = 0;
    return buffer;
}

/* Path resolution */

char* WINAPI crt_fullpath(char *absolute, const char *relative,
                          SIZE_T max_length)
{
    char resolved[260];
    if (!relative) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    DWORD length = GetFullPathNameA(relative, sizeof(resolved), resolved, NULL);
    if (!length || length >= sizeof(resolved)) {
        *crt_errno() = length ? CRT_ENAMETOOLONG : CRT_EINVAL;
        return NULL;
    }

    BOOL allocated = absolute == NULL;
    if (!allocated && (max_length == 0 || (SIZE_T)length >= max_length)) {
        *crt_errno() = max_length ? CRT_ERANGE : CRT_EINVAL;
        return NULL;
    }
    if (allocated) {
        absolute = (char *)crt_malloc((SIZE_T)length + 1);
        if (!absolute) {
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    memcpy(absolute, resolved, (SIZE_T)length + 1);
    *crt_errno() = 0;
    return absolute;
}

WCHAR* WINAPI crt_wfullpath(WCHAR *absolute, const WCHAR *relative,
                            SIZE_T max_length)
{
    WCHAR resolved[260];
    if (!relative) {
        *crt_errno() = CRT_EINVAL;
        return NULL;
    }

    DWORD length = GetFullPathNameW(relative, 260, resolved, NULL);
    if (!length || length >= 260) {
        *crt_errno() = length ? CRT_ENAMETOOLONG : CRT_EINVAL;
        return NULL;
    }

    BOOL allocated = absolute == NULL;
    if (!allocated && (max_length == 0 || (SIZE_T)length >= max_length)) {
        *crt_errno() = max_length ? CRT_ERANGE : CRT_EINVAL;
        return NULL;
    }
    if (allocated) {
        absolute = (WCHAR *)crt_malloc(((SIZE_T)length + 1) * sizeof(WCHAR));
        if (!absolute) {
            *crt_errno() = CRT_ENOMEM;
            return NULL;
        }
    }

    memcpy(absolute, resolved, ((SIZE_T)length + 1) * sizeof(WCHAR));
    *crt_errno() = 0;
    return absolute;
}

/* ── Export resolution table ───────────────────────────────── */

typedef struct {
    const char *name;
    PVOID       func;
    uint8_t     argc;
    uint8_t     cc;
} MSVCRT_EXPORT;

static const MSVCRT_EXPORT msvcrt_exports[] = {
    /* CRT init */
    { "_initterm",           (PVOID)_initterm,        2, CC_CDECL },
    { "_initterm_e",         (PVOID)_initterm_e,      2, CC_CDECL },
    { "__getmainargs",       (PVOID)__getmainargs,    5, CC_CDECL },
    { "__wgetmainargs",      (PVOID)__wgetmainargs,   5, CC_CDECL },
    { "__crtGetShowWindowMode", (PVOID)__crtGetShowWindowMode, 0, CC_CDECL },
    { "__crtSetUnhandledExceptionFilter",
                            (PVOID)SetUnhandledExceptionFilter, 1, CC_CDECL },
    { "__set_app_type",      (PVOID)__set_app_type,   1, CC_CDECL },
    { "_set_app_type",       (PVOID)__set_app_type,   1, CC_CDECL },
    { "_set_fmode",          (PVOID)crt_set_fmode,    1, CC_CDECL },
    { "_get_fmode",          (PVOID)crt_get_fmode,    1, CC_CDECL },
    { "_set_new_mode",       (PVOID)_set_new_mode,    1, CC_CDECL },
    { "?_set_new_mode@@YAHH@Z", (PVOID)_set_new_mode, 1, CC_CDECL },
    { "?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z",
                              (PVOID)crt_set_new_handler, 1, CC_CDECL },

    /* Memory */
    { "malloc",              (PVOID)crt_malloc,       1, CC_CDECL },
    { "_malloc_crt",         (PVOID)crt_malloc,       1, CC_CDECL },
    { "calloc",              (PVOID)crt_calloc,       2, CC_CDECL },
    { "realloc",             (PVOID)crt_realloc,      2, CC_CDECL },
    { "free",                (PVOID)crt_free,         1, CC_CDECL },
    { "_msize",              (PVOID)crt_msize,        1, CC_CDECL },
    { "?malloc@@YAPEAX_K@Z", (PVOID)crt_malloc,       1, CC_CDECL },  /* C++ mangled (64-bit) */
    { "?free@@YAXPEAX@Z",   (PVOID)crt_free,          1, CC_CDECL },
    /* MSVC 32-bit operator new/delete — used by C++ code via MSVCRT */
    { "??2@YAPAXI@Z",       (PVOID)crt_malloc,        1, CC_CDECL },  /* operator new(unsigned int) */
    { "??3@YAXPAX@Z",       (PVOID)crt_free,          1, CC_CDECL },  /* operator delete(void*) */
    { "??_U@YAPAXI@Z",      (PVOID)crt_malloc,        1, CC_CDECL },  /* operator new[](unsigned int) */
    { "??_V@YAXPAX@Z",      (PVOID)crt_free,          1, CC_CDECL },  /* operator delete[](void*) */

    /* String */
    { "strlen",              (PVOID)crt_strlen,       1, CC_CDECL },
    { "strcmp",              (PVOID)crt_strcmp,       2, CC_CDECL },
    { "strncmp",             (PVOID)crt_strncmp,      3, CC_CDECL },
    { "_stricmp",            (PVOID)crt_stricmp,      2, CC_CDECL },
    { "_strnicmp",           (PVOID)crt_strnicmp,     3, CC_CDECL },
    { "_strcmpi",            (PVOID)crt_stricmp,      2, CC_CDECL },
    { "strcpy",              (PVOID)crt_strcpy,       2, CC_CDECL },
    { "strncpy",             (PVOID)crt_strncpy,      3, CC_CDECL },
    { "strncpy_s",           (PVOID)crt_strncpy_s,    4, CC_CDECL },
    { "strcat",              (PVOID)crt_strcat,       2, CC_CDECL },
    { "strstr",              (PVOID)crt_strstr,       2, CC_CDECL },
    { "strchr",              (PVOID)crt_strchr,       2, CC_CDECL },
    { "strrchr",             (PVOID)crt_strrchr,      2, CC_CDECL },
    { "_strdup",             (PVOID)crt_strdup,       1, CC_CDECL },
    { "strcspn",             (PVOID)crt_strcspn,      2, CC_CDECL },
    { "strpbrk",             (PVOID)crt_strpbrk,      2, CC_CDECL },
    { "strtok",              (PVOID)crt_strtok,       2, CC_CDECL },
    { "strtok_s",            (PVOID)crt_strtok_s,     3, CC_CDECL },

    /* Memory ops */
    { "memcpy",              (PVOID)crt_memcpy,       3, CC_CDECL },
    { "memset",              (PVOID)crt_memset,       3, CC_CDECL },
    { "memmove",             (PVOID)crt_memmove,      3, CC_CDECL },
    { "memcmp",              (PVOID)crt_memcmp,       3, CC_CDECL },
    { "memchr",              (PVOID)crt_memchr,       3, CC_CDECL },

    /* Format I/O (variadic → fixed-param count) */
    { "printf",              (PVOID)crt_printf,       1, CC_CDECL | CC_VARIADIC },
    { "sprintf",             (PVOID)crt_sprintf,      2, CC_CDECL | CC_VARIADIC },
    { "_snprintf",           (PVOID)crt_snprintf,     3, CC_CDECL | CC_VARIADIC },
    { "_snwprintf",          (PVOID)crt_snwprintf,    3, CC_CDECL | CC_VARIADIC },
    { "_snprintf_s",         (PVOID)crt_snprintf_s,   4, CC_CDECL | CC_VARIADIC },
    { "_vsnprintf",          (PVOID)crt_vsnprintf,    4, CC_CDECL },
    { "fprintf",             (PVOID)crt_fprintf,      2, CC_CDECL | CC_VARIADIC },
    { "fwprintf",            (PVOID)crt_fwprintf,     2, CC_CDECL | CC_VARIADIC },
    { "vprintf",             (PVOID)crt_vprintf,      2, CC_CDECL },
    { "vsprintf",            (PVOID)crt_vsprintf,     3, CC_CDECL },
    { "vfprintf",            (PVOID)crt_vfprintf,     3, CC_CDECL },
    { "__stdio_common_vsprintf",
                            (PVOID)crt_stdio_common_vsprintf, 6, CC_CDECL },
    { "__stdio_common_vswprintf",
                            (PVOID)crt_stdio_common_vswprintf, 6, CC_CDECL },
    { "sscanf",              (PVOID)crt_sscanf,       2, CC_CDECL | CC_VARIADIC },
    { "sscanf_s",            (PVOID)crt_sscanf,       2, CC_CDECL | CC_VARIADIC },
    { "puts",                (PVOID)crt_puts,         1, CC_CDECL },
    { "putchar",             (PVOID)crt_putchar,      1, CC_CDECL },

    /* stdio FILE* */
    { "fopen",               (PVOID)crt_fopen,        2, CC_CDECL },
    { "_fdopen",             (PVOID)crt_fdopen,       2, CC_CDECL },
    { "_wfopen",             (PVOID)crt_wfopen,       2, CC_CDECL },
    { "fread",               (PVOID)crt_fread,        4, CC_CDECL },
    { "fwrite",              (PVOID)crt_fwrite,       4, CC_CDECL },
    { "fclose",              (PVOID)crt_fclose,       1, CC_CDECL },
    { "fseek",               (PVOID)crt_fseek,        3, CC_CDECL },
    { "ftell",               (PVOID)crt_ftell,        1, CC_CDECL },
    { "fflush",              (PVOID)crt_fflush,       1, CC_CDECL },
    { "feof",                (PVOID)crt_feof,         1, CC_CDECL },
    { "ferror",              (PVOID)crt_ferror,       1, CC_CDECL },
    { "fgetc",               (PVOID)crt_fgetc,        1, CC_CDECL },
    { "getc",                (PVOID)crt_fgetc,        1, CC_CDECL },
    { "fputc",               (PVOID)crt_fputc,        2, CC_CDECL },
    { "fgets",               (PVOID)crt_fgets,        3, CC_CDECL },
    { "fputs",               (PVOID)crt_fputs,        2, CC_CDECL },
    { "ungetc",              (PVOID)crt_ungetc,       2, CC_CDECL },
    { "clearerr",            (PVOID)crt_clearerr,     1, CC_CDECL },
    { "rewind",              (PVOID)crt_rewind,       1, CC_CDECL },
    { "setvbuf",             (PVOID)crt_setvbuf,      4, CC_CDECL },
    { "_fileno",             (PVOID)crt_fileno,       1, CC_CDECL },
    { "fileno",              (PVOID)crt_fileno,       1, CC_CDECL },
    { "__iob_func",          (PVOID)crt_iob_func,     0, CC_CDECL },
    { "__acrt_iob_func",     (PVOID)crt_acrt_iob_func, 1, CC_CDECL },
    WX_DATA_DYNAMIC("_iob"),

    /* Low-level UCRT descriptors. */
    { "_get_osfhandle",      (PVOID)crt_get_osfhandle, 1, CC_CDECL },
    { "_open_osfhandle",     (PVOID)crt_open_osfhandle, 2, CC_CDECL },
    { "_open",               (PVOID)crt_open,          3, CC_CDECL },
    { "?_open@@YAHPBDHH@Z",  (PVOID)crt_open,          3, CC_CDECL },
    { "_wopen",              (PVOID)crt_wopen,         3, CC_CDECL },
    { "_unlink",             (PVOID)crt_unlink,        1, CC_CDECL },
    { "_wunlink",            (PVOID)crt_wunlink,       1, CC_CDECL },
    { "unlink",              (PVOID)crt_unlink,        1, CC_CDECL },
    { "remove",              (PVOID)crt_unlink,        1, CC_CDECL },
    { "_wremove",            (PVOID)crt_wunlink,       1, CC_CDECL },
    { "rename",              (PVOID)crt_rename,        2, CC_CDECL },
    { "_wrename",            (PVOID)crt_wrename,       2, CC_CDECL },
    { "_getdrive",           (PVOID)crt_getdrive,      0, CC_CDECL },
    { "_close",              (PVOID)crt_close,         1, CC_CDECL },
    { "_read",               (PVOID)crt_read,          3, CC_CDECL },
    { "_write",              (PVOID)crt_write,         3, CC_CDECL },
    { "_lseek",              (PVOID)crt_lseek,         3, CC_CDECL },
    { "_lseeki64",           (PVOID)crt_lseeki64,      4, CC_CDECL },
    { "_dup",                (PVOID)crt_dup,           1, CC_CDECL },
    { "_dup2",               (PVOID)crt_dup2,          2, CC_CDECL },
    { "_commit",             (PVOID)crt_commit,        1, CC_CDECL },
    { "_isatty",             (PVOID)crt_isatty,        1, CC_CDECL },
    { "_setmode",            (PVOID)crt_setmode,       2, CC_CDECL },
    { "_chsize_s",           (PVOID)crt_chsize_s,      2, CC_CDECL },

    /* Conversion */
    { "atoi",                (PVOID)crt_atoi,         1, CC_CDECL },
    { "atol",                (PVOID)crt_atol,         1, CC_CDECL },
    { "atof",                (PVOID)crt_atof,         1, CC_CDECL },
    { "strtod",              (PVOID)crt_strtod,       2, CC_CDECL },
    { "mbstowcs",            (PVOID)crt_mbstowcs,     3, CC_CDECL },
    { "wcstombs",            (PVOID)crt_wcstombs,     3, CC_CDECL },
    { "abs",                 (PVOID)crt_abs,          1, CC_CDECL },
    { "strtol",              (PVOID)crt_strtol,       3, CC_CDECL },
    { "strtoul",             (PVOID)crt_strtoul,      3, CC_CDECL },

    /* Process */
    { "exit",                (PVOID)crt_exit,         1, CC_CDECL },
    { "abort",               (PVOID)crt_abort,        0, CC_CDECL },
    { "_exit",               (PVOID)crt__exit,        1, CC_CDECL },
    { "_cexit",              (PVOID)crt_exit,         1, CC_CDECL },
    { "_c_exit",             (PVOID)crt__exit,        1, CC_CDECL },
    { "_getpid",             (PVOID)crt_getpid,       0, CC_CDECL },
    { "_beginthreadex",      (PVOID)crt_beginthreadex, 6, CC_CDECL },
    { "_endthreadex",        (PVOID)crt_endthreadex,   1, CC_CDECL },
    { "atexit",              (PVOID)crt_atexit,       1, CC_CDECL },
    { "_crt_atexit",         (PVOID)crt_atexit,       1, CC_CDECL },
    { "_configure_narrow_argv",
                            (PVOID)crt_configure_narrow_argv, 1, CC_CDECL },
    { "_initialize_narrow_environment",
                            (PVOID)crt_initialize_narrow_environment, 0, CC_CDECL },
    { "_get_narrow_winmain_command_line",
                            (PVOID)crt_get_narrow_winmain_command_line,
                            0, CC_CDECL },
    { "getenv",              (PVOID)crt_getenv,       1, CC_CDECL },
    { "_wgetenv",            (PVOID)crt_wgetenv,      1, CC_CDECL },
    { "_putenv",             (PVOID)crt_putenv,       1, CC_CDECL },
    { "_wputenv",            (PVOID)crt_wputenv,      1, CC_CDECL },
    { "_putenv_s",           (PVOID)crt_putenv_s,     2, CC_CDECL },
    { "_wputenv_s",          (PVOID)crt_wputenv_s,    2, CC_CDECL },
    { "__p__environ",        (PVOID)crt_p_environ,    0, CC_CDECL },
    { "__p__wenviron",       (PVOID)crt_p_wenviron,   0, CC_CDECL },
    WX_DATA_DYNAMIC("_environ"),
    WX_DATA_DYNAMIC("_wenviron"),
    { "_initialize_onexit_table",
                            (PVOID)crt_initialize_onexit_table, 1, CC_CDECL },
    { "_register_onexit_function",
                            (PVOID)crt_register_onexit_function, 2, CC_CDECL },
    { "_execute_onexit_table",
                            (PVOID)crt_execute_onexit_table, 1, CC_CDECL },
    { "_set_thread_local_invalid_parameter_handler",
                            (PVOID)crt_set_thread_local_invalid_parameter_handler,
                            1, CC_CDECL },
    { "_get_thread_local_invalid_parameter_handler",
                            (PVOID)crt_get_thread_local_invalid_parameter_handler,
                            0, CC_CDECL },
    { "_set_invalid_parameter_handler",
                            (PVOID)crt_set_invalid_parameter_handler, 1, CC_CDECL },
    { "_get_invalid_parameter_handler",
                            (PVOID)crt_get_invalid_parameter_handler, 0, CC_CDECL },

    /* Locale */
    { "setlocale",           (PVOID)crt_setlocale,    2, CC_CDECL },
    { "_wsetlocale",         (PVOID)crt_wsetlocale,   2, CC_CDECL },
    { "localeconv",          (PVOID)crt_localeconv,   0, CC_CDECL },

    /* ctype */
    { "isalpha",             (PVOID)crt_isalpha,      1, CC_CDECL },
    { "isdigit",             (PVOID)crt_isdigit,      1, CC_CDECL },
    { "isalnum",             (PVOID)crt_isalnum,      1, CC_CDECL },
    { "isspace",             (PVOID)crt_isspace,      1, CC_CDECL },
    { "isupper",             (PVOID)crt_isupper,      1, CC_CDECL },
    { "islower",             (PVOID)crt_islower,      1, CC_CDECL },
    { "isprint",             (PVOID)crt_isprint,      1, CC_CDECL },
    { "toupper",             (PVOID)crt_toupper,      1, CC_CDECL },
    { "tolower",             (PVOID)crt_tolower,      1, CC_CDECL },
    { "iswctype",            (PVOID)crt_iswctype,     2, CC_CDECL },
    { "_iswctype",           (PVOID)crt_iswctype,     2, CC_CDECL },
    { "iswalpha",            (PVOID)crt_iswalpha,     1, CC_CDECL },
    { "iswalnum",            (PVOID)crt_iswalnum,     1, CC_CDECL },
    { "iswdigit",            (PVOID)crt_iswdigit,     1, CC_CDECL },
    { "iswspace",            (PVOID)crt_iswspace,     1, CC_CDECL },
    { "iswupper",            (PVOID)crt_iswupper,     1, CC_CDECL },
    { "iswlower",            (PVOID)crt_iswlower,     1, CC_CDECL },
    { "iswprint",            (PVOID)crt_iswprint,     1, CC_CDECL },
    { "towupper",            (PVOID)crt_towupper,     1, CC_CDECL },
    { "towlower",            (PVOID)crt_towlower,     1, CC_CDECL },

    /* Algorithm */
    { "qsort",               (PVOID)crt_qsort,        4, CC_CDECL },
    { "bsearch",             (PVOID)crt_bsearch,      5, CC_CDECL },

    /* Error */
    { "_errno",              (PVOID)crt_errno,        0, CC_CDECL },
    { "__doserrno",          (PVOID)crt_doserrno,     0, CC_CDECL },
    { "__sys_errlist",       (PVOID)crt_sys_errlist,  0, CC_CDECL },
    { "__sys_nerr",          (PVOID)crt_sys_nerr,     0, CC_CDECL },
    { "strerror",            (PVOID)crt_strerror,     1, CC_CDECL },
    { "_wcserror",           (PVOID)crt_wcserror,     1, CC_CDECL },
    { "__fpe_flt_rounds",    (PVOID)crt_fpe_flt_rounds, 0, CC_CDECL },

    /* Time */
    { "time",                (PVOID)crt_time,         1, CC_CDECL },
    { "_time64",             (PVOID)crt_time64,       1, CC_CDECL },
    { "_ctime64",            (PVOID)crt_ctime64,      1, CC_CDECL },
    { "_ftime",              (PVOID)crt_ftime,        1, CC_CDECL },
    { "_ftime32",            (PVOID)crt_ftime32,      1, CC_CDECL },
    { "_ftime64",            (PVOID)crt_ftime64,      1, CC_CDECL },
    { "clock",               (PVOID)crt_clock,        0, CC_CDECL },
    { "_tzset",              (PVOID)crt_tzset,        0, CC_CDECL },
    { "__timezone",          (PVOID)crt_timezone,     0, CC_CDECL },
    { "__daylight",          (PVOID)crt_daylight,     0, CC_CDECL },
    WX_DATA_DYNAMIC("_timezone"),
    WX_DATA_DYNAMIC("_daylight"),
    { "_gmtime64_s",         (PVOID)crt_gmtime64_s,   2, CC_CDECL },
    { "_localtime64",        (PVOID)crt_localtime64,  1, CC_CDECL },
    { "_localtime64_s",      (PVOID)crt_localtime64_s, 2, CC_CDECL },
    { "_mktime64",           (PVOID)crt_mktime64,     1, CC_CDECL },
    { "strftime",            (PVOID)crt_strftime,     4, CC_CDECL },
    { "_sleep",              (PVOID)crt_sleep,        1, CC_CDECL },

    /* SEH */
    { "_except_handler3",    (PVOID)crt_except_handler3, 4, CC_CDECL },
    { "_except_handler4",    (PVOID)crt_except_handler4, 4, CC_CDECL },
    { "_except_handler4_common", (PVOID)crt_except_handler4_common,
                                                        6, CC_CDECL },
    { "_XcptFilter",         (PVOID)crt_XcptFilter,   2, CC_CDECL },
    { "__CppXcptFilter",     (PVOID)crt_CppXcptFilter, 2, CC_CDECL },
    { "_setjmp",             (PVOID)crt_compat32_setjmp_marker,
                                                        1, CC_CDECL },
    { "_setjmp3",            (PVOID)crt_compat32_setjmp3_marker,
                                                        2, CC_CDECL },
    { "longjmp",             (PVOID)crt_compat32_longjmp_marker,
                                                        2, CC_CDECL },
    { "_longjmpex",          (PVOID)crt_compat32_longjmp_marker,
                                                        2, CC_CDECL },

    /* Misc CRT internal */
    { "_controlfp_s",        (PVOID)crt_controlfp_s,  3, CC_CDECL },
    { "_configthreadlocale", (PVOID)crt_configthreadlocale, 1, CC_CDECL },
    { "_lock",               (PVOID)crt_lock,         1, CC_CDECL },
    { "_unlock",             (PVOID)crt_unlock,       1, CC_CDECL },
    { "__CxxFrameHandler3",  (PVOID)crt_CxxFrameHandler, 4, CC_CDECL },
    { "__CxxFrameHandler4",  (PVOID)crt_except_handler4, 4, CC_CDECL },
    { "_CRT_DEBUGGER_HOOK",  (PVOID)crt_crt_debugger_hook, 1, CC_CDECL },
    { "_crt_debugger_hook",  (PVOID)crt_crt_debugger_hook, 1, CC_CDECL },
    { "_encoded_null",       (PVOID)crt_encoded_null, 0, CC_CDECL },
    { "_amsg_exit",          (PVOID)crt_amsg_exit,    1, CC_CDECL },

    /* C++ EH / UT99 required stubs */
    { "??1type_info@@UAE@XZ", (PVOID)crt_type_info_dtor, 0, CC_THISCALL },
    { "?_type_info_dtor_internal_method@type_info@@QAEXXZ",
                              (PVOID)crt_type_info_dtor_internal, 0, CC_THISCALL },
    { "?_Type_info_dtor@type_info@@CAXPAV1@@Z",
                              (PVOID)crt_type_info_dtor_internal, 1, CC_CDECL },
    { "?_Type_info_dtor_internal@type_info@@CAXPAV1@@Z",
                              (PVOID)crt_type_info_dtor_internal, 1, CC_CDECL },
    { "__clean_type_info_names_internal",
                              (PVOID)crt_clean_type_info_names_internal, 1, CC_CDECL },
    { "__std_type_info_destroy_list", (PVOID)crt_std_type_info_destroy_list, 1, CC_CDECL },
    { "_CxxThrowException",  (PVOID)crt_CxxThrowException, 2, CC_CDECL },
    { "__CxxFrameHandler",   (PVOID)crt_CxxFrameHandler, 4, CC_CDECL },
    { "__dllonexit",         (PVOID)crt_dllonexit,    3, CC_CDECL },
    { "__p__commode",        (PVOID)crt_p_commode,    0, CC_CDECL },
    { "__p__fmode",          (PVOID)crt_p_fmode,      0, CC_CDECL },
    { "__C_specific_handler",(PVOID)crt_C_specific_handler, 4, CC_CDECL },
    WX_DATA_DYNAMIC("__initenv"),
    WX_DATA_DYNAMIC("__mb_cur_max"),
    WX_DATA_DYNAMIC("_commode"),
    WX_DATA_DYNAMIC("_fmode"),
    { "signal",              (PVOID)crt_signal,       2, CC_CDECL },
    { "raise",               (PVOID)crt_raise,        1, CC_CDECL },
    { "__setusermatherr",    (PVOID)crt_setusermatherr, 1, CC_CDECL },
    WX_DATA_DYNAMIC("_acmdln"),
    WX_DATA_DYNAMIC("_adjust_fdiv"),
    { "_controlfp",          (PVOID)crt_controlfp,    2, CC_CDECL },
    { "_control87",          (PVOID)crt_control87,    2, CC_CDECL },
    { "_ftol",               (PVOID)crt_ftol,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "_onexit",             (PVOID)crt_onexit,       1, CC_CDECL },
    { "_purecall",           (PVOID)crt_purecall,     0, CC_CDECL },

    /* UT99 Core.dll / Engine.dll required exports */
    { "?terminate@@YAXXZ",   (PVOID)crt_terminate,    0, CC_CDECL },
    { "_CIacos",             (PVOID)crt_CIacos,       2, CC_CDECL },  /* double = 2 DWORDs */
    { "_CIexp",              (PVOID)crt_CIexp,        0, CC_CDECL },  /* x87 ST(0), direct PE32 target */
    { "_CIlog10",            (PVOID)crt_CIlog10,      0, CC_CDECL },  /* x87 ST(0), direct PE32 target */
    { "_CIsqrt",             (PVOID)crt_CIsqrt,       0, CC_CDECL },  /* x87 ST(0), direct PE32 target */
    { "_CIfmod",             (PVOID)crt_CIfmod,       4, CC_CDECL },  /* 2 doubles = 4 DWORDs */
    { "_CIpow",              (PVOID)crt_CIpow,        4, CC_CDECL },  /* 2 doubles = 4 DWORDs */
    { "_finite",             (PVOID)crt_finite,       2, CC_CDECL },  /* double = 2 DWORDs */
    { "_isnan",              (PVOID)crt_isnan,        2, CC_CDECL },  /* double = 2 DWORDs */
    { "_dclass",             (PVOID)crt_dclass,       2, CC_CDECL },  /* double = 2 DWORDs */
    { "_fdclass",            (PVOID)crt_fdclass,      1, CC_CDECL },
    { "_stat",               (PVOID)crt_stat,         2, CC_CDECL },
    { "_stat32",             (PVOID)crt_stat32,       2, CC_CDECL },
    { "_stat32i64",          (PVOID)crt_stat32i64,    2, CC_CDECL },
    { "_stat64i32",          (PVOID)crt_stat64i32,    2, CC_CDECL },
    { "_stat64",             (PVOID)crt_stat64,       2, CC_CDECL },
    { "_fstat64",            (PVOID)crt_fstat64,      2, CC_CDECL },
    { "_chmod",              (PVOID)crt_chmod,        2, CC_CDECL },
    { "_wchmod",             (PVOID)crt_wchmod,       2, CC_CDECL },
    { "_wstat",              (PVOID)crt_wstat,        2, CC_CDECL },
    { "_wstat32",            (PVOID)crt_wstat32,      2, CC_CDECL },
    { "_wstat32i64",         (PVOID)crt_wstat32i64,   2, CC_CDECL },
    { "_wstat64i32",         (PVOID)crt_wstat64i32,   2, CC_CDECL },
    { "_wstat64",            (PVOID)crt_wstat64,      2, CC_CDECL },
    { "_strdate",            (PVOID)crt_strdate,      1, CC_CDECL },
    { "_strtime",            (PVOID)crt_strtime,      1, CC_CDECL },
    { "_wstrdate",           (PVOID)crt_wstrdate,     1, CC_CDECL },
    { "_wstrtime",           (PVOID)crt_wstrtime,     1, CC_CDECL },
    { "_vsnwprintf",         (PVOID)crt_vsnwprintf,   4, CC_CDECL },
    { "_vswprintf_c_l",      (PVOID)crt_vswprintf_c_l, 5, CC_CDECL },
    { "_wcsicmp",            (PVOID)crt_wcsicmp,      2, CC_CDECL },
    { "_wcsnicmp",           (PVOID)crt_wcsnicmp,     3, CC_CDECL },
    { "_wcsupr",             (PVOID)crt_wcsupr,       1, CC_CDECL },
    { "_wtoi",               (PVOID)crt_wtoi,         1, CC_CDECL },
    { "ceil",                (PVOID)crt_ceil,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "floor",               (PVOID)crt_floor,        2, CC_CDECL },  /* double = 2 DWORDs */
    { "fabs",                (PVOID)crt_fabs,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "sqrt",                (PVOID)crt_sqrt,         2, CC_CDECL },  /* double = 2 DWORDs */
    { "difftime",            (PVOID)crt_difftime,     2, CC_CDECL },  /* 2× long (time_t) */
    { "gmtime",              (PVOID)crt_gmtime,       1, CC_CDECL },
    { "mktime",              (PVOID)crt_mktime,       1, CC_CDECL },
    { "rand",                (PVOID)crt_rand,         0, CC_CDECL },
    { "srand",               (PVOID)crt_srand,        1, CC_CDECL },
    { "strncat",             (PVOID)crt_strncat,      3, CC_CDECL },
    { "wcscat",              (PVOID)crt_wcscat,       2, CC_CDECL },
    { "wcscat_s",            (PVOID)crt_wcscat_s,     3, CC_CDECL },
    { "wcschr",              (PVOID)crt_wcschr,       2, CC_CDECL },
    { "wcsrchr",             (PVOID)crt_wcsrchr,      2, CC_CDECL },
    { "wcscoll",             (PVOID)crt_wcscoll,      2, CC_CDECL },
    { "wcscmp",              (PVOID)crt_wcscmp,       2, CC_CDECL },
    { "wcscpy",              (PVOID)crt_wcscpy,       2, CC_CDECL },
    { "wcscpy_s",            (PVOID)crt_wcscpy_s,     3, CC_CDECL },
    { "_wcsdup",             (PVOID)crt_wcsdup,       1, CC_CDECL },
    { "wcslen",              (PVOID)crt_wcslen,       1, CC_CDECL },
    { "wcsnlen",             (PVOID)crt_wcsnlen,      2, CC_CDECL },
    { "wcsncmp",             (PVOID)crt_wcsncmp,      3, CC_CDECL },
    { "wcsncpy",             (PVOID)crt_wcsncpy,      3, CC_CDECL },
    { "wcsncpy_s",           (PVOID)crt_wcsncpy_s,    4, CC_CDECL },
    { "wcsstr",              (PVOID)crt_wcsstr,       2, CC_CDECL },
    { "wcstok_s",            (PVOID)crt_wcstok_s,     3, CC_CDECL },
    { "wcsxfrm",             (PVOID)crt_wcsxfrm,      3, CC_CDECL },
    { "wcstoul",             (PVOID)crt_wcstoul,      3, CC_CDECL },
    /* File access */
    { "_access",             (PVOID)crt_access,       2, CC_CDECL },
    { "_waccess",            (PVOID)crt_waccess,      2, CC_CDECL },
    { "_getcwd",             (PVOID)crt_getcwd,       2, CC_CDECL },
    { "_wgetcwd",            (PVOID)crt_wgetcwd,      2, CC_CDECL },
    { "_getdcwd",            (PVOID)crt_getdcwd,      3, CC_CDECL },
    { "_wgetdcwd",           (PVOID)crt_wgetdcwd,     3, CC_CDECL },
    { "_fullpath",           (PVOID)crt_fullpath,     3, CC_CDECL },
    { "_wfullpath",          (PVOID)crt_wfullpath,    3, CC_CDECL },

    /* CRT globals (as accessor functions through INT 0x2E) */
    { "_fltused",            (PVOID)crt_fltused,      0, CC_CDECL },
    { "__p__osver",          (PVOID)crt_p_osver,      0, CC_CDECL },
    { "__p__winver",         (PVOID)crt_p_winver,     0, CC_CDECL },
    { "__p__winmajor",       (PVOID)crt_p_winmajor,   0, CC_CDECL },
    { "__p__winminor",       (PVOID)crt_p_winminor,   0, CC_CDECL },

    /* Stubs for bundled MSVCRT.dll imports */
    { "_getch",              (PVOID)crt_getch_stub,   0, CC_CDECL },
    { "_kbhit",              (PVOID)crt_kbhit_stub,   0, CC_CDECL },

    { NULL, NULL, 0, CC_CDECL }
};

const WIN32_EXPORT *msvcrt_abi_table(int *count) {
    *count = (int)(sizeof(msvcrt_exports)/sizeof(msvcrt_exports[0]));
    return (const WIN32_EXPORT *)msvcrt_exports;
}

static int msvcrt_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static PVOID msvcrt_resolve_data_export(const char *func_name)
{
    if (!func_name) return NULL;
    if (msvcrt_strcmp(func_name, "_environ") == 0)
        return (PVOID)crt_p_environ();
    if (msvcrt_strcmp(func_name, "_wenviron") == 0)
        return (PVOID)crt_p_wenviron();
    if (msvcrt_strcmp(func_name, "_iob") == 0)
        return (PVOID)crt_iob_func();

    enum {
        CRT_DATA_NONE,
        CRT_DATA_INITENV,
        CRT_DATA_MB_CUR_MAX,
        CRT_DATA_ACMDLN,
        CRT_DATA_ADJUST_FDIV,
        CRT_DATA_COMMODE,
        CRT_DATA_FMODE,
        CRT_DATA_TIMEZONE,
        CRT_DATA_DAYLIGHT,
    } data = CRT_DATA_NONE;

    if (msvcrt_strcmp(func_name, "__initenv") == 0)
        data = CRT_DATA_INITENV;
    else if (msvcrt_strcmp(func_name, "__mb_cur_max") == 0)
        data = CRT_DATA_MB_CUR_MAX;
    else if (msvcrt_strcmp(func_name, "_acmdln") == 0)
        data = CRT_DATA_ACMDLN;
    else if (msvcrt_strcmp(func_name, "_adjust_fdiv") == 0)
        data = CRT_DATA_ADJUST_FDIV;
    else if (msvcrt_strcmp(func_name, "_commode") == 0)
        data = CRT_DATA_COMMODE;
    else if (msvcrt_strcmp(func_name, "_fmode") == 0)
        data = CRT_DATA_FMODE;
    else if (msvcrt_strcmp(func_name, "_timezone") == 0)
        data = CRT_DATA_TIMEZONE;
    else if (msvcrt_strcmp(func_name, "_daylight") == 0)
        data = CRT_DATA_DAYLIGHT;
    if (data == CRT_DATA_NONE) return NULL;

    UCRT_PROCESS_MODE_VALUES *values = ucrt_process_mode_state(TRUE);
    if (!values) return NULL;

    switch (data) {
        case CRT_DATA_INITENV:     return &values->initenv_value;
        case CRT_DATA_MB_CUR_MAX:  return &values->mb_cur_max;
        case CRT_DATA_ACMDLN:      return &values->acmdln_value;
        case CRT_DATA_ADJUST_FDIV: return &values->adjust_fdiv;
        case CRT_DATA_COMMODE:     return &values->commode;
        case CRT_DATA_FMODE:       return &values->fmode;
        case CRT_DATA_TIMEZONE:    return &values->timezone;
        case CRT_DATA_DAYLIGHT:    return &values->daylight;
        default:                   return NULL;
    }
}

PVOID msvcrt_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    if (by_ordinal) return NULL;

    /* CRT data exports are process-local writable variables, not functions. */
    PVOID data_export = msvcrt_resolve_data_export(func_name);
    if (data_export) return data_export;

    for (int i = 0; msvcrt_exports[i].name; i++) {
        if (msvcrt_strcmp(func_name, msvcrt_exports[i].name) == 0)
            return msvcrt_exports[i].func;
    }

    return NULL;
}

PVOID msvcrt_shim_init(void)
{
    ensure_stdio_init();
    win32_abi_register_compat32_bridge((PVOID)crt_printf,
                                        (PVOID)crt_printf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_sprintf,
                                        (PVOID)crt_sprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_snprintf,
                                        (PVOID)crt_snprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_snwprintf,
                                        (PVOID)crt_snwprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_snprintf_s,
                                        (PVOID)crt_snprintf_s_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_fprintf,
                                        (PVOID)crt_fprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_fwprintf,
                                        (PVOID)crt_fwprintf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_sscanf,
                                        (PVOID)crt_sscanf_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_lseeki64,
                                        (PVOID)crt_lseeki64_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_finite,
                                        (PVOID)crt_finite_compat32);
    win32_abi_register_compat32_bridge((PVOID)crt_isnan,
                                        (PVOID)crt_isnan_compat32);
    return (PVOID)msvcrt_exports;
}
