/* OsitoK shim — unistd.h */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
typedef long ssize_t;
typedef int pid_t;
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
long lseek(int fd, long offset, int whence);
int access(const char *path, int mode);
char *getcwd(char *buf, size_t size);
long sysconf(int name);
pid_t getpid(void);
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define R_OK 4
#define W_OK 2
#define F_OK 0
#endif
