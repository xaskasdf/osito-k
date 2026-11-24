/*
 * OsitoK Win32 Layer — ZIP / OPC reader (for MSIX packages)
 *
 * Read-only. Parses the End-Of-Central-Directory record, walks the central
 * directory, and extracts members (stored or DEFLATE via zlib_inflate_raw).
 */

#ifndef OSITOK_ZIP_H
#define OSITOK_ZIP_H

#include "../include/types.h"

typedef struct zip_archive zip_archive_t;   /* opaque */

zip_archive_t *zip_open(const uint8_t *data, uint32_t len);
void           zip_close(zip_archive_t *z);

int  zip_entry_count(zip_archive_t *z);
/* Copy entry #idx's name into name_out, fill uncompressed size. Returns 1/0. */
int  zip_enum(zip_archive_t *z, int idx, char *name_out, int cap, uint32_t *usize);

/* Extract a member by exact name into a kmalloc'd buffer (caller kfrees). */
uint8_t *zip_extract(zip_archive_t *z, const char *name, uint32_t *out_size);

#endif /* OSITOK_ZIP_H */
