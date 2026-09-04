#ifndef OSITOK_DOS_FIND_H
#define OSITOK_DOS_FIND_H

#include "dos_types.h"

void dos_find_init(dos_vm_t *vm);
void dos_find_close_all(dos_vm_t *vm);
int dos_find_first(dos_vm_t *vm, const char *path, uint16_t attributes);
int dos_find_next(dos_vm_t *vm);
int dos_resolve_path(dos_vm_t *vm, const char *path,
                     bool allow_missing_leaf, char *resolved,
                     uint32_t resolved_capacity);
int dos_file_get_attributes(dos_vm_t *vm, const char *path,
                            uint16_t *attributes);
int dos_file_set_attributes(dos_vm_t *vm, const char *path,
                            uint16_t attributes);
int dos_find_selftest(void);

#endif /* OSITOK_DOS_FIND_H */
