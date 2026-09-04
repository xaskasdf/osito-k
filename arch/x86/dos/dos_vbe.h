#ifndef DOS_VBE_H
#define DOS_VBE_H

#include "../include/types.h"

struct dos_vm;

void dos_vbe_init(struct dos_vm *vm);
void dos_vbe_leave_mode(struct dos_vm *vm);
void dos_int10_vbe(struct dos_vm *vm);
void dos_vbe_fit_rect(uint32_t source_width, uint32_t source_height,
                      uint32_t target_width, uint32_t target_height,
                      uint32_t *origin_x, uint32_t *origin_y,
                      uint32_t *display_width, uint32_t *display_height);
int dos_vbe_selftest(void);

#endif
