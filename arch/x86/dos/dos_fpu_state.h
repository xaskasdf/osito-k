#ifndef OSITOK_DOS_FPU_STATE_H
#define OSITOK_DOS_FPU_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Operands have already passed stack/memory checks. Environments use the
 * layer's canonical 28-byte layout; the caller owns guest pointers/selectors. */
bool dos_fpu_state_step(uint8_t *image, uint8_t *operand, uint8_t opcode,
                        uint8_t modrm, uint32_t flags, uint64_t *result);
#ifdef __cplusplus
}
#endif

#endif
