/*
 * pmic_arb.c -- PMIC registers through the SM8350 SPMI PMIC arbiter (v5)
 *
 * Regions from the phone's device tree (qcom,spmi@c440000, qcom,ee = 0):
 *   core    0x0C440000  APID map: which peripheral (PPID = SID:periph) each
 *                       arbiter channel (APID) serves
 *   chnls   0x0C600000  write channels, 64 KB per APID
 *   obsrvr  0x0E600000  read channels, 0x80 per APID in each EE's 64 KB
 *   cnfg    0x0C40A000  APID owner table: which EE may write through it
 * The same scheme as Linux's spmi-pmic-arb (pmic_arb_*_v5). Only single-byte
 * reads and writes, for `reboot bootloader`, the shell's `pmic` and the keys
 * task (keys.c), under one lock. Refuses unless the arbiter reports v5.
 *
 * spmi.c is an earlier attempt (March 2026) that guessed a v7 layout with a
 * fixed channel number and data-aborted on its first observer read; this
 * one looks the channel up in the APID map instead.
 */

#include "../../include/hal.h"
#include "../../include/aarch64.h"
#include "../../include/spinlock.h"
#include "pmic_arb.h"

#define ARB_CORE        0x0C440000UL
#define ARB_CHNLS       0x0C600000UL
#define ARB_OBSRVR      0x0E600000UL
#define ARB_CNFG        0x0C40A000UL
#define ARB_EE          0
#define CORE_SIZE       0x1100

#define ARB_VERSION     0x000           /* core */
#define APID_MAP(n)     (0x900 + 4 * (n))   /* core: PPID in bits 19:8 */
#define APID_OWNER(n)   (0x700 + 4 * (n))   /* cnfg: writer EE in bits 2:0 */

#define CHAN_CMD        0x00
#define CHAN_STATUS     0x08
#define CHAN_WDATA0     0x10
#define CHAN_RDATA0     0x18

#define STATUS_DONE     (1u << 0)
#define STATUS_ERRORS   (7u << 1)       /* failure, denied, dropped */

#define OP_EXT_WRITEL   0               /* 16-bit address, 1-8 bytes */
#define OP_EXT_READL    1

static spinlock_t arb_lock;

/* find_apid's answers: the map doesn't change, a scan reads up to 512
 * registers, and the keys task reads every 20 ms */
#define APID_CACHE      8
static struct {
    uint16_t ppid;
    uint8_t  write;
    int16_t  apid;
} apid_cache[APID_CACHE];
static int apid_cached;

/* The channel serving SID:periph. A peripheral may have one per EE: for
 * writing it must be ours, reading works through any. -1 if none. Caller
 * holds arb_lock. */
static int find_apid(uint8_t sid, uint8_t periph, int write)
{
    uint32_t ppid = ((uint32_t)sid << 8) | periph;
    for (int i = 0; i < apid_cached; i++)
        if (apid_cache[i].ppid == ppid && apid_cache[i].write == write)
            return apid_cache[i].apid;
    int any = -1, found = -1;
    for (int n = 0; APID_MAP(n) < CORE_SIZE; n++) {
        uint32_t map = mmio_rd32(ARB_CORE + APID_MAP(n));
        if (!map || ((map >> 8) & 0xFFF) != ppid)
            continue;
        if ((mmio_rd32(ARB_CNFG + APID_OWNER(n)) & 7) == ARB_EE) {
            found = n;
            break;
        }
        if (any < 0)
            any = n;
    }
    if (found < 0 && !write)
        found = any;
    if (apid_cached < APID_CACHE) {
        apid_cache[apid_cached].ppid = (uint16_t)ppid;
        apid_cache[apid_cached].write = (uint8_t)write;
        apid_cache[apid_cached].apid = (int16_t)found;
        apid_cached++;
    }
    return found;
}

/* Issue a one-byte command on a channel and wait (~1 ms) for it */
static int run(uint64_t chan, uint32_t op, uint16_t addr)
{
    mmio_wr32(chan + CHAN_CMD, (op << 27) | ((addr & 0xFFu) << 4));
    for (int i = 0; i < 1000; i++) {
        uint32_t st = mmio_rd32(chan + CHAN_STATUS);
        if (st & STATUS_DONE)
            return (st & STATUS_ERRORS) ? PMIC_ERR_STATUS : 0;
        udelay(1);
    }
    return PMIC_ERR_TIMEOUT;
}

uint32_t pmic_arb_version(void)
{
    return mmio_rd32(ARB_CORE + ARB_VERSION);
}

static int arb_ok(void)
{
    return (pmic_arb_version() >> 28) == 5;
}

int pmic_read(uint8_t sid, uint16_t addr, uint8_t *val)
{
    if (!arb_ok())
        return PMIC_ERR_VERSION;
    uint64_t flags = spin_lock_irqsave(&arb_lock);
    int apid = find_apid(sid, addr >> 8, 0);
    int err = PMIC_ERR_NO_CHANNEL;
    if (apid >= 0) {
        uint64_t chan = ARB_OBSRVR + 0x10000UL * ARB_EE + 0x80UL * apid;
        err = run(chan, OP_EXT_READL, addr);
        if (!err)
            *val = (uint8_t)mmio_rd32(chan + CHAN_RDATA0);
    }
    spin_unlock_irqrestore(&arb_lock, flags);
    return err;
}

int pmic_write(uint8_t sid, uint16_t addr, uint8_t val)
{
    if (!arb_ok())
        return PMIC_ERR_VERSION;
    uint64_t flags = spin_lock_irqsave(&arb_lock);
    int apid = find_apid(sid, addr >> 8, 1);
    int err = PMIC_ERR_NO_CHANNEL;
    if (apid >= 0) {
        uint64_t chan = ARB_CHNLS + 0x10000UL * apid;
        mmio_wr32(chan + CHAN_WDATA0, val);
        err = run(chan, OP_EXT_WRITEL, addr);
    }
    spin_unlock_irqrestore(&arb_lock, flags);
    return err;
}
