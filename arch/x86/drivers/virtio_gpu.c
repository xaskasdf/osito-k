/*
 * OsitoK x86-64 — Virtio GPU Driver (2D scanout)
 *
 * Implements virtio-gpu protocol for framebuffer display via QEMU.
 * Creates a 2D resource, attaches guest memory, sets up scanout,
 * and provides a flush interface for updating the display.
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "../include/drivers/virtio_gpu.h"
#include "../kernel/smp.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);
extern uint64_t idt_get_ticks(void);
extern uint32_t *fb_get_base(void)   __attribute__((weak));
extern uint32_t  fb_get_width(void)  __attribute__((weak));
extern uint32_t  fb_get_height(void) __attribute__((weak));
extern uint32_t  fb_get_pitch(void)  __attribute__((weak));

void virtio_gpu_flush(void);

/* ── Virtio PCI Capability Types ──────────────────────────────── */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8

/* ── Virtio GPU Commands ──────────────────────────────────────── */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2

/* ── Virtio Status ────────────────────────────────────────────── */
#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4

#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2
#define VIRTQ_NO_DESC              0xFFFFu
#define VIRTIO_GPU_CMD_TIMEOUT_TICKS 500u
#define VIRTIO_GPU_MAX_MODE_DIM 16384u

#define VIRTIO_GPU_EIO    (-5)
#define VIRTIO_GPU_ENOMEM (-12)
#define VIRTIO_GPU_EINVAL (-22)
#define VIRTIO_GPU_ENOSYS (-38)

#define VIRTIO_GPU_F_VIRGL          (1ull << 0)
#define VIRTIO_GPU_F_EDID           (1ull << 1)
#define VIRTIO_GPU_F_RESOURCE_BLOB  (1ull << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT   (1ull << 4)
#define VIRTIO_F_VERSION_1          (1ull << 32)

/* ── Structures ───────────────────────────────────────────────── */

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

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ── Virtqueue (inline, simplified split ring) ────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} vq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vq_used_elem_t ring[];
} vq_used_t;

/* ── Driver State ─────────────────────────────────────────────── */

static struct {
    /* PCI MMIO regions (found via capabilities) */
    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    volatile uint8_t *device_cfg;
    uint32_t notify_off_mult;

    /* Control virtqueue */
    vq_desc_t  *desc;
    vq_avail_t *avail;
    vq_used_t  *used;
    uint16_t    vq_size;
    uint16_t    free_head;
    uint16_t    last_used;

    /* Display */
    uint32_t width, height;
    uint32_t native_width, native_height;
    uint32_t *framebuffer;
    uint64_t  fb_phys;
    uint64_t  fb_size;
    uint32_t  resource_id;
    uint32_t  next_resource_id;

    /* ECAM base for PCI config reads */
    uint64_t ecam_base;
    uint8_t  pci_bus, pci_dev, pci_func;
    uint64_t hostmem_base;
    uint64_t hostmem_size;

    bool initialized;
    bool scanout_active;
    bool queue_broken;

    /* Negotiated device feature vector (64-bit). Populated before FEATURES_OK
     * so the 3D driver can probe VIRTIO_GPU_F_VIRGL. */
    uint64_t device_features;
} gpu;

static spinlock_t gpu_cmd_lock = SPINLOCK_INIT;

static void virtio_gpu_seed_from_boot_fb(void)
{
    if (!fb_get_base || !fb_get_width || !fb_get_height || !fb_get_pitch) {
        return;
    }

    uint32_t *src = fb_get_base();
    if (!src || !gpu.framebuffer || !gpu.width || !gpu.height) {
        return;
    }

    uint32_t sw = fb_get_width();
    uint32_t sh = fb_get_height();
    uint32_t sp = fb_get_pitch();
    if (!sw || !sh || !sp) {
        return;
    }

    uint32_t copy_w = sw < gpu.width ? sw : gpu.width;
    uint32_t copy_h = sh < gpu.height ? sh : gpu.height;
    for (uint32_t y = 0; y < copy_h; y++) {
        memcpy(gpu.framebuffer + y * gpu.width, src + y * sp,
               (uint64_t)copy_w * sizeof(uint32_t));
    }

    serial_puts("[VIRTIO-GPU] Seeded scanout from boot framebuffer\n");
    virtio_gpu_flush();
}

/* ── PCI Config Read via ECAM ─────────────────────────────────── */

static uint32_t ecam_read32(uint16_t offset) {
    uint64_t addr = gpu.ecam_base
        | ((uint64_t)gpu.pci_bus << 20)
        | ((uint64_t)gpu.pci_dev << 15)
        | ((uint64_t)gpu.pci_func << 12)
        | offset;
    return *(volatile uint32_t *)addr;
}

static uint8_t ecam_read8(uint16_t offset) {
    uint64_t addr = gpu.ecam_base
        | ((uint64_t)gpu.pci_bus << 20)
        | ((uint64_t)gpu.pci_dev << 15)
        | ((uint64_t)gpu.pci_func << 12)
        | offset;
    return *(volatile uint8_t *)addr;
}

static void ecam_write32(uint16_t offset, uint32_t value) {
    uint64_t addr = gpu.ecam_base
        | ((uint64_t)gpu.pci_bus << 20)
        | ((uint64_t)gpu.pci_dev << 15)
        | ((uint64_t)gpu.pci_func << 12)
        | offset;
    *(volatile uint32_t *)addr = value;
    __asm__ volatile ("mfence" ::: "memory");
}

static uint64_t align_up_u64(uint64_t value, uint64_t align) {
    if (align == 0) return value;
    return (value + align - 1) & ~(align - 1);
}

/* ── Parse PCI Capabilities ───────────────────────────────────── */

static int parse_capabilities(uint64_t *bars) {
    uint8_t raw34 = ecam_read8(0x34);
    uint8_t cap_ptr = raw34 & 0xFC;
    serial_puts("[VIRTIO-GPU] caps: raw34=0x");
    serial_puthex(raw34, 2);
    serial_puts(" ptr=0x");
    serial_puthex(cap_ptr, 2);
    serial_puts("\n");
    int found = 0;

    while (cap_ptr) {
        uint8_t cap_id  = ecam_read8(cap_ptr);
        uint8_t cap_next = ecam_read8(cap_ptr + 1);
        serial_puts("[VIRTIO-GPU] cap@0x");
        serial_puthex(cap_ptr, 2);
        serial_puts(" id=0x");
        serial_puthex(cap_id, 2);
        serial_puts(" next=0x");
        serial_puthex(cap_next, 2);
        serial_puts("\n");

        if (cap_id == 0x09) {  /* Vendor-specific = virtio */
            uint8_t cfg_type = ecam_read8(cap_ptr + 3);
            uint8_t bar      = ecam_read8(cap_ptr + 4);
            uint8_t cap_id2  = ecam_read8(cap_ptr + 5);
            uint32_t offset  = ecam_read32(cap_ptr + 8);
            uint32_t length  = ecam_read32(cap_ptr + 12);
            uint64_t offset64 = offset;
            uint64_t length64 = length;
            if (cfg_type == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG) {
                offset64 |= (uint64_t)ecam_read32(cap_ptr + 16) << 32;
                length64 |= (uint64_t)ecam_read32(cap_ptr + 20) << 32;
            }
            serial_puts("[VIRTIO-GPU]   type=");
            serial_putdec(cfg_type);
            serial_puts(" bar=");
            serial_putdec(bar);
            if (cfg_type == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG) {
                serial_puts(" id=");
                serial_putdec(cap_id2);
            }
            serial_puts(" off=0x");
            serial_puthex(offset64, 16);
            serial_puts(" len=0x");
            serial_puthex(length64, 16);
            serial_puts(" baraddr=0x");
            serial_puthex(bars[bar], 16);
            serial_puts("\n");

            uint64_t bar_addr = bars[bar];
            if (!bar_addr) { cap_ptr = cap_next; continue; }

            volatile uint8_t *mapped = (volatile uint8_t *)PHYS_TO_VIRT(bar_addr + offset);

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                gpu.common_cfg = mapped;
                serial_puts("[VIRTIO-GPU] common_cfg at BAR");
                serial_putdec(bar);
                serial_puts("+0x"); serial_puthex(offset, 4);
                serial_puts("\n");
                found |= 1;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                gpu.notify_base = mapped;
                gpu.notify_off_mult = ecam_read32(cap_ptr + 16);
                serial_puts("[VIRTIO-GPU] notify at BAR");
                serial_putdec(bar);
                serial_puts("+0x"); serial_puthex(offset, 4);
                serial_puts(" mult="); serial_putdec(gpu.notify_off_mult);
                serial_puts("\n");
                found |= 2;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                gpu.device_cfg = mapped;
                found |= 4;
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                found |= 8;
                break;
            case VIRTIO_PCI_CAP_SHARED_MEMORY_CFG:
                if (cap_id2 == 1 && length64 != 0) {
                    gpu.hostmem_base = bar_addr + offset64;
                    gpu.hostmem_size = length64;
                    paging_map_mmio(gpu.hostmem_base, gpu.hostmem_size);
                    serial_puts("[VIRTIO-GPU] hostmem BAR");
                    serial_putdec(bar);
                    serial_puts("+0x");
                    serial_puthex(offset64, 16);
                    serial_puts(" size=0x");
                    serial_puthex(length64, 16);
                    serial_puts(" va=0x");
                    serial_puthex((uint64_t)PHYS_TO_VIRT(gpu.hostmem_base), 16);
                    serial_puts("\n");
                }
                break;
            }
        }
        cap_ptr = cap_next;
    }
    return found;
}

/* ── Virtqueue Setup ──────────────────────────────────────────── */

static int setup_controlq(void) {
    volatile uint8_t *cfg = gpu.common_cfg;

    /* Select queue 0 */
    *(volatile uint16_t *)(cfg + 0x16) = 0;
    __asm__ volatile ("mfence" ::: "memory");

    gpu.vq_size = *(volatile uint16_t *)(cfg + 0x18);
    if (gpu.vq_size == 0) gpu.vq_size = 64;
    *(volatile uint16_t *)(cfg + 0x18) = gpu.vq_size;
    __asm__ volatile ("mfence" ::: "memory");
    serial_puts("[VIRTIO-GPU] controlq size=");
    serial_putdec(gpu.vq_size);
    serial_puts("\n");

    /* Allocate descriptor table — physical for device, virtual for CPU */
    uint64_t desc_bytes = (uint64_t)gpu.vq_size * 16;
    void *desc_raw = mem_alloc_aligned(desc_bytes, 4096);
    if (!desc_raw) return -1;
    uint64_t desc_phys = (uint64_t)desc_raw;
    gpu.desc = (vq_desc_t *)PHYS_TO_VIRT((uint64_t)desc_raw);
    memset(gpu.desc, 0, desc_bytes);

    /* Allocate available ring */
    uint64_t avail_bytes = 6 + 2 * (uint64_t)gpu.vq_size;
    void *avail_raw = mem_alloc_aligned(avail_bytes, 4096);
    if (!avail_raw) return -1;
    uint64_t avail_phys = (uint64_t)avail_raw;
    gpu.avail = (vq_avail_t *)PHYS_TO_VIRT((uint64_t)avail_raw);
    memset(gpu.avail, 0, avail_bytes);

    /* Allocate used ring */
    uint64_t used_bytes = 6 + 8 * (uint64_t)gpu.vq_size;
    void *used_raw = mem_alloc_aligned(used_bytes, 4096);
    if (!used_raw) return -1;
    uint64_t used_phys = (uint64_t)used_raw;
    gpu.used = (vq_used_t *)PHYS_TO_VIRT((uint64_t)used_raw);
    memset(gpu.used, 0, used_bytes);

    /* Chain free descriptors.  Use an explicit end-of-list marker so a
     * timed-out descriptor is never accidentally reissued while the host
     * may still own it. */
    for (uint16_t i = 0; i < gpu.vq_size - 1; i++) {
        gpu.desc[i].next = i + 1;
        gpu.desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    gpu.desc[gpu.vq_size - 1].next = VIRTQ_NO_DESC;
    gpu.desc[gpu.vq_size - 1].flags = 0;
    gpu.free_head = 0;
    gpu.last_used = 0;
    gpu.queue_broken = false;

    /* Write queue physical addresses to device (desc_phys/avail_phys/used_phys
     * were captured above from mem_alloc_aligned before PHYS_TO_VIRT). */

    /* Actually: virtio common config layout (v1.0):
     * 0x00: device_feature_select (u32)
     * 0x04: device_feature (u32)
     * 0x08: driver_feature_select (u32)
     * 0x0C: driver_feature (u32)
     * 0x10: msix_config (u16)
     * 0x12: num_queues (u16)
     * 0x14: device_status (u8)
     * 0x15: config_generation (u8)
     * 0x16: queue_select (u16)
     * 0x18: queue_size (u16)
     * 0x1A: queue_msix_vector (u16)
     * 0x1C: queue_enable (u16)
     * 0x1E: queue_notify_off (u16)
     * 0x20: queue_desc (u64)
     * 0x28: queue_avail (u64)  [called queue_driver in spec]
     * 0x30: queue_used (u64)   [called queue_device in spec]
     */

    /* Correct register layout */
    *(volatile uint16_t *)(cfg + 0x16) = 0;  /* queue_select = 0 */
    __asm__ volatile ("mfence" ::: "memory");

    *(volatile uint32_t *)(cfg + 0x20) = (uint32_t)desc_phys;
    *(volatile uint32_t *)(cfg + 0x24) = (uint32_t)(desc_phys >> 32);
    *(volatile uint32_t *)(cfg + 0x28) = (uint32_t)avail_phys;
    *(volatile uint32_t *)(cfg + 0x2C) = (uint32_t)(avail_phys >> 32);
    *(volatile uint32_t *)(cfg + 0x30) = (uint32_t)used_phys;
    *(volatile uint32_t *)(cfg + 0x34) = (uint32_t)(used_phys >> 32);
    __asm__ volatile ("mfence" ::: "memory");

    /* Enable queue */
    *(volatile uint16_t *)(cfg + 0x1C) = 1;
    __asm__ volatile ("mfence" ::: "memory");

    serial_puts("[VIRTIO-GPU] controlq desc_virt=0x");
    serial_puthex((uint64_t)gpu.desc, 16);
    serial_puts(" desc_phys=0x");
    serial_puthex(desc_phys, 16);
    serial_puts("\n");
    serial_puts("[VIRTIO-GPU] controlq configured\n");
    return 0;
}

/* ── Send Command + Wait for Response ─────────────────────────── */

static int gpu_send_cmd_unlocked(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len) {
    if (gpu.queue_broken) return -1;
    if (gpu.free_head == VIRTQ_NO_DESC || gpu.free_head >= gpu.vq_size) {
        serial_puts("[VIRTIO-GPU] controlq no free descriptors\n");
        return -1;
    }

    uint16_t head = gpu.free_head;
    uint16_t d0 = head;
    uint16_t d1 = gpu.desc[d0].next;
    if (d1 == VIRTQ_NO_DESC || d1 >= gpu.vq_size || d1 == d0) {
        serial_puts("[VIRTIO-GPU] controlq free-list corrupt\n");
        gpu.queue_broken = true;
        return -1;
    }
    uint16_t next_free = gpu.desc[d1].next;

    uint64_t cmd_phys = VIRT_TO_PHYS((uint64_t)cmd);
    uint64_t resp_phys = VIRT_TO_PHYS((uint64_t)resp);

    /* Descriptor 0: command (device reads) */
    gpu.desc[d0].addr = cmd_phys;
    gpu.desc[d0].len = cmd_len;
    gpu.desc[d0].flags = VIRTQ_DESC_F_NEXT; /* NEXT | read-only for device */
    gpu.desc[d0].next = d1;

    /* Descriptor 1: response (device writes) */
    gpu.desc[d1].addr = resp_phys;
    gpu.desc[d1].len = resp_len;
    gpu.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    gpu.desc[d1].next = 0;

    gpu.free_head = next_free;

    /* Add to available ring */
    uint16_t avail_idx = gpu.avail->idx % gpu.vq_size;
    gpu.avail->ring[avail_idx] = head;
    __asm__ volatile ("sfence" ::: "memory");
    gpu.avail->idx++;
    __asm__ volatile ("sfence" ::: "memory");

    /* Notify device — write queue index to notify register */
    uint16_t notify_off = *(volatile uint16_t *)(gpu.common_cfg + 0x1E);
    volatile uint16_t *notify_addr = (volatile uint16_t *)(gpu.notify_base + notify_off * gpu.notify_off_mult);
    static bool logged_first_submit = false;
    if (!logged_first_submit) {
        const struct virtio_gpu_ctrl_hdr *hdr =
            (const struct virtio_gpu_ctrl_hdr *)cmd;
        serial_puts("[VIRTIO-GPU] submit type=0x");
        serial_puthex(hdr ? hdr->type : 0, 4);
        serial_puts(" cmd_pa=0x");
        serial_puthex(cmd_phys, 16);
        serial_puts(" resp_pa=0x");
        serial_puthex(resp_phys, 16);
        serial_puts(" notify_off=");
        serial_putdec(notify_off);
        serial_puts(" mult=");
        serial_putdec(gpu.notify_off_mult);
        serial_puts("\n");
        logged_first_submit = true;
    }
    __asm__ volatile ("mfence" ::: "memory");
    *notify_addr = 0;  /* queue 0 */
    __asm__ volatile ("mfence" ::: "memory");

    /* Poll for response.  QEMU's GL path can spend noticeable time in host
     * rendering code, so do not use a tiny fixed spin count.  The descriptor
     * pair is returned to the free list only after the used-ring entry naming
     * this head is observed. */
    uint64_t start_tick = idt_get_ticks();
    for (uint64_t spins = 0; ; spins++) {
        __asm__ volatile ("lfence" ::: "memory");
        if (gpu.used->idx != gpu.last_used) {
            uint16_t used_slot = gpu.last_used % gpu.vq_size;
            uint32_t used_id = gpu.used->ring[used_slot].id;
            gpu.last_used++;
            if (used_id != head) {
                serial_puts("[VIRTIO-GPU] controlq unexpected used id=");
                serial_putdec(used_id);
                serial_puts(" expected=");
                serial_putdec(head);
                serial_puts("\n");
                gpu.queue_broken = true;
                return -1;
            }
            /* Return descriptors to free list */
            gpu.desc[d1].next = gpu.free_head;
            gpu.desc[d1].flags = 0;
            gpu.desc[d0].next = d1;
            gpu.desc[d0].flags = VIRTQ_DESC_F_NEXT;
            gpu.free_head = d0;
            return 0;
        }
        if ((idt_get_ticks() - start_tick) >= VIRTIO_GPU_CMD_TIMEOUT_TICKS ||
            spins >= 5000000000ULL)
            break;
        __asm__ volatile ("pause");
    }
    const struct virtio_gpu_ctrl_hdr *hdr =
        (const struct virtio_gpu_ctrl_hdr *)cmd;
    serial_puts("[VIRTIO-GPU] Command timeout type=0x");
    serial_puthex(hdr ? hdr->type : 0, 4);
    serial_puts(" used_idx=");
    serial_putdec(gpu.used->idx);
    serial_puts(" last_used=");
    serial_putdec(gpu.last_used);
    serial_puts(" status=0x");
    serial_puthex(*(volatile uint8_t *)(gpu.common_cfg + 0x14), 2);
    serial_puts("\n");
    gpu.queue_broken = true;
    return -1;
}

static int gpu_send_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len) {
    spin_lock(&gpu_cmd_lock);
    int rc = gpu_send_cmd_unlocked(cmd, cmd_len, resp, resp_len);
    spin_unlock(&gpu_cmd_lock);
    return rc;
}

static bool virtio_gpu_valid_mode(uint32_t width, uint32_t height,
                                  uint64_t *out_size)
{
    if (!width || !height ||
        width > VIRTIO_GPU_MAX_MODE_DIM ||
        height > VIRTIO_GPU_MAX_MODE_DIM) {
        return false;
    }

    uint64_t pixels = (uint64_t)width * height;
    uint64_t bytes = pixels * sizeof(uint32_t);
    if (pixels / width != height || bytes / sizeof(uint32_t) != pixels)
        return false;
    if (bytes > 0xFFFFFFFFULL)
        return false;

    if (out_size) *out_size = bytes;
    return true;
}

static bool virtio_gpu_ok_nodata(const struct virtio_gpu_ctrl_hdr *resp)
{
    return resp && resp->type == VIRTIO_GPU_RESP_OK_NODATA;
}

static int virtio_gpu_create_resource_locked(uint32_t resource_id,
                                             uint32_t width,
                                             uint32_t height)
{
    static struct virtio_gpu_resource_create_2d cmd;
    static struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    memset(&resp, 0, sizeof(resp));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = resource_id;
    cmd.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cmd.width = width;
    cmd.height = height;

    if (gpu_send_cmd_unlocked(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return VIRTIO_GPU_EIO;
    return virtio_gpu_ok_nodata(&resp) ? 0 : VIRTIO_GPU_EIO;
}

static int virtio_gpu_attach_backing_locked(uint32_t resource_id,
                                            uint64_t fb_phys,
                                            uint64_t fb_size)
{
    static struct {
        struct virtio_gpu_resource_attach_backing hdr;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) cmd;
    static struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    memset(&resp, 0, sizeof(resp));
    cmd.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd.hdr.resource_id = resource_id;
    cmd.hdr.nr_entries = 1;
    cmd.entry.addr = fb_phys;
    cmd.entry.length = (uint32_t)fb_size;

    if (gpu_send_cmd_unlocked(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return VIRTIO_GPU_EIO;
    return virtio_gpu_ok_nodata(&resp) ? 0 : VIRTIO_GPU_EIO;
}

static int virtio_gpu_set_scanout_locked(uint32_t resource_id,
                                         uint32_t width,
                                         uint32_t height)
{
    static struct virtio_gpu_set_scanout cmd;
    static struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    memset(&resp, 0, sizeof(resp));
    cmd.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.r.width = width;
    cmd.r.height = height;
    cmd.scanout_id = 0;
    cmd.resource_id = resource_id;

    if (gpu_send_cmd_unlocked(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return VIRTIO_GPU_EIO;
    return virtio_gpu_ok_nodata(&resp) ? 0 : VIRTIO_GPU_EIO;
}

static int virtio_gpu_transfer_flush_locked(uint32_t resource_id,
                                            uint32_t width,
                                            uint32_t height)
{
    static struct virtio_gpu_transfer_to_host_2d cmd_xfer;
    static struct virtio_gpu_ctrl_hdr resp_xfer;
    memset(&cmd_xfer, 0, sizeof(cmd_xfer));
    memset(&resp_xfer, 0, sizeof(resp_xfer));
    cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_xfer.r.width = width;
    cmd_xfer.r.height = height;
    cmd_xfer.resource_id = resource_id;
    if (gpu_send_cmd_unlocked(&cmd_xfer, sizeof(cmd_xfer),
                              &resp_xfer, sizeof(resp_xfer)) < 0) {
        return VIRTIO_GPU_EIO;
    }

    static struct virtio_gpu_resource_flush cmd_flush;
    static struct virtio_gpu_ctrl_hdr resp_flush;
    memset(&cmd_flush, 0, sizeof(cmd_flush));
    memset(&resp_flush, 0, sizeof(resp_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.r.width = width;
    cmd_flush.r.height = height;
    cmd_flush.resource_id = resource_id;
    if (gpu_send_cmd_unlocked(&cmd_flush, sizeof(cmd_flush),
                              &resp_flush, sizeof(resp_flush)) < 0) {
        return VIRTIO_GPU_EIO;
    }

    return 0;
}

static int virtio_gpu_detach_backing_locked(uint32_t resource_id)
{
    static struct virtio_gpu_resource_detach_backing cmd;
    static struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    memset(&resp, 0, sizeof(resp));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd.resource_id = resource_id;

    if (gpu_send_cmd_unlocked(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return VIRTIO_GPU_EIO;
    return virtio_gpu_ok_nodata(&resp) ? 0 : VIRTIO_GPU_EIO;
}

static int virtio_gpu_unref_resource_locked(uint32_t resource_id)
{
    static struct virtio_gpu_resource_unref cmd;
    static struct virtio_gpu_ctrl_hdr resp;
    memset(&cmd, 0, sizeof(cmd));
    memset(&resp, 0, sizeof(resp));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = resource_id;

    if (gpu_send_cmd_unlocked(&cmd, sizeof(cmd), &resp, sizeof(resp)) < 0)
        return VIRTIO_GPU_EIO;
    return virtio_gpu_ok_nodata(&resp) ? 0 : VIRTIO_GPU_EIO;
}

static void virtio_gpu_free_backing(uint64_t fb_phys, uint64_t fb_size)
{
    if (!fb_phys || !fb_size) return;
    uint64_t pages = (fb_size + 4095ULL) / 4096ULL;
    mem_free_pages((void *)fb_phys, pages);
}

/* ── Public Interface ─────────────────────────────────────────── */

void virtio_gpu_init(uint64_t ecam, uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t fb_width, uint32_t fb_height)
{
    serial_puts("[VIRTIO-GPU] Initializing at ");
    serial_putdec(bus); serial_puts(":");
    serial_putdec(dev); serial_puts(".");
    serial_putdec(func); serial_puts("\n");
    gpu.initialized = false;

    gpu.ecam_base = ecam;
    gpu.pci_bus = bus;
    gpu.pci_dev = dev;
    gpu.pci_func = func;

    /* Need ECAM page for this BDF mapped */
    paging_map_mmio(gpu.ecam_base | ((uint64_t)bus << 20) | ((uint64_t)dev << 15) | ((uint64_t)func << 12), 4096);

    /* Enable PCI memory space + bus master if not already */
    uint16_t pci_cmd = (uint16_t)ecam_read32(0x04);
    if (!(pci_cmd & 0x06)) {
        /* Set Memory Space Enable + Bus Master Enable */
        uint64_t cmd_addr = gpu.ecam_base | (gpu.pci_bus << 20) | (gpu.pci_dev << 15) | (gpu.pci_func << 12) | 0x04;
        *(volatile uint16_t *)cmd_addr = pci_cmd | 0x06;
        __asm__ volatile ("mfence" ::: "memory");
        serial_puts("[VIRTIO-GPU] Enabled PCI memory + bus master\n");
    }

    /* Program any unassigned BARs. UEFI assigns BARs for known devices but
     * skips virtio. Avoid BAR sizing probes here: on macOS HVF, rewriting
     * a live virtio-vga-gl BAR to 0xffffffff can make QEMU remap hostmem
     * while vCPUs are running and abort in hvf_set_phys_mem(). */
    {
        uint64_t next32_addr = 0x81100000ULL;
        uint64_t next64_addr = 0x880000000ULL;
        for (int i = 0; i < 6; i++) {
            uint32_t raw = ecam_read32(0x10 + i * 4);
            bool is_mem = (raw & 1) == 0;
            bool is_64 = is_mem && ((raw & 0x6) == 0x4) && (i < 5);
            serial_puts("[VIRTIO-GPU] BAR"); serial_putdec(i);
            serial_puts(" raw=0x"); serial_puthex(raw, 8); serial_puts("\n");

            if (is_mem && (raw & ~0xFU) == 0) {
                uint64_t size = is_64 ? 0x40000000ULL : 0x100000ULL;
                uint64_t addr;

                if (is_64) {
                    addr = align_up_u64(next64_addr, size);
                    next64_addr = addr + size;
                } else {
                    addr = align_up_u64(next32_addr, size);
                    next32_addr = addr + size;
                }

                ecam_write32(0x10 + i * 4, (uint32_t)(addr | (raw & 0xF)));
                if (is_64)
                    ecam_write32(0x10 + (i + 1) * 4, (uint32_t)(addr >> 32));

                serial_puts("[VIRTIO-GPU] Programmed BAR"); serial_putdec(i);
                serial_puts("=0x"); serial_puthex(addr, 16);
                serial_puts(" assumed_size=0x"); serial_puthex(size, 16);
                serial_puts("\n");
            }

            if (is_64) {
                i++;  /* skip the high half even when the BAR was already assigned */
            }
        }
    }

    /* Read all 6 BARs (standard PCI has 6 BARs at offsets 0x10-0x24) */
    uint64_t bars[6];
    for (int i = 0; i < 6; i++) {
        uint32_t raw = ecam_read32(0x10 + i * 4);
        if ((raw & 0x1) == 0) {  /* Memory BAR */
            if ((raw & 0x6) == 0x4 && i < 5) {  /* 64-bit BAR */
                uint32_t hi = ecam_read32(0x10 + (i+1) * 4);
                bars[i] = ((uint64_t)hi << 32) | (raw & ~0xFULL);
                bars[i+1] = 0;  /* high part consumed */
                i++;  /* skip next BAR (used for high 32 bits) */
            } else {
                bars[i] = raw & ~0xFULL;
            }
        } else {  /* I/O BAR */
            bars[i] = raw & ~0x3ULL;
        }
        if (bars[i]) {
            serial_puts("[VIRTIO-GPU] BAR");
            serial_putdec(i);
            serial_puts("=0x");
            serial_puthex(bars[i], 16);
            serial_puts("\n");
        }
    }

    /* Map all non-zero BARs */
    for (int i = 0; i < 6; i++) {
        if (bars[i]) {
            paging_map_mmio(bars[i], 64 * 1024);
            serial_puts("[VIRTIO-GPU] Mapped BAR");
            serial_putdec(i);
            serial_puts("=0x"); serial_puthex(bars[i], 16);
            serial_puts("\n");
        }
    }

    /* Parse PCI capabilities */
    int caps = parse_capabilities(bars);
    if (!(caps & 3)) {
        serial_puts("[VIRTIO-GPU] FAIL: missing common_cfg or notify capability\n");
        return;
    }

    /* Device negotiation */
    volatile uint8_t *cfg = gpu.common_cfg;
    cfg[0x14] = 0;                                   /* Reset */
    __asm__ volatile ("mfence" ::: "memory");
    cfg[0x14] = VIRTIO_STATUS_ACK;
    __asm__ volatile ("mfence" ::: "memory");
    cfg[0x14] |= VIRTIO_STATUS_DRIVER;
    __asm__ volatile ("mfence" ::: "memory");

    /* Read the 64-bit device feature vector via the common_cfg window.
     * Offsets 0x00..0x07 are device_feature_select + device_feature.
     * We read lo (select=0) then hi (select=1). */
    *(volatile uint32_t *)(cfg + 0x00) = 0; /* device_feature_select = 0 */
    __asm__ volatile ("mfence" ::: "memory");
    uint32_t feat_lo = *(volatile uint32_t *)(cfg + 0x04);
    *(volatile uint32_t *)(cfg + 0x00) = 1; /* select = 1 (hi half) */
    __asm__ volatile ("mfence" ::: "memory");
    uint32_t feat_hi = *(volatile uint32_t *)(cfg + 0x04);
    gpu.device_features = ((uint64_t)feat_hi << 32) | feat_lo;
    serial_puts("[VIRTIO-GPU] device_features=0x");
    serial_puthex(gpu.device_features, 16);
    serial_puts("\n");

    /* Negotiate only features this split-ring driver knows how to use.
     * Echoing all host bits can accidentally enable queue semantics we
     * do not implement (for example event/ring extensions), which can
     * leave the first controlq command unanswered under QEMU GL. */
    uint64_t accepted_features = gpu.device_features &
        (VIRTIO_GPU_F_VIRGL |
         VIRTIO_GPU_F_EDID |
         VIRTIO_GPU_F_RESOURCE_BLOB |
         VIRTIO_GPU_F_CONTEXT_INIT |
         VIRTIO_F_VERSION_1);
    if (accepted_features & VIRTIO_GPU_F_VIRGL) {
        *(volatile uint32_t *)(cfg + 0x08) = 0; /* driver_feature_select */
        __asm__ volatile ("mfence" ::: "memory");
        *(volatile uint32_t *)(cfg + 0x0C) = (uint32_t)accepted_features;
        *(volatile uint32_t *)(cfg + 0x08) = 1;
        __asm__ volatile ("mfence" ::: "memory");
        *(volatile uint32_t *)(cfg + 0x0C) =
            (uint32_t)(accepted_features >> 32);
        __asm__ volatile ("mfence" ::: "memory");
        serial_puts("[VIRTIO-GPU] accepted_features=0x");
        serial_puthex(accepted_features, 16);
        serial_puts("\n");
    }

    cfg[0x14] |= VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile ("mfence" ::: "memory");
    if (!(cfg[0x14] & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[VIRTIO-GPU] Features negotiation failed\n");
        return;
    }

    /* Setup control virtqueue */
    if (setup_controlq() < 0) {
        serial_puts("[VIRTIO-GPU] controlq setup failed\n");
        return;
    }

    /* Driver OK */
    cfg[0x14] |= VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile ("mfence" ::: "memory");
    serial_puts("[VIRTIO-GPU] Device ready\n");

    /* ── GET_DISPLAY_INFO ─────────────────────────────────────── */
    static struct virtio_gpu_ctrl_hdr cmd_info;
    static struct virtio_gpu_resp_display_info resp_info;
    memset(&cmd_info, 0, sizeof(cmd_info));
    memset(&resp_info, 0, sizeof(resp_info));
    cmd_info.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (gpu_send_cmd(&cmd_info, sizeof(cmd_info), &resp_info, sizeof(resp_info)) == 0) {
        if (resp_info.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            gpu.native_width = resp_info.pmodes[0].r.width;
            gpu.native_height = resp_info.pmodes[0].r.height;
            serial_puts("[VIRTIO-GPU] Native display: ");
            serial_putdec(gpu.native_width); serial_puts("x");
            serial_putdec(gpu.native_height); serial_puts("\n");
        } else {
            serial_puts("[VIRTIO-GPU] GET_DISPLAY_INFO failed\n");
            return;
        }
    } else {
        return;
    }

    /* Use kernel's framebuffer dimensions to match GOP resolution */
    gpu.width = fb_width ? fb_width : 1024;
    gpu.height = fb_height ? fb_height : 768;
    gpu.resource_id = 1;
    gpu.next_resource_id = 2;
    serial_puts("[VIRTIO-GPU] Resource size: ");
    serial_putdec(gpu.width); serial_puts("x");
    serial_putdec(gpu.height); serial_puts("\n");

    /* ── CREATE 2D RESOURCE ───────────────────────────────────── */
    static struct virtio_gpu_resource_create_2d cmd_create;
    static struct virtio_gpu_ctrl_hdr resp_create;
    memset(&cmd_create, 0, sizeof(cmd_create));
    cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd_create.resource_id = gpu.resource_id;
    cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cmd_create.width = gpu.width;
    cmd_create.height = gpu.height;

    if (gpu_send_cmd(&cmd_create, sizeof(cmd_create), &resp_create, sizeof(resp_create)) < 0)
        return;
    serial_puts("[VIRTIO-GPU] Resource 1 created\n");

    /* ── ATTACH BACKING MEMORY ────────────────────────────────── */
    uint64_t fb_size = (uint64_t)gpu.width * gpu.height * 4;
    gpu.fb_size = fb_size;
    void *fb_raw = mem_alloc_aligned(fb_size, 4096);
    if (!fb_raw) { serial_puts("[VIRTIO-GPU] OOM for framebuffer\n"); return; }
    gpu.fb_phys = (uint64_t)fb_raw;  /* physical addr for DMA */
    gpu.framebuffer = (uint32_t *)PHYS_TO_VIRT((uint64_t)fb_raw);  /* virtual for CPU */
    memset(gpu.framebuffer, 0, fb_size);

    /* Command + 1 mem_entry in a single buffer */
    static struct {
        struct virtio_gpu_resource_attach_backing hdr;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) cmd_attach;
    static struct virtio_gpu_ctrl_hdr resp_attach;
    memset(&cmd_attach, 0, sizeof(cmd_attach));
    cmd_attach.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd_attach.hdr.resource_id = gpu.resource_id;
    cmd_attach.hdr.nr_entries = 1;
    cmd_attach.entry.addr = gpu.fb_phys;
    cmd_attach.entry.length = (uint32_t)fb_size;

    if (gpu_send_cmd(&cmd_attach, sizeof(cmd_attach), &resp_attach, sizeof(resp_attach)) < 0)
        return;
    serial_puts("[VIRTIO-GPU] Backing attached\n");

    /* SET_SCANOUT is deferred to the first virtio_gpu_flush() call.
     * This keeps VGA output visible during boot; the compositor takes
     * over when it starts calling display_flip → virtio_gpu_flush. */
    gpu.scanout_active = false;

    gpu.initialized = true;
    virtio_gpu_seed_from_boot_fb();
    serial_puts("[VIRTIO-GPU] ============================\n");
    serial_puts("[VIRTIO-GPU] Display initialized: ");
    serial_putdec(gpu.width); serial_puts("x");
    serial_putdec(gpu.height);
    serial_puts(" BGRA framebuffer LIVE\n");
    serial_puts("[VIRTIO-GPU] ============================\n");
}

/* ── Getters for userspace ────────────────────────────────────── */
uint32_t *virtio_gpu_get_fb(void)   { return gpu.framebuffer; }
uint32_t  virtio_gpu_get_width(void) { return gpu.width; }
uint32_t  virtio_gpu_get_height(void){ return gpu.height; }
bool      virtio_gpu_ready(void)     { return gpu.initialized; }

bool virtio_gpu_get_native_mode(uint32_t *width, uint32_t *height)
{
    if (!gpu.initialized || !gpu.native_width || !gpu.native_height)
        return false;
    if (width) *width = gpu.native_width;
    if (height) *height = gpu.native_height;
    return true;
}

int virtio_gpu_resize(uint32_t width, uint32_t height)
{
    uint64_t new_size;
    if (!virtio_gpu_valid_mode(width, height, &new_size))
        return VIRTIO_GPU_EINVAL;
    if (!gpu.initialized)
        return VIRTIO_GPU_ENOSYS;

    if (width == gpu.width && height == gpu.height)
        return 0;

    void *new_phys_raw = mem_alloc_aligned(new_size, 4096);
    if (!new_phys_raw)
        return VIRTIO_GPU_ENOMEM;

    uint64_t new_phys = (uint64_t)new_phys_raw;
    uint32_t *new_fb = (uint32_t *)PHYS_TO_VIRT(new_phys);
    memset(new_fb, 0, new_size);

    uint32_t new_resource = gpu.next_resource_id++;
    uint32_t old_resource = 0;
    uint64_t old_phys = 0;
    uint64_t old_size = 0;
    bool old_released = false;

    spin_lock(&gpu_cmd_lock);

    int rc = virtio_gpu_create_resource_locked(new_resource, width, height);
    if (rc == 0)
        rc = virtio_gpu_attach_backing_locked(new_resource, new_phys, new_size);
    if (rc == 0)
        rc = virtio_gpu_set_scanout_locked(new_resource, width, height);
    if (rc == 0)
        rc = virtio_gpu_transfer_flush_locked(new_resource, width, height);

    if (rc == 0) {
        old_resource = gpu.resource_id;
        old_phys = gpu.fb_phys;
        old_size = gpu.fb_size;

        gpu.resource_id = new_resource;
        gpu.width = width;
        gpu.height = height;
        gpu.fb_phys = new_phys;
        gpu.fb_size = new_size;
        gpu.framebuffer = new_fb;
        gpu.scanout_active = true;

        if (old_resource) {
            int detach_rc = virtio_gpu_detach_backing_locked(old_resource);
            int unref_rc = virtio_gpu_unref_resource_locked(old_resource);
            old_released = (detach_rc == 0 && unref_rc == 0);
        }
    } else {
        if (rc != VIRTIO_GPU_EIO)
            rc = VIRTIO_GPU_EIO;
        virtio_gpu_detach_backing_locked(new_resource);
        virtio_gpu_unref_resource_locked(new_resource);
    }

    spin_unlock(&gpu_cmd_lock);

    if (rc < 0) {
        virtio_gpu_free_backing(new_phys, new_size);
        serial_puts("[VIRTIO-GPU] Resize failed: ");
        serial_putdec(width);
        serial_puts("x");
        serial_putdec(height);
        serial_puts("\n");
        return rc;
    }

    if (old_resource && old_released) {
        virtio_gpu_free_backing(old_phys, old_size);
    } else if (old_resource) {
        serial_puts("[VIRTIO-GPU] Resize kept old backing allocated: cleanup failed\n");
    }

    serial_puts("[VIRTIO-GPU] Resized scanout to ");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts("\n");
    return 0;
}

/* ── Flush (call after drawing to framebuffer) ────────────────── */
void virtio_gpu_flush(void) {
    if (!gpu.initialized) return;

    spin_lock(&gpu_cmd_lock);

    /* Lazy SET_SCANOUT: activate on first flush so VGA stays visible during boot */
    if (!gpu.scanout_active) {
        static struct virtio_gpu_set_scanout cmd_scanout;
        static struct virtio_gpu_ctrl_hdr resp_scanout;
        memset(&cmd_scanout, 0, sizeof(cmd_scanout));
        cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
        cmd_scanout.r.width = gpu.width;
        cmd_scanout.r.height = gpu.height;
        cmd_scanout.scanout_id = 0;
        cmd_scanout.resource_id = gpu.resource_id;
        if (gpu_send_cmd_unlocked(&cmd_scanout, sizeof(cmd_scanout), &resp_scanout, sizeof(resp_scanout)) == 0) {
            gpu.scanout_active = true;
            serial_puts("[VIRTIO-GPU] Scanout activated (first flush)\n");
        }
    }

    static struct virtio_gpu_transfer_to_host_2d cmd_xfer;
    static struct virtio_gpu_ctrl_hdr resp_xfer;
    memset(&cmd_xfer, 0, sizeof(cmd_xfer));
    cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_xfer.r.width = gpu.width;
    cmd_xfer.r.height = gpu.height;
    cmd_xfer.resource_id = gpu.resource_id;
    gpu_send_cmd_unlocked(&cmd_xfer, sizeof(cmd_xfer), &resp_xfer, sizeof(resp_xfer));

    static struct virtio_gpu_resource_flush cmd_flush;
    static struct virtio_gpu_ctrl_hdr resp_flush;
    memset(&cmd_flush, 0, sizeof(cmd_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.r.width = gpu.width;
    cmd_flush.r.height = gpu.height;
    cmd_flush.resource_id = gpu.resource_id;
    gpu_send_cmd_unlocked(&cmd_flush, sizeof(cmd_flush), &resp_flush, sizeof(resp_flush));

    spin_unlock(&gpu_cmd_lock);
}

/* -- Internal accessors exposed to virtio_gpu_3d.c ------------ */
#include "virtio_gpu_internal.h"

volatile uint8_t *vgpu_common_cfg(void)      { return gpu.common_cfg; }
volatile uint8_t *vgpu_notify_base(void)     { return gpu.notify_base; }
uint32_t          vgpu_notify_off_mult(void) { return gpu.notify_off_mult; }
bool              vgpu_is_initialized(void)  { return gpu.initialized; }
uint64_t          vgpu_device_features(void) { return gpu.device_features; }
uint32_t          vgpu_ecam_read32(uint16_t offset) { return ecam_read32(offset); }
uint64_t          vgpu_hostmem_base(void)    { return gpu.hostmem_base; }
uint64_t          vgpu_hostmem_size(void)    { return gpu.hostmem_size; }
uint64_t          virtio_gpu_host_visible_bytes(void) { return gpu.hostmem_size; }

int vgpu_controlq_submit(const void *cmd, uint32_t cmd_len,
                         void *resp, uint32_t resp_len) {
    /* gpu_send_cmd wants non-const void*; the hardware only reads cmd. */
    return gpu_send_cmd((void *)cmd, cmd_len, resp, resp_len);
}
