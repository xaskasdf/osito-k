/*
 * OsitoK VFS — Path Resolution Layer
 *
 * Translates paths from different binary conventions to OsitoFS flat namespace.
 * Each binary type has its own path semantics:
 *   - POSIX:  /usr/lib/libc.so  → "libc.so"
 *   - WIN32:  C:\System\Core.u  → "Core.u"
 *   - NATIVE: Core.u            → "Core.u" (passthrough)
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>

/* Path resolution modes — one per binary convention */
#define VFS_MODE_NATIVE  0   /* OsitoFS direct (flat filenames) */
#define VFS_MODE_POSIX   1   /* Linux ELF: /path/to/file */
#define VFS_MODE_WIN32   2   /* Windows PE: C:\path\to\file */

/* Special return values — caller must handle these */
#define VFS_DEV_NULL     ((const char *)1)
#define VFS_DEV_ZERO     ((const char *)2)
#define VFS_DEV_URANDOM  ((const char *)3)
#define VFS_PROC_SELF    ((const char *)4)
#define VFS_IS_VIRTUAL(p) ((uintptr_t)(p) < 16)

/*
 * vfs_resolve — translate a path to an OsitoFS-compatible basename.
 *
 * @path: Input path (POSIX, Win32, or native depending on mode)
 * @mode: VFS_MODE_POSIX, VFS_MODE_WIN32, or VFS_MODE_NATIVE
 *
 * Returns:
 *   - Pointer to basename suitable for osfs2_find() / osfs2_find_ci()
 *   - VFS_DEV_NULL/ZERO/URANDOM/PROC_SELF for virtual paths (POSIX only)
 *   - NULL if path is invalid
 *
 * The returned pointer may point into the original path string (no copy).
 * For Win32 mode, caller should use osfs2_find_ci() (case-insensitive).
 * For POSIX/native mode, caller should use osfs2_find() (case-sensitive).
 */
const char *vfs_resolve(const char *path, int mode);

/*
 * vfs_resolve_w — wide string variant for Win32.
 * Converts wide path to narrow and resolves. Writes result to buf.
 *
 * @wpath: Wide (UTF-16) input path
 * @buf:   Output buffer for narrow basename (at least 260 bytes)
 * @bufsz: Size of buf
 *
 * Returns pointer to basename within buf, or NULL.
 */
const char *vfs_resolve_w(const uint16_t *wpath, char *buf, int bufsz);

#endif /* VFS_H */
