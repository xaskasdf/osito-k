/* Automation scalar coercion. Guest VARIANT pointers use the caller's ABI. */
#include "ole32_shim.h"
#include "compat32.h"
#include "crt_float.h"
#include "msvcrt_shim.h"
#include "kernel32_shim.h"
#include "wintime.h"

#define VA_INVALID ((HRESULT)0x80070057)
#define VA_MEMORY ((HRESULT)0x8007000e)
#define VA_TYPE ((HRESULT)0x80020005)
#define VA_BADTYPE ((HRESULT)0x80020008)
#define VA_OVERFLOW ((HRESULT)0x8002000a)

typedef union { double real; uint64_t bits; } VA_REAL;
typedef struct {
    uint32_t limb[3];
    unsigned scale;
    BOOL negative, floating;
    USHORT origin;
    VA_REAL real;
} VA_NUMBER;

static SIZE_T va_size(void) { return g_compat32_mode ? 16 : 24; }
static PVOID va_pointer(const BYTE *p)
{
    return g_compat32_mode ? (PVOID)(ULONG_PTR)*(const uint32_t *)p
                           : *(PVOID const *)p;
}
static void va_set_pointer(BYTE *p, PVOID value)
{
    if (g_compat32_mode) *(uint32_t *)p = (uint32_t)(ULONG_PTR)value;
    else *(PVOID *)p = value;
}
static void va_zero(BYTE *p)
{ for (SIZE_T i = 0; i < 24; i++) p[i] = 0; }
static BOOL va_valid(USHORT type)
{
    if (type & 0x9000) return FALSE;
    USHORT base = type & 0xfff;
    if (base <= 1) return type == base;
    return (base >= 2 && base <= 14) || (base >= 16 && base <= 23) || base == 36;
}
static BOOL va_nonzero(const VA_NUMBER *n)
{ return n->limb[0] || n->limb[1] || n->limb[2]; }
static BOOL va_mul10(VA_NUMBER *n, unsigned digit)
{
    uint64_t carry = digit;
    for (unsigned i = 0; i < 3; i++) {
        carry += (uint64_t)n->limb[i] * 10;
        n->limb[i] = (uint32_t)carry;
        carry >>= 32;
    }
    return carry == 0;
}
static unsigned va_div10(VA_NUMBER *n)
{
    uint64_t carry = 0;
    for (int i = 2; i >= 0; i--) {
        uint64_t value = (carry << 32) | n->limb[i];
        n->limb[i] = (uint32_t)(value / 10);
        carry = value % 10;
    }
    return (unsigned)carry;
}
static BOOL va_increment(VA_NUMBER *n)
{
    for (unsigned i = 0; i < 3; i++) if (++n->limb[i]) return TRUE;
    return FALSE;
}
static HRESULT va_rescale(VA_NUMBER *n, unsigned scale)
{
    if (n->scale > scale) {
        BOOL sticky = FALSE;
        unsigned last = 0;
        while (n->scale > scale) {
            sticky |= last != 0;
            last = va_div10(n);
            n->scale--;
            if (!va_nonzero(n) && n->scale > scale) {
                sticky |= last != 0;
                last = 0;
                n->scale = scale;
                break;
            }
        }
        if ((last > 5 || (last == 5 && (sticky || (n->limb[0] & 1)))) &&
            !va_increment(n)) return VA_OVERFLOW;
    }
    while (n->scale < scale) {
        if (!va_mul10(n, 0)) return VA_OVERFLOW;
        n->scale++;
    }
    return S_OK;
}

/* Scale a binary64 significand using integer arithmetic, then round to even.
 * This does not depend on or change the caller's x87/SSE rounding mode. */
static HRESULT va_float_integer(VA_REAL real, unsigned scale, VA_NUMBER *n)
{
    unsigned exponent = (unsigned)((real.bits >> 52) & 0x7ff);
    if (exponent == 0x7ff) return VA_OVERFLOW;
    uint64_t fraction = real.bits & 0xfffffffffffffULL;
    int shift = exponent ? (int)exponent - 1023 - 52 : -1074;
    if (exponent) fraction |= 1ULL << 52;
    __uint128_t value = fraction;
    for (unsigned i = 0; i < scale; i++) value *= 10;
    if (shift >= 0) {
        if (shift >= 96 || value > ((((__uint128_t)1 << 96) - 1) >> shift))
            return VA_OVERFLOW;
        value <<= shift;
    } else if (shift <= -128) {
        value = 0;
    } else {
        unsigned right = (unsigned)-shift;
        __uint128_t half = (__uint128_t)1 << (right - 1);
        __uint128_t remainder = value & ((half << 1) - 1);
        value >>= right;
        if (remainder > half || (remainder == half && (value & 1))) value++;
    }
    if (value >> 96) return VA_OVERFLOW;
    n->limb[0] = (uint32_t)value;
    n->limb[1] = (uint32_t)(value >> 32);
    n->limb[2] = (uint32_t)(value >> 64);
    n->negative = (real.bits >> 63) != 0;
    n->scale = scale;
    n->floating = FALSE;
    return S_OK;
}

static double va_double(const VA_NUMBER *n)
{
    if (n->floating) return n->real.real;
    static const double powers[] = {
        1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,1e12,1e13,
        1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22,1e23,1e24,1e25,1e26,1e27,1e28
    };
    double result = (double)n->limb[2] * 18446744073709551616.0 +
                    (double)n->limb[1] * 4294967296.0 + n->limb[0];
    unsigned scale = n->scale;
    while (scale > 28) { result /= 1e28;scale -= 28; }
    result /= powers[scale];
    return n->negative ? -result : result;
}

static BOOL va_space(WCHAR c)
{ return c == ' ' || (c >= 9 && c <= 13) || c == 0xa0; }

/* Parse the en-US numeric syntax exposed by the OS's locale APIs. Keep an
 * exact 96-bit decimal as well as a binary fallback for very large exponents. */
static HRESULT va_parse_number(PCWSTR text, VA_NUMBER *n)
{
    if (!text) return VA_TYPE;
    SIZE_T length = 0;
    while (text[length]) length++;
    if (length > (SIZE_T)-2) return VA_MEMORY;
    char *ascii = shim_CoTaskMemAlloc(length + 2);
    if (!ascii) return VA_MEMORY;
    SIZE_T begin = 0, end = length, at = 0;
    while (begin < end && va_space(text[begin])) begin++;
    while (end > begin && va_space(text[end - 1])) end--;
    BOOL negative = FALSE, sign = FALSE, currency = FALSE;
    if (begin < end && text[begin] == '(' && text[end - 1] == ')') {
        negative = sign = TRUE;begin++;end--;
    }
    if (begin < end && (text[begin] == '+' || text[begin] == '-')) {
        if (sign) goto invalid;
        negative = text[begin++] == '-';sign = TRUE;
    }
    if (begin < end && text[begin] == '$') { begin++;currency = TRUE; }
    while (begin < end && va_space(text[begin])) begin++;
    if (end > begin && (text[end - 1] == '+' || text[end - 1] == '-')) {
        if (sign) goto invalid;
        negative = text[--end] == '-';sign = TRUE;
    }
    if (end > begin && text[end - 1] == '$') {
        if (currency) goto invalid;
        end--;
    }
    while (end > begin && va_space(text[end - 1])) end--;
    if (begin == end) goto invalid;
    if (negative) ascii[at++] = '-';
    n->negative = negative;
    if (end - begin > 2 && text[begin] == '&' &&
        (text[begin + 1] == 'H' || text[begin + 1] == 'h' ||
         text[begin + 1] == 'O' || text[begin + 1] == 'o')) {
        unsigned base = text[begin + 1] == 'H' || text[begin + 1] == 'h' ? 16 : 8;
        uint64_t value = 0;
        for (SIZE_T i = begin + 2; i < end; i++) {
            unsigned digit = text[i] >= '0' && text[i] <= '9' ? text[i] - '0' :
                text[i] >= 'A' && text[i] <= 'F' ? text[i] - 'A' + 10 :
                text[i] >= 'a' && text[i] <= 'f' ? text[i] - 'a' + 10 : 99;
            if (digit >= base) goto invalid;
            if (value > (UINT64_MAX - digit) / base) {
                shim_CoTaskMemFree(ascii);return VA_OVERFLOW;
            }
            value = value * base + digit;
        }
        n->limb[0] = (uint32_t)value;n->limb[1] = (uint32_t)(value >> 32);
        shim_CoTaskMemFree(ascii);return S_OK;
    }
    BOOL decimal = FALSE, digit_seen = FALSE, exact = TRUE;
    unsigned fraction = 0;
    SIZE_T i = begin;
    for (; i < end; i++) {
        WCHAR ch = text[i];
        if (ch >= '0' && ch <= '9') {
            digit_seen = TRUE;
            if (exact && !va_mul10(n, ch - '0')) exact = FALSE;
            if (decimal && fraction < 10000) fraction++;
            ascii[at++] = (char)ch;
        } else if (ch == '.' && !decimal) {
            decimal = TRUE;ascii[at++] = '.';
        } else if (ch == ',' && !decimal && digit_seen) {
            continue;
        } else break;
    }
    if (!digit_seen) goto invalid;
    int exponent = 0;
    if (i < end && (text[i] == 'e' || text[i] == 'E')) {
        ascii[at++] = 'e';i++;
        BOOL minus = FALSE;
        if (i < end && (text[i] == '+' || text[i] == '-')) {
            minus = text[i] == '-';ascii[at++] = (char)text[i++];
        }
        if (i == end || text[i] < '0' || text[i] > '9') goto invalid;
        for (; i < end && text[i] >= '0' && text[i] <= '9'; i++) {
            if (exponent < 10000) exponent = exponent * 10 + text[i] - '0';
            ascii[at++] = (char)text[i];
        }
        if (minus) exponent = -exponent;
    }
    if (i != end) goto invalid;
    ascii[at] = 0;
    char *tail;
    n->real.real = crt_strtod(ascii, &tail);
    if (*tail) goto invalid;
    if ((n->real.bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) {
        shim_CoTaskMemFree(ascii);return VA_OVERFLOW;
    }
    int scale = (int)fraction - exponent;
    while (exact && scale < 0) { exact = va_mul10(n, 0);scale++; }
    n->scale = scale > 0 ? (unsigned)scale : 0;
    n->floating = !exact;
    shim_CoTaskMemFree(ascii);
    return S_OK;
invalid:
    shim_CoTaskMemFree(ascii);
    return VA_TYPE;
}

static HRESULT va_number(const BYTE *source, VA_NUMBER *n)
{
    USHORT type = *(const USHORT *)source;
    n->origin = type;
    uint64_t bits = *(const uint64_t *)(source + 8), magnitude = 0;
    int64_t signed_value = 0;
    BOOL signed_type = TRUE;
    switch (type) {
    case 0: return S_OK;
    case 2: case 11: signed_value = (int16_t)bits;break;
    case 3: case 22: signed_value = (int32_t)bits;break;
    case 16: signed_value = (int8_t)bits;break;
    case 20: signed_value = (int64_t)bits;break;
    case 17: magnitude = (uint8_t)bits;signed_type = FALSE;break;
    case 18: magnitude = (uint16_t)bits;signed_type = FALSE;break;
    case 19: case 23: magnitude = (uint32_t)bits;signed_type = FALSE;break;
    case 21: magnitude = bits;signed_type = FALSE;break;
    case 6: signed_value = (int64_t)bits;n->scale = 4;break;
    case 4: {
        union { float value; uint32_t bits; } real = {(float)0};
        real.bits = (uint32_t)bits;n->real.real = real.value;n->floating = TRUE;
        return S_OK;
    }
    case 5: case 7: n->real.bits = bits;n->floating = TRUE;return S_OK;
    case 14:
        if (source[2] > 28 || (source[3] & 0x7f)) return VA_INVALID;
        n->scale = source[2];n->negative = source[3] != 0;
        n->limb[0] = (uint32_t)bits;n->limb[1] = (uint32_t)(bits >> 32);
        n->limb[2] = *(const uint32_t *)(source + 4);return S_OK;
    case 8: return va_parse_number(va_pointer(source + 8), n);
    default: return VA_TYPE;
    }
    if (signed_type) {
        n->negative = signed_value < 0;
        magnitude = n->negative ? 0ULL - (uint64_t)signed_value : (uint64_t)signed_value;
    }
    n->limb[0] = (uint32_t)magnitude;n->limb[1] = (uint32_t)(magnitude >> 32);
    return S_OK;
}

typedef struct { char *data; SIZE_T used, capacity; } VA_TEXT;
static int va_text_write(void *ctx, const char *data, size_t count)
{
    VA_TEXT *text = ctx;
    if (count >= text->capacity - text->used) return 0;
    for (SIZE_T i = 0; i < count; i++) text->data[text->used++] = data[i];
    text->data[text->used] = 0;
    return 1;
}
static int va_text_repeat(void *ctx, char ch, int count)
{
    while (count-- > 0) if (!va_text_write(ctx, &ch, 1)) return 0;
    return 1;
}
static HRESULT va_float_text(VA_REAL real, unsigned digits, char *output)
{
    if (g_compat32_mode && (real.bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) {
        unsigned at = 0;
        if (real.bits >> 63) output[at++] = '-';
        const char *word = real.bits & 0xfffffffffffffULL ? "1.#QNAN" : "1.#INF";
        while (*word) output[at++] = *word++;
        output[at] = 0;return S_OK;
    }
    char scientific[64];
    VA_TEXT text = {scientific, 0, sizeof(scientific)};
    CRT_FLOAT_SINK sink = {&text, va_text_write, va_text_repeat, 0};
    if (crt_float_format(&sink, real.bits, 0, (int)digits - 1, 0, 'e',
                         CRT_FLOAT_STANDARD_ROUND, 0) < 0) return VA_OVERFLOW;
    if (!(real.bits & 0x7fffffffffffffffULL)) {
        unsigned at = 0;
        if (!g_compat32_mode && real.bits >> 63) output[at++] = '-';
        output[at++] = '0';output[at] = 0;return S_OK;
    }
    char *p = scientific;
    unsigned at = 0;
    if (*p == '-') { output[at++] = *p++; }
    if (*p < '0' || *p > '9') {
        while (*p) output[at++] = *p++;
        output[at] = 0;return S_OK;
    }
    char significant[20];unsigned length = 0;
    while (*p && *p != 'e') { if (*p != '.') significant[length++] = *p;p++; }
    if (*p != 'e') return VA_INVALID;
    p++;BOOL minus = *p == '-';if (*p == '+' || *p == '-') p++;
    int exponent = 0;while (*p >= '0' && *p <= '9') exponent = exponent * 10 + *p++ - '0';
    if (minus) exponent = -exponent;
    while (length > 1 && significant[length - 1] == '0') length--;
    if (exponent >= (int)digits || (exponent < 0 && (int)length - exponent > (int)digits + 1)) {
        output[at++] = significant[0];
        if (length > 1) output[at++] = '.';
        for (unsigned i = 1; i < length; i++) output[at++] = significant[i];
        output[at++] = 'E';output[at++] = exponent < 0 ? '-' : '+';
        unsigned e = exponent < 0 ? (unsigned)-exponent : (unsigned)exponent;
        if (e >= 100) output[at++] = (char)('0' + e / 100);
        output[at++] = (char)('0' + e / 10 % 10);
        output[at++] = (char)('0' + e % 10);
    } else if (exponent < 0) {
        output[at++] = '0';output[at++] = '.';
        for (int i = -1; i > exponent; i--) output[at++] = '0';
        for (unsigned i = 0; i < length; i++) output[at++] = significant[i];
    } else {
        for (unsigned i = 0; i <= (unsigned)exponent || i < length; i++) {
            if (i == (unsigned)exponent + 1) output[at++] = '.';
            output[at++] = i < length ? significant[i] : '0';
        }
    }
    output[at] = 0;return S_OK;
}
static HRESULT va_number_text(VA_NUMBER n, char *output)
{
    if (n.floating) return va_float_text(n.real, n.origin == 4 ? 7 : 15, output);
    char reverse[32];unsigned length = 0, at = 0;
    if (!va_nonzero(&n)) { output[0] = '0';output[1] = 0;return S_OK; }
    while (va_nonzero(&n)) reverse[length++] = (char)('0' + va_div10(&n));
    while (n.scale && length && reverse[0] == '0') {
        for (unsigned i = 1; i < length; i++) reverse[i - 1] = reverse[i];
        length--;n.scale--;
    }
    if (n.negative) output[at++] = '-';
    if (n.scale >= length) {
        output[at++] = '0';output[at++] = '.';
        if (n.scale > 28) return VA_OVERFLOW;
        for (unsigned i = length; i < n.scale; i++) output[at++] = '0';
    }
    for (unsigned i = length; i; i--) {
        if (i == n.scale && i < length) output[at++] = '.';
        output[at++] = reverse[i - 1];
    }
    output[at] = 0;return S_OK;
}
static unsigned va_month_days(unsigned year, unsigned month)
{
    static const BYTE days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!month || month > 12) return 0;
    return days[month - 1] + (month == 2 && year % 4 == 0 &&
                            (year % 100 != 0 || year % 400 == 0));
}

/* Automation DATE extends to year 100, below FILETIME's 1601 epoch. */
static int va_calendar_days(unsigned year, unsigned month, unsigned day)
{
    unsigned y = year - 1;
    int days = (int)(365 * (year - 100) + y / 4 - y / 100 + y / 400 - 24);
    for (unsigned m = 1; m < month; m++) days += va_month_days(year, m);
    return days + (int)day - 1 - 657434;
}

static void va_days_calendar(int days, unsigned *year, unsigned *month, unsigned *day)
{
    unsigned remaining = (unsigned)(days + 657434);
    unsigned y = 100 + (remaining / 146097) * 400;
    remaining %= 146097;
    for (;;) {
        unsigned length = 337 + va_month_days(y, 2);
        if (remaining < length) break;
        remaining -= length;y++;
    }
    unsigned m = 1;
    while (remaining >= va_month_days(y, m)) { remaining -= va_month_days(y, m);m++; }
    *year = y;*month = m;*day = remaining + 1;
}

static HRESULT va_parse_date(PCWSTR text, double *date)
{
    if (!text) return VA_TYPE;
    unsigned values[6], months[6], separators[6] = {0}, count = 0;
    int meridian = -1;
    const WCHAR *p = text;
    while (va_space(*p)) p++;
    while (*p) {
        unsigned value = 0, month = 0;
        if (*p >= '0' && *p <= '9') {
            if (count == 6) return VA_TYPE;
            do { value = value * 10 + *p++ - '0';if (value > 9999) return VA_TYPE; }
            while (*p >= '0' && *p <= '9');
        } else {
            const WCHAR *word = p;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
            SIZE_T length = (SIZE_T)(p - word);
            if (length == 2 && ((word[0] | 32) == 'a' || (word[0] | 32) == 'p') &&
                (word[1] | 32) == 'm') {
                meridian = (word[0] | 32) == 'p';
                while (va_space(*p)) p++;
                if (*p || !count) return VA_TYPE;
                break;
            }
            static const char *names[12] = {"january","february","march","april","may","june",
                "july","august","september","october","november","december"};
            for (unsigned m = 0; m < 12; m++) {
                unsigned i = 0;
                while (i < length && names[m][i] && (word[i] | 32) == (WCHAR)names[m][i]) i++;
                if (i == length && (length == 3 || !names[m][i])) { month = m + 1;break; }
            }
            if (!month) return VA_TYPE;
            if (count == 6) return VA_TYPE;
            value = month;
        }
        values[count] = value;months[count] = month;count++;
        const WCHAR *end = p;
        while (va_space(*p)) p++;
        if (!*p) break;
        if (*p == '/' || *p == ',' || *p == '-' || *p == ':' || *p == '.') {
            separators[count - 1] = *p++;
            while (va_space(*p)) p++;
            if (!*p) return VA_TYPE;
        } else if (p == end && !((*p | 32) == 'a' || (*p | 32) == 'p')) return VA_TYPE;
    }
    unsigned dates = count, times = 0;
    for (unsigned i = 0; i < count; i++) {
        if (separators[i] == ':' || separators[i] == '.') { dates = i;times = count - i;break; }
    }
    if (meridian >= 0 && !times) { if (!count) return VA_TYPE;dates = count - 1;times = 1; }
    if ((!dates && !times) || times > 3 || (dates && dates != 2 && dates != 3)) return VA_TYPE;
    unsigned seconds = 0;
    if (times) {
        for (unsigned i = dates; i < count; i++) {
            if (months[i] || (i + 1 < count && separators[i] != ':' && separators[i] != '.')) return VA_TYPE;
        }
        unsigned hour = values[dates], minute = times >= 2 ? values[dates + 1] : 0;
        unsigned second = times == 3 ? values[dates + 2] : 0;
        if (meridian >= 0) {
            if (!hour || hour > 12) return VA_TYPE;
            hour = hour % 12 + (unsigned)meridian * 12;
        }
        if (hour > 23 || minute > 59 || second > 59) return VA_TYPE;
        seconds = hour * 3600 + minute * 60 + second;
    }
    int days = 0;
    if (dates) {
        unsigned month = 0, year = 0, day = 1, numeric[3], n = 0;
        for (unsigned i = 0; i < dates; i++) {
            if (months[i]) { if (month) return VA_TYPE;month = months[i]; }
            else numeric[n++] = values[i];
        }
        if (month) {
            if (n == 2) { day = numeric[0];year = numeric[1];if (day > 31) { year = day;day = numeric[1]; } }
            else if (n == 1) { if (numeric[0] > 31) year = numeric[0];else day = numeric[0]; }
            else return VA_TYPE;
        } else if (dates == 3) {
            if (values[0] > 31) { year = values[0];month = values[1];day = values[2]; }
            else { month = values[0];day = values[1];year = values[2]; }
            if (month > 12 && day <= 12) { unsigned swap = month;month = day;day = swap; }
        } else if (values[0] > 31) { year = values[0];month = values[1]; }
        else if (values[1] > 31) { month = values[0];year = values[1]; }
        else { month = values[0];day = values[1]; }
        if (!year && dates == 2) {
            WINTIME_CALENDAR current;
            if (wintime_unix_to_calendar(wintime_now_unix_seconds(), &current)) return VA_TYPE;
            year = current.year;
        } else if (year < 100) year += year <= 49 ? 2000 : 1900;
        if (year < 100 || year > 9999 || !day || day > va_month_days(year, month)) return VA_TYPE;
        days = va_calendar_days(year, month, day);
    }
    double fraction = (double)seconds / 86400.0;
    *date = (double)days + (days < 0 ? -fraction : fraction);
    return S_OK;
}

static HRESULT va_date_text(double date, char *output)
{
    if (!(date > -657435.0 && date < 2958466.0)) return VA_INVALID;
    int64_t days = (int64_t)date;
    double part = date - (double)days;if (part < 0) part = -part;
    unsigned seconds = (unsigned)(part * 86400.0 + 0.5);
    if (seconds == 86400) { days++;seconds = 0; }
    if (days > 2958465) return VA_INVALID;
    unsigned year, month, day;
    va_days_calendar((int)days, &year, &month, &day);
    int at = 0;
    if (days) at = crt_snprintf(output, 96, "%u/%u/%u", month, day, year);
    if (!days || seconds) {
        if (at) output[at++] = ' ';
        unsigned hour = seconds / 3600 % 12;if (!hour) hour = 12;
        crt_snprintf(output + at, 96 - at, "%u:%02u:%02u %s", hour,
            seconds / 60 % 60, seconds % 60,
            seconds / 3600 >= 12 ? "PM" : "AM");
    }
    return S_OK;
}
static HRESULT va_bstr(BYTE *output, const BYTE *source, USHORT flags)
{
    USHORT type = *(const USHORT *)source;
    char text[128];HRESULT status;
    if (type == 11 && (flags & (2 | 16))) {
        const char *word = *(const USHORT *)(source + 8) ? "True" : "False";
        unsigned i = 0;do { text[i] = word[i]; } while (word[i++]);
    } else if (type == 7) {
        status = va_date_text(*(const double *)(source + 8), text);
        if (status != S_OK) return status;
    } else {
        VA_NUMBER number = {0};status = va_number(source, &number);
        if (status != S_OK) return status;
        if (!type) text[0] = 0;
        else { status = va_number_text(number, text);if (status != S_OK) return status; }
    }
    WCHAR wide[128];unsigned i = 0;
    do { wide[i] = (BYTE)text[i]; } while (text[i++]);
    PWSTR string = shim_SysAllocStringLen(wide, i - 1);
    if (!string) return VA_MEMORY;
    *(USHORT *)output = 8;va_set_pointer(output + 8, string);
    return S_OK;
}

static unsigned va_integer_width(USHORT type)
{
    switch (type) {
    case 16: case 17: return 8;
    case 2: case 18: return 16;
    case 3: case 19: case 22: case 23: return 32;
    case 20: case 21: return 64;
    default: return 0;
    }
}
static BOOL va_unsigned(USHORT type)
{ return type == 17 || type == 18 || type == 19 || type == 21 || type == 23; }

static HRESULT va_integer(BYTE *output, VA_NUMBER number, USHORT target)
{
    USHORT origin = number.origin;
    unsigned width = va_integer_width(target);
    /* Native VarI8FromCy rounds negative currency down even at integral
     * boundaries. Keep this ABI quirk separate from normal ties-to-even. */
    if (origin == 6 && target == 20 && number.negative) {
        uint64_t magnitude = (uint64_t)number.limb[1] << 32 | number.limb[0];
        *(USHORT *)output = target;
        *(uint64_t *)(output + 8) = 0ULL - (magnitude / 10000 + 1);
        return S_OK;
    }
    HRESULT status = number.floating ? va_float_integer(number.real, 0, &number)
                                     : va_rescale(&number, 0);
    if (status != S_OK) return status;
    if (number.limb[2]) return VA_OVERFLOW;
    uint64_t value = (uint64_t)number.limb[1] << 32 | number.limb[0];
    uint64_t mask = width == 64 ? UINT64_MAX : (1ULL << width) - 1;
    BOOL reinterpret = va_integer_width(origin) == width;
    if (origin == 11 && (va_unsigned(target) || target == 16)) {
        if (number.negative) value = 0ULL - value;
        value &= mask;
    } else if (reinterpret) {
        if (number.negative) value = 0ULL - value;
        value &= mask;
    } else if (va_unsigned(target)) {
        if ((number.negative && value) || value > mask) return VA_OVERFLOW;
    } else {
        uint64_t limit = 1ULL << (width - 1);
        if (value > limit || (!number.negative && value == limit)) return VA_OVERFLOW;
        if (number.negative) value = (0ULL - value) & mask;
    }
    *(USHORT *)output = target;
    for (unsigned i = 0; i < width / 8; i++) output[8 + i] = (BYTE)(value >> (8 * i));
    return S_OK;
}

static HRESULT va_change(BYTE *output, const BYTE *source, USHORT flags,
                         USHORT target, unsigned depth);

/* Dispatch default-value invocation and QI use guest-width vtables. The
 * argument/result block must be mapped in PE32 before a callback enters it. */
static HRESULT va_interface(BYTE *output, const BYTE *source, USHORT flags,
                            USHORT target, unsigned depth)
{
    PVOID object = va_pointer(source + 8);
    if (!object) {
        if (target != 9 && target != 13) return VA_TYPE;
        *(USHORT *)output = target;return S_OK;
    }
    BYTE *scratch = shim_CoTaskMemAlloc(128);
    if (!scratch) return VA_MEMORY;
    for (unsigned i = 0; i < 128; i++) scratch[i] = 0;
    HRESULT status;
    if (target == 9 || target == 13) {
        GUID *iid = (GUID *)scratch;
        iid->Data1 = target == 9 ? 0x20400 : 0;
        iid->Data4[0] = 0xc0;iid->Data4[7] = 0x46;
        if (g_compat32_mode) {
            uint32_t vtable = *(uint32_t *)object;
            uint32_t args[3] = {(uint32_t)(ULONG_PTR)object,
                (uint32_t)(ULONG_PTR)scratch,(uint32_t)(ULONG_PTR)(scratch + 16)};
            status = (HRESULT)compat32_callback_args(*(uint32_t *)(ULONG_PTR)vtable, 3, args);
        } else {
            typedef HRESULT (WINAPI *QI)(PVOID, PCVOID, PVOID *);
            status = ((QI)(*(PVOID **)object)[0])(object, iid, (PVOID *)(scratch + 16));
        }
        if (status == S_OK) {
            *(USHORT *)output = target;va_set_pointer(output + 8, va_pointer(scratch + 16));
        }
    } else if (*(const USHORT *)source == 9 && !(flags & 1)) {
        if (g_compat32_mode) {
            uint32_t vtable = *(uint32_t *)object;
            uint32_t invoke = ((uint32_t *)(ULONG_PTR)vtable)[6];
            uint32_t args[9] = {(uint32_t)(ULONG_PTR)object,0,
                (uint32_t)(ULONG_PTR)scratch,0x409,2,
                (uint32_t)(ULONG_PTR)(scratch + 16),
                (uint32_t)(ULONG_PTR)(scratch + 40),0,0};
            status = (HRESULT)compat32_callback_args(invoke, 9, args);
        } else {
            typedef HRESULT (WINAPI *INVOKE)(PVOID,LONG,PCVOID,DWORD,USHORT,PVOID,PVOID,PVOID,PVOID);
            status = ((INVOKE)(*(PVOID **)object)[6])(object,0,scratch,0x409,2,
                scratch + 16,scratch + 40,NULL,NULL);
        }
        if (status == S_OK) status = va_change(output, scratch + 40, flags | 1, target, depth + 1);
        (void)shim_VariantClear(scratch + 40);
    } else status = VA_TYPE;
    shim_CoTaskMemFree(scratch);
    return status;
}

static HRESULT va_change(BYTE *output, const BYTE *source, USHORT flags,
                         USHORT target, unsigned depth)
{
    if (depth >= 16) return VA_INVALID;
    USHORT type = *(const USHORT *)source;
    if (!va_valid(type) || !va_valid(target)) return VA_BADTYPE;
    if (target & 0x4000) return VA_TYPE;
    if (type & 0x4000) {
        const BYTE *p = va_pointer(source + 8);
        if (!p) return VA_TYPE;
        USHORT base = type & ~0x4000;
        if (base == 12) {
            if (*(const USHORT *)p == 0x400c) return VA_INVALID;
            return va_change(output, p, flags, target, depth + 1);
        }
        BYTE value[24] = {0};*(USHORT *)value = base;
        unsigned width = base == 16 || base == 17 ? 1 :
            base == 2 || base == 11 || base == 18 ? 2 :
            base == 3 || base == 4 || base == 10 || base == 19 || base == 22 || base == 23 ? 4 : 8;
        if (base == 14) {
            for (unsigned i = 0; i < 16; i++) value[i] = p[i];
            *(USHORT *)value = base;
        } else if ((base & 0x2000) || base == 8 || base == 9 || base == 13) {
            va_set_pointer(value + 8, va_pointer(p));
        } else for (unsigned i = 0; i < width; i++) value[8 + i] = p[i];
        return va_change(output, value, flags, target, depth + 1);
    }
    if (type == 12 || (type == 10 && target <= 1) ||
        (type == 22 && (target == 20 || target == 21))) return VA_TYPE;
    if (type == target) {
        if (type == 8 && !va_pointer(source + 8)) {
            PWSTR empty = shim_SysAllocStringLen(NULL, 0);
            if (!empty) return VA_MEMORY;
            *(USHORT *)output = 8;va_set_pointer(output + 8, empty);return S_OK;
        }
        return shim_VariantCopy(output, source);
    }
    if ((type | target) & 0x2000) return VA_TYPE;
    if (target == 1 || (target == 0 && type != 1)) {
        *(USHORT *)output = target;return S_OK;
    }
    if (type == 1) return VA_TYPE;
    if (type == 9 || type == 13) return va_interface(output, source, flags, target, depth);
    if (target == 8) return va_bstr(output, source, flags);
    if (target == 10 || target == 12 || target == 36 || target == 9 || target == 13) return VA_TYPE;
    if (target == 7 && type == 8) {
        HRESULT status = va_parse_date(va_pointer(source + 8), (double *)(output + 8));
        if (status == S_OK) *(USHORT *)output = 7;
        return status;
    }
    if (target == 11 && type == 8) {
        const WCHAR *s = va_pointer(source + 8);
        if (!s) return VA_INVALID;
        if (s) {
            while (va_space(*s)) s++;
            const char *words[2] = {"false","true"};
            for (unsigned w = 0; w < 2; w++) {
                unsigned i = 0;
                while (words[w][i] && (s[i] | 32) == (WCHAR)words[w][i]) i++;
                const WCHAR *tail = s + i;while (va_space(*tail)) tail++;
                if (!words[w][i] && !*tail) {
                    *(USHORT *)output = 11;*(USHORT *)(output + 8) = w ? 0xffff : 0;return S_OK;
                }
            }
        }
    }
    VA_NUMBER number = {0};HRESULT status = va_number(source, &number);
    if (status != S_OK) return status;
    if (va_integer_width(target)) return va_integer(output, number, target);
    if (target == 11) {
        *(USHORT *)output = target;
        *(USHORT *)(output + 8) = (number.floating || type == 8 ? va_double(&number) != 0 : va_nonzero(&number)) ? 0xffff : 0;
        return S_OK;
    }
    if (target == 4 || target == 5 || target == 7) {
        double value = va_double(&number);
        if (target == 7 && (value <= -657435.0 || value >= 2958466.0)) return VA_OVERFLOW;
        if (target == 4) {
            if (!(value <= 3.4028234663852886e38 && value >= -3.4028234663852886e38)) return VA_OVERFLOW;
            *(float *)(output + 8) = (float)value;
        } else *(double *)(output + 8) = value;
        *(USHORT *)output = target;return S_OK;
    }
    if (target == 6) {
        if (!g_compat32_mode && number.floating && number.real.real != number.real.real) {
            *(USHORT *)output = 6;*(uint64_t *)(output + 8) = 0x8000000000000000ULL;
            return S_OK;
        }
        status = number.floating ? va_float_integer(number.real, 4, &number) : va_rescale(&number, 4);
        if (status != S_OK) return status;
        uint64_t value = (uint64_t)number.limb[1] << 32 | number.limb[0];
        if (number.limb[2] || value > 0x8000000000000000ULL ||
            (!number.negative && value == 0x8000000000000000ULL)) return VA_OVERFLOW;
        *(uint64_t *)(output + 8) = number.negative ? 0ULL - value : value;
        *(USHORT *)output = 6;return S_OK;
    }
    if (target == 14) {
        if (number.floating) {
            char text[128];WCHAR wide[128];
            status = va_float_text(number.real, type == 4 ? 7 : 15, text);
            if (status != S_OK) return status;
            unsigned i = 0;do { wide[i] = (BYTE)text[i]; } while (text[i++]);
            VA_NUMBER decimal = {0};status = va_parse_number(wide, &decimal);
            if (status != S_OK || decimal.floating) return VA_OVERFLOW;
            number = decimal;
        }
        if (number.scale > 28 && va_rescale(&number, 28) != S_OK) return VA_OVERFLOW;
        if (!va_nonzero(&number) && type != 6) number.scale = 0;
        *(USHORT *)output = 14;output[2] = (BYTE)number.scale;
        output[3] = number.negative && va_nonzero(&number) ? 0x80 : 0;
        *(uint32_t *)(output + 4) = number.limb[2];
        *(uint32_t *)(output + 8) = number.limb[0];
        *(uint32_t *)(output + 12) = number.limb[1];return S_OK;
    }
    return VA_TYPE;
}

HRESULT WINAPI shim_VariantChangeType(PVOID destination, PCVOID source,
                                      USHORT flags, USHORT type)
{
    if (!destination || !source) return VA_INVALID;
    DWORD saved_error = GetLastError();
    BYTE result[24];va_zero(result);
    HRESULT status = va_change(result, source, flags, type, 0);
    if (status == S_OK) {
        status = shim_VariantClear(destination);
        if (status == S_OK) {
            for (SIZE_T i = 0; i < va_size(); i++) ((BYTE *)destination)[i] = result[i];
        } else (void)shim_VariantClear(result);
    }
    SetLastError(saved_error);
    return status;
}
