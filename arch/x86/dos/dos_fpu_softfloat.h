#ifndef OSITOK_DOS_FPU_SOFTFLOAT_H
#define OSITOK_DOS_FPU_SOFTFLOAT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t value[10];
    uint8_t pushed_value[10];
    uint16_t exceptions;
    uint16_t condition_clear, conditions;
    uint16_t integer_flags;
    unsigned destination, pops;
    unsigned memory_size; /* value[] is a staged memory result, not a register write. */
    bool push, write;
} dos_fpu_numeric_result_t;

#ifdef __cplusplus
extern "C" {
#endif
bool dos_fpu_soft_evaluate(const uint8_t *image, const uint8_t *operand,
                           uint8_t opcode, uint8_t modrm,
                           dos_fpu_numeric_result_t *result);
#ifdef __cplusplus
}
#endif

#endif
