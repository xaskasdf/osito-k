#include "msi_registration.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
#define RW(text) ((const uint16_t *)u##text)
static uint32_t registration_length(const uint16_t *text)
{ uint32_t n = 0; if (text) while (text[n]) n++; return n; }
static uint16_t *registration_join(const uint16_t *a, const uint16_t *b)
{
    uint32_t na = registration_length(a), nb = registration_length(b);
    if ((uint64_t)na + nb > 4096) return NULL;
    uint16_t *text = kmalloc(((uint64_t)na + nb + 1) * 2);
    if (text) {
        if (na) memcpy(text, a, na * 2U);
        if (nb) memcpy(text + na, b, nb * 2U);
        text[na+nb] = 0;
    }
    return text;
}
int msi_registration_guid(const uint16_t *text)
{
    if (registration_length(text) != 38 || text[0] != '{' || text[37] != '}') return 0;
    for (unsigned i = 1; i < 37; i++) {
        uint16_t c = text[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}
static uint32_t registration_add(msi_registry_t *list, const uint16_t *path,
    const uint16_t *name, const void *data, uint32_t size, uint32_t type)
{
    msi_registry_value_t *value = &list->values[list->count++];
    value->key = "OsitoK.MsiRegistration"; value->root = 2; value->view = 0x100;
    value->path = registration_join(path, NULL); value->name = registration_join(name, NULL);
    value->data = kmalloc(size ? size : 1);
    if (!value->path || !value->name || !value->data) return 8;
    if (size) memcpy(value->data, data, size);
    value->size = size; value->type = type; return 0;
}
static uint32_t registration_string(msi_registry_t *list, const uint16_t *path,
    const uint16_t *name, const uint16_t *text)
{ return registration_add(list, path, name, text, (registration_length(text) + 1) * 2U, 1); }
static uint32_t registration_number(msi_registry_t *list, const uint16_t *path,
    const uint16_t *name, uint32_t number)
{
    uint8_t data[4]; for (unsigned i = 0; i < 4; i++) data[i] = (uint8_t)(number >> (8 * i));
    return registration_add(list, path, name, data, 4, 4);
}
static const char *registration_cell(msi_table_view_t *view, uint32_t row, const char *column)
{
    int index = msi_table_column(view, column); msi_value_t value;
    if (index < 0 || !msi_table_value(view, row, (uint32_t)index, &value)) return NULL;
    return value.kind == MSI_VALUE_STRING ? value.string : value.kind == MSI_VALUE_NULL ? "" : NULL;
}
static uint32_t registration_component(msi_plan_t *plan, const msi_registry_t *authored,
    msi_table_view_t *table, uint32_t row, msi_registry_t *list, const uint16_t *path)
{
    const char *key = registration_cell(table, row, "KeyPath");
    if (!key) return 13;
    uint32_t error = 0;
    const uint16_t *target = NULL;
    if (!*key) {
        const char *dir = registration_cell(table, row, "Directory_");
        uint16_t *name = dir ? msi_package_decode(plan->package, dir) : NULL;
        if (!name) return 13;
        error = msi_package_directory(plan->package, name, &target); kfree(name);
        if (!error) error = registration_number(list, path, RW("Kind"), 3);
    } else if (plan->components[row].attributes & 4) {
        const msi_registry_value_t *value = NULL;
        for (uint32_t i = 0; i < authored->count; i++) if (!strcmp(authored->values[i].key, key)) value = &authored->values[i];
        if (!value || value->component != row) return 13;
        target = value->path;
        error = registration_number(list, path, RW("Kind"), 2);
        if (!error) error = registration_number(list, path, RW("Root"), value->root);
        if (!error) error = registration_number(list, path, RW("View"), value->view);
        if (!error) error = registration_string(list, path, RW("Name"), value->create_only ? RW("") : value->name);
        if (!error) error = registration_number(list, path, RW("KeyOnly"), value->create_only != 0);
    } else {
        for (uint32_t i = 0; i < plan->file_count; i++) if (!strcmp(plan->files[i].key, key)) {
            if (plan->files[i].component != row) return 13;
            target = plan->files[i].target;
        }
        if (!target) return 13;
        error = registration_number(list, path, RW("Kind"), 1);
    }
    if (!error) error = registration_string(list, path, RW("Path"), target);
    if (!error) error = registration_number(list, path, RW("State"), 3);
    return error;
}
msi_registry_t *msi_registration_prepare(msi_plan_t *plan, const msi_summary_t *summary,
    const msi_registry_t *authored, const uint16_t *source, const uint16_t *cache, uint32_t *error)
{
    if (error) *error = 87;
    if (!plan || !summary || !summary->platform || !*summary->platform || !authored ||
        !source || !*source || !cache || !*cache) return NULL;
    const uint16_t *product = msi_package_get(plan->package, RW("ProductCode"));
    const uint16_t *allusers = msi_package_get(plan->package, RW("ALLUSERS"));
    if (!msi_registration_guid(product) || !msi_registration_guid(summary->package_code)) { if (error) *error = 1620; return NULL; }
    if (allusers[0] != '1' || allusers[1]) { if (error) *error = 120; return NULL; }
    msi_registry_t *list = kmalloc(sizeof(*list));
    if (!list) { if (error) *error = 8; return NULL; }
    memset(list, 0, sizeof(*list));
    uint64_t capacity = 16ULL + plan->feature_count + (uint64_t)plan->component_count * 8;
    uint32_t result = capacity > 65536 ? 8 : 0;
    if (!result) {
        list->values = kmalloc(capacity * sizeof(*list->values));
        if (!list->values) result = 8;
        else memset(list->values, 0, capacity * sizeof(*list->values));
    }
    uint16_t *path = !result ? registration_join(MSI_PRODUCT_STORE, product) : NULL;
    if (!result && !path) result = 8;
    static const uint16_t *const properties[] = {RW("ProductCode"), RW("ProductName"), RW("ProductVersion"), RW("ProductLanguage"), RW("UpgradeCode"), RW("Manufacturer")};
    for (unsigned i = 0; !result && i < sizeof(properties)/sizeof(properties[0]); i++)
        result = registration_string(list, path, properties[i], msi_package_get(plan->package, properties[i]));
    if (!result) result = registration_string(list, path, RW("PackageCode"), summary->package_code);
    if (!result) result = registration_string(list, path, RW("Template"), summary->platform);
    if (!result) result = registration_string(list, path, RW("LocalPackage"), cache);
    if (!result) result = registration_string(list, path, RW("InstallSource"), source);
    if (!result) result = registration_number(list, path, RW("SchemaVersion"), 1);
    if (!result) result = registration_number(list, path, RW("Context"), 4);
    uint16_t *features = !result ? registration_join(path, RW("\\Features")) : NULL;
    if (!result && !features) result = 8;
    for (uint32_t i = 0; !result && i < plan->feature_count; i++)
        result = registration_number(list, features, plan->features[i].name, plan->features[i].action == 3 ? 3 : 2);
    if (features) kfree(features);
    msi_table_view_t *components = !result ? msi_table_open(plan->database, "Component") : NULL;
    if (!result && (!components || msi_table_rows(components) != plan->component_count)) result = 13;
    uint16_t *base = !result ? registration_join(path, RW("\\Components\\")) : NULL;
    if (!result && !base) result = 8;
    for (uint32_t i = 0; !result && i < plan->component_count; i++) {
        const char *id = registration_cell(components, i, "ComponentId");
        if (!id) { result = 13; break; }
        if (!*id) continue; /* An unmanaged component deliberately has no identity. */
        uint16_t *guid = msi_package_decode(plan->package, id);
        if (!guid) { result = 8; break; }
        if (!msi_registration_guid(guid)) result = 13;
        uint16_t *component = !result ? registration_join(base, guid) : NULL;
        kfree(guid);
        if (!result && !component) result = 8;
        if (!result && plan->components[i].action == 3) result = registration_component(plan, authored, components, i, list, component);
        else if (!result) result = registration_number(list, component, RW("State"), 2);
        if (component) kfree(component);
    }
    msi_table_close(components);
    if (base) kfree(base);
    if (!result) result = registration_number(list, path, RW("State"), 5);
    if (path) kfree(path);
    if (error) *error = result;
    if (result) { msi_registry_close(list); return NULL; }
    return list;
}
