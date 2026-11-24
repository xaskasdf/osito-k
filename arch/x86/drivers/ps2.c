/* Shared i8042 receive path and PS/2 relative pointing devices.
 * IRQ1/12 only decode and queue. GUI callbacks run from the polling owner. */
#include "ps2.h"

extern void kb_handle_hw_byte(uint8_t byte);
extern uint64_t idt_get_monotonic_ns(void);
extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);
extern void input_post_mouse_move(int16_t dx, int16_t dy);
extern void input_post_mouse_button(uint8_t buttons);
extern void input_post_mouse_wheel(int16_t wheel);
extern void input_get_cursor(int32_t *x, int32_t *y);
extern void win32_post_mouse_screen(int x, int y, int dx, int dy,
                                    uint32_t buttons, short wheel)
    __attribute__((weak));

#ifdef PS2_HOST_TEST
extern uint8_t ps2_test_inb(uint16_t port);
extern void ps2_test_outb(uint16_t port, uint8_t value);
extern uint64_t ps2_test_irq_save(void);
extern void ps2_test_irq_restore(uint64_t flags);
#define inb ps2_test_inb
#define outb ps2_test_outb
#define irq_save ps2_test_irq_save
#define irq_restore ps2_test_irq_restore
#else
static uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags)
{
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}
#endif

#define DATA 0x60
#define CONTROL 0x64
#define OBF 0x01
#define IBF 0x02
#define AUX 0x20
#define RX_ERROR 0xc0
#define WAIT_POLLS 1000000U
#define COMMAND_NS 200000000ULL
#define PACKET_GAP_NS 100000000ULL
#define QUEUE_SIZE 128U

typedef struct {
    int16_t dx, dy, wheel;
    uint8_t buttons;
} mouse_packet_t;

static ps2_stats_t stats = {.status = "not initialized"};
static uint8_t port_lock, poll_lock;
static bool initialized;
static uint8_t packet[4], packet_pos;
static uint64_t packet_time;
static mouse_packet_t queue[QUEUE_SIZE];
static uint32_t head, tail;

static bool wait_write(void)
{
    uint64_t start = idt_get_monotonic_ns();
    for (unsigned i = 0; i < WAIT_POLLS; i++) {
        uint8_t status = inb(CONTROL);
        if (status == 0xff) break;
        if (!(status & IBF)) return true;
        if (idt_get_monotonic_ns() - start >= COMMAND_NS) break;
        __asm__ volatile ("pause");
    }
    stats.timeouts++;
    return false;
}

static bool controller_command(uint8_t byte)
{
    if (!wait_write()) return false;
    outb(CONTROL, byte);
    return true;
}

static bool controller_config(uint8_t byte)
{
    if (!controller_command(0x60) || !wait_write()) return false;
    outb(DATA, byte);
    return wait_write();
}

static bool response(bool aux, uint8_t *byte, uint64_t timeout)
{
    uint64_t start = idt_get_monotonic_ns();
    for (unsigned i = 0; i < WAIT_POLLS; i++) {
        uint8_t status = inb(CONTROL);
        if (status == 0xff) break;
        if (status & OBF) {
            uint8_t data = inb(DATA);
            if (status & RX_ERROR) {
                stats.bad_bytes++;
                if (!!(status & AUX) == aux) return false;
                continue;
            }
            if (!!(status & AUX) == aux) {
                *byte = data;
                return true;
            }
            /* Keyboard input can arrive while an AUX command is pending. */
            if (!(status & AUX)) kb_handle_hw_byte(data);
        }
        if (idt_get_monotonic_ns() - start >= timeout) break;
        __asm__ volatile ("pause");
    }
    stats.timeouts++;
    return false;
}

static bool mouse_command(uint8_t byte)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        uint8_t ack;
        if (!controller_command(0xd4) || !wait_write()) return false;
        outb(DATA, byte);
        if (!response(true, &ack, COMMAND_NS)) return false;
        if (ack == 0xfa) return true;
        if (ack != 0xfe) return false;
        stats.resends++;
    }
    return false;
}

static bool mouse_parameter(uint8_t command, uint8_t value)
{
    return mouse_command(command) && mouse_command(value);
}

static bool synaptics_special(uint8_t value)
{
    if (!mouse_command(0xe6)) return false;
    for (int shift = 6; shift >= 0; shift -= 2)
        if (!mouse_parameter(0xe8, (value >> shift) & 3)) return false;
    return true;
}

static bool initialize_mouse(void)
{
    uint8_t bat, id, identify[3];
    stats.status = "AUX reset/ACK failed (absent or unsupported transport)";
    if (!mouse_command(0xff)) return false;
    stats.status = "AUX reset self-test failed";
    if (!response(true, &bat, 2000000000ULL) || bat != 0xaa ||
        !response(true, &id, COMMAND_NS)) return false;

    stats.status = "AUX defaults/disable failed";
    if (!mouse_command(0xf6) || !mouse_command(0xf5)) return false;
    stats.status = "AUX identify failed";
    if (!synaptics_special(0) || !mouse_command(0xe9)) return false;
    for (unsigned i = 0; i < 3; i++)
        if (!response(true, &identify[i], COMMAND_NS)) return false;
    if (identify[1] == 0x47) {
        stats.synaptics = true;
        stats.identity = ((uint32_t)identify[0] << 16) |
                         ((uint32_t)identify[1] << 8) | identify[2];
        /* Query signature first; never send vendor mode commands blindly.
         * Mode 0 selects relative packets, wakes the pad and enables gestures. */
        stats.status = "Synaptics relative-mode negotiation failed";
        if (!synaptics_special(0) || !mouse_parameter(0xf3, 0x14)) return false;
    }

    stats.status = "AUX stream configuration failed";
    if (!mouse_command(0xe6) || !mouse_parameter(0xe8, 2) ||
        !mouse_parameter(0xf3, 100) || !mouse_command(0xea) ||
        !mouse_command(0xf2) || !response(true, &id, COMMAND_NS)) return false;
    stats.device_id = id;
    stats.status = "unsupported PS/2 packet ID";
    if (id != 0 && id != 3 && id != 4) return false;
    stats.packet_size = id ? 4 : 3;
    stats.status = "AUX enable reporting failed";
    if (!mouse_command(0xf4)) return false;
    return true;
}

void ps2_init(void)
{
    if (initialized) return;
    initialized = true;
    uint64_t flags = irq_save();
    uint8_t master = inb(0x21), slave = inb(0xa1), config = 0;
    bool config_read = false;
    outb(0x21, master | 2);
    outb(0xa1, slave | 16);

    stats.status = "i8042 unavailable";
    if (inb(CONTROL) == 0xff) goto done;
    stats.controller = true;
    stats.status = "i8042 command-byte read failed";
    if (!controller_command(0xad) || !controller_command(0xa7)) goto restore;
    /* With both clocks stopped, stale bytes cannot be confused with the
     * controller's non-AUX command-byte response. Keep keyboard bytes. */
    for (unsigned i = 0; i < 64; i++) {
        uint8_t status = inb(CONTROL);
        if (status == 0xff || !(status & OBF)) break;
        uint8_t byte = inb(DATA);
        if (!(status & (AUX | RX_ERROR))) kb_handle_hw_byte(byte);
    }
    if (!controller_command(0x20) ||
        !response(false, &config, COMMAND_NS)) goto restore;
    config_read = true;
    /* Preserve translation/system bits. Keep IRQ12 disabled during commands. */
    config = (config | 1) & ~(0x10 | 0x20 | 2);
    stats.status = "i8042 AUX enable failed";
    if (!controller_config(config) || !controller_command(0xae) ||
        !controller_command(0xa8)) goto restore;
    if (!initialize_mouse()) goto restore;
    stats.status = "i8042 IRQ12 enable failed";
    if (!controller_config(config | 2)) goto restore;
    stats.ready = true;
    stats.status = stats.synaptics ? "Synaptics PS/2 relative" : "PS/2 relative";

restore:
    if (!stats.ready) {
        /* A missing AUX must not disable the working keyboard or its IRQ. */
        if (config_read && !controller_config((config | 0x21) & ~0x12))
            stats.status = "i8042 keyboard restore failed";
        (void)controller_command(0xa7);
    }
    (void)controller_command(0xae);
done:
    outb(0xa1, stats.ready ? (slave & ~16) : (slave | 16));
    outb(0x21, stats.ready ? (master & ~6) : (master & ~2));
    irq_restore(flags);
    serial_puts("[PS2] "); serial_puts(stats.status);
    if (stats.ready) {
        serial_puts(" id="); serial_puthex(stats.device_id, 2);
        if (stats.synaptics) {
            serial_puts(" identity="); serial_puthex(stats.identity, 6);
        }
        serial_puts(" IRQ12=0x7C");
    }
    serial_puts("\n");
}

static void mouse_byte(uint8_t byte)
{
    stats.aux_bytes++;
    if (!stats.ready) return;
    uint64_t now = idt_get_monotonic_ns();
    if (packet_pos && now - packet_time > PACKET_GAP_NS) {
        packet_pos = 0;
        stats.resyncs++;
    }
    packet_time = now;
    if (!packet_pos && !(byte & 8)) { stats.resyncs++; return; }
    packet[packet_pos++] = byte;
    if (packet_pos < stats.packet_size) return;
    packet_pos = 0;

    mouse_packet_t event = {0};
    event.buttons = packet[0] & 7;
    /* The sign bits form 9-bit values; casting the data bytes to int8_t
     * loses valid positive deltas 128..255 and negative deltas -256..-129. */
    if (!(packet[0] & 0xc0)) {
        event.dx = (int16_t)packet[1] - ((packet[0] & 0x10) ? 256 : 0);
        event.dy = -((int16_t)packet[2] - ((packet[0] & 0x20) ? 256 : 0));
    }
    if (stats.device_id == 3) event.wheel = -(int16_t)(int8_t)packet[3];
    if (stats.device_id == 4) {
        int16_t wheel = packet[3] & 15;
        if (wheel & 8) wheel -= 16;
        event.wheel = -wheel;
    }
    if (head - tail == QUEUE_SIZE) { tail++; stats.dropped++; }
    queue[head++ % QUEUE_SIZE] = event;
    stats.packets++;
}

/* Called by either IRQ and by the bounded polling fallback. Both paths share
 * the controller lock so one CPU cannot consume the other source's byte. */
void ps2_irq(void)
{
    uint64_t flags = irq_save();
    if (__atomic_test_and_set(&port_lock, __ATOMIC_ACQUIRE)) {
        irq_restore(flags);
        return;
    }
    for (unsigned i = 0; i < 64; i++) {
        uint8_t status = inb(CONTROL);
        if (status == 0xff || !(status & OBF)) break;
        uint8_t byte = inb(DATA);
        if (status & RX_ERROR) {
            stats.bad_bytes++;
            if (status & AUX) packet_pos = 0;
        } else if (status & AUX) {
            mouse_byte(byte);
        } else {
            kb_handle_hw_byte(byte);
        }
    }
    __atomic_clear(&port_lock, __ATOMIC_RELEASE);
    irq_restore(flags);
}

void ps2_poll(void)
{
    if (__atomic_test_and_set(&poll_lock, __ATOMIC_ACQUIRE)) return;
    ps2_irq();
    for (unsigned i = 0; i < QUEUE_SIZE; i++) {
        uint64_t flags = irq_save();
        if (__atomic_test_and_set(&port_lock, __ATOMIC_ACQUIRE)) {
            irq_restore(flags);
            break;
        }
        bool available = head != tail;
        mouse_packet_t event = {0};
        if (available) event = queue[tail++ % QUEUE_SIZE];
        __atomic_clear(&port_lock, __ATOMIC_RELEASE);
        if (!available) { irq_restore(flags); break; }
        if (event.dx || event.dy) input_post_mouse_move(event.dx, event.dy);
        input_post_mouse_button(event.buttons);
        if (event.wheel) input_post_mouse_wheel(event.wheel);
        int32_t x, y;
        input_get_cursor(&x, &y);
        stats.delivered++;
        irq_restore(flags);
        if (win32_post_mouse_screen)
            win32_post_mouse_screen(x, y, event.dx, event.dy,
                                    event.buttons, (short)(event.wheel * 120));
    }
    __atomic_clear(&poll_lock, __ATOMIC_RELEASE);
}

void ps2_aux_irq(void)
{
    stats.irq12++;
    ps2_irq();
}

void ps2_get_stats(ps2_stats_t *out)
{
    if (!out) return;
    uint64_t flags = irq_save();
    *out = stats;
    irq_restore(flags);
}
