#include "intel_gt.h"

/* Intel KBL PRM: MI_MODE, RESET_CTRL, GDRST, driver forcewake ACKs.
 * CFL GT2 has RCS, VCS0, VECS, BCS (no VCS1). */
static const uint32_t engine_base[4] = {0x2000, 0x12000, 0x1a000, 0x22000};
static const uint32_t wake_req[3] = {0xa278, 0xa270, 0xa188};
static const uint32_t wake_ack[3] = {0xd84, 0xd88, 0x130044};
#define STOP_RING (1u << 8)
#define RING_IDLE (1u << 9)
#define REQUEST_RESET 1u
#define READY_RESET 2u
#define GDRST 0x941c
#define GUC_STATUS 0xc000
#define GUC_DMA_CONTROL 0xc314
#define GT_POLL_LIMIT 100000u

static bool gt_read(const intel_gt_io_t *io, void *ctx, intel_gt_state_t *s,
                    uint32_t reg, uint32_t *value)
{
    s->fault_reg = reg;
    if (!io->read(ctx, reg, value) || *value == UINT32_MAX) {
        s->status = INTEL_GT_IO;
        return false;
    }
    s->last_value = *value;
    return true;
}

static bool gt_write(const intel_gt_io_t *io, void *ctx, intel_gt_state_t *s,
                     uint32_t reg, uint32_t value)
{
    s->fault_reg = reg;
    if (!io->write(ctx, reg, value)) {
        s->status = INTEL_GT_IO;
        return false;
    }
    /* Posting read, including for masked writes. */
    return gt_read(io, ctx, s, reg, &value);
}

static bool gt_poll(const intel_gt_io_t *io, void *ctx, intel_gt_state_t *s,
                    uint32_t reg, uint32_t mask, uint32_t want,
                    uint64_t timeout_ns)
{
    uint64_t start = io->now_ns(ctx);
    for (unsigned n = 0; n < GT_POLL_LIMIT; n++) {
        uint32_t value;
        if (!gt_read(io, ctx, s, reg, &value)) return false;
        if ((value & mask) == want) return true;
        if (io->now_ns(ctx) - start >= timeout_ns) break;
        io->relax(ctx);
    }
    s->status = INTEL_GT_TIMEOUT;
    return false;
}

static void gt_restore(const intel_gt_io_t *io, void *ctx, intel_gt_state_t *s)
{
    uint32_t fault = s->fault_reg, last = s->last_value;
    enum intel_gt_status status = s->status;
    bool ok = true;
    for (unsigned i = 0; i < 4; i++) {
        if (s->request_changed & (1u << i)) {
            bool done = gt_write(io, ctx, s, engine_base[i] + 0xd0, 0x10000) &&
                gt_poll(io, ctx, s, engine_base[i] + 0xd0, 3, 0, 2000000);
            if (done) s->request_changed &= ~(1u << i);
            else ok = false;
        }
    }
    /* If reset preparation could not be undone, keep stop and wake held. */
    if (ok) for (unsigned i = 0; i < 4; i++) {
        if (s->stop_changed & (1u << i)) {
            uint32_t want = s->original_mode[i] & STOP_RING;
            bool done = gt_write(io, ctx, s, engine_base[i] + 0x9c,
                                 (STOP_RING << 16) | want) &&
                gt_poll(io, ctx, s, engine_base[i] + 0x9c, STOP_RING, want, 2000000);
            if (done) s->stop_changed &= ~(1u << i);
            else ok = false;
        }
    }
    if (ok) for (unsigned i = 0; i < 3; i++) {
        if (s->wake_owned & (1u << i)) {
            bool done = gt_write(io, ctx, s, wake_req[i], 0x10000) &&
                gt_poll(io, ctx, s, wake_ack[i], 1, 0, 2000000);
            if (done) s->wake_owned &= ~(1u << i);
            else ok = false;
        }
    }
    s->fault_reg = fault;
    s->last_value = last;
    s->cleanup_ok = ok;
    s->status = ok ? status : INTEL_GT_RESTORE_FAILED;
}

enum intel_gt_status intel_gt_takeover(uint32_t pci_id,
    const intel_gt_io_t *io, void *ctx, intel_gt_state_t *s)
{
    if (pci_id != 0x3e9b8086u || !s || s->started || !io || !io->read ||
        !io->write || !io->now_ns || !io->relax) return INTEL_GT_INVALID;
    s->started = true;
    s->status = INTEL_GT_IO;
    uint32_t value;
    for (unsigned i = 0; i < 3; i++) {
        if (!gt_read(io, ctx, s, wake_req[i], &value)) goto fail;
        if (!(value & 1)) {
            /* Track before the write: a failed transport may have applied it. */
            s->wake_owned |= 1u << i;
            if (!gt_poll(io, ctx, s, wake_ack[i], 1, 0, 2000000) ||
                !gt_write(io, ctx, s, wake_req[i], 0x10001)) goto fail;
        }
        if (!gt_poll(io, ctx, s, wake_ack[i], 1, 1, 2000000)) goto fail;
    }
    if (!gt_read(io, ctx, s, GUC_STATUS, &s->guc_status) ||
        !gt_read(io, ctx, s, GUC_DMA_CONTROL, &s->dma_control)) goto fail;
    /* Do not interfere with an independently running firmware scheduler. */
    if (!(s->guc_status & 1) || (s->dma_control & 1)) {
        s->status = INTEL_GT_FIRMWARE_ACTIVE;
        goto fail;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (!gt_read(io, ctx, s, engine_base[i] + 0x9c, &s->original_mode[i]) ||
            !gt_read(io, ctx, s, engine_base[i] + 0xd0, &value)) goto fail;
        if (value & 7) { s->status = INTEL_GT_FIRMWARE_ACTIVE; goto fail; }
    }
    for (unsigned i = 0; i < 4; i++) {
        s->stop_changed |= 1u << i;
        if (!gt_write(io, ctx, s, engine_base[i] + 0x9c, 0x01000100) ||
            !gt_poll(io, ctx, s, engine_base[i] + 0x9c,
                     STOP_RING | RING_IDLE, STOP_RING | RING_IDLE, 2000000)) goto fail;
    }
    for (unsigned i = 0; i < 4; i++) {
        s->request_changed |= 1u << i;
        if (!gt_write(io, ctx, s, engine_base[i] + 0xd0, 0x10001) ||
            !gt_poll(io, ctx, s, engine_base[i] + 0xd0, READY_RESET,
                     READY_RESET, 2000000)) goto fail;
    }
    /* Reset only GT, preserving display. Two completed resets serialize the
     * self-clearing register on Gen9 (i915 gen6_hw_domain_reset sequence). */
    s->reset_issued = true;
    for (unsigned pass = 0; pass < 2; pass++) {
        if (!gt_write(io, ctx, s, GDRST, 1) ||
            !gt_poll(io, ctx, s, GDRST, 1, 0, 2000000)) goto fail;
    }
    uint64_t start = io->now_ns(ctx);
    unsigned delay;
    for (delay = 0; delay < GT_POLL_LIMIT; delay++) {
        if (io->now_ns(ctx) - start >= 50000) break;
        io->relax(ctx);
    }
    if (delay == GT_POLL_LIMIT) { s->status = INTEL_GT_TIMEOUT; goto fail; }
    for (unsigned i = 0; i < 4; i++) {
        if (!gt_write(io, ctx, s, engine_base[i] + 0xd0, 0x10000) ||
            !gt_poll(io, ctx, s, engine_base[i] + 0xd0, 3, 0, 2000000) ||
            !gt_write(io, ctx, s, engine_base[i] + 0x9c, 0x01000100) ||
            !gt_poll(io, ctx, s, engine_base[i] + 0x9c,
                     STOP_RING | RING_IDLE, STOP_RING | RING_IDLE, 2000000)) goto fail;
    }
    s->request_changed = 0;
    for (unsigned i = 0; i < 3; i++)
        if (!gt_poll(io, ctx, s, wake_ack[i], 1, 1, 2000000)) goto fail;
    if (!gt_read(io, ctx, s, GUC_STATUS, &value) || !(value & 1)) {
        s->status = INTEL_GT_FIRMWARE_ACTIVE;
        goto fail;
    }
    if (!gt_read(io, ctx, s, GUC_DMA_CONTROL, &value) || (value & 1)) {
        s->status = INTEL_GT_FIRMWARE_ACTIVE;
        goto fail;
    }
    s->ready = true;
    s->status = INTEL_GT_OK;
    return s->status;
fail:
    if (!s->reset_issued) gt_restore(io, ctx, s);
    return s->status;
}
