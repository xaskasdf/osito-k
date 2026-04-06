/*
 * OsitoK x86-64 — Virtio PCI Transport (MMIO-based)
 *
 * Shared infrastructure for virtio devices (block, network, etc).
 * Implements virtqueue management, device negotiation, and DMA.
 * Supports virtio 1.0+ (modern PCI transport, no legacy).
 *
 * Used by virtio_blk.c and virtio_net.c.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);

/* ── Virtio PCI Capability Offsets ───────────────────────────── */

/* PCI vendor/device for virtio */
#define VIRTIO_PCI_VENDOR   0x1AF4
#define VIRTIO_DEV_NET      0x1041  /* virtio 1.0 network */
#define VIRTIO_DEV_BLK      0x1042  /* virtio 1.0 block */

/* Device status register bits */
#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4

/* ── Virtqueue (Split Ring) ──────────────────────────────────── */

#define VRING_DESC_F_NEXT     1   /* Buffer continues via next field */
#define VRING_DESC_F_WRITE    2   /* Device writes (vs reads) this buffer */

typedef struct __attribute__((packed)) {
    uint64_t addr;       /* Physical address of buffer */
    uint32_t len;        /* Buffer length */
    uint16_t flags;      /* VRING_DESC_F_* */
    uint16_t next;       /* Next descriptor in chain */
} vring_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];     /* Available descriptor indices */
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;         /* Descriptor index */
    uint32_t len;        /* Bytes written by device */
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];
} vring_used_t;

typedef struct {
    uint16_t     size;           /* Number of descriptors (power of 2) */
    uint16_t     free_head;      /* First free descriptor */
    uint16_t     last_used_idx;  /* Last consumed used ring index */
    vring_desc_t  *desc;         /* Descriptor table */
    vring_avail_t *avail;        /* Available ring */
    vring_used_t  *used;         /* Used ring */
} virtqueue_t;

/* ── Virtqueue Setup ─────────────────────────────────────────── */

int virtqueue_init(virtqueue_t *vq, uint16_t size)
{
    vq->size = size;
    vq->free_head = 0;
    vq->last_used_idx = 0;

    /* Allocate descriptor table (16 bytes each, page-aligned) */
    uint64_t desc_size = (uint64_t)size * sizeof(vring_desc_t);
    vq->desc = (vring_desc_t *)mem_alloc_aligned(desc_size, 4096);
    if (!vq->desc) return -1;
    memset(vq->desc, 0, desc_size);

    /* Allocate available ring (4 + 2*size bytes, page-aligned) */
    uint64_t avail_size = 4 + (uint64_t)size * 2;
    vq->avail = (vring_avail_t *)mem_alloc_aligned(avail_size, 4096);
    if (!vq->avail) return -1;
    memset(vq->avail, 0, avail_size);

    /* Allocate used ring (4 + 8*size bytes, page-aligned) */
    uint64_t used_size = 4 + (uint64_t)size * sizeof(vring_used_elem_t);
    vq->used = (vring_used_t *)mem_alloc_aligned(used_size, 4096);
    if (!vq->used) return -1;
    memset(vq->used, 0, used_size);

    /* Chain free descriptors */
    for (uint16_t i = 0; i < size - 1; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = VRING_DESC_F_NEXT;
    }
    vq->desc[size - 1].next = 0;
    vq->desc[size - 1].flags = 0;

    serial_puts("[VIRTIO] Queue init: size=");
    serial_putdec(size);
    serial_puts(" desc=0x");
    serial_puthex((uint64_t)vq->desc, 16);
    serial_puts("\n");
    return 0;
}

/* Add a buffer to the available ring */
int virtqueue_add_buf(virtqueue_t *vq, uint64_t addr, uint32_t len,
                      uint16_t flags)
{
    uint16_t head = vq->free_head;
    if (head >= vq->size) return -1;

    vq->desc[head].addr = addr;
    vq->desc[head].len = len;
    vq->desc[head].flags = flags;
    vq->free_head = vq->desc[head].next;

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx % vq->size;
    vq->avail->ring[avail_idx] = head;
    __asm__ volatile ("sfence" ::: "memory");
    vq->avail->idx++;

    return (int)head;
}

/* Check if device has consumed any buffers */
bool virtqueue_has_used(virtqueue_t *vq)
{
    return vq->last_used_idx != vq->used->idx;
}

/* Get next used buffer (returns descriptor index, -1 if none) */
int virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out)
{
    if (!virtqueue_has_used(vq)) return -1;

    uint16_t idx = vq->last_used_idx % vq->size;
    uint32_t desc_id = vq->used->ring[idx].id;
    if (len_out) *len_out = vq->used->ring[idx].len;

    /* Return descriptor to free list */
    vq->desc[desc_id].next = vq->free_head;
    vq->desc[desc_id].flags = VRING_DESC_F_NEXT;
    vq->free_head = (uint16_t)desc_id;

    vq->last_used_idx++;
    return (int)desc_id;
}

/* ── Virtio Device Negotiation ───────────────────────────────── */

/* Generic virtio-pci device init sequence.
 * bar0: MMIO base for common config.
 * Returns 0 on success. */
int virtio_pci_init(volatile void *common_cfg)
{
    volatile uint8_t *cfg = (volatile uint8_t *)common_cfg;

    /* Reset device */
    cfg[0x14] = 0;  /* device_status = 0 (reset) */
    __asm__ volatile ("mfence" ::: "memory");

    /* Acknowledge */
    cfg[0x14] = VIRTIO_STATUS_ACK;
    __asm__ volatile ("mfence" ::: "memory");

    /* Driver loaded */
    cfg[0x14] |= VIRTIO_STATUS_DRIVER;
    __asm__ volatile ("mfence" ::: "memory");

    /* Feature negotiation (accept all device features for now) */
    cfg[0x14] |= VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile ("mfence" ::: "memory");

    /* Check features OK */
    if (!(cfg[0x14] & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[VIRTIO] Features negotiation failed\n");
        return -1;
    }

    /* Driver ready */
    cfg[0x14] |= VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile ("mfence" ::: "memory");

    serial_puts("[VIRTIO] Device initialized\n");
    return 0;
}
