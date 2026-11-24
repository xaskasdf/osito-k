/*
 * smmu.h -- can a device DMA 1:1 through the apps SMMU? (smmu.c)
 */

#ifndef SM8350_SMMU_H
#define SM8350_SMMU_H

#include "../../include/types.h"

/* Whether the master with stream `sid` reaches [va, va + len) at the same
 * physical address: bypass, stage-1 translation off, or an identity mapping
 * ABL left. Reports what it finds as "[SMMU] <name>: ...". */
int smmu_dma_identity(const char *name, uint32_t sid, uint64_t va, uint64_t len);

#endif
