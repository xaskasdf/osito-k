/*
 * OsitoK x86-64 — Virtio Block Device Driver
 *
 * Provides disk I/O via virtio-blk (QEMU, Firecracker, KVM).
 * Uses split virtqueue from virtio.c for request submission.
 * ~10x faster than emulated NVMe in QEMU TCG.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);

/* Virtqueue API from virtio.c */
typedef struct {
    uint16_t     size;
    uint16_t     free_head;
    uint16_t     last_used_idx;
    void        *desc;
    void        *avail;
    void        *used;
} virtqueue_t;

extern int  virtqueue_init(virtqueue_t *vq, uint16_t size);
extern int  virtqueue_add_buf(virtqueue_t *vq, uint64_t addr, uint32_t len,
                              uint16_t flags);
extern bool virtqueue_has_used(virtqueue_t *vq);
extern int  virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out);

/* ── Virtio Block Request ────────────────────────────────────── */

#define VIRTIO_BLK_T_IN   0   /* Read */
#define VIRTIO_BLK_T_OUT  1   /* Write */

typedef struct __attribute__((packed)) {
    uint32_t type;       /* VIRTIO_BLK_T_IN or _OUT */
    uint32_t reserved;
    uint64_t sector;     /* LBA (512-byte sectors) */
} virtio_blk_req_t;

/* ── Driver State ────────────────────────────────────────────── */

static struct {
    bool          ready;
    volatile void *bar0;
    virtqueue_t   vq;
    uint64_t      capacity;    /* Total sectors */
    uint32_t      sector_size; /* Usually 512 */
} vblk;

/* ── Public API ──────────────────────────────────────────────── */

int virtio_blk_init(uint64_t bar0_phys)
{
    paging_map_mmio(bar0_phys, 0x1000);
    vblk.bar0 = (volatile void *)bar0_phys;

    /* Read capacity from device config (offset 0x100 in modern virtio) */
    volatile uint64_t *cap = (volatile uint64_t *)((uint8_t *)vblk.bar0 + 0x100);
    vblk.capacity = *cap;
    vblk.sector_size = 512;

    if (virtqueue_init(&vblk.vq, 128) < 0) return -1;

    vblk.ready = true;
    serial_puts("[VBLK] Virtio block: ");
    serial_putdec(vblk.capacity);
    serial_puts(" sectors (");
    serial_putdec(vblk.capacity * 512 / (1024 * 1024));
    serial_puts(" MB)\n");
    return 0;
}

bool virtio_blk_is_ready(void) { return vblk.ready; }
uint64_t virtio_blk_capacity(void) { return vblk.capacity; }

int virtio_blk_read(uint64_t sector, uint32_t count, void *buf)
{
    if (!vblk.ready) return -1;

    virtio_blk_req_t *req = (virtio_blk_req_t *)mem_alloc_aligned(sizeof(*req), 16);
    if (!req) return -1;
    req->type = VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;

    uint8_t *status = (uint8_t *)mem_alloc_aligned(1, 16);
    if (!status) return -1;
    *status = 0xFF;

    /* Submit: header (device reads) → data (device writes) → status (device writes) */
    virtqueue_add_buf(&vblk.vq, (uint64_t)req, sizeof(*req), 0);
    virtqueue_add_buf(&vblk.vq, (uint64_t)buf, count * 512, 2); /* WRITE flag */
    virtqueue_add_buf(&vblk.vq, (uint64_t)status, 1, 2);

    /* Notify device (write to queue notify register) */
    volatile uint16_t *notify = (volatile uint16_t *)((uint8_t *)vblk.bar0 + 0x50);
    *notify = 0;

    /* Poll for completion */
    for (int i = 0; i < 1000000; i++) {
        if (virtqueue_has_used(&vblk.vq)) {
            virtqueue_get_used(&vblk.vq, NULL);
            return (*status == 0) ? 0 : -1;
        }
        __asm__ volatile ("pause");
    }
    return -1;  /* Timeout */
}

int virtio_blk_write(uint64_t sector, uint32_t count, const void *buf)
{
    if (!vblk.ready) return -1;

    virtio_blk_req_t *req = (virtio_blk_req_t *)mem_alloc_aligned(sizeof(*req), 16);
    if (!req) return -1;
    req->type = VIRTIO_BLK_T_OUT;
    req->reserved = 0;
    req->sector = sector;

    uint8_t *status = (uint8_t *)mem_alloc_aligned(1, 16);
    *status = 0xFF;

    virtqueue_add_buf(&vblk.vq, (uint64_t)req, sizeof(*req), 0);
    virtqueue_add_buf(&vblk.vq, (uint64_t)buf, count * 512, 0);
    virtqueue_add_buf(&vblk.vq, (uint64_t)status, 1, 2);

    volatile uint16_t *notify = (volatile uint16_t *)((uint8_t *)vblk.bar0 + 0x50);
    *notify = 0;

    for (int i = 0; i < 1000000; i++) {
        if (virtqueue_has_used(&vblk.vq)) {
            virtqueue_get_used(&vblk.vq, NULL);
            return (*status == 0) ? 0 : -1;
        }
        __asm__ volatile ("pause");
    }
    return -1;
}
