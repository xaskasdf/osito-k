#ifndef OSITO_TSSH_UNISTD_H
#define OSITO_TSSH_UNISTD_H
#include <sys/types.h>
ssize_t tssh_read(int fd, void *buf, size_t len);
ssize_t tssh_write(int fd, const void *buf, size_t len);
uid_t tssh_geteuid(void);
#define read tssh_read
#define write tssh_write
#define geteuid tssh_geteuid
#endif
