#ifndef OSITOK_X86_PCI_H
#define OSITOK_X86_PCI_H

#include "types.h"

typedef struct {
    uint8_t bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass;
    uint64_t bar[6];
} pci_dev_t;

/* Currently recognizes the discrete Intel AX200 (not AX201/CNVi). */
const pci_dev_t *pci_get_wlan(void);
bool pci_cfg_write8(uint8_t bus, uint8_t dev, uint8_t func,
                    uint16_t offset, uint8_t value);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t offset, uint16_t value);
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t offset, uint32_t value);

/* Strict, read-only diagnostics. Never alias extended config onto CF8/CFC. */
bool pci_diag_read32(uint8_t bus, uint8_t dev, uint8_t func,
                     uint16_t offset, uint32_t *value);
uint16_t pci_diag_segment(uint8_t bus);
/* 1: direct parent, 0: root/no parent, -1: incomplete/ambiguous topology. */
int pci_diag_parent(const pci_dev_t *child, pci_dev_t *parent);

#define PCI_DMA_PATH_MAX 32
typedef struct {
    uint8_t bus, dev, func;
    uint32_t id, buses;
    uint16_t before, after;
    bool written, verified;
} pci_dma_bridge_t;
typedef struct {
    const char *error;
    uint16_t segment;
    unsigned count;
    bool attempted, ready;
    pci_dma_bridge_t bridge[PCI_DMA_PATH_MAX]; /* nearest endpoint first */
} pci_dma_path_t;

/* Caller owns a quiescent, enumerated endpoint with BME clear and has prepared
 * its DMA buffers. Validate the whole firmware-configured path, then enable
 * upstream bridge BME root-first. Does not enable endpoint DMA or repair BARs,
 * bus numbers, decode windows, power or IOMMU state. NULL means success.
 * Bridges are bus-owned infrastructure: their BME stays enabled even on a
 * partial failure, never restored by endpoint stop (other children may use it).
 * Concurrent preparations return busy. No hotplug/rescan during this call. */
const char *pci_prepare_dma_path(const pci_dev_t *endpoint, pci_dma_path_t *report);

/* Irreversible kexec commit: peers stopped, interrupts disabled, disk flushed.
 * Stop new DMA and wait for outstanding PCIe transactions with a finite bound. */
bool pci_kexec_quiesce(void);

#endif
