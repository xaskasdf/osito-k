/*
 * OsitoK x86-64 — Virtio GPU Driver
 *
 * Implements virtio-gpu protocol for framebuffer display via QEMU.
 * With virglrenderer enabled in QEMU, this provides a path for
 * 3D-accelerated rendering (OpenGL/Vulkan passthrough to host GPU).
 *
 * Initial version: 2D scanout only (create resource, attach backing,
 * set scanout, transfer, flush). 3D context support (virgl commands)
 * will be added once the basic display path works.
 *
 * Depends on: virtio.c (virtqueue infrastructure)
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void *mem_alloc_pages(uint64_t num_pages);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);

/* From virtio.c */
extern int  virtio_pci_init(volatile void *common_cfg);

/* ── Virtio GPU Device ID ─────────────────────────────────────── */
#define VIRTIO_DEV_GPU      0x1050  /* virtio 1.0 GPU */

/* ── Virtio GPU Command Types ─────────────────────────────────── */
/* 2D commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109
#define VIRTIO_GPU_CMD_GET_EDID                0x010A

/* 3D commands (virgl) */
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207

/* Response types */
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103
#define VIRTIO_GPU_RESP_OK_EDID                0x1104
#define VIRTIO_GPU_RESP_ERR_UNSPEC             0x1200

/* Pixel formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM       1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM       3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM       4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM       67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM       68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM       121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM       134

/* ── Virtio GPU Command Structures ────────────────────────────── */

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

/* GET_DISPLAY_INFO response */
#define VIRTIO_GPU_MAX_SCANOUTS 16
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

/* RESOURCE_CREATE_2D */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* RESOURCE_ATTACH_BACKING */
struct virtio_gpu_mem_entry {
    uint64_t addr;   /* guest physical address */
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by nr_entries virtio_gpu_mem_entry structs */
} __attribute__((packed));

/* SET_SCANOUT */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

/* TRANSFER_TO_HOST_2D */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* RESOURCE_FLUSH */
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ── Driver State ─────────────────────────────────────────────── */

static struct {
    volatile void *common_cfg;   /* Virtio PCI common config MMIO */
    volatile void *notify;       /* Virtio PCI notify MMIO */
    uint32_t notify_off_mult;    /* Notify offset multiplier */

    void *controlq;        /* Control virtqueue (GPU commands) */
    void *cursorq;         /* Cursor virtqueue */

    uint32_t width, height;      /* Display resolution */
    uint32_t resource_id;        /* Current framebuffer resource ID */
    uint32_t *framebuffer;       /* Guest-side pixel buffer */
    uint64_t fb_phys;            /* Physical address of framebuffer */
    uint64_t fb_size;            /* Framebuffer size in bytes */

    bool initialized;
} gpu;

/* ── TODO: Implementation ─────────────────────────────────────── */
/*
 * Next steps:
 * 1. PCI probe for VIRTIO_DEV_GPU (0x1050)
 * 2. Parse PCI capabilities to find common_cfg, notify, ISR, device_cfg
 * 3. Init controlq + cursorq via virtio_pci_init + virtqueue_init
 * 4. Send GET_DISPLAY_INFO → get resolution
 * 5. Send RESOURCE_CREATE_2D → create BGRA framebuffer
 * 6. Send RESOURCE_ATTACH_BACKING → map guest memory
 * 7. Send SET_SCANOUT → attach to display 0
 * 8. Expose framebuffer to userspace via syscall
 * 9. On flush: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
 *
 * For 3D (virgl):
 * 10. Negotiate VIRTIO_GPU_F_VIRGL feature
 * 11. CTX_CREATE → create virgl rendering context
 * 12. SUBMIT_3D → submit virgl command buffers
 * 13. Expose as Vulkan ICD via venus protocol
 */

void virtio_gpu_init(uint64_t bar0_phys)
{
    serial_puts("[VIRTIO-GPU] Driver stub loaded\n");
    serial_puts("[VIRTIO-GPU] BAR0=0x");
    serial_puthex(bar0_phys, 16);
    serial_puts("\n");
    serial_puts("[VIRTIO-GPU] Full implementation pending — need PCI cap parsing + command submission\n");
    gpu.initialized = false;
}
