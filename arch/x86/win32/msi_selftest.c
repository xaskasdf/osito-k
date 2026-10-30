/* In-OS MSI preparation and I/O contract test. All writes are isolated under
 * C:\OsitoMsiTest and Software\OsitoK\Tests\MSI, never authored runtime paths.
 * Refuses pre-existing test files/registry values; removes its own writes. */
#include "msi_win32_io.h"
#include "msi_actions.h"
#include "msi_registration.h"
#include "kernel32_shim.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern void serial_puts(const char *text);
extern void serial_putdec(uint64_t value);
extern BOOL WINAPI RemoveDirectoryW(PCWSTR path);

static const uint16_t test_directory[] = {'C',':','\\','O','s','i','t','o','M','s','i','T','e','s','t',0};
typedef struct { const char *cabinet; uint32_t writes, fail_write, actions; int log_actions; } msi_test_context_t;
static uint16_t *test_wide(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    uint16_t *wide = kmalloc(((uint64_t)length + 1) * 2);
    if (wide) for (uint32_t i = 0; i <= length; i++) wide[i] = (uint8_t)text[i];
    return wide;
}
static uint32_t test_read_ascii(const char *path, uint8_t **data, uint32_t *size)
{
    uint16_t *wide = test_wide(path); if (!wide) return 8;
    int present = 0; uint32_t error = msi_win32_read_file(NULL, wide, data, size, &present);
    kfree(wide); return error ? error : present ? 0 : 2;
}
static uint32_t test_source(void *context, const char *name, uint8_t **data, uint32_t *size)
{
    msi_test_context_t *test = context;
    if (strcmp(name, "cab1.cab")) return 120;
    return test_read_ascii(test->cabinet, data, size);
}
static uint32_t test_write_registry(void *context, const msi_registry_value_t *value, int *changed)
{
    msi_test_context_t *test = context;
    uint32_t error = msi_win32_backend.write_registry(NULL, value, changed);
    if (!error && ++test->writes == test->fail_write) return 112;
    return error;
}
static uint32_t test_action(void *context, const char *name)
{
    msi_test_context_t *test = context; test->actions++;
    if (test->log_actions) { serial_puts("[MSI-TEST] action="); serial_puts(name); serial_puts("\n"); }
    return 0; /* Observe sequence/conditions only; never execute an action. */
}
static uint32_t test_property(msi_package_t *package, const char *name, const char *value)
{
    uint16_t *key = test_wide(name), *text = test_wide(value);
    uint32_t error = !key || !text ? 8 : msi_package_set(package, key, text);
    if (key) kfree(key);
    if (text) kfree(text);
    return error;
}

int msi_io_selftest(const char *msi_path, const char *cab_path, const char *reference)
{
    uint8_t *data = NULL; uint32_t size = 0, error = 0, checks = 0;
    msi_db_t *database = NULL; msi_package_t *package = NULL; msi_plan_t *plan = NULL;
    msi_payload_t *payload = NULL; msi_registry_t *values = NULL;
    msi_summary_t *summary = NULL; msi_registry_t *registration = NULL;
    int failure = 0, directory_created = 0, may_cleanup = 0;
    msi_test_context_t context = {.cabinet = cab_path, .log_actions = 1};
    msi_apply_backend_t backend = msi_win32_backend; backend.write_registry = test_write_registry;
    msi_apply_result_t result;
    uint8_t *probe = NULL; uint32_t probe_size = 0;
    msi_registry_before_t before = {0};
#define MSI_TEST(c) do { checks++; if (!(c)) { serial_puts("[MSI-TEST] FAIL line="); \
    serial_putdec(__LINE__); serial_puts(" error="); serial_putdec(error); serial_puts("\n"); failure = 1; goto done; } } while (0)
    MSI_TEST(msi_path && cab_path && reference);
    error = test_read_ascii(msi_path, &data, &size); MSI_TEST(!error);
    database = msi_open(data, size); MSI_TEST(database);
    summary = msi_summary_read(database, &error); MSI_TEST(summary && !error);
    MSI_TEST(summary->codepage == 1252 && summary->installer_version == 301 && summary->word_count == 2);
    MSI_TEST(msi_registration_guid(summary->package_code));
    package = msi_package_create(database, &error); MSI_TEST(package && !error);
    MSI_TEST(!test_property(package, "ROOTDRIVE", "C:\\"));
    MSI_TEST(!test_property(package, "TARGETDIR", "C:\\"));
    MSI_TEST(!test_property(package, "SystemFolder", "C:\\OsitoMsiTest\\"));
    MSI_TEST(!test_property(package, "VersionNT", "1000"));
    MSI_TEST(!test_property(package, "VersionNT64", "1000"));
    MSI_TEST(!test_property(package, "NOVSUI", "1"));
    error = msi_actions_walk(database, package, test_action, &context); MSI_TEST(!error && context.actions > 40);
    context.log_actions = 0;
    MSI_TEST(!test_property(package, "NOVSUI", ""));
    MSI_TEST(msi_actions_walk(database, package, test_action, &context) == 1603);
    MSI_TEST(!test_property(package, "NOVSUI", "1"));
    MSI_TEST(!test_property(package, "NEWERVCRUNTIMEVERSIONDETECTED", "1"));
    MSI_TEST(msi_actions_walk(database, package, test_action, &context) == 1603);
    MSI_TEST(!test_property(package, "NEWERVCRUNTIMEVERSIONDETECTED", ""));
    MSI_TEST(!test_property(package, "REMOVE", "ALL"));
    MSI_TEST(msi_actions_walk(database, package, test_action, &context) == 120);
    MSI_TEST(!test_property(package, "REMOVE", ""));
    plan = msi_plan_create_fresh(database, package, NULL, NULL, &error); MSI_TEST(plan && !error && plan->file_count);
    payload = msi_payload_prepare(plan, test_source, &context, &error); MSI_TEST(payload && !error);
    values = msi_registry_prepare(plan, &error); MSI_TEST(values && !error && values->count == 18);
    for (uint32_t i = 0; i < plan->file_count; i++) {
        char path[260]; uint32_t length = 0;
        for (const char *c = reference; *c && length < 258; c++) path[length++] = *c;
        path[length++] = '\\';
        for (const char *c = plan->files[i].key; *c && length < 259; c++) path[length++] = *c;
        path[length] = 0;
        error = test_read_ascii(path, &probe, &probe_size); MSI_TEST(!error);
        MSI_TEST(probe_size == payload->files[i].size && !memcmp(probe, payload->files[i].bytes, probe_size));
        kfree(probe); probe = NULL;
    }
    const uint16_t *cache_path = (const uint16_t *)u"C:\\OsitoMsiTest\\cached.msi";
    registration = msi_registration_prepare(plan, summary, values, (const uint16_t *)u"C:\\", cache_path, &error);
    MSI_TEST(registration && !error);
    msi_registry_t *merged = msi_registry_merge(&values, &registration, &error); MSI_TEST(merged && !error);
    values = merged;
    MSI_TEST(msi_payload_stage_cache(plan, payload, cache_path, data, size) == 0);
    /* Namespace registry targets before any backend mutation. */
    for (uint32_t i = 0; i < values->count; i++) {
        const char *prefix = "Software\\OsitoK\\Tests\\MSI\\";
        msi_registry_value_t *value = &values->values[i];
        uint32_t a = (uint32_t)strlen(prefix), b = 0; while (value->path[b]) b++;
        uint16_t *path = kmalloc(((uint64_t)a + b + 1) * 2); MSI_TEST(path);
        for (uint32_t c = 0; c < a; c++) path[c] = (uint8_t)prefix[c];
        memcpy(path + a, value->path, (b + 1) * 2U); kfree(value->path); value->path = path;
        error = backend.read_registry(NULL, value, &before); MSI_TEST(!error && !before.present && !before.key_exists);
        if (before.data) { kfree(before.data); before.data = NULL; }
    }
    MSI_TEST(GetFileAttributesW(test_directory) == 0xffffffffU && (GetLastError() == 2 || GetLastError() == 3));
    MSI_TEST(CreateDirectoryW(test_directory, NULL)); directory_created = 1;
    for (uint32_t i = 0; i < plan->file_count; i++) {
        int present = 0;
        error = backend.read_file(NULL, plan->files[i].target, &probe, &probe_size, &present); MSI_TEST(!error && !present);
    }
    may_cleanup = 1;
    context.fail_write = values->count;
    error = msi_apply_fresh(plan, payload, values, &backend, &context, &result);
    MSI_TEST(error == 112 && !result.rollback_error && result.files_created == plan->file_count && result.values_written == values->count);
    for (uint32_t i = 0; i < plan->file_count; i++) {
        int present = 0;
        error = backend.read_file(NULL, plan->files[i].target, &probe, &probe_size, &present); MSI_TEST(!error && !present);
    }
    for (uint32_t i = 0; i < values->count; i++) {
        error = backend.read_registry(NULL, &values->values[i], &before); MSI_TEST(!error && !before.present && !before.key_exists);
        if (before.data) { kfree(before.data); before.data = NULL; }
    }
    context.fail_write = 0; context.writes = 0;
    error = msi_apply_fresh(plan, payload, values, &backend, &context, &result);
    MSI_TEST(!error && result.files_created == plan->file_count && result.values_written == values->count);
    for (uint32_t i = 0; i < plan->file_count; i++) {
        int present = 0;
        error = backend.read_file(NULL, plan->files[i].target, &probe, &probe_size, &present); MSI_TEST(!error && present);
        MSI_TEST(probe_size == payload->files[i].size && !memcmp(probe, payload->files[i].bytes, probe_size));
        kfree(probe); probe = NULL;
    }
    for (uint32_t i = 0; i < values->count; i++) {
        msi_registry_value_t *value = &values->values[i];
        error = backend.read_registry(NULL, value, &before); MSI_TEST(!error && before.present);
        MSI_TEST(before.type == value->type && before.size == value->size && !memcmp(before.data, value->data, value->size));
        kfree(before.data); before.data = NULL;
    }
    error = msi_apply_fresh(plan, payload, values, &backend, &context, &result);
    MSI_TEST(!error && !result.files_created && result.files_retained == plan->file_count && !result.values_written);
done:
    if (probe) kfree(probe);
    if (before.data) kfree(before.data);
    if (may_cleanup) {
        msi_registry_before_t absent = {0};
        for (uint32_t i = values->count; i > 0; i--)
            if (backend.restore_registry(NULL, &values->values[i-1], &absent)) failure = 1;
        for (uint32_t i = 0; i < plan->file_count; i++)
            if (backend.remove_file(NULL, plan->files[i].target)) failure = 1;
        if (backend.flush(NULL)) failure = 1;
    }
    if (directory_created && !RemoveDirectoryW(test_directory)) failure = 1;
    serial_puts(failure ? "[MSI-TEST] FAIL checks=" : "[MSI-TEST] PASS checks="); serial_putdec(checks);
    serial_puts(" files="); serial_putdec(plan ? plan->file_count : 0);
    serial_puts(" registry="); serial_putdec(values ? values->count : 0); serial_puts("\n");
    msi_registry_close(values); msi_payload_close(payload); msi_plan_close(plan); msi_package_close(package); msi_close(database);
    msi_registry_close(registration); msi_summary_close(summary);
    if (data) kfree(data);
    return failure;
#undef MSI_TEST
}
