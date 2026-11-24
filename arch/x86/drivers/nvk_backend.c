/*
 * nvk_backend.c — kernel-side bridge from SYS_GPU_* syscalls to the
 * GSP compute / copy-engine / GMMU primitives that drivers/gsp.c and
 * drivers/gmmu.c already provide and that drivers/gpu_inference.c
 * uses successfully for ML workloads.
 *
 * Surface (called from kernel/syscall.c):
 *   nvk_backend_ready                 — gate on GSP+RM+compute online
 *   nvk_backend_ctx_create            — register per-pid ctx in res table
 *   nvk_backend_ctx_destroy           — free the ctx + its resources
 *   nvk_backend_res_create            — allocate VRAM, register handle
 *   nvk_backend_res_map               — return CPU/GPU VA for the resource
 *   nvk_backend_submit                — walk cmd-buf opcodes, emit GPU work
 *   nvk_backend_fence_wait            — wait on a previously-emitted fence
 *   nvk_backend_present               — copy VRAM image to compositor SHM,
 *                                       trigger compositor flip
 *
 * Multi-process model: ALL submits funnel through the single shared
 * compute channel that gsp_compute_init() set up at boot. Per-pid
 * isolation lives in the resource table (drivers/nvk_backend_res.c) —
 * each pid owns its own VRAM allocations and fence counter, but the
 * channel is serialized. Multi-channel isolation is a follow-up; the
 * shared channel is enough to validate the end-to-end Vulkan path.
 */

#include "nvk_backend.h"
#include "gpu.h"
#include "../include/paging.h"
#include "../include/sys/gpu_syscalls.h"
#include "../include/sys/nvk_cmd.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int width);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* From nvk_backend_res.c */
extern int32_t nvk_res_table_ctx_create(uint32_t pid);
extern int32_t nvk_res_table_ctx_destroy(uint32_t pid);
extern int32_t nvk_res_table_create(uint32_t pid, const struct gpu_res_create_args *a);
extern int64_t nvk_res_table_map(uint32_t pid, uint32_t res_id);
extern uint64_t nvk_res_table_next_fence(uint32_t pid);

typedef struct nvk_resource {
    bool      used;
    uint32_t  pid;
    uint32_t  res_id;
    uint32_t  kind;
    uint32_t  flags;
    uint32_t  format;
    uint32_t  width, height, pitch;
    uint64_t  size;
    uint64_t  vram_addr;
    void     *kvirt;
} nvk_resource_t;

extern nvk_resource_t *nvk_res_lookup(uint32_t pid, uint32_t res_id);

/* Per-pid in-flight fence map. We use the fence value as the semaphore
 * payload directly, sharing one host-visible semaphore page across all
 * processes. The shared sem is allocated lazily on the first submit. */
static volatile uint32_t *shared_sem;     /* 4 KB page, sem at offset 0 */
static uint64_t           shared_sem_phys;
static uint64_t           last_signaled;  /* monotonic, written by GPU */

static int nvk_backend_init_sem(void)
{
    if (shared_sem) return 0;
    void *p = mem_alloc_aligned(4096, 4096);
    if (!p) return -12;
    shared_sem      = (volatile uint32_t *)PHYS_TO_VIRT(p);
    shared_sem_phys = (uint64_t)(uintptr_t)p;
    *shared_sem     = 0;
    last_signaled   = 0;
    serial_puts("[NVK] semaphore page at phys=0x");
    serial_puthex(shared_sem_phys, 16);
    serial_puts("\n");
    return 0;
}

/* ── Public: readiness gate ──────────────────────────────────── */

bool nvk_backend_ready(void)
{
    gsp_state_t *g = gsp_get_state();
    if (!g) return false;
    if (!g->rm_init_done) return false;
    if (gsp_get_compute() == NULL) return false;
    return true;
}

/* ── Public: context lifecycle ───────────────────────────────── */

int32_t nvk_backend_ctx_create(uint32_t pid, uint32_t flags)
{
    (void)flags;
    if (!nvk_backend_ready()) return -38;  /* ENOSYS */
    if (nvk_backend_init_sem() < 0) return -12;
    int32_t rc = nvk_res_table_ctx_create(pid);
    if (rc < 0) return rc;
    serial_puts("[NVK] ctx_create pid=");
    serial_putdec(pid);
    serial_puts(" ctx=");
    serial_putdec((uint64_t)rc);
    serial_puts("\n");
    return rc;
}

int32_t nvk_backend_ctx_destroy(uint32_t pid, uint32_t ctx_id)
{
    (void)ctx_id;
    return nvk_res_table_ctx_destroy(pid);
}

void nvk_backend_init_hook(void)
{
    if (nvk_backend_ready()) {
        serial_puts("[NVK] backend READY (GSP+RM+compute online)\n");
        extern void fb_puts(const char *s);
        fb_puts(" [NVK] backend ready\n");
    } else {
        gsp_state_t *g = gsp_get_state();
        serial_puts("[NVK] backend NOT ready: ");
        if (!g)                    serial_puts("no gsp_state");
        else if (!g->rm_init_done) serial_puts("RM init incomplete");
        else                       serial_puts("no compute channel");
        serial_puts("\n");
        extern void fb_puts(const char *s);
        fb_puts(" [NVK] not ready (GPU compute path uninitialized)\n");
    }
}

/* ── Public: resource lifecycle ──────────────────────────────── */

int32_t nvk_backend_res_create(uint32_t pid, uint32_t ctx_id,
                                const struct gpu_res_create_args *a)
{
    (void)ctx_id;
    if (!nvk_backend_ready()) return -38;
    return nvk_res_table_create(pid, a);
}

int64_t nvk_backend_res_map(uint32_t pid, uint32_t res_id)
{
    return nvk_res_table_map(pid, res_id);
}

/* ── Submit: walk cmd-buf opcodes ────────────────────────────── */

/*
 * Fill a VRAM region with a 32-bit pattern using kernel-side stores on
 * the identity-mapped CPU view. The GMMU identity-maps this VRAM region
 * with VOL=0 (GPU-cached), but in practice the same approach works in
 * gpu_inference.c for weight uploads — CPU writes via PCIe BAR1 settle
 * in physical VRAM and the GPU L2 sees them on next compute access.
 *
 * For correctness with strict GPU L2 semantics we'd issue a SASS clear
 * kernel + L2 invalidate; that's a follow-up. For first-paint clear of
 * a frame this is sufficient.
 */
static void vram_fill32(void *dst_kvirt, uint32_t value, uint64_t bytes)
{
    uint32_t *p   = (uint32_t *)dst_kvirt;
    uint64_t  cnt = bytes / 4;
    for (uint64_t i = 0; i < cnt; i++) p[i] = value;
}

static int do_clear_color_image(uint32_t pid, const struct nvk_cmd_clear_color *c)
{
    nvk_resource_t *r = nvk_res_lookup(pid, c->res_id);
    if (!r) {
        serial_puts("[NVK] CLEAR_COLOR_IMAGE: bad res_id\n");
        return -2;
    }
    uint64_t bytes = (uint64_t)c->width * c->height * 4;
    if (bytes > r->size) bytes = r->size;
    vram_fill32(r->kvirt, c->color_rgba, bytes);
    return 0;
}

static int do_copy_image_to_buffer(uint32_t pid, const struct nvk_cmd_copy_i2b *c)
{
    nvk_resource_t *src = nvk_res_lookup(pid, c->src_res_id);
    nvk_resource_t *dst = nvk_res_lookup(pid, c->dst_res_id);
    if (!src || !dst) return -2;
    uint64_t bytes = (uint64_t)c->width * c->height * 4;
    if (bytes > src->size) bytes = src->size;
    if (bytes > dst->size) bytes = dst->size;
    /* Both src and dst are in identity-mapped VRAM. memcpy via the
     * PHYS_TO_VIRT alias is fine. CE-engine path lands in a follow-up. */
    memcpy(dst->kvirt, src->kvirt, bytes);
    return 0;
}

static int do_copy_buffer(uint32_t pid, const struct nvk_cmd_copy_buffer *c)
{
    nvk_resource_t *src = nvk_res_lookup(pid, c->src_res_id);
    nvk_resource_t *dst = nvk_res_lookup(pid, c->dst_res_id);
    if (!src || !dst) return -2;
    uint64_t bytes = c->size;
    if (c->src_offset + bytes > src->size) return -22;
    if (c->dst_offset + bytes > dst->size) return -22;
    memcpy((uint8_t *)dst->kvirt + c->dst_offset,
           (uint8_t *)src->kvirt + c->src_offset, bytes);
    return 0;
}

static int do_fill_buffer(uint32_t pid, const struct nvk_cmd_fill_buffer *c)
{
    nvk_resource_t *r = nvk_res_lookup(pid, c->res_id);
    if (!r) return -2;
    if (c->offset + c->size > r->size) return -22;
    vram_fill32((uint8_t *)r->kvirt + c->offset, c->value, c->size);
    return 0;
}

int32_t nvk_backend_submit(uint32_t pid, uint32_t ctx_id,
                            const uint8_t *cmd_bytes, uint32_t cmd_len,
                            uint64_t *out_fence)
{
    (void)ctx_id;
    if (!nvk_backend_ready()) return -38;
    if (!cmd_bytes || cmd_len < 4) return -22;

    /* Walk opcode stream. Each entry begins with uint32_t op; payload
     * size is determined by op type. Bail out on unknown or malformed
     * entries so a bad command list can't desync subsequent processing. */
    uint32_t pos = 0;
    while (pos + sizeof(uint32_t) <= cmd_len) {
        uint32_t op = *(const uint32_t *)(cmd_bytes + pos);
        int rc = 0;

        switch (op) {
        case NVK_CMD_CLEAR_COLOR_IMAGE: {
            if (pos + sizeof(struct nvk_cmd_clear_color) > cmd_len) goto bad;
            const struct nvk_cmd_clear_color *c =
                (const struct nvk_cmd_clear_color *)(cmd_bytes + pos);
            rc = do_clear_color_image(pid, c);
            pos += sizeof(*c);
            break;
        }
        case NVK_CMD_COPY_IMAGE_TO_BUFFER: {
            if (pos + sizeof(struct nvk_cmd_copy_i2b) > cmd_len) goto bad;
            const struct nvk_cmd_copy_i2b *c =
                (const struct nvk_cmd_copy_i2b *)(cmd_bytes + pos);
            rc = do_copy_image_to_buffer(pid, c);
            pos += sizeof(*c);
            break;
        }
        case NVK_CMD_COPY_BUFFER: {
            if (pos + sizeof(struct nvk_cmd_copy_buffer) > cmd_len) goto bad;
            const struct nvk_cmd_copy_buffer *c =
                (const struct nvk_cmd_copy_buffer *)(cmd_bytes + pos);
            rc = do_copy_buffer(pid, c);
            pos += sizeof(*c);
            break;
        }
        case NVK_CMD_FILL_BUFFER: {
            if (pos + sizeof(struct nvk_cmd_fill_buffer) > cmd_len) goto bad;
            const struct nvk_cmd_fill_buffer *c =
                (const struct nvk_cmd_fill_buffer *)(cmd_bytes + pos);
            rc = do_fill_buffer(pid, c);
            pos += sizeof(*c);
            break;
        }
        default:
            serial_puts("[NVK] submit: unknown opcode 0x");
            serial_puthex(op, 8);
            serial_puts(" at pos=");
            serial_putdec(pos);
            serial_puts("\n");
            goto bad;
        }

        if (rc < 0) {
            serial_puts("[NVK] submit: command failed at pos=");
            serial_putdec(pos);
            serial_puts(" rc=");
            serial_putdec((uint64_t)(-rc));
            serial_puts("\n");
            return rc;
        }
    }

    /* Allocate a fence value for this submit and signal it immediately.
     * Since our current submit path is synchronous (CPU memcpy/memset
     * runs to completion inside this syscall), we mark the fence as
     * already-signaled — fence_wait then returns immediately. When the
     * submit chain becomes asynchronous (real GPU compute dispatch),
     * the fence signal moves to the GPU semaphore write callback. */
    uint64_t fence = nvk_res_table_next_fence(pid);
    if (out_fence) *out_fence = fence;
    if (fence > last_signaled) last_signaled = fence;
    return 0;

bad:
    serial_puts("[NVK] submit: malformed command stream\n");
    return -22;
}

int32_t nvk_backend_fence_wait(uint64_t fence, uint64_t timeout_ns)
{
    (void)timeout_ns;
    /* Synchronous submit path means by the time the syscall returns,
     * the work has completed; nothing to wait for. Real timeout matters
     * only when we have async GPU work in flight. */
    if (fence <= last_signaled) return 0;
    return 0;
}

/* ── Present: VRAM image → compositor SHM surface ──────────────
 *
 * SYS_GPU_PRESENT hands us a resource id and an SHM handle from
 * SYS_SHM_MKSURFACE (kernel/shm.c). We map the SHM, do a CPU-side
 * memcpy (BGRA8888 round-trip), and trigger the compositor's surface
 * flip. CE-engine offload is a follow-up.
 */
extern void *shm_map(uint32_t handle);
extern void  shm_flush_surface(uint32_t handle);

int32_t nvk_backend_present(uint32_t pid, uint32_t ctx_id,
                             uint32_t res_id, uint32_t shm_handle)
{
    (void)ctx_id;
    nvk_resource_t *r = nvk_res_lookup(pid, res_id);
    if (!r) return -2;

    void *shm = shm_map(shm_handle);
    if (!shm) return -2;

    /* Image bounds came from vkCreateImage; assume the SHM surface is
     * at least width*height*4 bytes (compositor sized it that way via
     * SYS_SHM_MKSURFACE). */
    uint64_t bytes = (uint64_t)r->width * r->height * 4;
    if (bytes == 0)            bytes = r->size;
    if (bytes > r->size)       bytes = r->size;
    memcpy(shm, r->kvirt, bytes);
    shm_flush_surface(shm_handle);
    return 0;
}
