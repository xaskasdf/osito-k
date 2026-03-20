/*
 * virtio_net.c -- VirtIO network device driver (MMIO v1 transport)
 *
 * Two virtqueues: RX (queue 0) and TX (queue 1).
 * RX: pre-allocated buffers, device writes received packets.
 * TX: driver writes packets, notifies, polls completion.
 * Polling mode — no interrupts.
 */

#include "virtio_net.h"
#include "../include/hal.h"
#include "../include/types.h"

/* ── VirtIO MMIO register offsets (same as virtio_blk) ──── */

#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_CONFIG          0x100

#define VIRTIO_MMIO_MAGIC_VALUE     0x74726976
#define VIRTIO_DEV_NET              1

#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8

/* VirtIO-net features */
#define VIRTIO_NET_F_MAC            (1 << 5)

/* VirtIO-net header prepended to every packet */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    /* v1 adds: uint16_t num_buffers (RX only) — but not in legacy mode */
};

/* ── Split virtqueue structures ──────────────────────────── */

#define RX_QUEUE_SIZE  16
#define TX_QUEUE_SIZE  16
#define PKT_BUF_SIZE   2048

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VIRTQ_DESC_F_WRITE    2

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

/* ── MMIO helpers ────────────────────────────────────────── */

static volatile uint8_t *mmio_base;

static inline uint32_t vr32(uint32_t off) { return *(volatile uint32_t *)(mmio_base + off); }
static inline void     vw32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio_base + off) = v; }

/* ── Driver state ────────────────────────────────────────── */

/* RX queue (queue 0) */
static struct virtq_desc  *rx_desc;
static struct virtq_avail *rx_avail;
static struct virtq_used  *rx_used;
static uint16_t rx_used_last;
static uint8_t  rx_bufs[RX_QUEUE_SIZE][PKT_BUF_SIZE] __attribute__((aligned(16)));

/* TX queue (queue 1) */
static struct virtq_desc  *tx_desc;
static struct virtq_avail *tx_avail;
static struct virtq_used  *tx_used;
static uint16_t tx_used_last;
static uint8_t  tx_buf[PKT_BUF_SIZE] __attribute__((aligned(16)));

static uint8_t  dev_mac[6];
static int      initialized;

/* ── QEMU virt VirtIO MMIO scan ──────────────────────────── */

#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_SIZE    0x200
#define VIRTIO_MMIO_COUNT   32

static volatile uint8_t *find_virtio_net(void)
{
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        volatile uint8_t *base = (volatile uint8_t *)(VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_SIZE);
        uint32_t magic = *(volatile uint32_t *)base;
        if (magic != VIRTIO_MMIO_MAGIC_VALUE) continue;
        uint32_t dev_id = *(volatile uint32_t *)(base + VIRTIO_MMIO_DEVICE_ID);
        if (dev_id == VIRTIO_DEV_NET) return base;
    }
    return (void *)0;
}

/* ── Align up ────────────────────────────────────────────── */

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* ── Setup a virtqueue (v1 legacy layout) ────────────────── */

static int setup_queue(int qidx, int qsz,
                       struct virtq_desc **desc_out,
                       struct virtq_avail **avail_out,
                       struct virtq_used **used_out)
{
    vw32(VIRTIO_MMIO_QUEUE_SEL, qidx);
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t max = vr32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0) return -1;
    if ((uint32_t)qsz > max) qsz = max;
    vw32(VIRTIO_MMIO_QUEUE_NUM, qsz);

    /* Legacy layout: desc → avail → (page align) → used */
    uint64_t desc_bytes = qsz * sizeof(struct virtq_desc);
    uint64_t avail_bytes = sizeof(uint16_t) * (3 + qsz);
    uint64_t used_off = align_up(desc_bytes + avail_bytes, 4096);
    uint64_t used_bytes = sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * qsz;
    uint64_t total = used_off + align_up(used_bytes, 4096);

    void *mem = mem_alloc_aligned(total, 4096);
    if (!mem) return -1;
    memset(mem, 0, total);

    *desc_out  = (struct virtq_desc *)mem;
    *avail_out = (struct virtq_avail *)((uint8_t *)mem + desc_bytes);
    *used_out  = (struct virtq_used *)((uint8_t *)mem + used_off);

    vw32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)((uint64_t)mem >> 12));
    return 0;
}

/* ── Fill RX queue with pre-allocated buffers ────────────── */

static void rx_refill(void)
{
    for (int i = 0; i < RX_QUEUE_SIZE; i++) {
        rx_desc[i].addr  = (uint64_t)&rx_bufs[i];
        rx_desc[i].len   = PKT_BUF_SIZE;
        rx_desc[i].flags = VIRTQ_DESC_F_WRITE;  /* device writes here */
        rx_desc[i].next  = 0;
        rx_avail->ring[i] = i;
    }
    __asm__ volatile("dmb oshst" ::: "memory");
    rx_avail->idx = RX_QUEUE_SIZE;
    __asm__ volatile("dmb osh" ::: "memory");
    /* Notify device that RX buffers are available */
    vw32(VIRTIO_MMIO_QUEUE_SEL, 0);
    vw32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

/* ── Public API ──────────────────────────────────────────── */

int virtio_net_init(void)
{
    serial_puts("[VNET] Scanning VirtIO MMIO for network...\n");

    mmio_base = find_virtio_net();
    if (!mmio_base) {
        serial_puts("[VNET] No VirtIO-net device found\n");
        return -1;
    }

    serial_puts("[VNET] Found at ");
    serial_puthex((uint64_t)mmio_base, 8);
    serial_puts("\n");

    /* Reset */
    vw32(VIRTIO_MMIO_STATUS, 0);
    __asm__ volatile("dsb sy" ::: "memory");

    vw32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vw32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation: accept MAC feature */
    uint32_t dev_feat = vr32(VIRTIO_MMIO_DEVICE_FEATURES);
    vw32(VIRTIO_MMIO_DRIVER_FEATURES, dev_feat & VIRTIO_NET_F_MAC);
    vw32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);

    /* Setup RX queue (0) */
    if (setup_queue(0, RX_QUEUE_SIZE, &rx_desc, &rx_avail, &rx_used) < 0) {
        serial_puts("[VNET] RX queue setup failed\n");
        return -1;
    }
    rx_used_last = 0;

    /* Setup TX queue (1) */
    if (setup_queue(1, TX_QUEUE_SIZE, &tx_desc, &tx_avail, &tx_used) < 0) {
        serial_puts("[VNET] TX queue setup failed\n");
        return -1;
    }
    tx_used_last = 0;

    /* Read MAC address from device config */
    volatile uint8_t *cfg = mmio_base + VIRTIO_MMIO_CONFIG;
    for (int i = 0; i < 6; i++) dev_mac[i] = cfg[i];

    /* Mark driver ready */
    vw32(VIRTIO_MMIO_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Fill RX queue with buffers */
    rx_refill();

    initialized = 1;

    serial_puts("[VNET] Ready, MAC ");
    for (int i = 0; i < 6; i++) {
        serial_puthex(dev_mac[i], 2);
        if (i < 5) serial_putc(':');
    }
    serial_puts("\n");

    return 0;
}

void virtio_net_get_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) mac[i] = dev_mac[i];
}

int virtio_net_send(const void *data, uint32_t len)
{
    if (!initialized || len > PKT_BUF_SIZE - sizeof(struct virtio_net_hdr))
        return -1;

    /* Prepend virtio-net header (all zeros = no offload) */
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)tx_buf;
    memset(hdr, 0, sizeof(*hdr));
    memcpy(tx_buf + sizeof(*hdr), data, len);

    uint32_t total = sizeof(struct virtio_net_hdr) + len;

    /* Single descriptor for TX */
    tx_desc[0].addr  = (uint64_t)tx_buf;
    tx_desc[0].len   = total;
    tx_desc[0].flags = 0;  /* device reads this */
    tx_desc[0].next  = 0;

    __asm__ volatile("dmb oshst" ::: "memory");
    uint16_t avail_idx = tx_avail->idx;
    tx_avail->ring[avail_idx % TX_QUEUE_SIZE] = 0;
    __asm__ volatile("dmb oshst" ::: "memory");
    tx_avail->idx = avail_idx + 1;
    __asm__ volatile("dmb osh" ::: "memory");

    /* Notify TX queue (1) */
    vw32(VIRTIO_MMIO_QUEUE_SEL, 1);
    vw32(VIRTIO_MMIO_QUEUE_NOTIFY, 1);

    /* Poll for TX completion */
    for (uint32_t t = 0; t < 10000000; t++) {
        __asm__ volatile("dmb osh" ::: "memory");
        if (tx_used->idx != tx_used_last) {
            tx_used_last++;
            vw32(VIRTIO_MMIO_INTERRUPT_ACK, vr32(VIRTIO_MMIO_INTERRUPT_STATUS));
            return 0;
        }
    }
    return -1;
}

int virtio_net_recv(void *buf, uint32_t *len)
{
    if (!initialized) return -1;

    __asm__ volatile("dmb osh" ::: "memory");
    if (rx_used->idx == rx_used_last) return -1;  /* no packet */

    /* Get completed RX descriptor */
    uint16_t idx = rx_used_last % RX_QUEUE_SIZE;
    struct virtq_used_elem *elem = &rx_used->ring[idx];
    uint32_t desc_id = elem->id;
    uint32_t pkt_len = elem->len;
    rx_used_last++;

    /* Strip virtio-net header */
    uint32_t hdr_sz = sizeof(struct virtio_net_hdr);
    if (pkt_len > hdr_sz) {
        uint32_t data_len = pkt_len - hdr_sz;
        if (data_len > *len) data_len = *len;
        memcpy(buf, rx_bufs[desc_id] + hdr_sz, data_len);
        *len = data_len;
    } else {
        *len = 0;
    }

    /* Re-supply this buffer to the RX queue */
    rx_desc[desc_id].addr  = (uint64_t)&rx_bufs[desc_id];
    rx_desc[desc_id].len   = PKT_BUF_SIZE;
    rx_desc[desc_id].flags = VIRTQ_DESC_F_WRITE;
    __asm__ volatile("dmb oshst" ::: "memory");
    uint16_t avail_idx = rx_avail->idx;
    rx_avail->ring[avail_idx % RX_QUEUE_SIZE] = desc_id;
    __asm__ volatile("dmb oshst" ::: "memory");
    rx_avail->idx = avail_idx + 1;
    __asm__ volatile("dmb osh" ::: "memory");
    vw32(VIRTIO_MMIO_QUEUE_SEL, 0);
    vw32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    vw32(VIRTIO_MMIO_INTERRUPT_ACK, vr32(VIRTIO_MMIO_INTERRUPT_STATUS));

    return 0;
}
