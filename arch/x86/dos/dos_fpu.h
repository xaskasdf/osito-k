#ifndef OSITOK_DOS_FPU_H
#define OSITOK_DOS_FPU_H

#include "dos_types.h"
#include "dos_native_fp.h"

typedef struct dos_fpu_op {
    unsigned size;
    bool write;
    bool waiting;
    bool control;
    bool environment;
    bool restore;
    bool reset;
    bool flags;
    bool ax;
} dos_fpu_op_t;

bool dos_fpu_ensure(dos_vm_t *vm);
bool dos_fpu_decode(uint8_t opcode, uint8_t modrm, bool op32, dos_fpu_op_t *op);
bool dos_fpu_pending(const dos_vm_t *vm);
void dos_fpu_sync_irq(dos_vm_t *vm);
void dos_fpu_ack_irq(dos_vm_t *vm);
void dos_fpu_export_environment(const uint8_t *image, uint16_t fcs, uint16_t fds,
                                uint8_t *environment);
void dos_fpu_capture_native(dos_vm_t *vm, const void *frame);
/* Returns whether an admitted memory store actually produced a result. */
bool dos_fpu_execute(dos_vm_t *vm, const dos_fpu_op_t *op,
                       uint8_t opcode, uint8_t modrm, bool op32,
                       uint32_t ip, unsigned segment, uint32_t offset,
                       uint8_t *operand);
int dos_fpu_selftest(void);

#endif
