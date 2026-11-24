/*
 * OsitoK - Variable-size heap allocator
 *
 * First-fit with forward coalescing on both alloc and free.
 *
 * Block layout:
 *   [header (4B)][data ...]
 *
 * The header stores block size (header + data) in bits [31:2]
 * and a used flag in bit 0. Sizes are always 4-byte aligned.
 *
 * Allocation scans linearly, merging adjacent free blocks on the
 * fly (eager forward coalescing). Splits oversized blocks when
 * the remainder can hold at least MIN_DATA + header.
 */

#include "mem/heap.h"
#include "drivers/uart.h"

extern "C" {

/* Block header */
typedef struct {
    uint32_t info;  /* [31:2] = total size (hdr+data), [0] = used */
} heap_hdr_t;

#define HDR_SIZE    sizeof(heap_hdr_t)   /* 4 bytes */
#define MIN_DATA    4                     /* minimum useful allocation */
#define ALIGN4(x)   (((x) + 3) & ~3u)

#define BLK_SIZE(h)  ((h)->info & ~3u)
#define BLK_USED(h)  ((h)->info & 1u)
#define NEXT_BLK(h)  ((heap_hdr_t *)((uint8_t *)(h) + BLK_SIZE(h)))

/* Heap region */
static uint8_t heap_mem[HEAP_SIZE] __attribute__((aligned(4)));
static heap_hdr_t *heap_start;
static uint8_t    *heap_end;

void heap_init(void)
{
    heap_start = (heap_hdr_t *)heap_mem;
    heap_end   = heap_mem + HEAP_SIZE;

    /* Single free block spanning entire heap */
    heap_start->info = HEAP_SIZE;  /* used = 0 */

    uart_puts("heap: ");
    uart_put_dec(HEAP_SIZE);
    uart_puts(" bytes\n");
}

void *heap_alloc(uint32_t size)
{
    if (size == 0) return NULL;

    uint32_t need = ALIGN4(size) + HDR_SIZE;
    if (need < HDR_SIZE + MIN_DATA)
        need = HDR_SIZE + MIN_DATA;

    uint32_t ps = irq_save();

    heap_hdr_t *h = heap_start;

    while ((uint8_t *)h < heap_end) {
        if (BLK_USED(h)) {
            h = NEXT_BLK(h);
            continue;
        }

        /* Forward coalesce: merge consecutive free blocks */
        heap_hdr_t *next = NEXT_BLK(h);
        while ((uint8_t *)next < heap_end && !BLK_USED(next)) {
            h->info += BLK_SIZE(next);
            next = NEXT_BLK(h);
        }

        uint32_t bsz = BLK_SIZE(h);

        if (bsz >= need) {
            /* Split if leftover can hold another block */
            if (bsz - need >= HDR_SIZE + MIN_DATA) {
                heap_hdr_t *split = (heap_hdr_t *)((uint8_t *)h + need);
                split->info = bsz - need;   /* free remainder */
                h->info = need | 1u;        /* used, exact size */
            } else {
                h->info = bsz | 1u;         /* use whole block */
            }
            irq_restore(ps);
            return (uint8_t *)h + HDR_SIZE;
        }

        h = next;
    }

    irq_restore(ps);
    return NULL;  /* out of memory */
}

void heap_free(void *ptr)
{
    if (ptr == NULL) return;

    heap_hdr_t *h = (heap_hdr_t *)((uint8_t *)ptr - HDR_SIZE);

    /* Bounds check */
    if ((uint8_t *)h < (uint8_t *)heap_start ||
        (uint8_t *)h >= heap_end)
        return;

    uint32_t ps = irq_save();

    /* Mark free */
    h->info &= ~1u;

    /* Forward coalesce */
    heap_hdr_t *next = NEXT_BLK(h);
    while ((uint8_t *)next < heap_end && !BLK_USED(next)) {
        h->info += BLK_SIZE(next);
        next = NEXT_BLK(h);
    }

    irq_restore(ps);
}

/* ====== Diagnostics ====== */

uint32_t heap_free_total(void)
{
    uint32_t total = 0;
    heap_hdr_t *h = heap_start;
    while ((uint8_t *)h < heap_end) {
        if (!BLK_USED(h))
            total += BLK_SIZE(h) - HDR_SIZE;
        h = NEXT_BLK(h);
    }
    return total;
}

uint32_t heap_used_total(void)
{
    uint32_t total = 0;
    heap_hdr_t *h = heap_start;
    while ((uint8_t *)h < heap_end) {
        if (BLK_USED(h))
            total += BLK_SIZE(h) - HDR_SIZE;
        h = NEXT_BLK(h);
    }
    return total;
}

uint32_t heap_largest_free(void)
{
    uint32_t largest = 0;
    heap_hdr_t *h = heap_start;
    while ((uint8_t *)h < heap_end) {
        if (!BLK_USED(h)) {
            uint32_t sz = BLK_SIZE(h) - HDR_SIZE;
            if (sz > largest) largest = sz;
        }
        h = NEXT_BLK(h);
    }
    return largest;
}

uint32_t heap_frag_count(void)
{
    uint32_t count = 0;
    heap_hdr_t *h = heap_start;
    while ((uint8_t *)h < heap_end) {
        if (!BLK_USED(h))
            count++;
        h = NEXT_BLK(h);
    }
    return count;
}

} /* extern "C" */
