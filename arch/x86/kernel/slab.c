/*
 * OsitoK x86-64 — Slab Allocator (kmem_cache)
 *
 * Object-based memory allocator for frequently allocated/freed
 * fixed-size structures. Reduces fragmentation vs general heap.
 *
 * Each cache manages objects of a specific size. Slabs are
 * page-aligned blocks subdivided into object slots.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  kfree(void *ptr);

/* ── Slab Structure ──────────────────────────────────────────── */

#define SLAB_PAGE_SIZE  4096
#define SLAB_MAX_CACHES 16
#define SLAB_MAX_SLABS  32

typedef struct slab {
    void    *base;              /* Page-aligned memory */
    uint64_t bitmap[2];         /* 128-bit free bitmap (max 128 objects/slab) */
    uint32_t obj_count;         /* Total objects in this slab */
    uint32_t free_count;        /* Free objects remaining */
    bool     active;
} slab_t;

typedef struct {
    bool     active;
    char     name[32];
    uint32_t obj_size;          /* Size of each object (aligned) */
    uint32_t align;             /* Alignment requirement */
    uint32_t objs_per_slab;     /* Objects per slab page */
    slab_t   slabs[SLAB_MAX_SLABS];
    int      slab_count;
    /* Stats */
    uint64_t alloc_count;
    uint64_t free_count_stat;
} kmem_cache_t;

static kmem_cache_t caches[SLAB_MAX_CACHES];

/* ── Cache Creation ──────────────────────────────────────────── */

int kmem_cache_create(const char *name, uint32_t size, uint32_t align)
{
    if (size == 0) return -1;
    if (align == 0) align = 8;
    /* Round size up to alignment */
    size = (size + align - 1) & ~(align - 1);

    for (int i = 0; i < SLAB_MAX_CACHES; i++) {
        if (!caches[i].active) {
            kmem_cache_t *c = &caches[i];
            memset(c, 0, sizeof(*c));
            c->active = true;
            c->obj_size = size;
            c->align = align;
            c->objs_per_slab = SLAB_PAGE_SIZE / size;
            if (c->objs_per_slab > 128) c->objs_per_slab = 128;
            if (c->objs_per_slab == 0) c->objs_per_slab = 1;
            int j = 0;
            while (name[j] && j < 31) { c->name[j] = name[j]; j++; }
            c->name[j] = '\0';

            serial_puts("[SLAB] Cache: ");
            serial_puts(c->name);
            serial_puts(" obj=");
            serial_putdec(size);
            serial_puts(" per_slab=");
            serial_putdec(c->objs_per_slab);
            serial_puts("\n");
            return i;
        }
    }
    return -1;
}

/* ── Slab Allocation ─────────────────────────────────────────── */

static slab_t *slab_grow(kmem_cache_t *c)
{
    if (c->slab_count >= SLAB_MAX_SLABS) return NULL;

    slab_t *s = &c->slabs[c->slab_count++];
    uint64_t alloc_size = (uint64_t)c->objs_per_slab * c->obj_size;
    if (alloc_size < SLAB_PAGE_SIZE) alloc_size = SLAB_PAGE_SIZE;

    s->base = mem_alloc_aligned(alloc_size, SLAB_PAGE_SIZE);
    if (!s->base) { c->slab_count--; return NULL; }

    memset(s->base, 0, alloc_size);
    s->obj_count = c->objs_per_slab;
    s->free_count = c->objs_per_slab;
    s->active = true;

    /* Mark all objects as free (bit = 1 = free) */
    s->bitmap[0] = s->bitmap[1] = 0;
    for (uint32_t i = 0; i < s->obj_count && i < 128; i++) {
        s->bitmap[i / 64] |= (1ULL << (i % 64));
    }
    return s;
}

/* ── Object Allocation ───────────────────────────────────────── */

void *kmem_cache_alloc(int cache_idx)
{
    if (cache_idx < 0 || cache_idx >= SLAB_MAX_CACHES) return NULL;
    kmem_cache_t *c = &caches[cache_idx];
    if (!c->active) return NULL;

    /* Find a slab with free objects */
    for (int i = 0; i < c->slab_count; i++) {
        slab_t *s = &c->slabs[i];
        if (!s->active || s->free_count == 0) continue;

        /* Find first free bit */
        for (int w = 0; w < 2; w++) {
            if (s->bitmap[w] == 0) continue;
            int bit = __builtin_ctzll(s->bitmap[w]);
            uint32_t idx = w * 64 + bit;
            if (idx >= s->obj_count) continue;

            s->bitmap[w] &= ~(1ULL << bit);
            s->free_count--;
            c->alloc_count++;
            return (uint8_t *)s->base + (uint64_t)idx * c->obj_size;
        }
    }

    /* No free objects — grow */
    slab_t *ns = slab_grow(c);
    if (!ns) return NULL;

    /* Allocate from new slab (first object) */
    ns->bitmap[0] &= ~1ULL;
    ns->free_count--;
    c->alloc_count++;
    return ns->base;
}

/* ── Object Free ─────────────────────────────────────────────── */

void kmem_cache_free(int cache_idx, void *ptr)
{
    if (cache_idx < 0 || cache_idx >= SLAB_MAX_CACHES || !ptr) return;
    kmem_cache_t *c = &caches[cache_idx];

    for (int i = 0; i < c->slab_count; i++) {
        slab_t *s = &c->slabs[i];
        if (!s->active) continue;
        uint64_t off = (uint64_t)((uint8_t *)ptr - (uint8_t *)s->base);
        if (off < (uint64_t)s->obj_count * c->obj_size) {
            uint32_t idx = (uint32_t)(off / c->obj_size);
            s->bitmap[idx / 64] |= (1ULL << (idx % 64));
            s->free_count++;
            c->free_count_stat++;
            return;
        }
    }
}

/* ── Stats ───────────────────────────────────────────────────── */

void kmem_cache_stats(void)
{
    serial_puts("[SLAB] Caches:\n");
    for (int i = 0; i < SLAB_MAX_CACHES; i++) {
        if (!caches[i].active) continue;
        serial_puts("  "); serial_puts(caches[i].name);
        serial_puts(": allocs="); serial_putdec(caches[i].alloc_count);
        serial_puts(" frees="); serial_putdec(caches[i].free_count_stat);
        serial_puts(" slabs="); serial_putdec((uint64_t)caches[i].slab_count);
        serial_puts("\n");
    }
}
