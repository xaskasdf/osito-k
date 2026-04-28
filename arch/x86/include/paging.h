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

/* paging_init() installs the upper-half mirror, so these macros now
 * translate to/from the high virtual view. They are only valid for
 * addresses that point into RAM — MMIO and framebuffer ranges still
 * need explicit paging_map_mmio() + identity-mapped access.
 *
 * Callers in this Fase still mostly use physical addresses directly
 * (via the lower-half identity map that paging_init also installs).
 * The macros exist so drivers can migrate one at a time. */
#define PHYS_TO_VIRT(p) ((void *)((uintptr_t)(p) + KERNEL_VBASE))
#define VIRT_TO_PHYS(v) ((uint64_t)((uintptr_t)(v) - KERNEL_VBASE))

/* Safe virtual→physical for kernel addresses.
 *
 * In OsitoK both lower-half identity-mapped memory (UEFI-allocated boot
 * stacks, ELF .text/.data sections that boot.efi loaded at their LMA)
 * AND upper-half PHYS_TO_VIRT direct-map memory (Phase C migrations) are
 * in use. A naive `v - KERNEL_VBASE` underflows when `v` is already a
 * lower-half identity address, producing a garbage phys that the IOMMU
 * doesn't translate to anything real.
 *
 * Use this helper for any pointer that might come from either world —
 * e.g. on-stack DMA buffers in driver code paths, where the stack might
 * still be in UEFI-loaded territory. */
static inline uint64_t kvirt_to_phys(const void *v)
{
    uint64_t a = (uint64_t)(uintptr_t)v;
    return (a >= KERNEL_VBASE) ? (a - KERNEL_VBASE) : a;
}

#endif /* _OSITOK_PAGING_H */
