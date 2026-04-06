/*
 * OsitoK x86-64 — Virtio Network Device Driver
 *
 * Provides Ethernet I/O via virtio-net (QEMU, Firecracker, KVM).
 * Uses split virtqueues: RX (queue 0) + TX (queue 1).
 * Drop-in replacement for i211.c when running in VMs.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);

/* Virtqueue API */
typedef struct {
    uint16_t size;
    uint16_t free_head;
    uint16_t last_used_idx;
    void *desc, *avail, *used;
} virtqueue_t;

extern int  virtqueue_init(virtqueue_t *vq, uint16_t size);
extern int  virtqueue_add_buf(virtqueue_t *vq, uint64_t addr, uint32_t len,
                              uint16_t flags);
extern bool virtqueue_has_used(virtqueue_t *vq);
extern int  virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out);

/* ── Virtio Net Header ───────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    /* uint16_t num_buffers; — only with VIRTIO_NET_F_MRG_RXBUF */
} virtio_net_hdr_t;

#define VIRTIO_NET_HDR_SIZE  10

/* ── Driver State ────────────────────────────────────────────── */

#define VNET_RX_BUFS 32
#define VNET_BUF_SIZE 2048

static struct {
    bool          ready;
    volatile void *bar0;
    virtqueue_t   rxq;       /* Queue 0: receive */
    virtqueue_t   txq;       /* Queue 1: transmit */
    uint8_t       mac[6];
    uint8_t      *rx_bufs[VNET_RX_BUFS];  /* Pre-allocated RX buffers */
} vnet;

/* ── Receive Buffer Setup ────────────────────────────────────── */

static void vnet_fill_rx(void)
{
    for (int i = 0; i < VNET_RX_BUFS; i++) {
        if (!vnet.rx_bufs[i]) {
            vnet.rx_bufs[i] = (uint8_t *)mem_alloc_aligned(VNET_BUF_SIZE, 16);
            if (!vnet.rx_bufs[i]) break;
        }
        virtqueue_add_buf(&vnet.rxq, (uint64_t)vnet.rx_bufs[i],
                          VNET_BUF_SIZE, 2);  /* WRITE flag — device fills this */
    }
    /* Notify device that RX buffers are available */
    volatile uint16_t *notify = (volatile uint16_t *)((uint8_t *)vnet.bar0 + 0x50);
    *notify = 0;  /* Queue 0 */
}

/* ── Public API ──────────────────────────────────────────────── */

int virtio_net_init(uint64_t bar0_phys)
{
    paging_map_mmio(bar0_phys, 0x1000);
    vnet.bar0 = (volatile void *)bar0_phys;

    /* Read MAC address from device config (offset 0x100) */
    volatile uint8_t *cfg = (volatile uint8_t *)((uint8_t *)vnet.bar0 + 0x100);
    for (int i = 0; i < 6; i++)
        vnet.mac[i] = cfg[i];

    /* Initialize RX and TX queues */
    if (virtqueue_init(&vnet.rxq, 128) < 0) return -1;
    if (virtqueue_init(&vnet.txq, 128) < 0) return -1;

    /* Fill RX queue with buffers */
    memset(vnet.rx_bufs, 0, sizeof(vnet.rx_bufs));
    vnet_fill_rx();

    vnet.ready = true;

    serial_puts("[VNET] Virtio net: MAC=");
    for (int i = 0; i < 6; i++) {
        if (i > 0) serial_puts(":");
        serial_puthex(vnet.mac[i], 2);
    }
    serial_puts("\n");
    return 0;
}

bool virtio_net_is_ready(void) { return vnet.ready; }

void virtio_net_get_mac(uint8_t mac_out[6])
{
    memcpy(mac_out, vnet.mac, 6);
}

/* Send an Ethernet frame */
int virtio_net_send(const void *data, uint32_t len)
{
    if (!vnet.ready || len > 1514) return -1;

    /* Prepend virtio-net header */
    uint8_t *pkt = (uint8_t *)mem_alloc_aligned(VIRTIO_NET_HDR_SIZE + len, 16);
    if (!pkt) return -1;
    memset(pkt, 0, VIRTIO_NET_HDR_SIZE);
    memcpy(pkt + VIRTIO_NET_HDR_SIZE, data, len);

    virtqueue_add_buf(&vnet.txq, (uint64_t)pkt,
                      VIRTIO_NET_HDR_SIZE + len, 0);  /* READ flag — device reads */

    /* Notify TX queue */
    volatile uint16_t *notify = (volatile uint16_t *)((uint8_t *)vnet.bar0 + 0x50);
    *notify = 1;  /* Queue 1 */

    /* Poll for completion */
    for (int i = 0; i < 100000; i++) {
        if (virtqueue_has_used(&vnet.txq)) {
            virtqueue_get_used(&vnet.txq, NULL);
            return 0;
        }
        __asm__ volatile ("pause");
    }
    return -1;
}

/* Receive next Ethernet frame (non-blocking, returns 0 if no packet) */
int virtio_net_recv(void *buf, uint32_t *len_out)
{
    if (!vnet.ready) return -1;

    if (!virtqueue_has_used(&vnet.rxq)) return 0;  /* No packet */

    uint32_t total_len;
    int desc_id = virtqueue_get_used(&vnet.rxq, &total_len);
    if (desc_id < 0) return 0;

    /* Strip virtio-net header */
    if (total_len <= VIRTIO_NET_HDR_SIZE) {
        /* Re-add buffer to RX queue */
        virtqueue_add_buf(&vnet.rxq, (uint64_t)vnet.rx_bufs[desc_id % VNET_RX_BUFS],
                          VNET_BUF_SIZE, 2);
        return 0;
    }

    uint32_t data_len = total_len - VIRTIO_NET_HDR_SIZE;
    if (data_len > 1514) data_len = 1514;
    memcpy(buf, vnet.rx_bufs[desc_id % VNET_RX_BUFS] + VIRTIO_NET_HDR_SIZE, data_len);
    if (len_out) *len_out = data_len;

    /* Re-add buffer to RX queue */
    virtqueue_add_buf(&vnet.rxq, (uint64_t)vnet.rx_bufs[desc_id % VNET_RX_BUFS],
                      VNET_BUF_SIZE, 2);
    volatile uint16_t *notify = (volatile uint16_t *)((uint8_t *)vnet.bar0 + 0x50);
    *notify = 0;

    return 1;  /* Packet received */
}
