/*
 * OsitoK VFS — Virtual File System Layer
 */

#include "vfs.h"
#include "ositofs3.h"

/* External declarations for v2 */
extern bool  osfs2_is_mounted(void);
extern void  osfs2_list(void);
extern void *osfs2_find(const char *name);
extern void *osfs2_find_ci(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern uint64_t osfs2_file_size(void *file);

/* ── Helpers ─────────────────────────────────────────────── */

static int vfs_strncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* ── Path Resolution ───────────────────────────────────────── */

static const char *resolve_posix(const char *path)
{
    if (!path || !*path) return NULL;
    if (vfs_strncmp(path, "/dev/null", 9) == 0)    return VFS_DEV_NULL;
    if (vfs_strncmp(path, "/dev/zero", 9) == 0)    return VFS_DEV_ZERO;
    if (vfs_strncmp(path, "/dev/urandom", 12) == 0) return VFS_DEV_URANDOM;
    if (vfs_strncmp(path, "/dev/random", 11) == 0)  return VFS_DEV_URANDOM;
    if (vfs_strncmp(path, "/proc/self/", 11) == 0)  return VFS_PROC_SELF;
    if (vfs_strncmp(path, "/proc/", 6) == 0)        return VFS_PROC_SELF;
    return path;
}

static const char *resolve_win32(const char *path)
{
    if (!path || !*path) return NULL;
    /* Strip NT prefix */
    if (path[0] == '\\' && path[1] == '?' && path[2] == '?' && path[3] == '\\') path += 4;
    /* Strip drive letter C:\ */
    if (path[0] && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) path += 3;
    return path;
}

const char *vfs_resolve(const char *path, int mode)
{
    if (!path) return NULL;
    switch (mode) {
    case VFS_MODE_POSIX: return resolve_posix(path);
    case VFS_MODE_WIN32: return resolve_win32(path);
    default: return path;
    }
}

/* ── Unified API ─────────────────────────────────────────── */

bool vfs_find(const char *path, int mode, vfs_node_t *out_node)
{
    if (!out_node) return false;
    memset(out_node, 0, sizeof(vfs_node_t));

    const char *rpath = vfs_resolve(path, mode);
    if (!rpath || VFS_IS_VIRTUAL(rpath)) return false;

    const char *lookup_path = rpath;
    if (lookup_path[0] == '/') lookup_path++;

    /* Try v3 first */
    if (osfs3_is_mounted()) {
        uint32_t ino = osfs3_resolve_path(rpath);
        if (ino) {
            out_node->fs_version = 3;
            out_node->ino = ino;
            out_node->size = osfs3_get_size(ino);
            return true;
        }
    }

    /* Fallback to v2 */
    if (osfs2_is_mounted()) {
        void *f = (mode == VFS_MODE_WIN32) ? osfs2_find_ci(lookup_path) : osfs2_find(lookup_path);
        if (f) {
            out_node->fs_version = 2;
            out_node->data = f;
            out_node->size = osfs2_file_size(f);
            return true;
        }
    }

    return false;
}

int vfs_read(vfs_node_t *node, uint64_t offset, void *buf, uint64_t len)
{
    if (!node) return -1;
    if (node->fs_version == 3) return osfs3_read(node->ino, offset, buf, len);
    if (node->fs_version == 2) return osfs2_read(node->data, offset, buf, len);
    return -1;
}

void vfs_list(const char *path)
{
    if (osfs3_is_mounted()) {
        uint32_t ino = osfs3_resolve_path(path);
        if (ino && osfs3_is_dir(ino)) {
            osfs3_list_dir(ino);
            return;
        }
    }

    if (osfs2_is_mounted()) {
        osfs2_list();
    }
}

