/*
 * OsitoK x86-64 — xHCI USB 3.x Host Controller Driver
 *
 * Supports AMD 400 Series [1022:43d5] and Matisse [1022:149c].
 * Initializes xHCI, enumerates ports, addresses devices,
 * and sets up USB HID mouse + keyboard polling via interrupt endpoints.
 *
 * Uses polling (no IRQs) — same approach as NVMe/I211 drivers.
 */

#include "../include/types.h"
#include "xhci.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern void  pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);

/* Input injection (input_events.c — mouse) */
extern void input_post_mouse_move(int16_t dx, int16_t dy);
extern void input_set_mouse_abs(int32_t abs_x, int32_t abs_y);
extern void input_post_mouse_button(uint8_t buttons);

/* Keyboard injection (keyboard.c — weak: works without PS/2 driver) */
extern void kb_push(char c)     __attribute__((weak));
extern void kb_push_esc(const char *seq) __attribute__((weak));
extern bool compositor_is_running(void) __attribute__((weak));

/* ── Controller instances ────────────────────────────────────── */

static xhci_hc_t hc_list[XHCI_MAX_CONTROLLERS];
static int hc_count;

/* ── MMIO helpers ────────────────────────────────────────────── */

static uint32_t xr32(xhci_hc_t *hc, uint32_t off)
{
    return mmio_read32((volatile void *)((uint64_t)hc->base + off));
}

static void xw32(xhci_hc_t *hc, uint32_t off, uint32_t val)
{
    mmio_write32((volatile void *)((uint64_t)hc->base + off), val);
}

static void xw64(xhci_hc_t *hc, uint32_t off, uint64_t val)
{
    mmio_write64((volatile void *)((uint64_t)hc->base + off), val);
}

/* Operational register helpers */
static uint32_t op_read(xhci_hc_t *hc, uint32_t off)
{
    return xr32(hc, hc->op_off + off);
}
static void op_write(xhci_hc_t *hc, uint32_t off, uint32_t val)
{
    xw32(hc, hc->op_off + off, val);
}
static void op_write64(xhci_hc_t *hc, uint32_t off, uint64_t val)
{
    xw64(hc, hc->op_off + off, val);
}

/* Runtime register */
static void rt_write(xhci_hc_t *hc, uint32_t off, uint32_t val)
{
    xw32(hc, hc->rt_off + off, val);
}
static void rt_write64(xhci_hc_t *hc, uint32_t off, uint64_t val)
{
    xw64(hc, hc->rt_off + off, val);
}

/* Doorbell */
static void db_write(xhci_hc_t *hc, uint32_t slot, uint32_t val)
{
    xw32(hc, hc->db_off + slot * 4, val);
}

/* Spin delay */
static void spin(uint32_t iters)
{
    for (volatile uint32_t i = 0; i < iters; i++)
        __asm__ volatile ("pause");
}

/* ── Speed name ──────────────────────────────────────────────── */

static const char *speed_name(uint8_t spd)
{
    if (spd == XHCI_SPEED_FULL)  return "Full (12Mbps)";
    if (spd == XHCI_SPEED_LOW)   return "Low (1.5Mbps)";
    if (spd == XHCI_SPEED_HIGH)  return "High (480Mbps)";
    if (spd == XHCI_SPEED_SUPER) return "Super (5Gbps)";
    return "Unknown";
}

/* ── USB HID Keycode Tables (Usage Page 0x07) ────────────────── */

/* HID modifier bits (report byte 0) */
#define HID_MOD_LCTRL   (1 << 0)
#define HID_MOD_LSHIFT  (1 << 1)
#define HID_MOD_LALT    (1 << 2)
#define HID_MOD_LGUI    (1 << 3)
#define HID_MOD_RCTRL   (1 << 4)
#define HID_MOD_RSHIFT  (1 << 5)
#define HID_MOD_RALT    (1 << 6)
#define HID_MOD_RGUI    (1 << 7)

/* Unshifted: HID usage 0x00..0x52 → ASCII */
const char hid_normal[0x54] = {
    /* 0x00-0x03: Reserved/Error */
    0, 0, 0, 0,
    /* 0x04-0x1D: a-z */
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E-0x27: 1-9, 0 */
    '1','2','3','4','5','6','7','8','9','0',
    /* 0x28: Return, 0x29: Escape, 0x2A: Backspace, 0x2B: Tab, 0x2C: Space */
    '\n', 27, '\b', '\t', ' ',
    /* 0x2D-0x38: - = [ ] \ # ; ' ` , . / */
    '-','=','[',']','\\','\\',';','\'','`',',','.','/',
    /* 0x39: Caps Lock (handled separately) */
    0,
    /* 0x3A-0x45: F1-F12 (no ASCII) */
    0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x46-0x4E: PrtSc, ScrLk, Pause, Ins, Home, PgUp, Del, End, PgDn */
    0,0,0,0,0,0,0,0,0,
    /* 0x4F: Right, 0x50: Left, 0x51: Down, 0x52: Up (escape seqs) */
    0,0,0,0,
    /* 0x53: Num Lock (padding for array size) */
    0,
};

/* Shifted: HID usage 0x00..0x53 → ASCII */
const char hid_shifted[0x54] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b', '\t', ' ',
    '_','+','{','}','|','|',':','"','~','<','>','?',
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,
    0,0,0,0,
    0,
};

/* ── USB Keyboard Report Handler ─────────────────────────────── */

static void usb_kbd_handle_report(xhci_device_t *dev, uint8_t *r, uint32_t len)
{
    if (len < 8) return;
    // serial_puts("[USB-KB] Report received\n");
    if (!kb_push) return; /* keyboard.c not linked */

    uint8_t mods = r[0];
    /* r[1] is reserved */
    bool shift = (mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
    bool ctrl  = (mods & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  != 0;

    /* Check for modifier changes (Ctrl, Shift, Alt, GUI) */
    uint8_t changed_mods = mods ^ dev->prev_mods;
    if (changed_mods) {
        for (int i = 0; i < 8; i++) {
            if (changed_mods & (1 << i)) {
                bool pressed = (mods & (1 << i)) != 0;
                /* Map HID modifier index to a virtual scancode (0xE0 + index) */
                extern void input_post_key(uint8_t scancode, bool pressed, bool extended);
                input_post_key(0xE0 + i, pressed, false);
            }
        }
    }

    /* Check for key releases (present in prev, missing in current) */
    for (int i = 0; i < 6; i++) {
        uint8_t prev_code = dev->prev_keys[i];
        if (prev_code == 0 || prev_code == 1) continue;

        bool still_pressed = false;
        for (int j = 2; j < 8; j++) {
            if (r[j] == prev_code) {
                still_pressed = true;
                break;
            }
        }
        if (!still_pressed) {
            /* Key released! */
            extern void input_post_key(uint8_t scancode, bool pressed, bool extended);
            input_post_key(prev_code, false, false);
        }
    }

    /* Process each keycode in slots 2-7 (KeyPresses) */
    for (int i = 2; i < 8; i++) {
        uint8_t code = r[i];
        if (code == 0 || code == 1) continue; /* No event / ErrorRollOver */

        /* Check if this key was already pressed in previous report */
        bool was_pressed = false;
        for (int j = 2; j < 8; j++) {
            if (dev->prev_keys[j - 2] == code) {
                was_pressed = true;
                break;
            }
        }
        if (was_pressed) continue; /* Key held — don't repeat */

        /* ── NEW: Post raw event to input system for Doom ── */
        extern void input_post_key(uint8_t scancode, bool pressed, bool extended);
        input_post_key(code, true, false);

        /* When compositor is running, it reads HID events from the
         * input_events ring (posted above) and routes to kb_push itself.
         * Skip direct kb_push here to avoid double input. */
        if (compositor_is_running && compositor_is_running())
            continue;

        /* Arrow keys → VT100 escape sequences */
        if (kb_push_esc) {
            if (code == 0x4F) { kb_push_esc("C"); continue; } /* Right */
            if (code == 0x50) { kb_push_esc("D"); continue; } /* Left */
            if (code == 0x51) { kb_push_esc("B"); continue; } /* Down */
            if (code == 0x52) { kb_push_esc("A"); continue; } /* Up */
            if (code == 0x4A) { kb_push_esc("H"); continue; } /* Home */
            if (code == 0x4D) { kb_push_esc("F"); continue; } /* End */
            if (code == 0x49) { kb_push_esc("2~"); continue; } /* Insert */
            if (code == 0x4C) { kb_push_esc("3~"); continue; } /* Delete */
            if (code == 0x4B) { kb_push_esc("5~"); continue; } /* Page Up */
            if (code == 0x4E) { kb_push_esc("6~"); continue; } /* Page Down */
        }

        /* Map to ASCII */
        if (code >= 0x54) continue; /* Outside our table */

        char c = shift ? hid_shifted[code] : hid_normal[code];

        /* Ctrl+letter → ASCII 1-26 */
        if (ctrl && c >= 'a' && c <= 'z') {
            kb_push(c - 'a' + 1);
            continue;
        }
        if (ctrl && c >= 'A' && c <= 'Z') {
            kb_push(c - 'A' + 1);
            continue;
        }

        if (c) kb_push(c);
    }

    /* Save current report for next comparison */
    for (int i = 0; i < 6; i++)
        dev->prev_keys[i] = r[i + 2];
    dev->prev_mods = mods;
}

/* ── Disable PCI MSI/MSI-X (prevent unhandled interrupts) ──── */

/* Walk PCI capability list and disable MSI + MSI-X.  BIOS may have
 * enabled them; without an IDT handler for the MSI vector, any
 * interrupt fires into an unexpected entry → triple fault. */
static void pci_disable_msi(uint8_t bus, uint8_t dev, uint8_t func)
{
    extern uint32_t pci_cfg_read32(uint8_t, uint8_t, uint8_t, uint16_t);
    extern void     pci_cfg_write32(uint8_t, uint8_t, uint8_t, uint16_t, uint32_t);

    uint32_t status = pci_cfg_read32(bus, dev, func, 0x06);
    if (!(status & (1 << 20))) return; /* No capabilities list (bit 4 of status at byte 0x06) */

    uint8_t cap_ptr = (uint8_t)(pci_cfg_read32(bus, dev, func, 0x34) & 0xFF);

    for (int i = 0; i < 48 && cap_ptr >= 0x40; i++) {
        uint32_t cap = pci_cfg_read32(bus, dev, func, cap_ptr);
        uint8_t id = cap & 0xFF;
        uint8_t next = (cap >> 8) & 0xFF;

        if (id == 0x05) {
            /* MSI: clear MSI Enable (bit 16 = bit 0 of Message Control at cap+2) */
            cap &= ~(1 << 16);
            pci_cfg_write32(bus, dev, func, cap_ptr, cap);
            serial_puts("[xHCI] MSI disabled\n");
        } else if (id == 0x11) {
            /* MSI-X: clear MSI-X Enable (bit 31 of dword at cap+0) */
            cap &= ~(1U << 31);
            pci_cfg_write32(bus, dev, func, cap_ptr, cap);
            serial_puts("[xHCI] MSI-X disabled\n");
        }

        cap_ptr = next;
    }
}

/* ── BIOS Handoff ────────────────────────────────────────────── */

static void xhci_bios_handoff(xhci_hc_t *hc)
{
    uint32_t hcc1 = xr32(hc, XHCI_CAP_HCCPARAMS1);
    uint32_t xecp = XHCI_HCC1_XECP(hcc1);
    if (xecp == 0) return;

    uint32_t off = xecp * 4;
    for (int i = 0; i < 32; i++) {
        uint32_t val = xr32(hc, off);
        uint8_t id = val & 0xFF;
        uint8_t next = (val >> 8) & 0xFF;

        if (id == XHCI_EXT_CAP_LEGACY) {
            if (val & XHCI_USBLEGSUP_BIOS) {
                serial_puts("[xHCI] BIOS handoff...\n");
                xw32(hc, off, val | XHCI_USBLEGSUP_OS);

                /* Wait for BIOS to release (timeout ~1s) */
                for (int t = 0; t < 100; t++) {
                    spin(10000);
                    val = xr32(hc, off);
                    if (!(val & XHCI_USBLEGSUP_BIOS)) break;
                }
                if (val & XHCI_USBLEGSUP_BIOS) {
                    serial_puts("[xHCI] BIOS handoff timeout, forcing\n");
                    xw32(hc, off, XHCI_USBLEGSUP_OS);
                }
                /* Disable SMI */
                xw32(hc, off + 4, 0);
            }
            return;
        }

        if (next == 0) break;
        off += next * 4;
    }
}

/* ── Command Ring ────────────────────────────────────────────── */

static void cmd_submit(xhci_hc_t *hc, xhci_trb_t *trb)
{
    uint32_t idx = hc->cmd_enq;

    /* Copy TRB with correct cycle bit */
    hc->cmd_ring[idx].param = trb->param;
    hc->cmd_ring[idx].status = trb->status;
    wmb();
    hc->cmd_ring[idx].control = trb->control |
        (hc->cmd_cycle ? TRB_CYCLE : 0);
    wmb();

    hc->cmd_enq++;
    if (hc->cmd_enq >= XHCI_CMD_RING_SIZE - 1) {
        /* Write Link TRB */
        hc->cmd_ring[hc->cmd_enq].param = (uint64_t)hc->cmd_ring;
        hc->cmd_ring[hc->cmd_enq].status = 0;
        hc->cmd_ring[hc->cmd_enq].control =
            XHCI_TRB_TYPE(TRB_LINK) | TRB_TC |
            (hc->cmd_cycle ? TRB_CYCLE : 0);
        wmb();
        hc->cmd_enq = 0;
        hc->cmd_cycle ^= 1;
    }

    /* Ring doorbell 0 (host controller) */
    db_write(hc, 0, 0);
}

/* Wait for command completion event. Returns completion code, slot in *slot_out */
static int cmd_wait(xhci_hc_t *hc, uint8_t *slot_out)
{
    for (int t = 0; t < 2000000; t++) {
        uint32_t idx = hc->evt_deq;
        xhci_trb_t *e = &hc->evt_ring[idx];
        rmb();
        uint8_t cycle = e->control & TRB_CYCLE;

        if (cycle != hc->evt_cycle) {
            __asm__ volatile ("pause");
            continue;
        }

        uint8_t type = XHCI_TRB_GET_TYPE(e->control);
        uint8_t code = (e->status >> 24) & 0xFF;

        if (slot_out)
            *slot_out = (e->control >> 24) & 0xFF;

        /* Advance dequeue */
        hc->evt_deq++;
        if (hc->evt_deq >= XHCI_EVT_RING_SIZE) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }

        /* Update ERDP */
        uint64_t erdp = (uint64_t)&hc->evt_ring[hc->evt_deq] | (1 << 3);
        rt_write64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);

        if (type == TRB_CMD_COMPLETION)
            return (int)code;
        /* If port status change, just consume and continue */
    }

    serial_puts("[xHCI] Command timeout\n");
    return -1;
}

/* ── Event Ring Poll (non-blocking) ──────────────────────────── */

static void evt_poll(xhci_hc_t *hc)
{
    /* Clear Interrupt Pending (IP) by writing 1 to bit 0, keep IE=1 (bit 1).
     * This allows the controller to set IP again for new events.
     * Required on AMD hardware where INTE+IE gate event ring generation. */
    uint32_t iman = 0x3; /* IP=1 (W1C to clear) | IE=1 (keep enabled) */
    rt_write(hc, XHCI_RT_IR0 + XHCI_IR_IMAN, iman);

    /* Also clear STS.EINT (Event Interrupt) by writing 1 */
    uint32_t usbsts = op_read(hc, XHCI_OP_USBSTS);
    if (usbsts & (1 << 3)) /* EINT bit */
        op_write(hc, XHCI_OP_USBSTS, (1 << 3)); /* W1C */

    /* Safety: check controller is still running */
    uint32_t sts = usbsts;
    if (sts & (XHCI_STS_HCH | XHCI_STS_HSE)) {
        static bool dead_printed;
        if (!dead_printed) {
            dead_printed = true;
            serial_puts("[xHCI] Controller DEAD (USBSTS=");
            serial_puthex(sts, 8);
            serial_puts(")\n");
        }
        hc->initialized = false; /* Controller dead, stop polling */
        return;
    }

    for (int rounds = 0; rounds < 16; rounds++) {
        uint32_t idx = hc->evt_deq;
        xhci_trb_t *e = &hc->evt_ring[idx];
        rmb();

        if ((e->control & TRB_CYCLE) != hc->evt_cycle)
            return;

        uint8_t type = XHCI_TRB_GET_TYPE(e->control);

        if (type == TRB_TRANSFER_EVENT) {
            /* Find which slot this belongs to */
            uint8_t slot = (e->control >> 24) & 0xFF;
            uint8_t ep_dci = (e->control >> 16) & 0x1F;
            uint32_t len = e->status & 0xFFFFFF;
            uint8_t code = (e->status >> 24) & 0xFF;

            if (slot > 0 && slot <= XHCI_MAX_SLOTS) {
                xhci_device_t *dev = &hc->devices[slot - 1];
                if (dev->hid_active && ep_dci == dev->int_ep_dci && dev->report_buf) {
                    /* Only process successful transfers (1=Success, 13=Short Packet) */
                    if (code != 1 && code != 13)
                        goto advance;

                    uint32_t actual = dev->int_max_pkt - len;
                    uint8_t *r = dev->report_buf;

                    if (actual >= 8) {
                        /* ── Keyboard report (usually 8 bytes) ── */
                        usb_kbd_handle_report(dev, r, actual);
                    } else if (actual >= 3) {
                        /* ── Mouse/tablet report (3-6 bytes) ── */
                        uint8_t buttons = r[0] & 0x07;

                        if (actual >= 6 && dev->hid_protocol != 2) {
                            /* USB tablet: 16-bit absolute coordinates (0-32767) */
                            int32_t ax = (int32_t)(uint16_t)(r[1] | (r[2] << 8));
                            int32_t ay = (int32_t)(uint16_t)(r[3] | (r[4] << 8));
                            input_set_mouse_abs(ax, ay);
                        } else if (actual >= 6) {
                            /* 6-byte relative mouse */
                            int16_t dx = (int16_t)(r[1] | (r[2] << 8));
                            int16_t dy = (int16_t)(r[3] | (r[4] << 8));
                            if (dx != 0 || dy != 0)
                                input_post_mouse_move(dx, dy);
                        } else {
                            /* 3-byte relative mouse */
                            int16_t dx = (int8_t)r[1];
                            int16_t dy = (int8_t)r[2];
                            if (dx != 0 || dy != 0)
                                input_post_mouse_move(dx, dy);
                        }
                        input_post_mouse_button(buttons);
                    }

                    /* Re-submit Normal TRB for next poll */
                    uint32_t ei = dev->int_enq;
                    dev->int_ring[ei].param = (uint64_t)dev->report_buf;
                    dev->int_ring[ei].status = dev->int_max_pkt;
                    wmb();
                    dev->int_ring[ei].control =
                        XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC |
                        (dev->int_cycle ? TRB_CYCLE : 0);
                    wmb();

                    dev->int_enq++;
                    if (dev->int_enq >= XHCI_XFER_RING_SIZE - 1) {
                        dev->int_ring[dev->int_enq].param = (uint64_t)dev->int_ring;
                        dev->int_ring[dev->int_enq].status = 0;
                        dev->int_ring[dev->int_enq].control =
                            XHCI_TRB_TYPE(TRB_LINK) | TRB_TC |
                            (dev->int_cycle ? TRB_CYCLE : 0);
                        wmb();
                        dev->int_enq = 0;
                        dev->int_cycle ^= 1;
                    }

                    /* Ring endpoint doorbell */
                    db_write(hc, slot, dev->int_ep_dci);
                }
            }
        }
        /* PORT_STATUS_CHANGE and other events — just consume */

advance:
        /* Advance event dequeue */
        hc->evt_deq++;
        if (hc->evt_deq >= XHCI_EVT_RING_SIZE) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }

        uint64_t erdp = (uint64_t)&hc->evt_ring[hc->evt_deq] | (1 << 3);
        rt_write64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);
    }
}

/* ── Control Transfer on EP0 ─────────────────────────────────── */

static int ctrl_transfer(xhci_hc_t *hc, xhci_device_t *dev,
                          usb_setup_t *setup, void *data, uint16_t len,
                          bool dir_in)
{
    /* Setup TRB */
    uint32_t ei = dev->ep0_enq;
    uint64_t setup_param;
    memcpy(&setup_param, setup, 8);

    dev->ep0_ring[ei].param = setup_param;
    dev->ep0_ring[ei].status = 8; /* setup packet length */
    uint32_t trt = (len == 0) ? TRB_TRT_NO_DATA :
                   (dir_in ? TRB_TRT_IN : TRB_TRT_OUT);
    dev->ep0_ring[ei].control =
        XHCI_TRB_TYPE(TRB_SETUP) | TRB_IDT | trt |
        (dev->ep0_cycle ? TRB_CYCLE : 0);
    wmb();

    ei++;
    if (ei >= XHCI_XFER_RING_SIZE - 1) { ei = 0; dev->ep0_cycle ^= 1; }

    /* Data TRB (if any) */
    if (len > 0 && data) {
        dev->ep0_ring[ei].param = (uint64_t)data;
        dev->ep0_ring[ei].status = len;
        dev->ep0_ring[ei].control =
            XHCI_TRB_TYPE(TRB_DATA) |
            (dir_in ? TRB_DIR_IN : 0) |
            (dev->ep0_cycle ? TRB_CYCLE : 0);
        wmb();

        ei++;
        if (ei >= XHCI_XFER_RING_SIZE - 1) { ei = 0; dev->ep0_cycle ^= 1; }
    }

    /* Status TRB */
    dev->ep0_ring[ei].param = 0;
    dev->ep0_ring[ei].status = 0;
    dev->ep0_ring[ei].control =
        XHCI_TRB_TYPE(TRB_STATUS) | TRB_IOC |
        (dir_in ? 0 : TRB_DIR_IN) | /* Status direction opposite of data */
        (dev->ep0_cycle ? TRB_CYCLE : 0);
    wmb();

    ei++;
    if (ei >= XHCI_XFER_RING_SIZE - 1) { ei = 0; dev->ep0_cycle ^= 1; }

    dev->ep0_enq = ei;

    /* Ring EP0 doorbell: slot N, target = DCI 1 (EP0) */
    db_write(hc, dev->slot_id, 1);

    /* Wait for transfer completion — Full Speed needs 50-500ms for retries.
     * 20M iters ≈ 340ms at 3.8GHz. */
    for (int t = 0; t < 20000000; t++) {
        uint32_t eidx = hc->evt_deq;
        xhci_trb_t *ev = &hc->evt_ring[eidx];
        rmb();
        if ((ev->control & TRB_CYCLE) != hc->evt_cycle) {
            __asm__ volatile ("pause");
            continue;
        }

        uint8_t etype = XHCI_TRB_GET_TYPE(ev->control);
        uint8_t ecode = (ev->status >> 24) & 0xFF;

        hc->evt_deq++;
        if (hc->evt_deq >= XHCI_EVT_RING_SIZE) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }
        uint64_t erdp = (uint64_t)&hc->evt_ring[hc->evt_deq] | (1 << 3);
        rt_write64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);

        if (etype == TRB_TRANSFER_EVENT)
            return (ecode == 1 || ecode == 13) ? 0 : -(int)ecode;
        /* Consume other events */
    }

    serial_puts("[xHCI] Control transfer timeout\n");
    return -1;
}

/* ── Max Packet Size for EP0 by speed ────────────────────────── */

static uint16_t ep0_max_pkt(uint8_t speed)
{
    if (speed == XHCI_SPEED_LOW)   return 8;
    if (speed == XHCI_SPEED_FULL)  return 8;   /* FS devices may only support 8; safe default */
    if (speed == XHCI_SPEED_HIGH)  return 64;
    if (speed == XHCI_SPEED_SUPER) return 512;
    return 64;
}

/* ── Context helpers ─────────────────────────────────────────── */

static void *ctx_entry(xhci_hc_t *hc, void *ctx_base, int index)
{
    return (uint8_t *)ctx_base + index * hc->ctx_size;
}

/* ── Enumerate one port ──────────────────────────────────────── */

static void enumerate_port(xhci_hc_t *hc, int port)
{
    uint32_t portsc = op_read(hc, XHCI_PORTSC(port));
    if (!(portsc & XHCI_PORTSC_CCS)) return; /* No device */

    /* Determine if this is a USB2 or USB3 port (1-based port number) */
    uint8_t port1 = (uint8_t)(port + 1);
    bool is_usb3 = (port1 >= hc->usb3_port_start &&
                    port1 < hc->usb3_port_start + hc->usb3_port_count);
    bool is_usb2 = (port1 >= hc->usb2_port_start &&
                    port1 < hc->usb2_port_start + hc->usb2_port_count);

    uint8_t speed = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & 0xF;
    uint8_t pls = (portsc >> 5) & 0xF;

    serial_puts("[xHCI] Port ");
    serial_putdec(port);
    serial_puts(is_usb3 ? " (USB3)" : is_usb2 ? " (USB2)" : " (?)");
    serial_puts(": ");
    serial_puts(speed_name(speed));
    serial_puts(" PLS=");
    serial_putdec(pls);
    serial_puts("\n");

    /* USB 3.0 port with no SuperSpeed link (PLS != U0/U1/U2/U3):
     * The device is likely USB 2.0 — it will enumerate on the companion
     * USB 2.0 port. Skip to avoid wasting time on failed resets. */
    if (is_usb3 && speed == 0) {
        serial_puts("[xHCI] USB3 port ");
        serial_putdec(port);
        serial_puts(": no SS link, skip (USB2 companion will handle)\n");
        return;
    }

    /* Port reset */
    portsc = op_read(hc, XHCI_PORTSC(port));
    portsc &= ~XHCI_PORTSC_CHANGE_BITS;
    portsc |= XHCI_PORTSC_PR;
    op_write(hc, XHCI_PORTSC(port), portsc);

    /* Wait for reset complete — USB 2.0 needs 10-50ms, USB 3.0 up to 100ms.
     * 10M iters ≈ 170ms at 3.8GHz (pause ~65 cycles on Zen 3). */
    int reset_ok = 0;
    for (int t = 0; t < 10000000; t++) {
        portsc = op_read(hc, XHCI_PORTSC(port));
        if (portsc & XHCI_PORTSC_PRC) { reset_ok = 1; break; }
        __asm__ volatile ("pause");
    }
    /* Clear PRC */
    portsc = op_read(hc, XHCI_PORTSC(port));
    portsc &= ~XHCI_PORTSC_CHANGE_BITS;
    portsc |= XHCI_PORTSC_PRC;
    op_write(hc, XHCI_PORTSC(port), portsc);

    if (!reset_ok) {
        serial_puts("[xHCI] Port ");
        serial_putdec(port);
        serial_puts(" reset TIMEOUT\n");
        return;
    }

    /* Re-read speed after reset */
    portsc = op_read(hc, XHCI_PORTSC(port));
    speed = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & 0xF;

    if (!(portsc & XHCI_PORTSC_PED)) {
        serial_puts("[xHCI] Port ");
        serial_putdec(port);
        serial_puts(" not enabled (PORTSC=");
        serial_puthex(portsc, 8);
        serial_puts(")\n");
        return;
    }

    /* ── Enable Slot ── */
    xhci_trb_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = XHCI_TRB_TYPE(TRB_ENABLE_SLOT);

    cmd_submit(hc, &cmd);
    uint8_t slot_id = 0;
    int code = cmd_wait(hc, &slot_id);
    if (code != 1 || slot_id == 0) {
        serial_puts("[xHCI] Enable Slot failed (code=");
        serial_putdec(code < 0 ? 0 : (uint64_t)code);
        serial_puts(")\n");
        return;
    }

    serial_puts("[xHCI] Slot ");
    serial_putdec(slot_id);
    serial_puts(" assigned to port ");
    serial_putdec(port);
    serial_puts("\n");

    if (slot_id > XHCI_MAX_SLOTS) return;

    xhci_device_t *dev = &hc->devices[slot_id - 1];
    memset(dev, 0, sizeof(*dev));
    dev->slot_id = slot_id;
    dev->port = (uint8_t)port;
    dev->speed = speed;

    /* ── Allocate Output Context ── */
    uint32_t ctx_total = hc->ctx_size * 32; /* 32 entries (1 slot + 31 EPs) */
    dev->output_ctx = mem_alloc_aligned(ctx_total, 4096);
    if (!dev->output_ctx) return;
    memset(dev->output_ctx, 0, ctx_total);
    hc->dcbaa[slot_id] = (uint64_t)dev->output_ctx;
    wmb();

    /* ── Allocate EP0 Transfer Ring ── */
    uint32_t ring_bytes = XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t);
    dev->ep0_ring = (xhci_trb_t *)mem_alloc_aligned(ring_bytes, 4096);
    if (!dev->ep0_ring) return;
    memset(dev->ep0_ring, 0, ring_bytes);
    dev->ep0_enq = 0;
    dev->ep0_cycle = 1;

    /* ── Build Input Context for Address Device ── */
    uint32_t in_ctx_total = hc->ctx_size * 33; /* input ctrl ctx + 32 entries */
    void *in_ctx = mem_alloc_aligned(in_ctx_total, 4096);
    if (!in_ctx) return;
    memset(in_ctx, 0, in_ctx_total);

    /* Input Control Context (entry 0) */
    xhci_input_ctrl_ctx_t *icc = (xhci_input_ctrl_ctx_t *)ctx_entry(hc, in_ctx, 0);
    icc->add_flags = 0x3; /* Add Slot (bit 0) + EP0 (bit 1) */

    /* Slot Context (entry 1) */
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)ctx_entry(hc, in_ctx, 1);
    slot_ctx->field1 = ((uint32_t)speed << 20) | (1 << 27); /* ctx_entries = 1 */
    slot_ctx->field2 = ((uint32_t)(port + 1) << 16); /* Root hub port (1-based) */

    /* EP0 Context (entry 2) */
    xhci_ep_ctx_t *ep0_ctx = (xhci_ep_ctx_t *)ctx_entry(hc, in_ctx, 2);
    uint16_t mps = ep0_max_pkt(speed);
    ep0_ctx->field2 = (3 << 1) | /* CErr = 3 */
                      (EP_TYPE_CONTROL << 3) |
                      ((uint32_t)mps << 16);
    ep0_ctx->tr_dequeue = (uint64_t)dev->ep0_ring | 1; /* DCS = 1 */
    ep0_ctx->field4 = 8; /* Average TRB length (8 for control) */

    /* ── Address Device (BSR=0) ── */
    memset(&cmd, 0, sizeof(cmd));
    cmd.param = (uint64_t)in_ctx;
    cmd.control = XHCI_TRB_TYPE(TRB_ADDRESS_DEVICE) |
                  ((uint32_t)slot_id << 24);

    cmd_submit(hc, &cmd);
    uint8_t dummy;
    code = cmd_wait(hc, &dummy);
    if (code != 1) {
        serial_puts("[xHCI] Address Device failed (code=");
        serial_putdec(code < 0 ? 0 : (uint64_t)code);
        serial_puts(")\n");
        return;
    }

    dev->addressed = true;
    serial_puts("[xHCI] Device addressed on slot ");
    serial_putdec(slot_id);
    serial_puts("\n");

    /* Post-SET_ADDRESS recovery: USB spec requires up to 50ms
     * for device to accept new address. 2M iters ≈ 34ms. */
    spin(2000000);

    /* ── GET_DEVICE_DESCRIPTOR (8-byte short read first) ── */
    uint8_t *desc_buf = (uint8_t *)mem_alloc_aligned(256, 64);
    if (!desc_buf) return;
    memset(desc_buf, 0, 256);

    usb_setup_t setup;
    setup.bmRequestType = 0x80;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (USB_DESC_DEVICE << 8);
    setup.wIndex = 0;
    setup.wLength = 8;   /* Only first 8 bytes — safe for all EP0 sizes */

    int xfer_ret = ctrl_transfer(hc, dev, &setup, desc_buf, 8, true);
    if (xfer_ret < 0) {
        serial_puts("[xHCI] GET_DEVICE_DESCRIPTOR(8) failed code=");
        serial_putdec((uint64_t)(-(int64_t)xfer_ret));
        serial_puts("\n");
        return;
    }

    /* bMaxPacketSize0 is at offset 7 of the device descriptor */
    uint8_t actual_mps = desc_buf[7];
    if (actual_mps == 0) actual_mps = 8; /* Sanity */
    serial_puts("[xHCI] bMaxPacketSize0=");
    serial_putdec(actual_mps);
    serial_puts("\n");

    /* If actual max packet > what we configured, update EP0 via Evaluate Context */
    uint16_t cur_mps = ep0_max_pkt(speed);
    if (actual_mps > cur_mps) {
        serial_puts("[xHCI] Updating EP0 max_pkt ");
        serial_putdec(cur_mps);
        serial_puts(" -> ");
        serial_putdec(actual_mps);
        serial_puts("\n");

        /* Build Evaluate Context input */
        uint32_t eval_ctx_total = hc->ctx_size * 33;
        void *eval_ctx = mem_alloc_aligned(eval_ctx_total, 4096);
        if (eval_ctx) {
            memset(eval_ctx, 0, eval_ctx_total);

            /* Input Control Context: evaluate EP0 (bit 1) */
            xhci_input_ctrl_ctx_t *eval_icc =
                (xhci_input_ctrl_ctx_t *)ctx_entry(hc, eval_ctx, 0);
            eval_icc->add_flags = (1 << 1); /* EP0 only */

            /* EP0 Context with updated max packet size */
            xhci_ep_ctx_t *eval_ep0 =
                (xhci_ep_ctx_t *)ctx_entry(hc, eval_ctx, 2);
            eval_ep0->field2 = (3 << 1) | /* CErr = 3 */
                               (EP_TYPE_CONTROL << 3) |
                               ((uint32_t)actual_mps << 16);
            /* Must also set tr_dequeue — copy from current state */
            eval_ep0->tr_dequeue = (uint64_t)dev->ep0_ring |
                                   (dev->ep0_cycle ? 1 : 0);
            /* Re-read dequeue from output context if available */
            xhci_ep_ctx_t *out_ep0 =
                (xhci_ep_ctx_t *)ctx_entry(hc, dev->output_ctx, 1);
            eval_ep0->tr_dequeue = out_ep0->tr_dequeue;
            eval_ep0->field4 = 8;

            xhci_trb_t eval_cmd;
            memset(&eval_cmd, 0, sizeof(eval_cmd));
            eval_cmd.param = (uint64_t)eval_ctx;
            eval_cmd.control = XHCI_TRB_TYPE(TRB_EVALUATE_CONTEXT) |
                               ((uint32_t)slot_id << 24);

            cmd_submit(hc, &eval_cmd);
            uint8_t edummy;
            int ecode = cmd_wait(hc, &edummy);
            if (ecode == 1) {
                serial_puts("[xHCI] Evaluate Context OK\n");
            } else {
                serial_puts("[xHCI] Evaluate Context failed code=");
                serial_putdec(ecode < 0 ? 0 : (uint64_t)ecode);
                serial_puts("\n");
            }
        }
    }

    /* ── GET_DEVICE_DESCRIPTOR (full 18 bytes) ── */
    spin(500000); /* ~8ms recovery between control transfers */
    memset(desc_buf, 0, 256);
    setup.wLength = 18;

    xfer_ret = ctrl_transfer(hc, dev, &setup, desc_buf, 18, true);
    if (xfer_ret < 0) {
        serial_puts("[xHCI] GET_DEVICE_DESCRIPTOR(18) failed code=");
        serial_putdec((uint64_t)(-(int64_t)xfer_ret));
        serial_puts("\n");
        return;
    }

    dev->vendor_id = (uint16_t)(desc_buf[8] | (desc_buf[9] << 8));
    dev->product_id = (uint16_t)(desc_buf[10] | (desc_buf[11] << 8));
    dev->class_code = desc_buf[4];
    dev->subclass = desc_buf[5];
    dev->protocol = desc_buf[6];

    serial_puts("[xHCI] USB Device: ");
    serial_puthex(dev->vendor_id, 4);
    serial_puts(":");
    serial_puthex(dev->product_id, 4);
    serial_puts(" class=");
    serial_puthex(dev->class_code, 2);
    serial_puts("\n");

    /* VID:PID details go to serial only */

    /* ── GET_CONFIGURATION_DESCRIPTOR ── */
    memset(desc_buf, 0, 256);
    setup.wValue = (USB_DESC_CONFIGURATION << 8);
    setup.wLength = 9; /* Just the header first */

    if (ctrl_transfer(hc, dev, &setup, desc_buf, 9, true) < 0) {
        serial_puts("[xHCI] GET_CONFIG_DESC(header) failed\n");
        return;
    }

    uint16_t total_len = (uint16_t)(desc_buf[2] | (desc_buf[3] << 8));
    if (total_len > 255) total_len = 255;
    uint8_t config_val = desc_buf[5];

    memset(desc_buf, 0, 256);
    setup.wLength = total_len;

    if (ctrl_transfer(hc, dev, &setup, desc_buf, total_len, true) < 0) {
        serial_puts("[xHCI] GET_CONFIG_DESC(full) failed\n");
        return;
    }

    /* Parse for HID Interrupt IN endpoint.
     *
     * USB composite devices (gaming keyboards) have multiple HID interfaces:
     *   iface 0: keyboard (class=3, proto=1), EP 0x81 (EP1 IN, 8 bytes)
     *   iface 1: consumer control (class=3, proto=0), EP 0x82 (EP2 IN, 16 bytes)
     *   iface 2: vendor LED (class=3, proto=0), EP 0x83 (EP3 IN, 8 bytes)
     *
     * Endpoint descriptors follow their owning interface descriptor.
     * We MUST only collect endpoints from the selected interface, otherwise
     * we'd configure EP2/EP3 (consumer/vendor) instead of EP1 (keyboard)
     * and get zero transfer events when normal keys are pressed.
     */
    uint8_t hid_iface = 0xFF;
    uint8_t hid_proto = 0;   /* 1 = keyboard, 2 = mouse */
    uint8_t int_ep_addr = 0;
    uint16_t int_max_pkt_found = 0;
    uint8_t int_interval = 0;
    bool collecting_eps = false; /* true = current interface is selected */

    uint32_t pos = 0;
    while (pos + 2 <= total_len) {
        uint8_t dlen = desc_buf[pos];
        uint8_t dtype = desc_buf[pos + 1];
        if (dlen < 2) break;

        if (dtype == USB_DESC_INTERFACE && pos + 9 <= total_len) {
            uint8_t iclass = desc_buf[pos + 5];
            uint8_t isub = desc_buf[pos + 6];
            uint8_t iproto = desc_buf[pos + 7];

            /* New interface → stop collecting endpoints from previous */
            collecting_eps = false;

            if (iclass == 3) { /* HID */
                uint8_t this_iface = desc_buf[pos + 2];
                serial_puts("[xHCI] HID interface ");
                serial_putdec(this_iface);
                serial_puts(" (subclass=");
                serial_putdec(isub);
                serial_puts(" proto=");
                serial_putdec(iproto);
                serial_puts(")\n");

                /* Accept keyboard (proto=1), mouse (proto=2), or tablet (proto=0 with HID class). */
                if (iproto == 1 || iproto == 2 || iproto == 0 || hid_iface == 0xFF) {
                    hid_iface = this_iface;
                    hid_proto = iproto;
                    int_ep_addr = 0; /* reset EP — pick from THIS interface */
                    collecting_eps = true; /* Collect endpoints for this interface */
                }
            }
        }

        if (dtype == USB_DESC_ENDPOINT && pos + 7 <= total_len && collecting_eps) {
            uint8_t ep_addr = desc_buf[pos + 2];
            uint8_t ep_attr = desc_buf[pos + 3];
            uint16_t ep_mps = (uint16_t)(desc_buf[pos + 4] | (desc_buf[pos + 5] << 8));
            uint8_t ep_int = desc_buf[pos + 6];

            /* Interrupt IN? */
            if ((ep_attr & 0x03) == 0x03 && (ep_addr & 0x80)) {
                int_ep_addr = ep_addr;
                int_max_pkt_found = ep_mps & 0x7FF;
                int_interval = ep_int;
                serial_puts("[xHCI]  -> EP: ");
                serial_puthex(ep_addr, 2);
                serial_puts(" maxpkt=");
                serial_putdec(int_max_pkt_found);
                serial_puts(" interval=");
                serial_putdec(int_interval);
                serial_puts(" (selected)\n");
            }
        }

        pos += dlen;
    }

    if (int_ep_addr == 0) {
        serial_puts("[xHCI] No HID interrupt endpoint found\n");
        return;
    }

    /* ── SET_CONFIGURATION ── */
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_CONFIG;
    setup.wValue = config_val;
    setup.wIndex = 0;
    setup.wLength = 0;

    if (ctrl_transfer(hc, dev, &setup, NULL, 0, false) < 0) {
        serial_puts("[xHCI] SET_CONFIGURATION failed\n");
        return;
    }

    /* ── SET_PROTOCOL (boot protocol = 0) ── */
    setup.bmRequestType = 0x21; /* Class, Interface, Host-to-Device */
    setup.bRequest = USB_REQ_SET_PROTOCOL;
    setup.wValue = 0; /* Boot protocol */
    setup.wIndex = hid_iface;
    setup.wLength = 0;
    ctrl_transfer(hc, dev, &setup, NULL, 0, false); /* OK if fails */

    /* ── SET_IDLE (rate = 0, infinite) ── */
    setup.bRequest = USB_REQ_SET_IDLE;
    setup.wValue = 0;
    setup.wIndex = hid_iface;
    ctrl_transfer(hc, dev, &setup, NULL, 0, false); /* OK if fails */

    /* ── Configure Interrupt IN Endpoint ── */
    uint8_t ep_num = int_ep_addr & 0x0F;
    uint8_t ep_dci = ep_num * 2 + 1; /* IN endpoint DCI */

    /* Allocate interrupt transfer ring */
    dev->int_ring = (xhci_trb_t *)mem_alloc_aligned(
        XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t), 4096);
    if (!dev->int_ring) return;
    memset(dev->int_ring, 0, XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t));
    dev->int_enq = 0;
    dev->int_cycle = 1;
    dev->int_ep_dci = ep_dci;
    dev->int_max_pkt = int_max_pkt_found;

    /* Allocate report buffer */
    dev->report_buf = (uint8_t *)mem_alloc_aligned(int_max_pkt_found, 64);
    if (!dev->report_buf) return;
    memset(dev->report_buf, 0, int_max_pkt_found);

    /* Build Input Context for Configure Endpoint */
    memset(in_ctx, 0, in_ctx_total);
    icc = (xhci_input_ctrl_ctx_t *)ctx_entry(hc, in_ctx, 0);
    icc->add_flags = (1 << 0) | (1 << ep_dci); /* Slot + this EP */

    /* Update Slot Context entries */
    slot_ctx = (xhci_slot_ctx_t *)ctx_entry(hc, in_ctx, 1);
    /* Read current slot from output context */
    xhci_slot_ctx_t *out_slot = (xhci_slot_ctx_t *)ctx_entry(hc, dev->output_ctx, 0);
    *slot_ctx = *out_slot;
    /* Update context entries to include the new EP */
    slot_ctx->field1 &= ~(0x1F << 27);
    slot_ctx->field1 |= ((uint32_t)ep_dci << 27); /* context entries = ep_dci */

    /* Interrupt IN EP context */
    xhci_ep_ctx_t *int_ep = (xhci_ep_ctx_t *)ctx_entry(hc, in_ctx, ep_dci + 1);
    /* Calculate interval: for LS/FS, interval = endpoint bInterval (ms).
       For HS/SS, interval = 2^(bInterval-1) * 125µs.
       xHCI interval field = bInterval for HS/SS, log2(bInterval*8) for FS/LS */
    uint8_t xhci_interval;
    if (speed <= XHCI_SPEED_FULL) {
        /* FS/LS: interval in frames. xHCI wants exponent. */
        xhci_interval = 3; /* ~1ms minimum */
        for (uint8_t i = 3; i < 11; i++) {
            if ((uint32_t)(1 << i) >= (uint32_t)int_interval * 8) {
                xhci_interval = i;
                break;
            }
        }
    } else {
        xhci_interval = int_interval;
        if (xhci_interval > 0) xhci_interval--;
    }

    int_ep->field1 = ((uint32_t)xhci_interval << 16);
    int_ep->field2 = (3 << 1) | /* CErr = 3 */
                     (EP_TYPE_INTERRUPT_IN << 3) |
                     ((uint32_t)int_max_pkt_found << 16);
    int_ep->tr_dequeue = (uint64_t)dev->int_ring | 1; /* DCS = 1 */
    int_ep->field4 = int_max_pkt_found; /* Average TRB length */

    /* Submit Configure Endpoint command */
    memset(&cmd, 0, sizeof(cmd));
    cmd.param = (uint64_t)in_ctx;
    cmd.control = XHCI_TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                  ((uint32_t)slot_id << 24);

    cmd_submit(hc, &cmd);
    code = cmd_wait(hc, &dummy);
    if (code != 1) {
        serial_puts("[xHCI] Configure Endpoint failed (code=");
        serial_putdec(code < 0 ? 0 : (uint64_t)code);
        serial_puts(")\n");
        return;
    }

    /* ── Submit initial interrupt IN TRBs ── */
    for (int i = 0; i < 4; i++) {
        uint32_t ei2 = dev->int_enq;
        dev->int_ring[ei2].param = (uint64_t)dev->report_buf;
        dev->int_ring[ei2].status = int_max_pkt_found;
        dev->int_ring[ei2].control =
            XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC |
            (dev->int_cycle ? TRB_CYCLE : 0);
        wmb();

        dev->int_enq++;
        if (dev->int_enq >= XHCI_XFER_RING_SIZE - 1) {
            dev->int_ring[dev->int_enq].param = (uint64_t)dev->int_ring;
            dev->int_ring[dev->int_enq].status = 0;
            dev->int_ring[dev->int_enq].control =
                XHCI_TRB_TYPE(TRB_LINK) | TRB_TC |
                (dev->int_cycle ? TRB_CYCLE : 0);
            wmb();
            dev->int_enq = 0;
            dev->int_cycle ^= 1;
        }
    }

    /* Ring doorbell to start polling */
    db_write(hc, slot_id, ep_dci);

    dev->hid_active = true;
    dev->hid_protocol = hid_proto;
    hc->num_devices++;

    const char *kind = (hid_proto == 1) ? "keyboard" :
                       (hid_proto == 2) ? "mouse" :
                       (hid_proto == 0) ? "tablet" : "HID";
    serial_puts("[xHCI] HID ");
    serial_puts(kind);
    serial_puts(" active on slot ");
    serial_putdec(slot_id);
    serial_puts(", EP ");
    serial_puthex(int_ep_addr, 2);
    serial_puts("\n");

}

/* ── Init one xHCI controller ────────────────────────────────── */

int xhci_init(uint64_t bar0_phys, uint8_t bus, uint8_t dev, uint8_t func)
{
    if (hc_count >= XHCI_MAX_CONTROLLERS) return -1;

    xhci_hc_t *hc = &hc_list[hc_count];
    memset(hc, 0, sizeof(*hc));
    hc->base = (volatile void *)bar0_phys;
    hc->pci_bus = bus;
    hc->pci_dev = dev;
    hc->pci_func = func;

    serial_puts("[xHCI] Init controller ");
    serial_putdec(hc_count);
    serial_puts(" BAR0=");
    serial_puthex(bar0_phys, 16);
    serial_puts("\n");

    /* Validate BAR0 */
    if (bar0_phys == 0 || bar0_phys == 0xFFFFFFFFFFFFFFFFULL) {
        serial_puts("[xHCI] Invalid BAR0\n");
        return -1;
    }

    /* Enable PCI bus mastering + memory space */
    pci_enable_bus_master(bus, dev, func);

    /* Disable MSI/MSI-X at PCI level — prevents unhandled interrupts */
    pci_disable_msi(bus, dev, func);

    /* ── Read Capabilities ── */
    uint8_t caplength = (uint8_t)(xr32(hc, XHCI_CAP_CAPLENGTH) & 0xFF);
    if (caplength == 0 || caplength == 0xFF) {
        serial_puts("[xHCI] Invalid CAPLENGTH (BAR0 unmapped?)\n");
        return -1;
    }
    uint16_t version = (uint16_t)(xr32(hc, XHCI_CAP_CAPLENGTH) >> 16);
    uint32_t hcs1 = xr32(hc, XHCI_CAP_HCSPARAMS1);
    uint32_t hcs2 = xr32(hc, XHCI_CAP_HCSPARAMS2);
    uint32_t hcc1 = xr32(hc, XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = xr32(hc, XHCI_CAP_DBOFF) & ~0x3;
    uint32_t rtsoff = xr32(hc, XHCI_CAP_RTSOFF) & ~0x1F;

    hc->op_off = caplength;
    hc->rt_off = rtsoff;
    hc->db_off = dboff;
    hc->max_slots = XHCI_HCS1_MAXSLOTS(hcs1);
    hc->max_ports = XHCI_HCS1_MAXPORTS(hcs1);
    hc->ctx_size = XHCI_HCC1_CSZ(hcc1) ? 64 : 32;

    if (hc->max_slots > XHCI_MAX_SLOTS)
        hc->max_slots = XHCI_MAX_SLOTS;

    serial_puts("[xHCI] Version ");
    serial_puthex(version, 4);
    serial_puts(", Slots=");
    serial_putdec(hc->max_slots);
    serial_puts(", Ports=");
    serial_putdec(hc->max_ports);
    serial_puts(", CtxSize=");
    serial_putdec(hc->ctx_size);
    serial_puts("\n");

    /* ── BIOS Handoff ── */
    xhci_bios_handoff(hc);

    /* ── Parse Extended Capabilities for USB2/USB3 port ranges ── */
    {
        uint32_t xecp = XHCI_HCC1_XECP(hcc1);
        if (xecp) {
            uint32_t off = xecp * 4;
            for (int i = 0; i < 32; i++) {
                uint32_t val = xr32(hc, off);
                uint8_t id = val & 0xFF;
                uint8_t next = (val >> 8) & 0xFF;

                if (id == 2) { /* Supported Protocol */
                    uint32_t rev_major = (val >> 24) & 0xFF;
                    uint32_t word2 = xr32(hc, off + 8);
                    uint8_t port_off = word2 & 0xFF;       /* 1-based */
                    uint8_t port_cnt = (word2 >> 8) & 0xFF;

                    if (rev_major == 2) {
                        hc->usb2_port_start = port_off;
                        hc->usb2_port_count = port_cnt;
                    } else if (rev_major == 3) {
                        hc->usb3_port_start = port_off;
                        hc->usb3_port_count = port_cnt;
                    }

                    serial_puts("[xHCI] USB ");
                    serial_putdec(rev_major);
                    serial_puts(".x ports ");
                    serial_putdec(port_off);
                    serial_puts("-");
                    serial_putdec(port_off + port_cnt - 1);
                    serial_puts(" (");
                    serial_putdec(port_cnt);
                    serial_puts(")\n");

                    /* USB port range details go to serial only */
                }

                if (next == 0) break;
                off += next * 4;
            }
        }
    }

    /* ── Halt Controller ── */
    uint32_t usbcmd = op_read(hc, XHCI_OP_USBCMD);
    op_write(hc, XHCI_OP_USBCMD, usbcmd & ~XHCI_CMD_RUN);

    for (int t = 0; t < 100000; t++) {
        if (op_read(hc, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
        __asm__ volatile ("pause");
    }
    if (!(op_read(hc, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        serial_puts("[xHCI] Failed to halt\n");
        return -1;
    }

    /* ── Reset Controller ── */
    op_write(hc, XHCI_OP_USBCMD, XHCI_CMD_HCRST);

    for (int t = 0; t < 1000000; t++) {
        uint32_t cmd2 = op_read(hc, XHCI_OP_USBCMD);
        uint32_t sts = op_read(hc, XHCI_OP_USBSTS);
        if (!(cmd2 & XHCI_CMD_HCRST) && !(sts & XHCI_STS_CNR)) break;
        __asm__ volatile ("pause");
    }
    if (op_read(hc, XHCI_OP_USBCMD) & XHCI_CMD_HCRST) {
        serial_puts("[xHCI] Reset timeout\n");
        return -1;
    }

    serial_puts("[xHCI] Controller reset OK\n");

    /* ── Configure MaxSlots ── */
    op_write(hc, XHCI_OP_CONFIG, hc->max_slots);

    /* ── Allocate DCBAA ── */
    uint32_t dcbaa_size = (hc->max_slots + 1) * 8;
    hc->dcbaa = (uint64_t *)mem_alloc_aligned(dcbaa_size, 4096);
    if (!hc->dcbaa) return -1;
    memset(hc->dcbaa, 0, dcbaa_size);

    /* ── Scratchpad Buffers ── */
    uint32_t scratch_hi = XHCI_HCS2_SCRATCH_HI(hcs2);
    uint32_t scratch_lo = XHCI_HCS2_SCRATCH_LO(hcs2);
    uint32_t scratch_count = (scratch_hi << 5) | scratch_lo;

    if (scratch_count > 0) {
        serial_puts("[xHCI] Allocating ");
        serial_putdec(scratch_count);
        serial_puts(" scratchpad buffers\n");

        hc->scratchpad = (uint64_t *)mem_alloc_aligned(scratch_count * 8, 4096);
        if (!hc->scratchpad) return -1;

        for (uint32_t i = 0; i < scratch_count; i++) {
            void *page = mem_alloc_aligned(4096, 4096);
            if (!page) return -1;
            memset(page, 0, 4096);
            hc->scratchpad[i] = (uint64_t)page;
        }
        hc->dcbaa[0] = (uint64_t)hc->scratchpad;
    }

    op_write64(hc, XHCI_OP_DCBAAP, (uint64_t)hc->dcbaa);

    /* ── Command Ring ── */
    uint32_t cmd_bytes = XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t);
    hc->cmd_ring = (xhci_trb_t *)mem_alloc_aligned(cmd_bytes, 4096);
    if (!hc->cmd_ring) return -1;
    memset(hc->cmd_ring, 0, cmd_bytes);
    hc->cmd_enq = 0;
    hc->cmd_cycle = 1;

    op_write64(hc, XHCI_OP_CRCR, (uint64_t)hc->cmd_ring | 1); /* RCS=1 matches cmd_cycle=1 */

    /* ── Event Ring ── */
    uint32_t evt_bytes = XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t);
    hc->evt_ring = (xhci_trb_t *)mem_alloc_aligned(evt_bytes, 4096);
    if (!hc->evt_ring) return -1;
    memset(hc->evt_ring, 0, evt_bytes);
    hc->evt_deq = 0;
    hc->evt_cycle = 1;

    hc->erst = (xhci_erste_t *)mem_alloc_aligned(sizeof(xhci_erste_t), 64);
    if (!hc->erst) return -1;
    hc->erst->ring_base = (uint64_t)hc->evt_ring;
    hc->erst->ring_size = XHCI_EVT_RING_SIZE;
    hc->erst->reserved = 0;

    /* Configure interrupter 0 */
    uint32_t ir0 = XHCI_RT_IR0;
    rt_write(hc, ir0 + XHCI_IR_ERSTSZ, 1);
    rt_write64(hc, ir0 + XHCI_IR_ERDP, (uint64_t)hc->evt_ring | (1 << 3));
    rt_write64(hc, ir0 + XHCI_IR_ERSTBA, (uint64_t)hc->erst);

    /* Interrupter: MUST enable IE (IMAN bit 1) and INTE (USBCMD bit 2).
     * AMD xHCI controllers on real hardware require the interrupter to be
     * enabled for the event ring to generate Transfer Events. Without IE+INTE,
     * endpoints show "running" but the controller never writes events.
     * QEMU's xHCI emulation works without these — real AMD hardware does NOT.
     *
     * MSI/MSI-X are already disabled at PCI level (pci_disable_msi), so no
     * MSI vectors fire. INTx goes through IOAPIC which we haven't configured
     * for PCI devices, so no actual CPU interrupts are delivered.
     * We still use polling (xhci_poll) to consume the event ring. */
    rt_write(hc, ir0 + XHCI_IR_IMAN, 0x2); /* IE=1 (bit 1), IP=0 (bit 0) */
    rt_write(hc, ir0 + XHCI_IR_IMOD, 160); /* ~10µs moderation */

    /* ── Start Controller (INTE=1 required for AMD event ring) ── */
    op_write(hc, XHCI_OP_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);

    for (int t = 0; t < 100000; t++) {
        if (!(op_read(hc, XHCI_OP_USBSTS) & XHCI_STS_HCH)) break;
        __asm__ volatile ("pause");
    }
    if (op_read(hc, XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        serial_puts("[xHCI] Failed to start\n");
        return -1;
    }

    hc->initialized = true;
    hc_count++;

    serial_puts("[xHCI] Controller running, powering ports...\n");

    /* ── Power on all ports ──
     * After HCRST, if PPC (Port Power Control) = 1 in HCCPARAMS1,
     * PP bits in PORTSC are cleared → ports have no power → no devices.
     * AMD 400-series xHCI has PPC=1. Must set PP=1 on every port. */
    {
        uint32_t pp_set = 0;
        for (uint32_t p = 0; p < hc->max_ports; p++) {
            uint32_t portsc = op_read(hc, XHCI_PORTSC(p));
            if (!(portsc & XHCI_PORTSC_PP)) {
                portsc &= ~XHCI_PORTSC_CHANGE_BITS;
                portsc |= XHCI_PORTSC_PP;
                op_write(hc, XHCI_PORTSC(p), portsc);
                pp_set++;
            }
        }
        serial_puts("[xHCI] Ports powered (");
        serial_putdec(pp_set);
        serial_puts(" were off)\n");
    }

    /* ── Wait for link training ──
     * After HCRST + port power, USB devices need:
     * 1. VBUS stabilization (~100ms)
     * 2. USB 3.0 LFPS / link training (~100-360ms)
     * 3. USB 2.0 connect detect (~100ms)
     * 40M iters ≈ 1.0s at 3.8GHz. */
    spin(40000000);

    /* ── Drain pending Port Status Change events ──
     * Controller generates PSC events when devices connect. If event
     * ring isn't serviced, controller may stall. */
    for (int rounds = 0; rounds < 64; rounds++) {
        uint32_t idx = hc->evt_deq;
        xhci_trb_t *e = &hc->evt_ring[idx];
        rmb();
        if ((e->control & TRB_CYCLE) != hc->evt_cycle) break;

        hc->evt_deq++;
        if (hc->evt_deq >= XHCI_EVT_RING_SIZE) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }
        uint64_t erdp = (uint64_t)&hc->evt_ring[hc->evt_deq] | (1 << 3);
        rt_write64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);
    }

    /* ── Diagnostic: dump all port status to serial ── */
    uint32_t ccs_count = 0;
    for (uint32_t p = 0; p < hc->max_ports; p++) {
        uint32_t portsc = op_read(hc, XHCI_PORTSC(p));
        uint8_t ccs = portsc & 1;
        uint8_t ped = (portsc >> 1) & 1;
        uint8_t pls = (portsc >> 5) & 0xF;
        uint8_t pp  = (portsc >> 9) & 1;
        uint8_t spd = (portsc >> 10) & 0xF;
        if (ccs) ccs_count++;

        serial_puts("  P");
        serial_putdec(p);
        serial_puts(": ");
        serial_puthex(portsc, 8);
        serial_puts(" PP=");
        serial_putdec(pp);
        serial_puts(" CCS=");
        serial_putdec(ccs);
        serial_puts(" PED=");
        serial_putdec(ped);
        serial_puts(" PLS=");
        serial_putdec(pls);
        serial_puts(" SPD=");
        serial_putdec(spd);
        serial_puts("\n");
    }
    serial_puts("[xHCI] ");
    serial_putdec(ccs_count);
    serial_puts("/");
    serial_putdec(hc->max_ports);
    serial_puts(" ports connected\n");

    /* ── Enumerate connected ports ── */
    for (uint32_t p = 0; p < hc->max_ports; p++)
        enumerate_port(hc, (int)p);

    serial_puts("[xHCI] Init complete: ");
    serial_putdec(hc->num_devices);
    serial_puts(" device(s)\n");

    /* xHCI summary goes to serial; main.c shows compact HW info */

    return 0;
}

/* ── Poll all controllers ────────────────────────────────────── */

void xhci_poll(void)
{
    for (int i = 0; i < hc_count; i++) {
        if (hc_list[i].initialized)
            evt_poll(&hc_list[i]);
    }
}

bool xhci_is_ready(void)
{
    return hc_count > 0;
}
