/*
 * OsitoK x86-64 — GPU Abstraction Placeholder
 *
 * Defines GPU backend types for future driver implementations.
 * Phase 0: detection only (PCI ID match).
 * Phase 2+: GSP firmware loading, command submission.
 */

#ifndef OSITOK_GPU_H
#define OSITOK_GPU_H

#include "../include/types.h"

/* GPU backend types */
typedef enum {
    GPU_BACKEND_NONE = 0,
    GPU_BACKEND_NVIDIA_GSP,     /* GSP-shim: load firmware, RM protocol */
    GPU_BACKEND_NVIDIA_CUSTOM,  /* Direct MMIO: PFIFO/PGRAPH, no GSP */
    GPU_BACKEND_VULKAN,         /* NVK path: Vulkan compute dispatch */
} gpu_backend_t;

/* NVIDIA GPU generation */
typedef enum {
    GPU_GEN_UNKNOWN = 0,
    GPU_GEN_TURING,       /* RTX 2000 series */
    GPU_GEN_AMPERE,       /* RTX 3000 series */
    GPU_GEN_ADA_LOVELACE, /* RTX 4000 series */
    GPU_GEN_BLACKWELL,    /* RTX 5000 series */
} gpu_gen_t;

/* GPU device info (populated during PCI enumeration) */
typedef struct {
    uint16_t     vendor_id;     /* 0x10DE for NVIDIA */
    uint16_t     device_id;
    gpu_gen_t    generation;
    gpu_backend_t backend;

    /* PCI BARs (physical addresses) */
    uint64_t     bar0_base;     /* MMIO registers (16MB) */
    uint64_t     bar0_size;
    uint64_t     bar1_base;     /* VRAM aperture */
    uint64_t     bar1_size;

    /* Mapped virtual addresses (after memory manager init) */
    volatile void *bar0_mapped;
    volatile void *bar1_mapped;
} gpu_device_t;

/* PCI Vendor/Device IDs */
#define PCI_VENDOR_NVIDIA  0x10DE

/* Detect GPU generation from PCI device ID */
static inline gpu_gen_t gpu_detect_gen(uint16_t device_id)
{
    uint16_t chip = device_id >> 4;
    if (chip >= 0x1E0 && chip < 0x200) return GPU_GEN_TURING;
    if (chip >= 0x220 && chip < 0x260) return GPU_GEN_AMPERE;
    if (chip >= 0x260 && chip < 0x280) return GPU_GEN_ADA_LOVELACE;
    if (chip >= 0x280 && chip < 0x2C0) return GPU_GEN_BLACKWELL;
    return GPU_GEN_UNKNOWN;
}

static inline const char *gpu_gen_name(gpu_gen_t gen)
{
    switch (gen) {
    case GPU_GEN_TURING:       return "Turing";
    case GPU_GEN_AMPERE:       return "Ampere";
    case GPU_GEN_ADA_LOVELACE: return "Ada Lovelace";
    case GPU_GEN_BLACKWELL:    return "Blackwell";
    default:                   return "Unknown";
    }
}

#endif /* OSITOK_GPU_H */
