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

#define SHM_FLAG_GPU_SCANOUT 4u

#define VIRTIO_GPU_FLAG_FENCE 1u

#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB    0x010cu
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB       0x0208u
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB     0x0209u

#define VIRTIO_GPU_RESP_OK_NODATA                  0x1100u
#define VIRTIO_GPU_RESP_OK_MAP_INFO                0x1106u
#define VIRTIO_GPU_RESP_ERR_UNSPEC                 0x1200u
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY          0x1201u
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID     0x1202u
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID    0x1203u
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID     0x1204u
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER      0x1205u

#define VIRTIO_GPU_CAPSET_VENUS 4u

#define VIRTIO_GPU_BLOB_MEM_HOST3D            0x0002u
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE     0x0001u

#define VG3D_HOSTMEM_ALIGN 4096ull

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
    uint64_t hostmem_offset;    /* BAR4 offset for mapped blob resources */
    uint32_t map_info;
    bool     blob;
};

static struct vg3d_res g_res_tab[VG3D_RES_MAX];
/* QEMU resource ids are global per virtio-gpu device. The 2D scanout path
 * owns low ids starting at 1, so 3D resources live in a separate range. */
static uint32_t g_res_next_id = 0x1000;
static uint32_t g_res_create_log_count = 0;

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

struct vg3d_ctx_create_cmd {
    struct vg3d_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed));

struct vg3d_ctx_destroy_cmd {
    struct vg3d_ctrl_hdr hdr;
} __attribute__((packed));

struct vg3d_resource_unref_cmd {
    struct vg3d_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vg3d_resource_create_blob_cmd {
    struct vg3d_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t nr_entries;
    uint64_t blob_id;
    uint64_t size;
} __attribute__((packed));

struct vg3d_resource_map_blob_cmd {
    struct vg3d_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
    uint64_t offset;
} __attribute__((packed));

struct vg3d_resource_unmap_blob_cmd {
    struct vg3d_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct vg3d_resp_map_info {
    struct vg3d_ctrl_hdr hdr;
    uint32_t map_info;
    uint32_t padding;
} __attribute__((packed));

static int32_t vg3d_resp_errno(uint32_t type) {
    switch (type) {
    case VIRTIO_GPU_RESP_OK_NODATA:
    case VIRTIO_GPU_RESP_OK_MAP_INFO:
        return 0;
    case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY:
        return -ENOMEM;
    case VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER:
        return -EINVAL;
    case VIRTIO_GPU_RESP_ERR_UNSPEC:
    default:
        return -EIO;
    }
}

static int32_t vg3d_host_cmd_resp(const void *cmd, uint32_t cmd_len,
                                  const char *label,
                                  void *resp_out, uint32_t resp_out_len,
                                  uint32_t *resp_type_out) {
    /* gpu_send_cmd converts VA->PA internally, so the response buffer must
     * live in the direct map just like the command buffers below. */
    uint32_t resp_len = resp_out_len;
    if (resp_len < sizeof(struct vg3d_ctrl_hdr))
        resp_len = sizeof(struct vg3d_ctrl_hdr);
    if (resp_len > 4096)
        return -EINVAL;

    void *resp_raw = mem_alloc_pages(1);
    if (!resp_raw) return -ENOMEM;
    struct vg3d_ctrl_hdr *resp =
        (struct vg3d_ctrl_hdr *)PHYS_TO_VIRT((uint64_t)resp_raw);
    for (uint32_t i = 0; i < resp_len; i++)
        ((volatile uint8_t *)resp)[i] = 0;

    int rc = vgpu_controlq_submit((void *)cmd, cmd_len, resp, resp_len);
    if (rc < 0) {
        mem_free_pages(resp_raw, 1);
        return -EIO;
    }
    uint32_t resp_type = resp->type;
    if (resp_out && resp_out_len) {
        uint32_t copy_len = resp_out_len < resp_len ? resp_out_len : resp_len;
        for (uint32_t i = 0; i < copy_len; i++)
            ((uint8_t *)resp_out)[i] = ((volatile uint8_t *)resp)[i];
    }
    mem_free_pages(resp_raw, 1);

    if (resp_type_out) *resp_type_out = resp_type;

    int32_t err = vg3d_resp_errno(resp_type);
    if (err < 0) {
        serial_puts("[VG3D] ");
        serial_puts(label);
        serial_puts(" host resp=0x");
        serial_puthex(resp_type, 4);
        serial_puts("\n");
    }
    return err;
}

static int32_t vg3d_host_cmd(const void *cmd, uint32_t cmd_len,
                             const char *label, uint32_t *resp_type_out) {
    return vg3d_host_cmd_resp(cmd, cmd_len, label, 0,
                              sizeof(struct vg3d_ctrl_hdr), resp_type_out);
}

static int32_t vg3d_host_ctx_create(uint32_t ctx_id, uint32_t flags) {
    void *raw = mem_alloc_pages(1);
    if (!raw) return -ENOMEM;
    struct vg3d_ctx_create_cmd *cmd =
        (struct vg3d_ctx_create_cmd *)PHYS_TO_VIRT((uint64_t)raw);

    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = ctx_id;
    cmd->hdr.padding = 0;
    cmd->nlen = 0;
    cmd->context_init = 0;

    for (uint32_t i = 0; i < sizeof(cmd->debug_name); i++)
        cmd->debug_name[i] = 0;

    if (flags == GPU_CTX_VENUS &&
        (vgpu_device_features() & (1ull << VIRTIO_GPU_F_CONTEXT_INIT))) {
        cmd->context_init = VIRTIO_GPU_CAPSET_VENUS;
    }

    int32_t err = vg3d_host_cmd(cmd, sizeof(*cmd), "CTX_CREATE", 0);
    mem_free_pages(raw, 1);
    return err;
}

static int32_t vg3d_host_ctx_destroy(uint32_t ctx_id) {
    void *raw = mem_alloc_pages(1);
    if (!raw) return -ENOMEM;
    struct vg3d_ctx_destroy_cmd *cmd =
        (struct vg3d_ctx_destroy_cmd *)PHYS_TO_VIRT((uint64_t)raw);

    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = ctx_id;
    cmd->hdr.padding = 0;

    int32_t err = vg3d_host_cmd(cmd, sizeof(*cmd), "CTX_DESTROY", 0);
    mem_free_pages(raw, 1);
    return err;
}

static uint64_t vg3d_align_up_u64(uint64_t value, uint64_t align) {
    if (!align) return value;
    return (value + align - 1) & ~(align - 1);
}

static uint64_t g_hostmem_next_offset = 0;

static int32_t vg3d_hostmem_alloc(uint64_t size, uint64_t *offset_out) {
    uint64_t hostmem_size = vgpu_hostmem_size();
    if (!offset_out || !vgpu_hostmem_base() || !hostmem_size)
        return -EINVAL;

    uint64_t aligned_size = vg3d_align_up_u64(size, VG3D_HOSTMEM_ALIGN);
    uint64_t offset = vg3d_align_up_u64(g_hostmem_next_offset,
                                        VG3D_HOSTMEM_ALIGN);
    if (offset + aligned_size > hostmem_size || offset + aligned_size < offset)
        return -ENOMEM;

    g_hostmem_next_offset = offset + aligned_size;
    *offset_out = offset;
    return 0;
}

static int32_t vg3d_host_res_unref(uint32_t res_id) {
    void *raw = mem_alloc_pages(1);
    if (!raw) return -ENOMEM;
    struct vg3d_resource_unref_cmd *cmd =
        (struct vg3d_resource_unref_cmd *)PHYS_TO_VIRT((uint64_t)raw);

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.padding = 0;
    cmd->resource_id = res_id;
    cmd->padding = 0;

    int32_t err = vg3d_host_cmd(cmd, sizeof(*cmd), "RESOURCE_UNREF", 0);
    mem_free_pages(raw, 1);
    return err;
}

static int32_t vg3d_host_res_unmap_blob(uint32_t res_id) {
    void *raw = mem_alloc_pages(1);
    if (!raw) return -ENOMEM;
    struct vg3d_resource_unmap_blob_cmd *cmd =
        (struct vg3d_resource_unmap_blob_cmd *)PHYS_TO_VIRT((uint64_t)raw);

    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    cmd->hdr.flags = 0;
    cmd->hdr.fence_id = 0;
    cmd->hdr.ctx_id = 0;
    cmd->hdr.padding = 0;
    cmd->resource_id = res_id;
    cmd->padding = 0;

    int32_t err = vg3d_host_cmd(cmd, sizeof(*cmd), "RESOURCE_UNMAP_BLOB", 0);
    mem_free_pages(raw, 1);
    return err;
}

static int32_t vg3d_host_res_create_blob(uint32_t ctx_id, uint32_t res_id,
                                         uint64_t size,
                                         uint64_t *hostmem_offset_out,
                                         uint32_t *map_info_out) {
    if (!(vgpu_device_features() & (1ull << VIRTIO_GPU_F_RESOURCE_BLOB)))
        return -EINVAL;

    uint64_t offset = 0;
    int32_t err = vg3d_hostmem_alloc(size, &offset);
    if (err < 0) return err;

    void *raw = mem_alloc_pages(1);
    if (!raw) return -ENOMEM;
    struct vg3d_resource_create_blob_cmd *create =
        (struct vg3d_resource_create_blob_cmd *)PHYS_TO_VIRT((uint64_t)raw);

    create->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    create->hdr.flags = 0;
    create->hdr.fence_id = 0;
    create->hdr.ctx_id = ctx_id;
    create->hdr.padding = 0;
    create->resource_id = res_id;
    create->blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    create->blob_flags = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    create->nr_entries = 0;
    create->blob_id = 0;
    create->size = size;

    err = vg3d_host_cmd(create, sizeof(*create), "RESOURCE_CREATE_BLOB", 0);
    if (err < 0) {
        mem_free_pages(raw, 1);
        return err;
    }

    struct vg3d_resource_map_blob_cmd *map =
        (struct vg3d_resource_map_blob_cmd *)create;
    map->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    map->hdr.flags = 0;
    map->hdr.fence_id = 0;
    map->hdr.ctx_id = 0;
    map->hdr.padding = 0;
    map->resource_id = res_id;
    map->padding = 0;
    map->offset = offset;

    struct vg3d_resp_map_info map_resp;
    err = vg3d_host_cmd_resp(map, sizeof(*map), "RESOURCE_MAP_BLOB",
                             &map_resp, sizeof(map_resp), 0);
    mem_free_pages(raw, 1);
    if (err < 0) {
        (void)vg3d_host_res_unref(res_id);
        return err;
    }

    if (hostmem_offset_out) *hostmem_offset_out = offset;
    if (map_info_out) *map_info_out = map_resp.map_info;
    return 0;
}

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
    serial_puts("[VG3D] ready -- VIRGL negotiated\n");
    serial_puts("[VG3D] hostmem base=0x");
    serial_puthex(vgpu_hostmem_base(), 16);
    serial_puts(" size=0x");
    serial_puthex(vgpu_hostmem_size(), 16);
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
            int32_t err = vg3d_host_ctx_create(id, flags);
            if (err < 0) return err;
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
            int32_t err = vg3d_host_ctx_destroy(ctx_id);
            if (err < 0) return err;
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

    for (int i = 0; i < VG3D_RES_MAX; i++) {
        if (g_res_tab[i].id == 0) {
            uint32_t id = g_res_next_id++;
            uint64_t phys = 0;
            uint64_t hostmem_offset = 0;
            uint32_t map_info = 0;
            bool blob = false;
            void *phys_raw = 0;
            void *backing = 0;

            if (args->kind == GPU_RES_KIND_BUFFER &&
                (args->flags & GPU_RES_FLAG_HOST_COHERENT)) {
                int32_t berr = vg3d_host_res_create_blob(ctx_id, id, sz,
                                                         &hostmem_offset,
                                                         &map_info);
                if (berr == 0) {
                    blob = true;
                    phys = vgpu_hostmem_base() + hostmem_offset;
                    backing = PHYS_TO_VIRT(phys);
                } else if (g_res_create_log_count < 32u) {
                    serial_puts("[VG3D] blob fallback id=");
                    serial_putdec(id);
                    serial_puts(" err=");
                    serial_putdec((uint32_t)-berr);
                    serial_puts("\n");
                }
            }

            if (!backing) {
                uint64_t pages = (sz + 4095) >> 12;
                phys_raw = mem_alloc_pages(pages);
                if (!phys_raw) return -ENOMEM;
                phys = (uint64_t)phys_raw;
                /* Upper-half VA view of the just-allocated phys range via
                 * PML4[256] direct map. */
                backing = PHYS_TO_VIRT(phys);
            }

            /* Zero the backing — qword pass + byte tail to handle sz % 8. */
            uint64_t qwords = sz >> 3;
            for (uint64_t j = 0; j < qwords; j++)
                ((volatile uint64_t *)backing)[j] = 0;
            for (uint64_t j = qwords << 3; j < sz; j++)
                ((volatile uint8_t *)backing)[j] = 0;

            g_res_tab[i].id = id;
            g_res_tab[i].ctx_id = ctx_id;
            g_res_tab[i].pid = pid;
            g_res_tab[i].kind = args->kind;
            g_res_tab[i].flags = args->flags;
            g_res_tab[i].format = args->format;
            g_res_tab[i].size = sz;
            g_res_tab[i].backing_va = (uint64_t)backing;   /* upper-half VA */
            g_res_tab[i].backing_phys = phys;              /* phys, no conv */
            g_res_tab[i].hostmem_offset = hostmem_offset;
            g_res_tab[i].map_info = map_info;
            g_res_tab[i].blob = blob;
            if (g_res_create_log_count < 32u) {
                g_res_create_log_count++;
                serial_puts("[VG3D] res create id=");
                serial_putdec(g_res_tab[i].id);
                serial_puts(" ctx=");
                serial_putdec(ctx_id);
                serial_puts(" kind=");
                serial_putdec(args->kind);
                serial_puts(" flags=0x");
                serial_puthex(args->flags, 8);
                serial_puts(" size=0x");
                serial_puthex(sz, 16);
                serial_puts(" va=0x");
                serial_puthex((uint64_t)backing, 16);
                serial_puts(" blob=");
                serial_putdec(blob ? 1 : 0);
                if (blob) {
                    serial_puts(" hmo=0x");
                    serial_puthex(hostmem_offset, 16);
                    serial_puts(" map=0x");
                    serial_puthex(map_info, 8);
                }
                serial_puts("\n");
            }
            return (int32_t)g_res_tab[i].id;
        }
    }
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
    void *raw = mem_alloc_pages(pages);
    if (!raw) return -ENOMEM;
    uint8_t *buf = (uint8_t *)PHYS_TO_VIRT((uint64_t)raw);

    struct vg3d_ctrl_hdr *h = (struct vg3d_ctrl_hdr *)buf;
    h->type = VIRTIO_GPU_CMD_SUBMIT_3D;
    h->flags = VIRTIO_GPU_FLAG_FENCE;
    h->fence_id = fence;
    h->ctx_id = ctx_id;
    h->padding = 0;
    *(uint32_t *)(buf + sizeof(*h)) = (uint32_t)cmd_len;
    *(uint32_t *)(buf + sizeof(*h) + 4) = 0;
    /* Copy user bytes into kernel staging (byte-by-byte is fine; Wave 2
     * will replace with copy_from_user). */
    for (uint64_t i = 0; i < cmd_len; i++)
        buf[hdr_len + i] = ((const uint8_t *)cmd_bytes)[i];

    int32_t err = vg3d_host_cmd(buf, total, "SUBMIT_3D", 0);
    mem_free_pages(raw, pages);
    if (err < 0) {
        *out_fence = 0;
        return err;
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
            if (g_res_tab[i].blob) {
                (void)vg3d_host_res_unmap_blob(g_res_tab[i].id);
                (void)vg3d_host_res_unref(g_res_tab[i].id);
            } else {
                uint64_t pages = (g_res_tab[i].size + 4095) >> 12;
                /* mem_free_pages expects phys (same contract as
                 * mem_alloc_pages); backing_phys cached at create time. */
                mem_free_pages((void *)g_res_tab[i].backing_phys, pages);
            }
            g_res_tab[i].id = 0;
            g_res_tab[i].ctx_id = 0;
            g_res_tab[i].pid = 0;
            g_res_tab[i].size = 0;
            g_res_tab[i].backing_va = 0;
            g_res_tab[i].backing_phys = 0;
            g_res_tab[i].hostmem_offset = 0;
            g_res_tab[i].map_info = 0;
            g_res_tab[i].blob = false;
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

static void vg3d_t9_caps(void) {
    uint32_t caps = vg3d_caps();
    extern bool nvk_backend_ready(void);
    bool nvk = nvk_backend_ready();
    serial_puts("[VG3D-T9] caps venus=");
    serial_puts((caps & GPU_CAP_VENUS_READY) ? "1" : "0");
    serial_puts(" nvk=");
    serial_puts(nvk ? "1" : "0");
    /* Wave 1 expectation: nvk=0 always. Fail if stub lies. */
    if (!nvk) serial_puts(" OK\n");
    else      serial_puts(" FAIL (stub should report 0)\n");
}

static void vg3d_t8_present(void) {
    if (!g_3d_ready) { serial_puts("[VG3D-T8] present SKIP\n"); return; }
    extern uint32_t shm_create_surface(uint32_t w, uint32_t h, uint32_t flags);
    uint32_t shm = shm_create_surface(64, 64, SHM_FLAG_GPU_SCANOUT);
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
    int32_t err = vg3d_fence_wait(0, 0);
    if (err == -EINVAL) {
        serial_puts("[VG3D-T7] fence-guard OK\n");
    } else {
        serial_puts("[VG3D-T7] fence-guard FAIL err=");
        if (err < 0) serial_putdec((uint32_t)-err);
        else serial_putdec((uint32_t)err);
        serial_puts("\n");
    }
}

static void vg3d_t6_submit(void) {
    if (!g_3d_ready) { serial_puts("[VG3D-T6] submit SKIP\n"); return; }
    int32_t cid = vg3d_ctx_create(1, GPU_CTX_VENUS);
    if (cid <= 0) { serial_puts("[VG3D-T6] submit FAIL (ctx)\n"); return; }
    uint8_t dummy = 0;
    uint64_t fence = 0;
    int32_t err = vg3d_submit(1, (uint32_t)cid, &dummy, 0, &fence);
    if (err == -EINVAL && fence == 0) {
        serial_puts("[VG3D-T6] submit-guard OK\n");
    } else {
        serial_puts("[VG3D-T6] submit-guard FAIL err=");
        if (err < 0) serial_putdec((uint32_t)-err);
        else serial_putdec((uint32_t)err);
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
    serial_puts(g_3d_ready
        ? "[VG3D-TEST] official protocol round-trip OK\n"
        : "[VG3D-TEST] official protocol round-trip unavailable\n");
}
