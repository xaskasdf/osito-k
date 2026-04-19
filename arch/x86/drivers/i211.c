/*
 * OsitoK x86-64 — Intel I211 Gigabit Ethernet Driver
 *
 * Legacy descriptors (16B), single RX/TX queue, polling (no IRQs).
 * Init sequence based on Intel SDM + Linux igb driver.
 *
 * Hardware: Intel I211-AT (8086:1539), BAR0 = 128KB MMIO.
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "i211.h"

/* ── External Functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puthex(uint64_t val, int digits);
extern void fb_putdec(uint64_t val);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* ── Driver State ────────────────────────────────────────────── */

typedef struct {
    volatile void *bar0;

    /* MAC address */
    uint8_t mac[6];

    /* RX ring */
    i211_rx_desc_t *rx_ring;
    uint8_t        *rx_bufs;     /* RX_RING_SIZE * PKT_BUF_SIZE */
    uint32_t        rx_tail;     /* Next descriptor to check */

    /* TX ring */
    i211_tx_desc_t *tx_ring;
    uint8_t        *tx_bufs;       /* TX_RING_SIZE * PKT_BUF_SIZE (virt view) */
    uint64_t        tx_bufs_phys;  /* Physical base — handed to the controller */
    uint32_t        tx_tail;       /* Next descriptor to use */

    bool initialized;
} i211_state_t;

static i211_state_t nic;

/* ── Register Access ─────────────────────────────────────────── */

static uint32_t i211_read(uint32_t reg)
{
    return mmio_read32((volatile void *)((uint64_t)nic.bar0 + reg));
}

static void i211_write(uint32_t reg, uint32_t val)
{
    mmio_write32((volatile void *)((uint64_t)nic.bar0 + reg), val);
}

/* ── Spin delay (~1ms at typical clock speeds) ───────────────── */

static void i211_delay(uint32_t iterations)
{
    for (volatile uint32_t i = 0; i < iterations; i++)
        __asm__ volatile ("pause");
}

/* ── Read MAC Address from RAL/RAH ───────────────────────────── */

static void i211_read_mac(void)
{
    uint32_t ral = i211_read(I211_RAL0);
    uint32_t rah = i211_read(I211_RAH0);

    nic.mac[0] = (uint8_t)(ral);
    nic.mac[1] = (uint8_t)(ral >> 8);
    nic.mac[2] = (uint8_t)(ral >> 16);
    nic.mac[3] = (uint8_t)(ral >> 24);
    nic.mac[4] = (uint8_t)(rah);
    nic.mac[5] = (uint8_t)(rah >> 8);
}

/* ── Setup RX Ring ───────────────────────────────────────────── */

static int i211_setup_rx(void)
{
    uint64_t ring_bytes = (uint64_t)I211_RX_RING_SIZE * sizeof(i211_rx_desc_t);
    uint64_t bufs_bytes = (uint64_t)I211_RX_RING_SIZE * I211_PKT_BUF_SIZE;

    /* Allocate ring (must be 128-byte aligned per Intel spec). Driver
     * keeps virt pointers (CPU access) and converts to phys for the
     * hardware programming below. */
    void *rx_ring_phys = mem_alloc_aligned(ring_bytes, 4096);
    if (!rx_ring_phys) return -1;
    nic.rx_ring = (i211_rx_desc_t *)PHYS_TO_VIRT(rx_ring_phys);

    /* Allocate packet buffers */
    void *rx_bufs_phys = mem_alloc_aligned(bufs_bytes, 4096);
    if (!rx_bufs_phys) return -1;
    nic.rx_bufs = (uint8_t *)PHYS_TO_VIRT(rx_bufs_phys);

    memset(nic.rx_ring, 0, ring_bytes);
    memset(nic.rx_bufs, 0, bufs_bytes);

    /* Fill each descriptor with buffer phys address (hardware DMAs
     * incoming packets into these). */
    uint64_t rx_bufs_phys_addr = (uint64_t)rx_bufs_phys;
    for (int i = 0; i < I211_RX_RING_SIZE; i++) {
        nic.rx_ring[i].addr = rx_bufs_phys_addr + (uint64_t)i * I211_PKT_BUF_SIZE;
        nic.rx_ring[i].status = 0;
    }

    /* Program ring base address */
    uint64_t ring_phys = (uint64_t)rx_ring_phys;
    i211_write(I211_RDBAL0, (uint32_t)(ring_phys & 0xFFFFFFFF));
    i211_write(I211_RDBAH0, (uint32_t)(ring_phys >> 32));

    /* Ring length in bytes (must be 128-byte aligned) */
    i211_write(I211_RDLEN0, (uint32_t)ring_bytes);

    /* SRRCTL: legacy descriptor format (bits 25:27 = 0) */
    uint32_t srrctl = i211_read(I211_SRRCTL0);
    srrctl &= ~(7 << 25);              /* Clear descriptor type bits */
    srrctl |= I211_SRRCTL_DROP_EN;     /* Drop if no descriptors */
    i211_write(I211_SRRCTL0, srrctl);

    /* Set head to 0 */
    i211_write(I211_RDH0, 0);

    /* Enable RX queue */
    i211_write(I211_RXDCTL0, i211_read(I211_RXDCTL0) | I211_XDCTL_ENABLE);

    /* Poll until queue is enabled */
    for (int i = 0; i < 1000000; i++) {
        if (i211_read(I211_RXDCTL0) & I211_XDCTL_ENABLE)
            break;
        __asm__ volatile ("pause");
    }

    /* Set tail to last descriptor (tells HW all descriptors are available) */
    nic.rx_tail = 0;
    i211_write(I211_RDT0, I211_RX_RING_SIZE - 1);

    /* Enable receiver */
    i211_write(I211_RCTL, I211_RCTL_EN | I211_RCTL_BAM | I211_RCTL_SECRC |
                           I211_RCTL_BSIZE_2K);

    return 0;
}

/* ── Setup TX Ring ───────────────────────────────────────────── */

static int i211_setup_tx(void)
{
    uint64_t ring_bytes = (uint64_t)I211_TX_RING_SIZE * sizeof(i211_tx_desc_t);
    uint64_t bufs_bytes = (uint64_t)I211_TX_RING_SIZE * I211_PKT_BUF_SIZE;

    void *tx_ring_phys = mem_alloc_aligned(ring_bytes, 4096);
    if (!tx_ring_phys) return -1;
    nic.tx_ring = (i211_tx_desc_t *)PHYS_TO_VIRT(tx_ring_phys);

    void *tx_bufs_phys = mem_alloc_aligned(bufs_bytes, 4096);
    if (!tx_bufs_phys) return -1;
    nic.tx_bufs = (uint8_t *)PHYS_TO_VIRT(tx_bufs_phys);

    memset(nic.tx_ring, 0, ring_bytes);
    memset(nic.tx_bufs, 0, bufs_bytes);

    /* Stash tx_bufs phys for descriptor fill in i211_send (we need the
     * physical address to hand to the controller, but the CPU access
     * goes through nic.tx_bufs which is the upper-half view). */
    nic.tx_bufs_phys = (uint64_t)tx_bufs_phys;

    /* Program ring base address */
    uint64_t ring_phys = (uint64_t)tx_ring_phys;
    i211_write(I211_TDBAL0, (uint32_t)(ring_phys & 0xFFFFFFFF));
    i211_write(I211_TDBAH0, (uint32_t)(ring_phys >> 32));

    /* Ring length in bytes */
    i211_write(I211_TDLEN0, (uint32_t)ring_bytes);

    /* Head and tail start at 0 */
    i211_write(I211_TDH0, 0);
    i211_write(I211_TDT0, 0);
    nic.tx_tail = 0;

    /* Enable TX queue */
    i211_write(I211_TXDCTL0, i211_read(I211_TXDCTL0) | I211_XDCTL_ENABLE);

    for (int i = 0; i < 1000000; i++) {
        if (i211_read(I211_TXDCTL0) & I211_XDCTL_ENABLE)
            break;
        __asm__ volatile ("pause");
    }

    /* Enable transmitter: EN, pad short packets, collision params for full duplex */
    i211_write(I211_TCTL, I211_TCTL_EN | I211_TCTL_PSP |
                           I211_TCTL_CT(0x0F) | I211_TCTL_COLD(0x3F));

    return 0;
}

/* ── Initialize I211 ─────────────────────────────────────────── */

int __initk i211_init(uint64_t bar0_phys)
{
    memset(&nic, 0, sizeof(nic));
    /* MMIO via the upper-half alias so register access works from
     * any process CR3. */
    nic.bar0 = (volatile void *)PHYS_TO_VIRT(bar0_phys);

    serial_puts("[I211] Initializing, BAR0=");
    serial_puthex(bar0_phys, 16);
    serial_puts("\n");

    /* Sanity check — read STATUS register */
    uint32_t status = i211_read(I211_STATUS);
    if (status == 0xFFFFFFFF) {
        serial_puts("[I211] BAR0 not accessible\n");
        return -1;
    }

    /* Step 1: Disable all interrupts */
    i211_write(I211_IMC, 0xFFFFFFFF);
    i211_read(I211_ICR);    /* Clear pending */

    /* Step 2: Global reset */
    uint32_t ctrl = i211_read(I211_CTRL);
    i211_write(I211_CTRL, ctrl | I211_CTRL_RST);

    /* Wait for reset to complete (~1ms) */
    i211_delay(100000);
    for (int i = 0; i < 1000000; i++) {
        if (!(i211_read(I211_CTRL) & I211_CTRL_RST))
            break;
        __asm__ volatile ("pause");
    }

    /* Step 3: Disable interrupts post-reset */
    i211_write(I211_IMC, 0xFFFFFFFF);
    i211_read(I211_ICR);

    /* Step 4: Set Link Up */
    ctrl = i211_read(I211_CTRL);
    ctrl |= I211_CTRL_SLU;
    ctrl &= ~I211_CTRL_PHY_RST;
    i211_write(I211_CTRL, ctrl);

    /* Step 5: Read MAC address from RAL/RAH */
    i211_read_mac();
    serial_puts("[I211] MAC: ");
    for (int i = 0; i < 6; i++) {
        serial_puthex(nic.mac[i], 2);
        if (i < 5) serial_puts(":");
    }
    serial_puts("\n");

    /* Step 6: Clear Multicast Table Array (128 entries) */
    for (int i = 0; i < 128; i++)
        i211_write(I211_MTA + (uint32_t)i * 4, 0);

    /* Step 7: Setup RX */
    if (i211_setup_rx() < 0) {
        serial_puts("[I211] RX setup failed\n");
        return -1;
    }

    /* Step 8: Setup TX */
    if (i211_setup_tx() < 0) {
        serial_puts("[I211] TX setup failed\n");
        return -1;
    }

    /* Step 9: Check link status */
    /* Wait a bit for link to settle */
    i211_delay(500000);
    status = i211_read(I211_STATUS);

    if (status & I211_STATUS_LU) {
        uint32_t speed = (status & I211_STATUS_SPEED) >> 6;
        const char *speeds[] = { "10", "100", "1000", "1000" };
        serial_puts("[I211] Link up: ");
        serial_puts(speeds[speed]);
        serial_puts(" Mbps");
        if (status & I211_STATUS_FD)
            serial_puts(" Full-Duplex");
        serial_puts("\n");

        fb_puts("\n NIC: Intel I211 ");
        for (int i = 0; i < 6; i++) {
            fb_puthex(nic.mac[i], 2);
            if (i < 5) fb_puts(":");
        }
        fb_puts("\n  Link: ");
        fb_puts(speeds[speed]);
        fb_puts(" Mbps\n");
    } else {
        serial_puts("[I211] Link down (cable?)\n");
        fb_puts("\n NIC: Intel I211 (link down)\n");
    }

    nic.initialized = true;
    return 0;
}

/* ── Get MAC Address ─────────────────────────────────────────── */

void i211_get_mac(uint8_t mac[6])
{
    memcpy(mac, nic.mac, 6);
}

/* ── Send Packet ─────────────────────────────────────────────── */

int i211_send(const void *data, uint32_t len)
{
    if (!nic.initialized || len == 0 || len > I211_PKT_BUF_SIZE)
        return -1;

    uint32_t tail = nic.tx_tail;
    i211_tx_desc_t *desc = &nic.tx_ring[tail];

    /* Wait for previous descriptor to complete (if reused) */
    for (int i = 0; i < 1000000; i++) {
        if (desc->status & I211_TXD_STAT_DD || desc->cmd == 0)
            break;
        __asm__ volatile ("pause");
    }

    /* Copy packet data to TX buffer */
    uint8_t *buf = nic.tx_bufs + (uint64_t)tail * I211_PKT_BUF_SIZE;
    memcpy(buf, data, len);

    /* Fill descriptor — controller needs the physical address. */
    desc->addr = nic.tx_bufs_phys + (uint64_t)tail * I211_PKT_BUF_SIZE;
    desc->length = (uint16_t)len;
    desc->cso = 0;
    desc->cmd = I211_TXD_CMD_EOP | I211_TXD_CMD_IFCS | I211_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    /* Advance tail and notify hardware */
    nic.tx_tail = (tail + 1) % I211_TX_RING_SIZE;
    wmb();
    i211_write(I211_TDT0, nic.tx_tail);

    /* Poll for completion */
    for (int i = 0; i < 1000000; i++) {
        if (desc->status & I211_TXD_STAT_DD)
            return 0;
        __asm__ volatile ("pause");
    }

    serial_puts("[I211] TX timeout\n");
    return -1;
}

/* ── Receive Packet ──────────────────────────────────────────── */

int i211_recv(void *buf, uint32_t *len)
{
    if (!nic.initialized) return -1;

    uint32_t tail = nic.rx_tail;
    i211_rx_desc_t *desc = &nic.rx_ring[tail];

    /* Check if descriptor has been filled by hardware */
    if (!(desc->status & I211_RXD_STAT_DD))
        return -1;  /* No packet */

    /* Copy packet data */
    uint32_t pkt_len = desc->length;
    if (pkt_len > I211_PKT_BUF_SIZE)
        pkt_len = I211_PKT_BUF_SIZE;

    uint8_t *src = nic.rx_bufs + (uint64_t)tail * I211_PKT_BUF_SIZE;
    memcpy(buf, src, pkt_len);
    *len = pkt_len;

    /* Reset descriptor for reuse */
    desc->status = 0;
    desc->length = 0;
    desc->errors = 0;

    /* Advance tail and update RDT */
    uint32_t old_tail = tail;
    nic.rx_tail = (tail + 1) % I211_RX_RING_SIZE;
    wmb();
    i211_write(I211_RDT0, old_tail);

    return 0;
}

/* ── Link Status ─────────────────────────────────────────────── */

bool i211_link_up(void)
{
    if (!nic.initialized) return false;
    return (i211_read(I211_STATUS) & I211_STATUS_LU) != 0;
}

/* ── Interrupt-driven receive (NAPI hybrid) ────────────────────
 *
 * Flow: packet arrives → MSI vector 40 → i211_isr() →
 *       set irq_pending, disable RX interrupt → net_poll()
 *       drains ring → re-enable RX interrupt.
 * ───────────────────────────────────────────────────────────── */

volatile bool i211_irq_pending;

void i211_enable_interrupts(uint8_t pci_bus, uint8_t pci_dev, uint8_t pci_func)
{
    if (!nic.initialized) return;

    /* Configure MSI via PCI (vector 40) */
    extern int pci_enable_msi(uint8_t bus, uint8_t dev, uint8_t func, uint8_t vector);
    pci_enable_msi(pci_bus, pci_dev, pci_func, 40);

    /* Interrupt throttle: ~100us between interrupts (~10K/sec) */
    i211_write(I211_EITR0, 390 << 2);

    /* Enable RX and link status change interrupts */
    i211_write(I211_IMS, I211_ICR_RXT0 | I211_ICR_LSC);

    i211_irq_pending = false;
    serial_puts("[I211] Interrupts enabled (MSI vector 40, NAPI)\n");
}

void i211_isr(void)
{
    if (!nic.initialized) return;

    /* Read ICR — auto-clears on read */
    uint32_t cause = i211_read(I211_ICR);

    if (cause & I211_ICR_RXT0) {
        /* RX packet: disable RX interrupt, set pending flag.
         * net_poll() will drain the ring and re-enable. */
        i211_write(I211_IMC, I211_ICR_RXT0);
        i211_irq_pending = true;
    }

    if (cause & I211_ICR_LSC) {
        bool up = (i211_read(I211_STATUS) & I211_STATUS_LU) != 0;
        serial_puts("[I211] Link ");
        serial_puts(up ? "UP\n" : "DOWN\n");
    }
}

void i211_rx_irq_reenable(void)
{
    if (nic.initialized)
        i211_write(I211_IMS, I211_ICR_RXT0);
}

/* i211_napi_poll is not used — NAPI drain happens inside net_poll()
 * which already has the full packet processing switch. The ISR sets
 * irq_pending, net_poll drains, then calls i211_rx_irq_reenable(). */
