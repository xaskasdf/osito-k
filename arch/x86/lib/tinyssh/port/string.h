#ifndef OSITO_TSSH_STRING_H
#define OSITO_TSSH_STRING_H
#ifdef __KERNEL_X86__
#include "types.h"
#else
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#endif
#endif
