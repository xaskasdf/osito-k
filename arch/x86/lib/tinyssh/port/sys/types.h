#ifndef OSITO_TSSH_SYS_TYPES_H
#define OSITO_TSSH_SYS_TYPES_H
typedef unsigned int uid_t;
#ifdef __KERNEL_X86__
#include "../../../../include/types.h"
#else
#include <stddef.h>
typedef long ssize_t;
#endif
#endif
