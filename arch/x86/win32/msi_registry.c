#include "msi_registry.h"
#include "msi_format.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

static int registry_cell(msi_table_view_t *table, uint32_t row, const char *column, msi_value_t *value)
{
    int index = msi_table_column(table, column);
    return index >= 0 && msi_table_value(table, row, (uint32_t)index, value);
}
static uint32_t registry_text(msi_package_t *package, const msi_value_t *cell,
                              uint16_t **text, uint32_t *length)
{
    if (cell->kind != MSI_VALUE_STRING && cell->kind != MSI_VALUE_NULL) return 13;
    uint16_t *wide = msi_package_decode(package, cell->kind == MSI_VALUE_NULL ? "" : cell->string);
    if (!wide) return 8;
    uint32_t error = msi_format_properties(package, wide, text, length);
    kfree(wide); return error;
}
static int registry_hex(uint16_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static uint32_t registry_data(msi_registry_value_t *value, const uint16_t *text, uint32_t length)
{
    int nul = 0;
    for (uint32_t i = 0; i < length; i++) if (!text[i]) nul = 1;
    uint32_t offset = 0, type = 1, bytes = 0, number = 0;
    if (length && text[0] == '#') {
        offset = 1;
        if (length > 1 && text[1] == '#') type = 1;
        else if (length > 1 && text[1] == '%') { type = 2; offset = 2; }
        else if (length > 1 && text[1] == 'x') { type = 3; offset = 2; }
        else {
            type = 4;
            int negative = 0;
            if (offset < length && (text[offset] == '-' || text[offset] == '+')) negative = text[offset++] == '-';
            if (offset == length || nul) return 13;
            uint64_t accumulator = 0;
            for (uint32_t i = offset; i < length; i++) {
                if (text[i] < '0' || text[i] > '9') return 13;
                accumulator = accumulator * 10 + text[i] - '0';
                if (accumulator > (negative ? 2147483648ULL : 4294967295ULL)) return 13;
            }
            number = negative ? 0U - (uint32_t)accumulator : (uint32_t)accumulator;
        }
    } else if (nul) {
        type = 7;
        int leading = !text[0], trailing = !text[length - 1];
        if (leading != trailing) return 120; /* Needs a merge with existing registry data. */
        if (leading) { offset++; if (length > offset) length--; }
    }
    if ((type == 2 || type == 3) && nul) return 13;
    if (type == 3) {
        if ((length - offset) & 1U) return 13;
        bytes = (length - offset) / 2;
        for (uint32_t i = offset; i < length; i++) if (registry_hex(text[i]) < 0) return 13;
    } else if (type == 4) bytes = 4;
    else bytes = (length - offset + (type == 7 ? 2U : 1U)) * 2U;
    uint8_t *data = kmalloc(bytes ? bytes : 1);
    if (!data) return 8;
    if (type == 4) for (unsigned i = 0; i < 4; i++) data[i] = (uint8_t)(number >> (8 * i));
    else if (type == 3) for (uint32_t i = 0; i < bytes; i++)
        data[i] = (uint8_t)((registry_hex(text[offset + 2*i]) << 4) | registry_hex(text[offset + 2*i + 1]));
    else { memset(data, 0, bytes); memcpy(data, text + offset, (length - offset) * 2U); }
    value->data = data; value->size = bytes; value->type = type;
    return 0;
}
msi_registry_t *msi_registry_prepare(msi_plan_t *plan, uint32_t *error)
{
    if (error) *error = 87;
    if (!plan) return NULL;
    msi_registry_t *output = kmalloc(sizeof(*output));
    if (!output) { if (error) *error = 8; return NULL; }
    memset(output, 0, sizeof(*output));
    if (!plan->registry_count) { if (error) *error = 0; return output; }
    output->values = kmalloc((uint64_t)plan->registry_count * sizeof(*output->values));
    if (!output->values) { kfree(output); if (error) *error = 8; return NULL; }
    memset(output->values, 0, plan->registry_count * sizeof(*output->values));
    msi_table_view_t *view = msi_table_open(plan->database, "Registry");
    uint32_t result = view ? 0 : 1620;
    for (uint32_t i = 0; !result && i < plan->registry_count; i++) {
        msi_value_t key, root, path, name, data;
        uint32_t row = plan->registry[i].row, component = plan->registry[i].component;
        if (component >= plan->component_count ||
            !registry_cell(view, row, "Registry", &key) || key.kind != MSI_VALUE_STRING || !*key.string ||
            !registry_cell(view, row, "Root", &root) || root.kind != MSI_VALUE_INTEGER ||
            !registry_cell(view, row, "Key", &path) || path.kind != MSI_VALUE_STRING ||
            !registry_cell(view, row, "Name", &name) ||
            !registry_cell(view, row, "Value", &data)) { result = 13; break; }
        if (data.kind == MSI_VALUE_NULL && name.kind == MSI_VALUE_STRING && !strcmp(name.string, "-")) continue;
        msi_registry_value_t *value = &output->values[output->count++];
        value->key = key.string; value->component = component;
        value->view = plan->components[component].attributes & 256U ? 0x100 : 0x200;
        int r = root.integer;
        if (r == -1) {
            static const uint16_t allusers[] = {'A','L','L','U','S','E','R','S',0};
            const uint16_t *setting = msi_package_get(plan->package, allusers);
            if (setting[0] == '1' && !setting[1]) r = 2;
            else if (!*setting) r = 1;
            else { result = 120; break; }
        }
        if (r < 0 || r > 3) { result = 13; break; }
        value->root = (uint32_t)r;
        uint32_t path_length = 0, name_length = 0;
        result = registry_text(plan->package, &path, &value->path, &path_length);
        if (!result && !path_length) result = 13;
        if (!result) result = registry_text(plan->package, &name, &value->name, &name_length);
        for (uint32_t c = 0; !result && c < path_length; c++) if (!value->path[c]) result = 13;
        for (uint32_t c = 0; !result && c < name_length; c++) if (!value->name[c]) result = 13;
        if (result) break;
        if (data.kind == MSI_VALUE_NULL && name_length == 1 && (value->name[0] == '+' || value->name[0] == '*')) {
            value->create_only = 1; value->delete_on_uninstall = value->name[0] == '*';
        } else {
            uint16_t *text = NULL; uint32_t length = 0;
            result = registry_text(plan->package, &data, &text, &length);
            if (!result) result = registry_data(value, text, length);
            if (text) kfree(text);
        }
    }
    msi_table_close(view);
    if (error) *error = result;
    if (result) { msi_registry_close(output); return NULL; }
    return output;
}
void msi_registry_close(msi_registry_t *registry)
{
    if (!registry) return;
    for (uint32_t i = 0; i < registry->count; i++) {
        msi_registry_value_t *value = &registry->values[i];
        if (value->path) kfree(value->path);
        if (value->name) kfree(value->name);
        if (value->data) kfree(value->data);
    }
    if (registry->values) kfree(registry->values);
    kfree(registry);
}

msi_registry_t *msi_registry_merge(msi_registry_t **first, msi_registry_t **last, uint32_t *error)
{
    if (error) *error = 87;
    if (!first || !last || !*first || !*last || *first == *last) return NULL;
    uint64_t count = (uint64_t)(*first)->count + (*last)->count;
    if (count > 65536) { if (error) *error = 8; return NULL; }
    msi_registry_t *merged = kmalloc(sizeof(*merged));
    if (!merged) { if (error) *error = 8; return NULL; }
    memset(merged, 0, sizeof(*merged));
    if (count) {
        merged->values = kmalloc(count * sizeof(*merged->values));
        if (!merged->values) { kfree(merged); if (error) *error = 8; return NULL; }
        if ((*first)->count) memcpy(merged->values, (*first)->values, (*first)->count * sizeof(*merged->values));
        if ((*last)->count) memcpy(merged->values + (*first)->count, (*last)->values, (*last)->count * sizeof(*merged->values));
    }
    merged->count = (uint32_t)count;
    (*first)->count = 0; (*last)->count = 0;
    msi_registry_close(*first); msi_registry_close(*last); *first = NULL; *last = NULL;
    if (error) *error = 0;
    return merged;
}
