#ifndef OSITOK_MSI_PLAN_H
#define OSITOK_MSI_PLAN_H
#include "msi_package.h"

typedef struct {
    const char *key; /* Database-owned text. */
    uint16_t *name;
    int installed, action;
    uint32_t attributes;
} msi_plan_object_t;
typedef struct {
    const char *key, *cabinet, *version, *language;
    uint16_t *target;
    uint32_t component, size, sequence, attributes;
} msi_plan_file_t;
typedef struct {
    uint32_t row, component;
} msi_plan_registry_t;
typedef struct msi_plan {
    msi_db_t *database;
    msi_package_t *package;
    msi_plan_object_t *features, *components;
    msi_plan_file_t *files;
    msi_plan_registry_t *registry;
    uint32_t feature_count, component_count, file_count, registry_count;
    uint64_t payload_bytes;
    msi_condition_lookup_t environment;
    void *environment_context;
    int owns_symbols;
} msi_plan_t;

/* Cost the fresh, local-install selection, without consulting/writing the
 * machine. Existing-file versions and registered component ownership must be
 * resolved by the execution layer before using this as an installation plan.
 * Unsupported selection/source semantics fail explicitly (error 120).
 * Keep the package and database alive until msi_plan_close. */
msi_plan_t *msi_plan_create_fresh(msi_db_t *database, msi_package_t *package,
    msi_condition_lookup_t environment, void *context, uint32_t *error);
void msi_plan_close(msi_plan_t *plan);
#endif
