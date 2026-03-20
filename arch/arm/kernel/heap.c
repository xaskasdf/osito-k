/*
 * heap.c -- Kernel heap allocator (port of x86 version)
 * First-fit free list with block coalescing.
 */
#include "../include/hal.h"
#include "../include/types.h"

#define PAGE_SIZE       4096
#define HEAP_INIT_PAGES 64      /* 256 KB initial */
#define HEAP_GROW_PAGES 16      /* 64 KB growth */
#define MIN_ALLOC       16
#define ALIGNMENT       16
#define BLOCK_MAGIC     0x4F53  /* "OS" */
#define BLOCK_FREE      0
#define BLOCK_USED      1

typedef struct block_hdr {
    uint16_t            magic;
    uint16_t            flags;
    uint32_t            _pad;
    uint64_t            size;
    struct block_hdr   *next;
    struct block_hdr   *prev;
} block_hdr_t;

_Static_assert(sizeof(block_hdr_t) == 32, "block header must be 32 bytes");
_Static_assert(sizeof(block_hdr_t) % ALIGNMENT == 0, "header must be aligned");

static block_hdr_t *free_list;
static uint8_t     *heap_start;
static uint8_t     *heap_end;
static uint64_t     heap_size;
static uint64_t     heap_used;
static uint64_t     alloc_count;

static inline uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}

static void free_list_insert(block_hdr_t *block) {
    block->flags = BLOCK_FREE;
    block_hdr_t *prev = (void *)0, *curr = free_list;
    while (curr && curr < block) { prev = curr; curr = curr->next; }
    block->next = curr;
    block->prev = prev;
    if (prev) prev->next = block; else free_list = block;
    if (curr) curr->prev = block;
}

static void free_list_remove(block_hdr_t *block) {
    if (block->prev) block->prev->next = block->next;
    else free_list = block->next;
    if (block->next) block->next->prev = block;
    block->next = (void *)0;
    block->prev = (void *)0;
}

static void coalesce(block_hdr_t *block) {
    if (block->next) {
        uint8_t *end = (uint8_t *)block + sizeof(block_hdr_t) + block->size;
        if (end == (uint8_t *)block->next) {
            block_hdr_t *next = block->next;
            block->size += sizeof(block_hdr_t) + next->size;
            block->next = next->next;
            if (next->next) next->next->prev = block;
        }
    }
    if (block->prev) {
        uint8_t *pend = (uint8_t *)block->prev + sizeof(block_hdr_t) + block->prev->size;
        if (pend == (uint8_t *)block) {
            block_hdr_t *prev = block->prev;
            prev->size += sizeof(block_hdr_t) + block->size;
            prev->next = block->next;
            if (block->next) block->next->prev = prev;
        }
    }
}

static int heap_grow(uint64_t min_bytes) {
    uint64_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < HEAP_GROW_PAGES) pages = HEAP_GROW_PAGES;
    void *new_pages = mem_alloc_pages(pages);
    if (!new_pages) { serial_puts("[HEAP] Failed to grow\n"); return -1; }
    uint64_t new_size = pages * PAGE_SIZE;
    block_hdr_t *nb = (block_hdr_t *)new_pages;
    nb->magic = BLOCK_MAGIC;
    nb->flags = BLOCK_FREE;
    nb->size = new_size - sizeof(block_hdr_t);
    nb->next = (void *)0;
    nb->prev = (void *)0;
    free_list_insert(nb);
    coalesce(nb);
    heap_size += new_size;
    uint8_t *ne = (uint8_t *)new_pages + new_size;
    if (ne > heap_end) heap_end = ne;
    return 0;
}

static void block_split(block_hdr_t *block, uint64_t needed) {
    uint64_t remaining = block->size - needed;
    if (remaining < sizeof(block_hdr_t) + MIN_ALLOC) return;
    block_hdr_t *nb = (block_hdr_t *)((uint8_t *)block + sizeof(block_hdr_t) + needed);
    nb->magic = BLOCK_MAGIC;
    nb->flags = BLOCK_FREE;
    nb->size = remaining - sizeof(block_hdr_t);
    block->size = needed;
    nb->next = block->next;
    nb->prev = block->prev;
    if (block->next) block->next->prev = nb;
    if (block->prev) block->prev->next = nb;
    if (free_list == block) free_list = nb;
}

void *kmalloc(uint64_t size) {
    if (size == 0) return (void *)0;
    size = align_up(size, ALIGNMENT);
    if (size < MIN_ALLOC) size = MIN_ALLOC;
    block_hdr_t *block = free_list;
    while (block) {
        if (block->size >= size) {
            block_split(block, size);
            free_list_remove(block);
            block->flags = BLOCK_USED;
            heap_used += block->size;
            alloc_count++;
            return (void *)((uint8_t *)block + sizeof(block_hdr_t));
        }
        block = block->next;
    }
    if (heap_grow(sizeof(block_hdr_t) + size) < 0) return (void *)0;
    return kmalloc(size);
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - sizeof(block_hdr_t));
    if (block->magic != BLOCK_MAGIC) {
        serial_puts("[HEAP] CORRUPTION at ");
        serial_puthex((uint64_t)block, 16);
        serial_puts("\n");
        return;
    }
    if (block->flags != BLOCK_USED) {
        serial_puts("[HEAP] DOUBLE FREE at ");
        serial_puthex((uint64_t)ptr, 16);
        serial_puts("\n");
        return;
    }
    heap_used -= block->size;
    alloc_count--;
    free_list_insert(block);
    coalesce(block);
}

void *kcalloc(uint64_t count, uint64_t size) {
    uint64_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, uint64_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return (void *)0; }
    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - sizeof(block_hdr_t));
    if (block->magic != BLOCK_MAGIC) return (void *)0;
    if (block->size >= new_size) return ptr;
    void *np = kmalloc(new_size);
    if (!np) return (void *)0;
    memcpy(np, ptr, block->size);
    kfree(ptr);
    return np;
}

uint64_t heap_get_used(void)  { return heap_used; }
uint64_t heap_get_total(void) { return heap_size; }
uint64_t heap_get_count(void) { return alloc_count; }

void heap_init(void) {
    serial_puts("[HEAP] Initializing...\n");
    uint64_t init_size = HEAP_INIT_PAGES * PAGE_SIZE;
    heap_start = (uint8_t *)mem_alloc_pages(HEAP_INIT_PAGES);
    if (!heap_start) { serial_puts("[HEAP] FATAL: alloc failed\n"); return; }
    heap_end = heap_start + init_size;
    heap_size = init_size;
    heap_used = 0;
    alloc_count = 0;
    block_hdr_t *initial = (block_hdr_t *)heap_start;
    initial->magic = BLOCK_MAGIC;
    initial->flags = BLOCK_FREE;
    initial->size = init_size - sizeof(block_hdr_t);
    initial->next = (void *)0;
    initial->prev = (void *)0;
    free_list = initial;
    serial_puts("[HEAP] At ");
    serial_puthex((uint64_t)heap_start, 16);
    serial_puts(", ");
    serial_putdec(init_size / 1024);
    serial_puts(" KB\n");
    /* Self-test */
    void *a = kmalloc(128), *b = kmalloc(256), *c = kmalloc(64);
    if (a && b && c) {
        memset(a, 0xAA, 128); memset(b, 0xBB, 256); memset(c, 0xCC, 64);
        kfree(b);
        void *d = kmalloc(128);
        if (d) { kfree(a); kfree(c); kfree(d); serial_puts("[HEAP] Self-test passed\n"); }
        else serial_puts("[HEAP] Self-test FAILED\n");
    } else serial_puts("[HEAP] Self-test FAILED: alloc\n");
}
