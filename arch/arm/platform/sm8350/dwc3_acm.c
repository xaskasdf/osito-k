/*
 * dwc3_acm.c -- USB CDC-ACM serial gadget on the SM8350 DWC_usb31 (usb_1)
 *
 * Minimal polled device-mode driver: one event buffer, EP0 control transfers
 * and a CDC-ACM function, high speed only. Physical endpoints are
 * 2 * ep + dir_in: 0/1 = EP0 OUT/IN, 2/3 = EP1 bulk OUT/IN (data), 5 = EP2
 * interrupt IN (notifications, never used). ABL leaves the controller powered
 * with its USB2 PHY up (fastboot runs on it), so there is no PHY init here;
 * usb.c handles clocks and checks the SMMU.
 *
 * Programming model: Linux drivers/usb/dwc3 (core.h, gadget.c, ep0.c).
 * The D-cache is off on SM8350, so DMA buffers need no cache maintenance.
 *
 * No pointer tables anywhere (descriptors are byte arrays, strings are
 * passed directly): stored pointers hold link-time addresses, which are
 * wrong at the real load address.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "usb.h"

/* Registers, as offsets from the DWC3 base */
#define GCTL                0xC110
#define GSTS                0xC118
#define GUSB2PHYCFG0        0xC200
#define GUSB3PIPECTL0       0xC2C0
#define GEVNTADRLO          0xC400
#define GEVNTADRHI          0xC404
#define GEVNTSIZ            0xC408
#define GEVNTCOUNT          0xC40C
#define DCFG                0xC700
#define DCTL                0xC704
#define DEVTEN              0xC708
#define DSTS                0xC70C
#define DALEPENA            0xC720
#define DEP_BASE(n)         (0xC800 + (n) * 0x10)
#define DEPCMDPAR2          0x0
#define DEPCMDPAR1          0x4
#define DEPCMDPAR0          0x8
#define DEPCMD              0xC
#define QSCRATCH_GENERAL_CFG 0xF8808
#define QSCRATCH_HS_PHY_CTRL 0xF8810
#define QSCRATCH_SS_PHY_CTRL 0xF8830
#define PIPE_UTMI_CLK_SEL   (1U << 0)
#define PIPE3_PHYSTATUS_SW  (1U << 3)
#define PIPE_UTMI_CLK_DIS   (1U << 8)

#define GCTL_PRTCAP_MASK    (3U << 12)
#define GCTL_PRTCAP_DEVICE  (2U << 12)
#define USB2PHYCFG_SUSPHY   (1U << 6)
#define USB2PHYCFG_ENBLSLPM (1U << 8)
#define GEVNTSIZ_INTMASK    (1U << 31)
#define DCFG_SPEED_MASK     7U                  /* 0 = high speed */
#define DCFG_DEVADDR_MASK   (0x7FU << 3)
#define DCFG_LPM_CAP        (1U << 22)
#define DCTL_RUN_STOP       (1U << 31)
#define DCTL_CSFTRST        (1U << 30)
#define DSTS_DEVCTRLHLT     (1U << 22)
#define DEVTEN_DISCONN      (1U << 0)
#define DEVTEN_USBRST       (1U << 1)
#define DEVTEN_CONNDONE     (1U << 2)
#define UTMI_OTG_VBUS_VALID (1U << 20)
#define SW_SESSVLD_SEL      (1U << 28)
#define LANE0_PWR_PRESENT   (1U << 24)

#define CMD_SETEPCONFIG     0x1
#define CMD_SETXFERRES      0x2
#define CMD_SETSTALL        0x4
#define CMD_CLEARSTALL      0x5
#define CMD_STARTXFER       0x6
#define CMD_ENDXFER         0x8
#define CMD_STARTCFG        0x9
#define CMD_IOC             (1U << 8)
#define CMD_ACT             (1U << 10)
#define CMD_FORCERM         (1U << 11)
#define CMD_PARAM(x)        ((uint32_t)(x) << 16)

#define EPCFG_TYPE(t)       ((t) << 1)          /* 0 control, 2 bulk, 3 interrupt */
#define EPCFG_MPS(n)        ((n) << 3)
#define EPCFG_FIFO(n)       ((n) << 17)
#define EPCFG_MODIFY        (2U << 30)
#define EPCFG_XFERCMPL_EN   (1U << 8)
#define EPCFG_XFERNRDY_EN   (1U << 10)
#define EPCFG_BINTERVAL_M1(n) ((n) << 16)
#define EPCFG_EPNUM(n)      ((n) << 25)

#define TRB_HWO             (1U << 0)
#define TRB_LST             (1U << 1)
#define TRB_ISP_IMI         (1U << 10)
#define TRB_IOC             (1U << 11)
#define TRB_NORMAL          (1U << 4)
#define TRB_SETUP           (2U << 4)
#define TRB_STATUS2         (3U << 4)
#define TRB_STATUS3         (4U << 4)
#define TRB_DATA            (5U << 4)

#define DEPEVT_XFERCOMPLETE 1
#define DEPEVT_XFERNOTREADY 3
#define CONTROL_STATUS      2               /* XferNotReady phase, status & 3 */
#define DEVEVT_DISCONNECT   0
#define DEVEVT_RESET        1
#define DEVEVT_CONNDONE     2

/* DMA area layout */
#define DMA_EVT             0x0000          /* event buffer */
#define EVT_SIZE            4096
#define DMA_TRB(slot)       (0x1000 + (slot) * 16)
#define TRB_EP0             0
#define TRB_RX              1
#define TRB_TX              2
#define DMA_SETUP           0x1100          /* 8-byte SETUP packet */
#define DMA_EP0BUF          0x1200          /* control data stage */
#define DMA_RXBUF           0x2000
#define DMA_TXBUF           0x3000
#define DMA_SIZE            0x4000

#define EP0_MPS             64
#define BULK_MPS            512
#define TX_CHUNK            511             /* < BULK_MPS: every transfer ends short, no ZLP needed */

/* ── Descriptors ─────────────────────────────────────────────── */

/* pid.codes test VID/PID (0x1209:0x0001); class 0x02 at device level so
 * Windows 10+ binds its inbox usbser.sys */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, EP0_MPS,
    0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};

static const uint8_t cfg_desc[67] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 50,             /* config 1, 2 interfaces, 100 mA */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,         /* interface 0: CDC comm, ACM */
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 0x01,                  /* call management, data on interface 1 */
    4, 0x24, 0x02, 0x02,                        /* ACM: line coding + control line state */
    5, 0x24, 0x06, 0x00, 0x01,                  /* union: master 0, slave 1 */
    7, 5, 0x82, 0x03, 16, 0, 9,                 /* EP2 IN interrupt, 16 B, 2^8 uframes */
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,         /* interface 1: CDC data */
    7, 5, 0x01, 0x02, 0x00, 0x02, 0,            /* EP1 OUT bulk, 512 B */
    7, 5, 0x81, 0x02, 0x00, 0x02, 0,            /* EP1 IN bulk, 512 B */
};

/* 115200 8N1; the host's SET_LINE_CODING just overwrites it */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };

/* ── State ───────────────────────────────────────────────────── */

static uint64_t dwc, dma;
static uint32_t evt_pos;
static int started, configured;

enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS, EP0_STATUS };
static int ep0_state;
static int ep0_three_stage;
static int ep0_active = -1;                 /* phys EP0 with a transfer in flight */
static uint32_t ep0_rsc;                    /* its transfer resource index */
static uint8_t ep0_out_req;                 /* class request waiting for OUT data */

static int rx_active, tx_active;
static uint32_t tx_rsc, rx_rsc;

#define TX_RING 8192
#define RX_RING 512
static volatile uint8_t tx_ring[TX_RING];
static volatile uint32_t tx_head, tx_tail;
static volatile uint8_t rx_ring[RX_RING];
static volatile uint32_t rx_head, rx_tail;

static int log_budget = 40;                 /* enumeration log lines on the console */

/* ── Low level ───────────────────────────────────────────────── */

static uint32_t rd(uint32_t off) { return mmio_read32(dwc + off); }
static void wr(uint32_t off, uint32_t v) { mmio_write32(dwc + off, v); }

static volatile uint8_t *dma_ptr(uint32_t off)
{
    return (volatile uint8_t *)(uintptr_t)(dma + off);
}

static int log_ok(void)
{
    if (log_budget <= 0)
        return 0;
    log_budget--;
    return 1;
}

/* Issue an endpoint command and wait for it; returns DEPCMD (status in
 * bits 15:12, resource index in 22:16), or 0xFFFFFFFF on timeout. */
static uint32_t ep_cmd(int phys, uint32_t cmd, uint32_t p0, uint32_t p1)
{
    uint32_t base = DEP_BASE(phys);
    wr(base + DEPCMDPAR2, 0);
    wr(base + DEPCMDPAR1, p1);
    wr(base + DEPCMDPAR0, p0);
    wr(base + DEPCMD, cmd | CMD_ACT);
    for (int i = 0; i < 1000000; i++) {
        uint32_t r = rd(base + DEPCMD);
        if (!(r & CMD_ACT))
            return r;
    }
    if (log_ok()) {
        serial_puts("[USB ] ep");
        serial_putdec(phys);
        serial_puts(" cmd ");
        serial_puthex(cmd, 2);
        serial_puts(" timeout, depcmd=");
        serial_puthex(rd(base + DEPCMD), 8);
        serial_puts("\n");
    }
    return 0xFFFFFFFF;
}

static uint32_t ep_config(int phys, uint32_t type, uint32_t mps,
                          uint32_t action, uint32_t p1_extra)
{
    uint32_t p0 = EPCFG_TYPE(type) | EPCFG_MPS(mps) | action;
    if (phys & 1)
        p0 |= EPCFG_FIFO(phys >> 1);
    uint32_t p1 = EPCFG_XFERCMPL_EN | EPCFG_XFERNRDY_EN | EPCFG_EPNUM(phys) | p1_extra;
    return ep_cmd(phys, CMD_SETEPCONFIG, p0, p1);
}

/* One-TRB transfer; returns the transfer resource index, or 0xFFFFFFFF if
 * the command timed out */
static uint32_t start_xfer(int phys, int slot, uint32_t buf_off, uint32_t len,
                           uint32_t type)
{
    volatile uint32_t *trb = (volatile uint32_t *)dma_ptr(DMA_TRB(slot));
    uint64_t buf = dma + buf_off;
    trb[0] = (uint32_t)buf;
    trb[1] = (uint32_t)(buf >> 32);
    trb[2] = len;
    trb[3] = type | TRB_LST | TRB_IOC | TRB_ISP_IMI | TRB_HWO;
    uint64_t t = dma + DMA_TRB(slot);
    uint32_t r = ep_cmd(phys, CMD_STARTXFER, (uint32_t)(t >> 32), (uint32_t)t);
    return r == 0xFFFFFFFF ? r : (r >> 16) & 0x7F;
}

static void end_xfer(int phys, uint32_t rsc)
{
    ep_cmd(phys, CMD_ENDXFER | CMD_FORCERM | CMD_IOC | CMD_PARAM(rsc), 0, 0);
}

/* ── EP0 ─────────────────────────────────────────────────────── */

static void ep0_start_setup(void)
{
    ep0_state = EP0_SETUP;
    ep0_active = 0;
    ep0_rsc = start_xfer(0, TRB_EP0, DMA_SETUP, 8, TRB_SETUP);
}

static void ep0_stall(uint8_t bmrt, uint8_t req, uint16_t wvalue)
{
    if (log_ok()) {
        serial_puts("[USB ] stall ");
        serial_puthex(bmrt, 2);
        serial_putc(' ');
        serial_puthex(req, 2);
        serial_putc(' ');
        serial_puthex(wvalue, 4);
        serial_puts("\n");
    }
    ep_cmd(0, CMD_SETSTALL, 0, 0);
    ep0_start_setup();
}

/* No data stage: the status stage starts on XferNotReady */
static void ep0_ack(void)
{
    ep0_three_stage = 0;
    ep0_active = -1;
    ep0_state = EP0_WAIT_STATUS;
}

static void ep0_send(const uint8_t *src, uint32_t len, uint16_t wlength)
{
    volatile uint8_t *buf = dma_ptr(DMA_EP0BUF);
    if (len > wlength)
        len = wlength;
    for (uint32_t i = 0; i < len; i++)
        buf[i] = src[i];
    ep0_three_stage = 1;
    ep0_state = EP0_DATA;
    ep0_active = 1;
    ep0_rsc = start_xfer(1, TRB_EP0, DMA_EP0BUF, len, TRB_DATA);
}

static void ep0_send_string(const char *s, uint16_t wlength)
{
    uint8_t d[64];
    uint32_t n = 0;
    while (s[n] && n < 31) {
        d[2 + 2 * n] = (uint8_t)s[n];
        d[3 + 2 * n] = 0;
        n++;
    }
    d[0] = (uint8_t)(2 + 2 * n);
    d[1] = 3;
    ep0_send(d, d[0], wlength);
}

static void ep0_recv(uint8_t req)
{
    ep0_out_req = req;
    ep0_three_stage = 1;
    ep0_state = EP0_DATA;
    ep0_active = 0;
    ep0_rsc = start_xfer(0, TRB_EP0, DMA_EP0BUF, EP0_MPS, TRB_DATA);
}

static void acm_configure(void)
{
    ep_config(2, 2, BULK_MPS, 0, 0);
    ep_config(3, 2, BULK_MPS, 0, 0);
    ep_config(5, 3, 16, 0, EPCFG_BINTERVAL_M1(8));
    wr(DALEPENA, rd(DALEPENA) | (1U << 2) | (1U << 3) | (1U << 5));
    configured = 1;
    tx_active = 0;
    rx_active = 1;
    rx_rsc = start_xfer(2, TRB_RX, DMA_RXBUF, BULK_MPS, TRB_NORMAL);
}

static void acm_deconfigure(void)
{
    if (rx_active)
        end_xfer(2, rx_rsc);
    if (tx_active)
        end_xfer(3, tx_rsc);
    rx_active = tx_active = 0;
    wr(DALEPENA, 3);
    configured = 0;
}

static void ep0_get_descriptor(uint16_t wvalue, uint16_t wlength)
{
    uint8_t type = wvalue >> 8, idx = wvalue & 0xFF;
    if (type == 1) {
        ep0_send(dev_desc, sizeof(dev_desc), wlength);
    } else if (type == 2) {
        ep0_send(cfg_desc, sizeof(cfg_desc), wlength);
    } else if (type == 3 && idx == 0) {
        static const uint8_t langid[4] = { 4, 3, 0x09, 0x04 };   /* en-US */
        ep0_send(langid, 4, wlength);
    } else if (type == 3 && idx == 1) {
        ep0_send_string("naranjositos", wlength);
    } else if (type == 3 && idx == 2) {
        ep0_send_string("Osito-K", wlength);
    } else if (type == 3 && idx == 3) {
        ep0_send_string("0001", wlength);
    } else {
        ep0_stall(0x80, 6, wvalue);     /* e.g. device qualifier: no full-speed config */
    }
}

static void ep0_setup(void)
{
    volatile uint8_t *s = dma_ptr(DMA_SETUP);
    uint8_t bmrt = s[0], req = s[1];
    uint16_t wvalue = s[2] | (s[3] << 8);
    uint16_t windex = s[4] | (s[5] << 8);
    uint16_t wlength = s[6] | (s[7] << 8);
    static const uint8_t zero2[2] = { 0, 0 };

    if ((bmrt & 0x60) == 0x00) {                /* standard */
        switch (req) {
        case 0x06:                              /* GET_DESCRIPTOR */
            ep0_get_descriptor(wvalue, wlength);
            return;
        case 0x05:                              /* SET_ADDRESS */
            wr(DCFG, (rd(DCFG) & ~DCFG_DEVADDR_MASK) | ((wvalue & 0x7F) << 3));
            if (log_ok()) {
                serial_puts("[USB ] address ");
                serial_putdec(wvalue & 0x7F);
                serial_puts("\n");
            }
            ep0_ack();
            return;
        case 0x09:                              /* SET_CONFIGURATION */
            if (wvalue == 1 && !configured)
                acm_configure();
            else if (wvalue == 0 && configured)
                acm_deconfigure();
            if (log_ok()) {
                serial_puts("[USB ] configuration ");
                serial_putdec(wvalue);
                serial_puts("\n");
            }
            ep0_ack();
            return;
        case 0x08: {                            /* GET_CONFIGURATION */
            uint8_t c = (uint8_t)configured;
            ep0_send(&c, 1, wlength);
            return;
        }
        case 0x00:                              /* GET_STATUS */
            ep0_send(zero2, 2, wlength);
            return;
        case 0x0A:                              /* GET_INTERFACE */
            ep0_send(zero2, 1, wlength);
            return;
        case 0x01:                              /* CLEAR_FEATURE */
            if ((bmrt & 0x1F) == 2 && wvalue == 0) {    /* ENDPOINT_HALT */
                int phys = ((windex & 0xF) << 1) | ((windex >> 7) & 1);
                ep_cmd(phys, CMD_CLEARSTALL, 0, 0);
            }
            ep0_ack();
            return;
        case 0x03:                              /* SET_FEATURE */
        case 0x0B:                              /* SET_INTERFACE */
            ep0_ack();
            return;
        }
    } else if ((bmrt & 0x60) == 0x20) {         /* class (CDC) */
        switch (req) {
        case 0x20:                              /* SET_LINE_CODING */
            ep0_recv(req);
            return;
        case 0x21:                              /* GET_LINE_CODING */
            ep0_send(line_coding, 7, wlength);
            return;
        case 0x22:                              /* SET_CONTROL_LINE_STATE */
            if (log_ok()) {
                serial_puts("[USB ] host port ");
                if (wvalue & 1)
                    serial_puts("open\n");
                else
                    serial_puts("closed\n");
            }
            ep0_ack();
            return;
        case 0x23:                              /* SEND_BREAK */
            ep0_ack();
            return;
        }
    }
    ep0_stall(bmrt, req, wvalue);
}

static void ep0_event(int phys, uint32_t type, uint32_t status)
{
    if (type == DEPEVT_XFERNOTREADY) {
        if ((status & 3) == CONTROL_STATUS && ep0_state == EP0_WAIT_STATUS) {
            ep0_state = EP0_STATUS;
            ep0_active = phys;
            ep0_rsc = start_xfer(phys, TRB_EP0, DMA_SETUP, 0,
                                 ep0_three_stage ? TRB_STATUS3 : TRB_STATUS2);
        }
        return;
    }
    if (type != DEPEVT_XFERCOMPLETE)
        return;

    ep0_active = -1;
    if (ep0_state == EP0_SETUP) {
        ep0_setup();
    } else if (ep0_state == EP0_DATA) {
        if (ep0_out_req == 0x20) {
            volatile uint8_t *buf = dma_ptr(DMA_EP0BUF);
            for (int i = 0; i < 7; i++)
                line_coding[i] = buf[i];
        }
        ep0_out_req = 0;
        ep0_state = EP0_WAIT_STATUS;
    } else if (ep0_state == EP0_STATUS) {
        ep0_start_setup();
    }
}

/* ── Bulk data ───────────────────────────────────────────────── */

static void rx_complete(void)
{
    volatile uint32_t *trb = (volatile uint32_t *)dma_ptr(DMA_TRB(TRB_RX));
    uint32_t n = BULK_MPS - (trb[2] & 0xFFFFFF);
    volatile uint8_t *buf = dma_ptr(DMA_RXBUF);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (rx_head + 1) % RX_RING;
        if (next == rx_tail)
            break;                              /* full: drop */
        rx_ring[rx_head] = buf[i];
        rx_head = next;
    }
    rx_rsc = start_xfer(2, TRB_RX, DMA_RXBUF, BULK_MPS, TRB_NORMAL);
}

static void tx_kick(void)
{
    if (!configured || tx_active || tx_head == tx_tail)
        return;
    volatile uint8_t *buf = dma_ptr(DMA_TXBUF);
    uint32_t n = 0;
    while (n < TX_CHUNK && tx_tail != tx_head) {
        buf[n++] = tx_ring[tx_tail];
        tx_tail = (tx_tail + 1) % TX_RING;
    }
    tx_active = 1;
    tx_rsc = start_xfer(3, TRB_TX, DMA_TXBUF, n, TRB_NORMAL);
}

/* ── Events ──────────────────────────────────────────────────── */

static void dev_event(uint32_t ev)
{
    uint32_t type = (ev >> 8) & 0xF;
    if (type == DEVEVT_RESET) {
        if (log_ok())
            serial_puts("[USB ] bus reset\n");
        if (configured)
            acm_deconfigure();
        wr(DCFG, rd(DCFG) & ~DCFG_DEVADDR_MASK);
        /* Windows resets mid-transfer during its first enumeration */
        if (ep0_state != EP0_SETUP) {
            if (ep0_active >= 0)
                end_xfer(ep0_active, ep0_rsc);
            ep0_start_setup();
        }
    } else if (type == DEVEVT_CONNDONE) {
        uint32_t speed = rd(DSTS) & 7;
        if (log_ok()) {
            serial_puts("[USB ] connected, speed ");
            serial_putdec(speed);
            if (speed == 0)
                serial_puts(" (high)");
            serial_puts("\n");
        }
        ep_config(0, 0, EP0_MPS, EPCFG_MODIFY, 0);
        ep_config(1, 0, EP0_MPS, EPCFG_MODIFY, 0);
    } else if (type == DEVEVT_DISCONNECT) {
        if (log_ok())
            serial_puts("[USB ] disconnected\n");
        if (configured)
            acm_deconfigure();
    }
}

void acm_poll(void)
{
    if (!started)
        return;
    uint32_t count = rd(GEVNTCOUNT) & 0xFFFC;
    for (uint32_t done = 0; done < count; done += 4) {
        uint32_t ev = *(volatile uint32_t *)dma_ptr(DMA_EVT + evt_pos);
        evt_pos = (evt_pos + 4) % EVT_SIZE;
        if (ev & 1) {
            if (((ev >> 1) & 0x7F) == 0)        /* device event */
                dev_event(ev);
            continue;
        }
        int phys = (ev >> 1) & 0x1F;
        uint32_t type = (ev >> 6) & 0xF;
        uint32_t status = (ev >> 12) & 0xF;
        if (phys <= 1)
            ep0_event(phys, type, status);
        else if (type == DEPEVT_XFERCOMPLETE && phys == 2 && rx_active)
            rx_complete();
        else if (type == DEPEVT_XFERCOMPLETE && phys == 3)
            tx_active = 0;
    }
    if (count)
        wr(GEVNTCOUNT, count);
    tx_kick();
}

int acm_getc(void)
{
    if (rx_tail == rx_head)
        return -1;
    int c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_RING;
    return c;
}

void acm_putc(char c)
{
    uint32_t next = (tx_head + 1) % TX_RING;
    if (next == tx_tail)
        return;                                 /* full: drop */
    tx_ring[tx_head] = (uint8_t)c;
    tx_head = next;
}

/* ── Start ───────────────────────────────────────────────────── */

static int wait_clear(uint32_t off, uint32_t bit, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 10; i++) {
        if (!(rd(off) & bit))
            return 0;
        udelay(100);
    }
    return -1;
}

static void dump_state(void)
{
    serial_puts("[USB ] dctl=");
    serial_puthex(rd(DCTL), 8);
    serial_puts(" dsts=");
    serial_puthex(rd(DSTS), 8);
    serial_puts(" gsts=");
    serial_puthex(rd(GSTS), 8);
    serial_puts("\n[USB ] u2phy=");
    serial_puthex(rd(GUSB2PHYCFG0), 8);
    serial_puts(" u3pipe=");
    serial_puthex(rd(GUSB3PIPECTL0), 8);
    serial_puts(" gencfg=");
    serial_puthex(rd(QSCRATCH_GENERAL_CFG), 8);
    serial_puts("\n");
}

int acm_start(uint64_t dwc3_base, uint64_t dma_base)
{
    dwc = dwc3_base;
    dma = dma_base;

    /* ABL ran fastboot at SuperSpeed and shut the USB3 PHY down on exit,
     * taking the PIPE clock with it; the endpoint command logic runs on that
     * clock, so commands never completed. Clock it from UTMI instead (Linux
     * dwc3-qcom select_utmi_clk, "qcom,select-utmi-as-pipe-clk" in the
     * ROG5 DTS). */
    wr(QSCRATCH_GENERAL_CFG, rd(QSCRATCH_GENERAL_CFG) | PIPE_UTMI_CLK_DIS);
    udelay(100);
    wr(QSCRATCH_GENERAL_CFG, rd(QSCRATCH_GENERAL_CFG) | PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
    udelay(100);
    wr(QSCRATCH_GENERAL_CFG, rd(QSCRATCH_GENERAL_CFG) & ~PIPE_UTMI_CLK_DIS);

    /* Stop whatever ABL left, then soft-reset the device controller (keeps
     * the PHY configuration). DWC_usb31 needs time before the PHY domain
     * is touched again. */
    wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);
    for (int i = 0; i < 1000 && !(rd(DSTS) & DSTS_DEVCTRLHLT); i++)
        udelay(100);
    wr(DCTL, DCTL_CSFTRST);
    if (wait_clear(DCTL, DCTL_CSFTRST, 500)) {
        serial_puts("[USB ] soft reset timed out\n");
        return -1;
    }
    mdelay(50);

    wr(GCTL, (rd(GCTL) & ~GCTL_PRTCAP_MASK) | GCTL_PRTCAP_DEVICE);
    wr(GUSB2PHYCFG0, rd(GUSB2PHYCFG0) & ~(USB2PHYCFG_SUSPHY | USB2PHYCFG_ENBLSLPM));

    evt_pos = 0;
    wr(GEVNTADRLO, (uint32_t)(dma + DMA_EVT));
    wr(GEVNTADRHI, (uint32_t)((dma + DMA_EVT) >> 32));
    wr(GEVNTSIZ, EVT_SIZE | GEVNTSIZ_INTMASK);  /* polled: keep the IRQ masked */
    wr(GEVNTCOUNT, 0);

    wr(DCFG, rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK | DCFG_LPM_CAP));

    /* One transfer resource per endpoint, then EP0 OUT/IN. A command that
     * never completes means part of the controller has no clock: dump the
     * state and stay disconnected. */
    if (ep_cmd(0, CMD_STARTCFG, 0, 0) == 0xFFFFFFFF)
        goto fail;
    for (int phys = 0; phys <= 5; phys++)
        if (ep_cmd(phys, CMD_SETXFERRES, 1, 0) == 0xFFFFFFFF)
            goto fail;
    if (ep_config(0, 0, EP0_MPS, 0, 0) == 0xFFFFFFFF ||
        ep_config(1, 0, EP0_MPS, 0, 0) == 0xFFFFFFFF)
        goto fail;
    wr(DALEPENA, 3);
    ep0_start_setup();
    if (ep0_rsc == 0xFFFFFFFF)
        goto fail;

    wr(DEVTEN, DEVTEN_DISCONN | DEVTEN_USBRST | DEVTEN_CONNDONE);

    /* No PMIC/extcon hookup: tell the controller VBUS is there */
    wr(QSCRATCH_HS_PHY_CTRL, rd(QSCRATCH_HS_PHY_CTRL) | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
    wr(QSCRATCH_SS_PHY_CTRL, rd(QSCRATCH_SS_PHY_CTRL) | LANE0_PWR_PRESENT);

    wr(DCTL, rd(DCTL) | DCTL_RUN_STOP);
    if (wait_clear(DSTS, DSTS_DEVCTRLHLT, 500)) {
        serial_puts("[USB ] controller did not start\n");
        return -1;
    }
    started = 1;
    serial_puts("[USB ] gadget up on usb_1, waiting for the host\n");
    return 0;

fail:
    dump_state();
    serial_puts("[USB ] endpoint commands stuck, gadget not started\n");
    return -1;
}
