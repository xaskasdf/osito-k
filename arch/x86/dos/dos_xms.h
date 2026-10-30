#ifndef OSITOK_DOS_XMS_H
#define OSITOK_DOS_XMS_H

#include "dos_types.h"

#define DOS_XMS_ENTRY_OFF 0x20C0u
#define DOS_XMS_HMA_BASE  0x100000u
#define DOS_XMS_HMA_SIZE  0x10000u
#define DOS_XMS_UMB_BASE  0xD0000u
#define DOS_XMS_UMB_SIZE  0x10000u

bool dos_xms_init(dos_vm_t *vm);
void dos_xms_cleanup(dos_vm_t *vm);
bool dos_xms_multiplex(dos_vm_t *vm);
bool dos_xms_entry_source(const dos_vm_t *vm);
bool dos_xms_bios_hooked(const dos_vm_t *vm);
void dos_xms_dispatch(dos_vm_t *vm);
/* Host ownership, independent of guest-editable MCBs. Segment units. */
uint16_t dos_xms_umb_claim_dos(dos_vm_t *vm);
void dos_xms_umb_release_dos(dos_vm_t *vm);
uint16_t dos_xms_umb_dos_next(const dos_vm_t *vm, uint16_t segment);
uint16_t dos_xms_umb_dos_end(const dos_vm_t *vm, uint16_t segment);
int dos_xms_selftest(void);

#endif
