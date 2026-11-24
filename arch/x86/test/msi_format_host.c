#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_format.h"

static uint16_t *format_test_text(FILE *file, uint32_t *length)
{
    *length = get_number(file); CHECK(*length < 1048576);
    uint16_t *text = calloc(*length + 1, 2); CHECK(text != NULL);
    CHECK(fread(text, 2, *length, file) == *length); return text;
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    uint32_t size, error; uint8_t *data = read_file(argv[1], &size);
    msi_db_t *db = msi_open(data, size); CHECK(db != NULL);
    msi_package_t *package = msi_package_create(db, &error); CHECK(package && !error);
    FILE *file = fopen(argv[2], "rb"); CHECK(file && get_number(file) == 0x31544d46U);
    uint32_t properties = get_number(file);
    for (uint32_t i = 0; i < properties; i++) {
        uint16_t *key = get_text(file), *value = get_text(file);
        CHECK(msi_package_set(package, key, value) == 0);
        CHECK(equal_wide(msi_package_get(package, key), value)); free(key); free(value);
    }
    uint32_t cases = get_number(file), matched = 0, unsupported = 0;
    unsigned baseline = allocations;
    for (uint32_t i = 0; i < cases; i++) {
        uint32_t in_count, expected_count;
        uint16_t *input = format_test_text(file, &in_count);
        uint16_t *expected = format_test_text(file, &expected_count), *actual = NULL;
        uint32_t actual_count;
        error = msi_format_properties(package, input, &actual, &actual_count);
        int partial = 0;
        for (uint32_t c = 0; c < in_count; c++) {
            if (input[c] == '{' || input[c] == '}') partial = 1;
            if (input[c] == '[' && c + 1 < in_count &&
                (input[c+1] == '%' || input[c+1] == '#' || input[c+1] == '$' || input[c+1] == '!' ||
                (input[c+1] >= '0' && input[c+1] <= '9'))) partial = 1;
        }
        if (partial) { CHECK(error == 120 && actual == NULL && actual_count == 0); unsupported++; }
        else {
            if (error || expected_count != actual_count || memcmp(expected, actual, expected_count * 2U)) {
                fprintf(stderr, "format case %u mismatch error=%u actual=%u expected=%u\n", i, error, actual_count, expected_count);
                CHECK(0);
            }
            CHECK(actual[actual_count] == 0); matched++; kfree(actual);
        }
        for (int allocation = 0; allocation < 6; allocation++) {
            fail_after = allocation;
            error = msi_format_properties(package, input, &actual, &actual_count);
            if (!error) { CHECK(actual != NULL); kfree(actual); }
            else CHECK(actual == NULL && actual_count == 0);
            CHECK(allocations == baseline);
        }
        fail_after = -1; free(input); free(expected);
    }
    CHECK(fgetc(file) == EOF); fclose(file);
    msi_package_close(package); msi_close(db); free(data);
    CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI FORMAT PASS native_matches=%u unsupported=%u checks=%u\n", matched, unsupported, checks);
    return 0;
}
