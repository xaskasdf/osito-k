/* Bounded execution probes, with an optional checked return-address slot. */
#ifndef OSITOK_HWBP_TRACE_H
#define OSITOK_HWBP_TRACE_H

#include "../include/types.h"

#define HWBP_TRACE_SIGNATURE_BYTES 16
#define HWBP_TRACE_MAX_HITS 32

typedef enum {
    HWBP_TRACE_EAX32,
    HWBP_TRACE_EDI32,
} hwbp_trace_reg_t;

typedef enum {
    HWBP_TRACE_IGNORE,
    HWBP_TRACE_HIT,
    HWBP_TRACE_LIMIT,
    HWBP_TRACE_MISMATCH,
} hwbp_trace_result_t;

typedef struct {
    uint64_t addr;
    uint64_t cr3;
    uint32_t limit;
    uint32_t hits;
    hwbp_trace_reg_t reg;
    hwbp_trace_result_t last_result;
    bool active;
    uint8_t signature[HWBP_TRACE_SIGNATURE_BYTES];
    uint16_t caller_offset;
    uint8_t caller_bits;
} hwbp_trace_t;

static inline bool hwbp_trace_caller_config(unsigned bits, unsigned offset)
{
    if (!bits) return !offset;
    return (bits == 32 || bits == 64) && offset <= 512 &&
           !(offset % (bits / 8));
}

static inline bool hwbp_trace_caller_slot(const hwbp_trace_t *trace,
                                         uint64_t rsp, uint64_t *slot)
{
    if (!trace || !slot || !trace->caller_bits ||
        !hwbp_trace_caller_config(trace->caller_bits, trace->caller_offset))
        return false;
    unsigned width = trace->caller_bits / 8;
    uint64_t maximum = 0x00007FFFFFFFFFFFULL;
    if (rsp < 4096 || rsp > maximum - trace->caller_offset - (width - 1))
        return false;
    uint64_t address = rsp + trace->caller_offset;
    if ((address & (width - 1)) || (address & 4095) > 4096 - width)
        return false;
    *slot = address;
    return true;
}

static inline bool hwbp_trace_prepare(hwbp_trace_t *trace, uint64_t addr,
                                      uint64_t cr3, hwbp_trace_reg_t reg,
                                      uint32_t limit, const uint8_t *signature)
{
    /* The fetched instruction and its signature must occupy one user page.
     * The #DB handler then never walks an unmapped following page. */
    if (!trace || !signature || addr < 4096 ||
        addr > 0x00007FFFFFFFFFFFULL - (HWBP_TRACE_SIGNATURE_BYTES - 1) ||
        (addr & 4095) > 4096 - HWBP_TRACE_SIGNATURE_BYTES ||
        !cr3 || (cr3 & 4095) || !limit || limit > HWBP_TRACE_MAX_HITS ||
        (reg != HWBP_TRACE_EAX32 && reg != HWBP_TRACE_EDI32))
        return false;

    *trace = (hwbp_trace_t){
        .addr = addr, .cr3 = cr3, .limit = limit, .reg = reg, .active = true,
    };
    for (unsigned i = 0; i < HWBP_TRACE_SIGNATURE_BYTES; i++)
        trace->signature[i] = signature[i];
    return true;
}

static inline hwbp_trace_result_t hwbp_trace_accept(
    hwbp_trace_t *trace, uint64_t rip, uint64_t cr3,
    const volatile uint8_t *instruction)
{
    /* Scope is checked before any guest memory read, including on other
     * processes executing a different image at the same virtual address. */
    if (!trace->active || rip != trace->addr ||
        (cr3 & ~4095ULL) != trace->cr3)
        return HWBP_TRACE_IGNORE;

    if (!instruction) {
        trace->active = false;
        trace->last_result = HWBP_TRACE_MISMATCH;
        return trace->last_result;
    }
    for (unsigned i = 0; i < HWBP_TRACE_SIGNATURE_BYTES; i++) {
        if (instruction[i] != trace->signature[i]) {
            trace->active = false;
            trace->last_result = HWBP_TRACE_MISMATCH;
            return trace->last_result;
        }
    }

    trace->hits++;
    trace->last_result = trace->hits >= trace->limit
        ? HWBP_TRACE_LIMIT : HWBP_TRACE_HIT;
    if (trace->last_result == HWBP_TRACE_LIMIT)
        trace->active = false;
    return trace->last_result;
}

#endif
