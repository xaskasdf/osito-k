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
#include "../include/paging.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern uint32_t proc_exec_pid(void);
extern const char *proc_current_name(void);

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
    void       *base;            /* upper-half mirror (PHYS_TO_VIRT) */
    uint64_t    size;            /* size in bytes */
    uint64_t    pages;           /* size in pages */
    uint32_t    handle;          /* unique handle for IPC */
    uint32_t    flags;           /* SHM_FLAG_* */
    uint32_t    surf_width;      /* nonzero for shm_create_surface() */
    uint32_t    surf_height;     /* nonzero for shm_create_surface() */
    uint32_t    owner_pid;       /* creating process */
    int32_t     refcount;        /* number of active mappings */
    bool        active;          /* slot in use */
    bool        pending_destroy; /* shm_destroy pending until refcount==0 */
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
     * Page alignment (4096) satisfies this. mem_alloc_aligned returns
     * a physical address; convert to the upper-half mirror (PML4[256],
     * shared across every process's CR3 post-Phase-C) so CPU accesses
     * via this handle work regardless of which CR3 is live when the
     * syscall runs. shm_get_phys still returns the phys via
     * VIRT_TO_PHYS for GPU scanout callers. */
    void *phys = mem_alloc_aligned(alloc_size, SHM_PAGE_SIZE);
    if (!phys) {
        serial_puts("[SHM] Allocation failed (");
        serial_putdec(alloc_size / 1024);
        serial_puts(" KB)\n");
        return 0;
    }
    void *base = PHYS_TO_VIRT(phys);

    /* Zero the memory via the upper-half mirror */
    memset(base, 0, alloc_size);

    r->base            = base;
    r->size            = alloc_size;
    r->pages           = pages;
    r->handle          = shm_next_handle++;
    r->flags           = flags;
    r->surf_width      = 0;
    r->surf_height     = 0;
    r->owner_pid       = 0;  /* Set by caller if needed */
    r->refcount        = 1;
    r->active          = true;
    r->pending_destroy = false;

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

/* Unmap a shared region (decrement refcount). If the region was already
 * marked for destroy by an explicit shm_destroy() call AND we drop the
 * last reference, free the underlying pages here so that the original
 * destroy doesn't have to wait for refcount to reach zero before it
 * answers to the caller. */
static void shm_release_pages(shm_region_t *r)
{
    if (r->base && r->pages > 0)
        mem_free_pages((void *)VIRT_TO_PHYS(r->base), r->pages);
    r->base = NULL;
    r->pages = 0;
    r->active = false;
    shm_active_count--;
}

void shm_unmap(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return;
    if (r->refcount > 0) r->refcount--;
    /* Deferred destroy: the creator already asked us to destroy this
     * region (r->pending_destroy=true) but other holders kept a
     * reference. When the last one drops, finish the job. */
    if (r->pending_destroy && r->refcount == 0)
        shm_release_pages(r);
}

/* Destroy a shared region. Refcount-aware: if other holders still have
 * a reference (compositor's win->pixels, peer mappings, etc.), DO NOT
 * free the underlying pages — that would leave the other holders with
 * dangling pointers into pages that mem_free_pages handed back to the
 * page allocator, which `heap_grow()` could then re-use as a heap
 * arena page → silent heap corruption. Instead, mark the region
 * pending_destroy and let the last shm_unmap finish the job. */
void shm_destroy(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r) return;

    /* The creator counts as one reference. Drop it now so a "destroy
     * with no other holders" path collapses to refcount==0 below. */
    if (r->refcount > 0) r->refcount--;

    if (r->refcount > 0) {
        /* Other holders still around — defer the page free. They will
         * call shm_unmap() when they're done; the last one triggers
         * shm_release_pages() via the pending_destroy flag. */
        r->pending_destroy = true;
        return;
    }

    shm_release_pages(r);
}

/* Get the physical address of a shared region (for GPU scanout) */
uint64_t shm_get_phys(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r || !r->base) return 0;
    return VIRT_TO_PHYS(r->base);
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

/* Destroy all regions owned by a process (called on proc_free).
 *
 * Two cases:
 *   - The process never called shm_destroy on its region (crash, kill,
 *     or just left it dangling) → pending_destroy is false. We must
 *     drop the creator ref ourselves, which we do by calling
 *     shm_destroy(handle).
 *   - The process did call shm_destroy explicitly (e.g. Q2 in
 *     SWimp_Shutdown) → pending_destroy is true. The creator ref was
 *     already decremented; calling shm_destroy again would
 *     over-decrement. compositor_cleanup_process will run shortly
 *     after this and drop the remaining holder refs via shm_unmap,
 *     and the last one finishes the release via the pending flag.
 *     Just log and skip. */
void shm_cleanup_process(uint32_t pid)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!shm_table[i].active) continue;
        if (shm_table[i].owner_pid != pid) continue;

        serial_puts("[SHM] Cleanup pid=");
        serial_putdec(pid);
        serial_puts(" h=");
        serial_putdec(shm_table[i].handle);
        if (shm_table[i].pending_destroy) {
            serial_puts(" (already pending destroy)\n");
            continue;
        }
        serial_puts("\n");
        shm_destroy(shm_table[i].handle);
    }
}

/* ── Stats ───────────────────────────────────────────────────── */

uint32_t shm_get_active_count(void) { return shm_active_count; }

/* ── Convenience: Create a surface (width × height × 4 bytes) ── */

extern uint32_t compositor_create_window(uint32_t shm_handle, int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t pid, const char *title);
extern void compositor_set_fullscreen(uint32_t window_id, bool fullscreen);
extern void compositor_signal_dirty(uint32_t window_id);
extern uint32_t compositor_find_window_by_shm(uint32_t shm_handle);

uint32_t shm_create_surface(uint32_t width, uint32_t height, uint32_t flags)
{
    if (!width || !height || width > 0xFFFFU || height > 0xFFFFU)
        return 0;

    uint64_t size = (uint64_t)width * height * 4;
    uint32_t handle = shm_create(size, flags);
    if (handle) {
        shm_region_t *r = shm_find(handle);
        if (r) {
            r->surf_width = width;
            r->surf_height = height;
        }
    }

    if (handle && (flags & SHM_FLAG_GPU_SCANOUT)) {
        uint32_t owner_pid = proc_exec_pid();
        const char *title = proc_current_name();
        if (!title || !*title)
            title = "Application";
        shm_set_owner(handle, owner_pid);
        uint32_t wid = compositor_create_window(handle, 0, 0, width, height,
                                                owner_pid, title);
        if (wid)
            compositor_set_fullscreen(wid, true);
    }
    return handle;
}

extern uint32_t *fb_get_base(void);
extern void fb_flush(void);
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);
extern uint32_t fb_get_pitch(void);

extern bool virtio_gpu_ready(void) __attribute__((weak));
extern uint32_t *virtio_gpu_get_fb(void) __attribute__((weak));
extern uint32_t virtio_gpu_get_width(void) __attribute__((weak));
extern uint32_t virtio_gpu_get_height(void) __attribute__((weak));
extern void virtio_gpu_flush(void) __attribute__((weak));
extern uint64_t idt_get_ticks(void);

static void shm_direct_virtio_scanout(uint32_t *src, uint32_t src_pitch,
                                      uint32_t src_w, uint32_t src_h)
{
    if (!virtio_gpu_ready || !virtio_gpu_get_fb || !virtio_gpu_get_width ||
        !virtio_gpu_get_height || !virtio_gpu_flush)
        return;
    if (!virtio_gpu_ready()) return;

    uint32_t *dst = virtio_gpu_get_fb();
    if (!dst) return;

    uint32_t dst_w = virtio_gpu_get_width();
    uint32_t dst_h = virtio_gpu_get_height();
    uint32_t copy_w = src_w < dst_w ? src_w : dst_w;
    uint32_t copy_h = src_h < dst_h ? src_h : dst_h;

    for (uint32_t y = 0; y < copy_h; y++)
        memcpy(dst + y * dst_w, src + y * src_pitch, copy_w * sizeof(uint32_t));

    static uint64_t last_flush_tick;
    uint64_t now = idt_get_ticks();
    if (last_flush_tick != 0 && now - last_flush_tick < 6)
        return;
    last_flush_tick = now;

    static bool logged;
    if (!logged) {
        serial_puts("[SHM] direct virtio scanout fallback enabled\n");
        logged = true;
    }
    virtio_gpu_flush();
}

void shm_flush_surface(uint32_t handle)
{
    shm_region_t *r = shm_find(handle);
    if (!r || !r->base) return;

    /* When compositor is running it reads win->pixels (same SHM region)
     * directly each frame. Mark the matching compositor window dirty so
     * the next frame blits the SHM surface and flips it to scanout. */
    extern bool compositor_is_running(void);
    if (compositor_is_running()) {
        uint32_t wid = compositor_find_window_by_shm(handle);
        if (wid)
            compositor_signal_dirty(wid);
        return;
    }

    uint32_t *src = (uint32_t *)r->base;
    uint32_t sw = r->surf_width;
    uint32_t sh = r->surf_height;
    uint64_t npixels = r->size / 4;
    if (!sw || !sh || (uint64_t)sw * sh > npixels) {
        if      (npixels == 1024u * 768u) { sw = 1024; sh = 768; }
        else if (npixels == 640u  * 480u) { sw = 640;  sh = 480; }
        else if (npixels == 512u  * 512u) { sw = 512;  sh = 512; }
        else if (npixels == 320u  * 240u) { sw = 320;  sh = 240; }
        else if (npixels == 320u  * 200u) { sw = 320;  sh = 200; }
        else { sw = npixels ? (uint32_t)npixels : 1; sh = 1; }
    }

    /* Direct virtio fallback can run even when boot GOP exposed fb=0. */
    uint32_t *back = fb_get_base();
    if (!back) {
        static uint32_t nofb_log_count;
        if (nofb_log_count < 8) {
            serial_puts("[SHM] flush h=");
            serial_putdec(handle);
            serial_puts(" surface=");
            serial_putdec(sw);
            serial_puts("x");
            serial_putdec(sh);
            serial_puts(" fb=virtio-only\n");
            nofb_log_count++;
        }
        shm_direct_virtio_scanout(src, sw, sw, sh);
        return;
    }

    /* Direct-boot fallback (no compositor): blit SHM surface to framebuffer. */
    uint32_t dw = fb_get_width();
    uint32_t dh = fb_get_height();
    uint32_t pitch = fb_get_pitch();

    static uint32_t flush_log_count;
    if (flush_log_count < 8) {
        serial_puts("[SHM] flush h=");
        serial_putdec(handle);
        serial_puts(" surface=");
        serial_putdec(sw);
        serial_puts("x");
        serial_putdec(sh);
        serial_puts(" fb=");
        serial_putdec(dw);
        serial_puts("x");
        serial_putdec(dh);
        serial_puts("\n");
        flush_log_count++;
    }

    int scale = 1;
    if (dw >= sw * 2 && dh >= sh * 2) scale = 2;
    if (dw >= sw * 3 && dh >= sh * 3) scale = 3;

    int off_x = (int)((dw - sw * (uint32_t)scale) / 2);
    int off_y = (int)((dh - sh * (uint32_t)scale) / 2);

    memset(back, 0, (uint64_t)pitch * dh * sizeof(uint32_t));

    for (int y = 0; y < (int)sh; y++) {
        for (int x = 0; x < (int)sw; x++) {
            uint32_t pixel = src[y * sw + x];
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int dy = off_y + y * scale + sy;
                    int dx = off_x + x * scale + sx;
                    if (dx >= 0 && (uint32_t)dx < dw && dy >= 0 && (uint32_t)dy < dh)
                        back[dy * pitch + dx] = pixel;
                }
            }
        }
    }

    shm_direct_virtio_scanout(back, pitch, dw, dh);

    extern void fb_flush_all(void);
    fb_flush_all();
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
