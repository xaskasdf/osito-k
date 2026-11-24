#ifndef OSITOK_DMA_DIAG_H
#define OSITOK_DMA_DIAG_H

#include "pci.h"

#define DMA_DIAG_PATH 4
#define DMA_DIAG_FAULTS 16

typedef struct {
    const char *note;
    uint32_t id, command_status, device, link;
    uint32_t unc_status, unc_mask, severity, cor_status, cor_mask, control;
    uint32_t header[4], root_command, root_status, source;
    uint16_t aer_offset;
    bool valid, pcie_valid, root, changed;
} dma_aer_sample_t;

typedef struct {
    uint64_t low, high;
    uint16_t index;
    bool target;
} dma_fault_record_t;

typedef struct {
    const char *note;
    uint64_t root;
    uint32_t gsts, fsts, fsts_after, pmen, low_base, low_limit;
    uint64_t high_base, high_limit;
    unsigned checked, active, target, stored, unstable;
    bool valid, changed;
    dma_fault_record_t faults[DMA_DIAG_FAULTS];
} dma_iommu_sample_t;

typedef struct {
    bool captured;
    uint64_t start_ns, end_ns;
    dma_aer_sample_t aer[DMA_DIAG_PATH];
    dma_iommu_sample_t iommu;
} dma_diag_sample_t;

typedef struct {
    bool prepared, iommu_ready;
    const char *path_note, *dmar_note;
    uint16_t segment, requester;
    unsigned path_count, units_seen;
    pci_dev_t path[DMA_DIAG_PATH];
    uint64_t table, base, cap, ecap;
    uint32_t table_length, version, fault_offset, fault_regs;
    uint8_t host_width, table_flags, unit_flags, unit_size;
    dma_diag_sample_t sample[2]; /* before device preparation, before shutdown */
} dma_diag_t;

/* Explicit diagnostic only. Preparation maps validated firmware-described
 * ranges before device launch; capture does not map, allocate or write HW.
 * No error acknowledgement, translation/ASPM changes, or page-table walking. */
void dma_diag_prepare(const pci_dev_t *device, dma_diag_t *report);
void dma_diag_capture(dma_diag_t *report, unsigned sample);

#endif
