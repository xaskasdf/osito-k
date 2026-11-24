/* Compile with the generated setupapi_inf_parser.inc containing the production
 * parser helpers. These tests only read INF text; no setup payload is executed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned DWORD;
typedef unsigned UINT;
#include "../win32/system_paths.h"
#define TRUE 1
#define FALSE 0
#define kmalloc malloc
#define kfree free
static DWORD last_error;
#define SetLastError(value) (last_error = (value))
typedef struct { char *section, *key, *value; } SETUP_LINE;
typedef struct { char *text; SETUP_LINE *lines; unsigned count; char directory[260]; } SETUP_OBJECT;
#include "setupapi_inf_parser.inc"

static unsigned checks;
#define CHECK(c) inf_host_check((c), #c, __LINE__)
static void inf_host_check(int ok, const char *expression, unsigned line)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %u: %s\n", line, expression); exit(1); }
}
static void inf_host_parse(const char *input, size_t size, int query)
{
    SETUP_OBJECT inf = {0};
    inf.text = malloc(size + 1); CHECK(inf.text != NULL);
    memcpy(inf.text, input, size); inf.text[size] = 0;
    UINT error_line = 0;
    if (setup_inf_parse(&inf, &error_line)) {
        CHECK(inf.count < INF_MAX_LINES);
        for (unsigned i = 0; i < inf.count; i++) {
            CHECK(inf.lines[i].section >= inf.text && inf.lines[i].section <= inf.text + size);
            CHECK(inf.lines[i].value >= inf.text && inf.lines[i].value <= inf.text + size);
            if (inf.lines[i].key) CHECK(inf.lines[i].key >= inf.text && inf.lines[i].key <= inf.text + size);
            if (query) {
                unsigned length;
                char *text = setup_inf_compact(&inf, i, &length);
                if (text) { CHECK(length == strlen(text) + 1); CHECK(length <= INF_QUERY_LIMIT + 1); free(text); }
            }
        }
    }
    free(inf.lines); free(inf.text);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb"); CHECK(file != NULL);
    char input[32768]; size_t size = fread(input, 1, sizeof(input) - 1, file);
    CHECK(!ferror(file)); fclose(file); input[size] = 0;
    SETUP_OBJECT inf = {.text = input}; UINT error_line = 0;
    CHECK(setup_inf_parse(&inf, &error_line));
    const char *expected[] = {"first,second, part,value,with spaces,,", "one", "two",
        "bare,quoted, piece,last", "", "alpha,", "beta,gamma", "say \"hi\", space ",
        "%,%Unknown%,%Sub%", "alpha,beta,gamma", "abcd", "abcd", "a\\", "has key", "end"};
    unsigned count = 0;
    for (unsigned i = 0; i < inf.count; i++) if (setup_inf_eq(inf.lines[i].section, "Main")) {
        unsigned length; char *text = setup_inf_compact(&inf, i, &length);
        CHECK(text != NULL); CHECK(count < sizeof(expected)/sizeof(*expected));
        CHECK(strcmp(text, expected[count++]) == 0); CHECK(length == strlen(text) + 1); free(text);
    }
    CHECK(count == sizeof(expected)/sizeof(*expected));
    CHECK(setup_inf_find(&inf, "Main", "Quoted Key") != NULL);
    char field[INF_PATH];
    CHECK(setup_inf_field("a,\" b \"", 1, field) && strcmp(field, " b ") == 0);
    CHECK(setup_inf_field("a,b", 9, field) && !field[0]);
    CHECK(setup_inf_expand(&inf, "%Nested%", field, 0) && strcmp(field, "%Sub%") == 0);
    free(inf.lines);
    const char bad[] = "[Main]\nJoined=x,\\\n y\n[broken\n";
    char copy[sizeof(bad)]; memcpy(copy, bad, sizeof(bad));
    inf = (SETUP_OBJECT){.text = copy};
    CHECK(!setup_inf_parse(&inf, &error_line)); CHECK(error_line == 4); CHECK(last_error == INF_BAD_SECTION); free(inf.lines);
    char field_input[4098], field_output[4098];
    memset(field_input, 'a', sizeof(field_input)); field_input[4095] = 0;
    const char *cursor = field_input;
    CHECK(setup_inf_next_field(&cursor, field_output, 4096) && strlen(field_output) == 4095);
    field_input[4095] = 'a'; field_input[4096] = 0; cursor = field_input;
    CHECK(!setup_inf_next_field(&cursor, field_output, 4096)); CHECK(last_error == 206);
    uint32_t random = 0x3175ab91;
    const char prefix[] = "[Strings]\nSub=value\n[Main]\n";
    for (unsigned run = 0; run < 10000; run++) {
        char fuzz[1024]; memcpy(fuzz, prefix, sizeof(prefix)-1);
        random = random * 1664525U + 1013904223U;
        size_t n = sizeof(prefix)-1 + random % (sizeof(fuzz)-sizeof(prefix));
        const char alphabet[] = "abcXYZ0123[];=\"%\\, \t\r\n";
        for (size_t i = sizeof(prefix)-1; i < n; i++) {
            random = random * 1664525U + 1013904223U;
            fuzz[i] = alphabet[random % (sizeof(alphabet)-1)];
        }
        inf_host_parse(fuzz, n, 1);
    }
    printf("INF HOST PASS %u checks, 10000 malformed inputs\n", checks);
    return 0;
}
