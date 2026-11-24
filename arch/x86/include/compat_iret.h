#ifndef OSITO_COMPAT_IRET_H
#define OSITO_COMPAT_IRET_H

/* PML4[510]: reserved for 16-bit-stack IRET aliases, not the RAM direct map. */
#define X86_ESPFIX_BASE       0xFFFFFF0000000000
#define X86_ESPFIX_CPUS       256
#define X86_ESPFIX_SLOT_SIZE  64

#ifndef __ASSEMBLER__
#include "types.h"
void x86_compat_iret_init(uint64_t *pml4);
int x86_compat_iret_selftest(void);
#endif

#endif
