#include "msi_plan.h"
#include "kernel32_shim.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
#define PLAN_OBJECT_LIMIT 4096U
#define PLAN_FILE_LIMIT 16384U

static uint32_t plan_length(const uint16_t *s)
{ uint32_t n = 0; if (s) while (s[n]) n++; return n; }
static const char *plan_string(msi_table_view_t *view, uint32_t row, const char *column)
{
    int index = msi_table_column(view, column); msi_value_t value;
    if (index < 0 || !msi_table_value(view, row, (uint32_t)index, &value)) return NULL;
    return value.kind == MSI_VALUE_STRING ? value.string : value.kind == MSI_VALUE_NULL ? "" : NULL;
}
static int plan_integer(msi_table_view_t *view, uint32_t row, const char *column, int32_t *value)
{
    int index = msi_table_column(view, column); msi_value_t cell;
    if (index < 0 || !msi_table_value(view, row, (uint32_t)index, &cell) || cell.kind != MSI_VALUE_INTEGER) return 0;
    *value = cell.integer; return 1;
}
static int plan_find(msi_plan_object_t *objects, uint32_t count, const char *key)
{
    if (key) for (uint32_t i = 0; i < count; i++) if (!strcmp(objects[i].key, key)) return (int)i;
    return -1;
}
static int plan_lookup(void *context, uint16_t prefix, const uint16_t *name, uint32_t length,
                       msi_condition_value_t *value)
{
    msi_plan_t *p = context;
    if (prefix == '%') return p->environment ? p->environment(p->environment_context, prefix, name, length, value) : 0;
    msi_plan_object_t *objects;
    uint32_t count;
    if (prefix == '$' || prefix == '?') { objects = p->components; count = p->component_count; }
    else if (prefix == '&' || prefix == '!') { objects = p->features; count = p->feature_count; }
    else return 0;
    for (uint32_t i = 0; i < count; i++) if (plan_length(objects[i].name) == length &&
                                           !memcmp(objects[i].name, name, length * 2U)) {
        value->is_integer = 1;
        value->integer = prefix == '$' || prefix == '&' ? objects[i].action : objects[i].installed;
        break;
    }
    return 1;
}
static uint32_t plan_objects(msi_plan_t *p, const char *table, const char *key,
                              msi_plan_object_t **objects, uint32_t *count)
{
    msi_table_view_t *view = msi_table_open(p->database, table);
    if (!view) return 1620;
    uint32_t rows = msi_table_rows(view), result = 0;
    if (!rows || rows > PLAN_OBJECT_LIMIT) result = 1620;
    msi_plan_object_t *list = !result ? kmalloc((uint64_t)rows * sizeof(*list)) : NULL;
    if (!result && !list) result = 8;
    if (list) memset(list, 0, rows * sizeof(*list));
    for (uint32_t i = 0; !result && i < rows; i++) {
        list[i].key = plan_string(view, i, key);
        int32_t attributes;
        if (!list[i].key || !*list[i].key || !plan_integer(view, i, "Attributes", &attributes)) result = 13;
        else {
            list[i].attributes = (uint32_t)attributes;
            list[i].name = msi_package_decode(p->package, list[i].key);
            if (!list[i].name) result = 8;
            list[i].installed = 2; list[i].action = -1;
            if (plan_find(list, i, list[i].key) >= 0) result = 13;
        }
    }
    msi_table_close(view);
    if (result) {
        if (list) for (uint32_t i = 0; i < rows; i++) if (list[i].name) kfree(list[i].name);
        if (list) kfree(list);
    } else { *objects = list; *count = rows; }
    return result;
}
static int plan_has_table(msi_db_t *db, const char *name)
{
    for (uint32_t i = 0; i < msi_table_count(db); i++) if (!strcmp(msi_table_name(db, i), name)) return 1;
    return 0;
}
static uint32_t plan_condition(msi_plan_t *p, const char *text, int *enabled)
{
    if (!text) return 13;
    uint16_t *wide = msi_package_decode(p->package, text);
    if (!wide) return 8;
    int result = msi_package_condition(p->package, wide);
    kfree(wide);
    if (result == 3) return 1620;
    *enabled = result != 0; return 0;
}
static uint32_t plan_select(msi_plan_t *p)
{
    /* Repair/removal, advertised and source-only selections need their own
     * execution paths; a fresh-local plan must not silently impersonate them. */
    static const char *const changes[] = {"ADDLOCAL", "ADDSOURCE", "ADDDEFAULT", "ADVERTISE", "REMOVE", "REINSTALL"};
    for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        uint16_t *key = msi_package_decode(p->package, changes[i]);
        if (!key) return 8;
        int present = *msi_package_get(p->package, key) != 0; kfree(key);
        if (present) return 120;
    }
    static const uint16_t installlevel[] = {'I','N','S','T','A','L','L','L','E','V','E','L',0};
    const uint16_t *level = msi_package_get(p->package, installlevel);
    uint32_t limit = *level ? 0 : 1;
    for (; *level; level++) {
        if (*level < '0' || *level > '9' || limit > 214748364U) return 87;
        limit = limit * 10 + *level - '0';
    }
    msi_table_view_t *view = msi_table_open(p->database, "Feature");
    msi_table_view_t *conditions = plan_has_table(p->database, "Condition") ? msi_table_open(p->database, "Condition") : NULL;
    uint32_t result = !view || (plan_has_table(p->database, "Condition") && !conditions) ? 1620 : 0;
    for (uint32_t i = 0; !result && i < p->feature_count; i++) {
        int32_t authored;
        if (!plan_integer(view, i, "Level", &authored)) { result = 13; break; }
        for (uint32_t r = 0; !result && r < msi_table_rows(conditions); r++) {
            const char *feature = plan_string(conditions, r, "Feature_");
            if (!feature) { result = 13; break; }
            if (!strcmp(feature, p->features[i].key)) {
                int enabled = 0;
                result = plan_condition(p, plan_string(conditions, r, "Condition"), &enabled);
                if (!result && enabled && !plan_integer(conditions, r, "Level", &authored)) result = 13;
            }
        }
        if (!result && authored > 0 && (uint32_t)authored <= limit) {
            if (p->features[i].attributes & 7U) result = 120;
            else p->features[i].action = 3;
        }
    }
    msi_table_close(conditions); msi_table_close(view);
    if (result) return result;
    view = msi_table_open(p->database, "FeatureComponents");
    if (!view) return 1620;
    for (uint32_t r = 0; !result && r < msi_table_rows(view); r++) {
        int feature = plan_find(p->features, p->feature_count, plan_string(view, r, "Feature_"));
        int component = plan_find(p->components, p->component_count, plan_string(view, r, "Component_"));
        if (feature < 0 || component < 0) result = 13;
        else if (p->features[feature].action == 3) p->components[component].action = 3;
    }
    msi_table_close(view);
    if (result) return result;
    view = msi_table_open(p->database, "Component");
    if (!view) return 1620;
    for (uint32_t i = 0; !result && i < p->component_count; i++) if (p->components[i].action == 3) {
        int enabled = 0;
        result = plan_condition(p, plan_string(view, i, "Condition"), &enabled);
        if (!result && !enabled) p->components[i].action = -1;
        else if (!result && (p->components[i].attributes & 2U)) result = 120;
    }
    msi_table_close(view);
    return result;
}
static uint32_t plan_filename(msi_plan_t *p, const uint16_t *directory, const char *filename, uint16_t **target)
{
    if (!filename || !*filename) return 13;
    uint16_t *text = msi_package_decode(p->package, filename);
    if (!text) return 8;
    uint16_t *name = text;
    for (uint16_t *c = text; *c; c++) if (*c == '|') name = c + 1;
    uint32_t count = plan_length(name), base = plan_length(directory), result = 0;
    if (!count || (name[0] == '.' && (count == 1 || (count == 2 && name[1] == '.')))) result = 13;
    for (uint32_t i = 0; !result && i < count; i++)
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':' || name[i] == '*' || name[i] == '?' || name[i] == '"') result = 13;
    if (!result && (uint64_t)base + count >= 1048576) result = 8;
    if (!result) {
        *target = kmalloc(((uint64_t)base + count + 1) * 2);
        if (!*target) result = 8;
        else { memcpy(*target, directory, base * 2U); memcpy(*target + base, name, (count + 1) * 2U); }
    }
    kfree(text); return result;
}
static uint32_t plan_files(msi_plan_t *p)
{
    if (!plan_has_table(p->database, "File")) return 0;
    msi_table_view_t *files = msi_table_open(p->database, "File");
    msi_table_view_t *components = msi_table_open(p->database, "Component");
    msi_table_view_t *media = msi_table_open(p->database, "Media");
    uint32_t result = !files || !components || !media ? 1620 : 0;
    uint32_t rows = msi_table_rows(files);
    if (!result && rows > PLAN_FILE_LIMIT) result = 8;
    if (!result && rows) {
        p->files = kmalloc((uint64_t)rows * sizeof(*p->files));
        if (!p->files) result = 8;
        else memset(p->files, 0, rows * sizeof(*p->files));
    }
    for (uint32_t r = 0; !result && r < rows; r++) {
        int component = plan_find(p->components, p->component_count, plan_string(files, r, "Component_"));
        if (component < 0) { result = 13; break; }
        if (p->components[component].action != 3) continue;
        msi_plan_file_t *file = &p->files[p->file_count++];
        file->component = (uint32_t)component;
        file->key = plan_string(files, r, "File");
        file->version = plan_string(files, r, "Version");
        file->language = plan_string(files, r, "Language");
        int32_t size, sequence, attributes;
        if (!file->key || !*file->key || !file->version || !file->language ||
            !plan_integer(files, r, "FileSize", &size) || size < 0 ||
            !plan_integer(files, r, "Sequence", &sequence) || sequence <= 0 ||
            !plan_integer(files, r, "Attributes", &attributes)) { result = 13; break; }
        file->size = (uint32_t)size; file->sequence = (uint32_t)sequence; file->attributes = (uint32_t)attributes;
        if (file->attributes & 8192U) { result = 120; break; } /* Loose source tree not implemented yet. */
        int32_t last = 0x7fffffff;
        for (uint32_t m = 0; !result && m < msi_table_rows(media); m++) {
            int32_t end;
            if (!plan_integer(media, m, "LastSequence", &end)) result = 13;
            else if (sequence <= end && end <= last) { last = end; file->cabinet = plan_string(media, m, "Cabinet"); }
        }
        if (!result && (!file->cabinet || !*file->cabinet)) result = 120;
        const char *dirkey = plan_string(components, (uint32_t)component, "Directory_");
        uint16_t *name = dirkey ? msi_package_decode(p->package, dirkey) : NULL;
        const uint16_t *directory = NULL;
        if (!name) result = result ? result : 13;
        if (!result) result = msi_package_directory(p->package, name, &directory);
        if (!result) result = plan_filename(p, directory, plan_string(files, r, "FileName"), &file->target);
        if (name) kfree(name);
        for (uint32_t i = 0; !result && i + 1 < p->file_count; i++)
            if (!strcmp(file->key, p->files[i].key) || CompareStringOrdinal(file->target, -1, p->files[i].target, -1, 1) == 2) result = 13;
        if (!result) p->payload_bytes += file->size;
    }
    msi_table_close(files); msi_table_close(components); msi_table_close(media);
    return result;
}
static uint32_t plan_registry(msi_plan_t *p)
{
    if (!plan_has_table(p->database, "Registry")) return 0;
    msi_table_view_t *view = msi_table_open(p->database, "Registry");
    if (!view) return 1620;
    uint32_t count = msi_table_rows(view), result = 0;
    if (count > PLAN_FILE_LIMIT) result = 8;
    if (!result && count) {
        p->registry = kmalloc((uint64_t)count * sizeof(*p->registry));
        if (!p->registry) result = 8;
    }
    for (uint32_t r = 0; !result && r < count; r++) {
        int component = plan_find(p->components, p->component_count, plan_string(view, r, "Component_"));
        if (component < 0) result = 13;
        else if (p->components[component].action == 3)
            p->registry[p->registry_count++] = (msi_plan_registry_t){.row = r, .component = (uint32_t)component};
    }
    msi_table_close(view); return result;
}
msi_plan_t *msi_plan_create_fresh(msi_db_t *database, msi_package_t *package,
    msi_condition_lookup_t environment, void *context, uint32_t *error)
{
    if (error) *error = 87;
    if (!database || !package) return NULL;
    msi_plan_t *p = kmalloc(sizeof(*p));
    if (!p) { if (error) *error = 8; return NULL; }
    memset(p, 0, sizeof(*p)); p->database = database; p->package = package;
    p->environment = environment; p->environment_context = context;
    uint32_t result = plan_objects(p, "Feature", "Feature", &p->features, &p->feature_count);
    if (!result) result = plan_objects(p, "Component", "Component", &p->components, &p->component_count);
    if (!result) { msi_package_symbols(package, plan_lookup, p); p->owns_symbols = 1; }
    if (!result) result = plan_select(p);
    if (!result) result = plan_files(p);
    if (!result) result = plan_registry(p);
    if (error) *error = result;
    if (result) { msi_plan_close(p); return NULL; }
    return p;
}
void msi_plan_close(msi_plan_t *p)
{
    if (!p) return;
    if (p->owns_symbols) msi_package_symbols(p->package, NULL, NULL);
    for (uint32_t i = 0; i < p->feature_count; i++) if (p->features[i].name) kfree(p->features[i].name);
    for (uint32_t i = 0; i < p->component_count; i++) if (p->components[i].name) kfree(p->components[i].name);
    for (uint32_t i = 0; i < p->file_count; i++) if (p->files[i].target) kfree(p->files[i].target);
    if (p->features) kfree(p->features);
    if (p->components) kfree(p->components);
    if (p->files) kfree(p->files);
    if (p->registry) kfree(p->registry);
    kfree(p);
}
