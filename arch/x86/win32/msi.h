/*
 * OsitoK Win32 Layer — Windows Installer (MSI) database reader + installer
 *
 * Cracks an MSI (an OLE2 compound file holding SQL-like tables), resolves
 * the install-directory tree, extracts payload files from the embedded
 * cabinet (or uncompressed streams), and applies Registry-table rows. A
 * native mini-msiexec — no real msiexec.exe / COM is involved.
 */

#ifndef OSITOK_MSI_H
#define OSITOK_MSI_H

#include "../include/types.h"

typedef struct msi_db msi_db_t;

/* Parse the database (string pool + _Columns metadata). NULL on error.
 * The data buffer must outlive the handle (not copied). */
msi_db_t *msi_open(const uint8_t *data, uint32_t len);
void      msi_close(msi_db_t *db);

/* Resolve a string-pool id to a null-terminated string ("" if out of range). */
const char *msi_string(msi_db_t *db, uint32_t id);

/* Run the install: extract files to OsitoFS, apply registry rows.
 * pkg_name labels the uninstall manifest. Returns the number of files
 * written (>=0), or negative on a fatal parse error. */
int msi_install(msi_db_t *db, const char *pkg_name);

#endif /* OSITOK_MSI_H */
