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
#define MAX_PCI_DEVICES 32

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass;
    uint64_t bar[6];
} pci_dev_t;

static pci_dev_t pci_devices[MAX_PCI_DEVICES];
static int       pci_device_count;

/* GPU, NVMe, and NIC device pointers (set during scan) */
gpu_device_t gpu_dev;
static pci_dev_t *nvme_dev;
static pci_dev_t *nic_dev;

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

static int find_mcfg(void)
{
    /* Scan for RSDP in EBDA and BIOS area */
    /* For UEFI systems, RSDP is typically passed via EFI config table */
    /* We'll use the EFI System Table ACPI configuration table GUID */

    /* For now, try scanning well-known locations */
    const uint64_t scan_regions[][2] = {
        { 0x000E0000, 0x00100000 },  /* BIOS ROM area */
    };

    acpi_rsdp_t *rsdp = NULL;
    for (int r = 0; r < 1; r++) {
        for (uint64_t addr = scan_regions[r][0]; addr < scan_regions[r][1]; addr += 16) {
            char *p = (char *)addr;
            if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' &&
                p[4] == 'P' && p[5] == 'T' && p[6] == 'R' && p[7] == ' ') {
                rsdp = (acpi_rsdp_t *)addr;
                break;
            }
        }
        if (rsdp) break;
    }

    if (!rsdp) {
        serial_puts("[PCI] RSDP not found, using legacy I/O\n");
        return -1;
    }

    serial_puts("[PCI] RSDP found at ");
    serial_puthex((uint64_t)rsdp, 16);
    serial_puts("\n");

    /* Use XSDT if revision >= 2 */
    acpi_sdt_header_t *root;
    int use_xsdt = (rsdp->revision >= 2 && rsdp->xsdt_addr != 0);

    if (use_xsdt)
        root = (acpi_sdt_header_t *)rsdp->xsdt_addr;
    else
        root = (acpi_sdt_header_t *)(uint64_t)rsdp->rsdt_addr;

    /* Search for MCFG table */
    int entry_size = use_xsdt ? 8 : 4;
    int entries = (root->length - sizeof(acpi_sdt_header_t)) / entry_size;
    uint8_t *entry_ptr = (uint8_t *)root + sizeof(acpi_sdt_header_t);

    for (int i = 0; i < entries; i++) {
        uint64_t table_addr;
        if (use_xsdt)
            table_addr = *(uint64_t *)(entry_ptr + i * 8);
        else
            table_addr = *(uint32_t *)(entry_ptr + i * 4);

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
            return 0;
        }
    }

    serial_puts("[PCI] MCFG not found, using legacy I/O\n");
    return -1;
}

/* ── PCI Scan ────────────────────────────────────────────────── */

static void pci_add_device(uint8_t bus, uint8_t dev, uint8_t func,
                           uint16_t vendor, uint16_t device,
                           uint8_t class, uint8_t subclass)
{
    if (pci_device_count >= MAX_PCI_DEVICES) return;

    pci_dev_t *d = &pci_devices[pci_device_count];
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
        nvme_dev = d;
    }

    /* Check if Ethernet NIC */
    if (class == PCI_CLASS_NETWORK && subclass == PCI_SUBCLASS_ETHERNET) {
        nic_dev = d;
    }

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

    /* Try ECAM first */
    find_mcfg();

    uint8_t max_bus = ecam_base ? ecam_end_bus : 7; /* Scan fewer buses in legacy mode */

    serial_puts("\n[PCI] Scanning buses 0-");
    serial_putdec(max_bus);
    serial_puts("...\n");

    fb_puts("\n PCIe devices:\n");

    for (uint16_t bus = 0; bus <= max_bus; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t reg0 = pci_read32((uint8_t)bus, dev, func, 0);
                uint16_t vendor = reg0 & 0xFFFF;
                uint16_t device = reg0 >> 16;

                if (vendor == 0xFFFF || vendor == 0) continue;

                uint32_t reg2 = pci_read32((uint8_t)bus, dev, func, 8);
                uint8_t class = (reg2 >> 24) & 0xFF;
                uint8_t subclass = (reg2 >> 16) & 0xFF;

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
                fb_puts("\n");

                /* If not multi-function, skip remaining functions */
                if (func == 0) {
                    uint32_t hdr = pci_read32((uint8_t)bus, dev, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }

    serial_puts("[PCI] Found ");
    serial_putdec((uint64_t)pci_device_count);
    serial_puts(" devices\n");
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

int pci_get_device_count(void)
{
    return pci_device_count;
}
