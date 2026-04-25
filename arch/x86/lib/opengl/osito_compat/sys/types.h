/* OsitoK shim — sys/types.h
 * Minimum POSIX type aliases referenced by Mesa src/util/.
 *
 * In C++ mode (W4.2+) we delegate to the hosted gcc:12 glibc
 * <sys/types.h> via #include_next so libstdc++ + glibc agree on
 * uid_t/gid_t/pthread_attr_t. Without this, signal.h's
 *   typedef union pthread_attr_t pthread_attr_t;
 * collides with our `typedef int pthread_attr_t` from mesa_compat.h. */
#ifndef OSITO_SYS_TYPES_H
#define OSITO_SYS_TYPES_H 1

#ifdef __cplusplus
#  include_next <sys/types.h>
#else
#  include <stddef.h>
#  include <stdint.h>

typedef long          ssize_t;
typedef long          off_t;
typedef long          time_t;
typedef int           pid_t;
typedef unsigned long mode_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef unsigned long uid_t;
typedef unsigned long gid_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
typedef unsigned long size_t_alias;
#endif

#endif
