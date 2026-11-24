/*
 * OsitoK Win32 Layer — MSIX / APPX installer
 *
 * MSIX is a ZIP (OPC) container. We read AppxManifest.xml (tolerant attribute
 * scan, no XML parser), extract the payload under an install directory, and
 * record an uninstall manifest.
 */

#ifndef OSITOK_MSIX_H
#define OSITOK_MSIX_H

#include "../include/types.h"

/* Install an MSIX package from an in-memory buffer. Returns 0 on success. */
int msix_install(const uint8_t *data, uint32_t len, const char *pkg_name);

#endif /* OSITOK_MSIX_H */
