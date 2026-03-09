/*
 * OsitoK x86-64 — SATA AHCI Host Controller Driver
 *
 * Register definitions, command structures, FIS types.
 * Follows Serial ATA AHCI 1.3.1 Specification.
 */

#ifndef OSITOK_AHCI_H
#define OSITOK_AHCI_H

#include "../include/types.h"

/* ── GHC (Generic Host Control) Registers ────────────────────── */

#define AHCI_REG_CAP        0x00    /* Host Capabilities */
#define AHCI_REG_GHC        0x04    /* Global Host Control */
#define AHCI_REG_IS         0x08    /* Interrupt Status */
#define AHCI_REG_PI         0x0C    /* Ports Implemented */
#define AHCI_REG_VS         0x10    /* AHCI Version */
#define AHCI_REG_CCC_CTL    0x14    /* Command Completion Coalescing Control */
#define AHCI_REG_CCC_PORTS  0x18    /* CCC Ports */
#define AHCI_REG_EM_LOC     0x1C    /* Enclosure Management Location */
#define AHCI_REG_EM_CTL     0x20    /* Enclosure Management Control */
#define AHCI_REG_CAP2       0x24    /* Host Capabilities Extended */
#define AHCI_REG_BOHC       0x28    /* BIOS/OS Handoff Control and Status */

/* GHC bits */
#define AHCI_GHC_HR         (1 << 0)    /* HBA Reset */
#define AHCI_GHC_IE         (1 << 1)    /* Interrupt Enable */
#define AHCI_GHC_AE         (1u << 31)  /* AHCI Enable */

/* CAP bits */
#define AHCI_CAP_NP(c)      ((c) & 0x1F)           /* Number of Ports (0-based) */
#define AHCI_CAP_NCS(c)     (((c) >> 8) & 0x1F)    /* Num Command Slots (0-based) */
#define AHCI_CAP_S64A(c)    (((c) >> 31) & 1)      /* Supports 64-bit Addressing */

/* BOHC bits */
#define AHCI_BOHC_BOS       (1 << 0)    /* BIOS Owned Semaphore */
#define AHCI_BOHC_OOS       (1 << 1)    /* OS Owned Semaphore */
#define AHCI_BOHC_BB        (1 << 4)    /* BIOS Busy */

/* ── Port Registers (BAR5 + 0x100 + port * 0x80) ────────────── */

#define AHCI_PORT_BASE      0x100
#define AHCI_PORT_SIZE      0x80

#define AHCI_PxCLB          0x00    /* Command List Base Address (lo) */
#define AHCI_PxCLBU         0x04    /* Command List Base Address (hi) */
#define AHCI_PxFB           0x08    /* FIS Base Address (lo) */
#define AHCI_PxFBU          0x0C    /* FIS Base Address (hi) */
#define AHCI_PxIS           0x10    /* Interrupt Status */
#define AHCI_PxIE           0x14    /* Interrupt Enable */
#define AHCI_PxCMD          0x18    /* Command and Status */
#define AHCI_PxTFD          0x20    /* Task File Data */
#define AHCI_PxSIG          0x24    /* Signature */
#define AHCI_PxSSTS         0x28    /* SATA Status (SCR0: SStatus) */
#define AHCI_PxSCTL         0x2C    /* SATA Control (SCR2: SControl) */
#define AHCI_PxSERR         0x30    /* SATA Error (SCR1: SError) */
#define AHCI_PxSACT         0x34    /* SATA Active */
#define AHCI_PxCI           0x38    /* Command Issue */
#define AHCI_PxSNTF         0x3C    /* SATA Notification */

/* PxCMD bits */
#define AHCI_CMD_ST         (1 << 0)     /* Start */
#define AHCI_CMD_SUD        (1 << 1)     /* Spin-Up Device */
#define AHCI_CMD_POD        (1 << 2)     /* Power On Device */
#define AHCI_CMD_FRE        (1 << 4)     /* FIS Receive Enable */
#define AHCI_CMD_FR         (1 << 14)    /* FIS Receive Running */
#define AHCI_CMD_CR         (1 << 15)    /* Command List Running */

/* PxTFD bits */
#define AHCI_TFD_ERR        (1 << 0)     /* Error */
#define AHCI_TFD_DRQ        (1 << 3)     /* Data Request */
#define AHCI_TFD_BSY        (1 << 7)     /* Busy */

/* PxSSTS fields */
#define AHCI_SSTS_DET(s)    ((s) & 0x0F)           /* Device Detection */
#define AHCI_SSTS_IPM(s)    (((s) >> 8) & 0x0F)    /* Interface Power Management */

#define AHCI_SSTS_DET_PRESENT  3    /* Device present + phy comm established */
#define AHCI_SSTS_IPM_ACTIVE   1    /* Interface active */

/* PxSIG values */
#define AHCI_SIG_SATA       0x00000101  /* SATA drive */
#define AHCI_SIG_SATAPI     0xEB140101  /* SATAPI drive */
#define AHCI_SIG_SEMB       0xC33C0101  /* Enclosure management bridge */
#define AHCI_SIG_PM         0x96690101  /* Port multiplier */

/* ── FIS Types ───────────────────────────────────────────────── */

#define FIS_TYPE_REG_H2D    0x27    /* Register FIS — Host to Device */
#define FIS_TYPE_REG_D2H    0x34    /* Register FIS — Device to Host */
#define FIS_TYPE_DMA_ACT    0x39    /* DMA Activate FIS */
#define FIS_TYPE_DMA_SETUP  0x41    /* DMA Setup FIS */
#define FIS_TYPE_DATA       0x46    /* Data FIS */
#define FIS_TYPE_BIST       0x58    /* BIST Activate FIS */
#define FIS_TYPE_PIO_SETUP  0x5F    /* PIO Setup FIS */
#define FIS_TYPE_SET_DEVBITS 0xA1   /* Set Device Bits FIS */

/* ── Register FIS — Host to Device (20 bytes) ───────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  fis_type;      /* FIS_TYPE_REG_H2D (0x27) */
    uint8_t  pmport_c;      /* [7:4] PM Port, [7] C bit (1=command, 0=control) */
    uint8_t  command;        /* ATA command */
    uint8_t  feature_lo;     /* Feature (7:0) */

    uint8_t  lba0;           /* LBA (7:0) */
    uint8_t  lba1;           /* LBA (15:8) */
    uint8_t  lba2;           /* LBA (23:16) */
    uint8_t  device;         /* Device register */

    uint8_t  lba3;           /* LBA (31:24) */
    uint8_t  lba4;           /* LBA (39:32) */
    uint8_t  lba5;           /* LBA (47:40) */
    uint8_t  feature_hi;     /* Feature (15:8) */

    uint16_t count;          /* Sector count */
    uint8_t  icc;            /* Isochronous command completion */
    uint8_t  control;        /* Control register */

    uint32_t reserved;
} fis_reg_h2d_t;

_Static_assert(sizeof(fis_reg_h2d_t) == 20, "H2D FIS must be 20 bytes");

/* ── ATA Commands ────────────────────────────────────────────── */

#define ATA_CMD_IDENTIFY        0xEC    /* Identify Device */
#define ATA_CMD_IDENTIFY_PACKET 0xA1    /* Identify Packet Device (SATAPI) */
#define ATA_CMD_READ_DMA_EX     0x25    /* Read DMA Extended (48-bit LBA) */
#define ATA_CMD_WRITE_DMA_EX    0x35    /* Write DMA Extended (48-bit LBA) */
#define ATA_CMD_FLUSH_CACHE_EX  0xEA    /* Flush Cache Extended */

#define ATA_DEV_LBA             0x40    /* LBA mode bit in device register */

/* ── Command Header (32 bytes) — up to 32 per command list ─── */

typedef struct __attribute__((packed)) {
    uint16_t flags;          /* [4:0] CFL (Command FIS Length in DWORDs),
                                [5] A (ATAPI), [6] W (Write), [7] P (Prefetchable),
                                [8] R (Reset), [9] B (BIST), [10] C (Clear BSY),
                                [15:12] PMP (Port Multiplier Port) */
    uint16_t prdtl;         /* Physical Region Descriptor Table Length (entries) */
    uint32_t prdbc;         /* PRD Byte Count (updated by HBA on completion) */
    uint32_t ctba;          /* Command Table Descriptor Base Address (lo) */
    uint32_t ctbau;         /* Command Table Descriptor Base Address (hi) */
    uint32_t reserved[4];
} ahci_cmd_hdr_t;

_Static_assert(sizeof(ahci_cmd_hdr_t) == 32, "Command Header must be 32 bytes");

/* Command Header flags helpers */
#define AHCI_CMD_HDR_CFL(n)  ((n) & 0x1F)          /* FIS length in DWORDs */
#define AHCI_CMD_HDR_A       (1 << 5)               /* ATAPI */
#define AHCI_CMD_HDR_W       (1 << 6)               /* Write direction */
#define AHCI_CMD_HDR_P       (1 << 7)               /* Prefetchable */
#define AHCI_CMD_HDR_C       (1 << 10)              /* Clear BSY upon R_OK */

/* ── PRDT Entry (16 bytes) ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t dba;            /* Data Base Address (lo) — 2-byte aligned */
    uint32_t dbau;           /* Data Base Address (hi) */
    uint32_t reserved;
    uint32_t dbc;            /* Data Byte Count (bit 31 = Interrupt on Completion)
                                [21:0] byte count (0-based, must be odd = even+1)
                                max 4MB (0x3FFFFF) per entry */
} ahci_prdt_entry_t;

_Static_assert(sizeof(ahci_prdt_entry_t) == 16, "PRDT entry must be 16 bytes");

#define AHCI_PRDT_DBC_MAX    0x400000   /* 4MB per PRDT entry (actually 4M-1) */
#define AHCI_PRDT_IOC        (1u << 31) /* Interrupt on Completion */

/* ── Command Table ──────────────────────────────────────────── */

/* Command Table layout:
 *   [0x00 - 0x3F]  Command FIS (64 bytes max, padded)
 *   [0x40 - 0x4F]  ATAPI Command (16 bytes)
 *   [0x50 - 0x7F]  Reserved (48 bytes)
 *   [0x80 - ...]   PRDT entries (16 bytes each)
 */

#define AHCI_CMD_TBL_HDR_SIZE   0x80    /* CFIS + ACMD + reserved */
#define AHCI_MAX_PRDT_ENTRIES   8       /* Max PRDT entries per command */

typedef struct __attribute__((packed)) {
    uint8_t            cfis[64];        /* Command FIS */
    uint8_t            acmd[16];        /* ATAPI command */
    uint8_t            reserved[48];
    ahci_prdt_entry_t  prdt[AHCI_MAX_PRDT_ENTRIES];
} ahci_cmd_tbl_t;

/* ── Received FIS Structure (256 bytes) ─────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  dsfis[28];     /* DMA Setup FIS */
    uint8_t  pad0[4];
    uint8_t  psfis[20];     /* PIO Setup FIS */
    uint8_t  pad1[12];
    uint8_t  rfis[20];      /* D2H Register FIS */
    uint8_t  pad2[4];
    uint8_t  sdbfis[8];     /* Set Device Bits FIS */
    uint8_t  ufis[64];      /* Unknown FIS */
    uint8_t  reserved[96];
} ahci_recv_fis_t;

_Static_assert(sizeof(ahci_recv_fis_t) == 256, "Received FIS must be 256 bytes");

/* ── Driver Limits ───────────────────────────────────────────── */

#define AHCI_MAX_PORTS          8
#define AHCI_MAX_CMD_SLOTS      32
#define AHCI_SECTOR_SIZE        512

/* Max sectors per single command (128 sectors = 64KB per PRDT entry,
   8 PRDT entries = 512KB total) */
#define AHCI_MAX_SECTORS_PER_CMD  (AHCI_MAX_PRDT_ENTRIES * 128)

/* ── Device Types ────────────────────────────────────────────── */

#define AHCI_DEV_NONE       0
#define AHCI_DEV_SATA       1
#define AHCI_DEV_SATAPI     2

/* ── Device State ────────────────────────────────────────────── */

typedef struct {
    uint8_t     port;           /* Physical port number */
    uint8_t     type;           /* AHCI_DEV_SATA / AHCI_DEV_SATAPI / AHCI_DEV_NONE */
    bool        present;

    uint64_t    sector_count;   /* Total sectors (from IDENTIFY) */
    uint32_t    sector_size;    /* Bytes per sector (usually 512) */

    char        model[41];      /* Model string (40 chars + NUL) */
    char        serial[21];     /* Serial number (20 chars + NUL) */

    /* DMA structures (physically contiguous) */
    ahci_cmd_hdr_t  *cmd_list;  /* 32 Command Headers (1KB aligned) */
    ahci_recv_fis_t *recv_fis;  /* Received FIS buffer (256B aligned) */
    ahci_cmd_tbl_t  *cmd_tbl;   /* Command Table for slot 0 (128B aligned) */
} ahci_device_t;

/* ── Public API ──────────────────────────────────────────────── */

int  ahci_init(uint64_t bar5_phys);
int  ahci_read(uint32_t port, uint64_t lba, uint32_t count, void *buf);
int  ahci_write(uint32_t port, uint64_t lba, uint32_t count, const void *buf);
bool ahci_is_ready(void);

/* Accessors */
uint32_t ahci_port_count(void);
uint64_t ahci_port_sectors(uint32_t port);
const char *ahci_port_model(uint32_t port);

#endif /* OSITOK_AHCI_H */
