/* Data-only CAB extraction oracle. Compares each member against expand.exe
 * output; never loads or executes the original installer or extracted DLLs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define OSITOK_X86_TYPES_H
#include "../win32/cab.c"
#include "../win32/cab_lzx.c"
#include "../kernel/zlib.c"

static unsigned allocations, checks;
static int fail_after = -1;
#define CHECK(c) lzx_host_check((c), #c, __LINE__)
static void lzx_host_check(int condition, const char *name, int line)
{
    checks++;
    if (!condition) { fprintf(stderr, "FAIL %d: %s\n", line, name); exit(1); }
}
void *kmalloc(uint64_t size)
{
    if (size > 64U * 1024 * 1024 || fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc((size_t)size);
    if (p) allocations++;
    return p;
}
void kfree(void *pointer)
{
    if (pointer) { CHECK(allocations != 0); allocations--; free(pointer); }
}
void serial_puts(const char *text) { (void)text; }
static uint8_t *lzx_host_load(const char *path, uint32_t *length)
{
    FILE *f = fopen(path, "rb"); CHECK(f != NULL);
    CHECK(!fseek(f, 0, SEEK_END)); long n = ftell(f); rewind(f);
    CHECK(n >= 0 && n < 64 * 1024 * 1024);
    uint8_t *data = malloc(n ? (size_t)n : 1); CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)n, f) == (size_t)n); fclose(f);
    *length = (uint32_t)n; return data;
}
static void lzx_host_compare(cab_archive_t *c, int index, const char *directory)
{
    const char *name = cab_file_name(c, index);
    CHECK(name != NULL);
    char path[4096]; CHECK(snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path));
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    uint32_t expected_size, actual_size = 0;
    uint8_t *expected = lzx_host_load(path, &expected_size);
    uint8_t *actual = cab_extract_index(c, index, &actual_size);
    if (!actual) fprintf(stderr, "member %d %s extraction failed\n", index, name);
    CHECK(actual != NULL); CHECK(actual_size == expected_size);
    if (memcmp(actual, expected, actual_size)) {
        uint32_t at = 0; while (at < actual_size && actual[at] == expected[at]) at++;
        fprintf(stderr, "member %s mismatch at %u: %02x expected %02x\n", name, at, actual[at], expected[at]);
        CHECK(0);
    }
    kfree(actual); free(expected);
}
static void lzx_host_mutate(const uint8_t *data, uint32_t size)
{
    cab_archive_t *cab = cab_open(data, size); CHECK(cab != NULL);
    const uint8_t *fe = folder_entry(cab, 0);
    unsigned window = rd16(fe + 6) >> 8;
    uint32_t at = rd32(fe);
    uint32_t count = rd16(data + at + 4), output_size = rd16(data + at + 6);
    const uint8_t *frame = data + at + 8 + cab->cbCFData;
    uint8_t *output = malloc(output_size); CHECK(output != NULL);
    cab_close(cab);
    /* Use exact allocations for every truncated prefix; ASan detects overread. */
    for (uint32_t cut = 0; cut < count; cut += cut < 200 ? 1 : 37) {
        uint8_t *input = malloc(cut ? cut : 1); CHECK(input != NULL);
        memcpy(input, frame, cut);
        cab_lzx_t *s = cab_lzx_create(window); CHECK(s != NULL);
        CHECK(cab_lzx_frame(s, input, cut, output, 0, output_size) != 0);
        cab_lzx_destroy(s); free(input); CHECK(allocations == 0);
    }
    uint8_t *input = malloc(count); CHECK(input != NULL);
    memcpy(input, frame, count);
    uint32_t random = 0x1abacafe;
    for (unsigned i = 0; i < 3000; i++) {
        random = random * 1664525U + 1013904223U;
        unsigned index = i < 512 ? i % count : random % count;
        uint8_t old = input[index]; input[index] ^= (uint8_t)(1U << (i & 7));
        cab_lzx_t *s = cab_lzx_create(window); CHECK(s != NULL);
        if (!cab_lzx_frame(s, input, count, output, 0, output_size)) cab_lzx_translate(s, output, output_size);
        cab_lzx_destroy(s); input[index] = old; CHECK(allocations == 0);
    }
    free(input); free(output);
}
int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    uint32_t size; uint8_t *data = lzx_host_load(argv[1], &size);
    cab_archive_t *c = cab_open(data, size); CHECK(c != NULL);
    int count = cab_file_count(c); CHECK(count > 0);
    for (int i = 0; i < count; i++) lzx_host_compare(c, i, argv[2]);
    for (int i = count - 1; i >= 0; i--) lzx_host_compare(c, i, argv[2]);
    cab_close(c); CHECK(allocations == 0);
    for (int i = 0; i < 7; i++) {
        fail_after = i; c = cab_open(data, size);
        if (c) { uint8_t *bytes = cab_extract_index(c, count - 1, NULL); kfree(bytes); cab_close(c); }
        fail_after = -1; CHECK(allocations == 0);
    }
    if (argc == 4) lzx_host_mutate(data, size);
    free(data); CHECK(allocations == 0);
    printf("CAB LZX HOST PASS %d members %u checks\n", count, checks);
    return 0;
}
