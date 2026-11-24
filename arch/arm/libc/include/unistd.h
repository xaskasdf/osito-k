/* unistd.h -- Osito-K AArch64 libc */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
void *sbrk(intptr_t increment);
int brk(void *addr);
pid_t getpid(void);
unsigned sleep(unsigned seconds);
int usleep(useconds_t usec);
void _exit(int status) __attribute__((noreturn));

/* The raw Linux AArch64 syscall: returns -errno on failure */
long syscall(long nr, ...);

#endif
