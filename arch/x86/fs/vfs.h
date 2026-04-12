/*
 * OsitoK VFS — Virtual File System Layer
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>

/* Path resolution modes */
#define VFS_MODE_NATIVE  0
#define VFS_MODE_POSIX   1
#define VFS_MODE_WIN32   2

/* Special return values for vfs_resolve */
#define VFS_DEV_NULL     ((const char *)1)
#define VFS_DEV_ZERO     ((const char *)2)
#define VFS_DEV_URANDOM  ((const char *)3)
#define VFS_PROC_SELF    ((const char *)4)
#define VFS_IS_VIRTUAL(p) ((uintptr_t)(p) < 16)

/* Unified VFS Node */
typedef struct {
    int      fs_version; /* 2 or 3 */
    uint32_t ino;        /* Inode for v3, or index for v2 */
    void*    data;       /* Pointer to osfs2_file_t for v2 */
    uint64_t size;
} vfs_node_t;

/* Core VFS API */
const char *vfs_resolve(const char *path, int mode);

bool        vfs_find(const char *path, int mode, vfs_node_t *out_node);
int         vfs_read(vfs_node_t *node, uint64_t offset, void *buf, uint64_t len);
void        vfs_list(const char *path);

#endif /* VFS_H */
