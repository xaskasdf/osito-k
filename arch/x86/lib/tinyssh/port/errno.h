#ifndef OSITO_TSSH_ERRNO_H
#define OSITO_TSSH_ERRNO_H
extern int tssh_errno;
#define errno tssh_errno
#endif
