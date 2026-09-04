/*
 * OsitoK — BIOS Interrupt Services for DOS Emulator
 *
 * INT 10h: Video services (text mode)
 * INT 16h: Keyboard services
 * INT 1Ah: Timer services
 *
 * Services are backed by the VM's virtual VGA, keyboard, and wall clock.
 */

#include "cpu8086.h"
#include "dos_mouse.h"
#include "dos_time.h"
#include "dos_vbe.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);

extern int  kb_has_input(void);
extern char kb_getchar(void);
extern uint8_t kb_get_bios_shift_flags(void);

/* Kernel ticks (100 Hz) */
extern uint64_t idt_get_ticks(void);
extern void dos_vga_set_mode(uint8_t mode);
extern void dos_vga_invalidate_text(dos_vm_t *vm);
extern void dos_vga_mark_dirty(dos_vm_t *vm, uint32_t addr);
extern void dos_vga_flush(dos_vm_t *vm);

#define DOS_TEXT_COLUMNS 80U
#define DOS_TEXT_ROWS    25U
#define DOS_TEXT_PAGE_SIZE 4096U
#define DOS_KEY_BUFFER_MASK 15U

static uint8_t dos_bcd(uint32_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static bool dos_bcd_decode(uint8_t value, uint8_t *decoded)
{
    uint8_t high = value >> 4;
    uint8_t low = value & 0x0F;
    if (!decoded || high > 9U || low > 9U) return false;
    *decoded = (uint8_t)(high * 10U + low);
    return true;
}

static uint8_t dos_ascii_scancode(uint8_t ascii)
{
    switch (ascii) {
    case 27: return 0x01;
    case '\b': return 0x0E;
    case '\t': return 0x0F;
    case '\r': case '\n': return 0x1C;
    case ' ': return 0x39;
    default: break;
    }

    if (ascii >= 1U && ascii <= 26U)
        ascii = (uint8_t)('a' + ascii - 1U);
    if (ascii >= 'A' && ascii <= 'Z')
        ascii = (uint8_t)(ascii - 'A' + 'a');

    static const char row1[] = "1234567890-=";
    static const char row1_shift[] = "!@#$%^&*()_+";
    static const char row2[] = "qwertyuiop[]";
    static const char row3[] = "asdfghjkl;'`";
    static const char row4[] = "\\zxcvbnm,./";
    for (uint8_t i = 0; i < sizeof(row1) - 1U; i++)
        if ((uint8_t)row1[i] == ascii) return (uint8_t)(0x02U + i);
    for (uint8_t i = 0; i < sizeof(row1_shift) - 1U; i++)
        if ((uint8_t)row1_shift[i] == ascii) return (uint8_t)(0x02U + i);
    for (uint8_t i = 0; i < sizeof(row2) - 1U; i++)
        if ((uint8_t)row2[i] == ascii) return (uint8_t)(0x10U + i);
    for (uint8_t i = 0; i < sizeof(row3) - 1U; i++)
        if ((uint8_t)row3[i] == ascii) return (uint8_t)(0x1EU + i);
    for (uint8_t i = 0; i < sizeof(row4) - 1U; i++)
        if ((uint8_t)row4[i] == ascii) return (uint8_t)(0x2BU + i);

    switch (ascii) {
    case '{': return 0x1A;
    case '}': return 0x1B;
    case ':': return 0x27;
    case '"': return 0x28;
    case '~': return 0x29;
    case '|': return 0x2B;
    case '<': return 0x33;
    case '>': return 0x34;
    case '?': return 0x35;
    default: return 0;
    }
}

static uint16_t dos_key_word(uint8_t ascii)
{
    return (uint16_t)(((uint16_t)dos_ascii_scancode(ascii) << 8) | ascii);
}

static bool dos_key_pending(dos_vm_t *vm)
{
    return vm->kb_head != vm->kb_tail;
}

static void dos_key_enqueue(dos_vm_t *vm, uint8_t ascii)
{
    uint8_t next = (uint8_t)((vm->kb_head + 1U) & DOS_KEY_BUFFER_MASK);
    if (next == vm->kb_tail) return;
    vm->kb_buffer[vm->kb_head] = dos_key_word(ascii);
    vm->kb_head = next;
}

static bool dos_key_pump(dos_vm_t *vm)
{
    if (dos_key_pending(vm)) return true;
    if (!kb_has_input()) return false;
    dos_key_enqueue(vm, (uint8_t)kb_getchar());
    return dos_key_pending(vm);
}

static uint16_t dos_key_pop(dos_vm_t *vm)
{
    uint16_t key = vm->kb_buffer[vm->kb_tail];
    vm->kb_tail = (uint8_t)((vm->kb_tail + 1U) & DOS_KEY_BUFFER_MASK);
    return key;
}

static uint32_t dos_text_addr(uint8_t page, uint8_t row, uint8_t col)
{
    return DOS_VRAM_BASE + (uint32_t)(page & 7U) * DOS_TEXT_PAGE_SIZE +
           ((uint32_t)row * DOS_TEXT_COLUMNS + col) * 2U;
}

static uint16_t dos_cursor_position(dos_vm_t *vm, uint8_t page)
{
    return dos_mem_read16(vm, 0x450U + (uint32_t)(page & 7U) * 2U);
}

static void dos_cursor_coordinates(dos_vm_t *vm, uint8_t page,
                                   uint8_t *row, uint8_t *col)
{
    uint16_t position = dos_cursor_position(vm, page);
    uint8_t cursor_row = (uint8_t)(position >> 8);
    uint8_t cursor_col = (uint8_t)position;
    *row = cursor_row < DOS_TEXT_ROWS ? cursor_row : DOS_TEXT_ROWS - 1U;
    *col = cursor_col < DOS_TEXT_COLUMNS ? cursor_col
                                         : DOS_TEXT_COLUMNS - 1U;
}

static void dos_text_write(dos_vm_t *vm, uint8_t page, uint8_t row,
                           uint8_t col, uint8_t ch, uint8_t attr,
                           bool write_attr)
{
    if (row >= DOS_TEXT_ROWS || col >= DOS_TEXT_COLUMNS) return;
    uint32_t addr = dos_text_addr(page, row, col);
    vm->mem[addr] = ch;
    if (write_attr) vm->mem[addr + 1U] = attr;
    dos_vga_mark_dirty(vm, addr);
}

static void dos_video_diag(dos_vm_t *vm, const char *message, uint8_t value)
{
    if (vm->video_diag_count++ >= 16U) return;
    serial_puts("[DOS/VGA] ");
    serial_puts(message);
    serial_puthex(value, 2);
    serial_puts("\n");
}

/* ── INT 10h: Video Services ────────────────────────────────────── */

void dos_int10_video(dos_vm_t *vm)
{
    cpu8086_state_t *cpu = vm->cpu;
    uint8_t ah = cpu->ah;

    switch (ah) {

    /* AH=00h: Set video mode */
    case 0x00: {
        uint8_t request = cpu->al;
        uint8_t mode = request & 0x7FU;
        if (mode != 0x03 && mode != 0x13) {
            dos_video_diag(vm, "unsupported mode ", mode);
            break;
        }

        dos_vbe_leave_mode(vm);
        vm->vga_mode = mode;
        vm->vga_page = 0;
        dos_vga_set_mode(mode);
        dos_mouse_video_mode_changed(vm);
        vm->mem[0x449] = mode;
        vm->mem[0x462] = 0;
        dos_mem_write16(vm, 0x44A, mode == 0x03 ? 80U : 40U);
        dos_mem_write16(vm, 0x44C, mode == 0x03 ? 4096U : 0xFA00U);
        vm->mem[0x484] = 24;
        dos_mem_write16(vm, 0x485, mode == 0x03 ? 16U : 8U);

        if (mode == 0x03) {
            vm->cursor_row = 0;
            vm->cursor_col = 0;
            vm->text_attr = 0x07;  /* light gray on black */
            if (!(request & 0x80U)) {
                for (uint8_t page = 0; page < 8U; page++) {
                    for (uint32_t cell = 0;
                         cell < DOS_TEXT_COLUMNS * DOS_TEXT_ROWS; cell++) {
                        uint32_t addr = DOS_VRAM_BASE +
                            (uint32_t)page * DOS_TEXT_PAGE_SIZE + cell * 2U;
                        vm->mem[addr] = ' ';
                        vm->mem[addr + 1U] = 0x07;
                    }
                }
            }
            for (uint8_t page = 0; page < 8U; page++)
                dos_mem_write16(vm, 0x450U + (uint32_t)page * 2U, 0);
            dos_vga_invalidate_text(vm);
            dos_vga_flush(vm);
        } else if (!(request & 0x80U)) {
            for (uint32_t i = 0; i < 320U * 200U; i++)
                vm->mem[0xA0000U + i] = 0;
        }
        break;
    }

    /* AH=01h: Set cursor shape */
    case 0x01:
        vm->cursor_start = cpu->ch & 0x3F;
        vm->cursor_end = cpu->cl & 0x1F;
        dos_mem_write16(vm, 0x460,
                        (uint16_t)(((uint16_t)vm->cursor_start << 8) |
                                   vm->cursor_end));
        dos_vga_invalidate_text(vm);
        dos_vga_flush(vm);
        break;

    /* AH=02h: Set cursor position */
    case 0x02: {
        uint8_t page = cpu->bh & 7U;
        uint8_t row = cpu->dh < DOS_TEXT_ROWS ? cpu->dh : DOS_TEXT_ROWS - 1U;
        uint8_t col = cpu->dl < DOS_TEXT_COLUMNS ? cpu->dl
                                                 : DOS_TEXT_COLUMNS - 1U;
        dos_mem_write16(vm, 0x450U + (uint32_t)page * 2U,
                        (uint16_t)(((uint16_t)row << 8) | col));
        if (page == (vm->vga_page & 7U)) {
            vm->cursor_row = row;
            vm->cursor_col = col;
            dos_vga_flush(vm);
        }
        break;
    }

    /* AH=03h: Get cursor position */
    case 0x03: {
        uint8_t page = cpu->bh & 7U;
        uint16_t pos = dos_cursor_position(vm, page);
        cpu->dh = (uint8_t)(pos >> 8);
        cpu->dl = (uint8_t)pos;
        cpu->ch = vm->cursor_start;
        cpu->cl = vm->cursor_end;
        break;
    }

    /* AH=05h: Set active display page */
    case 0x05: {
        uint8_t page = cpu->al & 7U;
        vm->vga_page = page;
        vm->mem[0x462] = page;
        dos_mem_write16(vm, 0x44E, (uint16_t)(page * DOS_TEXT_PAGE_SIZE));
        dos_cursor_coordinates(vm, page, &vm->cursor_row, &vm->cursor_col);
        dos_vga_invalidate_text(vm);
        dos_vga_flush(vm);
        break;
    }

    /* AH=06h: Scroll up */
    case 0x06: {
        uint8_t lines = cpu->al;
        uint8_t attr  = cpu->bh;
        uint8_t r1 = cpu->ch, c1 = cpu->cl;
        uint8_t r2 = cpu->dh, c2 = cpu->dl;
        uint8_t page = vm->vga_page & 7U;
        if (r1 >= DOS_TEXT_ROWS || c1 >= DOS_TEXT_COLUMNS) break;
        if (r2 >= DOS_TEXT_ROWS) r2 = DOS_TEXT_ROWS - 1U;
        if (c2 >= DOS_TEXT_COLUMNS) c2 = DOS_TEXT_COLUMNS - 1U;
        if (r1 > r2 || c1 > c2) break;

        if (lines == 0) {
            /* Clear entire window */
            for (uint8_t r = r1; r <= r2 && r < DOS_TEXT_ROWS; r++)
                for (uint8_t c = c1; c <= c2 && c < DOS_TEXT_COLUMNS; c++)
                    dos_text_write(vm, page, r, c, ' ', attr, true);
        } else {
            /* Scroll up by 'lines' rows */
            for (uint8_t r = r1; r <= r2 && r < DOS_TEXT_ROWS; r++) {
                for (uint8_t c = c1; c <= c2 && c < DOS_TEXT_COLUMNS; c++) {
                    if (r + lines <= r2) {
                        uint32_t src = dos_text_addr(page,
                            (uint8_t)(r + lines), c);
                        dos_text_write(vm, page, r, c, vm->mem[src],
                                       vm->mem[src + 1U], true);
                    } else {
                        dos_text_write(vm, page, r, c, ' ', attr, true);
                    }
                }
            }
        }
        dos_vga_flush(vm);
        break;
    }

    /* AH=07h: Scroll down */
    case 0x07: {
        uint8_t lines = cpu->al;
        uint8_t attr  = cpu->bh;
        uint8_t r1 = cpu->ch, c1 = cpu->cl;
        uint8_t r2 = cpu->dh, c2 = cpu->dl;
        uint8_t page = vm->vga_page & 7U;
        if (r1 >= DOS_TEXT_ROWS || c1 >= DOS_TEXT_COLUMNS) break;
        if (r2 >= DOS_TEXT_ROWS) r2 = DOS_TEXT_ROWS - 1U;
        if (c2 >= DOS_TEXT_COLUMNS) c2 = DOS_TEXT_COLUMNS - 1U;
        if (r1 > r2 || c1 > c2) break;

        if (lines == 0) {
            for (uint8_t r = r1; r <= r2 && r < DOS_TEXT_ROWS; r++)
                for (uint8_t c = c1; c <= c2 && c < DOS_TEXT_COLUMNS; c++)
                    dos_text_write(vm, page, r, c, ' ', attr, true);
        } else {
            for (int r = r2; r >= (int)r1; r--) {
                for (uint8_t c = c1; c <= c2 && c < DOS_TEXT_COLUMNS; c++) {
                    if (r - (int)lines >= (int)r1) {
                        uint32_t src = dos_text_addr(page,
                            (uint8_t)(r - lines), c);
                        dos_text_write(vm, page, (uint8_t)r, c,
                                       vm->mem[src], vm->mem[src + 1U], true);
                    } else {
                        dos_text_write(vm, page, (uint8_t)r, c,
                                       ' ', attr, true);
                    }
                }
            }
        }
        dos_vga_flush(vm);
        break;
    }

    /* AH=08h: Read character and attribute */
    case 0x08: {
        uint8_t row, col;
        dos_cursor_coordinates(vm, cpu->bh, &row, &col);
        uint32_t off = dos_text_addr(cpu->bh, row, col);
        cpu->al = vm->mem[off];       /* character */
        cpu->ah = vm->mem[off + 1];   /* attribute */
        break;
    }

    /* AH=09h: Write character and attribute */
    case 0x09: {
        uint16_t count = cpu->cx;
        uint8_t ch = cpu->al;
        uint8_t attr = cpu->bl;
        uint8_t page = cpu->bh & 7U;
        uint8_t row, col;
        dos_cursor_coordinates(vm, page, &row, &col);
        for (uint16_t i = 0; i < count; i++) {
            if (col >= DOS_TEXT_COLUMNS) { col = 0; row++; }
            if (row >= DOS_TEXT_ROWS) break;
            dos_text_write(vm, page, row, col, ch, attr, true);
            col++;
        }
        dos_vga_flush(vm);
        break;
    }

    /* AH=0Ah: Write character only (keep attribute) */
    case 0x0A: {
        uint16_t count = cpu->cx;
        uint8_t ch = cpu->al;
        uint8_t page = cpu->bh & 7U;
        uint8_t row, col;
        dos_cursor_coordinates(vm, page, &row, &col);
        for (uint16_t i = 0; i < count; i++) {
            if (col >= DOS_TEXT_COLUMNS) { col = 0; row++; }
            if (row >= DOS_TEXT_ROWS) break;
            dos_text_write(vm, page, row, col, ch, 0, false);
            col++;
        }
        dos_vga_flush(vm);
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
            dos_text_write(vm, vm->vga_page, vm->cursor_row,
                           vm->cursor_col, ch, vm->text_attr, true);
            vm->cursor_col++;
            if (vm->cursor_col >= DOS_TEXT_COLUMNS) {
                vm->cursor_col = 0;
                vm->cursor_row++;
            }
        }
        /* Scroll if needed */
        if (vm->cursor_row >= DOS_TEXT_ROWS) {
            vm->cursor_row = DOS_TEXT_ROWS - 1U;
            /* Scroll up 1 line */
            for (uint8_t r = 0; r < DOS_TEXT_ROWS - 1U; r++) {
                for (uint8_t c = 0; c < DOS_TEXT_COLUMNS; c++) {
                    uint32_t src = dos_text_addr(vm->vga_page,
                                                (uint8_t)(r + 1U), c);
                    dos_text_write(vm, vm->vga_page, r, c,
                                   vm->mem[src], vm->mem[src + 1U], true);
                }
            }
            /* Clear bottom line */
            for (uint8_t c = 0; c < DOS_TEXT_COLUMNS; c++)
                dos_text_write(vm, vm->vga_page, DOS_TEXT_ROWS - 1U,
                               c, ' ', vm->text_attr, true);
        }
        dos_mem_write16(vm, 0x450U + (uint32_t)(vm->vga_page & 7U) * 2U,
                        (uint16_t)(((uint16_t)vm->cursor_row << 8) |
                                   vm->cursor_col));
        dos_vga_flush(vm);
        break;
    }

    /* AH=0Fh: Get video mode */
    case 0x0F:
        cpu->al = vm->vga_mode;   /* mode (3 = 80x25 text) */
        cpu->ah = vm->vga_mode == 0x13 ? 40 : 80;
        cpu->bh = vm->vga_page;   /* active page */
        break;

    /* AH=12h: Alternate function select */
    case 0x12:
        if (cpu->bl == 0x10) {
            cpu->bh = 0x00;  /* color display active */
            cpu->bl = 0x03;  /* 256 KB VGA memory */
            cpu->ch = 0x00;  /* feature connector bits */
            cpu->cl = 0x09;  /* color analog display */
        } else {
            dos_video_diag(vm, "unsupported AH=12h subfunction ", cpu->bl);
        }
        break;

    /* AH=1Ah: Get/set display combination code */
    case 0x1A:
        if (cpu->al == 0x00) {
            cpu->al = 0x1A;   /* function supported */
            cpu->bl = 0x08;   /* VGA color */
        }
        break;

    default:
        if (ah == 0x4F) {
            dos_int10_vbe(vm);
        } else {
            dos_video_diag(vm, "unsupported INT 10h AH=", ah);
        }
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
        if (!dos_key_pending(vm))
            dos_key_enqueue(vm, (uint8_t)kb_getchar());
        cpu->ax = dos_key_pop(vm);
        break;
    }

    /* AH=01h/11h: Check key (non-blocking) */
    case 0x01:
    case 0x11:
        if (dos_key_pump(vm)) {
            cpu->flags &= ~FLAG_ZF;
            cpu->ax = vm->kb_buffer[vm->kb_tail];
        } else {
            cpu->flags |= FLAG_ZF;
        }
        break;

    /* AH=02h/12h: Get shift flags */
    case 0x02:
    case 0x12:
        cpu->al = kb_get_bios_shift_flags();
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
        cpu->flags &= ~FLAG_CF;
        break;

    /* AH=02h: Read RTC time (BCD) */
    case 0x02: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        if (!dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->flags |= FLAG_CF;
            break;
        }
        cpu->ch = dos_bcd(calendar.hour);
        cpu->cl = dos_bcd(calendar.minute);
        cpu->dh = dos_bcd(calendar.second);
        cpu->dl = vm->clock_daylight;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* AH=04h: Read RTC date (BCD) */
    case 0x04: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        if (!dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->flags |= FLAG_CF;
            break;
        }
        cpu->ch = dos_bcd(calendar.year / 100U);
        cpu->cl = dos_bcd(calendar.year % 100U);
        cpu->dh = dos_bcd(calendar.month);
        cpu->dl = dos_bcd(calendar.day);
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* AH=03h: Set RTC time (BCD). */
    case 0x03: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        uint8_t hour;
        uint8_t minute;
        uint8_t second;
        if (cpu->dl > 1U ||
            !dos_bcd_decode(cpu->ch, &hour) ||
            !dos_bcd_decode(cpu->cl, &minute) ||
            !dos_bcd_decode(cpu->dh, &second) ||
            !dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->flags |= FLAG_CF;
            break;
        }
        calendar.hour = hour;
        calendar.minute = minute;
        calendar.second = second;
        if (!dos_clock_set(vm, &calendar, 0)) {
            cpu->flags |= FLAG_CF;
            break;
        }
        vm->clock_daylight = cpu->dl;
        cpu->flags &= ~FLAG_CF;
        break;
    }

    /* AH=05h: Set RTC date (BCD). */
    case 0x05: {
        dos_calendar_t calendar;
        uint8_t hundredths;
        uint8_t century;
        uint8_t year;
        uint8_t month;
        uint8_t day;
        if (!dos_bcd_decode(cpu->ch, &century) ||
            !dos_bcd_decode(cpu->cl, &year) ||
            !dos_bcd_decode(cpu->dh, &month) ||
            !dos_bcd_decode(cpu->dl, &day) ||
            !dos_clock_get(vm, &calendar, &hundredths)) {
            cpu->flags |= FLAG_CF;
            break;
        }
        calendar.year = (uint16_t)((uint16_t)century * 100U + year);
        calendar.month = month;
        calendar.day = day;
        if (!dos_clock_set(vm, &calendar, hundredths)) {
            cpu->flags |= FLAG_CF;
            break;
        }
        cpu->flags &= ~FLAG_CF;
        break;
    }

    default:
        cpu->flags |= FLAG_CF;
        break;
    }
}

static int dos_bios_video_selftest(void)
{
    extern void *mem_alloc_pages(uint64_t count);
    extern void mem_free_pages(void *addr, uint64_t count);

    const uint64_t pages =
        (DOS_VRAM_BASE + DOS_VRAM_SIZE + 4095U) / 4096U;
    uint8_t *memory = (uint8_t *)mem_alloc_pages(pages);
    if (!memory) {
        serial_puts("[DOS-TEST] BIOS video allocation\n");
        return 1;
    }
    for (uint64_t i = 0; i < pages * 4096U; i++)
        memory[i] = 0;

    dos_vm_t vm = {0};
    cpu8086_state_t cpu;
    vm.cpu = &cpu;
    vm.mem = memory;
    vm.total_mem_size = (uint32_t)(pages * 4096U);
    vm.vga_mode = 0x03;
    vm.text_attr = 0x07;
    vm.cursor_start = 6;
    vm.cursor_end = 7;
    cpu8086_init(&cpu, &vm);

    int failures = 0;
    dos_mem_write16(&vm, 0x450, 0x0000);
    dos_mem_write16(&vm, 0x452, 0x0203);
    uint32_t page1_cell = dos_text_addr(1, 2, 3);
    memory[page1_cell] = 'Q';
    memory[page1_cell + 1U] = 0x1E;

    cpu.ah = 0x08;
    cpu.bh = 1;
    dos_int10_video(&vm);
    if (cpu.ax != 0x1E51U) {
        serial_puts("[DOS-TEST] BIOS video page read\n");
        failures++;
    }

    cpu.ax = 0x0958; /* write 'X' with attribute */
    cpu.bx = 0x012E; /* page 1, attribute 2Eh */
    cpu.cx = 2;
    dos_int10_video(&vm);
    if (memory[page1_cell] != 'X' || memory[page1_cell + 1U] != 0x2E ||
        memory[page1_cell + 2U] != 'X' ||
        memory[page1_cell + 3U] != 0x2E ||
        dos_cursor_position(&vm, 1) != 0x0203U ||
        vm.cursor_row != 0 || vm.cursor_col != 0) {
        serial_puts("[DOS-TEST] BIOS video page write\n");
        failures++;
    }

    memory[page1_cell + 1U] = 0x5A;
    cpu.ax = 0x0A59; /* write 'Y', retaining the cell attribute */
    cpu.bx = 0x0100;
    cpu.cx = 1;
    dos_int10_video(&vm);
    if (memory[page1_cell] != 'Y' || memory[page1_cell + 1U] != 0x5A) {
        serial_puts("[DOS-TEST] BIOS video character-only write\n");
        failures++;
    }

    cpu.ah = 0x02;
    cpu.bh = 1;
    cpu.dh = 4;
    cpu.dl = 5;
    dos_int10_video(&vm);
    cpu.ah = 0x03;
    cpu.bh = 1;
    dos_int10_video(&vm);
    if (cpu.dh != 4 || cpu.dl != 5 || vm.cursor_row != 0 ||
        vm.cursor_col != 0) {
        serial_puts("[DOS-TEST] BIOS video cursor page\n");
        failures++;
    }

    cpu.ax = 0x0501;
    dos_int10_video(&vm);
    if (vm.vga_page != 1 || vm.cursor_row != 4 || vm.cursor_col != 5 ||
        memory[0x462] != 1 || dos_mem_read16(&vm, 0x44E) != 4096U) {
        serial_puts("[DOS-TEST] BIOS video active page\n");
        failures++;
    }

    cpu.ah = 0x01;
    cpu.ch = 0x26;
    cpu.cl = 0x07;
    dos_int10_video(&vm);
    if (vm.cursor_start != 0x26 || vm.cursor_end != 0x07 ||
        dos_mem_read16(&vm, 0x460) != 0x2607U) {
        serial_puts("[DOS-TEST] BIOS video cursor shape\n");
        failures++;
    }

    for (uint8_t row = 0; row < 3; row++) {
        uint32_t cell = dos_text_addr(1, row, 0);
        memory[cell] = (uint8_t)('A' + row);
        memory[cell + 1U] = (uint8_t)(row + 1U);
    }
    cpu.ax = 0x0601;
    cpu.bh = 0x4F;
    cpu.ch = 0;
    cpu.cl = 0;
    cpu.dh = 2;
    cpu.dl = 0;
    dos_int10_video(&vm);
    uint32_t row0 = dos_text_addr(1, 0, 0);
    uint32_t row1 = dos_text_addr(1, 1, 0);
    uint32_t row2 = dos_text_addr(1, 2, 0);
    if (memory[row0] != 'B' || memory[row0 + 1U] != 2 ||
        memory[row1] != 'C' || memory[row1 + 1U] != 3 ||
        memory[row2] != ' ' || memory[row2 + 1U] != 0x4F) {
        serial_puts("[DOS-TEST] BIOS video scroll\n");
        failures++;
    }

    memory[0xA0000U] = 0x5A;
    memory[0xA0000U + 320U * 200U - 1U] = 0xA5;
    cpu.ax = 0x0013;
    dos_int10_video(&vm);
    if (memory[0xA0000U] != 0 ||
        memory[0xA0000U + 320U * 200U - 1U] != 0 ||
        vm.vga_mode != 0x13 || memory[0x449] != 0x13 ||
        dos_mem_read16(&vm, 0x44A) != 40 ||
        dos_mem_read16(&vm, 0x44C) != 0xFA00U ||
        dos_mem_read16(&vm, 0x485) != 8) {
        serial_puts("[DOS-TEST] BIOS video mode 13h\n");
        failures++;
    }

    memory[0xA0000U] = 0x5A;
    cpu.ax = 0x0093; /* mode 13h with no-clear bit */
    dos_int10_video(&vm);
    if (memory[0xA0000U] != 0x5A || vm.vga_mode != 0x13) {
        serial_puts("[DOS-TEST] BIOS video no-clear mode\n");
        failures++;
    }

    cpu.ax = 0x0012; /* unsupported mode must preserve current state */
    dos_int10_video(&vm);
    if (vm.vga_mode != 0x13 || memory[0x449] != 0x13) {
        serial_puts("[DOS-TEST] BIOS video unsupported mode\n");
        failures++;
    }

    mem_free_pages(memory, pages);
    return failures;
}

int dos_bios_contract_selftest(void)
{
    int failures = 0;
    dos_calendar_t calendar;

    if (!dos_unix_to_calendar(951782400U, &calendar) ||
        calendar.year != 2000U || calendar.month != 2U ||
        calendar.day != 29U || calendar.hour != 0U ||
        calendar.minute != 0U || calendar.second != 0U) {
        serial_puts("[DOS-TEST] BIOS leap-day conversion\n");
        failures++;
    }
    if (!dos_unix_to_calendar(2147483647U, &calendar) ||
        calendar.year != 2038U || calendar.month != 1U ||
        calendar.day != 19U || calendar.hour != 3U ||
        calendar.minute != 14U || calendar.second != 7U) {
        serial_puts("[DOS-TEST] BIOS 2038 conversion\n");
        failures++;
    }
    uint16_t packed_date;
    uint16_t packed_time;
    uint64_t unpacked_time;
    if (!dos_pack_datetime(951827696ULL, &packed_date, &packed_time) ||
        packed_date != 0x285DU || packed_time != 0x645CU ||
        !dos_unpack_datetime(packed_date, packed_time, &unpacked_time) ||
        unpacked_time != 951827696ULL) {
        serial_puts("[DOS-TEST] DOS packed date/time round-trip\n");
        failures++;
    }
    if (dos_unpack_datetime(0x29A1U, 0x0000U, &unpacked_time) ||
        dos_unpack_datetime(0x2821U, 0xC000U, &unpacked_time)) {
        serial_puts("[DOS-TEST] DOS packed date/time validation\n");
        failures++;
    }
    uint8_t day_of_week;
    calendar.year = 2000;
    calendar.month = 2;
    calendar.day = 29;
    calendar.hour = 0;
    calendar.minute = 0;
    calendar.second = 0;
    if (!dos_calendar_day_of_week(&calendar, &day_of_week) ||
        day_of_week != 2U) {
        serial_puts("[DOS-TEST] DOS day-of-week conversion\n");
        failures++;
    }
    if (dos_bcd(59U) != 0x59U || dos_bcd(20U) != 0x20U) {
        serial_puts("[DOS-TEST] BIOS BCD conversion\n");
        failures++;
    }
    if (dos_ascii_scancode('a') != 0x1EU ||
        dos_ascii_scancode('A') != 0x1EU ||
        dos_ascii_scancode('?') != 0x35U ||
        dos_ascii_scancode('\n') != 0x1CU ||
        dos_ascii_scancode(1U) != 0x1EU) {
        serial_puts("[DOS-TEST] BIOS ASCII/scancode conversion\n");
        failures++;
    }

    dos_vm_t clock_vm = {0};
    cpu8086_state_t clock_cpu = {0};
    clock_vm.cpu = &clock_cpu;
    clock_vm.start_ticks = idt_get_ticks();
    calendar.year = 2000;
    calendar.month = 2;
    calendar.day = 29;
    calendar.hour = 12;
    calendar.minute = 34;
    calendar.second = 56;
    if (!dos_clock_set(&clock_vm, &calendar, 42U)) {
        serial_puts("[DOS-TEST] BIOS virtual clock setup\n");
        failures++;
    }

    clock_cpu.ah = 0x04;
    clock_cpu.flags |= FLAG_CF;
    dos_int1a_timer(&clock_vm);
    if ((clock_cpu.flags & FLAG_CF) || clock_cpu.ch != 0x20U ||
        clock_cpu.cl != 0x00U || clock_cpu.dh != 0x02U ||
        clock_cpu.dl != 0x29U) {
        serial_puts("[DOS-TEST] BIOS RTC date read\n");
        failures++;
    }

    clock_cpu.ah = 0x05;
    clock_cpu.ch = 0x20;
    clock_cpu.cl = 0x24;
    clock_cpu.dh = 0x02;
    clock_cpu.dl = 0x29;
    clock_cpu.flags |= FLAG_CF;
    dos_int1a_timer(&clock_vm);
    if (clock_cpu.flags & FLAG_CF) {
        serial_puts("[DOS-TEST] BIOS RTC date set\n");
        failures++;
    }

    clock_cpu.ah = 0x03;
    clock_cpu.ch = 0x08;
    clock_cpu.cl = 0x09;
    clock_cpu.dh = 0x10;
    clock_cpu.dl = 1;
    clock_cpu.flags |= FLAG_CF;
    dos_int1a_timer(&clock_vm);
    if (clock_cpu.flags & FLAG_CF) {
        serial_puts("[DOS-TEST] BIOS RTC time set\n");
        failures++;
    }

    uint8_t hundredths;
    if (!dos_clock_get(&clock_vm, &calendar, &hundredths) ||
        calendar.year != 2024U || calendar.month != 2U ||
        calendar.day != 29U || calendar.hour != 8U ||
        calendar.minute != 9U ||
        (calendar.second != 10U && calendar.second != 11U) ||
        hundredths > 99U || clock_vm.clock_daylight != 1U) {
        serial_puts("[DOS-TEST] BIOS/DOS clock coherence\n");
        failures++;
    }

    clock_cpu.ah = 0x02;
    clock_cpu.flags |= FLAG_CF;
    dos_int1a_timer(&clock_vm);
    if ((clock_cpu.flags & FLAG_CF) || clock_cpu.ch != 0x08U ||
        clock_cpu.cl != 0x09U ||
        (clock_cpu.dh != 0x10U && clock_cpu.dh != 0x11U) ||
        clock_cpu.dl != 1U) {
        serial_puts("[DOS-TEST] BIOS RTC time read\n");
        failures++;
    }

    clock_cpu.ah = 0x05;
    clock_cpu.ch = 0x20;
    clock_cpu.cl = 0x24;
    clock_cpu.dh = 0x1A;
    clock_cpu.dl = 0x01;
    clock_cpu.flags &= ~FLAG_CF;
    dos_int1a_timer(&clock_vm);
    if (!(clock_cpu.flags & FLAG_CF) ||
        !dos_clock_get(&clock_vm, &calendar, &hundredths) ||
        calendar.year != 2024U || calendar.month != 2U ||
        calendar.day != 29U) {
        serial_puts("[DOS-TEST] BIOS RTC invalid date\n");
        failures++;
    }

    clock_cpu.ah = 0x03;
    clock_cpu.ch = 0x24;
    clock_cpu.cl = 0;
    clock_cpu.dh = 0;
    clock_cpu.dl = 0;
    clock_cpu.flags &= ~FLAG_CF;
    dos_int1a_timer(&clock_vm);
    if (!(clock_cpu.flags & FLAG_CF)) {
        serial_puts("[DOS-TEST] BIOS RTC invalid time\n");
        failures++;
    }

    failures += dos_bios_video_selftest();
    return failures;
}
