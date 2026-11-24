/*
 * OsitoK x86-64 — Realtek RTL8111/8168/8169 Gigabit Ethernet driver
 *
 * Targets:
 *   - RTL8111H integrated en ASUS ROG STRIX B450-F (10EC:8168)
 *   - Familia entera RTL_GIGA_MAC (8111B-H, 8168E-EP, 8101G, ...)
 *
 * Diseño: 16-byte advanced descriptors (single ring TX + RX), polling
 * con IRQ-driven receive (NAPI hybrid, mismo modelo que i211).  BAR2
 * MMIO (preferido) o BAR0 PIO si el firmware lo configuró así.
 */

#ifndef OSITOK_RTL8111_H
#define OSITOK_RTL8111_H

#include "../include/types.h"

/* ── Public API ─────────────────────────────────────────────────────
 * Mismas signatures que i211 — el wrapper en kernel/net.c puede
 * dispatchar a uno u otro vía la abstracción nic_dispatch.            */

int  rtl8111_init(uint64_t bar0_phys, uint64_t bar2_phys);
void rtl8111_get_mac(uint8_t mac[6]);
int  rtl8111_send(const void *data, uint32_t len);
int  rtl8111_recv(void *buf, uint32_t *len);
bool rtl8111_link_up(void);
int  rtl8111_send_sg(const uint64_t frag_phys[], const uint32_t lens[], int n_frags);
void rtl8111_enable_interrupts(uint8_t pci_bus, uint8_t pci_dev, uint8_t pci_func);

extern volatile bool rtl8111_irq_pending;

/* ── Register offsets (BAR2 MMIO) ───────────────────────────────────
 * Source: Realtek RTL8169 datasheet rev 1.51 + Linux r8169 driver.    */

#define RTL_IDR0          0x00      /* MAC address byte 0..3              */
#define RTL_IDR4          0x04      /* MAC address byte 4..5              */
#define RTL_MAR0          0x08      /* Multicast filter (8 bytes)         */
#define RTL_TNPDS_LO      0x20      /* TX Normal Priority Descriptor Start*/
#define RTL_TNPDS_HI      0x24
#define RTL_THPDS_LO      0x28      /* TX High Priority Descriptor Start  */
#define RTL_THPDS_HI      0x2C
#define RTL_FLASH         0x30
#define RTL_ERSR          0x36      /* Early RX Status                    */
#define RTL_CR            0x37      /* Command Register (1 byte)          */
#define RTL_TPPOLL        0x38      /* Transmit Priority Polling          */
#define RTL_IMR           0x3C      /* Interrupt Mask                     */
#define RTL_ISR           0x3E      /* Interrupt Status                   */
#define RTL_TCR           0x40      /* TX Configuration                   */
#define RTL_RCR           0x44      /* RX Configuration                   */
#define RTL_TCTR          0x48      /* Timer Counter                      */
#define RTL_MPC           0x4C      /* Missed Packet Counter              */
#define RTL_9346CR        0x50      /* EEPROM Command                     */
#define RTL_CONFIG0       0x51
#define RTL_CONFIG1       0x52
#define RTL_CONFIG2       0x53
#define RTL_CONFIG3       0x54
#define RTL_CONFIG4       0x55
#define RTL_CONFIG5       0x56
#define RTL_TIMER_INT     0x58
#define RTL_PHYAR         0x60      /* PHY Access Register                */
#define RTL_TBI_STATUS    0x68
#define RTL_PHYSTATUS     0x6C      /* PHY Status (1 byte)                */
#define RTL_RMS           0xDA      /* RX Maximum Size                    */
#define RTL_CPCR          0xE0      /* C+ Command                         */
#define RTL_RDSAR_LO      0xE4      /* RX Descriptor Start                */
#define RTL_RDSAR_HI      0xE8
#define RTL_MTPS          0xEC      /* Max TX Packet Size                 */

/* ── Command Register (RTL_CR) bits ────────────────────────────────  */
#define RTL_CR_RST        0x10      /* Reset (auto-clear when done)       */
#define RTL_CR_RE         0x08      /* Receiver Enable                    */
#define RTL_CR_TE         0x04      /* Transmitter Enable                 */

/* ── TX Polling Register (RTL_TPPOLL) bits ─────────────────────────  */
#define RTL_TPPOLL_NPQ    0x40      /* Normal Priority Queue Polling      */
#define RTL_TPPOLL_HPQ    0x80

/* ── RX Configuration Register (RTL_RCR) bits ──────────────────────  */
#define RTL_RCR_AAP       0x00000001 /* Accept All Physical               */
#define RTL_RCR_APM       0x00000002 /* Accept Physical Match             */
#define RTL_RCR_AM        0x00000004 /* Accept Multicast                  */
#define RTL_RCR_AB        0x00000008 /* Accept Broadcast                  */
#define RTL_RCR_AR        0x00000010 /* Accept Runt                       */
#define RTL_RCR_AER       0x00000020 /* Accept Error                      */
#define RTL_RCR_RXFTH_UNL 0x00007000 /* RX FIFO Threshold = unlimited     */
#define RTL_RCR_MXDMA_UNL 0x00000700 /* Max DMA Burst = unlimited         */

/* ── TX Configuration Register (RTL_TCR) bits ──────────────────────  */
#define RTL_TCR_MXDMA_UNL 0x00000700 /* Max DMA Burst = unlimited         */
#define RTL_TCR_IFG_NORM  0x03000000 /* Inter-Frame Gap = standard        */

/* ── Interrupt Status / Mask bits (RTL_ISR / RTL_IMR) ──────────────  */
#define RTL_INT_ROK       0x0001    /* RX OK                              */
#define RTL_INT_RER       0x0002    /* RX Error                           */
#define RTL_INT_TOK       0x0004    /* TX OK                              */
#define RTL_INT_TER       0x0008    /* TX Error                           */
#define RTL_INT_RDU       0x0010    /* RX Descriptor Unavailable          */
#define RTL_INT_LINKCHG   0x0020    /* Link Change                        */
#define RTL_INT_FOVW      0x0040    /* RX FIFO Overflow                   */
#define RTL_INT_TDU       0x0080    /* TX Descriptor Unavailable          */
#define RTL_INT_SWI       0x0100    /* SW Interrupt                       */
#define RTL_INT_TIMEOUT   0x4000    /* Timeout                            */
#define RTL_INT_SERR      0x8000    /* System Error                       */

/* ── PHY Status Register (RTL_PHYSTATUS) bits ──────────────────────  */
#define RTL_PHYSTATUS_LINKSTS  0x02 /* Link Status                        */
#define RTL_PHYSTATUS_FULLDUP  0x01 /* Full Duplex                        */
#define RTL_PHYSTATUS_1000     0x10 /* 1000Mbps                           */
#define RTL_PHYSTATUS_100      0x08 /* 100Mbps                            */
#define RTL_PHYSTATUS_10       0x04 /* 10Mbps                             */

/* ── Descriptor Format (16 bytes, "C+ mode") ───────────────────────
 *
 * Bits in the first 32-bit word ("status") have shared semantics for
 * TX y RX. RX descriptor fills `frame_len` (low 14 bits) on receive.   */

#define RTL_DESC_OWN      0x80000000 /* HW owns descriptor (1 = HW)       */
#define RTL_DESC_EOR      0x40000000 /* End Of Ring                       */
#define RTL_DESC_FS       0x20000000 /* First Segment                     */
#define RTL_DESC_LS       0x10000000 /* Last Segment                      */
#define RTL_DESC_LEN_MASK 0x00003FFF /* Frame length (14 bits)            */

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t status;       /* OWN, EOR, FS, LS, length, vlan, ...        */
    uint32_t vlan;         /* VLAN tag (TX) or RX-side flags             */
    uint32_t buf_lo;       /* Buffer phys addr low                       */
    uint32_t buf_hi;       /* Buffer phys addr high                      */
} rtl_desc_t;

/* Sizing — RTL8169 family allows up to 1024 descriptors per ring.       */
#define RTL_TX_RING_SIZE  64
#define RTL_RX_RING_SIZE  64
#define RTL_PKT_BUF_SIZE  2048      /* Max Ethernet frame + slop          */

#endif /* OSITOK_RTL8111_H */
