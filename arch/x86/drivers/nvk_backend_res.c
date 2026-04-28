/*
 * nvk_backend_res.c — per-process resource table for the kernel NVK
 * Vulkan backend.
 *
 * The userland Vulkan ICD (lib/vulkan/icd/nvk_stub) issues SYS_GPU_*
 * syscalls. The kernel needs to track:
 *   - which GPU memory each (pid, res_id) owns,
 *   - the geometry / format for image resources (so submit can issue
 *     the right CE / compute work),
 *   - the user virtual address each resource is mapped at (so MapMemory
 *     returns the same VA on subsequent calls and Unmap can find it),
 *   - the live fence values issued for each pid so a stale fence wait
 *     doesn't block forever after the process exits.
 *
 * Storage is a fixed-size table (no kmalloc on the hot path). Per-pid
 * counters give monotonically-increasing res_ids per process so the
 * userland ICD can use them as opaque handles.
 *
 * Cleanup hook gets called from proc_exit (registered in main.c) so
 * VRAM and the resource slots are released even if the process never
 * called vkDestroy*. Without this a crashing app would leak the entire
 * NVK VRAM region.
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "gpu.h"
#include "nvk_backend.h"
#include "../include/sys/gpu_syscalls.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int width);
extern void *memset(void *, int, unsigned long);

/* User-VA mapping helpers (process-local PML4 mapping). For the first
 * pass we ignore the user VA and just give the process the kernel VA
 * (PHYS_TO_VIRT of the GPU buffer); the upper-half is shared across
 * all process address spaces via PML4[256] so userland can dereference
 * it. Real vkMapMemory semantics — distinct user VA per process —
 * lands in a follow-up.  */

#define NVK_MAX_RESOURCES 256       /* total across all processes */
#define NVK_MAX_PIDS      64        /* concurrent contexts */

typedef struct {
    bool      used;
    uint32_t  pid;                  /* owning process */
    uint32_t  res_id;               /* opaque handle returned to userland */
    uint32_t  kind;                 /* GPU_RES_KIND_* */
    uint32_t  flags;                /* GPU_RES_FLAG_* */
    uint32_t  format;
    uint32_t  width, height, pitch;
    uint64_t  size;                 /* bytes (rounded to 4 KB pages) */
    uint64_t  vram_addr;            /* GPU VA == phys (identity mapped) */
    void     *kvirt;                /* PHYS_TO_VIRT(vram_addr) for kernel access */
} nvk_resource_t;

typedef struct {
    bool     used;
    uint32_t pid;
    uint32_t next_res_id;           /* monotonic per-pid handle counter */
    uint64_t fence_high_water;      /* highest fence value issued for this pid */
} nvk_pid_ctx_t;

static nvk_resource_t nvk_res_table[NVK_MAX_RESOURCES];
static nvk_pid_ctx_t  nvk_pid_table[NVK_MAX_PIDS];

/* ── Per-pid context table ───────────────────────────────────── */

static nvk_pid_ctx_t *pid_ctx_lookup(uint32_t pid)
{
    for (int i = 0; i < NVK_MAX_PIDS; i++)
        if (nvk_pid_table[i].used && nvk_pid_table[i].pid == pid)
            return &nvk_pid_table[i];
    return NULL;
}

static nvk_pid_ctx_t *pid_ctx_alloc(uint32_t pid)
{
    /* If already exists, return it (idempotent ctx_create is allowed). */
    nvk_pid_ctx_t *e = pid_ctx_lookup(pid);
    if (e) return e;

    for (int i = 0; i < NVK_MAX_PIDS; i++) {
        if (!nvk_pid_table[i].used) {
            nvk_pid_table[i].used = true;
            nvk_pid_table[i].pid  = pid;
            nvk_pid_table[i].next_res_id = 1;
            nvk_pid_table[i].fence_high_water = 0;
            return &nvk_pid_table[i];
        }
    }
    return NULL;
}

/* ── Resource table helpers ──────────────────────────────────── */

nvk_resource_t *nvk_res_lookup(uint32_t pid, uint32_t res_id)
{
    for (int i = 0; i < NVK_MAX_RESOURCES; i++)
        if (nvk_res_table[i].used && nvk_res_table[i].pid == pid &&
            nvk_res_table[i].res_id == res_id)
            return &nvk_res_table[i];
    return NULL;
}

static nvk_resource_t *nvk_res_alloc_slot(void)
{
    for (int i = 0; i < NVK_MAX_RESOURCES; i++)
        if (!nvk_res_table[i].used)
            return &nvk_res_table[i];
    return NULL;
}

/* ── Public API used by nvk_backend.c ────────────────────────── */

int32_t nvk_res_table_ctx_create(uint32_t pid)
{
    nvk_pid_ctx_t *e = pid_ctx_alloc(pid);
    if (!e) {
        serial_puts("[NVK-RES] ctx table full\n");
        return -12;  /* ENOMEM */
    }
    /* Use pid as the ctx_id token — caller-side it's opaque, and we
     * always look up by pid in subsequent calls anyway. */
    return (int32_t)pid;
}

int32_t nvk_res_table_ctx_destroy(uint32_t pid)
{
    /* Free every resource owned by this pid. */
    uint32_t freed = 0;
    for (int i = 0; i < NVK_MAX_RESOURCES; i++) {
        if (nvk_res_table[i].used && nvk_res_table[i].pid == pid) {
            if (nvk_res_table[i].vram_addr)
                gmmu_free_vram(nvk_res_table[i].vram_addr,
                               nvk_res_table[i].size);
            memset(&nvk_res_table[i], 0, sizeof(nvk_res_table[i]));
            freed++;
        }
    }
    nvk_pid_ctx_t *e = pid_ctx_lookup(pid);
    if (e) {
        serial_puts("[NVK-RES] ctx_destroy pid=");
        serial_putdec(pid);
        serial_puts(" freed ");
        serial_putdec(freed);
        serial_puts(" resource(s)\n");
        memset(e, 0, sizeof(*e));
    }
    return 0;
}

/*
 * Allocate VRAM for a resource and register it in the per-pid table.
 * Returns res_id (>0) on success, -errno on failure.
 */
int32_t nvk_res_table_create(uint32_t pid, const struct gpu_res_create_args *a)
{
    if (!a) return -22;  /* EINVAL */

    nvk_pid_ctx_t *ctx = pid_ctx_lookup(pid);
    if (!ctx) {
        /* Auto-create — the userland ICD's vkCreateInstance might bypass
         * vkCreateDevice for some queries; safer to provision on demand. */
        ctx = pid_ctx_alloc(pid);
        if (!ctx) return -12;
    }

    /* Compute byte size — for IMAGE2D assume 4 BPP (BGRA8888 / RGBA8888)
     * unless the caller specified a pitch. */
    uint64_t size = a->size;
    if (a->kind == GPU_RES_KIND_IMAGE2D) {
        uint32_t pitch = a->pitch ? a->pitch : (a->width * 4);
        size = (uint64_t)pitch * a->height;
    }
    if (size == 0) return -22;

    /* Round up to 4 KB so the bitmap allocator's per-page tracking stays
     * exact when we later free. */
    size = (size + 4095) & ~(uint64_t)4095;

    uint64_t vram = gmmu_alloc_vram(size);
    if (!vram) {
        serial_puts("[NVK-RES] gmmu_alloc_vram failed for size=");
        serial_putdec(size);
        serial_puts("\n");
        return -12;
    }

    nvk_resource_t *r = nvk_res_alloc_slot();
    if (!r) {
        gmmu_free_vram(vram, size);
        return -12;
    }

    memset(r, 0, sizeof(*r));
    r->used      = true;
    r->pid       = pid;
    r->res_id    = ctx->next_res_id++;
    r->kind      = a->kind;
    r->flags     = a->flags;
    r->format    = a->format;
    r->width     = a->width;
    r->height    = a->height;
    r->pitch     = a->pitch ? a->pitch : (a->width * 4);
    r->size      = size;
    r->vram_addr = vram;
    r->kvirt     = PHYS_TO_VIRT(vram);  /* upper-half view for kernel ops */

    /* Zero the VRAM so reads from a fresh resource don't return stale
     * GPU residue. The CPU-side write is visible to the GPU because the
     * page is identity-mapped and L2 will fault it in on next compute
     * read. (For HOST_COHERENT the CPU is the producer anyway.) */
    memset(r->kvirt, 0, size);

    return (int32_t)r->res_id;
}

/*
 * Map a resource's VRAM into the user's address space.
 *
 * First-pass: just hand back the upper-half kernel VA. PML4[256] is
 * shared across all process page tables (Phase C invariant), so the
 * user can dereference it. Real per-process user-VA mapping with
 * paging_map_user is a follow-up — needs allocating user VA from the
 * process VMA list and inserting PTEs into the process PML4.
 */
int64_t nvk_res_table_map(uint32_t pid, uint32_t res_id)
{
    nvk_resource_t *r = nvk_res_lookup(pid, res_id);
    if (!r) return -2;  /* ENOENT */
    return (int64_t)(uintptr_t)r->kvirt;
}

/* Track and return the next fence value for this pid's submits. */
uint64_t nvk_res_table_next_fence(uint32_t pid)
{
    nvk_pid_ctx_t *ctx = pid_ctx_lookup(pid);
    if (!ctx) {
        /* Provision lazily so a misordered SUBMIT before CTX_CREATE
         * still gets a valid fence (at the cost of an implicit ctx). */
        ctx = pid_ctx_alloc(pid);
        if (!ctx) return 0;
    }
    return ++ctx->fence_high_water;
}

/* Cleanup hook: kernel/process.c calls this in proc_exit so resources
 * are released even when the process didn't call vkDestroy*. */
void nvk_res_table_proc_exit(uint32_t pid)
{
    nvk_res_table_ctx_destroy(pid);
}
