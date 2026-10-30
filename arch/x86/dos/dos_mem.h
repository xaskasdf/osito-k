/*
 * OsitoK DOS conventional and upper-memory manager contract.
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
#define DOS_MEM_ERR_UNAVAILABLE -4

void dos_mem_init(dos_vm_t *vm);
int dos_mem_set_umb_link(dos_vm_t *vm, uint16_t linked);
int dos_mem_alloc_checked(dos_vm_t *vm, uint16_t paragraphs, uint16_t owner,
                          uint16_t *segment, uint16_t *largest);
uint16_t dos_mem_alloc(dos_vm_t *vm, uint16_t paragraphs,
                       uint16_t *largest);
uint16_t dos_mem_alloc_owned(dos_vm_t *vm, uint16_t paragraphs,
                             uint16_t *largest, uint16_t owner);
int dos_mem_free(dos_vm_t *vm, uint16_t segment);
int dos_mem_resize(dos_vm_t *vm, uint16_t segment, uint16_t new_size,
                   uint16_t *max_avail);
int dos_mem_query_block(dos_vm_t *vm, uint16_t segment,
                        uint16_t *current_size, uint16_t *max_avail);
int dos_mem_largest_available(dos_vm_t *vm, uint16_t *largest);
int dos_mem_free_owner(dos_vm_t *vm, uint16_t owner);

#define DOS_TRANSFER_BUFFER_MAX 0x4000u
#define DOS_TRANSFER_STACK_SIZE 0x1000u
typedef struct {
    uint64_t generation;
    uint32_t capacity;
    uint16_t segment, stack_segment;
    uint8_t *scratch;
} dos_transfer_buffer_t;

bool dos_transfer_acquire(dos_vm_t *vm, uint32_t requested, uint32_t minimum,
                           dos_transfer_buffer_t *buffer);
bool dos_transfer_release(dos_vm_t *vm, uint64_t generation);
void dos_transfer_release_after(dos_vm_t *vm, uint64_t generation);
int dos_mem_selftest(void);
int dos_umb_selftest(void);

#endif
