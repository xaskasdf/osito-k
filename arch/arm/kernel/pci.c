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

#define PCI_MAX_DEVICES 32
static pci_dev_t pci_devices[PCI_MAX_DEVICES];
static int       pci_device_count;

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

    /* Check if 64-bit BAR (type bits [2:1] == 0b10) */
    int is_64bit = ((mask >> 1) & 3) == 2;

    /* Calculate size from mask (invert non-flag bits, add 1) */
    uint32_t size = ~(mask & 0xFFFFFFF0) + 1;
    if (size == 0) return 0;

    /* Align allocation to size */
    pci_mmio_next = (pci_mmio_next + size - 1) & ~((uint64_t)size - 1);
    uint64_t addr = pci_mmio_next;
    pci_mmio_next += size;

    /* Write assigned address to BAR */
    pci_ecam_write32(bus, dev, func, off, (uint32_t)addr);
    if (is_64bit) {
        /* Clear upper 32 bits (our allocations are in 32-bit MMIO window) */
        pci_ecam_write32(bus, dev, func, off + 4, 0);
    }

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
    pci_device_count = 0;

    /* ECAM + PCI MMIO already mapped by paging_init() */
    serial_puts("[PCI ] Scanning bus 0...\n");

    for (int dev = 0; dev < 32; dev++) {
        serial_puts(".");
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

        /* Store in device array (no BAR probing yet — only for known devices) */
        if (pci_device_count < PCI_MAX_DEVICES) {
            pci_dev_t *d = &pci_devices[pci_device_count];
            d->bus = 0;
            d->dev = (uint8_t)dev;
            d->func = 0;
            d->vendor_id = vendor;
            d->device_id = device;
            d->class_code = class_code;
            d->subclass = subclass;
            for (int b = 0; b < 6; b++) d->bar[b] = 0;

            /* Only probe BARs for devices we use (avoid QEMU virt hangs) */
            int probe_bars = 0;
            if (class_code == PCI_CLASS_MULTIMEDIA && subclass == PCI_SUBCLASS_HDA)
                probe_bars = 1;  /* HDA */
            if (vendor == 0x1AF4 && (device == 0x1001 || device == 0x1042))
                probe_bars = 1;  /* VirtIO-blk */

            if (probe_bars) {
                /* Read pre-assigned BARs (QEMU virt assigns during init).
                 * BAR probing (write 0xFFFFFFFF) hangs on some QEMU virt devices. */
                for (int b = 0; b < 6; b++) {
                    uint32_t bar_raw = pci_ecam_read32(0, dev, 0, PCI_BAR0 + b * 4);
                    if (bar_raw == 0) continue;
                    if (bar_raw & 1) continue;  /* skip I/O BARs */
                    int is_64bit = ((bar_raw >> 1) & 3) == 2;
                    uint64_t addr = bar_raw & 0xFFFFFFF0UL;
                    if (is_64bit && b + 1 < 6) {
                        uint32_t bar_hi = pci_ecam_read32(0, dev, 0, PCI_BAR0 + (b + 1) * 4);
                        addr |= (uint64_t)bar_hi << 32;
                    }
                    if (addr) {
                        d->bar[b] = addr;
                        paging_map_mmio(addr, 0x10000);  /* 64KB mapping */
                    }
                    if (is_64bit) {
                        b++;
                        if (b < 6) d->bar[b] = 0;
                    }
                }
                /* Don't enable here — let the driver handle it after BAR assignment */
            }

            /* Check for HDA */
            if (class_code == PCI_CLASS_MULTIMEDIA && subclass == PCI_SUBCLASS_HDA && !hda_dev) {
                hda_dev = d;
                serial_puts("[PCI ] HDA found, BAR0=");
                serial_puthex(d->bar[0], 8);
                serial_puts("\n");
            }

            pci_device_count++;
        }
    }

    if (!hda_dev)
        serial_puts("[PCI ] No HDA device found\n");
}

pci_dev_t *pci_get_hda(void)
{
    return hda_dev;
}

pci_dev_t *pci_get_device(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor && pci_devices[i].device_id == device)
            return &pci_devices[i];
    }
    return (pci_dev_t *)0;
}
