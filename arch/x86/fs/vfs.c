/*
 * OsitoK VFS — Path Resolution Layer
 *
 * Single point of path-to-OsitoFS translation for all binary types.
 * Replaces 6+ duplicate basename extraction loops across win32/ and kernel/.
 */

#include "vfs.h"

/* ── Helpers ─────────────────────────────────────────────── */

static int vfs_strncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* Extract basename: last component after any / or \ separator */
static const char *extract_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

/* ── POSIX path resolution ───────────────────────────────── */

static const char *resolve_posix(const char *path)
{
    if (!path || !*path) return NULL;

    /* Virtual device paths */
    if (vfs_strncmp(path, "/dev/null", 9) == 0)    return VFS_DEV_NULL;
    if (vfs_strncmp(path, "/dev/zero", 9) == 0)    return VFS_DEV_ZERO;
    if (vfs_strncmp(path, "/dev/urandom", 12) == 0) return VFS_DEV_URANDOM;
    if (vfs_strncmp(path, "/dev/random", 11) == 0)  return VFS_DEV_URANDOM;
    if (vfs_strncmp(path, "/proc/self/", 11) == 0)  return VFS_PROC_SELF;
    if (vfs_strncmp(path, "/proc/", 6) == 0)        return VFS_PROC_SELF;

    /* OsitoFS v2 supports subdirectories (e.g. "baseq2/pak0.pak").
     * Strip leading "/" or "./" to get the relative OsitoFS path. */
    if (path[0] == '/')
        return path + 1;           /* "/baseq2/pak0.pak" → "baseq2/pak0.pak" */
    if (path[0] == '.' && path[1] == '/')
        return path + 2;           /* "./baseq2/pak0.pak" → "baseq2/pak0.pak" */

    return path;                   /* already relative: "baseq2/pak0.pak" */
}

/* ── Win32 path resolution ───────────────────────────────── */

static const char *resolve_win32(const char *path)
{
    if (!path || !*path) return NULL;

    /* Strip NT object path prefix: \??\, \\?\, \Device\ */
    if (path[0] == '\\') {
        if (path[1] == '?' && path[2] == '?' && path[3] == '\\')
            path += 4;
        else if (path[1] == '\\' && path[2] == '?' && path[3] == '\\')
            path += 4;
    }

    /* Strip drive letter: C:\ or D:\ etc. */
    if (path[0] && path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
        path += 3;
    else if (path[0] && path[1] == ':' && path[2] == '\0')
        return "";  /* bare "C:" = root directory */

    /* Extract basename — OsitoFS is flat, no directory hierarchy */
    return extract_basename(path);
}

/* ── Public API ──────────────────────────────────────────── */

const char *vfs_resolve(const char *path, int mode)
{
    if (!path) return NULL;

    switch (mode) {
    case VFS_MODE_POSIX:
        return resolve_posix(path);
    case VFS_MODE_WIN32:
        return resolve_win32(path);
    case VFS_MODE_NATIVE:
    default:
        /* Native mode: passthrough (OsitoFS flat names) */
        return path[0] ? path : NULL;
    }
}

const char *vfs_resolve_w(const uint16_t *wpath, char *buf, int bufsz)
{
    if (!wpath || !buf || bufsz < 2) return NULL;

    /* Convert wide to narrow (ASCII subset) */
    int i = 0;
    for (; wpath[i] && i < bufsz - 1; i++)
        buf[i] = (char)(wpath[i] & 0xFF);
    buf[i] = '\0';

    /* Resolve as Win32 path */
    return vfs_resolve(buf, VFS_MODE_WIN32);
}
