#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_registration.h"

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
        CHECK(msi_package_set(package, key, value) == 0); free(key); free(value);
    }
    fclose(file);
    msi_plan_t *plan = msi_plan_create_fresh(db, package, NULL, NULL, &error); CHECK(plan && !error);
    msi_registry_t *authored = msi_registry_prepare(plan, &error); CHECK(authored && !error);
    msi_summary_t *summary = msi_summary_read(db, &error); CHECK(summary && !error);
    const uint16_t *source = (const uint16_t *)u"C:\\InstallerSource\\";
    const uint16_t *cache = (const uint16_t *)u"C:\\Windows\\Installer\\test.msi";
    unsigned baseline = allocations;
    msi_registry_t *list = msi_registration_prepare(plan, summary, authored, source, cache, &error); CHECK(list && !error);
    file = fopen(argv[3], "rb"); CHECK(file && get_number(file) == 0x31475052U);
    uint32_t count = get_number(file); CHECK(count == list->count);
    for (uint32_t i = 0; i < count; i++) {
        msi_registry_value_t *value = &list->values[i];
        uint16_t *path = get_text(file), *name = get_text(file);
        CHECK(value->root == 2 && value->view == 0x100);
        CHECK(equal_wide(path, value->path)); CHECK(equal_wide(name, value->name)); free(path); free(name);
        CHECK(value->type == get_number(file)); uint32_t bytes = get_number(file); CHECK(value->size == bytes);
        uint8_t *expected = malloc(bytes); CHECK(expected && fread(expected, 1, bytes, file) == bytes);
        CHECK(!memcmp(value->data, expected, bytes)); free(expected);
    }
    CHECK(fgetc(file) == EOF); fclose(file);
    msi_registry_close(list); CHECK(allocations == baseline);
    for (int i = 0; i < 500; i++) {
        fail_after = i; list = msi_registration_prepare(plan, summary, authored, source, cache, &error);
        if (list) { CHECK(!error); msi_registry_close(list); } else CHECK(error != 0);
        CHECK(allocations == baseline);
    }
    fail_after = -1;
    list = msi_registration_prepare(plan, summary, authored, source, cache, &error); CHECK(list && !error);
    unsigned merge_baseline = allocations;
    uint32_t combined_count = authored->count + list->count;
    for (int i = 0; i < 2; i++) {
        msi_registry_t *first = authored, *last = list;
        fail_after = i;
        CHECK(msi_registry_merge(&authored, &list, &error) == NULL && error == 8);
        CHECK(authored == first && list == last && allocations == merge_baseline);
    }
    fail_after = -1;
    CHECK(msi_registry_merge(&authored, &authored, &error) == NULL && error == 87);
    msi_registry_t *merged = msi_registry_merge(&authored, &list, &error);
    CHECK(merged && !error && !authored && !list && merged->count == combined_count);
    CHECK(equal_wide(merged->values[combined_count-1].name, (const uint16_t *)u"State"));
    CHECK(merged->values[combined_count-1].type == 4 && merged->values[combined_count-1].data[0] == 5);
    msi_registry_close(merged);
    CHECK(msi_registration_guid((const uint16_t *)u"{13A4EE12-23EA-3371-91EE-EFB36DDFFF3E}"));
    CHECK(!msi_registration_guid((const uint16_t *)u"{13A4EE12-23EA-3371-91EE-EFB36DDFFFGG}"));
    CHECK(!msi_registration_guid(NULL));
    msi_summary_close(summary); msi_registry_close(authored); msi_plan_close(plan); msi_package_close(package); msi_close(db); free(data);
    CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI REGISTRATION PASS rows=%u allocation_cases=500 checks=%u\n",count,checks);
    return 0;
}
