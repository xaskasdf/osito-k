/*
 * Decimal block conversion and emission adapted from musl src/stdio/vfprintf.c:
 * https://git.musl-libc.org/cgit/musl/tree/src/stdio/vfprintf.c
 * OsitoK changes: binary64 integer seed, explicit rounding policy, callback
 * sinks, and Microsoft exponent, hexadecimal and non-finite conventions.
 *
 * Copyright (c) 2005-2020 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "crt_float.h"

#define FP_INT_MAX 2147483647
#define FP_BASE 1000000000U
#define FP_MIN(a,b) ((a) < (b) ? (a) : (b))
#define FP_MAX(a,b) ((a) > (b) ? (a) : (b))

static void fp_out(CRT_FLOAT_SINK *sink, const char *s, size_t n)
{
    if (n && !sink->failed && !sink->write(sink->context, s, n))
        sink->failed = 1;
}

static void fp_repeat(CRT_FLOAT_SINK *sink, char c, int n)
{
    if (n > 0 && !sink->failed && !sink->repeat(sink->context, c, n))
        sink->failed = 1;
}

static char *fp_uint(unsigned value, char *end)
{
    while (value) {
        *--end = (char)('0' + value % 10);
        value /= 10;
    }
    return end;
}

static int fp_exponent(char *out, int exponent, char type, int digits)
{
    char tmp[12], *end = tmp + sizeof(tmp);
    char *start = fp_uint(exponent < 0 ? (unsigned)-exponent :
                                       (unsigned)exponent, end);
    while (end - start < digits) *--start = '0';
    *--start = exponent < 0 ? '-' : '+';
    *--start = type;
    int n = (int)(end - start);
    for (int i = 0; i < n; i++) out[i] = start[i];
    return n;
}

static int fp_round(uint64_t remainder, uint64_t half, int sticky, int odd,
                    int negative, uint64_t options, unsigned rounding)
{
    if (!remainder && !sticky) return 0;
    if (!(options & CRT_FLOAT_STANDARD_ROUND)) return remainder >= half;
    switch (rounding & 3) {
    case 1: return negative;
    case 2: return !negative;
    case 3: return 0;
    default: return remainder > half ||
                    (remainder == half && (sticky || odd));
    }
}

static int fp_field(CRT_FLOAT_SINK *sink, const char *prefix, int prefix_len,
                     const char *body, int body_len, int zeroes,
                     const char *suffix, int suffix_len, int width,
                     unsigned flags)
{
    int64_t total = (int64_t)prefix_len + body_len + zeroes + suffix_len;
    if (total > FP_INT_MAX) return -1;
    int padding = width > total ? width - (int)total : 0;
    if (!(flags & (CRT_FLOAT_LEFT | CRT_FLOAT_ZERO))) fp_repeat(sink, ' ', padding);
    fp_out(sink, prefix, prefix_len);
    if ((flags & (CRT_FLOAT_LEFT | CRT_FLOAT_ZERO)) == CRT_FLOAT_ZERO)
        fp_repeat(sink, '0', padding);
    fp_out(sink, body, body_len);
    fp_repeat(sink, '0', zeroes);
    fp_out(sink, suffix, suffix_len);
    if (flags & CRT_FLOAT_LEFT) fp_repeat(sink, ' ', padding);
    return sink->failed ? -1 : FP_MAX(width, (int)total);
}

static int fp_special(CRT_FLOAT_SINK *sink, uint64_t bits, int width,
                       int precision, unsigned flags, char type,
                       uint64_t options, unsigned rounding, char sign)
{
    uint64_t fraction = bits & 0x000fffffffffffffULL;
    int indefinite = bits == 0xfff8000000000000ULL;
    int signaling = fraction && !(fraction & 0x0008000000000000ULL);
    if (!(options & CRT_FLOAT_LEGACY_SPECIAL)) {
        const char *word = !fraction ? "inf" : indefinite ? "nan(ind)" :
                           signaling ? "nan(snan)" : "nan";
        char body[12], prefix[3]; int n = 0, pl = 0;
        if (sign) prefix[pl++] = sign;
        int point = precision == 0 && (flags & CRT_FLOAT_ALT) && (type | 32) != 'g';
        if (point && (bits >> 63)) {
            if ((type | 32) == 'a') { prefix[pl++] = '0'; prefix[pl++] = type & 32 ? 'x' : 'X'; }
            body[n++] = '.';
        }
        int at = 0;
        while (word[at]) {
            char c = word[at++];
            body[n++] = !(type & 32) && c >= 'a' && c <= 'z' ? c - 32 : c;
            if (point && !(bits >> 63) && at == 1) body[n++] = '.';
        }
        return fp_field(sink, prefix, pl, body, n, 0, 0, 0,
                         width, flags & ~CRT_FLOAT_ZERO);
    }

    const char *tag = !fraction ? "#INF" : indefinite ? "#IND" :
                      signaling ? "#SNAN" : "#QNAN";
    int tag_len = fraction && !indefinite ? 5 : 4;
    int hex = (type | 32) == 'a';
    int general = (type | 32) == 'g';
    int p = precision < 0 ? (hex && !(options & CRT_FLOAT_LEGACY_EXPORT) ? 13 : 6) : precision;
    if ((options & CRT_FLOAT_LEGACY_EXPORT) && p > 512) p = 512;
    if (general) p = p ? p - 1 : 0;
    char body[12], prefix[3], suffix[12];
    int n = 0, pl = 0, sl = 0;
    if (sign) prefix[pl++] = sign;
    if (hex) {
        if (options & CRT_FLOAT_LEGACY_EXPORT) { body[n++] = '0'; body[n++] = type & 32 ? 'x' : 'X'; }
        else { prefix[pl++] = '0'; prefix[pl++] = type & 32 ? 'x' : 'X'; }
    }
    int integer_pos = n;
    body[n++] = '1';
    if (p || (flags & CRT_FLOAT_ALT)) body[n++] = '.';
    int kept = FP_MIN(p, tag_len);
    for (int i = 0; i < kept; i++) body[n++] = tag[i];
    int round_up = p < tag_len && tag[p] >= '5';
    if (options & CRT_FLOAT_STANDARD_ROUND) {
        if ((rounding & 3) == 1) round_up = sign == '-';
        else if ((rounding & 3) == 2) round_up = sign != '-';
        else if ((rounding & 3) == 3) round_up = 0;
    }
    if (fraction && p >= tag_len) round_up = 0;
    if (round_up && p <= tag_len) body[kept ? n - 1 : integer_pos]++;
    int zeroes = p > tag_len ? p - tag_len : 0;
    if (round_up && zeroes) { zeroes--; suffix[sl++] = '1'; }
    else if (general && !(flags & CRT_FLOAT_ALT)) zeroes = 0;
    if (hex) sl += fp_exponent(suffix + sl, 0, type & 32 ? 'p' : 'P', 1);
    else if ((type | 32) == 'e')
        sl += fp_exponent(suffix + sl, 0, type, options & CRT_FLOAT_THREE_DIGITS ? 3 : 2);
    return fp_field(sink, prefix, pl, body, n, zeroes, suffix, sl, width, flags);
}

static int fp_hex(CRT_FLOAT_SINK *sink, uint64_t bits, int width, int precision,
                   unsigned flags, char type, uint64_t options,
                   unsigned rounding, char sign)
{
    unsigned exponent = (unsigned)(bits >> 52) & 0x7ff;
    uint64_t mantissa = bits & 0x000fffffffffffffULL;
    if (exponent) mantissa |= 0x0010000000000000ULL;
    int e = exponent ? (int)exponent - 1023 : mantissa ? -1022 : 0;
    int p = precision < 0 ? (options & CRT_FLOAT_LEGACY_EXPORT ? 6 : 13) : precision;
    if ((options & CRT_FLOAT_LEGACY_EXPORT) && p > 512) p = 512;
    int kept = FP_MIN(p, 13);
    if (kept < 13) {
        unsigned shift = 52 - 4 * kept;
        uint64_t mask = (1ULL << shift) - 1;
        uint64_t remainder = mantissa & mask;
        mantissa >>= shift;
        uint64_t guard = remainder >> (shift - 4);
        uint64_t rounding_remainder = remainder;
        /* Windows directed hex rounding uses the guard nibble, not sticky bits. */
        if ((options & CRT_FLOAT_STANDARD_ROUND) && (rounding & 3))
            rounding_remainder = guard << (shift - 4);
        int round_up = options & CRT_FLOAT_LEGACY_EXPORT ? guard > 8 :
            fp_round(rounding_remainder, 1ULL << (shift - 1), 0, mantissa & 1,
                     sign == '-', options, rounding);
        if (round_up)
            mantissa++;
    }
    const char *digits = type & 32 ? "0123456789abcdef" : "0123456789ABCDEF";
    char body[18], prefix[3], suffix[12];
    int n = 0, pl = 0;
    if (sign) prefix[pl++] = sign;
    if (options & CRT_FLOAT_LEGACY_EXPORT) { body[n++] = '0'; body[n++] = type & 32 ? 'x' : 'X'; }
    else { prefix[pl++] = '0'; prefix[pl++] = type & 32 ? 'x' : 'X'; }
    body[n++] = digits[(mantissa >> (4 * kept)) & 15];
    if (p || (flags & CRT_FLOAT_ALT)) body[n++] = '.';
    for (int i = kept - 1; i >= 0; i--) body[n++] = digits[(mantissa >> (4 * i)) & 15];
    int sl = fp_exponent(suffix, e, type & 32 ? 'p' : 'P', 1);
    return fp_field(sink, prefix, pl, body, n, p - kept, suffix, sl, width, flags);
}

int crt_float_format(CRT_FLOAT_SINK *sink, uint64_t bits, int width,
                     int precision, unsigned flags, char type,
                     uint64_t options, unsigned rounding)
{
    if (type == 'F' && (options & CRT_FLOAT_LEGACY_SPECIAL)) return 0;
    int negative = (int)(bits >> 63);
    char sign = negative ? '-' : flags & CRT_FLOAT_PLUS ? '+' :
                flags & CRT_FLOAT_SPACE ? ' ' : 0;
    unsigned exponent = (unsigned)(bits >> 52) & 0x7ff;
    if (exponent == 0x7ff)
        return fp_special(sink, bits, width, precision, flags, type, options, rounding, sign);
    if ((type | 32) == 'a')
        return fp_hex(sink, bits, width, precision, flags, type, options, rounding, sign);

    /* Binary64 needs at most 309 integer or 1074 fractional decimal places.
     * Two seed limbs and 126 spare base-1e9 limbs cover both extrema, without
     * allocation or an output-sized scratch buffer. */
    uint32_t big[128] = {0};
    uint64_t mantissa = bits & 0x000fffffffffffffULL;
    if (exponent) mantissa |= 0x0010000000000000ULL;
    int e2 = exponent ? (int)exponent - 1075 : -1074;
    uint32_t *r = e2 < 0 ? big + 1 : big + 127;
    uint32_t *a = r, *z = r + 1, *d;
    *r = (uint32_t)(mantissa % FP_BASE);
    if (mantissa >= FP_BASE) *--a = (uint32_t)(mantissa / FP_BASE);
    if (!mantissa) e2 = 0;
    while (e2 > 0) {
        uint32_t carry = 0;
        int shift = FP_MIN(29, e2);
        for (d = z; d > a;) {
            --d;
            uint64_t x = ((uint64_t)*d << shift) + carry;
            *d = (uint32_t)(x % FP_BASE);
            carry = (uint32_t)(x / FP_BASE);
        }
        if (carry) *--a = carry;
        while (z > a && !z[-1]) z--;
        e2 -= shift;
    }
    while (e2 < 0) {
        uint32_t carry = 0;
        int shift = FP_MIN(9, -e2);
        for (d = a; d < z; d++) {
            uint32_t remainder = *d & ((1U << shift) - 1);
            *d = (*d >> shift) + carry;
            carry = (FP_BASE >> shift) * remainder;
        }
        if (!*a) a++;
        if (carry) *z++ = carry;
        e2 += shift;
    }
    while (z > a && !z[-1]) z--;
    int e = 0;
    if (a < z) {
        e = 9 * (int)(r - a);
        for (uint32_t i = 10; *a >= i; i *= 10) e++;
    }
    int64_t p = precision < 0 ? 6 : precision;
    if ((options & CRT_FLOAT_LEGACY_EXPORT) && p > 512) p = 512;
    /* Legacy exports first round to a 17-digit decimal seed; UCRT does not. */
    for (int pass = 0; pass < 2; pass++) {
        if (!pass && (!(options & CRT_FLOAT_LEGACY_EXPORT) || !mantissa)) continue;
        int64_t j = pass ? p - ((type | 32) != 'f' ? e : 0) - ((type | 32) == 'g' && p) : 16 - e;
        if (j < 9 * (z - r - 1)) {
            int block = (int)j / 9, digit = (int)j % 9;
            if (digit < 0) { digit += 9; block--; }
            d = r + 1 + block;
            uint32_t divisor = 10;
            for (int k = digit + 1; k < 9; k++) divisor *= 10;
            uint32_t remainder = *d % divisor;
            int odd = (*d / divisor) & 1;
            if (divisor == FP_BASE && d > a) odd = d[-1] & 1;
            int round_up = fp_round(remainder, divisor / 2, d + 1 != z,
                                    odd, negative, pass ? options : 0, rounding);
            /* Windows emits zero when fixed precision cannot reach any source digit. */
            if (pass && (type | 32) == 'f' && p + e + 1 < 0) round_up = 0;
            *d -= remainder;
            if (round_up) {
                *d += divisor;
                while (*d >= FP_BASE) {
                    *d-- = 0;
                    if (d < a) *--a = 0;
                    (*d)++;
                }
                e = 9 * (int)(r - a);
                for (uint32_t i = 10; *a >= i; i *= 10) e++;
            }
            if (z > d + 1) {
                for (uint32_t *clear = d + 1; clear < z; clear++) *clear = 0;
                z = d + 1;
            }
        }
        while (z > a && !z[-1]) z--;
    }
    if ((type | 32) == 'g') {
        if (!p) p = 1;
        if (!mantissa && (options & CRT_FLOAT_LEGACY_EXPORT) && (flags & CRT_FLOAT_ALT)) p++;
        if (p > e && e >= -4) { type--; p -= e + 1; }
        else { type -= 2; p--; }
        if (!(flags & CRT_FLOAT_ALT)) {
            int trailing = 9;
            if (z > a && z[-1]) {
                trailing = 0;
                for (uint32_t i = 10; z[-1] % i == 0; i *= 10) trailing++;
            }
            int digits = 9 * (int)(z - r - 1) - trailing;
            if ((type | 32) != 'f') digits += e;
            p = FP_MIN(p, FP_MAX(0, digits));
        }
    }
    char suffix[12]; int suffix_len = 0;
    int64_t body_len = 1 + p + (p || (flags & CRT_FLOAT_ALT));
    if ((type | 32) == 'f') body_len += FP_MAX(e, 0);
    else {
        suffix_len = fp_exponent(suffix, e, type, options & CRT_FLOAT_THREE_DIGITS ? 3 : 2);
        body_len += suffix_len;
    }
    int sign_len = sign != 0;
    if (body_len > FP_INT_MAX - sign_len) return -1;
    int total = (int)body_len + sign_len;
    int padding = width > total ? width - total : 0;
    if (!(flags & (CRT_FLOAT_LEFT | CRT_FLOAT_ZERO))) fp_repeat(sink, ' ', padding);
    fp_out(sink, &sign, sign_len);
    if ((flags & (CRT_FLOAT_LEFT | CRT_FLOAT_ZERO)) == CRT_FLOAT_ZERO)
        fp_repeat(sink, '0', padding);

    char buf[9];
    if ((type | 32) == 'f') {
        if (a > r) a = r;
        for (d = a; d <= r; d++) {
            char *s = fp_uint(*d, buf + 9);
            if (d != a) while (s > buf) *--s = '0';
            else if (s == buf + 9) *--s = '0';
            fp_out(sink, s, (size_t)(buf + 9 - s));
        }
        if (p || (flags & CRT_FLOAT_ALT)) fp_out(sink, ".", 1);
        for (; d < z && p > 0; d++, p -= 9) {
            char *s = fp_uint(*d, buf + 9);
            while (s > buf) *--s = '0';
            fp_out(sink, s, (size_t)FP_MIN(9, p));
        }
        fp_repeat(sink, '0', (int)FP_MAX(p, 0));
    } else {
        if (z <= a) z = a + 1;
        for (d = a; d < z && p >= 0; d++) {
            char *s = fp_uint(*d, buf + 9);
            if (s == buf + 9) *--s = '0';
            if (d != a) while (s > buf) *--s = '0';
            else {
                fp_out(sink, s++, 1);
                if (p || (flags & CRT_FLOAT_ALT)) fp_out(sink, ".", 1);
            }
            fp_out(sink, s, (size_t)FP_MIN(buf + 9 - s, p));
            p -= buf + 9 - s;
        }
        fp_repeat(sink, '0', (int)FP_MAX(p, 0));
        fp_out(sink, suffix, suffix_len);
    }
    if (flags & CRT_FLOAT_LEFT) fp_repeat(sink, ' ', padding);
    return sink->failed ? -1 : FP_MAX(width, total);
}
