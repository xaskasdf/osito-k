/*
 * OsitoK x86-64 — xHCI USB 3.x Host Controller
 *
 * Register definitions, TRB types, device context structures.
 * Follows Intel xHCI Specification Revision 1.2.
 */

#ifndef OSITOK_XHCI_H
#define OSITOK_XHCI_H

#include "../include/types.h"

/* ── Capability Registers (BAR0 + 0x00) ──────────────────────── */

#define XHCI_CAP_CAPLENGTH     0x00
#define XHCI_CAP_HCIVERSION    0x02
#define XHCI_CAP_HCSPARAMS1    0x04
#define XHCI_CAP_HCSPARAMS2    0x08
#define XHCI_CAP_HCSPARAMS3    0x0C
#define XHCI_CAP_HCCPARAMS1    0x10
#define XHCI_CAP_DBOFF         0x14
#define XHCI_CAP_RTSOFF        0x18

#define XHCI_HCS1_MAXSLOTS(p)  ((p) & 0xFF)
#define XHCI_HCS1_MAXINTRS(p)  (((p) >> 8) & 0x7FF)
#define XHCI_HCS1_MAXPORTS(p)  (((p) >> 24) & 0xFF)

#define XHCI_HCS2_SCRATCH_HI(p) (((p) >> 21) & 0x1F)
#define XHCI_HCS2_SCRATCH_LO(p) (((p) >> 27) & 0x1F)

#define XHCI_HCC1_AC64(p)      ((p) & 1)
#define XHCI_HCC1_CSZ(p)       (((p) >> 2) & 1)
#define XHCI_HCC1_XECP(p)     (((p) >> 16) & 0xFFFF)

/* ── Operational Registers (BAR0 + CAPLENGTH) ────────────────── */

#define XHCI_OP_USBCMD        0x00
#define XHCI_OP_USBSTS        0x04
#define XHCI_OP_PAGESIZE      0x08
#define XHCI_OP_DNCTRL        0x14
#define XHCI_OP_CRCR          0x18
#define XHCI_OP_DCBAAP        0x30
#define XHCI_OP_CONFIG        0x38

#define XHCI_CMD_RUN           (1 << 0)
#define XHCI_CMD_HCRST         (1 << 1)
#define XHCI_CMD_INTE          (1 << 2)

#define XHCI_STS_HCH           (1 << 0)
#define XHCI_STS_HSE           (1 << 2)
#define XHCI_STS_EINT          (1 << 3)
#define XHCI_STS_PCD           (1 << 4)
#define XHCI_STS_CNR           (1 << 11)

/* Port registers (op_base + 0x400 + port * 0x10) */
#define XHCI_PORTSC_OFF        0x400
#define XHCI_PORTSC(n)         (XHCI_PORTSC_OFF + (n) * 0x10)

#define XHCI_PORTSC_CCS        (1 << 0)
#define XHCI_PORTSC_PED        (1 << 1)
#define XHCI_PORTSC_PR         (1 << 4)
#define XHCI_PORTSC_PLS_MASK   (0xF << 5)
#define XHCI_PORTSC_PP         (1 << 9)
#define XHCI_PORTSC_SPEED_MASK (0xF << 10)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_CSC        (1 << 17)
#define XHCI_PORTSC_PRC        (1 << 21)

/* RW1C/RW1CS bits mask — must write 0 to preserve, writing 1 clears them.
 * PED (bit 1) is RW1CS: writing 1 DISABLES the port! Always mask it out. */
#define XHCI_PORTSC_CHANGE_BITS (XHCI_PORTSC_PED | XHCI_PORTSC_CSC | (1<<18) | (1<<19) | \
                                  (1<<20) | XHCI_PORTSC_PRC | (1<<22) | (1<<23))

/* Speed encoding */
#define XHCI_SPEED_FULL   1
#define XHCI_SPEED_LOW    2
#define XHCI_SPEED_HIGH   3
#define XHCI_SPEED_SUPER  4

/* ── Extended Capabilities ───────────────────────────────────── */

#define XHCI_EXT_CAP_LEGACY   1
#define XHCI_USBLEGSUP_BIOS   (1 << 16)
#define XHCI_USBLEGSUP_OS     (1 << 24)

/* ── Runtime Registers (BAR0 + RTSOFF) ──────────────────────── */

#define XHCI_RT_IR0           0x020
#define XHCI_IR_IMAN          0x00
#define XHCI_IR_IMOD          0x04
#define XHCI_IR_ERSTSZ        0x08
#define XHCI_IR_ERSTBA        0x10
#define XHCI_IR_ERDP          0x18

/* ── Transfer Request Block (TRB) — 16 bytes ────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "TRB must be 16 bytes");

#define XHCI_TRB_TYPE(t)       ((t) << 10)
#define XHCI_TRB_GET_TYPE(c)   (((c) >> 10) & 0x3F)

/* TRB types */
#define TRB_NORMAL              1
#define TRB_SETUP               2
#define TRB_DATA                3
#define TRB_STATUS              4
#define TRB_LINK                6
#define TRB_ENABLE_SLOT         9
#define TRB_ADDRESS_DEVICE      11
#define TRB_CONFIGURE_ENDPOINT  12
#define TRB_EVALUATE_CONTEXT    13
#define TRB_NOOP_CMD            23
#define TRB_TRANSFER_EVENT      32
#define TRB_CMD_COMPLETION      33
#define TRB_PORT_STATUS_CHANGE  34

/* TRB control flags */
#define TRB_CYCLE               (1 << 0)
#define TRB_TC                  (1 << 1)   /* Toggle Cycle (Link TRB) */
#define TRB_IOC                 (1 << 5)
#define TRB_IDT                 (1 << 6)
#define TRB_BSR                 (1 << 9)
#define TRB_DIR_IN              (1 << 16)
#define TRB_TRT_NO_DATA         (0 << 16)  /* Setup TRB: no data stage */
#define TRB_TRT_OUT             (2 << 16)  /* Setup TRB: OUT data */
#define TRB_TRT_IN              (3 << 16)  /* Setup TRB: IN data */

/* ── Event Ring Segment Table Entry ──────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t ring_base;
    uint32_t ring_size;
    uint32_t reserved;
} xhci_erste_t;

_Static_assert(sizeof(xhci_erste_t) == 16, "ERSTE must be 16 bytes");

/* ── Slot Context (32 bytes) ─────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t field1;   /* route(19:0), speed(23:20), MTT(25), hub(26), ctx_entries(31:27) */
    uint32_t field2;   /* max_exit_lat(15:0), root_hub_port(23:16), num_ports(31:24) */
    uint32_t field3;   /* parent_hub(7:0), parent_port(15:8), TTT(17:16), intr_target(31:22) */
    uint32_t field4;   /* dev_addr(7:0), slot_state(31:27) */
    uint32_t reserved[4];
} xhci_slot_ctx_t;

/* ── Endpoint Context (32 bytes) ─────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t field1;   /* ep_state(2:0), mult(9:8), MaxPStreams(14:10), LSA(15), interval(23:16) */
    uint32_t field2;   /* CErr(2:1), ep_type(5:3), HID(7), max_burst(15:8), max_pkt(31:16) */
    uint64_t tr_dequeue; /* bits 63:4 = ring base, bit 0 = DCS */
    uint32_t field4;   /* avg_trb_len(15:0), max_esit_lo(31:16) */
    uint32_t reserved[3];
} xhci_ep_ctx_t;

/* EP types (bits 5:3 of field2) */
#define EP_TYPE_CONTROL         4
#define EP_TYPE_INTERRUPT_IN    7

/* ── Input Control Context (32 bytes) ────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} xhci_input_ctrl_ctx_t;

/* ── USB Setup Packet ────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

_Static_assert(sizeof(usb_setup_t) == 8, "USB setup must be 8 bytes");

#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_CONFIG       0x09
#define USB_REQ_SET_IDLE         0x0A
#define USB_REQ_SET_PROTOCOL     0x0B

#define USB_DESC_DEVICE          0x01
#define USB_DESC_CONFIGURATION   0x02
#define USB_DESC_INTERFACE       0x04
#define USB_DESC_ENDPOINT        0x05

/* ── Driver limits ───────────────────────────────────────────── */

#define XHCI_MAX_SLOTS       16
#define XHCI_CMD_RING_SIZE   32
#define XHCI_EVT_RING_SIZE   64
#define XHCI_XFER_RING_SIZE  32
#define XHCI_MAX_CONTROLLERS 2

/* ── Device State ────────────────────────────────────────────── */

typedef struct {
    uint8_t  slot_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  port;
    uint8_t  speed;
    bool     addressed;

    /* Output context (written by HC) */
    void    *output_ctx;

    /* EP0 transfer ring */
    xhci_trb_t *ep0_ring;
    uint32_t    ep0_enq;
    uint8_t     ep0_cycle;

    /* Interrupt IN ring (HID) */
    xhci_trb_t *int_ring;
    uint32_t    int_enq;
    uint8_t     int_cycle;
    uint8_t     int_ep_dci;     /* Device Context Index */
    uint16_t    int_max_pkt;
    bool        hid_active;

    /* HID report DMA buffer */
    uint8_t    *report_buf;

    /* HID type: 1 = keyboard, 2 = mouse */
    uint8_t     hid_protocol;

    /* Keyboard state (previous report for debounce) */
    uint8_t     prev_keys[6];
    uint8_t     prev_mods;
} xhci_device_t;

/* ── Controller State ────────────────────────────────────────── */

typedef struct {
    volatile void *base;       /* BAR0 */
    uint32_t op_off;           /* Operational register offset */
    uint32_t rt_off;           /* Runtime register offset */
    uint32_t db_off;           /* Doorbell register offset */

    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t ctx_size;         /* 32 or 64 */

    uint64_t *dcbaa;
    uint64_t *scratchpad;

    xhci_trb_t  *cmd_ring;
    uint32_t     cmd_enq;
    uint8_t      cmd_cycle;

    xhci_trb_t  *evt_ring;
    xhci_erste_t *erst;
    uint32_t     evt_deq;
    uint8_t      evt_cycle;

    xhci_device_t devices[XHCI_MAX_SLOTS];
    uint32_t      num_devices;

    /* Supported Protocol port ranges (from Extended Capabilities) */
    uint8_t usb2_port_start; /* 1-based port number */
    uint8_t usb2_port_count;
    uint8_t usb3_port_start;
    uint8_t usb3_port_count;

    uint8_t pci_bus, pci_dev, pci_func;
    bool    initialized;
} xhci_hc_t;

/* ── Public API ──────────────────────────────────────────────── */

int  xhci_init(uint64_t bar0_phys, uint8_t bus, uint8_t dev, uint8_t func);
void xhci_poll(void);
bool xhci_is_ready(void);

#endif /* OSITOK_XHCI_H */
