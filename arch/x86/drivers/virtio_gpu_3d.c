/*
 * virtio-gpu 3D driver -- Wave 1 skeleton.
 * Functions grow task-by-task. Until implemented, each returns -ENOSYS
 * and the corresponding selftest marker prints FAIL.
 */
#include "virtio_gpu_3d.h"
#include "virtio_gpu_internal.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);

#define ENOSYS 38
#define EINVAL 22
#define ENOMEM 12
#define ESRCH   3

/* -- Context table ---------------------------------------- */
#define VG3D_CTX_MAX 64

struct vg3d_ctx {
    uint32_t id;        /* 0 = free slot */
    uint32_t pid;       /* owning process */
    uint32_t flags;     /* GPU_CTX_* */
};

static struct vg3d_ctx g_ctx_tab[VG3D_CTX_MAX];
static uint32_t g_ctx_next_id = 1;

/* -- Resource table --------------------------------------- */
#define VG3D_RES_MAX 256

struct vg3d_res {
    uint32_t id;                /* 0 = free */
    uint32_t ctx_id;
    uint32_t pid;
    uint32_t kind;
    uint32_t flags;
    uint32_t format;
    uint64_t size;              /* byte size of backing */
    uint64_t backing_va;        /* kernel VA; user-mapped on demand */
    uint64_t backing_phys;      /* for future DMA iovec */
};

static struct vg3d_res g_res_tab[VG3D_RES_MAX];
static uint32_t g_res_next_id = 1;

extern void *mem_alloc_pages(uint64_t count);   /* 4 KiB pages */
extern void  mem_free_pages(void *addr, uint64_t count);

/* -- virtio-gpu 3D command header (matches 2D hdr layout) --- */
struct vg3d_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

/* Monotonic fence counter. Every SUBMIT allocates the next id; it is
 * signaled synchronously because vgpu_controlq_submit already waits on
 * the used-ring entry before returning. */
static uint64_t g_fence_next = 1;
static uint64_t g_fence_signaled = 0;

/* Called by vg3d_submit on successful return. */
static void vg3d_fence_signal(uint64_t fence) {
    if (fence > g_fence_signaled) g_fence_signaled = fence;
}

static bool g_3d_ready = false;

void virtio_gpu_3d_init(void) {
    if (!vgpu_is_initialized()) {
        serial_puts("[VG3D] skipped (2D driver not initialized)\n");
        g_3d_ready = false;
        return;
    }
    uint64_t feat = vgpu_device_features();
    if (!(feat & (1ull << VIRTIO_GPU_F_VIRGL))) {
        serial_puts("[VG3D] skipped (VIRGL feature not offered by host)\n");
        g_3d_ready = false;
        return;
    }
    serial_puts("[VG3D] ready -- VIRGL negotiated\n");
    g_3d_ready = true;
}

uint32_t vg3d_caps(void) {
    return g_3d_ready ? GPU_CAP_VENUS_READY : 0u;
}

int32_t vg3d_ctx_create(uint32_t pid, uint32_t flags) {
    if (!g_3d_ready) return -EINVAL;
    if (flags != GPU_CTX_VENUS && flags != GPU_CTX_NVK) return -EINVAL;
    for (int i = 0; i < VG3D_CTX_MAX; i++) {
        if (g_ctx_tab[i].id == 0) {
            g_ctx_tab[i].id = g_ctx_next_id++;
            g_ctx_tab[i].pid = pid;
            g_ctx_tab[i].flags = flags;
            return (int32_t)g_ctx_tab[i].id;
        }
    }
    return -ENOMEM;
}

int32_t vg3d_ctx_destroy(uint32_t pid, uint32_t ctx_id) {
    if (ctx_id == 0) return -EINVAL;
    for (int i = 0; i < VG3D_CTX_MAX; i++) {
        if (g_ctx_tab[i].id == ctx_id) {
            if (g_ctx_tab[i].pid != pid) return -EINVAL;
            g_ctx_tab[i].id = 0;
            g_ctx_tab[i].pid = 0;
            g_ctx_tab[i].flags = 0;
            return 0;
        }
    }
    return -ESRCH;
}
static struct vg3d_ctx *find_ctx(uint32_t pid, uint32_t ctx_id) {
    for (int i = 0; i < VG3D_CTX_MAX; i++)
        if (g_ctx_tab[i].id == ctx_id && g_ctx_tab[i].pid == pid)
            return &g_ctx_tab[i];
    return 0;
}

int32_t vg3d_res_create(uint32_t pid, uint32_t ctx_id,
                        const struct gpu_res_create_args *args) {
    if (!g_3d_ready || !args) return -EINVAL;
    if (!find_ctx(pid, ctx_id)) return -ESRCH;

    uint64_t sz;
    if (args->kind == GPU_RES_KIND_BUFFER) {
        sz = args->size;
    } else if (args->kind == GPU_RES_KIND_IMAGE2D) {
        uint32_t pitch = args->pitch ? args->pitch : args->width * 4;
        sz = (uint64_t)pitch * args->height;
    } else {
        return -EINVAL;
    }
    if (sz == 0 || sz > (256u << 20)) return -EINVAL;   /* clamp to 256 MiB */

    uint64_t pages = (sz + 4095) >> 12;
    void *backing = mem_alloc_pages(pages);
    if (!backing) return -ENOMEM;
    /* Zero the backing (musl-style cleared memory) */
    for (uint64_t i = 0; i < sz / 8; i++) ((volatile uint64_t *)backing)[i] = 0;

    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == 0) {
            g_res_tab[i].id = g_res_next_id++;
            g_res_tab[i].ctx_id = ctx_id;
            g_res_tab[i].pid = pid;
            g_res_tab[i].kind = args->kind;
            g_res_tab[i].flags = args->flags;
            g_res_tab[i].format = args->format;
            g_res_tab[i].size = sz;
            g_res_tab[i].backing_va = (uint64_t)backing;
            g_res_tab[i].backing_phys = VIRT_TO_PHYS((uint64_t)backing);
            return (int32_t)g_res_tab[i].id;
        }
    }
    mem_free_pages(backing, pages);
    return -ENOMEM;
}

uint64_t vg3d_res_map(uint32_t pid, uint32_t res_id) {
    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == res_id && g_res_tab[i].pid == pid) {
            /* Wave 1: return kernel VA directly. User VA mapping arrives
             * when per-process PML4 path is wired (Wave 1 runs kernel-only
             * selftest; userspace consumers come in Wave 2). */
            return g_res_tab[i].backing_va;
        }
    }
    return 0;
}
int32_t vg3d_submit(uint32_t pid, uint32_t ctx_id,
                    const void *cmd_bytes, uint64_t cmd_len,
                    uint64_t *out_fence) {
    if (!g_3d_ready || !cmd_bytes || !out_fence) return -EINVAL;
    if (cmd_len == 0 || cmd_len > (1u << 20)) return -EINVAL;  /* 1 MiB cap */
    if (!find_ctx(pid, ctx_id)) return -ESRCH;

    /* Build a SUBMIT_3D command. Layout:
     *   vg3d_ctrl_hdr (type=VIRTIO_GPU_CMD_SUBMIT_3D, fence_id, ctx_id)
     *   uint32_t size (cmd_len)
     *   uint32_t padding
     *   raw command bytes
     */
    uint64_t fence = g_fence_next++;
    uint32_t hdr_len = sizeof(struct vg3d_ctrl_hdr) + 8;
    uint32_t total = hdr_len + (uint32_t)cmd_len;
    uint64_t pages = (total + 4095) >> 12;
    uint8_t *buf = (uint8_t *)mem_alloc_pages(pages);
    if (!buf) return -ENOMEM;

    struct vg3d_ctrl_hdr *h = (struct vg3d_ctrl_hdr *)buf;
    h->type = VIRTIO_GPU_CMD_SUBMIT_3D;
    h->flags = 1;           /* VIRTIO_GPU_FLAG_FENCE */
    h->fence_id = fence;
    h->ctx_id = ctx_id;
    h->padding = 0;
    *(uint32_t *)(buf + sizeof(*h)) = (uint32_t)cmd_len;
    *(uint32_t *)(buf + sizeof(*h) + 4) = 0;
    /* Copy user bytes into kernel staging (byte-by-byte is fine; Wave 2
     * will replace with copy_from_user). */
    for (uint64_t i = 0; i < cmd_len; i++)
        buf[hdr_len + i] = ((const uint8_t *)cmd_bytes)[i];

    struct vg3d_ctrl_hdr resp;
    resp.type = 0; resp.flags = 0; resp.fence_id = 0;
    resp.ctx_id = 0; resp.padding = 0;
    int rc = vgpu_controlq_submit(buf, total, &resp, sizeof(resp));
    mem_free_pages(buf, pages);
    if (rc < 0) return -5;   /* -EIO */
    *out_fence = fence;
    vg3d_fence_signal(fence);
    return 0;
}
int32_t vg3d_fence_wait(uint64_t fence, uint64_t timeout_ns) {
    (void)timeout_ns;   /* polling is synchronous in Wave 1 */
    if (fence == 0 || fence >= g_fence_next) return -EINVAL;
    if (fence <= g_fence_signaled) return 0;
    return -110;        /* ETIMEDOUT -- shouldn't happen in Wave 1 */
}
extern void *shm_map(uint32_t handle);
extern void  compositor_signal_dirty(uint32_t window_id);
/* Our lookup: shm_handle == window_id in the current compositor impl.
 * If that changes, we'll need an explicit shm->window mapping. */

int32_t vg3d_present(uint32_t pid, uint32_t ctx_id,
                     uint32_t res_id, uint32_t shm_handle) {
    if (!g_3d_ready) return -EINVAL;
    if (!find_ctx(pid, ctx_id)) return -ESRCH;

    struct vg3d_res *r = 0;
    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == res_id && g_res_tab[i].pid == pid) {
            r = &g_res_tab[i]; break;
        }
    }
    if (!r) return -ESRCH;
    if (r->kind != GPU_RES_KIND_IMAGE2D) return -EINVAL;

    void *dst = shm_map(shm_handle);
    if (!dst) return -EINVAL;

    /* Wave 1: CPU memcpy. Wave 2 replaces with GPU-side
     * transfer_to_host_3d + blob alias when sizes match. */
    for (uint64_t i = 0; i < r->size / 8; i++)
        ((volatile uint64_t *)dst)[i] = ((volatile uint64_t *)r->backing_va)[i];

    compositor_signal_dirty(shm_handle);
    return 0;
}
void vg3d_cleanup_process(uint32_t pid) {
    for (int i = 0; i < VG3D_CTX_MAX; i++) {
        if (g_ctx_tab[i].id && g_ctx_tab[i].pid == pid) {
            g_ctx_tab[i].id = 0;
            g_ctx_tab[i].pid = 0;
            g_ctx_tab[i].flags = 0;
        }
    }
    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id && g_res_tab[i].pid == pid) {
            uint64_t pages = (g_res_tab[i].size + 4095) >> 12;
            mem_free_pages((void *)g_res_tab[i].backing_va, pages);
            g_res_tab[i].id = 0;
            g_res_tab[i].ctx_id = 0;
            g_res_tab[i].pid = 0;
            g_res_tab[i].size = 0;
            g_res_tab[i].backing_va = 0;
            g_res_tab[i].backing_phys = 0;
        }
    }
}

/* -- Self-test harness -------------------------------------- */

static void vg3d_t2_skeleton(void) {
    /* Task 2 passes if the skeleton links and this symbol is callable. */
    serial_puts("[VG3D-T2] skeleton OK\n");
}

static void vg3d_t3_virgl(void) {
    if (g_3d_ready) {
        serial_puts("[VG3D-T3] virgl-negotiated OK\n");
    } else {
        serial_puts("[VG3D-T3] virgl-negotiated FAIL (feature bit 0 not set)\n");
    }
}

static void vg3d_t4_ctx(void) {
    if (!g_3d_ready) {
        serial_puts("[VG3D-T4] ctx-create SKIP (3D not ready)\n");
        return;
    }
    int32_t id = vg3d_ctx_create(1, GPU_CTX_VENUS);
    if (id <= 0) {
        serial_puts("[VG3D-T4] ctx-create FAIL: id=");
        serial_putdec((uint32_t)-id);
        serial_puts("\n");
        return;
    }
    int32_t err = vg3d_ctx_destroy(1, (uint32_t)id);
    if (err < 0) {
        serial_puts("[VG3D-T4] ctx-destroy FAIL: err=");
        serial_putdec((uint32_t)-err);
        serial_puts("\n");
        return;
    }
    serial_puts("[VG3D-T4] ctx-lifecycle OK (id=");
    serial_putdec((uint32_t)id);
    serial_puts(")\n");
}

static void vg3d_t8_present(void) {
    if (!g_3d_ready) { serial_puts("[VG3D-T8] present SKIP\n"); return; }
    extern uint32_t shm_create_surface(uint32_t w, uint32_t h, uint32_t flags);
    uint32_t shm = shm_create_surface(64, 64, 0);
    if (!shm) { serial_puts("[VG3D-T8] present FAIL (shm)\n"); return; }

    int32_t cid = vg3d_ctx_create(1, GPU_CTX_VENUS);
    struct gpu_res_create_args args = {
        .kind = GPU_RES_KIND_IMAGE2D, .flags = GPU_RES_FLAG_HOST_COHERENT,
        .format = 0, .width = 64, .height = 64, .pitch = 64*4, .size = 0,
    };
    int32_t rid = vg3d_res_create(1, (uint32_t)cid, &args);
    if (rid <= 0) { serial_puts("[VG3D-T8] present FAIL (res)\n"); return; }

    int32_t err = vg3d_present(1, (uint32_t)cid, (uint32_t)rid, shm);
    if (err < 0) {
        serial_puts("[VG3D-T8] present FAIL err=");
        serial_putdec((uint32_t)-err);
        serial_puts("\n");
    } else {
        serial_puts("[VG3D-T8] present OK\n");
    }
    vg3d_ctx_destroy(1, (uint32_t)cid);
}

static void vg3d_t7_fence(void) {
    if (!g_3d_ready) { serial_puts("[VG3D-T7] fence SKIP\n"); return; }
    int32_t cid = vg3d_ctx_create(1, GPU_CTX_VENUS);
    uint8_t nop[16] = {0};
    uint64_t fence = 0;
    vg3d_submit(1, (uint32_t)cid, nop, sizeof(nop), &fence);
    /* Expect fence to be signaled within 1 second (since controlq submit
     * already waited for the used-ring entry, the fence is already
     * effectively retired -- wait should return 0 immediately). */
    int32_t err = vg3d_fence_wait(fence, 1000000000ull /* 1s */);
    if (err == 0) {
        serial_puts("[VG3D-T7] fence-wait OK\n");
    } else {
        serial_puts("[VG3D-T7] fence-wait FAIL err=");
        serial_putdec((uint32_t)-err);
        serial_puts("\n");
    }
    vg3d_ctx_destroy(1, (uint32_t)cid);
}

static void vg3d_t6_submit(void) {
    if (!g_3d_ready) { serial_puts("[VG3D-T6] submit SKIP\n"); return; }
    int32_t cid = vg3d_ctx_create(1, GPU_CTX_VENUS);
    if (cid <= 0) { serial_puts("[VG3D-T6] submit FAIL (ctx)\n"); return; }
    /* A venus "nop" is an empty command stream -- just enough to exercise
     * the virtqueue path. For Wave 1 we send 16 bytes of zeros and expect
     * the kernel to ACK with a fence id; the host either drops it or
     * sends a response ring entry. */
    uint8_t nop[16] = {0};
    uint64_t fence = 0;
    int32_t err = vg3d_submit(1, (uint32_t)cid, nop, sizeof(nop), &fence);
    if (err < 0) {
        serial_puts("[VG3D-T6] submit FAIL err=");
        serial_putdec((uint32_t)-err);
        serial_puts("\n");
    } else {
        serial_puts("[VG3D-T6] submit OK fence=");
        serial_putdec(fence);
        serial_puts("\n");
    }
    vg3d_ctx_destroy(1, (uint32_t)cid);
}

static void vg3d_t5_res(void) {
    if (!g_3d_ready) { serial_puts("[VG3D-T5] res SKIP\n"); return; }
    int32_t cid = vg3d_ctx_create(1, GPU_CTX_VENUS);
    if (cid <= 0) { serial_puts("[VG3D-T5] res FAIL (ctx create)\n"); return; }

    struct gpu_res_create_args args = {
        .kind = GPU_RES_KIND_BUFFER,
        .flags = GPU_RES_FLAG_HOST_COHERENT,
        .size = 4096,
    };
    int32_t rid = vg3d_res_create(1, (uint32_t)cid, &args);
    if (rid <= 0) {
        serial_puts("[VG3D-T5] res-create FAIL: err=");
        serial_putdec((uint32_t)-rid);
        serial_puts("\n");
        vg3d_ctx_destroy(1, (uint32_t)cid);
        return;
    }
    uint64_t va = vg3d_res_map(1, (uint32_t)rid);
    if (!va) {
        serial_puts("[VG3D-T5] res-map FAIL\n");
        vg3d_ctx_destroy(1, (uint32_t)cid);
        return;
    }
    *(volatile uint32_t *)va = 0xDEADBEEF;
    if (*(volatile uint32_t *)va != 0xDEADBEEF) {
        serial_puts("[VG3D-T5] res-map readback FAIL\n");
        vg3d_ctx_destroy(1, (uint32_t)cid);
        return;
    }
    serial_puts("[VG3D-T5] res-lifecycle OK (rid=");
    serial_putdec((uint32_t)rid);
    serial_puts(" va=0x");
    serial_puthex(va, 16);
    serial_puts(")\n");
    vg3d_ctx_destroy(1, (uint32_t)cid);
}

void virtio_gpu_3d_selftest(void) {
    serial_puts("[VG3D] selftest begin\n");
    vg3d_t2_skeleton();
    vg3d_t3_virgl();
    vg3d_t4_ctx();
    vg3d_t5_res();
    vg3d_t6_submit();
    vg3d_t7_fence();
    vg3d_t8_present();
    /* Later tasks append more markers here. */
    serial_puts("[VG3D] selftest end\n");
}
