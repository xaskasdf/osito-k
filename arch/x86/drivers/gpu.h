/*
 * OsitoK x86-64 — GPU Driver
 *
 * Phase 0: PCI detection (vendor/device ID, BAR addresses, generation).
 * Phase 1: MMIO probe (chip ID, engines, PTIMER, Falcon detect).
 * Phase 2: VRAM discovery, BAR1 read/write test, PRAMIN window.
 * Phase 3: PCI BAR sizes, gpu_write, PRAMIN window slide + R/W.
 * Phase 4: GSP Falcon deep probe, firmware load to RAM, upload to VRAM.
 * Phase 5: GSP boot (ELF parse, BOOTVEC, CPUCTL start, mailbox handshake).
 * Phase 6+: Message queues, RPC protocol, GPU init via GSP-RM.
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

/* Falcon Microcontroller Registers (offsets from falcon base) */
#define NV_FALCON_HWCFG2          0x000068   /* IMEM/DMEM sizes */
#define NV_FALCON_HWCFG2_IMEM_MASK   0x000001FF  /* bits 8:0 * 256 = IMEM bytes */
#define NV_FALCON_HWCFG2_DMEM_SHIFT  9
#define NV_FALCON_HWCFG2_DMEM_MASK   0x0003FE00  /* bits 17:9 * 256 = DMEM bytes */

#define NV_FALCON_MAILBOX0         0x000040
#define NV_FALCON_MAILBOX1         0x000044
#define NV_FALCON_OS               0x000080
#define NV_FALCON_CPUCTL           0x000100
#define NV_FALCON_CPUCTL_STARTCPU  (1 << 1)
#define NV_FALCON_CPUCTL_HALTED    (1 << 4)
#define NV_FALCON_CPUCTL_STOPPED   (1 << 5)
#define NV_FALCON_BOOTVEC          0x000104
#define NV_FALCON_DMACTL           0x00010C
#define NV_FALCON_DMATRFBASE      0x000110
#define NV_FALCON_IMEMC            0x000180
#define NV_FALCON_IMEMD            0x000184
#define NV_FALCON_DMEMC            0x0001C0
#define NV_FALCON_DMEMD            0x0001C4

#define GSP_FW_VRAM_OFFSET_MB      128   /* Firmware placement: VRAM+128MB */

/* ── Minimal ELF64 types (for GSP firmware parsing) ────────── */

#define ELF_MAGIC       0x464C457F  /* "\x7FELF" as uint32_t LE */
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define PT_LOAD         1

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;          /* Entry point */
    uint64_t e_phoff;          /* Program header table offset */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;          /* Number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;               /* 64 bytes */

typedef struct {
    uint32_t p_type;           /* PT_LOAD = 1 */
    uint32_t p_flags;
    uint64_t p_offset;         /* Offset in file */
    uint64_t p_vaddr;          /* Virtual address */
    uint64_t p_paddr;          /* Physical address */
    uint64_t p_filesz;         /* Size in file */
    uint64_t p_memsz;          /* Size in memory */
    uint64_t p_align;
} elf64_phdr_t;               /* 56 bytes */

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

/* ── Phase 4-5: GSP Falcon State ───────────────────────────── */

typedef struct {
    /* Falcon hardware */
    uint32_t    hwcfg, hwcfg2;
    uint32_t    imem_size, dmem_size;   /* bytes */
    uint32_t    cpuctl;
    bool        halted, stopped;
    uint32_t    mailbox0, mailbox1;
    /* Firmware */
    void       *fw_data;          /* RAM buffer */
    uint64_t    fw_size;
    uint64_t    vram_offset;      /* VRAM placement */
    bool        fw_loaded;        /* In RAM */
    bool        fw_uploaded;      /* In VRAM, verified */
    /* Boot (Phase 5) */
    uint64_t    elf_entry;        /* ELF e_entry */
    uint16_t    elf_phnum;        /* Number of program headers */
    uint16_t    elf_machine;      /* e_machine (RISC-V = 0xF3) */
    uint32_t    boot_status;      /* Post-boot mailbox0 value */
    bool        booted;           /* CPUCTL_STARTCPU sent */
    bool        boot_ack;         /* Mailbox handshake OK */
} gsp_state_t;

/* ── API ────────────────────────────────────────────────────── */

/* Phase 1-3: Probe GPU via MMIO (read-only in Phase 1) */
int gpu_init(uint64_t bar0_phys);

/* Get probe results (valid after gpu_init succeeds) */
gpu_probe_t *gpu_get_probe(void);

/* GPU register access (wrappers for use by gsp.c) */
uint32_t gpu_reg_read(uint32_t reg);
void     gpu_reg_write(uint32_t reg, uint32_t val);

/* Phase 4: GSP Falcon probe + firmware loading */
int  gsp_probe(void);
int  gsp_load_firmware(void);
gsp_state_t *gsp_get_state(void);

/* Phase 5: GSP Falcon boot (ELF parse, boot sequence, mailbox poll) */
int  gsp_boot(void);

#endif /* OSITOK_GPU_H */
