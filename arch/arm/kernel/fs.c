/*
 * fs.c -- paths on the mounted OsitoFS v3, relative to the shell's current
 * directory (include/fs.h)
 */
#include "../include/fs.h"
#include "../fs/ositofs3.h"

static char cwd[FS_PATH_MAX] = "/";

const char *fs_cwd(void)
{
    return cwd;
}

int fs_path(const char *path, char out[FS_PATH_MAX])
{
    char joined[2 * FS_PATH_MAX];
    uint32_t n = 0;
    if (path[0] != '/') {
        for (const char *c = cwd; *c; c++)
            joined[n++] = *c;
        joined[n++] = '/';
    }
    for (; *path; path++) {
        if (n >= sizeof(joined) - 1)
            return -1;
        joined[n++] = *path;
    }
    joined[n] = 0;

    /* One "/name" per component: "." dropped, ".." takes the last one off */
    uint32_t w = 0;
    const char *p = joined;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        uint32_t len = (uint32_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (w > 0 && out[w - 1] != '/')
                w--;
            if (w > 0)
                w--;
            continue;
        }
        if (w + 1 + len >= FS_PATH_MAX)
            return -1;
        out[w++] = '/';
        memcpy(out + w, s, len);
        w += len;
    }
    if (w == 0)
        out[w++] = '/';
    out[w] = 0;
    return 0;
}

int fs_chdir(const char *path)
{
    char abs[FS_PATH_MAX];
    if (fs_path(path, abs) < 0)
        return -1;
    uint32_t ino = osfs3_resolve_path(abs);
    if (!ino || !osfs3_is_dir(ino))
        return -1;
    strcpy(cwd, abs);
    return 0;
}

void *fs_find(const char *path)
{
    char abs[FS_PATH_MAX];
    return fs_path(path, abs) < 0 ? NULL : osfs3_find(abs);
}

int fs_write_file(const char *path, const void *data, uint64_t size)
{
    char abs[FS_PATH_MAX];
    if (fs_path(path, abs) < 0)
        return -1;
    if (osfs3_find(abs) && osfs3_delete(abs) < 0)
        return -1;
    void *f = osfs3_create(abs, size);
    if (!f || (size && osfs3_write(f, 0, data, size) < 0) || osfs3_sync() < 0)
        return -1;
    return 0;
}
