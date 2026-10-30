#define MSI_PACKAGE_ADAPTERS_ONLY
#define MultiByteToWideChar summary_cp1252_adapter
#include "msi_package_host.c"
#undef MultiByteToWideChar
#include "../win32/msi_summary.h"

/* The oracle's text is CP1252. Mutated unsupported codepages fail conversion;
 * they are parser robustness inputs, not additional NLS compatibility cases. */
int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, PCSTR input, int bytes, PWSTR output, int units)
{
    if (cp != 0 && cp != 1252) return 0;
    return summary_cp1252_adapter(cp, flags, input, bytes, output, units);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    uint32_t size, error; uint8_t *data = read_file(argv[1], &size);
    msi_db_t *db = msi_open(data, size); CHECK(db != NULL);
    msi_summary_t *summary = msi_summary_read(db, &error); CHECK(summary && !error);
    FILE *file = fopen(argv[2], "rb"); CHECK(file && get_number(file) == 0x314d5553U);
    CHECK(summary->codepage == get_number(file)); CHECK(summary->installer_version == get_number(file));
    CHECK(summary->word_count == get_number(file));
    uint16_t *platform = get_text(file), *package_code = get_text(file);
    CHECK(equal_wide(summary->platform, platform)); CHECK(equal_wide(summary->package_code, package_code));
    free(platform); free(package_code); CHECK(fgetc(file) == EOF); fclose(file);
    msi_summary_close(summary);
    uint32_t stream_size; uint8_t *stream = msi_read_stream(db, "\005SummaryInformation", &stream_size); CHECK(stream != NULL);
    unsigned baseline = allocations;
    for (uint32_t i = 0; i < stream_size; i++) {
        uint8_t *short_stream = malloc(i ? i : 1); CHECK(short_stream != NULL);
        if (i) memcpy(short_stream, stream, i);
        summary = msi_summary_parse(short_stream, i, &error); CHECK(!summary && error != 0);
        free(short_stream); CHECK(allocations == baseline);
    }
    uint32_t random = 0x553113U;
    for (unsigned i = 0; i < 1200; i++) {
        uint8_t *mutation = malloc(stream_size); CHECK(mutation != NULL); memcpy(mutation, stream, stream_size);
        random = random * 1664525U + 1013904223U; uint32_t at = random % stream_size;
        random = random * 1664525U + 1013904223U; mutation[at] ^= (uint8_t)((random >> 24) | 1U);
        summary = msi_summary_parse(mutation, stream_size, &error);
        if (summary) { CHECK(!error); msi_summary_close(summary); } else CHECK(error != 0);
        free(mutation); CHECK(allocations == baseline);
    }
    for (int i = 0; i < 30; i++) {
        fail_after = i; summary = msi_summary_parse(stream, stream_size, &error);
        if (summary) { CHECK(!error); msi_summary_close(summary); } else CHECK(error != 0);
        CHECK(allocations == baseline);
    }
    fail_after = -1; kfree(stream); msi_close(db); free(data);
    CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI SUMMARY PASS bytes=%u mutations=1200 checks=%u\n", stream_size, checks);
    return 0;
}
