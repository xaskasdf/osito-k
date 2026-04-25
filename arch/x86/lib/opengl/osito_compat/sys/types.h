/* OsitoK shim — sys/types.h
 * Minimum POSIX type aliases referenced by Mesa src/util/. */
#ifndef OSITO_SYS_TYPES_H
#define OSITO_SYS_TYPES_H 1

#include <stddef.h>
#include <stdint.h>

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
