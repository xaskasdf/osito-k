/*
 * OsitoK x86-64 — SATA AHCI Host Controller Driver
 *
 * Supports device detection, IDENTIFY, read, write, and flush.
 * Handles SATA drives behind any AHCI controller (Intel, AMD, etc.).
 *
 * Uses polling (no IRQs) — same approach as NVMe/xHCI drivers.
 */

#include "../include/types.h"
#include "ahci.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_putc(char c);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);

/* ── State ───────────────────────────────────────────────────── */

static volatile void *ahci_base;        /* BAR5 MMIO base */
static uint32_t ahci_num_cmd_slots;     /* From CAP.NCS */
static ahci_device_t ahci_devs[AHCI_MAX_PORTS];
static uint32_t ahci_num_ports;         /* Number of discovered ports with devices */
static bool ahci_ready;

/* ── MMIO helpers ────────────────────────────────────────────── */

static uint32_t ahci_read32(uint32_t off)
{
    return mmio_read32((volatile void *)((uint64_t)ahci_base + off));
}

static void ahci_write32(uint32_t off, uint32_t val)
{
    mmio_write32((volatile void *)((uint64_t)ahci_base + off), val);
}

/* Port register helpers */
static uint32_t port_off(uint32_t port, uint32_t reg)
{
    return AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg;
}

static uint32_t port_read(uint32_t port, uint32_t reg)
{
    return ahci_read32(port_off(port, reg));
}

static void port_write(uint32_t port, uint32_t reg, uint32_t val)
{
    ahci_write32(port_off(port, reg), val);
}

/* Spin delay */
static void spin(uint32_t iters)
{
    for (volatile uint32_t i = 0; i < iters; i++)
        __asm__ volatile ("pause");
}

/* ── Find a free command slot ────────────────────────────────── */

static int find_cmd_slot(uint32_t port)
{
    uint32_t slots = port_read(port, AHCI_PxSACT) | port_read(port, AHCI_PxCI);
    for (uint32_t i = 0; i < ahci_num_cmd_slots; i++) {
        if (!(slots & (1u << i)))
            return (int)i;
    }
    return -1;
}

/* ── Stop a port (clear ST and FRE, wait for CR/FR) ─────────── */

static void port_stop(uint32_t port)
{
    uint32_t cmd = port_read(port, AHCI_PxCMD);

    /* Clear ST (Command Start) */
    if (cmd & AHCI_CMD_ST) {
        cmd &= ~AHCI_CMD_ST;
        port_write(port, AHCI_PxCMD, cmd);
    }

    /* Wait for CR (Command List Running) to clear */
    for (int t = 0; t < 500000; t++) {
        if (!(port_read(port, AHCI_PxCMD) & AHCI_CMD_CR))
            break;
        __asm__ volatile ("pause");
    }

    /* Clear FRE (FIS Receive Enable) */
    cmd = port_read(port, AHCI_PxCMD);
    if (cmd & AHCI_CMD_FRE) {
        cmd &= ~AHCI_CMD_FRE;
        port_write(port, AHCI_PxCMD, cmd);
    }

    /* Wait for FR (FIS Receive Running) to clear */
    for (int t = 0; t < 500000; t++) {
        if (!(port_read(port, AHCI_PxCMD) & AHCI_CMD_FR))
            break;
        __asm__ volatile ("pause");
    }
}

/* ── Start a port (set FRE, then ST) ────────────────────────── */

static void port_start(uint32_t port)
{
    /* Wait until CR is clear before starting */
    for (int t = 0; t < 500000; t++) {
        if (!(port_read(port, AHCI_PxCMD) & AHCI_CMD_CR))
            break;
        __asm__ volatile ("pause");
    }

    uint32_t cmd = port_read(port, AHCI_PxCMD);

    /* Enable FIS Receive first */
    cmd |= AHCI_CMD_FRE;
    port_write(port, AHCI_PxCMD, cmd);
    wmb();

    /* Then enable command processing */
    cmd |= AHCI_CMD_ST;
    port_write(port, AHCI_PxCMD, cmd);
    wmb();
}

/* ── Issue a command on a port and poll for completion ────────── */

static int port_issue_cmd(uint32_t port, uint32_t slot)
{
    /* Issue command */
    wmb();
    port_write(port, AHCI_PxCI, 1u << slot);

    /* Poll for completion */
    for (uint32_t t = 0; t < 50000000; t++) {
        uint32_t ci = port_read(port, AHCI_PxCI);
        if (!(ci & (1u << slot))) {
            /* Command completed — check for errors */
            uint32_t tfd = port_read(port, AHCI_PxTFD);
            if (tfd & (AHCI_TFD_ERR | AHCI_TFD_BSY)) {
                uint32_t is = port_read(port, AHCI_PxIS);
                serial_puts("[AHCI] Port ");
                serial_putdec(port);
                serial_puts(" error: TFD=");
                serial_puthex(tfd, 8);
                serial_puts(" IS=");
                serial_puthex(is, 8);
                serial_puts("\n");
                /* Clear error state */
                port_write(port, AHCI_PxIS, is);
                port_write(port, AHCI_PxSERR, port_read(port, AHCI_PxSERR));
                return -1;
            }
            return 0;
        }

        /* Check for fatal errors */
        uint32_t is = port_read(port, AHCI_PxIS);
        if (is & (1 << 30)) { /* TFES — Task File Error Status */
            serial_puts("[AHCI] Port ");
            serial_putdec(port);
            serial_puts(" task file error\n");
            port_write(port, AHCI_PxIS, is);
            port_write(port, AHCI_PxSERR, port_read(port, AHCI_PxSERR));
            return -1;
        }

        __asm__ volatile ("pause");
    }

    serial_puts("[AHCI] Port ");
    serial_putdec(port);
    serial_puts(" command timeout\n");
    return -1;
}

/* ── Byte-swap ATA identify strings (word-swapped ASCII) ─────── */

static void ata_string_fix(char *dst, const uint16_t *src, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    /* Trim trailing spaces */
    int len = words * 2;
    dst[len] = '\0';
    while (len > 0 && dst[len - 1] == ' ')
        dst[--len] = '\0';
}

/* ── BIOS/OS Handoff ─────────────────────────────────────────── */

static void ahci_bios_handoff(void)
{
    uint32_t bohc = ahci_read32(AHCI_REG_BOHC);
    if (!(bohc & AHCI_BOHC_BOS))
        return; /* BIOS doesn't own it */

    serial_puts("[AHCI] BIOS handoff...\n");

    /* Set OS Owned Semaphore */
    ahci_write32(AHCI_REG_BOHC, bohc | AHCI_BOHC_OOS);

    /* Wait for BIOS to release (timeout ~2s) */
    for (int t = 0; t < 200; t++) {
        spin(10000);
        bohc = ahci_read32(AHCI_REG_BOHC);
        if (!(bohc & AHCI_BOHC_BOS))
            break;
    }

    if (bohc & AHCI_BOHC_BOS) {
        serial_puts("[AHCI] BIOS handoff timeout, forcing\n");
        ahci_write32(AHCI_REG_BOHC, AHCI_BOHC_OOS);
    }

    /* Wait for BIOS Busy to clear */
    for (int t = 0; t < 200; t++) {
        spin(10000);
        bohc = ahci_read32(AHCI_REG_BOHC);
        if (!(bohc & AHCI_BOHC_BB))
            break;
    }
}

/* ── Send IDENTIFY DEVICE command ────────────────────────────── */

static int port_identify(uint32_t port, ahci_device_t *dev)
{
    /* Allocate a 512-byte DMA buffer for IDENTIFY data */
    void *id_buf = mem_alloc_aligned(512, 4096);
    if (!id_buf) return -1;
    memset(id_buf, 0, 512);

    int slot = find_cmd_slot(port);
    if (slot < 0) {
        serial_puts("[AHCI] No free command slot\n");
        return -1;
    }

    /* Build Command Header */
    ahci_cmd_hdr_t *hdr = &dev->cmd_list[slot];
    memset(hdr, 0, sizeof(*hdr));
    hdr->flags = AHCI_CMD_HDR_CFL(5) | AHCI_CMD_HDR_C;   /* 5 DWORDs, clear BSY */
    hdr->prdtl = 1;
    hdr->ctba  = (uint32_t)((uint64_t)dev->cmd_tbl & 0xFFFFFFFF);
    hdr->ctbau = (uint32_t)((uint64_t)dev->cmd_tbl >> 32);

    /* Build Command Table */
    ahci_cmd_tbl_t *tbl = dev->cmd_tbl;
    memset(tbl, 0, sizeof(*tbl));

    /* H2D FIS: IDENTIFY DEVICE */
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type   = FIS_TYPE_REG_H2D;
    fis->pmport_c   = 0x80;    /* C bit = 1 (command register update) */
    fis->command    = (dev->type == AHCI_DEV_SATAPI) ?
                       ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY;
    fis->device     = 0;

    /* PRDT: one entry pointing to id_buf */
    tbl->prdt[0].dba  = (uint32_t)((uint64_t)id_buf & 0xFFFFFFFF);
    tbl->prdt[0].dbau = (uint32_t)((uint64_t)id_buf >> 32);
    tbl->prdt[0].dbc  = 512 - 1;   /* 0-based byte count */
    wmb();

    /* Issue and wait */
    if (port_issue_cmd(port, (uint32_t)slot) < 0) {
        return -1;
    }

    /* Parse IDENTIFY data */
    uint16_t *id = (uint16_t *)id_buf;

    /* Words 27-46: Model number (40 chars, byte-swapped) */
    ata_string_fix(dev->model, &id[27], 20);

    /* Words 10-19: Serial number (20 chars, byte-swapped) */
    ata_string_fix(dev->serial, &id[10], 10);

    /* Total sectors: words 100-103 for 48-bit LBA */
    uint64_t lba48 = (uint64_t)id[100] |
                     ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[103] << 48);

    if (lba48 == 0) {
        /* Fall back to 28-bit LBA: words 60-61 */
        lba48 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    }

    dev->sector_count = lba48;
    dev->sector_size  = AHCI_SECTOR_SIZE;

    /* Check for logical sector size (word 117-118) */
    if ((id[106] & (1 << 14)) && !(id[106] & (1 << 15))) {
        /* Word 106 bit 12: Logical sector > 256 words */
        if (id[106] & (1 << 12)) {
            uint32_t lss = ((uint32_t)id[118] << 16) | id[117];
            if (lss > 0)
                dev->sector_size = lss * 2;  /* words to bytes */
        }
    }

    return 0;
}

/* ── Probe one port ──────────────────────────────────────────── */

static void probe_port(uint32_t port)
{
    uint32_t ssts = port_read(port, AHCI_PxSSTS);
    uint32_t det = AHCI_SSTS_DET(ssts);
    uint32_t ipm = AHCI_SSTS_IPM(ssts);

    /* Device present and interface active? */
    if (det != AHCI_SSTS_DET_PRESENT || ipm != AHCI_SSTS_IPM_ACTIVE)
        return;

    /* Read signature to determine device type */
    uint32_t sig = port_read(port, AHCI_PxSIG);
    uint8_t type;
    const char *type_str;

    switch (sig) {
    case AHCI_SIG_SATA:
        type = AHCI_DEV_SATA;
        type_str = "SATA";
        break;
    case AHCI_SIG_SATAPI:
        type = AHCI_DEV_SATAPI;
        type_str = "SATAPI";
        break;
    default:
        /* Unknown or not interesting — skip */
        serial_puts("[AHCI] Port ");
        serial_putdec(port);
        serial_puts(" unknown sig=");
        serial_puthex(sig, 8);
        serial_puts("\n");
        return;
    }

    serial_puts("[AHCI] Port ");
    serial_putdec(port);
    serial_puts(": ");
    serial_puts(type_str);
    serial_puts(" device detected (SSTS=");
    serial_puthex(ssts, 8);
    serial_puts(")\n");

    if (ahci_num_ports >= AHCI_MAX_PORTS) {
        serial_puts("[AHCI] Max ports exceeded, skipping\n");
        return;
    }

    ahci_device_t *dev = &ahci_devs[ahci_num_ports];
    memset(dev, 0, sizeof(*dev));
    dev->port    = (uint8_t)port;
    dev->type    = type;
    dev->present = true;

    /* ── Stop port before reconfiguring ── */
    port_stop(port);

    /* ── Allocate Command List (1KB aligned, 32 entries x 32 bytes) ── */
    dev->cmd_list = (ahci_cmd_hdr_t *)mem_alloc_aligned(1024, 1024);
    if (!dev->cmd_list) {
        serial_puts("[AHCI] Failed to allocate command list\n");
        return;
    }
    memset(dev->cmd_list, 0, 1024);

    /* ── Allocate Received FIS buffer (256 bytes, 256B aligned) ── */
    dev->recv_fis = (ahci_recv_fis_t *)mem_alloc_aligned(256, 256);
    if (!dev->recv_fis) {
        serial_puts("[AHCI] Failed to allocate FIS buffer\n");
        return;
    }
    memset(dev->recv_fis, 0, 256);

    /* ── Allocate Command Table (128B aligned) ── */
    dev->cmd_tbl = (ahci_cmd_tbl_t *)mem_alloc_aligned(sizeof(ahci_cmd_tbl_t), 128);
    if (!dev->cmd_tbl) {
        serial_puts("[AHCI] Failed to allocate command table\n");
        return;
    }
    memset(dev->cmd_tbl, 0, sizeof(ahci_cmd_tbl_t));

    /* ── Set PxCLB and PxFB ── */
    uint64_t clb_addr = (uint64_t)dev->cmd_list;
    port_write(port, AHCI_PxCLB,  (uint32_t)(clb_addr & 0xFFFFFFFF));
    port_write(port, AHCI_PxCLBU, (uint32_t)(clb_addr >> 32));

    uint64_t fb_addr = (uint64_t)dev->recv_fis;
    port_write(port, AHCI_PxFB,   (uint32_t)(fb_addr & 0xFFFFFFFF));
    port_write(port, AHCI_PxFBU,  (uint32_t)(fb_addr >> 32));
    wmb();

    /* ── Clear pending errors and interrupts ── */
    port_write(port, AHCI_PxSERR, port_read(port, AHCI_PxSERR));
    port_write(port, AHCI_PxIS,   port_read(port, AHCI_PxIS));

    /* ── Start port ── */
    port_start(port);

    /* Small settle delay */
    spin(10000);

    /* ── IDENTIFY DEVICE ── */
    if (port_identify(port, dev) < 0) {
        serial_puts("[AHCI] Port ");
        serial_putdec(port);
        serial_puts(" IDENTIFY failed\n");
        /* Device present but identification failed — keep it but mark partial */
        dev->sector_count = 0;
    }

    /* Log device info */
    serial_puts("[AHCI] Port ");
    serial_putdec(port);
    serial_puts(": ");
    serial_puts(dev->model);
    serial_puts(" (");
    serial_puts(dev->serial);
    serial_puts(") ");
    serial_putdec(dev->sector_count);
    serial_puts(" sectors = ");
    serial_putdec(dev->sector_count * dev->sector_size / (1024 * 1024 * 1024));
    serial_puts(" GB\n");

    fb_puts(" SATA: ");
    for (int i = 0; dev->model[i]; i++) {
        if (dev->model[i] >= 32 && dev->model[i] < 127) {
            char tmp[2] = { dev->model[i], 0 };
            fb_puts(tmp);
        }
    }
    fb_puts("\n");

    ahci_num_ports++;
}

/* ── Internal: perform a DMA read/write command ──────────────── */

static int ahci_dma_xfer(uint32_t port_idx, uint64_t lba, uint32_t count,
                          void *buf, bool write)
{
    if (port_idx >= ahci_num_ports) return -1;

    ahci_device_t *dev = &ahci_devs[port_idx];
    if (!dev->present) return -1;

    uint32_t phys_port = dev->port;
    uint8_t *data = (uint8_t *)buf;

    while (count > 0) {
        uint32_t this_count = count;
        if (this_count > AHCI_MAX_SECTORS_PER_CMD)
            this_count = AHCI_MAX_SECTORS_PER_CMD;

        int slot = find_cmd_slot(phys_port);
        if (slot < 0) {
            serial_puts("[AHCI] No free command slot\n");
            return -1;
        }

        /* Build PRDT entries — 128 sectors (64KB) per entry max */
        uint32_t remaining = this_count;
        uint32_t prdt_count = 0;
        uint8_t *prdt_ptr = data;

        ahci_cmd_tbl_t *tbl = dev->cmd_tbl;
        memset(tbl, 0, sizeof(*tbl));

        while (remaining > 0 && prdt_count < AHCI_MAX_PRDT_ENTRIES) {
            uint32_t chunk = remaining;
            if (chunk > 128) chunk = 128;   /* 128 sectors = 64KB */

            uint64_t phys = (uint64_t)prdt_ptr;
            tbl->prdt[prdt_count].dba  = (uint32_t)(phys & 0xFFFFFFFF);
            tbl->prdt[prdt_count].dbau = (uint32_t)(phys >> 32);
            tbl->prdt[prdt_count].dbc  = (chunk * AHCI_SECTOR_SIZE) - 1;

            prdt_ptr += chunk * AHCI_SECTOR_SIZE;
            remaining -= chunk;
            prdt_count++;
        }

        /* Last PRDT entry gets IOC */
        if (prdt_count > 0)
            tbl->prdt[prdt_count - 1].dbc |= AHCI_PRDT_IOC;

        /* Build H2D FIS */
        fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
        fis->fis_type   = FIS_TYPE_REG_H2D;
        fis->pmport_c   = 0x80;    /* C=1 (command) */
        fis->command    = write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
        fis->device     = ATA_DEV_LBA;

        /* 48-bit LBA */
        fis->lba0 = (uint8_t)(lba & 0xFF);
        fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
        fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
        fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
        fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
        fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);

        fis->count = (uint16_t)this_count;

        /* Build Command Header */
        ahci_cmd_hdr_t *hdr = &dev->cmd_list[slot];
        memset(hdr, 0, sizeof(*hdr));
        hdr->flags = AHCI_CMD_HDR_CFL(5);  /* 5 DWORDs for H2D FIS */
        if (write)
            hdr->flags |= AHCI_CMD_HDR_W;
        hdr->prdtl = (uint16_t)prdt_count;
        hdr->ctba  = (uint32_t)((uint64_t)tbl & 0xFFFFFFFF);
        hdr->ctbau = (uint32_t)((uint64_t)tbl >> 32);

        wmb();

        /* Issue and poll */
        if (port_issue_cmd(phys_port, (uint32_t)slot) < 0)
            return -1;

        data  += (uint64_t)this_count * AHCI_SECTOR_SIZE;
        lba   += this_count;
        count -= this_count;
    }

    return 0;
}

/* ── Initialize AHCI Controller ──────────────────────────────── */

int ahci_init(uint64_t bar5_phys)
{
    ahci_base = (volatile void *)bar5_phys;
    ahci_ready = false;
    ahci_num_ports = 0;

    serial_puts("[AHCI] Initializing, BAR5=");
    serial_puthex(bar5_phys, 16);
    serial_puts("\n");

    /* Read version */
    uint32_t vs = ahci_read32(AHCI_REG_VS);
    serial_puts("[AHCI] Version ");
    serial_putdec((vs >> 16) & 0xFFFF);
    serial_puts(".");
    serial_putdec(vs & 0xFFFF);
    serial_puts("\n");

    /* BIOS/OS handoff */
    ahci_bios_handoff();

    /* Read capabilities */
    uint32_t cap = ahci_read32(AHCI_REG_CAP);
    uint32_t max_ports = AHCI_CAP_NP(cap) + 1;
    ahci_num_cmd_slots = AHCI_CAP_NCS(cap) + 1;

    serial_puts("[AHCI] CAP: ");
    serial_putdec(max_ports);
    serial_puts(" ports, ");
    serial_putdec(ahci_num_cmd_slots);
    serial_puts(" cmd slots");
    if (AHCI_CAP_S64A(cap))
        serial_puts(", 64-bit");
    serial_puts("\n");

    /* Enable AHCI mode (set GHC.AE) */
    uint32_t ghc = ahci_read32(AHCI_REG_GHC);
    if (!(ghc & AHCI_GHC_AE)) {
        ghc |= AHCI_GHC_AE;
        ahci_write32(AHCI_REG_GHC, ghc);
        wmb();
        spin(10000);
    }

    /* Read Ports Implemented bitmask */
    uint32_t pi = ahci_read32(AHCI_REG_PI);
    serial_puts("[AHCI] Ports Implemented: ");
    serial_puthex(pi, 8);
    serial_puts("\n");

    /* Clear global interrupt status */
    ahci_write32(AHCI_REG_IS, ahci_read32(AHCI_REG_IS));

    /* Probe each implemented port */
    for (uint32_t p = 0; p < 32 && p < max_ports; p++) {
        if (!(pi & (1u << p)))
            continue;

        probe_port(p);
    }

    if (ahci_num_ports > 0) {
        ahci_ready = true;
        serial_puts("[AHCI] Init complete: ");
        serial_putdec(ahci_num_ports);
        serial_puts(" device(s)\n");
    } else {
        serial_puts("[AHCI] No SATA devices found\n");
    }

    return ahci_num_ports > 0 ? 0 : -1;
}

/* ── Read sectors ────────────────────────────────────────────── */

int ahci_read(uint32_t port, uint64_t lba, uint32_t count, void *buf)
{
    if (!ahci_ready) return -1;
    if (count == 0) return -1;
    return ahci_dma_xfer(port, lba, count, buf, false);
}

/* ── Write sectors ───────────────────────────────────────────── */

int ahci_write(uint32_t port, uint64_t lba, uint32_t count, const void *buf)
{
    if (!ahci_ready) return -1;
    if (count == 0) return -1;
    return ahci_dma_xfer(port, lba, count, (void *)buf, true);
}

/* ── Accessors ───────────────────────────────────────────────── */

bool ahci_is_ready(void)
{
    return ahci_ready;
}

uint32_t ahci_port_count(void)
{
    return ahci_num_ports;
}

uint64_t ahci_port_sectors(uint32_t port)
{
    if (port >= ahci_num_ports) return 0;
    return ahci_devs[port].sector_count;
}

const char *ahci_port_model(uint32_t port)
{
    if (port >= ahci_num_ports) return "";
    return ahci_devs[port].model;
}
