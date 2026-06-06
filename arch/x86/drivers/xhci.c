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
#include "../include/paging.h"
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

/*
 * Convert a HID keycode (page 7) to ASCII via boot-protocol-style tables
 * and push it to the terminal kb_buf, honoring shift and ctrl modifiers.
 * Also generates VT100 escape sequences for arrow / Home / End / etc.
 */
static void kbd_route_to_term(uint8_t code, bool shift, bool ctrl)
{
    if (!kb_push) return;

    /* Compositor owns input routing when running — drop here, it pulls
     * raw HID from the input ring and pushes to kb_buf if the terminal
     * window is focused. */
    extern bool compositor_is_running(void) __attribute__((weak));
    if (compositor_is_running && compositor_is_running()) return;

    if (kb_push_esc) {
        if (code == 0x4F) { kb_push_esc("C");  return; }
        if (code == 0x50) { kb_push_esc("D");  return; }
        if (code == 0x51) { kb_push_esc("B");  return; }
        if (code == 0x52) { kb_push_esc("A");  return; }
        if (code == 0x4A) { kb_push_esc("H");  return; }
        if (code == 0x4D) { kb_push_esc("F");  return; }
        if (code == 0x49) { kb_push_esc("2~"); return; }
        if (code == 0x4C) { kb_push_esc("3~"); return; }
        if (code == 0x4B) { kb_push_esc("5~"); return; }
        if (code == 0x4E) { kb_push_esc("6~"); return; }
    }

    if (code >= 0x54) return;
    char c = shift ? hid_shifted[code] : hid_normal[code];
    if (ctrl && c >= 'a' && c <= 'z') { kb_push(c - 'a' + 1); return; }
    if (ctrl && c >= 'A' && c <= 'Z') { kb_push(c - 'A' + 1); return; }
    if (c) kb_push(c);
}

/* HID usage (page 7) → PS/2 scancode set 1, for the non-extended main block.
 * 0 = unmapped/extended (handled separately in hid_route_to_win32). */
static const uint8_t hid_to_ps2_set1[0x59] = {
    [0x04]=0x1E,[0x05]=0x30,[0x06]=0x2E,[0x07]=0x20,[0x08]=0x12,[0x09]=0x21,
    [0x0A]=0x22,[0x0B]=0x23,[0x0C]=0x17,[0x0D]=0x24,[0x0E]=0x25,[0x0F]=0x26,
    [0x10]=0x32,[0x11]=0x31,[0x12]=0x18,[0x13]=0x19,[0x14]=0x10,[0x15]=0x13,
    [0x16]=0x1F,[0x17]=0x14,[0x18]=0x16,[0x19]=0x2F,[0x1A]=0x11,[0x1B]=0x2D,
    [0x1C]=0x15,[0x1D]=0x2C,
    [0x1E]=0x02,[0x1F]=0x03,[0x20]=0x04,[0x21]=0x05,[0x22]=0x06,[0x23]=0x07,
    [0x24]=0x08,[0x25]=0x09,[0x26]=0x0A,[0x27]=0x0B,
    [0x28]=0x1C/*Enter*/,[0x29]=0x01/*Esc*/,[0x2A]=0x0E/*Bksp*/,[0x2B]=0x0F/*Tab*/,
    [0x2C]=0x39/*Space*/,[0x2D]=0x0C,[0x2E]=0x0D,[0x2F]=0x1A,[0x30]=0x1B,
    [0x31]=0x2B,[0x32]=0x2B,[0x33]=0x27,[0x34]=0x28,[0x35]=0x29,[0x36]=0x33,
    [0x37]=0x34,[0x38]=0x35,[0x39]=0x3A/*Caps*/,
    [0x3A]=0x3B,[0x3B]=0x3C,[0x3C]=0x3D,[0x3D]=0x3E,[0x3E]=0x3F,[0x3F]=0x40,
    [0x40]=0x41,[0x41]=0x42,[0x42]=0x43,[0x43]=0x44,[0x44]=0x57,[0x45]=0x58,
};

/* Deliver a USB HID key transition to the Win32 layer (UT99 etc.). Maps the
 * HID usage to a PS/2 set-1 scancode; extended keys (arrows / nav) are sent
 * with the 0xE0 prefix the way a real PS/2 controller would. No-op if the
 * win32 layer isn't present. */
static void hid_route_to_win32(uint8_t code, bool key_up)
{
    extern void win32_post_keyboard_event(uint8_t scancode, int key_up) __attribute__((weak));
    if (!win32_post_keyboard_event) return;

    uint8_t ext = 0;  /* extended PS/2 scancode (after 0xE0), 0 = none */
    switch (code) {
    case 0x4F: ext = 0x4D; break; /* Right  */
    case 0x50: ext = 0x4B; break; /* Left   */
    case 0x51: ext = 0x50; break; /* Down   */
    case 0x52: ext = 0x48; break; /* Up     */
    case 0x4A: ext = 0x47; break; /* Home   */
    case 0x4D: ext = 0x4F; break; /* End    */
    case 0x4B: ext = 0x49; break; /* PageUp */
    case 0x4E: ext = 0x51; break; /* PageDn */
    case 0x49: ext = 0x52; break; /* Insert */
    case 0x4C: ext = 0x53; break; /* Delete */
    default: break;
    }
    if (ext) {
        win32_post_keyboard_event(0xE0, key_up);
        win32_post_keyboard_event(ext, key_up);
        return;
    }
    if (code < 0x59 && hid_to_ps2_set1[code])
        win32_post_keyboard_event(hid_to_ps2_set1[code], key_up);
}

/*
 * Process a keyboard input report using descriptor-driven offsets.
 *
 * Pulls the modifier byte from caps->kbd_mods_field (variable, 8x1-bit,
 * usages 0xE0..0xE7) and the keycode array from caps->kbd_keys_field
 * (count×8-bit, each element a usage index on page 7). Generates
 * input_post_key events on press/release transitions vs dev->prev_*
 * and routes ASCII to the terminal via kbd_route_to_term.
 */
static void hid_process_keyboard(xhci_device_t *dev, const uint8_t *r)
{
    const hid_caps_t *caps = &dev->hid_caps;

    uint8_t mods = 0;
    if (caps->kbd_mods_field >= 0) {
        const hid_field_t *f = &caps->fields[caps->kbd_mods_field];
        /* Each modifier is 1 bit; pack 8 bits into a byte. */
        uint32_t total = (uint32_t)f->bit_size * (uint32_t)f->count;
        if (total > 8) total = 8;
        mods = (uint8_t)hid_extract(r, f->bit_offset, (uint8_t)total);
    }

    /* Keycode slots from current report. We scan up to 6 slots — the
     * boot-protocol minimum and the most common count. Devices that
     * report more usually still have the 6 most-recent in the first
     * 6 slots. */
    uint8_t keys[6] = {0};
    int n_keys = 0;
    if (caps->kbd_keys_field >= 0) {
        const hid_field_t *f = &caps->fields[caps->kbd_keys_field];
        uint16_t cnt = f->count;
        if (cnt > 6) cnt = 6;
        for (uint16_t i = 0; i < cnt; i++) {
            uint8_t code = (uint8_t)hid_extract(
                r, f->bit_offset + i * f->bit_size, f->bit_size);
            if (code > 1)  /* 0=none, 1=ErrorRollOver */
                keys[n_keys++] = code;
        }
    }

    bool shift = (mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
    bool ctrl  = (mods & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  != 0;

    extern void input_post_key(uint8_t scancode, bool pressed, bool extended);

    /* Modifier transition events. */
    uint8_t changed = mods ^ dev->prev_mods;
    if (changed) {
        for (int i = 0; i < 8; i++) {
            if (changed & (1 << i)) {
                bool pressed = (mods & (1 << i)) != 0;
                input_post_key(0xE0 + i, pressed, false);
                /* PS/2 set-1 scancodes for L/R Ctrl,Shift,Alt,Gui. */
                extern void win32_post_keyboard_event(uint8_t, int) __attribute__((weak));
                static const uint8_t mod_ps2[8]  = {0x1D,0x2A,0x38,0x5B,0x1D,0x36,0x38,0x5C};
                static const uint8_t mod_ext[8]  = {0,0,0,1,1,0,1,1};
                if (win32_post_keyboard_event) {
                    if (mod_ext[i]) win32_post_keyboard_event(0xE0, !pressed);
                    win32_post_keyboard_event(mod_ps2[i], !pressed);
                }
            }
        }
    }

    /* Releases: prev keys not in current report. */
    for (int i = 0; i < 6; i++) {
        uint8_t prev_code = dev->prev_keys[i];
        if (prev_code <= 1) continue;
        bool still = false;
        for (int j = 0; j < n_keys; j++)
            if (keys[j] == prev_code) { still = true; break; }
        if (!still) {
            input_post_key(prev_code, false, false);
            hid_route_to_win32(prev_code, true);
        }
    }

    /* Presses: current keys not in prev report. */
    for (int j = 0; j < n_keys; j++) {
        uint8_t code = keys[j];
        bool was_pressed = false;
        for (int i = 0; i < 6; i++)
            if (dev->prev_keys[i] == code) { was_pressed = true; break; }
        if (was_pressed) continue;

        input_post_key(code, true, false);
        hid_route_to_win32(code, false);
        kbd_route_to_term(code, shift, ctrl);
    }

    /* Snapshot for next report. */
    for (int i = 0; i < 6; i++)
        dev->prev_keys[i] = (i < n_keys) ? keys[i] : 0;
    dev->prev_mods = mods;
}

/*
 * Process a mouse input report using descriptor-driven offsets.
 * Buttons go through input_post_mouse_button; X/Y get posted as either
 * relative deltas (HID_INPUT_REL flag) or absolute coordinates.
 */
static void hid_process_mouse(xhci_device_t *dev, const uint8_t *r)
{
    const hid_caps_t *caps = &dev->hid_caps;
    extern void input_post_mouse_move(int16_t dx, int16_t dy);
    extern void input_post_mouse_button(uint8_t buttons);
    extern void input_set_mouse_abs(int32_t x, int32_t y);

    if (caps->mouse_btn_field >= 0) {
        const hid_field_t *f = &caps->fields[caps->mouse_btn_field];
        uint32_t total = (uint32_t)f->bit_size * (uint32_t)f->count;
        if (total > 8) total = 8;
        uint8_t btns = (uint8_t)hid_extract(r, f->bit_offset, (uint8_t)total) & 0x07;
        input_post_mouse_button(btns);
    }

    if (caps->mouse_x_field >= 0 && caps->mouse_y_field >= 0) {
        const hid_field_t *fx = &caps->fields[caps->mouse_x_field];
        const hid_field_t *fy = &caps->fields[caps->mouse_y_field];
        /* When X and Y live in the same hid_field_t (count >= 2,
         * QEMU usb-mouse style), fx == fy and both reads would
         * otherwise hit the same bits → only-diagonal cursor. Apply
         * the element-index offset stored at parse time so each axis
         * reads its own slice of the field. */
        uint16_t x_off = fx->bit_offset + (uint16_t)caps->mouse_x_elem * fx->bit_size;
        uint16_t y_off = fy->bit_offset + (uint16_t)caps->mouse_y_elem * fy->bit_size;
        if (fx->flags & HID_INPUT_REL) {
            int32_t dx = hid_extract_signed(r, x_off, fx->bit_size);
            int32_t dy = hid_extract_signed(r, y_off, fy->bit_size);
            if (dx != 0 || dy != 0)
                input_post_mouse_move((int16_t)dx, (int16_t)dy);
        } else {
            int32_t ax = hid_extract(r, x_off, fx->bit_size);
            int32_t ay = hid_extract(r, y_off, fy->bit_size);
            input_set_mouse_abs(ax, ay);
        }
    }
    (void)dev;
}

/*
 * Top-level HID report dispatcher. Strips Report ID if present and
 * routes the body through hid_process_{keyboard,mouse} based on the
 * device's parsed caps. Devices whose Report Descriptor failed to
 * parse, or that expose neither keyboard nor mouse, drop the report.
 */
static void usb_kbd_handle_report(xhci_device_t *dev, uint8_t *r, uint32_t len)
{
    if (len < 1) return;

    const hid_caps_t *caps = &dev->hid_caps;
    if (caps->n_fields == 0) return;  /* No descriptor parsed → drop. */

    /* When Report ID is in use, the first byte selects which report
     * layout this is. Our quick-lookup stores the ID associated with
     * the kbd/mouse fields; skip the byte before extracting. */
    uint8_t id = 0;
    if (caps->has_report_id) {
        id = r[0];
        r++;
        len--;
        if (len < 1) return;
    }

    if (caps->has_keyboard && (!caps->has_report_id || id == caps->kbd_report_id))
        hid_process_keyboard(dev, r);

    if (caps->has_mouse && (!caps->has_report_id || id == caps->mouse_report_id))
        hid_process_mouse(dev, r);
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
        /* Write Link TRB — controller follows it back to ring start */
        hc->cmd_ring[hc->cmd_enq].param = hc->cmd_ring_phys;
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
        uint64_t erdp = (hc->evt_ring_phys + hc->evt_deq * sizeof(xhci_trb_t)) | (1 << 3);
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

                    /* Single dispatcher — usb_kbd_handle_report uses the
                     * device's parsed Report Descriptor caps to demux the
                     * report into keyboard / mouse / both. The old size-
                     * based heuristic (>=8 bytes = keyboard, 3-6 = mouse)
                     * is gone — descriptor-driven layout is correct for
                     * all HID devices including non-boot keyboards. */
                    if (actual >= 1)
                        usb_kbd_handle_report(dev, r, actual);

                    /* Re-submit Normal TRB for next poll */
                    uint32_t ei = dev->int_enq;
                    dev->int_ring[ei].param = dev->report_buf_phys;
                    dev->int_ring[ei].status = dev->int_max_pkt;
                    wmb();
                    dev->int_ring[ei].control =
                        XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC |
                        (dev->int_cycle ? TRB_CYCLE : 0);
                    wmb();

                    dev->int_enq++;
                    if (dev->int_enq >= XHCI_XFER_RING_SIZE - 1) {
                        dev->int_ring[dev->int_enq].param = dev->int_ring_phys;
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

        uint64_t erdp = (hc->evt_ring_phys + hc->evt_deq * sizeof(xhci_trb_t)) | (1 << 3);
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

    /* Data TRB (if any). data is a kernel virt pointer — could be
     * either upper-half direct-map (Phase C migrations) or lower-half
     * identity (UEFI-loaded stacks). kvirt_to_phys handles both. */
    if (len > 0 && data) {
        dev->ep0_ring[ei].param = kvirt_to_phys(data);
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
        uint64_t erdp = (hc->evt_ring_phys + hc->evt_deq * sizeof(xhci_trb_t)) | (1 << 3);
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
    void *output_ctx_phys = mem_alloc_aligned(ctx_total, 4096);
    if (!output_ctx_phys) return;
    dev->output_ctx = PHYS_TO_VIRT(output_ctx_phys);
    dev->output_ctx_phys = (uint64_t)output_ctx_phys;
    memset(dev->output_ctx, 0, ctx_total);
    hc->dcbaa[slot_id] = dev->output_ctx_phys;
    wmb();

    /* ── Allocate EP0 Transfer Ring ── */
    uint32_t ring_bytes = XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t);
    void *ep0_ring_phys = mem_alloc_aligned(ring_bytes, 4096);
    if (!ep0_ring_phys) return;
    dev->ep0_ring = (xhci_trb_t *)PHYS_TO_VIRT(ep0_ring_phys);
    dev->ep0_ring_phys = (uint64_t)ep0_ring_phys;
    memset(dev->ep0_ring, 0, ring_bytes);
    dev->ep0_enq = 0;
    dev->ep0_cycle = 1;

    /* ── Build Input Context for Address Device ── */
    uint32_t in_ctx_total = hc->ctx_size * 33; /* input ctrl ctx + 32 entries */
    void *in_ctx_phys = mem_alloc_aligned(in_ctx_total, 4096);
    if (!in_ctx_phys) return;
    void *in_ctx = PHYS_TO_VIRT(in_ctx_phys);
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
    ep0_ctx->tr_dequeue = dev->ep0_ring_phys | 1; /* DCS = 1 (controller reads phys) */
    ep0_ctx->field4 = 8; /* Average TRB length (8 for control) */

    /* ── Address Device (BSR=0) ── */
    memset(&cmd, 0, sizeof(cmd));
    cmd.param = (uint64_t)in_ctx_phys;
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
    void *desc_buf_phys = mem_alloc_aligned(256, 64);
    if (!desc_buf_phys) return;
    uint8_t *desc_buf = (uint8_t *)PHYS_TO_VIRT(desc_buf_phys);
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
        void *eval_ctx_phys = mem_alloc_aligned(eval_ctx_total, 4096);
        if (eval_ctx_phys) {
            void *eval_ctx = PHYS_TO_VIRT(eval_ctx_phys);
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
            eval_ep0->tr_dequeue = dev->ep0_ring_phys |
                                   (dev->ep0_cycle ? 1 : 0);
            /* Re-read dequeue from output context if available */
            xhci_ep_ctx_t *out_ep0 =
                (xhci_ep_ctx_t *)ctx_entry(hc, dev->output_ctx, 1);
            eval_ep0->tr_dequeue = out_ep0->tr_dequeue;
            eval_ep0->field4 = 8;

            xhci_trb_t eval_cmd;
            memset(&eval_cmd, 0, sizeof(eval_cmd));
            eval_cmd.param = (uint64_t)eval_ctx_phys;
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
    /* Walk the configuration descriptor. We can land on either:
     *   - HID class (3): collect Interrupt IN endpoint + HID Report
     *     Descriptor length (from the 0x21 class descriptor).
     *   - Mass Storage class (8): collect Bulk IN + Bulk OUT endpoints
     *     for SCSI BBB (subclass 6, protocol 0x50).
     * The first matching interface wins (one functional unit per device
     * slot — composite devices that mix functions still get only one
     * for now). */
    enum { IFACE_NONE, IFACE_HID, IFACE_MSC };
    int      iface_kind         = IFACE_NONE;

    uint8_t  hid_iface          = 0xFF;
    uint8_t  hid_proto          = 0;
    uint8_t  hid_subclass       = 0;
    uint8_t  int_ep_addr        = 0;
    uint16_t int_max_pkt_found  = 0;
    uint8_t  int_interval       = 0;
    uint16_t report_desc_len    = 0;

    uint8_t  msc_iface          = 0xFF;
    uint8_t  bulk_in_addr       = 0;
    uint16_t bulk_in_max_pkt    = 0;
    uint8_t  bulk_out_addr      = 0;
    uint16_t bulk_out_max_pkt   = 0;
    /* SuperSpeed Endpoint Companion (desc type 0x30) carries the burst
     * size which the EP context's field2 needs in bits 8..15. Without
     * this, SS bulk EPs STALL on the first transfer. */
    uint8_t  bulk_in_max_burst  = 0;
    uint8_t  bulk_out_max_burst = 0;

    /* The SS-EP-Companion follows the EP descriptor it belongs to; we
     * track which EP we last saw so we can attribute the next companion. */
    enum { LAST_NONE, LAST_BULK_IN, LAST_BULK_OUT } last_ep = LAST_NONE;

    bool     collecting_eps     = false;

    uint32_t pos = 0;
    while (pos + 2 <= total_len) {
        uint8_t dlen = desc_buf[pos];
        uint8_t dtype = desc_buf[pos + 1];
        if (dlen < 2) break;

        if (dtype == USB_DESC_INTERFACE && pos + 9 <= total_len) {
            uint8_t iclass = desc_buf[pos + 5];
            uint8_t isub   = desc_buf[pos + 6];
            uint8_t iproto = desc_buf[pos + 7];

            /* New interface — stop collecting endpoints from a prior
             * interface that wasn't selected. */
            collecting_eps = false;

            if (iclass == 3 && iface_kind == IFACE_NONE) {  /* HID */
                uint8_t this_iface = desc_buf[pos + 2];
                serial_puts("[xHCI] HID interface ");
                serial_putdec(this_iface);
                serial_puts(" (subclass=");
                serial_putdec(isub);
                serial_puts(" proto=");
                serial_putdec(iproto);
                serial_puts(")\n");
                iface_kind     = IFACE_HID;
                hid_iface      = this_iface;
                hid_proto      = iproto;
                hid_subclass   = isub;
                collecting_eps = true;
            } else if (iclass == 0x08 && iface_kind == IFACE_NONE) {
                /* USB Mass Storage. Accept any subclass/protocol so we
                 * can at least log what kind it is. Common combos:
                 *   subclass=0x06 (SCSI), proto=0x50 (BBB)  ← supported
                 *   subclass=0x06 (SCSI), proto=0x62 (UAS)  ← not yet
                 *   subclass=0x05 (SFF-8070i), proto=0x50   ← supported
                 *   subclass=0x04 (UFI),       proto=0x50   ← supported
                 * Only iproto=0x50 (Bulk-Only Transport) really works
                 * with our bulk_xfer code; UAS needs Bulk Streams support
                 * which we don't have yet. */
                uint8_t this_iface = desc_buf[pos + 2];
                serial_puts("[xHCI] MSC interface ");
                serial_putdec(this_iface);
                serial_puts(" sub=");
                serial_putdec(isub);
                serial_puts(" proto=");
                serial_putdec(iproto);
                serial_puts("\n");
                if (iproto == 0x50) {
                    iface_kind     = IFACE_MSC;
                    msc_iface      = this_iface;
                    collecting_eps = true;
                } else {
                    extern void fb_puts(const char *s);
                    fb_puts(" [xHCI] MSC proto unsupported (need BBB=0x50)\n");
                }
            }
        }

        /* HID Class Descriptor (0x21): tells us the Report Descriptor
         * size we'll fetch via GET_DESCRIPTOR(0x22). */
        if (dtype == 0x21 && iface_kind == IFACE_HID && collecting_eps &&
            pos + 9 <= total_len) {
            uint8_t  num = desc_buf[pos + 5];
            for (uint8_t k = 0; k < num && pos + 6 + 3 * k + 3 <= total_len; k++) {
                uint8_t  type = desc_buf[pos + 6 + 3 * k];
                uint16_t blen = (uint16_t)(desc_buf[pos + 6 + 3 * k + 1] |
                                           (desc_buf[pos + 6 + 3 * k + 2] << 8));
                if (type == 0x22) {
                    report_desc_len = blen;
                    serial_puts("[xHCI]  -> Report Descriptor: ");
                    serial_putdec(blen);
                    serial_puts(" bytes\n");
                    break;
                }
            }
        }

        if (dtype == USB_DESC_ENDPOINT && pos + 7 <= total_len && collecting_eps) {
            uint8_t  ep_addr = desc_buf[pos + 2];
            uint8_t  ep_attr = desc_buf[pos + 3];
            uint16_t ep_mps  = (uint16_t)(desc_buf[pos + 4] | (desc_buf[pos + 5] << 8));
            uint8_t  ep_int  = desc_buf[pos + 6];
            uint8_t  ep_type = ep_attr & 0x03;
            bool     is_in   = (ep_addr & 0x80) != 0;

            if (iface_kind == IFACE_HID && ep_type == 0x03 && is_in) {
                /* Interrupt IN endpoint */
                int_ep_addr       = ep_addr;
                int_max_pkt_found = ep_mps & 0x7FF;
                int_interval      = ep_int;
                serial_puts("[xHCI]  -> Int-IN EP ");
                serial_puthex(ep_addr, 2);
                serial_puts(" maxpkt=");
                serial_putdec(int_max_pkt_found);
                serial_puts("\n");
            } else if (iface_kind == IFACE_MSC && ep_type == 0x02) {
                /* Bulk endpoint — direction tells IN vs OUT. */
                if (is_in) {
                    bulk_in_addr    = ep_addr;
                    bulk_in_max_pkt = ep_mps & 0x7FF;
                    last_ep         = LAST_BULK_IN;
                    serial_puts("[xHCI]  -> Bulk-IN EP ");
                } else {
                    bulk_out_addr    = ep_addr;
                    bulk_out_max_pkt = ep_mps & 0x7FF;
                    last_ep          = LAST_BULK_OUT;
                    serial_puts("[xHCI]  -> Bulk-OUT EP ");
                }
                serial_puthex(ep_addr, 2);
                serial_puts(" maxpkt=");
                serial_putdec(ep_mps & 0x7FF);
                serial_puts("\n");
            } else {
                last_ep = LAST_NONE;
            }
        }

        /* SuperSpeed Endpoint Companion descriptor (type 0x30) — sits
         * immediately after the EP descriptor in SS configurations.
         * Layout: bLength=6, bDescType=0x30, bMaxBurst, bmAttributes,
         *         wBytesPerInterval (2 bytes). */
        if (dtype == 0x30 && collecting_eps && pos + 6 <= total_len &&
            iface_kind == IFACE_MSC) {
            uint8_t mb = desc_buf[pos + 2];  /* bMaxBurst (0..15) */
            if (last_ep == LAST_BULK_IN)  bulk_in_max_burst  = mb;
            if (last_ep == LAST_BULK_OUT) bulk_out_max_burst = mb;
            serial_puts("[xHCI]  -> SS-EP-Companion bMaxBurst=");
            serial_putdec(mb);
            serial_puts("\n");
        }

        pos += dlen;
    }

    if (iface_kind == IFACE_NONE) {
        serial_puts("[xHCI] No supported interface (HID/MSC) found\n");
        return;
    }
    if (iface_kind == IFACE_HID && int_ep_addr == 0) {
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

  if (iface_kind == IFACE_HID) {
    /* SET_PROTOCOL and SET_IDLE are mandatory only for HID Boot
     * Interface Subclass (bInterfaceSubClass == 1) — i.e. boot
     * keyboards and boot mice. Generic HID devices (subclass 0,
     * which QEMU's usb-mouse and most non-boot pointers use) STALL
     * these class-specific requests. A stalled EP0 then refuses
     * every subsequent control transfer until CLEAR_FEATURE(EP_HALT),
     * so issuing them unconditionally killed our GET_DESCRIPTOR(Report)
     * for the mouse and left it HID-unparsed → reports dropped.
     *
     * Skip the boot-only requests for non-boot subclasses; the
     * device's default protocol (Report) is what we already expect
     * to parse anyway. */
    if (hid_subclass == 1) {
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
    } else {
        serial_puts("[xHCI] non-boot HID subclass — skip SET_PROTOCOL/SET_IDLE\n");
    }

    /* ── GET_DESCRIPTOR(Report) — fetch the HID Report Descriptor and
     * parse it into dev->hid_caps. The parsed capability map drives the
     * runtime report demux: which bits hold the modifier byte, where
     * the keycode array begins, mouse buttons, X/Y, etc.
     *
     * If report_desc_len is 0 (descriptor missing) or the device NAKs
     * the request, we leave hid_caps zero — the report handler will
     * silently drop reports for that device. */
    dev->report_desc_len = 0;
    if (report_desc_len > 0 && report_desc_len <= 256) {
        memset(desc_buf, 0, 256);
        setup.bmRequestType = 0x81;  /* Std, Interface, Device-to-Host */
        setup.bRequest      = USB_REQ_GET_DESCRIPTOR;
        setup.wValue        = (0x22 << 8);  /* Report Descriptor type, index 0 */
        setup.wIndex        = hid_iface;
        setup.wLength       = report_desc_len;
        if (ctrl_transfer(hc, dev, &setup, desc_buf, report_desc_len, true) >= 0) {
            if (hid_parse(desc_buf, report_desc_len, &dev->hid_caps) == 0) {
                dev->report_desc_len = report_desc_len;
                serial_puts("[xHCI] Report Desc parsed: ");
                serial_putdec(dev->hid_caps.n_fields);
                serial_puts(" field(s)");
                if (dev->hid_caps.has_keyboard) serial_puts(" KBD");
                if (dev->hid_caps.has_mouse)    serial_puts(" MOUSE");
                if (dev->hid_caps.has_report_id) serial_puts(" +ID");
                serial_puts("\n");
            } else {
                serial_puts("[xHCI] Report Desc parse FAILED\n");
            }
        } else {
            serial_puts("[xHCI] GET_DESCRIPTOR(Report) failed\n");
        }
    }

    /* ── Configure Interrupt IN Endpoint ── */
    uint8_t ep_num = int_ep_addr & 0x0F;
    uint8_t ep_dci = ep_num * 2 + 1; /* IN endpoint DCI */

    /* Allocate interrupt transfer ring */
    void *int_ring_phys = mem_alloc_aligned(
        XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t), 4096);
    if (!int_ring_phys) return;
    dev->int_ring = (xhci_trb_t *)PHYS_TO_VIRT(int_ring_phys);
    dev->int_ring_phys = (uint64_t)int_ring_phys;
    memset(dev->int_ring, 0, XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t));
    dev->int_enq = 0;
    dev->int_cycle = 1;
    dev->int_ep_dci = ep_dci;
    dev->int_max_pkt = int_max_pkt_found;

    /* Allocate report buffer */
    void *report_buf_phys = mem_alloc_aligned(int_max_pkt_found, 64);
    if (!report_buf_phys) return;
    dev->report_buf = (uint8_t *)PHYS_TO_VIRT(report_buf_phys);
    dev->report_buf_phys = (uint64_t)report_buf_phys;
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
    int_ep->tr_dequeue = dev->int_ring_phys | 1; /* DCS = 1 */
    int_ep->field4 = int_max_pkt_found; /* Average TRB length */

    /* Submit Configure Endpoint command */
    memset(&cmd, 0, sizeof(cmd));
    cmd.param = (uint64_t)in_ctx_phys;
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
        dev->int_ring[ei2].param = dev->report_buf_phys;
        dev->int_ring[ei2].status = int_max_pkt_found;
        dev->int_ring[ei2].control =
            XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC |
            (dev->int_cycle ? TRB_CYCLE : 0);
        wmb();

        dev->int_enq++;
        if (dev->int_enq >= XHCI_XFER_RING_SIZE - 1) {
            dev->int_ring[dev->int_enq].param = dev->int_ring_phys;
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

    /* Label based on what the parsed Report Descriptor actually says,
     * not the bInterfaceProtocol hint. A device with proto=0 can still
     * be a keyboard (Report Protocol only); only the descriptor knows. */
    const char *kind;
    if (dev->hid_caps.has_keyboard && dev->hid_caps.has_mouse) kind = "keyboard+mouse";
    else if (dev->hid_caps.has_keyboard) kind = "keyboard";
    else if (dev->hid_caps.has_mouse)    kind = "mouse";
    else if (dev->report_desc_len > 0)   kind = "HID-other";
    else                                 kind = "HID-unparsed";

    serial_puts("[xHCI] ");
    serial_puts(kind);
    serial_puts(" active on slot ");
    serial_putdec(slot_id);
    serial_puts(", EP ");
    serial_puthex(int_ep_addr, 2);
    serial_puts("\n");

    /* Mirror enumeration result to framebuffer for bare-metal diagnosis. */
    {
        extern void fb_puts(const char *s);
        extern void fb_putdec(uint64_t v);
        fb_puts(" [xHCI] ");
        fb_puts(kind);
        fb_puts(" on slot ");
        fb_putdec(slot_id);
        fb_puts("\n");
    }
  } else if (iface_kind == IFACE_MSC) {
    /* ── USB Mass Storage path ──
     * SET_CONFIGURATION already issued above. We now allocate two Bulk
     * transfer rings, configure both endpoints, and hand the slot off
     * to drivers/usb_storage.c (BBB protocol, SCSI commands). */
    if (bulk_in_addr == 0 || bulk_out_addr == 0) {
        serial_puts("[xHCI] MSC needs both Bulk-IN and Bulk-OUT EPs\n");
        return;
    }

    uint8_t in_dci  = (bulk_in_addr  & 0x0F) * 2 + 1; /* IN  → odd */
    uint8_t out_dci = (bulk_out_addr & 0x0F) * 2;     /* OUT → even */
    uint8_t max_dci = (in_dci > out_dci) ? in_dci : out_dci;

    /* Allocate the two transfer rings. */
    uint32_t ring_bytes = XHCI_XFER_RING_SIZE * sizeof(xhci_trb_t);
    void *bin_phys  = mem_alloc_aligned(ring_bytes, 4096);
    void *bout_phys = mem_alloc_aligned(ring_bytes, 4096);
    if (!bin_phys || !bout_phys) {
        serial_puts("[xHCI] MSC ring alloc failed\n");
        return;
    }
    dev->bulk_in_ring       = (xhci_trb_t *)PHYS_TO_VIRT(bin_phys);
    dev->bulk_in_ring_phys  = (uint64_t)bin_phys;
    dev->bulk_out_ring      = (xhci_trb_t *)PHYS_TO_VIRT(bout_phys);
    dev->bulk_out_ring_phys = (uint64_t)bout_phys;
    memset(dev->bulk_in_ring,  0, ring_bytes);
    memset(dev->bulk_out_ring, 0, ring_bytes);
    dev->bulk_in_enq    = 0;  dev->bulk_in_cycle  = 1;
    dev->bulk_out_enq   = 0;  dev->bulk_out_cycle = 1;
    dev->bulk_in_dci    = in_dci;
    dev->bulk_out_dci   = out_dci;
    dev->bulk_in_max_pkt  = bulk_in_max_pkt;
    dev->bulk_out_max_pkt = bulk_out_max_pkt;

    /* Configure Endpoint with both Bulk EPs added. */
    memset(in_ctx, 0, in_ctx_total);
    icc = (xhci_input_ctrl_ctx_t *)ctx_entry(hc, in_ctx, 0);
    icc->add_flags = (1u << 0) | (1u << in_dci) | (1u << out_dci);

    slot_ctx = (xhci_slot_ctx_t *)ctx_entry(hc, in_ctx, 1);
    xhci_slot_ctx_t *out_slot = (xhci_slot_ctx_t *)ctx_entry(hc, dev->output_ctx, 0);
    *slot_ctx = *out_slot;
    slot_ctx->field1 &= ~(0x1F << 27);
    slot_ctx->field1 |= ((uint32_t)max_dci << 27);

    /* Bulk IN endpoint context. For SuperSpeed bulk EPs we MUST set
     * Max Burst Size (field2 bits 8..15) from the SS-EP-Companion
     * descriptor's bMaxBurst — without it the EP STALLs on the first
     * transfer. HighSpeed and below use 0 for max_burst, which is also
     * the safe default if no companion was seen. */
    xhci_ep_ctx_t *bin_ep = (xhci_ep_ctx_t *)ctx_entry(hc, in_ctx, in_dci + 1);
    bin_ep->field1     = 0;
    bin_ep->field2     = (3 << 1) | (EP_TYPE_BULK_IN << 3) |
                         ((uint32_t)bulk_in_max_burst << 8) |
                         ((uint32_t)bulk_in_max_pkt   << 16);
    bin_ep->tr_dequeue = dev->bulk_in_ring_phys | 1; /* DCS=1 */
    bin_ep->field4     = bulk_in_max_pkt * (bulk_in_max_burst + 1);

    /* Bulk OUT endpoint context. */
    xhci_ep_ctx_t *bout_ep = (xhci_ep_ctx_t *)ctx_entry(hc, in_ctx, out_dci + 1);
    bout_ep->field1    = 0;
    bout_ep->field2    = (3 << 1) | (EP_TYPE_BULK_OUT << 3) |
                         ((uint32_t)bulk_out_max_burst << 8) |
                         ((uint32_t)bulk_out_max_pkt   << 16);
    bout_ep->tr_dequeue = dev->bulk_out_ring_phys | 1;
    bout_ep->field4    = bulk_out_max_pkt * (bulk_out_max_burst + 1);

    memset(&cmd, 0, sizeof(cmd));
    cmd.param   = (uint64_t)in_ctx_phys;
    cmd.control = XHCI_TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24);
    cmd_submit(hc, &cmd);
    code = cmd_wait(hc, &dummy);
    if (code != 1) {
        serial_puts("[xHCI] MSC Configure Endpoint failed code=");
        serial_putdec(code < 0 ? 0 : (uint64_t)code);
        serial_puts("\n");
        return;
    }

    dev->msc_active = true;
    hc->num_devices++;

    serial_puts("[xHCI] MSC active on slot ");
    serial_putdec(slot_id);
    serial_puts(" (in=");
    serial_puthex(bulk_in_addr, 2);
    serial_puts(" out=");
    serial_puthex(bulk_out_addr, 2);
    serial_puts(")\n");

    {
        extern void fb_puts(const char *s);
        extern void fb_putdec(uint64_t v);
        fb_puts(" [xHCI] mass-storage on slot ");
        fb_putdec(slot_id);
        fb_puts("\n");
    }

    /* Settling delay — devices need time after Configure Endpoint
     * before bulk transfers work. ~50ms at 3.8 GHz. */
    spin(2000000);

    /* Get Max LUN — required by some sticks before they accept CBW.
     * Result is ignored (single-LUN if NAK). */
    {
        uint8_t max_lun = 0;
        setup.bmRequestType = 0xA1;
        setup.bRequest      = 0xFE;
        setup.wValue        = 0;
        setup.wIndex        = msc_iface;
        setup.wLength       = 1;
        ctrl_transfer(hc, dev, &setup, &max_lun, 1, true);
    }

    /* Bulk-Only Mass Storage Reset — failure tolerated. */
    setup.bmRequestType = 0x21;
    setup.bRequest      = 0xFF;
    setup.wValue        = 0;
    setup.wIndex        = msc_iface;
    setup.wLength       = 0;
    ctrl_transfer(hc, dev, &setup, NULL, 0, false);

    /* Hand off to the SCSI BBB driver. dev_idx = slot_id - 1. */
    extern int usb_storage_init(int xhci_dev_idx);
    int us_ret = usb_storage_init(slot_id - 1);
    if (us_ret < 0) {
        extern void fb_puts(const char *s);
        fb_puts(" [USB-STOR] init failed\n");
    }
    if (us_ret == 0) {
        extern uint32_t usb_storage_block_size(void);
        extern uint32_t usb_storage_block_count(void);
        extern int usb_storage_read(uint64_t lba, uint32_t count, void *buf);
        extern int usb_storage_write(uint64_t lba, uint32_t count, const void *buf);
        extern int blkdev_register(const char *name, uint8_t type,
                                   uint32_t sector_size, uint64_t sector_count,
                                   int (*read)(uint64_t, uint32_t, void *),
                                   int (*write)(uint64_t, uint32_t, const void *));
        int idx = blkdev_register("usb0", 3 /* BLKDEV_USB */,
                                   usb_storage_block_size(),
                                   usb_storage_block_count(),
                                   usb_storage_read, usb_storage_write);
        extern void fb_puts(const char *s);
        extern void fb_putdec(uint64_t v);
        if (idx >= 0) {
            fb_puts(" [BLKDEV] usb0 registered: ");
            fb_putdec((uint64_t)usb_storage_block_count() *
                      usb_storage_block_size() / (1024 * 1024));
            fb_puts(" MB, ssz=");
            fb_putdec(usb_storage_block_size());
            fb_puts("\n");
        } else {
            fb_puts(" [BLKDEV] usb0 register FAILED\n");
        }
    }
  }
}

/* ── Init one xHCI controller ────────────────────────────────── */

int xhci_init(uint64_t bar0_phys, uint8_t bus, uint8_t dev, uint8_t func)
{
    if (hc_count >= XHCI_MAX_CONTROLLERS) return -1;

    xhci_hc_t *hc = &hc_list[hc_count];
    memset(hc, 0, sizeof(*hc));
    /* MMIO via the upper-half alias (PML4[256] is shared across all
     * process CR3s once the lower-half identity map is dropped). */
    hc->base = (volatile void *)PHYS_TO_VIRT(bar0_phys);
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
    void *dcbaa_phys = mem_alloc_aligned(dcbaa_size, 4096);
    if (!dcbaa_phys) return -1;
    hc->dcbaa = (uint64_t *)PHYS_TO_VIRT(dcbaa_phys);
    hc->dcbaa_phys = (uint64_t)dcbaa_phys;
    memset(hc->dcbaa, 0, dcbaa_size);

    /* ── Scratchpad Buffers ── */
    uint32_t scratch_hi = XHCI_HCS2_SCRATCH_HI(hcs2);
    uint32_t scratch_lo = XHCI_HCS2_SCRATCH_LO(hcs2);
    uint32_t scratch_count = (scratch_hi << 5) | scratch_lo;

    if (scratch_count > 0) {
        serial_puts("[xHCI] Allocating ");
        serial_putdec(scratch_count);
        serial_puts(" scratchpad buffers\n");

        void *scratchpad_phys = mem_alloc_aligned(scratch_count * 8, 4096);
        if (!scratchpad_phys) return -1;
        hc->scratchpad = (uint64_t *)PHYS_TO_VIRT(scratchpad_phys);

        for (uint32_t i = 0; i < scratch_count; i++) {
            void *page_phys = mem_alloc_aligned(4096, 4096);
            if (!page_phys) return -1;
            memset(PHYS_TO_VIRT(page_phys), 0, 4096);
            hc->scratchpad[i] = (uint64_t)page_phys;
        }
        hc->dcbaa[0] = (uint64_t)scratchpad_phys;
    }

    op_write64(hc, XHCI_OP_DCBAAP, (uint64_t)dcbaa_phys);

    /* ── Command Ring ── */
    uint32_t cmd_bytes = XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t);
    void *cmd_ring_phys = mem_alloc_aligned(cmd_bytes, 4096);
    if (!cmd_ring_phys) return -1;
    hc->cmd_ring = (xhci_trb_t *)PHYS_TO_VIRT(cmd_ring_phys);
    hc->cmd_ring_phys = (uint64_t)cmd_ring_phys;
    memset(hc->cmd_ring, 0, cmd_bytes);
    hc->cmd_enq = 0;
    hc->cmd_cycle = 1;

    op_write64(hc, XHCI_OP_CRCR, hc->cmd_ring_phys | 1); /* RCS=1 matches cmd_cycle=1 */

    /* ── Event Ring ── */
    uint32_t evt_bytes = XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t);
    void *evt_ring_phys = mem_alloc_aligned(evt_bytes, 4096);
    if (!evt_ring_phys) return -1;
    hc->evt_ring = (xhci_trb_t *)PHYS_TO_VIRT(evt_ring_phys);
    hc->evt_ring_phys = (uint64_t)evt_ring_phys;
    memset(hc->evt_ring, 0, evt_bytes);
    hc->evt_deq = 0;
    hc->evt_cycle = 1;

    void *erst_phys = mem_alloc_aligned(sizeof(xhci_erste_t), 64);
    if (!erst_phys) return -1;
    hc->erst = (xhci_erste_t *)PHYS_TO_VIRT(erst_phys);
    hc->erst->ring_base = (uint64_t)evt_ring_phys;
    hc->erst->ring_size = XHCI_EVT_RING_SIZE;
    hc->erst->reserved = 0;

    /* Configure interrupter 0 */
    uint32_t ir0 = XHCI_RT_IR0;
    rt_write(hc, ir0 + XHCI_IR_ERSTSZ, 1);
    rt_write64(hc, ir0 + XHCI_IR_ERDP, (uint64_t)evt_ring_phys | (1 << 3));
    rt_write64(hc, ir0 + XHCI_IR_ERSTBA, (uint64_t)erst_phys);

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
        uint64_t erdp = (hc->evt_ring_phys + hc->evt_deq * sizeof(xhci_trb_t)) | (1 << 3);
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

    /* Mirror to framebuffer for bare-metal diagnosis (no serial cable). */
    {
        extern void fb_puts(const char *s);
        extern void fb_putdec(uint64_t v);
        fb_puts("\n [xHCI] ");
        fb_putdec(ccs_count);
        fb_puts("/");
        fb_putdec(hc->max_ports);
        fb_puts(" ports connected\n");
    }

    /* ── Enumerate connected ports ── */
    for (uint32_t p = 0; p < hc->max_ports; p++)
        enumerate_port(hc, (int)p);

    serial_puts("[xHCI] Init complete: ");
    serial_putdec(hc->num_devices);
    serial_puts(" device(s)\n");

    /* Mirror to framebuffer. */
    {
        extern void fb_puts(const char *s);
        extern void fb_putdec(uint64_t v);
        fb_puts(" [xHCI] init done: ");
        fb_putdec(hc->num_devices);
        fb_puts(" HID device(s)\n");
    }

    /* xHCI summary goes to serial; main.c shows compact HW info */

    return 0;
}

/* ── Bulk transfers (USB Mass Storage BBB) ─────────────────────
 *
 * xhci_bulk_in / xhci_bulk_out are the API surface used by the upper
 * MSC driver (drivers/usb_storage.c) to talk to a SCSI-over-USB device.
 * dev_idx is the host-controller-relative slot index (slot_id - 1).
 *
 * Implementation: a single Normal TRB is queued on the appropriate Bulk
 * transfer ring with TRB_IOC, the endpoint doorbell is rung, and we
 * synchronously poll the event ring for a TRANSFER_EVENT addressed to
 * this slot+DCI. On completion we return 0 (success) or -1 (HC error,
 * timeout, or short-packet on a write). Short-packet on a read is
 * counted as success but `*actual_in` is updated so the caller can
 * tell.
 *
 * The synchronous-wait pattern here mirrors ctrl_transfer; long term we
 * may want async + completion callbacks but synchronous is correct and
 * matches the BBB protocol's CBW→Data→CSW lockstep semantics anyway.
 */

static int bulk_xfer(xhci_hc_t *hc, xhci_device_t *dev,
                     xhci_trb_t *ring, uint64_t ring_phys,
                     uint32_t *enq_io, uint8_t *cycle_io,
                     uint8_t ep_dci,
                     void *buf, uint32_t len, bool dir_in,
                     uint32_t *actual_out)
{
    if (!hc || !dev || !ring || ep_dci == 0) return -1;
    if (len == 0) { if (actual_out) *actual_out = 0; return 0; }

    uint32_t ei = *enq_io;
    uint8_t  cyc = *cycle_io;
    (void)dir_in; /* DCI encodes direction; param/IOC are the same. */

    /* Build a single Normal TRB.  buf is a kernel virt pointer in the
     * upper-half direct-map alias; the controller needs the phys. */
    /* buf may be either upper-half (Phase C migrated) or lower-half
     * identity (UEFI-loaded stack). Use kvirt_to_phys for safety —
     * a plain VIRT_TO_PHYS underflow on a lower-half pointer hands
     * the controller a garbage phys, the device receives junk in the
     * CBW, and STALLs the next IN. */
    ring[ei].param   = (uint64_t)kvirt_to_phys(buf);
    ring[ei].status  = len;
    wmb();
    ring[ei].control = XHCI_TRB_TYPE(TRB_NORMAL) | TRB_IOC |
                       (cyc ? TRB_CYCLE : 0);
    wmb();

    ei++;
    if (ei >= XHCI_XFER_RING_SIZE - 1) {
        /* Link TRB back to head — flip cycle when we cross. */
        ring[ei].param   = ring_phys;
        ring[ei].status  = 0;
        ring[ei].control = XHCI_TRB_TYPE(TRB_LINK) | TRB_TC |
                           (cyc ? TRB_CYCLE : 0);
        wmb();
        ei = 0;
        cyc ^= 1;
    }
    *enq_io   = ei;
    *cycle_io = cyc;

    /* Ring the endpoint doorbell. */
    db_write(hc, dev->slot_id, ep_dci);

    /* Wait for the matching Transfer Event. The same iteration limit as
     * ctrl_transfer (~340ms at 3.8 GHz) — bulk reads of 64 sectors are
     * safely under that. */
    for (int t = 0; t < 20000000; t++) {
        uint32_t eidx = hc->evt_deq;
        xhci_trb_t *ev = &hc->evt_ring[eidx];
        rmb();
        if ((ev->control & TRB_CYCLE) != hc->evt_cycle) {
            __asm__ volatile ("pause");
            continue;
        }

        uint8_t  etype = XHCI_TRB_GET_TYPE(ev->control);
        uint8_t  ecode = (ev->status >> 24) & 0xFF;
        uint8_t  eslot = (ev->control >> 24) & 0xFF;
        uint8_t  edci  = (ev->control >> 16) & 0x1F;
        uint32_t eres  = ev->status & 0xFFFFFF;  /* TRB Transfer Length residual. */

        /* Always advance and acknowledge — we mustn't get stuck on
         * an unrelated event. */
        hc->evt_deq++;
        if (hc->evt_deq >= XHCI_EVT_RING_SIZE) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }
        uint64_t erdp = (hc->evt_ring_phys + hc->evt_deq * sizeof(xhci_trb_t)) | (1 << 3);
        rt_write64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);

        if (etype != TRB_TRANSFER_EVENT) continue;
        if (eslot != dev->slot_id) continue;
        if (edci  != ep_dci) continue;

        if (actual_out) *actual_out = (eres > len) ? 0 : (len - eres);
        /* 1 = Success, 13 = Short Packet (still useful for IN). */
        return (ecode == 1 || ecode == 13) ? 0 : -1;
    }

    serial_puts("[xHCI] Bulk transfer timeout\n");
    return -1;
}

int xhci_bulk_out(int dev_idx, const void *data, uint32_t len)
{
    /* Locate the device — dev_idx is global across controllers; we
     * walk hc_list and use the first non-zero slot match. Real systems
     * typically have one xHCI; if you need multi-HC, add an HC index. */
    for (int h = 0; h < hc_count; h++) {
        xhci_hc_t *hc = &hc_list[h];
        if (dev_idx < 0 || dev_idx >= XHCI_MAX_SLOTS) continue;
        xhci_device_t *dev = &hc->devices[dev_idx];
        if (!dev->msc_active) continue;
        return bulk_xfer(hc, dev,
                         dev->bulk_out_ring, dev->bulk_out_ring_phys,
                         &dev->bulk_out_enq, &dev->bulk_out_cycle,
                         dev->bulk_out_dci,
                         (void *)(uintptr_t)data, len, false, NULL);
    }
    return -1;
}

int xhci_bulk_in(int dev_idx, void *data, uint32_t len, uint32_t *actual)
{
    for (int h = 0; h < hc_count; h++) {
        xhci_hc_t *hc = &hc_list[h];
        if (dev_idx < 0 || dev_idx >= XHCI_MAX_SLOTS) continue;
        xhci_device_t *dev = &hc->devices[dev_idx];
        if (!dev->msc_active) continue;
        return bulk_xfer(hc, dev,
                         dev->bulk_in_ring, dev->bulk_in_ring_phys,
                         &dev->bulk_in_enq, &dev->bulk_in_cycle,
                         dev->bulk_in_dci,
                         data, len, true, actual);
    }
    return -1;
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
