#ifndef OSITOK_MSI_PACKAGE_H
#define OSITOK_MSI_PACKAGE_H
#include "msi.h"
#include "msi_condition.h"

typedef struct msi_package msi_package_t;

/* Read-only database session. The caller owns the database and its bytes.
 * Platform properties (SystemFolder, VersionNT, ROOTDRIVE, etc.) are supplied
 * by the installer, after loading the authored Property table. No files or
 * registry values are written by this module. */
msi_package_t *msi_package_create(msi_db_t *database, uint32_t *error);
void msi_package_close(msi_package_t *package);
uint32_t msi_package_set(msi_package_t *package, const uint16_t *name, const uint16_t *value);
const uint16_t *msi_package_get(msi_package_t *package, const uint16_t *name);
/* Optional provider for environment, feature and component access prefixes. */
void msi_package_symbols(msi_package_t *package, msi_condition_lookup_t lookup, void *context);
int msi_package_condition(msi_package_t *package, const uint16_t *expression);
/* Borrowed target path, including its final backslash. Property changes
 * invalidate resolved directories and previous borrowed target paths. */
uint32_t msi_package_directory(msi_package_t *package, const uint16_t *name, const uint16_t **path);
/* Decode one database string using its recorded codepage; caller kfrees. */
uint16_t *msi_package_decode(msi_package_t *package, const char *text);
#endif
