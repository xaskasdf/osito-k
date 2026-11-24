#ifndef OSITOK_MSR_SAFE_H
#define OSITOK_MSR_SAFE_H

#include "types.h"

/* The output is unchanged on failure. Kernel SysV callers only. */
bool x86_read_msr_safe(uint32_t msr, uint64_t *value);
extern const uint8_t x86_read_msr_fault[], x86_read_msr_fixup[];

static inline bool x86_msr_fixup(uint64_t vector, uint64_t cs,
                                uint64_t error, uint64_t *rip)
{
    if (vector != 13 || (cs & 3) != 0 || error != 0 || !rip ||
        *rip != (uintptr_t)x86_read_msr_fault)
        return false;
    *rip = (uintptr_t)x86_read_msr_fixup;
    return true;
}

#endif
