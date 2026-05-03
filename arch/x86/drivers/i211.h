/*
 * OsitoK x86-64 — Intel I211 Gigabit Ethernet Driver
 *
 * Register definitions, descriptor formats, and API.
 * Family: igb (82575+). Legacy 16-byte descriptors, single queue, polling.
 * PCI: 8086:1539, BAR0 = 128KB MMIO.
 */

#ifndef OSITOK_I211_H
#define OSITOK_I211_H

#include "../include/types.h"

/* ── MMIO Register Offsets ───────────────────────────────────── */

/* General */
#define I211_CTRL       0x00000   /* Device Control */
#define I211_STATUS     0x00008   /* Device Status */
#define I211_CTRL_EXT   0x00018   /* Extended Device Control */
#define I211_EERD       0x00014   /* EEPROM Read */
#define I211_MDIC       0x00020   /* MDI Control */

/* Interrupt */
#define I211_ICR        0x000C0   /* Interrupt Cause Read */
#define I211_ICS        0x000C8   /* Interrupt Cause Set (test) */
#define I211_IMS        0x000D0   /* Interrupt Mask Set */
#define I211_IMC        0x000D8   /* Interrupt Mask Clear */
#define I211_EITR0      0x01680   /* Interrupt Throttling (queue 0) */

/* Extended interrupt registers — required by Linux igb's MSI/MSI-X
 * setup, even in legacy MSI mode.  Without GPIE.EIAME programmed,
 * the chip stops honoring IMS re-arm after ~22 IRQs and MSI delivery
 * goes silent.  See I210/I211 datasheet §8.13.31 (GPIE).             */
#define I211_GPIE       0x00028   /* General Purpose Interrupt Enable */
#define I211_EIAC       0x000DC   /* Extended Interrupt Auto-Clear    */
#define I211_EIAM       0x000E0   /* Extended Interrupt Auto-Mask     */
#define I211_IAM        0x000E8   /* Interrupt Auto-Mask (legacy)     */

/* GPIE bits */
#define I211_GPIE_NSICR     (1u << 0)   /* No-Snoop ICR clear            */
#define I211_GPIE_MSIX_MODE (1u << 4)   /* MSI-X enable (off in MSI mode) */
#define I211_GPIE_LL_INT    (1u << 5)
#define I211_GPIE_EIAME     (1u << 30)  /* Extended Int Auto-Mask Enable */
#define I211_GPIE_PBA       (1u << 31)  /* PBA support                   */

/* Interrupt cause bits */
#define I211_ICR_TXDW   (1 << 0)    /* TX Descriptor Written Back */
#define I211_ICR_LSC    (1 << 2)    /* Link Status Change */
#define I211_ICR_RXT0   (1 << 7)    /* RX Timer Interrupt */

/* Receive */
#define I211_RCTL       0x00100   /* Receive Control */
#define I211_RDBAL0     0x02800   /* RX Descriptor Base Low (queue 0) */
#define I211_RDBAH0     0x02804   /* RX Descriptor Base High */
#define I211_RDLEN0     0x02808   /* RX Descriptor Length */
#define I211_SRRCTL0    0x0280C   /* Split/Replication RX Control */
#define I211_RDH0       0x02810   /* RX Descriptor Head */
#define I211_RDT0       0x02818   /* RX Descriptor Tail */
#define I211_RXDCTL0    0x02828   /* RX Descriptor Control */

/* Transmit */
#define I211_TCTL       0x00400   /* Transmit Control */
#define I211_TDBAL0     0x03800   /* TX Descriptor Base Low (queue 0) */
#define I211_TDBAH0     0x03804   /* TX Descriptor Base High */
#define I211_TDLEN0     0x03808   /* TX Descriptor Length */
#define I211_TDH0       0x03810   /* TX Descriptor Head */
#define I211_TDT0       0x03818   /* TX Descriptor Tail */
#define I211_TXDCTL0    0x03828   /* TX Descriptor Control */

/* Receive Address */
#define I211_RAL0       0x05400   /* Receive Address Low (entry 0) */
#define I211_RAH0       0x05404   /* Receive Address High */
#define I211_MTA        0x05200   /* Multicast Table Array (128 entries) */

/* ── Control Register Bits ───────────────────────────────────── */

/* CTRL */
#define I211_CTRL_SLU       (1 << 6)    /* Set Link Up */
#define I211_CTRL_RST       (1 << 26)   /* Device Reset */
#define I211_CTRL_PHY_RST   (1 << 31)   /* PHY Reset */

/* STATUS */
#define I211_STATUS_FD      (1 << 0)    /* Full Duplex */
#define I211_STATUS_LU      (1 << 1)    /* Link Up */
#define I211_STATUS_SPEED   (3 << 6)    /* Speed: 00=10M 01=100M 10=1G 11=1G */

/* RCTL */
#define I211_RCTL_EN        (1 << 1)    /* Receiver Enable */
#define I211_RCTL_SBP       (1 << 2)    /* Store Bad Packets */
#define I211_RCTL_UPE       (1 << 3)    /* Unicast Promiscuous */
#define I211_RCTL_MPE       (1 << 4)    /* Multicast Promiscuous */
#define I211_RCTL_BAM       (1 << 15)   /* Broadcast Accept Mode */
#define I211_RCTL_BSIZE_2K  (0 << 16)   /* Buffer Size 2048 */
#define I211_RCTL_BSEX      (1 << 25)   /* Buffer Size Extension */
#define I211_RCTL_SECRC     (1 << 26)   /* Strip Ethernet CRC */

/* TCTL */
#define I211_TCTL_EN        (1 << 1)    /* Transmitter Enable */
#define I211_TCTL_PSP       (1 << 3)    /* Pad Short Packets */
#define I211_TCTL_CT(n)     ((uint32_t)(n) << 4)   /* Collision Threshold */
#define I211_TCTL_COLD(n)   ((uint32_t)(n) << 12)  /* Collision Distance */

/* RXDCTL / TXDCTL */
#define I211_XDCTL_ENABLE   (1 << 25)   /* Queue Enable */

/* SRRCTL */
#define I211_SRRCTL_DROP_EN     (1 << 31)         /* Drop Enable        */
#define I211_SRRCTL_DESCTYPE_ADV (1 << 25)        /* Advanced one-buffer */

/* ── RX Descriptor (advanced, 16 bytes) ──────────────────────────────
 *
 * El I211 *no soporta legacy descriptors* — solo advanced.  Tiene dos
 * formatos físicos del mismo layout de 16 bytes:
 *
 *   Read (SW → HW):    pkt_addr (8B) + hdr_addr (8B)
 *   Write-back (HW→SW): status_error (4B) + length (2B) + vlan (2B) +
 *                       mrq (4B) + rss_hash (4B)
 *
 * Usamos union para acceder al mismo descriptor con ambas vistas.        */
typedef struct __attribute__((packed)) {
    union {
        struct {                       /* Read view (programado por SW)  */
            uint64_t pkt_addr;
            uint64_t hdr_addr;
        } read;
        struct {                       /* Write-back view (escrito por HW).
                                        * Layout exacto de Linux igb driver
                                        * struct e1000_adv_rx_desc.upper:
                                        *   bytes 0..7  = lower dword (RSS
                                        *                 type/info + hash
                                        *                 o ip_id+csum)
                                        *   bytes 8..11 = status_error
                                        *   bytes 12..13= length
                                        *   bytes 14..15= vlan tag         */
            uint32_t lo_dword;         /* 0..3:  pkt_info (RSS type/info)  */
            uint32_t hi_dword;         /* 4..7:  RSS hash o ip_id+csum     */
            uint32_t status_error;     /* 8..11: DD=bit0 EOP=bit1 errors   */
            uint16_t length;           /* 12..13: packet length             */
            uint16_t vlan;             /* 14..15: VLAN tag                  */
        } wb;
    };
} i211_rx_desc_t;

/* TX descriptor — Advanced Transmit Data Descriptor (Type 0x3).
 *
 * The I210/I211 datasheet (section 7.2.2.2.4) requires advanced descriptor
 * format on this NIC family — legacy TX descriptors are NOT supported and
 * silently drop packets when used. This is the same constraint that forced
 * the RX side to advanced format in commit 4ce1549; the TX side was the
 * still-broken half (APIPA "assigned" without ever transmitting probes,
 * Mac saw zero packets).
 *
 * Layout (16 bytes):
 *   [0..7]   buffer physical address
 *   [8..11]  cmd_type_len:
 *              bits  0..15: data length
 *              bits 16..19: high bits of length (TSO; 0 here)
 *              bits 20..23: DTYP = 0x3 (data desc)
 *              bits 24..31: CMD — EOP(24), IFCS(25), RS(27), DEXT(29 must be 1)
 *   [12..15] olinfo_status:
 *              bits  0..3:  status (HW writes DD=bit0)
 *              bits  4..7:  IDX (stats counter, 0)
 *              bits  8..13: POPTS (offload options, 0)
 *              bits 14..31: PAYLEN (= data length for non-TSO)
 */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t cmd_type_len;
    uint32_t olinfo_status;
} i211_tx_desc_t;

/* Descriptor status/command bits */
#define I211_RXD_STAT_DD    (1 << 0)    /* Descriptor Done */
#define I211_RXD_STAT_EOP   (1 << 1)    /* End of Packet */

/* Advanced TX cmd_type_len — bits 24..31 are CMD, bit-relative inside CMD: */
#define I211_TXD_DTYP_DATA  (0x3u << 20)  /* DTYP = data descriptor */
#define I211_TXD_CMD_EOP    (1u << 24)    /* End of Packet */
#define I211_TXD_CMD_IFCS   (1u << 25)    /* Insert FCS/CRC */
#define I211_TXD_CMD_RS     (1u << 27)    /* Report Status */
#define I211_TXD_CMD_DEXT   (1u << 29)    /* Descriptor extension (must be 1) */

/* olinfo_status bit 0 — DD set by HW on completion */
#define I211_TXD_STAT_DD    (1u << 0)

/* ── Ring Configuration ──────────────────────────────────────── */

#define I211_RX_RING_SIZE   128
#define I211_TX_RING_SIZE   64
#define I211_PKT_BUF_SIZE   2048

/* ── API ─────────────────────────────────────────────────────── */

int  i211_init(uint64_t bar0_phys);
void i211_get_mac(uint8_t mac[6]);
int  i211_send(const void *data, uint32_t len);
/* Scatter-gather TX: up to 4 fragments described by (phys_addr, length).
 * Zero-copy — controller DMAs each fragment directly. Returns 0 on success. */
int  i211_send_sg(const uint64_t frag_phys[], const uint32_t lens[], int n_frags);
int  i211_recv(void *buf, uint32_t *len);
bool i211_link_up(void);

/* Interrupt-driven receive (NAPI hybrid) */
void i211_enable_interrupts(uint8_t pci_bus, uint8_t pci_dev, uint8_t pci_func);
void i211_isr(void);           /* Called from IDT vector 40 */
int  i211_napi_poll(int budget);
extern volatile bool i211_irq_pending;

#endif /* OSITOK_I211_H */
