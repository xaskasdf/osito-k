#ifndef OSITOK_MSI_ENGINE_H
#define OSITOK_MSI_ENGINE_H
#include "msi.h"

/* Native installer entry, with trusted kernel-owned UTF-16 arguments. Fresh,
 * per-machine compressed Intel packages only. Unsupported active actions,
 * maintenance/upgrade modes and source formats fail before mutation.
 * This is not yet a guest MSI API: guest pointer validation, UI callbacks and
 * process-cancellation ownership belong to the Win32 adapter. */
uint32_t msi_engine_install_fresh(const uint16_t *path, const uint16_t *properties);
/* The trusted controller must defer nonlocal process/thread exit until this
 * returns. Cancellation is polled between forward operations; rollback runs
 * to completion without polling. The callback never executes guest code. */
uint32_t msi_engine_install_controlled(const uint16_t *path, const uint16_t *properties,
    int (*cancelled)(void *), void *context);
#endif
