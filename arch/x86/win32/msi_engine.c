#include "msi_engine.h"
#include "msi_actions.h"
#include "msi_registration.h"
#include "msi_format.h"
#include "msi_win32_io.h"
#include "kernel32_shim.h"
#include "advapi32_shim.h"
#include "system_paths.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern void serial_puts(const char *text);
extern void serial_putdec(uint64_t value);
extern DWORD WINAPI GetVersion(void);
extern BOOL WINAPI RemoveDirectoryW(PCWSTR path);
#define EW_LITERAL(text) u##text
#define EW(text) ((const uint16_t *)EW_LITERAL(text))

typedef struct {
    msi_db_t *database;
    msi_package_t *package;
    msi_summary_t *summary;
    msi_plan_t *plan;
    msi_payload_t *payload;
    msi_registry_t *registry;
    uint8_t *data;
    uint32_t size, phase, flags, directory_count;
    uint16_t source[260], cache[260], directories[64][260];
    int (*cancelled)(void *);
    void *control;
} engine_t;
static uint32_t engine_busy;
static int engine_cancelled(void *context)
{
    engine_t *engine = context;
    return engine->cancelled && engine->cancelled(engine->control);
}
enum { ENGINE_FILES=1, ENGINE_REGISTRY=2, ENGINE_COMPONENTS=4, ENGINE_FEATURES=8,
       ENGINE_REGISTER=16, ENGINE_PUBLISH=32, ENGINE_FINALIZE=64 };

static uint32_t engine_length(const uint16_t *text)
{ uint32_t n = 0; if (text) while (n < 32768 && text[n]) n++; return n; }
static uint32_t engine_error(void)
{ uint32_t error = GetLastError(); return error ? error : 31; }
static int engine_equal(const uint16_t *a, const uint16_t *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static uint32_t engine_set_ascii(engine_t *engine, const uint16_t *name, const char *value)
{
    uint16_t text[260]; uint32_t n = 0;
    while (n < 259 && value[n]) { text[n] = (uint8_t)value[n]; n++; }
    if (value[n]) return 206;
    text[n] = 0; return msi_package_set(engine->package, name, text);
}
static uint32_t engine_set_number(engine_t *engine, const uint16_t *name, uint32_t number, int hash)
{
    uint16_t text[16]; unsigned n = 15; text[n] = 0;
    do { text[--n] = '0' + number % 10; number /= 10; } while (number);
    if (hash) text[--n] = '#';
    return msi_package_set(engine->package, name, text + n);
}
static int engine_has_table(engine_t *engine, const char *name)
{
    for (uint32_t i = 0; i < msi_table_count(engine->database); i++)
        if (!strcmp(msi_table_name(engine->database, i), name)) return 1;
    return 0;
}
static uint32_t engine_empty_table(engine_t *engine, const char *name)
{
    if (!engine_has_table(engine, name)) return 0;
    msi_table_view_t *view = msi_table_open(engine->database, name);
    uint32_t error = !view ? 1620 : msi_table_rows(view) ? 120 : 0;
    msi_table_close(view); return error;
}
static int engine_cell(msi_table_view_t *table, uint32_t row, const char *column, msi_value_t *value)
{
    int index = msi_table_column(table, column);
    return index >= 0 && msi_table_value(table, row, (uint32_t)index, value);
}
static const char *engine_string(msi_table_view_t *table, uint32_t row, const char *column)
{
    msi_value_t value;
    if (!engine_cell(table, row, column, &value)) return NULL;
    return value.kind == MSI_VALUE_STRING ? value.string : value.kind == MSI_VALUE_NULL ? "" : NULL;
}
static uint32_t engine_formatted(engine_t *engine, const char *text, uint16_t **value)
{
    if (!text) return 13;
    uint16_t *input = msi_package_decode(engine->package, text); if (!input) return 8;
    uint32_t length = 0, error = msi_format_properties(engine->package, input, value, &length);
    kfree(input);
    if (!error && engine_length(*value) != length) { kfree(*value); *value = NULL; return 13; }
    return error;
}
static uint32_t engine_command_line(engine_t *engine, const uint16_t *input)
{
    if (!input || !*input) return 0;
    uint32_t length = engine_length(input); if (length == 32768) return 1639;
    uint16_t *storage = kmalloc(((uint64_t)length + 2) * 2); if (!storage) return 8;
    uint32_t at = 0, error = 0;
    while (!error && at < length) {
        while (input[at] == ' ' || input[at] == '\t') at++;
        if (at == length) break;
        uint32_t used = 0;
        while (at < length && input[at] != '=') {
            uint16_t c = input[at++];
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            if (!((c >= 'A' && c <= 'Z') || c == '_' || (used && ((c >= '0' && c <= '9') || c == '.')))) { error = 1639; break; }
            storage[used++] = c;
        }
        if (error || !used || at == length) { error = 1639; break; }
        storage[used++] = 0; at++;
        uint16_t *value = storage + used; int quoted = 0;
        while (at < length) {
            uint16_t c = input[at++];
            if (c == '"') {
                if (quoted && input[at] == '"') { storage[used++] = '"'; at++; }
                else quoted = !quoted;
            } else if (!quoted && (c == ' ' || c == '\t')) break;
            else storage[used++] = c;
        }
        if (quoted) { error = 1639; break; }
        storage[used] = 0; error = msi_package_set(engine->package, storage, value);
    }
    kfree(storage); return error;
}
static uint32_t engine_platform(engine_t *engine)
{
    const uint16_t *platform = engine->summary->platform;
    if (engine_length(platform) < 6 || memcmp(platform, EW("Intel;"), 6 * 2U)) return 1633;
    if (engine->summary->word_count != 2 || engine->summary->installer_version > 500) return 120;
    static const struct { const uint16_t *name; const char *value; } settings[] = {
        {EW("ROOTDRIVE"), "C:\\"}, {EW("WindowsFolder"), WIN32_WINDOWS_DIRECTORY "\\"},
        {EW("SystemFolder"), WIN32_SYSWOW64_DIRECTORY "\\"}, {EW("System64Folder"), WIN32_SYSTEM32_DIRECTORY "\\"},
        {EW("ProgramFilesFolder"), "C:\\Program Files (x86)\\"}, {EW("ProgramFiles64Folder"), "C:\\Program Files\\"},
        {EW("CommonFilesFolder"), "C:\\Program Files (x86)\\Common Files\\"},
        {EW("CommonFiles64Folder"), "C:\\Program Files\\Common Files\\"},
        {EW("VersionMsi"), "500"}, {EW("UILevel"), "2"}, {EW("ACTION"), "INSTALL"}
    };
    uint32_t error = 0;
    for (unsigned i = 0; !error && i < sizeof(settings)/sizeof(settings[0]); i++)
        error = engine_set_ascii(engine, settings[i].name, settings[i].value);
    uint32_t version = GetVersion(), nt = (version & 255U) * 100 + ((version >> 8) & 255U);
    if (!error) error = engine_set_number(engine, EW("VersionNT"), nt, 0);
    if (!error) error = engine_set_number(engine, EW("VersionNT64"), nt, 0);
    if (!error) error = engine_set_number(engine, EW("WindowsBuild"), version >> 16, 0);
    if (!error) error = msi_package_set(engine->package, EW("SourceDir"), engine->source);
    if (!error) error = msi_package_set(engine->package, EW("Privileged"), advapi32_effective_is_admin() ? EW("1") : EW(""));
    if (!error) error = msi_package_set(engine->package, EW("AdminUser"), advapi32_effective_is_admin() ? EW("1") : EW(""));
    return error;
}
static uint32_t engine_source(void *context, const char *name, uint8_t **data, uint32_t *size)
{
    engine_t *engine = context;
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return 1612;
    uint16_t *leaf = msi_package_decode(engine->package, name); if (!leaf) return 8;
    uint32_t base = engine_length(engine->source), count = engine_length(leaf), error = 0;
    if (base + count >= 260) error = 206;
    for (uint32_t i = 0; !error && i < count; i++)
        if (leaf[i] == '\\' || leaf[i] == '/' || leaf[i] == ':' || leaf[i] == '*' || leaf[i] == '?') error = 1612;
    uint16_t path[260];
    if (!error) {
        memcpy(path, engine->source, base * 2U); memcpy(path + base, leaf, (count + 1) * 2U);
        int present = 0; error = msi_win32_read_file(NULL, path, data, size, &present);
        if (!error && !present) error = 1612;
    }
    kfree(leaf); return error;
}

/* Further action and execution helpers are kept below the package/source
 * setup so no caller can enter a write path without completing preparation. */

static int engine_guid_equal(const uint16_t *a, const uint16_t *b)
{
    if (!msi_registration_guid(a) || !msi_registration_guid(b)) return 0;
    for (unsigned i = 0; i < 38; i++) {
        uint16_t x = a[i], y = b[i];
        if (x >= 'a' && x <= 'f') x -= 32;
        if (y >= 'a' && y <= 'f') y -= 32;
        if (x != y) return 0;
    }
    return 1;
}
static uint32_t engine_reg_string(HKEY key, const uint16_t *name, uint16_t *text, uint32_t capacity)
{
    DWORD type = 0, size = capacity * 2U;
    uint32_t error = RegQueryValueExW(key, name, NULL, &type, (BYTE *)text, &size);
    if (!error && (type != REG_SZ || size < 2 || (size & 1) || size > capacity * 2U || text[size/2-1])) error = 13;
    if (!error && engine_length(text) != size/2-1) error = 13;
    return error;
}
static uint32_t engine_existing(engine_t *engine)
{
    /* Maintenance and upgrades require a separate execution path. */
    const uint16_t *product = msi_package_get(engine->package, EW("ProductCode"));
    if (!msi_registration_guid(product)) return 1620;
    HKEY products = NULL;
    uint32_t error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MSI_PRODUCT_STORE, 0, KEY_READ | KEY_WOW64_64KEY, &products);
    if (error) return error == 2 ? 0 : error;
    msi_table_view_t *upgrade = engine_has_table(engine, "Upgrade") ? msi_table_open(engine->database, "Upgrade") : NULL;
    if (engine_has_table(engine, "Upgrade") && !upgrade) error = 1620;
    for (DWORD index = 0; !error; index++) {
        uint16_t name[256]; DWORD length = 256;
        error = RegEnumKeyExW(products, index, name, &length, NULL, NULL, NULL, NULL);
        if (error == 259) { error = 0; break; }
        if (error) break;
        if (length >= 256) { error = 13; break; }
        name[length] = 0;
        if (!msi_registration_guid(name)) { error = 13; break; }
        HKEY installed = NULL;
        error = RegOpenKeyExW(products, name, 0, KEY_READ | KEY_WOW64_64KEY, &installed);
        if (error) break;
        /* Even an incomplete prior registration needs recovery, not another
         * fresh transaction over its files and values. */
        if (engine_guid_equal(product, name)) error = 1638;
        uint16_t code[40];
        uint32_t found = !error ? engine_reg_string(installed, EW("UpgradeCode"), code, 40) : 0;
        if (!error && found && found != 2) error = found;
        for (uint32_t row = 0; !error && !found && row < msi_table_rows(upgrade); row++) {
            const char *text = engine_string(upgrade, row, "UpgradeCode");
            uint16_t *guid = text ? msi_package_decode(engine->package, text) : NULL;
            if (!guid) error = text ? 8 : 13;
            else if (!msi_registration_guid(guid)) error = 13;
            else if (engine_guid_equal(guid, code)) error = 1638;
            kfree(guid);
        }
        RegCloseKey(installed);
    }
    msi_table_close(upgrade); RegCloseKey(products);
    return error;
}
static uint32_t engine_shared_components(const msi_registry_t *registration)
{
    HKEY products = NULL;
    uint32_t error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MSI_PRODUCT_STORE, 0, KEY_READ | KEY_WOW64_64KEY, &products);
    if (error) return error == 2 ? 0 : error;
    uint32_t base = engine_length(MSI_PRODUCT_STORE) + 38;
    for (DWORD index = 0; !error; index++) {
        uint16_t name[256]; DWORD length = 256;
        error = RegEnumKeyExW(products, index, name, &length, NULL, NULL, NULL, NULL);
        if (error == 259) { error = 0; break; }
        if (error) break;
        if (length != 38) { error = 13; break; }
        name[length] = 0; HKEY product = NULL;
        if (!msi_registration_guid(name)) { error = 13; break; }
        error = RegOpenKeyExW(products, name, 0, KEY_READ | KEY_WOW64_64KEY, &product);
        if (error) break;
        for (uint32_t i = 0; !error && i < registration->count; i++) {
            const msi_registry_value_t *value = &registration->values[i];
            if (engine_length(value->path) <= base + 12 || memcmp(value->path + base, EW("\\Components\\"), 12*2U)) continue;
            /* An unselected component publishes only State=ABSENT. */
            if (engine_equal(value->name, EW("State")) && value->type == REG_DWORD && value->size == 4 && value->data[0] == 2) continue;
            HKEY component = NULL;
            error = RegOpenKeyExW(product, value->path + base + 1, 0, KEY_READ | KEY_WOW64_64KEY, &component);
            if (error == 2) { error = 0; continue; }
            if (error) break;
            DWORD type = 0, size = 0;
            error = RegQueryValueExW(component, value->name, NULL, &type, NULL, &size);
            if (!error && (type != value->type || size != value->size)) error = 1638;
            uint8_t *data = !error ? kmalloc(size ? size : 1) : NULL;
            if (!error && !data) error = 8;
            if (!error) error = RegQueryValueExW(component, value->name, NULL, &type, data, &size);
            if (!error && (type != value->type || size != value->size || (size && memcmp(data,value->data,size)))) error = 1638;
            kfree(data); RegCloseKey(component);
        }
        RegCloseKey(product);
    }
    RegCloseKey(products); return error;
}
static uint32_t engine_raw_value(engine_t *engine, const uint16_t *name, const msi_registry_before_t *value)
{
    if (value->type == REG_DWORD) {
        if (value->size != 4) return 13;
        uint32_t number; memcpy(&number, value->data, 4);
        return engine_set_number(engine, name, number, 1);
    }
    uint32_t count = 0, prefix = 0;
    const uint16_t *wide = (const uint16_t *)value->data;
    if (value->type == REG_SZ || value->type == REG_EXPAND_SZ) {
        if ((value->size & 1) || value->size < 2 || wide[value->size/2-1]) return 13;
        count = value->size/2-1;
        if (engine_length(wide) != count) return 13;
        prefix = value->type == REG_EXPAND_SZ ? 2 : count && wide[0] == '#' ? 1 : 0;
    } else if (value->type == REG_BINARY) { count = value->size * 2U; prefix = 2; }
    else return 120;
    if (count + prefix > 32767) return 8;
    uint16_t *text = kmalloc(((uint64_t)count + prefix + 1) * 2); if (!text) return 8;
    if (prefix) text[0] = '#';
    if (value->type == REG_BINARY) {
        text[1] = 'x';
        for (uint32_t i = 0; i < value->size; i++) {
            text[2+i*2] = "0123456789ABCDEF"[value->data[i] >> 4];
            text[3+i*2] = "0123456789ABCDEF"[value->data[i] & 15];
        }
    } else {
        if (prefix == 2) text[1] = '%';
        if (count) memcpy(text + prefix, wide, count * 2U);
    }
    text[count+prefix] = 0;
    uint32_t error = msi_package_set(engine->package, name, text); kfree(text); return error;
}
static uint32_t engine_appsearch(engine_t *engine)
{
    if (!engine_has_table(engine, "AppSearch")) return 0;
    static const char *const unsupported[] = {"Signature", "CompLocator", "IniLocator", "DrLocator"};
    uint32_t error = 0;
    for (unsigned i = 0; !error && i < sizeof(unsupported)/sizeof(unsupported[0]); i++)
        error = engine_empty_table(engine, unsupported[i]);
    msi_table_view_t *search = !error ? msi_table_open(engine->database, "AppSearch") : NULL;
    msi_table_view_t *locator = !error ? msi_table_open(engine->database, "RegLocator") : NULL;
    if (!error && (!search || !locator)) error = 1620;
    for (uint32_t row = 0; !error && row < msi_table_rows(search); row++) {
        const char *property = engine_string(search, row, "Property"), *signature = engine_string(search, row, "Signature_");
        if (!property || !*property || !signature || !*signature) { error = 13; break; }
        uint32_t match = UINT32_MAX;
        for (uint32_t r = 0; !error && r < msi_table_rows(locator); r++) {
            const char *name = engine_string(locator, r, "Signature_");
            if (!name) error = 13;
            else if (!strcmp(signature, name)) { if (match != UINT32_MAX) error = 13; else match = r; }
        }
        if (!error && match == UINT32_MAX) error = 120;
        msi_value_t root, type;
        if (!error && (!engine_cell(locator, match, "Root", &root) || root.kind != MSI_VALUE_INTEGER || root.integer < 0 || root.integer > 3 ||
            !engine_cell(locator, match, "Type", &type) || type.kind != MSI_VALUE_INTEGER)) error = 13;
        if (!error && type.integer != 2 && type.integer != 18) error = 120;
        msi_registry_value_t value = {0}; msi_registry_before_t before = {0};
        if (!error) error = engine_formatted(engine, engine_string(locator, match, "Key"), &value.path);
        if (!error) error = engine_formatted(engine, engine_string(locator, match, "Name"), &value.name);
        if (!error) {
            value.root = root.integer; value.view = type.integer & 16 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
            error = msi_win32_backend.read_registry(NULL, &value, &before);
        }
        if (!error && before.present) {
            uint16_t *name = msi_package_decode(engine->package, property);
            if (!name) error = 8; else error = engine_raw_value(engine, name, &before);
            kfree(name);
        }
        kfree(before.data); kfree(value.path); kfree(value.name);
    }
    msi_table_close(locator); msi_table_close(search); return error;
}
static uint32_t engine_launch_conditions(engine_t *engine)
{
    if (!engine_has_table(engine, "LaunchCondition")) return 0;
    msi_table_view_t *table = msi_table_open(engine->database, "LaunchCondition");
    uint32_t error = table ? 0 : 1620;
    for (uint32_t row = 0; !error && row < msi_table_rows(table); row++) {
        const char *condition = engine_string(table, row, "Condition");
        uint16_t *wide = condition ? msi_package_decode(engine->package, condition) : NULL;
        if (!wide) error = condition ? 8 : 13;
        else {
            int result = msi_package_condition(engine->package, wide);
            if (result == 3) error = 1620;
            else if (!result) error = 1603;
        }
        kfree(wide);
    }
    msi_table_close(table); return error;
}
static uint32_t engine_supported(engine_t *engine)
{
    static const uint16_t *const modes[] = {EW("TRANSFORMS"), EW("PATCH"), EW("REMOVE"), EW("REINSTALL"), EW("REINSTALLMODE"),
        EW("ADDLOCAL"), EW("ADDSOURCE"), EW("ADDDEFAULT"), EW("ADVERTISE"), EW("MsiPatchRemovalList"), EW("UPGRADINGPRODUCTCODE")};
    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++)
        if (*msi_package_get(engine->package, modes[i])) return 120;
    if (!engine_equal(msi_package_get(engine->package, EW("ALLUSERS")), EW("1")) ||
        !engine_equal(msi_package_get(engine->package, EW("ARPSYSTEMCOMPONENT")), EW("1")) ||
        !engine_equal(msi_package_get(engine->package, EW("ACTION")), EW("INSTALL"))) return 120;
    if (!advapi32_effective_is_admin()) return 5;
    /* Feature hierarchy and non-local component semantics need their own
     * costing rules. Reject them, including unselected authored rows. */
    msi_table_view_t *features = msi_table_open(engine->database, "Feature");
    msi_table_view_t *components = msi_table_open(engine->database, "Component");
    uint32_t error = features && components ? 0 : 1620;
    for (uint32_t i = 0; !error && i < msi_table_rows(features); i++) {
        const char *parent = engine_string(features, i, "Feature_Parent"); msi_value_t attributes;
        if (!parent || !engine_cell(features, i, "Attributes", &attributes) || attributes.kind != MSI_VALUE_INTEGER) error = 13;
        else if (*parent || (attributes.integer & ~24U)) error = 120;
    }
    for (uint32_t i = 0; !error && i < msi_table_rows(components); i++) {
        msi_value_t attributes;
        if (!engine_cell(components, i, "Attributes", &attributes) || attributes.kind != MSI_VALUE_INTEGER) error = 13;
        else if (attributes.integer & ~4U) error = 120;
    }
    msi_table_close(components); msi_table_close(features);
    /* This fresh-product entry does not evaluate machine component/feature
     * state expressions or environment lookups. Shared components are accepted
     * only after their complete registered key-path metadata matches. */
    static const struct { const char *table, *column; } conditions[] = {
        {"InstallExecuteSequence", "Condition"}, {"LaunchCondition", "Condition"},
        {"Component", "Condition"}, {"Condition", "Condition"}
    };
    for (unsigned i = 0; !error && i < sizeof(conditions)/sizeof(conditions[0]); i++) {
        if (!engine_has_table(engine, conditions[i].table)) continue;
        msi_table_view_t *table = msi_table_open(engine->database, conditions[i].table);
        if (!table) error = 1620;
        for (uint32_t row = 0; !error && row < msi_table_rows(table); row++) {
            const char *condition = engine_string(table, row, conditions[i].column);
            if (!condition) { error = 13; break; }
            int quoted = 0;
            for (; *condition; condition++) {
                if (*condition == '"') quoted = !quoted;
                if (!quoted && (*condition == '%' || *condition == '?' || *condition == '!')) { error = 120; break; }
            }
        }
        msi_table_close(table);
    }
    return error;
}
static uint32_t engine_action(void *context, const char *action)
{
    engine_t *engine = context;
    if (engine_cancelled(engine)) return 1602;
    serial_puts("[MSI-ENGINE] prepare "); serial_puts(action); serial_puts("\n");
    if (!strcmp(action, "FindRelatedProducts")) return engine_existing(engine);
    if (!strcmp(action, "LaunchConditions")) return engine_launch_conditions(engine);
    if (!strcmp(action, "AppSearch")) return engine->phase ? 1620 : engine_appsearch(engine);
    if (!strcmp(action, "CostInitialize")) {
        if (engine->phase) return 1620;
        engine->phase = 1; return 0;
    }
    if (!strcmp(action, "FileCost")) {
        if (engine->phase != 1) return 1620;
        engine->phase = 2; return 0;
    }
    if (!strcmp(action, "CostFinalize")) {
        if (engine->phase != 2) return 1620;
        uint32_t error = 0;
        engine->plan = msi_plan_create_fresh(engine->database, engine->package, NULL, NULL, &error);
        if (!error) error = msi_package_set(engine->package, EW("CostingComplete"), EW("1"));
        if (!error) engine->phase = 3;
        return error;
    }
    if (!strcmp(action, "InstallValidate")) {
        if (engine->phase != 3) return 1620;
        uint32_t error = 0;
        engine->payload = msi_payload_prepare(engine->plan, engine_source, engine, &error);
        if (!error) engine->registry = msi_registry_prepare(engine->plan, &error);
        if (!error) engine->phase = 4;
        return error;
    }
    if (!strcmp(action, "InstallInitialize")) {
        if (engine->phase != 4) return 1620;
        engine->phase = 5; return 0;
    }
    if (!strcmp(action, "InstallFinalize")) {
        if (engine->phase != 5) return 1620;
        engine->flags |= ENGINE_FINALIZE; engine->phase = 6; return 0;
    }
    static const struct { const char *action; uint32_t flag; } required[] = {
        {"InstallFiles", ENGINE_FILES}, {"WriteRegistryValues", ENGINE_REGISTRY}, {"ProcessComponents", ENGINE_COMPONENTS},
        {"PublishFeatures", ENGINE_FEATURES}, {"RegisterProduct", ENGINE_REGISTER}, {"PublishProduct", ENGINE_PUBLISH}
    };
    for (unsigned i = 0; i < sizeof(required)/sizeof(required[0]); i++) if (!strcmp(action, required[i].action)) {
        if (engine->phase != 5) return 1620;
        engine->flags |= required[i].flag; return 0;
    }
    if (!strcmp(action, "ValidateProductID"))
        return *msi_package_get(engine->package, EW("PIDKEY")) || *msi_package_get(engine->package, EW("PIDTemplate")) ? 120 : 0;
    if (!strcmp(action, "AllocateRegistrySpace")) return *msi_package_get(engine->package, EW("RegistrySpace")) ? 120 : 0;
    if (!strcmp(action, "RegisterUser"))
        return *msi_package_get(engine->package, EW("USERNAME")) || *msi_package_get(engine->package, EW("COMPANYNAME")) ? 120 : 0;
    if (!strcmp(action, "UnpublishFeatures")) return 0; /* The product is verified absent. */
    if (!strcmp(action, "RemoveExistingProducts")) return engine_existing(engine);
    static const struct { const char *action, *table, *second, *third; } empty[] = {
        {"SetODBCFolders", "ODBCDriver", "ODBCTranslator", "ODBCDataSource"},
        {"InstallODBC", "ODBCDriver", "ODBCTranslator", "ODBCDataSource"}, {"RemoveODBC", "ODBCDriver", "ODBCTranslator", "ODBCDataSource"},
        {"RegisterClassInfo", "Class", "ProgId", "Extension"}, {"UnregisterClassInfo", "Class", "ProgId", "Extension"},
        {"RegisterMIMEInfo", "MIME", NULL, NULL}, {"UnregisterMIMEInfo", "MIME", NULL, NULL},
        {"PublishComponents", "PublishComponent", NULL, NULL}, {"UnpublishComponents", "PublishComponent", NULL, NULL},
        {"MsiPublishAssemblies", "MsiAssembly", NULL, NULL}, {"MsiUnpublishAssemblies", "MsiAssembly", NULL, NULL},
        {"StopServices", "ServiceControl", NULL, NULL}, {"DeleteServices", "ServiceControl", NULL, NULL},
        {"StartServices", "ServiceControl", NULL, NULL}, {"InstallServices", "ServiceInstall", NULL, NULL},
        {"UnregisterComPlus", "Complus", NULL, NULL}, {"RegisterComPlus", "Complus", NULL, NULL},
        {"SelfUnregModules", "SelfReg", NULL, NULL}, {"SelfRegModules", "SelfReg", NULL, NULL},
        {"UnregisterTypeLibraries", "TypeLib", NULL, NULL}, {"RegisterTypeLibraries", "TypeLib", NULL, NULL},
        {"UnregisterFonts", "Font", NULL, NULL}, {"RegisterFonts", "Font", NULL, NULL},
        {"RemoveRegistryValues", "RemoveRegistry", NULL, NULL},
        {"RemoveIniValues", "RemoveIniFile", "IniFile", NULL}, {"WriteIniValues", "IniFile", NULL, NULL},
        {"RemoveShortcuts", "Shortcut", NULL, NULL}, {"CreateShortcuts", "Shortcut", NULL, NULL},
        {"RemoveEnvironmentStrings", "Environment", NULL, NULL}, {"WriteEnvironmentStrings", "Environment", NULL, NULL},
        {"RemoveDuplicateFiles", "DuplicateFile", NULL, NULL}, {"DuplicateFiles", "DuplicateFile", NULL, NULL},
        {"RemoveFiles", "RemoveFile", NULL, NULL}, {"MoveFiles", "MoveFile", NULL, NULL},
        {"RemoveFolders", "CreateFolder", NULL, NULL}, {"CreateFolders", "CreateFolder", NULL, NULL},
        {"BindImage", "BindImage", NULL, NULL}
    };
    for (unsigned i = 0; i < sizeof(empty)/sizeof(empty[0]); i++) if (!strcmp(action, empty[i].action)) {
        uint32_t error = engine_empty_table(engine, empty[i].table);
        if (!error && empty[i].second) error = engine_empty_table(engine, empty[i].second);
        if (!error && empty[i].third) error = engine_empty_table(engine, empty[i].third);
        return error;
    }
    return 120;
}
static uint32_t engine_parents(engine_t *engine, const uint16_t *path)
{
    uint32_t length = engine_length(path);
    if (length >= 260 || length < 4 || path[1] != ':' || path[2] != '\\') return 123;
    uint16_t parent[260]; memcpy(parent, path, (length + 1) * 2U);
    for (uint32_t i = 3; i < length; i++) if (parent[i] == '\\') {
        parent[i] = 0;
        DWORD attributes = GetFileAttributesW(parent);
        if (attributes != UINT32_MAX) {
            if (!(attributes & 16)) return 267;
        } else {
            uint32_t error = engine_error();
            if (error != 2 && error != 3) return error;
            if (engine->directory_count == 64) return 8;
            if (!CreateDirectoryW(parent, NULL)) {
                error = engine_error();
                DWORD current = GetFileAttributesW(parent);
                if (error != 183 || current == UINT32_MAX || !(current & 16)) return error;
            } else memcpy(engine->directories[engine->directory_count++], parent, (i + 1) * 2U);
        }
        parent[i] = '\\';
    }
    return 0;
}
static uint32_t engine_create_file(void *context, const uint16_t *path, const uint8_t *data, uint32_t size, int *created)
{
    engine_t *engine = context; *created = 0;
    uint32_t error = engine_parents(engine, path);
    if (!error) error = msi_win32_backend.create_file(NULL, path, data, size, created);
    if (!error) {
        const msi_payload_file_t *payload = NULL;
        const msi_plan_file_t *file = NULL;
        for (uint32_t i = 0; i < engine->plan->file_count; i++) if (engine_equal(engine->plan->files[i].target, path)) {
            payload = &engine->payload->files[i]; file = &engine->plan->files[i]; break;
        }
        if (!file || !payload) return 13;
        if (file->component != UINT32_MAX) {
            typedef BOOL (WINAPI *dos_time_t)(WORD, WORD, void *);
            typedef BOOL (WINAPI *set_time_t)(HANDLE, const void *, const void *, const void *);
            dos_time_t convert = (dos_time_t)kernel32_resolve("DosDateTimeToFileTime", 0, FALSE);
            set_time_t set_time = (set_time_t)kernel32_resolve("SetFileTime", 0, FALSE);
            uint64_t stamp;
            if (!convert || !set_time) return 120;
            if (!convert(payload->date, payload->time, &stamp)) return engine_error();
            HANDLE handle = CreateFileW(path, 0x40000000U, 0, NULL, 3, 0x80, NULL);
            if (handle == INVALID_HANDLE_VALUE) return engine_error();
            if (!set_time(handle, NULL, NULL, &stamp)) error = engine_error();
            if (!CloseHandle(handle) && !error) error = engine_error();
            /* MSI File.Attributes controls target attributes; CAB attributes
             * describe the source member and its filename encoding. */
            DWORD attributes = file->attributes & 7U;
            if (!error && !SetFileAttributesW(path, attributes ? attributes : 0x80)) error = engine_error();
        }
    }
    return error;
}
static uint32_t engine_remove_file(void *context, const uint16_t *path)
{
    (void)context;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != UINT32_MAX && (attributes & 1) && !SetFileAttributesW(path, attributes & ~1U)) return engine_error();
    return msi_win32_backend.remove_file(NULL, path);
}
static uint32_t engine_directories_rollback(engine_t *engine)
{
    uint32_t error = 0;
    while (engine->directory_count) {
        const uint16_t *path = engine->directories[--engine->directory_count];
        if (!RemoveDirectoryW(path)) { uint32_t rc = engine_error(); if (rc != 2 && rc != 3 && !error) error = rc; }
    }
    return error;
}
uint32_t msi_engine_install_controlled(const uint16_t *path, const uint16_t *properties,
    int (*cancelled)(void *), void *context)
{
    if (!path || !*path) return 87;
    if (__atomic_exchange_n(&engine_busy, 1, __ATOMIC_ACQUIRE)) return 1618;
    engine_t *engine = kmalloc(sizeof(*engine));
    uint32_t error = engine ? 0 : 8; msi_apply_result_t applied = {0};
    if (engine) {
        memset(engine, 0, sizeof(*engine));
        engine->cancelled = cancelled; engine->control = context;
        if (engine_cancelled(engine)) error = 1602;
    }
    uint16_t fullpath[260];
    if (!error) {
        DWORD length = GetFullPathNameW(path, 260, fullpath, NULL);
        if (!length) error = engine_error(); else if (length >= 260) error = 206;
        else {
            while (length && fullpath[length-1] != '\\') length--;
            if (!length) error = 123;
            else { memcpy(engine->source, fullpath, length * 2U); engine->source[length] = 0; }
        }
    }
    int present = 0;
    if (!error) error = msi_win32_read_file(NULL, fullpath, &engine->data, &engine->size, &present);
    if (!error && !present) error = 1619;
    if (!error && !(engine->database = msi_open(engine->data, engine->size))) error = 1620;
    if (!error) engine->summary = msi_summary_read(engine->database, &error);
    if (!error) engine->package = msi_package_create(engine->database, &error);
    if (!error) error = engine_platform(engine);
    if (!error) error = engine_command_line(engine, properties);
    if (!error) error = engine_supported(engine);
    if (!error) error = engine_existing(engine);
    if (!error) error = msi_package_set(engine->package, EW("Installed"), EW(""));
    if (!error) error = msi_package_set(engine->package, EW("ProductState"), EW("-1"));
    if (!error) error = msi_actions_walk(engine->database, engine->package, engine_action, engine);
    if (!error && (engine->phase != 6 || engine->flags != 127)) error = 1620;
    msi_registry_t *registration = NULL;
    if (!error) {
        const uint16_t *prefix = EW(WIN32_WINDOWS_DIRECTORY "\\Installer\\");
        uint32_t length = engine_length(prefix);
        if (!msi_registration_guid(engine->summary->package_code) || length + 42 >= 260) error = 1620;
        else {
            memcpy(engine->cache, prefix, length * 2U);
            memcpy(engine->cache + length, engine->summary->package_code, 38 * 2U);
            memcpy(engine->cache + length + 38, EW(".msi"), 5 * 2U);
            registration = msi_registration_prepare(engine->plan, engine->summary, engine->registry, engine->source, engine->cache, &error);
        }
    }
    if (!error) error = engine_shared_components(registration);
    if (!error) {
        msi_registry_t *merged = msi_registry_merge(&engine->registry, &registration, &error);
        if (!error) engine->registry = merged;
    }
    if (!error) error = msi_payload_stage_cache(engine->plan, engine->payload, engine->cache, engine->data, engine->size);
    if (!error) {
        msi_apply_backend_t backend = msi_win32_backend;
        backend.create_file = engine_create_file; backend.remove_file = engine_remove_file;
        backend.cancelled = engine_cancelled;
        error = msi_apply_fresh(engine->plan, engine->payload, engine->registry, &backend, engine, &applied);
    }
    if (error && engine) {
        uint32_t rc = engine_directories_rollback(engine);
        if (!applied.rollback_error) applied.rollback_error = rc;
    }
    serial_puts("[MSI-ENGINE] result="); serial_putdec(error);
    serial_puts(" created="); serial_putdec(applied.files_created);
    serial_puts(" retained="); serial_putdec(applied.files_retained);
    serial_puts(" registry="); serial_putdec(applied.values_written);
    serial_puts(" rollback="); serial_putdec(applied.rollback_error); serial_puts("\n");
    msi_registry_close(registration);
    if (engine) {
        msi_registry_close(engine->registry); msi_payload_close(engine->payload); msi_plan_close(engine->plan);
        msi_package_close(engine->package); msi_summary_close(engine->summary); msi_close(engine->database);
        kfree(engine->data); kfree(engine);
    }
    __atomic_store_n(&engine_busy, 0, __ATOMIC_RELEASE);
    return error;
}
uint32_t msi_engine_install_fresh(const uint16_t *path, const uint16_t *properties)
{
    return msi_engine_install_controlled(path, properties, NULL, NULL);
}
