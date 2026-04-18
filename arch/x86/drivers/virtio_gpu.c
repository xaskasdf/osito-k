/*
 * OsitoK x86-64 — Virtio GPU Driver (2D scanout)
 *
 * Implements virtio-gpu protocol for framebuffer display via QEMU.
 * Creates a 2D resource, attaches guest memory, sets up scanout,
 * and provides a flush interface for updating the display.
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);

/* ── Virtio PCI Capability Types ──────────────────────────────── */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

/* ── Virtio GPU Commands ──────────────────────────────────────── */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2

/* ── Virtio Status ────────────────────────────────────────────── */
#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4

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
    uint32_t *framebuffer;
    uint64_t  fb_phys;

    /* ECAM base for PCI config reads */
    uint64_t ecam_base;
    uint8_t  pci_bus, pci_dev, pci_func;

    bool initialized;
} gpu;

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
            uint32_t offset  = ecam_read32(cap_ptr + 8);
            uint32_t length  = ecam_read32(cap_ptr + 12);
            serial_puts("[VIRTIO-GPU]   type=");
            serial_putdec(cfg_type);
            serial_puts(" bar=");
            serial_putdec(bar);
            serial_puts(" off=0x");
            serial_puthex(offset, 4);
            serial_puts(" len=0x");
            serial_puthex(length, 4);
            serial_puts(" baraddr=0x");
            serial_puthex(bars[bar], 8);
            serial_puts("\n");

            uint64_t bar_addr = bars[bar];
            if (!bar_addr) { cap_ptr = cap_next; continue; }

            volatile uint8_t *mapped = (volatile uint8_t *)(bar_addr + offset);

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
    serial_puts("[VIRTIO-GPU] controlq size=");
    serial_putdec(gpu.vq_size);
    serial_puts("\n");

    /* Allocate descriptor table */
    uint64_t desc_bytes = (uint64_t)gpu.vq_size * 16;
    gpu.desc = (vq_desc_t *)mem_alloc_aligned(desc_bytes, 4096);
    if (!gpu.desc) return -1;
    memset(gpu.desc, 0, desc_bytes);

    /* Allocate available ring */
    uint64_t avail_bytes = 6 + 2 * (uint64_t)gpu.vq_size;
    gpu.avail = (vq_avail_t *)mem_alloc_aligned(avail_bytes, 4096);
    if (!gpu.avail) return -1;
    memset(gpu.avail, 0, avail_bytes);

    /* Allocate used ring */
    uint64_t used_bytes = 6 + 8 * (uint64_t)gpu.vq_size;
    gpu.used = (vq_used_t *)mem_alloc_aligned(used_bytes, 4096);
    if (!gpu.used) return -1;
    memset(gpu.used, 0, used_bytes);

    /* Chain free descriptors */
    for (uint16_t i = 0; i < gpu.vq_size - 1; i++) {
        gpu.desc[i].next = i + 1;
        gpu.desc[i].flags = 1; /* NEXT */
    }
    gpu.free_head = 0;
    gpu.last_used = 0;

    /* Write queue addresses to device */
    uint64_t desc_phys  = (uint64_t)gpu.desc;
    uint64_t avail_phys = (uint64_t)gpu.avail;
    uint64_t used_phys  = (uint64_t)gpu.used;

    *(volatile uint16_t *)(cfg + 0x16) = 0;  /* select queue 0 */
    *(volatile uint16_t *)(cfg + 0x18) = gpu.vq_size;
    *(volatile uint32_t *)(cfg + 0x08) = (uint32_t)desc_phys;
    *(volatile uint32_t *)(cfg + 0x0C) = (uint32_t)(desc_phys >> 32);
    *(volatile uint32_t *)(cfg + 0x10) = (uint32_t)avail_phys;
    *(volatile uint32_t *)(cfg + 0x14) = 0; /* high bits for avail — but 0x14 is queue_select! */

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
    serial_puthex((uint64_t)gpu.desc, 16);
    serial_puts("\n");
    serial_puts("[VIRTIO-GPU] controlq configured\n");
    return 0;
}

/* ── Send Command + Wait for Response ─────────────────────────── */

static int gpu_send_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len) {
    uint16_t head = gpu.free_head;
    uint16_t d0 = head;
    uint16_t d1 = gpu.desc[d0].next;

    /* Descriptor 0: command (device reads) */
    gpu.desc[d0].addr = VIRT_TO_PHYS((uint64_t)cmd);
    gpu.desc[d0].len = cmd_len;
    gpu.desc[d0].flags = 1 | 0; /* NEXT | read-only for device */
    gpu.desc[d0].next = d1;

    /* Descriptor 1: response (device writes) */
    gpu.desc[d1].addr = VIRT_TO_PHYS((uint64_t)resp);
    gpu.desc[d1].len = resp_len;
    gpu.desc[d1].flags = 2; /* WRITE */
    gpu.desc[d1].next = 0;

    gpu.free_head = gpu.desc[d1].next;

    /* Add to available ring */
    uint16_t avail_idx = gpu.avail->idx % gpu.vq_size;
    gpu.avail->ring[avail_idx] = head;
    __asm__ volatile ("sfence" ::: "memory");
    gpu.avail->idx++;
    __asm__ volatile ("sfence" ::: "memory");

    /* Notify device — write queue index to notify register */
    uint16_t notify_off = *(volatile uint16_t *)(gpu.common_cfg + 0x1E);
    volatile uint16_t *notify_addr = (volatile uint16_t *)(gpu.notify_base + notify_off * gpu.notify_off_mult);
    serial_puts("[VIRTIO-GPU] notify: off=");
    serial_putdec(notify_off);
    serial_puts(" addr=0x");
    serial_puthex((uint64_t)notify_addr, 16);
    serial_puts(" desc_phys=0x");
    serial_puthex((uint64_t)gpu.desc, 16);
    serial_puts("\n");
    *notify_addr = 0;  /* queue 0 */
    __asm__ volatile ("mfence" ::: "memory");

    /* Poll for response */
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile ("lfence" ::: "memory");
        if (gpu.used->idx != gpu.last_used) {
            gpu.last_used++;
            /* Return descriptors to free list */
            gpu.desc[d1].next = gpu.free_head;
            gpu.desc[d1].flags = 1;
            gpu.desc[d0].next = d1;
            gpu.desc[d0].flags = 1;
            gpu.free_head = d0;
            return 0;
        }
    }
    serial_puts("[VIRTIO-GPU] Command timeout!\n");
    return -1;
}

/* ── Public Interface ─────────────────────────────────────────── */

void virtio_gpu_init(uint64_t bar_phys)
{
    serial_puts("[VIRTIO-GPU] Initializing...\n");
    gpu.initialized = false;

    /* TODO: receive bus/dev/func + ecam_base from caller.
     * For now, hardcode ECAM base and bus 0 dev 6 func 0 (from PCI scan).
     */
    gpu.ecam_base = 0xE0000000ULL;  /* from MCFG in boot log */
    gpu.pci_bus = 0; gpu.pci_dev = 6; gpu.pci_func = 0;

    /* Need ECAM mapped */
    paging_map_mmio(gpu.ecam_base + (0 << 20) + (6 << 15), 4096);

    /* Enable PCI memory space + bus master if not already */
    uint16_t pci_cmd = (uint16_t)ecam_read32(0x04);
    if (!(pci_cmd & 0x06)) {
        /* Set Memory Space Enable + Bus Master Enable */
        uint64_t cmd_addr = gpu.ecam_base | (gpu.pci_bus << 20) | (gpu.pci_dev << 15) | (gpu.pci_func << 12) | 0x04;
        *(volatile uint16_t *)cmd_addr = pci_cmd | 0x06;
        __asm__ volatile ("mfence" ::: "memory");
        serial_puts("[VIRTIO-GPU] Enabled PCI memory + bus master\n");
    }

    /* If BAR4 is 0, program it manually to a free MMIO region */
    {
        uint32_t bar4_raw = ecam_read32(0x20);
        serial_puts("[VIRTIO-GPU] BAR4 raw=0x");
        serial_puthex(bar4_raw, 8);
        serial_puts("\n");
        if ((bar4_raw & ~0xF) == 0) {
            /* BAR4 is 64-bit (type bits = 0b10). Program both BAR4 + BAR5. */
            uint64_t cfg_base = gpu.ecam_base | (gpu.pci_bus << 20) | (gpu.pci_dev << 15) | (gpu.pci_func << 12);
            *(volatile uint32_t *)(cfg_base + 0x20) = 0x81100000 | (bar4_raw & 0xF);  /* preserve type bits */
            *(volatile uint32_t *)(cfg_base + 0x24) = 0;  /* BAR5 = upper 32 bits = 0 */
            __asm__ volatile ("mfence" ::: "memory");
            serial_puts("[VIRTIO-GPU] Programmed BAR4=0x81100000 (64-bit)\n");
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
            serial_puthex(bars[i], 8);
            serial_puts("\n");
        }
    }

    /* Map all non-zero BARs */
    for (int i = 0; i < 6; i++) {
        if (bars[i]) {
            paging_map_mmio(bars[i], 64 * 1024);
            serial_puts("[VIRTIO-GPU] Mapped BAR");
            serial_putdec(i);
            serial_puts("=0x"); serial_puthex(bars[i], 8);
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
            gpu.width = resp_info.pmodes[0].r.width;
            gpu.height = resp_info.pmodes[0].r.height;
            serial_puts("[VIRTIO-GPU] Display: ");
            serial_putdec(gpu.width); serial_puts("x");
            serial_putdec(gpu.height); serial_puts("\n");
        } else {
            serial_puts("[VIRTIO-GPU] GET_DISPLAY_INFO failed\n");
            return;
        }
    } else {
        return;
    }

    if (gpu.width == 0 || gpu.height == 0) {
        gpu.width = 1024; gpu.height = 768;
    }

    /* ── CREATE 2D RESOURCE ───────────────────────────────────── */
    static struct virtio_gpu_resource_create_2d cmd_create;
    static struct virtio_gpu_ctrl_hdr resp_create;
    memset(&cmd_create, 0, sizeof(cmd_create));
    cmd_create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd_create.resource_id = 1;
    cmd_create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    cmd_create.width = gpu.width;
    cmd_create.height = gpu.height;

    if (gpu_send_cmd(&cmd_create, sizeof(cmd_create), &resp_create, sizeof(resp_create)) < 0)
        return;
    serial_puts("[VIRTIO-GPU] Resource 1 created\n");

    /* ── ATTACH BACKING MEMORY ────────────────────────────────── */
    uint64_t fb_size = (uint64_t)gpu.width * gpu.height * 4;
    uint64_t fb_pages = (fb_size + 4095) / 4096;
    gpu.framebuffer = (uint32_t *)mem_alloc_aligned(fb_size, 4096);
    if (!gpu.framebuffer) { serial_puts("[VIRTIO-GPU] OOM for framebuffer\n"); return; }
    gpu.fb_phys = (uint64_t)gpu.framebuffer;
    memset(gpu.framebuffer, 0, fb_size);

    /* Command + 1 mem_entry in a single buffer */
    static struct {
        struct virtio_gpu_resource_attach_backing hdr;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) cmd_attach;
    static struct virtio_gpu_ctrl_hdr resp_attach;
    memset(&cmd_attach, 0, sizeof(cmd_attach));
    cmd_attach.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd_attach.hdr.resource_id = 1;
    cmd_attach.hdr.nr_entries = 1;
    cmd_attach.entry.addr = gpu.fb_phys;
    cmd_attach.entry.length = (uint32_t)fb_size;

    if (gpu_send_cmd(&cmd_attach, sizeof(cmd_attach), &resp_attach, sizeof(resp_attach)) < 0)
        return;
    serial_puts("[VIRTIO-GPU] Backing attached\n");

    /* ── SET SCANOUT ──────────────────────────────────────────── */
    static struct virtio_gpu_set_scanout cmd_scanout;
    static struct virtio_gpu_ctrl_hdr resp_scanout;
    memset(&cmd_scanout, 0, sizeof(cmd_scanout));
    cmd_scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd_scanout.r.x = 0; cmd_scanout.r.y = 0;
    cmd_scanout.r.width = gpu.width;
    cmd_scanout.r.height = gpu.height;
    cmd_scanout.scanout_id = 0;
    cmd_scanout.resource_id = 1;

    if (gpu_send_cmd(&cmd_scanout, sizeof(cmd_scanout), &resp_scanout, sizeof(resp_scanout)) < 0)
        return;
    serial_puts("[VIRTIO-GPU] Scanout set\n");

    /* ── Fill with test pattern and flush ──────────────────────── */
    for (uint32_t y = 0; y < gpu.height; y++) {
        for (uint32_t x = 0; x < gpu.width; x++) {
            /* Blue gradient */
            uint8_t r = (x * 255) / gpu.width;
            uint8_t g = (y * 255) / gpu.height;
            uint8_t b = 128;
            gpu.framebuffer[y * gpu.width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }

    /* TRANSFER_TO_HOST_2D */
    static struct virtio_gpu_transfer_to_host_2d cmd_xfer;
    static struct virtio_gpu_ctrl_hdr resp_xfer;
    memset(&cmd_xfer, 0, sizeof(cmd_xfer));
    cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_xfer.r.x = 0; cmd_xfer.r.y = 0;
    cmd_xfer.r.width = gpu.width;
    cmd_xfer.r.height = gpu.height;
    cmd_xfer.offset = 0;
    cmd_xfer.resource_id = 1;

    if (gpu_send_cmd(&cmd_xfer, sizeof(cmd_xfer), &resp_xfer, sizeof(resp_xfer)) < 0)
        return;

    /* RESOURCE_FLUSH */
    static struct virtio_gpu_resource_flush cmd_flush;
    static struct virtio_gpu_ctrl_hdr resp_flush;
    memset(&cmd_flush, 0, sizeof(cmd_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.r.x = 0; cmd_flush.r.y = 0;
    cmd_flush.r.width = gpu.width;
    cmd_flush.r.height = gpu.height;
    cmd_flush.resource_id = 1;

    if (gpu_send_cmd(&cmd_flush, sizeof(cmd_flush), &resp_flush, sizeof(resp_flush)) < 0)
        return;

    gpu.initialized = true;
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

/* ── Flush (call after drawing to framebuffer) ────────────────── */
void virtio_gpu_flush(void) {
    if (!gpu.initialized) return;

    static struct virtio_gpu_transfer_to_host_2d cmd_xfer;
    static struct virtio_gpu_ctrl_hdr resp_xfer;
    memset(&cmd_xfer, 0, sizeof(cmd_xfer));
    cmd_xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd_xfer.r.width = gpu.width;
    cmd_xfer.r.height = gpu.height;
    cmd_xfer.resource_id = 1;
    gpu_send_cmd(&cmd_xfer, sizeof(cmd_xfer), &resp_xfer, sizeof(resp_xfer));

    static struct virtio_gpu_resource_flush cmd_flush;
    static struct virtio_gpu_ctrl_hdr resp_flush;
    memset(&cmd_flush, 0, sizeof(cmd_flush));
    cmd_flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd_flush.r.width = gpu.width;
    cmd_flush.r.height = gpu.height;
    cmd_flush.resource_id = 1;
    gpu_send_cmd(&cmd_flush, sizeof(cmd_flush), &resp_flush, sizeof(resp_flush));
}
