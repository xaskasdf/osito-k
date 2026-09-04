/*
 * fd.h — File descriptor types shared between syscall.c and process.c.
 *
 * Per-process fd_table refactor: the fd_entry_t array lives inline in
 * process_t so each process has its own fds. Fork clones the table and
 * bumps pipe_buf_t refcounts for inherited pipe ends.
 */

#ifndef _OSITOK_FD_H
#define _OSITOK_FD_H

#include "types.h"
#include "../fs/vfs.h"   /* vfs_node_t */

#define MAX_FDS          128

#define FD_TYPE_CONSOLE  1
#define FD_TYPE_FILE     2
#define FD_TYPE_PIPE     3
#define FD_TYPE_DEV      4   /* /dev/null, /dev/zero, ... */
#define FD_TYPE_PROC     5   /* /proc/self/maps, ... */
#define FD_TYPE_DIR      6   /* synthetic OsitoFS v2 directory */
#define FD_TYPE_SOCKET   7   /* BSD socket backed by kernel/socket.c */

typedef ssize_t (*fd_write_fn)(const void *buf, size_t count);
typedef ssize_t (*fd_read_fn)(void *buf, size_t count);

typedef struct {
    ssize_t (*read)(void *context, void *buf, size_t count,
                    uint32_t oflags);
    ssize_t (*write)(void *context, const void *buf, size_t count,
                     uint32_t oflags);
    int64_t (*ioctl)(void *context, uint64_t request, uint64_t arg,
                     uint32_t *oflags);
    bool (*read_ready)(void *context);
    bool (*write_ready)(void *context);
    void (*retain)(void *context);
    void (*release)(void *context, uint32_t oflags);
    uint64_t rdev;
} fd_device_ops_t;

typedef struct {
    bool        open;
    uint8_t     type;       /* FD_TYPE_* */
    uint32_t    oflags;     /* Linux O_* flags */
    fd_read_fn  read;
    fd_write_fn write;
    vfs_node_t  node;       /* embedded VFS node for regular files */
    void       *pipe;       /* pipe_buf_t *, or NULL */
    int32_t     socket_idx; /* index in kernel socket table */
    uint64_t    offset;     /* file position */
    const fd_device_ops_t *device_ops;
    void       *device_data;
    char        dir_path[64]; /* normalized prefix for FD_TYPE_DIR */
} fd_entry_t;

static inline void fd_device_retain(fd_entry_t *entry)
{
    if (entry && entry->device_ops && entry->device_ops->retain &&
        entry->device_data)
        entry->device_ops->retain(entry->device_data);
}

static inline void fd_device_release(fd_entry_t *entry)
{
    if (entry && entry->device_ops && entry->device_ops->release &&
        entry->device_data)
        entry->device_ops->release(entry->device_data, entry->oflags);
    if (entry) {
        entry->device_ops = NULL;
        entry->device_data = NULL;
    }
}

/* Refcounted fd table: shared between threads (CLONE_FILES),
 * separate copies for fork. */
typedef struct {
    int         refcount;
    fd_entry_t  entries[MAX_FDS];
} fd_table_t;

/* ── Pipe buffer ─────────────────────────────────────────────── */

#define PIPE_BUF_SIZE    4096
#define MAX_PIPES        8

/* Refcounted pipe_buf_t: each fd_entry_t pointing at this pipe bumps
 * read_refs or write_refs (based on O_RDONLY vs O_WRONLY). Fork bumps
 * them again for the child's inherited fds. close() decrements. When
 * both refs reach zero, in_use is cleared and the slot is reusable. */
typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;      /* write position */
    uint32_t tail;      /* read position */
    uint32_t count;     /* bytes in buffer */
    int      read_refs; /* # of open read-side fds (0 = no readers) */
    int      write_refs;/* # of open write-side fds (0 = EOF for readers) */
    bool     in_use;
} pipe_buf_t;

#endif /* _OSITOK_FD_H */
