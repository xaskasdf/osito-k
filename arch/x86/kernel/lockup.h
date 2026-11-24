#ifndef OSITOK_LOCKUP_H
#define OSITOK_LOCKUP_H

#include "../include/interrupt.h"

/* Written only by the requested BSP NMI path, without C calls or SIMD. */
extern volatile uint32_t lockup_nmi_pending, lockup_nmi_captured;
extern volatile x86_interrupt_frame_t lockup_nmi_frame;
extern volatile uint64_t lockup_nmi_cr2, lockup_nmi_cr3;
extern volatile uint64_t lockup_nmi_cr0, lockup_nmi_cr4;

void lockup_command(int argc, char **argv);

#endif
