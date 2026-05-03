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

/* Forward decls for IRQ counters used by TX kick diag.                     */
extern volatile uint32_t i211_isr_count;
extern volatile uint32_t i211_isr_rx_count;

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

    /* Garantizar Address-Valid (RAH[0] bit 31).  El EEPROM normalmente lo
     * deja seteado, pero algunos resets / quirks pueden limpiarlo —
     * sin AV el chip filtra TODOS los unicast hacia nuestra MAC, lo
     * cual deja ARP/broadcast pasando pero rompe ICMP/UDP/TCP entrantes.
     * Re-escribir es idempotente y barato.                                */
    uint32_t ral_w = ((uint32_t)nic.mac[0]) |
                     ((uint32_t)nic.mac[1] <<  8) |
                     ((uint32_t)nic.mac[2] << 16) |
                     ((uint32_t)nic.mac[3] << 24);
    uint32_t rah_w = ((uint32_t)nic.mac[4]) |
                     ((uint32_t)nic.mac[5] <<  8) |
                     (1u << 31);                  /* Address Valid           */
    i211_write(I211_RAL0, ral_w);
    i211_write(I211_RAH0, rah_w);
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

    /* Fill each descriptor con buffer phys address (advanced one-buffer
     * format).  pkt_addr = donde el HW DMA incoming packet; hdr_addr=0
     * (no usamos header splitting).                                       */
    uint64_t rx_bufs_phys_addr = (uint64_t)rx_bufs_phys;
    for (int i = 0; i < I211_RX_RING_SIZE; i++) {
        nic.rx_ring[i].read.pkt_addr =
            rx_bufs_phys_addr + (uint64_t)i * I211_PKT_BUF_SIZE;
        nic.rx_ring[i].read.hdr_addr = 0;
    }

    /* Program ring base address */
    uint64_t ring_phys = (uint64_t)rx_ring_phys;
    i211_write(I211_RDBAL0, (uint32_t)(ring_phys & 0xFFFFFFFF));
    i211_write(I211_RDBAH0, (uint32_t)(ring_phys >> 32));

    /* Ring length in bytes (must be 128-byte aligned) */
    i211_write(I211_RDLEN0, (uint32_t)ring_bytes);

    /* SRRCTL: advanced one-buffer descriptor format (DESCTYPE = 001).
     * El I211 NO soporta legacy descriptors — el datasheet Intel
     * §7.1.5 lo prohíbe explícitamente.  Programar 0 acá deja el HW
     * sin escribir el bit DD nunca → DHCP RX timeout permanente.        *
     * BSIZEPACKET[6:0] = 2 → buffers de 2 KB (encaja con I211_PKT_BUF_  *
     * SIZE y RCTL.BSIZE).                                               */
    uint32_t srrctl = i211_read(I211_SRRCTL0);
    srrctl &= ~(7u << 25);              /* Clear DESCTYPE bits          */
    srrctl |= I211_SRRCTL_DESCTYPE_ADV; /* DESCTYPE = 001 (advanced 1-buf)*/
    srrctl &= ~0x7Fu;                   /* Clear BSIZEPACKET[6:0]       */
    srrctl |= 2;                         /* BSIZEPACKET = 2 → 2 KB        */
    srrctl |= I211_SRRCTL_DROP_EN;
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

    /* Enable receiver in promiscuous mode (UPE+MPE+BAM) so we accept any
     * frame whose dst MAC isn't ours either — eliminates RAR[0] / MTA
     * filter mistakes from the suspect list while we debug why nothing's
     * arriving. SBP also stores bad-CRC frames so we see them.
     * Lockdown to RAR-only filtering after the link comes up clean. */
    i211_write(I211_RCTL, I211_RCTL_EN | I211_RCTL_BAM | I211_RCTL_SECRC |
                           I211_RCTL_BSIZE_2K | I211_RCTL_UPE |
                           I211_RCTL_MPE | I211_RCTL_SBP);

    /* Read RX + TX counters.  Read-to-clear, así que muestran "since
     * boot".  Comparados con el TX kick log nos dicen si el chip
     * realmente tocó el wire o solo "consumió" el descriptor.            */
    uint32_t gprc_init  = i211_read(0x4074);  /* Good Pkts RX            */
    uint32_t bprc_init  = i211_read(0x4078);  /* Broadcast Pkts RX       */
    uint32_t mprc_init  = i211_read(0x407C);  /* Multicast Pkts RX       */
    uint32_t gptc_init  = i211_read(0x4080);  /* Good Pkts TX            */
    uint32_t gotcl_init = i211_read(0x4090);  /* Good Octets TX low      */
    uint32_t bptc_init  = i211_read(0x40F4);  /* Broadcast Pkts TX       */
    uint32_t mptc_init  = i211_read(0x40F0);  /* Multicast Pkts TX       */
    uint32_t txerr_init = i211_read(0x4008);  /* TX Errors (TXERRC)      */
    uint32_t colc_init  = i211_read(0x4028);  /* Collision Count         */
    serial_puts("[I211] HW counters @init: GPRC=");
    serial_puthex(gprc_init, 8);
    serial_puts(" BPRC="); serial_puthex(bprc_init, 8);
    serial_puts(" MPRC="); serial_puthex(mprc_init, 8);
    serial_puts(" GPTC="); serial_puthex(gptc_init, 8);
    serial_puts(" GOTCL="); serial_puthex(gotcl_init, 8);
    serial_puts(" BPTC="); serial_puthex(bptc_init, 8);
    serial_puts(" MPTC="); serial_puthex(mptc_init, 8);
    serial_puts(" TXERR="); serial_puthex(txerr_init, 8);
    serial_puts(" COLC="); serial_puthex(colc_init, 8);
    serial_puts("\n");

    /* Print final RX programming so we can confirm registers stuck. */
    serial_puts("[I211] RX armed: RCTL=");
    serial_puthex(i211_read(I211_RCTL),    8);
    serial_puts(" RDBA=");
    serial_puthex(((uint64_t)i211_read(I211_RDBAH0) << 32) |
                  i211_read(I211_RDBAL0), 16);
    serial_puts(" RDLEN=");
    serial_puthex(i211_read(I211_RDLEN0), 8);
    serial_puts(" RDH=");
    serial_puthex(i211_read(I211_RDH0),   4);
    serial_puts(" RDT=");
    serial_puthex(i211_read(I211_RDT0),   4);
    serial_puts(" RXDCTL=");
    serial_puthex(i211_read(I211_RXDCTL0), 8);
    serial_puts(" SRRCTL=");
    serial_puthex(i211_read(I211_SRRCTL0), 8);
    serial_puts("\n");

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

    /* Enable TX queue.  CRITICAL para advanced descriptors:
     * datasheet I211 §7.2.7.4 dice "When the device is configured to use
     * Advanced Transmit Descriptors, the WTHRESH field of TXDCTL must be
     * set to a value greater than 0".  Sin eso la NIC nunca write-back-ea
     * el bit DD y, en algunos paths, ni siquiera fetchea los descriptores.
     *
     * Linux igb driver usa PTHRESH=31, HTHRESH=1, WTHRESH=1 — copio esos
     * valores que están en producción hace una década.                    */
    uint32_t txdctl = (31u <<  0) |     /* PTHRESH                          */
                      ( 1u <<  8) |     /* HTHRESH                          */
                      ( 1u << 16) |     /* WTHRESH                          */
                      I211_XDCTL_ENABLE;
    i211_write(I211_TXDCTL0, txdctl);

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
    } else {
        serial_puts("[I211] Link down (cable?)\n");
    }
    /* La línea cosmética del NIC con MAC + link la imprime main.c
     * después con el vendor:device real — los drivers solo registran
     * status interno por serial.                                       */

    nic.initialized = true;
    return 0;
}

/* ── Get MAC Address ─────────────────────────────────────────── */

void i211_get_mac(uint8_t mac[6])
{
    memcpy(mac, nic.mac, 6);
}

/* ── Send Packet ─────────────────────────────────────────────── */

/* ── Scatter-gather send (Phase 8: zero-copy) ──────────────────
 *
 * Chains N descriptors (1 per fragment). Only the last carries EOP and
 * RS so we wait for exactly one completion. Legacy descriptor format
 * supports this: the controller concatenates fragments into one wire
 * frame before transmitting.
 *
 * Caller must pass physical addresses for each fragment. Use
 * VIRT_TO_PHYS for upper-half kernel buffers, or pass identity-mapped
 * low-half pointers directly. Total length must fit one Ethernet frame.
 */
int i211_send_sg(const uint64_t frag_phys[], const uint32_t lens[], int n_frags)
{
    if (!nic.initialized || n_frags <= 0 || n_frags > 4) return -1;
    uint32_t total = 0;
    for (int i = 0; i < n_frags; i++) total += lens[i];
    if (total == 0 || total > I211_PKT_BUF_SIZE) return -1;

    /* Wait for enough free descriptors in the ring. olinfo_status bit0
     * is DD on advanced descriptors; cmd_type_len==0 means descriptor
     * has never been used (fresh ring). */
    uint32_t tail = nic.tx_tail;
    for (int spin = 0; spin < 1000000; spin++) {
        uint32_t count_free = 0;
        for (int i = 0; i < n_frags; i++) {
            i211_tx_desc_t *d = &nic.tx_ring[(tail + i) % I211_TX_RING_SIZE];
            if ((d->olinfo_status & I211_TXD_STAT_DD) || d->cmd_type_len == 0) count_free++;
            else break;
        }
        if ((int)count_free >= n_frags) break;
        __asm__ volatile ("pause");
    }

    for (int i = 0; i < n_frags; i++) {
        uint32_t idx = (tail + i) % I211_TX_RING_SIZE;
        i211_tx_desc_t *d = &nic.tx_ring[idx];
        uint32_t cmd = I211_TXD_DTYP_DATA | I211_TXD_CMD_IFCS | I211_TXD_CMD_DEXT;
        if (i == n_frags - 1)
            cmd |= I211_TXD_CMD_EOP | I211_TXD_CMD_RS;
        d->addr = frag_phys[i];
        d->cmd_type_len = cmd | (lens[i] & 0xFFFFu);
        /* PAYLEN goes in olinfo_status[31:14]. For simple non-TSO packets
         * the payload length equals the data length of this fragment. */
        d->olinfo_status = ((uint32_t)lens[i] & 0x3FFFFu) << 14;
    }

    nic.tx_tail = (tail + n_frags) % I211_TX_RING_SIZE;
    wmb();
    i211_write(I211_TDT0, nic.tx_tail);

    /* Poll completion on the last descriptor */
    uint32_t last_idx = (nic.tx_tail + I211_TX_RING_SIZE - 1) % I211_TX_RING_SIZE;
    i211_tx_desc_t *last = &nic.tx_ring[last_idx];
    {
        volatile uint32_t *dd = &last->olinfo_status;
        for (int i = 0; i < 1000000; i++) {
            if (*dd & I211_TXD_STAT_DD) return 0;
            __asm__ volatile ("pause");
        }
    }
    serial_puts("[I211] SG TX timeout\n");
    return -1;
}

int i211_send(const void *data, uint32_t len)
{
    if (!nic.initialized || len == 0 || len > I211_PKT_BUF_SIZE)
        return -1;

    uint32_t tail = nic.tx_tail;
    i211_tx_desc_t *desc = &nic.tx_ring[tail];

    /* Wait for previous descriptor to complete (if reused) */
    for (int i = 0; i < 1000000; i++) {
        if ((desc->olinfo_status & I211_TXD_STAT_DD) || desc->cmd_type_len == 0)
            break;
        __asm__ volatile ("pause");
    }

    /* Copy packet data to TX buffer */
    uint8_t *buf = nic.tx_bufs + (uint64_t)tail * I211_PKT_BUF_SIZE;
    memcpy(buf, data, len);

    /* Fill advanced TX descriptor.
     *  - cmd_type_len: data length | DTYP=data | CMD(EOP|IFCS|RS|DEXT)
     *  - olinfo_status: PAYLEN[31:14] = full payload length, DD cleared
     */
    uint32_t cmd = I211_TXD_DTYP_DATA |
                   I211_TXD_CMD_EOP | I211_TXD_CMD_IFCS |
                   I211_TXD_CMD_RS  | I211_TXD_CMD_DEXT;
    desc->addr           = nic.tx_bufs_phys + (uint64_t)tail * I211_PKT_BUF_SIZE;
    desc->cmd_type_len   = cmd | (len & 0xFFFFu);
    desc->olinfo_status  = ((uint32_t)len & 0x3FFFFu) << 14;

    /* Advance tail and notify hardware */
    nic.tx_tail = (tail + 1) % I211_TX_RING_SIZE;
    wmb();
    i211_write(I211_TDT0, nic.tx_tail);

    /* DEBUG: snapshot HW state right after kicking TDT. If TDH advances
     * past `tail` the chip fetched our descriptor; if it stays at `tail`
     * the chip is ignoring us.  Limit to first 8 sends so we don't flood
     * dmesg once we know what's happening. */
    static int dbg_count = 0;
    if (dbg_count < 64) {
        dbg_count++;
        uint32_t tdh_after = i211_read(I211_TDH0);
        uint32_t tdt_after = i211_read(I211_TDT0);
        uint32_t status    = i211_read(I211_STATUS);
        uint32_t gprc      = i211_read(0x4074);  /* clears on read */
        uint32_t rdh       = i211_read(0x02810); /* RDH0 */
        /* TX-side counters:  GPTC = good pkts TX'eados (≠ frames-en-cola).
         * Si GPTC sigue 0 después del kick, el chip "consumió" el
         * descriptor pero la PHY no driveó el wire — broken PHY o no-snoop
         * leyendo memoria stale.  TXERRC + COLC nos dicen si hubo error.   */
        uint32_t gptc      = i211_read(0x4080);
        uint32_t gotcl     = i211_read(0x4090);
        uint32_t txerr     = i211_read(0x4008);
        uint32_t colc      = i211_read(0x4028);
        serial_puts("[I211] TX kick: len=");      serial_putdec(len);
        serial_puts(" tail=");                    serial_putdec(tail);
        serial_puts(" -> TDH=");                  serial_puthex(tdh_after, 4);
        serial_puts(" TDT=");                     serial_puthex(tdt_after, 4);
        serial_puts(" RDH=");                     serial_puthex(rdh, 4);
        serial_puts(" GPRC=");                    serial_puthex(gprc, 4);
        serial_puts(" GPTC=");                    serial_puthex(gptc, 4);
        serial_puts(" GOTCL=");                   serial_puthex(gotcl, 4);
        serial_puts(" TXERR=");                   serial_puthex(txerr, 4);
        serial_puts(" COLC=");                    serial_puthex(colc, 4);
        serial_puts(" STATUS=");                  serial_puthex(status, 8);
        serial_puts(" desc.cmd_type_len=");       serial_puthex(desc->cmd_type_len, 8);
        serial_puts(" addr=");                    serial_puthex(desc->addr, 16);
        serial_puts(" ISR=");                     serial_puthex(i211_isr_count, 4);
        serial_puts(" RX_ISR=");                  serial_puthex(i211_isr_rx_count, 4);
        serial_puts("\n");
    }

    /* Poll for completion.  Read volatile para evitar que el compilador
     * cache-ee el valor en registro durante el loop.                     */
    volatile uint32_t *dd = &desc->olinfo_status;
    for (int i = 0; i < 1000000; i++) {
        if (*dd & I211_TXD_STAT_DD)
            return 0;
        __asm__ volatile ("pause");
    }

    /* Timeout — dump suficiente estado para diagnosticar.  Lee TDH/TDT/
     * TXDCTL/STATUS/TCTL así sabemos si el chip leyó el descriptor o
     * está ignorándolo, y si la cola sigue habilitada.                   */
    uint32_t tdh    = i211_read(I211_TDH0);
    uint32_t tdt    = i211_read(I211_TDT0);
    uint32_t txdctl = i211_read(I211_TXDCTL0);
    uint32_t tctl   = i211_read(I211_TCTL);
    uint32_t status = i211_read(I211_STATUS);
    serial_puts("[I211] TX timeout. TDH="); serial_puthex(tdh, 8);
    serial_puts(" TDT=");      serial_puthex(tdt, 8);
    serial_puts(" TXDCTL=");   serial_puthex(txdctl, 8);
    serial_puts(" TCTL=");     serial_puthex(tctl, 8);
    serial_puts(" STATUS=");   serial_puthex(status, 8);
    serial_puts("\n           desc.cmd_type_len=");
    serial_puthex(desc->cmd_type_len, 8);
    serial_puts(" olinfo=");   serial_puthex(*dd, 8);
    serial_puts(" addr=");     serial_puthex(desc->addr, 16);
    serial_puts("\n");
    return -1;
}

/* ── Receive Packet ──────────────────────────────────────────── */

int i211_recv(void *buf, uint32_t *len)
{
    if (!nic.initialized) return -1;

    uint32_t tail = nic.rx_tail;
    i211_rx_desc_t *desc = &nic.rx_ring[tail];

    /* Advanced descriptor write-back: status_error[0] = DD.              */
    if (!(desc->wb.status_error & I211_RXD_STAT_DD))
        return -1;  /* No packet                                          */

    /* DEBUG — first 8 RX events: log so we can confirm bidirectional link
     * with the peer. If TX seems silent on the wire but RX sees the peer's
     * frames, the bug is purely on the TX path. */
    static int rx_dbg = 0;
    if (rx_dbg < 8) {
        rx_dbg++;
        serial_puts("[I211] RX: tail=");      serial_putdec(tail);
        serial_puts(" len=");                  serial_putdec(desc->wb.length);
        serial_puts(" status_error=");         serial_puthex(desc->wb.status_error, 8);
        serial_puts("\n");
    }

    uint32_t pkt_len = desc->wb.length;
    if (pkt_len > I211_PKT_BUF_SIZE)
        pkt_len = I211_PKT_BUF_SIZE;

    uint8_t *src = nic.rx_bufs + (uint64_t)tail * I211_PKT_BUF_SIZE;
    memcpy(buf, src, pkt_len);
    *len = pkt_len;

    /* Re-armar descriptor: pkt_addr = mismo buffer físico.               */
    uint64_t buf_phys = kvirt_to_phys(src);
    desc->read.pkt_addr = buf_phys;
    desc->read.hdr_addr = 0;

    /* Avanzar tail.  RDT se escribe DESPUÉS de invalidar el descriptor
     * para evitar carrera con el HW DMA.
     *
     * Per Intel I210/I211 datasheet §7.1.6: RDT points one descriptor
     * BEYOND the last one HW may use.  Linux igb writes RDT=next_to_use
     * (= last-re-armed + 1).  Writing RDT=old_tail (off-by-one) means
     * HW does NOT see the slot we just re-armed until the NEXT consume,
     * starving the ring under bursty load — observed as "37 packets
     * received but only 5 drained per IRQ batch" in the post-pings
     * nic_stats snapshot. */
    nic.rx_tail = (tail + 1) % I211_RX_RING_SIZE;
    wmb();
    i211_write(I211_RDT0, nic.rx_tail);

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

/* Counters for diagnostic — leemos en TX kick log para confirmar que
 * el MSI llega y el ISR fire.  Si i211_isr_count siempre es 0 al hacer
 * TX kick mientras Mac envía frames, MSI no está siendo entregado y el
 * problema es en pci_enable_msi / IDT vector wiring.                    */
volatile uint32_t i211_isr_count;
volatile uint32_t i211_isr_rx_count;

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

    i211_isr_count++;

    /* Read ICR — auto-clears on read */
    uint32_t cause = i211_read(I211_ICR);

    if (cause & I211_ICR_RXT0) {
        i211_isr_rx_count++;
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

/* Live diagnostic dump.  Walks state observable post-boot: ISR/RX_ISR
 * counters, current IMS/ICR/RDH/RDT/GPRC, and irq_pending flag.
 * Useful when the TX-kick log has rolled over its cap. */
extern volatile bool i211_irq_pending;
void i211_print_stats(void)
{
    if (!nic.initialized) { serial_puts("[I211] not initialized\n"); return; }
    uint32_t ims  = i211_read(I211_IMS);
    uint32_t icr  = i211_read(I211_ICR);   /* note: read clears */
    uint32_t rdh  = i211_read(I211_RDH0);
    uint32_t rdt  = i211_read(I211_RDT0);
    uint32_t gprc = i211_read(0x4074);     /* I211_GPRC */
    serial_puts("[I211 stats] ISR=0x");      serial_puthex(i211_isr_count, 8);
    serial_puts(" RX_ISR=0x");               serial_puthex(i211_isr_rx_count, 8);
    serial_puts(" IMS=0x");                  serial_puthex(ims, 8);
    serial_puts(" ICR=0x");                  serial_puthex(icr, 8);
    serial_puts(" RDH=0x");                  serial_puthex(rdh, 4);
    serial_puts(" RDT=0x");                  serial_puthex(rdt, 4);
    serial_puts(" GPRC=0x");                 serial_puthex(gprc, 8);
    serial_puts(" irq_pending=");            serial_puts(i211_irq_pending ? "1" : "0");
    serial_puts("\n");
}
