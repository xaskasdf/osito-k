/*
 * OsitoK WASM — Memory HAL
 *
 * Replaces memory.c (UEFI page allocator) and heap.c.
 * All allocation backed by libc malloc (provided by Emscripten).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Page allocator (replaces memory.c) ─────────────────────── */

static uint64_t wasm_total_mem  = 256ULL * 1024 * 1024;
static uint64_t wasm_used_pages = 0;

void mem_init(void *mmap, uint64_t mmap_size, uint64_t desc_size)
{
    (void)mmap; (void)mmap_size; (void)desc_size;
}

void mem_reserve_kernel(uint64_t phys_base, uint64_t size)
{
    (void)phys_base; (void)size;
}

void *mem_alloc_pages(uint64_t count)
{
    size_t bytes = (size_t)(count * 4096);
    void *p = malloc(bytes);
    if (p) { memset(p, 0, bytes); wasm_used_pages += count; }
    return p;
}

void mem_free_pages(void *addr, uint64_t count)
{
    if (addr) { wasm_used_pages -= count; free(addr); }
}

uint64_t mem_get_total(void) { return wasm_total_mem; }
uint64_t mem_get_used(void)  { return wasm_used_pages * 4096; }

/* Aligned allocation — used by gguf.c for tensor table and tok arrays */
void *mem_alloc_aligned(uint64_t size, uint64_t align)
{
    void *p = NULL;
    if (posix_memalign(&p, (size_t)(align < sizeof(void*) ? sizeof(void*) : align),
                       (size_t)size) != 0)
        return NULL;
    if (p) memset(p, 0, (size_t)size);
    return p;
}

/* Exported to JS for status bar */
uint32_t wasm_get_mem_used_kb(void) { return (uint32_t)(wasm_used_pages * 4); }

/* ── Kernel heap (replaces heap.c) ──────────────────────────── */

void heap_init(void) {}

void *kmalloc(uint64_t size)  { return malloc((size_t)size); }
void  kfree(void *ptr)        { free(ptr); }

void *kcalloc(uint64_t count, uint64_t size)
{
    size_t total = (size_t)(count * size);
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *krealloc(void *ptr, uint64_t new_size)
{
    return realloc(ptr, (size_t)new_size);
}
