#define PS2_HOST_TEST 1
#include "../drivers/ps2.c"

extern int printf(const char *format, ...);
extern void abort(void);

static struct {
    struct { uint8_t status, byte; } rx[4096];
    unsigned read, write, io_reads, io_writes, resend, resolutions, mode_sets;
    unsigned keys, moves, buttons, wheels, bridged, violations;
    uint8_t config, master, slave, pending, parameter, id, resolution;
    uint8_t last_key, mode, last_buttons, nack;
    uint64_t now;
    int16_t dx, dy, wheel;
    int32_t x, y;
    bool absent, no_aux, stuck, no_clock, synaptics, bad_bat;
    bool interleave, inject_bad_key, irq_on, in_irq, reenter;
} hw;

static void receive(uint8_t byte, uint8_t flags)
{
    if (hw.write - hw.read >= 4096) abort();
    hw.rx[hw.write % 4096].status = OBF | flags;
    hw.rx[hw.write++ % 4096].byte = byte;
}

uint8_t ps2_test_inb(uint16_t port)
{
    hw.io_reads++;
    if (port == 0x21) return hw.master;
    if (port == 0xa1) return hw.slave;
    if (port == CONTROL) {
        if (hw.absent) return 0xff;
        if (hw.stuck) return IBF;
        return hw.read != hw.write ? hw.rx[hw.read % 4096].status : 0;
    }
    if (port != DATA || hw.read == hw.write) { hw.violations++; return 0; }
    return hw.rx[hw.read++ % 4096].byte;
}

void ps2_test_outb(uint16_t port, uint8_t byte)
{
    hw.io_writes++;
    if (port == 0x21) { hw.master = byte; return; }
    if (port == 0xa1) { hw.slave = byte; return; }
    if (port == CONTROL) {
        switch (byte) {
        case 0xad: hw.config |= 0x10; break;
        case 0xae: hw.config &= ~0x10; break;
        case 0xa7: hw.config |= 0x20; break;
        case 0xa8: hw.config &= ~0x20; break;
        case 0x20: receive(hw.config, 0); break;
        case 0x60: case 0xd4: hw.pending = byte; break;
        default: hw.violations++; break;
        }
        return;
    }
    if (port != DATA) { hw.violations++; return; }
    if (hw.pending == 0x60) { hw.config = byte; hw.pending = 0; return; }
    if (hw.pending != 0xd4) { hw.violations++; return; }
    hw.pending = 0;
    if (hw.no_aux) return;
    if (hw.resend) { hw.resend--; receive(0xfe, AUX); return; }
    if (byte == hw.nack) { receive(0xfc, AUX); return; }
    if (hw.interleave) { receive(0x1e, 0); hw.interleave = false; }
    if (hw.inject_bad_key) { receive(0x2a, RX_ERROR); hw.inject_bad_key = false; }
    receive(0xfa, AUX);
    if (hw.parameter) {
        if (hw.parameter == 0xe8) { hw.resolutions++; hw.resolution = byte; }
        if (hw.parameter == 0xf3 && byte == 0x14) {
            if (!hw.synaptics) hw.violations++;
            hw.mode_sets++;
            hw.mode = 0;
        }
        hw.parameter = 0;
        return;
    }
    switch (byte) {
    case 0xff: receive(hw.bad_bat ? 0xfc : 0xaa, AUX); receive(hw.id, AUX); break;
    case 0xe9:
        receive(hw.synaptics ? 0x05 : 0, AUX);
        receive(hw.synaptics ? 0x47 : hw.resolution, AUX);
        receive(hw.synaptics ? 0x08 : 100, AUX);
        break;
    case 0xf2: receive(hw.id, AUX); break;
    case 0xf3: case 0xe8: hw.parameter = byte; break;
    case 0xf6: case 0xf5: case 0xf4: case 0xea: case 0xe6: break;
    default: hw.violations++; break;
    }
}

uint64_t ps2_test_irq_save(void)
{
    uint64_t flags = hw.irq_on ? 512 : 0;
    hw.irq_on = false;
    return flags;
}
void ps2_test_irq_restore(uint64_t flags) { if (flags & 512) hw.irq_on = true; }
uint64_t idt_get_monotonic_ns(void) { return hw.no_clock ? 0 : (hw.now += 10000); }
void kb_handle_hw_byte(uint8_t byte) { hw.last_key = byte; hw.keys++; }
void serial_puts(const char *text) { (void)text; }
void serial_puthex(uint64_t value, int digits) { (void)value; (void)digits; }
void input_post_mouse_move(int16_t dx, int16_t dy)
{
    hw.moves++; hw.dx = dx; hw.dy = dy; hw.x += dx; hw.y += dy;
}
void input_post_mouse_button(uint8_t buttons) { hw.buttons++; hw.last_buttons = buttons; }
void input_post_mouse_wheel(int16_t wheel) { hw.wheels++; hw.wheel = wheel; }
void input_get_cursor(int32_t *x, int32_t *y) { *x = hw.x; *y = hw.y; }
void win32_post_mouse_screen(int x, int y, int dx, int dy, uint32_t buttons, short wheel)
{
    (void)dx; (void)dy; (void)wheel;
    hw.bridged++;
    if (hw.in_irq || x != hw.x || y != hw.y || buttons != hw.last_buttons) hw.violations++;
    if (hw.reenter) ps2_poll();
}

static void reset(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.irq_on = true;
    hw.config = 0x45;
    hw.master = hw.slave = 0xff;
    hw.nack = 0xab;
    memset(&stats, 0, sizeof(stats));
    stats.status = "not initialized";
    initialized = false;
    port_lock = poll_lock = packet_pos = 0;
    head = tail = 0;
    packet_time = 0;
}

static void irq(void)
{
    hw.in_irq = true;
    ps2_aux_irq();
    hw.in_irq = false;
}

static void movement(int dx, int dy, uint8_t buttons)
{
    receive(8 | buttons | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0), AUX);
    receive((uint8_t)dx, AUX);
    receive((uint8_t)dy, AUX);
}

int main(void)
{
    unsigned checks = 0, failures = 0;
#define CHECK(test) do { checks++; if (!(test)) { failures++; printf("[PS2] FAIL line %u: %s\n", __LINE__, #test); } } while (0)
    reset(); ps2_init();
    CHECK(stats.ready && stats.controller && !stats.synaptics && stats.packet_size == 3);
    CHECK(hw.config == 0x47 && hw.master == 0xf9 && hw.slave == 0xef);
    CHECK(hw.irq_on && !hw.violations && !stats.timeouts && !hw.mode_sets);
    unsigned writes = hw.io_writes;
    ps2_init(); CHECK(hw.io_writes == writes);

    for (int d = -256; d <= 255; d++) {
        movement(d, -1, 1); irq();
        CHECK(hw.bridged == stats.delivered);
        ps2_poll();
        CHECK(hw.dx == d && hw.dy == 1 && hw.last_buttons == 1);
        CHECK(hw.irq_on && !hw.violations);
    }
    for (int d = -256; d <= 255; d++) {
        movement(1, d, 0); ps2_poll();
        CHECK(hw.dx == 1 && hw.dy == -d && !hw.last_buttons);
    }

    reset(); hw.synaptics = true; hw.mode = 0x8c; hw.interleave = true; hw.resend = 1;
    ps2_init();
    CHECK(stats.ready && stats.synaptics && stats.identity == 0x054708);
    CHECK(hw.mode_sets == 1 && hw.mode == 0 && hw.resolutions == 9 && hw.resolution == 2);
    CHECK(hw.keys == 1 && hw.last_key == 0x1e && stats.resends == 1 && !hw.violations);
    receive(0x30, 0); movement(20, 9, 2); receive(0xb0, 0); irq();
    CHECK(hw.keys == 3 && hw.last_key == 0xb0 && !hw.bridged);
    hw.reenter = true; ps2_poll();
    CHECK(hw.bridged == 1 && hw.dx == 20 && hw.dy == -9 && hw.last_buttons == 2);
    CHECK(!hw.violations && hw.irq_on);

    reset(); hw.no_aux = true; ps2_init();
    CHECK(!stats.ready && stats.timeouts && hw.config == 0x65 && hw.master == 0xfd && hw.slave == 0xff);
    receive(0x1c, 0); irq(); CHECK(hw.keys == 1 && hw.last_key == 0x1c);
    CHECK(!hw.violations && hw.irq_on);
    reset(); hw.absent = true; ps2_init(); ps2_poll();
    CHECK(!stats.controller && !stats.ready && !hw.violations && hw.io_reads < 20);
    reset(); hw.stuck = true; ps2_init();
    CHECK(!stats.ready && stats.timeouts && hw.irq_on);
    reset(); hw.stuck = hw.no_clock = true; ps2_init();
    CHECK(!stats.ready && stats.timeouts && hw.io_reads < WAIT_POLLS * 5 && hw.irq_on);
    reset(); hw.resend = 10; ps2_init();
    CHECK(!stats.ready && stats.resends == 3 && hw.config == 0x65);
    reset(); hw.bad_bat = true; ps2_init(); CHECK(!stats.ready && !hw.violations);
    reset(); hw.id = 7; ps2_init(); CHECK(!stats.ready && stats.device_id == 7 && !hw.violations);
    reset(); hw.nack = 0xf4; ps2_init(); CHECK(!stats.ready && hw.config == 0x65 && !hw.violations);
    reset(); hw.synaptics = true; hw.nack = 0x14; ps2_init();
    CHECK(stats.synaptics && !stats.ready && !hw.mode_sets && !hw.violations);
    reset(); hw.inject_bad_key = true; ps2_init();
    CHECK(stats.ready && stats.bad_bytes == 1 && hw.keys == 0 && !hw.violations);

    reset(); ps2_init();
    receive(0, AUX); movement(2, 3, 1); irq(); ps2_poll();
    CHECK(stats.resyncs == 1 && hw.dx == 2 && hw.dy == -3);
    receive(8, AUX); receive(10, AUX | RX_ERROR); irq();
    movement(4, 5, 0); irq(); ps2_poll();
    CHECK(stats.bad_bytes == 1 && hw.dx == 4 && hw.dy == -5);
    receive(8, AUX); irq(); hw.now += PACKET_GAP_NS + 1;
    movement(6, 7, 0); irq(); ps2_poll();
    CHECK(stats.resyncs == 2 && hw.dx == 6 && hw.dy == -7);
    unsigned moves = hw.moves;
    receive(0xc9, AUX); receive(255, AUX); receive(255, AUX); irq(); ps2_poll();
    CHECK(hw.moves == moves && hw.last_buttons == 1);
    movement(0, 0, 0); irq(); ps2_poll(); CHECK(hw.last_buttons == 0 && hw.moves == moves);

    reset(); hw.id = 3; ps2_init();
    movement(0, 0, 0); receive(0xff, AUX); ps2_poll();
    CHECK(stats.packet_size == 4 && hw.wheels == 1 && hw.wheel == 1 && !hw.violations);
    reset(); hw.id = 4; ps2_init();
    movement(0, 0, 0); receive(0x1f, AUX); ps2_poll();
    CHECK(stats.packet_size == 4 && hw.wheels == 1 && hw.wheel == 1 && !hw.violations);

    reset(); ps2_init();
    for (unsigned i = 0; i < QUEUE_SIZE + 10; i++) { movement(1, 0, 1); irq(); }
    movement(0, 0, 0); irq(); ps2_poll();
    CHECK(stats.dropped == 11 && stats.delivered == QUEUE_SIZE && hw.last_buttons == 0);
    CHECK(!hw.violations && hw.irq_on);
    reset(); ps2_init();
    port_lock = 1; movement(1, 0, 0); irq(); CHECK(hw.read != hw.write);
    port_lock = 0; ps2_poll(); CHECK(stats.delivered == 1);
    for (unsigned i = 0; i < 200; i++) receive(0x1e, 0);
    irq(); CHECK(hw.keys == 64); ps2_poll(); CHECK(hw.keys == 128);
    irq(); irq(); CHECK(hw.keys == 200 && !hw.violations);
    hw.irq_on = false; ps2_poll(); CHECK(!hw.irq_on);

    printf("[PS2] checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
