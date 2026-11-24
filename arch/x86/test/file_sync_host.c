/* POSIX storage boundary for the production sync engine and OpenSSH harness. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../kernel/file_sync.h"

static int input = -1, output = -1, had_previous;
static DIR *directory;
static struct stat input_stat, previous_stat, directory_stat;
static char target[512], stage[512];

static const char *relative(const char *path)
{
    while (*path == '/') path++;
    return *path ? path : ".";
}

static int equal_stat(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int host_stat(const char *path, unsigned *kind, unsigned long long *size)
{
    struct stat st;
    if (lstat(relative(path), &st)) return errno == ENOENT ? SYNC_MISSING : SYNC_IO;
    if (S_ISREG(st.st_mode)) *kind = 1;
    else if (S_ISDIR(st.st_mode)) *kind = 2;
    else return SYNC_INVALID;
    *size = st.st_size;
    return SYNC_OK;
}

static int host_list(const char *path)
{
    if (directory) closedir(directory);
    directory = opendir(relative(path));
    if (!directory || fstat(dirfd(directory), &directory_stat)) return SYNC_IO;
    return SYNC_OK;
}

static int host_next(char *name, unsigned cap, unsigned *kind)
{
    struct stat st;
    if (!directory || fstat(dirfd(directory), &st)) return SYNC_IO;
    if (!equal_stat(&st, &directory_stat)) return SYNC_CHANGED;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strlen(entry->d_name) >= cap ||
            fstatat(dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) return SYNC_IO;
        if (S_ISREG(st.st_mode)) *kind = 1;
        else if (S_ISDIR(st.st_mode)) *kind = 2;
        else return SYNC_INVALID;
        strcpy(name, entry->d_name);
        return SYNC_OK;
    }
    closedir(directory); directory = NULL;
    return SYNC_END;
}

static int host_open(const char *path, unsigned long long *size)
{
    input = open(relative(path), O_RDONLY | O_NOFOLLOW);
    if (input < 0) return errno == ENOENT ? SYNC_MISSING : SYNC_IO;
    if (fstat(input, &input_stat) || !S_ISREG(input_stat.st_mode)) {
        close(input); input = -1; return SYNC_INVALID;
    }
    *size = input_stat.st_size;
    return SYNC_OK;
}

static int host_read(unsigned long long offset, void *p, unsigned n)
{
    struct stat st;
    if (input < 0 || fstat(input, &st)) return SYNC_IO;
    if (!equal_stat(&st, &input_stat)) return SYNC_CHANGED;
    if (pread(input, p, n, (off_t)offset) != n) return SYNC_IO;
    if (fstat(input, &st) || !equal_stat(&st, &input_stat)) return SYNC_CHANGED;
    return SYNC_OK;
}

static void host_close(void)
{
    if (input >= 0) close(input);
    input = -1;
}

static void host_abort(void)
{
    if (output >= 0) close(output);
    output = -1;
    if (*stage) unlink(stage);
    *stage = *target = 0;
}

static int host_begin(const char *path, unsigned long long size)
{
    (void)size;
    strcpy(target, relative(path));
    had_previous = lstat(target, &previous_stat) == 0;
    if (had_previous && !S_ISREG(previous_stat.st_mode)) return SYNC_INVALID;
    if (!had_previous && errno != ENOENT) return SYNC_IO;
    char *slash = strrchr(target, '/');
    unsigned prefix = slash ? (unsigned)(slash - target + 1) : 0;
    memcpy(stage, target, prefix);
    strcpy(stage + prefix, ".osync-XXXXXX");
    output = mkstemp(stage);
    if (output < 0) { *stage = 0; return SYNC_IO; }
    return SYNC_OK;
}

static int host_write(unsigned long long offset, const void *p, unsigned n)
{
    if (strstr(target, "fail-write")) return SYNC_IO;
    return pwrite(output, p, n, (off_t)offset) == n ? SYNC_OK : SYNC_IO;
}

static int host_readback(unsigned long long offset, void *p, unsigned n)
{
    if (pread(output, p, n, (off_t)offset) != n) return SYNC_IO;
    if (n && strstr(target, "corrupt-write")) ((unsigned char *)p)[0] ^= 1;
    return SYNC_OK;
}

static int host_commit(void)
{
    struct stat now;
    int exists = lstat(target, &now) == 0;
    if (exists != had_previous || (exists && !equal_stat(&now, &previous_stat)))
        return SYNC_CHANGED;
    if (strstr(target, "fail-flush") || fsync(output)) return SYNC_IO;
    if (had_previous && fchmod(output, previous_stat.st_mode & 0777)) return SYNC_IO;
    if (rename(stage, target)) return SYNC_IO;
    close(output); output = -1; *stage = *target = 0;
    return SYNC_OK;
}

static int host_mkdir(const char *path)
{
    if (!mkdir(relative(path), 0755)) return SYNC_OK;
    unsigned kind; unsigned long long size;
    return !host_stat(path, &kind, &size) && kind == 2 ? SYNC_OK : SYNC_IO;
}

static const file_sync_ops_t host_ops = {
    host_stat, host_list, host_next, host_open, host_read, host_close,
    host_begin, host_write, host_readback, host_commit, host_abort, host_mkdir
};

void file_sync_host_init(const char *root)
{
    if (!root || chdir(root)) { perror("sync root"); exit(2); }
    file_sync_init(&host_ops);
}

#ifdef FILE_SYNC_STDIO
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    file_sync_host_init(argv[1]);
    unsigned char in[FILE_SYNC_FRAME], out[FILE_SYNC_FRAME], header[4];
    int finished = 0, code = 0;
    while (!finished) {
        if (fread(header, 1, 4, stdin) != 4) break;
        unsigned n = (unsigned)header[0] << 24 | (unsigned)header[1] << 16 |
                     (unsigned)header[2] << 8 | header[3];
        if (!n || n > sizeof(in) || fread(in, 1, n, stdin) != n) { code = 2; break; }
        int result = file_sync_request(NULL, in, n, out, sizeof(out), &finished);
        if (result < 1) { code = 3; break; }
        header[0] = (unsigned)result >> 24; header[1] = (unsigned)result >> 16;
        header[2] = (unsigned)result >> 8; header[3] = result;
        if (fwrite(header, 1, 4, stdout) != 4 ||
            fwrite(out, 1, result, stdout) != (unsigned)result || fflush(stdout)) { code = 4; break; }
    }
    file_sync_reset(NULL);
    if (directory) closedir(directory);
    return code;
}
#endif
