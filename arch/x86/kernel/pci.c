/*
 * OsitoK x86-64 — PCIe Enumeration (ECAM via MCFG)
 *
 * Scans all PCIe buses using ECAM (Enhanced Configuration Access Mechanism).
 * MCFG ACPI table provides the base address for memory-mapped config space.
 * Falls back to legacy I/O port access (CF8/CFC) if MCFG unavailable.
 */

#include "../include/types.h"
#include "../drivers/gpu.h"

/* ── Declarations ────────────────────────────────────────────── */

extern int paging_map_mmio(uint64_t phys, uint64_t size);

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puthex(uint64_t val, int digits);
extern void fb_putdec(uint64_t val);
extern void fb_putc(char c, uint32_t color);

/* ── ACPI Tables ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    uint64_t base_addr;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} mcfg_entry_t;

/* ── PCI Config ──────────────────────────────────────────────── */

#define PCI_CONFIG_PORT  0xCF8
#define PCI_DATA_PORT    0xCFC

/* PCI class codes we care about */
#define PCI_CLASS_STORAGE    0x01
#define PCI_CLASS_NETWORK    0x02
#define PCI_CLASS_DISPLAY    0x03
#define PCI_SUBCLASS_VGA     0x00
#define PCI_SUBCLASS_NVME    0x08
#define PCI_SUBCLASS_ETHERNET 0x00

/* ECAM state */
static uint64_t ecam_base;
static uint8_t  ecam_start_bus;
static uint8_t  ecam_end_bus;

/* Detected devices */
#define MAX_PCI_DEVICES 64

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass;
    uint64_t bar[6];
} pci_dev_t;

static pci_dev_t pci_devices[MAX_PCI_DEVICES];
static int       pci_device_count;

/* GPU, NVMe, NIC, and xHCI devices (set during scan) */
gpu_device_t gpu_dev;
static pci_dev_t nvme_dev_store, nic_dev_store, xhci_dev_store;
static pci_dev_t *nvme_dev;
static pci_dev_t *nic_dev;
static pci_dev_t *xhci_dev;

/* ── Legacy PCI I/O access ───────────────────────────────────── */

static uint32_t pci_io_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1 << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_PORT, addr);
    return inl(PCI_DATA_PORT);
}

/* ── ECAM access ─────────────────────────────────────────────── */

static volatile void *ecam_addr(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    uint64_t addr = ecam_base +
                    (((uint64_t)(bus - ecam_start_bus) << 20) |
                     ((uint64_t)dev << 15) |
                     ((uint64_t)func << 12) |
                     offset);
    return (volatile void *)addr;
}

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    if (ecam_base)
        return mmio_read32(ecam_addr(bus, dev, func, offset));
    return pci_io_read32(bus, dev, func, (uint8_t)offset);
}

static uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    if (ecam_base) {
        volatile uint16_t *p = (volatile uint16_t *)ecam_addr(bus, dev, func, offset);
        return *p;
    }
    uint32_t val = pci_io_read32(bus, dev, func, (uint8_t)(offset & 0xFC));
    return (uint16_t)(val >> ((offset & 2) * 8));
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val)
{
    if (ecam_base)
        mmio_write32(ecam_addr(bus, dev, func, offset), val);
    else {
        uint32_t addr = (1 << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                        ((uint32_t)func << 8) | (offset & 0xFC);
        outl(PCI_CONFIG_PORT, addr);
        outl(PCI_DATA_PORT, val);
    }
}

/* ── Read PCI BAR ────────────────────────────────────────────── */

static uint64_t pci_read_bar(uint8_t bus, uint8_t dev, uint8_t func, int bar_idx)
{
    uint16_t offset = (uint16_t)(0x10 + bar_idx * 4);
    uint32_t lo = pci_read32(bus, dev, func, offset);

    if (lo == 0) return 0;

    /* Check if 64-bit BAR */
    if ((lo & 0x6) == 0x4) {
        uint32_t hi = pci_read32(bus, dev, func, offset + 4);
        return ((uint64_t)hi << 32) | (lo & ~0xFULL);
    }
    return lo & ~0xFULL;
}

/* ── Read PCI BAR Size ───────────────────────────────────────── */

static uint64_t pci_read_bar_size(uint8_t bus, uint8_t dev, uint8_t func, int bar_idx)
{
    uint16_t offset = (uint16_t)(0x10 + bar_idx * 4);

    /* Save original BAR value */
    uint32_t orig_lo = pci_read32(bus, dev, func, offset);
    if (orig_lo == 0) return 0;

    bool is_64bit = ((orig_lo & 0x6) == 0x4);
    uint32_t orig_hi = 0;
    if (is_64bit)
        orig_hi = pci_read32(bus, dev, func, offset + 4);

    /* Write all-ones to get size mask */
    pci_write32(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t mask_lo = pci_read32(bus, dev, func, offset);

    uint64_t mask = mask_lo & ~0xFULL;   /* Clear type/prefetch bits */

    if (is_64bit) {
        pci_write32(bus, dev, func, offset + 4, 0xFFFFFFFF);
        uint32_t mask_hi = pci_read32(bus, dev, func, offset + 4);
        mask = ((uint64_t)mask_hi << 32) | (mask_lo & ~0xFULL);
        /* Restore high */
        pci_write32(bus, dev, func, offset + 4, orig_hi);
    }

    /* Restore original */
    pci_write32(bus, dev, func, offset, orig_lo);

    if (mask == 0) return 0;
    return (~mask) + 1;
}

/* ── Find MCFG ACPI table ────────────────────────────────────── */

/* RSDP from UEFI config table (set by main.c from boot_info) */
extern uint64_t kernel_acpi_rsdp;

static int find_mcfg(void)
{
    acpi_rsdp_t *rsdp = NULL;

    /* Try UEFI-provided RSDP first (from EFI configuration table) */
    if (kernel_acpi_rsdp) {
        /* Map RSDP page before accessing (may be in ACPI Reclaim above mapped RAM) */
        paging_map_mmio(kernel_acpi_rsdp & ~0xFFFULL, 8192);
        __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

        char *p = (char *)kernel_acpi_rsdp;
        if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
            p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
            rsdp = (acpi_rsdp_t *)kernel_acpi_rsdp;
        } else {
            fb_puts(" PCI: RSDP sig bad at ");
            fb_puthex(kernel_acpi_rsdp, 16);
            fb_puts("\n");
        }
    } else {
        fb_puts(" PCI: no UEFI RSDP\n");
    }

    /* Fallback: scan BIOS ROM area (works in QEMU/legacy BIOS) */
    if (!rsdp) {
        for (uint64_t addr = 0x000E0000; addr < 0x00100000; addr += 16) {
            char *p = (char *)addr;
            if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
                p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
                rsdp = (acpi_rsdp_t *)addr;
                break;
            }
        }
    }

    if (!rsdp) {
        serial_puts("[PCI] RSDP not found, using legacy I/O\n");
        fb_puts(" PCI: RSDP not found\n");
        return -1;
    }

    serial_puts("[PCI] RSDP found at ");
    serial_puthex((uint64_t)rsdp, 16);
    serial_puts(" rev=");
    serial_putdec(rsdp->revision);
    serial_puts("\n");
    fb_puts(" PCI: RSDP ");
    fb_puthex((uint64_t)rsdp, 16);
    fb_puts(" rev=");
    fb_putdec(rsdp->revision);
    fb_puts("\n");

    /* Use XSDT if revision >= 2 */
    acpi_sdt_header_t *root;
    int use_xsdt = (rsdp->revision >= 2 && rsdp->xsdt_addr != 0);

    if (use_xsdt)
        root = (acpi_sdt_header_t *)rsdp->xsdt_addr;
    else
        root = (acpi_sdt_header_t *)(uint64_t)rsdp->rsdt_addr;

    fb_puts(" PCI: ");
    fb_puts(use_xsdt ? "XSDT" : "RSDT");
    fb_puts(" at ");
    fb_puthex((uint64_t)root, 16);
    fb_puts("\n");

    /* Map the XSDT/RSDT pages before accessing (may be above mapped RAM) */
    paging_map_mmio((uint64_t)root & ~0xFFFULL, 16384);
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    /* Validate XSDT/RSDT signature */
    if (use_xsdt) {
        if (root->signature[0] != 'X' || root->signature[1] != 'S' ||
            root->signature[2] != 'D' || root->signature[3] != 'T') {
            fb_puts(" PCI: XSDT sig invalid!\n");
            return -1;
        }
    } else {
        if (root->signature[0] != 'R' || root->signature[1] != 'S' ||
            root->signature[2] != 'D' || root->signature[3] != 'T') {
            fb_puts(" PCI: RSDT sig invalid!\n");
            return -1;
        }
    }

    /* Search for MCFG table */
    int entry_size = use_xsdt ? 8 : 4;
    int entries = (root->length - sizeof(acpi_sdt_header_t)) / entry_size;
    uint8_t *entry_ptr = (uint8_t *)root + sizeof(acpi_sdt_header_t);

    fb_puts(" PCI: ");
    fb_putdec(entries);
    fb_puts(" ACPI tables, searching MCFG...\n");

    for (int i = 0; i < entries; i++) {
        uint64_t table_addr;
        if (use_xsdt)
            table_addr = *(uint64_t *)(entry_ptr + i * 8);
        else
            table_addr = *(uint32_t *)(entry_ptr + i * 4);

        /* Map each ACPI table page before reading its header */
        paging_map_mmio(table_addr & ~0xFFFULL, 8192);
        __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

        acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)table_addr;
        if (hdr->signature[0] == 'M' && hdr->signature[1] == 'C' &&
            hdr->signature[2] == 'F' && hdr->signature[3] == 'G') {
            /* Found MCFG */
            mcfg_entry_t *mcfg = (mcfg_entry_t *)((uint8_t *)hdr + sizeof(acpi_sdt_header_t) + 8);
            ecam_base = mcfg->base_addr;
            ecam_start_bus = mcfg->start_bus;
            ecam_end_bus = mcfg->end_bus;

            serial_puts("[PCI] MCFG: ECAM base ");
            serial_puthex(ecam_base, 16);
            serial_puts(", buses ");
            serial_putdec(ecam_start_bus);
            serial_puts("-");
            serial_putdec(ecam_end_bus);
            serial_puts("\n");
            fb_puts(" PCI: ECAM ");
            fb_puthex(ecam_base, 8);
            fb_puts(" bus ");
            fb_putdec(ecam_start_bus);
            fb_puts("-");
            fb_putdec(ecam_end_bus);
            fb_puts("\n");

            /* Map ECAM MMIO region into page tables.
             * Each bus uses 1MB of config space (32 devs × 8 funcs × 4KB).
             * Total = (end_bus - start_bus + 1) MB. */
            uint64_t ecam_size = (uint64_t)(ecam_end_bus - ecam_start_bus + 1) << 20;
            paging_map_mmio(ecam_base, ecam_size);
            /* Flush TLB so ECAM reads work immediately */
            __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

            return 0;
        }
    }

    serial_puts("[PCI] MCFG not found, using legacy I/O\n");
    fb_puts(" PCI: no MCFG found\n");
    return -1;
}

/* ── PCI Scan ────────────────────────────────────────────────── */

static void pci_add_device(uint8_t bus, uint8_t dev, uint8_t func,
                           uint16_t vendor, uint16_t device,
                           uint8_t class, uint8_t subclass)
{
    /* Always detect important devices (GPU, NVMe, NIC, xHCI) even if array full */
    pci_dev_t tmp_dev;
    pci_dev_t *d;

    if (pci_device_count < MAX_PCI_DEVICES) {
        d = &pci_devices[pci_device_count];
    } else {
        d = &tmp_dev; /* Use stack temp — still detect special devices */
    }

    d->bus = bus;
    d->dev = dev;
    d->func = func;
    d->vendor_id = vendor;
    d->device_id = device;
    d->class_code = class;
    d->subclass = subclass;

    /* Read BARs */
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read_bar(bus, dev, func, i);

    /* Check if NVIDIA GPU */
    if (vendor == PCI_VENDOR_NVIDIA && class == PCI_CLASS_DISPLAY) {
        gpu_dev.vendor_id = vendor;
        gpu_dev.device_id = device;
        gpu_dev.generation = gpu_detect_gen(device);
        gpu_dev.backend = GPU_BACKEND_NVIDIA_GSP;
        gpu_dev.bar0_base = d->bar[0];
        gpu_dev.bar1_base = d->bar[1];
        gpu_dev.pci_bus  = bus;
        gpu_dev.pci_dev  = dev;
        gpu_dev.pci_func = func;
        gpu_dev.bar0_size = pci_read_bar_size(bus, dev, func, 0);
        gpu_dev.bar1_size = pci_read_bar_size(bus, dev, func, 1);

        serial_puts("[PCI] GPU BAR0: ");
        serial_putdec(gpu_dev.bar0_size >> 20);
        serial_puts(" MB, BAR1: ");
        serial_putdec(gpu_dev.bar1_size >> 20);
        serial_puts(" MB\n");
    }

    /* Check if NVMe controller */
    if (class == PCI_CLASS_STORAGE && subclass == PCI_SUBCLASS_NVME) {
        nvme_dev_store = *d;
        nvme_dev = &nvme_dev_store;
    }

    /* Check if Ethernet NIC */
    if (class == PCI_CLASS_NETWORK && subclass == PCI_SUBCLASS_ETHERNET) {
        nic_dev_store = *d;
        nic_dev = &nic_dev_store;
    }

    /* Check if xHCI USB 3.x controller (class 0x0C, subclass 0x03, prog_if 0x30) */
    if (class == 0x0C && subclass == 0x03) {
        uint32_t reg2 = pci_read32(bus, dev, func, 8);
        uint8_t prog_if = (reg2 >> 8) & 0xFF;
        if (prog_if == 0x30 && !xhci_dev) {
            xhci_dev_store = *d;
            xhci_dev = &xhci_dev_store;
            serial_puts("[PCI] xHCI controller: ");
            serial_puthex(vendor, 4);
            serial_puts(":");
            serial_puthex(device, 4);
            serial_puts(" BAR0=");
            serial_puthex(d->bar[0], 16);
            serial_puts("\n");
        }
    }

    if (pci_device_count < MAX_PCI_DEVICES)
        pci_device_count++;
}

static const char *pci_class_name(uint8_t class, uint8_t subclass)
{
    /* NOTE: -fno-jump-tables in KCFLAGS prevents GCC from generating
     * relative jump tables that break under EFI relocation. */
    if (class == 0x01) {
        if (subclass == 0x06) return "SATA";
        if (subclass == 0x08) return "NVMe";
        return "Storage";
    }
    if (class == 0x02) return "Network";
    if (class == 0x03) return "Display";
    if (class == 0x04) return "Multimedia";
    if (class == 0x06) {
        if (subclass == 0x00) return "Host Bridge";
        if (subclass == 0x01) return "ISA Bridge";
        if (subclass == 0x04) return "PCI Bridge";
        return "Bridge";
    }
    if (class == 0x0C) {
        if (subclass == 0x03) return "USB";
        if (subclass == 0x05) return "SMBus";
        return "Serial Bus";
    }
    return "Other";
}

void pci_scan(void)
{
    pci_device_count = 0;
    memset(&gpu_dev, 0, sizeof(gpu_dev));
    nvme_dev = NULL;
    nic_dev = NULL;
    xhci_dev = NULL;

    /* Try ECAM first */
    find_mcfg();

    uint8_t max_bus = ecam_base ? ecam_end_bus : 255;

    /* Track subordinate buses from PCI bridges (for legacy scan optimization) */
    uint8_t bus_active[256];
    memset(bus_active, 0, 256);
    bus_active[0] = 1; /* Always scan bus 0 */

    serial_puts("\n[PCI] Scanning (");
    serial_puts(ecam_base ? "ECAM" : "legacy");
    serial_puts(")...\n");

    fb_puts("\n PCIe devices:\n");

    /* Two-pass scan for legacy mode: first pass finds bridges on bus 0,
     * second pass scans subordinate buses discovered from bridges.
     * With ECAM, single pass scanning all buses. */
    int pass, max_pass = ecam_base ? 1 : 2;
    for (pass = 0; pass < max_pass; pass++) {
    for (uint16_t bus = 0; bus <= max_bus; bus++) {
        /* In legacy mode, only scan buses we know about */
        if (!ecam_base && !bus_active[bus]) continue;

        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t reg0 = pci_read32((uint8_t)bus, dev, func, 0);
                uint16_t vendor = reg0 & 0xFFFF;
                uint16_t device = reg0 >> 16;

                if (vendor == 0xFFFF || vendor == 0) continue;

                uint32_t reg2 = pci_read32((uint8_t)bus, dev, func, 8);
                uint8_t class = (reg2 >> 24) & 0xFF;
                uint8_t subclass = (reg2 >> 16) & 0xFF;

                /* PCI-to-PCI bridge: record subordinate buses for next pass */
                if (class == 0x06 && subclass == 0x04 && pass == 0) {
                    uint32_t busreg = pci_read32((uint8_t)bus, dev, func, 0x18);
                    uint8_t secondary = (busreg >> 8) & 0xFF;
                    uint8_t subordinate = (busreg >> 16) & 0xFF;
                    if (secondary > 0 && secondary <= max_bus) {
                        for (uint8_t b = secondary; b <= subordinate && b <= max_bus; b++)
                            bus_active[b] = 1;
                    }
                    serial_puts("  Bridge: bus ");
                    serial_putdec(secondary);
                    serial_puts("-");
                    serial_putdec(subordinate);
                    serial_puts("\n");
                }

                /* Only add devices on second pass (or first if ECAM) */
                if (pass > 0 || ecam_base) {
                    pci_add_device((uint8_t)bus, dev, func, vendor, device, class, subclass);

                    /* Print to serial */
                    serial_puts("  ");
                    serial_puthex(bus, 2);
                    serial_puts(":");
                    serial_puthex(dev, 2);
                    serial_puts(".");
                    serial_putdec(func);
                    serial_puts(" [");
                    serial_puthex(vendor, 4);
                    serial_puts(":");
                    serial_puthex(device, 4);
                    serial_puts("] ");
                    serial_puts(pci_class_name(class, subclass));
                    serial_puts("\n");

                    /* Print to framebuffer (condensed) */
                    fb_puts("  ");
                    fb_puthex(bus, 2); fb_puts(":");
                    fb_puthex(dev, 2); fb_puts(".");
                    fb_putdec(func);
                    fb_puts(" ");
                    fb_puts(pci_class_name(class, subclass));
                    if (vendor == PCI_VENDOR_NVIDIA && class == PCI_CLASS_DISPLAY) {
                        fb_puts(" (NVIDIA ");
                        fb_puts(gpu_gen_name(gpu_detect_gen(device)));
                        fb_puts(")");
                    }
                    if (class == PCI_CLASS_STORAGE && subclass == PCI_SUBCLASS_NVME)
                        fb_puts(" (NVMe)");
                    if (class == PCI_CLASS_NETWORK)
                        fb_puts(" (NIC)");
                    if (class == 0x0C && subclass == 0x03)
                        fb_puts(" (xHCI)");
                    fb_puts("\n");
                }

                /* If not multi-function, skip remaining functions */
                if (func == 0) {
                    uint32_t hdr = pci_read32((uint8_t)bus, dev, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }
    } /* end pass loop */

    serial_puts("[PCI] Found ");
    serial_putdec((uint64_t)pci_device_count);
    serial_puts(" devices\n");

    fb_puts(" PCI: ");
    fb_putdec((uint64_t)pci_device_count);
    fb_puts(" devs");
    if (gpu_dev.vendor_id) fb_puts(" GPU");
    if (nvme_dev) fb_puts(" NVMe");
    if (nic_dev) fb_puts(" NIC");
    if (xhci_dev) fb_puts(" xHCI");
    fb_puts("\n");
}

/* ── Accessors ───────────────────────────────────────────────── */

gpu_device_t *pci_get_gpu(void)
{
    return gpu_dev.vendor_id ? &gpu_dev : NULL;
}

pci_dev_t *pci_get_nvme(void)
{
    return nvme_dev;
}

pci_dev_t *pci_get_nic(void)
{
    return nic_dev;
}

pci_dev_t *pci_get_xhci(void)
{
    return xhci_dev;
}

int pci_get_device_count(void)
{
    return pci_device_count;
}

/* Public PCI config access (for drivers that need direct config reads) */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    return pci_read32(bus, dev, func, offset);
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val)
{
    pci_write32(bus, dev, func, offset, val);
}

/* ECAM status for diagnostics */
uint64_t pci_get_ecam_base(void) { return ecam_base; }
uint8_t  pci_get_ecam_end_bus(void) { return ecam_end_bus; }

/* Enable bus mastering + memory space for a PCI device */
void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_read32(bus, dev, func, 0x04);
    cmd |= (1 << 1) | (1 << 2);  /* Memory Space + Bus Master */
    pci_write32(bus, dev, func, 0x04, cmd);
}
