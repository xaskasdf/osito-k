/* Bounded native Coffee Lake GT takeover. Display registers are untouched. */
#ifndef OSITOK_INTEL_GT_H
#define OSITOK_INTEL_GT_H
#include "intel_ggtt.h"

typedef struct {
    bool (*read)(void *, uint32_t reg, uint32_t *value);
    bool (*write)(void *, uint32_t reg, uint32_t value);
    uint64_t (*now_ns)(void *);
    void (*relax)(void *);
} intel_gt_io_t;

enum intel_gt_status {
    INTEL_GT_OK, INTEL_GT_INVALID, INTEL_GT_IO, INTEL_GT_TIMEOUT,
    INTEL_GT_FIRMWARE_ACTIVE, INTEL_GT_RESTORE_FAILED
};
typedef struct {
    bool started, reset_issued, ready, cleanup_ok;
    uint8_t wake_owned, stop_changed, request_changed;
    uint32_t original_mode[4];
    uint32_t fault_reg, last_value, guc_status, dma_control;
    enum intel_gt_status status;
} intel_gt_state_t;

/* Requires exclusive kernel ownership, identity DMA already validated and
 * no other submitting driver. Qualified device: 8086:3e9b only (GT2, 4 CS).
 * State starts zeroed and must not be reused. GuC must be in reset and its
 * DMA inactive. Failure before GDRST restores only our masked request bits.
 * Once reset is issued, firmware engine state cannot be restored: retain
 * forcewake even on failure, when engine state is uncertain. The caller must
 * not bind or submit unless ready. Successful takeover also retains wake:
 * RC6/context reinitialization and submission belong to a later driver step.
 * This is not a render-ready adapter and does not change PCI BME or display. */
enum intel_gt_status intel_gt_takeover(uint32_t pci_id,
    const intel_gt_io_t *, void *, intel_gt_state_t *);
#endif
