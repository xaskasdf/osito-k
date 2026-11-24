/*
 * OsitoK - Fixed-size block pool allocator header
 */
#ifndef OSITO_POOL_ALLOC_H
#define OSITO_POOL_ALLOC_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the memory pool */
void pool_init(void);

/* Allocate one block (POOL_BLOCK_SIZE bytes). Returns NULL if pool exhausted. */
void *pool_alloc(void);

/* Free a previously allocated block */
void pool_free(void *ptr);

/* Get number of free blocks */
uint32_t pool_free_count(void);

/* Get number of used blocks */
uint32_t pool_used_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_POOL_ALLOC_H */
