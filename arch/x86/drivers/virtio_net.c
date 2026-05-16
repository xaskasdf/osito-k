/*
 * OsitoA x86-64 — Modern virtio-net driver
 *
 * Implements the virtio 1.0+ PCI transport for QEMU's
 * `-device virtio-net-pci,disable-legacy=on` (PCI device 0x1AF4:0x1041).
 * Walks the PCI capability list to discover the four cfg regions
 * (COMMON, NOTIFY, ISR, DEVICE), negotiates the minimal feature set
 * (VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC), and brings up RX/TX split
 * virtqueues using virtio.c's virtqueue_t primitive.
 *
 * Wire format on send/recv: each frame is prefixed with a 10-byte
 * virtio_net_hdr (no MRG_RXBUF, num_buffers field elided per spec).
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  paging_map_mmio(uint64_t phys, uint64_t size);
extern uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);

/* Reuse virtqueue_t + helpers from virtio.c */
typedef struct {
    uint16_t size;
    uint16_t free_head;
    uint16_t last_used_idx;
    void *desc, *avail, *used;
} virtqueue_t;
extern int  virtqueue_init(virtqueue_t *vq, uint16_t size);
extern int  virtqueue_add_buf(virtqueue_t *vq, uint64_t addr, uint32_t len, uint16_t flags);
extern bool virtqueue_has_used(virtqueue_t *vq);
extern int  virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out);

static inline uint64_t kv2p(const void *v) { return kvirt_to_phys(v); }

/* ── Modern virtio PCI capability layout (spec §4.1.4) ─────────── */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* ── Common-cfg structure offsets (spec §4.1.4.3) ──────────────── */
#define CFG_DEVICE_FEATURE_SELECT  0x00
#define CFG_DEVICE_FEATURE         0x04
#define CFG_DRIVER_FEATURE_SELECT  0x08
#define CFG_DRIVER_FEATURE         0x0C
#define CFG_MSIX_CONFIG            0x10
#define CFG_NUM_QUEUES             0x12
#define CFG_DEVICE_STATUS          0x14
#define CFG_CONFIG_GENERATION      0x15
#define CFG_QUEUE_SELECT           0x16
#define CFG_QUEUE_SIZE             0x18
#define CFG_QUEUE_MSIX_VECTOR      0x1A
#define CFG_QUEUE_ENABLE           0x1C
#define CFG_QUEUE_NOTIFY_OFF       0x1E
#define CFG_QUEUE_DESC             0x20
#define CFG_QUEUE_AVAIL            0x28
#define CFG_QUEUE_USED             0x30

/* Device-status bits (spec §2.1) */
#define VIRTIO_STATUS_ACK          0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_FAILED       0x80

/* Features we care about */
#define VIRTIO_F_VERSION_1     32   /* bit 32 — REQUIRED for modern */
#define VIRTIO_NET_F_MAC       5    /* bit 5  — device provides MAC */

/* virtio-net header.
 *
 * Spec §5.1.6: under VIRTIO_F_VERSION_1 the num_buffers field is
 * ALWAYS present, regardless of MRG_RXBUF. So the header is 12 bytes
 * in modern mode. (Legacy without MRG_RXBUF is 10 bytes — different.)
 * Skipping num_buffers misaligns the wire frame by 2 bytes and the
 * device interprets random payload as the Ethernet header. */
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} virtio_net_hdr_t;
#define VNET_HDR_SIZE  12

/* ── Driver state ──────────────────────────────────────────────── */
/* 256 RX buffers × 2048 B ≈ 512 KiB.  The previous 32 was too small
 * for sustained inbound traffic: under vmnet-shared at ~80 KiB/s, a
 * burst of >32 packets arrived between application drain windows and
 * the device-side used ring filled up, causing virtio to drop further
 * inbound segments and stalling the TCP stream around 11–15 KiB.
 * 256 covers a worst-case ~370 ms drain delay at line rate. */
#define VNET_RX_BUFS 256
#define VNET_BUF_SIZE 2048

static struct {
    bool          ready;
    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    uint32_t          notify_off_multiplier;
    volatile uint8_t *device_cfg;
    virtqueue_t       rxq;        /* queue 0 */
    virtqueue_t       txq;        /* queue 1 */
    uint16_t          rx_notify_off;
    uint16_t          tx_notify_off;
    uint8_t           mac[6];
    uint8_t          *rx_bufs[VNET_RX_BUFS];
    /* TX scratch buffer (header + frame) — single in-flight at a time. */
    uint8_t          *tx_buf;
} vnet;

/* ── MMIO accessors (volatile, native size) ────────────────────── */
static inline void w8 (volatile uint8_t  *p, uint32_t off, uint8_t  v) { p[off] = v; }
static inline uint8_t  r8 (volatile uint8_t  *p, uint32_t off) { return p[off]; }
static inline void w16(volatile uint8_t *p, uint32_t off, uint16_t v)
{ *(volatile uint16_t *)(p + off) = v; }
static inline uint16_t r16(volatile uint8_t *p, uint32_t off)
{ return *(volatile uint16_t *)(p + off); }
static inline void w32(volatile uint8_t *p, uint32_t off, uint32_t v)
{ *(volatile uint32_t *)(p + off) = v; }
static inline uint32_t r32(volatile uint8_t *p, uint32_t off)
{ return *(volatile uint32_t *)(p + off); }
static inline void w64(volatile uint8_t *p, uint32_t off, uint64_t v)
{ *(volatile uint64_t *)(p + off) = v; }

/* ── Capability walk: find all four cfg regions ────────────────── */
static int walk_caps(uint8_t bus, uint8_t dev, uint8_t func,
                     const uint64_t bars[6])
{
    /* Status register at offset 0x06 contains the cap-list bit; in
     * QEMU's PCI mapping that bit is set on virtio-modern. We trust
     * cap_ptr at 0x34 directly. */
    uint8_t cap_ptr = (uint8_t)(pci_cfg_read32(bus, dev, func, 0x34) & 0xFF);
    int found = 0;

    while (cap_ptr && cap_ptr != 0xFF) {
        uint32_t cap0 = pci_cfg_read32(bus, dev, func, cap_ptr);
        uint8_t  cap_id   = (uint8_t)(cap0 & 0xFF);
        uint8_t  cap_next = (uint8_t)((cap0 >> 8) & 0xFF);

        if (cap_id == 0x09) {
            uint8_t  cfg_type = (uint8_t)((cap0 >> 24) & 0xFF);
            uint32_t bar_off  = (uint32_t)((pci_cfg_read32(bus, dev, func, cap_ptr) >> 24) & 0xFF);
            (void)bar_off;
            uint8_t  bar_idx  = (uint8_t)(pci_cfg_read32(bus, dev, func, cap_ptr + 4) & 0xFF);
            uint32_t off      = pci_cfg_read32(bus, dev, func, cap_ptr + 8);
            uint32_t length   = pci_cfg_read32(bus, dev, func, cap_ptr + 12);

            if (bar_idx > 5) { cap_ptr = cap_next; continue; }
            uint64_t bar_phys = bars[bar_idx];
            if (!bar_phys)   { cap_ptr = cap_next; continue; }

            paging_map_mmio(bar_phys + off, length ? length : 0x1000);
            volatile uint8_t *region = (volatile uint8_t *)(bar_phys + off);

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                vnet.common_cfg = region;
                found |= 1;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                vnet.notify_base = region;
                vnet.notify_off_multiplier =
                    pci_cfg_read32(bus, dev, func, cap_ptr + 16);
                found |= 2;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                vnet.device_cfg = region;
                found |= 4;
                break;
            default:
                break;  /* ISR + PCI_CFG ignored for now */
            }
        }
        cap_ptr = cap_next;
    }
    return (found == 7) ? 0 : -1;
}

/* ── Feature negotiation: ack VERSION_1 + NET_F_MAC, drop the rest ─ */
static int negotiate_features(void)
{
    volatile uint8_t *c = vnet.common_cfg;

    /* Read low 32 features (offered) */
    w32(c, CFG_DEVICE_FEATURE_SELECT, 0);
    uint32_t lo = r32(c, CFG_DEVICE_FEATURE);
    /* Read high 32 features */
    w32(c, CFG_DEVICE_FEATURE_SELECT, 1);
    uint32_t hi = r32(c, CFG_DEVICE_FEATURE);

    /* What we accept */
    uint32_t want_lo = 0;
    uint32_t want_hi = 0;
    if (lo & (1u << VIRTIO_NET_F_MAC)) want_lo |= (1u << VIRTIO_NET_F_MAC);
    /* VIRTIO_F_VERSION_1 = bit 32 → set bit 0 of high word */
    if (hi & (1u << (VIRTIO_F_VERSION_1 - 32)))
        want_hi |= (1u << (VIRTIO_F_VERSION_1 - 32));

    w32(c, CFG_DRIVER_FEATURE_SELECT, 0);
    w32(c, CFG_DRIVER_FEATURE, want_lo);
    w32(c, CFG_DRIVER_FEATURE_SELECT, 1);
    w32(c, CFG_DRIVER_FEATURE, want_hi);

    return (want_hi & 1) ? 0 : -1;  /* must have VERSION_1 */
}

/* ── Bring one queue online (descriptor table + ring base) ─────── */
static int setup_queue(virtqueue_t *vq, uint16_t qsel, uint16_t *notify_off_out)
{
    volatile uint8_t *c = vnet.common_cfg;

    w16(c, CFG_QUEUE_SELECT, qsel);
    /* Explicitly disable per-queue MSI-X vector. We're polled — letting
     * this default may leave the queue in a state where the device only
     * notifies the configured (possibly invalid) MSI-X vector instead
     * of letting the driver poll. 0xFFFF = "no vector" per spec. */
    w16(c, CFG_QUEUE_MSIX_VECTOR, 0xFFFF);
    uint16_t qsize = r16(c, CFG_QUEUE_SIZE);
    if (qsize == 0) {
        serial_puts("[VNET] queue not present: ");
        serial_putdec(qsel);
        serial_puts("\n");
        return -1;
    }
    /* Cap to VNET_RX_BUFS so the avail ring exactly matches our pool;
     * with size > pool, indices past `pool size` would point at unused
     * descriptors and confuse the device. */
    if (qsize > VNET_RX_BUFS) qsize = VNET_RX_BUFS;

    if (virtqueue_init(vq, qsize) < 0) return -1;

    w64(c, CFG_QUEUE_DESC,  kv2p(vq->desc));
    w64(c, CFG_QUEUE_AVAIL, kv2p(vq->avail));
    w64(c, CFG_QUEUE_USED,  kv2p(vq->used));
    *notify_off_out = r16(c, CFG_QUEUE_NOTIFY_OFF);
    w16(c, CFG_QUEUE_ENABLE, 1);

    return 0;
}

static void notify_queue(uint16_t qsel, uint16_t notify_off)
{
    volatile uint16_t *p = (volatile uint16_t *)
        (vnet.notify_base + (uint64_t)notify_off * vnet.notify_off_multiplier);
    *p = qsel;
}

static void fill_rx(void)
{
    for (int i = 0; i < VNET_RX_BUFS; i++) {
        if (!vnet.rx_bufs[i]) {
            vnet.rx_bufs[i] = (uint8_t *)mem_alloc_aligned(VNET_BUF_SIZE, 16);
            if (!vnet.rx_bufs[i]) break;
        }
        /* WRITE flag: device writes incoming packet into this buffer. */
        virtqueue_add_buf(&vnet.rxq, kv2p(vnet.rx_bufs[i]),
                          VNET_BUF_SIZE, 2);
    }
    notify_queue(0, vnet.rx_notify_off);
}

/* ── Public API ────────────────────────────────────────────────── */

int virtio_net_init(uint8_t bus, uint8_t dev, uint8_t func,
                    const uint64_t bars[6])
{
    if (walk_caps(bus, dev, func, bars) < 0) {
        serial_puts("[VNET] missing virtio caps (need COMMON+NOTIFY+DEVICE)\n");
        return -1;
    }

    volatile uint8_t *c = vnet.common_cfg;

    /* Reset → ACK → DRIVER */
    w8(c, CFG_DEVICE_STATUS, 0);
    while (r8(c, CFG_DEVICE_STATUS) != 0) { __asm__ volatile("pause"); }
    w8(c, CFG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    w8(c, CFG_DEVICE_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    if (negotiate_features() < 0) {
        serial_puts("[VNET] device doesn't offer VIRTIO_F_VERSION_1\n");
        w8(c, CFG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    w8(c, CFG_DEVICE_STATUS,
       VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if (!(r8(c, CFG_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[VNET] FEATURES_OK rejected\n");
        return -1;
    }

    if (setup_queue(&vnet.rxq, 0, &vnet.rx_notify_off) < 0) return -1;
    if (setup_queue(&vnet.txq, 1, &vnet.tx_notify_off) < 0) return -1;

    /* Read MAC from device config (offset 0, 6 bytes) — only valid if
     * VIRTIO_NET_F_MAC was negotiated, which we asked for. */
    for (int i = 0; i < 6; i++) vnet.mac[i] = vnet.device_cfg[i];

    /* DRIVER_OK: device may now post buffers */
    w8(c, CFG_DEVICE_STATUS,
       VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* Allocate the persistent TX buffer (header + max frame) */
    vnet.tx_buf = (uint8_t *)mem_alloc_aligned(VNET_BUF_SIZE, 16);
    if (!vnet.tx_buf) return -1;

    /* Fill RX ring + notify */
    for (int i = 0; i < VNET_RX_BUFS; i++) vnet.rx_bufs[i] = NULL;
    fill_rx();

    vnet.ready = true;

    serial_puts("[VNET] modern virtio-net ready, MAC=");
    for (int i = 0; i < 6; i++) {
        if (i > 0) serial_puts(":");
        serial_puthex(vnet.mac[i], 2);
    }
    serial_puts(" (rx_notify_off=");
    serial_putdec(vnet.rx_notify_off);
    serial_puts(" tx_notify_off=");
    serial_putdec(vnet.tx_notify_off);
    serial_puts(" mult=");
    serial_putdec(vnet.notify_off_multiplier);
    serial_puts(")\n");
    return 0;
}

bool virtio_net_is_ready(void) { return vnet.ready; }

void virtio_net_get_mac(uint8_t mac_out[6])
{
    for (int i = 0; i < 6; i++) mac_out[i] = vnet.mac[i];
}

int virtio_net_send(const void *data, uint32_t len)
{
    if (!vnet.ready || len > 1514) return -1;

    /* Drain any prior TX completions before queueing this one — keeps
     * the txq from accumulating used entries we never consumed and
     * helps surface device-side issues early. */
    while (virtqueue_has_used(&vnet.txq))
        virtqueue_get_used(&vnet.txq, NULL);

    /* Header is zero (no GSO/CSUM offload) followed by the frame. */
    memset(vnet.tx_buf, 0, VNET_HDR_SIZE);
    memcpy(vnet.tx_buf + VNET_HDR_SIZE, data, len);

    /* READ flag — device pulls from this buffer. */
    virtqueue_add_buf(&vnet.txq, kv2p(vnet.tx_buf),
                      VNET_HDR_SIZE + len, 0);
    notify_queue(1, vnet.tx_notify_off);

    /* Poll for completion. Shorter loop than before — we don't want
     * to hold the CPU for >100 ms if the device's TX completion is
     * lost (modern QEMU virtio-net sometimes does this in poll mode). */
    for (int i = 0; i < 100000; i++) {
        if (virtqueue_has_used(&vnet.txq)) {
            virtqueue_get_used(&vnet.txq, NULL);
            return 0;
        }
        __asm__ volatile ("pause");
    }
    /* TX consumption lost. The packet was likely sent by QEMU but the
     * used-ring update isn't visible. Return success on best-effort —
     * caller can't do anything about it and the next send will drain
     * the stale used entry above. */
    return 0;
}

int virtio_net_recv(void *buf, uint32_t *len_out)
{
    if (!vnet.ready) return -1;

    if (!virtqueue_has_used(&vnet.rxq)) return 0;

    uint32_t total_len;
    int desc_id = virtqueue_get_used(&vnet.rxq, &total_len);
    if (desc_id < 0) return 0;

    int slot = desc_id % VNET_RX_BUFS;
    uint8_t *src = vnet.rx_bufs[slot];

    int rc = 0;
    if (total_len > VNET_HDR_SIZE) {
        uint32_t data_len = total_len - VNET_HDR_SIZE;
        if (data_len > 1514) data_len = 1514;
        memcpy(buf, src + VNET_HDR_SIZE, data_len);
        if (len_out) *len_out = data_len;
        rc = 1;
    }

    /* Re-arm this buffer for the next receive */
    virtqueue_add_buf(&vnet.rxq, kv2p(src), VNET_BUF_SIZE, 2);
    notify_queue(0, vnet.rx_notify_off);
    return rc;
}
