#ifndef OSITOK_MSI_REGISTRY_H
#define OSITOK_MSI_REGISTRY_H
#include "msi_plan.h"

typedef struct {
    const char *key; /* Registry table row identifier, database owned. */
    uint16_t *path, *name;
    uint8_t *data;
    uint32_t root, view, type, size, component;
    int create_only, delete_on_uninstall;
} msi_registry_value_t;
typedef struct {
    msi_registry_value_t *values;
    uint32_t count;
} msi_registry_t;

/* Resolve selected Registry rows completely before modifying the machine.
 * Append/prepend REG_MULTI_SZ and unsupported Formatted syntax fail with 120.
 * A '-' key marker has no install operation; '+'/'*' create an empty key. */
msi_registry_t *msi_registry_prepare(msi_plan_t *plan, uint32_t *error);
void msi_registry_close(msi_registry_t *registry);
/* Transfer both lists into one journal order. On success both inputs become
 * NULL; failure leaves them untouched. */
msi_registry_t *msi_registry_merge(msi_registry_t **first, msi_registry_t **last, uint32_t *error);
#endif
