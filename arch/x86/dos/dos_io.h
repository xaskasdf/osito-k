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
void dos_io_get_pic_bases(const struct dos_vm *vm, uint8_t *master,
                          uint8_t *slave);
bool dos_io_copy_dac(const struct dos_vm *vm, uint8_t palette[256][3]);

int dos_io_selftest(void);

#endif
