#ifndef OSITOK_DOS_KEYBOARD_H
#define OSITOK_DOS_KEYBOARD_H

#include "dos_types.h"

/* BIOS and DOS consume the same VM queue; probing never removes a key. */
bool dos_keyboard_ready(dos_vm_t *vm);
uint16_t dos_keyboard_read(dos_vm_t *vm);
void dos_keyboard_flush(dos_vm_t *vm);
void dos_keyboard_irq(dos_vm_t *vm);

#endif
