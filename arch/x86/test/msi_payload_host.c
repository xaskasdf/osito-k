#define MSI_DATABASE_REAL_CAB
#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_payload.h"

typedef struct { uint8_t *data; uint32_t size; int mode; unsigned calls; } source_t;
static uint32_t source_read(void *context, const char *name, uint8_t **data, uint32_t *size)
{
    source_t *source = context; source->calls++;
    CHECK(!strcmp(name, "cab1.cab"));
    if (source->mode == 1) return 2;
    /* Signed CABs have trailing certificate data. Truncate compressed data,
     * not merely that overlay, so the final selected member is incomplete. */
    *size = source->mode == 2 ? source->size / 2 : source->size;
    *data = kmalloc(*size); if (!*data) return 8;
    memcpy(*data, source->data, *size);
    if (source->mode == 3) *size = 128U * 1024 * 1024 + 1;
    return 0;
}
int main(int argc, char **argv)
{
    CHECK(argc == 5);
    uint32_t size; uint8_t *data = read_file(argv[1], &size);
    msi_db_t *db = msi_open(data, size); CHECK(db != NULL);
    uint32_t error; msi_package_t *package = msi_package_create(db, &error); CHECK(package != NULL && !error);
    FILE *f = fopen(argv[2], "rb"); CHECK(f != NULL && get_number(f) == 0x314b504dU);
    uint32_t count = get_number(f);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t *name = get_text(f), *value = get_text(f);
        CHECK(msi_package_set(package, name, value) == 0);
        CHECK(equal_wide(msi_package_get(package, name), value)); free(name); free(value);
    }
    fclose(f);
    msi_plan_t *plan = msi_plan_create_fresh(db, package, NULL, NULL, &error);
    CHECK(plan != NULL && !error && plan->file_count > 0);
    source_t source = {0}; source.data = read_file(argv[3], &source.size);
    unsigned baseline = allocations;
    msi_payload_t *payload = msi_payload_prepare(plan, source_read, &source, &error);
    CHECK(payload != NULL && !error && payload->count == plan->file_count && source.calls == 1);
    uint64_t bytes_checked = 0;
    for (uint32_t i = 0; i < payload->count; i++) {
        char path[4096];
        CHECK(snprintf(path, sizeof(path), "%s/%s", argv[4], plan->files[i].key) < (int)sizeof(path));
        uint32_t expected_size; uint8_t *expected = read_file(path, &expected_size);
        CHECK(expected_size == plan->files[i].size && expected_size == payload->files[i].size);
        CHECK(!memcmp(expected, payload->files[i].bytes, expected_size)); bytes_checked += expected_size;
        free(expected);
    }
    msi_payload_close(payload); CHECK(allocations == baseline);
    for (int mode = 1; mode <= 3; mode++) {
        source.mode = mode;
        payload = msi_payload_prepare(plan, source_read, &source, &error);
        CHECK(payload == NULL && error != 0); CHECK(allocations == baseline);
    }
    source.mode = 0;
    plan->files[plan->file_count - 1].size++;
    payload = msi_payload_prepare(plan, source_read, &source, &error);
    CHECK(payload == NULL && error != 0); CHECK(allocations == baseline);
    plan->files[plan->file_count - 1].size--;
    payload = msi_payload_prepare(plan, NULL, NULL, &error);
    CHECK(payload == NULL && error == 1612); CHECK(allocations == baseline);
    for (int i = 0; i < 24; i++) {
        fail_after = i;
        payload = msi_payload_prepare(plan, source_read, &source, &error);
        if (payload) { CHECK(!error); msi_payload_close(payload); }
        else CHECK(error != 0);
        CHECK(allocations == baseline);
    }
    fail_after = -1;
    uint32_t file_count = plan->file_count;
    payload = msi_payload_prepare(plan, source_read, &source, &error); CHECK(payload && !error);
    unsigned cache_baseline = allocations;
    static const uint16_t cache_path[] = {'C',':','\\','c','a','c','h','e','d','.','m','s','i',0};
    uint64_t old_bytes = plan->payload_bytes;
    for (int i = 0; i < 4; i++) {
        fail_after = i;
        CHECK(msi_payload_stage_cache(plan, payload, cache_path, data, size) == 8);
        CHECK(plan->file_count == file_count && payload->count == file_count && plan->payload_bytes == old_bytes);
        CHECK(allocations == cache_baseline);
    }
    fail_after = -1;
    CHECK(msi_payload_stage_cache(plan, payload, cache_path, data, size) == 0);
    CHECK(plan->file_count == file_count + 1 && payload->count == file_count + 1);
    CHECK(payload->files[file_count].size == size && !memcmp(payload->files[file_count].bytes, data, size));
    CHECK(msi_payload_stage_cache(plan, payload, cache_path, data, size) == 87);
    msi_payload_close(payload);
    msi_plan_close(plan); msi_package_close(package); msi_close(db); free(data); free(source.data);
    CHECK(allocations == 0 && writes == 0 && registry == 0 && commits == 0);
    printf("MSI PAYLOAD PASS files=%u bytes=%llu checks=%u allocations=%u writes=%u registry=%u\n",
           file_count, (unsigned long long)bytes_checked, checks, allocations, writes, registry);
    return 0;
}
