/* W4.8 — minimal fcntl.h shim for u_cpu_detect / os_file et al. */
#ifndef OSITO_FCNTL_H
#define OSITO_FCNTL_H 1
#ifdef __cplusplus
#include_next <fcntl.h>
#else

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_CLOEXEC   02000000
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC  1
#define AT_FDCWD    -100

extern int open(const char *path, int flags, ...);
extern int fcntl(int fd, int cmd, ...);
extern int close(int fd);
extern long read(int fd, void *buf, unsigned long count);
extern long write(int fd, const void *buf, unsigned long count);

#endif /* __cplusplus */
#endif
