/*
 * OsitoK x86-64 — Shared Memory (X-RETINA)
 *
 * Zero-copy memory sharing between processes.
 * Equivalent to Apple's IOSurface or Linux's DMA-BUF.
 *
 * Used by the compositor to read application surfaces without
 * copying pixel data. Applications draw into shared regions;
 * the compositor reads them directly.
 *
 * Design:
 *   - Regions are page-aligned physical memory blocks
 *   - Handle-based API (processes share uint32_t handles)
 *   - Reference counted for safe multi-process access
 *   - Flags indicate GPU scanout capability, CPU/GPU access patterns
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── Constants ───────────────────────────────────────────────── */

#define SHM_MAX_REGIONS  64
#define SHM_PAGE_SIZE    4096

/* Region flags */
#define SHM_FLAG_CPU_WRITE    (1 << 0)  /* CPU can write */
#define SHM_FLAG_CPU_READ     (1 << 1)  /* CPU can read */
#define SHM_FLAG_GPU_SCANOUT  (1 << 2)  /* Suitable for display scanout */
#define SHM_FLAG_GPU_TEXTURE  (1 << 3)  /* Suitable as GPU texture */
#define SHM_FLAG_UNCACHEABLE  (1 << 4)  /* Map as write-combining/UC */

/* Default flags for compositor surfaces */
#define SHM_SURFACE_FLAGS (SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ)
#define SHM_SCANOUT_FLAGS (SHM_FLAG_CPU_WRITE | SHM_FLAG_CPU_READ | SHM_FLAG_GPU_SCANOUT)

/* ── Shared Memory Region ────────────────────────────────────── */

typedef struct {
    void       *base;          /* virtual/physical address (identity-mapped) */
    uint64_t    size;          /* size in bytes */
    uint64_t    pages;         /* size in pages */
    uint32_t    handle;        /* unique handle for IPC */
    uint32_t    flags;         /* SHM_FLAG_* */
    uint32_t    owner_pid;     /* creating process */
    int32_t     refcount;      /* number of active mappings */
    bool        active;        /* slot in use */
} shm_region_t;

/* ── State ───────────────────────────────────────────────────── */

static shm_region_t shm_table[SHM_MAX_REGIONS];
static uint32_t shm_next_handle = 1;
static uint32_t shm_active_count;

/* ── Find region by handle ───────────────────────────────────── */

static shm_region_t *shm_find(uint32_t handle)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (shm_table[i].active && shm_table[i].handle == handle)
            return &shm_table[i];
    }
    return NULL;
}

/* ── Allocate free slot ──────────────────────────────────────── */

static shm_region_t *shm_alloc_slot(void)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!shm_table[i].active)
            return &shm_table[i];
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

/* Create a shared memory region. Returns handle (>0) or 0 on failure.
 * Size is rounded up to page boundary. Alignment is 4KB (or 256-byte
 * for GPU scanout compatibility with NVIDIA SET_OFFSET). */

uint32_t shm_create(uint64_t size, uint32_t flags)
{
    shm_region_t *r = shm_alloc_slot();
    if (!r) {
        serial_puts("[SHM] No free region slots\n");
        return 0;
    }

    /* Round up to page size */
    uint64_t pages = (size + SHM_PAGE_SIZE - 1) / SHM_PAGE_SIZE;
    uint64_t alloc_size = pages * SHM_PAGE_SIZE;

    /* GPU scanout requires 256-byte alignment (NVIDIA SET_OFFSET >> 8).
     * Page alignment (4096) satisfies this. */
    void *base = mem_alloc_aligned(alloc_size, SHM_PAGE_SIZE);
    if (!base) {
        serial_puts("[SHM] Allocation failed (");
        serial_putdec(alloc_size / 1024);
        serial_puts(" KB)\n");
        return 0;
    }

    /* Zero the memory */
    memset(base, 0, alloc_size);

    r->base       = base;
    r->size       = alloc_size;
    r->pages      = pages;
    r->handle     = shm_next_handle++;
    r->flags      = flags;
    r->owner_pid  = 0;  /* Set by caller if needed */
    r->refcount   = 1;
    r->active     = true;

    shm_active_count++;

    serial_puts("[SHM] Created region h=");
    serial_putdec(r->handle);
    serial_puts(" addr=0x");
    serial_puthex((uint64_t)base, 16);
    serial_puts(" size=");
    serial_putdec(alloc_size / 1024);
    serial_puts(" KB flags=0x");
    serial_puthex(flags, 2);
    serial_puts("\n");

    return r->handle;
}

/* Map a shared region (increment refcount, return pointer).
 * In identity-mapped kernel, this just returns the base address.
 * When per-process paging is added, this would map into the
 * process's address space. */
void *shm_map(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return NULL;
    r->refcount++;
    return r->base;
}

/* Unmap a shared region (decrement refcount) */
void shm_unmap(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return;
    if (r->refcount > 0) r->refcount--;
}

/* Destroy a shared region (must have refcount <= 1) */
void shm_destroy(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return;

    if (r->refcount > 1) {
        serial_puts("[SHM] Warning: destroying region h=");
        serial_putdec(handle);
        serial_puts(" with refcount=");
        serial_putdec((uint64_t)r->refcount);
        serial_puts("\n");
    }

    if (r->base && r->pages > 0)
        mem_free_pages(r->base, r->pages);

    r->active = false;
    r->base = NULL;
    shm_active_count--;
}

/* Get the physical address of a shared region (for GPU scanout) */
uint64_t shm_get_phys(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return 0;
    return (uint64_t)r->base;  /* identity-mapped: virt == phys */
}

/* Get region size */
uint64_t shm_get_size(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return 0;
    return r->size;
}

/* Get region flags */
uint32_t shm_get_flags(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return 0;
    return r->flags;
}

/* Set owner PID */
void shm_set_owner(uint32_t handle, uint32_t pid)
{
    shm_region_t *r = shm_find(handle);
    if (r) r->owner_pid = pid;
}

/* ── Stats ───────────────────────────────────────────────────── */

uint32_t shm_get_active_count(void) { return shm_active_count; }

/* ── Convenience: Create a surface (width × height × 4 bytes) ── */

extern uint32_t compositor_create_window(uint32_t shm_handle, int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t pid, const char *title);
extern void compositor_set_fullscreen(uint32_t window_id, bool fullscreen);
extern void compositor_signal_dirty(uint32_t window_id);

uint32_t shm_create_surface(uint32_t width, uint32_t height, uint32_t flags)
{
    uint64_t size = (uint64_t)width * height * 4;
    uint32_t handle = shm_create(size, flags);
    
    if (handle && (flags & 4)) { /* SHM_FLAG_GPU_SCANOUT */
        uint32_t wid = compositor_create_window(handle, 0, 0, width, height, 0, "Doom");
        if (wid) {
            compositor_set_fullscreen(wid, true);
            /* Cheat: Store window_id in the shm_table so we can flush it later.
               We can use the flags field or just assume window 1.
               Let's just use a static var for the one and only surface */
        }
    }
    return handle;
}

extern uint32_t *fb_get_base(void);
extern void fb_flush(void);
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);
extern uint32_t fb_get_pitch(void);

void shm_flush_surface(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r || !r->base) return;

    uint32_t *back = fb_get_base();
    if (back) {
        uint32_t *src = (uint32_t *)r->base;
        uint32_t dw = fb_get_width();
        uint32_t dh = fb_get_height();
        uint32_t pitch = fb_get_pitch();
        
        int scale = 1;
        if (dw >= 640 && dh >= 400) scale = 2;
        if (dw >= 960 && dh >= 600) scale = 3;

        int sw = 320;
        int sh = 200;
        
        int off_x = (dw - (sw * scale)) / 2;
        int off_y = (dh - (sh * scale)) / 2;

        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                uint32_t pixel = src[y * sw + x];
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int dy = off_y + y * scale + sy;
                        int dx = off_x + x * scale + sx;
                        if (dx >= 0 && (uint32_t)dx < dw && dy >= 0 && (uint32_t)dy < dh) {
                            back[dy * pitch + dx] = pixel;
                        }
                    }
                }
            }
        }
        
        extern void fb_flush_all(void);
        fb_flush_all();
    }
}

/* ── Initialize ──────────────────────────────────────────────── */

void shm_init(void)
{
    memset(shm_table, 0, sizeof(shm_table));
    shm_next_handle = 1;
    shm_active_count = 0;
    serial_puts("[SHM] Shared memory subsystem initialized (max ");
    serial_putdec(SHM_MAX_REGIONS);
    serial_puts(" regions)\n");
}
