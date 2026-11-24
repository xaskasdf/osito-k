#ifndef OSITOK_MSI_APPLY_H
#define OSITOK_MSI_APPLY_H
#include "msi_payload.h"
#include "msi_registry.h"

typedef struct {
    uint8_t *data; /* kmalloc-owned; a zero-size existing value is still present. */
    uint32_t size, type;
    int key_exists, present;
} msi_registry_before_t;
typedef struct {
    uint32_t (*read_file)(void *, const uint16_t *, uint8_t **, uint32_t *, int *present);
    /* Must use create-new semantics. Set created even if a later write fails. */
    uint32_t (*create_file)(void *, const uint16_t *, const uint8_t *, uint32_t, int *created);
    uint32_t (*remove_file)(void *, const uint16_t *);
    uint32_t (*read_registry)(void *, const msi_registry_value_t *, msi_registry_before_t *);
    /* Set changed even if only creating the key succeeded. */
    uint32_t (*write_registry)(void *, const msi_registry_value_t *, int *changed);
    /* Remove only keys introduced by this transaction, and only if empty. */
    uint32_t (*restore_registry)(void *, const msi_registry_value_t *, const msi_registry_before_t *);
    uint32_t (*flush)(void *);
    /* Optional cooperative cancellation. Never polled during rollback. */
    int (*cancelled)(void *);
} msi_apply_backend_t;
typedef struct { uint32_t files_created, files_retained, values_written, rollback_error; } msi_apply_result_t;

/* Fresh installation only: existing identical files are retained; differing
 * files return 1638 before any write. Registry snapshots and file comparisons
 * finish before mutation. On failure restore changed registry entries and
 * remove only files created by this call, in reverse order. This journal is
 * in memory; power-loss recovery and upgrade/repair are not implemented here.
 * Product registration must be supplied as final registry operations, after
 * authored rows, so failed installation cannot report a registered product. */
uint32_t msi_apply_fresh(const msi_plan_t *plan, const msi_payload_t *payload,
    const msi_registry_t *registry, const msi_apply_backend_t *backend,
    void *context, msi_apply_result_t *result);
#endif
