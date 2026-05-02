/*
 * OsitoK x86-64 — Realtek RTL8111/8168/8169 Gigabit Ethernet driver
 *
 * Implementación minimal pero correcta del flujo "C+ mode" (16-byte
 * advanced descriptors) que cubre RTL8111B-H y la familia entera de
 * RTL8168/8169 PCIe NICs.  Inicializado con MMIO en BAR2.
 *
 * Boot sequence (basado en r8169 driver de Linux + datasheet rev 1.51):
 *   1. Reset (CR.RST=1, espera auto-clear)
 *   2. Habilitar acceso config (9346CR.unlock = 0xC0)
 *   3. Leer MAC desde IDR0..5
 *   4. Setup TX/RX descriptor rings (16-byte advanced desc)
 *   5. Programar RDSAR/TNPDS/THPDS, RCR, TCR
 *   6. Enable RX+TX (CR.RE=1, CR.TE=1)
 *   7. Lock config + enable IRQ ROK/RER
 *
 * RX flow: HW DMA al buffer apuntado por desc.buf, escribe length
 * + flags en desc.status, clear OWN bit (transferring ownership a SW).
 * SW polling lee desc.status, procesa, re-set OWN bit.
 */

#include "../include/types.h"
#include "../include/paging.h"
#include "rtl8111.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puthex(uint64_t val, int digits);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);

/* PCI MSI helpers (también usados por i211).                            */
extern void pci_enable_msi(uint8_t bus, uint8_t dev, uint8_t func,
                           uint8_t vector);

/* ── Driver State ──────────────────────────────────────────────────  */

typedef struct {
    volatile uint8_t *bar2;           /* MMIO virtual address                  */

    uint8_t mac[6];

    /* TX ring (cmd ring 0 = normal priority).                              */
    rtl_desc_t *tx_ring;              /* virt view                            */
    uint64_t    tx_ring_phys;
    uint8_t    *tx_bufs;              /* virt view                            */
    uint64_t    tx_bufs_phys;
    uint32_t    tx_tail;              /* next slot to use                     */

    /* RX ring.                                                              */
    rtl_desc_t *rx_ring;
    uint64_t    rx_ring_phys;
    uint8_t    *rx_bufs;
    uint64_t    rx_bufs_phys;
    uint32_t    rx_tail;              /* next slot to check                   */

    bool initialized;
} rtl_state_t;

static rtl_state_t nic;

volatile bool rtl8111_irq_pending;

/* ── MMIO helpers ──────────────────────────────────────────────────  */

static inline uint8_t  rtl_r8 (uint32_t off) { return *(volatile uint8_t  *)(nic.bar2 + off); }
static inline uint16_t rtl_r16(uint32_t off) { return *(volatile uint16_t *)(nic.bar2 + off); }
static inline uint32_t rtl_r32(uint32_t off) { return *(volatile uint32_t *)(nic.bar2 + off); }
static inline void rtl_w8 (uint32_t off, uint8_t  v) { *(volatile uint8_t  *)(nic.bar2 + off) = v; }
static inline void rtl_w16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(nic.bar2 + off) = v; }
static inline void rtl_w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(nic.bar2 + off) = v; }

/* wmb() ya está definida como macro en include/types.h.                    */

static void rtl_delay(uint32_t loops)
{
    while (loops--) __asm__ volatile ("pause");
}

/* ── Reset ─────────────────────────────────────────────────────────  */

static int rtl_reset(void)
{
    rtl_w8(RTL_CR, RTL_CR_RST);
    /* Spec: reset auto-clears in <100ms.  Spin up to ~1s before giving up. */
    for (int i = 0; i < 1000000; i++) {
        if ((rtl_r8(RTL_CR) & RTL_CR_RST) == 0) return 0;
        rtl_delay(100);
    }
    serial_puts("[RTL8111] reset timeout\n");
    return -1;
}

/* ── EEPROM unlock (algunos clones requieren acceso CONFIG explícito) */

static void rtl_unlock_config(void)
{
    rtl_w8(RTL_9346CR, 0xC0);   /* Unlock — write 0xC0 a 9346CR             */
}

static void rtl_lock_config(void)
{
    rtl_w8(RTL_9346CR, 0x00);
}

/* ── MAC read ──────────────────────────────────────────────────────  */

static void rtl_read_mac(void)
{
    /* MAC bytes están en IDR0..5 — accesible byte-a-byte.                  */
    for (int i = 0; i < 6; i++) {
        nic.mac[i] = rtl_r8(RTL_IDR0 + i);
    }
}

/* ── Ring setup ────────────────────────────────────────────────────  */

static int rtl_setup_rings(void)
{
    /* TX descriptor ring — 256-byte aligned por spec.                      */
    uint64_t tx_ring_bytes = (uint64_t)RTL_TX_RING_SIZE * sizeof(rtl_desc_t);
    void *tx_ring_phys = mem_alloc_aligned(tx_ring_bytes, 256);
    if (!tx_ring_phys) return -1;
    nic.tx_ring      = (rtl_desc_t *)PHYS_TO_VIRT(tx_ring_phys);
    nic.tx_ring_phys = (uint64_t)tx_ring_phys;

    void *tx_bufs_phys = mem_alloc_aligned((uint64_t)RTL_TX_RING_SIZE * RTL_PKT_BUF_SIZE, 4096);
    if (!tx_bufs_phys) return -1;
    nic.tx_bufs      = (uint8_t *)PHYS_TO_VIRT(tx_bufs_phys);
    nic.tx_bufs_phys = (uint64_t)tx_bufs_phys;

    for (int i = 0; i < RTL_TX_RING_SIZE; i++) {
        nic.tx_ring[i].status = 0;
        nic.tx_ring[i].vlan   = 0;
        uint64_t buf = nic.tx_bufs_phys + (uint64_t)i * RTL_PKT_BUF_SIZE;
        nic.tx_ring[i].buf_lo = (uint32_t)(buf & 0xFFFFFFFF);
        nic.tx_ring[i].buf_hi = (uint32_t)(buf >> 32);
    }
    /* EOR (End Of Ring) en último descriptor — HW circula al primero.      */
    nic.tx_ring[RTL_TX_RING_SIZE - 1].status |= RTL_DESC_EOR;
    nic.tx_tail = 0;

    /* RX descriptor ring — mismo layout 256-aligned.                       */
    uint64_t rx_ring_bytes = (uint64_t)RTL_RX_RING_SIZE * sizeof(rtl_desc_t);
    void *rx_ring_phys = mem_alloc_aligned(rx_ring_bytes, 256);
    if (!rx_ring_phys) return -1;
    nic.rx_ring      = (rtl_desc_t *)PHYS_TO_VIRT(rx_ring_phys);
    nic.rx_ring_phys = (uint64_t)rx_ring_phys;

    void *rx_bufs_phys = mem_alloc_aligned((uint64_t)RTL_RX_RING_SIZE * RTL_PKT_BUF_SIZE, 4096);
    if (!rx_bufs_phys) return -1;
    nic.rx_bufs      = (uint8_t *)PHYS_TO_VIRT(rx_bufs_phys);
    nic.rx_bufs_phys = (uint64_t)rx_bufs_phys;

    for (int i = 0; i < RTL_RX_RING_SIZE; i++) {
        uint64_t buf = nic.rx_bufs_phys + (uint64_t)i * RTL_PKT_BUF_SIZE;
        nic.rx_ring[i].buf_lo = (uint32_t)(buf & 0xFFFFFFFF);
        nic.rx_ring[i].buf_hi = (uint32_t)(buf >> 32);
        nic.rx_ring[i].vlan   = 0;
        /* OWN=1 para HW, length=buf size en bits 13:0.                     */
        nic.rx_ring[i].status = RTL_DESC_OWN | (RTL_PKT_BUF_SIZE & RTL_DESC_LEN_MASK);
    }
    nic.rx_ring[RTL_RX_RING_SIZE - 1].status |= RTL_DESC_EOR;
    nic.rx_tail = 0;

    return 0;
}

/* ── Public init ───────────────────────────────────────────────────  */

int rtl8111_init(uint64_t bar0_phys, uint64_t bar2_phys)
{
    (void)bar0_phys;       /* PIO BAR — preferimos MMIO en BAR2.            */

    memset(&nic, 0, sizeof(nic));
    nic.bar2 = (volatile uint8_t *)PHYS_TO_VIRT(bar2_phys);

    serial_puts("[RTL8111] init BAR2=");
    serial_puthex(bar2_phys, 16);
    serial_puts("\n");

    /* Sanity: leer CONFIG0 — bus error daría 0xFF.                         */
    uint8_t cfg0 = rtl_r8(RTL_CONFIG0);
    if (cfg0 == 0xFF) {
        serial_puts("[RTL8111] BAR2 not accessible\n");
        return -1;
    }

    /* Step 1: reset                                                        */
    if (rtl_reset() < 0) return -1;

    /* Step 2: unlock config para permitir RCR/TCR writes                   */
    rtl_unlock_config();

    /* Step 3: MAC                                                          */
    rtl_read_mac();
    serial_puts("[RTL8111] MAC: ");
    for (int i = 0; i < 6; i++) {
        serial_puthex(nic.mac[i], 2);
        if (i < 5) serial_puts(":");
    }
    serial_puts("\n");

    /* Step 4: rings                                                        */
    if (rtl_setup_rings() < 0) {
        serial_puts("[RTL8111] ring setup OOM\n");
        return -1;
    }

    /* Step 5: programar descriptor base addresses.                         */
    rtl_w32(RTL_TNPDS_LO, (uint32_t)(nic.tx_ring_phys & 0xFFFFFFFF));
    rtl_w32(RTL_TNPDS_HI, (uint32_t)(nic.tx_ring_phys >> 32));
    rtl_w32(RTL_THPDS_LO, 0);   /* High priority queue — no usada           */
    rtl_w32(RTL_THPDS_HI, 0);
    rtl_w32(RTL_RDSAR_LO, (uint32_t)(nic.rx_ring_phys & 0xFFFFFFFF));
    rtl_w32(RTL_RDSAR_HI, (uint32_t)(nic.rx_ring_phys >> 32));

    /* RX max size 2048 (cubre Ethernet jumbo "no extendido").              */
    rtl_w16(RTL_RMS, 2048);

    /* Max TX packet size — 0x3F = 8192 bytes, suficiente.                  */
    rtl_w8(RTL_MTPS, 0x3F);

    /* C+ Command — habilitar TX/RX checksum offload + auto-FIFO.            *
     * Bit 1 = RxChkSum, bit 3 = MulRW (multi-burst memory R/W).            */
    rtl_w16(RTL_CPCR, 0x0002 | 0x0008);

    /* TX configuration: max DMA burst, IFG estándar.                       */
    rtl_w32(RTL_TCR, RTL_TCR_MXDMA_UNL | RTL_TCR_IFG_NORM);

    /* RX configuration: aceptar broadcast + multicast + own MAC + pkt err. */
    rtl_w32(RTL_RCR, RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_AM |
                     RTL_RCR_RXFTH_UNL | RTL_RCR_MXDMA_UNL);

    /* Multicast filter — accept all (0xFFFFFFFF en MAR0..MAR1).            */
    rtl_w32(RTL_MAR0,     0xFFFFFFFF);
    rtl_w32(RTL_MAR0 + 4, 0xFFFFFFFF);

    /* Step 6: enable RX+TX                                                 */
    rtl_w8(RTL_CR, RTL_CR_RE | RTL_CR_TE);

    /* Step 7: lock config + clear ISR + habilitar ROK/RER por ahora.       *
     * IRQ se habilita después con rtl8111_enable_interrupts().              */
    rtl_lock_config();
    rtl_w16(RTL_ISR, 0xFFFF);    /* clear-on-write — limpia todo            */
    rtl_w16(RTL_IMR, 0);          /* mask todo hasta enable_interrupts()    */

    /* Step 8: link status (informativo — el driver continúa igual).        */
    rtl_delay(500000);
    uint8_t phystatus = rtl_r8(RTL_PHYSTATUS);
    if (phystatus & RTL_PHYSTATUS_LINKSTS) {
        serial_puts("[RTL8111] Link up: ");
        if      (phystatus & RTL_PHYSTATUS_1000) serial_puts("1000");
        else if (phystatus & RTL_PHYSTATUS_100)  serial_puts("100");
        else                                      serial_puts("10");
        serial_puts(" Mbps");
        if (phystatus & RTL_PHYSTATUS_FULLDUP) serial_puts(" Full-Duplex");
        serial_puts("\n");
    } else {
        serial_puts("[RTL8111] Link down (cable?)\n");
    }
    /* fb_puts cosmético lo hace main.c con el vendor:device real.       */

    nic.initialized = true;
    return 0;
}

/* ── Public API helpers ────────────────────────────────────────────  */

void rtl8111_get_mac(uint8_t mac[6])
{
    memcpy(mac, nic.mac, 6);
}

bool rtl8111_link_up(void)
{
    if (!nic.initialized) return false;
    return (rtl_r8(RTL_PHYSTATUS) & RTL_PHYSTATUS_LINKSTS) != 0;
}

/* ── TX ────────────────────────────────────────────────────────────  */

int rtl8111_send(const void *data, uint32_t len)
{
    if (!nic.initialized) return -1;
    if (len == 0 || len > RTL_PKT_BUF_SIZE) return -1;

    uint32_t idx = nic.tx_tail;
    rtl_desc_t *d = &nic.tx_ring[idx];

    /* Esperar a que el descriptor esté libre (OWN=0 = SW dueño).            */
    uint32_t spin = 0;
    while ((d->status & RTL_DESC_OWN) && spin++ < 1000000) {
        __asm__ volatile ("pause");
    }
    if (d->status & RTL_DESC_OWN) {
        serial_puts("[RTL8111] TX desc still owned by HW — drop\n");
        return -1;
    }

    /* Copiar payload al buffer asociado.                                    */
    uint8_t *buf = nic.tx_bufs + (uint64_t)idx * RTL_PKT_BUF_SIZE;
    memcpy(buf, data, len);

    /* Construir status: OWN | FS | LS | length, preservar EOR.             */
    uint32_t status = RTL_DESC_OWN | RTL_DESC_FS | RTL_DESC_LS |
                      (len & RTL_DESC_LEN_MASK);
    if (idx == RTL_TX_RING_SIZE - 1) status |= RTL_DESC_EOR;
    wmb();
    d->status = status;
    wmb();

    /* Despertar al transmitter — escribir TPPOLL.NPQ.                      */
    rtl_w8(RTL_TPPOLL, RTL_TPPOLL_NPQ);

    nic.tx_tail = (idx + 1) % RTL_TX_RING_SIZE;
    return 0;
}

int rtl8111_send_sg(const uint64_t frag_phys[], const uint32_t lens[], int n_frags)
{
    /* RTL8111 soporta scatter-gather con cadena FS/middle/LS, pero por
     * ahora no lo necesitamos (net.c usa SG sólo en el path de I211 zero-
     * copy).  Linealizar: copiar todos los fragmentos a un buffer interno
     * y mandarlo como un único frame.                                      */
    uint8_t tmp[2048];
    uint32_t off = 0;
    for (int i = 0; i < n_frags; i++) {
        if (off + lens[i] > sizeof(tmp)) return -1;
        memcpy(tmp + off, (const void *)PHYS_TO_VIRT(frag_phys[i]), lens[i]);
        off += lens[i];
    }
    return rtl8111_send(tmp, off);
}

/* ── RX ────────────────────────────────────────────────────────────  */

int rtl8111_recv(void *buf, uint32_t *len)
{
    if (!nic.initialized) return -1;

    uint32_t idx = nic.rx_tail;
    rtl_desc_t *d = &nic.rx_ring[idx];

    /* OWN=1 → HW aún dueño del descriptor → no hay paquete listo.          */
    if (d->status & RTL_DESC_OWN) return -1;

    uint32_t status = d->status;
    uint32_t pkt_len = status & RTL_DESC_LEN_MASK;
    if (pkt_len > RTL_PKT_BUF_SIZE) pkt_len = RTL_PKT_BUF_SIZE;

    /* Sólo aceptar si tiene FS y LS (paquete completo en un descriptor).
     * Para frames > buffer-size habría que concatenar — por ahora drop.    */
    if (!(status & RTL_DESC_FS) || !(status & RTL_DESC_LS)) {
        /* Partial — re-arm y skip.                                         */
    } else {
        uint8_t *src = nic.rx_bufs + (uint64_t)idx * RTL_PKT_BUF_SIZE;
        memcpy(buf, src, pkt_len);
        *len = pkt_len;
    }

    /* Re-armar el descriptor: OWN=1, length=buf size, preservar EOR.       */
    uint32_t rearm = RTL_DESC_OWN | (RTL_PKT_BUF_SIZE & RTL_DESC_LEN_MASK);
    if (idx == RTL_RX_RING_SIZE - 1) rearm |= RTL_DESC_EOR;
    wmb();
    d->status = rearm;
    wmb();

    nic.rx_tail = (idx + 1) % RTL_RX_RING_SIZE;

    if (!(status & RTL_DESC_FS) || !(status & RTL_DESC_LS)) return -1;
    return 0;
}

/* ── IRQ wiring ────────────────────────────────────────────────────  */

/* ISR llamado desde el dispatcher común de idt.c (vec==41 → rtl8111_isr).
 * No se registra dinámicamente: el stub isr_stub_41 + el ramal en
 * common_isr_handler ya están wired en kernel/idt.c y kernel/isr_stubs.S. */
void rtl8111_isr(void)
{
    uint16_t isr = rtl_r16(RTL_ISR);
    rtl_w16(RTL_ISR, isr);   /* ack — write-1-to-clear                      */
    if (isr & (RTL_INT_ROK | RTL_INT_RDU | RTL_INT_FOVW)) {
        rtl8111_irq_pending = true;
    }
}

void rtl8111_enable_interrupts(uint8_t bus, uint8_t dev, uint8_t func)
{
    if (!nic.initialized) return;

    /* MSI vía PCI config write — entrega al LAPIC del BSP, vector 41.     */
    pci_enable_msi(bus, dev, func, 41);

    /* Habilitar ROK + RDU + FOVW + LinkChg.                                */
    rtl_w16(RTL_IMR, RTL_INT_ROK | RTL_INT_RDU | RTL_INT_FOVW |
                     RTL_INT_LINKCHG);

    serial_puts("[RTL8111] IRQ enabled (MSI vector 41)\n");
}
