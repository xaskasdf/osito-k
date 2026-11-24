/* MSI properties and target-directory resolution shared by the installation
 * engine and its Win32 session APIs. Payload execution belongs to the engine. */
#include "msi_package.h"
#include "kernel32_shim.h"

extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
#define PACKAGE_TEXT_LIMIT 1048576U
#define PACKAGE_DIRECTORY_LIMIT 4096U

typedef struct package_property {
    struct package_property *next;
    uint16_t *name, *value;
} package_property_t;
typedef struct {
    uint16_t *name, *parent, *target;
    uint16_t *resolved;
    int visiting;
} package_directory_t;
struct msi_package {
    msi_db_t *database;
    package_property_t *properties;
    package_directory_t *directories;
    uint32_t directory_count;
    int directories_loaded;
    msi_condition_lookup_t symbols;
    void *symbol_context;
};
static const uint16_t package_empty[] = {0};
static const uint16_t package_targetdir[] = {'T','A','R','G','E','T','D','I','R',0};
static const uint16_t package_rootdrive[] = {'R','O','O','T','D','R','I','V','E',0};

static uint32_t package_length(const uint16_t *text)
{
    uint32_t length = 0;
    if (text) while (length < PACKAGE_TEXT_LIMIT && text[length]) length++;
    return length;
}
static int package_equal(const uint16_t *a, const uint16_t *b)
{
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static uint16_t *package_copy(const uint16_t *text, uint32_t length)
{
    if (length >= PACKAGE_TEXT_LIMIT) return NULL;
    uint16_t *copy = kmalloc(((uint64_t)length + 1) * 2);
    if (copy) { if (length) memcpy(copy, text, length * 2U); copy[length] = 0; }
    return copy;
}
static const uint16_t *package_property(msi_package_t *p, const uint16_t *name)
{
    for (package_property_t *prop = p->properties; prop; prop = prop->next)
        if (package_equal(prop->name, name)) return prop->value;
    return package_empty;
}
uint16_t *msi_package_decode(msi_package_t *package, const char *text)
{
    if (!package || !text) return NULL;
    uint32_t cp = msi_codepage(package->database);
    int units = MultiByteToWideChar(cp, 0, text, -1, NULL, 0);
    if (units <= 0 || (uint32_t)units > PACKAGE_TEXT_LIMIT) return NULL;
    uint16_t *result = kmalloc((uint64_t)(uint32_t)units * 2);
    if (result && MultiByteToWideChar(cp, 0, text, -1, result, units) != units) {
        kfree(result); result = NULL;
    }
    return result;
}
uint32_t msi_package_set(msi_package_t *p, const uint16_t *name, const uint16_t *value)
{
    uint32_t name_length = package_length(name), value_length = package_length(value);
    if (!p || !name_length) return 87;
    if (name_length == PACKAGE_TEXT_LIMIT || value_length == PACKAGE_TEXT_LIMIT) return 8;
    package_property_t **link = &p->properties;
    while (*link && !package_equal((*link)->name, name)) link = &(*link)->next;
    if (value_length) {
        uint16_t *copy = package_copy(value, value_length);
        if (!copy) return 8;
        if (*link) { kfree((*link)->value); (*link)->value = copy; }
        else {
            package_property_t *prop = kmalloc(sizeof(*prop));
            uint16_t *key = package_copy(name, name_length);
            if (!prop || !key) { if (prop) kfree(prop); if (key) kfree(key); kfree(copy); return 8; }
            *prop = (package_property_t){.name = key, .value = copy}; *link = prop;
        }
    } else if (*link) {
        package_property_t *prop = *link; *link = prop->next;
        kfree(prop->name); kfree(prop->value); kfree(prop);
    }
    for (uint32_t i = 0; i < p->directory_count; i++) {
        if (p->directories[i].resolved) kfree(p->directories[i].resolved);
        p->directories[i].resolved = NULL;
    }
    return 0;
}
const uint16_t *msi_package_get(msi_package_t *p, const uint16_t *name)
{
    if (!p || !name) return package_empty;
    const uint16_t *value = package_property(p, name);
    if (*value) return value;
    for (uint32_t i = 0; i < p->directory_count; i++)
        if (p->directories[i].resolved && package_equal(p->directories[i].name, name))
            return p->directories[i].resolved;
    return package_empty;
}
static int package_lookup(void *context, uint16_t prefix, const uint16_t *name, uint32_t length,
                          msi_condition_value_t *value)
{
    msi_package_t *p = context;
    if (prefix) return p->symbols ? p->symbols(p->symbol_context, prefix, name, length, value) : 0;
    /* Identifiers are slices, not necessarily terminated. */
    for (package_property_t *prop = p->properties; prop; prop = prop->next)
        if (package_length(prop->name) == length && !memcmp(prop->name, name, length * 2U)) {
            value->text = prop->value; value->length = package_length(prop->value); return 1;
        }
    for (uint32_t i = 0; i < p->directory_count; i++) {
        package_directory_t *dir = &p->directories[i];
        if (dir->resolved && package_length(dir->name) == length && !memcmp(dir->name, name, length * 2U)) {
            value->text = dir->resolved; value->length = package_length(dir->resolved); break;
        }
    }
    return 1;
}
void msi_package_symbols(msi_package_t *p, msi_condition_lookup_t lookup, void *context)
{
    if (p) { p->symbols = lookup; p->symbol_context = context; }
}
int msi_package_condition(msi_package_t *p, const uint16_t *expression)
{
    return p ? msi_condition_evaluate(expression, package_lookup, p) : 3;
}
static uint16_t *package_cell(msi_package_t *p, msi_table_view_t *view, uint32_t row, const char *column)
{
    int index = msi_table_column(view, column);
    msi_value_t value;
    if (index < 0 || !msi_table_value(view, row, (uint32_t)index, &value)) return NULL;
    if (value.kind == MSI_VALUE_NULL) return package_copy(NULL, 0);
    return value.kind == MSI_VALUE_STRING ? msi_package_decode(p, value.string) : NULL;
}
msi_package_t *msi_package_create(msi_db_t *database, uint32_t *error)
{
    if (error) *error = 87;
    if (!database) return NULL;
    msi_package_t *p = kmalloc(sizeof(*p));
    if (!p) { if (error) *error = 8; return NULL; }
    memset(p, 0, sizeof(*p)); p->database = database;
    msi_table_view_t *view = msi_table_open(database, "Property");
    uint32_t result = view ? 0 : 1620;
    if (view) for (uint32_t r = 0; !result && r < msi_table_rows(view); r++) {
        uint16_t *name = package_cell(p, view, r, "Property");
        uint16_t *value = package_cell(p, view, r, "Value");
        result = !name || !value ? 13 : msi_package_set(p, name, value);
        if (name) kfree(name);
        if (value) kfree(value);
    }
    msi_table_close(view);
    if (result) { msi_package_close(p); p = NULL; }
    if (error) *error = result;
    return p;
}
void msi_package_close(msi_package_t *p)
{
    if (!p) return;
    while (p->properties) {
        package_property_t *prop = p->properties; p->properties = prop->next;
        kfree(prop->name); kfree(prop->value); kfree(prop);
    }
    for (uint32_t i = 0; i < p->directory_count; i++) {
        package_directory_t *dir = &p->directories[i];
        if (dir->name) kfree(dir->name);
        if (dir->parent) kfree(dir->parent);
        if (dir->target) kfree(dir->target);
        if (dir->resolved) kfree(dir->resolved);
    }
    if (p->directories) kfree(p->directories);
    kfree(p);
}
static uint32_t package_load_directories(msi_package_t *p)
{
    if (p->directories_loaded) return 0;
    msi_table_view_t *view = msi_table_open(p->database, "Directory");
    if (!view) return 1606;
    uint32_t count = msi_table_rows(view), result = 0;
    if (count > PACKAGE_DIRECTORY_LIMIT) result = 8;
    package_directory_t *dirs = !result && count ? kmalloc((uint64_t)count * sizeof(*dirs)) : NULL;
    if (!result && count && !dirs) result = 8;
    if (dirs) memset(dirs, 0, count * sizeof(*dirs));
    for (uint32_t i = 0; !result && i < count; i++) {
        dirs[i].name = package_cell(p, view, i, "Directory");
        dirs[i].parent = package_cell(p, view, i, "Directory_Parent");
        dirs[i].target = package_cell(p, view, i, "DefaultDir");
        if (!dirs[i].name || !*dirs[i].name || !dirs[i].parent || !dirs[i].target || !*dirs[i].target) result = 13;
        for (uint32_t j = 0; !result && j < i; j++)
            if (package_equal(dirs[j].name, dirs[i].name)) result = 13;
    }
    msi_table_close(view);
    if (result) {
        if (dirs) for (uint32_t i = 0; i < count; i++) {
            if (dirs[i].name) kfree(dirs[i].name);
            if (dirs[i].parent) kfree(dirs[i].parent);
            if (dirs[i].target) kfree(dirs[i].target);
        }
        if (dirs) kfree(dirs);
        return result;
    }
    p->directories = dirs; p->directory_count = count; p->directories_loaded = 1;
    return 0;
}
static int package_find_directory(msi_package_t *p, const uint16_t *name)
{
    for (uint32_t i = 0; i < p->directory_count; i++)
        if (package_equal(p->directories[i].name, name)) return (int)i;
    return -1;
}
static uint32_t package_resolve_directory(msi_package_t *p, uint32_t index, unsigned depth)
{
    package_directory_t *dir = &p->directories[index];
    if (dir->resolved) return 0;
    if (dir->visiting || depth > 128) return 1606;
    dir->visiting = 1;
    uint32_t result = 0;
    const uint16_t *override = package_property(p, dir->name);
    if (!*override && package_equal(dir->name, package_targetdir)) override = package_property(p, package_rootdrive);
    if (*override) {
        uint32_t length = package_length(override);
        if (!((length >= 3 && override[1] == ':' && (override[2] == '\\' || override[2] == '/')) ||
              (length >= 3 && override[0] == '\\' && override[1] == '\\'))) result = 1606;
        else {
            dir->resolved = kmalloc(((uint64_t)length + 2) * 2);
            if (!dir->resolved) result = 8;
            else {
                for (uint32_t i = 0; i < length; i++) dir->resolved[i] = override[i] == '/' ? '\\' : override[i];
                if (dir->resolved[length - 1] != '\\') dir->resolved[length++] = '\\';
                dir->resolved[length] = 0;
            }
        }
    } else {
        int parent = package_find_directory(p, dir->parent);
        if (parent < 0) result = 1606;
        else result = package_resolve_directory(p, (uint32_t)parent, depth + 1);
        if (!result) {
            uint32_t start = 0, end = 0;
            for (; dir->target[end] && dir->target[end] != ':'; end++)
                if (dir->target[end] == '|') start = end + 1;
            const uint16_t *part = dir->target + start;
            uint32_t length = end - start;
            if (length == 1 && part[0] == '.') length = 0;
            if ((length == 2 && part[0] == '.' && part[1] == '.') || !end) result = 1606;
            for (uint32_t i = 0; !result && i < length; i++)
                if (part[i] == '\\' || part[i] == '/' || part[i] == '|' || part[i] == '*' || part[i] == '?') result = 1606;
            uint32_t base = package_length(p->directories[parent].resolved);
            if (!result && (uint64_t)base + length + 2 > PACKAGE_TEXT_LIMIT) result = 8;
            if (!result) {
                dir->resolved = kmalloc(((uint64_t)base + length + 2) * 2);
                if (!dir->resolved) result = 8;
                else {
                    memcpy(dir->resolved, p->directories[parent].resolved, base * 2U);
                    memcpy(dir->resolved + base, part, length * 2U);
                    base += length;
                    if (length) dir->resolved[base++] = '\\';
                    dir->resolved[base] = 0;
                }
            }
        }
    }
    dir->visiting = 0;
    return result;
}
uint32_t msi_package_directory(msi_package_t *p, const uint16_t *name, const uint16_t **path)
{
    if (path) *path = NULL;
    if (!p || !name || !path) return 87;
    uint32_t result = package_load_directories(p);
    if (result) return result;
    int index = package_find_directory(p, name);
    if (index < 0) return 1606;
    result = package_resolve_directory(p, (uint32_t)index, 0);
    if (!result) *path = p->directories[index].resolved;
    return result;
}
