/*
 * pci.c -- Minimal PCI ECAM scanner for QEMU virt (AArch64)
 *
 * Scans bus 0 for Intel HDA (class 0x04, subclass 0x03).
 * Reads 32-bit and 64-bit BARs, enables bus master + memory space.
 */

#include "../include/types.h"
#include "../platform/virt/virt.h"

/* ── QEMU virt PCI ECAM ──────────────────────────────────────────── */

#define VIRT_ECAM_BASE      0x4010000000ULL /* QEMU virt ECAM (256 buses, 256MB) */
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_HDA     0x03

/* PCI config space offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_CLASS_REV       0x08
#define PCI_BAR0            0x10
#define PCI_CMD_MEM_SPACE   (1 << 1)
#define PCI_CMD_BUS_MASTER  (1 << 2)

/* ── Types ────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass;
    uint64_t bar[6];
} pci_dev_t;

/* ── State ────────────────────────────────────────────────────────── */

static pci_dev_t hda_dev_store;
static pci_dev_t *hda_dev;

/* ── External ─────────────────────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern int  paging_map_mmio(uint64_t phys, uint64_t size);

/* ── ECAM config access ──────────────────────────────────────────── */

static uint32_t pci_ecam_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    uintptr_t addr = VIRT_ECAM_BASE |
                     ((uint64_t)bus << 20) |
                     ((uint64_t)dev << 15) |
                     ((uint64_t)func << 12) |
                     offset;
    return mmio_read32(addr);
}

static void pci_ecam_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val)
{
    uintptr_t addr = VIRT_ECAM_BASE |
                     ((uint64_t)bus << 20) |
                     ((uint64_t)dev << 15) |
                     ((uint64_t)func << 12) |
                     offset;
    mmio_write32(addr, val);
}

static __attribute__((unused)) uint16_t pci_ecam_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    uintptr_t addr = VIRT_ECAM_BASE |
                     ((uint64_t)bus << 20) |
                     ((uint64_t)dev << 15) |
                     ((uint64_t)func << 12) |
                     (offset & ~3U);
    uint32_t val = mmio_read32(addr);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

/* ── BAR allocation (no firmware assigns BARs on virt) ────────── */

/* PCI MMIO32 window: 0x10000000 - 0x3EFFFFFF */
#define PCI_MMIO32_BASE  0x10000000ULL
static uint64_t pci_mmio_next = PCI_MMIO32_BASE;

static uint64_t pci_assign_bar(uint8_t bus, uint8_t dev, uint8_t func, int bar_idx)
{
    uint16_t off = PCI_BAR0 + (uint16_t)(bar_idx * 4);

    /* Save original BAR and write all-ones to probe size */
    uint32_t orig = pci_ecam_read32(bus, dev, func, off);
    pci_ecam_write32(bus, dev, func, off, 0xFFFFFFFF);
    uint32_t mask = pci_ecam_read32(bus, dev, func, off);
    pci_ecam_write32(bus, dev, func, off, orig);  /* Restore */

    if (mask == 0 || mask == 0xFFFFFFFF) return 0;

    /* I/O BAR — skip */
    if (mask & 1) return 0;

    /* Calculate size from mask (invert non-flag bits, add 1) */
    uint32_t size = ~(mask & 0xFFFFFFF0) + 1;
    if (size == 0) return 0;

    /* Align allocation to size */
    pci_mmio_next = (pci_mmio_next + size - 1) & ~((uint64_t)size - 1);
    uint64_t addr = pci_mmio_next;
    pci_mmio_next += size;

    /* Write assigned address to BAR */
    pci_ecam_write32(bus, dev, func, off, (uint32_t)addr);

    /* Map the BAR region for MMIO access */
    paging_map_mmio(addr, size);

    return addr;
}

/* ── Enable bus master + memory space ────────────────────────────── */

static void pci_enable_device(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t cmd = pci_ecam_read32(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_ecam_write32(bus, dev, func, PCI_COMMAND, cmd);
}

/* ── Scan ─────────────────────────────────────────────────────────── */

void pci_scan(void)
{
    hda_dev = (pci_dev_t *)0;

    /* ECAM + PCI MMIO already mapped by paging_init() */
    serial_puts("[PCI ] Scanning bus 0...\n");

    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_ecam_read32(0, dev, 0, PCI_VENDOR_ID);
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);

        if (vendor == 0xFFFF || vendor == 0x0000)
            continue;

        uint32_t class_rev = pci_ecam_read32(0, dev, 0, PCI_CLASS_REV);
        uint8_t class_code = (class_rev >> 24) & 0xFF;
        uint8_t subclass   = (class_rev >> 16) & 0xFF;

        serial_puts("[PCI ] ");
        serial_puthex(vendor, 4);
        serial_puts(":");
        serial_puthex(device, 4);
        serial_puts(" class ");
        serial_puthex(class_code, 2);
        serial_puts(":");
        serial_puthex(subclass, 2);
        serial_puts(" @ 0:");
        serial_putdec(dev);
        serial_puts(".0\n");

        /* Check for HDA: class 0x04, subclass 0x03 */
        if (class_code == PCI_CLASS_MULTIMEDIA && subclass == PCI_SUBCLASS_HDA && !hda_dev) {
            hda_dev_store.bus = 0;
            hda_dev_store.dev = (uint8_t)dev;
            hda_dev_store.func = 0;
            hda_dev_store.vendor_id = vendor;
            hda_dev_store.device_id = device;
            hda_dev_store.class_code = class_code;
            hda_dev_store.subclass = subclass;

            /* Assign BARs (no firmware does this on virt) */
            for (int b = 0; b < 6; b++)
                hda_dev_store.bar[b] = pci_assign_bar(0, dev, 0, b);

            /* Enable bus master + memory */
            pci_enable_device(0, dev, 0);

            hda_dev = &hda_dev_store;

            serial_puts("[PCI ] HDA found, BAR0=");
            serial_puthex(hda_dev_store.bar[0], 8);
            serial_puts("\n");
        }
    }

    if (!hda_dev)
        serial_puts("[PCI ] No HDA device found\n");
}

pci_dev_t *pci_get_hda(void)
{
    return hda_dev;
}
