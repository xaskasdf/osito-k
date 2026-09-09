/*
 * OsitoK - DOS virtual ISA I/O bus
 *
 * IN/OUT instructions are mediated here for both the interpreter and native
 * DPMI clients. Device state belongs to the DOS VM; no register or timing
 * cursor is shared between guests.
 */

#include "dos_types.h"
#include "dos_audio.h"
#include "dos_io.h"
#include "dos_hostmem.h"
#include "../include/input_events.h"

extern void serial_puts(const char *text);
extern void serial_puthex(uint64_t value, int digits);
extern void serial_putdec(uint64_t value);
extern void *kmalloc(uint64_t size);
extern void kfree(void *pointer);
extern uint64_t idt_get_monotonic_ns(void);
extern void dos_vga_dac_update(struct dos_vm *vm, uint8_t index,
                               uint8_t component, uint8_t value);
extern void dos_vga_mark_dirty(struct dos_vm *vm, uint32_t address);

#define DOS_PIT_FREQUENCY_HZ       1193182ULL
#define DOS_NS_PER_SECOND          1000000000ULL
#define DOS_IO_DIAGNOSTIC_PORTS    16U
#define DOS_KBD_QUEUE_CAPACITY      8U

typedef struct {
    uint16_t reload;
    uint16_t count_latch;
    uint16_t read_snapshot;
    uint64_t loaded_ns;
    uint64_t irq_period;
    uint8_t access;
    uint8_t mode;
    uint8_t write_lsb;
    uint8_t write_phase;
    uint8_t read_phase;
    uint8_t status_latch;
    bool count_latched;
    bool read_snapshot_valid;
    bool status_latched;
    bool null_count;
    bool bcd;
    bool gate;
    bool running;
} DOS_PIT_CHANNEL;

typedef struct {
    uint8_t vector_base;
    uint8_t mask;
    uint8_t irr;
    uint8_t isr;
    uint8_t init_step;
    bool expect_icw4;
    bool single;
    bool auto_eoi;
    bool read_isr;
} DOS_PIC;

typedef struct {
    uint8_t queue[DOS_KBD_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t command_byte;
    uint8_t output_port;
    uint8_t last_data;
    uint8_t expect;
    bool scanning;
    bool irq_level;
    uint64_t poll_ns;
    uint32_t host_bytes;
    uint32_t reads;
    uint32_t irqs;
} DOS_KBD_CONTROLLER;

typedef struct {
    uint16_t port;
    uint8_t directions;
} DOS_IO_DIAGNOSTIC;

typedef struct {
    DOS_PIT_CHANNEL pit[3];
    DOS_PIC pic[2];
    DOS_KBD_CONTROLLER keyboard;

    uint8_t crtc_index[2];
    uint8_t crtc[2][32];
    uint8_t sequencer_index;
    uint8_t sequencer[8];
    uint8_t graphics_index;
    uint8_t graphics[16];
    uint8_t attribute_index;
    uint8_t attribute[32];
    bool attribute_data_phase;
    bool attribute_palette_enabled;
    uint8_t misc_output;
    uint8_t feature_control;
    uint8_t pixel_mask;
    uint8_t *vga_memory;
    uint8_t vga_latch[4];

    uint8_t dac[256][3];
    uint8_t dac_read_index;
    uint8_t dac_write_index;
    uint8_t dac_read_component;
    uint8_t dac_write_component;
    bool dac_read_mode;

    uint8_t system_control_b;
    uint32_t diagnostic_flags;
    DOS_IO_DIAGNOSTIC diagnostics[DOS_IO_DIAGNOSTIC_PORTS];
    uint8_t diagnostic_count;
    bool diagnostic_saturated;
    bool test_mode;
} DOS_IO_STATE;

enum {
    DOS_IO_DIAG_READ = 1U,
    DOS_IO_DIAG_WRITE = 2U,
    DOS_IO_DIAG_SPEAKER = 1U << 0,
    DOS_IO_DIAG_KBD_COMMAND = 1U << 1,
    DOS_IO_DIAG_PIT_BCD = 1U << 2,
};

static DOS_IO_STATE *dos_io_state(dos_vm_t *vm)
{
    return vm ? (DOS_IO_STATE *)vm->io : NULL;
}

static const DOS_IO_STATE *dos_io_const_state(const dos_vm_t *vm)
{
    return vm ? (const DOS_IO_STATE *)vm->io : NULL;
}

static uint64_t dos_pit_elapsed_cycles(const DOS_PIT_CHANNEL *channel,
                                       uint64_t now_ns)
{
    if (!channel->running || !channel->gate || now_ns <= channel->loaded_ns)
        return 0;
    uint64_t elapsed_ns = now_ns - channel->loaded_ns;
    uint64_t seconds = elapsed_ns / DOS_NS_PER_SECOND;
    uint64_t remainder = elapsed_ns % DOS_NS_PER_SECOND;
    return seconds * DOS_PIT_FREQUENCY_HZ +
           (remainder * DOS_PIT_FREQUENCY_HZ) / DOS_NS_PER_SECOND;
}

static uint32_t dos_pit_period(const DOS_PIT_CHANNEL *channel)
{
    return channel->reload ? channel->reload : 65536U;
}

static uint16_t dos_pit_count_at(const DOS_PIT_CHANNEL *channel,
                                 uint64_t now_ns)
{
    uint32_t period = dos_pit_period(channel);
    uint64_t elapsed = dos_pit_elapsed_cycles(channel, now_ns);
    uint32_t remaining;

    switch (channel->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        remaining = elapsed >= period ? 0U : period - (uint32_t)elapsed;
        break;
    default:
        remaining = period - (uint32_t)(elapsed % period);
        break;
    }
    return remaining == 65536U ? 0U : (uint16_t)remaining;
}

static bool dos_pit_output_at(const DOS_PIT_CHANNEL *channel,
                              uint64_t now_ns)
{
    if (!channel->running || !channel->gate)
        return false;
    uint32_t period = dos_pit_period(channel);
    uint64_t elapsed = dos_pit_elapsed_cycles(channel, now_ns);

    switch (channel->mode) {
    case 0:
    case 1:
        return elapsed >= period;
    case 2:
        return (elapsed % period) != period - 1U;
    case 3:
        return (elapsed % period) < (period + 1U) / 2U;
    case 4:
    case 5:
        return elapsed != period;
    default:
        return false;
    }
}

static uint8_t dos_pit_status(const DOS_PIT_CHANNEL *channel,
                              uint64_t now_ns)
{
    return (dos_pit_output_at(channel, now_ns) ? 0x80U : 0U) |
           (channel->null_count ? 0x40U : 0U) |
           ((channel->access & 3U) << 4) |
           ((channel->mode & 7U) << 1) |
           (channel->bcd ? 1U : 0U);
}

static void dos_pit_latch_count(DOS_PIT_CHANNEL *channel, uint64_t now_ns)
{
    if (!channel->count_latched) {
        channel->count_latch = dos_pit_count_at(channel, now_ns);
        channel->count_latched = true;
        channel->read_phase = 0;
    }
}

static void dos_pit_latch_status(DOS_PIT_CHANNEL *channel, uint64_t now_ns)
{
    if (!channel->status_latched) {
        channel->status_latch = dos_pit_status(channel, now_ns);
        channel->status_latched = true;
    }
}

static void dos_pit_load(DOS_PIT_CHANNEL *channel, uint16_t reload,
                         uint64_t now_ns)
{
    channel->reload = reload;
    channel->loaded_ns = now_ns;
    channel->irq_period = 0;
    channel->running = true;
    channel->null_count = false;
    channel->count_latched = false;
    channel->read_snapshot_valid = false;
    channel->read_phase = 0;
}

static uint8_t dos_pit_read(DOS_PIT_CHANNEL *channel, uint64_t now_ns)
{
    if (channel->status_latched) {
        channel->status_latched = false;
        return channel->status_latch;
    }

    uint16_t count;
    if (channel->count_latched) {
        count = channel->count_latch;
    } else if (channel->access == 3U) {
        if (!channel->read_snapshot_valid) {
            channel->read_snapshot = dos_pit_count_at(channel, now_ns);
            channel->read_snapshot_valid = true;
        }
        count = channel->read_snapshot;
    } else {
        count = dos_pit_count_at(channel, now_ns);
    }

    if (channel->access == 2U) {
        channel->count_latched = false;
        return (uint8_t)(count >> 8);
    }
    if (channel->access != 3U) {
        channel->count_latched = false;
        return (uint8_t)count;
    }
    if (!channel->read_phase) {
        channel->read_phase = 1U;
        return (uint8_t)count;
    }

    channel->read_phase = 0;
    channel->count_latched = false;
    channel->read_snapshot_valid = false;
    return (uint8_t)(count >> 8);
}

static void dos_pit_write(DOS_PIT_CHANNEL *channel, uint8_t value,
                          uint64_t now_ns)
{
    if (channel->access == 1U) {
        dos_pit_load(channel, value, now_ns);
    } else if (channel->access == 2U) {
        dos_pit_load(channel, (uint16_t)value << 8, now_ns);
    } else if (channel->access == 3U) {
        if (!channel->write_phase) {
            channel->write_lsb = value;
            channel->write_phase = 1U;
        } else {
            dos_pit_load(channel,
                         channel->write_lsb | ((uint16_t)value << 8),
                         now_ns);
            channel->write_phase = 0;
        }
    }
}

static void dos_pit_control(DOS_IO_STATE *state, uint8_t value,
                            uint64_t now_ns)
{
    uint8_t selection = value >> 6;
    if (selection == 3U) {
        for (uint8_t index = 0; index < 3U; index++) {
            if (value & (1U << (index + 1U)))
                continue;
            if (!(value & 0x20U))
                dos_pit_latch_count(&state->pit[index], now_ns);
            if (!(value & 0x10U))
                dos_pit_latch_status(&state->pit[index], now_ns);
        }
        return;
    }

    DOS_PIT_CHANNEL *channel = &state->pit[selection];
    uint8_t access = (value >> 4) & 3U;
    if (!access) {
        dos_pit_latch_count(channel, now_ns);
        return;
    }

    uint8_t mode = (value >> 1) & 7U;
    if (mode >= 6U)
        mode -= 4U;
    channel->access = access;
    channel->mode = mode;
    channel->bcd = (value & 1U) != 0;
    if (channel->bcd &&
        !(state->diagnostic_flags & DOS_IO_DIAG_PIT_BCD)) {
        state->diagnostic_flags |= DOS_IO_DIAG_PIT_BCD;
        if (!state->test_mode)
            serial_puts("[DOS-IO] PIT BCD mode uses binary counter fallback\n");
    }
    channel->null_count = true;
    channel->write_phase = 0;
    channel->read_phase = 0;
    channel->count_latched = false;
    channel->read_snapshot_valid = false;
    channel->status_latched = false;
}

static int dos_pic_highest_bit(uint8_t value)
{
    for (int bit = 0; bit < 8; bit++)
        if (value & (1U << bit))
            return bit;
    return -1;
}

static uint8_t dos_pic_read_command(const DOS_PIC *pic)
{
    return pic->read_isr ? pic->isr : pic->irr;
}

static void dos_pic_write_command(DOS_PIC *pic, uint8_t value)
{
    if (value & 0x10U) {
        pic->init_step = 2U;
        pic->expect_icw4 = (value & 1U) != 0;
        pic->single = (value & 2U) != 0;
        pic->auto_eoi = false;
        pic->read_isr = false;
        pic->irr = 0;
        pic->isr = 0;
        pic->mask = 0;
        return;
    }

    if ((value & 0x18U) == 0x08U) {
        if (value & 0x02U)
            pic->read_isr = (value & 1U) != 0;
        return;
    }

    if (value & 0x20U) {
        int bit = (value & 0x40U) ? (int)(value & 7U)
                                  : dos_pic_highest_bit(pic->isr);
        if (bit >= 0)
            pic->isr &= (uint8_t)~(1U << bit);
    }
}

static void dos_pic_write_data(DOS_PIC *pic, uint8_t value)
{
    switch (pic->init_step) {
    case 2:
        pic->vector_base = value & 0xF8U;
        pic->init_step = pic->single ? (pic->expect_icw4 ? 4U : 0U) : 3U;
        break;
    case 3:
        pic->init_step = pic->expect_icw4 ? 4U : 0U;
        break;
    case 4:
        pic->auto_eoi = (value & 2U) != 0;
        pic->init_step = 0;
        break;
    default:
        pic->mask = value;
        break;
    }
}

static bool dos_keyboard_push(DOS_KBD_CONTROLLER *keyboard, uint8_t value)
{
    if (keyboard->count == DOS_KBD_QUEUE_CAPACITY)
        return false;
    keyboard->queue[keyboard->tail] = value;
    keyboard->tail = (keyboard->tail + 1U) % DOS_KBD_QUEUE_CAPACITY;
    keyboard->count++;
    return true;
}

static void dos_keyboard_update_irq(DOS_IO_STATE *state)
{
    DOS_KBD_CONTROLLER *keyboard = &state->keyboard;
    bool level = keyboard->count && (keyboard->command_byte & 1U) &&
                 !(keyboard->command_byte & 0x10U);
    if (level && !keyboard->irq_level) state->pic[0].irr |= 2U;
    keyboard->irq_level = level;
}

bool dos_io_keyboard_acquire(dos_vm_t *vm)
{
    return dos_io_state(vm) && input_keyboard_acquire(vm);
}

bool dos_io_keyboard_poll(dos_vm_t *vm)
{
    DOS_IO_STATE *state = dos_io_state(vm);
    if (!state) return false;
    DOS_KBD_CONTROLLER *keyboard = &state->keyboard;
    if (!state->test_mode && input_keyboard_is_owner(vm)) {
        /* Native DOS owns the CPU descriptors, so the compositor cannot
         * poll USB for us. The driver drain is bounded and re-entry guarded. */
        uint64_t now = idt_get_monotonic_ns();
        if (!keyboard->poll_ns || now - keyboard->poll_ns >= 1000000ULL) {
            extern void xhci_poll(void) __attribute__((weak));
            keyboard->poll_ns = now;
            if (xhci_poll) xhci_poll();
        }
        uint8_t byte;
        unsigned budget = 64;
        while (budget-- && keyboard->count < DOS_KBD_QUEUE_CAPACITY &&
               !(keyboard->command_byte & 0x10U) &&
               input_keyboard_read_set1(vm, &byte)) {
            keyboard->host_bytes++;
            if (keyboard->scanning) dos_keyboard_push(keyboard, byte);
        }
    }
    dos_keyboard_update_irq(state);
    return (state->pic[0].irr & 2U) != 0;
}

static uint8_t dos_keyboard_read_data(DOS_IO_STATE *state)
{
    DOS_KBD_CONTROLLER *keyboard = &state->keyboard;
    if (keyboard->count) {
        keyboard->irq_level = false;
        keyboard->last_data = keyboard->queue[keyboard->head];
        keyboard->head = (keyboard->head + 1U) % DOS_KBD_QUEUE_CAPACITY;
        keyboard->count--;
        keyboard->reads++;
        dos_keyboard_update_irq(state);
    }
    return keyboard->last_data;
}

static uint8_t dos_keyboard_status(const DOS_IO_STATE *state)
{
    bool output_full = state->keyboard.count != 0;
    return 0x04U | (output_full ? 1U : 0U);
}

static void dos_keyboard_write_command(DOS_IO_STATE *state, uint8_t value)
{
    DOS_KBD_CONTROLLER *keyboard = &state->keyboard;
    switch (value) {
    case 0x20:
        dos_keyboard_push(keyboard, keyboard->command_byte);
        break;
    case 0x60:
        keyboard->expect = 1U;
        break;
    case 0xAA:
        dos_keyboard_push(keyboard, 0x55U);
        break;
    case 0xAB:
        dos_keyboard_push(keyboard, 0x00U);
        break;
    case 0xAD:
        keyboard->command_byte |= 0x10U;
        break;
    case 0xAE:
        keyboard->command_byte &= (uint8_t)~0x10U;
        break;
    case 0xD0:
        dos_keyboard_push(keyboard, keyboard->output_port);
        break;
    case 0xD1:
        keyboard->expect = 2U;
        break;
    case 0xD2:
        keyboard->expect = 4U;
        break;
    default:
        if (!(state->diagnostic_flags & DOS_IO_DIAG_KBD_COMMAND)) {
            state->diagnostic_flags |= DOS_IO_DIAG_KBD_COMMAND;
            if (!state->test_mode) {
                serial_puts("[DOS-IO] unsupported 8042 command 0x");
                serial_puthex(value, 2);
                serial_puts(" ignored\n");
            }
        }
        break;
    }
}

static void dos_keyboard_write_data(DOS_IO_STATE *state, uint8_t value)
{
    DOS_KBD_CONTROLLER *keyboard = &state->keyboard;
    if (keyboard->expect == 1U) {
        keyboard->command_byte = value;
        keyboard->expect = 0;
        return;
    }
    if (keyboard->expect == 2U) {
        keyboard->output_port = value;
        keyboard->expect = 0;
        return;
    }
    if (keyboard->expect == 3U) {
        keyboard->expect = 0;
        dos_keyboard_push(keyboard, 0xFAU);
        return;
    }
    if (keyboard->expect == 4U) {
        keyboard->expect = 0;
        dos_keyboard_push(keyboard, value);
        return;
    }

    switch (value) {
    case 0xED:
    case 0xF3:
        dos_keyboard_push(keyboard, 0xFAU);
        keyboard->expect = 3U;
        break;
    case 0xEE:
        dos_keyboard_push(keyboard, 0xEEU);
        break;
    case 0xF2:
        dos_keyboard_push(keyboard, 0xFAU);
        dos_keyboard_push(keyboard, 0xABU);
        dos_keyboard_push(keyboard, 0x83U);
        break;
    case 0xF4:
        keyboard->scanning = true;
        dos_keyboard_push(keyboard, 0xFAU);
        break;
    case 0xF5:
        keyboard->scanning = false;
        dos_keyboard_push(keyboard, 0xFAU);
        break;
    case 0xF6:
        keyboard->scanning = true;
        dos_keyboard_push(keyboard, 0xFAU);
        break;
    case 0xFF:
        keyboard->scanning = false;
        dos_keyboard_push(keyboard, 0xFAU);
        dos_keyboard_push(keyboard, 0xAAU);
        break;
    default:
        dos_keyboard_push(keyboard, 0xFEU);
        if (!(state->diagnostic_flags & DOS_IO_DIAG_KBD_COMMAND)) {
            state->diagnostic_flags |= DOS_IO_DIAG_KBD_COMMAND;
            if (!state->test_mode) {
                serial_puts("[DOS-IO] unsupported keyboard command 0x");
                serial_puthex(value, 2);
                serial_puts(" -> RESEND\n");
            }
        }
        break;
    }
}

static uint16_t dos_vga_cursor_address(const dos_vm_t *vm)
{
    return (uint16_t)((uint16_t)vm->cursor_row * 80U + vm->cursor_col);
}

static uint8_t dos_vga_crtc_read(const dos_vm_t *vm,
                                 const DOS_IO_STATE *state, bool mono)
{
    uint8_t index = state->crtc_index[mono ? 1 : 0] & 0x1FU;
    uint16_t cursor = dos_vga_cursor_address(vm);
    switch (index) {
    case 0x0A: return vm->cursor_start;
    case 0x0B: return vm->cursor_end;
    case 0x0E: return (uint8_t)(cursor >> 8);
    case 0x0F: return (uint8_t)cursor;
    default: return state->crtc[mono ? 1 : 0][index];
    }
}

static void dos_vga_crtc_write(dos_vm_t *vm, DOS_IO_STATE *state, bool mono,
                               uint8_t value)
{
    uint8_t bank = mono ? 1U : 0U;
    uint8_t index = state->crtc_index[bank] & 0x1FU;
    if (index < 8u && (state->crtc[bank][0x11] & 0x80u)) {
        if (index != 7u) return;
        value = (state->crtc[bank][7] & ~0x10u) | (value & 0x10u);
    }
    state->crtc[bank][index] = value;
    dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
    if (index == 0x0AU) {
        vm->cursor_start = value & 0x3FU;
    } else if (index == 0x0BU) {
        vm->cursor_end = value & 0x1FU;
    } else if (index == 0x0EU || index == 0x0FU) {
        uint16_t cursor = dos_vga_cursor_address(vm);
        if (index == 0x0EU)
            cursor = ((uint16_t)value << 8) | (cursor & 0x00FFU);
        else
            cursor = (cursor & 0xFF00U) | value;
        state->crtc[bank][0x0E] = (uint8_t)(cursor >> 8);
        state->crtc[bank][0x0F] = (uint8_t)cursor;
        vm->cursor_row = (uint8_t)((cursor / 80U) % 25U);
        vm->cursor_col = (uint8_t)(cursor % 80U);
    }
}

static uint8_t dos_vga_status1(DOS_IO_STATE *state)
{
    uint64_t now_ns = idt_get_monotonic_ns();
    uint64_t frame_phase = now_ns % 14285714ULL;
    uint64_t line_phase = now_ns % 31746ULL;
    uint8_t status = frame_phase >= 12800000ULL ? 0x08U : 0U;
    if (line_phase >= 25400ULL || (status & 0x08U))
        status |= 0x01U;
    state->attribute_data_phase = false;
    return status;
}

static uint8_t dos_vga_dac_read(DOS_IO_STATE *state)
{
    uint8_t value = state->dac[state->dac_read_index]
                              [state->dac_read_component];
    if (++state->dac_read_component == 3U) {
        state->dac_read_component = 0;
        state->dac_read_index++;
    }
    return value;
}

static void dos_vga_dac_write(dos_vm_t *vm, DOS_IO_STATE *state,
                              uint8_t value)
{
    uint8_t index = state->dac_write_index;
    uint8_t component = state->dac_write_component;
    value &= 0x3FU;
    state->dac[index][component] = value;
    dos_vga_dac_update(vm, index, component, value);
    if (++state->dac_write_component == 3U) {
        state->dac_write_component = 0;
        state->dac_write_index++;
    }
}

static void dos_vga_default_palette(DOS_IO_STATE *state)
{
    static const uint8_t ega[16][3] = {
        { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 },
        { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 }, { 42, 42, 42 },
        { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
        { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
    };
    static const uint8_t level[6] = { 0, 12, 24, 36, 48, 63 };

    for (uint16_t i = 0; i < 16U; i++)
        for (uint8_t component = 0; component < 3U; component++)
            state->dac[i][component] = ega[i][component];
    for (uint16_t i = 16U; i < 232U; i++) {
        uint16_t cube = i - 16U;
        state->dac[i][0] = level[(cube / 36U) % 6U];
        state->dac[i][1] = level[(cube / 6U) % 6U];
        state->dac[i][2] = level[cube % 6U];
    }
    for (uint16_t i = 232U; i < 256U; i++) {
        uint8_t gray = (uint8_t)(((i - 232U) * 63U) / 23U);
        state->dac[i][0] = gray;
        state->dac[i][1] = gray;
        state->dac[i][2] = gray;
    }
}

void dos_io_vga_set_mode(dos_vm_t *vm, uint8_t mode, bool clear)
{
    DOS_IO_STATE *state = dos_io_state(vm);
    if (!state) return;
    if (mode == 0x13u) {
        static const uint8_t sequencer[5] = {3, 1, 15, 0, 14};
        static const uint8_t graphics[9] = {0, 0, 0, 0, 0, 0x40, 5, 15, 255};
        static const uint8_t crtc[25] = {
            0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
            0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF
        };
        memcpy(state->sequencer, sequencer, sizeof(sequencer));
        memcpy(state->graphics, graphics, sizeof(graphics));
        memcpy(state->crtc[0], crtc, sizeof(crtc));
        state->misc_output = 0x63;
        for (unsigned i = 0; i < 16; i++) state->attribute[i] = i;
        state->attribute[0x10] = 0x41;
        state->attribute[0x11] = 0;
        state->attribute[0x12] = 15;
        state->attribute[0x13] = 0;
        state->attribute[0x14] = 0;
        state->attribute_palette_enabled = true;
        state->attribute_data_phase = false;
        state->pixel_mask = 255;
        memset(state->vga_latch, 0, sizeof(state->vga_latch));
        if (clear) memset(state->vga_memory, 0, DOS_VGA_MEMORY_SIZE);
    }
    dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
}

static bool dos_vga_aperture_offset(const DOS_IO_STATE *state,
                                    uint32_t address, uint32_t *offset)
{
    static const uint32_t bases[4] = {0xA0000, 0xA0000, 0xB0000, 0xB8000};
    static const uint32_t sizes[4] = {0x20000, 0x10000, 0x8000, 0x8000};
    unsigned map = (state->graphics[6] >> 2) & 3;
    *offset = address - bases[map];
    return *offset < sizes[map];
}

/* Keep physical plane addresses. CRTC byte/word/dword addressing belongs
 * to scanout, not to CPU access, including when Chain-4 is toggled. */
static uint32_t dos_vga_plane_offset(const DOS_IO_STATE *state, uint32_t offset)
{
    if (state->sequencer[4] & 8u) offset &= ~3u;
    else if (state->graphics[6] & 2u)
        offset = (offset & ~1u) | ((offset >> 16) & 1u);
    return offset & 0xFFFFu;
}

uint8_t dos_io_vga_read_memory(dos_vm_t *vm, uint32_t address)
{
    DOS_IO_STATE *state = dos_io_state(vm);
    uint32_t offset;
    if (!state || !state->vga_memory ||
        !dos_vga_aperture_offset(state, address, &offset)) return 0xFF;
    unsigned plane = state->graphics[4] & 3u;
    if (state->sequencer[4] & 8u) plane = offset & 3u;
    else if (state->graphics[5] & 0x10u)
        plane = (plane & 2u) | (offset & 1u);
    uint32_t cell = dos_vga_plane_offset(state, offset) * 4u;
    for (unsigned p = 0; p < 4; p++)
        state->vga_latch[p] = state->vga_memory[cell + p];
    if (!(state->graphics[5] & 8u)) return state->vga_latch[plane];

    uint8_t match = 0xFF;
    for (unsigned p = 0; p < 4; p++) {
        if (!(state->graphics[7] & (1u << p))) continue;
        uint8_t compare = (state->graphics[2] & (1u << p)) ? 0xFF : 0;
        match &= (uint8_t)~(state->vga_latch[p] ^ compare);
    }
    return match;
}

void dos_io_vga_write_memory(dos_vm_t *vm, uint32_t address, uint8_t value)
{
    DOS_IO_STATE *state = dos_io_state(vm);
    uint32_t offset;
    if (!state || !state->vga_memory ||
        !dos_vga_aperture_offset(state, address, &offset)) return;
    unsigned planes = state->sequencer[2] & 15u;
    if (state->sequencer[4] & 8u) planes &= 1u << (offset & 3u);
    else if (!(state->sequencer[4] & 4u))
        planes &= (offset & 1u) ? 10u : 5u;
    uint32_t cell = dos_vga_plane_offset(state, offset) * 4u;
    unsigned mode = state->graphics[5] & 3u;
    unsigned rotate = state->graphics[3] & 7u;
    uint8_t rotated = (uint8_t)((value >> rotate) | (value << (8u - rotate)));
    uint8_t mask = state->graphics[8];
    if (mode == 3) mask &= rotated;

    for (unsigned p = 0; p < 4; p++) {
        if (!(planes & (1u << p))) continue;
        uint8_t latch = state->vga_latch[p];
        uint8_t data;
        if (mode == 1) {
            state->vga_memory[cell + p] = latch;
            continue;
        }
        if (mode == 2) data = (value & (1u << p)) ? 0xFF : 0;
        else if (mode == 3 || (state->graphics[1] & (1u << p)))
            data = (state->graphics[0] & (1u << p)) ? 0xFF : 0;
        else data = rotated;
        switch ((state->graphics[3] >> 3) & 3u) {
        case 1: data &= latch; break;
        case 2: data |= latch; break;
        case 3: data ^= latch; break;
        default: break;
        }
        state->vga_memory[cell + p] = (data & mask) | (latch & (uint8_t)~mask);
    }
    dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
}

static unsigned dos_vga_scan_repeat(const uint8_t *crtc)
{
    return ((crtc[9] & 31u) + 1u) << (crtc[9] >> 7);
}

bool dos_io_vga_geometry(const dos_vm_t *vm, uint32_t *width, uint32_t *height)
{
    const DOS_IO_STATE *state = dos_io_const_state(vm);
    if (!state || !width || !height || vm->vga_mode != 0x13 || vm->vbe_active)
        return false;
    const uint8_t *crtc = state->crtc[(state->misc_output & 1u) ? 0 : 1];
    *width = ((uint32_t)crtc[1] + 1u) * ((state->graphics[5] & 0x40u) ? 4u : 8u);
    unsigned lines = (crtc[0x12] | ((crtc[7] & 2u) << 7) |
                      ((crtc[7] & 0x40u) << 3)) + 1u;
    unsigned repeat = dos_vga_scan_repeat(crtc);
    *height = (lines + repeat - 1u) / repeat;
    return *width <= DOS_VGA_MAX_WIDTH && *height <= 1024u;
}

static uint16_t dos_vga_crtc_address(const uint8_t *crtc, uint32_t counter)
{
    if (crtc[0x14] & 0x40u)
        return (uint16_t)((counter << 2) | ((counter >> 12) & 3u));
    if (!(crtc[0x17] & 0x40u))
        return (uint16_t)((counter << 1) |
                          ((counter >> ((crtc[0x17] & 0x20u) ? 15 : 13)) & 1u));
    return (uint16_t)counter;
}

bool dos_io_vga_scanline(const dos_vm_t *vm, uint32_t y,
                         uint8_t *pixels, uint32_t capacity)
{
    uint32_t width, height;
    if (!pixels || !dos_io_vga_geometry(vm, &width, &height) ||
        y >= height || capacity < width) return false;
    const DOS_IO_STATE *state = dos_io_const_state(vm);
    const uint8_t *crtc = state->crtc[(state->misc_output & 1u) ? 0 : 1];
    if (!state->attribute_palette_enabled || (state->sequencer[1] & 0x20u)) {
        memset(pixels, 0, width);
        return true;
    }
    unsigned repeat = dos_vga_scan_repeat(crtc);
    unsigned raster = y * repeat;
    unsigned split = crtc[0x18] | ((crtc[7] & 0x10u) << 4) |
                     ((crtc[9] & 0x40u) << 3);
    unsigned start = ((unsigned)crtc[0x0C] << 8) | crtc[0x0D];
    unsigned panning = state->attribute[0x13] & 7u;
    if (raster > split) {
        raster -= split + 1u;
        start = 0;
        if (state->attribute[0x10] & 0x20u) panning = 0;
    } else raster += crtc[8] & 31u;
    unsigned row = raster / repeat;
    unsigned row_scan = raster % repeat;
    unsigned counter = start + row * 2u * crtc[0x13] + ((crtc[8] >> 5) & 3u);
    bool indexed = (state->graphics[5] & 0x40u) != 0;
    if (indexed) panning >>= 1;
    unsigned divisor = (crtc[0x14] & 0x20u) ? 4u : (crtc[0x17] & 8u) ? 2u : 1u;
    for (unsigned x = 0; x < width; x++) {
        unsigned pel = x + panning;
        unsigned address = dos_vga_crtc_address(crtc, counter +
                              (pel / (indexed ? 4u : 8u)) / divisor);
        if (!(crtc[0x17] & 1u)) address = (address & ~0x2000u) | ((row_scan & 1u) << 13);
        if (!(crtc[0x17] & 2u)) address = (address & ~0x4000u) | ((row_scan & 2u) << 13);
        uint8_t index;
        if (indexed) index = state->vga_memory[address * 4u + (pel & 3u)];
        else {
            unsigned color = 0;
            for (unsigned p = 0; p < 4; p++)
                if ((state->attribute[0x12] & (1u << p)) &&
                    (state->vga_memory[address * 4u + p] & (0x80u >> (pel & 7u))))
                    color |= 1u << p;
            index = state->attribute[color] & 0x3Fu;
            if (state->attribute[0x10] & 0x80u)
                index = (index & 15u) | ((state->attribute[0x14] & 3u) << 4);
            index |= (state->attribute[0x14] & 12u) << 4;
        }
        pixels[x] = index & state->pixel_mask;
    }
    return true;
}

static void dos_io_report_unknown(DOS_IO_STATE *state, uint16_t port,
                                  uint8_t direction)
{
    if (!state || state->test_mode)
        return;
    for (uint8_t i = 0; i < state->diagnostic_count; i++) {
        if (state->diagnostics[i].port != port)
            continue;
        if (state->diagnostics[i].directions & direction)
            return;
        state->diagnostics[i].directions |= direction;
        goto report;
    }
    if (state->diagnostic_count == DOS_IO_DIAGNOSTIC_PORTS) {
        if (!state->diagnostic_saturated) {
            state->diagnostic_saturated = true;
            serial_puts("[DOS-IO] further unsupported port diagnostics suppressed\n");
        }
        return;
    }
    state->diagnostics[state->diagnostic_count].port = port;
    state->diagnostics[state->diagnostic_count].directions = direction;
    state->diagnostic_count++;

report:
    serial_puts(direction == DOS_IO_DIAG_READ
                    ? "[DOS-IO] unsupported IN  port 0x"
                    : "[DOS-IO] unsupported OUT port 0x");
    serial_puthex(port, 4);
    serial_puts(direction == DOS_IO_DIAG_READ
                    ? " -> open bus (0xFF)\n"
                    : " ignored\n");
}

bool dos_io_init(struct dos_vm *vm)
{
    if (!vm || vm->io)
        return false;
    DOS_IO_STATE *state = (DOS_IO_STATE *)kmalloc(sizeof(*state));
    if (!state)
        return false;
    memset(state, 0, sizeof(*state));
    state->vga_memory = dos_host_alloc_pages(DOS_VGA_MEMORY_SIZE / 4096u);
    if (!state->vga_memory) {
        kfree(state);
        return false;
    }
    memset(state->vga_memory, 0, DOS_VGA_MEMORY_SIZE);

    uint64_t now_ns = idt_get_monotonic_ns();
    for (uint8_t i = 0; i < 3U; i++) {
        state->pit[i].access = 3U;
        state->pit[i].mode = i == 1U ? 2U : 3U;
        state->pit[i].reload = 0;
        state->pit[i].loaded_ns = now_ns;
        state->pit[i].gate = i != 2U;
        state->pit[i].running = true;
    }
    state->pic[0].vector_base = 0x08U;
    state->pic[1].vector_base = 0x70U;
    state->keyboard.command_byte = 0x45U;
    state->keyboard.output_port = 0x03U;
    state->keyboard.scanning = true;
    state->misc_output = 0x01U;
    state->pixel_mask = 0xFFU;
    state->attribute_palette_enabled = true;
    state->crtc[0][0x0A] = vm->cursor_start;
    state->crtc[0][0x0B] = vm->cursor_end;
    state->crtc[1][0x0A] = vm->cursor_start;
    state->crtc[1][0x0B] = vm->cursor_end;
    dos_vga_default_palette(state);
    vm->io = state;
    return true;
}

void dos_io_shutdown(struct dos_vm *vm)
{
    if (!vm || !vm->io)
        return;
    DOS_IO_STATE *state = (DOS_IO_STATE *)vm->io;
    input_keyboard_release(vm);
    if (!state->test_mode && (state->keyboard.host_bytes || state->keyboard.irqs)) {
        serial_puts("[DOS-KBD] host bytes=");
        serial_putdec(state->keyboard.host_bytes);
        serial_puts(" data reads=");
        serial_putdec(state->keyboard.reads);
        serial_puts(" IRQ1 deliveries=");
        serial_putdec(state->keyboard.irqs);
        serial_puts("\n");
    }
    vm->io = NULL;
    dos_host_free_pages(state->vga_memory, DOS_VGA_MEMORY_SIZE / 4096u);
    kfree(state);
}

uint8_t dos_io_read8(dos_vm_t *vm, uint16_t port)
{
    uint8_t value = 0xFFU;
    if (dos_audio_port_read8(vm, port, &value))
        return value;

    DOS_IO_STATE *state = dos_io_state(vm);
    if (!state)
        return 0xFFU;
    uint64_t now_ns;

    switch (port) {
    case 0x20: return dos_pic_read_command(&state->pic[0]);
    case 0x21: return state->pic[0].mask;
    case 0xA0: return dos_pic_read_command(&state->pic[1]);
    case 0xA1: return state->pic[1].mask;

    case 0x40:
    case 0x41:
    case 0x42:
        return dos_pit_read(&state->pit[port - 0x40U],
                            idt_get_monotonic_ns());

    case 0x60:
        (void)dos_io_keyboard_poll(vm);
        return dos_keyboard_read_data(state);
    case 0x64:
        (void)dos_io_keyboard_poll(vm);
        return dos_keyboard_status(state);
    case 0x61:
        now_ns = idt_get_monotonic_ns();
        return (state->system_control_b & 0x0FU) |
               (((now_ns / 15000ULL) & 1U) ? 0x10U : 0U) |
               (dos_pit_output_at(&state->pit[2], now_ns) ? 0x20U : 0U);

    case 0x3B4: return state->crtc_index[1];
    case 0x3B5: return dos_vga_crtc_read(vm, state, true);
    case 0x3BA: return dos_vga_status1(state);
    case 0x3C0:
        return state->attribute_index |
               (state->attribute_palette_enabled ? 0x20U : 0U);
    case 0x3C1: return state->attribute[state->attribute_index & 0x1FU];
    case 0x3C2: return state->dac_read_mode ? 0x03U : 0U;
    case 0x3C4: return state->sequencer_index;
    case 0x3C5: return state->sequencer[state->sequencer_index & 7U];
    case 0x3C6: return state->pixel_mask;
    case 0x3C7: return state->dac_read_mode ? 0x03U : 0U;
    case 0x3C8: return state->dac_write_index;
    case 0x3C9: return dos_vga_dac_read(state);
    case 0x3CA: return state->feature_control;
    case 0x3CC: return state->misc_output;
    case 0x3CE: return state->graphics_index;
    case 0x3CF: return state->graphics[state->graphics_index & 0x0FU];
    case 0x3D4: return state->crtc_index[0];
    case 0x3D5: return dos_vga_crtc_read(vm, state, false);
    case 0x3DA: return dos_vga_status1(state);

    case 0x80:
        return 0xFFU;
    default:
        dos_io_report_unknown(state, port, DOS_IO_DIAG_READ);
        return 0xFFU;
    }
}

uint16_t dos_io_read16(dos_vm_t *vm, uint16_t port)
{
    return dos_io_read8(vm, port) |
           ((uint16_t)dos_io_read8(vm, port + 1U) << 8);
}

uint32_t dos_io_read32(dos_vm_t *vm, uint16_t port)
{
    return dos_io_read16(vm, port) |
           ((uint32_t)dos_io_read16(vm, port + 2U) << 16);
}

void dos_io_write8(dos_vm_t *vm, uint16_t port, uint8_t value)
{
    if (dos_audio_port_write8(vm, port, value))
        return;

    DOS_IO_STATE *state = dos_io_state(vm);
    if (!state)
        return;

    switch (port) {
    case 0x20: dos_pic_write_command(&state->pic[0], value); return;
    case 0x21: dos_pic_write_data(&state->pic[0], value); return;
    case 0xA0: dos_pic_write_command(&state->pic[1], value); return;
    case 0xA1: dos_pic_write_data(&state->pic[1], value); return;

    case 0x40:
    case 0x41:
    case 0x42:
        dos_pit_write(&state->pit[port - 0x40U], value,
                      idt_get_monotonic_ns());
        return;
    case 0x43:
        dos_pit_control(state, value, idt_get_monotonic_ns());
        return;

    case 0x60:
        dos_keyboard_write_data(state, value);
        dos_keyboard_update_irq(state);
        return;
    case 0x64:
        dos_keyboard_write_command(state, value);
        dos_keyboard_update_irq(state);
        return;
    case 0x61: {
        bool old_gate = state->pit[2].gate;
        state->system_control_b = value & 0x0FU;
        state->pit[2].gate = (value & 1U) != 0;
        if (state->pit[2].gate && !old_gate)
            state->pit[2].loaded_ns = idt_get_monotonic_ns();
        if (state->pit[2].gate && !old_gate)
            state->pit[2].irq_period = 0;
        if ((value & 2U) &&
            !(state->diagnostic_flags & DOS_IO_DIAG_SPEAKER)) {
            state->diagnostic_flags |= DOS_IO_DIAG_SPEAKER;
            if (!state->test_mode)
                serial_puts("[DOS-IO] PC speaker enabled; waveform output is unavailable\n");
        }
        return;
    }

    case 0x3B4: state->crtc_index[1] = value & 0x1FU; return;
    case 0x3B5: dos_vga_crtc_write(vm, state, true, value); return;
    case 0x3BA: state->feature_control = value; return;
    case 0x3C0:
        if (!state->attribute_data_phase) {
            state->attribute_index = value & 0x1FU;
            state->attribute_palette_enabled = (value & 0x20U) != 0;
        } else {
            state->attribute[state->attribute_index & 0x1FU] = value;
        }
        state->attribute_data_phase = !state->attribute_data_phase;
        dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
        return;
    case 0x3C2:
        state->misc_output = value;
        dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
        return;
    case 0x3C4: state->sequencer_index = value & 7U; return;
    case 0x3C5:
        state->sequencer[state->sequencer_index & 7U] = value;
        dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
        return;
    case 0x3C6:
        state->pixel_mask = value;
        dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
        return;
    case 0x3C7:
        state->dac_read_index = value;
        state->dac_read_component = 0;
        state->dac_read_mode = true;
        return;
    case 0x3C8:
        state->dac_write_index = value;
        state->dac_write_component = 0;
        state->dac_read_mode = false;
        return;
    case 0x3C9: dos_vga_dac_write(vm, state, value); return;
    case 0x3CE: state->graphics_index = value & 0x0FU; return;
    case 0x3CF:
        state->graphics[state->graphics_index & 0x0FU] = value;
        dos_vga_mark_dirty(vm, DOS_VGA_APERTURE_BASE);
        return;
    case 0x3D4: state->crtc_index[0] = value & 0x1FU; return;
    case 0x3D5: dos_vga_crtc_write(vm, state, false, value); return;
    case 0x3DA: state->feature_control = value; return;

    case 0x80:
        return;
    default:
        dos_io_report_unknown(state, port, DOS_IO_DIAG_WRITE);
        return;
    }
}

void dos_io_write16(dos_vm_t *vm, uint16_t port, uint16_t value)
{
    dos_io_write8(vm, port, (uint8_t)value);
    dos_io_write8(vm, port + 1U, (uint8_t)(value >> 8));
}

void dos_io_write32(dos_vm_t *vm, uint16_t port, uint32_t value)
{
    dos_io_write16(vm, port, (uint16_t)value);
    dos_io_write16(vm, port + 2U, (uint16_t)(value >> 16));
}

bool dos_io_irq_begin(struct dos_vm *vm, uint8_t irq, uint8_t *vector)
{
    if (!vector || irq >= 16U)
        return false;
    DOS_IO_STATE *state = dos_io_state(vm);
    if (!state) {
        *vector = irq < 8U ? (uint8_t)(0x08U + irq)
                           : (uint8_t)(0x70U + irq - 8U);
        return true;
    }

    uint8_t controller = irq >= 8U ? 1U : 0U;
    uint8_t line = irq & 7U;
    if ((state->pic[controller].mask & (1U << line)) ||
        (controller && (state->pic[0].mask & (1U << 2))))
        return false;

    DOS_PIC *pic = &state->pic[controller];
    /* Fully nested fixed priority: an in-service line blocks itself and
     * lower priorities until EOI, including the master's cascade line. */
    if ((pic->isr & ((1U << (line + 1U)) - 1U)) ||
        (controller && (state->pic[0].isr & 7U)))
        return false;
    pic->irr &= (uint8_t)~(1U << line);
    if (!pic->auto_eoi)
        pic->isr |= (uint8_t)(1U << line);
    if (controller && !state->pic[0].auto_eoi)
        state->pic[0].isr |= 1U << 2;
    *vector = (uint8_t)(pic->vector_base + line);
    if (irq == 1U) state->keyboard.irqs++;
    return true;
}

bool dos_io_timer_poll(struct dos_vm *vm)
{
    DOS_IO_STATE *state = dos_io_state(vm);
    if (!state)
        return false;
    DOS_PIT_CHANNEL *channel = &state->pit[0];
    if (!channel->running || !channel->gate)
        return false;

    uint64_t elapsed = dos_pit_elapsed_cycles(channel,
                                               idt_get_monotonic_ns());
    uint64_t period = dos_pit_period(channel);
    uint64_t completed;
    switch (channel->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        completed = elapsed >= period ? 1U : 0U;
        break;
    default:
        completed = elapsed / period;
        break;
    }
    if (completed <= channel->irq_period)
        return false;
    channel->irq_period = completed;
    return true;
}

void dos_io_get_pic_bases(const struct dos_vm *vm, uint8_t *master,
                          uint8_t *slave)
{
    const DOS_IO_STATE *state = dos_io_const_state(vm);
    if (master)
        *master = state ? state->pic[0].vector_base : 0x08U;
    if (slave)
        *slave = state ? state->pic[1].vector_base : 0x70U;
}

bool dos_io_copy_dac(const struct dos_vm *vm, uint8_t palette[256][3])
{
    const DOS_IO_STATE *state = dos_io_const_state(vm);
    if (!state || !palette)
        return false;
    memcpy(palette, state->dac, sizeof(state->dac));
    return true;
}

static int dos_keyboard_controller_selftest(dos_vm_t *vm, dos_vm_t *other)
{
    DOS_IO_STATE *s = dos_io_state(vm);
    uint32_t checks = 0;
    int failures = 0;
    uint8_t vector = 0, byte = 0;
#define KBD_CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    serial_puts("[DOS-KBD-TEST] FAIL: " #condition "\n"); } } while (0)
    s->pic[0] = (DOS_PIC){.vector_base = 8, .mask = 2};
    vm->kb_buffer[0] = 0x3062;
    vm->kb_head = 1;
    vm->kb_tail = 0;
    KBD_CHECK(!(dos_io_read8(vm, 0x64) & 1));
    (void)dos_io_read8(vm, 0x60);
    KBD_CHECK(vm->kb_head == 1 && vm->kb_tail == 0);
    dos_io_write8(vm, 0x64, 0xD2);
    dos_io_write8(vm, 0x60, 0x1E);
    KBD_CHECK(dos_io_keyboard_poll(vm) && (dos_io_read8(vm, 0x64) & 1));
    KBD_CHECK(!(dos_io_read8(other, 0x64) & 1));
    KBD_CHECK(!dos_io_irq_begin(vm, 1, &vector) && (s->pic[0].irr & 2));
    dos_io_write8(vm, 0x21, 0);
    KBD_CHECK(dos_io_irq_begin(vm, 1, &vector) && vector == 9);
    KBD_CHECK(!dos_io_irq_begin(vm, 1, &vector));
    KBD_CHECK(!dos_io_irq_begin(vm, 5, &vector));
    KBD_CHECK(dos_io_irq_begin(vm, 0, &vector) && vector == 8);
    dos_io_write8(vm, 0x20, 0x60);
    dos_io_write8(vm, 0x20, 0x61);
    KBD_CHECK(!dos_io_keyboard_poll(vm)); /* EOI alone must not repeat OBF. */
    KBD_CHECK(dos_io_read8(vm, 0x60) == 0x1E);
    KBD_CHECK(vm->kb_head == 1 && vm->kb_tail == 0);
    const uint8_t sequence[] = {0x9E,0xE0,0x48,0xE0,0xC8};
    for (unsigned i = 0; i < sizeof(sequence); i++) {
        dos_io_write8(vm, 0x64, 0xD2);
        dos_io_write8(vm, 0x60, sequence[i]);
    }
    for (unsigned i = 0; i < sizeof(sequence); i++) {
        KBD_CHECK(dos_io_keyboard_poll(vm));
        KBD_CHECK(dos_io_irq_begin(vm, 1, &vector) && vector == 9);
        KBD_CHECK(dos_io_read8(vm, 0x60) == sequence[i]);
        KBD_CHECK(!dos_io_irq_begin(vm, 1, &vector));
        dos_io_write8(vm, 0x20, 0x61);
    }
    KBD_CHECK(!dos_io_keyboard_poll(vm) && !(dos_io_read8(vm, 0x64) & 1));
    dos_io_write8(vm, 0x64, 0xAD);
    dos_io_write8(vm, 0x64, 0xD2);
    dos_io_write8(vm, 0x60, 0x30);
    KBD_CHECK(!dos_io_keyboard_poll(vm));
    dos_io_write8(vm, 0x64, 0xAE);
    KBD_CHECK(dos_io_keyboard_poll(vm));
    KBD_CHECK(dos_io_irq_begin(vm, 1, &vector));
    KBD_CHECK(dos_io_read8(vm, 0x60) == 0x30);
    dos_io_write8(vm, 0x20, 0x61);
    KBD_CHECK(input_keyboard_acquire(vm));
    if (input_keyboard_is_owner(vm)) {
        KBD_CHECK(!input_keyboard_acquire(other));
        input_keyboard_release(other);
        KBD_CHECK(input_keyboard_is_owner(vm));
        KBD_CHECK(input_keyboard_post_set1(sequence, sizeof(sequence)));
        KBD_CHECK(!input_keyboard_read_set1(other, &byte));
        for (unsigned i = 0; i < sizeof(sequence); i++)
            KBD_CHECK(input_keyboard_read_set1(vm, &byte) && byte == sequence[i]);
        KBD_CHECK(!input_keyboard_read_set1(vm, &byte));
        for (unsigned i = 0; i < 256; i++)
            KBD_CHECK(input_keyboard_post_set1(sequence, 1));
        KBD_CHECK(input_keyboard_post_set1(sequence, sizeof(sequence)));
        for (unsigned i = 0; i < 256; i++)
            KBD_CHECK(input_keyboard_read_set1(vm, &byte) && byte == sequence[0]);
        KBD_CHECK(input_keyboard_read_set1(vm, &byte) && byte == 0);
        KBD_CHECK(!input_keyboard_read_set1(vm, &byte));
        input_keyboard_release(vm);
        KBD_CHECK(!input_keyboard_post_set1(sequence, sizeof(sequence)));
    }
    vm->kb_head = vm->kb_tail = 0;
    serial_puts("[DOS-KBD-TEST] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#undef KBD_CHECK
    return failures;
}

static int dos_vga_memory_selftest(dos_vm_t *vm, dos_vm_t *other)
{
    DOS_IO_STATE *state = dos_io_state(vm);
    const uint32_t base = DOS_VGA_APERTURE_BASE;
    const uint8_t latches[4] = {0x96, 0x3C, 0xA5, 0x69};
    const uint8_t inputs[] = {0, 1, 5, 0x96, 0xFF};
    const uint8_t masks[] = {0, 0x55, 0x96, 0xFF};
    uint32_t checks = 0;
    int failures = 0;
#define VGA_CHECK(condition) do { checks++; if (!(condition)) failures++; } while (0)
    vm->vga_mode = other->vga_mode = 0x13;
    dos_io_vga_set_mode(vm, 0x13, true);
    dos_io_vga_set_mode(other, 0x13, true);
    dos_io_write16(vm, 0x3C4, 0x0604); /* sequential plane addresses */
    memcpy(state->vga_memory + 16u * 4u, latches, 4);
    (void)dos_io_vga_read_memory(vm, base + 16u);
    dos_io_write16(vm, 0x3CE, 0x0500); /* set/reset */
    dos_io_write16(vm, 0x3CE, 0x0A01); /* enable set/reset */
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned alu = 0; alu < 4; alu++)
    for (unsigned rotate = 0; rotate < 8; rotate++)
    for (unsigned map = 0; map < 16; map++)
    for (unsigned mask = 0; mask < sizeof(masks); mask++)
    for (unsigned input = 0; input < sizeof(inputs); input++) {
        dos_io_write16(vm, 0x3CE, (uint16_t)((0x40u | mode) << 8) | 5u);
        dos_io_write16(vm, 0x3CE, (uint16_t)((alu * 8u + rotate) << 8) | 3u);
        dos_io_write16(vm, 0x3CE, ((uint16_t)masks[mask] << 8) | 8u);
        dos_io_write16(vm, 0x3C4, (uint16_t)(map << 8) | 2u);
        memset(state->vga_memory + 32u * 4u, 0xCC, 4);
        dos_io_vga_write_memory(vm, base + 32u, inputs[input]);
        for (unsigned p = 0; p < 4; p++) {
            /* Independent bit-level reference for the VGA write pipeline. */
            uint8_t expected = 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                unsigned old = (latches[p] >> bit) & 1u;
                unsigned source = (inputs[input] >> ((bit + rotate) & 7u)) & 1u;
                unsigned enable = (masks[mask] >> bit) & 1u;
                if (mode == 3) enable &= source;
                if (mode == 2) source = (inputs[input] >> p) & 1u;
                else if (mode == 3 || (0xAu & (1u << p))) source = (5u >> p) & 1u;
                if (alu == 1) source &= old;
                else if (alu == 2) source |= old;
                else if (alu == 3) source ^= old;
                unsigned result = mode == 1 || !enable ? old : source;
                if (!(map & (1u << p))) result = (0xCCu >> bit) & 1u;
                expected |= (uint8_t)(result << bit);
            }
            VGA_CHECK(state->vga_memory[32u * 4u + p] == expected);
            VGA_CHECK(state->vga_latch[p] == latches[p]);
        }
    }
    for (unsigned care = 0; care < 16; care++)
    for (unsigned color = 0; color < 16; color++) {
        dos_io_write16(vm, 0x3CE, 0x4805);
        dos_io_write16(vm, 0x3CE, (uint16_t)(care << 8) | 7u);
        dos_io_write16(vm, 0x3CE, (uint16_t)(color << 8) | 2u);
        uint8_t expected = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned pixel = 0;
            for (unsigned p = 0; p < 4; p++) pixel |= ((latches[p] >> bit) & 1u) << p;
            if ((pixel & care) == (color & care)) expected |= 1u << bit;
        }
        VGA_CHECK(dos_io_vga_read_memory(vm, base + 16u) == expected);
    }

    dos_io_vga_set_mode(vm, 0x13, true);
    for (unsigned x = 0; x < 640; x++)
        dos_io_vga_write_memory(vm, base + x, (uint8_t)(x / 320u * 37u + x));
    uint8_t pixels[320];
    uint32_t width = 0, height = 0;
    VGA_CHECK(dos_io_vga_geometry(vm, &width, &height) && width == 320 && height == 200);
    VGA_CHECK(dos_io_vga_scanline(vm, 1, pixels, sizeof(pixels)));
    for (unsigned x = 0; x < 320; x++) VGA_CHECK(pixels[x] == (uint8_t)(x + 357u));
    VGA_CHECK(dos_io_vga_read_memory(other, base) == 0);
    VGA_CHECK(dos_io_vga_read_memory(vm, base + 0x10000) == 0xFF);
    /* Chain-4 must not reshuffle physical memory when it is disabled. */
    dos_io_write16(vm, 0x3C4, 0x0604);
    for (unsigned p = 0; p < 4; p++) {
        dos_io_write16(vm, 0x3CE, (uint16_t)(p << 8) | 4u);
        VGA_CHECK(dos_io_vga_read_memory(vm, base + 4u) == 4u + p);
        VGA_CHECK(dos_io_vga_read_memory(vm, base + 1u) == 0);
    }

    /* Mode X byte addressing and an independent start-address page flip. */
    dos_io_write16(vm, 0x3D4, 0x0014);
    dos_io_write16(vm, 0x3D4, 0xE317);
    for (unsigned p = 0; p < 4; p++) {
        dos_io_write16(vm, 0x3C4, (uint16_t)((1u << p) << 8) | 2u);
        for (unsigned i = 0; i < 80; i++)
            dos_io_vga_write_memory(vm, base + 0x4000u + i, (uint8_t)(4u * i + p));
    }
    dos_io_write16(vm, 0x3D4, 0x400C);
    dos_io_write16(vm, 0x3D4, 0x000D);
    uint8_t saved_latch[4];
    memcpy(saved_latch, state->vga_latch, 4);
    VGA_CHECK(dos_io_vga_scanline(vm, 0, pixels, sizeof(pixels)));
    for (unsigned x = 0; x < 320; x++) VGA_CHECK(pixels[x] == (uint8_t)x);
    VGA_CHECK(memcmp(saved_latch, state->vga_latch, 4) == 0);
    dos_io_write8(vm, 0x3C6, 15);
    VGA_CHECK(dos_io_vga_scanline(vm, 0, pixels, sizeof(pixels)));
    for (unsigned x = 0; x < 320; x++) VGA_CHECK(pixels[x] == (x & 15u));
    (void)dos_io_read8(vm, 0x3DA);
    dos_io_write8(vm, 0x3C0, 0x33);
    dos_io_write8(vm, 0x3C0, 2);
    VGA_CHECK(dos_io_vga_scanline(vm, 0, pixels, sizeof(pixels)) && pixels[0] == 1);
    (void)dos_io_read8(vm, 0x3DA);
    dos_io_write8(vm, 0x3C0, 0);
    VGA_CHECK(dos_io_vga_scanline(vm, 0, pixels, sizeof(pixels)) && pixels[1] == 0);

    /* Aperture selection, upper address wrap and odd/even plane pairing. */
    dos_io_vga_set_mode(vm, 0x13, true);
    dos_io_write16(vm, 0x3C4, 0x0604);
    dos_io_write16(vm, 0x3CE, 0x0106);
    dos_io_vga_write_memory(vm, base + 0x10000, 0x5A);
    VGA_CHECK(dos_io_vga_read_memory(vm, base) == 0x5A);
    dos_io_write16(vm, 0x3CE, 0x0D06);
    dos_io_vga_write_memory(vm, 0xB8000, 0xA5);
    VGA_CHECK(dos_io_vga_read_memory(vm, 0xB8000) == 0xA5);
    VGA_CHECK(dos_io_vga_read_memory(vm, base) == 0xFF);
    dos_io_write16(vm, 0x3CE, 0x0906);
    VGA_CHECK(dos_io_vga_read_memory(vm, 0xB0000) == 0xA5);
    VGA_CHECK(dos_io_vga_read_memory(vm, 0xB8000) == 0xFF);
    dos_io_write16(vm, 0x3CE, 0x0706);
    dos_io_write16(vm, 0x3C4, 0x0204);
    dos_io_write16(vm, 0x3CE, 0x1005);
    dos_io_vga_write_memory(vm, base, 0x12);
    dos_io_vga_write_memory(vm, base + 1, 0x34);
    VGA_CHECK(dos_io_vga_read_memory(vm, base) == 0x12);
    VGA_CHECK(dos_io_vga_read_memory(vm, base + 1) == 0x34);
    dos_io_write16(vm, 0x3CE, 0x0204);
    VGA_CHECK(dos_io_vga_read_memory(vm, base) == 0x12);
    VGA_CHECK(dos_io_vga_read_memory(vm, base + 1) == 0x34);
    dos_io_vga_set_mode(vm, 0x13, true);
    VGA_CHECK(dos_io_vga_read_memory(vm, base) == 0);
    serial_puts("[DOS-TEST] VGA checks=");
    serial_putdec(checks);
    serial_puts(" failures=");
    serial_putdec((uint32_t)failures);
    serial_puts("\n");
#undef VGA_CHECK
    return failures;
}

int dos_io_selftest(void)
{
    dos_vm_t first = {0};
    dos_vm_t second = {0};
    first.cursor_start = second.cursor_start = 6U;
    first.cursor_end = second.cursor_end = 7U;
    if (!dos_io_init(&first) || !dos_io_init(&second)) {
        dos_io_shutdown(&first);
        dos_io_shutdown(&second);
        return 1;
    }

    DOS_IO_STATE *a = dos_io_state(&first);
    DOS_IO_STATE *b = dos_io_state(&second);
    a->test_mode = true;
    b->test_mode = true;
    int failures = 0;

    dos_io_write8(&first, 0x3D4U, 0x12U);
    dos_io_write8(&first, 0x3D5U, 0xA5U);
    dos_io_write8(&second, 0x3D4U, 0x12U);
    if (dos_io_read8(&first, 0x3D5U) != 0xA5U ||
        dos_io_read8(&second, 0x3D5U) != 0U)
        failures++;

    dos_io_write8(&first, 0x43U, 0x34U);
    dos_io_write8(&first, 0x40U, 0x34U);
    dos_io_write8(&first, 0x40U, 0x12U);
    a->pit[0].loaded_ns = UINT64_MAX;
    dos_io_write8(&first, 0x43U, 0x00U);
    uint16_t latched = dos_io_read8(&first, 0x40U);
    latched |= (uint16_t)dos_io_read8(&first, 0x40U) << 8;
    if (a->pit[0].reload != 0x1234U || a->pit[0].mode != 2U ||
        latched != 0x1234U || b->pit[0].reload != 0U)
        failures++;
    a->pit[0].reload = 0;
    a->pit[0].loaded_ns = idt_get_monotonic_ns() - 60000000ULL;
    a->pit[0].irq_period = 0;
    if (!dos_io_timer_poll(&first) || a->pit[0].irq_period != 1U)
        failures++;

    dos_io_write8(&first, 0x20U, 0x11U);
    dos_io_write8(&first, 0x21U, 0x20U);
    dos_io_write8(&first, 0x21U, 0x04U);
    dos_io_write8(&first, 0x21U, 0x01U);
    dos_io_write8(&first, 0x21U, 0xFEU);
    uint8_t vector = 0;
    if (dos_io_read8(&first, 0x21U) != 0xFEU ||
        dos_io_read8(&second, 0x21U) != 0U ||
        !dos_io_irq_begin(&first, 0U, &vector) || vector != 0x20U ||
        dos_io_irq_begin(&first, 1U, &vector))
        failures++;
    uint8_t master_base = 0, slave_base = 0;
    dos_io_get_pic_bases(&first, &master_base, &slave_base);
    if (master_base != 0x20U || slave_base != 0x70U)
        failures++;

    dos_io_write8(&first, 0x61U, 0x03U);
    if ((dos_io_read8(&first, 0x61U) & 3U) != 3U ||
        (dos_io_read8(&second, 0x61U) & 3U) != 0U)
        failures++;

    dos_io_write8(&first, 0x3C8U, 7U);
    dos_io_write8(&first, 0x3C9U, 1U);
    dos_io_write8(&first, 0x3C9U, 2U);
    dos_io_write8(&first, 0x3C9U, 3U);
    dos_io_write8(&first, 0x3C7U, 7U);
    if (dos_io_read8(&first, 0x3C9U) != 1U ||
        dos_io_read8(&first, 0x3C9U) != 2U ||
        dos_io_read8(&first, 0x3C9U) != 3U ||
        b->dac[7][0] == 1U)
        failures++;

    dos_io_write8(&first, 0xDEADU, 0x55U);
    if (dos_io_read8(&first, 0xDEADU) != 0xFFU)
        failures++;

    failures += dos_keyboard_controller_selftest(&first, &second);
    failures += dos_vga_memory_selftest(&first, &second);
    dos_io_shutdown(&first);
    dos_io_shutdown(&second);
    if (first.io || second.io)
        failures++;
    return failures;
}
