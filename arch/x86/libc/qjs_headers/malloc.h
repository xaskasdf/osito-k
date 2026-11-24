/* OsitoK shim — malloc.h */
#ifndef _MALLOC_H
#define _MALLOC_H
#include <stddef.h>
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
size_t malloc_usable_size(const void *ptr);
#endif
