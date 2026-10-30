/* Windows Installer condition expressions. Values remain distinct from
 * booleans and quoted literals from properties: Windows does not coerce a
 * quoted literal to a number even when its contents are decimal digits. */
#include "msi_condition.h"
#include "kernel32_shim.h"
extern int WINAPI CompareStringW(DWORD locale, DWORD flags, PCWSTR a, int na, PCWSTR b, int nb);

enum {
    TK_END, TK_BAD, TK_VALUE, TK_LEFT, TK_RIGHT, TK_NOT,
    TK_AND, TK_OR, TK_XOR, TK_EQV, TK_IMP,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_CONTAINS, TK_STARTS, TK_ENDS
};
enum { VALUE_INTEGER, VALUE_LITERAL, VALUE_PROPERTY };
typedef struct {
    const uint16_t *text;
    uint32_t length;
    int64_t integer;
    int kind;
} condition_value_t;
typedef struct {
    const uint16_t *cursor;
    msi_condition_lookup_t lookup;
    void *context;
    condition_value_t value;
    int token, ignore_case, error;
    unsigned depth;
} condition_parser_t;

static int condition_letter(uint16_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int condition_digit(uint16_t c) { return c >= '0' && c <= '9'; }
static int condition_keyword(const uint16_t *text, uint32_t length, const char *word)
{
    uint32_t i = 0;
    for (; i < length && word[i]; i++) {
        uint16_t c = text[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c != (uint16_t)word[i]) return 0;
    }
    return i == length && !word[i];
}
static int condition_integer(const uint16_t *text, uint32_t length, int64_t *out)
{
    if (!length) return 0;
    uint32_t i = text[0] == '-' ? 1U : 0U;
    int negative = i != 0;
    if (i == length) return 0;
    uint64_t number = 0;
    for (; i < length; i++) {
        if (!condition_digit(text[i])) return 0;
        unsigned digit = text[i] - '0';
        number = number * 10 + digit;
    }
    if (number > 0x7fffffffffffffffULL) return 0;
    *out = negative ? (int64_t)(0ULL - number) : (int64_t)number;
    return 1;
}
static void condition_next(condition_parser_t *p)
{
    const uint16_t *s = p->cursor;
    while (*s == ' ' || *s == '\t') s++;
    p->ignore_case = 0;
    p->token = TK_BAD;
    if (!*s) { p->cursor = s; p->token = TK_END; return; }
    uint16_t c = *s++;
    if (c == '(') p->token = TK_LEFT;
    else if (c == ')') p->token = TK_RIGHT;
    else if (c == '"') {
        p->value = (condition_value_t){.text = s, .kind = VALUE_LITERAL};
        while (*s && *s != '"') s++;
        p->value.length = (uint32_t)(s - p->value.text);
        if (*s) { s++; p->token = TK_VALUE; }
    } else if (condition_digit(c) || (c == '-' && condition_digit(*s))) {
        const uint16_t *start = s - 1;
        while (condition_digit(*s)) s++;
        p->value = (condition_value_t){.kind = VALUE_INTEGER};
        if (condition_integer(start, (uint32_t)(s - start), &p->value.integer)) {
            if (p->value.integer == -2147483647LL - 1)
                p->value = (condition_value_t){.kind = VALUE_PROPERTY}; /* MSI_NULL_INTEGER */
            p->token = TK_VALUE;
        }
    } else if (condition_letter(c) || c == '%' || c == '$' || c == '?' || c == '&' || c == '!') {
        uint16_t prefix = condition_letter(c) ? 0 : c;
        const uint16_t *start = prefix ? s : s - 1;
        if (!condition_letter(*start)) { p->cursor = s; return; }
        s = start + 1;
        while (condition_letter(*s) || condition_digit(*s) || *s == '.') s++;
        uint32_t length = (uint32_t)(s - start);
        if (!prefix && condition_keyword(start, length, "NOT")) p->token = TK_NOT;
        else if (!prefix && condition_keyword(start, length, "AND")) p->token = TK_AND;
        else if (!prefix && condition_keyword(start, length, "OR")) p->token = TK_OR;
        else if (!prefix && condition_keyword(start, length, "XOR")) p->token = TK_XOR;
        else if (!prefix && condition_keyword(start, length, "EQV")) p->token = TK_EQV;
        else if (!prefix && condition_keyword(start, length, "IMP")) p->token = TK_IMP;
        else {
            msi_condition_value_t value = {0};
            if (p->lookup && !p->lookup(p->context, prefix, start, length, &value)) p->error = 1;
            if (value.length && !value.text && !value.is_integer) { p->error = 1; value.length = 0; }
            p->value = (condition_value_t){.text = value.text, .length = value.length,
                .integer = value.integer, .kind = value.is_integer ? VALUE_INTEGER : VALUE_PROPERTY};
            p->token = TK_VALUE;
        }
    } else {
        if (c == '~') { p->ignore_case = 1; c = *s; if (*s) s++; }
        if (c == '=') p->token = TK_EQ;
        else if (c == '<') {
            if (*s == '>') { s++; p->token = TK_NE; }
            else if (*s == '=') { s++; p->token = TK_LE; }
            else if (*s == '<') { s++; p->token = TK_STARTS; }
            else p->token = TK_LT;
        } else if (c == '>') {
            if (*s == '=') { s++; p->token = TK_GE; }
            else if (*s == '<') { s++; p->token = TK_CONTAINS; }
            else if (*s == '>') { s++; p->token = TK_ENDS; }
            else p->token = TK_GT;
        }
    }
    p->cursor = s;
}
static int condition_compare_numbers(int64_t a, int64_t b, int token)
{
    switch (token) {
    case TK_EQ: return a == b;
    case TK_NE: return a != b;
    case TK_LT: return a < b;
    case TK_LE: return a <= b;
    case TK_GT: return a > b;
    case TK_GE: return a >= b;
    case TK_CONTAINS: return ((uint64_t)a & (uint64_t)b) != 0;
    case TK_STARTS: return (a >> 16) == b;
    case TK_ENDS: return (a & 0xffff) == b;
    default: return 0;
    }
}
static int condition_compare_strings(condition_value_t a, condition_value_t b, int token, int fold)
{
    static const uint16_t empty[] = {0};
    if (!a.text) a.text = empty;
    if (!b.text) b.text = empty;
    if (token == TK_CONTAINS || token == TK_STARTS || token == TK_ENDS) {
        if (!a.length || b.length > a.length) return 0;
        uint32_t first = token == TK_ENDS ? a.length - b.length : 0;
        uint32_t last = token == TK_CONTAINS ? a.length - b.length : first;
        for (uint32_t i = first; i <= last; i++)
            if (CompareStringOrdinal(a.text + i, (int)b.length, b.text, (int)b.length, fold) == 2) return 1;
        return 0;
    }
    /* MSI uses linguistic ordering for the case-insensitive relational
     * operators; substring matching only needs equality. */
    int order = (fold ? CompareStringW(0x400, 1, a.text, (int)a.length, b.text, (int)b.length)
                      : CompareStringOrdinal(a.text, (int)a.length, b.text, (int)b.length, 0)) - 2;
    return condition_compare_numbers(order, 0, token);
}
static int condition_compare(condition_value_t a, condition_value_t b, int token, int fold)
{
    int64_t number, other;
    if (a.kind == VALUE_PROPERTY && b.kind == VALUE_PROPERTY &&
        condition_integer(a.text, a.length, &number) && number != -2147483647LL - 1 &&
        condition_integer(b.text, b.length, &other) && other != -2147483647LL - 1)
        return condition_compare_numbers(number, other, token);
    if (a.kind == VALUE_INTEGER && b.kind == VALUE_PROPERTY &&
        condition_integer(b.text, b.length, &number) && number != -2147483647LL - 1)
        b = (condition_value_t){.kind = VALUE_INTEGER, .integer = number};
    if (b.kind == VALUE_INTEGER && a.kind == VALUE_PROPERTY &&
        condition_integer(a.text, a.length, &number) && number != -2147483647LL - 1)
        a = (condition_value_t){.kind = VALUE_INTEGER, .integer = number};
    if (a.kind == VALUE_INTEGER && b.kind == VALUE_INTEGER)
        return condition_compare_numbers(a.integer, b.integer, token);
    if (a.kind == VALUE_INTEGER || b.kind == VALUE_INTEGER) return token == TK_NE;
    return condition_compare_strings(a, b, token, fold);
}
static int condition_boolean(condition_parser_t *p, int minimum);
static int condition_term(condition_parser_t *p)
{
    if (++p->depth > 128) { p->error = 1; p->depth--; return 0; }
    int result = 0;
    if (p->token == TK_NOT) {
        condition_next(p); result = !condition_term(p);
    } else if (p->token == TK_LEFT) {
        condition_next(p); result = condition_boolean(p, 1);
        if (p->token != TK_RIGHT) p->error = 1;
        else condition_next(p);
    } else if (p->token == TK_VALUE) {
        condition_value_t a = p->value;
        condition_next(p);
        if (p->token >= TK_EQ && p->token <= TK_ENDS) {
            int token = p->token, fold = p->ignore_case;
            condition_next(p);
            if (p->token != TK_VALUE) p->error = 1;
            else { result = condition_compare(a, p->value, token, fold); condition_next(p); }
        } else result = a.kind == VALUE_INTEGER ? a.integer != 0 : a.length != 0;
    } else p->error = 1;
    p->depth--;
    return result;
}
static int condition_precedence(int token)
{
    switch (token) {
    case TK_AND: return 5;
    case TK_OR: return 4;
    case TK_XOR: return 3;
    case TK_EQV: return 2;
    case TK_IMP: return 1;
    default: return 0;
    }
}
static int condition_boolean(condition_parser_t *p, int minimum)
{
    int a = condition_term(p);
    while (!p->error && condition_precedence(p->token) >= minimum) {
        int token = p->token, level = condition_precedence(token);
        condition_next(p);
        int b = condition_boolean(p, level + 1);
        switch (token) {
        case TK_AND: a = a && b; break;
        case TK_OR: a = a || b; break;
        case TK_XOR: a = a != b; break;
        case TK_EQV: a = a == b; break;
        case TK_IMP: a = !a || b; break;
        }
    }
    return a;
}
int msi_condition_evaluate(const uint16_t *expression, msi_condition_lookup_t lookup, void *context)
{
    if (!expression) return 2;
    /* An initial empty parenthesized expression is MSI's empty-condition
     * result, including when further tokens follow it. */
    const uint16_t *start = expression;
    unsigned opening = 0;
    while (*start == ' ' || *start == '\t' || *start == '(') {
        if (*start == '(') opening++;
        start++;
    }
    if (opening && *start == ')') return 2;
    condition_parser_t parser = {.cursor = expression, .lookup = lookup, .context = context};
    condition_next(&parser);
    if (parser.token == TK_END) return 2;
    int result = condition_boolean(&parser, 1);
    return parser.error || parser.token != TK_END ? 3 : result;
}
