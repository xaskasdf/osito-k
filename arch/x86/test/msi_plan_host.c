#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_plan.h"

static void match_objects(FILE *f, msi_plan_object_t *objects, uint32_t count)
{
    CHECK(get_number(f) == count);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t *name = get_text(f);
        int installed = (int)get_number(f), action = (int)get_number(f), found = 0;
        for (uint32_t j = 0; j < count; j++) if (equal_wide(objects[j].name, name)) {
            found++; CHECK(objects[j].installed == installed); CHECK(objects[j].action == action);
        }
        CHECK(found == 1); free(name);
    }
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    uint32_t size; uint8_t *data = read_file(argv[1], &size);
    msi_db_t *db = msi_open(data, size); CHECK(db != NULL);
    uint32_t error;
    FILE *f = fopen(argv[2], "rb"); CHECK(f != NULL);
    CHECK(get_number(f) == 0x3143504dU); /* MPC1 */
    uint32_t cases = get_number(f), files_checked = 0, registry_checked = 0;
    CHECK(cases > 0 && cases < 100);
    for (uint32_t scenario = 0; scenario < cases; scenario++) {
        msi_package_t *package = msi_package_create(db, &error); CHECK(package != NULL && !error);
        uint32_t properties = get_number(f);
        for (uint32_t i = 0; i < properties; i++) {
            uint16_t *name = get_text(f), *value = get_text(f);
            CHECK(msi_package_set(package, name, value) == 0); free(name); free(value);
        }
        msi_plan_t *plan = msi_plan_create_fresh(db, package, NULL, NULL, &error);
        if (!plan) fprintf(stderr, "plan scenario=%u error=%u\n", scenario, error);
        CHECK(plan != NULL && !error);
        match_objects(f, plan->features, plan->feature_count);
        match_objects(f, plan->components, plan->component_count);
        CHECK(get_number(f) == plan->file_count);
        uint64_t payload = 0;
        for (uint32_t i = 0; i < plan->file_count; i++) {
            uint16_t *key = get_text(f), *path = get_text(f), *cabinet = get_text(f);
            uint32_t bytes = get_number(f), sequence = get_number(f), found = 0;
            for (uint32_t j = 0; j < plan->file_count; j++) {
                uint16_t *wide = msi_package_decode(package, plan->files[j].key);
                CHECK(wide != NULL); int match = equal_wide(wide, key); kfree(wide);
                if (!match) continue;
                found++; CHECK(equal_wide(plan->files[j].target, path));
                wide = msi_package_decode(package, plan->files[j].cabinet);
                CHECK(wide != NULL && equal_wide(wide, cabinet)); kfree(wide);
                CHECK(plan->files[j].size == bytes && plan->files[j].sequence == sequence);
            }
            CHECK(found == 1); payload += bytes; files_checked++;
            free(key); free(path); free(cabinet);
        }
        CHECK(payload == plan->payload_bytes);
        uint32_t registry = get_number(f); CHECK(registry == plan->registry_count);
        for (uint32_t i = 0; i < registry; i++) { CHECK(get_number(f) == plan->registry[i].row); registry_checked++; }
        msi_plan_close(plan);
        unsigned baseline = allocations;
        for (int allocation = 0; allocation < 160; allocation++) {
            fail_after = allocation;
            plan = msi_plan_create_fresh(db, package, NULL, NULL, &error);
            if (plan) msi_plan_close(plan); else CHECK(error != 0);
            /* Directory caches created by a successful partial costing are
             * retained in the package, so close the package before leak checks. */
            fail_after = -1;
            static const uint16_t key[] = {'T','e','s','t','R','e','s','e','t',0};
            CHECK(msi_package_set(package, key, NULL) == 0);
            if (allocation == 0) baseline = allocations;
            CHECK(allocations == baseline);
        }
        msi_package_close(package);
    }
    CHECK(fgetc(f) == EOF); fclose(f);
    msi_close(db); free(data); CHECK(allocations == 0);
    printf("MSI PLAN PASS scenarios=%u files=%u registry=%u checks=%u\n", cases, files_checked, registry_checked, checks);
    return 0;
}
