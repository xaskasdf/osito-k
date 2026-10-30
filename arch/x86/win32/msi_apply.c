#include "msi_apply.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

uint32_t msi_apply_fresh(const msi_plan_t *plan, const msi_payload_t *payload,
    const msi_registry_t *registry, const msi_apply_backend_t *backend,
    void *context, msi_apply_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
    if (!plan || !payload || !registry || !backend || !result ||
        !backend->read_file || !backend->create_file || !backend->remove_file ||
        !backend->read_registry || !backend->write_registry || !backend->restore_registry || !backend->flush ||
        payload->count != plan->file_count) return 87;
    uint8_t *files = plan->file_count ? kmalloc(plan->file_count) : NULL;
    uint8_t *changed = registry->count ? kmalloc(registry->count) : NULL;
    msi_registry_before_t *before = registry->count ? kmalloc((uint64_t)registry->count * sizeof(*before)) : NULL;
    uint32_t error = ((plan->file_count && !files) || (registry->count && (!changed || !before))) ? 8 : 0;
    if (files) memset(files, 0, plan->file_count);
    if (changed) memset(changed, 0, registry->count);
    if (before) memset(before, 0, registry->count * sizeof(*before));
    uint64_t backup_bytes = 0;
    for (uint32_t i = 0; !error && i < plan->file_count; i++) {
        if (backend->cancelled && backend->cancelled(context)) { error = 1602; break; }
        if (!plan->files[i].target || !payload->files[i].bytes || payload->files[i].size != plan->files[i].size) {
            error = 13; break;
        }
        uint8_t *data = NULL; uint32_t size = 0; int present = 0;
        error = backend->read_file(context, plan->files[i].target, &data, &size, &present);
        if (!error && present) {
            if (size != payload->files[i].size || (size && (!data || memcmp(data, payload->files[i].bytes, size)))) error = 1638;
            else { files[i] = 1; result->files_retained++; }
        }
        if (data) kfree(data);
    }
    for (uint32_t i = 0; !error && i < registry->count; i++) {
        if (backend->cancelled && backend->cancelled(context)) { error = 1602; break; }
        if (!registry->values[i].path || !registry->values[i].name ||
            (!registry->values[i].create_only && registry->values[i].size && !registry->values[i].data)) { error = 13; break; }
        error = backend->read_registry(context, &registry->values[i], &before[i]);
        if (!error && ((before[i].present && !before[i].key_exists) || (before[i].size && !before[i].data))) error = 13;
        backup_bytes += before[i].size;
        if (!error && backup_bytes > 16U * 1024 * 1024) error = 8;
    }
    for (uint32_t i = 0; !error && i < plan->file_count; i++) if (!files[i]) {
        if (backend->cancelled && backend->cancelled(context)) { error = 1602; break; }
        int created = 0;
        error = backend->create_file(context, plan->files[i].target, payload->files[i].bytes, payload->files[i].size, &created);
        if (created) { files[i] = 2; result->files_created++; }
        else if (!error) error = 13;
    }
    for (uint32_t i = 0; !error && i < registry->count; i++) {
        if (backend->cancelled && backend->cancelled(context)) { error = 1602; break; }
        const msi_registry_value_t *value = &registry->values[i];
        if ((value->create_only && before[i].key_exists) ||
            (!value->create_only && before[i].present && value->type == before[i].type &&
             value->size == before[i].size && (!value->size || !memcmp(value->data, before[i].data, value->size)))) continue;
        int written = 0;
        error = backend->write_registry(context, value, &written);
        if (written) { changed[i] = 1; result->values_written++; }
        else if (!error) error = 13;
    }
    if (!error && backend->cancelled && backend->cancelled(context)) error = 1602;
    if (!error) error = backend->flush(context);
    if (!error && backend->cancelled && backend->cancelled(context)) error = 1602;
    if (error) {
        for (uint32_t i = registry->count; changed && i > 0; i--) if (changed[i - 1]) {
            uint32_t rc = backend->restore_registry(context, &registry->values[i - 1], &before[i - 1]);
            if (rc && !result->rollback_error) result->rollback_error = rc;
        }
        for (uint32_t i = plan->file_count; files && i > 0; i--) if (files[i - 1] == 2) {
            uint32_t rc = backend->remove_file(context, plan->files[i - 1].target);
            if (rc && !result->rollback_error) result->rollback_error = rc;
        }
        if (result->files_created || result->values_written) {
            uint32_t rc = backend->flush(context);
            if (rc && !result->rollback_error) result->rollback_error = rc;
        }
    }
    if (before) {
        for (uint32_t i = 0; i < registry->count; i++) if (before[i].data) kfree(before[i].data);
        kfree(before);
    }
    if (changed) kfree(changed);
    if (files) kfree(files);
    return error;
}
