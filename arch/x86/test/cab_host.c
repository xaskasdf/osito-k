/* CAB parser/extraction regressions, including the name API used by MSI.
 * Input fixtures are data only; no extracted payload is executed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define OSITOK_X86_TYPES_H
#include "../win32/cab.c"
#include "../win32/cab_lzx.c"
#include "../kernel/zlib.c"

static unsigned checks, allocations;
static int fail_after = -1;
#define CHECK(c) cab_host_check((c), #c, __LINE__)
static void cab_host_check(int condition, const char *expression, int line)
{
    checks++;
    if (!condition) { fprintf(stderr, "FAIL line %d: %s\n", line, expression); exit(1); }
}
void *kmalloc(uint64_t size)
{
    if (size > 1048576 || fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc((size_t)size);
    if (p) allocations++;
    return p;
}
void kfree(void *p)
{
    if (p) { CHECK(allocations > 0); allocations--; free(p); }
}
void serial_puts(const char *s) { (void)s; }
static uint32_t cab_host_hash(const uint8_t *data, uint32_t size)
{
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 16777619U;
    return hash;
}
static uint8_t *cab_host_load(const char *directory, const char *name, uint32_t *length)
{
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path));
    FILE *file = fopen(path, "rb"); CHECK(file != NULL);
    CHECK(!fseek(file, 0, SEEK_END)); long size = ftell(file);
    CHECK(size > 0 && size < 1048576); rewind(file);
    uint8_t *data = malloc((size_t)size); CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)size, file) == (size_t)size); fclose(file);
    *length = (uint32_t)size; return data;
}
static void cab_host_extract(cab_archive_t *cab, int index, uint32_t size, uint32_t hash)
{
    uint32_t actual = 0;
    uint8_t *bytes = cab_extract_index(cab, index, &actual);
    CHECK(bytes != NULL); CHECK(actual == size); CHECK(cab_host_hash(bytes, actual) == hash);
    kfree(bytes);
    char uppercase[260]; const char *name = cab_file_name(cab, index);
    CHECK(name != NULL && strlen(name) < sizeof(uppercase));
    for (unsigned i = 0;; i++) {
        uppercase[i] = name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i];
        if (!name[i]) break;
    }
    bytes = cab_extract(cab, uppercase, &actual);
    CHECK(bytes != NULL); CHECK(actual == size); CHECK(cab_host_hash(bytes, actual) == hash);
    kfree(bytes);
}
static void cab_host_mutate(uint8_t *data, uint32_t size)
{
    uint32_t state = 0xcab12345;
    for (unsigned i = 0; i < 1200; i++) {
        state = state * 1664525U + 1013904223U;
        uint32_t offset = i < 512 ? i % (size < 256 ? size : 256) : state % size;
        uint8_t old = data[offset]; data[offset] ^= 1U << (i % 8);
        cab_archive_t *cab = cab_open(data, size);
        if (cab) {
            for (int n = 0; n < cab_file_count(cab) && n < 3; n++) {
                uint32_t length;
                uint8_t *bytes = cab_extract_index(cab, n, &length);
                if (bytes) kfree(bytes);
            }
            cab_close(cab);
        }
        data[offset] = old; CHECK(allocations == 0);
    }
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *names[] = {"stored.cab", "mszip.cab", "dxupdate.cab"};
    const uint32_t sizes[2][3] = {{14, 0, 70000}, {177152, 12848, 66865}};
    const uint32_t hashes[2][3] = {{0x23e1760f, 0x811c9dc5, 0xe32e51f5},
                                  {0xe2f6f3f8, 0x1a1edede, 0x140baaaf}};
    for (unsigned kind = 0; kind < 3; kind++) {
        uint32_t size; uint8_t *data = cab_host_load(argv[1], names[kind], &size);
        cab_archive_t *cab = cab_open(data, size); CHECK(cab != NULL);
        CHECK(cab_file_count(cab) == 3);
        cab_info_t info; CHECK(!cab_get_info(cab, &info));
        if (kind < 2) CHECK(info.set_id == 0x1234 && info.number == 2 && !info.flags);
        for (int i = 0; i < 3; i++) cab_host_extract(cab, i, sizes[kind == 2][i], hashes[kind == 2][i]);
        for (int i = 2; i >= 0; i--) cab_host_extract(cab, i, sizes[kind == 2][i], hashes[kind == 2][i]);
        CHECK(cab_extract_index(cab, -1, NULL) == NULL);
        CHECK(cab_extract_index(cab, 3, NULL) == NULL);
        CHECK(cab_extract(cab, "absent", NULL) == NULL);
        uint32_t metadata_end = file_at(cab, 2, NULL, NULL, NULL) + 16;
        metadata_end += (uint32_t)strlen((char *)data + metadata_end) + 1;
        cab_close(cab); CHECK(!allocations);
        for (uint32_t n = 0; n < metadata_end; n++) {
            uint8_t *cut = malloc(n ? n : 1); CHECK(cut != NULL); memcpy(cut, data, n);
            CHECK(cab_open(cut, n) == NULL); free(cut); CHECK(!allocations);
        }
        for (int limit = 0; limit < 5; limit++) {
            fail_after = limit; cab = cab_open(data, size);
            if (cab) { uint8_t *bytes = cab_extract_index(cab, 2, NULL); kfree(bytes); cab_close(cab); }
            fail_after = -1; CHECK(!allocations);
        }
        cab_host_mutate(data, size); free(data);
    }
    uint32_t size; uint8_t *data = cab_host_load(argv[1], "truncated.cab", &size);
    cab_archive_t *cab = cab_open(data, size); CHECK(cab != NULL);
    cab_host_extract(cab, 0, 14, 0x23e1760f);
    cab_host_extract(cab, 1, 0, 0x811c9dc5);
    CHECK(cab_extract_index(cab, 2, NULL) == NULL); cab_close(cab); free(data);
    data = cab_host_load(argv[1], "bad-checksum.cab", &size);
    cab = cab_open(data, size); CHECK(cab != NULL);
    CHECK(cab_extract_index(cab, 0, NULL) == NULL); cab_close(cab); free(data);
    data = cab_host_load(argv[1], "stored.cab", &size);
    uint32_t block = rd32(data + 36); data[block + 4] = 1; data[block + 5] = 0;
    cab = cab_open(data, size); CHECK(cab != NULL);
    CHECK(cab_extract_index(cab, 0, NULL) == NULL); cab_close(cab); free(data);
    data = cab_host_load(argv[1], "mszip.cab", &size);
    block = rd32(data + 36); data[block + 6] = 16; data[block + 7] = 0;
    cab = cab_open(data, size); CHECK(cab != NULL);
    CHECK(cab_extract_index(cab, 0, NULL) == NULL); cab_close(cab); free(data);
    CHECK(!allocations); printf("CAB READER HOST PASS %u checks\n", checks); return 0;
}
