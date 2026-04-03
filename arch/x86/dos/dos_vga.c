/*
 * OsitoK — VGA Text Mode Emulation on GOP Framebuffer
 *
 * Phase 3: Full CGA/VGA text mode rendering.
 * Phase 1: Stub — rendering goes through fb_putchar in dos_bios.c.
 */

#include "dos_types.h"

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
