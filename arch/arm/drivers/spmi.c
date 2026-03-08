/*
 * spmi.c -- SPMI PMIC Arbiter for SM8350 (button input, regulators)
 *
 * Status: WIP — observer probe causes data abort on real hardware.
 *
 * Tested on ASUS ROG Phone 5, 2026-03-04:
 *   - Arbiter version detected as v7 (stride 0x800)
 *   - Channel 0x13 (PON periph) read via observer caused Data Abort
 *     (Synchronous External Abort, FAR in low SPMI address range)
 *   - Likely issue: observer channels not mapped by ABL for this periph
 *
 * From stock kernel interrupts.txt:
 *   - kpdpwr (power button): pmic_arb 20381786
 *   - resin (vol_down): pmic_arb 20316250
 *   - volume_up: spmi-gpio 5
 *
 * TODO: Either dump SPMI channel mapping from ABL at runtime,
 *       or use known working channel values from Linux kernel sources.
 */

#include "sm8350.h"

#define SPMI_CMD          0x00
#define SPMI_CONFIG       0x04
#define SPMI_STATUS       0x08
#define SPMI_RDATA0       0x18
#define SPMI_STATUS_DONE  (1 << 0)

static uint32_t spmi_stride;
static int spmi_initialized;

static void spmi_detect_version(void) {
    uint32_t ver = mmio_read32(SPMI_CORE_BASE);
    uint32_t major = (ver >> 28) & 0xF;

    if (major >= 7)
        spmi_stride = 0x800;        /* v7: 2KB per channel */
    else if (major >= 5)
        spmi_stride = 0x10000;      /* v5: 64KB per channel */
    else
        spmi_stride = 0x800;        /* Default to v7 */
    spmi_initialized = 1;
}

/*
 * Read one byte from a PMIC register via SPMI observer.
 * Returns byte value or -1 on failure.
 *
 * WARNING: This may data-abort if the observer channel is not mapped.
 */
int spmi_read_byte(uint32_t channel, uint8_t sid, uint16_t addr) {
    if (!spmi_initialized)
        spmi_detect_version();

    uintptr_t base = SPMI_OBSRVR_BASE + spmi_stride * channel;

    /* Bounds check */
    if (base >= SPMI_OBSRVR_BASE + 0x100000)
        return -1;

    /* Extended read: opcode=1, sid, addr, bc=0 (1 byte) */
    uint32_t cmd = (1 << 27) | ((sid & 0x7) << 20) | ((addr & 0xFFFF) << 4);
    mmio_write32(base + SPMI_CMD, cmd);

    /* Poll with timeout */
    for (int i = 0; i < 10000; i++) {
        uint32_t status = mmio_read32(base + SPMI_STATUS);
        if (status & SPMI_STATUS_DONE) {
            if (status & 0x0E)
                return -1;     /* Error bits set */
            return mmio_read32(base + SPMI_RDATA0) & 0xFF;
        }
    }
    return -1;  /* Timeout */
}

/*
 * Poll button state.
 * Returns bitmask: bit 0 = power, bit 1 = vol_down, bit 2 = vol_up.
 *
 * NOTE: Not yet functional. SPMI observer access not working.
 */
uint32_t key_poll(void) {
    /* TODO: implement once SPMI channel mapping is resolved */
    return 0;
}
