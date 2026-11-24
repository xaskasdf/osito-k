/* Exercise the production poller with coherent rings and recorded MMIO. */
#include "../include/types.h"
static uint32_t test_read32(volatile void *addr);
static void test_write32(volatile void *addr, uint32_t value);
static void test_write64(volatile void *addr, uint64_t value);
#define mmio_read32 test_read32
#define mmio_write32 test_write32
#define mmio_write64 test_write64
#define XHCI_HOST_TEST 1
#include "../drivers/xhci.c"

extern int printf(const char *, ...);
extern int puts(const char *);

typedef struct { uint32_t off, width; uint64_t value; } access_t;
static access_t accesses[512];
static unsigned n_accesses, failures, keys, last_key;
static uint64_t clock_ns;
static uint32_t status;
static xhci_trb_t events[XHCI_EVT_RING_SIZE];
static xhci_trb_t transfers[XHCI_XFER_RING_SIZE];
static uint8_t report[8];
static void (*write_hook)(uint32_t off);
static void (*key_hook)(void);
static uint64_t mapped_base, mapped_pages[64];
static unsigned mapped_count, translations;
static uint64_t control_physical;

#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); failures++; \
} } while (0)

uint64_t idt_get_monotonic_ns(void) { return clock_ns; }
uint64_t xhci_test_cr3(void) { return 0x12345000; }
static uint64_t mapped_phys(uint64_t va)
{
    if (!mapped_count) return kvirt_to_phys((void *)(uintptr_t)va);
    if (va < mapped_base || (va - mapped_base) / 4096 >= mapped_count)
        return UINT64_MAX;
    uint64_t page = mapped_pages[(va - mapped_base) / 4096];
    return page == UINT64_MAX ? page : page + (va & 4095);
}
uint64_t paging_translate_in_cr3(uint64_t cr3, uint64_t va)
{
    CHECK(cr3 == xhci_test_cr3());
    translations++;
    return mapped_phys(va);
}
void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint64_t v, int d) { (void)v; (void)d; }
void serial_putdec(uint64_t v) { (void)v; }
void input_post_key(uint8_t s, bool p, bool e)
{
    (void)s; (void)p; (void)e;
    if (key_hook) { void (*fn)(void) = key_hook; key_hook = NULL; fn(); }
}
bool input_keyboard_post_set1(const uint8_t *bytes, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) { keys++; last_key = bytes[i]; }
    if (key_hook) { void (*fn)(void) = key_hook; key_hook = NULL; fn(); }
    return true;
}
void input_post_mouse_move(int16_t x, int16_t y) { (void)x; (void)y; }
void input_set_mouse_abs(int32_t x, int32_t y) { (void)x; (void)y; }
void input_post_mouse_button(uint8_t b) { (void)b; }
void input_post_mouse_wheel(int16_t delta) { (void)delta; }
void input_get_cursor(int32_t *x, int32_t *y) { *x = *y = 0; }

static void record_access(volatile void *addr, unsigned width, uint64_t value)
{
    CHECK(n_accesses < sizeof(accesses) / sizeof(accesses[0]));
    if (n_accesses < sizeof(accesses) / sizeof(accesses[0]))
        accesses[n_accesses++] = (access_t){(uint32_t)((uintptr_t)addr - 0x10000), width, value};
}

static uint32_t test_read32(volatile void *addr)
{
    CHECK((uintptr_t)addr == 0x10000 + 0x40 + XHCI_OP_USBSTS);
    record_access(addr, 0, status);
    return status;
}

static void test_write32(volatile void *addr, uint32_t value)
{
    uint32_t off = (uint32_t)((uintptr_t)addr - 0x10000);
    record_access(addr, 4, value);
    if (off == 0x40 + XHCI_OP_USBSTS) status &= ~value;
    if (write_hook) write_hook(off);
}

static void test_write64(volatile void *addr, uint64_t value)
{
    record_access(addr, 8, value);
    if (write_hook) write_hook((uint32_t)((uintptr_t)addr - 0x10000));
}

static xhci_hc_t *reset(void)
{
    memset(hc_list, 0, sizeof(hc_list));
    memset(events, 0, sizeof(events));
    memset(transfers, 0, sizeof(transfers));
    memset(report, 0, sizeof(report));
    n_accesses = keys = last_key = 0;
    status = 0;
    clock_ns = 0;
    write_hook = NULL;
    key_hook = NULL;
    mapped_count = translations = 0;
    mapped_base = 0;
    control_physical = 0;
    hc_count = 1;
    xhci_hc_t *hc = &hc_list[0];
    hc->initialized = true;
    hc->base = (void *)0x10000;
    hc->op_off = 0x40;
    hc->rt_off = 0x1000;
    hc->db_off = 0x2000;
    hc->evt_ring = events;
    hc->evt_ring_phys = 0x800000;
    hc->evt_cycle = 1;
    return hc;
}

static void event(unsigned idx, unsigned cycle)
{
    events[idx].control = XHCI_TRB_TYPE(TRB_PORT_STATUS_CHANGE) | cycle;
}

static void check_ack(unsigned end, unsigned expected_accesses)
{
    CHECK(n_accesses == expected_accesses);
    if (n_accesses) {
        access_t *last = &accesses[n_accesses - 1];
        CHECK(last->off == 0x1000 + XHCI_RT_IR0 + XHCI_IR_ERDP);
        CHECK(last->width == 8);
        CHECK(last->value == (0x800000 + end * 16u + XHCI_ERDP_EHB));
    }
}

static void inject_during_ack(uint32_t off)
{
    if (off != 0x1000 + XHCI_RT_IR0 + XHCI_IR_IMAN) return;
    write_hook = NULL;
    event(hc_list[0].evt_deq, hc_list[0].evt_cycle);
    status |= XHCI_STS_EINT;
    xhci_poll(); /* Reentrant caller must not drain or acknowledge twice. */
}

static void inject_after_drain(uint32_t off)
{
    if (off != 0x1000 + XHCI_RT_IR0 + XHCI_IR_ERDP) return;
    write_hook = NULL;
    event(hc_list[0].evt_deq, hc_list[0].evt_cycle);
    status |= XHCI_STS_EINT;
}

static unsigned bulk_start, bulk_length, bulk_short, bulk_bad_trbs, bulk_events;
static unsigned bulk_mps, bulk_error, bulk_bad_residue;
static bool bulk_in, bulk_stale;
static bool bulk_desktop_poll, bulk_hid;
static uint64_t bulk_address;
static xhci_trb_t hid_transfers[XHCI_XFER_RING_SIZE];

static void bulk_event(unsigned idx, unsigned trb, unsigned code, unsigned residue)
{
    events[idx].param = 0xa00000 + trb * sizeof(xhci_trb_t);
    events[idx].status = code << 24 | residue;
    events[idx].control = XHCI_TRB_TYPE(TRB_TRANSFER_EVENT) | TRB_CYCLE |
                         (1u << 24) | (3u << 16);
}

static void complete_bulk(uint32_t off)
{
    if (off != 0x2004) return;
    write_hook = NULL;
    unsigned at = bulk_start, cycle = 1, done = 0, n = 0;
    unsigned residue = 0, code = 1;
    if (bulk_hid) {
        bulk_event(n, 0, 1, 0);
        events[n++].control = XHCI_TRB_TYPE(TRB_TRANSFER_EVENT) | TRB_CYCLE |
                              (2u << 24) | (3u << 16);
    }
    if (bulk_stale) bulk_event(n++, XHCI_XFER_RING_SIZE - 1, 1, 0);
    for (unsigned t = 0; t < XHCI_XFER_RING_SIZE - 1; t++) {
        xhci_trb_t *trb = &transfers[at];
        unsigned bytes = trb->status & 0x1ffff;
        CHECK((trb->control & TRB_CYCLE) == cycle);
        CHECK(XHCI_TRB_GET_TYPE(trb->control) == TRB_NORMAL);
        CHECK(trb->param == mapped_phys(bulk_address + done));
        if (!bytes || (trb->param & 0xffff) + bytes > 65536) {
            bulk_bad_trbs++;
            bulk_event(n++, at, 5, bytes);
            break;
        }
        done += bytes;
        CHECK(done <= bulk_length);
        unsigned packets = (bulk_length + bulk_mps - 1) / bulk_mps - done / bulk_mps;
        if (packets > 31) packets = 31;
        if (done == bulk_length) packets = 0;
        CHECK(((trb->status >> 17) & 31) == packets);
        CHECK(!!(trb->control & (1u << 2)) == bulk_in);
        if (bulk_error) { bulk_event(n++, at, bulk_error, bytes); break; }
        if (bulk_short && t == 0) {
            residue = bytes - bulk_short;
            code = 13;
            if (bulk_bad_residue) residue = bytes + 1;
            if (!(trb->control & TRB_IOC)) bulk_event(n++, at, code, residue);
        }
        if (trb->control & TRB_IOC) {
            CHECK(done == bulk_length && !(trb->control & (1u << 4)));
            bulk_event(n++, at, code, residue);
            break;
        }
        CHECK(trb->control & (1u << 4));
        if (++at == XHCI_XFER_RING_SIZE - 1) {
            CHECK(transfers[at].param == 0xa00000);
            CHECK(transfers[at].control == (XHCI_TRB_TYPE(TRB_LINK) | TRB_TC |
                                           (1u << 4) | cycle));
            at = 0;
            cycle ^= 1;
        }
    }
    bulk_events = n;
    if (bulk_desktop_poll) xhci_poll();
}

static xhci_trb_t recovery_cmds[XHCI_CMD_RING_SIZE];
static xhci_trb_t recovery_ep0[XHCI_XFER_RING_SIZE];
static unsigned recovery_step, recovery_fail, recovery_dci;
static bool recovery_in;

static void complete_recovery(uint32_t off)
{
    xhci_hc_t *hc = &hc_list[0];
    if (off != 0x2000 && off != 0x2004) return;
    xhci_trb_t event = {0};
    unsigned step = recovery_step++;
    event.status = (step == recovery_fail ? 19u : 1u) << 24;
    if (off == 0x2000) {
        unsigned at = hc->cmd_enq ? hc->cmd_enq - 1 : XHCI_CMD_RING_SIZE - 2;
        xhci_trb_t *cmd = &recovery_cmds[at];
        CHECK(step == 0 || step == 2);
        CHECK(XHCI_TRB_GET_TYPE(cmd->control) ==
              (step == 0 ? TRB_RESET_ENDPOINT : TRB_SET_TR_DEQUEUE));
        CHECK((cmd->control >> 24) == 1);
        CHECK(((cmd->control >> 16) & 31) == recovery_dci);
        CHECK(cmd->status == 0);
        if (step == 0) CHECK(cmd->param == 0 && !(cmd->control & (1u << 9)));
        else CHECK(cmd->param == (recovery_in ? 0xa00000 + 30 * 16 + 1 : 0xb00000));
        event.param = hc->cmd_ring_phys + at * 16;
        event.control = XHCI_TRB_TYPE(TRB_CMD_COMPLETION) | (1u << 24);
    } else {
        CHECK(step == 1);
        usb_setup_t setup;
        memcpy(&setup, &recovery_ep0[0].param, sizeof(setup));
        CHECK(setup.bmRequestType == 2 && setup.bRequest == 1);
        CHECK(setup.wIndex == ((recovery_dci / 2) | (recovery_in ? 0x80 : 0)));
        CHECK(setup.wValue == 0 && setup.wLength == 0);
        event.param = hc->devices[0].ep0_ring_phys + 16;
        event.control = XHCI_TRB_TYPE(TRB_TRANSFER_EVENT) | (1u << 24) | (1u << 16);
    }
    CHECK(evt_pending_push(hc, &event));
}

static void test_bulk_recovery(void)
{
    for (unsigned in = 0; in < 2; in++) {
        for (unsigned fail = 0; fail < 4; fail++) {
            xhci_hc_t *hc = reset();
            xhci_device_t *dev = &hc->devices[0];
            dev->msc_active = true; dev->slot_id = 1;
            dev->bulk_in_dci = 3; dev->bulk_out_dci = 4;
            dev->bulk_in_ring_phys = 0xa00000; dev->bulk_out_ring_phys = 0xb00000;
            dev->bulk_in_enq = 30; dev->bulk_in_cycle = 1;
            dev->bulk_out_enq = 0; dev->bulk_out_cycle = 0;
            hc->cmd_ring = recovery_cmds; hc->cmd_ring_phys = 0xc00000;
            hc->cmd_cycle = 1;
            dev->ep0_ring = recovery_ep0; dev->ep0_ring_phys = 0xd00000;
            dev->ep0_cycle = 1;
            memset(recovery_cmds, 0, sizeof(recovery_cmds));
            memset(recovery_ep0, 0, sizeof(recovery_ep0));
            recovery_step = 0; recovery_fail = fail; recovery_in = in;
            recovery_dci = in ? 3 : 4;
            write_hook = complete_recovery;
            CHECK((xhci_bulk_clear_halt(0, in) == 0) == (fail == 3));
            CHECK(recovery_step == (fail < 3 ? fail + 1 : 3));
        }
    }
    reset();
    CHECK(xhci_bulk_clear_halt(-1, true) < 0);
    CHECK(xhci_bulk_clear_halt(0, false) < 0);
}

static void test_bulk(void)
{
    const uint64_t addresses[] = {0x90f000, 0x900000, 0x90ffff};
    const unsigned lengths[] = {8192, 31, 65536, 131073};
    for (unsigned start = 0; start < XHCI_XFER_RING_SIZE - 1; start++) {
        for (unsigned a = 0; a < sizeof(addresses)/sizeof(addresses[0]); a++) {
            for (unsigned l = 0; l < sizeof(lengths)/sizeof(lengths[0]); l++) {
                for (unsigned mode = 0; mode < 3; mode++) {
                    xhci_hc_t *hc = reset();
                    xhci_device_t *dev = &hc->devices[0];
                    dev->slot_id = 1;
                    bulk_mps = a == 0 ? 64 : a == 1 ? 512 : 1024;
                    dev->bulk_in_max_pkt = dev->bulk_out_max_pkt = bulk_mps;
                    bulk_start = start;
                    bulk_address = addresses[a];
                    bulk_length = lengths[l];
                    bulk_in = mode != 0;
                    bulk_short = mode == 2 ? 1 : 0;
                    bulk_stale = start == 1;
                    bulk_desktop_poll = mode == 1;
                    bulk_hid = false;
                    bulk_bad_trbs = bulk_events = 0;
                    bulk_error = bulk_bad_residue = 0;
                    unsigned enq = start, actual = 0xffffffff;
                    uint8_t cycle = 1;
                    write_hook = complete_bulk;
                    int rc = bulk_xfer(hc, dev, transfers, 0xa00000,
                        &enq, &cycle, 3, (void *)(uintptr_t)bulk_address,
                        bulk_length, bulk_in, &actual);
                    CHECK(rc == 0);
                    CHECK(bulk_bad_trbs == 0);
                    CHECK(actual == (bulk_short ? bulk_short : bulk_length));
                    CHECK(hc->evt_deq == bulk_events);
                    if (failures) return;
                }
            }
        }
    }
    puts("PASS bulk: 1116 TDs, 64-KiB boundaries, ring wrap, TD size, short IN, stale events");

    for (unsigned mode = 0; mode < 5; mode++) {
        xhci_hc_t *hc = reset();
        xhci_device_t *dev = &hc->devices[0];
        dev->slot_id = 1;
        dev->bulk_in_max_pkt = dev->bulk_out_max_pkt = bulk_mps = 512;
        bulk_start = 0; bulk_address = 0x900000; bulk_length = 31;
        bulk_in = mode != 0;
        bulk_short = mode < 2 ? 1 : 0;
        bulk_error = mode == 2 ? 6 : 0;
        bulk_bad_residue = mode == 1;
        bulk_stale = false;
        unsigned enq = 0, actual = 0xffffffff;
        uint8_t cycle = 1;
        write_hook = complete_bulk;
        if (mode == 3) bulk_length = 32 * 65536;
        if (mode == 4) dev->bulk_in_max_pkt = 0;
        CHECK(bulk_xfer(hc, dev, transfers, 0xa00000, &enq, &cycle, 3,
                       (void *)(uintptr_t)bulk_address, bulk_length, bulk_in, &actual) < 0);
        CHECK(actual == 0);
        if (mode >= 3) CHECK(n_accesses == 0 && enq == 0 && cycle == 1);
    }
    puts("PASS bulk: reject short OUT, invalid residue, stall, ring overflow and zero packet size");
}

static void nested_keyboard_poll(void)
{
    CHECK(!event_polling);
    xhci_hc_t *hc = &hc_list[0];
    event(hc->evt_deq, hc->evt_cycle);
    xhci_poll();
}

static void test_bulk_mapped(void)
{
    /* Same low virtual stack range as a PE64 caller, backed by unrelated RAM. */
    for (unsigned mode = 0; mode < 6; mode++) {
        xhci_hc_t *hc = reset();
        xhci_device_t *dev = &hc->devices[0];
        dev->slot_id = 1;
        dev->bulk_in_max_pkt = dev->bulk_out_max_pkt = bulk_mps = 512;
        mapped_base = 0x4000a00000;
        if (mode == 5) mapped_base = KERNEL_VBASE + 0x900000;
        mapped_count = 3;
        mapped_pages[0] = 0x16a3f000;
        mapped_pages[1] = 0x800000;
        mapped_pages[2] = 0x801000;
        bulk_start = XHCI_XFER_RING_SIZE - 2;
        bulk_address = mapped_base + 0xfc0;
        bulk_length = mode < 2 ? 31 : 8192;
        bulk_short = mode == 4 ? 5 : 0; bulk_error = bulk_bad_residue = 0;
        bulk_in = mode == 4 || (mode & 1) != 0;
        bulk_stale = bulk_hid = bulk_desktop_poll = false;
        bulk_bad_trbs = bulk_events = 0;
        unsigned enq = bulk_start, actual = 0;
        uint8_t cycle = 1;
        write_hook = complete_bulk;
        CHECK(bulk_xfer(hc, dev, transfers, 0xa00000, &enq, &cycle, 3,
                       (void *)(uintptr_t)bulk_address, bulk_length, bulk_in, &actual) == 0);
        CHECK(translations && actual == (bulk_short ? bulk_short : bulk_length) && bulk_bad_trbs == 0);
        CHECK(transfers[bulk_start].param == 0x16a3ffc0);
        if (mode >= 2) CHECK(transfers[0].param == 0x800000);
    }
    for (unsigned mode = 0; mode < 5; mode++) {
        xhci_hc_t *hc = reset();
        xhci_device_t *dev = &hc->devices[0];
        dev->slot_id = 1;
        dev->bulk_in_max_pkt = dev->bulk_out_max_pkt = bulk_mps = 512;
        mapped_base = 0x4000a00000; mapped_count = 32;
        for (unsigned i = 0; i < mapped_count; i++) mapped_pages[i] = 0x800000 + i * 8192;
        uint64_t va = mapped_base;
        unsigned len = 8192;
        if (mode == 0) mapped_pages[0] = UINT64_MAX;
        if (mode == 1) mapped_pages[1] = UINT64_MAX;
        if (mode == 2) { va = UINT64_MAX - 31; len = 64; }
        if (mode == 3) len = 32 * 4096;
        if (mode == 4) va = 0x0000800000000000ULL;
        memset(transfers, 0x55, sizeof(transfers));
        xhci_trb_t before[XHCI_XFER_RING_SIZE];
        memcpy(before, transfers, sizeof(before));
        unsigned enq = 7, actual = 99;
        uint8_t cycle = 1;
        CHECK(bulk_xfer(hc, dev, transfers, 0xa00000, &enq, &cycle, 3,
                       (void *)(uintptr_t)va, len, true, &actual) < 0);
        CHECK(!n_accesses && enq == 7 && cycle == 1 && actual == 0);
        CHECK(!memcmp(before, transfers, sizeof(before)));
    }
    puts("PASS DMA: remapped stack, noncontiguous pages, invalid ranges rejected before submission");
}

static void complete_control(uint32_t off)
{
    if (off != 0x2004) return;
    write_hook = NULL;
    if (control_physical) CHECK(transfers[1].param == control_physical);
    for (unsigned i = 0; i < 4; i++) {
        events[i].param = 0xa00020;
        events[i].status = 1u << 24;
        events[i].control = XHCI_TRB_TYPE(TRB_TRANSFER_EVENT) | TRB_CYCLE |
                             ((i == 0 ? 2u : 1u) << 24) | ((i == 1 ? 4u : 1u) << 16);
    }
    events[2].param = 0xa00010;
    events[2].status = 13u << 24 | 1;
    xhci_poll();
}

static void test_control_mapped(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        xhci_hc_t *hc = reset();
        xhci_device_t *dev = &hc->devices[0];
        dev->slot_id = 1; dev->ep0_ring = transfers;
        dev->ep0_ring_phys = 0xa00000; dev->ep0_cycle = 1;
        mapped_base = 0x4000a00000; mapped_count = 2;
        mapped_pages[0] = 0x90f000;
        mapped_pages[1] = mode == 3 ? UINT64_MAX : 0x800000;
        unsigned len = mode ? 80 : 8;
        control_physical = 0x90ffc0;
        if (mode == 1) mapped_pages[0] = UINT64_MAX;
        usb_setup_t setup = {0};
        write_hook = complete_control;
        int rc = ctrl_transfer(hc, dev, &setup, (void *)(uintptr_t)(mapped_base + 0xfc0), len, true);
        CHECK((rc == 0) == (mode == 0));
        CHECK(translations);
        if (mode) {
            CHECK(!n_accesses && dev->ep0_enq == 0 && dev->ep0_cycle == 1);
            CHECK(!transfers[0].control && !transfers[1].control);
        }
    }
    puts("PASS control DMA: remapped buffers, absent/noncontiguous/64-KiB-crossing spans rejected");
}

static void halt_on_doorbell(uint32_t off)
{
    if (off != 0x2004) return;
    write_hook = NULL;
    status = XHCI_STS_HSE | XHCI_STS_HCH;
}

static void test_bulk_dead(void)
{
    for (unsigned dead = 0; dead < 2; dead++) {
        xhci_hc_t *hc = reset();
        xhci_device_t *dev = &hc->devices[0];
        dev->slot_id = 1; dev->bulk_in_max_pkt = 512;
        hc->initialized = !dead;
        unsigned enq = 0, actual = 99;
        uint8_t cycle = 1;
        write_hook = halt_on_doorbell;
        CHECK(bulk_xfer(hc, dev, transfers, 0xa00000, &enq, &cycle, 3,
                       (void *)0x900000, 31, true, &actual) < 0);
        CHECK(!actual && !hc->initialized);
        CHECK(n_accesses == (dead ? 0 : 2));
    }
    puts("PASS bulk: stop immediately on HSE, no submission to a stopped controller");
}

static void test_shared_events(void)
{
    xhci_hc_t *hc = reset();
    xhci_device_t *disk = &hc->devices[0], *keyboard = &hc->devices[1];
    disk->slot_id = 1;
    disk->bulk_in_max_pkt = disk->bulk_out_max_pkt = bulk_mps = 512;
    keyboard->slot_id = 2;
    keyboard->hid_active = true;
    keyboard->int_ep_dci = 3;
    keyboard->int_max_pkt = 8;
    keyboard->int_cycle = 1;
    keyboard->report_buf = report;
    keyboard->report_buf_phys = 0x900000;
    keyboard->int_ring = hid_transfers;
    keyboard->int_ring_phys = 0xb00000;
    hid_caps_set_boot_keyboard(&keyboard->hid_caps);
    report[2] = 4;
    key_hook = nested_keyboard_poll;
    bulk_start = 30; bulk_address = 0x90f000; bulk_length = 8192;
    bulk_short = 3; bulk_error = bulk_bad_residue = 0;
    bulk_in = bulk_hid = bulk_desktop_poll = true;
    bulk_stale = false;
    unsigned enq = bulk_start, actual;
    uint8_t cycle = 1;
    write_hook = complete_bulk;
    CHECK(bulk_xfer(hc, disk, transfers, 0xa00000, &enq, &cycle, 3,
                   (void *)(uintptr_t)bulk_address, bulk_length, true, &actual) == 0);
    CHECK(actual == 3 && keys == 1 && last_key == 0x1e);
    CHECK(keyboard->int_enq == 1 && keyboard->hid_pending == 0);
    CHECK(hc->pending_count == 0 && hc->evt_deq == bulk_events + 1);
    puts("PASS shared ring: desktop drains bulk+HID, short IOC preserved, nested HID callback can poll");

    hc = reset();
    hc->cmd_enq = 1; hc->cmd_ring_phys = 0x500000;
    events[0].param = hc->cmd_ring_phys;
    events[0].status = 1u << 24;
    events[0].control = XHCI_TRB_TYPE(TRB_CMD_COMPLETION) | TRB_CYCLE | (7u << 24);
    xhci_poll();
    uint8_t slot = 0;
    CHECK(cmd_wait(hc, &slot) == 1 && slot == 7 && hc->pending_count == 0);

    /* Reusing a free inbox slot must not reorder short and final events. */
    xhci_trb_t saved, ev = { .param = 0x500000, .status = 1u << 24,
        .control = XHCI_TRB_TYPE(TRB_CMD_COMPLETION) };
    CHECK(evt_pending_push(hc, &ev));
    ev.param = 0xa00000; ev.status = 13u << 24 | 7;
    ev.control = XHCI_TRB_TYPE(TRB_TRANSFER_EVENT) | (1u << 24) | (3u << 16);
    CHECK(evt_pending_push(hc, &ev));
    CHECK(evt_pending_take(hc, TRB_CMD_COMPLETION, 0, 0, 0x500000, 0, 1, 31, &saved));
    ev.param += 16;
    CHECK(evt_pending_push(hc, &ev));
    CHECK(evt_pending_take(hc, TRB_TRANSFER_EVENT, 1, 3, 0xa00000, 0, 2, 31, &saved));
    CHECK(saved.param == 0xa00000);
    CHECK(evt_pending_take(hc, TRB_TRANSFER_EVENT, 1, 3, 0xa00000, 0, 2, 31, &saved));
    CHECK(saved.param == 0xa00010 && hc->pending_count == 0);
    puts("PASS command delivery and FIFO order across inbox slot reuse");

    hc = reset();
    disk = &hc->devices[0];
    disk->slot_id = 1; disk->ep0_ring = transfers;
    disk->ep0_ring_phys = 0xa00000; disk->ep0_cycle = 1;
    usb_setup_t setup = {0};
    write_hook = complete_control;
    CHECK(ctrl_transfer(hc, disk, &setup, (void *)0x90f400, 8, true) == 0);
    CHECK(hc->evt_deq == 4 && hc->pending_count == 2);
    puts("PASS control waits for its own EP0 status, not another slot/endpoint or early short packet");

    hc = reset();
    ev.control = XHCI_TRB_TYPE(TRB_CMD_COMPLETION);
    for (unsigned i = 0; i < XHCI_EVT_RING_SIZE; i++) {
        ev.param = 0x500000 + i * 16;
        CHECK(evt_pending_push(hc, &ev));
    }
    events[0] = ev; events[0].control |= TRB_CYCLE;
    xhci_poll();
    CHECK(hc->evt_deq == 0 && hc->pending_count == XHCI_EVT_RING_SIZE);
    CHECK(evt_pending_take(hc, TRB_CMD_COMPLETION, 0, 0, 0x500000, 0, 1, 64, &saved));
    xhci_poll();
    CHECK(hc->evt_deq == 1 && hc->pending_count == XHCI_EVT_RING_SIZE);
    puts("PASS full inbox backpressure retains the DMA event until space is available");
}

int main(void)
{
    xhci_hc_t *hc = reset();
    xhci_poll();
    CHECK(n_accesses == 2 && hc->poll_status_valid && hc->poll_status_ns == 0);
    n_accesses = 0;
    for (unsigned i = 0; i < 100000; i++) { clock_ns++; xhci_poll(); }
    CHECK(n_accesses == 0);
    clock_ns = 99999999;
    xhci_poll();
    CHECK(n_accesses == 0);
    clock_ns++;
    xhci_poll();
    CHECK(n_accesses == 2 && hc->poll_status_ns == clock_ns);
    puts("PASS idle: 100000 polls without MMIO, health check at 100 ms (also time zero)");

    n_accesses = 0;
    event(0, 1);
    status = XHCI_STS_EINT | XHCI_STS_PCD;
    xhci_poll();
    check_ack(1, 4);
    CHECK(hc->evt_deq == 1 && status == XHCI_STS_PCD);
    CHECK(accesses[0].width == 0 && accesses[0].off == 0x44);
    CHECK(accesses[1].off == 0x44 && accesses[1].value == XHCI_STS_EINT);
    CHECK(accesses[2].off == 0x1020 && accesses[2].value == (XHCI_IMAN_IP | XHCI_IMAN_IE));
    puts("PASS ready event bypasses deadline; EINT precedes IP, IE and unrelated status preserved");

    hc = reset();
    for (unsigned i = 0; i < 40; i++) event(i, 1);
    xhci_poll();
    CHECK(hc->evt_deq == 16);
    check_ack(15, 3);
    n_accesses = 0;
    xhci_poll();
    CHECK(hc->evt_deq == 32);
    check_ack(31, 3);
    n_accesses = 0;
    xhci_poll();
    CHECK(hc->evt_deq == 40);
    check_ack(40, 3);
    n_accesses = 0;
    xhci_poll();
    CHECK(n_accesses == 0);
    puts("PASS 40-event burst: bounded 16/16/8, one ERDP each, no release of unconsumed entries");

    hc = reset();
    hc->evt_deq = XHCI_EVT_RING_SIZE - 2;
    event(XHCI_EVT_RING_SIZE - 2, 1);
    event(XHCI_EVT_RING_SIZE - 1, 1);
    event(0, 0);
    event(1, 0);
    events[2].control = TRB_CYCLE;
    xhci_poll();
    CHECK(hc->evt_deq == 2 && hc->evt_cycle == 0);
    check_ack(2, 3);
    puts("PASS event ring wrap and consumer cycle toggle");

    hc = reset();
    write_hook = inject_during_ack;
    status = XHCI_STS_EINT;
    xhci_poll();
    CHECK(hc->evt_deq == 1 && status == XHCI_STS_EINT);
    check_ack(1, 4);
    n_accesses = 0;
    event(1, 1);
    write_hook = inject_after_drain;
    xhci_poll();
    CHECK(hc->evt_deq == 2);
    n_accesses = 0;
    xhci_poll();
    CHECK(hc->evt_deq == 3);
    check_ack(3, 4);
    puts("PASS arrivals during IP/ERDP ack, reentrant guard, next-poll delivery");

    hc = reset();
    xhci_device_t *dev = &hc->devices[0];
    dev->slot_id = 1;
    dev->hid_active = true;
    dev->int_ep_dci = 3;
    dev->int_max_pkt = 8;
    dev->report_buf = report;
    dev->report_buf_phys = 0x900000;
    dev->int_ring = transfers;
    dev->int_ring_phys = 0xa00000;
    dev->int_enq = XHCI_XFER_RING_SIZE - 2;
    dev->int_cycle = 1;
    hid_caps_set_boot_keyboard(&dev->hid_caps);
    report[2] = 4; /* A make, then release, through the real HID decoder. */
    events[0].control = XHCI_TRB_TYPE(TRB_TRANSFER_EVENT) | TRB_CYCLE | (1u << 24) | (3u << 16);
    events[0].status = 1u << 24;
    xhci_poll();
    CHECK(keys == 1 && last_key == 0x1e);
    CHECK(dev->int_enq == 0 && dev->int_cycle == 0);
    CHECK(transfers[XHCI_XFER_RING_SIZE - 2].param == dev->report_buf_phys);
    CHECK(transfers[XHCI_XFER_RING_SIZE - 1].control == (XHCI_TRB_TYPE(TRB_LINK) | TRB_TC | TRB_CYCLE));
    CHECK(accesses[n_accesses - 1].off == 0x2004 && accesses[n_accesses - 1].value == 3);
    report[2] = 0;
    events[1] = events[0];
    events[1].status = 13u << 24; /* Short packet completion is also accepted. */
    xhci_poll();
    CHECK(keys == 2 && last_key == 0x9e && dev->int_enq == 1);
    CHECK((transfers[0].control & TRB_CYCLE) == 0);
    puts("PASS keyboard make/break, short completion, transfer ring wrap and doorbell");

    for (unsigned i = 0; i < 2; i++) {
        hc = reset();
        xhci_poll();
        status = i ? XHCI_STS_HSE : XHCI_STS_HCH;
        clock_ns = 100000000;
        n_accesses = 0;
        xhci_poll();
        CHECK(!hc->initialized && n_accesses == 1);
        xhci_poll();
        CHECK(n_accesses == 1);
    }
    hc = reset();
    xhci_poll();
    status = XHCI_STS_HSE;
    event(0, 1);
    n_accesses = 0;
    xhci_poll();
    CHECK(!hc->initialized && n_accesses == 1 && hc->evt_deq == 0);
    puts("PASS halted/faulted controller detection, idle and ready, no resubmission");
    test_bulk();
    test_bulk_mapped();
    test_bulk_dead();
    test_bulk_recovery();
    test_control_mapped();
    test_shared_events();
    printf("xHCI poll/bulk failures: %u\n", failures);
    return failures != 0;
}
