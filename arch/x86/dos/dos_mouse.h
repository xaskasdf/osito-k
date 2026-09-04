#ifndef OSITOK_DOS_MOUSE_H
#define OSITOK_DOS_MOUSE_H

#include "dos_types.h"

void dos_mouse_init(dos_vm_t *vm);
void dos_mouse_video_mode_changed(dos_vm_t *vm);
void dos_int33_mouse(dos_vm_t *vm);
int dos_mouse_selftest(void);

#endif
