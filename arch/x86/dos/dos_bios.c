/*
 * OsitoK — BIOS Interrupt Services for DOS Emulator
 *
 * INT 10h: Video services (text mode)
 * INT 16h: Keyboard services
 * INT 1Ah: Timer services
 *
 * Phase 1: Minimal stubs for basic operation.
 * Phase 3: Full VGA text mode via dos_vga.c.
 */

#include "cpu8086.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

/* Console interface */
extern void fb_putchar(char c);
extern int  kb_has_input(void);
extern char kb_getchar(void);

/* Kernel ticks (100 Hz) */
extern uint64_t idt_get_ticks(void);

/* ── INT 10h: Video Services ────────────────────────────────────── */

void dos_int10_video(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t ah = cpu->ah;

    switch (ah) {

    /* AH=00h: Set video mode */
    case 0x00:
        vm->vga_mode = cpu->al;
        /* Accept mode 3 (80x25 text), ignore others silently */
        if (cpu->al == 0x03) {
            vm->cursor_row = 0;
            vm->cursor_col = 0;
            vm->text_attr = 0x07;  /* light gray on black */
            /* Clear text buffer */
            for (uint32_t i = 0; i < 80 * 25 * 2; i += 2) {
                vm->mem[DOS_VRAM_BASE + i] = ' ';
                vm->mem[DOS_VRAM_BASE + i + 1] = 0x07;
            }
        }
        break;

    /* AH=01h: Set cursor shape */
    case 0x01:
        vm->cursor_start = cpu->ch & 0x1F;
        vm->cursor_end = cpu->cl & 0x1F;
        break;

    /* AH=02h: Set cursor position */
    case 0x02:
        vm->cursor_row = cpu->dh;
        vm->cursor_col = cpu->dl;
        /* Also update BDA */
        if (vm->cursor_row >= 25) vm->cursor_row = 24;
        if (vm->cursor_col >= 80) vm->cursor_col = 79;
        break;

    /* AH=03h: Get cursor position */
    case 0x03:
        cpu->dh = vm->cursor_row;
        cpu->dl = vm->cursor_col;
        cpu->ch = vm->cursor_start;
        cpu->cl = vm->cursor_end;
        break;

    /* AH=05h: Set active display page */
    case 0x05:
        vm->vga_page = cpu->al;
        break;

    /* AH=06h: Scroll up */
    case 0x06: {
        uint8_t lines = cpu->al;
        uint8_t attr  = cpu->bh;
        uint8_t r1 = cpu->ch, c1 = cpu->cl;
        uint8_t r2 = cpu->dh, c2 = cpu->dl;

        if (lines == 0) {
            /* Clear entire window */
            for (uint8_t r = r1; r <= r2 && r < 25; r++) {
                for (uint8_t c = c1; c <= c2 && c < 80; c++) {
                    uint32_t off = DOS_VRAM_BASE + (r * 80 + c) * 2;
                    vm->mem[off] = ' ';
                    vm->mem[off + 1] = attr;
                }
            }
        } else {
            /* Scroll up by 'lines' rows */
            for (uint8_t r = r1; r <= r2 && r < 25; r++) {
                for (uint8_t c = c1; c <= c2 && c < 80; c++) {
                    uint32_t dst = DOS_VRAM_BASE + (r * 80 + c) * 2;
                    if (r + lines <= r2) {
                        uint32_t src = DOS_VRAM_BASE + ((r + lines) * 80 + c) * 2;
                        vm->mem[dst] = vm->mem[src];
                        vm->mem[dst + 1] = vm->mem[src + 1];
                    } else {
                        vm->mem[dst] = ' ';
                        vm->mem[dst + 1] = attr;
                    }
                }
            }
        }
        break;
    }

    /* AH=07h: Scroll down */
    case 0x07: {
        uint8_t lines = cpu->al;
        uint8_t attr  = cpu->bh;
        uint8_t r1 = cpu->ch, c1 = cpu->cl;
        uint8_t r2 = cpu->dh, c2 = cpu->dl;

        if (lines == 0) {
            for (uint8_t r = r1; r <= r2 && r < 25; r++)
                for (uint8_t c = c1; c <= c2 && c < 80; c++) {
                    uint32_t off = DOS_VRAM_BASE + (r * 80 + c) * 2;
                    vm->mem[off] = ' ';
                    vm->mem[off + 1] = attr;
                }
        } else {
            for (int r = r2; r >= (int)r1; r--) {
                for (uint8_t c = c1; c <= c2 && c < 80; c++) {
                    uint32_t dst = DOS_VRAM_BASE + (r * 80 + c) * 2;
                    if (r - (int)lines >= (int)r1) {
                        uint32_t src = DOS_VRAM_BASE + ((r - lines) * 80 + c) * 2;
                        vm->mem[dst] = vm->mem[src];
                        vm->mem[dst + 1] = vm->mem[src + 1];
                    } else {
                        vm->mem[dst] = ' ';
                        vm->mem[dst + 1] = attr;
                    }
                }
            }
        }
        break;
    }

    /* AH=08h: Read character and attribute */
    case 0x08: {
        uint32_t off = DOS_VRAM_BASE + (vm->cursor_row * 80 + vm->cursor_col) * 2;
        cpu->al = vm->mem[off];       /* character */
        cpu->ah = vm->mem[off + 1];   /* attribute */
        break;
    }

    /* AH=09h: Write character and attribute */
    case 0x09: {
        uint16_t count = cpu->cx;
        uint8_t ch = cpu->al;
        uint8_t attr = cpu->bl;
        uint8_t row = vm->cursor_row;
        uint8_t col = vm->cursor_col;
        for (uint16_t i = 0; i < count; i++) {
            if (col >= 80) { col = 0; row++; }
            if (row >= 25) break;
            uint32_t off = DOS_VRAM_BASE + (row * 80 + col) * 2;
            vm->mem[off] = ch;
            vm->mem[off + 1] = attr;
            col++;
        }
        break;
    }

    /* AH=0Ah: Write character only (keep attribute) */
    case 0x0A: {
        uint16_t count = cpu->cx;
        uint8_t ch = cpu->al;
        uint8_t row = vm->cursor_row;
        uint8_t col = vm->cursor_col;
        for (uint16_t i = 0; i < count; i++) {
            if (col >= 80) { col = 0; row++; }
            if (row >= 25) break;
            uint32_t off = DOS_VRAM_BASE + (row * 80 + col) * 2;
            vm->mem[off] = ch;
            col++;
        }
        break;
    }

    /* AH=0Eh: Teletype output */
    case 0x0E: {
        uint8_t ch = cpu->al;
        if (ch == '\r') {
            vm->cursor_col = 0;
        } else if (ch == '\n') {
            vm->cursor_row++;
        } else if (ch == '\b') {
            if (vm->cursor_col > 0) vm->cursor_col--;
        } else if (ch == 7) {
            /* BEL: ignore */
        } else {
            uint32_t off = DOS_VRAM_BASE +
                           (vm->cursor_row * 80 + vm->cursor_col) * 2;
            vm->mem[off] = ch;
            vm->mem[off + 1] = vm->text_attr;
            vm->cursor_col++;
            if (vm->cursor_col >= 80) {
                vm->cursor_col = 0;
                vm->cursor_row++;
            }
        }
        /* Scroll if needed */
        if (vm->cursor_row >= 25) {
            vm->cursor_row = 24;
            /* Scroll up 1 line */
            for (int r = 0; r < 24; r++) {
                uint32_t dst = DOS_VRAM_BASE + r * 80 * 2;
                uint32_t src = DOS_VRAM_BASE + (r + 1) * 80 * 2;
                for (int c = 0; c < 160; c++)
                    vm->mem[dst + c] = vm->mem[src + c];
            }
            /* Clear bottom line */
            uint32_t last = DOS_VRAM_BASE + 24 * 80 * 2;
            for (int c = 0; c < 80; c++) {
                vm->mem[last + c * 2] = ' ';
                vm->mem[last + c * 2 + 1] = vm->text_attr;
            }
        }
        /* Also output to serial/framebuffer for Phase 1 */
        fb_putchar((char)ch);
        break;
    }

    /* AH=0Fh: Get video mode */
    case 0x0F:
        cpu->al = vm->vga_mode;   /* mode (3 = 80x25 text) */
        cpu->ah = 80;             /* columns */
        cpu->bh = vm->vga_page;   /* active page */
        break;

    /* AH=12h: Alternate function select */
    case 0x12:
        /* VGA: return EGA/VGA info */
        cpu->bx = 0x0003;  /* 256K video memory, color */
        cpu->cx = 0x0009;  /* feature bits */
        break;

    /* AH=1Ah: Get/set display combination code */
    case 0x1A:
        if (cpu->al == 0x00) {
            cpu->al = 0x1A;   /* function supported */
            cpu->bl = 0x08;   /* VGA color */
        }
        break;

    default:
        /* Silently ignore unknown video functions */
        break;
    }
}

/* ── INT 16h: Keyboard Services ─────────────────────────────────── */

void dos_int16_keyboard(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    switch (cpu->ah) {

    /* AH=00h/10h: Read key (blocking) */
    case 0x00:
    case 0x10: {
        while (!kb_has_input()) { /* spin */ }
        char ch = kb_getchar();
        cpu->al = (uint8_t)ch;  /* ASCII */
        cpu->ah = 0;            /* scancode (simplified) */
        break;
    }

    /* AH=01h/11h: Check key (non-blocking) */
    case 0x01:
    case 0x11:
        if (kb_has_input()) {
            cpu->flags &= ~FLAG_ZF;
            /* Peek: we'd need to peek without consuming.
             * For now, report key available but can't show which. */
            cpu->ax = 0x0020;  /* space as placeholder */
        } else {
            cpu->flags |= FLAG_ZF;
        }
        break;

    /* AH=02h/12h: Get shift flags */
    case 0x02:
    case 0x12:
        cpu->al = 0;  /* no modifiers */
        break;

    default:
        break;
    }
}

/* ── INT 1Ah: Timer Services ────────────────────────────────────── */

void dos_int1a_timer(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;

    /* Convert kernel ticks (100 Hz) to BIOS ticks (18.2 Hz) */
    uint64_t elapsed = idt_get_ticks() - vm->start_ticks;
    uint32_t bios_ticks = (uint32_t)(elapsed * 182 / 1000);
    vm->bios_ticks = bios_ticks;

    switch (cpu->ah) {

    /* AH=00h: Read system timer counter */
    case 0x00:
        cpu->cx = (uint16_t)(bios_ticks >> 16);
        cpu->dx = (uint16_t)(bios_ticks & 0xFFFF);
        cpu->al = 0;  /* midnight flag */
        break;

    /* AH=02h: Read RTC time (BCD) */
    case 0x02: {
        /* Approximate time from ticks (18.2 Hz, 65536 ticks/hour) */
        uint32_t secs = bios_ticks / 18;
        cpu->ch = (uint8_t)((secs / 3600) % 24);  /* hours (BCD simplified) */
        cpu->cl = (uint8_t)((secs / 60) % 60);    /* minutes */
        cpu->dh = (uint8_t)(secs % 60);            /* seconds */
        cpu->dl = 0;                               /* DST flag */
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* AH=04h: Read RTC date (BCD) */
    case 0x04:
        cpu->ch = 0x20;  /* century: 20 */
        cpu->cl = 0x26;  /* year: 26 */
        cpu->dh = 0x03;  /* month: March */
        cpu->dl = 0x31;  /* day: 31 */
        cpu->flags &= ~FLAG_CF;
        break;

    default:
        break;
    }
}
