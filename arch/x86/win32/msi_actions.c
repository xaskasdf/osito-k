#include "msi_actions.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern void serial_puts(const char *text);
extern void serial_putdec(uint64_t value);

typedef struct { const char *name, *condition; int32_t sequence; } action_row_t;
static int action_cell(msi_table_view_t *table, uint32_t row, const char *column, msi_value_t *value)
{
    int index = msi_table_column(table, column);
    return index >= 0 && msi_table_value(table, row, (uint32_t)index, value);
}
static int action_table_exists(msi_db_t *database, const char *name)
{
    for (uint32_t i = 0; i < msi_table_count(database); i++)
        if (!strcmp(msi_table_name(database, i), name)) return 1;
    return 0;
}
uint32_t msi_actions_walk(msi_db_t *database, msi_package_t *package,
    msi_action_visitor_t visitor, void *context)
{
    if (!database || !package || !visitor) return 87;
    msi_table_view_t *table = msi_table_open(database, "InstallExecuteSequence");
    int has_custom = action_table_exists(database, "CustomAction");
    msi_table_view_t *custom = has_custom ? msi_table_open(database, "CustomAction") : NULL;
    uint32_t error = !table || (has_custom && !custom) ? 1620 : 0;
    uint32_t count = msi_table_rows(table);
    if (!error && (!count || count > 8192)) error = 1620;
    action_row_t *rows = !error ? kmalloc((uint64_t)count * sizeof(*rows)) : NULL;
    if (!error && !rows) error = 8;
    for (uint32_t r = 0; !error && r < count; r++) {
        msi_value_t name, condition, sequence;
        if (!action_cell(table, r, "Action", &name) || name.kind != MSI_VALUE_STRING || !*name.string ||
            !action_cell(table, r, "Condition", &condition) ||
            (condition.kind != MSI_VALUE_STRING && condition.kind != MSI_VALUE_NULL) ||
            !action_cell(table, r, "Sequence", &sequence) || sequence.kind != MSI_VALUE_INTEGER) { error = 13; break; }
        rows[r] = (action_row_t){name.string, condition.kind == MSI_VALUE_STRING ? condition.string : "", sequence.integer};
        if (sequence.integer < 0) { error = 120; break; }
        for (uint32_t previous = 0; previous < r; previous++)
            if (!strcmp(rows[previous].name, rows[r].name) ||
                (rows[r].sequence > 0 && rows[previous].sequence == rows[r].sequence)) error = 13;
    }
    /* Sequence tables are small; insertion sort avoids any libc dependency. */
    for (uint32_t i = 1; !error && i < count; i++) {
        action_row_t row = rows[i]; uint32_t at = i;
        while (at && rows[at-1].sequence > row.sequence) { rows[at] = rows[at-1]; at--; }
        rows[at] = row;
    }
    for (uint32_t i = 0; !error && i < count; i++) {
        if (!rows[i].sequence) continue;
        uint16_t *condition = msi_package_decode(package, rows[i].condition);
        if (!condition) { error = 8; break; }
        int enabled = msi_package_condition(package, condition); kfree(condition);
        if (enabled == 3) { error = 1620; break; }
        if (!enabled) continue;
        int is_custom = 0;
        for (uint32_t r = 0; !error && r < msi_table_rows(custom); r++) {
            msi_value_t name, type;
            if (!action_cell(custom, r, "Action", &name) || name.kind != MSI_VALUE_STRING) { error = 13; break; }
            if (strcmp(name.string, rows[i].name)) continue;
            is_custom = 1;
            if (!action_cell(custom, r, "Type", &type) || type.kind != MSI_VALUE_INTEGER) error = 13;
            else error = type.integer == 19 ? 1603 : 120;
        }
        if (!error && !is_custom) error = visitor(context, rows[i].name);
        if (error) {
            serial_puts("[MSI] action failed: "); serial_puts(rows[i].name);
            serial_puts(" error="); serial_putdec(error); serial_puts("\n");
        }
    }
    if (rows) kfree(rows);
    msi_table_close(custom); msi_table_close(table);
    return error;
}
