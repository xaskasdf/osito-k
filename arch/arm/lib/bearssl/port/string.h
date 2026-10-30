/*
 * string.h for BearSSL in the kernel: the freestanding toolchain has none.
 * memcpy, memmove and memset are kernel/mem.S's; memcmp and strlen are
 * port/string.c's.
 */
#ifndef OSITO_BEARSSL_STRING_H
#define OSITO_BEARSSL_STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
