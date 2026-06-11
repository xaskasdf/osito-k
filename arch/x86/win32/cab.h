/*
 * OsitoK Win32 Layer — Microsoft Cabinet (.cab) reader
 *
 * Read-only. Supports stored (uncompressed) and MSZIP folders — the two
 * compressions Windows Installer emits for embedded cabinets. LZX/Quantum
 * are detected and rejected cleanly. Decompression reuses zlib_inflate_raw
 * with a per-folder window so MSZIP cross-block back-references resolve.
 *
 * Reference: [MS-CAB] Cabinet File Format.
 */

#ifndef OSITOK_CAB_H
#define OSITOK_CAB_H

#include "../include/types.h"

typedef struct cab_archive cab_archive_t;   /* opaque */

/* Validate the MSCF header and locate folder/file tables. NULL on error.
 * The data buffer must outlive the handle (not copied). */
cab_archive_t *cab_open(const uint8_t *data, uint32_t len);
void           cab_close(cab_archive_t *c);

int            cab_file_count(cab_archive_t *c);
/* Name of file #idx (pointer into the cab buffer; valid until cab_close). */
const char    *cab_file_name(cab_archive_t *c, int idx);

/* Extract a member by name (case-insensitive) into a kmalloc'd buffer
 * (caller kfrees). Returns NULL if not found or on error. */
uint8_t       *cab_extract(cab_archive_t *c, const char *name, uint32_t *out_size);

#endif /* OSITOK_CAB_H */
