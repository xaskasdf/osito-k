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
typedef struct msi_table_view msi_table_view_t;

typedef enum { MSI_VALUE_NULL, MSI_VALUE_INTEGER, MSI_VALUE_STRING, MSI_VALUE_STREAM } msi_value_kind_t;
typedef struct {
    msi_value_kind_t kind;
    int32_t integer;
    const char *string;  /* Database codepage bytes, borrowed until msi_close. */
} msi_value_t;

/* Parse the database (string pool + _Columns metadata). NULL on error.
 * The data buffer must outlive the handle (not copied). */
msi_db_t *msi_open(const uint8_t *data, uint32_t len);
void      msi_close(msi_db_t *db);

/* Resolve a string-pool id to a null-terminated string ("" if out of range). */
const char *msi_string(msi_db_t *db, uint32_t id);

/* Read-only table access. Views must be closed before their database.
 * Empty declared tables are valid; system tables are not synthesized here. */
uint32_t msi_codepage(msi_db_t *db);
uint32_t msi_table_count(msi_db_t *db);
const char *msi_table_name(msi_db_t *db, uint32_t index);
msi_table_view_t *msi_table_open(msi_db_t *db, const char *name);
void msi_table_close(msi_table_view_t *view);
uint32_t msi_table_rows(msi_table_view_t *view);
uint32_t msi_table_columns(msi_table_view_t *view);
const char *msi_table_column_name(msi_table_view_t *view, uint32_t column);
uint16_t msi_table_column_type(msi_table_view_t *view, uint32_t column);
int msi_table_column(msi_table_view_t *view, const char *name);
/* Returns 1 on success, 0 for an invalid row/column/string reference. */
int msi_table_value(msi_table_view_t *view, uint32_t row, uint32_t column, msi_value_t *out);
/* Materialize a binary field's named stream; caller kfrees the result. */
uint8_t *msi_table_read_binary(msi_table_view_t *view, uint32_t row, uint32_t column, uint32_t *size);
/* Read an embedded stream by its decoded MSI name (for example a cabinet).
 * A present empty stream returns a freeable buffer and size zero. */
uint8_t *msi_read_stream(msi_db_t *db, const char *name, uint32_t *size);

/* Run the install: extract files to OsitoFS, apply registry rows.
 * pkg_name labels the uninstall manifest. Returns the number of files
 * written (>=0), or negative on a parse/payload/write error. Failed installs
 * do not apply registry rows or commit a success manifest. */
int msi_install(msi_db_t *db, const char *pkg_name);

#endif /* OSITOK_MSI_H */
