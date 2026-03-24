/* Stub stdlib.h — satisfies mm_malloc.h include for bare-metal AVX2 builds */
#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
extern void *malloc(size_t size);
extern void free(void *ptr);
#endif
