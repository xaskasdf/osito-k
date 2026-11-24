/* Exercise preparation on the original packages. Machine adapters are inert;
 * the separate QEMU test covers writes, rollback and loading installed DLLs. */
#define MSI_DATABASE_REAL_CAB
#define MSI_PACKAGE_ADAPTERS_ONLY
#include "msi_package_host.c"
#include "../win32/msi_engine.c"
static uint8_t *source_bytes;
static uint32_t source_size;
static int administrator = 1;
DWORD WINAPI GetVersion(void) { return 0x4a64000a; }
BOOL advapi32_effective_is_admin(void) { return administrator; }
LONG WINAPI RegOpenKeyExW(HKEY root, PCWSTR path, DWORD options, DWORD access, PHKEY out)
{ (void)root; (void)path; (void)options; (void)access; *out = NULL; return 2; }
LONG WINAPI RegEnumKeyExW(HKEY key, DWORD index, PWSTR name, DWORD *length, DWORD *reserved, PWSTR cls, DWORD *clen, PVOID stamp)
{ (void)key; (void)index; (void)name; (void)length; (void)reserved; (void)cls; (void)clen; (void)stamp; CHECK(0); return 259; }
LONG WINAPI RegQueryValueExW(HKEY key, PCWSTR name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *size)
{ (void)key; (void)name; (void)reserved; (void)type; (void)data; (void)size; CHECK(0); return 2; }
LONG WINAPI RegCloseKey(HKEY key) { (void)key; CHECK(0); return 0; }
static uint32_t absent_registry(void *context, const msi_registry_value_t *value, msi_registry_before_t *before)
{ (void)context; CHECK(value->root == 2 && value->view == 0x200); memset(before,0,sizeof(*before)); return 0; }
const msi_apply_backend_t msi_win32_backend = {.read_registry = absent_registry};
uint32_t msi_win32_read_file(void *context, const uint16_t *path, uint8_t **data, uint32_t *size, int *present)
{
    (void)context; CHECK(equal_wide(path, EW("C:\\source\\cab1.cab")));
    *size = source_size; *present = 1; *data = kmalloc(*size); if (!*data) return 8;
    memcpy(*data,source_bytes,*size); return 0;
}
static void release_engine(engine_t *engine)
{
    msi_registry_close(engine->registry); msi_payload_close(engine->payload); msi_plan_close(engine->plan);
    msi_package_close(engine->package); engine->registry = NULL; engine->payload = NULL; engine->plan = NULL; engine->package = NULL;
    engine->phase = engine->flags = 0;
}
int main(int argc, char **argv)
{
    (void)get_text; /* Shared oracle adapters also support record fixtures. */
    CHECK(argc == 3);
    engine_t engine = {0}; engine.data = read_file(argv[1], &engine.size);
    source_bytes = read_file(argv[2], &source_size);
    engine.database = msi_open(engine.data,engine.size); CHECK(engine.database != NULL);
    uint32_t error = 0;
    engine.summary = msi_summary_read(engine.database,&error); CHECK(engine.summary && !error);
    memcpy(engine.source,EW("C:\\source\\"),11*2U);
    engine.package = msi_package_create(engine.database,&error); CHECK(engine.package && !error);
    uint16_t *original = engine.summary->platform;
    engine.summary->platform = (uint16_t *)u"I"; CHECK(engine_platform(&engine) == 1633);
    engine.summary->platform = original; CHECK(!engine_platform(&engine));
    CHECK(!engine_command_line(&engine, EW("novsui=1 TEST=\"a b\" QUOTE=\"a\"\"b\" EMPTY= TAB=x\tEND=ok")));
    CHECK(equal_wide(msi_package_get(engine.package,EW("TEST")),EW("a b")));
    CHECK(equal_wide(msi_package_get(engine.package,EW("QUOTE")),EW("a\"b")));
    CHECK(equal_wide(msi_package_get(engine.package,EW("EMPTY")),EW("")));
    CHECK(equal_wide(msi_package_get(engine.package,EW("END")),EW("ok")));
    static const uint16_t *const invalid[] = {EW("=x"),EW("BAD"),EW("0KEY=x"),EW("A-B=x"),EW("A=\"open"),EW("A B=x")};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) CHECK(engine_command_line(&engine,invalid[i]) == 1639);
    CHECK(!engine_supported(&engine)); administrator = 0; CHECK(engine_supported(&engine) == 5); administrator = 1;
    msi_registry_before_t raw = {.type = REG_SZ, .data = (uint8_t *)u"#value", .size = 14};
    CHECK(!engine_raw_value(&engine,EW("RAW"),&raw)); CHECK(equal_wide(msi_package_get(engine.package,EW("RAW")),EW("##value")));
    raw.type = REG_EXPAND_SZ; CHECK(!engine_raw_value(&engine,EW("RAW"),&raw)); CHECK(equal_wide(msi_package_get(engine.package,EW("RAW")),EW("#%#value")));
    uint8_t binary[] = {0, 0x12, 0xab, 0xff}; raw = (msi_registry_before_t){.type = REG_BINARY,.data = binary,.size = 4};
    CHECK(!engine_raw_value(&engine,EW("RAW"),&raw)); CHECK(equal_wide(msi_package_get(engine.package,EW("RAW")),EW("#x0012ABFF")));
    raw.type = REG_DWORD; CHECK(!engine_raw_value(&engine,EW("RAW"),&raw));
    CHECK(equal_wide(msi_package_get(engine.package,EW("RAW")),EW("#4289401344")));
    raw.type = REG_MULTI_SZ; CHECK(engine_raw_value(&engine,EW("RAW"),&raw) == 120);
    uint8_t *unused = NULL; uint32_t size = 0;
    CHECK(engine_source(&engine,"../cab1.cab",&unused,&size) == 1612 && !unused);
    CHECK(engine_action(&engine,"UnknownAction") == 120);
    CHECK(engine_action(&engine,"InstallFiles") == 1620);
    CHECK(!msi_actions_walk(engine.database,engine.package,engine_action,&engine));
    CHECK(engine.phase == 6 && engine.flags == 127 && engine.payload && engine.registry && engine.registry->count == 18);
    uint32_t files = engine.plan->file_count; release_engine(&engine);
    unsigned baseline = allocations;
    for (int failure = 0; failure < 800; failure++) {
        fail_after = failure;
        engine.package = msi_package_create(engine.database,&error);
        if (!error) error = engine_platform(&engine);
        if (!error) error = engine_command_line(&engine,EW("NOVSUI=1 MSIFASTINSTALL=7"));
        if (!error) error = engine_supported(&engine);
        if (!error) error = engine_existing(&engine);
        if (!error) error = msi_actions_walk(engine.database,engine.package,engine_action,&engine);
        fail_after = -1;
        if (!error) CHECK(engine.phase == 6 && engine.flags == 127);
        release_engine(&engine); CHECK(allocations == baseline);
    }
    msi_summary_close(engine.summary); msi_close(engine.database); free(engine.data); free(source_bytes);
    CHECK(!allocations && !writes && !registry && !commits);
    printf("MSI ENGINE PREPARE PASS files=%u allocation_cases=800 checks=%u\n",files,checks);
    return 0;
}
