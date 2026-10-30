/* Grammar/coercion oracle. Linguistic comparison is supplied by captured
 * Windows NLS results; this deliberately does not qualify the kernel's NLS
 * tables. The fixture is data only: no MSI actions are executed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define OSITOK_X86_TYPES_H
#include "../win32/msi_condition.h"
#include "../win32/kernel32_shim.h"

typedef struct { uint16_t *text; uint32_t size; } text_t;
typedef struct { text_t name, value; } property_t;
typedef struct { text_t left, right; int result; } collation_t;
static property_t *properties;
static collation_t *collations;
static uint32_t property_count, collation_count;
static unsigned checks, fuzzing;
#define CHECK(x) check((x), #x, __LINE__)
static void check(int value, const char *expression, int line)
{
    checks++;
    if (!value) { fprintf(stderr, "FAIL %d: %s\n", line, expression); exit(1); }
}
static uint32_t read_number(FILE *f)
{
    uint8_t bytes[4]; CHECK(fread(bytes, 1, 4, f) == 4);
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static text_t read_text(FILE *f)
{
    text_t result = {.size = read_number(f)};
    if (result.size == UINT32_MAX) { result.size = 0; return result; }
    CHECK(result.size < 1048576);
    result.text = calloc((size_t)result.size + 1, 2); CHECK(result.text != NULL);
    CHECK(fread(result.text, 2, result.size, f) == result.size);
    return result;
}
static int same(text_t value, const uint16_t *text, uint32_t size)
{
    return value.size == size && (!size || !memcmp(value.text, text, size * 2U));
}
static int lookup(void *context, uint16_t prefix, const uint16_t *name, uint32_t length,
                  msi_condition_value_t *value)
{
    (void)context;
    if (!prefix) for (uint32_t i = 0; i < property_count; i++) if (same(properties[i].name, name, length)) {
        value->text = properties[i].value.text;
        value->length = properties[i].value.size;
        break;
    }
    return 1;
}
int WINAPI CompareStringOrdinal(PCWSTR a, int na, PCWSTR b, int nb, BOOL fold)
{
    for (int i = 0; i < na && i < nb; i++) {
        unsigned x = a[i], y = b[i];
        if (fold) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x < y) return 1;
        if (x > y) return 3;
    }
    return na < nb ? 1 : na > nb ? 3 : 2;
}
int WINAPI CompareStringW(DWORD locale, DWORD flags, PCWSTR a, int na, PCWSTR b, int nb)
{
    CHECK(locale == 0x400 && flags == 1 && na >= 0 && nb >= 0);
    for (uint32_t i = 0; i < collation_count; i++)
        if (same(collations[i].left, a, (uint32_t)na) && same(collations[i].right, b, (uint32_t)nb))
            return collations[i].result;
    CHECK(fuzzing != 0);
    return CompareStringOrdinal(a, na, b, nb, 1);
}
static int broken_lookup(void *context, uint16_t prefix, const uint16_t *name, uint32_t length,
                         msi_condition_value_t *value)
{
    (void)context; (void)prefix; (void)name; (void)length;
    value->length = 4; /* Must report an error instead of reading a NULL string. */
    return 0;
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "rb"); CHECK(f != NULL);
    CHECK(read_number(f) == 0x3143494dU); /* MIC1 */
    property_count = read_number(f); collation_count = read_number(f);
    uint32_t cases = read_number(f);
    CHECK(property_count < 1000 && collation_count < 10000 && cases < 1000000);
    properties = calloc(property_count, sizeof(*properties)); CHECK(properties != NULL);
    collations = calloc(collation_count, sizeof(*collations)); CHECK(collations != NULL);
    for (uint32_t i = 0; i < property_count; i++) {
        properties[i].name = read_text(f); properties[i].value = read_text(f);
    }
    for (uint32_t i = 0; i < collation_count; i++) {
        collations[i].left = read_text(f); collations[i].right = read_text(f);
        collations[i].result = (int)read_number(f);
    }
    for (uint32_t i = 0; i < cases; i++) {
        text_t condition = read_text(f); int expected = (int)read_number(f);
        int result = msi_condition_evaluate(condition.text, lookup, NULL);
        if (result != expected) fprintf(stderr, "case %u: actual=%d expected=%d\n", i, result, expected);
        CHECK(result == expected); free(condition.text);
    }
    CHECK(fgetc(f) == EOF); fclose(f);
    fuzzing = 1;
    const uint16_t invalid_provider[] = {'X','=', '1',0};
    CHECK(msi_condition_evaluate(invalid_provider, broken_lookup, NULL) == 3);
    uint16_t nested[4098];
    for (unsigned n = 1; n <= 2048; n *= 2) {
        for (unsigned i = 0; i < n; i++) nested[i] = '(';
        nested[n] = '1';
        for (unsigned i = 0; i < n; i++) nested[n + 1 + i] = ')';
        nested[n * 2 + 1] = 0;
        CHECK(msi_condition_evaluate(nested, lookup, NULL) == (n < 128 ? 1 : 3));
    }
    static const char alphabet[] = " \t\r\n0123456789-+<>~=()\"ANDORNOTX_EQVIMP.$%&!?";
    uint32_t random = 0x918fadeU;
    for (unsigned i = 0; i < 20000; i++) {
        random = random * 1664525U + 1013904223U;
        unsigned length = random % 256;
        uint16_t *text = malloc((length + 1) * 2U); CHECK(text != NULL);
        for (unsigned j = 0; j < length; j++) {
            random = random * 1664525U + 1013904223U;
            text[j] = (uint16_t)alphabet[random % (sizeof(alphabet) - 1)];
        }
        text[length] = 0;
        int result = msi_condition_evaluate(text, lookup, NULL); CHECK(result >= 0 && result <= 3);
        free(text);
    }
    for (uint32_t i = 0; i < property_count; i++) { free(properties[i].name.text); free(properties[i].value.text); }
    for (uint32_t i = 0; i < collation_count; i++) { free(collations[i].left.text); free(collations[i].right.text); }
    free(properties); free(collations);
    printf("MSI CONDITIONS PASS cases=%u malformed=20000 checks=%u\n", cases, checks);
    return 0;
}
