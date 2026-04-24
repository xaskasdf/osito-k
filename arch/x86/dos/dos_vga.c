/*
 * OsitoK — VGA Text Mode Emulation on GOP Framebuffer
 *
 * Phase 3: Full CGA/VGA text mode rendering.
 * Phase 1: Stub — rendering goes through fb_putchar in dos_bios.c.
 */

#include "dos_types.h"

extern uint32_t *fb_get_vram(void);
extern uint32_t  fb_get_width(void);
extern uint32_t  fb_get_height(void);
extern uint32_t  fb_get_pitch(void);

static inline uint8_t dos_vga_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void dos_vga_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* Pointer to the active native-DOS VM so the timer can present VGA
 * mode 13h vram to the real framebuffer. Set by dos_set_native_vm. */
static dos_vm_t *dos_vga_native_vm = 0;

/* Shadow of the VGA DAC palette (6-bit per channel). We read it back
 * from the emulated VGA controller each present cycle so DOS programs
 * that write palette via OUT 0x3C8/0x3C9 get their colors displayed. */
static uint8_t dos_vga_dac[256][3];
static uint8_t dos_vga_dac_valid = 0;

void dos_vga_set_native_vm(dos_vm_t *vm) { dos_vga_native_vm = vm; }

static void dos_vga_refresh_dac(void)
{
    dos_vga_outb(0x3C7, 0);  /* set DAC read index to 0 */
    for (int i = 0; i < 256; i++) {
        dos_vga_dac[i][0] = dos_vga_inb(0x3C9);
        dos_vga_dac[i][1] = dos_vga_inb(0x3C9);
        dos_vga_dac[i][2] = dos_vga_inb(0x3C9);
    }
    dos_vga_dac_valid = 1;
}

/* Present VGA mode 13h (320×200×8 indexed) to the real framebuffer.
 * Called from the APIC timer tick; centered, 1:1 pixels. Palette is
 * read back from the emulated VGA DAC (ports 0x3C7/0x3C9) once per
 * frame, so colors that DOS programs set via OUT 0x3C8/0x3C9 show up.
 * 6-bit DAC values (0..63) are scaled to 8-bit via << 2. */
void dos_vga_mode13_present(void)
{
    dos_vm_t *vm = dos_vga_native_vm;
    if (!vm || vm->vga_mode != 0x13) return;

    uint32_t *fb = fb_get_vram();
    uint32_t  fw = fb_get_width();
    uint32_t  fh = fb_get_height();
    uint32_t  pp = fb_get_pitch() / 4;  /* pixels per row */
    if (!fb || fw < 320 || fh < 200) return;

    /* Snapshot palette every frame (cheap: 768 inb). */
    dos_vga_refresh_dac();

    const uint8_t *vram = &vm->mem[0xA0000];
    uint32_t ox = (fw - 320) / 2;
    uint32_t oy = (fh - 200) / 2;
    for (uint32_t y = 0; y < 200; y++) {
        uint32_t *dst = fb + (oy + y) * pp + ox;
        const uint8_t *src = vram + y * 320;
        for (uint32_t x = 0; x < 320; x++) {
            uint8_t v = src[x];
            uint32_t r = (uint32_t)(dos_vga_dac[v][0] << 2);
            uint32_t g = (uint32_t)(dos_vga_dac[v][1] << 2);
            uint32_t b = (uint32_t)(dos_vga_dac[v][2] << 2);
            dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    (void)dos_vga_dac_valid;
}

/* Flush VGA text buffer to framebuffer (Phase 3) */
void dos_vga_flush(dos_vm_t *vm)
{
    (void)vm;
    /* TODO Phase 3: scan dirty cells and render to GOP framebuffer
     * using gui_font8x16 with per-cell fg/bg from CGA palette. */
}

/* Mark a cell as dirty when VGA memory is written */
void dos_vga_mark_dirty(dos_vm_t *vm, uint32_t addr)
{
    if (addr < DOS_VRAM_BASE || addr >= DOS_VRAM_BASE + DOS_VRAM_SIZE) return;
    uint32_t cell = (addr - DOS_VRAM_BASE) / 2;
    if (cell < 80 * 25) {
        vm->vga_dirty[cell / 8] |= (1 << (cell % 8));
    }
}
