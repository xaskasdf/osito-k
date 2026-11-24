/*
 * fs.h -- the mounted filesystem (OsitoFS v3) for the shell and programs
 *
 * v3's own paths (fs/ositofs3.h) always start at the root. These take paths
 * relative to the shell's current directory too, and resolve "." and "..".
 */
#ifndef OSITO_ARM_FS_H
#define OSITO_ARM_FS_H

#include "types.h"

#define FS_PATH_MAX 260     /* OSFS3_PATH_MAX */

/* `path` as an absolute "/a/b": 0, or -1 if it's too long */
int  fs_path(const char *path, char out[FS_PATH_MAX]);
const char *fs_cwd(void);
int  fs_chdir(const char *path);            /* 0, or -1: not a directory */
void *fs_find(const char *path);            /* a regular file, or NULL */
/* `path` replaced by `size` bytes of `data`, out of the device's cache too:
 * 0, or -1 */
int  fs_write_file(const char *path, const void *data, uint64_t size);

#endif
