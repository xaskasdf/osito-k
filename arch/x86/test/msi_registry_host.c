#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_registry.h"

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    uint32_t size, error; uint8_t *data = read_file(argv[1], &size);
    msi_db_t *db = msi_open(data, size); CHECK(db != NULL);
    msi_package_t *package = msi_package_create(db, &error); CHECK(package && !error);
    FILE *file = fopen(argv[2], "rb"); CHECK(file && get_number(file) == 0x314b504dU);
    uint32_t properties = get_number(file);
    for (uint32_t i = 0; i < properties; i++) {
        uint16_t *key = get_text(file), *value = get_text(file);
        CHECK(msi_package_set(package, key, value) == 0);
        CHECK(equal_wide(msi_package_get(package, key), value)); free(key); free(value);
    }
    fclose(file);
    msi_plan_t *plan = msi_plan_create_fresh(db, package, NULL, NULL, &error); CHECK(plan && !error);
    unsigned baseline = allocations;
    msi_registry_t *list = msi_registry_prepare(plan, &error); CHECK(list && !error);
    file = fopen(argv[3], "rb"); CHECK(file && get_number(file) == 0x31474552U);
    uint32_t count = get_number(file); CHECK(count == list->count);
    for (uint32_t i = 0; i < count; i++) {
        msi_registry_value_t *value = &list->values[i];
        uint16_t *key = get_text(file), *actual_key = msi_package_decode(package, value->key);
        CHECK(actual_key && equal_wide(key, actual_key)); free(key); kfree(actual_key);
        CHECK(value->root == get_number(file)); CHECK(value->view == get_number(file));
        uint16_t *path = get_text(file), *name = get_text(file);
        CHECK(equal_wide(value->path, path)); CHECK(equal_wide(value->name, name)); free(path); free(name);
        CHECK(value->type == get_number(file)); uint32_t bytes = get_number(file); CHECK(value->size == bytes);
        uint8_t *expected = malloc(bytes); CHECK(expected && fread(expected, 1, bytes, file) == bytes);
        CHECK(!memcmp(expected, value->data, bytes)); CHECK(!value->create_only); free(expected);
    }
    CHECK(fgetc(file) == EOF); fclose(file);
    msi_registry_close(list); CHECK(allocations == baseline);
    for (int i = 0; i < 400; i++) {
        fail_after = i; list = msi_registry_prepare(plan, &error);
        if (list) { CHECK(!error); msi_registry_close(list); }
        else CHECK(error != 0);
        CHECK(allocations == baseline);
    }
    fail_after = -1; msi_plan_close(plan); msi_package_close(package); msi_close(db); free(data);
    CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI REGISTRY PASS rows=%u allocation_cases=400 checks=%u\n", count, checks);
    return 0;
}
