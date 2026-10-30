/* Gen9 BCS context-switch/error MSI delivery. Kernel device owner only. */
#ifndef OSITOK_INTEL_BCS_IRQ_H
#define OSITOK_INTEL_BCS_IRQ_H
#include "intel_gt.h"
#define INTEL_BCS_IRQ_VECTOR 43u
#define INTEL_BCS_IRQ_BITS 0x01080000u
typedef struct {
    intel_gt_io_t mmio;
    bool (*cfg_read)(void *,uint16_t,uint32_t *);
    bool (*cfg_write)(void *,uint16_t,unsigned,uint32_t);
} intel_bcs_irq_io_t;
typedef struct intel_bcs_irq {
    struct intel_bcs_irq *self;
    intel_bcs_irq_io_t io;
    void *context, *wake_context;
    void (*wake)(void *,bool fault);
    uint32_t imr,ier,ring,addr_lo,addr_hi;
    uint16_t command,control,data,cap,data_offset;
    uint32_t calls,events,spurious,fault_reg,last_value,open_stage;
    bool started,armed,closed,failed;
} intel_bcs_irq_t;
/* All calls serialized on BSP with IRQs disabled; IRQ handler calls handle.
 * Open requires exclusive 8086:3e9b ownership, disabled master/MSI, no prior
 * GT0 enables, validated DMA domain/path and pinned MMIO/forcewake/resources.
 * BME is leased for MSI delivery. An already active IRQ owner is refused.
 * Unknown interrupt categories fail closed, not acknowledged as BCS work.
 * Only command/status-safe PCI widths are used. PVM MSI is not supported.
 * Stable sink/storage remain alive until close returns true. Failure after
 * the first write retains started state and requires close before reuse. */
bool intel_bcs_irq_open(intel_bcs_irq_t *,const intel_bcs_irq_io_t *,void *,
    uint32_t bsp_apic,void (*wake)(void *,bool),void *wake_context);
/* Mask delivery, retaining BME and all configuration/sink state. Then stop
 * and join the GPU service. Only after engine retirement may close restore
 * the original command register (which may revoke bus mastering). */
bool intel_bcs_irq_quiesce(intel_bcs_irq_t *);
bool intel_bcs_irq_close(intel_bcs_irq_t *);
void intel_bcs_irq_handle(intel_bcs_irq_t *);
#endif
