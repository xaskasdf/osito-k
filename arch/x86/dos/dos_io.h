#ifndef OSITOK_DOS_IO_H
#define OSITOK_DOS_IO_H

#include "../include/types.h"

struct dos_vm;

bool dos_io_init(struct dos_vm *vm);
void dos_io_shutdown(struct dos_vm *vm);

uint8_t dos_io_read8(struct dos_vm *vm, uint16_t port);
uint16_t dos_io_read16(struct dos_vm *vm, uint16_t port);
uint32_t dos_io_read32(struct dos_vm *vm, uint16_t port);
void dos_io_write8(struct dos_vm *vm, uint16_t port, uint8_t value);
void dos_io_write16(struct dos_vm *vm, uint16_t port, uint16_t value);
void dos_io_write32(struct dos_vm *vm, uint16_t port, uint32_t value);

bool dos_io_irq_begin(struct dos_vm *vm, uint8_t irq, uint8_t *vector);
bool dos_io_timer_poll(struct dos_vm *vm);
bool dos_io_keyboard_acquire(struct dos_vm *vm);
bool dos_io_keyboard_poll(struct dos_vm *vm);
void dos_io_get_pic_bases(const struct dos_vm *vm, uint8_t *master,
                          uint8_t *slave);
bool dos_io_copy_dac(const struct dos_vm *vm, uint8_t palette[256][3]);

#define DOS_VGA_APERTURE_BASE 0x000A0000u
#define DOS_VGA_APERTURE_SIZE 0x00020000u
#define DOS_VGA_MEMORY_SIZE   0x00040000u
#define DOS_VGA_MAX_WIDTH     2048u

void dos_io_vga_set_mode(struct dos_vm *vm, uint8_t mode, bool clear);
uint8_t dos_io_vga_read_memory(struct dos_vm *vm, uint32_t address);
void dos_io_vga_write_memory(struct dos_vm *vm, uint32_t address, uint8_t value);
bool dos_io_vga_geometry(const struct dos_vm *vm, uint32_t *width,
                         uint32_t *height);
bool dos_io_vga_scanline(const struct dos_vm *vm, uint32_t y,
                         uint8_t *pixels, uint32_t capacity);

int dos_io_selftest(void);

#endif
