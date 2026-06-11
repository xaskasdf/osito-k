/*
 * OsitoK Win32 Layer — package installer dispatch + shared helpers
 *
 * Sniffs MSI (OLE2) vs MSIX (ZIP) and routes to the right engine, writes
 * extracted files into OsitoFS through the same VFS path mapping the running
 * app will use, and records an uninstall manifest.
 */

#ifndef OSITOK_INSTALLER_H
#define OSITOK_INSTALLER_H

#include "../include/types.h"

/* Sniff the package format in `data` and install it. pkg_name labels the
 * uninstall manifest. Returns 0 on success, negative on error. */
int installer_run_buffer(const uint8_t *data, uint32_t len, const char *pkg_name);

/* Read a package file from OsitoFS and install it. */
int installer_run_file(const char *filename);

/* Write a file to OsitoFS at a Windows-style path. The drive/NT prefix is
 * stripped via vfs_resolve(VFS_MODE_WIN32) so the stored name matches what
 * CreateFile resolves to. Overwrites any existing file. Returns 0 on success. */
int installer_write_file(const char *win_path, const uint8_t *data, uint32_t len);

/* Uninstall manifest: begin() resets, add() records an OsitoFS name, commit()
 * flushes the list to "<pkg>.osito-install" on OsitoFS. */
void installer_manifest_begin(const char *pkg_name);
void installer_manifest_add(const char *osfs_name);
void installer_manifest_commit(void);

/* Replay "<pkg>.osito-install": delete every listed file + the manifest.
 * Returns the number of files removed, or negative if no manifest exists. */
int installer_uninstall(const char *pkg);

#endif /* OSITOK_INSTALLER_H */
