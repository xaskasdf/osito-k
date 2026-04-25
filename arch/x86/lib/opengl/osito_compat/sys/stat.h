/* OsitoK shim — sys/stat.h
 * Minimal stat surface for Mesa src/util/. We expose just enough types
 * for u_hash_table.c (which only references stat fields in debug paths
 * we never enable). All functions are no-ops returning -1. */
#ifndef OSITO_SYS_STAT_H
#define OSITO_SYS_STAT_H 1

#include <stddef.h>

typedef unsigned long mode_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef unsigned long uid_t;
typedef unsigned long gid_t;
typedef long          off_t;
typedef long          blksize_t;
typedef long          blkcnt_t;

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    long      st_atime;
    long      st_mtime;
    long      st_ctime;
};

/* Zero the buffer so callers that ignore the -1 return don't read
 * uninitialised fields (Mesa's u_screen.c equal_file_description does
 * this; with garbage st_dev/st_ino it would compare two stale fds equal
 * and return the wrong cached pipe_screen). */
extern void *memset(void *, int, unsigned long);
static inline int stat (const char *p, struct stat *s) { (void)p; if (s) memset(s, 0, sizeof(*s)); return -1; }
static inline int fstat(int fd,         struct stat *s) { (void)fd; if (s) memset(s, 0, sizeof(*s)); return -1; }
static inline int lstat(const char *p, struct stat *s) { (void)p; if (s) memset(s, 0, sizeof(*s)); return -1; }
static inline int mkdir(const char *p, mode_t m) { (void)p; (void)m; return -1; }

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)

#endif
