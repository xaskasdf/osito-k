/* fcntl.h -- Osito-K AArch64 libc (files are read-only for now) */
#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_APPEND    02000
#define AT_FDCWD    (-100)

int open(const char *path, int flags, ...);

#endif
