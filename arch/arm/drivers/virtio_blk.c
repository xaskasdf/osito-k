/*
 * virtio_blk.c -- VirtIO block device driver (MMIO transport)
 *
 * Polling-mode split virtqueue, single request at a time.
 * Uses VirtIO MMIO transport on QEMU virt (addresses 0x0a000000+).
 *
 * VirtIO MMIO spec: https://docs.oasis-open.org/virtio/virtio/v1.1/
 */

#include "virtio_blk.h"
#include "../include/hal.h"
#include "../include/types.h"

/* ── VirtIO MMIO register offsets ────────────────────────── */

#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEAT_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEAT_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028  /* v1: guest page size */
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03C  /* v1: alignment */
#define VIRTIO_MMIO_QUEUE_PFN       0x040  /* v1: page frame number */
#define VIRTIO_MMIO_QUEUE_READY     0x044  /* v2 only */
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0A0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0A4
#define VIRTIO_MMIO_CONFIG          0x100

/* Magic value = "virt" */
#define VIRTIO_MMIO_MAGIC_VALUE     0x74726976

/* Device IDs */
#define VIRTIO_DEV_BLK              2

/* Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8

/* VirtIO-blk request types */
#define VIRTIO_BLK_T_IN            0   /* read */
#define VIRTIO_BLK_T_OUT           1   /* write */
#define VIRTIO_BLK_T_FLUSH         4

/* ── Split virtqueue structures ──────────────────────────── */

#define QUEUE_SIZE  128

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[QUEUE_SIZE];
};

/* VirtIO-blk request header */
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

/* ── MMIO helpers ────────────────────────────────────────── */

static volatile uint8_t *mmio_base;

static inline uint32_t vr32(uint32_t off) { return *(volatile uint32_t *)(mmio_base + off); }
static inline void     vw32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio_base + off) = v; }

/* ── Driver state ────────────────────────────────────────── */

static struct virtq_desc  *vq_desc;
static struct virtq_avail *vq_avail;
static struct virtq_used  *vq_used;
static uint16_t vq_used_last;
static uint32_t mmio_version;

static uint64_t blk_capacity;  /* total sectors (512B each) */
static int      initialized;

/* Per-request buffers (static, one request at a time) */
static struct virtio_blk_req req_hdr  __attribute__((aligned(16)));
static uint8_t               req_status __attribute__((aligned(16)));
static uint8_t               sector_buf[512] __attribute__((aligned(512)));

/* ── QEMU virt VirtIO MMIO addresses ─────────────────────── */

#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_SIZE    0x200
#define VIRTIO_MMIO_COUNT   32

/* Scan MMIO regions for a VirtIO block device */
static volatile uint8_t *find_virtio_blk(void)
{
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        volatile uint8_t *base = (volatile uint8_t *)(VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_SIZE);
        uint32_t magic = *(volatile uint32_t *)base;
        if (magic != VIRTIO_MMIO_MAGIC_VALUE) continue;
        uint32_t dev_id = *(volatile uint32_t *)(base + VIRTIO_MMIO_DEVICE_ID);
        if (dev_id == VIRTIO_DEV_BLK) return base;
    }
    return (void *)0;
}

/* ── Setup split virtqueue ───────────────────────────────── */

/* Align up to power of 2 */
static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int setup_virtqueue(void)
{
    vw32(VIRTIO_MMIO_QUEUE_SEL, 0);
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t max_size = vr32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max_size == 0) return -1;
    uint32_t qsz = (max_size < QUEUE_SIZE) ? max_size : QUEUE_SIZE;
    vw32(VIRTIO_MMIO_QUEUE_NUM, qsz);

    if (mmio_version == 1) {
        /* Legacy layout: desc + avail + padding + used, contiguous, page-aligned.
         * VirtIO 1.0 §2.6.2: desc at offset 0, avail after desc,
         * used at page-aligned boundary after avail. */
        uint64_t desc_size = qsz * sizeof(struct virtq_desc);
        uint64_t avail_size = sizeof(uint16_t) * (3 + qsz);
        uint64_t used_off = align_up(desc_size + avail_size, 4096);
        uint64_t used_size = sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * qsz;
        uint64_t total = used_off + align_up(used_size, 4096);

        void *vq_mem = mem_alloc_aligned(total, 4096);
        if (!vq_mem) return -1;
        memset(vq_mem, 0, total);

        vq_desc  = (struct virtq_desc *)vq_mem;
        vq_avail = (struct virtq_avail *)((uint8_t *)vq_mem + desc_size);
        vq_used  = (struct virtq_used *)((uint8_t *)vq_mem + used_off);
        vq_used_last = 0;

        /* Tell device: page frame number = phys >> 12 */
        uint32_t pfn = (uint32_t)((uint64_t)vq_mem >> 12);
        vw32(VIRTIO_MMIO_QUEUE_PFN, pfn);
    } else {
        /* Modern (v2): separate desc/avail/used addresses */
        void *vq_mem = mem_alloc_aligned(16384, 4096);
        if (!vq_mem) return -1;
        memset(vq_mem, 0, 16384);

        vq_desc  = (struct virtq_desc *)vq_mem;
        vq_avail = (struct virtq_avail *)((uint8_t *)vq_mem + 4096);
        vq_used  = (struct virtq_used *)((uint8_t *)vq_mem + 8192);
        vq_used_last = 0;

        uint64_t desc_pa  = (uint64_t)vq_desc;
        uint64_t avail_pa = (uint64_t)vq_avail;
        uint64_t used_pa  = (uint64_t)vq_used;

        vw32(VIRTIO_MMIO_QUEUE_DESC_LOW,  (uint32_t)desc_pa);
        vw32(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_pa >> 32));
        vw32(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail_pa);
        vw32(VIRTIO_MMIO_QUEUE_AVAIL_HIGH,(uint32_t)(avail_pa >> 32));
        vw32(VIRTIO_MMIO_QUEUE_USED_LOW,  (uint32_t)used_pa);
        vw32(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used_pa >> 32));
        vw32(VIRTIO_MMIO_QUEUE_READY, 1);
    }

    return 0;
}

/* ── Submit request and poll ─────────────────────────────── */

static int virtio_blk_submit(uint32_t type, uint64_t sector, void *data, uint32_t data_len)
{
    /* Descriptor 0: request header */
    req_hdr.type = type;
    req_hdr.reserved = 0;
    req_hdr.sector = sector;

    vq_desc[0].addr  = (uint64_t)&req_hdr;
    vq_desc[0].len   = sizeof(req_hdr);
    vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[0].next  = 1;

    /* Descriptor 1: data buffer */
    vq_desc[1].addr  = (uint64_t)data;
    vq_desc[1].len   = data_len;
    vq_desc[1].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN)
        vq_desc[1].flags |= VIRTQ_DESC_F_WRITE;
    vq_desc[1].next  = 2;

    /* Descriptor 2: status byte */
    req_status = 0xFF;
    vq_desc[2].addr  = (uint64_t)&req_status;
    vq_desc[2].len   = 1;
    vq_desc[2].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[2].next  = 0;

    /* Add to available ring */
    __asm__ volatile("dmb oshst" ::: "memory");  /* store barrier before ring update */
    uint16_t avail_idx = vq_avail->idx;
    vq_avail->ring[avail_idx % QUEUE_SIZE] = 0;
    __asm__ volatile("dmb oshst" ::: "memory");  /* store barrier before idx update */
    vq_avail->idx = avail_idx + 1;
    __asm__ volatile("dmb osh" ::: "memory");    /* full barrier before notify */

    /* Notify device */
    vw32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion */
    for (uint32_t t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb osh" ::: "memory");  /* load barrier for DMA visibility */
        if (vq_used->idx != vq_used_last) {
            vq_used_last++;
            vw32(VIRTIO_MMIO_INTERRUPT_ACK, vr32(VIRTIO_MMIO_INTERRUPT_STATUS));
            return (req_status == 0) ? 0 : -1;
        }
    }

    serial_puts("[VBLK] Timeout!\n");
    return -1;
}

/* ── Public API ──────────────────────────────────────────── */

int virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func, uint64_t bars[6])
{
    (void)bus; (void)dev; (void)func; (void)bars;

    serial_puts("[VBLK] Scanning VirtIO MMIO...\n");

    mmio_base = find_virtio_blk();
    if (!mmio_base) {
        serial_puts("[VBLK] No VirtIO-blk device found\n");
        return -1;
    }

    mmio_version = vr32(VIRTIO_MMIO_VERSION);
    serial_puts("[VBLK] Found at ");
    serial_puthex((uint64_t)mmio_base, 8);
    serial_puts(", version ");
    serial_putdec(mmio_version);
    serial_puts("\n");

    /* Device init sequence */
    vw32(VIRTIO_MMIO_STATUS, 0);  /* Reset */
    __asm__ volatile("dsb sy" ::: "memory");

    vw32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vw32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation */
    if (mmio_version >= 2) {
        /* Modern: negotiate VIRTIO_F_VERSION_1 */
        vw32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
        vw32(VIRTIO_MMIO_DRIVER_FEATURES, 0x01);
        vw32(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
        vw32(VIRTIO_MMIO_DRIVER_FEATURES, 0);

        vw32(VIRTIO_MMIO_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
        __asm__ volatile("dsb sy" ::: "memory");

        if (!(vr32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            serial_puts("[VBLK] Features negotiation failed\n");
            return -1;
        }
    } else {
        /* Legacy (v1): accept features + set guest page size */
        uint32_t dev_feat = vr32(VIRTIO_MMIO_DEVICE_FEATURES);
        vw32(VIRTIO_MMIO_DRIVER_FEATURES, dev_feat);
        vw32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }

    /* Setup virtqueue */
    if (setup_virtqueue() < 0) {
        serial_puts("[VBLK] Queue setup failed\n");
        return -1;
    }

    /* Read capacity from device config (offset 0x100) */
    uint32_t cap_lo = *(volatile uint32_t *)(mmio_base + VIRTIO_MMIO_CONFIG + 0);
    uint32_t cap_hi = *(volatile uint32_t *)(mmio_base + VIRTIO_MMIO_CONFIG + 4);
    blk_capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    /* Mark driver ready */
    vw32(VIRTIO_MMIO_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
         VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    initialized = 1;

    serial_puts("[VBLK] Ready: ");
    serial_putdec(blk_capacity / 2048);
    serial_puts(" MB (");
    serial_putdec(blk_capacity);
    serial_puts(" sectors)\n");

    return 0;
}

#define MAX_SECTORS_PER_REQ  2048  /* 1MB per request */

int virtio_blk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len)
{
    if (!initialized) return -1;

    uint8_t *dst = (uint8_t *)buf;
    while (len > 0) {
        uint64_t sector = byte_offset / 512;
        uint32_t off = byte_offset % 512;

        if (off == 0 && len >= 512) {
            /* Aligned: read multiple sectors at once */
            uint32_t sectors = (uint32_t)(len / 512);
            if (sectors > MAX_SECTORS_PER_REQ) sectors = MAX_SECTORS_PER_REQ;
            uint32_t bytes = sectors * 512;
            if (virtio_blk_submit(VIRTIO_BLK_T_IN, sector, dst, bytes) < 0)
                return -1;
            dst += bytes;
            byte_offset += bytes;
            len -= bytes;
        } else {
            /* Unaligned: read single sector via temp buffer */
            uint32_t chunk = 512 - off;
            if (chunk > len) chunk = (uint32_t)len;
            if (virtio_blk_submit(VIRTIO_BLK_T_IN, sector, sector_buf, 512) < 0)
                return -1;
            memcpy(dst, sector_buf + off, chunk);
            dst += chunk;
            byte_offset += chunk;
            len -= chunk;
        }
    }
    return 0;
}

int virtio_blk_write_bytes(uint64_t byte_offset, const void *buf, uint64_t len)
{
    if (!initialized) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    while (len > 0) {
        uint64_t sector = byte_offset / 512;
        uint32_t off = byte_offset % 512;
        uint32_t chunk = 512 - off;
        if (chunk > len) chunk = (uint32_t)len;

        if (off == 0 && chunk == 512) {
            memcpy(sector_buf, src, 512);
            if (virtio_blk_submit(VIRTIO_BLK_T_OUT, sector, sector_buf, 512) < 0)
                return -1;
        } else {
            if (virtio_blk_submit(VIRTIO_BLK_T_IN, sector, sector_buf, 512) < 0)
                return -1;
            memcpy(sector_buf + off, src, chunk);
            if (virtio_blk_submit(VIRTIO_BLK_T_OUT, sector, sector_buf, 512) < 0)
                return -1;
        }

        src += chunk;
        byte_offset += chunk;
        len -= chunk;
    }
    return 0;
}

int virtio_blk_flush(void)
{
    if (!initialized) return -1;
    return virtio_blk_submit(VIRTIO_BLK_T_FLUSH, 0, sector_buf, 0);
}

uint64_t virtio_blk_capacity(void)
{
    return blk_capacity * 512;
}
