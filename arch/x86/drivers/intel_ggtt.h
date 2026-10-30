/* Intel Gen9 client GGTT discovery. No allocation or hardware writes. */
#ifndef OSITOK_INTEL_GGTT_H
#define OSITOK_INTEL_GGTT_H

#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#define INTEL_GGTT_BAR_BYTES (16ULL * 1024 * 1024)
#define INTEL_GGTT_MMIO_OFFSET (8ULL * 1024 * 1024)
#define INTEL_GGTT_PHYS_LIMIT (1ULL << 39)

/* Captured without BAR sizing writes. GGC/BDSM/BGSM/TOLUD are host-bridge
 * registers at 00:00.0; BARs, command and MSAC belong to the integrated GPU.
 * MSAC is the byte at 0x62, not the full dword at 0x60. */
typedef struct {
    uint32_t host_id, igd_id, command;
    uint32_t ggc, bdsm, bgsm, tolud;
    uint32_t bar0_lo, bar0_hi, bar2_lo, bar2_hi;
    uint8_t msac;
} intel_ggtt_config_t;

typedef struct {
    uint64_t mmio_base, ggtt_mmio_base, ggtt_bytes, gpu_va_bytes;
    uint64_t aperture_base, aperture_bytes;
    uint64_t stolen_base, stolen_bytes, table_stolen_base;
} intel_ggtt_layout_t;

enum intel_ggtt_status {
    INTEL_GGTT_OK,
    INTEL_GGTT_BAD_ARGUMENT,
    INTEL_GGTT_BAD_DEVICE,
    INTEL_GGTT_MEMORY_DISABLED,
    INTEL_GGTT_BAD_BAR,
    INTEL_GGTT_BAD_APERTURE,
    INTEL_GGTT_BAD_GGC,
    INTEL_GGTT_DISABLED,
    INTEL_GGTT_BAD_STOLEN
};

/* Only describes firmware ranges. Success does not make any region free for
 * allocation, authorize CPU access to backing pages, or establish DMA safety.
 * All outputs below are unchanged on failure. */
enum intel_ggtt_status intel_ggtt_decode(const intel_ggtt_config_t *,
                                        intel_ggtt_layout_t *);
/* Helpers take a successfully decoded, immutable layout. The entire nonempty
 * CPU aperture range must also fit the GPU virtual address space. */
bool intel_ggtt_aperture_offset(const intel_ggtt_layout_t *, uint64_t cpu_phys,
                               uint64_t bytes, uint64_t *gpu_offset);
/* Offset relative to BAR0; never an offset into the stolen RAM itself. */
bool intel_ggtt_pte_offset(const intel_ggtt_layout_t *, uint64_t gpu_address,
                          uint32_t *bar0_offset);
/* Decode a captured PTE for one address, not a live or pinned mapping. Ignored
 * PTE bits are masked; only present and the 39-bit client address are used. */
bool intel_ggtt_decode_pte(uint64_t pte, uint64_t gpu_address,
                         uint64_t *backing_phys);

#endif
