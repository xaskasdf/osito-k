#include "../win32/crt_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned checks;

static void check_case(const void *input, size_t input_units, unsigned width)
{
    CRT_ARG_COUNTS counted, written;
    CHECK(!crt_args_parse(input, input_units, width, NULL, 0, NULL, 0, &counted));
    CHECK(counted.argc && counted.units && counted.units <= input_units);
    void *strings = malloc((size_t)counted.units * width);
    CRT_ARG_RECORD *records = malloc(counted.argc * sizeof(*records));
    CHECK(strings && records);
    CHECK(!crt_args_parse(input, input_units, width, strings, counted.units,
                          records, counted.argc, &written));
    CHECK(!memcmp(&written, &counted, sizeof(counted)));
    CHECK(!records[0].offset && !records[0].expand);
    for (unsigned i = 0; i < counted.argc; ++i) {
        CHECK(records[i].offset < counted.units);
        unsigned end = i + 1 < counted.argc ? records[i + 1].offset : counted.units;
        CHECK(end > records[i].offset && end <= counted.units);
        CHECK(width == 1 ? !((uint8_t *)strings)[end - 1] : !((uint16_t *)strings)[end - 1]);
    }
    CHECK(crt_args_parse(input, input_units, width, strings, counted.units - 1,
                         records, counted.argc, &written) < 0);
    CHECK(crt_args_parse(input, input_units, width, strings, counted.units,
                         records, counted.argc - 1, &written) < 0);
    free(strings); free(records);
}

int main(void)
{
    uint32_t random = 7;
    const uint16_t alphabet[] = {'a', ' ', '\t', '\\', '"', '*', '?', 0x3a9, 0xd83d, 0xde42};
    for (unsigned test = 0; test < 5000; ++test) {
        size_t length = test % 113;
        for (unsigned width = 1; width <= 2; ++width) {
            void *input = calloc(length + 1, width);
            for (size_t i = 0; i < length; ++i) {
                random = random * 1664525u + 1013904223u;
                uint16_t c = alphabet[(random >> 16) % (width == 1 ? 7 : 10)];
                if (width == 1) ((uint8_t *)input)[i] = (uint8_t)c;
                else ((uint16_t *)input)[i] = c;
            }
            check_case(input, length + 1, width);
            free(input);
        }
    }
    for (unsigned width = 1; width <= 2; ++width) {
        void *input = malloc(CRT_ARGS_MAX_UNITS * width);
        for (unsigned i = 0; i < CRT_ARGS_MAX_UNITS; ++i) {
            if (width == 1) ((uint8_t *)input)[i] = 'a';
            else ((uint16_t *)input)[i] = 'a';
        }
        CRT_ARG_COUNTS counts;
        CHECK(crt_args_parse(input, CRT_ARGS_MAX_UNITS, width, NULL, 0, NULL, 0, &counts) < 0);
        if (width == 1) ((uint8_t *)input)[CRT_ARGS_MAX_UNITS - 1] = 0;
        else ((uint16_t *)input)[CRT_ARGS_MAX_UNITS - 1] = 0;
        check_case(input, CRT_ARGS_MAX_UNITS, width);
        CHECK(crt_args_parse(input, CRT_ARGS_MAX_UNITS + 1, width, NULL, 0, NULL, 0, &counts) < 0);
        free(input);
    }
    printf("CRT ARGS HOST PASS checks=%u\n", checks);
    return 0;
}
