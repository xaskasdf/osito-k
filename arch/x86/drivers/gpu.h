/*
 * OsitoK x86-64 — GPU Driver
 *
 * Phase 0: PCI detection (vendor/device ID, BAR addresses, generation).
 * Phase 1: MMIO probe (chip ID, engines, PTIMER, Falcon detect).
 * Phase 2: VRAM discovery, BAR1 read/write test, PRAMIN window.
 * Phase 3: PCI BAR sizes, gpu_write, PRAMIN window slide + R/W.
 * Phase 4: GSP Falcon deep probe, firmware load to RAM, upload to VRAM.
 * Phase 5: GSP boot (ELF parse, BOOTVEC, CPUCTL start, mailbox handshake).
 * Phase 6: GSP shared memory message queues (host↔GSP bidirectional).
 * Phase 7: RPC protocol (function IDs, poll with timeout, init sequence).
 * Phase 8: RM init commands (SET_SYSTEM_INFO, ALLOC_ROOT, etc.).
 * Phase 9: VBIOS read + BIT parse + FWSEC extraction.
 * Phase 10: FWSEC-FRTS execution + WPR2 creation.
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

/* GSP Queue Doorbell (notify GSP of new messages) */
#define NV_PGSP_QUEUE_HEAD         0x110C00

/* ── GSP Message Queue Constants ─────────────────────────────── */

#define GSP_PAGE_SIZE              4096
#define GSP_PAGE_SHIFT             12
#define GSP_MSGQ_NUM_PAGES         63       /* Entries per queue */
#define GSP_MSG_SIGNATURE          0x43505256  /* "VRPC" LE */
#define GSP_MSG_HDR_VERSION        0x03000000

/* Shared memory region offsets */
#define GSP_SHM_PTE_OFF            0x00000
#define GSP_SHM_CPUQ_HDR_OFF      0x01000
#define GSP_SHM_CPUQ_DATA_OFF     0x02000
#define GSP_SHM_GSPQ_HDR_OFF      0x41000
#define GSP_SHM_GSPQ_DATA_OFF     0x42000
#define GSP_SHM_TOTAL_SIZE         0x81000  /* ~513KB */

#define GSP_QUEUE_ARGS_VRAM_MB     126   /* Init args at VRAM+126MB */

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

/* ── GSP Message Queue Structures ─────────────────────────────── */

/* TX header — written by producer, read by consumer (32 bytes) */
typedef struct {
    uint32_t version;       /* Queue version = 1 */
    uint32_t size;          /* Total queue data size (bytes) */
    uint32_t msgSize;       /* Entry size = 4096 */
    uint32_t msgCount;      /* Number of entries = 63 */
    uint32_t writePtr;      /* Next write index (volatile) */
    uint32_t flags;         /* 0 normally */
    uint32_t rxHdrOff;      /* Offset of RX header from queue header start */
    uint32_t entryOff;      /* Offset of data entries from queue header start */
} gsp_msgq_tx_hdr_t;       /* 32 bytes */

/* RX header — written by consumer, read by producer (4 bytes + pad) */
typedef struct {
    uint32_t readPtr;       /* Next read index (volatile) */
} gsp_msgq_rx_hdr_t;

/* Message element header (48 bytes, prefixes every queue entry) */
typedef struct {
    uint8_t  authTag[16];   /* Zeros (no encryption) */
    uint8_t  aad[16];       /* Zeros (no encryption) */
    uint32_t checkSum;      /* XOR checksum (total XOR = 0) */
    uint32_t seqNum;        /* Sequence number */
    uint32_t elemCount;     /* Pages used by this message */
    uint32_t pad;
} gsp_msg_elem_hdr_t;      /* 48 bytes = 0x30 */

/* RPC message header (32 bytes, follows element header) */
typedef struct {
    uint32_t header_version; /* 0x03000000 */
    uint32_t signature;      /* 0x43505256 "VRPC" */
    uint32_t length;         /* Total length incl header */
    uint32_t function;       /* RPC function number */
    uint32_t rpc_result;     /* Status from GSP */
    uint32_t rpc_result_private;
    uint32_t sequence;       /* RPC sequence */
    uint32_t cpuRmGfid;     /* GPU function ID */
} gsp_rpc_hdr_t;            /* 32 bytes = 0x20 */

/* Message queue init arguments (passed to GSP at boot via VRAM) */
typedef struct {
    uint64_t sharedMemPhysAddr;    /* Physical addr of shared region */
    uint32_t pageTableEntryCount;  /* PTEs in first page */
    uint32_t pad;
    uint64_t cmdQueueOffset;       /* CPU queue offset = 0x1000 */
    uint64_t statQueueOffset;      /* GSP queue offset = 0x41000 */
} gsp_msgq_init_args_t;           /* 32 bytes */

/* ── GSP-RM RPC Function IDs (rpc_global_enums.h) ──────────── */

#define GSP_RPC_NOP                       0
#define GSP_RPC_SET_GUEST_SYSTEM_INFO     1
#define GSP_RPC_ALLOC_ROOT                2
#define GSP_RPC_ALLOC_DEVICE              3
#define GSP_RPC_ALLOC_MEMORY              4
#define GSP_RPC_FREE                      10
#define GSP_RPC_GET_GSP_STATIC_INFO       68
#define GSP_RPC_SET_REGISTRY              69
#define GSP_RPC_GSP_SET_SYSTEM_INFO       70
#define GSP_RPC_GSP_INIT_POST_OBJGPU      71
#define GSP_RPC_GSP_RM_CONTROL            76
#define GSP_RPC_GSP_RM_ALLOC              77
#define GSP_RPC_CONTINUATION_RECORD       0x43

/* GSP-RM Event IDs (async GSP→host) */
#define GSP_EVENT_GSP_INIT_DONE           0x80
#define GSP_EVENT_RUN_CPU_SEQUENCER       0x81
#define GSP_EVENT_POST_EVENT              0x82

/* RPC result sentinels */
#define GSP_RPC_RESULT_PENDING            0xFFFFFFFF
#define GSP_RPC_RESULT_OK                 0x00000000

/* ── GSP-RM Handle Constants ───────────────────────────────── */

#define GSP_RM_CLIENT_HANDLE     0xC1D00000
#define GSP_RM_DEVICE_HANDLE     0xDE1D0000
#define GSP_RM_SUBDEVICE_HANDLE  0x5D1D0000

/* ── GSP-RM Payload Structures (Phase 8) ──────────────────── */

/* SET_SYSTEM_INFO payload (func 70, 88 bytes) */
typedef struct {
    uint64_t gpuPhysAddr;           /* BAR0 */
    uint64_t gpuPhysFbAddr;         /* BAR1 */
    uint64_t gpuPhysInstAddr;       /* 0 */
    uint64_t nvDomainBusDeviceFunc; /* PCI BDF encoded */
    uint64_t simAccessBufPhysAddr;  /* 0 */
    uint64_t pcieAtomicsOpMask;     /* 0 */
    uint64_t consoleMemSize;        /* 0 */
    uint64_t maxUserVa;             /* (1ULL << 47) - 4096 */
    uint32_t pciConfigMirrorBase;   /* 0x088000 */
    uint32_t pciConfigMirrorSize;   /* 0x001000 */
    uint32_t PCIDeviceID;           /* (device_id << 16) | vendor_id */
    uint32_t PCISubDeviceID;        /* 0 */
    uint32_t PCIRevisionID;         /* 0 */
    uint32_t pad0;
} gsp_system_info_t;               /* 88 bytes */

/* Registry entry (for SET_REGISTRY, func 69) */
typedef struct {
    char     name[64];
    uint32_t type;      /* 1=DWORD */
    uint32_t len;       /* 4 */
    uint32_t value;
    uint32_t pad;
} gsp_registry_entry_t;            /* 76 bytes */

typedef struct {
    uint32_t numEntries;
    uint32_t pad;
    gsp_registry_entry_t entries[2];
} gsp_registry_table_t;            /* 160 bytes */

/* ALLOC_ROOT payload (func 2) */
typedef struct {
    uint32_t hClient;   /* 0xC1D00000 */
    uint32_t hClass;    /* 0x0000 NV01_ROOT */
    uint32_t processID; /* 0 */
    uint32_t pad;
} gsp_alloc_root_t;                /* 16 bytes */

/* ALLOC_DEVICE payload (func 3) */
typedef struct {
    uint32_t hClient;         /* parent */
    uint32_t hDevice;         /* 0xDE1D0000 */
    uint32_t hClass;          /* 0x0080 NV01_DEVICE */
    uint32_t pad;
    uint32_t deviceInstance;  /* 0 */
    uint32_t pad2;
} gsp_alloc_device_t;              /* 24 bytes */

/* GET_GSP_STATIC_INFO response (partial — only GPU name parsed) */
typedef struct {
    char     gpu_name[40];     /* Null-terminated GPU name string */
    /* ... many more fields (~0x6c8 bytes total, not parsed yet) ... */
} gsp_static_info_t;

/* ── VBIOS ROM Structures (Phase 9) ──────────────────────────── */

/* VBIOS ROM header (at offset 0x00 of each image) */
typedef struct {
    uint16_t signature;       /* 0xAA55 */
    uint8_t  reserved[22];
    uint16_t pcir_offset;     /* Offset to PCIR structure */
} __attribute__((packed)) vbios_rom_hdr_t;

/* PCIR structure (PCI Data Structure, PCI spec 3.0 §6.3.1.2) */
typedef struct {
    uint8_t  signature[4];    /* "PCIR" */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t device_list_off;
    uint16_t pcir_length;
    uint8_t  pcir_revision;
    uint8_t  class_code[3];
    uint16_t image_length;    /* In 512-byte units */
    uint16_t image_revision;
    uint8_t  code_type;       /* 0x00=x86, 0x03=UEFI, 0xE0=FwSec */
    uint8_t  last_image;      /* Bit 7 = last image flag */
    uint16_t max_runtime_size;
} __attribute__((packed)) vbios_pcir_t;

/* VBIOS code type constants */
#define VBIOS_CODE_TYPE_PCAT   0x00
#define VBIOS_CODE_TYPE_UEFI   0x03
#define VBIOS_CODE_TYPE_FWSEC  0xE0

/* VBIOS image descriptor */
#define VBIOS_MAX_IMAGES  8
typedef struct {
    uint32_t offset;          /* Offset within VBIOS data */
    uint32_t size;            /* Image size in bytes */
    uint8_t  code_type;       /* Code type from PCIR */
    uint16_t vendor_id;       /* PCI vendor from PCIR */
    uint16_t device_id;       /* PCI device from PCIR */
} vbios_image_t;

/* VBIOS state */
#define VBIOS_MAX_SIZE  (256 * 1024)  /* 256KB max VBIOS */
typedef struct {
    uint8_t       *data;          /* Allocated VBIOS buffer */
    uint32_t       size;          /* Total VBIOS size */
    uint32_t       image_count;
    vbios_image_t  images[VBIOS_MAX_IMAGES];
    uint32_t       fwsec_count;   /* Number of FwSec images */
    bool           valid;
} vbios_state_t;

/* ── BIT Table Structures (Phase 9) ──────────────────────────── */

/* BIT header (BIOS Information Table) */
#define BIT_SIGNATURE  0x00544942  /* "BIT\0" as uint32_t LE */
typedef struct {
    uint32_t signature;       /* "BIT\0" */
    uint16_t header_size;
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  token_count;
    uint8_t  token_entry_size;
} __attribute__((packed)) bit_header_t;

/* BIT token entry */
typedef struct {
    uint8_t  id;              /* Token ID (0x70 = Falcon Data) */
    uint8_t  data_version;
    uint16_t data_size;
    uint16_t data_offset;     /* Offset from VBIOS start */
} __attribute__((packed)) bit_token_t;

#define BIT_TOKEN_FALCON_DATA  0x70

/* Falcon ucode table entry (pointed to by token 0x70) */
typedef struct {
    uint8_t  version;
    uint8_t  header_size;
    uint8_t  entry_size;
    uint8_t  entry_count;
    uint8_t  desc_version;
    uint8_t  desc_size;
} __attribute__((packed)) falcon_ucode_table_hdr_t;

/* Falcon ucode descriptor (follows table header) */
typedef struct {
    uint32_t stored_size;     /* Compressed size in VBIOS */
    uint32_t uncompressed_size;
    uint32_t vbios_offset;    /* Offset into VBIOS data */
    uint8_t  application_id;  /* 0x01 = FWSEC */
    uint8_t  target_id;       /* Falcon target: 0x01=PMU, 0x03=GSP, 0x04=SEC2 */
    uint8_t  flags;
    uint8_t  pad;
} __attribute__((packed)) falcon_ucode_desc_t;

#define FALCON_APP_FWSEC  0x01
#define FALCON_TARGET_GSP 0x03

/* FWSEC state */
typedef struct {
    uint8_t  *data;           /* Pointer into VBIOS buffer (not separately allocated) */
    uint32_t  size;           /* FWSEC blob size */
    uint32_t  vbios_offset;   /* Offset within VBIOS */
    uint8_t   target_id;      /* Falcon target */
    bool      found;
} fwsec_state_t;

/* ── FWSEC-FRTS / WPR2 Structures (Phase 10) ────────────────── */

/* GspFwWprMeta — WPR2 metadata structure in VRAM */
typedef struct {
    uint32_t magic;                /* 0x57505232 "WPR2" */
    uint32_t revision;             /* Structure revision */
    uint64_t sysmemAddrOfRadix3Elf;
    uint32_t sizeOfRadix3Elf;
    uint32_t pad0;
    uint64_t sysmemAddrOfBootloader;
    uint32_t sizeOfBootloader;
    uint32_t bootloaderCodeOffset;
    uint32_t bootloaderDataOffset;
    uint32_t pad1;
    uint32_t nonWprHeapOffset;
    uint32_t nonWprHeapSize;
    uint64_t gspFwRsvdStart;
    uint64_t gspFwWprEnd;
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;
    uint32_t bootCount;
    uint32_t pad2;
} gsp_fw_wpr_meta_t;

#define WPR2_MAGIC  0x57505232  /* "WPR2" */

/* FWSEC command defines */
#define FWSEC_FRTS_CMD         0x15  /* FRTS = Falcon Recovery Table Setup */
#define FWSEC_SB_CMD           0x16  /* Secure Boot command */

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

    /* PCI Bus/Device/Function */
    uint8_t      pci_bus;
    uint8_t      pci_dev;
    uint8_t      pci_func;
    uint8_t      pci_pad;

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
    /* Message Queues (Phase 6) */
    void       *shm_base;         /* Shared memory region (513KB) */
    uint64_t    shm_phys;         /* Physical address */
    uint32_t    cmd_seq;          /* Command sequence counter */
    uint32_t    rpc_seq;          /* RPC sequence counter */
    bool        queues_ready;     /* Queues initialized */
    /* RPC Protocol (Phase 7) */
    char        gpu_name[40];     /* From GET_GSP_STATIC_INFO */
    bool        rpc_ready;        /* INIT_DONE received */
    /* RM Init (Phase 8) */
    bool        rm_init_done;     /* RM init sequence completed */
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

/* Phase 6: GSP message queues */
int  gsp_queue_init(void);      /* Allocate + init shared memory */
int  gsp_queue_send(uint32_t function, const void *payload, uint32_t len);
int  gsp_queue_recv(void *buf, uint32_t buf_size,
                    uint32_t *function, uint32_t *rpc_result);

/* Phase 7: RPC protocol */
int  gsp_rpc_poll(uint32_t *function, uint32_t *result,
                  void *buf, uint32_t buf_size, uint32_t timeout_ms);
int  gsp_rpc_init(void);       /* Post-boot RPC init sequence */

/* Phase 8: RM init commands */
int  gsp_rm_init(void);        /* 5-step RM init sequence */

/* Phase 9: VBIOS read + BIT parse + FWSEC extraction */
int  gpu_read_vbios(void);           /* Read VBIOS from VRAM via PRAMIN */
int  gpu_parse_bit(void);            /* Parse BIT table, extract FWSEC */
vbios_state_t *gpu_get_vbios(void);  /* Get VBIOS state */
fwsec_state_t *gpu_get_fwsec(void);  /* Get FWSEC state */

/* Phase 10: FWSEC-FRTS execution + WPR2 creation */
int  gsp_fwsec_frts(void);    /* Load FWSEC, execute FRTS, create WPR2 */

#endif /* OSITOK_GPU_H */
