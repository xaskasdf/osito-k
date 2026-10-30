#ifndef OSITOK_MSI_WIN32_IO_H
#define OSITOK_MSI_WIN32_IO_H
#include "msi_apply.h"

/* Trusted installer-internal Win32 adapters. Paths are concrete target paths,
 * not VFS basenames, and registry views come from Component.Attributes. */
extern const msi_apply_backend_t msi_win32_backend;
uint32_t msi_win32_read_file(void *context, const uint16_t *path,
    uint8_t **data, uint32_t *size, int *present);
#endif
