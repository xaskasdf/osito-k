#ifndef OSITOK_MSI_REGISTRATION_H
#define OSITOK_MSI_REGISTRATION_H
#include "msi_summary.h"
#include "msi_registry.h"

/* Private versioned store for MSI APIs, shared by PE32/PE64. Authored Registry
 * rows continue to use their own component-specific registry views. */
#define MSI_PRODUCT_STORE ((const uint16_t *)u"Software\\OsitoK\\Installer\\Products\\")

/* Prepare per-machine fresh-product metadata. No registry writes occur here.
 * Append these operations after authored registry values in the same journal.
 * The State marker is last. Source names the package's source directory and
 * cache names its real cached MSI. Preparing registration alone does not
 * establish a successful installation. */
msi_registry_t *msi_registration_prepare(msi_plan_t *plan, const msi_summary_t *summary,
    const msi_registry_t *authored, const uint16_t *source, const uint16_t *cache, uint32_t *error);
int msi_registration_guid(const uint16_t *text);
#endif
