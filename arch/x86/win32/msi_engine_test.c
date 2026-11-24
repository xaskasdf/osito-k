/* Native integration diagnostic for disposable QEMU disks. Successful runs
 * leave the original runtime installed so PE probes can load it by DLL name.
 * Refuse any existing target/product; never remove a pre-existing runtime. */
#include "msi_engine.h"
#include "msi_registration.h"
#include "msi_win32_io.h"
#include "kernel32_shim.h"
#include "ntprocess.h"
#include "system_paths.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern void win32_init(void);
extern int proc_current_pid(void);
extern void serial_puts(const char *text);
extern void serial_putdec(uint64_t value);
#define TW(text) ((const uint16_t *)u##text)
static uint16_t *engine_test_wide(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    uint16_t *result = kmalloc(((uint64_t)length + 1) * 2);
    if (result) for (uint32_t i = 0; i <= length; i++) result[i] = (uint8_t)text[i];
    return result;
}
static uint32_t engine_test_absent(const msi_plan_t *plan, const msi_registry_t *registry,
    const msi_registry_before_t *baseline)
{
    for (uint32_t i = 0; i < plan->file_count; i++) {
        uint8_t *data = NULL; uint32_t size = 0; int present = 0;
        uint32_t error = msi_win32_read_file(NULL, plan->files[i].target, &data, &size, &present);
        kfree(data); if (error || present) return error ? error : 183;
    }
    for (uint32_t i = 0; i < registry->count; i++) {
        msi_registry_before_t before = {0};
        uint32_t error = msi_win32_backend.read_registry(NULL, &registry->values[i], &before);
        const msi_registry_before_t *old = &baseline[i];
        if (!error && (before.present != old->present || before.key_exists != old->key_exists ||
            (before.present && (before.type != old->type || before.size != old->size ||
             (before.size && memcmp(before.data,old->data,before.size)))))) error = 183;
        kfree(before.data); if (error) return error;
    }
    return 0;
}
static int msi_engine_test_run(const char *path, const char *reference, int fault_test)
{
    uint32_t error = 0, checks = 0, size = 0; int failure = 0, present = 0, blocker = 0;
    uint8_t *data = NULL, *actual = NULL, *expected = NULL;
    uint16_t *wide = engine_test_wide(path), *system = engine_test_wide(WIN32_SYSWOW64_DIRECTORY "\\");
    msi_db_t *db = NULL; msi_package_t *package = NULL; msi_plan_t *plan = NULL;
    msi_registry_t *authored = NULL, *registration = NULL, *values = NULL; msi_summary_t *summary = NULL;
    msi_registry_before_t *baseline = NULL; uint32_t baseline_count = 0;
    uint16_t cache[260], source[260]; msi_registry_before_t before = {0};
#define ET(c) do { checks++; if (!(c)) { serial_puts("[MSI-ENGINE-TEST] FAIL line="); serial_putdec(__LINE__); \
    serial_puts(" error="); serial_putdec(error); serial_puts("\n"); failure = 1; goto done; } } while (0)
    ET(wide && system);
    error = msi_win32_read_file(NULL, wide, &data, &size, &present); ET(!error && present);
    db = msi_open(data, size); ET(db);
    package = msi_package_create(db, &error); ET(package && !error);
    summary = msi_summary_read(db, &error); ET(summary && !error);
    ET(!msi_package_set(package, TW("ROOTDRIVE"), TW("C:\\")));
    ET(!msi_package_set(package, TW("SystemFolder"), system));
    ET(!msi_package_set(package, TW("VersionNT"), TW("1000")));
    plan = msi_plan_create_fresh(db, package, NULL, NULL, &error); ET(plan && !error);
    authored = msi_registry_prepare(plan, &error); ET(authored && !error);
    const uint16_t *prefix = TW("C:\\Windows\\Installer\\"); uint32_t n = 0;
    while (prefix[n]) { cache[n] = prefix[n]; n++; }
    ET(msi_registration_guid(summary->package_code));
    memcpy(cache+n, summary->package_code, 38*2U); memcpy(cache+n+38, TW(".msi"), 5*2U);
    n = GetFullPathNameW(wide, 260, source, NULL); ET(n && n < 260);
    while (n && source[n-1] != '\\') n--;
    ET(n); source[n] = 0;
    registration = msi_registration_prepare(plan, summary, authored, source, cache, &error); ET(registration && !error);
    uint32_t authored_count = authored->count;
    values = msi_registry_merge(&authored, &registration, &error); ET(values && !error);
    baseline = kmalloc((uint64_t)values->count * sizeof(*baseline)); ET(baseline);
    memset(baseline,0,values->count * sizeof(*baseline)); baseline_count = values->count;
    for (uint32_t i = 0; i < values->count; i++) {
        error = msi_win32_backend.read_registry(NULL,&values->values[i],&baseline[i]); ET(!error);
        /* Minimum and Additional share authored runtime-detection values.
         * Snapshot those; only product-specific registration must be absent. */
        if (i >= authored_count) ET(!baseline[i].present && !baseline[i].key_exists);
    }
    error = engine_test_absent(plan, values, baseline); ET(!error);
    error = msi_engine_install_fresh(wide, TW("NOVSUI=1")); ET(error == 5);
    ET(nt_process_set_main_admin());
    error = msi_engine_install_fresh(wide, NULL); ET(error == 1603);
    error = engine_test_absent(plan, values, baseline); ET(!error);
    static const struct { const uint16_t *text; uint32_t error; } rejected[] = {
        {TW("NOVSUI=1 ALLUSERS=2"),120}, {TW("NOVSUI=1 REMOVE=ALL"),120},
        {TW("NOVSUI=1 TRANSFORMS=test.mst"),120}, {TW("NOVSUI=1 PATCH=test.msp"),120},
        {TW("NOVSUI=1 ACTION=ADMIN"),120}, {TW("NOVSUI=\"unterminated"),1639},
        {TW("NOVSUI=1 NEWERVCRUNTIMEVERSIONDETECTED=1"),1603}
    };
    for (unsigned i = 0; i < sizeof(rejected)/sizeof(rejected[0]); i++) {
        error = msi_engine_install_fresh(wide, rejected[i].text); ET(error == rejected[i].error);
        error = engine_test_absent(plan, values, baseline); ET(!error);
    }
    if (fault_test) {
        /* A file where the cache directory should be forces a real I/O error
         * after runtime DLL creation. Preserve it while the engine rolls back. */
        ET(GetFileAttributesW(TW("C:\\Windows\\Installer")) == UINT32_MAX);
        error = msi_win32_backend.create_file(NULL, TW("C:\\Windows\\Installer"), (const uint8_t *)"blocker", 7, &blocker);
        ET(!error && blocker);
        error = msi_engine_install_fresh(wide, TW("NOVSUI=1 MSIFASTINSTALL=7")); ET(error != 0);
        error = engine_test_absent(plan, values, baseline); ET(!error);
        uint32_t bytes = 0;
        error = msi_win32_read_file(NULL, TW("C:\\Windows\\Installer"), &actual, &bytes, &present);
        ET(!error && present && bytes == 7 && !memcmp(actual,"blocker",7)); kfree(actual); actual = NULL;
        ET(DeleteFileW(TW("C:\\Windows\\Installer"))); blocker = 0;
    }
    error = msi_engine_install_fresh(wide, TW("novsui=1 MSIFASTINSTALL=7")); ET(!error);
    for (uint32_t i = 0; i < plan->file_count; i++) {
        char ref[512]; uint32_t a = (uint32_t)strlen(reference), b = (uint32_t)strlen(plan->files[i].key);
        ET(a+b+2 < sizeof(ref)); memcpy(ref, reference, a); ref[a++] = '/'; memcpy(ref+a,plan->files[i].key,b+1);
        uint16_t *file = engine_test_wide(ref); ET(file);
        uint32_t want = 0, got = 0;
        error = msi_win32_read_file(NULL,file,&expected,&want,&present); kfree(file); ET(!error && present);
        error = msi_win32_read_file(NULL,plan->files[i].target,&actual,&got,&present);
        ET(!error && present && want == got && got == plan->files[i].size && !memcmp(actual,expected,got));
        kfree(actual); kfree(expected); actual = expected = NULL;
    }
    uint32_t bytes = 0;
    error = msi_win32_read_file(NULL,cache,&actual,&bytes,&present); ET(!error && present && bytes == size && !memcmp(actual,data,size));
    kfree(actual); actual = NULL;
    for (uint32_t i = 0; i < values->count; i++) {
        const msi_registry_value_t *value = &values->values[i];
        error = msi_win32_backend.read_registry(NULL,value,&before);
        ET(!error && before.key_exists && before.present && before.type == value->type);
        ET(before.size == value->size && !memcmp(before.data,value->data,value->size));
        kfree(before.data); memset(&before,0,sizeof(before));
    }
    error = msi_engine_install_fresh(wide,TW("NOVSUI=1")); ET(error == 1638);
done:
    if (blocker) DeleteFileW(TW("C:\\Windows\\Installer"));
    for (uint32_t i = 0; i < baseline_count; i++) kfree(baseline[i].data);
    kfree(baseline);
    kfree(before.data); kfree(actual); kfree(expected); kfree(wide); kfree(system);
    msi_registry_close(values); msi_registry_close(authored); msi_registry_close(registration);
    msi_plan_close(plan); msi_summary_close(summary); msi_package_close(package); msi_close(db); kfree(data);
    serial_puts("[MSI-ENGINE-TEST] "); serial_puts(failure ? "FAIL" : "PASS");
    serial_puts(" checks="); serial_putdec(checks); serial_puts("\n");
    return failure;
#undef ET
}
typedef struct { const char *path, *reference; int fault, result; } engine_test_call_t;
static DWORD WINAPI engine_test_thread(void *argument)
{
    engine_test_call_t *call = argument;
    call->result = msi_engine_test_run(call->path, call->reference, call->fault);
    return (DWORD)call->result;
}
int msi_engine_selftest(const char *path, const char *reference, int fault_test)
{
    /* The native shell has no guest thread object. Use an actual Win32 worker
     * so token checks and registry view selection run through the real path. */
    extern void proc_start_reaper(void);
    win32_init(); proc_start_reaper();
    nt_process_begin_main_cpu_accounting((uint32_t)proc_current_pid());
    engine_test_call_t call = {path, reference, fault_test, 1};
    HANDLE thread = CreateThread(NULL, 0, engine_test_thread, &call, 0, NULL);
    if (thread) {
        if (WaitForSingleObject(thread, 0xffffffffU) != 0) call.result = 1;
        CloseHandle(thread);
    }
    nt_process_end_main_cpu_accounting();
    return call.result;
}
