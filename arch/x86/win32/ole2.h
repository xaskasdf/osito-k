/*
 * OsitoK Win32 Layer — OLE2 / Compound File Binary Format reader
 *
 * Minimal read-only parser for the Microsoft Compound File (CFBF /
 * "structured storage") container used by MSI databases. Resolves the
 * FAT / mini-FAT, materializes the directory, and reads named streams
 * (including the <4096-byte ministream path). Format-agnostic: MSI
 * stream-name de-mangling lives in msi.c, not here.
 *
 * Reference: [MS-CFB] Compound File Binary File Format.
 */

#ifndef OSITOK_OLE2_H
#define OSITOK_OLE2_H

#include "../include/types.h"

typedef struct ole2_file ole2_file_t;   /* opaque */

/* Validate signature, build FAT/miniFAT/directory. Returns NULL on error.
 * The data buffer must outlive the returned handle (not copied). */
ole2_file_t *ole2_open(const uint8_t *data, uint32_t len);
void         ole2_close(ole2_file_t *f);

/* Number of directory entries (index 0 is the root storage). */
uint32_t ole2_count(ole2_file_t *f);

/* Enumerate directory entry by index. Fills name_out with the RAW UTF-16LE
 * name (null-terminated, may be mangled), size_out with the stream size, and
 * type_out (1=storage, 2=stream, 5=root). Returns 1 if present, 0 if past end. */
int ole2_enum(ole2_file_t *f, int index,
              uint16_t *name_out, int name_cap,
              uint32_t *size_out, int *type_out);

/* Read a stream by directory index into a kmalloc'd buffer (caller kfrees).
 * Returns NULL if index is not a stream or on error. */
uint8_t *ole2_read_index(ole2_file_t *f, int index, uint32_t *out_size);

/* Read a stream by ASCII name (matched against the raw UTF-16 name as
 * Latin-1, i.e. low byte per code unit). For non-mangled ASCII names. */
uint8_t *ole2_read_stream(ole2_file_t *f, const char *name, uint32_t *out_size);

#endif /* OSITOK_OLE2_H */
