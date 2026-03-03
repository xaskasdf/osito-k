/*
 * OsitoK x86-64 — GPU Driver
 *
 * Phase 0: PCI detection (vendor/device ID, BAR addresses, generation).
 * Phase 1: MMIO probe (chip ID, engines, PTIMER, Falcon detect).
 * Phase 2: VRAM discovery, BAR1 read/write test, PRAMIN window.
 * Phase 3: PCI BAR sizes, gpu_write, PRAMIN window slide + R/W.
 * Phase 4+: GSP firmware loading, command submission.
 */

#ifndef OSITOK_GPU_H
#define OSITOK_GPU_H

#include "../include/types.h"

/* ── NVIDIA MMIO Register Defines (BAR0 offsets, envytools) ──── */

/* PMC — Card Master Control */
#define NV_PMC_BOOT_0          0x000000
#define NV_PMC_BOOT_42         0x0000A8
#define NV_PMC_INTR_0          0x000100
#define NV_PMC_ENABLE          0x000200

/* PTIMER — GPU Timer */
#define NV_PTIMER_TIME_0       0x009400   /* Low 32 bits (ns) */
#define NV_PTIMER_TIME_1       0x009410   /* High 32 bits (ns) */

/* PFB — Framebuffer / Memory Controller */
#define NV_PFB_CFG0            0x100C04

/* Falcon microcontroller bases (Turing+) */
#define NV_PGSP_BASE           0x110000
#define NV_PSEC_BASE           0x087000
#define NV_PPMU_BASE           0x10A000
#define NV_FALCON_HWCFG        0x000064   /* Offset within falcon base */

/* PMC_ENABLE engine bits */
#define NV_PMC_ENABLE_PGRAPH   (1 << 12)
#define NV_PMC_ENABLE_PFB      (1 << 20)
#define NV_PMC_ENABLE_PFIFO    (1 <<  8)
#define NV_PMC_ENABLE_PTIMER   (1 << 16)
#define NV_PMC_ENABLE_CE0      (1 <<  6)
#define NV_PMC_ENABLE_CE1      (1 <<  7)

/* PFB — VRAM size (Turing+, envytools) */
#define NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE  0x100CE0  /* bits 29:0 << 17 = bytes */

/* PRAMIN — Instance memory window through BAR0 */
#define NV_PRAMIN_BASE      0x700000   /* 1MB window */
#define NV_PRAMIN_SIZE      0x100000

/* PBUS — BAR0 PRAMIN window control (Fermi+, envytools) */
#define NV_PBUS_BAR0_WINDOW    0x001700   /* bits 23:0 = VRAM addr >> 16 */

/* Dead register sentinel */
#define NV_DEAD_REG            0xFFFFFFFF

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

/* ── Phase 1: MMIO Probe Results ───────────────────────────────── */

typedef struct {
    bool        present;          /* BAR0 accessible */
    uint32_t    boot0;            /* Raw PMC_BOOT_0 value */
    uint32_t    boot42;           /* Raw PMC_BOOT_42 value */
    uint32_t    chip_id;          /* bits 31:20 of boot0 */
    uint32_t    chip_rev;         /* bits 3:0 of boot0 */
    uint32_t    engines;          /* Raw PMC_ENABLE value */
    uint64_t    gpu_timer_ns;     /* PTIMER nanoseconds since power-on */
    bool        gsp_present;      /* GSP falcon detected */
    bool        sec2_present;     /* SEC2 falcon detected */
    bool        pmu_present;      /* PMU falcon detected */

    /* Phase 2: VRAM / BAR1 */
    uint32_t    vram_size_mb;      /* VRAM total in MB */
    bool        bar1_accessible;   /* BAR1 reads != 0xFFFFFFFF */
    bool        bar1_rw_ok;        /* Write/read test pattern OK */
    bool        pramin_accessible; /* PRAMIN window readable */
    bool        pramin_rw_ok;      /* PRAMIN write/read through window slide OK */
} gpu_probe_t;

/* Phase 1: Probe GPU via MMIO reads (read-only, no writes) */
int gpu_init(uint64_t bar0_phys);

/* Get probe results (valid after gpu_init succeeds) */
gpu_probe_t *gpu_get_probe(void);

#endif /* OSITOK_GPU_H */
