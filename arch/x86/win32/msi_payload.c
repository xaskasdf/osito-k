#include "msi_payload.h"
#include "cab.h"
#include "kernel32_shim.h"
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);

msi_payload_t *msi_payload_prepare(msi_plan_t *plan, msi_payload_source_t source,
                                 void *context, uint32_t *error)
{
    if (error) *error = 87;
    if (!plan) return NULL;
    if (plan->payload_bytes > 256U * 1024 * 1024) { if (error) *error = 8; return NULL; }
    msi_payload_t *payload = kmalloc(sizeof(*payload));
    if (!payload) { if (error) *error = 8; return NULL; }
    memset(payload, 0, sizeof(*payload));
    if (plan->file_count) {
        payload->files = kmalloc((uint64_t)plan->file_count * sizeof(*payload->files));
        if (!payload->files) { kfree(payload); if (error) *error = 8; return NULL; }
        memset(payload->files, 0, plan->file_count * sizeof(*payload->files));
    }
    uint32_t result = 0;
    uint8_t *cabinet_data = NULL;
    const char *cabinet_name = NULL;
    cab_archive_t *cabinet = NULL;
    for (uint32_t i = 0; !result && i < plan->file_count; i++) {
        msi_plan_file_t *file = &plan->files[i];
        const char *name = file->cabinet;
        if (!name || !*name) { result = 1612; break; }
        if (!cabinet_name || strcmp(name, cabinet_name)) {
            if (cabinet) { cab_close(cabinet); cabinet = NULL; }
            if (cabinet_data) { kfree(cabinet_data); cabinet_data = NULL; }
            uint32_t size = 0;
            if (*name == '#') {
                cabinet_data = msi_read_stream(plan->database, name + 1, &size);
                if (!cabinet_data) result = 1612;
            } else if (source) result = source(context, name, &cabinet_data, &size);
            else result = 1612;
            if (!result && (!cabinet_data || !size || size > 128U * 1024 * 1024)) result = 13;
            if (!result) {
                cabinet = cab_open(cabinet_data, size);
                if (!cabinet) result = 13;
            }
            if (!result) {
                cab_info_t info;
                if (cab_get_info(cabinet, &info)) result = 13;
                else if (info.flags & 3U) result = 120; /* Split cabinets require another source protocol. */
            }
            cabinet_name = name;
        }
        if (result) break;
        int found = -1;
        cab_file_info_t info = {0};
        for (int c = 0; !result && c < cab_file_count(cabinet); c++) {
            const char *member = cab_file_name(cabinet, c);
            if (!member) result = 13;
            else if (!strcmp(member, file->key)) {
                if (found >= 0) result = 13;
                else found = c;
            }
        }
        if (!result && (found < 0 || cab_file_info(cabinet, found, &info) || info.size != file->size)) result = 13;
        if (result) break;
        msi_payload_file_t *output = &payload->files[payload->count++];
        output->bytes = cab_extract_index(cabinet, found, &output->size);
        if (!output->bytes || output->size != file->size) result = 13;
        output->date = info.date; output->time = info.time; output->attributes = info.attributes;
    }
    if (cabinet) cab_close(cabinet);
    if (cabinet_data) kfree(cabinet_data);
    if (error) *error = result;
    if (result) { msi_payload_close(payload); return NULL; }
    return payload;
}
void msi_payload_close(msi_payload_t *payload)
{
    if (!payload) return;
    for (uint32_t i = 0; i < payload->count; i++) if (payload->files[i].bytes) kfree(payload->files[i].bytes);
    if (payload->files) kfree(payload->files);
    kfree(payload);
}

uint32_t msi_payload_stage_cache(msi_plan_t *plan, msi_payload_t *payload,
    const uint16_t *target, const uint8_t *data, uint32_t size)
{
    if (!plan || !payload || !target || !*target || !data || !size || payload->count != plan->file_count) return 87;
    if (plan->file_count >= 16384 || plan->payload_bytes + size > 256U * 1024 * 1024) return 8;
    uint32_t length = 0;
    while (length < 1048576 && target[length]) length++;
    if (length == 1048576) return 8;
    for (uint32_t i = 0; i < plan->file_count; i++)
        if (!strcmp(plan->files[i].key, "OsitoK.CachedPackage") ||
            CompareStringOrdinal(plan->files[i].target, -1, target, -1, 1) == 2) return 87;
    uint32_t count = plan->file_count;
    uint16_t *name = kmalloc(((uint64_t)length + 1) * 2);
    uint8_t *bytes = kmalloc(size);
    msi_plan_file_t *files = kmalloc(((uint64_t)count + 1) * sizeof(*files));
    msi_payload_file_t *staged = kmalloc(((uint64_t)count + 1) * sizeof(*staged));
    if (!name || !bytes || !files || !staged) {
        if (name) kfree(name);
        if (bytes) kfree(bytes);
        if (files) kfree(files);
        if (staged) kfree(staged);
        return 8;
    }
    memcpy(name, target, (length + 1) * 2U); memcpy(bytes, data, size);
    if (count) { memcpy(files, plan->files, count * sizeof(*files)); memcpy(staged, payload->files, count * sizeof(*staged)); }
    files[count] = (msi_plan_file_t){.key="OsitoK.CachedPackage", .target=name, .component=UINT32_MAX, .size=size};
    staged[count] = (msi_payload_file_t){.bytes=bytes, .size=size};
    if (plan->files) kfree(plan->files);
    if (payload->files) kfree(payload->files);
    plan->files = files; plan->file_count++; plan->payload_bytes += size;
    payload->files = staged; payload->count++;
    return 0;
}
