/* Fault-injection test of the production GT ownership/reset state machine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_gt.h"
static const uint32_t bases[] = {0x2000, 0x12000, 0x1a000, 0x22000};
static const uint32_t requests[] = {0xa278, 0xa270, 0xa188};
static const uint32_t acks[] = {0xd84, 0xd88, 0x130044};
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL GT line %d: %s\n", __LINE__, #x); abort(); \
} } while (0)
typedef struct {
    uint32_t fw[3], ack[3], mode[4], reset[4], guc, dma, gdrst;
    uint64_t ns;
    unsigned ops, fail_at, writes, resets, relaxes;
    uint32_t stall_reg;
    bool frozen_clock, all_ones, fail_cleanup;
} model_t;
static model_t hw;

static bool gt_model_read(void *ctx, uint32_t reg, uint32_t *value)
{
    CHECK(ctx == &hw);
    if (++hw.ops == hw.fail_at) return false;
    if (hw.all_ones) { *value = UINT32_MAX; return true; }
    for (unsigned i = 0; i < 3; i++) {
        if (reg == requests[i]) { *value = hw.fw[i]; return true; }
        if (reg == acks[i]) { *value = hw.ack[i]; return true; }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (reg == bases[i] + 0x9c) { *value = hw.mode[i]; return true; }
        if (reg == bases[i] + 0xd0) { *value = hw.reset[i]; return true; }
    }
    if (reg == 0x941c) { *value = hw.gdrst; return true; }
    if (reg == 0xc000) { *value = hw.guc; return true; }
    if (reg == 0xc314) { *value = hw.dma; return true; }
    CHECK(!"unexpected MMIO read");
    return false;
}

static bool gt_model_write(void *ctx, uint32_t reg, uint32_t value)
{
    CHECK(ctx == &hw);
    hw.writes++;
    bool failed = ++hw.ops == hw.fail_at;
    /* A failed write can nevertheless reach the device. */
    for (unsigned i = 0; i < 3; i++) if (reg == requests[i]) {
        CHECK(value == 0x10001 || value == 0x10000);
        if (hw.fail_cleanup && value == 0x10000) return false;
        hw.fw[i] = (hw.fw[i] & ~1u) | (value & 1);
        if (hw.stall_reg != acks[i]) hw.ack[i] = hw.fw[i];
        return !failed;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (reg == bases[i] + 0x9c) {
            CHECK(value == 0x01000100 || value == 0x01000000);
            CHECK((hw.ack[0] & hw.ack[1] & hw.ack[2] & 1) != 0);
            hw.mode[i] = (hw.mode[i] & ~0x100u) | (value & 0x100);
            if (hw.stall_reg != reg) hw.mode[i] |= 0x200;
            return !failed;
        }
        if (reg == bases[i] + 0xd0) {
            CHECK(value == 0x10001 || value == 0x10000);
            if (hw.fail_cleanup && value == 0x10000) return false;
            hw.reset[i] = value & 1;
            if (hw.stall_reg != reg && (value & 1)) hw.reset[i] |= 2;
            return !failed;
        }
    }
    if (reg == 0x941c) {
        CHECK(value == 1);
        if (!hw.resets) for (unsigned i = 0; i < 4; i++) {
            CHECK((hw.mode[i] & 0x300) == 0x300 && hw.reset[i] == 3);
        }
        hw.resets++;
        hw.gdrst = hw.stall_reg == reg ? 1 : 0;
        if (!hw.gdrst) for (unsigned i = 0; i < 4; i++) {
            hw.mode[i] = 0x200;
            hw.reset[i] = 0;
        }
        return !failed;
    }
    CHECK(!"unexpected MMIO write (display must be untouched)");
    return false;
}

static uint64_t gt_test_now(void *ctx)
{
    CHECK(ctx == &hw);
    if (!hw.frozen_clock) hw.ns += 1000;
    return hw.ns;
}

static void gt_test_relax(void *ctx)
{
    CHECK(ctx == &hw);
    hw.relaxes++;
}

static void gt_test_reset(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.guc = 1;
    for (unsigned i = 0; i < 4; i++) hw.mode[i] = 0x4200;
}

int main(void)
{
    const intel_gt_io_t io = {gt_model_read, gt_model_write, gt_test_now, gt_test_relax};
    intel_gt_state_t s = {0};
    gt_test_reset();
    CHECK(intel_gt_takeover(0x19128086, &io, &hw, &s) == INTEL_GT_INVALID);
    CHECK(intel_gt_takeover(0x3e9b8086, NULL, &hw, &s) == INTEL_GT_INVALID);
    CHECK(!s.started && !hw.ops);
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_OK);
    CHECK(s.ready && s.reset_issued && s.wake_owned == 7 && hw.resets == 2);
    unsigned complete_ops = hw.ops;
    for (unsigned i = 0; i < 4; i++) CHECK(hw.mode[i] == 0x300 && !hw.reset[i]);
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_INVALID);
    CHECK(hw.ops == complete_ops);

    /* Every read/write failure, including writes that applied before failure.
     * Before reset all owned state must be restored; after reset retain wake. */
    for (unsigned fail = 1; fail <= complete_ops; fail++) {
        gt_test_reset(); memset(&s, 0, sizeof(s)); hw.fail_at = fail;
        CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) != INTEL_GT_OK);
        CHECK(!s.ready);
        if (!s.reset_issued) {
            CHECK(s.cleanup_ok && !s.wake_owned && !s.stop_changed && !s.request_changed);
            for (unsigned i = 0; i < 3; i++) CHECK(!hw.fw[i] && !hw.ack[i]);
            for (unsigned i = 0; i < 4; i++) CHECK(hw.mode[i] == 0x4200 && !hw.reset[i]);
        } else CHECK(s.wake_owned == 7);
    }
    /* Preserve firmware-owned forcewake and unrelated mode bits on refusal. */
    gt_test_reset(); memset(&s, 0, sizeof(s));
    hw.fw[0] = hw.ack[0] = 3; hw.guc = 0;
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_FIRMWARE_ACTIVE);
    CHECK(s.cleanup_ok && hw.fw[0] == 3 && !hw.fw[1] && !hw.fw[2] && !hw.resets);
    gt_test_reset(); memset(&s, 0, sizeof(s)); hw.dma = 1;
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_FIRMWARE_ACTIVE);
    CHECK(s.cleanup_ok && !hw.resets);
    gt_test_reset(); memset(&s, 0, sizeof(s)); hw.reset[2] = 1;
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_FIRMWARE_ACTIVE);
    CHECK(s.cleanup_ok && hw.reset[2] == 1 && !hw.resets);
    gt_test_reset(); memset(&s, 0, sizeof(s)); hw.all_ones = true;
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_IO);
    CHECK(!hw.writes);

    const uint32_t stalled[] = {0xd84, 0xd88, 0x130044, 0x209c, 0x1209c,
        0x1a09c, 0x2209c, 0x20d0, 0x120d0, 0x1a0d0, 0x220d0, 0x941c};
    for (unsigned frozen = 0; frozen < 2; frozen++)
        for (unsigned n = 0; n < sizeof(stalled)/sizeof(stalled[0]); n++) {
            gt_test_reset(); memset(&s, 0, sizeof(s));
            hw.stall_reg = stalled[n]; hw.frozen_clock = frozen;
            for (unsigned i = 0; i < 4; i++)
                if (stalled[n] == bases[i] + 0x9c) hw.mode[i] &= ~0x200u;
            CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_TIMEOUT);
            CHECK(!s.ready && hw.ops < 110000);
            if (!s.reset_issued) CHECK(s.cleanup_ok && !s.wake_owned);
            else CHECK(s.wake_owned == 7);
        }
    gt_test_reset(); memset(&s, 0, sizeof(s));
    hw.stall_reg = 0x20d0; hw.fail_cleanup = true;
    CHECK(intel_gt_takeover(0x3e9b8086, &io, &hw, &s) == INTEL_GT_RESTORE_FAILED);
    CHECK(!s.cleanup_ok && !s.ready && s.wake_owned == 7 && !s.reset_issued);
    printf("Intel GT ownership: %u checks passed\n", checks);
    return 0;
}
