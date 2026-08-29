/* virtio-gpu Venus transport for OsitoK. */
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
#define EIO     5
#define EPERM   1

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
    uint64_t size;
    uint64_t backing_va;
    uint32_t host_resource_id;
};

static struct vg3d_res g_res_tab[VG3D_RES_MAX];

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

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100
#define VIRTIO_GPU_CAPSET_VENUS   4

struct vg3d_ctx_create_cmd {
    struct vg3d_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed));

struct vg3d_ctx_destroy_cmd {
    struct vg3d_ctrl_hdr hdr;
} __attribute__((packed));

struct virgl_renderer_capset_venus {
    uint32_t wire_format_version;
    uint32_t vk_xml_version;
    uint32_t vk_ext_command_serialization_spec_version;
    uint32_t vk_mesa_venus_protocol_spec_version;
    uint32_t supports_blob_id_0;
    uint32_t vk_extension_mask1[32];
    uint32_t allow_vk_wait_syncs;
    uint32_t supports_multiple_timelines;
    uint32_t use_guest_vram;
};

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
static struct virgl_renderer_capset_venus g_venus_capset;

#define VN_CMD_ENUMERATE_INSTANCE_VERSION 137u
#define VN_CMD_SET_REPLY_STREAM           178u
#define VN_CMD_CREATE_RING                188u
#define VN_CMD_DESTROY_RING               189u
#define VN_CMD_NOTIFY_RING                190u
#define VN_COMMAND_GENERATE_REPLY         1u
#define VN_STRUCTURE_TYPE_RING_CREATE_INFO 1000384000u

static void vn_put_u32(uint8_t **dst, uint32_t value) {
    __builtin_memcpy(*dst, &value, sizeof(value));
    *dst += sizeof(value);
}

static void vn_put_u64(uint8_t **dst, uint64_t value) {
    __builtin_memcpy(*dst, &value, sizeof(value));
    *dst += sizeof(value);
}

static int vg3d_host_submit(uint32_t ctx_id, const void *cmd_bytes,
                            uint32_t cmd_len) {
    uint32_t hdr_len = sizeof(struct vg3d_ctrl_hdr) + 8;
    uint32_t total = hdr_len + cmd_len;
    uint64_t pages = (total + 4095) >> 12;
    void *buf_phys = mem_alloc_pages(pages);
    if (!buf_phys)
        return -ENOMEM;
    uint8_t *buf = (uint8_t *)PHYS_TO_VIRT((uint64_t)buf_phys);
    __builtin_memset(buf, 0, total);

    struct vg3d_ctrl_hdr *hdr = (struct vg3d_ctrl_hdr *)buf;
    hdr->type = VIRTIO_GPU_CMD_SUBMIT_3D;
    hdr->ctx_id = ctx_id;
    *(uint32_t *)(buf + sizeof(*hdr)) = cmd_len;
    __builtin_memcpy(buf + hdr_len, cmd_bytes, cmd_len);

    struct vg3d_ctrl_hdr resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    int rc = vgpu_controlq_submit(buf, total, &resp, sizeof(resp));
    mem_free_pages(buf_phys, pages);
    return rc == 0 && resp.type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -EIO;
}

/* Exercise the official Mesa Venus transport rather than a private packet
 * format. The renderer creates a real shared ring, invokes its host Vulkan
 * vkEnumerateInstanceVersion entry point and writes the generated reply into
 * a second host-owned blob. */
static int vg3d_probe_venus_protocol(uint32_t ctx_id) {
    const uint32_t ring_size = 4096;
    const uint32_t head_offset = 0;
    const uint32_t tail_offset = 64;
    const uint32_t status_offset = 128;
    const uint32_t buffer_offset = 192;
    const uint32_t buffer_size = 2048;
    const uint32_t extra_offset = buffer_offset + buffer_size;
    const uint64_t ring_id = 0x4f5349544f564b31ull;
    uint32_t ring_res = 0;
    uint32_t reply_res = 0;
    void *ring_map = 0;
    void *reply_map = 0;
    int result = -EIO;

    if (vgpu_blob_create_map(ctx_id, ring_size, 0,
                             &ring_res, &ring_map) < 0 ||
        vgpu_blob_create_map(ctx_id, 4096, 0,
                             &reply_res, &reply_map) < 0)
        goto cleanup;
    __builtin_memset(ring_map, 0, ring_size);
    __builtin_memset(reply_map, 0, 4096);

    uint8_t create[160];
    uint8_t *p = create;
    vn_put_u32(&p, VN_CMD_CREATE_RING);
    vn_put_u32(&p, 0);
    vn_put_u64(&p, ring_id);
    vn_put_u64(&p, 1); /* pCreateInfo */
    vn_put_u32(&p, VN_STRUCTURE_TYPE_RING_CREATE_INFO);
    vn_put_u64(&p, 0); /* pNext */
    vn_put_u32(&p, 0); /* flags */
    vn_put_u32(&p, ring_res);
    vn_put_u64(&p, 0); /* resource offset */
    vn_put_u64(&p, ring_size);
    vn_put_u64(&p, 1000000); /* idle timeout, ns */
    vn_put_u64(&p, head_offset);
    vn_put_u64(&p, tail_offset);
    vn_put_u64(&p, status_offset);
    vn_put_u64(&p, buffer_offset);
    vn_put_u64(&p, buffer_size);
    vn_put_u64(&p, extra_offset);
    vn_put_u64(&p, 0); /* extra size */
    if (vg3d_host_submit(ctx_id, create, (uint32_t)(p - create)) < 0)
        goto cleanup;

    uint8_t *ring_cmd = (uint8_t *)ring_map + buffer_offset;
    p = ring_cmd;
    vn_put_u32(&p, VN_CMD_SET_REPLY_STREAM);
    vn_put_u32(&p, 0);
    vn_put_u64(&p, 1); /* pStream */
    vn_put_u32(&p, reply_res);
    vn_put_u64(&p, 0); /* reply offset */
    vn_put_u64(&p, 64); /* reply size */
    vn_put_u32(&p, VN_CMD_ENUMERATE_INSTANCE_VERSION);
    vn_put_u32(&p, VN_COMMAND_GENERATE_REPLY);
    vn_put_u64(&p, 1); /* pApiVersion */
    uint32_t command_size = (uint32_t)(p - ring_cmd);

    __asm__ volatile ("mfence" ::: "memory");
    *(volatile uint32_t *)((uint8_t *)ring_map + tail_offset) = command_size;
    __asm__ volatile ("mfence" ::: "memory");

    uint8_t notify[32];
    p = notify;
    vn_put_u32(&p, VN_CMD_NOTIFY_RING);
    vn_put_u32(&p, 0);
    vn_put_u64(&p, ring_id);
    vn_put_u32(&p, command_size);
    vn_put_u32(&p, 0);
    if (vg3d_host_submit(ctx_id, notify, (uint32_t)(p - notify)) < 0)
        goto destroy_ring;

    bool consumed = false;
    for (uint32_t attempt = 0; attempt < 100000000; attempt++) {
        __asm__ volatile ("lfence" ::: "memory");
        if (*(volatile uint32_t *)((uint8_t *)ring_map + head_offset) ==
            command_size) {
            consumed = true;
            break;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    if (!consumed)
        goto destroy_ring;

    const volatile uint8_t *reply = (const volatile uint8_t *)reply_map;
    uint32_t reply_cmd;
    int32_t reply_result;
    uint64_t reply_pointer;
    uint32_t api_version;
    __builtin_memcpy(&reply_cmd, (const void *)(reply + 0), 4);
    __builtin_memcpy(&reply_result, (const void *)(reply + 4), 4);
    __builtin_memcpy(&reply_pointer, (const void *)(reply + 8), 8);
    __builtin_memcpy(&api_version, (const void *)(reply + 16), 4);
    if (reply_cmd != VN_CMD_ENUMERATE_INSTANCE_VERSION ||
        reply_result != 0 || reply_pointer != 1 || api_version == 0)
        goto destroy_ring;

    serial_puts("[VG3D] official Venus protocol host Vulkan version=0x");
    serial_puthex(api_version, 8);
    serial_puts("\n");
    result = 0;

destroy_ring: {
    uint8_t destroy[16];
    p = destroy;
    vn_put_u32(&p, VN_CMD_DESTROY_RING);
    vn_put_u32(&p, 0);
    vn_put_u64(&p, ring_id);
    if (vg3d_host_submit(ctx_id, destroy, sizeof(destroy)) < 0)
        result = -EIO;
}
cleanup:
    if (reply_res && vgpu_blob_destroy(reply_res) < 0)
        result = -EIO;
    if (ring_res && vgpu_blob_destroy(ring_res) < 0)
        result = -EIO;
    return result;
}

static int vg3d_host_ctx_create(uint32_t ctx_id) {
    struct vg3d_ctx_create_cmd cmd;
    struct vg3d_ctrl_hdr resp;
    static const char name[] = "ositok-venus";
    __builtin_memset(&cmd, 0, sizeof(cmd));
    __builtin_memset(&resp, 0, sizeof(resp));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.nlen = sizeof(name) - 1;
    cmd.context_init = VIRTIO_GPU_CAPSET_VENUS;
    for (uint32_t i = 0; i < sizeof(name) - 1; i++)
        cmd.debug_name[i] = name[i];
    if (vgpu_controlq_submit(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -EIO;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -EIO;
}

static int vg3d_host_ctx_destroy(uint32_t ctx_id) {
    struct vg3d_ctx_destroy_cmd cmd;
    struct vg3d_ctrl_hdr resp;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    __builtin_memset(&resp, 0, sizeof(resp));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;
    if (vgpu_controlq_submit(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return -EIO;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -EIO;
}

void virtio_gpu_3d_init(void) {
    if (!vgpu_is_initialized()) {
        serial_puts("[VG3D] skipped (2D driver not initialized)\n");
        g_3d_ready = false;
        return;
    }
    if (!vgpu_has_feature(VIRTIO_GPU_F_VIRGL) ||
        !vgpu_has_feature(VIRTIO_GPU_F_RESOURCE_BLOB) ||
        !vgpu_has_feature(VIRTIO_GPU_F_CONTEXT_INIT)) {
        serial_puts("[VG3D] skipped (Venus virtio features incomplete)\n");
        g_3d_ready = false;
        return;
    }

    __builtin_memset(&g_venus_capset, 0, sizeof(g_venus_capset));
    int capset_size = vgpu_get_capset(VIRTIO_GPU_CAPSET_VENUS,
                                      &g_venus_capset,
                                      sizeof(g_venus_capset));
    if (capset_size < 20 || g_venus_capset.wire_format_version == 0 ||
        !g_venus_capset.supports_blob_id_0) {
        serial_puts("[VG3D] skipped (valid Venus capset unavailable)\n");
        g_3d_ready = false;
        return;
    }

    uint32_t probe_ctx = g_ctx_next_id++;
    if (vg3d_host_ctx_create(probe_ctx) < 0) {
        serial_puts("[VG3D] skipped (host Venus context rejected)\n");
        g_3d_ready = false;
        return;
    }

    int protocol_rc = vg3d_probe_venus_protocol(probe_ctx);
    int destroy_rc = vg3d_host_ctx_destroy(probe_ctx);
    if (protocol_rc < 0 || destroy_rc < 0) {
        serial_puts("[VG3D] skipped (official Venus protocol probe failed)\n");
        g_3d_ready = false;
        return;
    }

    serial_puts("[VG3D] ready -- official Venus transport validated, wire=");
    serial_putdec(g_venus_capset.wire_format_version);
    serial_puts("\n");
    g_3d_ready = true;
}

uint32_t vg3d_caps(void) {
    return g_3d_ready ? GPU_CAP_VENUS_READY : 0u;
}

int32_t vg3d_ctx_create(uint32_t pid, uint32_t flags) {
    if (!g_3d_ready) return -EINVAL;
    if (flags != GPU_CTX_VENUS) return -EINVAL;
    for (int i = 0; i < VG3D_CTX_MAX; i++) {
        if (g_ctx_tab[i].id == 0) {
            uint32_t id = g_ctx_next_id++;
            if (vg3d_host_ctx_create(id) < 0)
                return -EIO;
            g_ctx_tab[i].id = id;
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
            for (int r = 0; r < VG3D_RES_MAX; r++) {
                if (g_res_tab[r].id && g_res_tab[r].pid == pid &&
                    g_res_tab[r].ctx_id == ctx_id) {
                    if (vgpu_blob_destroy(g_res_tab[r].host_resource_id) < 0)
                        return -EIO;
                    __builtin_memset(&g_res_tab[r], 0, sizeof(g_res_tab[r]));
                }
            }
            if (vg3d_host_ctx_destroy(ctx_id) < 0) return -EIO;
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

    if (args->kind != GPU_RES_KIND_BUFFER)
        return -EINVAL;
    uint64_t sz = args->size;
    if (sz == 0 || sz > (256u << 20)) return -EINVAL;   /* clamp to 256 MiB */

    uint32_t host_resource_id = 0;
    void *backing = 0;
    if (vgpu_blob_create_map(ctx_id, sz, args->blob_id,
                             &host_resource_id, &backing) < 0)
        return -EIO;
    __builtin_memset(backing, 0, sz);

    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == 0) {
            /* Venus ring and reply-stream commands consume the renderer's
             * virtio resource ID directly.  Keep that ID as the syscall
             * handle instead of hiding it behind a second guest-only ID. */
            g_res_tab[i].id = host_resource_id;
            g_res_tab[i].ctx_id = ctx_id;
            g_res_tab[i].pid = pid;
            g_res_tab[i].kind = args->kind;
            g_res_tab[i].flags = args->flags;
            g_res_tab[i].format = args->format;
            g_res_tab[i].size = sz;
            g_res_tab[i].backing_va = (uint64_t)backing;
            g_res_tab[i].host_resource_id = host_resource_id;
            return (int32_t)host_resource_id;
        }
    }
    (void)vgpu_blob_destroy(host_resource_id);
    return -ENOMEM;
}

uint64_t vg3d_res_map(uint32_t pid, uint32_t res_id) {
    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == res_id && g_res_tab[i].pid == pid) {
            return g_res_tab[i].backing_va;
        }
    }
    return 0;
}
int32_t vg3d_res_destroy(uint32_t pid, uint32_t res_id) {
    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == res_id && g_res_tab[i].pid == pid) {
            if (vgpu_blob_destroy(g_res_tab[i].host_resource_id) < 0)
                return -EIO;
            __builtin_memset(&g_res_tab[i], 0, sizeof(g_res_tab[i]));
            return 0;
        }
    }
    return -ESRCH;
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
    void *buf_phys = mem_alloc_pages(pages);
    if (!buf_phys) return -ENOMEM;
    uint8_t *buf = (uint8_t *)PHYS_TO_VIRT((uint64_t)buf_phys);

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
    mem_free_pages(buf_phys, pages);
    if (rc < 0 || resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("[VG3D] SUBMIT_3D response rc=");
        serial_putdec((uint32_t)rc);
        serial_puts(" type=0x");
        serial_puthex(resp.type, 8);
        serial_puts(" ctx=");
        serial_putdec(ctx_id);
        serial_puts(" bytes=");
        serial_putdec(cmd_len);
        serial_puts("\n");
        return -EIO;
    }
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
int32_t vg3d_present(uint32_t pid, uint32_t ctx_id,
                      uint32_t res_id, uint32_t shm_handle) {
    (void)pid;
    (void)ctx_id;
    (void)res_id;
    (void)shm_handle;
    return -ENOSYS;
}
void vg3d_cleanup_process(uint32_t pid) {
    for (int i = 0; i < VG3D_CTX_MAX; i++) {
        if (g_ctx_tab[i].id && g_ctx_tab[i].pid == pid) {
            (void)vg3d_host_ctx_destroy(g_ctx_tab[i].id);
            g_ctx_tab[i].id = 0;
            g_ctx_tab[i].pid = 0;
            g_ctx_tab[i].flags = 0;
        }
    }
    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id && g_res_tab[i].pid == pid) {
            (void)vgpu_blob_destroy(g_res_tab[i].host_resource_id);
            __builtin_memset(&g_res_tab[i], 0, sizeof(g_res_tab[i]));
        }
    }
}

/* -- Self-test harness -------------------------------------- */

void virtio_gpu_3d_selftest(void) {
    serial_puts(g_3d_ready
        ? "[VG3D-TEST] official protocol round-trip OK\n"
        : "[VG3D-TEST] official protocol round-trip unavailable\n");
}
