/*
 * OsitoK DOS conventional-memory manager contract.
 */

#ifndef DOS_MEM_H
#define DOS_MEM_H

#include "dos_types.h"

/* Existing callers only distinguish success from failure.  DPMI also needs
 * the DOS-compatible reason so it can return 0007h, 0008h, or 0009h. */
#define DOS_MEM_OK             0
#define DOS_MEM_ERR_ARENA     -1
#define DOS_MEM_ERR_BLOCK     -2
#define DOS_MEM_ERR_NOMEM     -3

void dos_mem_init(dos_vm_t *vm);
uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs,
                       uint16_t *largest);
int dos_mem_free(dos_vm_t *vm, uint16_t segment);
int dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size,
                   uint16_t *max_avail);
int dos_mem_query_block(dos_vm_t *vm, uint16_t segment,
                        uint16_t *current_size, uint16_t *max_avail);
int dos_mem_largest_available(dos_vm_t *vm, uint16_t *largest);
int dos_mem_free_owner(dos_vm_t *vm, uint16_t owner);
int dos_mem_selftest(void);

#endif
