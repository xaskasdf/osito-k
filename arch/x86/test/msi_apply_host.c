#define MSI_DATABASE_REAL_CAB
#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_apply.h"

typedef struct { uint8_t *data; uint32_t size, type; int present; } stored_t;
typedef struct {
    msi_plan_t *plan; msi_registry_t *registry;
    stored_t *files, *values;
    int *keys;
    unsigned calls, mutations, removes, restores;
    int fail_call, partial, restore_error;
} machine_t;
static int machine_fail(machine_t *m)
{ return (int)m->calls++ == m->fail_call; }
static void stored_set(stored_t *s, const uint8_t *data, uint32_t size, uint32_t type)
{
    if (s->data) free(s->data);
    s->data = malloc(size ? size : 1); CHECK(s->data != NULL);
    if (size) memcpy(s->data, data, size);
    s->present = 1; s->size = size; s->type = type;
}
static void stored_clear(stored_t *s)
{ if (s->data) free(s->data); memset(s, 0, sizeof(*s)); }
static uint32_t machine_file_index(machine_t *m, const uint16_t *path)
{
    for (uint32_t i = 0; i < m->plan->file_count; i++) if (equal_wide(path, m->plan->files[i].target)) return i;
    CHECK(0); return 0;
}
static uint32_t machine_value_index(machine_t *m, const msi_registry_value_t *v)
{
    CHECK(v >= m->registry->values && v < m->registry->values + m->registry->count);
    return (uint32_t)(v - m->registry->values);
}
static uint32_t machine_read_file(void *context, const uint16_t *path, uint8_t **data, uint32_t *size, int *present)
{
    machine_t *m = context; if (machine_fail(m)) return 31;
    stored_t *file = &m->files[machine_file_index(m, path)];
    *present = file->present; *size = file->size;
    if (!file->present) return 0;
    *data = kmalloc(file->size ? file->size : 1); if (!*data) return 8;
    if (file->size) memcpy(*data, file->data, file->size); return 0;
}
static uint32_t machine_create_file(void *context, const uint16_t *path, const uint8_t *data, uint32_t size, int *created)
{
    machine_t *m = context; int fail = machine_fail(m);
    if (fail && !m->partial) return 31;
    stored_t *file = &m->files[machine_file_index(m, path)]; CHECK(!file->present);
    stored_set(file, data, fail ? size / 2 : size, 0); *created = 1; m->mutations++;
    return fail ? 31 : 0;
}
static uint32_t machine_remove_file(void *context, const uint16_t *path)
{
    machine_t *m = context; stored_clear(&m->files[machine_file_index(m, path)]); m->removes++; return 0;
}
static uint32_t machine_read_registry(void *context, const msi_registry_value_t *value, msi_registry_before_t *before)
{
    machine_t *m = context; if (machine_fail(m)) return 31;
    uint32_t i = machine_value_index(m, value); stored_t *v = &m->values[i];
    before->key_exists = m->keys[i]; before->present = v->present; before->size = v->size; before->type = v->type;
    if (v->present) {
        before->data = kmalloc(v->size ? v->size : 1); if (!before->data) return 8;
        if (v->size) memcpy(before->data, v->data, v->size);
    }
    return 0;
}
static uint32_t machine_write_registry(void *context, const msi_registry_value_t *value, int *changed)
{
    machine_t *m = context; int fail = machine_fail(m); if (fail && !m->partial) return 31;
    uint32_t i = machine_value_index(m, value);
    m->keys[i] = 1; *changed = 1; m->mutations++;
    if (!fail && !value->create_only) stored_set(&m->values[i], value->data, value->size, value->type);
    return fail ? 31 : 0;
}
static uint32_t machine_restore_registry(void *context, const msi_registry_value_t *value, const msi_registry_before_t *before)
{
    machine_t *m = context; m->restores++;
    if (m->restore_error) { m->restore_error = 0; return 5; }
    uint32_t i = machine_value_index(m, value); m->keys[i] = before->key_exists;
    if (before->present) stored_set(&m->values[i], before->data, before->size, before->type);
    else stored_clear(&m->values[i]);
    return 0;
}
static uint32_t machine_flush(void *context)
{ return machine_fail(context) ? 31 : 0; }
static int cancel_step, cancel_poll;
static int machine_cancelled(void *context)
{ (void)context; return cancel_step >= 0 && cancel_poll++ >= cancel_step; }
static const msi_apply_backend_t backend = {machine_read_file, machine_create_file, machine_remove_file,
    machine_read_registry, machine_write_registry, machine_restore_registry, machine_flush, NULL};
typedef struct { uint8_t *data; uint32_t size; } test_source_t;
static uint32_t machine_source(void *context, const char *name, uint8_t **data, uint32_t *size)
{
    test_source_t *source = context; CHECK(!strcmp(name, "cab1.cab"));
    *size = source->size; *data = kmalloc(*size); if (!*data) return 8;
    memcpy(*data, source->data, *size); return 0;
}
static void machine_reset(machine_t *m, const msi_payload_t *payload, int existing)
{
    for (uint32_t i = 0; i < m->plan->file_count; i++) stored_clear(&m->files[i]);
    for (uint32_t i = 0; i < m->registry->count; i++) { stored_clear(&m->values[i]); m->keys[i] = 0; }
    m->calls = m->mutations = m->removes = m->restores = 0; m->fail_call = -1; m->partial = 0; m->restore_error = 0;
    if (existing) {
        stored_set(&m->files[0], payload->files[0].bytes, payload->files[0].size, 0);
        m->keys[0] = m->keys[1] = m->keys[2] = 1;
        stored_set(&m->values[0], (const uint8_t *)"old value", 9, 3);
        stored_set(&m->values[1], NULL, 0, 7);
    }
}
static void machine_unchanged(machine_t *m, const msi_payload_t *payload, int existing)
{
    for (uint32_t i = 0; i < m->plan->file_count; i++) {
        stored_t *s = &m->files[i];
        if (existing && !i) {
            CHECK(s->present && s->size == payload->files[0].size);
            CHECK(!memcmp(s->data, payload->files[0].bytes, s->size));
        } else CHECK(!s->present && !s->data);
    }
    for (uint32_t i = 0; i < m->registry->count; i++) {
        stored_t *s = &m->values[i]; CHECK(m->keys[i] == (existing && i < 3));
        if (existing && !i) CHECK(s->present && s->type == 3 && s->size == 9 && !memcmp(s->data,"old value",9));
        else if (existing && i == 1) CHECK(s->present && s->type == 7 && s->size == 0);
        else CHECK(!s->present && !s->data);
    }
}
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
    test_source_t source; source.data = read_file(argv[3], &source.size);
    msi_payload_t *payload = msi_payload_prepare(plan, machine_source, &source, &error); CHECK(payload && !error);
    msi_registry_t *list = msi_registry_prepare(plan, &error); CHECK(list && !error && list->count > 2);
    machine_t machine = {.plan = plan, .registry = list};
    machine.files = calloc(plan->file_count, sizeof(*machine.files));
    machine.values = calloc(list->count, sizeof(*machine.values)); machine.keys = calloc(list->count, sizeof(*machine.keys));
    CHECK(machine.files && machine.values && machine.keys);
    unsigned baseline = allocations, failure_cases = 0, cancellation_cases = 0;
    msi_apply_result_t result;
    for (int existing = 0; existing < 2; existing++) {
        machine_reset(&machine, payload, existing);
        CHECK(msi_apply_fresh(plan, payload, list, &backend, &machine, &result) == 0);
        unsigned calls = machine.calls;
        CHECK(result.files_created == plan->file_count - (uint32_t)existing && result.files_retained == (uint32_t)existing);
        CHECK(result.values_written == list->count && !result.rollback_error);
        for (uint32_t i = 0; i < plan->file_count; i++) CHECK(machine.files[i].present &&
            machine.files[i].size == payload->files[i].size && !memcmp(machine.files[i].data, payload->files[i].bytes, payload->files[i].size));
        for (uint32_t i = 0; i < list->count; i++) CHECK(machine.values[i].present && machine.values[i].type == list->values[i].type &&
            machine.values[i].size == list->values[i].size && !memcmp(machine.values[i].data, list->values[i].data, list->values[i].size));
        CHECK(allocations == baseline);
        for (int partial = 0; partial < 2; partial++) for (unsigned fail = 0; fail < calls; fail++) {
            machine_reset(&machine, payload, existing); machine.partial = partial; machine.fail_call = (int)fail;
            CHECK(msi_apply_fresh(plan, payload, list, &backend, &machine, &result) == 31);
            CHECK(result.rollback_error == 0); machine_unchanged(&machine, payload, existing);
            CHECK(allocations == baseline); failure_cases++;
        }
        for (int fail = 0; fail < 12; fail++) {
            machine_reset(&machine, payload, existing); fail_after = fail;
            error = msi_apply_fresh(plan, payload, list, &backend, &machine, &result); fail_after = -1;
            if (error) { CHECK(error == 8); machine_unchanged(&machine, payload, existing); CHECK(!machine.mutations); }
            CHECK(allocations == baseline); failure_cases++;
        }
        msi_apply_backend_t cancellable = backend; cancellable.cancelled = machine_cancelled;
        machine_reset(&machine, payload, existing); cancel_step = 10000; cancel_poll = 0;
        CHECK(msi_apply_fresh(plan, payload, list, &cancellable, &machine, &result) == 0);
        int polls = cancel_poll;
        for (int fail = 0; fail < polls; fail++) {
            machine_reset(&machine, payload, existing); cancel_step = fail; cancel_poll = 0;
            CHECK(msi_apply_fresh(plan, payload, list, &cancellable, &machine, &result) == 1602);
            CHECK(!result.rollback_error); machine_unchanged(&machine, payload, existing);
            CHECK(allocations == baseline); cancellation_cases++;
        }
    }
    machine_reset(&machine, payload, 0);
    stored_set(&machine.files[0], (const uint8_t *)"different file", 14, 0);
    CHECK(msi_apply_fresh(plan, payload, list, &backend, &machine, &result) == 1638);
    CHECK(machine.mutations == 0 && machine.removes == 0 && !memcmp(machine.files[0].data, "different file", 14));
    machine_reset(&machine, payload, 0); machine.fail_call = (int)(2 * (plan->file_count + list->count)); machine.restore_error = 1;
    CHECK(msi_apply_fresh(plan, payload, list, &backend, &machine, &result) == 31 && result.rollback_error == 5);
    CHECK(machine.removes == plan->file_count && machine.restores == list->count);
    machine_reset(&machine, payload, 0); free(machine.files); free(machine.values); free(machine.keys);
    msi_registry_close(list); msi_payload_close(payload); msi_plan_close(plan); msi_package_close(package); msi_close(db);
    free(source.data); free(data); CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI APPLY PASS failure_cases=%u cancellation_cases=%u checks=%u allocation_balance=%u\n", failure_cases, cancellation_cases, checks, allocations);
    return 0;
}
