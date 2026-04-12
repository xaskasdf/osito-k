/*
 * arch/x86/include/paging.h — higher-half addressing primitives
 *
 * This header defines the kernel virtual base and the PHYS_TO_VIRT /
 * VIRT_TO_PHYS macros used to translate between physical addresses (as
 * seen by DMA / MMIO / page tables) and the kernel's high virtual view
 * of RAM.
 *
 * Currently the kernel code still runs identity-mapped (text at its
 * physical load address), and paging_init() installs both the identity
 * map at VA = PA and a mirror map of all RAM at VA = PA + KERNEL_VBASE.
 * Drivers migrate one at a time to use the upper-half alias; the
 * long-term goal is to drop the lower-half identity map from user
 * process PML4s so user space owns the entire lower canonical half.
 *
 * See docs/os-selfhost-roadmap.md, section "Implementación en fases".
 */

#ifndef _OSITOK_PAGING_H
#define _OSITOK_PAGING_H

#include "types.h"

/* Kernel virtual base: first address of the upper canonical half that
 * we reserve for the kernel's direct map of RAM. Chosen so that
 * PML4[256] owns 512 GB of virtual space, which is more than enough
 * for any machine OsitoK currently targets. */
#define KERNEL_VBASE  0xFFFF800000000000ULL

/* Fase 0: both macros are the identity. No caller sees any difference
 * yet. Fase 1 flips PHYS_TO_VIRT to add KERNEL_VBASE once paging_init
 * has mirrored all of RAM at the upper-half address. */
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v) ((uint64_t)(uintptr_t)(v))

#endif /* _OSITOK_PAGING_H */
