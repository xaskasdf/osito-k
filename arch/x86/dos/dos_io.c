/*
 * OsitoK — DOS I/O Port Emulation
 *
 * Handles IN/OUT instructions from 16-bit DOS code.
 * Phase 3: VGA CRT controller, keyboard controller, PIT timer.
 * Phase 1: Stubs that return safe defaults.
 */

#include "dos_types.h"

static uint8_t vga_status_toggle = 0;
static uint16_t pit_counter = 0;

/* ── Port read (IN) ─────────────────────────────────────────────── */

uint8_t dos_io_read8(dos_vm_t *vm, uint16_t port)
{
    (void)vm;

    switch (port) {

    /* VGA Input Status Register 1 — many DOS programs poll this for vsync */
    case 0x3DA:
        vga_status_toggle ^= 0x09;  /* toggle bits 0 (retrace) and 3 (vretrace) */
        return vga_status_toggle;

    /* VGA CRT controller data */
    case 0x3D5:
        return 0;

    /* Keyboard data port */
    case 0x60:
        return 0;

    /* Keyboard status port */
    case 0x64:
        return 0x00;  /* no data available */

    /* PIT channel 0 counter — return decrementing value for timing loops */
    case 0x40:
        pit_counter -= 100;
        return (uint8_t)(pit_counter & 0xFF);
    case 0x41:
        return (uint8_t)((pit_counter >> 8) & 0xFF);

    /* PIC1 */
    case 0x20:
    case 0x21:
        return 0;

    /* System control port B */
    case 0x61:
        return 0;

    default:
        return 0xFF;
    }
}

uint16_t dos_io_read16(dos_vm_t *vm, uint16_t port)
{
    return dos_io_read8(vm, port) | ((uint16_t)dos_io_read8(vm, port + 1) << 8);
}

/* ── Port write (OUT) ───────────────────────────────────────────── */

void dos_io_write8(dos_vm_t *vm, uint16_t port, uint8_t val)
{
    (void)vm;
    (void)val;

    switch (port) {

    /* VGA CRT controller index */
    case 0x3D4:
        break;

    /* VGA CRT controller data */
    case 0x3D5:
        break;

    /* PIT channel 0 */
    case 0x40:
        break;

    /* PIT mode/command */
    case 0x43:
        break;

    /* PIC1 command/data */
    case 0x20:
    case 0x21:
        break;

    /* PIC2 command/data */
    case 0xA0:
    case 0xA1:
        break;

    /* System control port B (speaker) */
    case 0x61:
        break;

    default:
        break;
    }
}

void dos_io_write16(dos_vm_t *vm, uint16_t port, uint16_t val)
{
    dos_io_write8(vm, port, (uint8_t)(val & 0xFF));
    dos_io_write8(vm, port + 1, (uint8_t)(val >> 8));
}
