/*
 * OsitoK x86-64 — Heap Allocator (malloc/free)
 *
 * X-OS3: General-purpose heap over the physical page allocator.
 * First-fit free list with block coalescing. Grows by requesting
 * pages from mem_alloc_pages(). Provides malloc(), free(), calloc(),
 * realloc() for kernel and (eventually) userspace via brk/sbrk.
 *
 * Design: Simple boundary-tag allocator. Each block has a header
 * with size + in-use flag. Free blocks are linked in a list.
 * Adjacent free blocks are merged on free() to reduce fragmentation.
 */

#include "../include/types.h"
#include "../include/paging.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── Constants ───────────────────────────────────────────────── */

#define PAGE_SIZE       4096
/* Heap sizes: dynamic from sys_caps (scale with RAM) */
#include "../include/sys_caps.h"
#define HEAP_INIT_PAGES ((uint32_t)(g_sys_caps.heap_init_size ? g_sys_caps.heap_init_size / 4096 : 64))
#define HEAP_GROW_PAGES ((uint32_t)(g_sys_caps.heap_grow_size ? g_sys_caps.heap_grow_size / 4096 : 16))
#define MIN_ALLOC       16      /* Minimum allocation size (alignment) */
#define ALIGNMENT       16      /* All allocations 16-byte aligned */

#define BLOCK_MAGIC     0x4F53  /* "OS" — corruption detector */
#define BLOCK_FREE      0
#define BLOCK_USED      1

/* ── Block header ────────────────────────────────────────────── */

/* Block header. The `next`/`prev` slots are only used for free-list
 * linking while the block is FREE. While USED, those 16 bytes are dead
 * space, so we repurpose them as a `(alloc_ra, alloc_ra2)` pair filled
 * by `kmalloc()` from `__builtin_return_address(0/1)`. On double-free
 * the diagnostic in `kfree()` reads them to identify the original
 * allocator's call site (resolved via kallsyms). */
typedef struct block_hdr {
    uint16_t            magic;      /* BLOCK_MAGIC for corruption check */
    uint16_t            flags;      /* BLOCK_FREE or BLOCK_USED */
    uint32_t            _pad;
    uint64_t            size;       /* Usable size (excludes header) */
    union {
        struct {
            struct block_hdr *next; /* free-list link (only when FREE) */
            struct block_hdr *prev; /* free-list link (only when FREE) */
        };
        struct {
            uint64_t  alloc_ra;     /* kmalloc caller (only when USED) */
            uint64_t  alloc_ra2;    /* one frame above (only when USED) */
        };
    };
} block_hdr_t;

_Static_assert(sizeof(block_hdr_t) == 32, "block header must be 32 bytes");
_Static_assert(sizeof(block_hdr_t) % ALIGNMENT == 0, "header must be aligned");

/* ── Heap state ──────────────────────────────────────────────── */

static block_hdr_t *free_list;     /* Head of free block list */
static uint8_t     *heap_start;    /* Start of heap region */
static uint8_t     *heap_end;      /* Current end of heap */
static uint64_t     heap_size;     /* Total heap bytes */
static uint64_t     heap_used;     /* Currently allocated bytes */
static uint64_t     alloc_count;   /* Number of active allocations */

/* ── Align up ────────────────────────────────────────────────── */

static inline uint64_t align_up(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/* ── Insert block into free list (sorted by address) ─────────── */

static void free_list_insert(block_hdr_t *block)
{
    block->flags = BLOCK_FREE;

    /* Find insertion point (sorted by address for coalescing) */
    block_hdr_t *prev = NULL;
    block_hdr_t *curr = free_list;

    while (curr && curr < block) {
        prev = curr;
        curr = curr->next;
    }

    block->next = curr;
    block->prev = prev;

    if (prev) prev->next = block;
    else free_list = block;

    if (curr) curr->prev = block;
}

/* ── Remove block from free list ─────────────────────────────── */

static void free_list_remove(block_hdr_t *block)
{
    if (block->prev) block->prev->next = block->next;
    else free_list = block->next;

    /* Pre-existing typo: this used to assign `block->next->prev = block`
     * (pointing the successor's prev BACK at the block being removed),
     * which left the doubly-linked list inconsistent. The bug was
     * dormant until the block-header union turned the prev/next slots
     * of USED blocks into alloc_ra1/alloc_ra2 (return addresses). After
     * that change, any free-list walker that traversed a stale prev
     * pointer into a now-USED block read a non-NULL garbage pointer
     * (the saved RA) and faulted on the next deref — observed as a
     * non-canonical CR2 inside elf_exec on the second `exec quake2.elf`
     * after a clean Q2 shutdown. */
    if (block->next) block->next->prev = block->prev;

    block->next = NULL;
    block->prev = NULL;
}

/* ── Coalesce adjacent free blocks ───────────────────────────── */

static void coalesce(block_hdr_t *block)
{
    /* Merge with next block if adjacent and free */
    if (block->next) {
        uint8_t *block_end = (uint8_t *)block + sizeof(block_hdr_t) + block->size;
        if (block_end == (uint8_t *)block->next) {
            block_hdr_t *next = block->next;
            block->size += sizeof(block_hdr_t) + next->size;
            block->next = next->next;
            if (next->next) next->next->prev = block;
        }
    }

    /* Merge with previous block if adjacent and free */
    if (block->prev) {
        uint8_t *prev_end = (uint8_t *)block->prev + sizeof(block_hdr_t) + block->prev->size;
        if (prev_end == (uint8_t *)block) {
            block_hdr_t *prev = block->prev;
            prev->size += sizeof(block_hdr_t) + block->size;
            prev->next = block->next;
            if (block->next) block->next->prev = prev;
        }
    }
}

/* ── Grow heap by requesting more pages ──────────────────────── */

static int heap_grow(uint64_t min_bytes)
{
    uint64_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < HEAP_GROW_PAGES) pages = HEAP_GROW_PAGES;

    /* Allocate fresh physical pages and view them through the
     * upper-half direct map so the whole heap runs at KERNEL_VBASE+. */
    void *phys = mem_alloc_pages(pages);
    if (!phys) {
        serial_puts("[HEAP] Failed to grow heap\n");
        return -1;
    }
    void *new_pages = PHYS_TO_VIRT(phys);

    uint64_t new_size = pages * PAGE_SIZE;

    /* If new pages are adjacent, extend the last free block or create new one */
    block_hdr_t *new_block = (block_hdr_t *)new_pages;
    new_block->magic = BLOCK_MAGIC;
    new_block->flags = BLOCK_FREE;
    new_block->size = new_size - sizeof(block_hdr_t);
    new_block->next = NULL;
    new_block->prev = NULL;

    free_list_insert(new_block);
    coalesce(new_block);

    heap_size += new_size;

    /* Update heap_end if this extends it */
    uint8_t *new_end = (uint8_t *)new_pages + new_size;
    if (new_end > heap_end) heap_end = new_end;

    return 0;
}

/* ── Split a free block if it's much larger than needed ─────── */

static void block_split(block_hdr_t *block, uint64_t needed)
{
    uint64_t remaining = block->size - needed;

    /* Only split if remainder can hold a header + MIN_ALLOC */
    if (remaining < sizeof(block_hdr_t) + MIN_ALLOC)
        return;

    /* Create new free block after the used portion */
    block_hdr_t *new_block = (block_hdr_t *)((uint8_t *)block + sizeof(block_hdr_t) + needed);
    new_block->magic = BLOCK_MAGIC;
    new_block->flags = BLOCK_FREE;
    new_block->size = remaining - sizeof(block_hdr_t);

    /* Shrink original block */
    block->size = needed;

    /* Insert new block into free list after current */
    new_block->next = block->next;
    new_block->prev = block->prev;

    if (block->next) block->next->prev = new_block;
    if (block->prev) block->prev->next = new_block;
    if (free_list == block) free_list = new_block;

    /* Original block is being removed from free list by caller */
}

/* ── malloc ──────────────────────────────────────────────────── */

/* Internal allocator: receives a pre-captured caller RA so the public
 * `kmalloc()` wrapper can stamp it into the block header. The grow-path
 * recursion stays here so the original RA is preserved across retry. */
static void *_kmalloc_with_ra(uint64_t size, uint64_t alloc_ra, uint64_t alloc_ra2)
{
    if (size == 0) return NULL;

    /* Align size up */
    size = align_up(size, ALIGNMENT);
    if (size < MIN_ALLOC) size = MIN_ALLOC;

    /* First-fit search */
    block_hdr_t *block = free_list;
    while (block) {
        if (block->size >= size) {
            /* Found a fit — split if much larger */
            block_split(block, size);
            free_list_remove(block);
            block->flags     = BLOCK_USED;
            block->alloc_ra  = alloc_ra;   /* diagnostic — see kfree() */
            block->alloc_ra2 = alloc_ra2;
            heap_used += block->size;
            alloc_count++;
            return (void *)((uint8_t *)block + sizeof(block_hdr_t));
        }
        block = block->next;
    }

    /* No fit — grow heap and retry */
    uint64_t needed = sizeof(block_hdr_t) + size;
    if (heap_grow(needed) < 0)
        return NULL;

    return _kmalloc_with_ra(size, alloc_ra, alloc_ra2);
}

void *kmalloc(uint64_t size)
{
    return _kmalloc_with_ra(size,
                            (uint64_t)__builtin_return_address(0),
                            (uint64_t)__builtin_return_address(1));
}

/* ── free ────────────────────────────────────────────────────── */

/* Print "(symbol+0xoffset)" or nothing if kallsyms doesn't know `addr`.
 * Declared local so heap.c doesn't need a header for kallsyms. */
extern const char *kallsyms_lookup(uint64_t addr, uint64_t *offset_out);
static void heap_print_sym(uint64_t addr)
{
    uint64_t off = 0;
    const char *sym = kallsyms_lookup(addr, &off);
    if (!sym) return;
    serial_puts(" (");
    serial_puts(sym);
    serial_puts("+0x");
    serial_puthex(off, 4);
    serial_puts(")");
}

void kfree(void *ptr)
{
    if (!ptr) return;

    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - sizeof(block_hdr_t));

    /* Corruption check */
    if (block->magic != BLOCK_MAGIC) {
        serial_puts("[HEAP] !!! CORRUPTION: bad magic at 0x");
        serial_puthex((uint64_t)block, 16);
        serial_puts(" caller=0x");
        serial_puthex((uint64_t)__builtin_return_address(0), 16);
        heap_print_sym((uint64_t)__builtin_return_address(0));
        serial_puts("\n");
        return;
    }

    if (block->flags != BLOCK_USED) {
        /* Read alloc_ra/alloc_ra2 BEFORE anything else — coalesce or
         * free_list_insert from a later iteration may have stomped
         * the next/prev union members already. We are early-returning
         * here so the read is safe. */
        uint64_t orig_ra  = block->alloc_ra;
        uint64_t orig_ra2 = block->alloc_ra2;
        uint64_t free_ra  = (uint64_t)__builtin_return_address(0);

        serial_puts("[HEAP] !!! DOUBLE FREE at 0x");
        serial_puthex((uint64_t)ptr, 16);
        serial_puts(" size=");
        serial_putdec(block->size);
        serial_puts("\n  freed by 0x");
        serial_puthex(free_ra, 16);
        heap_print_sym(free_ra);
        serial_puts("\n  originally allocated from 0x");
        serial_puthex(orig_ra, 16);
        heap_print_sym(orig_ra);
        serial_puts("\n                     parent 0x");
        serial_puthex(orig_ra2, 16);
        heap_print_sym(orig_ra2);
        serial_puts("\n");
        return;
    }

    heap_used -= block->size;
    alloc_count--;

    free_list_insert(block);
    coalesce(block);
}

/* ── calloc ──────────────────────────────────────────────────── */

void *kcalloc(uint64_t count, uint64_t size)
{
    uint64_t total = count * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/* ── realloc ─────────────────────────────────────────────────── */

void *krealloc(void *ptr, uint64_t new_size)
{
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    block_hdr_t *block = (block_hdr_t *)((uint8_t *)ptr - sizeof(block_hdr_t));
    if (block->magic != BLOCK_MAGIC) return NULL;

    /* If current block is large enough, keep it */
    if (block->size >= new_size) return ptr;

    /* Allocate new, copy, free old */
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

/* ── Stats ───────────────────────────────────────────────────── */

uint64_t heap_get_used(void)  { return heap_used; }
uint64_t heap_get_total(void) { return heap_size; }
uint64_t heap_get_count(void) { return alloc_count; }

/* ── Initialize heap ─────────────────────────────────────────── */

void heap_init(void)
{
    serial_puts("[HEAP] Initializing kernel heap...\n");

    /* Allocate initial heap pages and keep them through the upper-half
     * direct map — the entire heap lives at KERNEL_VBASE+. */
    uint64_t init_size = HEAP_INIT_PAGES * PAGE_SIZE;  /* 256 KB */
    void *phys = mem_alloc_pages(HEAP_INIT_PAGES);
    if (!phys) {
        serial_puts("[HEAP] FATAL: Cannot allocate initial heap\n");
        return;
    }
    heap_start = (uint8_t *)PHYS_TO_VIRT(phys);

    heap_end = heap_start + init_size;
    heap_size = init_size;
    heap_used = 0;
    alloc_count = 0;

    /* Create one big free block spanning the whole heap */
    block_hdr_t *initial = (block_hdr_t *)heap_start;
    initial->magic = BLOCK_MAGIC;
    initial->flags = BLOCK_FREE;
    initial->size = init_size - sizeof(block_hdr_t);
    initial->next = NULL;
    initial->prev = NULL;

    free_list = initial;

    serial_puts("[HEAP] Heap at 0x");
    serial_puthex((uint64_t)heap_start, 16);
    serial_puts(", size ");
    serial_putdec(init_size / 1024);
    serial_puts(" KB\n");

    /* Quick self-test */
    void *a = kmalloc(128);
    void *b = kmalloc(256);
    void *c = kmalloc(64);

    if (a && b && c) {
        memset(a, 0xAA, 128);
        memset(b, 0xBB, 256);
        memset(c, 0xCC, 64);

        kfree(b);  /* Free middle block — tests fragmentation */
        void *d = kmalloc(128);  /* Should reuse part of b's space */

        if (d) {
            kfree(a);
            kfree(c);
            kfree(d);
            serial_puts("[HEAP] Self-test passed (alloc/free/coalesce)\n");
        } else {
            serial_puts("[HEAP] Self-test FAILED: realloc after free\n");
        }
    } else {
        serial_puts("[HEAP] Self-test FAILED: initial alloc\n");
    }

    fb_puts(" Heap: ");
    fb_putdec(init_size / 1024);
    fb_puts(" KB initial\n");
}
