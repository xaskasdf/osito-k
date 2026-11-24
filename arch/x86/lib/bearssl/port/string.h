#ifndef OSITO_BEARSSL_STRING_H
#define OSITO_BEARSSL_STRING_H
#include "../../../include/types.h"
/* types.h has no memmove; kernel/memory.c's (the X.509 engine and i31 use it) */
void *memmove(void *dst, const void *src, size_t n);
#endif
