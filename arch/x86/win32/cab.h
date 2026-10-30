/*
 * OsitoK Win32 Layer — Microsoft Cabinet (.cab) reader
 *
 * Read-only. Supports stored, MSZIP and LZX folders. Quantum and cabinet
 * continuation are unsupported. MSZIP reuses zlib_inflate_raw; both compressed
 * formats preserve folder history across CFDATA blocks.
 *
 * Reference: [MS-CAB] Cabinet File Format.
 */

#ifndef OSITOK_CAB_H
#define OSITOK_CAB_H

#include "../include/types.h"

typedef struct cab_archive cab_archive_t;   /* opaque */
typedef struct {
    const char *name;
    uint32_t size, offset;
    uint16_t folder, date, time, attributes, compression;
} cab_file_info_t;
typedef struct {
    uint16_t flags, set_id, number;
    const char *next_cabinet, *next_disk;
} cab_info_t;

/* Validate the MSCF header and locate folder/file tables. NULL on error.
 * The data buffer must outlive the handle (not copied). */
cab_archive_t *cab_open(const uint8_t *data, uint32_t len);
void           cab_close(cab_archive_t *c);

int            cab_file_count(cab_archive_t *c);
/* Name of file #idx (pointer into the cab buffer; valid until cab_close). */
const char    *cab_file_name(cab_archive_t *c, int idx);
int            cab_file_info(cab_archive_t *c, int idx, cab_file_info_t *info);
int            cab_get_info(cab_archive_t *c, cab_info_t *info);
uint8_t       *cab_extract_index(cab_archive_t *c, int idx, uint32_t *out_size);

/* Extract a member by name (case-insensitive) into a kmalloc'd buffer
 * (caller kfrees). Returns NULL if not found or on error. */
uint8_t       *cab_extract(cab_archive_t *c, const char *name, uint32_t *out_size);

#endif /* OSITOK_CAB_H */
