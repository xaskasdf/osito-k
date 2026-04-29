/*
 * OsitoK - Variable-size heap allocator
 *
 * First-fit allocator with forward coalescing.
 * 4 bytes overhead per allocation (block header).
 * Thread-safe: interrupts disabled during alloc/free.
 *
 * Usage:
 *   void *p = heap_alloc(100);  // allocate 100 bytes
 *   heap_free(p);               // release
 */
#ifndef OSITO_HEAP_H
#define OSITO_HEAP_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the heap */
void heap_init(void);

/* Allocate size bytes. Returns NULL if no space. */
void *heap_alloc(uint32_t size);

/* Free a previously allocated block */
void heap_free(void *ptr);

/* Total free bytes (sum of all free blocks) */
uint32_t heap_free_total(void);

/* Total allocated bytes (sum of all used blocks) */
uint32_t heap_used_total(void);

/* Largest single free block available */
uint32_t heap_largest_free(void);

/* Number of free fragments */
uint32_t heap_frag_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_HEAP_H */
