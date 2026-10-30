#ifndef OSITOK_MSI_FORMAT_H
#define OSITOK_MSI_FORMAT_H
#include "msi_package.h"

/* Property-only Formatted fields for the installation engine. Supports nested
 * property names, escaped characters, embedded [~] NULs and unmatched brackets.
 * Record fields, environment/file/component references and conditional brace
 * groups return ERROR_CALL_NOT_IMPLEMENTED; they must never be silently lost.
 * The result is kmalloc-owned, terminated, and length excludes that terminator
 * but includes any embedded NULs. This is not the MsiFormatRecord API. */
uint32_t msi_format_properties(msi_package_t *package, const uint16_t *input,
                               uint16_t **output, uint32_t *length);
#endif
