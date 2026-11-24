/*
 * arch/x86/include/tensor_arena.h — superpage-backed tensor allocator
 *
 * Reserves a physically-contiguous, 2 MB-aligned region at boot. All
 * inference scratch buffers and KV cache allocate from this arena so
 * the TLB needs only a handful of entries to cover hundreds of MB of
 * tensor accesses per forward pass.
 *
 * Rationale: matvec over a 2048x2048 Q4_0 weight matrix touches ~1.2 MB
 * of weights, crossing ~300 4 KB pages (> 64-entry TLB). With 2 MB
 * pages that's 1 TLB entry. See plan Phase 2.
 */
#ifndef OSITOK_TENSOR_ARENA_H
#define OSITOK_TENSOR_ARENA_H

#include "types.h"
#include "stdint.h"

#define TENSOR_SUPERPAGE_SIZE (2ULL * 1024 * 1024)

typedef struct {
    uint64_t phys_base;       /* 2 MB-aligned physical address */
    void    *virt_base;       /* KERNEL_VBASE + phys_base */
    uint64_t total_size;      /* multiple of 2 MB */
    uint64_t used;            /* bump-pointer offset */
    uint32_t superpages;      /* total_size / 2 MB */
    uint32_t peak_used;       /* high-water mark for diagnostics */
} tensor_arena_t;

/* Initialize an arena reserving size_mb MB (rounded up to 2 MB).
 * Returns 0 on success, -1 if physical allocation fails. */
int  tensor_arena_init(tensor_arena_t *a, uint64_t size_mb);

/* Bump allocator. align must be power of 2, typically 64 (cache line). */
void *tensor_arena_alloc(tensor_arena_t *a, uint64_t size, uint64_t align);

/* Reset bump pointer without freeing physical memory. KV cache reset
 * after a conversation completes calls this. */
void tensor_arena_reset(tensor_arena_t *a);

/* Print usage to serial. */
void tensor_arena_stats(const tensor_arena_t *a, const char *name);

/* Global arena exposed for inference.c and sys_inference.c. */
extern tensor_arena_t g_tensor_arena;

#endif /* OSITOK_TENSOR_ARENA_H */
